"""Recursive-descent parser for the canonical Rust subset (FR-250).

The parser DEFINES the Rust side of the subset: it accepts exactly what
rs_print emits plus a few trivially-equivalent spellings (any
parenthesization, `println!`, `x = x + e` for `x += e`, numeric literals
with `_` separators), and rejects everything else with a located
``unsupported:`` error.  It does no type inference: every `let` is
explicitly typed and every non-i32 integer literal carries its suffix, so
the Kernel types are read straight off the text.

FR-251: `Vec<T>`, `String`, `&T`, `&mut T`; `&e` / `&mut e` parse to a
SURFACE ``Borrow`` and `*x` to ``Var(x, spelling='deref')`` -- the
checker (surface='rust') verifies they sit exactly where the canonical
spelling puts them, and normalize drops them.  Statement-only methods
(`.push`, `.pop`, `.clear`, `.resize`, `.push_str`, `bx::push_byte`) and
the range-for iterator adaptors (`.iter()`, `.iter_mut()`,
`.iter().copied()`, `.iter().cloned()`) parse to private markers that
are only legal in their statement / `for` position.

FR-260: `mod a { .. }` at the root, `pub mod b { .. }` nested, `pub fn`
(or a private `fn`, a C++ anonymous-namespace member) inside a module.
Calls are RESOLVED here with Rust's own rules, to the absolute Kernel path:
a bare `f` names a function of the CURRENT module only (a Rust path never
searches enclosing modules -- C++ unqualified lookup would, so such a call
is a located error, not a guess); `crate::`, `self::`, `super::` and a
relative child-module path `b::f` are accepted.  `use`, restricted
visibility and a reopened module (E0428 in Rust) are located errors.

FR-261: `&[T]` / `&mut [T]` (pointers), `&x[k..]` / `&mut x[..]` (re-slices,
an Offset), `std::slice::from_ref(&x)` / `std::slice::from_mut(&mut x)`
(an AddrOf); `p = &p[k..];` is normalized to `p += k` (N18).  A bare
`x[k..]` and a range with an end are located errors.

FR-259: `i16` / `u16`; an `if c { a } else { b }` EXPRESSION (each arm one
expression, `else if` chains) is a Cond; a block expression is accepted
only as the canonical increment `{ x += 1; x }` / `{ x += 1; x - 1 }` /
`{ x -= 1; x }` / `{ x -= 1; x + 1 }` (unsigned `{ x =
x.wrapping_add(1u32); x }` ..; `*x` for a reference), an IncDec.  Neither
may be followed, unparenthesized, by a method call, an index, `as` or a
binary operator (rustc's statement-position rules make those fragile; the
printer parenthesizes them).

FR-257: `#[derive(Clone)] [pub] struct S { pub f: T, .. }` (exactly that
attribute; every field `pub`; a private struct in a module is internal),
one `impl S { pub fn .. }` per struct in its module (`&self`, `&mut self`,
none; `new` is the constructor), field access `e.f`, method calls `e.m(..)`
(any name that is not a Vec/String/iterator row), struct literals
`Path { f: e, g }` (not at the top level of an `if`/`while` condition or a
`for` iterable, like rustc).  Struct types and literal paths are resolved
after the whole crate is read, with the rules calls use; a literal must list
every field in declaration order.

FR-252: `let [mut] f = [move] |a: T, ..| [-> R] { .. };` (every parameter
typed, a block body; the `let mut` is checked against the derived FnMut-ness
by the checker), `&impl Fn(..) -> R` / `&mut impl FnMut(..) -> R` parameter
types, and a bare call `f(..)` of a closure or callable parameter in scope
(block-scoped, so a Rust local hides a function of its name) as a
ClosureCall; any other `|..|` is a located error.
"""

from dataclasses import dataclass, field
from typing import Optional, Tuple

from . import fmt
from . import kernel as K
from .kernel import Loc
from .rs_lex import LexError, lex

RUST_TYPES = {"i8": K.I8, "u8": K.U8, "i16": K.I16, "u16": K.U16, "i32": K.I32, "i64": K.I64, "u32": K.U32,
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


@dataclass(frozen=True)
class _MStmt:
    """A statement-only method call (surface marker, never in a Kernel)."""

    kind: str
    target: object
    args: Tuple[object, ...]
    loc: Optional[Loc] = field(default=None, compare=False)


@dataclass(frozen=True)
class _Iter:
    """`v.iter()` & co (surface marker, only legal as a `for` iterable)."""

    mode: str  # 'ref' 'mut' 'copied' 'cloned'
    recv: object
    loc: Optional[Loc] = field(default=None, compare=False)


STMT_METHODS = {"push": 1, "pop": 0, "clear": 0, "resize": 2, "push_str": 1,
                # FR-253
                "sort_unstable": 0, "sort": 0, "reverse": 0, "fill": 1, "extend": 1}


@dataclass(frozen=True)
class _IterMap:
    """FR-253: `v.iter().copied().map(&f)` (surface marker, only legal as
    the argument of `w.extend(..)`)."""

    recv: object
    fn: object
    loc: Optional[Loc] = field(default=None, compare=False)


@dataclass(frozen=True)
class _Unwrap:
    """FR-253: `v.iter().max().unwrap()` (surface marker, only legal under
    `*`: the extreme element's value)."""

    op: str  # 'max_element' | 'min_element'
    recv: object
    loc: Optional[Loc] = field(default=None, compare=False)


@dataclass(frozen=True)
class _SliceIdx:
    """FR-261: `x[k..]` / `x[..]` (surface marker, only legal under `&` /
    `&mut`, where it becomes a Kernel Offset)."""

    base: object
    start: object
    loc: Optional[Loc] = field(default=None, compare=False)


@dataclass(frozen=True)
class _RawPath:
    """FR-257: a struct path as written, before resolution (never in a Kernel)."""

    raw: str
    ns: Tuple[str, ...]
    loc: Optional[Loc] = field(default=None, compare=False)

    def __str__(self):
        return self.raw


@dataclass(frozen=True)
class _RawConst:
    """FR-264: a value path `crate::a::k` as written (never in a Kernel)."""

    raw: str
    loc: Optional[Loc] = field(default=None, compare=False)


class ParseError(Exception):
    def __init__(self, loc, msg):
        Exception.__init__(self, msg)
        self.loc = loc
        self.msg = msg


class _Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0
        self.ns = ()  # FR-260: the module being parsed
        self.no_struct = False  # FR-257: an if/while condition or for iterable
        self.impls = []  # FR-257: (struct raw name, ns, [Func], loc)
        self.bx_defined = set()  # helpers defined in `mod bx`
        self.bx_uses = []  # (helper name, token): each must be defined (rustc E0433)
        self.closures = [set()]  # FR-252: closure / callable-parameter names per block scope

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

    def close_angle(self):
        """Consume one `>`, splitting a `>>` token (`Vec<Vec<i32>>`)."""
        t = self.tok
        if t.kind == "punct" and t.text == ">>":
            from .rs_lex import Tok
            self.toks[self.i] = Tok("punct", ">", Loc(t.loc.line, t.loc.col + 1))
            return t
        return self.expect(">")

    def type_(self):
        t = self.tok
        if t.kind == "ident" and t.text in RUST_TYPES:
            self.next()
            return RUST_TYPES[t.text]
        if t.kind == "ident" and t.text == "String":
            self.next()
            return K.STRING
        if t.kind == "ident" and t.text == "Vec" and self.peek().text == "<":
            self.next()
            self.next()
            el = self.type_()
            self.close_angle()
            return K.VecT(el)
        if t.text == "&" and t.kind == "punct":
            self.next()
            if self.tok.kind == "punct" and self.tok.text == "'":
                self.unsupported(self.tok, "lifetime")
            mut = False
            if self.at("mut"):
                self.next()
                mut = True
            if self.at("impl"):
                return K.RefT(self.fn_type(t, mut), mut)
            if self.at("["):
                # FR-261: a slice `&[T]` / `&mut [T]` is a pointer
                self.next()
                inner = self.type_()
                if self.at(";"):
                    self.unsupported(self.tok, "array type")
                self.expect("]")
                if isinstance(inner, (K.RefT, K.PtrT)):
                    self.unsupported(t, "slice of references or slices")
                return K.PtrT(inner, mut)
            inner = self.type_()
            if isinstance(inner, (K.RefT, K.PtrT)):
                self.unsupported(t, "reference to a reference")
            return K.RefT(inner, mut)
        if t.text == "&&" and t.kind == "punct":
            self.unsupported(t, "reference to a reference")
        if t.text == "&":
            self.unsupported(t, "reference type")
        if t.text == "(":
            self.unsupported(t, "tuple or unit type")
        if t.kind == "ident" and t.text not in _tc().RUST_KEYWORDS | {"crate", "self", "super"} \
                and self.peek().text != "<":
            # FR-257: a struct, by a path resolved after the whole crate is
            # read (an unknown name is reported then, as `type `X``)
            raw = self.path_segments()
            return K.StructT(_RawPath(raw, self.ns, t.loc))
        if t.kind == "ident" and t.text in ("crate", "self", "super"):
            raw = self.path_segments()
            return K.StructT(_RawPath(raw, self.ns, t.loc))
        if t.kind == "ident":
            self.unsupported(t, "type `%s`" % t.text)
        raise ParseError(t.loc, "expected a type, found %s" % self.describe(t))

    def fn_type(self, amp, mut):
        """FR-252: `impl Fn(T, ..) -> R` after `&`, `impl FnMut(..)` after
        `&mut` (a callable parameter; C++ `const F&` / `F&`)."""
        self.expect("impl")
        tr = self.ident()
        want = "FnMut" if mut else "Fn"
        if tr.text not in ("Fn", "FnMut", "FnOnce"):
            self.unsupported(tr, "trait `impl %s`" % tr.text)
        if tr.text != want:
            self.unsupported(tr, "`&%simpl %s` (the subset spells a callable parameter `&impl Fn(..)` or "
                                 "`&mut impl FnMut(..)`)" % ("mut " if mut else "", tr.text))
        self.expect("(")
        ps = []
        while not self.at(")"):
            ps.append(self.type_())
            if not self.at(")"):
                self.expect(",")
        self.expect(")")
        ret = K.UNIT
        if self.at("->"):
            self.next()
            ret = self.type_()
        return K.FnT(tuple(ps), ret)

    def path_segments(self):
        segs = [self.ident().text]
        while self.at("::") and self.peek().kind == "ident":
            self.next()
            segs.append(self.ident().text)
        return "::".join(segs)

    # -------------------------------------------------------------- items
    def program(self):
        # FR-264: the one inner attribute the subset prints (with constants)
        want = ["#", "!", "[", "allow", "(", "non_upper_case_globals", ")", "]"]
        if [x.text for x in self.toks[self.i:self.i + len(want)]] == want:
            self.i += len(want)
        prog = K.Program(tuple(self.items(top=True)))
        for name, t in self.bx_uses:
            if name not in self.bx_defined:
                self.unsupported(t, "`bx::%s` without its definition in `mod bx` (rustc E0433)" % name)
        return prog

    def items(self, top):
        items = []
        seen_mods = set()
        while not (self.tok.kind == "eof" if top else self.at("}")):
            t = self.tok
            if self.tok.kind == "eof":
                raise ParseError(t.loc, "expected `}`, found end of file")
            derive = None
            if t.text == "#" and t.kind == "punct":
                derive = self.attribute()
                t = self.tok
            pub = False
            if self.at("pub"):
                if self.peek().text == "(":
                    self.unsupported(t, "restricted visibility `pub(..)`")
                if top:
                    self.unsupported(t, "`pub` at the crate root (the subset spells root items without it)")
                pub = True
                self.next()
            if derive is not None and not self.at("struct"):
                self.unsupported(derive, "attribute on something other than a struct")
            if self.at("struct"):
                if derive is None:
                    self.unsupported(t, "struct without `#[derive(Clone)]` (the subset's structs are Clone, "
                                        "like every copyable C++ aggregate)")
                sd = self.struct_()
                # a private struct in a module is a C++ anonymous-namespace member
                items.append(K.StructDef(sd.name, sd.fields, sd.methods, sd.loc, sd.ns, bool(self.ns) and not pub))
            elif self.at("impl"):
                if pub:
                    self.unsupported(t, "`pub impl`")
                self.impl_()
            elif self.at("fn"):
                f = self.func()
                items.append(K.Func(f.name, f.params, f.ret, f.body, f.loc, self.ns, bool(self.ns) and not pub))
            elif self.at("const"):
                # FR-264: `[pub] const k: T = e;`
                self.next()
                name = self.ident()
                self.expect(":")
                ty = self.type_()
                self.expect("=")
                e = self.free_expr()
                self.expect(";")
                items.append(K.ConstItem(name.text, ty, e, name.loc, self.ns, bool(self.ns) and not pub))
            elif self.at("mod") and top and not pub and self.peek().text == "bx":
                self.bx_module()
            elif self.at("mod"):
                if not top and not pub:
                    self.unsupported(t, "private nested module (the subset spells it `pub mod`: a private "
                                        "one is unreachable from the crate root)")
                self.next()
                name = self.ident()
                p = _tc().ident_problem(name.text) or (
                    "identifier '%s' names a Rust prelude type" % name.text
                    if name.text in _tc().NS_RESERVED else None)
                if p:
                    raise ParseError(name.loc, "unsupported: module name: " + p)
                if name.text in seen_mods:
                    raise ParseError(name.loc, "unsupported: module '%s' defined twice (a Rust module cannot be "
                                               "reopened; the subset merges a reopened C++ namespace into one "
                                               "`mod`)" % name.text)
                seen_mods.add(name.text)
                self.expect("{")
                saved = self.ns
                self.ns = saved + (name.text,)
                try:
                    items.extend(self.items(top=False))
                finally:
                    self.ns = saved
                self.expect("}")
            elif pub:
                self.unsupported(self.tok, "`pub` item other than `pub fn` / `pub mod` / `pub const`")
            elif t.kind == "ident" and t.text in ("use", "struct", "enum", "impl", "trait", "static",
                                                  "const", "type", "mod", "pub", "extern", "unsafe",
                                                  "macro_rules"):
                self.unsupported(t, "item `%s`" % t.text)
            else:
                raise ParseError(t.loc, "expected an item, found %s" % self.describe(t))
        return items

    # ------------------------------------------------------------ FR-257
    def attribute(self):
        """Exactly `#[derive(Clone)]`."""
        t = self.expect("#")
        want = ["[", "derive", "(", "Clone", ")", "]"]
        got = [x.text for x in self.toks[self.i:self.i + len(want)]]
        if got != want:
            self.unsupported(t, "attribute other than `#[derive(Clone)]` on a struct")
        self.i += len(want)
        return t

    def struct_(self):
        start = self.expect("struct")
        name = self.ident()
        if self.at("<"):
            self.unsupported(self.tok, "generic struct")
        if self.at("(") or self.at(";"):
            self.unsupported(self.tok, "tuple or unit struct")
        self.expect("{")
        fields = []
        while not self.at("}"):
            ft = self.tok
            if not self.at("pub"):
                self.unsupported(ft, "private field (the subset's data members are public: `pub %s`)" % ft.text)
            self.next()
            if self.at("("):
                self.unsupported(ft, "restricted visibility `pub(..)`")
            fname = self.ident()
            self.expect(":")
            fty = self.type_()
            fields.append(K.FieldDef(fname.text, fty, fname.loc))
            if not self.at("}"):
                self.expect(",")
        self.expect("}")
        return K.StructDef(name.text, tuple(fields), (), start.loc, self.ns, False)

    def impl_(self):
        start = self.expect("impl")
        if self.at("<"):
            self.unsupported(self.tok, "generic impl")
        nt = self.tok
        name = self.path_segments()
        if self.at("for"):
            self.unsupported(self.tok, "trait implementation (`impl Trait for S`)")
        if "::" in name:
            self.unsupported(nt, "`impl` of a struct of another module")
        self.expect("{")
        methods = []
        while not self.at("}"):
            mt = self.tok
            if self.tok.kind == "eof":
                raise ParseError(mt.loc, "expected `}`, found end of file")
            if not self.at("pub"):
                self.unsupported(mt, "private member function (the subset spells it `pub fn`)")
            self.next()
            if self.at("("):
                self.unsupported(mt, "restricted visibility `pub(..)`")
            if not self.at("fn"):
                self.unsupported(self.tok, "`impl` item other than `pub fn`")
            methods.append(self.func(owner=name))
        self.expect("}")
        self.impls.append((name, self.ns, methods, start.loc, nt.loc))

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
            self.bx_defined.add(name)
        self.expect("}")
        return start

    def func(self, owner=None):
        start = self.expect("fn")
        name = self.ident()
        if self.at("<"):
            self.unsupported(self.tok, "generic function")
        self.expect("(")
        params = []
        recv = "" if owner is None else "static"
        if owner is not None and (self.at("&") and self.peek().text in ("self", "mut")):
            st = self.next()
            if self.at("mut"):
                self.next()
                recv = "mut"
            else:
                recv = "ref"
            if not self.at("self"):
                self.unsupported(st, "reference parameter in the receiver position")
            self.next()
            if not self.at(")"):
                self.expect(",")
        elif owner is not None and (self.at("self") or (self.at("mut") and self.peek().text == "self")):
            self.unsupported(self.tok, "member function taking `self` by value (C++ has no by-value `this`)")
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
        # FR-252: a callable parameter is called like a closure
        self.closures = [{p.name for p in params if isinstance(p.ty, K.RefT) and isinstance(p.ty.inner, K.FnT)}]
        body = self.block()
        if owner is not None:
            return K.Func(name.text, tuple(params), ret, body, start.loc, self.ns, False, owner, recv)
        return K.Func(name.text, tuple(params), ret, body, start.loc)

    # -------------------------------------------------------------- stmts
    def block(self):
        start = self.expect("{")
        stmts = []
        self.closures.append(set())
        try:
            while not self.at("}"):
                if self.tok.kind == "eof":
                    raise ParseError(self.tok.loc, "expected `}`, found end of file")
                stmts.append(self.stmt())
        finally:
            self.closures.pop()
        self.expect("}")
        return K.Block(tuple(stmts), start.loc)

    def is_closure(self, name):
        """FR-252: does ``name`` (a bare call) name a closure in scope?  A
        Rust local hides a function of the same name; any other local name
        shadowing it hides the closure too."""
        return any(name in sc for sc in self.closures)

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
                cond = self.cond_expr()
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
        if isinstance(e, _MStmt):
            self.expect(";")
            return self.method_stmt(e, t)
        if self.tok.kind == "punct" and (self.tok.text == "=" or self.tok.text in COMPOUND):
            op_tok = self.next()
            v = self.expr()
            self.expect(";")
            if isinstance(e, K.Var) and e.spelling == "deref":
                if op_tok.text == "=":
                    return K.Assign(e.name, v, t.loc)
                return K.CompoundAssign(COMPOUND[op_tok.text], e.name, v, t.loc, "plain")
            if isinstance(e, (K.Index, K.Field)):
                if op_tok.text == "=":
                    return K.Store(e, v, t.loc)
                return K.CompoundStore(COMPOUND[op_tok.text], e, v, t.loc, "plain")
            self.unsupported(t, "assignment to something other than a variable or an element")
        if not isinstance(e, (K.Call, K.MethodCall, K.ClosureCall)):
            if self.at("}"):
                self.unsupported(t, "tail expression (the subset spells it `return e;`)")
            self.unsupported(t, "expression statement that is not a call")
        self.expect(";")
        return K.ExprStmt(e, t.loc)

    def method_stmt(self, m, t):
        L = t.loc
        if m.kind == "push":
            return K.Push(m.target, m.args[0], L)
        if m.kind == "pop":
            return K.Pop(m.target, L)
        if m.kind == "clear":
            return K.Clear(m.target, L)
        if m.kind == "resize":
            return K.Resize(m.target, m.args[0], m.args[1], L)
        if m.kind == "push_str":
            return K.PushStr(m.target, m.args[0], L)
        if m.kind == "push_byte":
            return K.PushByte(m.target, m.args[0], L)
        # FR-253
        kind = m.kind
        if kind in ("sort_unstable", "sort", "reverse"):
            return K.AlgStmt({"sort_unstable": "sort", "sort": "stable_sort"}.get(kind, kind), m.target, None, (), L)
        if kind == "fill":
            return K.AlgStmt("fill", m.target, None, m.args, L)
        if kind in ("sort_by", "transform"):
            return K.AlgStmt(kind, m.target, None, m.args, L)
        if kind == "for_each":
            return K.AlgStmt("for_each", None, m.target, m.args, L)
        if kind == "extend":
            it = m.args[0]
            if not isinstance(it, _IterMap):
                raise ParseError(getattr(it, "loc", None) or L, "unsupported: `.extend()` of something other than "
                                                                "`v.iter().copied().map(&f)`")
            return K.AlgStmt("extend_map", m.target, it.recv, (it.fn,), L)
        raise ParseError(L, "unsupported: method `.%s()`" % m.kind)

    def let(self):
        t = self.expect("let")
        mutable = False
        if self.at("mut"):
            self.next()
            mutable = True
        if self.tok.kind != "ident":
            self.unsupported(self.tok, "pattern in `let`")
        name = self.ident()
        if self.at("=") and (self.peek().text in ("|", "||", "move")):
            return self.closure(t, name, mutable)
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

    def closure(self, t, name, mutable):
        """FR-252: `let [mut] f = [move] |a: T, ..| [-> R] { body };`."""
        self.expect("=")
        move = False
        if self.at("move"):
            self.next()
            move = True
        params = []
        if self.at("||"):
            self.next()
        else:
            self.expect("|")
            while not self.at("|"):
                pt = self.tok
                pm = False
                if self.at("mut"):
                    self.next()
                    pm = True
                pname = self.ident()
                if not self.at(":"):
                    self.unsupported(self.tok, "closure parameter without a type annotation")
                self.next()
                ty = self.type_()
                params.append(K.Param(pname.text, ty, pm, pt.loc))
                if not self.at("|"):
                    self.expect(",")
            self.expect("|")
        ret = K.UNIT
        if self.at("->"):
            self.next()
            ret = self.type_()
        if not self.at("{"):
            self.unsupported(self.tok, "closure body that is not a block")
        saved = self.closures
        # a call of an outer closure in the body stays a ClosureCall, so the
        # checker can name it (a closure is not captured by another one)
        self.closures = [set(), {n for sc in saved for n in sc}]
        try:
            body = self.block()
        finally:
            self.closures = saved
        self.expect(";")
        self.closures[-1].add(name.text)
        return K.Closure(name.text, move, tuple(params), ret, body, t.loc, "let mut" if mutable else "let")

    def if_(self):
        t = self.expect("if")
        if self.at("let"):
            self.unsupported(self.tok, "`if let`")
        cond = self.cond_expr()
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
        mutable = False
        if self.at("mut"):
            self.next()
            mutable = True
        if self.tok.kind != "ident":
            self.unsupported(self.tok, "pattern in `for`")
        var = self.ident()
        self.expect("in")
        lo = self.cond_expr()
        if isinstance(lo, _Iter):
            body = self.block()
            if lo.mode in ("copied", "cloned"):
                return K.ForEach(var.text, mutable, "copy", lo.recv, body, t.loc)
            if mutable:
                self.unsupported(t, "`for mut` over references")
            return K.ForEach(var.text, False, lo.mode, lo.recv, body, t.loc)
        if mutable:
            self.unsupported(t, "`for mut` over a range")
        if self.at("..="):
            self.unsupported(self.tok, "inclusive range `..=`")
        if not self.at(".."):
            self.unsupported(self.tok, "`for` over something other than a half-open range `a..b`")
        self.next()
        hi = self.cond_expr()
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

    def take_crate_bx(self):
        """FR-260: `crate::bx::` leaves the cursor on `bx`; a bare `bx::`
        inside a module would resolve relative to it (rustc E0433)."""
        t = self.tok
        if t.kind != "ident":
            return
        if (t.text == "crate" and self.peek().text == "::" and self.peek(2).text == "bx"
                and self.peek(3).text == "::"):
            self.next()
            self.next()
        elif (t.text == "bx" and self.peek().text == "::" and self.ns
              and not (self.i >= 2 and self.toks[self.i - 2].text == "crate"
                       and self.toks[self.i - 1].text == "::")):
            self.unsupported(t, "`bx::` inside a module resolves relative to it (the subset spells it "
                                "`crate::bx::`)")

    def print_arg(self):
        self.take_crate_bx()
        t = self.tok
        if (t.kind == "ident" and t.text == "bx" and self.peek().text == "::"
                and self.peek(2).text in ("fmt_f", "chr")):
            self.next()
            self.next()
            name = self.ident()
            self.bx_uses.append((name.text, t))
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

    def cond_expr(self):
        """An expression followed by a block: like rustc, no struct literal
        at its top level (`if x == S { .. }` would be ambiguous)."""
        saved, self.no_struct = self.no_struct, True
        try:
            return self.expr()
        finally:
            self.no_struct = saved

    def free_expr(self):
        """Inside parentheses / brackets / arguments: struct literals again."""
        saved, self.no_struct = self.no_struct, False
        try:
            return self.expr()
        finally:
            self.no_struct = saved

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
            elif isinstance(n, K.Cond):
                stack.extend([n.then, n.els])  # (the arms take the cast's expectation too)

    def unary(self):
        t = self.tok
        if t.kind == "punct":
            if t.text == "-":
                self.next()
                return K.Unary("neg", self.unary(), t.loc, "plain")
            if t.text == "!":
                self.next()
                return K.Unary("not", self.unary(), t.loc, "plain")
            if t.text == "&":
                self.next()
                mut = False
                if self.at("mut"):
                    self.next()
                    mut = True
                e = self.unary()
                if isinstance(e, _SliceIdx):
                    # FR-261: `&p[k..]` / `&mut v[..]` is a re-slice
                    return K.Offset(e.base, e.start, getattr(e.base, "loc", None) or t.loc,
                                    "&mut" if mut else "&")
                return K.Borrow(e, mut, t.loc)
            if t.text == "&&":
                self.unsupported(t, "reference `&&`")
            if t.text == "*":
                self.next()
                e = self.unary()
                if isinstance(e, _Unwrap):
                    return K.Alg(e.op, e.recv, (), t.loc)  # FR-253: `*v.iter().max().unwrap()`
                if not isinstance(e, K.Var) or e.spelling:
                    self.unsupported(t, "dereference `*` of something other than a reference variable")
                return K.Var(e.name, e.loc, "deref")
        return self.postfix()

    def postfix(self):
        start = self.tok
        e = self.primary()
        if isinstance(e, (K.Cond, K.IncDec)) and not (start.kind == "punct" and start.text == "("):
            t = self.tok
            if t.kind == "punct" and (t.text in (".", "[", "?") or any(t.text in lv for lv in BIN_LEVELS)) \
                    or (t.kind == "ident" and t.text == "as"):
                self.unsupported(t, "`%s` after an unparenthesized %s (the subset parenthesizes it)" % (
                    t.text, "`if` expression" if isinstance(e, K.Cond) else "block expression"))
        while True:
            t = self.tok
            if t.kind == "punct" and t.text == ".":
                self.next()
                m = self.tok
                if m.kind != "ident":
                    self.unsupported(m, "field access")
                self.next()
                if not self.at("(") and m.text not in WRAPPING_METHODS and m.text != "wrapping_neg":
                    if isinstance(e, (_MStmt, _Iter, _IterMap, _Unwrap)):
                        self.unsupported(m, "field access `.%s`" % m.text)
                    e = K.Field(e, m.text, getattr(e, "loc", None) or m.loc)  # located at its start, like C++
                    continue
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
                e = self.method(e, m)
                continue
            if t.kind == "punct" and t.text == "[":
                self.next()
                if self.at(".."):
                    self.next()
                    self.expect("]")
                    e = _SliceIdx(e, None, t.loc)
                    continue
                idx = self.free_expr()
                if self.at(".."):
                    self.next()
                    if not self.at("]"):
                        self.unsupported(self.tok, "slice range with an end (the subset's pointers are suffix "
                                                   "slices `&p[k..]`)")
                    self.next()
                    e = _SliceIdx(e, idx, t.loc)
                    continue
                if self.at("..="):
                    self.unsupported(self.tok, "inclusive range `..=`")
                self.expect("]")
                e = K.Index(e, idx, getattr(e, "loc", None) or t.loc)  # located at its start, like C++
                continue
            if t.kind == "punct" and t.text == "?":
                self.unsupported(t, "`?` operator")
            return e

    def method(self, recv, m):
        """`.name(...)` after ``recv`` (the `.` and name are consumed)."""
        name = m.text
        if not self.at("("):
            self.unsupported(m, "field access `.%s`" % name)
        if isinstance(recv, _Iter) and name in ("max", "min", "fold", "any", "all", "for_each", "map"):
            return self.iter_method(recv, m)  # FR-253 (any other name keeps the generic wording)
        if isinstance(recv, (_MStmt, _Iter, _IterMap, _Unwrap)):
            self.unsupported(m, "method call `.%s()`" % name)
        if name == "contains":
            args = self.call_args()
            if len(args) != 1:
                self.unsupported(m, "`.contains()` with %d arguments" % len(args))
            return K.Alg("contains", recv, (args[0],), getattr(recv, "loc", None) or m.loc)  # FR-253
        if name in ("len", "is_empty", "clone", "to_string", "as_bytes", "iter", "iter_mut") \
                or (name in STMT_METHODS and STMT_METHODS[name] == 0):
            self.expect("(")
            self.expect(")")
            if name == "len":
                return K.Len(recv, m.loc)
            if name == "is_empty":
                return K.IsEmpty(recv, m.loc)
            if name == "clone":
                return K.Clone(recv, m.loc)
            if name == "to_string":
                return K.ToStr(recv, m.loc)
            if name == "as_bytes":
                lb = self.tok
                if not self.at("["):
                    self.unsupported(m, "`.as_bytes()` other than `s.as_bytes()[i] as i8`")
                self.next()
                idx = self.expr()
                self.expect("]")
                if not self.at("as") or self.peek().text != "i8":
                    self.unsupported(lb, "byte read other than `s.as_bytes()[i] as i8` (std::string's "
                                         "char is signed)")
                self.next()
                self.next()
                return K.ByteAt(recv, idx, m.loc)
            if name == "iter":
                if self.at(".") and self.peek().text in ("copied", "cloned") and self.peek(2).text == "(":
                    self.next()
                    how = self.next().text
                    self.expect("(")
                    self.expect(")")
                    return _Iter(how, recv, m.loc)
                return _Iter("ref", recv, m.loc)
            if name == "iter_mut":
                return _Iter("mut", recv, m.loc)
            return _MStmt(name, recv, (), m.loc)
        if name in STMT_METHODS:
            self.expect("(")
            args = []
            while not self.at(")"):
                args.append(self.free_expr())
                if not self.at(")"):
                    self.expect(",")
            self.expect(")")
            if len(args) != STMT_METHODS[name]:
                self.unsupported(m, "`.%s()` with %d arguments" % (name, len(args)))
            return _MStmt(name, recv, tuple(args), m.loc)
        # FR-257: any other name is a user member function, resolved by the
        # receiver's struct type (the checker reports `.capacity()` on a Vec)
        return K.MethodCall(recv, name, tuple(self.call_args()), m.loc)

    # ---------------------------------------------------- FR-253
    ITER_BAD = ("iterator method `.%s()` (the subset's iterator chains are `v.iter().fold(init, |a, &e| ..)`, "
                "`v.iter().max().unwrap()` / `.min()`, and `v.iter().copied()` followed by `.fold(init, &f)`, "
                "`.any(&p)`, `.all(&p)`, `.for_each(&f)` or, inside `w.extend(..)`, `.map(&f)`)")

    def iter_method(self, it, m):
        """`.name(..)` on `v.iter()` / `v.iter().copied()`: the algorithm rows."""
        name = m.text
        L = getattr(it.recv, "loc", None) or m.loc
        if it.mode == "ref" and name in ("max", "min"):
            self.expect("(")
            self.expect(")")
            if not (self.at(".") and self.peek().text == "unwrap" and self.peek(2).text == "("):
                self.unsupported(m, "`.%s()` other than `*v.iter().%s().unwrap()`" % (name, name))
            self.next()
            self.next()
            self.expect("(")
            self.expect(")")
            return _Unwrap(name + "_element", it.recv, L)
        if it.mode == "ref" and name == "fold":
            self.expect("(")
            init = self.free_expr()
            self.expect(",")
            spelling = self.fold_closure()
            self.expect(")")
            return K.Alg("accumulate", it.recv, (init,), L, spelling)
        if it.mode == "copied" and name in ("fold", "any", "all", "for_each", "map"):
            args = self.call_args()
            want = 2 if name == "fold" else 1
            if len(args) != want:
                self.unsupported(m, "`.%s()` with %d arguments" % (name, len(args)))
            if name == "fold":
                return K.Alg("accumulate", it.recv, (args[0], args[1]), L)
            if name in ("any", "all"):
                return K.Alg(name + "_of", it.recv, (args[0],), L)
            if name == "for_each":
                return _MStmt("for_each", it.recv, (args[0],), L)
            return _IterMap(it.recv, args[0], L)
        self.unsupported(m, self.ITER_BAD % name)

    def fold_closure(self):
        """The canonical fold closure of std::accumulate: `|a, &e| a + e`,
        `|a, &e| a.wrapping_add(e)`, either with `e as T`.  Returns
        (wrapping, cast type name or '') for the checker."""
        t = self.tok
        bad = ("fold closure other than `|a, &e| a + e` (the canonical std::accumulate step: `a.wrapping_add(e)` "
               "for an unsigned accumulator, `e as T` when the element type differs; any other operation is a "
               "closure variable `.copied().fold(init, &f)`)")
        for want in ("|", "a", ",", "&", "e", "|", "a"):
            if self.tok.text != want:
                self.unsupported(t, bad)
            self.next()
        wrapping = False
        if self.at("."):
            self.next()
            if self.tok.text != "wrapping_add":
                self.unsupported(t, bad)
            self.next()
            self.expect("(")
            wrapping = True
        elif self.at("+"):
            self.next()
        else:
            self.unsupported(t, bad)
        if self.tok.text != "e":
            self.unsupported(t, bad)
        self.next()
        cast = ""
        if self.at("as"):
            self.next()
            ct = self.tok
            if ct.text not in RUST_TYPES:
                self.unsupported(t, bad)
            self.next()
            cast = ct.text
        if wrapping:
            self.expect(")")
        if not self.at(")"):
            self.unsupported(t, bad)
        return (wrapping, cast)

    def call_args(self):
        self.expect("(")
        args = []
        while not self.at(")"):
            args.append(self.free_expr())
            if not self.at(")"):
                self.expect(",")
        self.expect(")")
        return args

    def path_expr(self, t):
        """`Vec::new()`, `String::new()`, `String::from("..")`, `bx::..`."""
        self.next()  # ::
        if t.text == "Vec" and self.at("<"):
            self.next()
            el = self.type_()
            self.close_angle()
            self.expect("::")
            m = self.ident()
            if m.text != "new":
                self.unsupported(m, "path expression `Vec::<..>::%s`" % m.text)
            self.expect("(")
            self.expect(")")
            return K.VecLit(el, (), t.loc)
        m = self.ident()
        path = "%s::%s" % (t.text, m.text)
        if path in ("Vec::new", "String::new"):
            self.expect("(")
            self.expect(")")
            return K.VecLit(None, (), t.loc) if t.text == "Vec" else K.StrNew(t.loc)
        if path == "String::from":
            self.expect("(")
            st = self.tok
            if st.kind != "str":
                self.unsupported(st, "`String::from` of something other than a string literal")
            self.next()
            self.expect(")")
            return K.StrFrom(st.value, t.loc)
        if path in ("bx::count", "bx::count_if", "bx::transform", "bx::sort_by_less"):
            # FR-253
            self.bx_uses.append((m.text, t))
            args = self.call_args()
            if len(args) != 2:
                self.unsupported(m, "`%s` with %d arguments" % (path, len(args)))
            if m.text in ("count", "count_if"):
                return K.Alg(m.text, args[0], (args[1],), t.loc)
            return _MStmt("sort_by" if m.text == "sort_by_less" else "transform", args[0], (args[1],), t.loc)
        if path in ("bx::substr", "bx::substr_from", "bx::push_byte"):
            self.bx_uses.append((m.text, t))
            args = self.call_args()
            want = {"bx::substr": 3, "bx::substr_from": 2, "bx::push_byte": 2}[path]
            if len(args) != want:
                self.unsupported(m, "`%s` with %d arguments" % (path, len(args)))
            if path == "bx::push_byte":
                return _MStmt("push_byte", args[0], (args[1],), t.loc)
            return K.Substr(args[0], args[1], args[2] if len(args) == 3 else None, t.loc)
        self.unsupported(t, "path expression `%s`" % path)

    def slice_from(self, fn, t):
        """FR-261: `std::slice::from_ref(&x)` / `std::slice::from_mut(&mut x)`
        (or a bare reference variable `x`) is `&x` -- a one-element slice."""
        args = self.call_args()
        if len(args) != 1:
            self.unsupported(t, "`std::slice::%s` with %d arguments" % (fn, len(args)))
        a = args[0]
        if isinstance(a, K.Borrow):
            return K.AddrOf(a.operand, t.loc, "%s(%s)" % (fn, "&mut" if a.mutable else "&"))
        return K.AddrOf(a, t.loc, fn + "()")

    def struct_lit(self, raw, t):
        """`S { f: e, g }` (FR-257); the path and the field order are
        checked once every struct is known."""
        self.expect("{")
        names, values = [], []
        while not self.at("}"):
            if self.at(".."):
                self.unsupported(self.tok, "struct update syntax `..base`")
            n = self.ident()
            if self.at(":"):
                self.next()
                values.append(self.free_expr())
            else:
                values.append(K.Var(n.text, n.loc))  # field-init shorthand
            names.append(n.text)
            if not self.at("}"):
                self.expect(",")
        self.expect("}")
        return K.StructLit(_RawPath(raw, self.ns, t.loc), tuple(values), t.loc, tuple(names))

    # ---------------------------------------------------- FR-259
    def if_expr(self):
        """`if c { a } else { b }` (`else if ..` chains) as an expression."""
        t = self.expect("if")
        if self.at("let"):
            self.unsupported(self.tok, "`if let`")
        cond = self.cond_expr()
        then = self.if_arm()
        if not self.at("else"):
            self.unsupported(t, "`if` expression without `else`")
        self.next()
        els = self.if_expr() if self.at("if") else self.if_arm()
        return K.Cond(cond, then, els, t.loc)

    def if_arm(self):
        self.expect("{")
        e = self.free_expr()
        if not self.at("}"):
            self.unsupported(self.tok, "`if` expression arm other than one expression")
        self.next()
        return e

    def incdec_block(self):
        """A block expression: only the canonical increment of FR-259."""
        t = self.expect("{")
        bad = ("block expression other than the canonical increment `{ x += 1; x }` / `{ x += 1; x - 1 }` "
               "(`{ x = x.wrapping_add(1u32); x }` when unsigned)")
        saved, self.no_struct = self.no_struct, False
        try:
            lhs = self.unary()
            op = self.tok
            if not (op.kind == "punct" and op.text in ("=", "+=", "-=")):
                self.unsupported(t, bad)
            self.next()
            val = self.expr()
            if not self.at(";"):
                self.unsupported(t, bad)
            self.next()
            tail = self.expr()
            if not self.at("}"):
                self.unsupported(t, bad)
            self.next()
        finally:
            self.no_struct = saved
        shape = _incdec_shape(lhs, op.text, val, tail)
        if shape is None:
            self.unsupported(t, bad)
        var, kop, post, wrapping, lit_ty = shape
        return K.IncDec(K.Var(var.name, var.loc, var.spelling), kop, post, t.loc, (wrapping, lit_ty))

    def vec_macro(self, t):
        self.next()  # !
        self.expect("[")
        if self.at("]"):
            self.unsupported(t, "`vec![]` (the subset spells the empty vector `Vec::new()`)")
        first = self.free_expr()
        if self.at(";"):
            self.next()
            n = self.free_expr()
            self.expect("]")
            return K.VecFill(first, n, t.loc)
        elems = [first]
        while self.at(","):
            self.next()
            if self.at("]"):
                break
            elems.append(self.free_expr())
        self.expect("]")
        return K.VecLit(None, tuple(elems), t.loc)

    def primary(self):
        self.take_crate_bx()
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
            if t.text == "if":
                return self.if_expr()
            if t.text in ("match", "loop", "while", "for", "unsafe", "move", "return",
                          "break", "continue", "let"):
                self.unsupported(t, "`%s` expression" % t.text)
            self.next()
            if self.at("::"):
                if t.text in ("Vec", "String", "bx"):
                    return self.path_expr(t)
                segs = [t.text]
                while self.at("::"):
                    self.next()
                    segs.append(self.ident().text)
                if segs[:2] == ["std", "slice"] and len(segs) == 3 and segs[2] in ("from_ref", "from_mut"):
                    return self.slice_from(segs[2], t)
                if segs[:2] == ["std", "cmp"] and len(segs) == 3 and segs[2] in ("max", "min") and self.at("("):
                    args = self.call_args()  # FR-253
                    if len(args) != 2:
                        self.unsupported(t, "`std::cmp::%s` with %d arguments" % (segs[2], len(args)))
                    return K.MinMax(segs[2], args[0], args[1], t.loc)
                if self.at("{") and not self.no_struct:
                    return self.struct_lit("::".join(segs), t)
                if not self.at("("):
                    return _RawConst("::".join(segs), t.loc)  # FR-264: resolved after the crate is read
                return K.Call("::".join(segs), tuple(self.call_args()), t.loc)
            if self.at("!"):
                if t.text == "vec" and self.peek().text == "[":
                    return self.vec_macro(t)
                self.unsupported(t, "macro `%s!` in an expression" % t.text)
            if self.at("("):
                self.next()
                args = []
                while not self.at(")"):
                    args.append(self.free_expr())
                    if not self.at(")"):
                        self.expect(",")
                self.expect(")")
                if self.is_closure(t.text):
                    return K.ClosureCall(t.text, tuple(args), t.loc)
                return K.Call(t.text, tuple(args), t.loc)
            if self.at("{") and not self.no_struct and t.text != "self":
                return self.struct_lit(t.text, t)
            return K.Var(t.text, t.loc)
        if t.kind == "punct" and t.text == "(":
            self.next()
            if self.at(")"):
                self.unsupported(t, "unit value `()`")
            e = self.free_expr()
            if self.at(","):
                self.unsupported(self.tok, "tuple")
            self.expect(")")
            return e
        if t.kind == "punct" and t.text == "{":
            return self.incdec_block()
        if t.kind == "punct" and t.text == "|":
            self.unsupported(t, "closure")
        if t.kind == "punct" and t.text == "[":
            self.unsupported(t, "array expression")
        raise ParseError(t.loc, "expected an expression, found %s" % self.describe(t))


def _incdec_shape(stmt_lhs, stmt_op, stmt_val, tail):
    """(var, op, post, wrapping, literal type) of a canonical increment block,
    or None.  ``stmt_*``: the block's one statement `lhs op= val;` / `lhs = val;`."""
    def one(x):
        return isinstance(x, K.IntLit) and x.value == 1

    def same(v):
        return isinstance(v, K.Var) and v.name == stmt_lhs.name and v.spelling == stmt_lhs.spelling

    if not isinstance(stmt_lhs, K.Var):
        return None
    if stmt_op in ("+=", "-="):
        if not one(stmt_val):
            return None
        op, wrapping, lit = ("add" if stmt_op == "+=" else "sub"), False, stmt_val
    elif stmt_op == "=":
        v = stmt_val
        if not (isinstance(v, K.Binary) and v.spelling == "wrapping" and v.op in K.INCDEC_OPS and same(v.lhs)
                and one(v.rhs)):
            return None
        op, wrapping, lit = v.op, True, v.rhs
    else:
        return None
    inv = "sub" if op == "add" else "add"
    if same(tail):
        post = False
    elif (isinstance(tail, K.Binary) and tail.op == inv and same(tail.lhs) and one(tail.rhs)
          and tail.rhs.ty == lit.ty and (tail.spelling == "wrapping") == wrapping):
        post = True
    else:
        return None
    return stmt_lhs, op, post, wrapping, lit.ty


def _tc():
    # typecheck imports this module's siblings; import it lazily, like
    # rs_parse_all does
    from . import typecheck
    return typecheck


PATH_KEYWORDS = ("crate", "self", "super")


def _resolve(raw, ns, funcs, loc, what="function"):
    """Rust's resolution of a call path written in module ``ns``, as an
    absolute Kernel path.  ``funcs``: every absolute function path (FR-257:
    or struct path, with ``what='struct'``)."""
    segs = raw.split("::")
    lead = 0
    while lead < len(segs) and segs[lead] == "super":
        lead += 1
    if any(x in PATH_KEYWORDS for x in segs[max(lead, 1):]):
        raise ParseError(loc, "unsupported: `crate`/`self`/`super` in the middle of the path `%s`" % raw)
    head = segs[0]
    if head == "crate":
        absolute = segs[1:]
    elif head == "self":
        absolute = list(ns) + segs[1:]
    elif head == "super":
        k = 0
        while k < len(segs) and segs[k] == "super":
            k += 1
        if k > len(ns):
            raise ParseError(loc, "unsupported: `super` above the crate root")
        absolute = list(ns[:len(ns) - k]) + segs[k:]
    else:
        absolute = list(ns) + segs
    if not absolute:
        raise ParseError(loc, "unsupported: path `%s` names no %s" % (raw, what))
    path = "::".join(absolute)
    if len(segs) == 1 and path not in funcs and ns:
        # C++ would search the enclosing namespaces; Rust does not.
        for i in range(len(ns) - 1, -1, -1):
            outer = K.path_of(ns[:i], raw)
            if outer in funcs:
                raise ParseError(loc, "unsupported: `%s` is not defined in module `%s`: a Rust path does not "
                                      "search enclosing modules (C++ unqualified lookup would find `%s`); spell "
                                      "it `crate::%s`" % (raw, "::".join(ns), outer, outer))
    return path


def _resolve_struct(rp, structs):
    """A _RawPath -> the absolute struct path (Rust's rules, like calls)."""
    path = _resolve(rp.raw, rp.ns, structs, rp.loc, "struct")
    if path not in structs:
        raise ParseError(rp.loc, "unsupported: type `%s`" % rp.raw)
    return path


def _resolve_ty(t, structs):
    if isinstance(t, K.StructT) and isinstance(t.path, _RawPath):
        return K.StructT(_resolve_struct(t.path, structs))
    if isinstance(t, K.VecT):
        e = _resolve_ty(t.elem, structs)
        return t if e is t.elem else K.VecT(e)
    if isinstance(t, K.RefT):
        e = _resolve_ty(t.inner, structs)
        return t if e is t.inner else K.RefT(e, t.mutable)
    if isinstance(t, K.PtrT):
        e = _resolve_ty(t.inner, structs)
        return t if e is t.inner else K.PtrT(e, t.mutable)
    return t


def _resolve_calls(node, ns, funcs, structs=frozenset(), consts=frozenset()):
    """Rebuild ``node`` with every Call's raw path resolved (FR-257: and
    every struct type / struct literal path; FR-264: every constant path,
    and every bare name of a constant of the module -- Rust can never
    bind a local of that name, so the name always means the constant)."""
    from dataclasses import fields as dfields, is_dataclass, replace
    if isinstance(node, _RawConst):
        path = _resolve(node.raw, ns, consts, node.loc, "constant")
        if path not in consts:
            raise ParseError(node.loc, "unsupported: path expression `%s`" % node.raw)
        return K.ConstRef(path, node.loc)
    if isinstance(node, K.Var) and K.path_of(ns, node.name) in consts:
        return K.ConstRef(K.path_of(ns, node.name), node.loc)
    if isinstance(node, tuple):
        out = tuple(_resolve_calls(x, ns, funcs, structs, consts) for x in node)
        return node if all(a is b for a, b in zip(out, node)) else out
    if isinstance(node, K.TYPE_CLASSES):
        return _resolve_ty(node, structs)
    if not is_dataclass(node) or isinstance(node, K.Loc):
        return node
    changes = {}
    for f in dfields(node):
        if f.name in ("loc", "spelling", "names"):
            continue
        v = getattr(node, f.name)
        nv = _resolve_calls(v, ns, funcs, structs, consts)
        if nv is not v:
            changes[f.name] = nv
    if isinstance(node, K.Call):
        changes["name"] = _resolve(node.name, ns, funcs, node.loc)
    if isinstance(node, K.StructLit) and isinstance(node.path, _RawPath):
        changes["path"] = _resolve_struct(node.path, structs)
    return replace(node, **changes) if changes else node


def rs_parse_surface(src):
    """Parse Rust source to an UN-normalized Kernel (raises ParseError)."""
    try:
        toks = lex(src)
    except LexError as ex:
        raise ParseError(ex.loc, ex.msg)
    parser = _Parser(toks)
    prog = parser.program()
    prog = _attach_impls(prog, parser.impls)
    funcs = {f.path for f in prog.all_funcs()}
    structs = {sd.path for sd in prog.structs()}
    consts = {c.path for c in prog.consts()}
    items = []
    for it in prog.items:
        if isinstance(it, K.StructDef):
            items.append(K.StructDef(it.name, _resolve_calls(it.fields, it.ns, funcs, structs),
                                     tuple(_resolve_func(m, funcs, structs, consts) for m in it.methods), it.loc,
                                     it.ns, it.internal))
        elif isinstance(it, K.ConstItem):
            items.append(K.ConstItem(it.name, it.ty, _resolve_calls(it.value, it.ns, funcs, structs, consts),
                                     it.loc, it.ns, it.internal))
        else:
            items.append(_resolve_func(it, funcs, structs, consts))
    prog = K.Program(tuple(items))
    _check_literal_fields(prog)
    for n in K.walk(prog):
        if isinstance(n, _MStmt):
            raise ParseError(n.loc, "unsupported: `.%s()` used as a value" % n.kind)
        if isinstance(n, _Iter):
            raise ParseError(n.loc, "unsupported: iterator outside a `for` loop")
        if isinstance(n, _IterMap):
            raise ParseError(n.loc, "unsupported: `.map(&f)` outside `w.extend(v.iter().copied().map(&f))`")
        if isinstance(n, _Unwrap):
            raise ParseError(n.loc, "unsupported: `v.iter().%s().unwrap()` other than dereferenced at once "
                                    "(`*v.iter().%s().unwrap()`)" % (n.op[:3], n.op[:3]))
        if isinstance(n, _SliceIdx):
            raise ParseError(n.loc, "unsupported: unsized slice place `x[k..]` (the subset spells a re-slice "
                                    "`&x[k..]` / `&mut x[k..]`)")
    return prog


def _resolve_func(f, funcs, structs, consts=frozenset()):
    return K.Func(f.name, _resolve_calls(f.params, f.ns, funcs, structs), _resolve_ty(f.ret, structs),
                  _resolve_calls(f.body, f.ns, funcs, structs, consts), f.loc, f.ns, f.internal, f.owner, f.recv)


def _attach_impls(prog, impls):
    """Each `impl S { .. }` joins the struct S of its module (one impl per
    struct; a constructor is `pub fn new(..) -> S { return S { .. }; }`)."""
    by = {(sd.ns, sd.name): sd for sd in prog.structs()}
    methods = {}
    for name, ns, ms, loc, nloc in impls:
        if (ns, name) not in by:
            raise ParseError(nloc, "unsupported: `impl %s` without a struct `%s` in the same module" % (name, name))
        if (ns, name) in methods:
            raise ParseError(loc, "unsupported: second `impl %s` block (the subset spells one per struct, "
                                  "like the one C++ class definition)" % name)
        out = []
        for m in ms:
            recv = m.recv
            if m.name == "new" and recv == "static":
                recv = "ctor"
            out.append(K.Func(m.name, m.params, m.ret, m.body, m.loc, ns, False, name, recv))
        methods[(ns, name)] = tuple(out)
    items = []
    for it in prog.items:
        if isinstance(it, K.StructDef):
            it = K.StructDef(it.name, it.fields, methods.get((it.ns, it.name), ()), it.loc, it.ns, it.internal)
        items.append(it)
    return K.Program(tuple(items))


def _check_literal_fields(prog):
    """Rust evaluates a struct literal's fields in WRITTEN order, C++ an
    aggregate's initializers in DECLARATION order: the subset requires
    every field, in declaration order (FR-257)."""
    decl = {sd.path: tuple(f.name for f in sd.fields) for sd in prog.structs()}
    for n in K.walk(prog):
        if isinstance(n, K.StructLit) and n.path in decl and n.names != decl[n.path]:
            raise ParseError(n.loc, "unsupported: the literal of struct '%s' does not list every field in "
                                    "declaration order (Rust evaluates field initializers in written order, "
                                    "C++ in declaration order)" % n.path)


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
    surf = TC.normalize(surf, keep_surface=True)  # N11 only: empty-vector element types
    errs = TC.check(surf, surface="rust")
    if errs:
        return None, errs
    prog = TC.normalize(surf)
    errs = TC.check(prog)
    return (None, errs) if errs else (prog, [])
