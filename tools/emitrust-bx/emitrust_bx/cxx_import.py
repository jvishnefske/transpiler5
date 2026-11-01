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
        items = []
        funcs = []
        for n in mains:
            k = n.get("kind")
            if k == "FunctionDecl":
                funcs.append(n)
                self.user_funcs[n["id"]] = n.get("name")
            elif k == "LinkageSpecDecl":
                for c in n.get("inner", []):
                    if (c.get("kind") == "FunctionDecl" and c.get("name") == "printf"
                            and not any(x.get("kind") == "CompoundStmt" for x in c.get("inner", []))
                            and (self.qual(c.get("type")) or "").replace(" ", "")
                            in ("int(constchar*,...)", "int(constchar*__restrict,...)")):
                        self.libc_printf.add(c["id"])
                    else:
                        items.append(K.ResidualItem("extern \"C\" declaration", self.loc(c)))
            elif k == "UsingDirectiveDecl":
                ns = n.get("nominatedNamespace", {}).get("name")
                if ns != "std":
                    items.append(K.ResidualItem("using-directive", self.loc(n)))
            else:
                items.append(K.ResidualItem(friendly(k), self.loc(n)))
        # signatures (for typing calls)
        self.sigs = {}
        defined = set()
        for n in funcs:
            if self.has_body(n):
                ptys, ret = self.signature_types(n)
                if n.get("name") != "main":
                    self.sigs.setdefault(n["name"], (ptys, ret))
                defined.add(n.get("name"))
        for n in funcs:
            if not self.has_body(n):
                if n.get("name") not in defined:
                    items.append(K.ResidualItem("function declared but not defined", self.loc(n)))
                continue
            items.append(self.func(n))
        # keep source order
        items.sort(key=lambda it: (it.loc.line, it.loc.col) if it.loc else (0, 0))
        return K.Program(tuple(items))

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
        ret = TYPE_MAP.get(rs)
        return tuple(ptys), ret

    # -------------------------------------------------------------- funcs
    def func(self, n):
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
            if ty is None or ty == K.UNIT:
                pre.append(K.ResidualStmt("parameter of type '%s'" % spelled, pl))
                ty = K.I32
            if p.get("init") or p.get("inner"):
                pre.append(K.ResidualStmt("default argument", pl))
            params.append(K.Param(pname, ty, not const, pl))
            self.scope.declare(pname, ty, not const)
            self.locals[p["id"]] = pname
        rs = self.ret_type_str(n)
        ret = TYPE_MAP.get(rs)
        if ret is None:
            pre.append(K.ResidualStmt("return type '%s'" % rs, floc))
            ret = K.UNIT
        self.ret = ret
        body_node = [c for c in n.get("inner", []) if c.get("kind") == "CompoundStmt"][-1]
        stmts = []
        is_main = name == "main"
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
        return K.Func(name, tuple(params), ret, K.Block(tuple(pre + stmts), self.loc(body_node)), floc)

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
            return [self.decl(d) for d in n.get("inner", [])]
        if k == "ReturnStmt":
            if self.in_main:
                return [self.rstmt(n, "return inside main other than the final `return 0;`")]
            v = (n.get("inner") or [None])[0]
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
        if k in ("ExprWithCleanups", "ParenExpr"):
            return self.stmt(n["inner"][0])
        if k == "BinaryOperator" and n.get("opcode") == "=":
            name = self.local_ref(n["inner"][0])
            if name is None:
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
        if ty is None or ty == K.UNIT:
            return K.ResidualStmt("variable of type '%s'" % spelled, L)
        if "init" not in d:
            return K.ResidualStmt("declaration without an initializer", L)
        if d.get("init") != "c":
            return K.ResidualStmt("%s-initialization (the subset spells it `T x = e;`)" % d.get("init"), L)
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
            return self.rstmt(n, "compound assignment to something other than a local variable")
        op = COMPOUND_OPCODES.get(n.get("opcode"))
        if op is None:
            return self.rstmt(n, "operator '%s'" % n.get("opcode"))
        v = self.scope.lookup(name)
        T = v[0] if v else None
        CL = self.map_type(n.get("computeLHSType"))[0]
        rhs = self.expr(n["inner"][1])
        if T == K.BOOL:
            return self.rstmt(n, "compound assignment to bool")
        if CL is None:
            return self.rstmt(n, "compound assignment computed in type '%s'" % self.qual(n.get("computeLHSType")))
        if CL == T:
            return K.CompoundAssign(op, name, rhs, L)
        return K.Assign(name, K.Cast(T, K.Binary(op, K.Cast(CL, K.Var(name, L), L), rhs, L), L), L)

    def incdec(self, n):
        L = self.loc(n)
        name = self.local_ref(n["inner"][0])
        if name is None:
            return self.rstmt(n, "increment of something other than a local variable")
        op = "add" if n.get("opcode") == "++" else "sub"
        v = self.scope.lookup(name)
        T = v[0] if v else None
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
                return self.rexpr(n, "call to library function '%s'" % rd.get("name"))
            args = []
            for a in n["inner"][1:]:
                if a.get("kind") == "CXXDefaultArgExpr":
                    return self.rexpr(a, "default argument")
                args.append(self.expr(a))
            return K.Call(fname, tuple(args), L)
        return self.rexpr(n, friendly(k))


def cxx_import(path, clang="clang++"):
    """Surface IR for the C++ file at ``path`` (may contain Residuals)."""
    ast = run_clang(path, clang)
    with open(path, "rb") as f:
        source = f.read()
    return CxxImporter(path, ast, source).run()
