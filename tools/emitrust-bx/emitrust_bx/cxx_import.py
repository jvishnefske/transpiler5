"""clang JSON AST -> Surface IR (Kernel + Residual nodes) (FR-250).

Measured facts this module is built on (FR-249 spike, Clang 21.1.8):

* clang++ is invoked DIRECTLY (never through `nix develop -c`, whose
  shellHook banner on stdout would corrupt the JSON).
* The whole TU is dumped and filtered here: a location object prints
  `file` only when it CHANGES from the previously printed location, so a
  pre-order walk over every location in print order tracks the current
  file; a top-level decl belongs to the main file iff its own `loc` does.
* `DeclRefExpr.referencedDecl` carries a name but no namespace, so a
  library function (printf) is resolved by decl id against the decls
  indexed during the walk, and must live OUTSIDE the main file (or be a
  body-less `extern "C"` redeclaration of printf in it).
* Implicit nodes are seen through (LValueToRValue, NoOp, ParenExpr,
  ExprWithCleanups); arithmetic conversions become explicit Kernel Casts;
  int/float -> bool conversions become `x != 0`.
* Line/column are computed from byte offsets against the source text.
* `unsigned long` IS `std::size_t` on LP64 and clang drops the typedef
  sugar on arithmetic results, so it maps to usize; u64 is
  `unsigned long long`.

Unsupported constructs do not stop the import: each becomes a located
Residual node and the walk continues, so every blocker is counted.

FR-251 (std::vector / std::string), measured on the same clang:

* COPY vs MOVE is read from the AST, never guessed: a CXXConstructExpr
  whose ctorType is `void (const vector<int> &)` is a copy (Kernel Clone),
  `void (vector<int> &&) noexcept` a move; `return v;` of a local is the
  move ctor over an xvalue NoOp (C++17 implicit move).  Overloaded
  operators (`=`, `push_back`, `+`) pick copy vs move by the argument's
  value category: an lvalue binds `const T&` (copy), `std::move(x)` (a
  CallExpr to std's builtin `move`) or a temporary binds `T&&`.
* types arrive in many spellings (`std::string`, `basic_string<char>`,
  `basic_string<char, std::char_traits<char>, std::allocator<char>>`,
  `value_type` desugaring to `int`, `std::vector<int, std::allocator<int>>`);
  ``parse_cxx_type`` reads all of them.
* `std::vector<T> v;` is init "call" with a zero-argument CXXConstructExpr;
  `= {..}` an initializer_list ctor over CXXStdInitializerListExpr /
  InitListExpr; `(n, x)` the (size_type, const value_type &) ctor.
* CXXForRangeStmt carries the desugared __range/__begin/__end decls; only
  the range initializer and the loop VarDecl are read.
* import-side desugarings (C++ spellings, not Kernel normal forms):
  `v.front()` -> `v[0]`, `v.back()` -> `v[v.size() - 1]` (both UB on an
  empty vector; the Rust index panics), `s.length()` -> `s.size()`,
  `s += c` (char) -> `s.push_back(c)`, `"lit" + s` -> `std::string("lit") + s`.

FR-260 (namespaces), measured on the same clang:

* a NamespaceDecl nests its members; C++17 `namespace a::b {` is a NamespaceDecl
  `a` holding `b` with `isNested`; an anonymous namespace has no name and is
  followed by an implicit UsingDirectiveDecl; an out-of-line `int a::f() {..}`
  is a top-level FunctionDecl whose `parentDeclContextId` is namespace a;
  a definition redeclaring a header decl carries `previousDecl`.
* a DeclRefExpr's `referencedDecl` id names the function whatever the
  spelling (`f`, `a::f`, `::a::f`, via `using`, via an alias), so every call
  becomes an ABSOLUTE Kernel path and using-directives, using-declarations
  of user functions and aliases of this file's namespaces are erased
  (spelling only).  `using std::string;` & co keep FR-251's residual.

FR-257 (structs), measured on the same clang:

* a struct/class definition is a CXXRecordDecl with `completeDefinition`;
  members carry no access key (AccessSpecDecl entries switch it; `class`
  starts private); implicit special members are `isImplicit`.  A FieldDecl
  flags `isBitfield`, `hasInClassInitializer`, `mutable`.
* a MemberExpr names its member by `referencedMemberDecl` id; implicit
  member access is a MemberExpr with `isArrow` over an implicit CXXThisExpr.
* type spellings: `desugaredQualType` is the CANONICAL, fully qualified one
  (`a::Q`) and is read first; a reference's pointee and a function type's
  return keep only the WRITTEN spelling (`const Q &` inside namespace a),
  resolved here with C++'s innermost-namespace-first lookup.
* an aggregate `S{a, b}` is a NoOp CXXFunctionalCastExpr over an
  InitListExpr (a CXXBindTemporaryExpr in between when S has a non-trivial
  destructor); an omitted scalar member is an ImplicitValueInitExpr.
* a call of the user constructor is a CXXConstructExpr whose `ctorType` is
  the constructor's own type string; the implicit copy constructor's type
  may end in `noexcept(false)` (a struct holding a vector of itself), so
  the parameter list is the FIRST parenthesized group.
* constructor initializers are listed in initialization (= declaration)
  order whatever order they are written in.

FR-261 (pointers), measured on the same clang:

* a pointer type has NO desugared spelling (`const uint8_t *`, `size_t *`,
  `S *`, `int *const`), so its pointee is parsed here, the common
  <cstdint>/<cstddef> names through POINTEE_TYPEDEFS; `T**`, `void*`,
  function pointers and a reference to a pointer stay outside.
* `p[i]` is an ArraySubscriptExpr whose base is LValueToRValue of the
  pointer and whose index is NOT converted (an `int` literal stays `int`):
  normal form N19 makes it usize; `*p` a UnaryOperator `*`, `p->f` a
  MemberExpr with `isArrow` over the pointer (N16: all `p[0]`); `p + k` a
  BinaryOperator of pointer type; `p++` / `++p` / `p += k` act on the
  pointer local; `&v[i]` is `&` over an operator[] call (N17: the suffix).
* a `T*` passed for `const T*` is a NoOp ImplicitCastExpr (seen through);
  `nullptr` / `0` for a pointer is NullToPointer, `if (p)` PointerToBoolean.

FR-259, measured on the same clang:

* a REFERENCE's pointee (`const std::vector<uint8_t> &`) and a function
  type's return (`uint32_t (int)`) carry no desugared spelling either, so
  the <cstdint>/<cstddef> names are resolved in every written spelling
  (normal form N20) -- unless the TU declares one of those names as a
  typedef of a DIFFERENT type (anywhere: a header's namespace too) or as a
  class / enum, which turns the resolution off (the spellings keep their
  residual).  A TypedefType's nested `decl` reference has no `type` and is
  not a declaration.
* a free function's `inline` / `static` is the FunctionDecl's `inline` /
  `storageClass` (normal form N21).
* an `auto` VarDecl's `type` is the DEDUCED type (`size_t`, `const
  value_type` desugaring to `const unsigned char`, `int16_t &` for `auto&`),
  read like a written one (N22); a lambda's is `(lambda at FILE:L:C)`.
* `c ? a : b` is a ConditionalOperator [cond, then, else] whose arms carry
  the usual arithmetic conversions as ImplicitCastExprs (so the Kernel arms
  have one type) and whose condition is converted to bool; it is an LVALUE
  when both arms are lvalues of one type, read through LValueToRValue.
* `x++` / `++x` is a UnaryOperator `++` with `isPostfix`; used as a value it
  is not a statement of its own.

FR-252 (closures), measured on the same clang:

* `auto f = [..](..) {..};` is a VarDecl of type `(lambda at F:L:C)` over a
  LambdaExpr [implicit CXXRecordDecl, capture initializers.., body].  The
  record's FieldDecls are the captures in order (a `T &` field is by
  reference); its `operator()` is a CXXMethodDecl whose type lacks `const`
  iff `mutable` (a spelling here: N23's derived list decides) and reads
  `auto (int) const -> int` for a written trailing return type, `int (int)
  const` for a deduced one; a generic lambda's is a FunctionTemplateDecl.
  The body's DeclRefExprs name the ENCLOSING VarDecls by id, so captures
  resolve like any local.  Explicit vs default capture is NOT in the JSON:
  the capture list is read from the source text at the LambdaExpr's range.
* a call `f(x)` is a CXXOperatorCallExpr `operator()` whose first argument
  is the closure variable; `auto g = f;` / a temporary in place / an
  argument is any other node (refused).
* `template <typename F> R g(const F& f, ..)` is a FunctionTemplateDecl
  [TemplateTypeParmDecl, pattern FunctionDecl, instantiations..]; each
  instantiation names its lambda record through TemplateArgument ->
  RecordType.decl and has a fully typed body, which is what is imported.
"""

import bisect
import json
import os
import re
import subprocess

from . import fmt
from . import kernel as K
from . import normalize as NZ
from . import typecheck as TC


class ClangError(Exception):
    pass


def _split_targs(s):
    """'a, b<c, d>, e' -> ['a', 'b<c, d>', 'e'] (top-level commas)."""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur).strip())
    return out


_STRING_SPELLINGS = (
    "string", "basic_string<char>",
    "basic_string<char, char_traits<char>, allocator<char>>",
    "basic_string<char, char_traits<char>, allocator<char> >",
)


def resolve_typedefs(q):
    """N20: the <cstdint>/<cstddef> names in a written spelling -> their
    LP64 builtin type (also inside template arguments)."""
    return _INT_TYPEDEF_RE.sub(lambda m: INT_TYPEDEFS.get(m.group(0), m.group(0)), q)


def parse_cxx_type(q, struct=None, typedefs=True):
    """A clang type spelling -> (Kernel type or None, top-level const).

    References become RefT; `const` under a reference makes it shared.
    FR-257: ``struct(name)`` resolves a record name to a StructT (or None).
    FR-259: with ``typedefs`` the <cstdint>/<cstddef> names are resolved
    (normal form N20).
    """
    if q is None:
        return None, False
    q = q.strip()
    if typedefs:
        q = resolve_typedefs(q)
    if q.endswith("&&"):
        return None, False
    if q.endswith("&"):
        inner, const = parse_cxx_type(q[:-1], struct, typedefs)
        if inner is None or isinstance(inner, (K.RefT, K.PtrT)):
            return None, False
        return K.RefT(inner, not const), False
    if is_ptr_spelling(q):
        # FR-261: `const T *` <=> &[T], `T *` <=> &mut [T]; `T *const` is
        # top-level const.  T** / void* / a function pointer stay outside.
        top = False
        if not q.endswith("*"):
            q = q[:-len("const")].rstrip()
            top = True
        body = q[:-1].strip()
        if body.endswith("*") or "(" in body or "&" in body:
            return None, top
        bconst = body.startswith("const ")
        bare = body[len("const "):].strip() if bconst else body
        if bare.endswith(" const"):
            bconst, bare = True, bare[:-len(" const")].strip()
        inner, iconst = parse_cxx_type(bare, struct, typedefs)
        if inner is None or inner == K.UNIT or isinstance(inner, (K.RefT, K.PtrT)) or "volatile" in body:
            return None, top
        return K.PtrT(inner, not (bconst or iconst)), top
    const = False
    if q.startswith("const "):
        const = True
        q = q[len("const "):].strip()
    elif q.endswith(" const"):
        # FR-259: clang spells a DEDUCED (`auto`) const type east-const
        # (`unsigned char const &`, `P const &`)
        const = True
        q = q[:-len(" const")].strip()
    if "volatile" in q:
        return None, const
    if q in TYPE_MAP:
        return TYPE_MAP[q], const
    plain = q.replace("std::", "").replace("__cxx11::", "")
    if plain in _STRING_SPELLINGS:
        return K.STRING, const
    if plain.startswith("vector<") and plain.endswith(">"):
        args = _split_targs(plain[len("vector<"):-1])
        if len(args) == 2 and args[1].replace(" ", "") != ("allocator<%s>" % args[0]).replace(" ", ""):
            return None, const
        if len(args) not in (1, 2):
            return None, const
        el, econst = parse_cxx_type(args[0], struct, typedefs)
        if el is None or econst or isinstance(el, K.RefT):
            return None, const
        return K.VecT(el), const
    if struct is not None:
        name = q[len("struct "):] if q.startswith("struct ") else q[len("class "):] if q.startswith("class ") else q
        st = struct(name.strip())
        if st is not None:
            return st, const
    return None, const


def is_ptr_spelling(q):
    """Is the clang spelling ``q`` a pointer type (`T *`, `T *const`)?"""
    q = (q or "").strip()
    return q.endswith("*") or q.replace(" ", "").endswith("*const")


# FR-261: a pointer type has no desugared spelling in clang's JSON
# (`const uint8_t *`), so the <cstdint>/<cstddef> names a pointee is
# commonly written with are resolved here, to the type their scalar
# desugaring maps to on LP64 (uint64_t is `unsigned long`, like size_t).
# FR-259 (N20): neither has a reference's pointee nor a function's return
# type, so every written spelling is resolved; int8_t / int16_t / uint16_t /
# ptrdiff_t joined.
INT_TYPEDEFS = {
    "int8_t": "signed char", "uint8_t": "unsigned char", "int16_t": "short", "uint16_t": "unsigned short",
    "int32_t": "int", "uint32_t": "unsigned int", "int64_t": "long", "uint64_t": "unsigned long",
    "size_t": "unsigned long", "ptrdiff_t": "long",
}
INT_TYPEDEFS.update({"std::" + k: v for k, v in list(INT_TYPEDEFS.items())})
POINTEE_TYPEDEFS = INT_TYPEDEFS  # (FR-261's name)
# (also inside template arguments: `std::vector<uint8_t> *`)
_INT_TYPEDEF_RE = re.compile(r"(?<![\w:])(?:std::)?(?:u?int(?:8|16|32|64)_t|size_t|ptrdiff_t)\b")
_POINTEE_TYPEDEF_RE = _INT_TYPEDEF_RE


def run_clang(path, clang="clang++"):
    cmd = [clang, "-std=c++17", "-fsyntax-only", "-Xclang", "-ast-dump=json", path]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as ex:
        raise ClangError("cannot run %s: %s" % (clang, ex))
    if p.returncode != 0:
        raise ClangError(p.stderr.decode("utf-8", "replace"))
    return json.loads(p.stdout)


TYPE_MAP = {
    "int": K.I32, "long": K.I64, "long long": K.I64, "unsigned int": K.U32,
    "unsigned long": K.USIZE, "unsigned long long": K.U64, "size_t": K.USIZE,
    "std::size_t": K.USIZE, "double": K.F64, "bool": K.BOOL, "char": K.I8,
    "unsigned char": K.U8, "void": K.UNIT,
    # FR-259: `signed char` is int8_t's type (N20: canonical `char`)
    "signed char": K.I8, "short": K.I16, "unsigned short": K.U16,
}

OPCODES = {
    "+": "add", "-": "sub", "*": "mul", "/": "div", "%": "rem", "&": "band", "|": "bor",
    "^": "bxor", "<<": "shl", ">>": "shr", "==": "eq", "!=": "ne", "<": "lt", "<=": "le",
    ">": "gt", ">=": "ge", "&&": "and", "||": "or",
}
COMPOUND_OPCODES = {k + "=": v for k, v in OPCODES.items() if v in K.COMPOUND_OPS}

FRIENDLY = {
    "ArraySubscriptExpr": "array subscript", "MemberExpr": "member access",
    "CXXMemberCallExpr": "member function call", "LambdaExpr": "lambda expression",
    "CXXNewExpr": "new-expression", "CXXDeleteExpr": "delete-expression",
    "UnaryExprOrTypeTraitExpr": "sizeof/alignof", "ConditionalOperator": "conditional operator `?:`",
    "CXXThisExpr": "this", "CXXNullPtrLiteralExpr": "nullptr", "InitListExpr": "initializer list",
    "CXXConstructExpr": "object construction", "CXXOperatorCallExpr": "overloaded operator call",
    "StringLiteral": "string literal outside a printf argument",
    "CXXThrowExpr": "throw-expression", "CXXDefaultArgExpr": "default argument",
    "SwitchStmt": "switch statement", "DoStmt": "do-while loop", "GotoStmt": "goto",
    "LabelStmt": "label", "CXXTryStmt": "try block", "CXXForRangeStmt": "range-based for loop",
    "VarDecl": "global variable", "CXXRecordDecl": "class/struct definition",
    "NamespaceDecl": "namespace", "FunctionTemplateDecl": "function template",
    "ClassTemplateDecl": "class template", "TypedefDecl": "typedef",
    "TypeAliasDecl": "type alias", "EnumDecl": "enum", "UsingDecl": "using-declaration",
    "UsingDirectiveDecl": "using-directive", "NamespaceAliasDecl": "namespace alias",
    "StaticAssertDecl": "static_assert", "CXXMethodDecl": "member function",
    "BinaryConditionalOperator": "conditional operator `?:`",
    "CompoundLiteralExpr": "compound literal", "StmtExpr": "statement expression",
    "CXXReinterpretCastExpr": "reinterpret_cast", "CXXConstCastExpr": "const_cast",
}

FOR_BAD = "`for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`"

# FR-261 refusals (each names the pointer SHAPE; see the module docstring)
R3_SIGNED = ("signed pointer offset (C++ allows a negative offset into the middle of an array; a Rust suffix "
             "slice cannot go backwards)")
R3_MINUS = "pointer arithmetic `p - k` (a Rust suffix slice cannot go backwards)"
R3_DEC = "pointer decrement (a Rust suffix slice cannot go backwards)"
R4_CMP = ("pointer comparison (Rust compares slices by contents, and suffixes of distinct objects can have equal "
          "lengths)")
R4_DIFF = "pointer difference (a Rust slice has no address to subtract)"
R6_NULL = "null pointer (the subset's pointers always point into an object)"


def friendly(kind):
    return FRIENDLY.get(kind, kind)


class _Loc:
    """Byte-offset -> (line, col) for the main file."""

    def __init__(self, data):
        self.starts = [0]
        for i, b in enumerate(data):
            if b == 0x0A:
                self.starts.append(i + 1)

    def at(self, off):
        if off is None:
            return None
        i = bisect.bisect_right(self.starts, off) - 1
        return K.Loc(i + 1, off - self.starts[i] + 1)


def _off(locobj):
    if not isinstance(locobj, dict):
        return None
    if "expansionLoc" in locobj:
        return locobj["expansionLoc"].get("offset")
    return locobj.get("offset")


class _FileTracker:
    """Replays clang's print order to know each location's file."""

    def __init__(self):
        self.cur = None

    def loc(self, lo):
        if not isinstance(lo, dict):
            return
        if "spellingLoc" in lo or "expansionLoc" in lo:
            self.loc(lo.get("spellingLoc"))
            self.loc(lo.get("expansionLoc"))
            return
        f = lo.get("file")
        if f is not None:
            self.cur = f

    def node(self, n, visit=None):
        """Walk ``n`` in print order; ``visit(node, file)`` for decls."""
        stack = [n]
        while stack:
            x = stack.pop()
            if isinstance(x, list):
                stack.extend(reversed(x))
                continue
            if not isinstance(x, dict):
                continue
            if "loc" in x and isinstance(x["loc"], dict):
                self.loc(x["loc"])
            if "range" in x and isinstance(x["range"], dict):
                self.loc(x["range"].get("begin"))
                self.loc(x["range"].get("end"))
            if visit is not None and "id" in x and "kind" in x:
                visit(x, self.cur)
            kids = []
            for k, v in x.items():
                if k in ("loc", "range", "type", "referencedDecl", "foundReferencedDecl"):
                    continue
                if isinstance(v, (list, dict)):
                    kids.append(v)
            stack.extend(reversed(kids))


class CxxImporter:
    def __init__(self, path, ast, source):
        self.path = path
        self.real = os.path.realpath(path)
        self.ast = ast
        self.source = source
        self.lines = _Loc(source)
        self.realcache = {}
        self.decl_main = {}  # decl id -> bool (declared in main file)
        self.decl_info = {}  # decl id -> node
        self.libc_printf = set()
        self.user_funcs = {}  # decl id -> name
        self.locals = {}  # decl id -> name
        self.poisoned = set()  # ids of local decls that became residuals
        # FR-257 structs
        self.records = []  # (node, ns, internal) main-file struct definitions
        self.bad_records = set()  # ids of records with a residual AT the class
        self.record_path = {}  # record decl id -> absolute path
        self.struct_canon = {}  # canonical clang spelling -> path
        self.struct_paths = set()
        self.field_ids = {}  # FieldDecl id -> (struct path, name)
        self.method_ids = {}  # CXXMethodDecl/ctor id (decl and definition) -> path
        self.ctor_of = {}  # struct path -> ctor decl node
        self.defs = {}  # in-class method decl id -> out-of-line definition node
        self.ctx_ns = ()
        self.has_using = False
        self.ambiguous = set()
        self.cur = None  # (owner path, recv kind) while importing a member function
        self.cpath = []  # clang's spelling of the enclosing namespaces
        self.record_cpath = {}
        self.ctor_types = {}  # struct path -> its user constructor's type string
        self.nonneg_ids = set()  # FR-261: decl ids of counting-loop variables with a literal lower bound
        self.td_ok = True  # FR-259 (N20): may the <cstdint> names be resolved in written spellings?
        self.in_closure = None  # FR-252: the closure whose body is being imported
        self.fn_params = {}  # FR-252: template instantiation id -> (param index, RefT(FnT))
        self.bad_funcs = set()  # FR-252: decl ids of rejected function templates (pattern + instances)
        self._std = None  # FR-253: ids of the functions declared directly in namespace std
        self.normalize = False  # FR-254: apply normalize.RULES instead of refusing their patterns
        self.notes = []  # FR-254: (loc, "normalized: <rule>") per applied rule

    # -------------------------------------------------------------- utils
    def is_main(self, f):
        if f is None:
            return False
        r = self.realcache.get(f)
        if r is None:
            r = self.realcache[f] = os.path.realpath(f)
        return r == self.real

    def loc(self, n):
        if not isinstance(n, dict):
            return None
        rg = n.get("range")
        off = None
        if isinstance(rg, dict):
            off = _off(rg.get("begin"))
        if off is None:
            off = _off(n.get("loc"))
        return self.lines.at(off)

    def text_at(self, n, k=40):
        rg = n.get("range")
        off = _off(rg.get("begin")) if isinstance(rg, dict) else None
        if off is None:
            return ""
        return self.source[off:off + k].decode("utf-8", "replace")

    def rexpr(self, n, reason):
        return K.ResidualExpr(reason, self.loc(n))

    def rstmt(self, n, reason):
        return K.ResidualStmt(reason, self.loc(n))

    @staticmethod
    def qual(t):
        if not isinstance(t, dict):
            return None
        return t.get("qualType")

    def map_type(self, t):
        """(Kernel type or None, is_const, spelling)."""
        q = self.qual(t) or ""
        d0 = t.get("desugaredQualType") if isinstance(t, dict) else None
        if is_ptr_spelling(q) or is_ptr_spelling(d0):
            # FR-261: the pointee's constness is not top-level const
            kt, top = (None, False)
            if d0 and is_ptr_spelling(d0):
                kt, top = parse_cxx_type(d0, self.struct_canonical, self.td_ok)
            if kt is None:
                kt, top = parse_cxx_type(q, self.struct_written, self.td_ok)
            return kt, top, q
        const = False
        s = q
        if s.startswith("const "):
            const = True
            s = s[len("const "):]
        if s in TYPE_MAP:
            return TYPE_MAP[s], const, q
        d = t.get("desugaredQualType") if isinstance(t, dict) else None
        if d:
            if d.startswith("const "):
                const = True
                d = d[len("const "):]
            if d in TYPE_MAP and "volatile" not in q:
                return TYPE_MAP[d], const, q
        # FR-251: vectors, strings, references.  FR-257: the DESUGARED
        # spelling is canonical (fully qualified `a::Q`) and is read first;
        # the written one (`Q` inside namespace a) only when clang printed
        # no desugared form (a reference's pointee, a function type).
        kt, c2 = (None, False)
        if d:
            kt, c2 = parse_cxx_type(d, self.struct_canonical, self.td_ok)
        if kt is None:
            kt, c3 = parse_cxx_type(q, self.struct_written, self.td_ok)
            c2 = c2 or c3
        if kt is not None:
            return kt, const or c2, q
        return None, const, q

    # -------------------------------------------------------------- FR-257
    def struct_canonical(self, name):
        p = self.struct_canon.get(name)
        return K.StructT(p) if p is not None else None

    def struct_written(self, name):
        """A record name as WRITTEN in namespace ``self.ctx_ns``: C++
        unqualified/qualified lookup searches the innermost enclosing
        namespace first (a type can hide only behind a member, and no
        member may be named like a struct).  A using-directive could make a
        farther struct win, so with one in the file an ambiguous name is
        refused rather than guessed."""
        if name in self.struct_canon and "(anonymous namespace)" in name:
            return K.StructT(self.struct_canon[name])
        name = name[2:] if name.startswith("::") else name
        ns = self.ctx_ns
        cands = []
        for i in range(len(ns), -1, -1):
            p = K.path_of(ns[:i], name) if ns[:i] else name
            if p in self.struct_paths:
                cands.append(p)
        if not cands:
            return None
        if len(set(cands)) > 1 and self.has_using:
            self.ambiguous.add(name)
            return None
        return K.StructT(cands[0])

    def ty_of(self, e):
        try:
            return TC.type_of(e, self.scope, self.sigs)
        except TC.KernelTypeError:
            return None

    # -------------------------------------------------------------- top
    def run(self):
        top = self.ast.get("inner", [])
        trk = _FileTracker()
        mains = []
        for n in top:
            trk.loc(n.get("loc"))
            in_main = self.is_main(trk.cur)
            # finish the walk of this decl (keeps the file state exact)
            rest = {k: v for k, v in n.items() if k != "loc"}

            def visit(x, f, in_main=in_main):
                if x.get("kind", "").endswith("Decl"):
                    self.decl_main[x["id"]] = in_main
                    self.decl_info[x["id"]] = x
                    if not in_main and x.get("kind") == "FunctionDecl" and x.get("name") == "printf":
                        self.libc_printf.add(x["id"])
                    if x.get("name") in INT_TYPEDEFS:
                        self.typedef_seen(x)

            visit(n, trk.cur)
            trk.node(rest, visit)
            if in_main and not n.get("isImplicit"):
                mains.append(n)
        # FR-260: every namespace decl (any file) -> (path, internal)
        self.ns_index = {}
        for n in top:
            self.index_ns(n, (), False)
        items = []
        funcs = []
        self.out_of_line = []
        for n in mains:
            self.collect(n, (), False, items, funcs)
        # FR-257: index the structs BEFORE any type is read
        for n, ns, internal in self.records:
            path = K.path_of(ns, n["name"])
            self.record_path[n["id"]] = path
            self.struct_paths.add(path)
            canon = "::".join(self.record_cpath[n["id"]] + [n["name"]])
            self.struct_canon[canon] = path
        # a main-file function redeclaring an earlier (e.g. header) decl:
        # a reference may name either decl id
        for n, ns, internal in funcs:
            prev = n.get("previousDecl")
            if prev and prev not in self.user_funcs:
                self.user_funcs[prev] = K.path_of(ns, n.get("name"))
        # signatures (for typing calls)
        self.sigs = TC.Sigs()
        defined = set()
        for n, ns, internal in funcs:
            if self.has_body(n):
                self.ctx_ns = ns
                ptys, ret = self.signature_types(n)
                path = K.path_of(ns, n.get("name"))
                if path != "main":
                    self.sigs.setdefault(path, (ptys, ret))
                defined.add(path)
        structs = [self.struct_shell(n, ns, internal, items) for n, ns, internal in self.records]
        for n in self.out_of_line:
            items.extend(self.attach_definition(n))
        for n, ns, internal in funcs:
            if not self.has_body(n):
                if K.path_of(ns, n.get("name")) not in defined:
                    items.append(K.ResidualItem("function declared but not defined", self.loc(n)))
                continue
            items.append(self.func(n, ns, internal))
        for sd in structs:
            items.append(self.struct_def(sd, items))
        # keep source order
        items.sort(key=lambda it: (it.loc.line, it.loc.col) if it.loc else (0, 0))
        return K.Program(tuple(items))

    def typedef_seen(self, x):
        """FR-259 (N20): a declaration named like a <cstdint>/<cstddef> type.
        The real ones (global, std) are typedefs of the expected builtin; a
        typedef of any other type, or a class/enum of that name, may be what
        a written spelling means, so the resolution is turned off."""
        k = x.get("kind")
        if k in ("TypedefDecl", "TypeAliasDecl"):
            if "type" not in x:
                return  # a reference to a declaration (a TypedefType's `decl`), not one
            t = x.get("type") or {}
            d = t.get("desugaredQualType") or t.get("qualType")
            if d != INT_TYPEDEFS[x["name"]]:
                self.td_ok = False
        elif k in ("CXXRecordDecl", "RecordDecl", "EnumDecl", "ClassTemplateDecl", "TypeAliasTemplateDecl"):
            self.td_ok = False

    # -------------------------------------------------------------- FR-260
    def index_ns(self, n, path, internal):
        if not isinstance(n, dict) or n.get("kind") != "NamespaceDecl":
            return
        name = n.get("name") or ""
        if name:
            path = path + (name,)
        else:
            internal = True
        self.ns_index[n["id"]] = (path, internal)
        for c in n.get("inner", []):
            self.index_ns(c, path, internal)

    def collect(self, n, ns, internal, items, funcs):
        """One main-file declaration at namespace scope ``ns``."""
        if n.get("isImplicit"):
            return
        k = n.get("kind")
        if k == "FunctionDecl":
            pid = n.get("parentDeclContextId")
            if pid in self.ns_index:  # an out-of-line `int a::f() {..}`
                ns, internal = self.ns_index[pid]
            funcs.append((n, ns, internal))
            self.user_funcs[n["id"]] = K.path_of(ns, n.get("name"))
        elif k == "CXXRecordDecl":
            self.collect_record(n, ns, internal, items)
        elif k in ("CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl"):
            # an out-of-line member definition; matched to its class once
            # every class is known (an unknown class keeps the old residual)
            self.out_of_line.append(n)
        elif k == "NamespaceDecl":
            name = n.get("name") or ""
            if n.get("isInline"):
                items.append(K.ResidualItem("inline namespace", self.loc(n)))
                return
            if name and internal:
                items.append(K.ResidualItem("named namespace inside an anonymous namespace", self.loc(n)))
                return
            if name:
                p = TC.ident_problem(name) or (
                    "identifier '%s' names a Rust prelude type" % name if name in TC.NS_RESERVED else None)
                if p:
                    items.append(K.ResidualItem("namespace name: " + p, self.loc(n)))
                    return
            inner_ns, inner_internal = (ns + (name,), internal) if name else (ns, True)
            self.cpath.append(name or "(anonymous namespace)")
            try:
                for c in n.get("inner", []):
                    self.collect(c, inner_ns, inner_internal, items, funcs)
            finally:
                self.cpath.pop()
        elif k == "LinkageSpecDecl":
            for c in n.get("inner", []):
                if (not ns and c.get("kind") == "FunctionDecl" and c.get("name") == "printf"
                        and not any(x.get("kind") == "CompoundStmt" for x in c.get("inner", []))
                        and (self.qual(c.get("type")) or "").replace(" ", "")
                        in ("int(constchar*,...)", "int(constchar*__restrict,...)")):
                    self.libc_printf.add(c["id"])
                else:
                    items.append(K.ResidualItem("extern \"C\" declaration", self.loc(c)))
        elif k == "UsingDirectiveDecl":
            # FR-260: name lookup is RESOLVED by clang (every reference
            # carries its decl id), so a using-directive is pure spelling:
            # erased, like `using namespace std;` always was.
            if (n.get("nominatedNamespace") or {}).get("id") in self.ns_index \
                    and self.decl_main.get((n.get("nominatedNamespace") or {}).get("id")):
                self.has_using = True
            return
        elif k in ("UsingDecl", "UsingShadowDecl"):
            if self.using_is_user(n):
                self.has_using = True
                return  # `using a::f;` of a user function: spelling only
            if k == "UsingDecl":
                items.append(K.ResidualItem(friendly(k), self.loc(n)))
        elif k == "FunctionTemplateDecl":
            r = self.template_func(n, ns, internal, funcs)
            if r is not None:
                items.append(K.ResidualItem(r, self.loc(n)))
                # its calls are cascades of this one report
                self.bad_funcs |= {c.get("id") for c in n.get("inner", []) if c.get("kind") == "FunctionDecl"}
        elif k == "NamespaceAliasDecl":
            al = n.get("aliasedNamespace", {}).get("id")
            if al in self.ns_index and self.decl_main.get(al):
                return  # an alias of a namespace of this file: spelling only
            items.append(K.ResidualItem("namespace alias of a namespace outside this file", self.loc(n)))
        else:
            items.append(K.ResidualItem(friendly(k), self.loc(n)))

    # -------------------------------------------------------------- FR-257
    def collect_record(self, n, ns, internal, items):
        """A main-file struct/class at namespace scope."""
        if n.get("isImplicit") or not n.get("completeDefinition"):
            return  # a forward declaration is spelling only
        L = self.loc(n)
        if n.get("tagUsed") == "union":
            items.append(K.ResidualItem("union", L))
            return
        if not n.get("name"):
            items.append(K.ResidualItem("class/struct definition", L))
            return
        self.records.append((n, ns, internal))
        self.record_cpath[n["id"]] = list(self.cpath)

    MEMBER_KINDS = {
        "FriendDecl": "friend declaration", "FunctionTemplateDecl": "member function template",
        "CXXRecordDecl": "nested class", "EnumDecl": "enum inside a class", "TypedefDecl": "member typedef",
        "TypeAliasDecl": "member typedef", "StaticAssertDecl": "static_assert inside a class",
        "UsingDecl": "using-declaration inside a class", "IndirectFieldDecl": "anonymous union or struct member",
        "CXXConversionDecl": "conversion operator",
    }

    def struct_shell(self, n, ns, internal, items):
        """Fields and member-function SIGNATURES of one struct (bodies are
        imported later, when every signature is known)."""
        path = self.record_path[n["id"]]
        L = self.loc(n)
        self.ctx_ns = ns
        sh = {"path": path, "name": n["name"], "ns": ns, "internal": internal, "loc": L, "fields": [],
              "methods": [], "node": n}
        n_before = len(items)
        if n.get("bases"):
            items.append(K.ResidualItem("class with a base class (inheritance)", L))
            self.bad_records.add(n["id"])
        access = "private" if n.get("tagUsed") == "class" else "public"
        for m in n.get("inner", []):
            k = m.get("kind")
            if m.get("isImplicit"):
                continue
            ML = self.lines.at(_off(m.get("loc"))) or L
            if k == "AccessSpecDecl":
                access = m.get("access")
                continue
            if access != "public" and k in ("FieldDecl", "CXXMethodDecl", "CXXConstructorDecl",
                                            "CXXDestructorDecl", "VarDecl"):
                items.append(K.ResidualItem("private or protected member", ML))
                self.poison_member(m, path)
                continue
            if k == "FieldDecl":
                self.shell_field(m, sh, items, ML)
            elif k == "VarDecl":
                items.append(K.ResidualItem("static data member", ML))
            elif k in ("CXXMethodDecl", "CXXConstructorDecl"):
                self.shell_method(m, sh, items, ML)
            elif k == "CXXDestructorDecl":
                items.append(K.ResidualItem("user-declared destructor", ML))
                self.poison_member(m, path)
            else:
                items.append(K.ResidualItem(self.MEMBER_KINDS.get(k, "%s inside a class" % friendly(k)), ML))
                self.poison_member(m, path)
        member_res = [it for it in items[n_before:] if it.loc != L]
        if member_res and not any(it.loc == L for it in items[n_before:]):
            # the struct cannot be emitted, so neither can any of its member
            # functions: one residual AT THE CLASS, which a per-function census
            # attributes to every member (the member-level ones above stay
            # where the construct is)
            items.append(K.ResidualItem("struct with members outside the subset", L))
            self.bad_records.add(n["id"])
        shell = K.StructDef(sh["name"], tuple(sh["fields"]), (), L, ns, internal)
        self.sigs.structs[path] = shell
        return sh

    def poison_member(self, m, path):
        if m.get("kind") == "FieldDecl":
            self.field_ids[m["id"]] = (path, None)
        else:
            self.method_ids[m["id"]] = None

    def shell_field(self, m, sh, items, ML):
        name = m.get("name")
        bad = None
        if m.get("isBitfield"):
            bad = "bit-field"
        elif m.get("hasInClassInitializer"):
            bad = "default member initializer"
        elif m.get("mutable"):
            bad = "mutable data member"
        elif not name:
            bad = "unnamed data member"
        ty = None
        if bad is None:
            ty, const, spelled = self.map_type(m.get("type"))
            if const:
                bad = "const data member"
            elif ty is None or ty == K.UNIT or isinstance(ty, (K.RefT, K.PtrT)):
                bad = "data member of type '%s'" % spelled
            elif isinstance(ty, K.VecT) and not K.elem_ok(ty.elem):
                bad = "data member of type '%s'" % spelled
        if bad is not None:
            items.append(K.ResidualItem(bad, ML))
            self.field_ids[m["id"]] = (sh["path"], None)
            return
        self.field_ids[m["id"]] = (sh["path"], name)
        sh["fields"].append(K.FieldDef(name, ty, ML))

    def shell_method(self, m, sh, items, ML):
        k = m.get("kind")
        name = m.get("name") or ""
        qt = self.qual(m.get("type")) or ""
        bad = None
        if k == "CXXMethodDecl" and name.startswith("operator"):
            bad = "operator overload"
        elif m.get("virtual") or m.get("pure"):
            bad = "virtual member function"
        elif m.get("explicitlyDeleted") or m.get("explicitlyDefaulted"):
            bad = "defaulted or deleted member function"
        elif "volatile" in qt:
            bad = "volatile member function"
        elif k == "CXXConstructorDecl":
            ps = [p for p in m.get("inner", []) if p.get("kind") == "ParmVarDecl"]
            if len(ps) == 1:
                pq = (self.qual(ps[0].get("type")) or "").replace(" ", "")
                if pq in ("const%s&" % sh["name"], "%s&&" % sh["name"], "%s&" % sh["name"]) \
                        or pq.endswith("::%s&" % sh["name"]) or pq.endswith("::%s&&" % sh["name"]):
                    bad = "user-declared copy or move constructor"
            if bad is None and any(x["kind"] == "CXXConstructorDecl" for x, _ in sh["methods"]):
                bad = "overloaded constructor"
        if bad is not None:
            items.append(K.ResidualItem(bad, ML))
            self.method_ids[m["id"]] = None
            return
        if k == "CXXConstructorDecl":
            recv = "ctor"
            mpath = sh["path"] + "::new"
            self.ctor_types[sh["path"]] = qt
        else:
            recv = "static" if m.get("storageClass") == "static" else (
                "ref" if qt.rstrip().endswith("const") else "mut")
            mpath = sh["path"] + "::" + name
        self.method_ids[m["id"]] = mpath
        self.user_funcs[m["id"]] = mpath
        sh["methods"].append((m, recv))
        if mpath in self.sigs:
            return  # (an overload: reported when the struct is checked)
        ptys = []
        for p in m.get("inner", []):
            if p.get("kind") == "ParmVarDecl":
                ptys.append(self.map_type(p.get("type"))[0])
        if recv == "ctor":
            ret = K.StructT(sh["path"])
        else:
            rs = self.ret_type_str(m)
            if rs.endswith("const"):
                rs = rs[:-len("const")].strip()
            ret = TYPE_MAP.get(rs) or parse_cxx_type(rs, self.struct_written, self.td_ok)[0]
        self.sigs[mpath] = (tuple(ptys), ret)
        self.sigs.recv[mpath] = recv

    def attach_definition(self, n):
        """An out-of-line member definition `int S::f() const {..}`."""
        rid = n.get("parentDeclContextId")
        if rid not in self.record_path:
            return [K.ResidualItem(friendly(n.get("kind")), self.loc(n))]
        prev = n.get("previousDecl")
        if prev in self.method_ids and self.has_body(n):
            # The definition is a separate item from its in-class declaration,
            # so a rejection of the declaration or of its class is repeated
            # HERE, located: a per-function census would otherwise count the
            # body of a rejected member as clean.
            if self.method_ids[prev] is None:
                return [K.ResidualItem("definition of a rejected member function", self.loc(n))]
            self.defs[prev] = n
            self.method_ids[n["id"]] = self.method_ids[prev]
            self.user_funcs[n["id"]] = self.method_ids[prev]
            if rid in self.bad_records:
                return [K.ResidualItem("member function of a struct with members outside the subset",
                                       self.loc(n))]
            return []
        if prev in self.method_ids:
            return []  # a redeclaration without a body
        return [K.ResidualItem(friendly(n.get("kind")), self.loc(n))]

    def struct_def(self, sh, items):
        methods = []
        for decl, recv in sh["methods"]:
            body = decl if self.has_body(decl) else self.defs.get(decl["id"])
            if body is None:
                items.append(K.ResidualItem("member function declared but not defined", self.loc(decl)))
                continue
            methods.append(self.func(body, sh["ns"], False, owner=sh, recv=recv, decl=decl))
        return K.StructDef(sh["name"], tuple(self.sigs.structs[sh["path"]].fields), tuple(methods), sh["loc"],
                           sh["ns"], sh["internal"])

    def spelling_only(self, d):
        """FR-260: a block-scope using-directive, using-declaration of a
        user function, or alias of a user namespace changes only how names
        are SPELLED -- every reference is resolved by clang -- so it is
        erased like its namespace-scope counterpart."""
        k = d.get("kind")
        if k == "UsingDirectiveDecl":
            return True
        if k in ("UsingDecl", "UsingShadowDecl"):
            return self.using_is_user(d)
        if k == "NamespaceAliasDecl":
            al = d.get("aliasedNamespace", {}).get("id")
            return al in self.ns_index and bool(self.decl_main.get(al))
        return False

    def using_is_user(self, n):
        """Is this using-declaration (or its shadow) about a main-file
        function?  (`using std::string;` & co keep FR-251's residual.)"""
        if n.get("kind") == "UsingShadowDecl":
            t = n.get("target", {})
            return t.get("kind") == "FunctionDecl" and bool(self.decl_main.get(t.get("id")))
        name = (n.get("name") or "").lstrip(":")
        head = name.split("::")[0]
        return any(path and path[0] == head and self.decl_main.get(i) for i, (path, _) in self.ns_index.items())

    @staticmethod
    def has_body(n):
        return any(isinstance(c, dict) and c.get("kind") == "CompoundStmt" for c in n.get("inner", []))

    def ret_type_str(self, n):
        q = self.qual(n.get("type")) or ""
        i = q.find("(")
        return q[:i].strip() if i >= 0 else q

    def signature_types(self, n):
        ptys = []
        fp = self.fn_params.get(n.get("id"))
        for p in n.get("inner", []):
            if p.get("kind") == "ParmVarDecl":
                if fp is not None and fp[0] == len(ptys):
                    ptys.append(fp[1])  # FR-252: the callable parameter
                    continue
                ptys.append(self.map_type(p.get("type"))[0])
        rs = self.ret_type_str(n)
        ret = TYPE_MAP.get(rs) or parse_cxx_type(rs, self.struct_written, self.td_ok)[0]
        return tuple(ptys), ret

    # -------------------------------------------------------------- funcs
    def func(self, n, ns=(), internal=False, owner=None, recv="", decl=None):
        name = n.get("name")
        floc = self.lines.at(_off(n.get("loc")))
        pre = []
        self.ctx_ns = ns
        self.cur = (owner["path"], recv) if owner else None
        if owner is not None:
            name = "new" if recv == "ctor" else name
            if n.get("inline"):
                pre.append(K.ResidualStmt("inline function", floc))
                pre.append(K.ResidualStmt("inline function", floc))
        elif n.get("storageClass") == "extern":
            pre.append(K.ResidualStmt("extern function", floc))
        elif n.get("storageClass") == "static" and ns:
            # FR-259 (N21): internal linkage inside a named namespace is the
            # anonymous-namespace member it means (Rust: a private `fn`);
            # at the root it is dropped like `inline` (N14)
            internal = True
        if n.get("constexpr"):
            pre.append(K.ResidualStmt("constexpr function", floc))
        if n.get("variadic"):
            pre.append(K.ResidualStmt("variadic function", floc))
        qt = self.qual(n.get("type")) or ""
        if "noexcept" in qt or qt.rstrip().endswith("&") or "throw(" in qt:
            pre.append(K.ResidualStmt("exception specification", floc))
        self.scope = TC.Scope()
        if recv in ("ref", "mut"):
            self.scope.declare("self", K.RefT(K.StructT(owner["path"]), recv == "mut"), False)
        params = []
        fp = self.fn_params.get(n.get("id"))
        for p in n.get("inner", []):
            if p.get("kind") != "ParmVarDecl":
                continue
            if fp is not None and fp[0] == len(params):
                # FR-252: `const F& f` / `F& f`, typed by the instantiations
                pl = self.lines.at(_off(p.get("loc")))
                pname = p.get("name")
                if not pname:
                    pre.append(K.ResidualStmt("unnamed parameter", pl))
                    pname = "unnamed_param_%d" % len(params)
                params.append(K.Param(pname, fp[1], False, pl))
                self.scope.declare(pname, fp[1], False)
                self.locals[p["id"]] = pname
                continue
            ty, const, spelled = self.map_type(p.get("type"))
            pl = self.lines.at(_off(p.get("loc")))
            pname = p.get("name")
            if not pname:
                pre.append(K.ResidualStmt("unnamed parameter", pl))
                pname = "unnamed_param_%d" % len(params)
            if ty is None or ty == K.UNIT or (isinstance(K.deref(ty), K.VecT) and not K.elem_ok(K.deref(ty).elem)):
                pre.append(K.ResidualStmt("parameter of type '%s'" % spelled, pl))
                ty = K.I32
            if p.get("init") or p.get("inner"):
                pre.append(K.ResidualStmt("default argument", pl))
            mut = not const and not isinstance(ty, K.RefT)
            params.append(K.Param(pname, ty, mut, pl))
            self.scope.declare(pname, ty, mut)
            self.locals[p["id"]] = pname
        rs = self.ret_type_str(n)
        if rs.endswith(" const") and recv == "ref":
            rs = rs[:-len(" const")]
        ret = TYPE_MAP.get(rs) or parse_cxx_type(rs, self.struct_written, self.td_ok)[0]
        if recv == "ctor":
            ret = K.StructT(owner["path"])
            body = self.ctor_body(n, owner, floc)
            self.cur = None
            return K.Func(name, tuple(params), ret, K.Block(tuple(pre) + (body,), self.loc(n)), floc,
                          tuple(ns), False, owner["name"], recv)
        self.ret_ref = isinstance(ret, (K.RefT, K.PtrT))
        if isinstance(ret, K.RefT):
            pre.append(K.ResidualStmt("function returning a reference", floc))
            ret = ret.inner  # its returns become cascades, not more reports
        elif isinstance(ret, K.PtrT):
            pre.append(K.ResidualStmt("return type '%s'" % rs, floc))
            ret = ret.inner  # FR-261: like a reference, its returns become cascades
        elif ret is None or (isinstance(ret, K.VecT) and not K.elem_ok(ret.elem)):
            pre.append(K.ResidualStmt("return type '%s'" % rs, floc))
            ret = K.UNIT
        self.ret = ret
        body_node = [c for c in n.get("inner", []) if c.get("kind") == "CompoundStmt"][-1]
        stmts = []
        is_main = name == "main" and not ns
        inner = list(body_node.get("inner", []))
        if is_main:
            if ret != K.I32:
                pre.append(K.ResidualStmt("main not returning int", floc))
            if inner and inner[-1].get("kind") == "ReturnStmt":
                last = inner[-1]
                v = (last.get("inner") or [None])[0]
                if v is not None and v.get("kind") == "IntegerLiteral" and v.get("value") == "0":
                    inner = inner[:-1]
            ret = K.UNIT
            self.ret = K.UNIT
        self.in_main = is_main
        for s in inner:
            stmts.extend(self.stmt(s))
        self.cur = None
        if owner is not None:
            return K.Func(name, tuple(params), ret, K.Block(tuple(pre + stmts), self.loc(body_node)), floc,
                          tuple(ns), False, owner["name"], recv)
        return K.Func(name, tuple(params), ret, K.Block(tuple(pre + stmts), self.loc(body_node)), floc,
                      tuple(ns), internal)

    def ctor_body(self, n, owner, floc):
        """`S(..) : f1(e1), f2(e2) {}` -> `return S { f1: e1, f2: e2 };`:
        every data member initialized, in declaration order (clang lists
        the initializers in initialization order), and an empty body."""
        body = [c for c in n.get("inner", []) if c.get("kind") == "CompoundStmt"]
        inits = [c for c in n.get("inner", []) if c.get("kind") == "CXXCtorInitializer"]
        fields = self.sigs.structs[owner["path"]].fields
        bad = "constructor whose body is not a member-initializer list of every data member"
        if not body or body[-1].get("inner"):
            return K.ResidualStmt(bad, floc)
        if any("anyInit" not in i for i in inits):
            return K.ResidualStmt("delegating or base-class constructor initializer", floc)
        names = [i["anyInit"].get("name") for i in inits]
        if names != [f.name for f in fields]:
            return K.ResidualStmt(bad, floc)
        vals = []
        for f, i in zip(fields, inits):
            e = i["inner"][0]
            if K.is_owned_nc(f.ty):
                v = self.consume(e, f.ty)
            else:
                v = self.expr(e)
                if f.ty == K.BOOL:
                    v = self.truthy(v)
            vals.append(v)
        return K.Return(K.StructLit(owner["path"], tuple(vals), floc), floc)

    # -------------------------------------------------------------- stmts
    def as_block(self, n):
        saved = self.scope
        self.scope = saved.child()
        try:
            if n.get("kind") == "CompoundStmt":
                out = []
                for s in n.get("inner", []):
                    out.extend(self.stmt(s))
                return K.Block(tuple(out), self.loc(n))
            return K.Block(tuple(self.stmt(n)), self.loc(n))
        finally:
            self.scope = saved

    @staticmethod
    def strip(n):
        while isinstance(n, dict) and (n.get("kind") in ("ParenExpr", "ExprWithCleanups", "ConstantExpr")
                                       or (n.get("kind") == "ImplicitCastExpr"
                                           and n.get("castKind") in ("LValueToRValue", "NoOp"))):
            n = n["inner"][0]
        return n

    def local_ref(self, n):
        """Name of the local variable ``n`` refers to, or None."""
        n = self.strip(n)
        if n.get("kind") != "DeclRefExpr":
            return None
        rd = n.get("referencedDecl", {})
        return self.locals.get(rd.get("id"))

    def stmt(self, n):
        if n is None:
            return []
        k = n.get("kind")
        L = self.loc(n)
        if k == "CompoundStmt":
            return [self.as_block(n)]
        if k == "NullStmt":
            return []
        if k == "DeclStmt":
            return [self.decl(d) for d in n.get("inner", []) if not self.spelling_only(d)]
        if k == "ReturnStmt":
            if self.in_main:
                return [self.rstmt(n, "return inside main other than the final `return 0;`")]
            v = (n.get("inner") or [None])[0]
            if v is not None and getattr(self, "ret_ref", False):
                # (a reference / FR-261 pointer return type is reported once, at the function)
                return [K.Return(K.ResidualExpr("returned reference", L, cascade=True), L)]
            if v is not None and K.is_owned_nc(self.ret):
                return [K.Return(self.consume(v, self.ret), L)]
            return [K.Return(None if v is None else self.expr(v), L)]
        if k == "BreakStmt":
            return [K.Break(L)]
        if k == "ContinueStmt":
            return [K.Continue(L)]
        if k == "IfStmt":
            if n.get("hasInit") or n.get("hasVar"):
                return [self.rstmt(n, "if with an init-statement or condition variable")]
            if n.get("isConstexpr"):
                return [self.rstmt(n, "if constexpr")]
            inner = n["inner"]
            cond = self.truthy(self.expr(inner[0]))
            then = self.as_block(inner[1])
            els = self.as_block(inner[2]) if n.get("hasElse") and len(inner) > 2 else None
            return [K.If(cond, then, els, L)]
        if k == "WhileStmt":
            if n.get("hasVar"):
                return [self.rstmt(n, "while with a condition variable")]
            inner = n["inner"]
            return [K.While(self.truthy(self.expr(inner[0])), self.as_block(inner[1]), L)]
        if k == "ForStmt":
            s = self.for_(n)
            if self.normalize and isinstance(s, K.ResidualStmt) and s.reason == FOR_BAD:
                return self.rewrite(n, NZ.FOR_TO_WHILE)
            return [s]
        if k == "DoStmt" and self.normalize:
            return self.rewrite(n, NZ.DO_WHILE)
        if k == "CXXForRangeStmt":
            return [self.range_for(n)]
        if k == "CXXMemberCallExpr":
            return [self.member_stmt(n)]
        if k == "CXXOperatorCallExpr":
            return [self.operator_stmt(n)]
        if k in ("ExprWithCleanups", "ParenExpr", "CXXBindTemporaryExpr", "MaterializeTemporaryExpr"):
            return self.stmt(n["inner"][0])
        if k == "BinaryOperator" and n.get("opcode") == "=":
            name = self.local_ref(n["inner"][0])
            if name is None:
                tgt = self.elem_target(n["inner"][0])
                if isinstance(tgt, (K.Index, K.Field)):
                    T = self.ty_of(tgt)
                    v = self.expr(n["inner"][1])
                    if T == K.BOOL:
                        v = self.truthy(v)
                    return [K.Store(tgt, v, L)]
                if isinstance(tgt, K.ResidualStmt):
                    return [tgt]
                return [self.rstmt(n, "assignment to something other than a local variable")]
            return [K.Assign(name, self.expr(n["inner"][1]), L)]
        if k == "CompoundAssignOperator":
            return [self.compound(n)]
        if k == "UnaryOperator" and n.get("opcode") in ("++", "--"):
            return [self.incdec(n)]
        if k == "CallExpr":
            c = self.callee(n)
            if c == "printf":
                return [self.printf(n)]
            st = self.alg_stmt(n)  # FR-253
            if st is not None:
                return [st]
            e = self.expr(n)
            if isinstance(e, K.Call):
                return [K.ExprStmt(e, L)]
            return [K.ResidualStmt(e.reason, e.loc) if isinstance(e, K.ResidualExpr) else self.rstmt(n, "call")]
        if k in FRIENDLY:
            return [self.rstmt(n, friendly(k))]
        if k.endswith("Stmt"):
            return [self.rstmt(n, friendly(k))]
        if k.endswith("Expr") or k.endswith("Operator") or k.endswith("Literal"):
            e = self.expr(n)
            if isinstance(e, K.ResidualExpr):
                return [K.ResidualStmt(e.reason, e.loc)]
            return [self.rstmt(n, "expression statement without effect")]
        return [self.rstmt(n, friendly(k))]

    def rewrite(self, n, rule):
        """FR-254: import ``rule``'s rewrite of ``n`` (or its guard's
        located refusal)."""
        why = rule.guard(n)
        if why is not None:
            return [self.rstmt(n, why)]
        self.notes.append((self.loc(n), "normalized: " + rule.name))
        out = []
        for s in rule.apply(n):
            out.extend(self.stmt(s))
        return out

    def decl(self, d):
        r = self.decl_inner(d)
        if isinstance(r, K.ResidualStmt) and "id" in d:
            self.poisoned.add(d["id"])
        return r

    def decl_inner(self, d):
        k = d.get("kind")
        L = self.loc(d)
        if k != "VarDecl":
            return self.rstmt(d, "local " + friendly(k))
        name = d.get("name")
        ty, const, spelled = self.map_type(d.get("type"))
        head = self.text_at(d).lstrip()
        if head.startswith("const "):
            head = head[len("const "):].lstrip()
        if head.startswith("auto") and (len(head) == 4 or not (head[4].isalnum() or head[4] == "_")):
            # FR-259 (N22): `type` is clang's DEDUCED type, read like a written
            # one (a reference, an iterator or a lambda keeps a residual)
            if "(lambda at " in (spelled or ""):
                return self.closure_decl(d, L, ty)
        if head.startswith("decltype"):
            return K.ResidualStmt("decltype-typed declaration", L)
        if d.get("storageClass") in ("static", "extern"):
            return K.ResidualStmt("%s local variable" % d["storageClass"], L)
        if d.get("constexpr"):
            return K.ResidualStmt("constexpr variable", L)
        if ty is None or ty == K.UNIT or isinstance(ty, K.RefT):
            return K.ResidualStmt("variable of type '%s'" % spelled, L)
        if isinstance(ty, K.VecT) and not K.elem_ok(ty.elem):
            return K.ResidualStmt("variable of type '%s' (std::vector<bool> is a bit-packed proxy "
                                  "container)" % spelled if ty.elem == K.BOOL else "variable of type '%s'" % spelled, L)
        if isinstance(ty, K.StructT) and d.get("init") in ("call", "list"):
            # FR-257: `S s(a, b);` / `S s{a, b};` (a constructor or an
            # aggregate) are the same Kernel term as `S s = ...;`
            c = self.unwrap_ctor((d.get("inner") or [{}])[0])
            if c.get("kind") == "InitListExpr":
                init = self.struct_init(c)
            elif c.get("kind") in ("CXXConstructExpr", "CXXTemporaryObjectExpr"):
                kind, _ = self.ctor_kind(c)
                if kind == "default" and self.ctor_of_struct(ty.path) is None:
                    return K.ResidualStmt("struct variable without an initializer (default-initialized data "
                                          "members)", L)
                init = self.class_expr(c) if kind != "copy" else self.consume(d["inner"][0], ty)
            else:
                init = self.consume(d["inner"][0], ty)
            self.scope.declare(name, ty, not const)
            self.locals[d["id"]] = name
            return K.Let(name, ty, not const, init, L)
        if K.is_owned_nc(ty) and d.get("init") == "call":
            init = self.class_direct_init(d, ty)
            if isinstance(init, K.ResidualStmt):
                return init
            self.scope.declare(name, ty, not const)
            self.locals[d["id"]] = name
            return K.Let(name, ty, not const, init, L)
        if "init" not in d:
            return K.ResidualStmt("declaration without an initializer", L)
        if d.get("init") != "c":
            return K.ResidualStmt("%s-initialization (the subset spells it `T x = e;`)" % d.get("init"), L)
        if K.is_owned_nc(ty):
            init = self.consume(d["inner"][0], ty)
            self.scope.declare(name, ty, not const)
            self.locals[d["id"]] = name
            return K.Let(name, ty, not const, init, L)
        init = self.expr(d["inner"][0])
        if ty == K.BOOL:
            init = self.truthy(init)
        self.scope.declare(name, ty, not const)
        self.locals[d["id"]] = name
        return K.Let(name, ty, not const, init, L)

    def compound(self, n):
        L = self.loc(n)
        name = self.local_ref(n["inner"][0])
        if name is None:
            tgt = self.elem_target(n["inner"][0])
            if isinstance(tgt, K.ResidualStmt):
                return tgt
            if isinstance(tgt, (K.Index, K.Field)):
                return self.compound_elem(n, tgt)
            return self.rstmt(n, "compound assignment to something other than a local variable")
        op = COMPOUND_OPCODES.get(n.get("opcode"))
        v = self.scope.lookup(name)
        T = K.deref(v[0]) if v else None
        if isinstance(T, K.PtrT):
            # FR-261: `p += k;` (a forward re-slice); `p -= k;` cannot be one
            if op == "sub":
                return self.rstmt(n, R3_DEC)
            if op != "add":
                return self.rstmt(n, "operator '%s'" % n.get("opcode"))
            k = self.ptr_offset(n["inner"][1])
            if isinstance(k, K.ResidualExpr):
                return K.ResidualStmt(k.reason, k.loc)
            return K.CompoundAssign("add", name, k, L)
        if op is None:
            return self.rstmt(n, "operator '%s'" % n.get("opcode"))
        CL = self.map_type(n.get("computeLHSType"))[0]
        rhs = self.expr(n["inner"][1])
        if T == K.BOOL:
            return self.rstmt(n, "compound assignment to bool")
        if CL is None:
            return self.rstmt(n, "compound assignment computed in type '%s'" % self.qual(n.get("computeLHSType")))
        if CL == T:
            return K.CompoundAssign(op, name, rhs, L)
        return K.Assign(name, K.Cast(T, K.Binary(op, K.Cast(CL, K.Var(name, L), L), rhs, L), L), L)

    def compound_elem(self, n, tgt):
        L = self.loc(n)
        op = COMPOUND_OPCODES.get(n.get("opcode"))
        if op is None:
            return self.rstmt(n, "operator '%s'" % n.get("opcode"))
        T = self.ty_of(tgt)
        CL = self.map_type(n.get("computeLHSType"))[0]
        rhs = self.expr(n["inner"][1])
        if T == K.BOOL:
            return self.rstmt(n, "compound assignment to bool")
        if CL is None:
            return self.rstmt(n, "compound assignment computed in type '%s'" % self.qual(n.get("computeLHSType")))
        if CL == T:
            return K.CompoundStore(op, tgt, rhs, L)
        return K.Store(tgt, K.Cast(T, K.Binary(op, K.Cast(CL, tgt, L), rhs, L), L), L)

    def incdec(self, n):
        L = self.loc(n)
        name = self.local_ref(n["inner"][0])
        op = "add" if n.get("opcode") == "++" else "sub"
        if name is None:
            tgt = self.elem_target(n["inner"][0])
            if isinstance(tgt, K.ResidualStmt):
                return tgt
            if isinstance(tgt, (K.Index, K.Field)):
                T = self.ty_of(tgt)
                if T in K.ARITH_INT:
                    return K.CompoundStore(op, tgt, K.IntLit(1, T, L), L)
                if T == K.F64:
                    return K.CompoundStore(op, tgt, K.FloatLit("1.0", L), L)
                if T in K.SMALL_INT:
                    return K.Store(tgt, K.Cast(T, K.Binary(op, K.Cast(K.I32, tgt, L), K.IntLit(1, K.I32, L), L), L), L)
            return self.rstmt(n, "increment of something other than a local variable")
        v = self.scope.lookup(name)
        T = K.deref(v[0]) if v else None
        if isinstance(T, K.PtrT):
            # FR-261: `++p;` is the forward re-slice `p = &p[1..];`
            if op != "add":
                return self.rstmt(n, R3_DEC)
            return K.CompoundAssign("add", name, K.IntLit(1, K.USIZE, L), L)
        if T in K.ARITH_INT:
            return K.CompoundAssign(op, name, K.IntLit(1, T, L), L)
        if T == K.F64:
            return K.CompoundAssign(op, name, K.FloatLit("1.0", L), L)
        if T in K.SMALL_INT:
            return K.Assign(name, K.Cast(T, K.Binary(op, K.Cast(K.I32, K.Var(name, L), L),
                                                     K.IntLit(1, K.I32, L), L), L), L)
        return self.rstmt(n, "increment of a bool")

    def for_(self, n):
        L = self.loc(n)
        bad = FOR_BAD
        inner = n.get("inner", [])
        if len(inner) != 5:
            return self.rstmt(n, bad)
        init, cvar, cond, inc, body = inner
        if not init or cvar or not cond or not inc or not body:
            return self.rstmt(n, bad)
        if init.get("kind") != "DeclStmt" or len(init.get("inner", [])) != 1:
            return self.rstmt(n, bad)
        vd = init["inner"][0]
        if vd.get("kind") != "VarDecl" or vd.get("init") != "c":
            return self.rstmt(n, bad)
        ty, const, _ = self.map_type(vd.get("type"))
        if ty not in (K.I32, K.USIZE) or const:
            return self.rstmt(n, bad)
        head = self.text_at(vd).lstrip()
        if head.startswith("auto"):
            return self.rstmt(n, bad)
        vid = vd["id"]

        def is_var(x):
            x = self.strip(x)
            return x.get("kind") == "DeclRefExpr" and x.get("referencedDecl", {}).get("id") == vid

        if not (cond.get("kind") == "BinaryOperator" and cond.get("opcode") == "<" and is_var(cond["inner"][0])):
            return self.rstmt(n, bad)
        ok_inc = (inc.get("kind") == "UnaryOperator" and inc.get("opcode") == "++" and is_var(inc["inner"][0]))
        if not ok_inc:
            return self.rstmt(n, bad)
        name = vd.get("name")
        lo = self.expr(vd["inner"][0])
        hi = self.expr(cond["inner"][1])
        if isinstance(lo, K.IntLit):
            # FR-261 (N19): starts at a literal >= 0, only ever incremented
            # below a bound of its own type -- never negative
            self.nonneg_ids.add(vid)
        saved = self.scope
        self.scope = saved.child()
        self.scope.declare(name, ty, False)
        self.locals[vid] = name
        try:
            if body.get("kind") == "CompoundStmt":
                out = []
                for s in body.get("inner", []):
                    out.extend(self.stmt(s))
                blk = K.Block(tuple(out), self.loc(body))
            else:
                blk = K.Block(tuple(self.stmt(body)), self.loc(body))
        finally:
            self.scope = saved
        return K.For(name, lo, hi, blk, L)

    # -------------------------------------------------------------- printf
    def callee(self, n):
        """'printf' for libc printf, the user function name, or None."""
        c = n.get("inner", [None])[0]
        c = self.strip(c)
        while isinstance(c, dict) and c.get("kind") == "ImplicitCastExpr" and c.get("castKind") == "FunctionToPointerDecay":
            c = self.strip(c["inner"][0])
        if not isinstance(c, dict) or c.get("kind") != "DeclRefExpr":
            return None
        rid = c.get("referencedDecl", {}).get("id")
        if rid in self.libc_printf:
            return "printf"
        return None

    def string_lit(self, n):
        n = self.strip(n)
        while isinstance(n, dict) and n.get("kind") == "ImplicitCastExpr" and n.get("castKind") in ("ArrayToPointerDecay", "NoOp"):
            n = self.strip(n["inner"][0])
        if isinstance(n, dict) and n.get("kind") == "StringLiteral":
            if not (self.qual(n.get("type")) or "").startswith("const char["):
                return None
            return n
        return None

    def printf(self, n):
        L = self.loc(n)
        args = n.get("inner", [])[1:]
        if not args:
            return self.rstmt(n, "printf without a format")
        lit = self.string_lit(args[0])
        if lit is None:
            return self.rstmt(n, "printf with a non-literal format")
        try:
            text = fmt.c_unescape_literal(lit.get("value", ""))
        except fmt.FormatError as ex:
            return self.rstmt(n, str(ex))
        if any(not (1 <= ord(c) <= 0x7F) for c in text):
            return self.rstmt(n, "printf format with non-ASCII or NUL characters")
        try:
            parts = fmt.parse_c_format(text)
        except fmt.FormatError as ex:
            return self.rstmt(n, str(ex))
        specs = [p for p in parts if isinstance(p, tuple)]
        if len(specs) != len(args) - 1:
            return self.rstmt(n, "printf with %d conversions and %d arguments" % (len(specs), len(args) - 1))
        segs = []
        ai = iter(args[1:])
        for p in parts:
            if isinstance(p, str):
                segs.append(K.Text(p))
                continue
            flags, width, prec, length, conv = p
            a = next(ai)
            want = fmt.C_ARG_TYPE[(conv, length)]
            spelled = "%" + flags + ("" if width is None else str(width)) + length + conv
            if conv == "s":
                sl = self.string_lit(a)
                cs = self.c_str_base(a)
                if sl is None and cs is not None:
                    segs.append(K.Spec("s", self.borrowed(cs) if isinstance(cs, dict) else cs))
                    continue
                if sl is None:
                    return self.rstmt(a, "printf %s of something other than a string literal")
                try:
                    sv = fmt.c_unescape_literal(sl.get("value", ""))
                except fmt.FormatError as ex:
                    return self.rstmt(a, str(ex))
                if any(not (1 <= ord(c) <= 0x7F) for c in sv):
                    return self.rstmt(a, "printf %s literal with non-ASCII or NUL characters")
                segs.append(K.Spec("s", K.StrLit(sv, self.loc(sl))))
                continue
            e = self.expr(a)
            t = self.ty_of(e)
            if t is not None and t != want:
                return self.rstmt(a, "printf argument of type '%s' for '%s'" % (
                    self.qual(self.strip(a).get("type")) or t, spelled))
            kind = {"d": "d", "i": "d", "u": "d", "f": "f", "c": "c"}[conv]
            if kind == "d":
                segs.append(K.Spec("d", e, flags, width, None))
            elif kind == "f":
                segs.append(K.Spec("f", e, "", None, prec))
            else:
                segs.append(K.Spec("c", e))
        return K.Print(tuple(segs), L)

    # -------------------------------------------------------------- exprs
    def truthy(self, e):
        t = self.ty_of(e)
        L = getattr(e, "loc", None)
        if t == K.BOOL or t is None:
            return e
        if t in K.SMALL_INT:
            return K.Binary("ne", K.Cast(K.I32, e, L), K.IntLit(0, K.I32, L), L)
        if t in K.ARITH_INT:
            return K.Binary("ne", e, K.IntLit(0, t, L), L)
        if t == K.F64:
            return K.Binary("ne", e, K.FloatLit("0.0", L), L)
        return K.ResidualExpr("conversion of %s to bool" % t, L)

    def expr(self, n):
        k = n.get("kind")
        L = self.loc(n)
        if k in ("ParenExpr", "ExprWithCleanups", "ConstantExpr"):
            return self.expr(n["inner"][0])
        if k == "ImplicitCastExpr":
            ck = n.get("castKind")
            if ck in ("LValueToRValue", "NoOp"):
                return self.expr(n["inner"][0])
            if ck in ("IntegralCast", "FloatingCast", "IntegralToFloating", "FloatingToIntegral"):
                T, _, spelled = self.map_type(n.get("type"))
                e = self.expr(n["inner"][0])
                if T is None or T in (K.UNIT, K.BOOL):
                    return self.rexpr(n, "conversion to '%s'" % spelled)
                return K.Cast(T, e, L)
            if ck in ("IntegralToBoolean", "FloatingToBoolean"):
                return self.truthy(self.expr(n["inner"][0]))
            if ck == "ArrayToPointerDecay":
                if self.string_lit(n) is not None:
                    return self.rexpr(n, "string literal outside a printf argument")
                return self.rexpr(n, "array-to-pointer decay")
            if ck == "FunctionToPointerDecay":
                return self.rexpr(n, "function pointer")
            if ck in ("PointerToBoolean", "NullToPointer"):
                return self.rexpr(n, R6_NULL)
            return self.rexpr(n, "implicit conversion (%s)" % ck)
        if k in ("CXXStaticCastExpr", "CStyleCastExpr", "CXXFunctionalCastExpr"):
            T, _, spelled = self.map_type(n.get("type"))
            if K.is_owned_nc(T):
                if k == "CXXFunctionalCastExpr" and n.get("castKind") == "ConstructorConversion":
                    return self.class_expr(n["inner"][0])
                if (k == "CXXFunctionalCastExpr" and isinstance(T, K.StructT) and n.get("castKind") == "NoOp"
                        and self.strip_tmp(n["inner"][0]).get("kind") == "InitListExpr"):
                    return self.struct_init(self.strip_tmp(n["inner"][0]))
                return self.rexpr(n, "cast to '%s'" % spelled)
            if n.get("castKind") == "ToVoid":
                return self.rexpr(n, "cast to void")
            if T is None or T == K.UNIT or isinstance(T, K.PtrT):
                return self.rexpr(n, "cast to '%s'" % spelled)
            e = self.expr(n["inner"][0])
            if T == K.BOOL:
                return self.truthy(e)
            t = self.ty_of(e)
            if t is not None and t != T:
                e = K.Cast(T, e, L)
            return e
        if k == "IntegerLiteral":
            T, _, spelled = self.map_type(n.get("type"))
            if T is None or T in (K.BOOL, K.UNIT, K.F64):
                return self.rexpr(n, "integer literal of type '%s'" % spelled)
            return K.IntLit(int(n.get("value")), T, L)
        if k == "CharacterLiteral":
            T, _, spelled = self.map_type(n.get("type"))
            v = n.get("value")
            if T != K.I8:
                return self.rexpr(n, "character literal of type '%s'" % spelled)
            if not (0 <= v <= 127):
                return self.rexpr(n, "non-ASCII character literal")
            return K.IntLit(v, K.I8, L)
        if k == "FloatingLiteral":
            T, _, spelled = self.map_type(n.get("type"))
            if T != K.F64:
                return self.rexpr(n, "floating literal of type '%s'" % spelled)
            try:
                v = float(n.get("value"))
            except (TypeError, ValueError):
                return self.rexpr(n, "floating literal")
            if v != v or v in (float("inf"), float("-inf")):
                return self.rexpr(n, "non-finite floating literal")
            return K.FloatLit(repr(v), L)
        if k == "CXXBoolLiteralExpr":
            return K.BoolLit(bool(n.get("value")), L)
        if k == "DeclRefExpr":
            rd = n.get("referencedDecl", {})
            name = self.locals.get(rd.get("id"))
            if name is not None:
                return K.Var(name, L)
            if rd.get("id") in self.poisoned:
                return K.ResidualExpr("use of a rejected declaration", L, cascade=True)
            rk = rd.get("kind")
            if rk == "VarDecl":
                return self.rexpr(n, "global variable")
            if rk == "EnumConstantDecl":
                return self.rexpr(n, "enumerator")
            if rk == "FunctionDecl":
                return self.rexpr(n, "function used as a value")
            return self.rexpr(n, "reference to %s" % friendly(rk or "declaration"))
        if k == "UnaryOperator":
            op = n.get("opcode")
            if op in ("++", "--"):
                return self.incdec_value(n)
            if op == "&":
                return self.addr_of(n)
            if op == "*":
                if self.is_this_deref(n):
                    return K.Var("self", L)
                p = self.ptr_var(n["inner"][0])
                if p is not None:
                    return K.Index(p, K.IntLit(0, K.USIZE, L), L)  # N16: `*p` is `p[0]`
                if self.is_ptr_node(n["inner"][0]):
                    inner = self.expr(n["inner"][0])
                    if isinstance(inner, K.ResidualExpr):
                        return inner  # name the pointer shape (`*(p - 1)` is R3)
                return self.rexpr(n, "dereference")
            e = self.expr(n["inner"][0])
            if op == "+":
                return e
            if op == "-":
                return K.Unary("neg", e, L)
            if op in ("!", "~"):
                return K.Unary("not", e, L)
            return self.rexpr(n, "unary operator '%s'" % op)
        if k == "BinaryOperator":
            op = n.get("opcode")
            if op == "=":
                return self.rexpr(n, "assignment used as a value")
            if op == ",":
                return self.rexpr(n, "comma operator")
            if any(self.is_ptr_node(x) for x in n["inner"][:2]):
                return self.ptr_binary(n)
            kop = OPCODES.get(op)
            if kop is None:
                return self.rexpr(n, "operator '%s'" % op)
            a = self.expr(n["inner"][0])
            b = self.expr(n["inner"][1])
            return K.Binary(kop, a, b, L)
        if k == "CompoundAssignOperator":
            return self.rexpr(n, "compound assignment used as a value")
        if k == "CallExpr":
            if self.callee(n) == "printf":
                return self.rexpr(n, "printf result used as a value")
            c = self.strip(n["inner"][0])
            while c.get("kind") == "ImplicitCastExpr" and c.get("castKind") == "FunctionToPointerDecay":
                c = self.strip(c["inner"][0])
            if c.get("kind") == "MemberExpr" and self.sigs.recv.get(
                    self.method_ids.get(c.get("referencedMemberDecl")) or "") == "static":
                return self.rexpr(n, "static member function called through an object")
            if c.get("kind") != "DeclRefExpr":
                return self.rexpr(n, "indirect call")
            rd = c.get("referencedDecl", {})
            fname = self.user_funcs.get(rd.get("id"))
            if fname is None and rd.get("id") in self.bad_funcs:
                return K.ResidualExpr("call of a rejected function template", L, cascade=True)
            if fname is None:
                alg = self.alg_value(n)  # FR-253
                if alg is not None:
                    return alg
                if rd.get("name") == "move":
                    return self.rexpr(n, "std::move outside an initializer, assignment, by-value argument, "
                                         "push_back or return")
                if rd.get("name") == "to_string" and len(n["inner"]) == 2:
                    return self.to_string(n)
                return self.rexpr(n, "call to library function '%s'" % rd.get("name"))
            if self.method_ids.get(rd.get("id"), "") is None:
                return K.ResidualExpr("use of a rejected member function", L)
            if self.sigs.recv.get(fname) in ("ref", "mut"):
                return self.rexpr(n, "member function call")
            args = self.import_args(fname, n["inner"][1:])
            if isinstance(args, K.ResidualExpr):
                return args
            return K.Call(fname, tuple(args), L)
        if k == "CXXMemberCallExpr":
            return self.member_expr(n)
        if k == "CXXOperatorCallExpr":
            return self.operator_expr(n)
        if k in ("CXXConstructExpr", "CXXTemporaryObjectExpr", "CXXBindTemporaryExpr",
                 "MaterializeTemporaryExpr"):
            T = self.map_type(n.get("type"))[0]
            if K.is_owned_nc(T) or k in ("CXXBindTemporaryExpr", "MaterializeTemporaryExpr"):
                return self.class_expr(n)
        if k == "MemberExpr":
            return self.member_access(n)
        if k == "InitListExpr" and isinstance(self.map_type(n.get("type"))[0], K.StructT):
            return self.struct_init(n)
        if k == "ArraySubscriptExpr":
            return self.subscript(n)
        if k == "CXXThisExpr" and self.cur is not None and self.cur[1] in ("ref", "mut"):
            return K.AddrOf(K.Var("self", L), L)  # FR-261: `this` is `&*this`
        if k == "ConditionalOperator":
            return self.conditional(n)
        return self.rexpr(n, friendly(k))

    # ================================================== FR-259
    def conditional(self, n):
        """`c ? a : b` of a scalar type <=> Cond (both arms already converted
        to clang's result type; only the chosen one is evaluated)."""
        T, _, spelled = self.map_type(n.get("type"))
        if T not in K.ALL_PRIMS:
            return self.rexpr(n, "conditional operator `?:` of type '%s' (the subset's `?:` yields a scalar)"
                              % spelled)
        c, a, b = n["inner"][:3]
        return K.Cond(self.truthy(self.expr(c)), self.expr(a), self.expr(b), self.loc(n))

    def incdec_value(self, n):
        """`++x` / `x++` / `--x` / `x--` used as a value, on an integer
        variable of an arithmetic type (the sequencing rule -- no other
        mention of x in the statement -- is the checker's)."""
        L = self.loc(n)
        name = self.local_ref(n["inner"][0])
        if name is not None:
            v = self.scope.lookup(name)
            T = K.deref(v[0]) if v else None
            if T in K.ARITH_INT:
                return K.IncDec(K.Var(name, self.loc(self.strip(n["inner"][0]))),
                                "add" if n.get("opcode") == "++" else "sub", bool(n.get("isPostfix")), L)
        return self.rexpr(n, "increment/decrement used as a value")

    # ================================================== FR-261: pointers
    def is_ptr_node(self, n):
        return isinstance(n, dict) and is_ptr_spelling(self.qual(n.get("type")))

    def ptr_var(self, n):
        """K.Var for a reference to a pointer-typed local or parameter, or None."""
        name = self.local_ref(n)
        if name is None:
            return None
        v = self.scope.lookup(name)
        if v is None or not isinstance(v[0], K.PtrT):
            return None
        return K.Var(name, self.loc(self.strip(n)))

    def nonneg(self, e, n):
        """Is the signed Kernel expression ``e`` (clang node ``n``) provably
        non-negative?  A counting-loop variable with a literal lower bound, or
        a widening conversion of an unsigned value (`unsigned char` promoted
        to int)."""
        u = self.strip(n)
        if u.get("kind") == "DeclRefExpr" and u.get("referencedDecl", {}).get("id") in self.nonneg_ids:
            return True
        bits = {"i8": 8, "u8": 8, "i16": 16, "u16": 16, "i32": 32, "u32": 32, "i64": 64, "u64": 64, "usize": 64}
        if isinstance(e, K.Cast) and e.ty in K.SIGNED:
            t = self.ty_of(e.operand)
            return t in K.UNSIGNED and bits[t.name] < bits[e.ty.name]
        return False

    def ptr_offset(self, n):
        """N19: a pointer index / offset as a usize Kernel expression."""
        e = self.expr(n)
        if isinstance(e, K.ResidualExpr):
            return e
        t = self.ty_of(e)
        if t is None or t == K.USIZE:
            return e
        L = getattr(e, "loc", None) or self.loc(n)
        if isinstance(e, K.IntLit):
            return K.IntLit(e.value, K.USIZE, e.loc)
        if t in K.UNSIGNED or (t in K.SIGNED and self.nonneg(e, n)):
            return K.Cast(K.USIZE, e, L)
        if t in K.SIGNED:
            return self.rexpr(n, R3_SIGNED)
        return self.rexpr(n, "pointer offset of type '%s'" % (self.qual(n.get("type")) or t))

    def subscript(self, n):
        """`p[i]` on a pointer variable (an array, `(p + 1)[i]` or `i[p]`
        keep the old residual)."""
        base, idx = n["inner"][0], n["inner"][1]
        p = self.ptr_var(base)
        if p is None:
            return self.rexpr(n, "array subscript")
        i = self.ptr_offset(idx)
        if isinstance(i, K.ResidualExpr):
            return i
        return K.Index(p, i, self.loc(n))

    def addr_of(self, n):
        """`&x` (a one-element slice), `&v[i]` / `&p[i]` (N17: the suffix)."""
        L = self.loc(n)
        u = self.strip_tmp(n["inner"][0])
        if self.ptr_var(u) is not None:
            return self.rexpr(n, "address of a pointer (a pointer to a pointer)")
        pl = self.place(u)
        if isinstance(pl, K.ResidualExpr):
            if pl.reason == "copy of something other than a variable or element":
                return self.rexpr(n, "address-of")
            return pl
        if isinstance(pl, K.Index) and isinstance(self.ty_of(pl.base), (K.PtrT, K.VecT)):
            return K.Offset(pl.base, pl.index, L)
        if isinstance(pl, K.Index):
            return self.rexpr(n, "address-of")
        return K.AddrOf(pl, L)

    def ptr_binary(self, n):
        """A binary operator with a pointer operand: only `p + k` (on a
        pointer variable or `v.data()`) is in the subset."""
        op = n.get("opcode")
        a, b = n["inner"][0], n["inner"][1]
        pa, pb = self.is_ptr_node(a), self.is_ptr_node(b)
        null = any(self.is_null(x) for x in (a, b))
        if null:
            return self.rexpr(n, R6_NULL)
        if op in ("==", "!=", "<", "<=", ">", ">="):
            return self.rexpr(n, R4_CMP)
        if op == "-" and pa and pb:
            return self.rexpr(n, R4_DIFF)
        if op == "-":
            return self.rexpr(n, R3_MINUS)
        if op != "+" or not pa or pb:
            return self.rexpr(n, "pointer arithmetic other than `p + k`" if op == "+" else "operator '%s'" % op)
        base = self.expr(a)
        if isinstance(base, K.ResidualExpr):
            return base
        k = self.ptr_offset(b)
        if isinstance(k, K.ResidualExpr):
            return k
        L = self.loc(n)
        if isinstance(base, K.Var) and isinstance(self.ty_of(base), K.PtrT):
            return K.Offset(base, k, L)
        if isinstance(base, K.Offset) and base.start is None:
            return K.Offset(base.base, k, L)  # `v.data() + k`
        return self.rexpr(n, "pointer arithmetic other than `p + k` on a pointer variable or `v.data()`")

    def is_null(self, n):
        u = self.strip(n)
        while isinstance(u, dict) and u.get("kind") == "ImplicitCastExpr" and u.get("castKind") == "NullToPointer":
            return True
        return isinstance(u, dict) and u.get("kind") in ("CXXNullPtrLiteralExpr", "GNUNullExpr")

    def import_args(self, fname, nodes):
        """Kernel arguments for a call of ``fname`` (or a ResidualExpr)."""
        ptys = self.sigs.get(fname, ((), None))[0]
        args = []
        for i, a in enumerate(nodes):
            if a.get("kind") == "CXXDefaultArgExpr":
                return self.rexpr(a, "default argument")
            pt = ptys[i] if i < len(ptys) else None
            if isinstance(pt, K.RefT):
                name = self.local_ref(a)
                if name is not None:
                    args.append(K.Var(name, self.loc(a)))
                    continue
                if self.is_this_deref(self.strip_tmp(a)):
                    args.append(K.Var("self", self.loc(a)))
                    continue
                if not pt.mutable and self.strip_tmp(a).get("kind") == "ConditionalOperator":
                    args.append(self.expr(a))  # FR-259: a `const T&` reads the chosen arm's value
                    continue
                tgt = self.elem_target(a)
                if isinstance(tgt, (K.Index, K.Field)):
                    args.append(tgt)
                    continue
                if isinstance(tgt, K.ResidualStmt):
                    return K.ResidualExpr(tgt.reason, tgt.loc)
                if pt.mutable:
                    return self.rexpr(a, "argument %d of '%s' binds a non-const reference to something "
                                         "other than a variable or element" % (i + 1, fname))
                args.append(self.borrowed(a) if K.is_owned_nc(pt.inner) else self.expr(a))
            elif K.is_owned_nc(pt):
                args.append(self.consume(a, pt))
            else:
                args.append(self.expr(a))
        return args

    # ================================================== FR-257: structs
    def is_this_deref(self, n):
        n = self.strip(n) if isinstance(n, dict) else n
        return (isinstance(n, dict) and n.get("kind") == "UnaryOperator" and n.get("opcode") == "*"
                and self.strip(n["inner"][0]).get("kind") == "CXXThisExpr"
                and self.cur is not None and self.cur[1] in ("ref", "mut"))

    def member_access(self, n):
        """`p.f` / `this->f` / implicit `f` (a MemberExpr over CXXThisExpr)."""
        L = self.loc(n)
        fid = n.get("referencedMemberDecl")
        info = self.field_ids.get(fid)
        if info is None:
            if fid in self.method_ids:
                return self.rexpr(n, "member function used as a value")
            return self.rexpr(n, "member access")
        path, fname = info
        if fname is None:
            return K.ResidualExpr("use of a rejected data member", L)
        base = self.strip(n["inner"][0])
        if n.get("isArrow"):
            if base.get("kind") == "CXXThisExpr" and self.cur is not None and self.cur[1] in ("ref", "mut"):
                return K.Field(K.Var("self", L), fname, L)
            p = self.ptr_var(base)
            if p is not None:
                # FR-261 (N16): `p->f` is `p[0].f`
                return K.Field(K.Index(p, K.IntLit(0, K.USIZE, L), L), fname, L)
            return self.rexpr(n, "member access through a pointer")
        u = self.strip_tmp(n["inner"][0])
        if u.get("valueCategory") != "lvalue" and not self.is_this_deref(u):
            return self.rexpr(n, "member access on a temporary")
        b = self.place(u)
        if isinstance(b, K.ResidualExpr):
            return b
        return K.Field(b, fname, L)

    def struct_init(self, il):
        """Aggregate initialization `{a, b}` / `S{a, b}` of a struct: every
        data member, in declaration order."""
        L = self.loc(il)
        T = self.map_type(il.get("type"))[0]
        if not isinstance(T, K.StructT) or T.path not in self.sigs.structs:
            return self.rexpr(il, "initializer list")
        sd = self.sigs.structs[T.path]
        elems = il.get("inner", []) or []
        if any(e.get("kind") == "ImplicitValueInitExpr" for e in elems) or "array_filler" in il:
            return self.rexpr(il, "aggregate initialization that omits data members")
        if self.ctor_of_struct(T.path) is not None:
            return self.rexpr(il, "initializer list")
        if len(elems) != len(sd.fields):
            return self.rexpr(il, "aggregate initialization of a struct with rejected data members")
        vals = []
        for f, e in zip(sd.fields, elems):
            if K.is_owned_nc(f.ty):
                u = self.strip_tmp(e)
                v = self.struct_init(u) if u.get("kind") == "InitListExpr" else self.consume(e, f.ty)
            else:
                v = self.expr(e)
                if f.ty == K.BOOL:
                    v = self.truthy(v)
            vals.append(v)
        return K.StructLit(T.path, tuple(vals), L)

    def is_user_ctor(self, n, T):
        """Does this construct-expression call T's user constructor (rather
        than the implicit copy / move / default one)?  Its ctorType is the
        constructor's own type string."""
        ct = (n.get("ctorType") or {}).get("qualType")
        return ct is not None and ct == self.ctor_types.get(T.path)

    def ctor_of_struct(self, path):
        p = path + "::new"
        return p if self.sigs.recv.get(p) == "ctor" else None

    def ctor_call(self, n, T):
        """`S(a, b)` / `S{a, b}` / `S s(a, b);` through the user constructor."""
        cp = self.ctor_of_struct(T.path)
        if cp is None:
            return None
        args = self.import_args(cp, self.real_args(n))
        if isinstance(args, K.ResidualExpr):
            return args
        return K.Call(cp, tuple(args), self.loc(n))

    # ================================================== FR-251: vector/string
    TMP_KINDS = ("ParenExpr", "ExprWithCleanups", "ConstantExpr", "CXXBindTemporaryExpr",
                 "MaterializeTemporaryExpr")

    def strip_tmp(self, n):
        """See through temporaries, parens and NoOp/LValueToRValue casts."""
        while isinstance(n, dict):
            k = n.get("kind")
            if k in self.TMP_KINDS or (k == "ImplicitCastExpr" and n.get("castKind") in ("LValueToRValue", "NoOp")):
                n = n["inner"][0]
            else:
                return n
        return n

    def callee_decl(self, n):
        c = self.strip(n["inner"][0])
        while isinstance(c, dict) and c.get("kind") == "ImplicitCastExpr" and c.get("castKind") in (
                "FunctionToPointerDecay", "BuiltinFnToFnPtr"):
            c = self.strip(c["inner"][0])
        if isinstance(c, dict) and c.get("kind") == "DeclRefExpr":
            return c.get("referencedDecl", {})
        return {}

    def std_move_arg(self, n):
        """The argument of a `std::move(x)` call, or None."""
        n = self.strip_tmp(n)
        if not isinstance(n, dict) or n.get("kind") != "CallExpr" or len(n.get("inner", [])) != 2:
            return None
        rd = self.callee_decl(n)
        if rd.get("name") == "move" and rd.get("id") not in self.user_funcs:
            return n["inner"][1]
        return None

    @staticmethod
    def real_args(n):
        return [a for a in n.get("inner", []) if isinstance(a, dict) and a.get("kind") != "CXXDefaultArgExpr"]

    def ctor_kind(self, n):
        args = self.real_args(n)
        ct = (n.get("ctorType") or {}).get("qualType", "")
        if not args:
            return "default", args
        if len(args) == 1 and self.strip_tmp(args[0]).get("kind") == "CXXStdInitializerListExpr":
            return "ilist", args
        params = ""
        if "(" in ct:
            # the FIRST parenthesized group (a trailing `noexcept(false)` --
            # measured on the implicit copy constructor of a struct holding a
            # vector of itself -- has its own parentheses)
            depth, start = 0, ct.find("(")
            for j in range(start, len(ct)):
                depth += {"(": 1, ")": -1}.get(ct[j], 0)
                if depth == 0:
                    params = ct[start + 1:j]
                    break
        if len(args) == 1 and "&&" in params and "," not in params:
            return "move", args
        if len(args) == 1 and params.startswith("const ") and params.endswith("&") and "," not in params:
            return "copy", args
        if len(args) == 1 and params.startswith("const char *"):
            return "cstr", args
        if len(args) == 2 and params.startswith("size_type, const value_type &"):
            return "fill", args
        return "other", args

    def unwrap_ctor(self, n):
        """Strip temporaries and constructor-conversion wrappers."""
        while True:
            n = self.strip_tmp(n)
            k = n.get("kind")
            if k == "ImplicitCastExpr" and n.get("castKind") == "ConstructorConversion":
                n = n["inner"][0]
            elif k == "CXXFunctionalCastExpr" and n.get("castKind") == "ConstructorConversion":
                n = n["inner"][0]
            elif (k == "CXXFunctionalCastExpr" and n.get("castKind") == "NoOp"
                  and self.strip_tmp(n["inner"][0]).get("kind") == "InitListExpr"):
                n = self.strip_tmp(n["inner"][0])  # `S{a, b}`
            else:
                return n

    def literal_value(self, n):
        """(text, None) for an ASCII string literal node, (None, residual) or (None, None)."""
        sl = self.string_lit(n)
        if sl is None:
            return None, None
        try:
            v = fmt.c_unescape_literal(sl.get("value", ""))
        except fmt.FormatError as ex:
            return None, self.rexpr(n, str(ex))
        if any(not (1 <= ord(c) <= 0x7F) for c in v):
            return None, self.rexpr(n, "non-ASCII string literal (every String is ASCII so byte indexing "
                                       "never splits a character)")
        return v, None

    def class_expr(self, n):
        """A FRESH vector/string value (constructor, call, operator result)."""
        L = self.loc(n)
        n = self.unwrap_ctor(n)
        k = n.get("kind")
        if k in ("CXXConstructExpr", "CXXTemporaryObjectExpr"):
            T, _, spelled = self.map_type(n.get("type"))
            if not K.is_owned_nc(T):
                return self.rexpr(n, "construction of '%s'" % spelled)
            kind, args = self.ctor_kind(n)
            if isinstance(T, K.StructT) and self.is_user_ctor(n, T):
                return self.ctor_call(n, T)
            if isinstance(T, K.StructT) and kind not in ("copy", "move"):
                if kind == "default":
                    return self.rexpr(n, "value-initialized or default-initialized struct (`S()`, `S s;`)")
                return self.rexpr(n, "construction of '%s'" % spelled)
            if kind == "default":
                return K.VecLit(T.elem, (), L) if isinstance(T, K.VecT) else K.StrNew(L)
            if kind == "ilist" and isinstance(T, K.VecT):
                il = self.strip_tmp(self.strip_tmp(args[0])["inner"][0])
                if il.get("kind") != "InitListExpr":
                    return self.rexpr(n, "initializer list")
                return K.VecLit(T.elem, tuple(self.copied(x, T.elem) for x in il.get("inner", [])), L)
            if kind == "fill" and isinstance(T, K.VecT):
                return K.VecFill(self.copied(args[1], T.elem), self.expr(args[0]), L)
            if kind == "cstr" and T == K.STRING:
                v, bad = self.literal_value(args[0])
                if bad is not None:
                    return bad
                if v is None:
                    return self.rexpr(n, "std::string from something other than a string literal")
                return K.StrFrom(v, L)
            if kind == "copy":
                return K.Clone(self.place(args[0]), L)
            if kind == "move":
                return self.moved(args[0])
            return self.rexpr(n, "%s constructor" % ("std::string" if T == K.STRING else "std::vector"))
        if k == "InitListExpr" and isinstance(self.map_type(n.get("type"))[0], K.StructT):
            return self.struct_init(n)
        return self.expr(n)

    def moved(self, a):
        L = self.loc(a)
        inner = self.std_move_arg(a)
        if inner is None:
            s = self.strip_tmp(a)
            if s.get("kind") == "DeclRefExpr" and s.get("valueCategory") == "lvalue":
                inner = s  # `return v;`: C++17 implicit move (xvalue NoOp over the DeclRefExpr)
            else:
                return self.class_expr(a)  # moving from a temporary is the temporary
        name = self.local_ref(inner)
        if name is None:
            return self.rexpr(a, "std::move of something other than a variable")
        return K.Var(name, L)

    def consume(self, n, ty=None):
        """A CONSUMING position for a non-Copy value: copy -> Clone, move ->
        bare Var, temporary -> itself."""
        u = self.unwrap_ctor(n)
        k = u.get("kind")
        if k in ("CXXConstructExpr", "CXXTemporaryObjectExpr"):
            kind, args = self.ctor_kind(u)
            T = self.map_type(u.get("type"))[0]
            if isinstance(T, K.StructT) and self.is_user_ctor(u, T):
                kind = "user"
            if kind == "copy":
                if self.std_move_arg(args[0]) is not None:
                    return self.rexpr(args[0], "std::move of a const object (it copies)")
                return K.Clone(self.place(args[0]), self.loc(n))
            if kind == "move":
                return self.moved(args[0])
            return self.class_expr(u)
        if self.std_move_arg(n) is not None:
            return self.moved(n)
        if u.get("valueCategory") == "lvalue":
            return K.Clone(self.place(u), self.loc(n))
        return self.class_expr(u)

    def copied(self, n, ty):
        """A COPIED position (C++ takes `const T&` and copies): a place stays bare."""
        if K.is_copy(ty):
            return self.expr(n)
        u = self.unwrap_ctor(n)
        if u.get("kind") in ("CXXConstructExpr", "CXXTemporaryObjectExpr"):
            kind, args = self.ctor_kind(u)
            T = self.map_type(u.get("type"))[0]
            if kind == "copy" and not (isinstance(T, K.StructT) and self.is_user_ctor(u, T)):
                return self.place(args[0])
            return self.class_expr(u)
        if u.get("valueCategory") == "lvalue":
            return self.place(u)
        return self.class_expr(u)

    def borrowed(self, n):
        """A position that only READS a vector/string (receiver, operand)."""
        u = self.strip_tmp(n)
        if u.get("valueCategory") == "lvalue":
            return self.place(u)
        return self.class_expr(u)

    def place(self, n):
        """A local variable, `v[i]`, `v.front()` / `v.back()` (desugared)."""
        u = self.strip_tmp(n)
        L = self.loc(u)
        name = self.local_ref(u)
        if name is not None:
            return K.Var(name, L)
        k = u.get("kind")
        if k == "DeclRefExpr":
            return self.expr(u)
        if k == "MemberExpr":
            return self.member_access(u)
        if self.is_this_deref(u):
            return K.Var("self", L)
        if k == "UnaryOperator" and u.get("opcode") == "*":
            return self.expr(u)  # FR-261: `*p` (or the old "dereference" residual)
        if k == "ArraySubscriptExpr":
            return self.subscript(u)
        if k == "CXXOperatorCallExpr" and self.op_name(u) == "operator[]":
            e = self.operator_expr(u)
            if isinstance(e, K.ByteAt):
                return self.rexpr(u, "reference to a std::string element")
            return e
        if k == "CXXMemberCallExpr":
            me = self.strip(u["inner"][0])
            if me.get("kind") == "MemberExpr" and me.get("name") in ("front", "back"):
                return self.member_expr(u)
        if k == "ConditionalOperator":
            # FR-259: a `?:` is a value; as a place (`&(c ? x : y)`, a copy of
            # `c ? s : t`) it would be one of two objects
            e = self.conditional(u)
            if isinstance(e, K.ResidualExpr):
                return e
            return self.rexpr(u, "conditional operator `?:` used as an lvalue")
        return self.rexpr(u, "copy of something other than a variable or element")

    def op_name(self, n):
        return self.callee_decl(n).get("name", "")

    def to_string(self, n):
        a = self.expr(n["inner"][1])
        t = self.ty_of(a)
        if t is not None and t not in K.ARITH_INT:
            return self.rexpr(n, "std::to_string of '%s' (only int, long long, unsigned, unsigned long long, "
                                 "std::size_t)" % (self.qual(self.strip(n["inner"][1]).get("type")) or t))
        return K.ToStr(a, self.loc(n))

    def c_str_base(self, n):
        u = self.strip_tmp(n)
        if u.get("kind") != "CXXMemberCallExpr":
            return None
        me = self.strip(u["inner"][0])
        if me.get("kind") == "MemberExpr" and me.get("name") == "c_str" and not me.get("isArrow"):
            return me["inner"][0]
        if me.get("kind") == "MemberExpr" and me.get("name") == "c_str" and self.ptr_var(me["inner"][0]) is not None:
            # FR-261: `p->c_str()` (a std::string through a pointer)
            L = self.loc(me)
            return K.Index(self.ptr_var(me["inner"][0]), K.IntLit(0, K.USIZE, L), L)
        return None

    def back_front(self, recv, which, n):
        L = self.loc(n)
        if not TC.is_place(recv) or any(isinstance(x, K.Call) for x in K.walk(recv)):
            return self.rexpr(n, "%s() of something other than a variable or element" % which)
        if which == "front":
            return K.Index(recv, K.IntLit(0, K.USIZE, L), L)
        return K.Index(recv, K.Binary("sub", K.Len(recv, L), K.IntLit(1, K.USIZE, L), L), L)

    def member_call_parts(self, n):
        me = self.strip(n["inner"][0])
        if me.get("kind") != "MemberExpr":
            return None, None, None, self.rexpr(n, "member function call")
        if me.get("isArrow"):
            base = self.strip(me["inner"][0])
            if base.get("kind") == "CXXThisExpr" and self.cur is not None and self.cur[1] in ("ref", "mut"):
                return me.get("name"), K.Var("self", self.loc(me)), self.real_args(n)[1:], None
            p = self.ptr_var(base)
            if p is not None:
                # FR-261 (N16): `p->m(..)` is `p[0].m(..)`
                L = self.loc(me)
                return me.get("name"), K.Index(p, K.IntLit(0, K.USIZE, L), L), self.real_args(n)[1:], None
            return None, None, None, self.rexpr(n, "member access through a pointer")
        recv = self.borrowed(me["inner"][0])
        if isinstance(recv, K.ResidualExpr):
            return None, None, None, recv
        return me.get("name"), recv, self.real_args(n)[1:], None

    def member_expr(self, n):
        L = self.loc(n)
        name, recv, args, bad = self.member_call_parts(n)
        if bad is not None:
            return bad
        rt = self.ty_of(recv)
        if isinstance(rt, K.StructT):
            mid = self.strip(n["inner"][0]).get("referencedMemberDecl")
            mpath = self.method_ids.get(mid, "")
            if mpath is None:
                return K.ResidualExpr("use of a rejected member function", L)
            if not mpath:
                return self.rexpr(n, "member function call")
            if self.sigs.recv.get(mpath) not in ("ref", "mut"):
                return self.rexpr(n, "static member function called through an object")
            if not TC.is_place(recv):
                return self.rexpr(n, "member function call on a temporary")
            a = self.import_args(mpath, args)
            if isinstance(a, K.ResidualExpr):
                return a
            return K.MethodCall(recv, name, tuple(a), L)
        if isinstance(rt, K.VecT):
            if name == "size" and not args:
                return K.Len(recv, L)
            if name == "empty" and not args:
                return K.IsEmpty(recv, L)
            if name in ("front", "back") and not args:
                return self.back_front(recv, name, n)
            if name == "at":
                return self.rexpr(n, "std::vector::at (bounds-checked access that throws; the subset has "
                                     "operator[])")
            if name == "data" and not args:
                # FR-261: `v.data()` <=> `&v[..]` (the elements as a slice)
                if not TC.is_place(recv) or any(isinstance(x, K.Call) for x in K.walk(recv)):
                    return self.rexpr(n, "std::vector::data of something other than a variable or element")
                return K.Offset(recv, None, L)
            if name in ("begin", "end", "cbegin", "cend", "rbegin", "rend", "data", "insert", "erase"):
                return self.rexpr(n, "std::vector::%s (iterators and pointers are outside the subset)" % name)
            if name in ("push_back", "pop_back", "clear", "resize"):
                return self.rexpr(n, "std::vector::%s used as a value" % name)
            return self.rexpr(n, "std::vector::%s" % name)
        if rt == K.STRING:
            if name in ("size", "length") and not args:
                return K.Len(recv, L)
            if name == "empty" and not args:
                return K.IsEmpty(recv, L)
            if name == "substr" and len(args) in (1, 2):
                return K.Substr(recv, self.expr(args[0]), self.expr(args[1]) if len(args) == 2 else None, L)
            if name == "c_str":
                return self.rexpr(n, "std::string::c_str outside a printf %s argument")
            return self.rexpr(n, "std::string::%s" % name)
        return self.rexpr(n, "member function call")

    def member_stmt(self, n):
        L = self.loc(n)
        name, recv, args, bad = self.member_call_parts(n)
        if bad is not None:
            return K.ResidualStmt(bad.reason, bad.loc)
        rt = self.ty_of(recv)
        if isinstance(rt, K.VecT):
            if name == "push_back" and len(args) == 1:
                v = self.copied(args[0], rt.elem) if K.is_copy(rt.elem) else self.consume(args[0], rt.elem)
                return K.Push(recv, v, L)
            if name == "pop_back" and not args:
                return K.Pop(recv, L)
            if name == "clear" and not args:
                return K.Clear(recv, L)
            if name == "resize":
                if len(args) != 2:
                    return self.rstmt(n, "std::vector::resize without a fill value")
                return K.Resize(recv, self.expr(args[0]), self.copied(args[1], rt.elem), L)
        if rt == K.STRING:
            if name == "push_back" and len(args) == 1:
                return K.PushByte(recv, self.expr(args[0]), L)
            if name == "clear" and not args:
                return K.Clear(recv, L)
        e = self.member_expr(n)
        if isinstance(e, K.MethodCall):
            return K.ExprStmt(e, L)
        if isinstance(e, K.ResidualExpr):
            return K.ResidualStmt(e.reason, e.loc)
        return self.rstmt(n, "expression statement without effect")

    def str_operand(self, a, allow_lit):
        v, bad = self.literal_value(a)
        if bad is not None:
            return bad
        if v is not None:
            if not allow_lit:
                return self.rexpr(a, "ordering comparison of a std::string with a string literal")
            return K.StrLit(v, self.loc(a))
        return self.borrowed(a)

    CMP_OPS = {"operator==": "eq", "operator!=": "ne", "operator<": "lt", "operator<=": "le",
               "operator>": "gt", "operator>=": "ge"}

    def operator_expr(self, n):
        L = self.loc(n)
        name = self.op_name(n)
        args = n.get("inner", [])[1:]
        if name == "operator()":
            return self.closure_call(n, args)
        if name in ("operator==", "operator!=") and len(args) == 2:
            found = self.find_compare(n, name, args)  # FR-253
            if found is not None:
                return found
        if name == "operator*" and len(args) == 1 and self.std_name(self.strip_tmp(args[0])) in (
                "max_element", "min_element"):
            return self.extreme(self.strip_tmp(args[0]))  # FR-253
        if name == "operator[]" and len(args) == 2:
            base = self.borrowed(args[0])
            bt = self.ty_of(base)
            if isinstance(base, K.ResidualExpr):
                return base
            if isinstance(bt, K.VecT):
                return K.Index(base, self.expr(args[1]), L)
            if bt == K.STRING:
                return K.ByteAt(base, self.expr(args[1]), L)
            return self.rexpr(n, "overloaded operator call")
        if name in self.CMP_OPS and len(args) == 2:
            op = self.CMP_OPS[name]
            a = self.str_operand(args[0], op in ("eq", "ne"))
            b = self.str_operand(args[1], op in ("eq", "ne"))
            for x in (a, b):
                if isinstance(x, K.ResidualExpr):
                    return x
            ta, tb = self.ty_of(a), self.ty_of(b)
            if isinstance(ta, K.VecT) and op not in ("eq", "ne"):
                return self.rexpr(n, "ordering comparison of vectors")
            if ta == K.STR and tb == K.STR:
                return self.rexpr(n, "comparison of two string literals")
            return K.Binary(op, a, b, L)
        if name == "operator+" and len(args) == 2:
            v, bad = self.literal_value(args[0])
            if bad is not None:
                return bad
            if v is not None:
                lhs = K.StrFrom(v, self.loc(args[0]))
            else:
                if self.map_type(self.strip_tmp(args[0]).get("type"))[0] == K.I8:
                    return self.rexpr(n, "char + std::string")
                lhs = self.consume(args[0], K.STRING)
            if self.map_type(self.strip_tmp(args[1]).get("type"))[0] == K.I8:
                return self.rexpr(n, "std::string + char")
            rhs = self.str_operand(args[1], True)
            for x in (lhs, rhs):
                if isinstance(x, K.ResidualExpr):
                    return x
            return K.Binary("add", lhs, rhs, L)
        if name in ("operator=", "operator+="):
            return self.rexpr(n, "assignment used as a value")
        for a in args:
            # name the out-of-subset operand (an iterator from v.begin()) rather than the operator
            u = self.strip_tmp(a)
            if u.get("kind") == "CXXMemberCallExpr":
                e = self.member_expr(u)
                if isinstance(e, K.ResidualExpr):
                    return e
        return self.rexpr(n, "overloaded operator call")

    def elem_target(self, n):
        """Index for an assignable `v[i]` / `v.back()` (FR-257: Field for
        `p.f` / `this->f`), a ResidualStmt, or None."""
        u = self.strip_tmp(n)
        k = u.get("kind")
        if k == "ConditionalOperator":
            return self.rstmt(u, "conditional operator `?:` used as an lvalue")  # FR-259
        if k == "MemberExpr":
            e = self.member_access(u)
            if isinstance(e, K.ResidualExpr):
                return K.ResidualStmt(e.reason, e.loc)
            return e
        if (k == "UnaryOperator" and u.get("opcode") == "*" and not self.is_this_deref(u)) \
                or k == "ArraySubscriptExpr":
            # FR-261: a store through a pointer, `*p = e` / `p[i] = e`
            e = self.expr(u)
            if isinstance(e, K.ResidualExpr):
                return K.ResidualStmt(e.reason, e.loc)
            return e
        if k == "CXXOperatorCallExpr" and self.op_name(u) == "operator[]":
            e = self.operator_expr(u)
            if isinstance(e, K.ByteAt):
                return self.rstmt(u, "assignment to a std::string element (Rust has no safe String byte "
                                     "write)")
            if isinstance(e, K.ResidualExpr):
                return K.ResidualStmt(e.reason, e.loc)
            return e
        if k == "CXXMemberCallExpr":
            e = self.member_expr(u)  # front()/back() desugar to an Index; v.at(i) is named
            if isinstance(e, K.ResidualExpr):
                return K.ResidualStmt(e.reason, e.loc)
            if isinstance(e, K.Index):
                return e
        return None

    def operator_stmt(self, n):
        L = self.loc(n)
        name = self.op_name(n)
        args = n.get("inner", [])[1:]
        if name in ("operator=", "operator+=") and len(args) == 2:
            lname = self.local_ref(args[0])
            if lname is None and name == "operator=" and self.is_this_deref(self.strip_tmp(args[0])):
                lname = "self"  # FR-257: `*this = e;` <=> `*self = e;`
            tgt = K.Var(lname, self.loc(args[0])) if lname is not None else self.elem_target(args[0])
            if isinstance(tgt, K.ResidualStmt):
                return tgt
            if tgt is None:
                return self.rstmt(n, "assignment to something other than a local variable")
            tt = self.ty_of(tgt)
            if name == "operator=":
                v, bad = self.literal_value(args[1])
                if bad is not None:
                    return K.ResidualStmt(bad.reason, bad.loc)
                val = K.StrFrom(v, self.loc(args[1])) if v is not None else self.consume(args[1], tt)
                if isinstance(tgt, K.Var):
                    return K.Assign(tgt.name, val, L)
                return K.Store(tgt, val, L)
            if tt != K.STRING:
                return self.rstmt(n, "overloaded operator call")
            if self.map_type(self.strip_tmp(args[1]).get("type"))[0] == K.I8:
                return K.PushByte(tgt, self.expr(args[1]), L)
            val = self.str_operand(args[1], True)
            if isinstance(val, K.ResidualExpr):
                return K.ResidualStmt(val.reason, val.loc)
            return K.PushStr(tgt, val, L)
        e = self.operator_expr(n)
        if isinstance(e, K.ResidualExpr):
            return K.ResidualStmt(e.reason, e.loc)
        if isinstance(e, K.ClosureCall):
            return K.ExprStmt(e, L)
        return self.rstmt(n, "expression statement without effect")

    def class_direct_init(self, d, ty):
        """`std::vector<T> v;` / `std::vector<T> v(n, x);` / `std::string s;`."""
        L = self.loc(d)
        c = self.unwrap_ctor((d.get("inner") or [{}])[0])
        if c.get("kind") != "CXXConstructExpr":
            return K.ResidualStmt("call-initialization (the subset spells it `T x = e;`)", L)
        kind, args = self.ctor_kind(c)
        if kind == "default" or (kind == "fill" and isinstance(ty, K.VecT)):
            e = self.class_expr(c)
            return e
        if isinstance(ty, K.VecT) and len(args) == 1 and kind == "other":
            return K.ResidualStmt("std::vector(n) without a fill value", L)
        return K.ResidualStmt("call-initialization (the subset spells it `T x = e;`)", L)

    def range_for(self, n):
        L = self.loc(n)
        inner = n.get("inner", [])
        if len(inner) < 3:
            return self.rstmt(n, "range-based for loop")
        if inner[0] and inner[0].get("kind"):
            return self.rstmt(n, "range-based for with an init-statement")
        rng = None
        for c in inner:
            if c.get("kind") == "DeclStmt":
                vd = c.get("inner", [{}])[0]
                if (vd.get("name") or "").startswith("__range"):
                    rng = vd
                    break
        lv_stmt, body = inner[-2], inner[-1]
        if rng is None or lv_stmt.get("kind") != "DeclStmt":
            return self.rstmt(n, "range-based for loop")
        vd = lv_stmt["inner"][0]
        head = self.text_at(vd).lstrip()
        if head.startswith("const "):
            head = head[len("const "):].lstrip()
        # FR-259 (N22): an `auto` / `const auto&` / `auto&` loop variable has
        # clang's deduced type, checked against the element type below
        it = self.borrowed(rng["inner"][0])
        if isinstance(it, K.ResidualExpr):
            return K.ResidualStmt(it.reason, it.loc)
        if not TC.is_place(it):
            return self.rstmt(n, "range-based for over something other than a variable or element")
        t = self.ty_of(it)
        if t == K.STRING:
            return self.rstmt(n, "range-based for over a std::string")
        if not isinstance(t, K.VecT):
            return self.rstmt(n, "range-based for loop")
        vt, const, spelled = self.map_type(vd.get("type"))
        name = vd.get("name")
        if isinstance(vt, K.RefT):
            mode, mutable = ("mut" if vt.mutable else "ref"), False
        else:
            mode, mutable = "copy", not const
        if vt is None or K.deref(vt) != t.elem:
            return K.ResidualStmt("range-for variable of type '%s' over a %s" % (spelled, t), self.loc(vd))
        saved = self.scope
        self.scope = saved.child()
        self.scope.declare(name, vt if mode != "copy" else t.elem, mutable)
        self.locals[vd["id"]] = name
        try:
            if body.get("kind") == "CompoundStmt":
                out = []
                for st in body.get("inner", []):
                    out.extend(self.stmt(st))
                blk = K.Block(tuple(out), self.loc(body))
            else:
                blk = K.Block(tuple(self.stmt(body)), self.loc(body))
        finally:
            self.scope = saved
        return K.ForEach(name, mutable, mode, it, blk, L)


    # ================================================== FR-252: closures
    def lambda_parts(self, lam):
        """(closure record, operator() decl or None, capture-init exprs, body)."""
        kids = lam.get("inner", [])
        rec = next((c for c in kids if c.get("kind") == "CXXRecordDecl"), {})
        body = kids[-1] if kids and kids[-1].get("kind") == "CompoundStmt" else None
        inits = [c for c in kids[1:-1]] if body is not None else []
        op = next((m for m in rec.get("inner", []) if m.get("name") == "operator()"
                   and m.get("kind") in ("CXXMethodDecl", "FunctionTemplateDecl")), None)
        return rec, op, inits, body

    def lambda_sig(self, op):
        """(param Kernel types or None, ret or None, spelled ret) of a
        closure's operator(); clang prints `auto (int) const -> int` for a
        written trailing return type and `int (int) const` for a deduced one."""
        q = self.qual(op.get("type")) or ""
        if ") const ->" in q or ") ->" in q or ") mutable ->" in q:
            rs = q.rsplit("->", 1)[1].strip()
        else:
            rs = q[:q.find("(")].strip() if "(" in q else q
        ret = TYPE_MAP.get(rs) or parse_cxx_type(rs, self.struct_written, self.td_ok)[0]
        ptys = [self.map_type(p.get("type")) for p in op.get("inner", []) if p.get("kind") == "ParmVarDecl"]
        return ptys, ret, rs

    def capture_list(self, lam, rec, inits):
        """The explicit list a default capture stands for, read off the
        closure record: one field per capture, in capture order (a
        reference-typed field is a by-reference capture), named by its
        initializer."""
        fields = [f for f in rec.get("inner", []) if f.get("kind") == "FieldDecl"]
        out = []
        for f, e in zip(fields, inits):
            u = self.strip(e)
            while isinstance(u, dict) and u.get("kind") in ("CXXConstructExpr", "ImplicitCastExpr") and u.get("inner"):
                u = self.strip(u["inner"][0])
            if isinstance(u, dict) and u.get("kind") == "CXXThisExpr":
                out.append("this")
                continue
            nm = (u.get("referencedDecl") or {}).get("name", "?") if isinstance(u, dict) else "?"
            ref = (self.qual(f.get("type")) or "").rstrip().endswith("&")
            out.append(("&" if ref else "") + nm)
        return "[%s]" % ", ".join(out)

    def closure_decl(self, d, L, ty):
        """`auto f = [caps](params) -> R { body };` (FR-252)."""
        name = d.get("name")
        lam = self.strip_tmp(self.strip((d.get("inner") or [{}])[0]))
        if lam.get("kind") != "LambdaExpr":
            return K.ResidualStmt("copy of a closure (`auto g = f;` copies its state in C++, Rust would move "
                                  "it; the subset only calls a closure)", L)
        if d.get("init") != "c":
            return K.ResidualStmt("%s-initialization of a closure (the subset spells it `auto f = [..](..) "
                                  "{..};`)" % d.get("init"), L)
        if self.in_closure is not None:
            return K.ResidualStmt("lambda inside a lambda body (nested closures are not in the subset)", L)
        rec, op, inits, body = self.lambda_parts(lam)
        if op is None or body is None:
            return K.ResidualStmt("lambda expression", L)
        if op.get("kind") == "FunctionTemplateDecl":
            return K.ResidualStmt("generic lambda (an `auto` parameter)", self.loc(lam))
        rg = lam.get("range") or {}
        off = _off(rg.get("begin"))
        text = self.source[off:off + 4000].decode("utf-8", "replace") if off is not None else ""
        caps = _split_captures(text)
        LL = self.loc(lam)
        if caps is None:
            return K.ResidualStmt("lambda expression", LL)
        written = "[%s]" % ", ".join(caps)
        kinds = []
        for c in caps:
            cc = c.replace(" ", "")
            if cc in ("=", "&"):
                return K.ResidualStmt("default capture `%s` (normalizer: list the captures explicitly: `%s`)"
                                      % (written, self.capture_list(lam, rec, inits)), LL)
            if cc in ("this", "*this"):
                return K.ResidualStmt("capture of `%s` (a closure in the subset captures local variables only)"
                                      % cc, LL)
            if "=" in cc or "(" in cc or "{" in cc:
                return K.ResidualStmt("init-capture `%s` (normalizer: declare it as a local before the lambda "
                                      "and capture that)" % c, LL)
            if cc.endswith("..."):
                return K.ResidualStmt("capture pack `%s`" % c, LL)
            ref = cc.startswith("&")
            nm = cc[1:] if ref else cc
            if not _IDENT_RE.match(nm):
                return K.ResidualStmt("capture `%s`" % c, LL)
            kinds.append(ref)
        if kinds and any(kinds) and not all(kinds):
            return K.ResidualStmt("mixed by-copy and by-reference captures `%s` (normalizer: copy each by-copy "
                                  "capture into a local before the lambda and capture everything by reference)"
                                  % written, LL)
        move = bool(kinds) and not any(kinds)
        ptys, ret, rs = self.lambda_sig(op)
        if ret is None or not (ret in K.ALL_PRIMS or ret == K.UNIT):
            return K.ResidualStmt("closure returning '%s' (the subset's closures return a scalar or nothing)"
                                  % rs, LL)
        pre = []
        params = []
        saved = (self.scope, getattr(self, "ret", None), getattr(self, "in_main", False),
                 getattr(self, "ret_ref", False), self.in_closure)
        self.scope = self.scope.child()
        try:
            pnodes = [p for p in op.get("inner", []) if p.get("kind") == "ParmVarDecl"]
            for p, (pty, const, spelled) in zip(pnodes, ptys):
                pl = self.lines.at(_off(p.get("loc")))
                pname = p.get("name")
                if not pname:
                    return K.ResidualStmt("unnamed closure parameter", pl)
                if pty not in K.ALL_PRIMS:
                    return K.ResidualStmt("closure parameter of type '%s' (the subset's closures take scalars)"
                                          % spelled, pl)
                if p.get("init") or p.get("inner"):
                    return K.ResidualStmt("default argument", pl)
                params.append(K.Param(pname, pty, not const, pl))
                self.scope.declare(pname, pty, not const)
                self.locals[p["id"]] = pname
            self.ret, self.in_main, self.ret_ref, self.in_closure = ret, False, False, name
            stmts = []
            for st in body.get("inner", []):
                stmts.extend(self.stmt(st))
        finally:
            self.scope, self.ret, self.in_main, self.ret_ref, self.in_closure = saved
        self.scope.declare(name, K.FnT(tuple(p.ty for p in params), ret), False)
        self.locals[d["id"]] = name
        return K.Closure(name, move, tuple(params), ret, K.Block(tuple(pre + stmts), self.loc(body)), L)

    def closure_call(self, n, args):
        """`f(a, b)` of a local closure / callable parameter (`operator()`)."""
        L = self.loc(n)
        if not args:
            return self.rexpr(n, "overloaded operator call")
        name = self.local_ref(args[0])
        v = self.scope.lookup(name) if name is not None else None
        if v is None or not isinstance(K.deref(v[0]), K.FnT):
            obj = self.strip_tmp(self.strip(args[0]))
            if obj.get("kind") == "LambdaExpr":
                return self.rexpr(obj, friendly("LambdaExpr"))  # called in place
            if obj.get("kind") == "DeclRefExpr" and (obj.get("referencedDecl") or {}).get("id") in self.poisoned:
                return K.ResidualExpr("use of a rejected declaration", L, cascade=True)
            return self.rexpr(n, "call of a callable object other than a local closure or callable parameter")
        out = []
        for a in args[1:]:
            if a.get("kind") == "CXXDefaultArgExpr":
                return self.rexpr(a, "default argument")
            out.append(self.expr(a))
        return K.ClosureCall(name, tuple(out), L)

    # ================================================== FR-253: algorithms
    ALG_STMT_NAMES = ("sort", "stable_sort", "reverse", "fill", "for_each", "transform")
    ALG_VALUE_NAMES = ("count", "count_if", "any_of", "all_of", "none_of", "accumulate", "max", "min")

    def std_ids(self):
        """Ids of the functions (and their template specializations) declared
        DIRECTLY in namespace std in a header: clang's referencedDecl carries
        no namespace (FR-249 spike (c)), and a user `sort` must never be
        mistaken for std::sort, nor std::ranges::sort for it."""
        if self._std is None:
            out = set()
            for n in self.ast.get("inner", []):
                if n.get("kind") != "NamespaceDecl" or n.get("name") != "std" or self.decl_main.get(n.get("id"), True):
                    continue
                for c in n.get("inner", []):
                    if c.get("kind") == "FunctionDecl":
                        out.add(c.get("id"))
                    elif c.get("kind") == "FunctionTemplateDecl":
                        out.add(c.get("id"))
                        out.update(x.get("id") for x in c.get("inner", []) if x.get("kind") == "FunctionDecl")
            self._std = out
        return self._std

    def std_name(self, n):
        """The name of the namespace-std function CallExpr ``n`` calls, or None."""
        if not isinstance(n, dict) or n.get("kind") != "CallExpr":
            return None
        rd = self.callee_decl(n)
        rid = rd.get("id")
        if rid is None or rid in self.user_funcs or rid not in self.std_ids():
            return None
        return rd.get("name")

    def whole(self, n, b, e, what):
        """(place, None) for `v.begin(), v.end()` on ONE vector place, else
        (None, a located residual)."""
        free = self.rexpr(n, "%s over something other than a whole container `v.begin(), v.end()` (free "
                             "iterators are outside the subset)" % what)
        parts = []
        for node, want in ((b, "begin"), (e, "end")):
            u = self.strip_tmp(node)
            if u.get("kind") != "CXXMemberCallExpr":
                return None, free
            me = self.strip(u["inner"][0])
            if me.get("kind") != "MemberExpr" or me.get("name") != want:
                return None, free
            _, recv, margs, bad = self.member_call_parts(u)
            if bad is not None:
                return None, bad
            if margs:
                return None, free
            parts.append(recv)
        if parts[0] != parts[1]:
            return None, self.rexpr(n, "%s over the begin() and end() of two different containers" % what)
        recv = parts[0]
        if not TC.is_place(recv) or any(isinstance(x, (K.Call, K.MethodCall)) for x in K.walk(recv)):
            return None, self.rexpr(n, "%s over something other than a variable, element or data member" % what)
        t = self.ty_of(recv)
        if t == K.STRING:
            return None, self.rexpr(n, "%s over a std::string (the subset's algorithms take a std::vector)" % what)
        if not isinstance(t, K.VecT):
            return None, self.rexpr(n, "%s over a container that is not a std::vector" % what)
        return recv, None

    def alg_closure(self, a, what):
        """A callable argument: a LOCAL closure variable (C++ copies the
        closure object into the algorithm; FR-252's lambdas are only ever
        bound by `auto f = ..;`, so an inline lambda is refused, naming the
        rewrite)."""
        u = self.strip_tmp(a)
        while isinstance(u, dict) and u.get("kind") == "CXXConstructExpr" and len(self.real_args(u)) == 1:
            u = self.strip_tmp(self.real_args(u)[0])
        u = self.strip(u)
        if u.get("kind") == "LambdaExpr":
            return self.rexpr(u, "lambda expression as an argument of %s (normalizer: bind it to a local closure "
                                 "first, `auto f = [..](..) {..};`, and pass `f`)" % what)
        name = self.local_ref(u)
        if name is not None:
            return K.Var(name, self.loc(u))
        rd = (u.get("referencedDecl") or {}) if u.get("kind") == "DeclRefExpr" else {}
        if rd.get("id") in self.poisoned:
            return K.ResidualExpr("use of a rejected declaration", self.loc(a), cascade=True)
        while u.get("kind") == "ImplicitCastExpr" and u.get("castKind") == "FunctionToPointerDecay":
            u = self.strip(u["inner"][0])
            rd = (u.get("referencedDecl") or {}) if u.get("kind") == "DeclRefExpr" else {}
        if rd.get("kind") == "FunctionDecl":
            return self.rexpr(a, "function '%s' passed to %s (the subset passes a local closure)"
                              % (rd.get("name"), what))
        return self.rexpr(a, "the callable argument of %s is not a local closure variable" % what)

    def alg_elem_value(self, a, E, what):
        """The value `std::find` / `std::count` compares each element with.
        C++ deduces its type from the argument and compares after the usual
        arithmetic conversions; the Kernel compares values of the ELEMENT
        type, so a literal that fits the element type is converted (the
        comparison is the same either way) and any other type is left for
        the checker to refuse, located."""
        if E == K.STRING:
            return self.borrowed(a)
        e = self.expr(self.strip_tmp(a))
        t = self.ty_of(e)
        if t is None or t == E or isinstance(e, K.ResidualExpr):
            return e
        lit, neg = (e.operand, True) if isinstance(e, K.Unary) and e.op == "neg" else (e, False)
        if isinstance(lit, K.IntLit) and (E in K.INT_TYPES or E == K.F64):
            v = -lit.value if neg else lit.value
            lo, hi = K.INT_RANGE[E.name] if E != K.F64 else (-(1 << 53), 1 << 53)
            if lo <= v <= hi:
                return K.Cast(E, e, self.loc(a))
        return e

    def alg_stmt(self, n):
        """A namespace-std algorithm evaluated for its effect, or None."""
        name = self.std_name(n)
        if name not in self.ALG_STMT_NAMES:
            return None
        L = self.loc(n)
        what = "std::" + name
        args = self.real_args(n)[1:]

        def residual(e):
            return K.ResidualStmt(e.reason, e.loc)

        need = {"sort": (2, 3), "stable_sort": (2,), "reverse": (2,), "fill": (3,), "for_each": (3,),
                "transform": (4,)}[name]
        if len(args) not in need:
            if name == "stable_sort" and len(args) == 3:
                return self.rstmt(n, "std::stable_sort with a comparator")
            if name == "transform" and len(args) == 5:
                return self.rstmt(n, "std::transform over two ranges (a binary operation)")
            return self.rstmt(n, "%s with %d arguments" % (what, len(args)))
        v, bad = self.whole(n, args[0], args[1], what)
        if bad is not None:
            return residual(bad)
        E = self.ty_of(v).elem
        if name in ("sort", "stable_sort", "reverse") and len(args) == 2:
            return K.AlgStmt(name, v, None, (), L)
        if name == "sort":
            f = self.alg_closure(args[2], what)
            return residual(f) if isinstance(f, K.ResidualExpr) else K.AlgStmt("sort_by", v, None, (f,), L)
        if name == "fill":
            if isinstance(E, K.Prim):
                x = self.expr(self.strip_tmp(args[2]))
                t = self.ty_of(x)
                if t is not None and t != E and t in K.ALL_PRIMS:
                    x = K.Cast(E, x, self.loc(args[2]))  # `*it = value` converts to the element type
            else:
                x = self.copied(args[2], E)
            if isinstance(x, K.ResidualExpr):
                return residual(x)
            return K.AlgStmt("fill", v, None, (x,), L)
        if name == "for_each":
            f = self.alg_closure(args[2], what)
            return residual(f) if isinstance(f, K.ResidualExpr) else K.AlgStmt("for_each", None, v, (f,), L)
        # transform: in place (`v.begin()` again) or appended (`std::back_inserter(w)`)
        out = self.strip_tmp(args[2])
        f = self.alg_closure(args[3], what)
        if isinstance(f, K.ResidualExpr):
            return residual(f)
        if out.get("kind") == "CXXMemberCallExpr":
            me = self.strip(out["inner"][0])
            if me.get("kind") == "MemberExpr" and me.get("name") == "begin":
                _, w, margs, bad = self.member_call_parts(out)
                if bad is None and not margs and w == v:
                    return K.AlgStmt("transform", v, None, (f,), L)
        while isinstance(out, dict) and out.get("kind") == "CXXConstructExpr" and len(self.real_args(out)) == 1:
            out = self.strip_tmp(self.real_args(out)[0])
        if self.std_name(out) == "back_inserter" and len(self.real_args(out)) == 2:
            w = self.place(self.real_args(out)[1])
            if isinstance(w, K.ResidualExpr):
                return residual(w)
            if not isinstance(self.ty_of(w), K.VecT):
                return self.rstmt(n, "std::transform into a std::back_inserter of a container that is not a "
                                     "std::vector")
            return K.AlgStmt("extend_map", w, v, (f,), L)
        return self.rstmt(n, "std::transform into something other than its own begin() or a "
                             "std::back_inserter(w) (free iterators are outside the subset)")

    def alg_value(self, n):
        """A namespace-std algorithm whose value is used (FR-253), a located
        residual for one outside the subset, or None (not an algorithm)."""
        name = self.std_name(n)
        if name is None:
            return None
        L = self.loc(n)
        what = "std::" + name
        args = self.real_args(n)[1:]
        if name in self.ALG_STMT_NAMES:
            return self.rexpr(n, "%s used as a value" % what)
        if name == "find":
            return self.rexpr(n, "std::find other than `std::find(v.begin(), v.end(), x) != v.end()` or `== "
                                 "v.end()` (iterators are outside the subset)")
        if name in ("max_element", "min_element"):
            return self.rexpr(n, "%s other than `*%s(v.begin(), v.end())` (iterators are outside the subset)"
                              % (what, what))
        if name in ("max", "min"):
            if len(args) != 2:
                return self.rexpr(n, "%s with %s" % (what, "a comparator" if len(args) == 3 else
                                                     "%d arguments" % len(args)))
            a = self.expr(self.strip_tmp(args[0]))
            b = self.expr(self.strip_tmp(args[1]))
            return K.MinMax(name, a, b, L)
        if name not in self.ALG_VALUE_NAMES:
            return None
        need = {"accumulate": (3, 4)}.get(name, (3,))
        if len(args) not in need:
            return self.rexpr(n, "%s with %d arguments" % (what, len(args)))
        v, bad = self.whole(n, args[0], args[1], what)
        if bad is not None:
            return bad
        E = self.ty_of(v).elem
        if name == "count":
            x = self.alg_elem_value(args[2], E, what)
            return x if isinstance(x, K.ResidualExpr) else K.Alg("count", v, (x,), L)
        if name == "accumulate":
            init = self.expr(self.strip_tmp(args[2]))
            if isinstance(init, K.ResidualExpr):
                return init
            if len(args) == 3:
                return K.Alg("accumulate", v, (init,), L)
            f = self.alg_closure(args[3], what)
            return f if isinstance(f, K.ResidualExpr) else K.Alg("accumulate", v, (init, f), L)
        f = self.alg_closure(args[2], what)
        if isinstance(f, K.ResidualExpr):
            return f
        if name == "none_of":
            return K.Unary("not", K.Alg("any_of", v, (f,), L), L)  # N25: none_of is !any_of
        return K.Alg(name, v, (f,), L)

    def find_compare(self, n, opname, args):
        """`std::find(v.begin(), v.end(), x) != v.end()` <=> contains (`==`:
        its negation, N26); either operand order.  None when neither operand
        is a std::find call."""
        a, b = (self.strip_tmp(x) for x in args)
        if self.std_name(b) == "find" and self.std_name(a) != "find":
            a, b = b, a
        if self.std_name(a) != "find":
            return None
        L = self.loc(n)
        fargs = self.real_args(a)[1:]
        if len(fargs) != 3:
            return self.rexpr(a, "std::find with %d arguments" % len(fargs))
        v, bad = self.whole(a, fargs[0], fargs[1], "std::find")
        if bad is not None:
            return bad
        end_ok = False
        if b.get("kind") == "CXXMemberCallExpr":
            me = self.strip(b["inner"][0])
            if me.get("kind") == "MemberExpr" and me.get("name") == "end":
                _, w, margs, wbad = self.member_call_parts(b)
                end_ok = wbad is None and not margs and w == v
        if not end_ok:
            return self.rexpr(n, "std::find compared with something other than the same container's end() "
                                 "(iterators are outside the subset)")
        x = self.alg_elem_value(fargs[2], self.ty_of(v).elem, "std::find")
        if isinstance(x, K.ResidualExpr):
            return x
        c = K.Alg("contains", v, (x,), L)
        return c if opname == "operator!=" else K.Unary("not", c, L)

    def extreme(self, call):
        """`*std::max_element(v.begin(), v.end())` (dereferenced at once)."""
        name = self.std_name(call)
        args = self.real_args(call)[1:]
        what = "std::" + name
        if len(args) != 2:
            return self.rexpr(call, "%s with a comparator" % what if len(args) == 3 else
                              "%s with %d arguments" % (what, len(args)))
        v, bad = self.whole(call, args[0], args[1], what)
        if bad is not None:
            return bad
        return K.Alg(name, v, (), self.loc(call))

    def template_func(self, n, ns, internal, funcs):
        """FR-252: `template <typename F> R g(const F& f, ..)` (or `F& f`)
        with exactly one type parameter used ONLY as that one callable
        parameter <=> `fn g(f: &impl Fn(..) -> R, ..)` (`&mut impl
        FnMut`).  The closure signature is read off the instantiations,
        which must all be closures of ONE signature.  Returns a residual
        reason, or None after adding the first instantiation to ``funcs``."""
        kids = n.get("inner", [])
        tps = [c for c in kids if c.get("kind") in ("TemplateTypeParmDecl", "NonTypeTemplateParmDecl",
                                                       "TemplateTemplateParmDecl")]
        fds = [c for c in kids if c.get("kind") == "FunctionDecl"]
        if not fds:
            return "function template"
        pat = fds[0]
        insts = fds[1:]
        if len(tps) != 1 or tps[0].get("kind") != "TemplateTypeParmDecl" or tps[0].get("isParameterPack"):
            return "function template (the subset's templates take exactly one type parameter, a callable)"
        F = tps[0].get("name") or ""
        word = re.compile(r"(?<![\w:])%s(?![\w])" % re.escape(F)) if F else None
        pps = [p for p in pat.get("inner", []) if p.get("kind") == "ParmVarDecl"]
        idx = None
        for i, p in enumerate(pps):
            q = (self.qual(p.get("type")) or "").replace(" ", "")
            if word is None or not word.search(self.qual(p.get("type")) or ""):
                continue
            if idx is not None:
                return "function template with more than one parameter of the template type `%s`" % F
            if q == F or q == "const" + F:
                return ("callable parameter taken by value `%s %s` (normalizer: take it as `const %s& %s`, or "
                        "`%s& %s` for a closure that modifies state -- C++ copies a by-value closure and its "
                        "state, Rust would borrow it)" % (F, p.get("name"), F, p.get("name"), F, p.get("name")))
            if q not in ("const" + F + "&", F + "&"):
                return "parameter of type '%s' in a function template" % self.qual(p.get("type"))
            idx = (i, q.startswith("const"))
        rs = self.ret_type_str(pat)
        if idx is None or (word is not None and word.search(rs)):
            return "function template (its type parameter is not exactly one callable parameter)"
        if pat.get("storageClass") or pat.get("inline") or pat.get("constexpr"):
            return "function template with a storage class, inline or constexpr"
        if not insts:
            return "function template that is never instantiated (its callable's signature is unknown)"
        sigs = set()
        for fd in insts:
            targ = next((c for c in fd.get("inner", []) if c.get("kind") == "TemplateArgument"), None)
            rt = next((c for c in (targ or {}).get("inner", []) if c.get("kind") == "RecordType"), None)
            rec = self.decl_info.get(((rt or {}).get("decl") or {}).get("id"))
            if rec is None or "(lambda at " not in (self.qual((targ or {}).get("type")) or ""):
                return "function template instantiated with something other than a closure"
            op = next((m for m in rec.get("inner", []) if m.get("name") == "operator()"), None)
            if op is None or op.get("kind") != "CXXMethodDecl":
                return "function template instantiated with a generic lambda"
            ptys, ret, _ = self.lambda_sig(op)
            if any(p[0] not in K.ALL_PRIMS for p in ptys) or ret is None or not (ret in K.ALL_PRIMS or ret == K.UNIT):
                return "function template instantiated with a closure outside the subset's signatures"
            sigs.add((tuple(p[0] for p in ptys), ret))
        if len(sigs) != 1:
            return ("function template instantiated with closures of different signatures (Rust's `impl Fn` "
                    "names one)")
        (ptys, ret), = sigs
        ft = K.RefT(K.FnT(ptys, ret), not idx[1])
        first = insts[0]
        for fd in insts:
            self.fn_params[fd["id"]] = (idx[0], ft)
            self.user_funcs[fd["id"]] = K.path_of(ns, n.get("name"))
        self.user_funcs[pat["id"]] = K.path_of(ns, n.get("name"))
        self.user_funcs[n["id"]] = K.path_of(ns, n.get("name"))
        funcs.append((first, ns, internal))
        return None


# ================================================== FR-252: closures
def _split_captures(text):
    """The elements of a lambda's written capture list `[..]` (top-level
    commas only), or None when ``text`` does not start with one."""
    if not text.startswith("["):
        return None
    depth = 0
    cur = []
    out = []
    for i, ch in enumerate(text[1:]):
        if ch in "([{":
            depth += 1
        elif ch in ")}":
            depth -= 1
        elif ch == "]":
            if depth == 0:
                tail = "".join(cur).strip()
                if tail or out:
                    out.append(tail)
                return out
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    return None


_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")


def cxx_import(path, clang="clang++", normalize=False, notes=None):
    """Surface IR for the C++ file at ``path`` (may contain Residuals).

    FR-254: ``normalize`` applies normalize.RULES; each applied rule is
    appended to ``notes`` as (loc, "normalized: <rule>")."""
    ast = run_clang(path, clang)
    with open(path, "rb") as f:
        source = f.read()
    imp = CxxImporter(path, ast, source)
    imp.normalize = normalize
    prog = imp.run()
    if notes is not None:
        notes.extend(imp.notes)
    return prog
