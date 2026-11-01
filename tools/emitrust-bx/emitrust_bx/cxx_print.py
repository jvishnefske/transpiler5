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

    def ty(self, e, scope):
        return TC.type_of(e, scope, self.sigs)

    @staticmethod
    def atomic(e):
        return isinstance(e, (K.IntLit, K.FloatLit, K.BoolLit, K.Var, K.Call, K.Cast, K.StrLit))

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
            return "%s(%s)" % (e.name, ", ".join(self.expr(a, scope) for a in e.args))
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
            return "%s %s %s" % (self.operand(e, e.lhs, scope, True), CXX_BINOP[e.op],
                                 self.operand(e, e.rhs, scope, False))
        raise PrintError("cannot print %r" % (e,))

    def operand(self, parent, child, scope, is_lhs):
        s = self.expr(child, scope)
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
            self.emit(d, "%s%s %s = %s;" % ("" if s.mutable else "const ", CXX_TYPE[s.ty.name], s.name,
                                           self.expr(s.init, scope)))
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, K.Assign):
            self.emit(d, "%s = %s;" % (s.name, self.expr(s.value, scope)))
        elif isinstance(s, K.CompoundAssign):
            t = scope.lookup(s.name)[0]
            v = s.value
            if (s.op in ("add", "sub") and isinstance(v, K.IntLit) and v.value == 1 and v.ty == t
                    and t in K.ARITH_INT):
                self.emit(d, "%s%s;" % ("++" if s.op == "add" else "--", s.name))
            else:
                self.emit(d, "%s %s= %s;" % (s.name, CXX_BINOP[s.op], self.expr(v, scope)))
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
                self.emit(d, "return %s;" % self.expr(s.value, scope))
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
            ps.append("%s%s %s" % ("" if p.mutable else "const ", CXX_TYPE[p.ty.name], p.name))
            scope.declare(p.name, p.ty, p.mutable)
        if f.name == "main":
            return "int main()", scope
        return "%s %s(%s)" % (CXX_TYPE[f.ret.name], f.name, ", ".join(ps)), scope

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
    p.emit(0, "#include <cstdio>")
    protos = needs_prototype(program)
    if protos:
        p.lines.append("")
        for f in protos:
            p.emit(0, p.signature(f)[0] + ";")
    for f in program.funcs():
        p.lines.append("")
        p.func(f)
    return "\n".join(p.lines) + "\n"
