"""The BX Kernel IR (FR-250).

One small, typed tree that is the meeting point of four functions:
cxx_import / cxx_print on the C++ side and rs_parse / rs_print on the Rust
side.  Every Kernel construct has exactly ONE canonical C++ spelling and ONE
canonical Rust spelling; the bijection laws are stated over this tree:

  law 1   rs_parse(rs_print(k)) == k   and   cxx_import(cxx_print(k)) == k
  law 2   print(parse(x)) is idempotent on both sides

Design rules:

* Every node is a frozen dataclass compared structurally.  Source locations
  and surface-spelling hints are carried in fields declared
  ``field(compare=False)`` so they never take part in equality.
* The Kernel has NO implicit conversions: every C++ implicit conversion is
  an explicit ``Cast``.  It has no Paren node: printers parenthesize.
* Types are a CLOSED SUM (``Ty``).  FR-250 has ``Prim``; FR-251 adds
  ``VecT`` (std::vector <=> Vec), ``StrT`` (std::string <=> String) and
  ``RefT`` (a reference PARAMETER or range-for variable: `const T&` <=>
  `&T`, `T&` <=> `&mut T`).  A ``Var`` of a RefT-declared name has the
  REFERENT type in expressions, exactly like a C++ reference.
* FR-251 value semantics are Rust's: a bare ``Var`` of a non-Copy type
  (Vec/String) in a CONSUMING position (let/assign value, by-value
  argument, return, push value) is a MOVE (C++ `std::move(x)`); a copy is
  an explicit ``Clone`` (C++ copy construction, Rust `.clone()`).  In a
  COPIED position (vec literal elements, vec-fill value, resize value --
  C++ takes `const T&` and copies) a place is written bare and the Rust
  printer adds `.clone()` for non-Copy element types.
* FR-261 adds ``PtrT`` (`const T*` <=> `&[T]`, `T*` <=> `&mut [T]`: the
  suffix slice of an allocation) for parameters and locals, and two
  pointer-VALUE nodes, ``Offset`` (`p + k`, `v.data()`) and ``AddrOf``
  (`&x`); `*p` / `p[i]` / `p->f` are the existing Index / Field.
* FR-259 adds ``Prim('i16')`` / ``Prim('u16')`` (`short` / `unsigned short`,
  arithmetic only after an explicit promotion, like i8/u8) and two value
  nodes: ``Cond`` (`c ? a : b` <=> `if c { a } else { b }`, scalar arms of
  ONE type -- clang's usual arithmetic conversions are explicit Casts) and
  ``IncDec`` (`++x` / `x++` / `--x` / `x--` USED AS A VALUE on an integer
  variable of an arithmetic type <=> a canonical Rust block `{ x += 1; x }`
  / `{ x += 1; x - 1 }`, wrapping for unsigned).  A statement `++x;` stays a
  CompoundAssign.
* ``Residual*`` nodes are the Surface-IR seam: cxx_import keeps going past an
  unsupported construct and leaves a located Residual in its place, so a
  program's distance from the subset is a COUNT, not a first failure
  (FR-254).  A program containing a Residual never reaches a printer.
* FR-252 adds closures: the statement ``Closure`` (`auto f = [caps](params)
  -> R { body };` <=> `let f = [move] |params| -> R { body };`), whose ONE
  capture bit is ``move`` (all by copy) or not (all by reference) -- the
  capture list itself is derived from the body (``closure_captures``), the
  expression ``ClosureCall`` (`f(a)`), and the type ``FnT`` (a closure's
  scalar signature; behind a ``RefT`` it is a callable PARAMETER: `const F&`
  <=> `&impl Fn`, `F&` <=> `&mut impl FnMut`).
"""

from dataclasses import dataclass, field, fields, is_dataclass
from typing import Optional, Tuple, Union


# ---------------------------------------------------------------- locations

@dataclass(frozen=True)
class Loc:
    """A 1-based source position. Never part of node equality."""

    line: int
    col: int

    def __str__(self):
        return "%d:%d" % (self.line, self.col)


def _loc():
    return field(default=None, compare=False, repr=False)


# ---------------------------------------------------------------- types

@dataclass(frozen=True)
class Prim:
    """A scalar type, named by its Rust spelling."""

    name: str

    def __str__(self):
        return self.name


@dataclass(frozen=True)
class VecT:
    """std::vector<T> <=> Vec<T>; T is a non-bool Prim, String or a VecT."""

    elem: "Ty"

    def __str__(self):
        return "Vec<%s>" % self.elem


@dataclass(frozen=True)
class StrT:
    """std::string <=> String.  INVARIANT: every String is ASCII."""

    def __str__(self):
        return "String"


@dataclass(frozen=True)
class RefT:
    """A reference: `const T&` <=> `&T`, `T&` <=> `&mut T` (FR-251).

    Only parameters and range-for variables have reference type; nothing
    returns or stores a reference.
    """

    inner: "Ty"
    mutable: bool

    def __str__(self):
        if isinstance(self.inner, FnT) and self.mutable:
            return "&mut impl FnMut" + str(self.inner)[len("impl Fn"):]
        return "&%s%s" % ("mut " if self.mutable else "", self.inner)


@dataclass(frozen=True)
class StructT:
    """FR-257: a user aggregate, named by its ABSOLUTE path ('a::P').

    C++ `struct P` (public data members of Kernel types) <=> Rust
    `#[derive(Clone)] struct P` with `pub` fields.  Like Vec and String it
    is NOT Copy: a C++ copy is an explicit ``Clone`` (`.clone()`), a bare
    variable in a consuming position is a move (`std::move(x)`).
    """

    path: str

    def __str__(self):
        return self.path


@dataclass(frozen=True)
class PtrT:
    """FR-261: a pointer is the SUFFIX SLICE of its allocation: `const T*`
    <=> `&[T]`, `T*` <=> `&mut [T]`.

    Only parameters and locals have pointer type (never a data member, a
    vector element, a reference's referent or a return type), and a pointer
    value is only ever made from a real allocation: `v.data()` (the vector's
    elements), `&x` (a one-element slice), or a forward re-slice `p + k` of
    another pointer.  Lengths stay ordinary integers -- there is NO bounds
    proof: an index or offset outside the allocation panics in Rust where it
    is undefined behaviour in C++, i.e. the two differ on UB input only.
    """

    inner: "Ty"
    mutable: bool

    def __str__(self):
        return "&%s[%s]" % ("mut " if self.mutable else "", self.inner)


@dataclass(frozen=True)
class FnT:
    """FR-252: the type of a CLOSURE -- its parameter types (scalars) and
    return type (a scalar or unit).  A closure variable (``Closure``) has
    this type in a scope; as a parameter it is always behind a reference:
    `const F& f` <=> `f: &impl Fn(..) -> R`, `F& f` <=> `f: &mut impl
    FnMut(..) -> R` (``RefT(FnT, mutable)``)."""

    params: Tuple["Ty", ...]
    ret: "Ty"

    def __str__(self):
        return "impl Fn(%s)%s" % (", ".join(str(p) for p in self.params),
                                  "" if self.ret == Prim("()") else " -> %s" % self.ret)


Ty = Union[Prim, VecT, StrT, RefT, StructT, PtrT, FnT]  # the closed sum
TYPE_CLASSES = (Prim, VecT, StrT, RefT, StructT, PtrT, FnT)
STRING = StrT()

I8 = Prim("i8")
U8 = Prim("u8")
I16 = Prim("i16")  # FR-259
U16 = Prim("u16")  # FR-259
I32 = Prim("i32")
I64 = Prim("i64")
U32 = Prim("u32")
U64 = Prim("u64")
USIZE = Prim("usize")
F64 = Prim("f64")
BOOL = Prim("bool")
UNIT = Prim("()")
STR = Prim("&str")  # only the type of a printf %s string-literal argument

ALL_PRIMS = (I8, U8, I32, I64, U32, U64, USIZE, F64, BOOL, I16, U16)
INT_TYPES = (I8, U8, I32, I64, U32, U64, USIZE, I16, U16)
# FR-259: the integer types C++ PROMOTES to int before any arithmetic (the
# Kernel has no arithmetic on them: every operand is an explicit Cast)
SMALL_INT = (I8, U8, I16, U16)
# The types arithmetic happens in after C++ integer promotion.
ARITH_INT = (I32, I64, U32, U64, USIZE)
ARITH = ARITH_INT + (F64,)
UNSIGNED = (U8, U32, U64, USIZE, U16)
SIGNED = (I8, I32, I64, I16)

INT_RANGE = {
    "i8": (-(1 << 7), (1 << 7) - 1),
    "u8": (0, (1 << 8) - 1),
    "i16": (-(1 << 15), (1 << 15) - 1),
    "u16": (0, (1 << 16) - 1),
    "i32": (-(1 << 31), (1 << 31) - 1),
    "i64": (-(1 << 63), (1 << 63) - 1),
    "u32": (0, (1 << 32) - 1),
    "u64": (0, (1 << 64) - 1),
    "usize": (0, (1 << 64) - 1),
}

PRIM_BY_NAME = {p.name: p for p in ALL_PRIMS + (UNIT,)}


def is_int(t):
    return t in INT_TYPES


def is_copy(t):
    """Copy types are the scalars; Vec and String are move-only.  FR-261: a
    pointer value is copied in C++ and, in every position the subset allows
    it (a typed `let`, an argument), copied (`&[T]`) or implicitly reborrowed
    (`&mut [T]`) in Rust -- never moved."""
    return isinstance(t, (Prim, PtrT))


def ptr_fits(actual, expected):
    """FR-261: may a value of type ``actual`` stand where ``expected`` is
    wanted?  Equal types, or the qualification conversion `T*` -> `const
    T*` (Rust: the `&mut [T]` -> `&[T]` coercion)."""
    if actual == expected:
        return True
    return (isinstance(actual, PtrT) and isinstance(expected, PtrT) and actual.inner == expected.inner
            and actual.mutable and not expected.mutable)


def is_owned_nc(t):
    """An owned non-Copy value type (Vec, String or a struct)."""
    return isinstance(t, (VecT, StrT, StructT))


def deref(t):
    return t.inner if isinstance(t, RefT) else t


def elem_ok(t):
    """May ``t`` be a vector element type?  (std::vector<bool> is a bit-
    packed specialization whose operator[] returns a proxy, not a bool&.)"""
    if isinstance(t, Prim):
        return t in ALL_PRIMS and t != BOOL
    if isinstance(t, StrT):
        return True
    if isinstance(t, VecT):
        return elem_ok(t.elem)
    if isinstance(t, StructT):
        return True
    return False


def has_struct(t):
    """Does ``t`` contain a struct anywhere (no `==` in either language)?"""
    if isinstance(t, StructT):
        return True
    if isinstance(t, (VecT,)):
        return has_struct(t.elem)
    if isinstance(t, RefT):
        return has_struct(t.inner)
    return False


def is_unsigned(t):
    return t in UNSIGNED


# ---------------------------------------------------------------- expressions

@dataclass(frozen=True)
class IntLit:
    """A non-negative integer literal of an integer type (negation is Unary)."""

    value: int
    ty: Prim
    loc: Optional[Loc] = _loc()
    # Rust input only: was the literal written without a type suffix?
    unsuffixed: bool = field(default=False, compare=False, repr=False)


@dataclass(frozen=True)
class FloatLit:
    """A finite non-negative f64 literal, keyed by Python's shortest repr.

    The repr (not the float) is the compared value so 0.0 and -0.0 could
    never alias; negative literals are Unary('neg', FloatLit).
    """

    text: str
    loc: Optional[Loc] = _loc()

    @staticmethod
    def of(x):
        return FloatLit(repr(float(x)))

    @property
    def value(self):
        return float(self.text)


@dataclass(frozen=True)
class BoolLit:
    value: bool
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class StrLit:
    """An ASCII string literal; FR-250 only allows it as a printf %s arg."""

    value: str
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Var:
    name: str
    loc: Optional[Loc] = _loc()
    # Rust input only: 'deref' for `*x` (x a reference).
    spelling: str = field(default="", compare=False, repr=False)


# Unary ops: 'neg' (arithmetic negation; wrapping on unsigned), 'not'
# (logical not on bool, bitwise complement on integers -- C++ `!`/`~`,
# Rust `!` for both).
UNARY_OPS = ("neg", "not")


@dataclass(frozen=True)
class Unary:
    op: str
    operand: "Expr"
    loc: Optional[Loc] = _loc()
    # Surface spelling hint ('plain' / 'wrapping'), Rust input only.
    spelling: str = field(default="", compare=False, repr=False)


ARITH_OPS = ("add", "sub", "mul", "div", "rem")
BIT_OPS = ("band", "bor", "bxor")
SHIFT_OPS = ("shl", "shr")
CMP_OPS = ("eq", "ne", "lt", "le", "gt", "ge")
LOGIC_OPS = ("and", "or")
BINARY_OPS = ARITH_OPS + BIT_OPS + SHIFT_OPS + CMP_OPS + LOGIC_OPS
# Ops that have a compound-assignment form (x op= e).
COMPOUND_OPS = ARITH_OPS + BIT_OPS + SHIFT_OPS
# Unsigned arithmetic that wraps in C++ and is spelled .wrapping_*() in Rust.
WRAPPING_OPS = ("add", "sub", "mul")


@dataclass(frozen=True)
class Binary:
    op: str
    lhs: "Expr"
    rhs: "Expr"
    loc: Optional[Loc] = _loc()
    spelling: str = field(default="", compare=False, repr=False)


@dataclass(frozen=True)
class Cast:
    """Explicit conversion: C++ static_cast<T>(e), Rust `e as T`."""

    ty: Prim
    operand: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Call:
    name: str
    args: Tuple["Expr", ...]
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class ResidualExpr:
    """An expression outside the subset (Surface IR only).

    ``cascade`` marks a use of a declaration that was itself rejected: it
    keeps the program out of the printers but is not reported or counted
    again (the declaration's own residual already is).
    """

    reason: str
    loc: Optional[Loc] = _loc()
    cascade: bool = field(default=False, compare=False, repr=False)


# ---------------------------------------------------------------- FR-251 exprs

@dataclass(frozen=True)
class Clone:
    """An explicit copy of a non-Copy PLACE (Var or Index): C++ copy
    construction / copy assignment, Rust `x.clone()`."""

    operand: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Index:
    """`v[i]` on a vector place; ``index`` is usize."""

    base: "Expr"
    index: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class ByteAt:
    """Read-only `s[i]` on a std::string (char, i8) <=> `s.as_bytes()[i] as i8`.

    C++ defines `s[s.size()]` as '\\0'; Rust panics there (a LOUD
    divergence, never a silent one) -- tests keep indices below size().
    A write through `s[i]` is not in the subset (no safe String byte write).
    """

    base: "Expr"
    index: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Len:
    """`x.size()` / `x.length()` <=> `x.len()` (Vec or String), usize."""

    base: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class IsEmpty:
    """`x.empty()` <=> `x.is_empty()`."""

    base: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class VecLit:
    """`{a, b}` / `std::vector<T>{a, b}` / `std::vector<T>()` <=>
    `vec![a, b]` / `Vec::new()`.  ``elem`` is the element type (needed for
    the empty literal); rs_parse leaves it None and normalize fills it."""

    elem: Optional["Ty"]
    elems: Tuple["Expr", ...]
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class VecFill:
    """`std::vector<T>(n, x)` <=> `vec![x; n]` (n usize; x copied n times)."""

    value: "Expr"
    count: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class StrNew:
    """`std::string()` <=> `String::new()`."""

    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class StrFrom:
    """`std::string("lit")` <=> `String::from("lit")` (ASCII only)."""

    value: str
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Substr:
    """`s.substr(p, n)` <=> `bx::substr(&s, p, n)`; `s.substr(p)` (count
    None) <=> `bx::substr_from(&s, p)`.  Both clamp n like C++."""

    base: "Expr"
    pos: "Expr"
    count: Optional["Expr"]
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class ToStr:
    """`std::to_string(x)` <=> `x.to_string()` for integer x."""

    operand: "Expr"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Borrow:
    """SURFACE-ONLY (rs_parse): Rust `&e` / `&mut e`.  The borrow is a
    spelling the printer derives from types, so normalize removes it; the
    Kernel proper never contains one."""

    operand: "Expr"
    mutable: bool
    loc: Optional[Loc] = _loc()


# ---------------------------------------------------------------- FR-257 exprs

@dataclass(frozen=True)
class Field:
    """`p.f` <=> `p.f` (C++ `this->f` / implicit `f` <=> Rust `self.f`).

    ``base`` is a PLACE of struct type (a variable -- `self` included --
    an element, or another field); the Kernel has no member access on a
    temporary."""

    base: "Expr"
    name: str
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class StructLit:
    """Aggregate initialization `S{a, b}` / `S s = {a, b};` <=> `S { f1: a,
    f2: b }`: EVERY field, in DECLARATION order (both languages then
    evaluate the initializers left to right).  ``names`` is the Rust
    surface's field list (compare=False), checked against the declaration."""

    path: str
    values: Tuple["Expr", ...]
    loc: Optional[Loc] = _loc()
    names: Tuple[str, ...] = field(default=(), compare=False, repr=False)


@dataclass(frozen=True)
class MethodCall:
    """`r.m(args)` <=> `r.m(args)` on a non-static member function; the
    method is resolved from the receiver's struct type.  ``recv`` is a
    place (`self` for C++ `this->m(..)` / implicit `m(..)`)."""

    recv: "Expr"
    name: str
    args: Tuple["Expr", ...]
    loc: Optional[Loc] = _loc()
    # the receiver's struct path, filled in by normalize (which has the
    # scope) so scope-free analyses resolve the method exactly
    owner: str = field(default="", compare=False, repr=False)


# ---------------------------------------------------------------- FR-261 exprs

@dataclass(frozen=True)
class Offset:
    """A pointer made by RE-SLICING: `p + k` <=> `&p[k..]` / `&mut p[k..]`
    (``base`` a pointer VARIABLE, ``start`` usize), `v.data()` <=> `&v[..]`
    (``base`` a vector PLACE, ``start`` None) and `v.data() + k` <=>
    `&v[k..]`.  Its pointee constness is the base's; the Rust `&` / `&mut`
    is derived from the position's expected type (``spelling`` records the
    Rust surface, compare=False)."""

    base: "Expr"
    start: Optional["Expr"]
    loc: Optional[Loc] = _loc()
    spelling: str = field(default="", compare=False, repr=False)


@dataclass(frozen=True)
class AddrOf:
    """`&x` of a single object <=> `std::slice::from_ref(&x)` /
    `std::slice::from_mut(&mut x)`: a one-element slice.  ``place`` is a
    variable or a data member, never an element (`&v[i]` is the suffix
    `v.data() + i`, normal form N17)."""

    place: "Expr"
    loc: Optional[Loc] = _loc()
    spelling: str = field(default="", compare=False, repr=False)


# ---------------------------------------------------------------- FR-259 exprs

@dataclass(frozen=True)
class Cond:
    """`c ? a : b` <=> `if c { a } else { b }`.  ``cond`` is bool, ``then``
    and ``els`` have ONE scalar type (the usual arithmetic conversions are
    explicit Casts); only the chosen arm is evaluated, in both languages."""

    cond: "Expr"
    then: "Expr"
    els: "Expr"
    loc: Optional[Loc] = _loc()


INCDEC_OPS = ("add", "sub")


@dataclass(frozen=True)
class IncDec:
    """An increment / decrement USED AS A VALUE: `++x` (``post`` False, the
    new value) / `x++` (the old value), ``op`` 'add' or 'sub'.  ``var`` is a
    Var of an integer variable (or reference to one) of an ARITHMETIC type.
    Rust: `{ x += 1; x }` / `{ x += 1; x - 1 }` (`{ x -= 1; x + 1 }` for
    `x--`), unsigned `{ x = x.wrapping_add(1u32); x.wrapping_sub(1u32) }`.
    ``spelling`` (compare=False) is the Rust surface: (wrapping?, literal
    type), checked against the variable's type."""

    var: "Var"
    op: str
    post: bool
    loc: Optional[Loc] = _loc()
    spelling: tuple = field(default=(), compare=False, repr=False)


# ---------------------------------------------------------------- FR-252 exprs

@dataclass(frozen=True)
class ClosureCall:
    """`f(a, b)` of a local closure ``f`` (C++ `operator()` of the lambda's
    closure object), or of a callable PARAMETER (``RefT(FnT)``)."""

    name: str
    args: Tuple["Expr", ...]
    loc: Optional[Loc] = _loc()


Expr = Union[IntLit, FloatLit, BoolLit, StrLit, Var, Unary, Binary, Cast, Call,
             ResidualExpr, Clone, Index, ByteAt, Len, IsEmpty, VecLit, VecFill,
             StrNew, StrFrom, Substr, ToStr, Field, StructLit, MethodCall, Offset, AddrOf,
             Cond, IncDec, ClosureCall]


# ---------------------------------------------------------------- printf

@dataclass(frozen=True)
class Text:
    """Literal text of a printf format (unescaped, '%' not doubled)."""

    text: str


# Spec kinds: 'd' integer (i32/i64/u32/u64/usize; the conversion letter is
# derived from the argument type), 'f' f64 fixed with precision, 'c' an
# int-promoted character, 's' a string literal.
SPEC_KINDS = ("d", "f", "c", "s")


@dataclass(frozen=True)
class Spec:
    kind: str
    arg: "Expr"
    flag: str = ""  # '' | '-' | '0'   ('d' only)
    width: Optional[int] = None  # ('d' only)
    prec: Optional[int] = None  # ('f' only, always set for 'f')


Seg = Union[Text, Spec]


# ---------------------------------------------------------------- statements

@dataclass(frozen=True)
class Block:
    stmts: Tuple["Stmt", ...]
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Let:
    name: str
    ty: Prim
    mutable: bool
    init: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Assign:
    name: str
    value: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class CompoundAssign:
    op: str
    name: str
    value: Expr
    loc: Optional[Loc] = _loc()
    spelling: str = field(default="", compare=False, repr=False)


@dataclass(frozen=True)
class ExprStmt:
    """A call (or FR-257 method call) evaluated for effect (its value, if
    any, discarded)."""

    expr: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Print:
    segs: Tuple[Seg, ...]
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class If:
    cond: Expr
    then: Block
    els: Optional[Block] = None
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class While:
    cond: Expr
    body: Block
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class For:
    """The canonical counting loop: for (T v = lo; v < hi; ++v) body.

    The loop variable's type is the type of ``lo``.  Well-formedness:
    ``hi`` is a literal or a variable, the body never assigns ``var`` or
    ``hi`` (C++ re-evaluates the bound every iteration, Rust evaluates the
    range once).
    """

    var: str
    lo: Expr
    hi: Expr
    body: Block
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Break:
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Continue:
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Return:
    value: Optional[Expr] = None
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class ResidualStmt:
    reason: str
    loc: Optional[Loc] = _loc()


# ---------------------------------------------------------------- FR-251 stmts

@dataclass(frozen=True)
class Store:
    """`v[i] = e;` / `p.f = e;` (target an Index or Field place)."""

    target: Expr
    value: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class CompoundStore:
    """`v[i] op= e;` / `p.f op= e;` (`++v[i];` for += 1)."""

    op: str
    target: Expr
    value: Expr
    loc: Optional[Loc] = _loc()
    spelling: str = field(default="", compare=False, repr=False)


@dataclass(frozen=True)
class Push:
    """`v.push_back(e);` <=> `v.push(e);` -- e is CONSUMED."""

    target: Expr
    value: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Pop:
    """`v.pop_back();` <=> `v.pop();` (pop on empty is UB in C++: excluded)."""

    target: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Clear:
    """`x.clear();` (Vec or String)."""

    target: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Resize:
    """`v.resize(n, x);` (x COPIED into new slots)."""

    target: Expr
    count: Expr
    value: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class PushStr:
    """`s += t;` <=> `s.push_str(&t);` (t a String or a literal)."""

    target: Expr
    value: Expr
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class PushByte:
    """`s.push_back(c);` / `s += c;` <=> `bx::push_byte(&mut s, c);`."""

    target: Expr
    value: Expr
    loc: Optional[Loc] = _loc()


# Range-for modes: 'copy' `for (T x : v)` <=> `for x in v.iter().copied()`
# (`.cloned()` for non-Copy T), 'ref' `for (const T& x : v)` <=>
# `for x in v.iter()`, 'mut' `for (T& x : v)` <=> `for x in v.iter_mut()`.
FOREACH_MODES = ("copy", "ref", "mut")


@dataclass(frozen=True)
class ForEach:
    var: str
    mutable: bool  # 'copy' only: `T x` (mut) vs `const T x`
    mode: str
    iterable: Expr
    body: Block
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Closure:
    """FR-252: a local closure, `auto f = [caps](params) -> R { body };` <=>
    `let f = move |params| -> R { body };`.

    ``move`` True: every capture is BY COPY (C++ `[a, b]`, Rust `move`);
    False: every capture is BY REFERENCE (C++ `[&a, &b]`, a non-move Rust
    closure, which borrows exactly what it uses).  The capture LIST is not
    stored: it is the set of enclosing variables the body uses, in order of
    first use (``closure_captures``) -- Rust spells no list at all, and
    C++'s written order and unused captures are spellings (normal form
    N23).  Whether the closure MUTATES state (C++ `mutable` for a by-copy
    closure, Rust `let mut f` / FnMut) is derived from the body too.
    ``params`` are scalars, ``ret`` a scalar or unit.  ``spelling``
    (compare=False) is the Rust surface's `let mut`, checked against the
    derived FnMut-ness."""

    name: str
    move: bool
    params: Tuple["Param", ...]
    ret: "Ty"
    body: "Block"
    loc: Optional[Loc] = _loc()
    spelling: str = field(default="", compare=False, repr=False)


Stmt = Union[Block, Let, Assign, CompoundAssign, ExprStmt, Print, If, While,
             For, Break, Continue, Return, ResidualStmt, Store, CompoundStore,
             Push, Pop, Clear, Resize, PushStr, PushByte, ForEach, Closure]

MUTATING_STMTS = (Push, Pop, Clear, Resize, PushStr, PushByte)


# ---------------------------------------------------------------- items

@dataclass(frozen=True)
class Param:
    name: str
    ty: Prim
    mutable: bool
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Func:
    """A free function.  ``main`` has ret UNIT, no params and no Return:
    C++ spells it `int main() { ... return 0; }`, Rust `fn main() { ... }`.
    """

    name: str
    params: Tuple[Param, ...]
    ret: Prim
    body: Block
    loc: Optional[Loc] = _loc()
    # FR-260: the enclosing named namespaces, outermost first (C++
    # `namespace a { namespace b {` <=> Rust `mod a { pub mod b {`).
    ns: Tuple[str, ...] = ()
    # FR-260: a member of an anonymous namespace (C++) <=> a private `fn`
    # inside a `mod` (Rust).  Always False at the root (normal form N14).
    internal: bool = False
    # FR-257: a member function of struct ``owner`` (its leaf name; the
    # struct lives in ``ns``).  ``recv``: '' (a free function), 'static'
    # (C++ `static` member <=> Rust associated fn `S::f`), 'ref' (C++ const
    # member function <=> `&self`), 'mut' (non-const <=> `&mut self`),
    # 'ctor' (a constructor whose body is a member-initializer list <=>
    # `pub fn new(..) -> S { return S { .. }; }`; its name is 'new' and its
    # body is exactly `return StructLit;`).
    owner: str = ""
    recv: str = ""

    @property
    def path(self):
        if self.owner:
            return path_of(tuple(self.ns) + (self.owner,), self.name)
        return path_of(self.ns, self.name)


RECV_KINDS = ("static", "ref", "mut", "ctor")


@dataclass(frozen=True)
class FieldDef:
    name: str
    ty: "Ty"
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class StructDef:
    """FR-257: `struct S { fields; methods; };` <=> `#[derive(Clone)]
    struct S { pub fields } impl S { pub fns }`.  ``methods`` keep their
    C++ in-class declaration order (= the Rust `impl` order)."""

    name: str
    fields: Tuple[FieldDef, ...]
    methods: Tuple[Func, ...]
    loc: Optional[Loc] = _loc()
    ns: Tuple[str, ...] = ()
    internal: bool = False

    @property
    def path(self):
        return path_of(self.ns, self.name)

    def field_ty(self, name):
        for f in self.fields:
            if f.name == name:
                return f.ty
        return None

    def method(self, name):
        for m in self.methods:
            if m.name == name:
                return m
        return None

    def ctor(self):
        for m in self.methods:
            if m.recv == "ctor":
                return m
        return None


def path_of(ns, name):
    """The absolute Kernel name of a function: 'a::b::f', or 'f' at the root.

    Every ``Call.name`` is such an absolute path (FR-260): name lookup is
    RESOLVED on import (clang decl ids / rs_parse's module resolution), so
    no Kernel term depends on declaration order or on a using-directive.
    """
    return "::".join(tuple(ns) + (name,))


def split_path(path):
    """'a::b::f' -> (('a', 'b'), 'f')."""
    parts = path.split("::")
    return tuple(parts[:-1]), parts[-1]


@dataclass(frozen=True)
class ResidualItem:
    reason: str
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Program:
    items: Tuple[Union[Func, StructDef, ResidualItem], ...]

    def funcs(self):
        """The FREE functions (FR-257 member functions live in StructDefs)."""
        return [f for f in self.items if isinstance(f, Func)]

    def structs(self):
        return [s for s in self.items if isinstance(s, StructDef)]

    def all_funcs(self):
        """Free functions and member functions, in item order."""
        out = []
        for it in self.items:
            if isinstance(it, Func):
                out.append(it)
            elif isinstance(it, StructDef):
                out.extend(it.methods)
        return out


RESIDUALS = (ResidualExpr, ResidualStmt, ResidualItem)


def closure_captures(c):
    """FR-252: the variables closure ``c`` captures -- every name its body
    uses (a Var, or the target of an assignment) that the body does not
    declare itself, in order of first use (pre-order).  The checker refuses
    a closure that declares a name of its enclosing scope, so a used name is
    either the closure's own or a capture."""
    own = {p.name for p in c.params}
    for n in walk(c.body):
        if isinstance(n, Let):
            own.add(n.name)
        elif isinstance(n, (For, ForEach)):
            own.add(n.var)
        elif isinstance(n, Closure):
            own.add(n.name)
    out = []
    for n in walk(c.body):
        name = None
        if isinstance(n, Var):
            name = n.name
        elif isinstance(n, (Assign, CompoundAssign)):
            name = n.name
        elif isinstance(n, ClosureCall):
            name = n.name
        if name is not None and name not in own and name not in out:
            out.append(name)
    return tuple(out)


# ---------------------------------------------------------------- utilities

def walk(node):
    """Pre-order iterator over every Kernel node reachable from ``node``."""
    stack = [node]
    while stack:
        n = stack.pop()
        if isinstance(n, (tuple, list)):
            stack.extend(reversed(n))
            continue
        if not is_dataclass(n) or isinstance(n, (Loc,) + TYPE_CLASSES):
            continue
        yield n
        kids = []
        for f in fields(n):
            if f.name in ("loc", "spelling", "ty", "elem"):
                continue
            v = getattr(n, f.name)
            if isinstance(v, (tuple, list)) or (is_dataclass(v) and not isinstance(v, (Loc,) + TYPE_CLASSES)):
                kids.append(v)
        stack.extend(reversed(kids))


def residuals(node, include_cascade=False):
    """All Residual nodes, sorted by location (unlocated last)."""
    out = [n for n in walk(node) if isinstance(n, RESIDUALS)
           and (include_cascade or not getattr(n, "cascade", False))]

    def key(r):
        return (r.loc is None, r.loc.line if r.loc else 0, r.loc.col if r.loc else 0, r.reason)

    return sorted(out, key=key)


def subset_distance(program):
    """FR-254's metric: how many constructs keep ``program`` out of the subset."""
    return len(residuals(program))


# ---------------------------------------------------------------- dump

def dump(node, indent=0):
    """A stable, location-free textual dump (``--emit=kernel``)."""
    pad = "  " * indent
    if isinstance(node, Program):
        return "".join(dump(i, indent) for i in node.items)
    if isinstance(node, Func):
        ps = ", ".join("%s%s: %s" % ("mut " if p.mutable else "", p.name, p.ty) for p in node.params)
        s = "%sfunc %s%s%s(%s) -> %s\n" % (pad, "internal " if node.internal else "",
                                          ("%s " % node.recv) if node.recv else "", node.path, ps, node.ret)
        return s + dump(node.body, indent + 1)
    if isinstance(node, StructDef):
        s = "%sstruct %s%s\n" % (pad, "internal " if node.internal else "", node.path)
        s += "".join("%s  field %s: %s\n" % (pad, f.name, f.ty) for f in node.fields)
        return s + "".join(dump(m, indent + 1) for m in node.methods)
    if isinstance(node, Block):
        return pad + "block\n" + "".join(dump(s, indent + 1) for s in node.stmts)
    if isinstance(node, Let):
        return "%slet %s%s: %s = %s\n" % (pad, "mut " if node.mutable else "", node.name, node.ty, dump_expr(node.init))
    if isinstance(node, Assign):
        return "%sassign %s = %s\n" % (pad, node.name, dump_expr(node.value))
    if isinstance(node, CompoundAssign):
        return "%scompound %s %s= %s\n" % (pad, node.name, node.op, dump_expr(node.value))
    if isinstance(node, ExprStmt):
        return "%sexpr %s\n" % (pad, dump_expr(node.expr))
    if isinstance(node, Print):
        parts = []
        for sg in node.segs:
            if isinstance(sg, Text):
                parts.append("text %r" % sg.text)
            else:
                parts.append("spec(%s flag=%r width=%s prec=%s %s)" % (
                    sg.kind, sg.flag, sg.width, sg.prec, dump_expr(sg.arg)))
        return "%sprint [%s]\n" % (pad, ", ".join(parts))
    if isinstance(node, If):
        s = "%sif %s\n" % (pad, dump_expr(node.cond)) + dump(node.then, indent + 1)
        if node.els is not None:
            s += pad + "else\n" + dump(node.els, indent + 1)
        return s
    if isinstance(node, While):
        return "%swhile %s\n" % (pad, dump_expr(node.cond)) + dump(node.body, indent + 1)
    if isinstance(node, For):
        return "%sfor %s in %s .. %s\n" % (pad, node.var, dump_expr(node.lo), dump_expr(node.hi)) + dump(node.body, indent + 1)
    if isinstance(node, Break):
        return pad + "break\n"
    if isinstance(node, Continue):
        return pad + "continue\n"
    if isinstance(node, Return):
        return pad + "return" + ("" if node.value is None else " " + dump_expr(node.value)) + "\n"
    if isinstance(node, Store):
        return "%sstore %s = %s\n" % (pad, dump_expr(node.target), dump_expr(node.value))
    if isinstance(node, CompoundStore):
        return "%sstore %s %s= %s\n" % (pad, dump_expr(node.target), node.op, dump_expr(node.value))
    if isinstance(node, Push):
        return "%spush %s %s\n" % (pad, dump_expr(node.target), dump_expr(node.value))
    if isinstance(node, Pop):
        return "%spop %s\n" % (pad, dump_expr(node.target))
    if isinstance(node, Clear):
        return "%sclear %s\n" % (pad, dump_expr(node.target))
    if isinstance(node, Resize):
        return "%sresize %s %s %s\n" % (pad, dump_expr(node.target), dump_expr(node.count), dump_expr(node.value))
    if isinstance(node, PushStr):
        return "%spush_str %s %s\n" % (pad, dump_expr(node.target), dump_expr(node.value))
    if isinstance(node, PushByte):
        return "%spush_byte %s %s\n" % (pad, dump_expr(node.target), dump_expr(node.value))
    if isinstance(node, ForEach):
        return "%sforeach %s%s %s in %s\n" % (pad, "mut " if node.mutable else "", node.var, node.mode,
                                              dump_expr(node.iterable)) + dump(node.body, indent + 1)
    if isinstance(node, Closure):
        ps = ", ".join("%s%s: %s" % ("mut " if p.mutable else "", p.name, p.ty) for p in node.params)
        return "%sclosure %s%s(%s) -> %s [%s]\n" % (pad, "move " if node.move else "", node.name, ps, node.ret,
                                                  ", ".join(closure_captures(node))) + dump(node.body, indent + 1)
    if isinstance(node, (ResidualStmt, ResidualItem)):
        return "%sresidual %r\n" % (pad, node.reason)
    raise TypeError("dump: %r" % (node,))


def dump_expr(e):
    if isinstance(e, IntLit):
        return "%d:%s" % (e.value, e.ty)
    if isinstance(e, FloatLit):
        return e.text + ":f64"
    if isinstance(e, BoolLit):
        return "true" if e.value else "false"
    if isinstance(e, StrLit):
        return repr(e.value)
    if isinstance(e, Var):
        return e.name
    if isinstance(e, Unary):
        return "(%s %s)" % (e.op, dump_expr(e.operand))
    if isinstance(e, Binary):
        return "(%s %s %s)" % (e.op, dump_expr(e.lhs), dump_expr(e.rhs))
    if isinstance(e, Cast):
        return "(cast %s %s)" % (e.ty, dump_expr(e.operand))
    if isinstance(e, Call):
        return "(call %s%s)" % (e.name, "".join(" " + dump_expr(a) for a in e.args))
    if isinstance(e, Clone):
        return "(clone %s)" % dump_expr(e.operand)
    if isinstance(e, Index):
        return "(index %s %s)" % (dump_expr(e.base), dump_expr(e.index))
    if isinstance(e, ByteAt):
        return "(byte %s %s)" % (dump_expr(e.base), dump_expr(e.index))
    if isinstance(e, Len):
        return "(len %s)" % dump_expr(e.base)
    if isinstance(e, IsEmpty):
        return "(is_empty %s)" % dump_expr(e.base)
    if isinstance(e, VecLit):
        return "(vec %s [%s])" % (e.elem, ", ".join(dump_expr(x) for x in e.elems))
    if isinstance(e, VecFill):
        return "(vec_fill %s %s)" % (dump_expr(e.value), dump_expr(e.count))
    if isinstance(e, StrNew):
        return "(string_new)"
    if isinstance(e, StrFrom):
        return "(string_from %r)" % e.value
    if isinstance(e, Substr):
        return "(substr %s %s%s)" % (dump_expr(e.base), dump_expr(e.pos),
                                     "" if e.count is None else " " + dump_expr(e.count))
    if isinstance(e, ToStr):
        return "(to_string %s)" % dump_expr(e.operand)
    if isinstance(e, Borrow):
        return "(borrow%s %s)" % (" mut" if e.mutable else "", dump_expr(e.operand))
    if isinstance(e, Field):
        return "(field %s %s)" % (dump_expr(e.base), e.name)
    if isinstance(e, StructLit):
        return "(struct %s [%s])" % (e.path, ", ".join(dump_expr(x) for x in e.values))
    if isinstance(e, MethodCall):
        return "(method %s %s%s)" % (dump_expr(e.recv), e.name, "".join(" " + dump_expr(a) for a in e.args))
    if isinstance(e, Offset):
        return "(offset %s%s)" % (dump_expr(e.base), "" if e.start is None else " " + dump_expr(e.start))
    if isinstance(e, AddrOf):
        return "(addr_of %s)" % dump_expr(e.place)
    if isinstance(e, Cond):
        return "(cond %s %s %s)" % (dump_expr(e.cond), dump_expr(e.then), dump_expr(e.els))
    if isinstance(e, IncDec):
        return "(%s%s %s)" % ("post" if e.post else "pre", "inc" if e.op == "add" else "dec", dump_expr(e.var))
    if isinstance(e, ClosureCall):
        return "(call_closure %s%s)" % (e.name, "".join(" " + dump_expr(a) for a in e.args))
    if isinstance(e, ResidualExpr):
        return "(residual %r)" % e.reason
    raise TypeError("dump_expr: %r" % (e,))
