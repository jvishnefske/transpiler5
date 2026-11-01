"""Kernel typing, well-formedness and normalization (FR-250).

Three entry points:

* ``type_of(expr, scope, sigs)`` -- the type of one expression (raises
  ``KernelTypeError``).  Printers call it while walking, so they never rely
  on a node-identity side table.
* ``check(program, surface=None)`` -- every well-formedness violation,
  COLLECTED (not first-failure), as ``(loc, message)`` pairs.  With
  ``surface='rust'`` it also enforces the Rust-surface spelling rules (an
  unsigned `+` must be spelled `.wrapping_add`, a signed one must not).
* ``normalize(program)`` -- rewrites the handful of constructs that have two
  Kernel spellings into the single normal one.  The bijection laws are
  stated over NORMAL Kernel terms; both parsers normalize their output.

Normal-form rules (each is meaning-preserving):

  N1  Cast(T, e) with type(e) == T            -> e
  N2  Cast(T int, IntLit(v, S)), v fits T     -> IntLit(v, T)
  N3  Cast(f64, IntLit(v, S)), |v| <= 2**53   -> FloatLit(v)
  N4  Cast(T in {i32,i64}, Neg(IntLit(v,S))), v fits T -> Neg(IntLit(v, T))
      Cast(f64, Neg(IntLit(v, S))), |v| <= 2**53     -> Neg(FloatLit(v))
  N5  Assign(x, Binary(op, Var(x), e)), op compoundable -> CompoundAssign(op, x, e)
  N6  adjacent printf Text segments are merged; empty Text is dropped
"""

from . import kernel as K


class KernelTypeError(Exception):
    def __init__(self, loc, msg):
        Exception.__init__(self, msg)
        self.loc = loc
        self.msg = msg


# Identifiers that cannot be used on one side or the other.
RUST_KEYWORDS = {
    "as", "break", "const", "continue", "crate", "else", "enum", "extern",
    "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
    "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct",
    "super", "trait", "true", "type", "unsafe", "use", "where", "while",
    "async", "await", "dyn", "abstract", "become", "box", "do", "final",
    "macro", "override", "priv", "typeof", "unsized", "virtual", "yield",
    "try", "union", "gen",
    # prelude names that are patterns in `let` position
    "Some", "None", "Ok", "Err",
}
CXX_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char16_t", "char32_t",
    "char8_t", "class", "compl", "concept", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await",
    "co_return", "co_yield", "decltype", "default", "delete", "do",
    "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline",
    "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short",
    "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    "final", "override", "import", "module",
}
# Names the canonical outputs themselves use or that <cstdio> declares.
RESERVED = {
    "std", "bx", "printf", "fprintf", "sprintf", "snprintf", "puts",
    "putchar", "getchar", "scanf", "perror", "fflush", "fopen", "fclose",
    "remove", "rename", "stdout", "stdin", "stderr", "EOF", "NULL", "FILE",
    "BUFSIZ", "size_t", "print", "println", "format", "main",
    "i8", "u8", "i32", "i64", "u32", "u64", "usize", "f64", "str",
    # the rest of <cstdio>'s global names and object-like macros: a C++
    # local or function by one of these names either fails to compile
    # (macro expansion, same-signature redefinition) or overloads libc
    "getc", "putc", "fgetc", "fputc", "fgets", "fputs", "gets", "ungetc",
    "fread", "fwrite", "fseek", "ftell", "rewind", "feof", "ferror",
    "clearerr", "setbuf", "setvbuf", "tmpfile", "tmpnam", "freopen",
    "vprintf", "vfprintf", "vsprintf", "vsnprintf", "sscanf", "fscanf",
    "vscanf", "dprintf", "getline", "getdelim", "popen", "pclose", "fileno",
    "fgetpos", "fsetpos", "fpos_t", "va_list", "L_tmpnam", "FILENAME_MAX",
    "FOPEN_MAX", "TMP_MAX", "SEEK_SET", "SEEK_CUR", "SEEK_END", "P_tmpdir",
    "String",
}


def ident_problem(name):
    """Why ``name`` cannot be a Kernel identifier, or None."""
    if not name or not (name[0].isalpha() or name[0] == "_"):
        return "identifier '%s' is not a plain identifier" % name
    if not all(c.isascii() and (c.isalnum() or c == "_") for c in name):
        return "identifier '%s' is not ASCII" % name
    if name.startswith("_"):
        return "identifier '%s' starts with an underscore (reserved/unused-marker)" % name
    if "__" in name:
        return "identifier '%s' contains a double underscore (reserved in C++)" % name
    if name in RUST_KEYWORDS:
        return "identifier '%s' is a Rust keyword" % name
    if name in CXX_KEYWORDS:
        return "identifier '%s' is a C++ keyword" % name
    if name in RESERVED:
        return "identifier '%s' is reserved by the canonical spelling" % name
    return None


class Scope:
    def __init__(self, parent=None):
        self.parent = parent
        self.vars = {}

    def child(self):
        return Scope(self)

    def lookup(self, name):
        s = self
        while s is not None:
            if name in s.vars:
                return s.vars[name]
            s = s.parent
        return None

    def declare(self, name, ty, mutable):
        self.vars[name] = (ty, mutable)


def sigs_of(program):
    sigs = {}
    for f in program.funcs():
        if f.name not in sigs:
            sigs[f.name] = (tuple(p.ty for p in f.params), f.ret)
    return sigs


LITERAL_TYPES = K.INT_TYPES


def type_of(e, scope, sigs):
    """The Kernel type of ``e``; raises KernelTypeError when ill-typed."""
    if isinstance(e, K.IntLit):
        if e.ty not in LITERAL_TYPES:
            raise KernelTypeError(e.loc, "integer literal of non-integer type %s" % e.ty)
        lo, hi = K.INT_RANGE[e.ty.name]
        if not (0 <= e.value <= hi):
            raise KernelTypeError(e.loc, "integer literal %d out of range for %s" % (e.value, e.ty))
        return e.ty
    if isinstance(e, K.FloatLit):
        try:
            v = float(e.text)
        except ValueError:
            raise KernelTypeError(e.loc, "malformed float literal %r" % e.text)
        if v != v or v in (float("inf"),) or e.text.startswith("-") or repr(v) != e.text:
            raise KernelTypeError(e.loc, "float literal %r is not a canonical finite non-negative value" % e.text)
        return K.F64
    if isinstance(e, K.BoolLit):
        return K.BOOL
    if isinstance(e, K.StrLit):
        return K.STR
    if isinstance(e, K.Var):
        v = scope.lookup(e.name)
        if v is None:
            raise KernelTypeError(e.loc, "use of undeclared variable '%s'" % e.name)
        return v[0]
    if isinstance(e, K.Unary):
        t = type_of(e.operand, scope, sigs)
        if e.op == "neg":
            if t not in K.ARITH:
                raise KernelTypeError(e.loc, "negation of %s" % t)
            return t
        if e.op == "not":
            if t == K.BOOL or t in K.ARITH_INT:
                return t
            raise KernelTypeError(e.loc, "'not' of %s" % t)
        raise KernelTypeError(e.loc, "unknown unary op %r" % e.op)
    if isinstance(e, K.Binary):
        a = type_of(e.lhs, scope, sigs)
        b = type_of(e.rhs, scope, sigs)
        op = e.op
        if op in K.SHIFT_OPS:
            if a not in K.ARITH_INT or b not in K.ARITH_INT:
                raise KernelTypeError(e.loc, "shift of %s by %s" % (a, b))
            return a
        if op in K.LOGIC_OPS:
            if a != K.BOOL or b != K.BOOL:
                raise KernelTypeError(e.loc, "logical %s of %s and %s" % (op, a, b))
            return K.BOOL
        if a != b:
            raise KernelTypeError(e.loc, "operands of %s have different types %s and %s" % (op, a, b))
        if op in K.CMP_OPS:
            if a not in K.ARITH:
                raise KernelTypeError(e.loc, "comparison of %s" % a)
            return K.BOOL
        if op in ("add", "sub", "mul", "div"):
            if a not in K.ARITH:
                raise KernelTypeError(e.loc, "arithmetic %s on %s" % (op, a))
            return a
        if op == "rem" or op in K.BIT_OPS:
            if a not in K.ARITH_INT:
                raise KernelTypeError(e.loc, "integer op %s on %s" % (op, a))
            return a
        raise KernelTypeError(e.loc, "unknown binary op %r" % op)
    if isinstance(e, K.Cast):
        t = type_of(e.operand, scope, sigs)
        if e.ty not in K.ALL_PRIMS or e.ty == K.BOOL:
            raise KernelTypeError(e.loc, "cast to %s" % e.ty)
        if t not in K.ALL_PRIMS:
            raise KernelTypeError(e.loc, "cast from %s" % t)
        if t == K.BOOL and not K.is_int(e.ty):
            raise KernelTypeError(e.loc, "cast from bool to %s" % e.ty)
        return e.ty
    if isinstance(e, K.Call):
        sig = sigs.get(e.name)
        if sig is None:
            raise KernelTypeError(e.loc, "call to undeclared function '%s'" % e.name)
        if e.name == "main":
            raise KernelTypeError(e.loc, "call to main")
        ptys, ret = sig
        if len(ptys) != len(e.args):
            raise KernelTypeError(e.loc, "'%s' takes %d arguments, %d given" % (e.name, len(ptys), len(e.args)))
        for i, (pt, a) in enumerate(zip(ptys, e.args)):
            at = type_of(a, scope, sigs)
            if at != pt:
                raise KernelTypeError(getattr(a, "loc", None) or e.loc,
                                      "argument %d of '%s' has type %s, expected %s" % (i + 1, e.name, at, pt))
        return ret
    if isinstance(e, K.ResidualExpr):
        raise KernelTypeError(e.loc, "residual: " + e.reason)
    raise KernelTypeError(None, "not an expression: %r" % (e,))


def value_type(e, scope, sigs):
    """type_of, additionally rejecting unit-typed calls and bare strings."""
    t = type_of(e, scope, sigs)
    if t == K.UNIT:
        raise KernelTypeError(e.loc, "call to void function '%s' used as a value" % getattr(e, "name", "?"))
    if t == K.STR:
        raise KernelTypeError(e.loc, "string literal outside a printf %s argument")
    _check_value_subexprs(e, scope, sigs)
    return t


def _check_value_subexprs(e, scope, sigs):
    for n in K.walk(e):
        if n is e:
            continue
        if isinstance(n, K.StrLit):
            raise KernelTypeError(n.loc, "string literal outside a printf %s argument")
        if isinstance(n, K.Call) and sigs.get(n.name, (None, None))[1] == K.UNIT:
            raise KernelTypeError(n.loc, "call to void function '%s' used as a value" % n.name)


def definitely_returns(s):
    if isinstance(s, K.Return):
        return True
    if isinstance(s, K.Block):
        return any(definitely_returns(x) for x in s.stmts)
    if isinstance(s, K.If):
        return s.els is not None and definitely_returns(s.then) and definitely_returns(s.els)
    return False


def mentions(e, name):
    return any(isinstance(n, K.Var) and n.name == name for n in K.walk(e))


def assigned_names(block):
    out = set()
    for n in K.walk(block):
        if isinstance(n, (K.Assign, K.CompoundAssign)):
            out.add(n.name)
    return out


def declared_names(block):
    out = set()
    for n in K.walk(block):
        if isinstance(n, K.Let):
            out.add(n.name)
        elif isinstance(n, K.For):
            out.add(n.var)
    return out


class _Checker:
    def __init__(self, program, surface):
        self.program = program
        self.surface = surface
        self.sigs = sigs_of(program)
        self.errors = []

    def err(self, loc, msg):
        self.errors.append((loc, msg))

    def ident(self, name, loc):
        p = ident_problem(name)
        if p:
            self.err(loc, p)
        elif name in self.sigs:
            self.err(loc, "variable '%s' has the name of a function" % name)

    def expr(self, e, scope, want=None):
        try:
            t = value_type(e, scope, self.sigs)
        except KernelTypeError as ex:
            self.err(ex.loc or getattr(e, "loc", None), ex.msg)
            return None
        if self.surface == "rust":
            self.rust_spelling(e, scope)
        if want is not None and t != want:
            self.err(getattr(e, "loc", None), "expression has type %s, expected %s" % (t, want))
        return t

    def rust_spelling(self, e, scope):
        for n in K.walk(e):
            if isinstance(n, K.Binary) and n.op in K.WRAPPING_OPS:
                t = type_of(n.lhs, scope, self.sigs)
                self._spelled(n, t, n.op)
            elif isinstance(n, K.Unary) and n.op == "neg":
                t = type_of(n.operand, scope, self.sigs)
                self._spelled(n, t, "neg")

    def _spelled(self, n, t, op):
        sym = {"add": "+", "sub": "-", "mul": "*", "neg": "-"}[op]
        if K.is_unsigned(t) and n.spelling == "plain":
            self.err(n.loc, "unsupported: unsigned `%s` (panics on overflow in debug builds); "
                            "the subset spells it `.wrapping_%s(..)`" % (sym, op))
        elif not K.is_unsigned(t) and n.spelling == "wrapping":
            self.err(n.loc, "unsupported: `.wrapping_%s` on %s (signed overflow is undefined "
                            "behaviour in C++)" % (op, t))

    def run(self):
        seen = set()
        for it in self.program.items:
            if isinstance(it, K.ResidualItem):
                continue
            f = it
            if f.name in seen:
                self.err(f.loc, "function '%s' defined twice" % f.name)
            seen.add(f.name)
            if f.name != "main":
                p = ident_problem(f.name)
                if p:
                    self.err(f.loc, p)
            self.func(f)
        return self.errors

    def func(self, f):
        if f.ret not in K.ALL_PRIMS and f.ret != K.UNIT:
            self.err(f.loc, "return type %s" % f.ret)
        if f.name == "main":
            if f.params:
                self.err(f.loc, "main with parameters")
            if f.ret != K.UNIT:
                self.err(f.loc, "main must return unit")
            if any(isinstance(n, K.Return) for n in K.walk(f.body)):
                self.err(f.loc, "main may not contain return")
        scope = Scope()
        for p in f.params:
            self.ident(p.name, p.loc)
            if p.ty not in K.ALL_PRIMS:
                self.err(p.loc, "parameter type %s" % p.ty)
            if p.name in scope.vars:
                self.err(p.loc, "duplicate parameter '%s'" % p.name)
            scope.declare(p.name, p.ty, p.mutable)
        self.ret = f.ret
        self.block(f.body, scope, loop=False, same_scope=True)
        if f.ret != K.UNIT and not definitely_returns(f.body):
            self.err(f.loc, "function '%s' may reach its end without returning a value" % f.name)

    def block(self, b, scope, loop, same_scope=False):
        inner = scope if same_scope else scope.child()
        for s in b.stmts:
            self.stmt(s, inner, loop)

    def stmt(self, s, scope, loop):
        sigs = self.sigs
        if isinstance(s, K.Block):
            self.block(s, scope, loop)
        elif isinstance(s, K.Let):
            self.ident(s.name, s.loc)
            if s.name in scope.vars:
                self.err(s.loc, "redeclaration of '%s' in the same scope" % s.name)
            if mentions(s.init, s.name):
                # C++'s point of declaration is BEFORE the initializer, so
                # `int x = x + 1;` reads the new, uninitialized x; Rust's
                # `let x = x + 1;` reads the outer one.
                self.err(s.loc, "initializer of '%s' refers to a variable of the same name" % s.name)
            if s.ty not in K.ALL_PRIMS:
                self.err(s.loc, "variable type %s" % s.ty)
            self.expr(s.init, scope, s.ty)
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, (K.Assign, K.CompoundAssign)):
            v = scope.lookup(s.name)
            if v is None:
                self.err(s.loc, "assignment to undeclared '%s'" % s.name)
                return
            ty, mut = v
            if not mut:
                self.err(s.loc, "assignment to immutable '%s'" % s.name)
            if isinstance(s, K.Assign):
                self.expr(s.value, scope, ty)
                return
            op = s.op
            if op not in K.COMPOUND_OPS:
                self.err(s.loc, "bad compound op %r" % op)
                return
            if op in K.SHIFT_OPS:
                if ty not in K.ARITH_INT:
                    self.err(s.loc, "compound shift on %s" % ty)
                t = self.expr(s.value, scope)
                if t is not None and t not in K.ARITH_INT:
                    self.err(s.loc, "shift amount of type %s" % t)
            else:
                ok = K.ARITH if op in ("add", "sub", "mul", "div") else K.ARITH_INT
                if ty not in ok:
                    self.err(s.loc, "compound %s on %s" % (op, ty))
                self.expr(s.value, scope, ty)
            if self.surface == "rust" and op in K.WRAPPING_OPS and K.is_unsigned(ty) and s.spelling == "plain":
                sym = {"add": "+", "sub": "-", "mul": "*"}[op]
                self.err(s.loc, "unsupported: unsigned `%s=` (panics on overflow in debug builds); "
                                "the subset spells it `x = x.wrapping_%s(..)`" % (sym, op))
        elif isinstance(s, K.ExprStmt):
            if not isinstance(s.expr, K.Call):
                self.err(s.loc, "expression statement that is not a call")
                return
            try:
                type_of(s.expr, scope, sigs)
                for a in s.expr.args:
                    value_type(a, scope, sigs)
            except KernelTypeError as ex:
                self.err(ex.loc or s.loc, ex.msg)
            if self.surface == "rust":
                for a in s.expr.args:
                    self.rust_spelling(a, scope)
        elif isinstance(s, K.Print):
            self.print_(s, scope)
        elif isinstance(s, K.If):
            self.expr(s.cond, scope, K.BOOL)
            self.block(s.then, scope, loop)
            if s.els is not None:
                self.block(s.els, scope, loop)
        elif isinstance(s, K.While):
            self.expr(s.cond, scope, K.BOOL)
            self.block(s.body, scope, True)
        elif isinstance(s, K.For):
            self.ident(s.var, s.loc)
            t = self.expr(s.lo, scope)
            if t is not None and t not in (K.I32, K.USIZE):
                self.err(s.loc, "counting loop variable of type %s (only int and std::size_t)" % t)
            if mentions(s.lo, s.var) or mentions(s.hi, s.var):
                # in C++ both bounds would see the NEW loop variable
                self.err(s.loc, "counting loop bounds refer to a variable of the loop variable's name '%s'" % s.var)
            if not isinstance(s.hi, (K.IntLit, K.Var)):
                self.err(getattr(s.hi, "loc", None) or s.loc,
                         "counting loop bound must be a literal or a variable")
            self.expr(s.hi, scope, t)
            assigned = assigned_names(s.body)
            if s.var in assigned:
                self.err(s.loc, "counting loop body assigns the loop variable '%s'" % s.var)
            if isinstance(s.hi, K.Var):
                if s.hi.name in assigned or s.hi.name in declared_names(s.body):
                    self.err(s.loc, "counting loop body assigns or shadows the bound '%s'" % s.hi.name)
            inner = scope.child()
            # already reported above if the body assigns it: declaring it
            # mutable then avoids a second, cascading "immutable" error
            inner.declare(s.var, t, s.var in assigned)
            self.block(s.body, inner, True, same_scope=True)
        elif isinstance(s, (K.Break, K.Continue)):
            if not loop:
                self.err(s.loc, "%s outside a loop" % type(s).__name__.lower())
        elif isinstance(s, K.Return):
            if s.value is None:
                if self.ret != K.UNIT:
                    self.err(s.loc, "return without a value in a function returning %s" % self.ret)
            else:
                if self.ret == K.UNIT:
                    self.err(s.loc, "return with a value in a void function")
                else:
                    self.expr(s.value, scope, self.ret)
        elif isinstance(s, K.ResidualStmt):
            pass
        else:
            self.err(getattr(s, "loc", None), "not a statement: %r" % (s,))

    def print_(self, s, scope):
        prev_text = False
        for sg in s.segs:
            if isinstance(sg, K.Text):
                if prev_text or not sg.text:
                    self.err(s.loc, "printf text segments not normalized")
                bad = [c for c in sg.text if not (1 <= ord(c) <= 0x7F)]
                if bad:
                    self.err(s.loc, "printf text outside ASCII")
                prev_text = True
                continue
            prev_text = False
            if sg.kind not in K.SPEC_KINDS:
                self.err(s.loc, "bad printf spec kind %r" % sg.kind)
                continue
            if sg.kind != "d" and (sg.flag or sg.width is not None):
                self.err(s.loc, "printf flags/width only on integer conversions")
            if sg.flag not in ("", "-", "0"):
                self.err(s.loc, "printf flag %r" % sg.flag)
            if sg.flag and sg.width is None:
                self.err(s.loc, "printf flag without width")
            if sg.width is not None and not (1 <= sg.width <= 99):
                self.err(s.loc, "printf width %r" % sg.width)
            if (sg.kind == "f") != (sg.prec is not None):
                self.err(s.loc, "printf precision only (and always) on %f")
            if sg.prec is not None and not (0 <= sg.prec <= 30):
                self.err(s.loc, "printf precision %r" % sg.prec)
            if sg.kind == "s":
                if not isinstance(sg.arg, K.StrLit):
                    self.err(s.loc, "printf %s argument must be a string literal")
                elif any(not (1 <= ord(c) <= 0x7F) for c in sg.arg.value):
                    self.err(s.loc, "printf %s literal outside ASCII")
                continue
            t = self.expr(sg.arg, scope)
            if t is None:
                continue
            if sg.kind == "d" and t not in K.ARITH_INT:
                self.err(getattr(sg.arg, "loc", None) or s.loc,
                         "printf integer conversion of a %s argument" % t)
            if sg.kind == "f" and t != K.F64:
                self.err(getattr(sg.arg, "loc", None) or s.loc, "printf %%f of a %s argument" % t)
            if sg.kind == "c" and t != K.I32:
                self.err(getattr(sg.arg, "loc", None) or s.loc, "printf %%c of a %s argument" % t)


def check(program, surface=None):
    """All well-formedness errors of ``program`` as (loc, msg), in order."""
    return _Checker(program, surface).run()


# ---------------------------------------------------------------- normalize

_F64_EXACT = 1 << 53


class _Normalizer:
    def __init__(self, program):
        self.sigs = sigs_of(program)

    def expr(self, e, scope):
        if isinstance(e, K.Unary):
            return K.Unary(e.op, self.expr(e.operand, scope), e.loc, e.spelling)
        if isinstance(e, K.Binary):
            return K.Binary(e.op, self.expr(e.lhs, scope), self.expr(e.rhs, scope), e.loc, e.spelling)
        if isinstance(e, K.Call):
            return K.Call(e.name, tuple(self.expr(a, scope) for a in e.args), e.loc)
        if isinstance(e, K.Cast):
            inner = self.expr(e.operand, scope)
            T = e.ty
            try:
                t = type_of(inner, scope, self.sigs)
            except KernelTypeError:
                t = None
            if t == T:
                return inner  # N1
            if isinstance(inner, K.IntLit):
                if T in K.INT_TYPES and K.INT_RANGE[T.name][1] >= inner.value:
                    return K.IntLit(inner.value, T, e.loc)  # N2
                if T == K.F64 and inner.value <= _F64_EXACT:
                    return K.FloatLit(repr(float(inner.value)), e.loc)  # N3
            if isinstance(inner, K.Unary) and inner.op == "neg" and isinstance(inner.operand, K.IntLit):
                v = inner.operand.value
                if T in (K.I32, K.I64) and v <= K.INT_RANGE[T.name][1]:
                    return K.Unary("neg", K.IntLit(v, T, inner.operand.loc), e.loc)  # N4
                if T == K.F64 and v <= _F64_EXACT:
                    return K.Unary("neg", K.FloatLit(repr(float(v)), inner.operand.loc), e.loc)
            return K.Cast(T, inner, e.loc)
        return e

    def block(self, b, scope, same=False):
        inner = scope if same else scope.child()
        out = [self.stmt(s, inner) for s in b.stmts]
        return K.Block(tuple(out), b.loc)

    def stmt(self, s, scope):
        if isinstance(s, K.Block):
            return self.block(s, scope)
        if isinstance(s, K.Let):
            init = self.expr(s.init, scope)
            scope.declare(s.name, s.ty, s.mutable)
            return K.Let(s.name, s.ty, s.mutable, init, s.loc)
        if isinstance(s, K.Assign):
            v = self.expr(s.value, scope)
            if (isinstance(v, K.Binary) and v.op in K.COMPOUND_OPS and isinstance(v.lhs, K.Var)
                    and v.lhs.name == s.name):
                return K.CompoundAssign(v.op, s.name, v.rhs, s.loc, v.spelling)  # N5
            return K.Assign(s.name, v, s.loc)
        if isinstance(s, K.CompoundAssign):
            return K.CompoundAssign(s.op, s.name, self.expr(s.value, scope), s.loc, s.spelling)
        if isinstance(s, K.ExprStmt):
            return K.ExprStmt(self.expr(s.expr, scope), s.loc)
        if isinstance(s, K.Print):
            segs = []
            for sg in s.segs:
                if isinstance(sg, K.Text):
                    if not sg.text:
                        continue
                    if segs and isinstance(segs[-1], K.Text):
                        segs[-1] = K.Text(segs[-1].text + sg.text)  # N6
                    else:
                        segs.append(sg)
                else:
                    segs.append(K.Spec(sg.kind, self.expr(sg.arg, scope), sg.flag, sg.width, sg.prec))
            return K.Print(tuple(segs), s.loc)
        if isinstance(s, K.If):
            return K.If(self.expr(s.cond, scope), self.block(s.then, scope),
                        None if s.els is None else self.block(s.els, scope), s.loc)
        if isinstance(s, K.While):
            return K.While(self.expr(s.cond, scope), self.block(s.body, scope), s.loc)
        if isinstance(s, K.For):
            lo = self.expr(s.lo, scope)
            hi = self.expr(s.hi, scope)
            inner = scope.child()
            try:
                t = type_of(lo, scope, self.sigs)
            except KernelTypeError:
                t = None
            inner.declare(s.var, t, False)
            return K.For(s.var, lo, hi, self.block(s.body, inner, same=True), s.loc)
        if isinstance(s, K.Return):
            return K.Return(None if s.value is None else self.expr(s.value, scope), s.loc)
        return s

    def func(self, f):
        scope = Scope()
        for p in f.params:
            scope.declare(p.name, p.ty, p.mutable)
        return K.Func(f.name, f.params, f.ret, self.block(f.body, scope, same=True), f.loc)


def normalize(program):
    n = _Normalizer(program)
    return K.Program(tuple(n.func(i) if isinstance(i, K.Func) else i for i in program.items))
