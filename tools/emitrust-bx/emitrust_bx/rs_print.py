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
"""

from . import fmt
from . import kernel as K
from . import typecheck as TC

RUST_BINOP = {
    "add": "+", "sub": "-", "mul": "*", "div": "/", "rem": "%",
    "band": "&", "bor": "|", "bxor": "^", "shl": "<<", "shr": ">>",
    "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">=",
    "and": "&&", "or": "||",
}


class PrintError(Exception):
    pass


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

    def ty(self, e, scope):
        return TC.type_of(e, scope, self.sigs)

    def atomic(self, e, scope):
        if isinstance(e, (K.IntLit, K.FloatLit, K.BoolLit, K.Var, K.Call, K.StrLit)):
            return True
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
            return e.name
        if isinstance(e, K.Call):
            saved, self.in_cast = self.in_cast, 0
            try:
                return "%s(%s)" % (e.name, ", ".join(self.expr(a, scope) for a in e.args))
            finally:
                self.in_cast = saved
        if isinstance(e, K.Cast):
            self.in_cast += 1
            try:
                inner = self.expr(e.operand, scope)
            finally:
                self.in_cast -= 1
            return "%s as %s" % (self.paren_if(not self.atomic(e.operand, scope), inner), e.ty.name)
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
            return "%s %s %s" % (self.operand(e, e.lhs, scope, True), RUST_BINOP[e.op],
                                 self.operand(e, e.rhs, scope, False))
        raise PrintError("cannot print %r" % (e,))

    def operand(self, parent, child, scope, is_lhs):
        s = self.expr(child, scope)
        if isinstance(child, K.Cast):
            return "(" + s + ")"
        if isinstance(child, K.Binary) and not self.is_wrapping(child, scope):
            same = child.op == parent.op and is_lhs and parent.op not in K.CMP_OPS
            return s if same else "(" + s + ")"
        return s

    # -------------------------------------------------------------- stmts

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
            self.emit(d, "let %s%s: %s = %s;" % ("mut " if s.mutable else "", s.name, s.ty.name,
                                                 self.expr(s.init, scope)))
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, K.Assign):
            self.emit(d, "%s = %s;" % (s.name, self.expr(s.value, scope)))
        elif isinstance(s, K.CompoundAssign):
            t = scope.lookup(s.name)[0]
            if s.op in K.WRAPPING_OPS and K.is_unsigned(t):
                self.emit(d, "%s = %s.wrapping_%s(%s);" % (s.name, s.name, s.op, self.expr(s.value, scope)))
            else:
                self.emit(d, "%s %s= %s;" % (s.name, RUST_BINOP[s.op], self.expr(s.value, scope)))
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
                a = self.expr(sg.arg, scope)
                if sg.kind == "f":
                    self.used.add("fmt_f")
                    a = "bx::fmt_f(%s, %d)" % (a, sg.prec)
                elif sg.kind == "c":
                    self.used.add("chr")
                    a = "bx::chr(%s)" % a
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
                self.emit(d, "return %s;" % self.expr(s.value, scope))
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
        ps = []
        for p in f.params:
            ps.append("%s%s: %s" % ("mut " if p.mutable else "", p.name, p.ty.name))
            scope.declare(p.name, p.ty, p.mutable)
        ret = "" if f.ret == K.UNIT else " -> " + f.ret.name
        self.emit(0, "fn %s(%s)%s {" % (f.name, ", ".join(ps), ret))
        self.block_body(f.body, scope, 1, same=True)
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
    first = True
    for f in program.funcs():
        if not first:
            p.lines.append("")
        first = False
        p.func(f)
    out = "\n".join(p.lines) + "\n"
    mod = fmt.bx_module(p.used)
    if mod:
        out += "\n" + mod
    return out
