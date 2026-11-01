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
"""

from . import fmt
from . import kernel as K
from . import typecheck as TC

CXX_TYPE = {
    "i8": "char", "u8": "unsigned char", "i32": "int", "i64": "long long",
    "u32": "unsigned", "u64": "unsigned long long", "usize": "std::size_t",
    "f64": "double", "bool": "bool", "()": "void",
}

CXX_BINOP = {
    "add": "+", "sub": "-", "mul": "*", "div": "/", "rem": "%",
    "band": "&", "bor": "|", "bxor": "^", "shl": "<<", "shr": ">>",
    "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">=",
    "and": "&&", "or": "||",
}


class PrintError(Exception):
    pass


def cxx_ty(t):
    if isinstance(t, K.VecT):
        return "std::vector<%s>" % cxx_ty(t.elem)
    if isinstance(t, K.StrT):
        return "std::string"
    if isinstance(t, K.RefT):
        return "%s%s&" % ("" if t.mutable else "const ", cxx_ty(t.inner))
    return CXX_TYPE[t.name]


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

    def ty(self, e, scope):
        return TC.type_of(e, scope, self.sigs)

    def note_ty(self, t):
        if isinstance(t, K.RefT):
            t = t.inner
        if isinstance(t, K.VecT):
            self.includes.add("vector")
            self.note_ty(t.elem)
        elif isinstance(t, K.StrT):
            self.includes.add("string")

    def tyname(self, t):
        self.note_ty(t)
        return cxx_ty(t)

    @staticmethod
    def atomic(e):
        return isinstance(e, (K.IntLit, K.FloatLit, K.BoolLit, K.Var, K.Call, K.Cast, K.StrLit, K.Index,
                              K.ByteAt, K.Len, K.IsEmpty, K.VecLit, K.VecFill, K.StrNew, K.StrFrom,
                              K.Substr, K.ToStr, K.Clone))

    def recv(self, e, scope):
        s = self.expr(e, scope)
        return s if self.atomic(e) else "(" + s + ")"

    def consume(self, e, scope, is_return=False):
        """A consuming position: a bare owned non-Copy variable MOVES."""
        if isinstance(e, K.Var) and not is_return:
            v = scope.lookup(e.name)
            if v is not None and K.is_owned_nc(v[0]):
                self.includes.add("utility")
                return "std::move(%s)" % e.name
        return self.expr(e, scope)

    def nc_expr(self, e, scope):
        if isinstance(e, K.Clone):
            return self.expr(e.operand, scope)
        if isinstance(e, (K.Index, K.ByteAt)):
            return "%s[%s]" % (self.recv(e.base, scope), self.expr(e.index, scope))
        if isinstance(e, K.Len):
            return self.recv(e.base, scope) + ".size()"
        if isinstance(e, K.IsEmpty):
            return self.recv(e.base, scope) + ".empty()"
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
            return "%s.substr(%s)" % (self.recv(e.base, scope), args)
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
            return e.name
        if isinstance(e, K.Call):
            ptys = self.sigs[e.name][0]
            return "%s(%s)" % (e.name, ", ".join(
                self.expr(a, scope) if isinstance(pt, K.RefT) else self.consume(a, scope)
                for pt, a in zip(ptys, e.args)))
        if isinstance(e, (K.Clone, K.Index, K.ByteAt, K.Len, K.IsEmpty, K.VecLit, K.VecFill, K.StrNew,
                          K.StrFrom, K.Substr, K.ToStr)):
            return self.nc_expr(e, scope)
        if isinstance(e, K.Cast):
            return "static_cast<%s>(%s)" % (CXX_TYPE[e.ty.name], self.expr(e.operand, scope))
        if isinstance(e, K.Unary):
            t = self.ty(e.operand, scope)
            if e.op == "neg":
                sym = "-"
            else:
                sym = "!" if t == K.BOOL else "~"
            inner = self.expr(e.operand, scope)
            return sym + ("(" + inner + ")" if not self.atomic(e.operand) else inner)
        if isinstance(e, K.Binary):
            consume = e.op == "add" and self.ty(e.lhs, scope) == K.STRING
            return "%s %s %s" % (self.operand(e, e.lhs, scope, True, consume), CXX_BINOP[e.op],
                                 self.operand(e, e.rhs, scope, False))
        raise PrintError("cannot print %r" % (e,))

    def operand(self, parent, child, scope, is_lhs, consume=False):
        s = self.consume(child, scope) if consume else self.expr(child, scope)
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
        elif isinstance(s, K.Let):
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
            else:
                self.emit(d, "%s = %s;" % (head, self.consume(i, scope)))
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, K.Assign):
            self.emit(d, "%s = %s;" % (s.name, self.consume(s.value, scope)))
        elif isinstance(s, K.CompoundAssign):
            t = K.deref(scope.lookup(s.name)[0])
            v = s.value
            if (s.op in ("add", "sub") and isinstance(v, K.IntLit) and v.value == 1 and v.ty == t
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
            self.emit(d, "%s.push_back(%s);" % (self.recv(s.target, scope), self.consume(s.value, scope)))
        elif isinstance(s, K.Pop):
            self.emit(d, "%s.pop_back();" % self.recv(s.target, scope))
        elif isinstance(s, K.Clear):
            self.emit(d, "%s.clear();" % self.recv(s.target, scope))
        elif isinstance(s, K.Resize):
            self.emit(d, "%s.resize(%s, %s);" % (self.recv(s.target, scope), self.expr(s.count, scope),
                                                 self.expr(s.value, scope)))
        elif isinstance(s, K.PushStr):
            self.emit(d, "%s += %s;" % (self.expr(s.target, scope), self.expr(s.value, scope)))
        elif isinstance(s, K.PushByte):
            self.emit(d, "%s.push_back(%s);" % (self.recv(s.target, scope), self.expr(s.value, scope)))
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
                    args.append(self.recv(sg.arg, scope) + ".c_str()")
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

    def signature(self, f):
        scope = TC.Scope()
        ps = []
        for p in f.params:
            if isinstance(p.ty, K.RefT):
                ps.append("%s %s" % (self.tyname(p.ty), p.name))
            else:
                ps.append("%s%s %s" % ("" if p.mutable else "const ", self.tyname(p.ty), p.name))
            scope.declare(p.name, p.ty, p.mutable)
        if f.name == "main":
            return "int main()", scope
        return "%s %s(%s)" % (self.tyname(f.ret), f.name, ", ".join(ps)), scope

    def func(self, f):
        sig, scope = self.signature(f)
        self.emit(0, sig + " {")
        self.block_body(f.body, scope, 1, same=True)
        if f.name == "main":
            self.emit(1, "return 0;")
        self.emit(0, "}")


def needs_prototype(program):
    funcs = program.funcs()
    index = {f.name: i for i, f in enumerate(funcs)}
    need = set()
    for i, f in enumerate(funcs):
        for n in K.walk(f.body):
            if isinstance(n, K.Call) and n.name in index and index[n.name] > i:
                need.add(n.name)
    return [f for f in funcs if f.name in need]


def cxx_print(program):
    """Canonical C++17 source for a residual-free, well-formed program."""
    if K.residuals(program, include_cascade=True):
        raise PrintError("program has residuals")
    errs = TC.check(program)
    if errs:
        loc, msg = errs[0]
        raise PrintError("ill-formed kernel at %s: %s" % (loc, msg))
    p = _Printer(program)
    protos = needs_prototype(program)
    if protos:
        p.lines.append("")
        for f in protos:
            p.emit(0, p.signature(f)[0] + ";")
    for f in program.funcs():
        p.lines.append("")
        p.func(f)
    head = ["#include <cstdio>"] + ["#include <%s>" % h for h in sorted(p.includes)]
    return "\n".join(head + p.lines) + "\n"
