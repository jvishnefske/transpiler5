"""Kernel -> canonical Rust (FR-250).

Canonical choices (each the ONLY spelling the printer emits; rs_parse
accepts it and a few trivially-equivalent ones):

* `let mut x: T = e;` / `let x: T = e;` -- always typed, always initialized.
* parameters: `mut x: T` (C++ `T x`) or `x: T` (C++ `const T x`).
* integer literals carry their type suffix except i32 (`5`, `5i64`,
  `5u32`, `5u64`, `5usize`, `5i8`, `5u8`); inside the operand of a cast
  (up to a call boundary) i32 literals are suffixed too (`(-74i32) as u8`),
  because rustc infers an unsuffixed literal from the cast target; f64 literals are Python's
  shortest repr (`2.5`, `1e+300`); negative literals are unary minus.
* unsigned + - * and unary minus: `a.wrapping_add(b)`, `.wrapping_sub`,
  `.wrapping_mul`, `.wrapping_neg()`; / % are plain (they cannot wrap).
* compound assignment: `x op= e;` except unsigned + - *, which is
  `x = x.wrapping_add(e);` (Kernel normal form N5 makes that unambiguous).
* casts: `e as T`; the operand is parenthesized unless it is a literal,
  variable, call or method call, and a cast that is an operand of a binary
  operator is parenthesized (`(x as i64) < y` -- `x as i64 < y` is a
  rustc parse error).
* a binary operand that is itself a binary operation is parenthesized
  unless it is the LEFT operand of the same left-associative operator
  (comparisons never chain).  Unary operands are parenthesized unless
  atomic.
* shifts: `a << b` with the amount in whatever integer type the Kernel has
  (std implements Shl<U> for every integer pair, so no cast is canonical).
* `print!(..)` only; see fmt.py for the printf mapping and its probe.
* `mod bx { .. }` (safe helpers) is appended iff a helper is used.

FR-251 rows (types: `Vec<T>`, `String`, and for reference parameters and
range-for variables `&T` / `&mut T` -- `const std::vector<T>&` is
`&Vec<T>`, NOT `&[T]`: the type constructors stay one-to-one, a slice is
`std::span`'s row; clippy's ptr_arg lint prefers the slice but it is a
style lint, not a typing rule):

* a reference variable is written `*x` in a VALUE position (scalar
  arithmetic, comparisons, assignment targets `*x = e;`) and bare `x` as a
  method receiver, an index base, a print! argument, or an argument to
  another reference parameter (auto-deref / reborrow).  `*x` is never an
  atomic operand: `(*x).wrapping_add(1u32)`, `-(*x)`, `(*x) as i64`.
* consuming positions (let/assign value, by-value argument, return, push):
  Clone(x) is `x.clone()`, a bare owned variable is a move `x`.
* copied positions (vec! elements, vec! fill value, resize value): a
  non-Copy place gets `.clone()`, a reference to a Copy scalar `*x`.
* borrowed positions taking `&str`/`&T` (string `+` rhs, push_str,
  bx::substr base, reference arguments): an owned variable or element is
  `&x` / `&mut x`, a reference variable bare, a temporary `&(e)` (or `&e`
  when atomic), a string literal bare.
* the empty vector literal is `Vec::new()` as a let/assign/return value
  (typed by the context) and `Vec::<T>::new()` everywhere else, where
  rustc could not infer T (`Vec::new().len()` is E0282); `vec![a, b]`, `vec![x; n]`,
  `String::new()`, `String::from("..")`, `x.len()`, `x.is_empty()`,
  `s.as_bytes()[i] as i8` (std::string's char is SIGNED), `x.to_string()`,
  `bx::substr(&s, p, n)`, `bx::substr_from(&s, p)`,
  `bx::push_byte(&mut s, c)` (panics on a byte >= 0x80: the ASCII
  invariant), `s.push_str(..)`, `v.push(..)`, `v.pop();`, `v.clear();`,
  `v.resize(n, x);`, `for x in v.iter().copied()` (`.cloned()` for a
  non-Copy element; `for mut x` when the C++ copy is non-const),
  `for x in v.iter()`, `for x in v.iter_mut()`.

FR-257 rows (structs): `#[derive(Clone)] struct S { pub f: T, .. }`
(`pub struct` in a module unless internal), then `impl S { pub fn .. }`
with `&self` / `&mut self` / no receiver; `pub fn new(..) -> S { return
S { .. }; }` for a constructor; a struct of the same module is bare, any
other `crate::a::S`; `S { f: e, g }` (field-init shorthand exactly when the
value is a non-reference variable of the field's name); `S::f(..)`,
`r.m(..)`, `p.f`, `self.f`.

FR-261 rows (pointers): `const T*` <=> `&[T]`, `T*` <=> `&mut [T]` (the
suffix slice of the allocation; lengths stay ordinary integers); `p[i]`,
`*p` <=> `p[0usize]`, `p->f` <=> `p[0usize].f`, `p + k` <=> `&p[k..]` /
`&mut p[k..]` and `v.data()` <=> `&v[..]` (the `&` / `&mut` follows the
expected type, not the base), `&x` <=> `std::slice::from_ref(&x)` /
`std::slice::from_mut(&mut x)`, `p += k;` <=> `p = &p[k..];` (measured to
borrow-check for `&mut` parameters and locals alike on rustc 1.96 and
1.98), a pointer argument or `let` initializer that is a pointer variable
bare (`&[T]` is Copy; `&mut [T]` is implicitly reborrowed at a coercion
site).

FR-259 rows: `i16` / `u16`; `c ? a : b` <=> `if c { a } else { b }` (a `?:`
in the else arm is `else if`; parenthesized as an operand of a unary,
binary, cast or method call, bare elsewhere -- measured on rustc 1.96 to
parse and lint clean as a let/assign/return value, an argument, an index,
an `if`/`while` condition and a range bound); `++x` / `x++` / `--x` / `x--`
used as a value <=> `{ x += 1; x }` / `{ x += 1; x - 1 }` / `{ x -= 1; x }` /
`{ x -= 1; x + 1 }` (`*x` for a reference; unsigned `{ x =
x.wrapping_add(1u32); x.wrapping_sub(1u32) }`), the literal always of the
variable's type, parenthesized exactly where a `?:` is.

FR-260 rows (namespaces): a C++ named namespace is a `mod`, one block per
namespace (N13), contents indented one level; a nested one is `pub mod`
(a private nested module is unreachable from the root, E0603).  A function
in a module is `pub fn`, or plain `fn` when it is internal (a C++
anonymous-namespace member).  A call to a function of the SAME module is
bare `f(..)`; every other call is absolute, `crate::a::b::f(..)` (C++
`::a::b::f(..)`) -- both languages then resolve it with no dependence on
declaration order, enclosing-scope search, or using-directives.  Inside a
module the safe helpers are `crate::bx::..` (`bx::` resolves relative to
the current module).

FR-252 rows (closures): `let f = move |mut a: i32| -> i32 {` .. `};` (`move`
for by-copy captures; `let mut f` when it modifies state or is passed as
`&mut impl FnMut`; `|| ->` with no parameter; no `-> ()`), `f(a)`; a
callable parameter `f: &impl Fn(i32) -> i32` / `f: &mut impl FnMut(i32)`,
an argument `&add` / `&mut bump` (a callable parameter passed on bare).

FR-253 rows (algorithms over whole vectors; a closure argument is `&f`, or
`&mut f` when f modifies state): `v.sort_unstable();` (std::sort),
`v.sort();` (std::stable_sort), `v.reverse();`, `v.fill(x);` (`.clone()` of
a String place), `bx::sort_by_less(&mut v, &cmp);` (libstdc++'s introsort),
`bx::transform(&mut v, &f);`, `v.iter().copied().for_each(&f);`,
`w.extend(v.iter().copied().map(&f));`; values `v.contains(&x)` (`!v.contains
(&x)` for `== v.end()`), `bx::count(&v, &x)` / `bx::count_if(&v, &p)` (i64,
C++'s ptrdiff_t), `v.iter().copied().any(&p)` / `.all(&p)` (`!..any(&p)` for
none_of), `v.iter().fold(init, |a, &e| a + e)` (`a.wrapping_add(e)` for an
unsigned accumulator, `e as A` when the element type differs),
`v.iter().copied().fold(init, &f)`, `*v.iter().max().unwrap()` /
`.min()`, `std::cmp::max(a, b)` / `std::cmp::min(a, b)`.
"""

from . import fmt
from . import kernel as K
from . import typecheck as TC
from .typecheck import is_place

RUST_BINOP = {
    "add": "+", "sub": "-", "mul": "*", "div": "/", "rem": "%",
    "band": "&", "bor": "|", "bxor": "^", "shl": "<<", "shr": ">>",
    "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">=",
    "and": "&&", "or": "||",
}


class PrintError(Exception):
    pass


def rs_ty(t, ns=()):
    if isinstance(t, K.VecT):
        return "Vec<%s>" % rs_ty(t.elem, ns)
    if isinstance(t, K.StrT):
        return "String"
    if isinstance(t, K.RefT) and isinstance(t.inner, K.FnT):
        # FR-252: `const F& f` <=> `&impl Fn(..)`, `F& f` <=> `&mut impl FnMut(..)`
        return "&%simpl %s(%s)%s" % ("mut " if t.mutable else "", "FnMut" if t.mutable else "Fn",
                                     ", ".join(rs_ty(p, ns) for p in t.inner.params),
                                     "" if t.inner.ret == K.UNIT else " -> " + rs_ty(t.inner.ret, ns))
    if isinstance(t, K.RefT):
        return "&%s%s" % ("mut " if t.mutable else "", rs_ty(t.inner, ns))
    if isinstance(t, K.StructT):
        return rs_path(t.path, ns)
    if isinstance(t, K.PtrT):
        return "&%s[%s]" % ("mut " if t.mutable else "", rs_ty(t.inner, ns))
    return t.name


def rs_path(path, ns):
    """FR-257 (FR-260's rule for items): a struct of the SAME module is
    bare, any other is `crate::`-absolute."""
    sns, leaf = K.split_path(path)
    return leaf if sns == tuple(ns) else "crate::" + path


def int_lit(e, in_cast=False):
    if e.ty == K.I32 and not in_cast:
        return str(e.value)
    return "%d%s" % (e.value, e.ty.name)


class _Printer:
    def __init__(self, program):
        self.sigs = TC.sigs_of(program)
        self.lines = []
        self.used = set()
        # >0 while printing the operand of a Cast: rustc infers an
        # unsuffixed literal there from the cast TARGET (`-74 as u8` makes
        # 74 a u8), so i32 literals are suffixed `74i32` in that context.
        self.in_cast = 0
        self.ns = ()  # the namespace of the function being printed
        self.base = 0  # module nesting depth (indentation)

    @property
    def bxp(self):
        return "crate::bx::" if self.ns else "bx::"

    def callee(self, path):
        """FR-260: bare in the same module, otherwise `crate::`-absolute.
        FR-257: an associated function (static member / `new`) is `S::f`
        when S is in the same module."""
        if TC.recv_of(self.sigs).get(path):
            sp, leaf = path.rsplit("::", 1)
            return rs_path(sp, self.ns) + "::" + leaf
        ns, leaf = K.split_path(path)
        if ns == self.ns:
            return leaf
        return "crate::" + path

    def rty(self, t):
        return rs_ty(t, self.ns)

    def ty(self, e, scope):
        return TC.type_of(e, scope, self.sigs)

    def is_ref(self, e, scope):
        if not isinstance(e, K.Var):
            return False
        v = scope.lookup(e.name)
        return v is not None and isinstance(v[0], K.RefT)

    def atomic(self, e, scope):
        if isinstance(e, K.Var):
            return not self.is_ref(e, scope)
        if isinstance(e, (K.IntLit, K.FloatLit, K.BoolLit, K.ConstRef, K.Call, K.ClosureCall, K.StrLit, K.Clone,
                          K.Index, K.Len, K.IsEmpty, K.VecLit, K.VecFill, K.StrNew, K.StrFrom, K.Substr, K.ToStr, K.Field,
                          K.MethodCall, K.StructLit, K.MinMax)):
            return True
        if isinstance(e, K.Alg):
            return e.op not in ("max_element", "min_element")  # `*v.iter().max().unwrap()`
        if isinstance(e, K.Binary) and self.is_wrapping(e, scope):
            return True
        if isinstance(e, K.Unary) and e.op == "neg" and K.is_unsigned(self.ty(e.operand, scope)):
            return True
        return False

    def is_wrapping(self, e, scope):
        return e.op in K.WRAPPING_OPS and K.is_unsigned(self.ty(e.lhs, scope))

    def paren_if(self, cond, s):
        return "(" + s + ")" if cond else s

    def expr(self, e, scope):
        if isinstance(e, K.IntLit):
            return int_lit(e, self.in_cast > 0)
        if isinstance(e, K.FloatLit):
            return e.text
        if isinstance(e, K.BoolLit):
            return "true" if e.value else "false"
        if isinstance(e, K.StrLit):
            return '"' + fmt.rust_escape(e.value, False) + '"'
        if isinstance(e, K.Var):
            return "*" + e.name if self.is_ref(e, scope) else e.name
        if isinstance(e, K.ConstRef):
            return rs_path(e.path, self.ns)  # FR-264: bare in its module, else `crate::`
        if isinstance(e, K.ClosureCall):
            saved, self.in_cast = self.in_cast, 0
            try:
                return "%s(%s)" % (e.name, ", ".join(self.expr(a, scope) for a in e.args))
            finally:
                self.in_cast = saved
        if isinstance(e, K.Call):
            saved, self.in_cast = self.in_cast, 0
            try:
                sig = self.sigs.get(e.name)
                return "%s(%s)" % (self.callee(e.name), ", ".join(self.arg(pt, a, scope)
                                                                 for pt, a in zip(sig[0], e.args)))
            finally:
                self.in_cast = saved
        if isinstance(e, (K.Clone, K.Index, K.ByteAt, K.Len, K.IsEmpty, K.VecLit, K.VecFill, K.StrNew,
                          K.StrFrom, K.Substr, K.ToStr, K.Field, K.StructLit, K.MethodCall)):
            saved, self.in_cast = self.in_cast, 0
            try:
                return self.nc_expr(e, scope)
            finally:
                self.in_cast = saved
        if isinstance(e, (K.Alg, K.MinMax)):
            saved, self.in_cast = self.in_cast, 0
            try:
                return self.alg(e, scope)
            finally:
                self.in_cast = saved
        if isinstance(e, K.Cast):
            self.in_cast += 1
            try:
                inner = self.expr(e.operand, scope)
            finally:
                self.in_cast -= 1
            return "%s as %s" % (self.paren_if(not self.atomic(e.operand, scope), inner), e.ty.name)
        if isinstance(e, K.Cond):
            return self.cond(e, scope)
        if isinstance(e, K.IncDec):
            saved, self.in_cast = self.in_cast, 0
            try:
                return self.incdec(e, scope)
            finally:
                self.in_cast = saved
        if isinstance(e, K.Unary):
            t = self.ty(e.operand, scope)
            inner = self.expr(e.operand, scope)
            if e.op == "neg" and K.is_unsigned(t):
                return "%s.wrapping_neg()" % self.paren_if(not self.atomic(e.operand, scope), inner)
            sym = "-" if e.op == "neg" else "!"
            return sym + self.paren_if(not self.atomic(e.operand, scope), inner)
        if isinstance(e, K.Binary):
            if self.is_wrapping(e, scope):
                recv = self.paren_if(not self.atomic(e.lhs, scope), self.expr(e.lhs, scope))
                return "%s.wrapping_%s(%s)" % (recv, e.op, self.expr(e.rhs, scope))
            lt = self.ty(e.lhs, scope)
            if lt == K.STRING and e.op == "add":
                return "%s + %s" % (self.operand(e, e.lhs, scope, True, consume=True),
                                    self.borrow(e.rhs, scope, False))
            return "%s %s %s" % (self.operand(e, e.lhs, scope, True), RUST_BINOP[e.op],
                                 self.operand(e, e.rhs, scope, False))
        raise PrintError("cannot print %r" % (e,))

    # ---------------------------------------------------- FR-253
    def fn_arg(self, a):
        """A closure argument: `&f`, `&mut f` when it modifies state."""
        return ("&mut " if a.name in self.fnmut else "&") + a.name

    def alg(self, e, scope):
        if isinstance(e, K.MinMax):
            return "std::cmp::%s(%s, %s)" % (e.op, self.expr(e.lhs, scope), self.expr(e.rhs, scope))
        v = self.recv(e.src, scope)
        if e.op == "contains":
            return "%s.contains(%s)" % (v, self.borrow(e.args[0], scope, False))
        if e.op == "count":
            self.used.add("count")
            return self.bxp + "count(%s, %s)" % (self.borrow(e.src, scope, False), self.borrow(e.args[0], scope, False))
        if e.op == "count_if":
            self.used.add("count_if")
            return self.bxp + "count_if(%s, %s)" % (self.borrow(e.src, scope, False), self.fn_arg(e.args[0]))
        if e.op in ("any_of", "all_of"):
            return "%s.iter().copied().%s(%s)" % (v, e.op[:3], self.fn_arg(e.args[0]))
        if e.op in ("max_element", "min_element"):
            return "*%s.iter().%s().unwrap()" % (v, e.op[:3])
        if e.op == "accumulate":
            init = self.expr(e.args[0], scope)
            if len(e.args) == 2:
                return "%s.iter().copied().fold(%s, %s)" % (v, init, self.fn_arg(e.args[1]))
            A = self.ty(e, scope)
            E = self.ty(e.src, scope).elem
            wrapping, cast = TC.fold_spelling(A, E)
            el = "e as " + cast if cast else "e"
            body = "a.wrapping_add(%s)" % el if wrapping else "a + " + el
            return "%s.iter().fold(%s, |a, &e| %s)" % (v, init, body)
        raise PrintError("cannot print %r" % (e,))

    def alg_stmt(self, s, scope, d):
        if s.op in ("sort", "stable_sort", "reverse"):
            m = {"sort": "sort_unstable", "stable_sort": "sort", "reverse": "reverse"}[s.op]
            self.emit(d, "%s.%s();" % (self.recv(s.target, scope), m))
        elif s.op == "fill":
            self.emit(d, "%s.fill(%s);" % (self.recv(s.target, scope), self.copied(s.args[0], scope)))
        elif s.op in ("sort_by", "transform"):
            h = "sort_by_less" if s.op == "sort_by" else "transform"
            self.used.add(h)
            self.emit(d, self.bxp + "%s(%s, %s);" % (h, self.borrow(s.target, scope, True), self.fn_arg(s.args[0])))
        elif s.op == "for_each":
            self.emit(d, "%s.iter().copied().for_each(%s);" % (self.recv(s.src, scope), self.fn_arg(s.args[0])))
        elif s.op == "extend_map":
            self.emit(d, "%s.extend(%s.iter().copied().map(%s));" % (self.recv(s.target, scope), self.recv(s.src, scope),
                                                                      self.fn_arg(s.args[0])))
        else:
            raise PrintError("cannot print %r" % (s,))

    # ---------------------------------------------------- FR-259
    def cond(self, e, scope):
        """`if c { a } else { b }`; a Cond in the else arm is `else if`.  The
        condition never sits in a cast operand (it is bool); the arms keep
        the cast context (rustc types their literals from the cast)."""
        saved, self.in_cast = self.in_cast, 0
        try:
            c = self.expr(e.cond, scope)
        finally:
            self.in_cast = saved
        s = "if %s { %s }" % (c, self.expr(e.then, scope))
        if isinstance(e.els, K.Cond):
            return s + " else " + self.cond(e.els, scope)
        return s + " else { %s }" % self.expr(e.els, scope)

    def incdec(self, e, scope):
        t = self.ty(e, scope)
        x = self.expr(e.var, scope)
        one = int_lit(K.IntLit(1, t))
        inv = "sub" if e.op == "add" else "add"
        if K.is_unsigned(t):
            step = "%s = %s.wrapping_%s(%s)" % (x, self.paren_if(not self.atomic(e.var, scope), x), e.op, one)
            tail = "%s.wrapping_%s(%s)" % (self.paren_if(not self.atomic(e.var, scope), x), inv, one) if e.post else x
        else:
            step = "%s %s= %s" % (x, RUST_BINOP[e.op], one)
            tail = "%s %s %s" % (x, RUST_BINOP[inv], one) if e.post else x
        return "{ %s; %s }" % (step, tail)

    # ---------------------------------------------------- FR-251 positions
    def recv(self, e, scope):
        """A method receiver / index base: references auto-deref."""
        if isinstance(e, K.Var):
            return e.name
        s = self.expr(e, scope)
        return self.paren_if(not self.atomic(e, scope), s)

    def typed(self, e, scope):
        """A value whose type the context fixes (let/assign/return)."""
        if isinstance(e, K.VecLit) and not e.elems:
            return "Vec::new()"
        return self.expr(e, scope)

    def consume(self, e, scope):
        """A consuming position: Clone is `.clone()`, a bare owned var moves."""
        return self.expr(e, scope)

    def copied(self, e, scope):
        """A COPIED position (C++ copies from `const T&`)."""
        t = self.ty(e, scope)
        if not K.is_copy(t) and is_place(e):
            return self.recv(e, scope) + ".clone()"
        return self.expr(e, scope)

    def borrow(self, e, scope, mutable):
        """A position taking `&T` / `&mut T` / `&str`."""
        if isinstance(e, K.StrLit):
            return self.expr(e, scope)
        if self.is_ref(e, scope):
            return e.name
        amp = "&mut " if mutable else "&"
        if is_place(e):
            return amp + self.recv(e, scope)
        s = self.expr(e, scope)
        return amp + self.paren_if(not self.atomic(e, scope), s)

    def arg(self, pt, a, scope):
        if isinstance(pt, K.RefT):
            return self.borrow(a, scope, pt.mutable)
        if isinstance(pt, K.PtrT):
            return self.ptr_val(a, scope, pt.mutable)
        return self.expr(a, scope)

    def ptr_val(self, e, scope, mutable):
        """FR-261: a pointer value where a `&[T]` (``mutable`` False) or
        `&mut [T]` is expected: a pointer variable bare (copied / implicitly
        reborrowed), `&p[k..]` / `&mut v[..]`, `std::slice::from_ref(&x)` /
        `std::slice::from_mut(&mut x)`."""
        if isinstance(e, K.Offset):
            saved, self.in_cast = self.in_cast, 0
            try:
                start = "" if e.start is None else self.expr(e.start, scope)
                return "&%s%s[%s..]" % ("mut " if mutable else "", self.recv(e.base, scope), start)
            finally:
                self.in_cast = saved
        if isinstance(e, K.AddrOf):
            return "std::slice::%s(%s)" % ("from_mut" if mutable else "from_ref",
                                           self.borrow(e.place, scope, mutable))
        return self.expr(e, scope)

    def nc_expr(self, e, scope):
        if isinstance(e, K.Clone):
            return self.recv(e.operand, scope) + ".clone()"
        if isinstance(e, K.Field):
            return "%s.%s" % (self.recv(e.base, scope), e.name)
        if isinstance(e, K.StructLit):
            sd = TC.structs_of(self.sigs)[e.path]
            parts = []
            for fd, v in zip(sd.fields, e.values):
                if isinstance(v, K.Var) and v.name == fd.name and not self.is_ref(v, scope):
                    parts.append(fd.name)  # field-init shorthand
                else:
                    parts.append("%s: %s" % (fd.name, self.typed(v, scope)))
            head = rs_path(e.path, self.ns)
            return "%s { %s }" % (head, ", ".join(parts)) if parts else head + " {}"
        if isinstance(e, K.MethodCall):
            rt = self.ty(e.recv, scope)
            sig = self.sigs[TC.method_path(rt.path, e.name)]
            return "%s.%s(%s)" % (self.recv(e.recv, scope), e.name,
                                  ", ".join(self.arg(pt, a, scope) for pt, a in zip(sig[0], e.args)))
        if isinstance(e, K.Index):
            return "%s[%s]" % (self.recv(e.base, scope), self.expr(e.index, scope))
        if isinstance(e, K.ByteAt):
            return "%s.as_bytes()[%s] as i8" % (self.recv(e.base, scope), self.expr(e.index, scope))
        if isinstance(e, K.Len):
            return self.recv(e.base, scope) + ".len()"
        if isinstance(e, K.IsEmpty):
            return self.recv(e.base, scope) + ".is_empty()"
        if isinstance(e, K.VecLit):
            if not e.elems:
                return "Vec::<%s>::new()" % self.rty(e.elem)
            return "vec![%s]" % ", ".join(self.copied(x, scope) for x in e.elems)
        if isinstance(e, K.VecFill):
            return "vec![%s; %s]" % (self.copied(e.value, scope), self.expr(e.count, scope))
        if isinstance(e, K.StrNew):
            return "String::new()"
        if isinstance(e, K.StrFrom):
            return 'String::from("%s")' % fmt.rust_escape(e.value, False)
        if isinstance(e, K.Substr):
            if e.count is None:
                self.used.add("substr_from")
                return self.bxp + "substr_from(%s, %s)" % (self.borrow(e.base, scope, False), self.expr(e.pos, scope))
            self.used.add("substr")
            return self.bxp + "substr(%s, %s, %s)" % (self.borrow(e.base, scope, False), self.expr(e.pos, scope),
                                               self.expr(e.count, scope))
        if isinstance(e, K.ToStr):
            return self.recv(e.operand, scope) + ".to_string()"
        raise PrintError("cannot print %r" % (e,))

    def operand(self, parent, child, scope, is_lhs, consume=False):
        s = self.expr(child, scope)
        if isinstance(child, (K.Cast, K.ByteAt, K.Cond, K.IncDec)):
            return "(" + s + ")"
        if isinstance(child, K.Binary) and not self.is_wrapping(child, scope):
            same = child.op == parent.op and is_lhs and parent.op not in K.CMP_OPS
            return s if same else "(" + s + ")"
        return s

    # -------------------------------------------------------------- stmts

    def emit(self, depth, text):
        self.lines.append("    " * (self.base + depth) + text)

    def block_body(self, b, scope, depth, same=False):
        inner = scope if same else scope.child()
        for s in b.stmts:
            self.stmt(s, inner, depth)

    def stmt(self, s, scope, d):
        if isinstance(s, K.Block):
            self.emit(d, "{")
            self.block_body(s, scope, d + 1)
            self.emit(d, "}")
        elif isinstance(s, K.Closure):
            # FR-252: `let f = move |a: i32| -> i32 {` .. `};` (`let mut f`
            # when it modifies state or is passed as `&mut impl FnMut`)
            ps = ", ".join("%s%s: %s" % ("mut " if p.mutable else "", p.name, self.rty(p.ty)) for p in s.params)
            ret = "" if s.ret == K.UNIT else " -> " + self.rty(s.ret)
            mut = s.name in self.mut_closures
            self.emit(d, "let %s%s = %s|%s|%s {" % ("mut " if mut else "", s.name, "move " if s.move else "",
                                                     ps, ret))
            inner = scope.child()
            for p in s.params:
                inner.declare(p.name, p.ty, p.mutable)
            self.block_body(s.body, inner, d + 1, same=True)
            self.emit(d, "};")
            scope.declare(s.name, K.FnT(tuple(p.ty for p in s.params), s.ret), mut)
        elif isinstance(s, K.Let):
            init = self.ptr_val(s.init, scope, s.ty.mutable) if isinstance(s.ty, K.PtrT) else self.typed(s.init, scope)
            self.emit(d, "let %s%s: %s = %s;" % ("mut " if s.mutable else "", s.name, self.rty(s.ty), init))
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, K.Assign):
            self.emit(d, "%s = %s;" % (self.expr(K.Var(s.name), scope), self.typed(s.value, scope)))
        elif isinstance(s, K.CompoundAssign):
            t = K.deref(scope.lookup(s.name)[0])
            lhs = self.expr(K.Var(s.name), scope)
            if isinstance(t, K.PtrT):
                # FR-261: `p += k;` <=> `p = &p[k..];` (a forward re-slice)
                self.emit(d, "%s = %s;" % (s.name, self.ptr_val(K.Offset(K.Var(s.name), s.value), scope, t.mutable)))
            elif s.op in K.WRAPPING_OPS and K.is_unsigned(t):
                recv = self.paren_if(not self.atomic(K.Var(s.name), scope), lhs)
                self.emit(d, "%s = %s.wrapping_%s(%s);" % (lhs, recv, s.op, self.expr(s.value, scope)))
            else:
                self.emit(d, "%s %s= %s;" % (lhs, RUST_BINOP[s.op], self.expr(s.value, scope)))
        elif isinstance(s, K.Store):
            self.emit(d, "%s = %s;" % (self.expr(s.target, scope), self.expr(s.value, scope)))
        elif isinstance(s, K.CompoundStore):
            t = self.ty(s.target, scope)
            lhs = self.expr(s.target, scope)
            if s.op in K.WRAPPING_OPS and K.is_unsigned(t):
                self.emit(d, "%s = %s.wrapping_%s(%s);" % (lhs, lhs, s.op, self.expr(s.value, scope)))
            else:
                self.emit(d, "%s %s= %s;" % (lhs, RUST_BINOP[s.op], self.expr(s.value, scope)))
        elif isinstance(s, K.Push):
            self.emit(d, "%s.push(%s);" % (self.recv(s.target, scope), self.consume(s.value, scope)))
        elif isinstance(s, K.Pop):
            self.emit(d, "%s.pop();" % self.recv(s.target, scope))
        elif isinstance(s, K.Clear):
            self.emit(d, "%s.clear();" % self.recv(s.target, scope))
        elif isinstance(s, K.Resize):
            self.emit(d, "%s.resize(%s, %s);" % (self.recv(s.target, scope), self.expr(s.count, scope),
                                                 self.copied(s.value, scope)))
        elif isinstance(s, K.PushStr):
            self.emit(d, "%s.push_str(%s);" % (self.recv(s.target, scope), self.borrow(s.value, scope, False)))
        elif isinstance(s, K.PushByte):
            self.used.add("push_byte")
            self.emit(d, self.bxp + "push_byte(%s, %s);" % (self.borrow(s.target, scope, True),
                                                     self.expr(s.value, scope)))
        elif isinstance(s, K.ForEach):
            t = self.ty(s.iterable, scope)
            it = self.recv(s.iterable, scope)
            inner = scope.child()
            if s.mode == "copy":
                it += ".iter().copied()" if K.is_copy(t.elem) else ".iter().cloned()"
                inner.declare(s.var, t.elem, s.mutable)
            else:
                it += ".iter()" if s.mode == "ref" else ".iter_mut()"
                inner.declare(s.var, K.RefT(t.elem, s.mode == "mut"), False)
            self.emit(d, "for %s%s in %s {" % ("mut " if s.mutable else "", s.var, it))
            self.block_body(s.body, inner, d + 1, same=True)
            self.emit(d, "}")
        elif isinstance(s, K.AlgStmt):
            self.alg_stmt(s, scope, d)
        elif isinstance(s, K.ExprStmt):
            self.emit(d, self.expr(s.expr, scope) + ";")
        elif isinstance(s, K.Print):
            text = []
            args = []
            for sg in s.segs:
                if isinstance(sg, K.Text):
                    text.append(fmt.rust_escape(sg.text, True))
                    continue
                text.append(fmt.rust_placeholder(sg))
                if sg.kind == "s" and self.is_ref(sg.arg, scope):
                    a = sg.arg.name  # Display through the reference
                else:
                    a = self.expr(sg.arg, scope)
                if sg.kind == "f":
                    self.used.add("fmt_f")
                    a = self.bxp + "fmt_f(%s, %d)" % (a, sg.prec)
                elif sg.kind == "c":
                    self.used.add("chr")
                    a = self.bxp + "chr(%s)" % a
                args.append(a)
            self.emit(d, 'print!("%s"%s);' % ("".join(text), "".join(", " + a for a in args)))
        elif isinstance(s, K.If):
            self.if_chain(s, scope, d, "if")
        elif isinstance(s, K.While):
            self.emit(d, "while %s {" % self.expr(s.cond, scope))
            self.block_body(s.body, scope, d + 1)
            self.emit(d, "}")
        elif isinstance(s, K.For):
            t = self.ty(s.lo, scope)
            self.emit(d, "for %s in %s..%s {" % (s.var, self.expr(s.lo, scope), self.expr(s.hi, scope)))
            inner = scope.child()
            inner.declare(s.var, t, False)
            self.block_body(s.body, inner, d + 1, same=True)
            self.emit(d, "}")
        elif isinstance(s, K.Break):
            self.emit(d, "break;")
        elif isinstance(s, K.Continue):
            self.emit(d, "continue;")
        elif isinstance(s, K.Return):
            if s.value is None:
                self.emit(d, "return;")
            else:
                self.emit(d, "return %s;" % self.typed(s.value, scope))
        else:
            raise PrintError("cannot print %r" % (s,))

    def if_chain(self, s, scope, d, kw):
        self.emit(d, "%s %s {" % (kw, self.expr(s.cond, scope)))
        self.block_body(s.then, scope, d + 1)
        e = s.els
        if e is None:
            self.emit(d, "}")
            return
        if len(e.stmts) == 1 and isinstance(e.stmts[0], K.If):
            self.emit_join(d, e.stmts[0], scope.child())
            return
        self.emit(d, "} else {")
        self.block_body(e, scope, d + 1)
        self.emit(d, "}")

    def emit_join(self, d, s, scope):
        # `} else if cond {`
        self.emit(d, "} else if %s {" % self.expr(s.cond, scope))
        self.block_body(s.then, scope, d + 1)
        e = s.els
        if e is None:
            self.emit(d, "}")
            return
        if len(e.stmts) == 1 and isinstance(e.stmts[0], K.If):
            self.emit_join(d, e.stmts[0], scope.child())
            return
        self.emit(d, "} else {")
        self.block_body(e, scope, d + 1)
        self.emit(d, "}")

    def func(self, f):
        scope = TC.Scope()
        self.mut_closures = TC.mut_closure_names(f, self.sigs)
        self.fnmut = TC.closure_info(f.body, self.sigs)[1]  # FR-253: `&mut f` arguments
        ps = []
        if f.recv in ("ref", "mut"):
            ps.append("&mut self" if f.recv == "mut" else "&self")
            scope.declare("self", K.RefT(K.StructT(K.path_of(f.ns, f.owner)), f.recv == "mut"), False)
        for p in f.params:
            ps.append("%s%s: %s" % ("mut " if p.mutable else "", p.name, self.rty(p.ty)))
            scope.declare(p.name, p.ty, p.mutable)
        ret = "" if f.ret == K.UNIT else " -> " + self.rty(f.ret)
        vis = "pub " if (f.ns and not f.internal) or f.owner else ""
        self.emit(0, "%sfn %s(%s)%s {" % (vis, f.name, ", ".join(ps), ret))
        self.block_body(f.body, scope, 1, same=True)
        self.emit(0, "}")

    def struct(self, sd):
        """FR-257: `#[derive(Clone)] struct S { pub f: T, .. }` (`pub struct`
        inside a module), then `impl S { pub fn .. }` when it has member
        functions."""
        self.emit(0, "#[derive(Clone)]")
        head = "%sstruct %s" % ("pub " if sd.ns and not sd.internal else "", sd.name)
        if not sd.fields:
            self.emit(0, head + " {}")
        else:
            self.emit(0, head + " {")
            for fd in sd.fields:
                self.emit(1, "pub %s: %s," % (fd.name, self.rty(fd.ty)))
            self.emit(0, "}")
        if sd.methods:
            self.lines.append("")
            self.emit(0, "impl %s {" % sd.name)
            self.base += 1
            for i, m in enumerate(sd.methods):
                if i:
                    self.lines.append("")
                self.func(m)
            self.base -= 1
            self.emit(0, "}")


def rs_print(program):
    """Canonical Rust source for a residual-free, well-formed program."""
    if K.residuals(program, include_cascade=True):
        raise PrintError("program has residuals")
    errs = TC.check(program)
    if errs:
        loc, msg = errs[0]
        raise PrintError("ill-formed kernel at %s: %s" % (loc, msg))
    p = _Printer(program)
    open_ns = ()

    def gap():
        # a blank line between items, none right after a `mod .. {`
        if p.lines and not p.lines[-1].endswith("{"):
            p.lines.append("")

    for f in program.items:
        # N13 makes each module's items contiguous, so one walk opens and
        # closes every `mod` block exactly once.
        common = 0
        while common < min(len(open_ns), len(f.ns)) and open_ns[common] == f.ns[common]:
            common += 1
        while len(open_ns) > common:
            open_ns = open_ns[:-1]
            p.base = len(open_ns)
            p.emit(0, "}")
        while len(open_ns) < len(f.ns):
            gap()
            p.base = len(open_ns)
            name = f.ns[len(open_ns)]
            p.emit(0, "%smod %s {" % ("pub " if open_ns else "", name))
            open_ns = open_ns + (name,)
        p.base = len(open_ns)
        p.ns = f.ns
        gap()
        if isinstance(f, K.StructDef):
            p.struct(f)
        elif isinstance(f, K.ConstItem):
            p.emit(0, "%sconst %s: %s = %s;" % ("pub " if f.ns and not f.internal else "", f.name, f.ty.name,
                                               p.expr(f.value, TC.Scope())))
        else:
            p.func(f)
    while open_ns:
        open_ns = open_ns[:-1]
        p.base = len(open_ns)
        p.emit(0, "}")
    out = "\n".join(p.lines) + "\n"
    if program.consts():
        out = "#![allow(non_upper_case_globals)]\n\n" + out
    mod = fmt.bx_module(p.used)
    if mod:
        out += "\n" + mod
    return out
