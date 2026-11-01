"""Kernel -> canonical C++17 (FR-250).

Canonical choices (the ONLY spelling the printer emits):

* `#include <cstdio>` is the single include; printf is `std::printf`.
* types: char, unsigned char, int, long long, unsigned, unsigned long long,
  std::size_t, double, bool, void.
* integer literals: `5`, `5LL`, `5U`, `5ULL`; an i8 literal is a char
  literal `'a'` when printable ASCII (not quote/backslash) else
  `static_cast<char>(N)`; u8 and usize literals are
  `static_cast<unsigned char>(N)` / `static_cast<std::size_t>(N)` (C++ has
  no such literal; cxx_import folds the cast back -- normal form N2).
* every Kernel Cast is `static_cast<T>(e)`; there are no implicit
  conversions in the printed program except the ones clang re-derives as
  identity (normal form N1).
* `!` on bool, `~` on integers (Kernel 'not').
* compound assignment `x op= e;`; `x += 1` of the variable's own integer
  type is spelled `++x;` (and `--x;`), the import of both `++x`/`x++`.
* parenthesization: same rule as the Rust printer (binary operands that
  are binary are parenthesized unless left operand of the same
  non-comparison operator; unary operands unless atomic).
* the counting loop: `for (T i = lo; i < hi; ++i) { .. }`.
* `int main() { ...; return 0; }`.
* a prototype is printed for every function that is called (by another
  function) before its definition.

FR-251 rows:

* includes: `<cstdio>`, then `<string>` / `<utility>` / `<vector>` exactly
  when used (`<utility>` for std::move).
* types `std::vector<T>`, `std::string`; reference parameters
  `const T& x` / `T& x`; range-for `for (T x : v)`, `for (const T x : v)`,
  `for (const T& x : v)`, `for (T& x : v)`.
* declarations: `std::vector<T> v;` (empty literal), `std::vector<T> v =
  {a, b};`, `std::vector<T> v(n, x);`, `std::string s;`, `std::string s =
  "lit";`, otherwise `T v = e;`.  As expressions: `std::vector<T>{}`,
  `std::vector<T>{a, b}`, `std::vector<T>(n, x)`, `std::string{}`,
  `std::string("lit")`.  The value-initialized temporaries use BRACES:
  `std::vector<T>()[i]` and `(std::vector<T>())[i]` are declarator / cast
  ambiguities (a function type; a cast followed by a lambda) -- measured
  on clang 21 by the property test.
* a MOVE (a bare owned non-Copy variable in a consuming position) is
  `std::move(x)`, except `return x;` (C++17 implicit move); a Clone is the
  bare place (the copy constructor / copy assignment does it).
* `x.size()`, `x.empty()`, `v[i]`, `s[i]`, `s.substr(p, n)`,
  `std::to_string(x)`, `v.push_back(e);`, `v.pop_back();`, `x.clear();`,
  `v.resize(n, x);`, `s += t;`, `s.push_back(c);`, `++v[i];` for += 1,
  `x.c_str()` as a printf %s argument.

FR-261 rows (pointers): `const T*` / `T*` parameters and locals (top-level
const after the star: `T* const p`); `p[i]` (a literal index bare: `p[2]`),
`*p` for `p[0]` and `p->f` / `p->m(..)` / `p->size()` for its members
(normal form N16), `p + k`, `v.data()`, `v.data() + k`, `&x`, `this` (the
address of `*this`), `p += k;` / `++p;`.

FR-259 rows: `short` / `unsigned short` (i16 / u16; literals
`static_cast<short>(N)` / `static_cast<unsigned short>(N)`, folded back by
N2 like u8's); `c ? a : b` (a nested `?:` operand and a `?:` operand of a
unary or binary operator are parenthesized); `++x` / `x++` / `--x` / `x--`
as a value (parenthesized as a unary operand: `-(--x)` must not lex as
`---x`).  The typedef, `static` / `inline` and `auto` spellings clang reads
are normal forms N20-N22: the canonical C++ spells the builtin type, no
`static` / `inline` (an internal function is in `namespace {`), and every
declared type.

FR-257 rows (structs):

* layout: forward declarations `struct S;` (only for a struct named before
  its definition through a vector, a reference or a member-function
  signature), then every struct definition in normal-form-N15 order inside
  its namespace blocks -- data members, then member-function DECLARATIONS
  (`int get() const;`, `void add(int k);`, `static S mk(int z);`,
  `S(int a, int b);`) -- then prototypes, then every definition in item
  order, member functions OUT OF LINE (`int S::get() const {`,
  `S::S(int a, int b) : x(a), y(b) {}`) so a body may call anything.
* a struct type is bare in its own namespace, else `::a::S`; inside a
  member function every FREE function call is `::`-qualified (a member of
  the same name would hide it); `S::f(..)` / `S(..)` for static members and
  constructors.
* `self` is `*this`; a member through it is `this->f` / `this->m(..)`.
* aggregate init: `S s = {a, b};` as a declaration, `S{a, b}` elsewhere.

FR-252 rows (closures): `auto f = [k, v](int a) mutable -> int {` .. `};`
(captures by copy, or `[&acc]` by reference, in first-use order; `mutable`
exactly when a by-copy closure modifies its copy; `-> R` unless void; an
empty list `[]`), `f(a)`; a function with a callable parameter is `template
<typename F> R g(const F& f, ..)` (`F& f` for `&mut impl FnMut`), one line
(the prototype too), called `g(add, ..)`.
"""

from . import fmt
from . import kernel as K
from . import typecheck as TC

CXX_TYPE = {
    "i8": "char", "u8": "unsigned char", "i32": "int", "i64": "long long",
    "u32": "unsigned", "u64": "unsigned long long", "usize": "std::size_t",
    "f64": "double", "bool": "bool", "()": "void",
    "i16": "short", "u16": "unsigned short",
}

CXX_BINOP = {
    "add": "+", "sub": "-", "mul": "*", "div": "/", "rem": "%",
    "band": "&", "bor": "|", "bxor": "^", "shl": "<<", "shr": ">>",
    "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">=",
    "and": "&&", "or": "||",
}


class PrintError(Exception):
    pass


def cxx_ty(t, ns=()):
    if isinstance(t, K.VecT):
        return "std::vector<%s>" % cxx_ty(t.elem, ns)
    if isinstance(t, K.StrT):
        return "std::string"
    if isinstance(t, K.RefT) and isinstance(t.inner, K.FnT):
        return "%sF&" % ("" if t.mutable else "const ")  # FR-252: the one template parameter
    if isinstance(t, K.RefT):
        return "%s%s&" % ("" if t.mutable else "const ", cxx_ty(t.inner, ns))
    if isinstance(t, K.StructT):
        return cxx_path(t.path, ns)
    if isinstance(t, K.PtrT):
        return "%s%s*" % ("" if t.mutable else "const ", cxx_ty(t.inner, ns))
    return CXX_TYPE[t.name]


def cxx_path(path, ns):
    """FR-257: a struct of the SAME namespace is bare, any other is
    `::a::S` (never an enclosing-scope search, like FR-260's calls)."""
    sns, leaf = K.split_path(path)
    return leaf if sns == tuple(ns) else "::" + path


def cxx_int_lit(e):
    v = e.value
    t = e.ty.name
    if t == "i32":
        return str(v)
    if t == "i64":
        return "%dLL" % v
    if t == "u32":
        return "%dU" % v
    if t == "u64":
        return "%dULL" % v
    inner = str(v) if v <= 0x7FFFFFFF else "%dULL" % v
    if t == "usize":
        return "static_cast<std::size_t>(%s)" % inner
    if t == "u8":
        return "static_cast<unsigned char>(%s)" % inner
    if t in ("i16", "u16"):
        return "static_cast<%s>(%s)" % (CXX_TYPE[t], inner)
    if t == "i8":
        c = chr(v)
        if 0x20 <= v <= 0x7E and c not in "'\\":
            return "'%s'" % c
        return "static_cast<char>(%s)" % inner
    raise PrintError("literal of type %s" % t)


class _Printer:
    def __init__(self, program):
        self.sigs = TC.sigs_of(program)
        self.lines = []
        self.includes = set()
        self.ns = ()  # the namespace of the function being printed
        self.method = None  # FR-257: the member function being printed

    def callee(self, path):
        """FR-260: bare in the same namespace (unqualified lookup finds the
        innermost declaration first, and a prototype always precedes the
        use), otherwise global-qualified `::a::b::f` -- which never searches
        enclosing scopes and suppresses argument-dependent lookup."""
        if TC.recv_of(self.sigs).get(path):
            # FR-257: `S::f(..)` (a constructor call is `S(..)`)
            sp, leaf = path.rsplit("::", 1)
            head = cxx_path(sp, self.ns)
            return head if TC.recv_of(self.sigs)[path] == "ctor" else head + "::" + leaf
        ns, leaf = K.split_path(path)
        if ns == self.ns and self.method is None:
            # (inside a member function a member of the same name would
            # hide it, so there every free function is `::`-qualified)
            return leaf
        return "::" + path

    def ty(self, e, scope):
        return TC.type_of(e, scope, self.sigs)

    def note_ty(self, t):
        if isinstance(t, (K.RefT, K.PtrT)):
            t = t.inner
        if isinstance(t, K.VecT):
            self.includes.add("vector")
            self.note_ty(t.elem)
        elif isinstance(t, K.StrT):
            self.includes.add("string")

    def tyname(self, t):
        self.note_ty(t)
        return cxx_ty(t, self.ns)

    def is_deref(self, e, scope):
        """FR-261: Index(p, 0usize) on a pointer variable -- printed `*p`
        (normal form N16), a member of it `p->f`."""
        return (isinstance(e, K.Index) and e.index == K.IntLit(0, K.USIZE) and scope is not None
                and isinstance(self.ty(e.base, scope), K.PtrT))

    def atomic(self, e, scope=None):
        if isinstance(e, K.Var) and e.name == "self":
            return False
        if self.is_deref(e, scope):
            return False
        return isinstance(e, (K.IntLit, K.FloatLit, K.BoolLit, K.Var, K.Call, K.ClosureCall, K.Cast, K.StrLit, K.Index,
                              K.ByteAt, K.Len, K.IsEmpty, K.VecLit, K.VecFill, K.StrNew, K.StrFrom,
                              K.Substr, K.ToStr, K.Clone, K.Field, K.StructLit, K.MethodCall))

    def recv(self, e, scope):
        s = self.expr(e, scope)
        return s if self.atomic(e, scope) else "(" + s + ")"

    def consume(self, e, scope, is_return=False):
        """A consuming position: a bare owned non-Copy variable MOVES."""
        if isinstance(e, K.Var) and not is_return:
            v = scope.lookup(e.name)
            if v is not None and K.is_owned_nc(v[0]):
                self.includes.add("utility")
                return "std::move(%s)" % e.name
        return self.expr(e, scope)

    def member(self, base, scope):
        """`this->` for `self`, otherwise `base.`."""
        if isinstance(base, K.Var) and base.name == "self":
            return "this->"
        if self.is_deref(base, scope):
            return self.expr(base.base, scope) + "->"
        return self.recv(base, scope) + "."

    def offset_index(self, e, scope):
        """FR-261: a pointer index / offset (usize in the Kernel).  A literal
        is printed as a plain int literal -- C++ pointer arithmetic takes
        any integer, and cxx_import reads a non-negative int literal back as
        the usize one (N19)."""
        if isinstance(e, K.IntLit) and e.ty == K.USIZE and e.value <= 0x7FFFFFFF:
            return str(e.value)
        return self.expr(e, scope)

    def nc_expr(self, e, scope):
        if isinstance(e, K.Clone):
            return self.expr(e.operand, scope)
        if isinstance(e, K.Field):
            return self.member(e.base, scope) + e.name
        if isinstance(e, K.StructLit):
            return "%s{%s}" % (cxx_path(e.path, self.ns), self.struct_values(e, scope))
        if isinstance(e, K.MethodCall):
            rt = self.ty(e.recv, scope)
            sig = self.sigs[TC.method_path(rt.path, e.name)]
            return "%s%s(%s)" % (self.member(e.recv, scope), e.name, self.args(sig[0], e.args, scope))
        if isinstance(e, K.Index) and isinstance(self.ty(e.base, scope), K.PtrT):
            if self.is_deref(e, scope):
                return "*" + self.expr(e.base, scope)
            return "%s[%s]" % (self.expr(e.base, scope), self.offset_index(e.index, scope))
        if isinstance(e, (K.Index, K.ByteAt)):
            return "%s[%s]" % (self.recv(e.base, scope), self.expr(e.index, scope))
        if isinstance(e, K.Offset):
            if isinstance(self.ty(e.base, scope), K.PtrT):
                head = self.expr(e.base, scope)
            else:
                head = self.member(e.base, scope) + "data()"
            if e.start is None:
                return head
            st = self.offset_index(e.start, scope)
            return "%s + %s" % (head, "(" + st + ")" if isinstance(e.start, K.Binary) else st)
        if isinstance(e, K.AddrOf):
            if isinstance(e.place, K.Var) and e.place.name == "self":
                return "this"
            return "&" + self.expr(e.place, scope)
        if isinstance(e, K.Len):
            return self.member(e.base, scope) + "size()"
        if isinstance(e, K.IsEmpty):
            return self.member(e.base, scope) + "empty()"
        if isinstance(e, K.VecLit):
            t = self.tyname(K.VecT(e.elem))
            if not e.elems:
                return t + "{}"
            return "%s{%s}" % (t, ", ".join(self.expr(x, scope) for x in e.elems))
        if isinstance(e, K.VecFill):
            t = self.tyname(self.ty(e, scope))
            return "%s(%s, %s)" % (t, self.expr(e.count, scope), self.expr(e.value, scope))
        if isinstance(e, K.StrNew):
            self.includes.add("string")
            return "std::string{}"
        if isinstance(e, K.StrFrom):
            self.includes.add("string")
            return 'std::string("%s")' % fmt.c_escape(e.value, False)
        if isinstance(e, K.Substr):
            args = self.expr(e.pos, scope)
            if e.count is not None:
                args += ", " + self.expr(e.count, scope)
            return "%ssubstr(%s)" % (self.member(e.base, scope), args)
        if isinstance(e, K.ToStr):
            self.includes.add("string")
            return "std::to_string(%s)" % self.expr(e.operand, scope)
        raise PrintError("cannot print %r" % (e,))

    def expr(self, e, scope):
        if isinstance(e, K.IntLit):
            return cxx_int_lit(e)
        if isinstance(e, K.FloatLit):
            return e.text
        if isinstance(e, K.BoolLit):
            return "true" if e.value else "false"
        if isinstance(e, K.StrLit):
            return '"' + fmt.c_escape(e.value, False) + '"'
        if isinstance(e, K.Var):
            return "*this" if e.name == "self" else e.name
        if isinstance(e, K.Call):
            ptys = self.sigs[e.name][0]
            return "%s(%s)" % (self.callee(e.name), self.args(ptys, e.args, scope))
        if isinstance(e, K.ClosureCall):
            return "%s(%s)" % (e.name, ", ".join(self.expr(a, scope) for a in e.args))
        if isinstance(e, (K.Clone, K.Index, K.ByteAt, K.Len, K.IsEmpty, K.VecLit, K.VecFill, K.StrNew,
                          K.StrFrom, K.Substr, K.ToStr, K.Field, K.StructLit, K.MethodCall, K.Offset, K.AddrOf)):
            return self.nc_expr(e, scope)
        if isinstance(e, K.Cast):
            return "static_cast<%s>(%s)" % (CXX_TYPE[e.ty.name], self.expr(e.operand, scope))
        if isinstance(e, K.Cond):
            def arm(x):
                s = self.expr(x, scope)
                return "(" + s + ")" if isinstance(x, K.Cond) else s
            return "%s ? %s : %s" % (arm(e.cond), arm(e.then), arm(e.els))
        if isinstance(e, K.IncDec):
            sym = "++" if e.op == "add" else "--"
            return e.var.name + sym if e.post else sym + e.var.name
        if isinstance(e, K.Unary):
            t = self.ty(e.operand, scope)
            if e.op == "neg":
                sym = "-"
            else:
                sym = "!" if t == K.BOOL else "~"
            inner = self.expr(e.operand, scope)
            return sym + ("(" + inner + ")" if not self.atomic(e.operand, scope) else inner)
        if isinstance(e, K.Binary):
            consume = e.op == "add" and self.ty(e.lhs, scope) == K.STRING
            return "%s %s %s" % (self.operand(e, e.lhs, scope, True, consume), CXX_BINOP[e.op],
                                 self.operand(e, e.rhs, scope, False))
        raise PrintError("cannot print %r" % (e,))

    def args(self, ptys, args, scope):
        return ", ".join(self.expr(a, scope) if isinstance(pt, K.RefT) else self.consume(a, scope)
                         for pt, a in zip(ptys, args))

    def struct_values(self, e, scope):
        return ", ".join(self.consume(v, scope) for v in e.values)

    def operand(self, parent, child, scope, is_lhs, consume=False):
        s = self.consume(child, scope) if consume else self.expr(child, scope)
        if isinstance(child, K.Cond):
            return "(" + s + ")"
        if isinstance(child, K.Binary):
            same = child.op == parent.op and is_lhs and parent.op not in K.CMP_OPS
            return s if same else "(" + s + ")"
        return s

    def emit(self, depth, text):
        self.lines.append("    " * depth + text)

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
            # FR-252: `auto f = [k, v](int a) mutable -> int {` .. `};`
            caps = ", ".join(("" if s.move else "&") + c for c in K.closure_captures(s))
            ps = ", ".join("%s%s %s" % ("" if p.mutable else "const ", self.tyname(p.ty), p.name) for p in s.params)
            mutable = " mutable" if s.move and s.name in self.fnmut else ""
            ret = "" if s.ret == K.UNIT else " -> " + self.tyname(s.ret)
            self.emit(d, "auto %s = [%s](%s)%s%s {" % (s.name, caps, ps, mutable, ret))
            inner = scope.child()
            for p in s.params:
                inner.declare(p.name, p.ty, p.mutable)
            self.block_body(s.body, inner, d + 1, same=True)
            self.emit(d, "};")
            scope.declare(s.name, K.FnT(tuple(p.ty for p in s.params), s.ret), True)
        elif isinstance(s, K.Let):
            if isinstance(s.ty, K.PtrT):
                # FR-261: top-level const follows the pointer (`T* const q`)
                head = "%s%s %s" % (self.tyname(s.ty), "" if s.mutable else " const", s.name)
            else:
                head = "%s%s %s" % ("" if s.mutable else "const ", self.tyname(s.ty), s.name)
            i = s.init
            if isinstance(i, K.VecLit) and not i.elems or isinstance(i, K.StrNew):
                self.emit(d, head + ";")
            elif isinstance(i, K.VecLit):
                self.emit(d, "%s = {%s};" % (head, ", ".join(self.expr(x, scope) for x in i.elems)))
            elif isinstance(i, K.VecFill):
                self.emit(d, "%s(%s, %s);" % (head, self.expr(i.count, scope), self.expr(i.value, scope)))
            elif isinstance(i, K.StrFrom):
                self.emit(d, '%s = "%s";' % (head, fmt.c_escape(i.value, False)))
            elif isinstance(i, K.StructLit):
                self.emit(d, "%s = {%s};" % (head, self.struct_values(i, scope)))
            else:
                self.emit(d, "%s = %s;" % (head, self.consume(i, scope)))
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, K.Assign):
            self.emit(d, "%s = %s;" % ("*this" if s.name == "self" else s.name, self.consume(s.value, scope)))
        elif isinstance(s, K.CompoundAssign):
            t = K.deref(scope.lookup(s.name)[0])
            v = s.value
            if isinstance(t, K.PtrT):
                # FR-261: a forward re-slice `p += k;` / `++p;`
                if v == K.IntLit(1, K.USIZE):
                    self.emit(d, "++%s;" % s.name)
                else:
                    self.emit(d, "%s += %s;" % (s.name, self.offset_index(v, scope)))
            elif (s.op in ("add", "sub") and isinstance(v, K.IntLit) and v.value == 1 and v.ty == t
                    and t in K.ARITH_INT):
                self.emit(d, "%s%s;" % ("++" if s.op == "add" else "--", s.name))
            else:
                self.emit(d, "%s %s= %s;" % (s.name, CXX_BINOP[s.op], self.expr(v, scope)))
        elif isinstance(s, K.Store):
            self.emit(d, "%s = %s;" % (self.expr(s.target, scope), self.consume(s.value, scope)))
        elif isinstance(s, K.CompoundStore):
            t = self.ty(s.target, scope)
            v = s.value
            tgt = self.expr(s.target, scope)
            if (s.op in ("add", "sub") and isinstance(v, K.IntLit) and v.value == 1 and v.ty == t
                    and t in K.ARITH_INT):
                self.emit(d, "%s%s;" % ("++" if s.op == "add" else "--", tgt))
            else:
                self.emit(d, "%s %s= %s;" % (tgt, CXX_BINOP[s.op], self.expr(v, scope)))
        elif isinstance(s, K.Push):
            self.emit(d, "%spush_back(%s);" % (self.member(s.target, scope), self.consume(s.value, scope)))
        elif isinstance(s, K.Pop):
            self.emit(d, "%spop_back();" % self.member(s.target, scope))
        elif isinstance(s, K.Clear):
            self.emit(d, "%sclear();" % self.member(s.target, scope))
        elif isinstance(s, K.Resize):
            self.emit(d, "%sresize(%s, %s);" % (self.member(s.target, scope), self.expr(s.count, scope),
                                                 self.expr(s.value, scope)))
        elif isinstance(s, K.PushStr):
            self.emit(d, "%s += %s;" % (self.expr(s.target, scope), self.expr(s.value, scope)))
        elif isinstance(s, K.PushByte):
            self.emit(d, "%spush_back(%s);" % (self.member(s.target, scope), self.expr(s.value, scope)))
        elif isinstance(s, K.ForEach):
            t = self.ty(s.iterable, scope)
            el = self.tyname(t.elem)
            if s.mode == "copy":
                decl = "%s%s %s" % ("" if s.mutable else "const ", el, s.var)
                vt, vm = t.elem, s.mutable
            else:
                decl = "%s%s& %s" % ("const " if s.mode == "ref" else "", el, s.var)
                vt, vm = K.RefT(t.elem, s.mode == "mut"), False
            self.emit(d, "for (%s : %s) {" % (decl, self.expr(s.iterable, scope)))
            inner = scope.child()
            inner.declare(s.var, vt, vm)
            self.block_body(s.body, inner, d + 1, same=True)
            self.emit(d, "}")
        elif isinstance(s, K.ExprStmt):
            self.emit(d, self.expr(s.expr, scope) + ";")
        elif isinstance(s, K.Print):
            text = []
            args = []
            for sg in s.segs:
                if isinstance(sg, K.Text):
                    text.append(fmt.c_escape(sg.text, True))
                    continue
                argty = K.STR if sg.kind == "s" else self.ty(sg.arg, scope)
                text.append(fmt.c_spec_text(sg, argty))
                if sg.kind == "s" and not isinstance(sg.arg, K.StrLit):
                    args.append(self.member(sg.arg, scope) + "c_str()")
                else:
                    args.append(self.expr(sg.arg, scope))
            self.emit(d, 'std::printf("%s"%s);' % ("".join(text), "".join(", " + a for a in args)))
        elif isinstance(s, K.If):
            self.emit(d, "if (%s) {" % self.expr(s.cond, scope))
            self.block_body(s.then, scope, d + 1)
            self.else_part(s.els, scope, d)
        elif isinstance(s, K.While):
            self.emit(d, "while (%s) {" % self.expr(s.cond, scope))
            self.block_body(s.body, scope, d + 1)
            self.emit(d, "}")
        elif isinstance(s, K.For):
            t = self.ty(s.lo, scope)
            self.emit(d, "for (%s %s = %s; %s < %s; ++%s) {" % (
                CXX_TYPE[t.name], s.var, self.expr(s.lo, scope), s.var, self.expr(s.hi, scope), s.var))
            inner = scope.child()
            inner.declare(s.var, t, True)
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
                self.emit(d, "return %s;" % self.consume(s.value, scope, is_return=True))
        else:
            raise PrintError("cannot print %r" % (s,))

    def else_part(self, e, scope, d):
        if e is None:
            self.emit(d, "}")
            return
        if len(e.stmts) == 1 and isinstance(e.stmts[0], K.If):
            s = e.stmts[0]
            inner = scope.child()
            self.emit(d, "} else if (%s) {" % self.expr(s.cond, inner))
            self.block_body(s.then, inner, d + 1)
            self.else_part(s.els, inner, d)
            return
        self.emit(d, "} else {")
        self.block_body(e, scope, d + 1)
        self.emit(d, "}")

    def params(self, f):
        scope = TC.Scope()
        if f.recv in ("ref", "mut"):
            scope.declare("self", K.RefT(K.StructT(K.path_of(f.ns, f.owner)), f.recv == "mut"), False)
        ps = []
        for p in f.params:
            if isinstance(p.ty, K.RefT):
                ps.append("%s %s" % (self.tyname(p.ty), p.name))
            elif isinstance(p.ty, K.PtrT):
                ps.append("%s%s %s" % (self.tyname(p.ty), "" if p.mutable else " const", p.name))
            else:
                ps.append("%s%s %s" % ("" if p.mutable else "const ", self.tyname(p.ty), p.name))
            scope.declare(p.name, p.ty, p.mutable)
        return ", ".join(ps), scope

    def signature(self, f, in_class=False):
        """The declarator of ``f``.  FR-257: a member function is declared
        in its class (`int get() const;`, `static S mk(int z);`, `S(int a);`)
        and defined out of line (`int S::get() const {`, `S::S(int a) : ..`)."""
        ps, scope = self.params(f)
        if f.path == "main":
            return "int main()", scope
        if any(isinstance(p.ty, K.RefT) and isinstance(p.ty.inner, K.FnT) for p in f.params):
            # FR-252: one callable parameter, the template's only parameter F
            return "template <typename F> %s %s(%s)" % (self.tyname(f.ret), f.name, ps), scope
        if f.recv == "ctor":
            return ("%s(%s)" % (f.owner, ps) if in_class else "%s::%s(%s)" % (f.owner, f.owner, ps)), scope
        name = f.name if not f.owner or in_class else "%s::%s" % (f.owner, f.name)
        head = "%s %s(%s)" % (self.tyname(f.ret), name, ps)
        if f.recv == "static" and in_class:
            head = "static " + head
        if f.recv == "ref":
            head += " const"
        return head, scope

    def func(self, f):
        sig, scope = self.signature(f)
        self.fnmut = TC.closure_info(f.body, self.sigs)[1]
        self.method = f if f.owner else None
        try:
            if f.recv == "ctor":
                lit = f.body.stmts[0].value
                sd = TC.structs_of(self.sigs)[lit.path]
                inits = ", ".join("%s(%s)" % (fd.name, self.consume(v, scope)) for fd, v in zip(sd.fields, lit.values))
                self.emit(0, "%s%s {}" % (sig, " : " + inits if inits else ""))
                return
            self.emit(0, sig + " {")
            self.block_body(f.body, scope, 1, same=True)
            if f.path == "main":
                self.emit(1, "return 0;")
            self.emit(0, "}")
        finally:
            self.method = None

    def struct(self, sd):
        """`struct S {` data members, then member-function declarations `};`"""
        self.emit(0, "struct %s {" % sd.name)
        for fd in sd.fields:
            self.emit(1, "%s %s;" % (self.tyname(fd.ty), fd.name))
        for m in sd.methods:
            self.emit(1, self.signature(m, in_class=True)[0] + ";")
        self.emit(0, "};")


def definitions(program):
    """FR-257: the C++ definition sequence -- free functions, and in a
    StructDef's place its member functions (defined out of line)."""
    return program.all_funcs()


def needs_prototype(program):
    """Free functions called (by any definition, member functions too)
    before their own definition.  A member function never needs one: its
    class declares it."""
    defs = definitions(program)
    index = {f.path: i for i, f in enumerate(defs) if not f.owner}
    need = set()
    for i, f in enumerate(defs):
        for n in K.walk(f.body):
            if isinstance(n, K.Call) and n.name in index and index[n.name] > i:
                need.add(n.name)
    return [f for f in program.funcs() if f.path in need]


def struct_mentions(sd):
    """Struct paths a definition of ``sd`` names: data member and member-
    function signature types (by value, in a vector, or by reference)."""
    out = []

    def add(t):
        while isinstance(t, (K.VecT, K.RefT, K.PtrT)):
            t = t.elem if isinstance(t, K.VecT) else t.inner
        if isinstance(t, K.StructT) and t.path != sd.path and t.path not in out:
            out.append(t.path)

    for fd in sd.fields:
        add(fd.ty)
    for m in sd.methods:
        for p in m.params:
            add(p.ty)
        add(m.ret)
    return out


def forward_decls(structs):
    """The structs that need a forward declaration `struct S;`: those named
    by a struct printed before them (only through a vector, a reference or
    a member-function signature -- a by-value data member forces the
    contained struct earlier, normal form N15)."""
    pos = {sd.path: i for i, sd in enumerate(structs)}
    need = set()
    for i, sd in enumerate(structs):
        for p in struct_mentions(sd):
            if pos.get(p, -1) > i:
                need.add(p)
    return [sd for sd in structs if sd.path in need]


def _block_path(f):
    """The C++ block nesting of ``f``: its namespaces, then '' (an anonymous
    namespace) when it is internal."""
    return f.ns + (("",) if f.internal else ())


def _close_comment(name):
    return "}  // namespace" + (" " + name if name else "")


def emit_nested(p, funcs, body, spaced):
    """Print ``funcs`` (in N13 order) opening and closing namespace blocks
    as the block path changes.  ``spaced``: a blank line around every item
    and block edge (definitions); none (prototypes)."""
    open_ = ()

    def line(text):
        if spaced and p.lines and p.lines[-1] != "":
            p.lines.append("")
        p.lines.append(text)

    for f in funcs:
        want = _block_path(f)
        common = 0
        while common < min(len(open_), len(want)) and open_[common] == want[common]:
            common += 1
        while len(open_) > common:
            line(_close_comment(open_[-1]))
            open_ = open_[:-1]
        while len(open_) < len(want):
            name = want[len(open_)]
            line("namespace %s{" % (name + " " if name else ""))
            open_ = open_ + (name,)
        p.ns = f.ns
        if spaced and p.lines and p.lines[-1] != "":
            p.lines.append("")
        body(f)
    while open_:
        line(_close_comment(open_[-1]))
        open_ = open_[:-1]


def cxx_print(program):
    """Canonical C++17 source for a residual-free, well-formed program."""
    if K.residuals(program, include_cascade=True):
        raise PrintError("program has residuals")
    errs = TC.check(program)
    if errs:
        loc, msg = errs[0]
        raise PrintError("ill-formed kernel at %s: %s" % (loc, msg))
    p = _Printer(program)
    structs = TC.struct_order(program.structs())  # N15: C++ defines before use
    fwd = forward_decls(structs)
    if fwd:
        p.lines.append("")
        emit_nested(p, fwd, lambda sd: p.emit(0, "struct %s;" % sd.name), spaced=False)
    if structs:
        p.lines.append("")
        emit_nested(p, structs, p.struct, spaced=True)
    protos = needs_prototype(program)
    if protos:
        p.lines.append("")
        emit_nested(p, protos, lambda f: p.emit(0, p.signature(f)[0] + ";"), spaced=False)
    defs = definitions(program)
    if defs:
        p.lines.append("")
    emit_nested(p, defs, p.func, spaced=True)
    head = ["#include <cstdio>"] + ["#include <%s>" % h for h in sorted(p.includes)]
    return "\n".join(head + p.lines) + "\n"
