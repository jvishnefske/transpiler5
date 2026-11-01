"""Recursive-descent parser for the canonical Rust subset (FR-250).

The parser DEFINES the Rust side of the subset: it accepts exactly what
rs_print emits plus a few trivially-equivalent spellings (any
parenthesization, `println!`, `x = x + e` for `x += e`, numeric literals
with `_` separators), and rejects everything else with a located
``unsupported:`` error.  It does no type inference: every `let` is
explicitly typed and every non-i32 integer literal carries its suffix, so
the Kernel types are read straight off the text.
"""

from . import fmt
from . import kernel as K
from .kernel import Loc
from .rs_lex import LexError, lex

RUST_TYPES = {"i8": K.I8, "u8": K.U8, "i32": K.I32, "i64": K.I64, "u32": K.U32,
              "u64": K.U64, "usize": K.USIZE, "f64": K.F64, "bool": K.BOOL}

BIN_LEVELS = [
    {"||": "or"},
    {"&&": "and"},
    {"==": "eq", "!=": "ne", "<": "lt", "<=": "le", ">": "gt", ">=": "ge"},
    {"|": "bor"},
    {"^": "bxor"},
    {"&": "band"},
    {"<<": "shl", ">>": "shr"},
    {"+": "add", "-": "sub"},
    {"*": "mul", "/": "div", "%": "rem"},
]
COMPOUND = {"+=": "add", "-=": "sub", "*=": "mul", "/=": "div", "%=": "rem", "&=": "band",
            "|=": "bor", "^=": "bxor", "<<=": "shl", ">>=": "shr"}
WRAPPING_METHODS = {"wrapping_add": "add", "wrapping_sub": "sub", "wrapping_mul": "mul"}


class ParseError(Exception):
    def __init__(self, loc, msg):
        Exception.__init__(self, msg)
        self.loc = loc
        self.msg = msg


class _Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    # -------------------------------------------------------------- helpers
    @property
    def tok(self):
        return self.toks[self.i]

    def peek(self, k=1):
        j = min(self.i + k, len(self.toks) - 1)
        return self.toks[j]

    def at(self, text, kind=None):
        t = self.tok
        if kind is not None and t.kind != kind:
            return False
        return t.text == text and t.kind in ("punct", "ident")

    def next(self):
        t = self.tok
        if t.kind != "eof":
            self.i += 1
        return t

    def expect(self, text):
        t = self.tok
        if t.text != text or t.kind not in ("punct", "ident"):
            raise ParseError(t.loc, "expected `%s`, found %s" % (text, self.describe(t)))
        return self.next()

    @staticmethod
    def describe(t):
        if t.kind == "eof":
            return "end of file"
        return "`%s`" % t.text

    def unsupported(self, t, what):
        raise ParseError(t.loc, "unsupported: " + what)

    def ident(self):
        t = self.tok
        if t.kind != "ident":
            raise ParseError(t.loc, "expected an identifier, found %s" % self.describe(t))
        return self.next()

    def type_(self):
        t = self.tok
        if t.kind == "ident" and t.text in RUST_TYPES:
            self.next()
            return RUST_TYPES[t.text]
        if t.text == "&":
            self.unsupported(t, "reference type")
        if t.text == "(":
            self.unsupported(t, "tuple or unit type")
        if t.kind == "ident":
            self.unsupported(t, "type `%s`" % t.text)
        raise ParseError(t.loc, "expected a type, found %s" % self.describe(t))

    # -------------------------------------------------------------- items
    def program(self):
        items = []
        while self.tok.kind != "eof":
            t = self.tok
            if self.at("fn"):
                items.append(self.func())
            elif self.at("mod") and self.peek().text == "bx":
                self.bx_module()
            elif t.text == "#":
                self.unsupported(t, "attribute")
            elif t.kind == "ident" and t.text in ("use", "struct", "enum", "impl", "trait", "static",
                                                  "const", "type", "mod", "pub", "extern", "unsafe",
                                                  "macro_rules"):
                self.unsupported(t, "item `%s`" % t.text)
            else:
                raise ParseError(t.loc, "expected an item, found %s" % self.describe(t))
        return K.Program(tuple(items))

    def bx_module(self):
        start = self.tok
        self.expect("mod")
        self.expect("bx")
        self.expect("{")
        while not self.at("}"):
            t = self.tok
            name = self.peek(2).text if self.at("pub") else None
            if name not in fmt.BX_HELPERS:
                raise ParseError(t.loc, "unsupported: `mod bx` item that is not a canonical bx helper")
            want = [x.text for x in lex(fmt.BX_HELPERS[name])[:-1]]
            got = [x.text for x in self.toks[self.i:self.i + len(want)]]
            if got != want:
                raise ParseError(t.loc, "unsupported: `bx::%s` differs from the canonical helper" % name)
            self.i += len(want)
        self.expect("}")
        return start

    def func(self):
        start = self.expect("fn")
        name = self.ident()
        if self.at("<"):
            self.unsupported(self.tok, "generic function")
        self.expect("(")
        params = []
        while not self.at(")"):
            pt = self.tok
            mutable = False
            if self.at("mut"):
                self.next()
                mutable = True
            pname = self.ident()
            self.expect(":")
            ty = self.type_()
            params.append(K.Param(pname.text, ty, mutable, pt.loc))
            if not self.at(")"):
                self.expect(",")
        self.expect(")")
        ret = K.UNIT
        if self.at("->"):
            self.next()
            ret = self.type_()
        body = self.block()
        return K.Func(name.text, tuple(params), ret, body, start.loc)

    # -------------------------------------------------------------- stmts
    def block(self):
        start = self.expect("{")
        stmts = []
        while not self.at("}"):
            if self.tok.kind == "eof":
                raise ParseError(self.tok.loc, "expected `}`, found end of file")
            stmts.append(self.stmt())
        self.expect("}")
        return K.Block(tuple(stmts), start.loc)

    def stmt(self):
        t = self.tok
        if t.kind == "ident":
            kw = t.text
            if kw == "let":
                return self.let()
            if kw == "if":
                return self.if_()
            if kw == "while":
                self.next()
                cond = self.expr()
                return K.While(cond, self.block(), t.loc)
            if kw == "for":
                return self.for_()
            if kw == "loop":
                self.unsupported(t, "`loop` (C++ has no direct canonical spelling; use `while`)")
            if kw == "match":
                self.unsupported(t, "`match`")
            if kw in ("break", "continue"):
                self.next()
                if self.tok.kind == "punct" and self.tok.text == "'":
                    self.unsupported(self.tok, "loop label")
                if not self.at(";"):
                    self.unsupported(self.tok, "`%s` with a value or label" % kw)
                self.expect(";")
                return K.Break(t.loc) if kw == "break" else K.Continue(t.loc)
            if kw == "return":
                self.next()
                if self.at(";"):
                    self.next()
                    return K.Return(None, t.loc)
                v = self.expr()
                self.expect(";")
                return K.Return(v, t.loc)
            if kw in ("print", "println") and self.peek().text == "!":
                return self.print_(kw == "println")
            if self.peek().text == "!":
                self.unsupported(t, "macro `%s!`" % kw)
            if self.peek().kind == "punct" and self.peek().text == "=":
                self.next()
                self.next()
                v = self.expr()
                self.expect(";")
                return K.Assign(t.text, v, t.loc)
            if self.peek().kind == "punct" and self.peek().text in COMPOUND:
                self.next()
                op = COMPOUND[self.next().text]
                v = self.expr()
                self.expect(";")
                return K.CompoundAssign(op, t.text, v, t.loc, "plain")
            if kw in ("fn", "use", "struct", "const", "static"):
                self.unsupported(t, "nested item `%s`" % kw)
        if t.text == "{" and t.kind == "punct":
            return self.block()
        if t.text == ";" and t.kind == "punct":
            self.unsupported(t, "empty statement")
        e = self.expr()
        if not isinstance(e, K.Call):
            if self.at("}"):
                self.unsupported(t, "tail expression (the subset spells it `return e;`)")
            self.unsupported(t, "expression statement that is not a call")
        self.expect(";")
        return K.ExprStmt(e, t.loc)

    def let(self):
        t = self.expect("let")
        mutable = False
        if self.at("mut"):
            self.next()
            mutable = True
        if self.tok.kind != "ident":
            self.unsupported(self.tok, "pattern in `let`")
        name = self.ident()
        if not self.at(":"):
            self.unsupported(self.tok if self.tok.kind != "eof" else t,
                             "`let` without a type annotation (every declaration is explicitly typed)")
        self.next()
        ty = self.type_()
        if not self.at("="):
            self.unsupported(self.tok, "`let` without an initializer")
        self.next()
        init = self.expr()
        self.expect(";")
        return K.Let(name.text, ty, mutable, init, t.loc)

    def if_(self):
        t = self.expect("if")
        if self.at("let"):
            self.unsupported(self.tok, "`if let`")
        cond = self.expr()
        then = self.block()
        els = None
        if self.at("else"):
            self.next()
            if self.at("if"):
                inner = self.if_()
                els = K.Block((inner,), inner.loc)
            else:
                els = self.block()
        return K.If(cond, then, els, t.loc)

    def for_(self):
        t = self.expect("for")
        if self.tok.kind != "ident":
            self.unsupported(self.tok, "pattern in `for`")
        var = self.ident()
        self.expect("in")
        lo = self.expr()
        if self.at("..="):
            self.unsupported(self.tok, "inclusive range `..=`")
        if not self.at(".."):
            self.unsupported(self.tok, "`for` over something other than a half-open range `a..b`")
        self.next()
        hi = self.expr()
        body = self.block()
        return K.For(var.text, lo, hi, body, t.loc)

    def print_(self, newline):
        t = self.next()
        self.expect("!")
        self.expect("(")
        ft = self.tok
        if ft.kind != "str":
            raise ParseError(ft.loc, "unsupported: print! without a string-literal format")
        self.next()
        text = ft.value + ("\n" if newline else "")
        try:
            parts = fmt.parse_rust_format(text)
        except fmt.FormatError as ex:
            raise ParseError(ft.loc, "unsupported: " + str(ex))
        args = []
        while self.at(","):
            self.next()
            if self.at(")"):
                break
            args.append(self.print_arg())
        self.expect(")")
        self.expect(";")
        holes = [p for p in parts if isinstance(p, tuple)]
        if len(holes) != len(args):
            raise ParseError(t.loc, "print! has %d placeholders and %d arguments" % (len(holes), len(args)))
        segs = []
        it = iter(args)
        for p in parts:
            if isinstance(p, str):
                segs.append(K.Text(p))
                continue
            flag, width = p
            kind, arg, prec, aloc = next(it)
            if kind != "d" and width is not None:
                raise ParseError(aloc, "unsupported: width on a non-integer print! argument")
            segs.append(K.Spec(kind, arg, flag, width, prec))
        return K.Print(tuple(segs), t.loc)

    def print_arg(self):
        t = self.tok
        if t.kind == "ident" and t.text == "bx" and self.peek().text == "::":
            self.next()
            self.next()
            name = self.ident()
            self.expect("(")
            e = self.expr()
            if name.text == "fmt_f":
                self.expect(",")
                pt = self.tok
                if pt.kind != "int" or pt.suffix not in ("", "usize"):
                    raise ParseError(pt.loc, "unsupported: bx::fmt_f precision must be an integer literal")
                self.next()
                self.expect(")")
                return ("f", e, pt.value, t.loc)
            if name.text == "chr":
                self.expect(")")
                return ("c", e, None, t.loc)
            self.unsupported(name, "`bx::%s`" % name.text)
        if t.kind == "str" and self.peek().text in (",", ")"):
            self.next()
            return ("s", K.StrLit(t.value, t.loc), None, t.loc)
        return ("d", self.expr(), None, t.loc)

    # -------------------------------------------------------------- exprs
    def expr(self):
        return self.binary(0)

    def binary(self, level):
        if level == len(BIN_LEVELS):
            return self.cast()
        ops = BIN_LEVELS[level]
        lhs = self.binary(level + 1)
        while self.tok.kind == "punct" and self.tok.text in ops:
            t = self.next()
            rhs = self.binary(level + 1)
            lhs = K.Binary(ops[t.text], lhs, rhs, t.loc, "plain")
            if level == 2:
                if self.tok.kind == "punct" and self.tok.text in ops:
                    raise ParseError(self.tok.loc, "comparison operators cannot be chained")
                break
        return lhs

    def cast(self):
        e = self.unary()
        while self.at("as"):
            t = self.next()
            ty = self.type_()
            self.no_inferred_literal(e)
            e = K.Cast(ty, e, t.loc)
        return e

    @staticmethod
    def no_inferred_literal(e):
        """rustc types an unsuffixed integer literal in a cast operand FROM
        THE CAST TARGET (`(1 << 40) as i64` shifts an i64; `200 as i8` is
        a hard error), while the subset reads every unsuffixed literal as
        i32.  Rather than model inference, the subset requires the suffix
        there -- exactly what rs_print emits (`(1i32 << 40i32) as i64`).
        Call arguments are typed by the callee, so the walk stops at calls.
        """
        stack = [e]
        while stack:
            n = stack.pop()
            if isinstance(n, K.IntLit) and n.unsuffixed:
                raise ParseError(n.loc, "unsupported: unsuffixed integer literal in the operand of "
                                        "`as` (rustc infers its type from the cast target); "
                                        "write `%di32`" % n.value)
            if isinstance(n, K.Unary):
                stack.append(n.operand)
            elif isinstance(n, K.Binary):
                stack.extend([n.lhs, n.rhs])
            elif isinstance(n, K.Cast):
                stack.append(n.operand)

    def unary(self):
        t = self.tok
        if t.kind == "punct":
            if t.text == "-":
                self.next()
                return K.Unary("neg", self.unary(), t.loc, "plain")
            if t.text == "!":
                self.next()
                return K.Unary("not", self.unary(), t.loc, "plain")
            if t.text in ("&", "&&"):
                self.unsupported(t, "reference `&`")
            if t.text == "*":
                self.unsupported(t, "dereference `*`")
        return self.postfix()

    def postfix(self):
        e = self.primary()
        while True:
            t = self.tok
            if t.kind == "punct" and t.text == ".":
                self.next()
                m = self.tok
                if m.kind != "ident":
                    self.unsupported(m, "field access")
                self.next()
                if m.text in WRAPPING_METHODS:
                    self.expect("(")
                    arg = self.expr()
                    self.expect(")")
                    e = K.Binary(WRAPPING_METHODS[m.text], e, arg, m.loc, "wrapping")
                    continue
                if m.text == "wrapping_neg":
                    self.expect("(")
                    self.expect(")")
                    e = K.Unary("neg", e, m.loc, "wrapping")
                    continue
                if self.at("("):
                    self.unsupported(m, "method call `.%s()`" % m.text)
                self.unsupported(m, "field access `.%s`" % m.text)
            if t.kind == "punct" and t.text == "[":
                self.unsupported(t, "indexing")
            if t.kind == "punct" and t.text == "?":
                self.unsupported(t, "`?` operator")
            return e

    def primary(self):
        t = self.tok
        if t.kind == "int":
            self.next()
            if t.suffix == "":
                ty = K.I32
            elif t.suffix in RUST_TYPES:
                ty = RUST_TYPES[t.suffix]
            else:
                self.unsupported(t, "integer type `%s`" % t.suffix)
            return K.IntLit(t.value, ty, t.loc, unsuffixed=(t.suffix == ""))
        if t.kind == "float":
            self.next()
            v = t.value
            if v != v or v == float("inf"):
                self.unsupported(t, "non-finite float literal")
            return K.FloatLit(repr(v), t.loc)
        if t.kind == "str":
            self.next()
            return K.StrLit(t.value, t.loc)
        if t.kind == "ident":
            if t.text in ("true", "false"):
                self.next()
                return K.BoolLit(t.text == "true", t.loc)
            if t.text in ("if", "match", "loop", "while", "for", "unsafe", "move", "return",
                          "break", "continue", "let"):
                self.unsupported(t, "`%s` expression" % t.text)
            self.next()
            if self.at("::"):
                self.unsupported(t, "path expression `%s::..`" % t.text)
            if self.at("!"):
                self.unsupported(t, "macro `%s!` in an expression" % t.text)
            if self.at("("):
                self.next()
                args = []
                while not self.at(")"):
                    args.append(self.expr())
                    if not self.at(")"):
                        self.expect(",")
                self.expect(")")
                return K.Call(t.text, tuple(args), t.loc)
            return K.Var(t.text, t.loc)
        if t.kind == "punct" and t.text == "(":
            self.next()
            if self.at(")"):
                self.unsupported(t, "unit value `()`")
            e = self.expr()
            if self.at(","):
                self.unsupported(self.tok, "tuple")
            self.expect(")")
            return e
        if t.kind == "punct" and t.text == "|":
            self.unsupported(t, "closure")
        if t.kind == "punct" and t.text == "[":
            self.unsupported(t, "array expression")
        raise ParseError(t.loc, "expected an expression, found %s" % self.describe(t))


def rs_parse_surface(src):
    """Parse Rust source to an UN-normalized Kernel (raises ParseError)."""
    try:
        toks = lex(src)
    except LexError as ex:
        raise ParseError(ex.loc, ex.msg)
    return _Parser(toks).program()


def rs_parse(src):
    """Parse + Rust-surface checks + normalize.  Raises ParseError listing
    the FIRST problem; use ``rs_parse_all`` for every well-formedness error."""
    prog, errs = rs_parse_all(src)
    if errs:
        loc, msg = errs[0]
        raise ParseError(loc, msg)
    return prog


def rs_parse_all(src):
    """(normalized program or None, [(loc, msg)...])"""
    from . import typecheck as TC
    try:
        surf = rs_parse_surface(src)
    except ParseError as ex:
        return None, [(ex.loc, ex.msg)]
    errs = TC.check(surf, surface="rust")
    if errs:
        return None, errs
    prog = TC.normalize(surf)
    errs = TC.check(prog)
    return (None, errs) if errs else (prog, [])
