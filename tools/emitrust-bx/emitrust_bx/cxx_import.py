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
"""

import bisect
import json
import os
import subprocess

from . import fmt
from . import kernel as K
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


def parse_cxx_type(q):
    """A clang type spelling -> (Kernel type or None, top-level const).

    References become RefT; `const` under a reference makes it shared.
    """
    if q is None:
        return None, False
    q = q.strip()
    if q.endswith("&&"):
        return None, False
    if q.endswith("&"):
        inner, const = parse_cxx_type(q[:-1])
        if inner is None or isinstance(inner, K.RefT):
            return None, False
        return K.RefT(inner, not const), False
    const = False
    if q.startswith("const "):
        const = True
        q = q[len("const "):].strip()
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
        el, econst = parse_cxx_type(args[0])
        if el is None or econst or isinstance(el, K.RefT):
            return None, const
        return K.VecT(el), const
    return None, const


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
}


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
        # FR-251: vectors, strings, references
        kt, c2 = parse_cxx_type(q)
        if kt is None and d:
            kt, c3 = parse_cxx_type(d)
            c2 = c2 or c3
        if kt is not None:
            return kt, const or c2, q
        return None, const, q

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
        for n in mains:
            self.collect(n, (), False, items, funcs)
        # a main-file function redeclaring an earlier (e.g. header) decl:
        # a reference may name either decl id
        for n, ns, internal in funcs:
            prev = n.get("previousDecl")
            if prev and prev not in self.user_funcs:
                self.user_funcs[prev] = K.path_of(ns, n.get("name"))
        # signatures (for typing calls)
        self.sigs = {}
        defined = set()
        for n, ns, internal in funcs:
            if self.has_body(n):
                ptys, ret = self.signature_types(n)
                path = K.path_of(ns, n.get("name"))
                if path != "main":
                    self.sigs.setdefault(path, (ptys, ret))
                defined.add(path)
        for n, ns, internal in funcs:
            if not self.has_body(n):
                if K.path_of(ns, n.get("name")) not in defined:
                    items.append(K.ResidualItem("function declared but not defined", self.loc(n)))
                continue
            items.append(self.func(n, ns, internal))
        # keep source order
        items.sort(key=lambda it: (it.loc.line, it.loc.col) if it.loc else (0, 0))
        return K.Program(tuple(items))

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
            for c in n.get("inner", []):
                self.collect(c, inner_ns, inner_internal, items, funcs)
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
            return
        elif k in ("UsingDecl", "UsingShadowDecl"):
            if self.using_is_user(n):
                return  # `using a::f;` of a user function: spelling only
            if k == "UsingDecl":
                items.append(K.ResidualItem(friendly(k), self.loc(n)))
        elif k == "NamespaceAliasDecl":
            al = n.get("aliasedNamespace", {}).get("id")
            if al in self.ns_index and self.decl_main.get(al):
                return  # an alias of a namespace of this file: spelling only
            items.append(K.ResidualItem("namespace alias of a namespace outside this file", self.loc(n)))
        else:
            items.append(K.ResidualItem(friendly(k), self.loc(n)))

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
        for p in n.get("inner", []):
            if p.get("kind") == "ParmVarDecl":
                ptys.append(self.map_type(p.get("type"))[0])
        rs = self.ret_type_str(n)
        ret = TYPE_MAP.get(rs) or parse_cxx_type(rs)[0]
        return tuple(ptys), ret

    # -------------------------------------------------------------- funcs
    def func(self, n, ns=(), internal=False):
        name = n.get("name")
        floc = self.lines.at(_off(n.get("loc")))
        pre = []
        if n.get("storageClass") in ("static", "extern"):
            pre.append(K.ResidualStmt("%s function" % n["storageClass"], floc))
        if n.get("inline"):
            pre.append(K.ResidualStmt("inline function", floc))
        if n.get("constexpr"):
            pre.append(K.ResidualStmt("constexpr function", floc))
        if n.get("variadic"):
            pre.append(K.ResidualStmt("variadic function", floc))
        qt = self.qual(n.get("type")) or ""
        if "noexcept" in qt or qt.rstrip().endswith("&") or "throw(" in qt:
            pre.append(K.ResidualStmt("exception specification", floc))
        self.scope = TC.Scope()
        params = []
        for p in n.get("inner", []):
            if p.get("kind") != "ParmVarDecl":
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
        ret = TYPE_MAP.get(rs) or parse_cxx_type(rs)[0]
        self.ret_ref = isinstance(ret, K.RefT)
        if isinstance(ret, K.RefT):
            pre.append(K.ResidualStmt("function returning a reference", floc))
            ret = ret.inner  # its returns become cascades, not more reports
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
        return K.Func(name, tuple(params), ret, K.Block(tuple(pre + stmts), self.loc(body_node)), floc,
                      tuple(ns), internal)

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
            return [self.for_(n)]
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
                if isinstance(tgt, K.Index):
                    return [K.Store(tgt, self.expr(n["inner"][1]), L)]
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
            return K.ResidualStmt("auto-typed declaration", L)
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
            if isinstance(tgt, K.Index):
                return self.compound_elem(n, tgt)
            return self.rstmt(n, "compound assignment to something other than a local variable")
        op = COMPOUND_OPCODES.get(n.get("opcode"))
        if op is None:
            return self.rstmt(n, "operator '%s'" % n.get("opcode"))
        v = self.scope.lookup(name)
        T = K.deref(v[0]) if v else None
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
            if isinstance(tgt, K.Index):
                T = self.ty_of(tgt)
                if T in K.ARITH_INT:
                    return K.CompoundStore(op, tgt, K.IntLit(1, T, L), L)
                if T == K.F64:
                    return K.CompoundStore(op, tgt, K.FloatLit("1.0", L), L)
                if T in (K.I8, K.U8):
                    return K.Store(tgt, K.Cast(T, K.Binary(op, K.Cast(K.I32, tgt, L), K.IntLit(1, K.I32, L), L), L), L)
            return self.rstmt(n, "increment of something other than a local variable")
        v = self.scope.lookup(name)
        T = K.deref(v[0]) if v else None
        if T in K.ARITH_INT:
            return K.CompoundAssign(op, name, K.IntLit(1, T, L), L)
        if T == K.F64:
            return K.CompoundAssign(op, name, K.FloatLit("1.0", L), L)
        if T in (K.I8, K.U8):
            return K.Assign(name, K.Cast(T, K.Binary(op, K.Cast(K.I32, K.Var(name, L), L),
                                                     K.IntLit(1, K.I32, L), L), L), L)
        return self.rstmt(n, "increment of a bool")

    def for_(self, n):
        L = self.loc(n)
        bad = "`for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`"
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
                    segs.append(K.Spec("s", self.borrowed(cs)))
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
        if t in (K.I8, K.U8):
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
            return self.rexpr(n, "implicit conversion (%s)" % ck)
        if k in ("CXXStaticCastExpr", "CStyleCastExpr", "CXXFunctionalCastExpr"):
            T, _, spelled = self.map_type(n.get("type"))
            if K.is_owned_nc(T):
                if k == "CXXFunctionalCastExpr" and n.get("castKind") == "ConstructorConversion":
                    return self.class_expr(n["inner"][0])
                return self.rexpr(n, "cast to '%s'" % spelled)
            if n.get("castKind") == "ToVoid":
                return self.rexpr(n, "cast to void")
            if T is None or T == K.UNIT:
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
                return self.rexpr(n, "increment/decrement used as a value")
            if op == "&":
                return self.rexpr(n, "address-of")
            if op == "*":
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
            if c.get("kind") != "DeclRefExpr":
                return self.rexpr(n, "indirect call")
            rd = c.get("referencedDecl", {})
            fname = self.user_funcs.get(rd.get("id"))
            if fname is None:
                if rd.get("name") == "move":
                    return self.rexpr(n, "std::move outside an initializer, assignment, by-value argument, "
                                         "push_back or return")
                if rd.get("name") == "to_string" and len(n["inner"]) == 2:
                    return self.to_string(n)
                return self.rexpr(n, "call to library function '%s'" % rd.get("name"))
            ptys = self.sigs.get(fname, ((), None))[0]
            args = []
            for i, a in enumerate(n["inner"][1:]):
                if a.get("kind") == "CXXDefaultArgExpr":
                    return self.rexpr(a, "default argument")
                pt = ptys[i] if i < len(ptys) else None
                if isinstance(pt, K.RefT):
                    name = self.local_ref(a)
                    if name is not None:
                        args.append(K.Var(name, self.loc(a)))
                        continue
                    tgt = self.elem_target(a)
                    if isinstance(tgt, K.Index):
                        args.append(tgt)
                        continue
                    if pt.mutable:
                        return self.rexpr(a, "argument %d of '%s' binds a non-const reference to something "
                                             "other than a variable or element" % (i + 1, fname))
                    args.append(self.borrowed(a) if K.is_owned_nc(pt.inner) else self.expr(a))
                elif K.is_owned_nc(pt):
                    args.append(self.consume(a, pt))
                else:
                    args.append(self.expr(a))
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
        return self.rexpr(n, friendly(k))

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
        params = ct[ct.find("(") + 1:ct.rfind(")")] if "(" in ct else ""
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
            if kind == "copy":
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
        if k == "CXXOperatorCallExpr" and self.op_name(u) == "operator[]":
            e = self.operator_expr(u)
            if isinstance(e, K.ByteAt):
                return self.rexpr(u, "reference to a std::string element")
            return e
        if k == "CXXMemberCallExpr":
            me = self.strip(u["inner"][0])
            if me.get("kind") == "MemberExpr" and me.get("name") in ("front", "back"):
                return self.member_expr(u)
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
        """Index for an assignable `v[i]` / `v.back()`, a ResidualStmt, or None."""
        u = self.strip_tmp(n)
        k = u.get("kind")
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
        if head.startswith("auto") and (len(head) == 4 or not (head[4].isalnum() or head[4] == "_")):
            return K.ResidualStmt("auto-typed declaration", self.loc(vd))
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


def cxx_import(path, clang="clang++"):
    """Surface IR for the C++ file at ``path`` (may contain Residuals)."""
    ast = run_clang(path, clang)
    with open(path, "rb") as f:
        source = f.read()
    return CxxImporter(path, ast, source).run()
