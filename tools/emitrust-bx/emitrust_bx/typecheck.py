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

FR-251 (vectors, strings, references):

  N7  Borrow(e) -> e: `&x` / `&mut x` are Rust spellings the printer
      derives from types; the Kernel has no borrow node.  (Likewise `*x`
      is Var(x) with a compare=False spelling hint.)
  N8  Clone(p) in a COPIED position (vec literal element, vec-fill value,
      resize value) -> p: C++ copies from `const T&` there anyway; Rust
      spells the copy `.clone()`.
  N9  Return(Clone(Var x)), x an owned MUTABLE local/by-value param ->
      Return(Var x): C++17 `return x;` is an implicit move.
  N12 Return(Var x), x an owned IMMUTABLE local -> Return(Clone(Var x)):
      `return x;` of a const object copies (it cannot be moved).
  N10 Spec('d', e) with e a String (Rust `{}`) -> Spec('s', e) (%s).
  N11 VecLit(None, ..) (Rust `Vec::new()` / `vec![..]`) gets its element
      type from the context (let/assign/return/parameter/element type) or
      its first element.  This one also runs BEFORE the Rust surface
      check (normalize(keep_surface=True)), which keeps every spelling.
  N5' N5 on elements: Store(v[i], Binary(op, v[i], e)) -> CompoundStore
      when v[i] contains no call (else the index would be evaluated twice).
      N5 itself only fires on scalar variables (never `s = s + t`).

FR-260 (namespaces <=> modules).  Every Call names an ABSOLUTE path
('a::b::f'); lookup is resolved by the parsers, so item order carries no
meaning and the printers re-derive every spelling (same namespace: bare
`f`; otherwise `::a::b::f` / `crate::a::b::f`) and every C++ prototype.

  N13 Program items are in NAMESPACE-TREE order: the items of a namespace
      form one contiguous block (a reopened namespace is merged into its
      first appearance), and siblings -- functions and nested namespaces
      alike -- keep their first-appearance order.  Meaning-preserving
      because no Kernel reference depends on declaration order.
  N14 Func.internal is False at the root: a C++ anonymous namespace at
      global scope only changes LINKAGE, which a single-TU program cannot
      observe, and every crate-root Rust item is already private.
      (FR-257: likewise StructDef.internal.)

FR-257 (structs and member functions).  A StructDef holds its data members
and its member functions (Funcs with ``owner``/``recv``); a struct type is
its ABSOLUTE path, resolved by the parsers like a call.  Structs are not
Copy (FR-251's Clone/move rule applies unchanged); `self` is a reference
variable (`&self` / `&mut self`, C++ `this` / `const this`).  The checker
adds: receiver alias rules measured on rustc 1.98's two-phase borrows (see
``method_call``), a counting loop's `x.size()` bound protected over ACCESS
PATHS (``resizes``), struct/member naming rules, no by-value recursion, a
constructor that is exactly `return S { every field };`, aggregate
initialization only of a struct without a constructor, and internal
(anonymous-namespace) structs named only inside their namespace.

  N15 Struct definitions come first, in a canonical order that depends only
      on the SET of structs: topological over by-value containment (C++
      needs a member's type complete), smallest absolute path first among
      the ready ones; then N13 places them in their namespace blocks.
      Meaning-preserving because no Kernel reference depends on declaration
      order; it is what makes law 1 hold for Rust input (any order) and for
      C++ input (define-before-use order) alike.
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
# FR-260: names a namespace (a Rust module) may not take on top of RESERVED:
# a root `mod Vec` would shadow the prelude type every canonical
# `Vec<T>` / `Vec::new()` spells.
NS_RESERVED = {"Vec"}
# FR-257: names a struct may not take.  A struct named like a prelude item
# shadows it for the whole module: `Clone` would break `#[derive(Clone)]` /
# `.clone()`, `Iterator` `.iter().copied()`, `ToString` `.to_string()`.
STRUCT_RESERVED = {
    "Vec", "String", "Clone", "Copy", "Send", "Sync", "Sized", "Unpin", "Drop", "Fn", "FnMut", "FnOnce",
    "Box", "ToOwned", "ToString", "Option", "Some", "None", "Result", "Ok", "Err", "Iterator",
    "IntoIterator", "DoubleEndedIterator", "ExactSizeIterator", "Extend", "Default", "Eq", "PartialEq",
    "Ord", "PartialOrd", "AsRef", "AsMut", "Into", "From", "TryFrom", "TryInto", "FromIterator", "Debug",
}
# FR-257: member-function names rs_parse reads as a Vec/String/Clone method
# (or that the derived Clone already defines).
METHOD_RESERVED = {
    "len", "is_empty", "clone", "clone_from", "to_string", "as_bytes", "iter", "iter_mut", "push", "pop",
    "clear", "resize", "push_str", "wrapping_add", "wrapping_sub", "wrapping_mul", "wrapping_neg",
    "copied", "cloned",
}

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


class Sigs(dict):
    """Absolute function path -> (param types, return type), plus FR-257's
    ``structs`` (struct path -> StructDef) and ``recv`` (member-function
    path -> receiver kind).  A plain dict also works where no struct
    occurs (every lookup below goes through ``getattr(.., {})``)."""

    def __init__(self, *a):
        dict.__init__(self, *a)
        self.structs = {}
        self.recv = {}


def structs_of(sigs):
    return getattr(sigs, "structs", None) or {}


def recv_of(sigs):
    return getattr(sigs, "recv", None) or {}


def sigs_of(program):
    """Absolute function path -> (param types, return type)."""
    sigs = Sigs()
    for f in program.all_funcs():
        if f.path not in sigs:
            sigs[f.path] = (tuple(p.ty for p in f.params), f.ret)
        if f.owner:
            sigs.recv.setdefault(f.path, f.recv)
    for sd in program.structs():
        sigs.structs.setdefault(sd.path, sd)
    return sigs


def method_path(struct_path, name):
    return struct_path + "::" + name


def tree_order(items):
    """N13: ``items`` (Funcs and ResidualItems) in namespace-tree order."""
    root = ([], {})  # (ordered children, name -> child node)

    def node(ns):
        n = root
        for c in ns:
            if c not in n[1]:
                child = ([], {})
                n[1][c] = child
                n[0].append(child)
            n = n[1][c]
        return n

    for it in items:
        node(getattr(it, "ns", ()))[0].append(it)
    out = []

    def flat(n):
        for x in n[0]:
            if isinstance(x, tuple):
                flat(x)
            else:
                out.append(x)

    flat(root)
    return tuple(out)


LITERAL_TYPES = K.INT_TYPES

# Places: what can be borrowed, indexed, cloned or written through.
PLACES = (K.Var, K.Index, K.Field)


def is_place(e):
    if isinstance(e, K.Var):
        return True
    if isinstance(e, (K.Index, K.Field)):
        return is_place(e.base)
    return False


def base_var(e):
    """The variable a place is rooted at (``v`` for ``v[i].f[j]``), or None."""
    while isinstance(e, (K.Index, K.Field)):
        e = e.base
    return e.name if isinstance(e, K.Var) else None


def place_path(e):
    """FR-257: a place as a comparable access path: ('v', '[]', 'f', ..).
    Every index is the same wildcard '[]' (two indices may be equal)."""
    out = []
    while isinstance(e, (K.Index, K.Field)):
        out.append("[]" if isinstance(e, K.Index) else e.name)
        e = e.base
    if not isinstance(e, K.Var):
        return None
    out.append(e.name)
    return tuple(reversed(out))


def path_prefix(a, b):
    """Is place path ``a`` a prefix of (or equal to) place path ``b``?"""
    return a is not None and b is not None and len(a) <= len(b) and b[:len(a)] == a


def has_index(e):
    while isinstance(e, (K.Index, K.Field)):
        if isinstance(e, K.Index):
            return True
        e = e.base
    return False


def var_type(scope, name):
    """(declared type, mutable) or None; a RefT is NOT unwrapped."""
    return scope.lookup(name)


def _str_operand(t):
    return t == K.STRING or t == K.STR


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
        return K.deref(v[0])
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
        if K.is_owned_nc(a) or K.is_owned_nc(b):
            return _nc_binary(e, a, b)
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
        if recv_of(sigs).get(e.name) in ("ref", "mut"):
            raise KernelTypeError(e.loc, "member function '%s' called without an object" % e.name)
        return _call_type(e, e.name, sig, e.args, scope, sigs)
    if isinstance(e, K.MethodCall):
        rt = type_of(e.recv, scope, sigs)
        if not isinstance(rt, K.StructT):
            raise KernelTypeError(e.loc, "unsupported: method call `.%s()`" % e.name)
        mp = method_path(rt.path, e.name)
        sig = sigs.get(mp)
        kind = recv_of(sigs).get(mp)
        if sig is None or kind is None:
            raise KernelTypeError(e.loc, "struct '%s' has no member function '%s'" % (rt.path, e.name))
        if kind not in ("ref", "mut"):
            raise KernelTypeError(e.loc, "%s '%s' called on an object" % (
                "constructor" if kind == "ctor" else "static member function", mp))
        return _call_type(e, mp, sig, e.args, scope, sigs)
    if isinstance(e, K.Field):
        bt = type_of(e.base, scope, sigs)
        if not isinstance(bt, K.StructT):
            raise KernelTypeError(e.loc, "member access on a value of type %s" % bt)
        sd = structs_of(sigs).get(bt.path)
        if sd is None:
            raise KernelTypeError(e.loc, "use of undeclared struct '%s'" % bt.path)
        ft = sd.field_ty(e.name)
        if ft is None:
            raise KernelTypeError(e.loc, "struct '%s' has no field '%s'" % (bt.path, e.name))
        return ft
    if isinstance(e, K.StructLit):
        sd = structs_of(sigs).get(e.path)
        if sd is None:
            raise KernelTypeError(e.loc, "use of undeclared struct '%s'" % e.path)
        if len(e.values) != len(sd.fields):
            raise KernelTypeError(e.loc, "initializer of '%s' has %d values for %d fields" % (
                e.path, len(e.values), len(sd.fields)))
        for fd, v in zip(sd.fields, e.values):
            t = type_of(v, scope, sigs)
            if t != fd.ty:
                raise KernelTypeError(getattr(v, "loc", None) or e.loc,
                                      "field '%s' of '%s' initialized with %s, expected %s" % (fd.name, e.path, t, fd.ty))
        return K.StructT(e.path)
    if isinstance(e, K.Clone):
        t = type_of(e.operand, scope, sigs)
        if K.is_copy(t):
            raise KernelTypeError(e.loc, "clone of a Copy value of type %s" % t)
        return t
    if isinstance(e, K.Index):
        bt = type_of(e.base, scope, sigs)
        if not isinstance(bt, K.VecT):
            raise KernelTypeError(e.loc, "indexing a value of type %s" % bt)
        it = type_of(e.index, scope, sigs)
        if it != K.USIZE:
            raise KernelTypeError(getattr(e.index, "loc", None) or e.loc, "index of type %s, expected usize" % it)
        return bt.elem
    if isinstance(e, K.ByteAt):
        bt = type_of(e.base, scope, sigs)
        if bt != K.STRING:
            raise KernelTypeError(e.loc, "byte read of a value of type %s" % bt)
        it = type_of(e.index, scope, sigs)
        if it != K.USIZE:
            raise KernelTypeError(getattr(e.index, "loc", None) or e.loc, "index of type %s, expected usize" % it)
        return K.I8
    if isinstance(e, (K.Len, K.IsEmpty)):
        bt = type_of(e.base, scope, sigs)
        if not (isinstance(bt, K.VecT) or bt == K.STRING):
            raise KernelTypeError(e.loc, "%s of a value of type %s" % (
                "size" if isinstance(e, K.Len) else "empty", bt))
        return K.USIZE if isinstance(e, K.Len) else K.BOOL
    if isinstance(e, K.VecLit):
        if e.elem is None:
            raise KernelTypeError(e.loc, "vector literal of unknown element type")
        if not K.elem_ok(e.elem):
            raise KernelTypeError(e.loc, "vector of %s" % e.elem)
        for x in e.elems:
            t = type_of(x, scope, sigs)
            if t != e.elem:
                raise KernelTypeError(getattr(x, "loc", None) or e.loc,
                                      "vector element of type %s, expected %s" % (t, e.elem))
        return K.VecT(e.elem)
    if isinstance(e, K.VecFill):
        t = type_of(e.value, scope, sigs)
        if not K.elem_ok(t):
            raise KernelTypeError(e.loc, "vector of %s" % t)
        ct = type_of(e.count, scope, sigs)
        if ct != K.USIZE:
            raise KernelTypeError(getattr(e.count, "loc", None) or e.loc, "vector count of type %s, expected usize" % ct)
        return K.VecT(t)
    if isinstance(e, (K.StrNew, K.StrFrom)):
        return K.STRING
    if isinstance(e, K.Substr):
        if type_of(e.base, scope, sigs) != K.STRING:
            raise KernelTypeError(e.loc, "substr of a non-string")
        for x in (e.pos, e.count):
            if x is not None and type_of(x, scope, sigs) != K.USIZE:
                raise KernelTypeError(getattr(x, "loc", None) or e.loc, "substr position/count must be usize")
        return K.STRING
    if isinstance(e, K.ToStr):
        t = type_of(e.operand, scope, sigs)
        if t not in K.ARITH_INT:
            raise KernelTypeError(e.loc, "to_string of %s (only int, long long, unsigned, unsigned long long, "
                                         "std::size_t)" % t)
        return K.STRING
    if isinstance(e, K.Borrow):
        return type_of(e.operand, scope, sigs)
    if isinstance(e, K.ResidualExpr):
        raise KernelTypeError(e.loc, "residual: " + e.reason)
    raise KernelTypeError(None, "not an expression: %r" % (e,))


def _call_type(e, name, sig, args, scope, sigs):
    ptys, ret = sig
    if len(ptys) != len(args):
        raise KernelTypeError(e.loc, "'%s' takes %d arguments, %d given" % (name, len(ptys), len(args)))
    for i, (pt, a) in enumerate(zip(ptys, args)):
        at = type_of(a, scope, sigs)
        if at != K.deref(pt):
            raise KernelTypeError(getattr(a, "loc", None) or e.loc,
                                  "argument %d of '%s' has type %s, expected %s" % (i + 1, name, at, K.deref(pt)))
    return ret


def _nc_binary(e, a, b):
    """Binary operators on String / Vec operands (FR-251)."""
    op = e.op
    if a == K.STRING and op == "add":
        if not _str_operand(b):
            raise KernelTypeError(e.loc, "string + %s" % b)
        return K.STRING
    if op in ("eq", "ne"):
        if (a == K.STRING and _str_operand(b)) or (b == K.STRING and _str_operand(a)):
            return K.BOOL
        if isinstance(a, K.VecT) and a == b and not K.has_struct(a):
            return K.BOOL
    if op in ("lt", "le", "gt", "ge") and a == K.STRING and b == K.STRING:
        return K.BOOL
    raise KernelTypeError(e.loc, "operator %s on %s and %s" % (op, a, b))


def value_type(e, scope, sigs):
    """type_of, additionally rejecting unit-typed calls and bare strings."""
    t = type_of(e, scope, sigs)
    if t == K.UNIT:
        raise KernelTypeError(e.loc, "call to void function '%s' used as a value" % getattr(e, "name", "?"))
    if isinstance(e, K.StructLit) or K.has_struct(t):
        _struct_known(t, e, sigs)
    if t == K.STR:
        raise KernelTypeError(e.loc, "string literal outside a printf %s argument")
    _check_value_subexprs(e, scope, sigs)
    return t


def _struct_known(t, e, sigs):
    while isinstance(t, (K.VecT, K.RefT)):
        t = t.elem if isinstance(t, K.VecT) else t.inner
    if isinstance(t, K.StructT) and t.path not in structs_of(sigs):
        raise KernelTypeError(getattr(e, "loc", None), "use of undeclared struct '%s'" % t.path)


def _strlit_slots(e, scope, sigs):
    """ids of StrLit nodes directly in a position that takes a C string:
    the rhs of string `+`, an operand of string ==/!=."""
    out = set()
    for n in K.walk(e):
        if isinstance(n, K.Binary) and n.op in ("add", "eq", "ne"):
            for side in (n.lhs, n.rhs):
                if isinstance(side, K.StrLit):
                    try:
                        if type_of(n, scope, sigs) is not None:
                            out.add(id(side))
                    except KernelTypeError:
                        pass
            if n.op == "add" and isinstance(n.lhs, K.StrLit):
                out.discard(id(n.lhs))
    return out


def _check_value_subexprs(e, scope, sigs):
    ok = _strlit_slots(e, scope, sigs)
    for n in K.walk(e):
        if n is e:
            continue
        if isinstance(n, K.StrLit) and id(n) not in ok:
            raise KernelTypeError(n.loc, "string literal outside a printf %s argument")
        if isinstance(n, K.Call) and sigs.get(n.name, (None, None))[1] == K.UNIT:
            raise KernelTypeError(n.loc, "call to void function '%s' used as a value" % n.name)
        if isinstance(n, K.MethodCall) and type_of(n, scope, sigs) == K.UNIT:
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


def mut_ref_args(call, sigs):
    """Names passed to `T&` (mutable reference) parameters of ``call``."""
    sig = sigs.get(call.name) if sigs else None
    out = set()
    if sig is None:
        return out
    for pt, a in zip(sig[0], call.args):
        a = a.operand if isinstance(a, K.Borrow) else a
        if isinstance(pt, K.RefT) and pt.mutable and is_place(a):
            out.add(base_var(a))
    return out


def call_sig(n, scope, sigs):
    """(path, signature) of a Call or MethodCall, or (None, None)."""
    if isinstance(n, K.Call):
        return n.name, sigs.get(n.name)
    if isinstance(n, K.MethodCall) and scope is not None:
        try:
            rt = type_of(n.recv, scope, sigs)
        except KernelTypeError:
            return None, None
        if isinstance(rt, K.StructT):
            mp = method_path(rt.path, n.name)
            return mp, sigs.get(mp)
    return None, None


def mut_receivers(block, sigs):
    """FR-257: places that receive a `&mut self` member-function call (by
    name: a call on a place rooted at `self` mutates `self`).  Resolved by
    name without a scope: ANY struct's non-const member function of that
    name counts (conservative)."""
    names = {p.rsplit("::", 1)[1] for p, k in recv_of(sigs).items() if k == "mut"}
    return [n.recv for n in K.walk(block) if isinstance(n, K.MethodCall)
            and (recv_of(sigs).get(method_path(n.owner, n.name)) == "mut" if n.owner else n.name in names)]


def method_sigs(n, sigs):
    """Signatures a MethodCall may call: exact when normalize resolved its
    owner, otherwise every member function of that name (conservative)."""
    if n.owner:
        sig = (sigs or {}).get(method_path(n.owner, n.name))
        return [sig] if sig else []
    return [sg for pth, sg in (sigs or {}).items()
            if recv_of(sigs).get(pth) in ("ref", "mut") and pth.rsplit("::", 1)[1] == n.name]


def assigned_names(block, sigs=None):
    out = set()
    for n in K.walk(block):
        if isinstance(n, (K.Assign, K.CompoundAssign)):
            out.add(n.name)
        elif isinstance(n, K.Call):
            out |= mut_ref_args(n, sigs)
        elif isinstance(n, K.MethodCall):
            out |= _method_mut_ref_args(n, sigs)
    return out


def _method_mut_ref_args(n, sigs):
    """Names passed to `T&` parameters of ANY member function named like
    ``n`` (no scope here: conservative over every struct)."""
    out = set()
    for sig in method_sigs(n, sigs):
        if len(sig[0]) == len(n.args):
            for pt, a in zip(sig[0], n.args):
                a = a.operand if isinstance(a, K.Borrow) else a
                if isinstance(pt, K.RefT) and pt.mutable and is_place(a):
                    out.add(base_var(a))
    return out


def mutated_bases(block, sigs):
    """Variables a block may MUTATE: assigned, stored into, pushed to,
    passed by `T&`, or iterated mutably -- the range-for alias check."""
    out = assigned_names(block, sigs)
    for n in K.walk(block):
        if isinstance(n, (K.Store, K.CompoundStore) + K.MUTATING_STMTS):
            b = base_var(n.target)
            if b:
                out.add(b)
        elif isinstance(n, K.ForEach) and n.mode == "mut":
            b = base_var(n.iterable)
            if b:
                out.add(b)
    for r in mut_receivers(block, sigs):
        b = base_var(r)
        if b:
            out.add(b)
    return out


def resizes(block, sigs, bound):
    """FR-257 (generalizing FR-251's resized_vars): may ``block`` change the
    SIZE of the place ``bound`` (the `v` / `p.v` / `v[0]` of a counting
    loop's `i < v.size()`)?  Yes when it assigns the root variable, or when
    a mutating statement, a store, a `T&` argument or a `&mut self`
    receiver reaches a place that is a PREFIX of the bound (every index is
    a wildcard: `v[i] = w` may replace `v[0]`); writing an element of the
    bound itself (`v[i] = x` under `i < v.size()`) does not."""
    P = place_path(bound)
    if P is None:
        return True
    if P[0] in assigned_names(block, None):
        return True
    targets = []
    for n in K.walk(block):
        if isinstance(n, (K.Store, K.CompoundStore) + K.MUTATING_STMTS):
            targets.append(n.target.operand if isinstance(n.target, K.Borrow) else n.target)
        elif isinstance(n, (K.Call, K.MethodCall)):
            if isinstance(n, K.Call):
                sig = (sigs or {}).get(n.name)
                sig_list = [sig] if sig else []
            else:
                sig_list = method_sigs(n, sigs)
            for sig in sig_list:
                for pt, a in zip(sig[0], n.args):
                    a = a.operand if isinstance(a, K.Borrow) else a
                    if isinstance(pt, K.RefT) and pt.mutable and is_place(a):
                        targets.append(a)
        elif isinstance(n, K.ForEach) and n.mode == "mut":
            targets.append(K.Index(n.iterable, K.IntLit(0, K.USIZE)))
    targets += mut_receivers(block, sigs)
    return any(path_prefix(place_path(t), P) for t in targets if place_path(t) is not None)


def resized_vars(block, sigs):
    """Variables whose SIZE a block may change: whole-variable assignment,
    push/pop/clear/resize/+= on the variable itself (not on an element),
    or passing it by `T&` (the callee may resize it)."""
    out = assigned_names(block, sigs)
    for n in K.walk(block):
        if isinstance(n, K.MUTATING_STMTS):
            t = n.target.operand if isinstance(n.target, K.Borrow) else n.target
            if isinstance(t, K.Var):
                out.add(t.name)
    return out


def declared_names(block):
    out = set()
    for n in K.walk(block):
        if isinstance(n, K.Let):
            out.add(n.name)
        elif isinstance(n, (K.For, K.ForEach)):
            out.add(n.var)
    return out


def stmt_exprs(s):
    """The expressions a statement evaluates ITSELF (not nested blocks)."""
    if isinstance(s, K.Let):
        return [s.init]
    if isinstance(s, (K.Assign, K.CompoundAssign)):
        return [K.Var(s.name, s.loc), s.value]
    if isinstance(s, (K.Store, K.CompoundStore, K.Push, K.PushStr, K.PushByte)):
        return [s.target, s.value]
    if isinstance(s, (K.Pop, K.Clear)):
        return [s.target]
    if isinstance(s, K.Resize):
        return [s.target, s.count, s.value]
    if isinstance(s, K.ExprStmt):
        return [s.expr]
    if isinstance(s, K.Print):
        return [sg.arg for sg in s.segs if isinstance(sg, K.Spec)]
    if isinstance(s, K.Return):
        return [] if s.value is None else [s.value]
    if isinstance(s, (K.If, K.While)):
        return [s.cond]
    if isinstance(s, K.For):
        return [s.lo, s.hi]
    if isinstance(s, K.ForEach):
        return [s.iterable]
    return []


class _Checker:
    def __init__(self, program, surface):
        self.program = program
        self.surface = surface
        self.sigs = sigs_of(program)
        self.errors = []
        self.ns = ()  # FR-260: the namespace of the function being checked
        self.internal = {}
        self.structs = structs_of(self.sigs)
        self.struct_leaves = {sd.name for sd in program.structs()}
        self.cur = None  # FR-257: the function being checked
        self.ctor_lit = None

    def err(self, loc, msg):
        self.errors.append((loc, msg))

    def ident(self, name, loc):
        p = ident_problem(name)
        if p:
            self.err(loc, p)
        elif name in self.struct_leaves:
            # FR-257: a C++ variable hides a type of its name (`S S = ..;`
            # makes a later `S{..}` an error); the subset keeps them apart
            self.err(loc, "variable '%s' has the name of a struct" % name)
        elif K.path_of(self.ns, name) in self.sigs:
            # only a SAME-namespace function is spelled bare (FR-260), so
            # only it can be hidden by a local of the same name
            self.err(loc, "variable '%s' has the name of a function" % name)

    def expr(self, e, scope, want=None, pos="value"):
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
        self.wf(e, scope, pos)
        return t

    # ---------------------------------------------------- FR-251 value rules
    def wf(self, e, scope, pos):
        """Structural rules beyond typing.  ``pos`` is the position kind:
        'value' (a scalar, or a non-Copy value that is only read),
        'consume' (the value is moved into a new owner), 'copy' (C++ takes
        `const T&` and copies; Rust needs an owned clone), 'borrow' (read
        through a reference)."""
        try:
            t = type_of(e, scope, self.sigs)
        except KernelTypeError:
            return
        L = getattr(e, "loc", None)
        if isinstance(e, K.Borrow):
            if self.surface != "rust" or id(e) not in self.borrow_ok:
                self.err(L, "unsupported: `&` borrow outside an argument position")
            self.wf(e.operand, scope, "borrow")
            return
        if pos == "consume" and not K.is_copy(t):
            if isinstance(e, K.Index):
                self.err(L, "unsupported: move out of an indexed element (the subset copies it: "
                            "C++ copy, Rust `.clone()`)")
            elif isinstance(e, K.Field):
                self.err(L, "unsupported: move out of a data member (the subset copies it: "
                            "C++ copy, Rust `.clone()`)")
            elif isinstance(e, K.Var):
                v = scope.lookup(e.name)
                if v is not None and isinstance(v[0], K.RefT):
                    self.err(L, "unsupported: move out of reference '%s' (the subset copies it: "
                                "C++ copy, Rust `.clone()`)" % e.name)
                elif v is not None and not v[1]:
                    self.err(L, "unsupported: move of immutable '%s' (C++ cannot move a const "
                                "object; it would copy)" % e.name)
                else:
                    self.moves.append(e)
        if isinstance(e, K.Clone):
            if pos == "copy" and self.surface != "rust":
                # (the Rust spelling of a copied non-Copy place IS `.clone()`; N8 drops it)
                self.err(L, "clone in a copied position (C++ copies from `const T&` already)")
            if not is_place(e.operand):
                self.err(L, "clone of a temporary (only a variable or an element is copied)")
            self.wf(e.operand, scope, "borrow")
        elif isinstance(e, K.Var):
            if e.spelling == "deref":
                v = scope.lookup(e.name)
                if v is not None and not isinstance(v[0], K.RefT):
                    self.err(L, "unsupported: `*` of '%s', which is not a reference" % e.name)
        elif isinstance(e, (K.Index, K.ByteAt)):
            self.wf(e.base, scope, "borrow")
            self.wf(e.index, scope, "value")
        elif isinstance(e, (K.Len, K.IsEmpty)):
            self.wf(e.base, scope, "borrow")
        elif isinstance(e, K.VecLit):
            for x in e.elems:
                self.wf(x, scope, "copy")
        elif isinstance(e, K.VecFill):
            self.wf(e.value, scope, "copy")
            self.wf(e.count, scope, "value")
        elif isinstance(e, K.StrFrom):
            if any(not (1 <= ord(c) <= 0x7F) for c in e.value):
                self.err(L, "unsupported: non-ASCII string literal (every String is ASCII so byte "
                            "indexing never splits a character)")
        elif isinstance(e, K.Substr):
            self.wf(e.base, scope, "borrow")
            self.wf(e.pos, scope, "value")
            if e.count is not None:
                self.wf(e.count, scope, "value")
        elif isinstance(e, (K.ToStr, K.Unary, K.Cast)):
            self.wf(e.operand, scope, "value")
        elif isinstance(e, K.Binary):
            try:
                lt = type_of(e.lhs, scope, self.sigs)
            except KernelTypeError:
                return
            if lt == K.STRING and e.op == "add":
                self.wf(e.lhs, scope, "consume")
                self.wf(e.rhs, scope, "borrow")
            elif e.op in K.CMP_OPS and not (K.is_copy(lt) or lt == K.STR):
                self.wf(e.lhs, scope, "borrow")
                self.wf(e.rhs, scope, "borrow")
            elif e.op in K.CMP_OPS and lt == K.STR:
                self.wf(e.rhs, scope, "borrow")
            else:
                self.wf(e.lhs, scope, "value")
                self.wf(e.rhs, scope, "value")
            if isinstance(e.lhs, K.StrLit) and e.op == "add":
                self.err(e.lhs.loc, "string literal outside a printf %s argument")
        elif isinstance(e, K.Call):
            self.call_args(e, scope)
        elif isinstance(e, K.Field):
            self.wf(e.base, scope, "borrow")
        elif isinstance(e, K.StructLit):
            sd = self.structs.get(e.path)
            if sd is not None:
                if sd.ctor() is not None and e is not self.ctor_lit:
                    self.err(L, "unsupported: aggregate initialization of '%s', which has a constructor "
                                "(C++ would call the constructor)" % e.path)
                for fd, v in zip(sd.fields, e.values):
                    self.wf(v, scope, self.consume_pos(fd.ty))
        elif isinstance(e, K.MethodCall):
            self.method_call(e, scope)
        elif isinstance(e, K.StrLit):
            pass

    def call_args(self, e, scope, name=None, sig=None, recv_use=None):
        if sig is None:
            sig = self.sigs.get(e.name)
            name = e.name
        if sig is None:
            return
        e_name = name
        uses = [] if recv_use is None else [recv_use]
        for i, (pt, a) in enumerate(zip(sig[0], e.args)):
            raw = a
            if isinstance(pt, K.RefT):
                inner = a.operand if isinstance(a, K.Borrow) else a
                L = getattr(inner, "loc", None) or e.loc
                if pt.mutable and not is_place(inner):
                    self.err(L, "unsupported: argument %d of '%s' binds a non-const reference to something "
                                "other than a variable or element" % (i + 1, e_name))
                    continue
                b = base_var(inner) if is_place(inner) else None
                v = scope.lookup(b) if b is not None else None
                is_refvar = isinstance(inner, K.Var) and v is not None and isinstance(v[0], K.RefT)
                if pt.mutable and isinstance(inner, K.Index):
                    self.self_indexed(inner)
                if pt.mutable and v is not None and not (
                        (isinstance(v[0], K.RefT) and v[0].mutable) or (not isinstance(v[0], K.RefT) and v[1])):
                    self.err(L, "argument %d of '%s' binds a non-const reference to immutable '%s'" % (i + 1, e_name, b))
                if self.surface == "rust":
                    if isinstance(raw, K.Borrow):
                        if is_refvar or raw.mutable != pt.mutable:
                            self.err(raw.loc, "unsupported: argument %d of '%s' is not spelled the canonical "
                                              "way (`%s`)" % (i + 1, e_name, "x" if is_refvar else
                                                              ("&mut x" if pt.mutable else "&x")))
                        self.borrow_ok.add(id(raw))
                    elif not is_refvar:
                        self.err(L, "unsupported: argument %d of '%s' binds a reference; the subset spells it "
                                    "`%s`" % (i + 1, e_name, "&mut x" if pt.mutable else "&x"))
                self.wf(raw, scope, "borrow")
                if b is not None:
                    uses.append((b, "mut" if pt.mutable else "shared", i))
                for n in K.walk(inner):
                    if isinstance(n, K.Var) and n is not inner and not (isinstance(inner, K.Index)
                                                                        and n.name == b and is_place(n)):
                        uses.append((n.name, "read", i))
                    elif isinstance(n, K.Var) and b is None:
                        uses.append((n.name, "read", i))
                continue
            self.wf(a, scope, "value" if K.is_copy(K.deref(pt)) else "consume")
            for n in K.walk(a):
                if isinstance(n, K.Var):
                    uses.append((n.name, "read", i))
        for name, kind, i in uses:
            if kind != "mut" or i == -1:
                continue
            others = [u for u in uses if u[0] == name and u[2] != i]
            if others:
                self.err(e.loc, "unsupported: '%s' is passed to '%s' by non-const reference and also used "
                                "by another argument of the same call (aliasing)" % (name, e_name))
                break
        return uses

    def method_call(self, e, scope):
        """FR-257: `r.m(args)`.  Measured on rustc 1.98 (two-phase borrows):
        with `&mut self`, arguments may READ the receiver when it is a
        variable or a field chain (`s.set(s.get())`, `p.q.set(p.q.x)`,
        `self.set(self.x + 1)` compile) but not when the receiver goes
        through an index (`v[0].set(v[0].get())` and `v[0].set(v.len())`
        are E0502: IndexMut is not two-phase); an argument that is itself a
        reference to the receiver's variable (`s.absorb(&s)`) is E0502, and
        one that mutates it (`s.set(bump(&mut s))`) E0499.  With `&self`,
        an argument that mutates the receiver's variable is E0502.  Each of
        those shapes is a located residual."""
        try:
            rt = type_of(e.recv, scope, self.sigs)
        except KernelTypeError:
            return
        mp = method_path(rt.path, e.name)
        kind = recv_of(self.sigs).get(mp)
        sig = self.sigs.get(mp)
        L = e.loc
        if not is_place(e.recv):
            self.err(getattr(e.recv, "loc", None) or L,
                     "unsupported: member function call on something other than a variable, element or "
                     "data member")
            return
        if kind == "mut":
            self.writable(e.recv, scope, "call of non-const member function '%s'" % e.name)
        self.wf(e.recv, scope, "borrow")
        b = base_var(e.recv)
        uses = self.call_args(e, scope, mp, sig) or []
        if b is None:
            return
        direct_ref = [u for u in uses if u[0] == b and u[1] in ("shared", "mut")]
        mutating = [u for u in uses if u[0] == b and u[1] == "mut"]
        for a in e.args:
            for n in K.walk(a):
                if isinstance(n, (K.Call, K.MethodCall)) and b in self.call_mutates(n, scope):
                    mutating.append(n)
        if mutating:
            self.err(L, "unsupported: an argument of '%s' modifies '%s', the object it is called on "
                        "(a second borrow in Rust)" % (e.name, b))
        elif kind == "mut" and direct_ref:
            self.err(L, "unsupported: '%s' is passed by reference to '%s', a non-const member function "
                        "called on it (aliasing)" % (b, e.name))
        elif kind == "mut" and has_index(e.recv) and any(mentions(a, b) for a in e.args):
            self.err(L, "unsupported: non-const member function '%s' is called on an element of '%s' "
                        "with an argument that reads '%s' (a second borrow in Rust)" % (e.name, b, b))

    def call_mutates(self, n, scope):
        """Variables a single call node may modify itself: `T&` arguments
        and the receiver of a non-const member function."""
        out = set()
        path, sig = call_sig(n, scope, self.sigs)
        if sig is None:
            return out
        for pt, a in zip(sig[0], n.args):
            a = a.operand if isinstance(a, K.Borrow) else a
            if isinstance(pt, K.RefT) and pt.mutable and is_place(a):
                out.add(base_var(a))
        if isinstance(n, K.MethodCall) and recv_of(self.sigs).get(path) == "mut":
            out.add(base_var(n.recv))
        return out

    @staticmethod
    def ref_arg_spelling(pt, owned, name):
        if not owned:
            return name
        return "&%s%s" % ("mut " if pt.mutable else "", name)

    def writable(self, place, scope, what):
        b = base_var(place)
        if b is None:
            self.err(getattr(place, "loc", None), "%s of something other than a variable or element" % what)
            return
        self.self_indexed(place)
        v = scope.lookup(b)
        if v is None:
            return
        ty, mut = v
        if isinstance(ty, K.RefT):
            mut = ty.mutable
        if not mut:
            self.err(getattr(place, "loc", None), "%s of immutable '%s'" % (what, b))

    def self_indexed(self, place):
        """`v[v.len() - 1] = x`: Rust takes `&mut v` (IndexMut) BEFORE it
        evaluates the index, which reads v -- E0502 (measured, also through
        `&mut Vec` and for nested `v[0][v.len() - 1]`).  `v.back() = x`
        desugars to exactly this, so it lands here too."""
        b = base_var(place)
        p = place
        while isinstance(p, (K.Index, K.Field)):
            if isinstance(p, K.Index) and mentions(p.index, b):
                self.err(getattr(place, "loc", None),
                         "unsupported: an element of '%s' is written through an index that reads '%s' "
                         "(a second borrow in Rust)" % (b, b))
                return True
            p = p.base
        return False

    def place_ty(self, place, scope):
        try:
            return type_of(place, scope, self.sigs)
        except KernelTypeError as ex:
            self.err(ex.loc or getattr(place, "loc", None), ex.msg)
            return None

    def rust_spelling(self, e, scope):
        for n in K.walk(e):
            if isinstance(n, K.Binary) and n.op in K.WRAPPING_OPS:
                t = type_of(n.lhs, scope, self.sigs)
                if isinstance(t, K.Prim):
                    self._spelled(n, t, n.op)
                elif t == K.STRING and isinstance(n.rhs, K.Borrow) and not n.rhs.mutable:
                    self.borrow_ok.add(id(n.rhs))
            elif isinstance(n, K.Unary) and n.op == "neg":
                t = type_of(n.operand, scope, self.sigs)
                self._spelled(n, t, "neg")
            elif isinstance(n, K.Substr) and isinstance(n.base, K.Borrow) and not n.base.mutable:
                self.borrow_ok.add(id(n.base))

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
        funcs = self.program.funcs()
        structs = self.program.structs()
        # FR-260: namespaces declared in each scope (C++ forbids a function
        # and a namespace of one name in one scope; Rust would allow it)
        ns_names = set()
        self.internal = {}
        for f in funcs + structs:
            for i in range(len(f.ns)):
                ns_names.add(f.ns[:i + 1])
            if f.internal and isinstance(f, K.Func):
                self.internal[f.path] = f.ns
        bad_ns = set()
        for f in funcs + structs:
            for i, c in enumerate(f.ns):
                p = ident_problem(c) or (("identifier '%s' names a Rust prelude type" % c) if c in NS_RESERVED else None)
                if p and f.ns[:i + 1] not in bad_ns:
                    bad_ns.add(f.ns[:i + 1])
                    self.err(f.loc, "namespace name: " + p)
        for f in funcs:
            if f.path in seen:
                self.err(f.loc, "function '%s' defined twice" % f.path)
            seen.add(f.path)
            if f.name == "main" and f.ns:
                self.err(f.loc, "function 'main' inside a namespace")
            elif f.name != "main":
                p = ident_problem(f.name)
                if p:
                    self.err(f.loc, p)
            if f.ns + (f.name,) in ns_names:
                self.err(f.loc, "function '%s' has the name of a namespace declared in the same scope" % f.path)
            if f.internal and not f.ns:
                self.err(f.loc, "internal function at the root (normal form N14)")
            if f.owner or f.recv:
                self.err(f.loc, "member function '%s' outside its struct" % f.path)
            self.func(f)
        self.check_structs(structs, ns_names, {f.path for f in funcs})
        return self.errors

    # ---------------------------------------------------- FR-257 structs
    def check_structs(self, structs, ns_names, free_paths):
        seen = set()
        for sd in structs:
            if sd.path in seen:
                self.err(sd.loc, "struct '%s' defined twice" % sd.path)
            seen.add(sd.path)
            p = ident_problem(sd.name) or (
                "identifier '%s' names a Rust prelude item" % sd.name if sd.name in STRUCT_RESERVED else None)
            if p:
                self.err(sd.loc, "struct name: " + p)
            if sd.path in free_paths:
                self.err(sd.loc, "struct '%s' has the name of a function declared in the same scope" % sd.path)
            if sd.ns + (sd.name,) in ns_names:
                self.err(sd.loc, "struct '%s' has the name of a namespace declared in the same scope" % sd.path)
            if sd.internal and not sd.ns:
                self.err(sd.loc, "internal struct at the root (normal form N14)")
            self.struct_visibility(sd.ns, [fd.ty for fd in sd.fields], sd.loc)
            names = set()
            for fd in sd.fields:
                p = ident_problem(fd.name)
                if p:
                    self.err(fd.loc or sd.loc, p)
                elif fd.name in self.struct_leaves:
                    self.err(fd.loc or sd.loc, "data member '%s' has the name of a struct" % fd.name)
                if fd.name in names:
                    self.err(fd.loc or sd.loc, "data member '%s' declared twice" % fd.name)
                names.add(fd.name)
                if not self.type_ok(fd.ty, fd.loc or sd.loc):
                    self.err(fd.loc or sd.loc, "data member type %s" % fd.ty)
            mnames = set()
            for m in sd.methods:
                if m.owner != sd.name or tuple(m.ns) != tuple(sd.ns) or m.recv not in K.RECV_KINDS:
                    self.err(m.loc, "malformed member function '%s'" % m.path)
                    continue
                if m.recv == "ctor":
                    if m.name != "new":
                        self.err(m.loc, "constructor not named 'new'")
                else:
                    p = ident_problem(m.name) or (
                        "identifier '%s' is reserved by the canonical Rust spelling (a method of Vec/String or "
                        "of the derived Clone)" % m.name if m.name in METHOD_RESERVED else None)
                    if p:
                        self.err(m.loc, p)
                    elif m.name in self.struct_leaves:
                        self.err(m.loc, "member function '%s' has the name of a struct" % m.name)
                if m.name in mnames:
                    self.err(m.loc, "member function '%s' defined twice" % m.path)
                mnames.add(m.name)
                if m.name in names:
                    self.err(m.loc, "member function '%s' has the name of a data member" % m.path)
                self.func(m)
        self.check_recursion(structs)

    def check_recursion(self, structs):
        by = {sd.path: sd for sd in structs}

        def direct(sd):
            return {fd.ty.path for fd in sd.fields if isinstance(fd.ty, K.StructT) and fd.ty.path in by}

        for sd in structs:
            stack, seen = list(direct(sd)), set()
            while stack:
                x = stack.pop()
                if x == sd.path:
                    self.err(sd.loc, "struct '%s' contains itself by value (infinite size)" % sd.path)
                    break
                if x in seen:
                    continue
                seen.add(x)
                stack.extend(direct(by[x]))

    def type_ok(self, t, loc=None, ref_ok=False):
        if isinstance(t, K.RefT):
            return ref_ok and self.type_ok(t.inner, loc)
        if isinstance(t, K.StructT):
            return t.path in self.structs
        if isinstance(t, K.VecT):
            return K.elem_ok(t.elem) and self.type_ok(t.elem, loc)
        return t in K.ALL_PRIMS or t == K.STRING

    def struct_visibility(self, ns, types, loc, paths=()):
        """FR-257: an internal struct (C++ anonymous-namespace member, a
        private Rust `struct` in a `mod`) may only be NAMED from inside its
        namespace (rustc E0603); C++ would also accept `::a::S` from outside."""
        names = []
        for t in types:
            while isinstance(t, (K.VecT, K.RefT)):
                t = t.elem if isinstance(t, K.VecT) else t.inner
            if isinstance(t, K.StructT):
                names.append(t.path)
        for p in list(names) + list(paths):
            sd = self.structs.get(p)
            if sd is not None and sd.internal and tuple(ns[:len(sd.ns)]) != tuple(sd.ns):
                self.err(loc, "unsupported: '%s' is internal to namespace '%s' (a C++ anonymous-namespace member, "
                              "a private Rust `struct`) and is named outside it" % (p, "::".join(sd.ns)))

    def func_struct_visibility(self, f):
        types = [p.ty for p in f.params] + [f.ret]
        paths = []
        for n in K.walk(f.body):
            if isinstance(n, K.Let):
                types.append(n.ty)
            elif isinstance(n, K.VecLit) and n.elem is not None:
                types.append(n.elem)
            elif isinstance(n, K.StructLit):
                paths.append(n.path)
            elif isinstance(n, K.Call) and recv_of(self.sigs).get(n.name):
                paths.append(n.name.rsplit("::", 1)[0])
        self.struct_visibility(f.ns, types, f.loc, paths)

    def check_visibility(self, call):
        """An internal function (C++ anonymous-namespace member, Rust private
        `fn` in a `mod`) may only be referenced from inside its namespace:
        Rust's privacy rule (E0603).  C++ would also accept `::a::h` from
        outside, which has no 1:1 Rust spelling, so it is a residual."""
        owner = self.internal.get(call.name)
        if owner is not None and self.ns[:len(owner)] != owner:
            self.err(call.loc, "unsupported: '%s' is internal to namespace '%s' (a C++ anonymous-namespace "
                               "member, a private Rust `fn`) and is referenced from outside it"
                     % (call.name, "::".join(owner)))

    def func(self, f):
        self.moves = []
        self.borrow_ok = set()
        self.ns = f.ns
        self.cur = f
        for n in K.walk(f.body):
            if isinstance(n, K.Call):
                self.check_visibility(n)
        self.func_struct_visibility(f)
        if not (f.ret in K.ALL_PRIMS or f.ret == K.UNIT or K.is_owned_nc(f.ret)):
            if isinstance(f.ret, K.RefT):
                self.err(f.loc, "unsupported: function returning a reference")
            else:
                self.err(f.loc, "return type %s" % f.ret)
        elif f.ret != K.UNIT and not self.type_ok(f.ret):
            self.err(f.loc, "return type %s" % f.ret)
        if f.path == "main":
            if f.params:
                self.err(f.loc, "main with parameters")
            if f.ret != K.UNIT:
                self.err(f.loc, "main must return unit")
            if any(isinstance(n, K.Return) for n in K.walk(f.body)):
                self.err(f.loc, "main may not contain return")
        scope = Scope()
        if f.recv in ("ref", "mut"):
            # FR-257: C++ `this` (const in a const member function) <=> Rust
            # `&self` / `&mut self`; `self` is a Rust keyword, so no Kernel
            # identifier can collide with it
            scope.declare("self", K.RefT(K.StructT(K.path_of(f.ns, f.owner)), f.recv == "mut"), False)
        self.ctor_lit = None
        has_res = any(isinstance(x, K.ResidualStmt) for x in f.body.stmts)
        if f.recv == "ctor" and not has_res:
            self.ctor_shape(f)
        if f.recv == "ctor":
            st = f.body.stmts
            if st and isinstance(st[-1], K.Return) and isinstance(st[-1].value, K.StructLit):
                # only the returned literal itself is the member-initializer
                # list; a nested `S{..}` would call the constructor in C++
                self.ctor_lit = st[-1].value
        for p in f.params:
            self.ident(p.name, p.loc)
            if not self.type_ok(p.ty, p.loc, ref_ok=True):
                self.err(p.loc, "parameter type %s" % p.ty)
            if isinstance(p.ty, K.RefT) and p.mutable:
                self.err(p.loc, "reference parameter '%s' declared mutable" % p.name)
            if p.name in scope.vars:
                self.err(p.loc, "duplicate parameter '%s'" % p.name)
            scope.declare(p.name, p.ty, p.mutable)
        self.ret = K.deref(f.ret)  # a returned reference is reported above, once
        self.block(f.body, scope, loop=False, same_scope=True)
        if f.ret != K.UNIT and not definitely_returns(f.body) and not (f.recv == "ctor" and has_res):
            self.err(f.loc, "function '%s' may reach its end without returning a value" % f.path)
        self.check_moves(f)

    def ctor_shape(self, f):
        """A constructor is a member-initializer list: its body is exactly
        `return S { every field };` and it returns its own struct."""
        owner = K.path_of(f.ns, f.owner)
        st = f.body.stmts
        if (f.ret != K.StructT(owner) or len(st) != 1 or not isinstance(st[0], K.Return)
                or not isinstance(st[0].value, K.StructLit) or st[0].value.path != owner):
            self.err(f.loc, "unsupported: constructor of '%s' whose body is not a member-initializer list "
                            "of every data member" % owner)

    @staticmethod
    def param_type_ok(t):
        if isinstance(t, K.RefT):
            t = t.inner
            return t in K.ALL_PRIMS or t == K.STRING or (isinstance(t, K.VecT) and K.elem_ok(t.elem))
        return t in K.ALL_PRIMS or t == K.STRING or (isinstance(t, K.VecT) and K.elem_ok(t.elem))

    def local_type_ok(self, t):
        return self.type_ok(t)

    # ---------------------------------------------------- the move rule
    def check_moves(self, f):
        """A move `std::move(x)` <=> `x` is accepted only when x is never
        mentioned again: not elsewhere in the same statement (C++ argument
        evaluation order is unspecified) and not in any later statement of
        the function (conservative: by name, any later mention), and never
        inside a loop that x was declared outside of (the second iteration
        would see a moved-from object in C++ and fail to compile in Rust)
        -- except `return x;`, which ends the path (later code is on other
        paths) and leaves any loop."""
        if not self.moves:
            return
        moved = {id(v): v for v in self.moves}
        units = []  # (stmt, exprs, loops, declared-name-or-None)

        def visit(block, loops):
            for s in block.stmts:
                exprs = stmt_exprs(s)
                inner_loops = loops
                if isinstance(s, K.While):
                    inner_loops = loops + (s,)
                    units.append((s, exprs, inner_loops, None))  # cond runs every iteration
                elif isinstance(s, K.Let):
                    units.append((s, exprs, loops, s.name))
                else:
                    units.append((s, exprs, loops, None))
                if isinstance(s, K.Block):
                    visit(s, loops)
                elif isinstance(s, K.If):
                    visit(s.then, loops)
                    if s.els is not None:
                        visit(s.els, loops)
                elif isinstance(s, K.While):
                    visit(s.body, inner_loops)
                elif isinstance(s, (K.For, K.ForEach)):
                    units.append((None, [], loops + (s,), s.var))
                    visit(s.body, loops + (s,))

        visit(f.body, ())
        decl_loops = {p.name: () for p in f.params}
        reported = set()
        for i, (s, exprs, loops, declared) in enumerate(units):
            for e in exprs:
                for n in K.walk(e):
                    if id(n) not in moved or id(n) in reported:
                        continue
                    name = n.name
                    is_return = isinstance(s, K.Return) and s.value is n
                    same = sum(1 for x in exprs for m in K.walk(x)
                               if isinstance(m, K.Var) and m.name == name)
                    # `return x;` ends the path: textually later code is on other paths
                    later = not is_return and any(isinstance(m, K.Var) and m.name == name
                                                  for (_, ex2, _, _) in units[i + 1:] for x in ex2
                                                  for m in K.walk(x))
                    dl = decl_loops.get(name, ())
                    in_loop = len(loops) > len(dl) and not is_return
                    if same > 1 or later:
                        reported.add(id(n))
                        self.err(n.loc, "unsupported: '%s' is used after it is moved (std::move(%s) <=> %s "
                                        "requires no later mention of %s)" % (name, name, name, name))
                    elif in_loop:
                        reported.add(id(n))
                        self.err(n.loc, "unsupported: move of '%s' inside a loop it was declared outside of"
                                 % name)
            if declared is not None:
                decl_loops[declared] = loops

    # ---------------------------------------------------- statements
    def block(self, b, scope, loop, same_scope=False):
        inner = scope if same_scope else scope.child()
        for s in b.stmts:
            self.stmt(s, inner, loop)

    def consume_pos(self, t):
        return "value" if t is None or K.is_copy(t) else "consume"

    def stmt(self, s, scope, loop):
        self.scope_now = scope
        self.stmt_alias(s)
        self.stmt_inner(s, scope, loop)

    def stmt_alias(self, s):
        """A variable a call may modify (passed by `T&`) is not mentioned
        anywhere else in the statement: C++ leaves the order of `bump(k) + k`
        unspecified, Rust fixes it left to right."""
        exprs = stmt_exprs(s)
        if not exprs:
            return
        calls = [n for e in exprs for n in K.walk(e) if isinstance(n, (K.Call, K.MethodCall))]
        for c in calls:
            path, sig = call_sig(c, self.scope_now, self.sigs)
            if sig is None:
                continue
            for pt, a in zip(sig[0], c.args):
                a = a.operand if isinstance(a, K.Borrow) else a
                if not (isinstance(pt, K.RefT) and pt.mutable and is_place(a)):
                    continue
                b = base_var(a)
                inside = sum(1 for n in K.walk(c) if isinstance(n, K.Var) and n.name == b)
                total = sum(1 for e in exprs for n in K.walk(e) if isinstance(n, K.Var) and n.name == b)
                if total > inside:
                    self.err(c.loc or s.loc, "unsupported: '%s' is passed to '%s' by non-const reference and "
                                             "also used elsewhere in the statement (C++ leaves the order "
                                             "unspecified)" % (b, path))
                    return
            if isinstance(c, K.MethodCall) and recv_of(self.sigs).get(path) == "mut" and is_place(c.recv):
                # FR-257: the receiver of a non-const member function is
                # modified exactly like a `T&` argument
                b = base_var(c.recv)
                inside = sum(1 for n in K.walk(c) if isinstance(n, K.Var) and n.name == b)
                total = sum(1 for e in exprs for n in K.walk(e) if isinstance(n, K.Var) and n.name == b)
                if total > inside:
                    self.err(c.loc or s.loc, "unsupported: non-const member function '%s' is called on '%s', "
                                             "which is also used elsewhere in the statement (C++ leaves the "
                                             "order unspecified)" % (c.name, b))
                    return

    def stmt_inner(self, s, scope, loop):
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
            if not self.local_type_ok(s.ty):
                self.err(s.loc, "variable type %s" % s.ty)
            self.expr(s.init, scope, s.ty, self.consume_pos(s.ty))
            scope.declare(s.name, s.ty, s.mutable)
        elif isinstance(s, (K.Assign, K.CompoundAssign)):
            v = scope.lookup(s.name)
            if v is None:
                self.err(s.loc, "assignment to undeclared '%s'" % s.name)
                return
            ty, mut = v
            if isinstance(ty, K.RefT):
                ty, mut = ty.inner, ty.mutable
            if not mut:
                self.err(s.loc, "assignment to immutable '%s'" % s.name)
            if isinstance(s, K.Assign):
                self.expr(s.value, scope, ty, self.consume_pos(ty))
                return
            self.compound(s, ty, scope)
        elif isinstance(s, K.Store):
            if not isinstance(s.target, (K.Index, K.Field)) or not is_place(s.target):
                self.err(s.loc, "store to something other than an element")
                return
            self.writable(s.target, scope, "assignment")
            ty = self.place_ty(s.target, scope)
            if ty is not None:
                self.wf(s.target, scope, "borrow")
                self.expr(s.value, scope, ty, self.consume_pos(ty))
        elif isinstance(s, K.CompoundStore):
            if not isinstance(s.target, (K.Index, K.Field)) or not is_place(s.target):
                self.err(s.loc, "store to something other than an element")
                return
            self.writable(s.target, scope, "assignment")
            ty = self.place_ty(s.target, scope)
            if ty is not None:
                self.wf(s.target, scope, "borrow")
                self.compound(s, ty, scope)
                if (K.is_unsigned(ty) and s.op in K.WRAPPING_OPS
                        and any(isinstance(n, K.Call) for n in K.walk(s.target))):
                    # Rust spells it `v[i] = v[i].wrapping_add(e)`: the index
                    # would be evaluated twice
                    self.err(s.loc, "unsupported: unsigned compound assignment to an element whose index "
                                    "calls a function")
        elif isinstance(s, K.Push):
            ty = self.place_ty(s.target, scope)
            if ty is None:
                return
            if not isinstance(ty, K.VecT):
                self.err(s.loc, "push_back on %s" % ty)
                return
            self.writable(s.target, scope, "push_back")
            self.wf(s.target, scope, "borrow")
            self.expr(s.value, scope, ty.elem, self.consume_pos(ty.elem))
            self.elem_arg_alias(s, [s.value])
        elif isinstance(s, (K.Pop, K.Clear)):
            ty = self.place_ty(s.target, scope)
            if ty is None:
                return
            if not (isinstance(ty, K.VecT) or (isinstance(s, K.Clear) and ty == K.STRING)):
                self.err(s.loc, "%s on %s" % ("pop_back" if isinstance(s, K.Pop) else "clear", ty))
                return
            self.writable(s.target, scope, "pop_back" if isinstance(s, K.Pop) else "clear")
            self.wf(s.target, scope, "borrow")
        elif isinstance(s, K.Resize):
            ty = self.place_ty(s.target, scope)
            if ty is None:
                return
            if not isinstance(ty, K.VecT):
                self.err(s.loc, "resize on %s" % ty)
                return
            self.writable(s.target, scope, "resize")
            self.wf(s.target, scope, "borrow")
            self.expr(s.count, scope, K.USIZE)
            self.expr(s.value, scope, ty.elem, "copy")
            self.elem_arg_alias(s, [s.count, s.value])
        elif isinstance(s, K.PushStr):
            ty = self.place_ty(s.target, scope)
            if ty is None:
                return
            if ty != K.STRING:
                self.err(s.loc, "+= on %s" % ty)
                return
            self.writable(s.target, scope, "+=")
            self.wf(s.target, scope, "borrow")
            if isinstance(s.value, K.StrLit):
                if any(not (1 <= ord(c) <= 0x7F) for c in s.value.value):
                    self.err(s.value.loc, "unsupported: non-ASCII string literal (every String is ASCII so "
                                          "byte indexing never splits a character)")
            else:
                raw = s.value
                if isinstance(raw, K.Borrow) and self.surface == "rust" and not raw.mutable:
                    self.borrow_ok.add(id(raw))
                self.expr(raw, scope, K.STRING, "borrow")
            b = base_var(s.target)
            if b is not None and mentions(s.value, b):
                # Rust: `s.push_str(&s)` borrows s twice
                self.err(s.loc, "unsupported: appending '%s' to itself" % b)
        elif isinstance(s, K.PushByte):
            ty = self.place_ty(s.target, scope)
            raw = s.target
            if isinstance(raw, K.Borrow):
                if self.surface == "rust" and raw.mutable:
                    self.borrow_ok.add(id(raw))
            if ty is None:
                return
            if ty != K.STRING:
                self.err(s.loc, "push_back of a char on %s" % ty)
                return
            self.writable(raw.operand if isinstance(raw, K.Borrow) else raw, scope, "push_back")
            self.wf(raw, scope, "borrow")
            self.expr(s.value, scope, K.I8)
            self.elem_arg_alias(s, [s.value])
        elif isinstance(s, K.ExprStmt):
            if not isinstance(s.expr, (K.Call, K.MethodCall)):
                self.err(s.loc, "expression statement that is not a call")
                return
            if isinstance(s.expr, K.Call) and recv_of(sigs).get(s.expr.name) == "ctor":
                # C++ `S(x);` DECLARES a variable x (the most vexing parse)
                self.err(s.loc, "unsupported: constructor call evaluated only for its effect")
                return
            try:
                type_of(s.expr, scope, sigs)
                for a in s.expr.args:
                    value_type(a, scope, sigs)
            except KernelTypeError as ex:
                self.err(ex.loc or s.loc, ex.msg)
                return
            if self.surface == "rust":
                for a in s.expr.args:
                    self.rust_spelling(a, scope)
            self.wf(s.expr, scope, "value")
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
            len_bound = (isinstance(s.hi, K.Len) and is_place(s.hi.base)
                         and not any(isinstance(n, K.Call) for n in K.walk(s.hi.base)))
            if not isinstance(s.hi, (K.IntLit, K.Var)) and not len_bound:
                self.err(getattr(s.hi, "loc", None) or s.loc,
                         "counting loop bound must be a literal or a variable")
            self.expr(s.hi, scope, t)
            assigned = assigned_names(s.body, sigs)
            if len_bound:
                # FR-251: `i < v.size()` is re-evaluated by C++ every
                # iteration, `0..v.len()` once by Rust: equal only while
                # the body leaves v's SIZE alone (element writes are fine,
                # and Rust holds no borrow of v across the loop)
                b = base_var(s.hi.base)
                if resizes(s.body, sigs, s.hi.base) or b in declared_names(s.body):
                    self.err(s.loc, "unsupported: counting loop body modifies or shadows '%s', whose size is "
                                    "the bound" % b)
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
        elif isinstance(s, K.ForEach):
            self.foreach(s, scope)
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
                    self.expr(s.value, scope, self.ret, self.consume_pos(self.ret))
        elif isinstance(s, K.ResidualStmt):
            pass
        else:
            self.err(getattr(s, "loc", None), "not a statement: %r" % (s,))

    def elem_arg_alias(self, s, args):
        """`v[i].push(v[j].len())` borrows v mutably (IndexMut) while the
        argument reads it: E0502 in Rust.  A target that is a bare variable
        is fine (two-phase borrows)."""
        tgt = s.target.operand if isinstance(s.target, K.Borrow) else s.target
        if not isinstance(tgt, K.Index) and not isinstance(s, K.PushByte):
            return
        b = base_var(tgt)
        if b is not None and any(mentions(a, b) for a in args):
            self.err(s.loc, "unsupported: mutating an element of '%s' with an argument that reads '%s'" % (b, b))

    def compound(self, s, ty, scope):
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

    def foreach(self, s, scope):
        self.ident(s.var, s.loc)
        if s.mode not in K.FOREACH_MODES:
            self.err(s.loc, "bad range-for mode %r" % s.mode)
            return
        if not is_place(s.iterable):
            self.err(getattr(s.iterable, "loc", None) or s.loc,
                     "unsupported: range-based for over something other than a variable or element")
            return
        ty = self.place_ty(s.iterable, scope)
        if ty is None:
            return
        if not isinstance(ty, K.VecT):
            self.err(s.loc, "unsupported: range-based for over a %s" % ty)
            return
        self.wf(s.iterable, scope, "borrow")
        if s.mode == "mut":
            self.writable(s.iterable, scope, "mutable iteration")
        if s.mutable and s.mode != "copy":
            self.err(s.loc, "range-for reference variable '%s' declared mutable" % s.var)
        if mentions(s.iterable, s.var):
            self.err(s.loc, "range-for over a variable of the loop variable's name '%s'" % s.var)
        b = base_var(s.iterable)
        if b is not None:
            if s.mode == "mut" and mentions(s.body, b):
                self.err(s.loc, "unsupported: the body of a mutating range-for over '%s' uses '%s' "
                                "(iterator invalidation in C++, a second borrow in Rust)" % (b, b))
            elif b in mutated_bases(s.body, self.sigs):
                self.err(s.loc, "unsupported: the body of a range-for over '%s' modifies '%s' "
                                "(iterator invalidation in C++, a borrow conflict in Rust)" % (b, b))
            elif b in declared_names(s.body) or b == s.var:
                self.err(s.loc, "unsupported: range-for body shadows the iterated '%s'" % b)
        if s.mode == "copy":
            vt, vm = ty.elem, s.mutable
        else:
            vt, vm = K.RefT(ty.elem, s.mode == "mut"), False
        inner = scope.child()
        inner.declare(s.var, vt, vm)
        self.block(s.body, inner, True, same_scope=True)

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
                if isinstance(sg.arg, K.StrLit):
                    if any(not (1 <= ord(c) <= 0x7F) for c in sg.arg.value):
                        self.err(s.loc, "printf %s literal outside ASCII")
                    continue
                try:
                    st = type_of(sg.arg, scope, self.sigs)
                except KernelTypeError:
                    st = None
                if st != K.STRING:
                    self.err(s.loc, "printf %s argument must be a string literal")
                    continue
                self.expr(sg.arg, scope, K.STRING, "borrow")
                continue
            if (sg.kind == "d" and self.surface == "rust" and sg.width is None
                    and not isinstance(sg.arg, K.StrLit)):
                try:
                    if type_of(sg.arg, scope, self.sigs) == K.STRING:
                        # `{}` of a String: normalize makes it %s
                        self.expr(sg.arg, scope, K.STRING, "borrow")
                        continue
                except KernelTypeError:
                    pass
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
    def __init__(self, program, keep_surface=False):
        self.sigs = sigs_of(program)
        self.ret = None
        # keep_surface: ONLY infer empty-vector element types (N11), keep
        # every surface spelling -- run before the Rust surface check.
        self.keep = keep_surface

    def ty(self, e, scope):
        try:
            return type_of(e, scope, self.sigs)
        except KernelTypeError:
            return None

    def expr(self, e, scope, expect=None):
        if isinstance(e, K.Borrow):
            if self.keep:
                return K.Borrow(self.expr(e.operand, scope, expect), e.mutable, e.loc)
            return self.expr(e.operand, scope, expect)  # N7
        if isinstance(e, K.Unary):
            return K.Unary(e.op, self.expr(e.operand, scope), e.loc, e.spelling)
        if isinstance(e, K.Binary):
            lhs = self.expr(e.lhs, scope)
            return K.Binary(e.op, lhs, self.expr(e.rhs, scope, self.ty(lhs, scope)), e.loc, e.spelling)
        if isinstance(e, K.Call):
            sig = self.sigs.get(e.name)
            ptys = sig[0] if sig and len(sig[0]) == len(e.args) else (None,) * len(e.args)
            return K.Call(e.name, tuple(self.expr(a, scope, None if pt is None else K.deref(pt))
                                        for pt, a in zip(ptys, e.args)), e.loc)
        if isinstance(e, K.Clone):
            return K.Clone(self.expr(e.operand, scope), e.loc)
        if isinstance(e, K.Index):
            return K.Index(self.expr(e.base, scope), self.expr(e.index, scope), e.loc)
        if isinstance(e, K.ByteAt):
            return K.ByteAt(self.expr(e.base, scope), self.expr(e.index, scope), e.loc)
        if isinstance(e, K.Len):
            return K.Len(self.expr(e.base, scope), e.loc)
        if isinstance(e, K.IsEmpty):
            return K.IsEmpty(self.expr(e.base, scope), e.loc)
        if isinstance(e, K.VecLit):
            elem = e.elem
            if elem is None and isinstance(expect, K.VecT):
                elem = expect.elem  # N11
            elems = tuple(self.copied(x, scope, elem) for x in e.elems)
            if elem is None and elems:
                elem = self.ty(elems[0], scope)  # N11
            return K.VecLit(elem, elems, e.loc)
        if isinstance(e, K.VecFill):
            el = expect.elem if isinstance(expect, K.VecT) else None
            return K.VecFill(self.copied(e.value, scope, el), self.expr(e.count, scope), e.loc)
        if isinstance(e, K.Substr):
            return K.Substr(self.expr(e.base, scope), self.expr(e.pos, scope),
                            None if e.count is None else self.expr(e.count, scope), e.loc)
        if isinstance(e, K.ToStr):
            return K.ToStr(self.expr(e.operand, scope), e.loc)
        if isinstance(e, K.Field):
            return K.Field(self.expr(e.base, scope), e.name, e.loc)
        if isinstance(e, K.StructLit):
            sd = structs_of(self.sigs).get(e.path)
            ftys = [f.ty for f in sd.fields] if sd is not None and len(sd.fields) == len(e.values) \
                else [None] * len(e.values)
            return K.StructLit(e.path, tuple(self.expr(v, scope, t) for t, v in zip(ftys, e.values)), e.loc,
                               e.names)
        if isinstance(e, K.MethodCall):
            recv = self.expr(e.recv, scope)
            path, sig = call_sig(K.MethodCall(recv, e.name, e.args), scope, self.sigs)
            ptys = sig[0] if sig and len(sig[0]) == len(e.args) else (None,) * len(e.args)
            owner = path.rsplit("::", 1)[0] if path else ""
            return K.MethodCall(recv, e.name, tuple(self.expr(a, scope, None if pt is None else K.deref(pt))
                                                    for pt, a in zip(ptys, e.args)), e.loc, owner)
        if isinstance(e, K.Cast):
            inner = self.expr(e.operand, scope)
            T = e.ty
            if self.keep:
                return K.Cast(T, inner, e.loc)
            t = self.ty(inner, scope)
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

    def copied(self, e, scope, expect):
        """A COPIED position: a Clone there is the copy C++ makes anyway."""
        e = self.expr(e, scope, expect)
        if isinstance(e, K.Clone) and not self.keep:
            return e.operand  # N8
        return e

    def block(self, b, scope, same=False):
        inner = scope if same else scope.child()
        out = [self.stmt(s, inner) for s in b.stmts]
        return K.Block(tuple(out), b.loc)

    def var_ty(self, scope, name):
        v = scope.lookup(name)
        return None if v is None else K.deref(v[0])

    def stmt(self, s, scope):
        if isinstance(s, K.Block):
            return self.block(s, scope)
        if isinstance(s, K.Let):
            init = self.expr(s.init, scope, s.ty)
            scope.declare(s.name, s.ty, s.mutable)
            return K.Let(s.name, s.ty, s.mutable, init, s.loc)
        if isinstance(s, K.Assign):
            t = self.var_ty(scope, s.name)
            v = self.expr(s.value, scope, t)
            if (isinstance(v, K.Binary) and v.op in K.COMPOUND_OPS and isinstance(v.lhs, K.Var)
                    and v.lhs.name == s.name and isinstance(t, K.Prim) and not self.keep):
                return K.CompoundAssign(v.op, s.name, v.rhs, s.loc, v.spelling)  # N5
            return K.Assign(s.name, v, s.loc)
        if isinstance(s, K.CompoundAssign):
            return K.CompoundAssign(s.op, s.name, self.expr(s.value, scope), s.loc, s.spelling)
        if isinstance(s, K.Store):
            tgt = self.expr(s.target, scope)
            t = self.ty(tgt, scope)
            v = self.expr(s.value, scope, t)
            if (isinstance(v, K.Binary) and v.op in K.COMPOUND_OPS and v.lhs == tgt and isinstance(t, K.Prim)
                    and not self.keep and not any(isinstance(n, K.Call) for n in K.walk(tgt))):
                return K.CompoundStore(v.op, tgt, v.rhs, s.loc, v.spelling)  # N5 on an element
            return K.Store(tgt, v, s.loc)
        if isinstance(s, K.CompoundStore):
            return K.CompoundStore(s.op, self.expr(s.target, scope), self.expr(s.value, scope), s.loc, s.spelling)
        if isinstance(s, K.Push):
            tgt = self.expr(s.target, scope)
            t = self.ty(tgt, scope)
            return K.Push(tgt, self.expr(s.value, scope, t.elem if isinstance(t, K.VecT) else None), s.loc)
        if isinstance(s, K.Pop):
            return K.Pop(self.expr(s.target, scope), s.loc)
        if isinstance(s, K.Clear):
            return K.Clear(self.expr(s.target, scope), s.loc)
        if isinstance(s, K.Resize):
            tgt = self.expr(s.target, scope)
            t = self.ty(tgt, scope)
            return K.Resize(tgt, self.expr(s.count, scope),
                            self.copied(s.value, scope, t.elem if isinstance(t, K.VecT) else None), s.loc)
        if isinstance(s, K.PushStr):
            return K.PushStr(self.expr(s.target, scope), self.expr(s.value, scope), s.loc)
        if isinstance(s, K.PushByte):
            return K.PushByte(self.expr(s.target, scope), self.expr(s.value, scope), s.loc)
        if isinstance(s, K.ExprStmt):
            return K.ExprStmt(self.expr(s.expr, scope), s.loc)
        if isinstance(s, K.Print):
            segs = []
            for sg in s.segs:
                if isinstance(sg, K.Text):
                    if not sg.text and not self.keep:
                        continue
                    if segs and isinstance(segs[-1], K.Text) and not self.keep:
                        segs[-1] = K.Text(segs[-1].text + sg.text)  # N6
                    else:
                        segs.append(sg)
                else:
                    a = self.expr(sg.arg, scope)
                    kind = sg.kind
                    if (kind == "d" and sg.width is None and not sg.flag and not self.keep
                            and self.ty(a, scope) == K.STRING):
                        kind = "s"  # N10: `{}` of a String is %s
                    segs.append(K.Spec(kind, a, sg.flag, sg.width, sg.prec))
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
            t = self.ty(lo, scope)
            inner.declare(s.var, t, False)
            return K.For(s.var, lo, hi, self.block(s.body, inner, same=True), s.loc)
        if isinstance(s, K.ForEach):
            it = self.expr(s.iterable, scope)
            t = self.ty(it, scope)
            el = t.elem if isinstance(t, K.VecT) else None
            inner = scope.child()
            if s.mode == "copy":
                inner.declare(s.var, el, s.mutable)
            else:
                inner.declare(s.var, K.RefT(el, s.mode == "mut"), False)
            return K.ForEach(s.var, s.mutable, s.mode, it, self.block(s.body, inner, same=True), s.loc)
        if isinstance(s, K.Return):
            if s.value is None:
                return K.Return(None, s.loc)
            v = self.expr(s.value, scope, self.ret)
            if isinstance(v, K.Clone) and isinstance(v.operand, K.Var) and not self.keep:
                d = scope.lookup(v.operand.name)
                if d is not None and not isinstance(d[0], K.RefT) and d[1]:
                    v = v.operand  # N9: C++ `return x;` of a local moves
            elif isinstance(v, K.Var):
                d = scope.lookup(v.name)
                if d is not None and K.is_owned_nc(d[0]) and not d[1]:
                    v = K.Clone(v, v.loc)  # N12: `return x;` of a const local copies
            return K.Return(v, s.loc)
        return s

    def func(self, f):
        scope = Scope()
        if f.recv in ("ref", "mut"):
            scope.declare("self", K.RefT(K.StructT(K.path_of(f.ns, f.owner)), f.recv == "mut"), False)
        for p in f.params:
            scope.declare(p.name, p.ty, p.mutable)
        self.ret = f.ret
        internal = f.internal and bool(f.ns) and not f.owner  # N14
        return K.Func(f.name, f.params, f.ret, self.block(f.body, scope, same=True), f.loc, f.ns, internal,
                      f.owner, f.recv)

    def struct(self, sd):
        return K.StructDef(sd.name, sd.fields, tuple(self.func(m) for m in sd.methods), sd.loc, sd.ns,
                           sd.internal and bool(sd.ns))  # N14


def struct_order(structs):
    """N15: the canonical order of struct definitions -- a topological
    order of the BY-VALUE containment graph (C++ needs a data member's type
    complete), choosing the smallest absolute path among the ready ones.
    It depends only on the SET of structs, so neither language's source
    order (C++ must define before use; Rust does not care) survives."""
    by = {sd.path: sd for sd in structs}
    deps = {p: {f.ty.path for f in sd.fields if isinstance(f.ty, K.StructT) and f.ty.path in by and f.ty.path != p}
            for p, sd in by.items()}
    out, done = [], set()
    while len(done) < len(by):
        ready = sorted(p for p in by if p not in done and deps[p] <= done)
        if not ready:  # a by-value cycle: check() reports it; keep a total order
            ready = sorted(p for p in by if p not in done)
        out.append(by[ready[0]])
        done.add(ready[0])
    # duplicates (reported by check) keep their relative order after the first
    seen = set(id(x) for x in out)
    return out + [sd for sd in structs if id(sd) not in seen]


def normalize(program, keep_surface=False):
    n = _Normalizer(program, keep_surface)
    items = tuple(n.func(i) if isinstance(i, K.Func) else n.struct(i) if isinstance(i, K.StructDef) else i
                  for i in program.items)
    if not keep_surface:
        structs = struct_order([i for i in items if isinstance(i, K.StructDef)])
        rest = [i for i in items if not isinstance(i, K.StructDef)]
        items = tree_order(tuple(structs) + tuple(rest))  # N15, then N13
    return K.Program(items)
