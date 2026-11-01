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
* Types are a CLOSED SUM (``Ty``).  FR-250 only has ``Prim``; later FRs add
  vectors/strings/closures as new variants of the same sum.
* ``Residual*`` nodes are the Surface-IR seam: cxx_import keeps going past an
  unsupported construct and leaves a located Residual in its place, so a
  program's distance from the subset is a COUNT, not a first failure
  (FR-254).  A program containing a Residual never reaches a printer.
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


Ty = Union[Prim]  # the closed sum; extended by later FRs

I8 = Prim("i8")
U8 = Prim("u8")
I32 = Prim("i32")
I64 = Prim("i64")
U32 = Prim("u32")
U64 = Prim("u64")
USIZE = Prim("usize")
F64 = Prim("f64")
BOOL = Prim("bool")
UNIT = Prim("()")
STR = Prim("&str")  # only the type of a printf %s string-literal argument

ALL_PRIMS = (I8, U8, I32, I64, U32, U64, USIZE, F64, BOOL)
INT_TYPES = (I8, U8, I32, I64, U32, U64, USIZE)
# The types arithmetic happens in after C++ integer promotion.
ARITH_INT = (I32, I64, U32, U64, USIZE)
ARITH = ARITH_INT + (F64,)
UNSIGNED = (U8, U32, U64, USIZE)
SIGNED = (I8, I32, I64)

INT_RANGE = {
    "i8": (-(1 << 7), (1 << 7) - 1),
    "u8": (0, (1 << 8) - 1),
    "i32": (-(1 << 31), (1 << 31) - 1),
    "i64": (-(1 << 63), (1 << 63) - 1),
    "u32": (0, (1 << 32) - 1),
    "u64": (0, (1 << 64) - 1),
    "usize": (0, (1 << 64) - 1),
}

PRIM_BY_NAME = {p.name: p for p in ALL_PRIMS + (UNIT,)}


def is_int(t):
    return t in INT_TYPES


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


Expr = Union[IntLit, FloatLit, BoolLit, StrLit, Var, Unary, Binary, Cast, Call,
             ResidualExpr]


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
    """A call evaluated for effect (its value, if any, discarded)."""

    expr: Call
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


Stmt = Union[Block, Let, Assign, CompoundAssign, ExprStmt, Print, If, While,
             For, Break, Continue, Return, ResidualStmt]


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


@dataclass(frozen=True)
class ResidualItem:
    reason: str
    loc: Optional[Loc] = _loc()


@dataclass(frozen=True)
class Program:
    items: Tuple[Union[Func, ResidualItem], ...]

    def funcs(self):
        return [f for f in self.items if isinstance(f, Func)]


RESIDUALS = (ResidualExpr, ResidualStmt, ResidualItem)


# ---------------------------------------------------------------- utilities

def walk(node):
    """Pre-order iterator over every Kernel node reachable from ``node``."""
    stack = [node]
    while stack:
        n = stack.pop()
        if isinstance(n, (tuple, list)):
            stack.extend(reversed(n))
            continue
        if not is_dataclass(n) or isinstance(n, (Loc, Prim)):
            continue
        yield n
        kids = []
        for f in fields(n):
            if f.name in ("loc", "spelling", "ty"):
                continue
            v = getattr(n, f.name)
            if isinstance(v, (tuple, list)) or (is_dataclass(v) and not isinstance(v, (Loc, Prim))):
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
        s = "%sfunc %s(%s) -> %s\n" % (pad, node.name, ps, node.ret)
        return s + dump(node.body, indent + 1)
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
    if isinstance(e, ResidualExpr):
        return "(residual %r)" % e.reason
    raise TypeError("dump_expr: %r" % (e,))
