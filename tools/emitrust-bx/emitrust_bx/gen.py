"""Seeded random generator of well-typed, NORMAL Kernel programs (FR-250).

The property tests state the bijection laws over its output, so it must
reach every Kernel construct: all scalar types, every operator, casts,
calls (forward and recursive, hence prototypes), every statement form,
else-if chains, the counting loop, and printf with every spec kind, flag
and escape-worthy text character.  Output is passed through
``typecheck.normalize`` (laws are stated over normal forms) and checked.

FR-251: vectors (of scalars, String, nested), String, reference
parameters, range-for in all three modes, and every vector/string row.
The generator obeys the subset's own rules BY CONSTRUCTION -- it is not
filtered: a moved variable is dead for the rest of the function and is
never mentioned elsewhere in its statement; moves out of a loop's outer
variables only by `return`; a range-for body never modifies (shared) or
mentions (mutable) the iterated vector; a variable passed by `T&` is not
mentioned elsewhere in its statement; a written element's index never
reads its own vector.  ``check`` must accept every program, and the
rustc subset in test_property must borrow-check.

FR-260: with ``namespaces`` (the default) about 60% of the functions live in
namespaces up to three deep, drawn from a SEPARATE random stream so the
function bodies of a seed are unchanged.  Leaf names are drawn from a small
pool so the same name recurs in sibling namespaces and in a namespace and
its ancestors (inner hides outer -- a bare same-namespace call must find the
inner one on both sides); a quarter of namespaced functions are internal
(anonymous-namespace / private `fn`) and are only called from inside their
namespace, as the subset requires.

FR-257: with ``structs`` (the default) every program also defines 2-4
structs (drawn from a THIRD stream), placed like functions, whose data
members are scalars, String, Vec<scalar>, an earlier struct by value, or a
vector of any struct (itself included: recursion through Vec is legal in
both languages).  Each has 1-3 member functions (`&self`, `&mut self`,
static) and sometimes a constructor (then aggregate initialization of it is
not in the subset, so values come from `S::new`).  Bodies read and write
data members through every place shape (`p.f`, `v[i].f`, `p.v[i]`,
`self.f`), call member functions on places, and pass structs by value
(moved or cloned) and by `&` / `&mut`.  The alias discipline extends to
receivers: the receiver's variable is not mentioned elsewhere in the
statement when the call may modify it, and arguments never modify it.

FR-261: with ``pointers`` (OFF by default: the FR-250..260 coverage pins are
measured on their own seeds, several within one or two of their thresholds,
so pointer programs are a SEPARATE population -- test_property's
``ptr_programs``) functions get EXTRA `const T*` / `T*` parameters and
EXTRA pointer statements, every pointer draw from a FOURTH stream; bodies
make pointer values -- a
pointer variable, `p + k`, `v.data()`, `v.data() + k`, `&x`, `this` -- as
arguments and as pointer locals, read and write through them (`*p`, `p[i]`,
`p->f`, `p->m(..)`, `p->push_back(..)`) and re-slice them (`p += k`).  The
alias rules hold by construction: a mutable pointer argument's root is not
mentioned elsewhere in its statement; a MUTABLE pointer local hides its
roots for the rest of its block, a CONST one freezes them (no write, no
move, no mutable pointer); a mutable re-slice or slice never reads its own
root.

FR-259: with ``fr259`` (OFF by default, a SEPARATE population like FR-261's,
test_fr259's ``fr259_programs``) the scalar types include i16 / u16 and
expressions get EXTRA `c ? a : b` (Cond) and `x++` / `++x` / `x--` / `--x`
used as a value (IncDec), drawn from a FIFTH stream.  The sequencing rule
holds by construction: an incremented variable is one the statement has not
mentioned yet, and it is hidden for the rest of the statement.

FR-252: with ``closures`` (OFF by default, a SEPARATE population like
FR-259's, test_fr252's ``closure_programs``) blocks get EXTRA closure
groups drawn from a SIXTH stream: `auto f = [caps](params) -> R { body };`
over 0-2 scalar captures, all by copy (`move`) or all by reference, a body of
ordinary generated statements over the captures and parameters only (fresh
names, so nothing shadows), then 1-3 consecutive calls of it (`T y = f(..);`
/ `f(..);`).  The alias discipline holds by construction: the calls are the
closure's whole live range, and a by-reference closure's captures are hidden
from the call arguments; each call statement calls it once.
"""

import contextlib
import random

from . import kernel as K
from . import typecheck as TC

FLOATS = ["0.0", "0.5", "1.0", "2.5", "0.125", "1e-05", "123.25", "1e+300", "3.0", "100.0",
          "0.1", "6.02e+23"]
TEXT_POOL = list("abcXYZ019 ,.:;=!?%{}\"\\'\n\t()[]#@$&*+-/<>|^~`_")
NS_POOL = ((), ("na",), ("nb",), ("na", "nc"), ("nb", "nc"), ("na", "nc", "nd"), ("na", "nd"),
           ("nb", "nc", "na"))
# FR-261: the pointer stream's rates (an EXTRA pointer parameter / statement)
PTR_PARAM_RATE = 0.4
PTR_STMT_RATE = 0.3
NC_TYPES = (K.STRING, K.STRING, K.VecT(K.I32), K.VecT(K.I64), K.VecT(K.U8), K.VecT(K.F64), K.VecT(K.I8),
            K.VecT(K.USIZE), K.VecT(K.STRING), K.VecT(K.VecT(K.I32)))
# the scalar types of the FR-250..261 populations (FR-259's i16 / u16 are
# drawn only in its own population, so these seeds keep their sequences)
FR250_PRIMS = (K.I8, K.U8, K.I32, K.I64, K.U32, K.U64, K.USIZE, K.F64, K.BOOL)
VALUE_TYPES = FR250_PRIMS + NC_TYPES
# FR-259: the rate of an EXTRA `?:` / increment-as-a-value in an expression
FR259_RATE = 0.12
# FR-252: the rate of an EXTRA closure group in a block
CLOSURE_RATE = 0.35


class Gen:
    def __init__(self, seed, namespaces=True, structs=True, pointers=False, fr259=False, closures=False):
        self.r = random.Random(seed)
        self.closures = closures
        self.cr = random.Random(seed * 49979687 + 3)  # FR-252's own stream
        self.in_closure = False
        self.pointers = pointers
        self.fr259 = fr259
        self.prims = K.ALL_PRIMS if fr259 else FR250_PRIMS
        self.fr = random.Random(seed * 32452843 + 11)  # FR-259's own stream
        self.pr = random.Random(seed * 15485863 + 7)  # FR-261's own stream
        self.roots_of = {}  # FR-261: pointer local -> its (transitive) roots
        self.pcounter = 0
        self.namespaces = namespaces
        self.nsr = random.Random(seed * 7919 + 17)  # FR-260's own stream
        self.sr = random.Random(seed * 104729 + 31)  # FR-257's own stream
        self.structs_on = structs
        self.meta = {}  # path -> (ns, leaf, internal)
        self.cur_ns = ()
        self.sdefs = []  # FR-257: StructDef shells (fields; methods filled later)
        self.sd_by = {}
        self.msigs = []  # (path, params, ret, recv, struct path)
        self._sigmap = None

    def placement(self, i, nfuncs, prefix, taken):
        """(ns, leaf, internal) for the i-th generated function."""
        r = self.nsr
        if not self.namespaces or r.random() < 0.4:
            ns = ()
        else:
            ns = r.choice(NS_POOL[1:])
        while True:
            leaf = "%s%d" % (prefix, r.randint(0, max(2, nfuncs // 3)) if ns else i)
            if K.path_of(ns, leaf) not in taken:
                break
            if not ns:
                leaf = "%s%d" % (prefix, i + nfuncs)  # unique at the root
                if K.path_of(ns, leaf) not in taken:
                    break
        return ns, leaf, bool(ns) and r.random() < 0.25

    def callable_sigs(self):
        """Signatures the current function may call (FR-260: an internal
        function only from inside its namespace)."""
        out = []
        for s in self.sigs:
            ns, _, internal = self.meta.get(s[0], ((), s[0], False))
            if internal and self.cur_ns[:len(ns)] != ns:
                continue
            if not self.types_visible([p.ty for p in s[1]] + [s[2]]):
                continue
            out.append(s)
        for mp, params, ret, recv, owner in self.msigs:
            if recv in ("static", "ctor") and self.types_visible([K.StructT(owner)] + [p.ty for p in params] + [ret]):
                out.append((mp, params, ret))
        return out

    def choice(self, xs):
        return self.r.choice(xs)

    # -------------------------------------------------------------- program
    def struct_visible(self, path, ns=None):
        """May a function (or struct) in namespace ``ns`` NAME struct
        ``path``?  An internal struct only from inside its namespace."""
        ns = self.cur_ns if ns is None else ns
        sd = self.sd_by[path]
        return not sd.internal or tuple(ns[:len(sd.ns)]) == tuple(sd.ns)

    def types_visible(self, types, ns=None, seen=None):
        """Are ``types`` NAMEABLE from ``ns``: every struct in them visible,
        and (since a fresh value spells every member's type, e.g. an empty
        `Vec::<T>::new()`) every struct reachable through their members too."""
        seen = set() if seen is None else seen
        for t in types:
            while isinstance(t, (K.VecT, K.RefT, K.PtrT)):
                t = t.elem if isinstance(t, K.VecT) else t.inner
            if isinstance(t, K.StructT) and t.path not in seen:
                seen.add(t.path)
                if not self.struct_visible(t.path, ns):
                    return False
                if not self.types_visible([f.ty for f in self.sd_by[t.path].fields], ns, seen):
                    return False
        return True

    def struct_types(self):
        return tuple(K.StructT(sd.path) for sd in self.sdefs if self.types_visible([K.StructT(sd.path)]))

    def ptr_inner(self):
        sts = self.struct_types()
        pool = [K.I32, K.I32, K.I32, K.U8, K.U8, K.USIZE, K.USIZE, K.F64, K.STRING, K.VecT(K.I32)] + list(sts) * 2
        return self.pr.choice(pool)

    def ptr_params(self, params, own=None):
        """FR-261: pointer parameters are ADDED (drawn from the pointer
        stream) so the other constructs' coverage is not diluted.  A member
        function often takes a pointer to its own struct (`this` passes)."""
        params = list(params)
        while self.pointers and self.pr.random() < PTR_PARAM_RATE:
            inner = K.StructT(own) if own and self.pr.random() < 0.4 else self.ptr_inner()
            params.append(K.Param("p%d" % len(params), K.PtrT(inner, self.pr.random() < 0.55),
                                  self.pr.random() < 0.6))
        return params

    def param_type(self, r=None):
        r = r or self.r
        roll = r.random()
        if roll < 0.55:
            return r.choice(self.prims), r.random() < 0.6
        sts = self.struct_types()
        if sts and roll < 0.65:
            if r.random() < 0.5:
                return r.choice(sts), r.random() < 0.6
            return K.RefT(r.choice(sts), r.random() < 0.5), False
        if roll < 0.8:
            return r.choice(NC_TYPES), r.random() < 0.6
        inner = r.choice(NC_TYPES + (K.I32, K.I64, K.U32, K.F64, K.BOOL))
        return K.RefT(inner, r.random() < 0.5), False

    def ret_type(self, r=None):
        r = r or self.r
        sts = self.struct_types()
        if sts and r.random() < 0.12:
            return r.choice(sts)
        return r.choice(self.prims + (K.UNIT, K.UNIT) + NC_TYPES[:4])

    # ------------------------------------------------------------ FR-257
    def make_structs(self):
        """Struct shells: names, placement, data members, member-function
        signatures (bodies are generated with the free functions)."""
        r = self.sr
        if not self.structs_on:
            return
        for i in range(r.randint(2, 4)):
            ns = () if (not self.namespaces or r.random() < 0.4) else r.choice(NS_POOL[1:])
            internal = bool(ns) and r.random() < 0.3
            name = "S%d" % i
            path = K.path_of(ns, name)
            earlier = [sd.path for sd in self.sdefs if self.types_visible([K.StructT(sd.path)], ns)]
            fields = []
            for j in range(r.randint(1, 4)):
                roll = r.random()
                if roll < 0.4 or (roll >= 0.6 and not earlier and roll < 0.82):
                    ty = r.choice(self.prims)
                elif roll < 0.6:
                    ty = r.choice(NC_TYPES[:8])
                elif roll < 0.82:
                    ty = K.StructT(r.choice(earlier))
                else:
                    ty = K.VecT(K.StructT(r.choice(earlier + [path])))
                fields.append(K.FieldDef("g%d" % j, ty))
            sd = K.StructDef(name, tuple(fields), (), None, ns, internal)
            self.sdefs.append(sd)
            self.sd_by[path] = sd
        for sd in self.sdefs:
            ms = []
            self.cur_ns = sd.ns
            if r.random() < 0.35:
                ms.append((sd.path + "::new", tuple(K.Param("p%d" % j, f.ty, True) for j, f in enumerate(sd.fields)),
                           K.StructT(sd.path), "ctor"))
            for j in range(r.randint(1, 3)):
                recv = r.choice(("ref", "ref", "mut", "mut", "static"))
                params = []
                for k in range(r.randint(0, 3)):
                    t, m = self.param_type(r)
                    params.append(K.Param("p%d" % k, t, m))
                params = self.ptr_params(params, own=sd.path)
                ms.append((sd.path + "::m%d" % j, tuple(params), self.ret_type(r), recv))
            for mp, params, ret, recv in ms:
                self.msigs.append((mp, params, ret, recv, sd.path))
                self.meta[mp] = (sd.ns, mp.rsplit("::", 1)[1], False)
        self.cur_ns = ()

    def has_ctor(self, path):
        return any(m[3] == "ctor" and m[4] == path for m in self.msigs)

    def program(self, nfuncs, prefix="f"):
        r = self.r
        self.make_structs()
        sigs = []
        for i in range(nfuncs):
            # (placement draws from its own stream, so drawing it first leaves
            # the main stream's sequence as it was)
            ns, leaf, internal = self.placement(i, nfuncs, prefix, self.meta)
            self.cur_ns = ns
            nparams = r.randint(0, 4)
            params = []
            for j in range(nparams):
                t, m = self.param_type()
                params.append(K.Param("p%d" % j, t, m))
            params = self.ptr_params(params)
            ret = self.ret_type()
            path = K.path_of(ns, leaf)
            self.meta[path] = (ns, leaf, internal)
            sigs.append((path, tuple(params), ret))
        self.sigs = sigs
        self._sigmap = None
        funcs = []
        for name, params, ret in sigs:
            ns, leaf, internal = self.meta[name]
            self.cur_ns = ns
            f = self.func(name, params, ret)
            funcs.append(K.Func(leaf, f.params, f.ret, f.body, None, ns, internal))
        structs = []
        for sd in self.sdefs:
            ms = []
            self.cur_ns = sd.ns
            for mp, params, ret, recv, owner in self.msigs:
                if owner != sd.path:
                    continue
                leaf = mp.rsplit("::", 1)[1]
                f = self.func(mp, params, ret, recv=recv, owner=sd)
                ms.append(K.Func(leaf, f.params, f.ret, f.body, None, sd.ns, False, sd.name, recv))
            structs.append(K.StructDef(sd.name, sd.fields, tuple(ms), None, sd.ns, sd.internal))
        prog = TC.normalize(K.Program(tuple(structs) + tuple(funcs)))
        errs = TC.check(prog)
        assert not errs, errs[:3]
        return prog

    def func(self, name, params, ret, recv="", owner=None):
        self.counter = 0
        self.ret = ret
        self.dead = set()
        self.frozen = set()
        self.hidden = set()
        self.loop_depth = 0
        self.decl_depth = {}
        self.stmt_seen = set()
        self.stmt_hidden = set()
        scope = TC.Scope()
        if recv in ("ref", "mut"):
            scope.declare("self", K.RefT(K.StructT(owner.path), recv == "mut"), False)
            self.decl_depth["self"] = 0
        for p in params:
            scope.declare(p.name, p.ty, p.mutable)
            self.decl_depth[p.name] = 0
        if recv == "ctor":
            # a member-initializer list: `return S { every field };`
            self.begin_stmt()
            self.ncalls = 1
            lit = K.StructLit(owner.path, tuple(self.value(f.ty, scope, 2, "consume") for f in owner.fields))
            return K.Func(name, params, ret, K.Block((K.Return(lit),)))
        self.roots_of = {}
        self.pcounter = 0
        tail = None
        if ret != K.UNIT:
            def tail():
                self.begin_stmt()
                return K.Return(self.value(ret, scope, 3, "consume", is_return=True))
        stmts = self.stmts(scope, depth=0, loop=False, n=self.r.randint(1, 6), tail=tail)
        return K.Func(name, params, ret, K.Block(tuple(stmts)))

    def fresh(self):
        self.counter += 1
        return "v%d" % self.counter

    def begin_stmt(self):
        self.stmt_seen = set()
        self.stmt_hidden = set()
        # FR-263: at most one call per statement (a constructor is exempt
        # and its body calls nothing): two calls that may print, unsequenced
        # in one statement, are refused by the checker
        self.ncalls = 0

    # -------------------------------------------------------------- scope
    def all_vars(self, scope):
        out = []
        s = scope
        seen = set()
        while s is not None:
            for name, (t, m) in s.vars.items():
                if name not in seen:
                    seen.add(name)
                    out.append((name, t, m))
            s = s.parent
        return sorted(out, key=lambda x: x[0])

    def visible(self, name):
        return name not in self.dead and name not in self.hidden and name not in self.stmt_hidden

    def vars_of(self, scope, pred, write=False):
        """(name, referent type, writable) of visible variables."""
        out = []
        for name, t, m in self.all_vars(scope):
            if not self.visible(name):
                continue
            w = t.mutable if isinstance(t, K.RefT) else m
            dt = K.deref(t)
            if write and (not w or name in self.frozen):
                continue
            if pred(dt, w):
                out.append((name, dt, w))
        return out

    def var(self, name):
        self.stmt_seen.add(name)
        return K.Var(name)

    def is_ref(self, scope, name):
        v = scope.lookup(name)
        return v is not None and isinstance(v[0], K.RefT)

    # -------------------------------------------------------------- stmts
    def stmts(self, scope, depth, loop, n, tail=None):
        """``n`` statements (then ``tail()``) of ONE block: FR-261 pointer
        locals hide / freeze their roots until the block ends."""
        saved = (set(self.hidden), set(self.frozen))
        try:
            out = []
            for _ in range(n):
                if self.pointers and self.pr.random() < PTR_STMT_RATE:
                    # FR-261: pointer statements are ADDED (see ptr_params)
                    self.begin_stmt()
                    st = self.ptr_stmt(scope)
                    if st is not None:
                        out.append(st)
                if self.fr259 and self.fr.random() < 0.3:
                    # FR-259: an EXTRA increment-as-a-value statement
                    self.begin_stmt()
                    with self.fr259_stream():
                        out.append(self.fr259_stmt(scope))
                if self.closures and not self.in_closure and self.cr.random() < CLOSURE_RATE:
                    # FR-252: an EXTRA closure and its calls
                    with self.closure_stream():
                        out.extend(self.closure_group(scope, depth))
                out.append(self.stmt(scope, depth, loop))
            if tail is not None:
                out.append(tail())
            return out
        finally:
            self.hidden, self.frozen = saved

    def block(self, scope, depth, loop, n=None):
        inner = scope.child()
        if n is None:
            n = self.r.randint(0, 3)
        return K.Block(tuple(self.stmts(inner, depth + 1, loop, n)))

    def declare(self, scope, name, t, m):
        scope.declare(name, t, m)
        self.decl_depth[name] = self.loop_depth

    def stmt(self, scope, depth, loop):
        self.begin_stmt()
        r = self.r
        kinds = ["let", "let", "assign", "compound", "print", "call", "nlet", "nlet", "vecop", "vecop",
                 "strop", "strop", "strop", "store"]
        if self.sdefs:
            # (one more `nlet` keeps FR-251's move coverage undiluted)
            kinds += ["mcall", "mcall", "mcall", "store", "slet", "nlet"]
        if depth < 3:
            kinds += ["if", "if", "while", "for", "block", "foreach", "foreach"]
        if loop:
            kinds += ["break", "continue"]
        if self.ret != K.UNIT:
            kinds += ["return"]
        elif r.random() < 0.3:
            kinds += ["return"]
        k = self.choice(kinds)
        if k == "let":
            t = self.choice(self.prims)
            e = self.expr(t, scope, 3)
            name = self.fresh()
            mut = r.random() < 0.7
            self.declare(scope, name, t, mut)
            return K.Let(name, t, mut, e)
        if k == "nlet":
            t = self.choice(NC_TYPES + (K.STRING,))
            e = self.value(t, scope, 3, "consume")
            name = self.fresh()
            mut = r.random() < 0.8
            self.declare(scope, name, t, mut)
            return K.Let(name, t, mut, e)
        if k == "assign":
            vs = self.vars_of(scope, lambda t, m: not isinstance(t, K.PtrT), write=True)
            if not vs:
                return self.stmt(scope, depth, loop) if r.random() < 0.5 else K.Break() if loop else self.print_(scope)
            name, t, _ = self.choice(vs)
            self.stmt_seen.add(name)
            self.stmt_hidden.add(name)  # `x = f(&mut x)`-style aliasing stays out
            return K.Assign(name, self.value(t, scope, 3, "consume"))
        if k == "compound":
            vs = self.vars_of(scope, lambda t, m: t in K.ARITH, write=True)
            if not vs:
                return self.print_(scope)
            name, t, _ = self.choice(vs)
            self.stmt_seen.add(name)
            self.stmt_hidden.add(name)
            ops = list(K.ARITH_OPS if t in K.ARITH_INT else ("add", "sub", "mul", "div"))
            if t in K.ARITH_INT:
                ops += list(K.BIT_OPS + K.SHIFT_OPS)
            op = self.choice(ops)
            if op in K.SHIFT_OPS:
                v = self.expr(self.choice(K.ARITH_INT), scope, 2)
            elif r.random() < 0.3 and t in K.ARITH_INT and op in ("add", "sub"):
                v = K.IntLit(1, t)  # the ++/-- spelling
            else:
                v = self.expr(t, scope, 2)
            return K.CompoundAssign(op, name, v)
        if k == "print":
            return self.print_(scope)
        if k == "call":
            name, params, ret = self.choice([c for c in self.callable_sigs() if not c[0].endswith("::new")])
            try:
                return K.ExprStmt(self.call(name, params, scope, 2))
            except _NoCall:
                return self.print_(scope)
        if k == "mcall":
            try:
                return K.ExprStmt(self.mcall(scope, 2, None))
            except _NoCall:
                return self.print_(scope)
        if k == "slet":
            if not self.struct_types():
                return self.print_(scope)
            t = self.choice(self.struct_types())
            e = self.value(t, scope, 3, "consume")
            name = self.fresh()
            mut = r.random() < 0.8
            self.declare(scope, name, t, mut)
            return K.Let(name, t, mut, e)
        if k == "vecop":
            return self.vecop(scope)
        if k == "strop":
            return self.strop(scope)
        if k == "store":
            return self.store(scope)
        if k == "if":
            cond = self.expr(K.BOOL, scope, 3)
            then = self.block(scope, depth, loop)
            roll = r.random()
            if roll < 0.3:
                els = None
            elif roll < 0.6:
                els = K.Block(tuple(self.stmts(scope.child(), depth + 1, loop, 1)))
                if not isinstance(els.stmts[0], K.If):
                    self.begin_stmt()
                    els = K.Block((K.If(self.expr(K.BOOL, scope, 2), self.block(scope, depth, loop),
                                        self.block(scope, depth, loop) if r.random() < 0.5 else None),))
            else:
                els = self.block(scope, depth, loop)
            return K.If(cond, then, els)
        if k == "while":
            self.loop_depth += 1
            try:
                cond = self.expr(K.BOOL, scope, 3)
                return K.While(cond, self.block(scope, depth, True))
            finally:
                self.loop_depth -= 1
        if k == "for":
            return self.for_(scope, depth)
        if k == "foreach":
            return self.foreach(scope, depth)
        if k == "block":
            return self.block(scope, depth, loop, self.r.randint(1, 3))
        if k == "break":
            return K.Break()
        if k == "continue":
            return K.Continue()
        if k == "return":
            if self.ret == K.UNIT:
                return K.Return(None)
            return K.Return(self.value(self.ret, scope, 3, "consume", is_return=True))
        raise AssertionError(k)

    def for_(self, scope, depth):
        r = self.r
        t = self.choice((K.I32, K.USIZE))
        lo = self.expr(t, scope, 2)
        lo_hidden = set(self.stmt_hidden)  # `T&` arguments in lo: hi must not name them
        var = self.fresh()
        inner = scope.child()
        hi = None
        frozen = None
        if t == K.USIZE and r.random() < 0.35:
            places = [p for p in self.places(scope, lambda tt: isinstance(tt, K.VecT) or tt == K.STRING, 1,
                                             index_ok=False) if TC.base_var(p) not in lo_hidden]
            if places:
                pl = self.choice(places)
                hi = K.Len(pl)
                frozen = TC.base_var(pl)
        self.declare(inner, var, t, False)
        self.loop_depth += 1
        added = frozen is not None and frozen not in self.frozen
        if added:
            self.frozen.add(frozen)
        try:
            body = K.Block(tuple(self.stmts(inner, depth + 1, True, r.randint(0, 3))))
        finally:
            self.loop_depth -= 1
            if added:
                self.frozen.discard(frozen)
        if hi is None:
            assigned = TC.assigned_names(body, self.sig_map())
            cands = [v for v in self.vars_of(scope, lambda tt, m: tt == t)
                     if v[0] not in assigned and not self.is_ref(scope, v[0]) and v[0] not in lo_hidden]
            if cands and r.random() < 0.5:
                hi = K.Var(self.choice(cands)[0])
            else:
                hi = self.lit(t)
        return K.For(var, lo, hi, body)

    def sig_map(self):
        if self._sigmap is None:
            sm = TC.Sigs({n: (tuple(p.ty for p in ps), ret) for n, ps, ret in self.sigs})
            for mp, params, ret, recv, owner in self.msigs:
                sm[mp] = (tuple(p.ty for p in params), ret)
                sm.recv[mp] = recv
            for sd in self.sdefs:
                sm.structs[sd.path] = sd
            self._sigmap = sm
        return self._sigmap

    def foreach(self, scope, depth):
        r = self.r
        mode = self.choice(("copy", "copy", "ref", "mut"))
        if mode == "mut":
            places = self.write_places(scope, lambda tt: isinstance(tt, K.VecT), 1)
        else:
            places = self.places(scope, lambda tt: isinstance(tt, K.VecT), 1)
        if not places:
            return self.print_(scope)
        it = self.choice(places)
        b = TC.base_var(it)
        el = self.ty(it, scope).elem
        var = self.fresh()
        inner = scope.child()
        mutable = mode == "copy" and r.random() < 0.6
        if mode == "copy":
            self.declare(inner, var, el, mutable)
        else:
            self.declare(inner, var, K.RefT(el, mode == "mut"), False)
        hide = mode == "mut" and b not in self.hidden
        froz = mode != "mut" and b not in self.frozen
        if hide:
            self.hidden.add(b)
        if froz:
            self.frozen.add(b)
        self.loop_depth += 1
        try:
            body = K.Block(tuple(self.stmts(inner, depth + 1, True, r.randint(1, 3))))
        finally:
            self.loop_depth -= 1
            if hide:
                self.hidden.discard(b)
            if froz:
                self.frozen.discard(b)
        return K.ForEach(var, mutable, mode, it, body)

    def ty(self, e, scope):
        return TC.type_of(e, scope, self.sig_map())

    # ------------------------------------------------ FR-251 statements
    def access_paths(self, name, t, depth):
        """(steps, type) of the places rooted at variable ``name`` -- FR-257:
        through data members as well as elements; at most one index."""
        out = [((), t)]
        if depth <= 0:
            return out
        if isinstance(t, K.PtrT):
            # FR-261: `p[i]`, `*p`, `p->f` (the pointer itself is no place
            # the value generators may pick)
            return [(("[]",) + st, tt) for st, tt in self.access_paths(name, t.inner, depth) if "[]" not in st]
        if isinstance(t, K.VecT):
            out += [(("[]",) + st, tt) for st, tt in self.access_paths(name, t.elem, depth - 1)
                    if "[]" not in st]
        elif isinstance(t, K.StructT) and t.path in self.sd_by:
            for f in self.sd_by[t.path].fields:
                out += [((f.name,) + st, tt) for st, tt in self.access_paths(name, f.ty, depth - 1)]
        return out

    def place_of(self, name, steps, idx):
        e = K.Var(name)
        for st in steps:
            e = K.Index(e, idx()) if st == "[]" else K.Field(e, st)
        return e

    @staticmethod
    def ptr_root_ok(t, ptr):
        """FR-261 ``ptr``: False -- no place through a pointer (the FR-251/257
        generators keep their distributions), True -- any, 'only' -- only
        places through a pointer."""
        is_ptr = isinstance(t, K.PtrT)
        return (ptr == "only" and is_ptr) or (ptr is True) or (ptr is False and not is_ptr)

    def write_places(self, scope, pred, depth, ptr=False):
        """Writable places (Var, element, FR-257 data member) whose type
        satisfies pred."""
        out = []
        for name, t, w in self.vars_of(scope, lambda t, m: True):
            if name in self.stmt_seen or name in self.frozen or not self.ptr_root_ok(t, ptr):
                continue
            if isinstance(t, K.PtrT):
                w = t.mutable  # FR-261: writes through a pointer need a `T*`
            if not w:
                continue
            for steps, tt in self.access_paths(name, t, depth + (1 if self.sdefs else 0)):
                if steps and "[]" in steps and depth <= 0 and not isinstance(t, K.PtrT):
                    continue
                if pred(tt):
                    out.append(self.place_of(name, steps, lambda n=name: self.ptr_index(scope, n, t)))
        return out

    def ptr_index(self, scope, base, t):
        """An index for a place rooted at ``base``: `0` half the time through
        a pointer (`*p`, `p->f`), else a usize expression not reading base."""
        if isinstance(t, K.PtrT) and self.pr.random() < 0.5:
            return K.IntLit(0, K.USIZE)
        return self.index_for(scope, base)

    def index_for(self, scope, base):
        """A usize index expression that does not read ``base``."""
        added = base not in self.stmt_hidden
        self.stmt_hidden.add(base)
        try:
            return self.expr(K.USIZE, scope, 1)
        finally:
            if added:
                self.stmt_hidden.discard(base)

    def places(self, scope, pred, depth, index_ok=True, hide_root=False, ptr=False):
        out = []
        for name, t, w in self.vars_of(scope, lambda t, m: True):
            if not self.ptr_root_ok(t, ptr):
                continue
            for steps, tt in self.access_paths(name, t, depth + (1 if self.sdefs else 0)):
                if "[]" in steps and (not index_ok or depth <= 0) and not isinstance(t, K.PtrT):
                    continue
                if pred(tt):
                    # the root is SEEN before its index is drawn: an index may
                    # read it but never modify it (`v[f(&mut v)]`, `v[v.m()]`)
                    self.stmt_seen.add(name)
                    idx = (lambda n=name: self.index_for(scope, n)) if hide_root else \
                        (lambda: self.expr(K.USIZE, scope, 1))
                    if isinstance(t, K.PtrT) and (not index_ok or self.pr.random() < 0.5):
                        idx = (lambda: K.IntLit(0, K.USIZE))
                    out.append(self.place_of(name, steps, idx))
        for p in out:
            for n in K.walk(p):
                if isinstance(n, K.Var):
                    self.stmt_seen.add(n.name)
        return out

    def target(self, places):
        t = self.choice(places)
        b = TC.base_var(t)
        self.stmt_seen.add(b)
        self.stmt_hidden.add(b)  # the arguments must not read the target's vector
        return t

    def vecop(self, scope):
        places = self.write_places(scope, lambda tt: isinstance(tt, K.VecT), 1)
        if not places:
            return self.print_(scope)
        tgt = self.target(places)
        el = self.ty(tgt, scope).elem
        k = self.choice(("push", "push", "push", "pop", "clear", "resize"))
        if k == "push":
            return K.Push(tgt, self.value(el, scope, 2, "consume"))
        if k == "pop":
            return K.Pop(tgt)
        if k == "clear":
            return K.Clear(tgt)
        return K.Resize(tgt, self.expr(K.USIZE, scope, 1), self.value(el, scope, 2, "copy"))

    def strop(self, scope):
        places = self.write_places(scope, lambda tt: tt == K.STRING, 1)
        if not places:
            return self.print_(scope)
        tgt = self.target(places)
        k = self.choice(("push_str", "push_str", "push_lit", "push_byte", "push_byte", "clear"))
        if k == "push_str":
            return K.PushStr(tgt, self.value(K.STRING, scope, 2, "borrow"))
        if k == "push_lit":
            return K.PushStr(tgt, K.StrLit(self.ascii()))
        if k == "push_byte":
            return K.PushByte(tgt, self.expr(K.I8, scope, 2))
        return K.Clear(tgt)

    def store(self, scope):
        places = [p for p in self.write_places(scope, lambda tt: True, 1) if isinstance(p, (K.Index, K.Field))]
        if not places:
            return self.print_(scope)
        tgt = self.choice(places)
        self.stmt_seen.add(TC.base_var(tgt))
        el = self.ty(tgt, scope)
        if el in K.ARITH and self.r.random() < 0.5:
            op = self.choice(("add", "sub", "mul") + (("band", "bor", "bxor") if el in K.ARITH_INT else ()))
            self.stmt_hidden.add(TC.base_var(tgt))
            if (K.is_unsigned(el) and op in K.WRAPPING_OPS
                    and any(isinstance(n, K.Call) for n in K.walk(tgt))):
                # the subset's rule: Rust would evaluate the index twice
                return K.Store(tgt, self.value(el, scope, 2, "consume"))
            if self.r.random() < 0.3 and el in K.ARITH_INT and op in ("add", "sub"):
                return K.CompoundStore(op, tgt, K.IntLit(1, el))
            return K.CompoundStore(op, tgt, self.expr(el, scope, 2))
        return K.Store(tgt, self.value(el, scope, 2, "consume"))

    def ascii(self):
        return "".join(self.choice(TEXT_POOL) for _ in range(self.r.randint(0, 5)))

    def print_(self, scope):
        r = self.r
        segs = []
        for _ in range(r.randint(0, 4)):
            if r.random() < 0.5:
                segs.append(K.Text("".join(self.choice(TEXT_POOL) for _ in range(r.randint(1, 5)))))
                continue
            kind = self.choice("dddfcss")
            if kind == "d":
                t = self.choice(K.ARITH_INT)
                flag, width = "", None
                if r.random() < 0.5:
                    width = r.randint(1, 12)
                    flag = self.choice(["", "-", "0"])
                segs.append(K.Spec("d", self.expr(t, scope, 2), flag, width, None))
            elif kind == "f":
                segs.append(K.Spec("f", self.expr(K.F64, scope, 2), "", None, self.choice([0, 1, 2, 3, 6, 10])))
            elif kind == "c":
                segs.append(K.Spec("c", self.expr(K.I32, scope, 1), "", None, None))
            elif r.random() < 0.5:
                s = "".join(self.choice(TEXT_POOL) for _ in range(r.randint(0, 4)))
                segs.append(K.Spec("s", K.StrLit(s), "", None, None))
            else:
                segs.append(K.Spec("s", self.value(K.STRING, scope, 2, "borrow"), "", None, None))
        return K.Print(tuple(segs))

    # -------------------------------------------------------------- exprs
    def lit(self, t):
        r = self.r
        if t == K.BOOL:
            return K.BoolLit(r.random() < 0.5)
        if t == K.F64:
            return K.FloatLit(self.choice(FLOATS))
        lo, hi = K.INT_RANGE[t.name]
        if t == K.I8:
            hi = 127
        roll = r.random()
        if roll < 0.6:
            v = r.randint(0, min(hi, 100))
        elif roll < 0.8:
            v = hi
        else:
            v = r.randint(0, hi)
        return K.IntLit(v, t)

    def call(self, name, params, scope, d, counted=False):
        """A call obeying the alias rules: a `T&` argument is a variable
        not mentioned anywhere else in the statement."""
        if not counted and not name.endswith("::new"):
            self.count_call()
        args = [None] * len(params)
        for i, p in enumerate(params):
            if isinstance(p.ty, K.PtrT) and p.ty.mutable:
                # FR-261: a mutable pointer argument claims its root first
                args[i] = self.ptr_value(scope, p.ty)
                if args[i] is None:
                    raise _NoCall()
                continue
            if isinstance(p.ty, K.RefT) and p.ty.mutable:
                vs = [v for v in self.vars_of(scope, lambda t, m: t == p.ty.inner, write=True)
                      if v[0] not in self.stmt_seen]
                if not vs:
                    raise _NoCall()
                vn = self.choice(vs)[0]
                args[i] = self.var(vn)
                self.stmt_hidden.add(vn)
        for i, p in enumerate(params):
            if args[i] is not None:
                continue
            if isinstance(p.ty, K.PtrT):
                args[i] = self.ptr_value(scope, p.ty)
                if args[i] is None:
                    raise _NoCall()
                continue
            if isinstance(p.ty, K.RefT):
                args[i] = self.value(p.ty.inner, scope, d - 1, "borrow")
            elif K.is_copy(p.ty):
                args[i] = self.expr(p.ty, scope, d - 1)
            else:
                args[i] = self.value(p.ty, scope, d - 1, "consume")
        return K.Call(name, tuple(args))

    # ------------------------------------------------ FR-261 pointers
    @contextlib.contextmanager
    def ptr_stream(self):
        """Every random draw made while generating a pointer construct comes
        from the pointer stream, so the FR-250..260 constructs of a seed keep
        (nearly) the sequence -- and the coverage -- they had before FR-261."""
        saved, self.r = self.r, self.pr
        try:
            yield
        finally:
            self.r = saved

    def scope_types(self, scope):
        """Pointee types a pointer local could be made of right now: the
        visible variables' (referent) types, their elements and members, the
        pointees of pointer variables."""
        out = []
        for name, t, w in self.vars_of(scope, lambda t, m: True):
            for steps, tt in self.access_paths(name, t, 1 + (1 if self.sdefs else 0)):
                if not isinstance(tt, K.PtrT) and tt not in out and tt != K.UNIT:
                    out.append(tt)
                if isinstance(tt, K.VecT) and K.elem_ok(tt.elem) and tt.elem not in out:
                    out.append(tt.elem)
        return out

    def ptr_value(self, scope, want):
        with self.ptr_stream():
            return self._ptr_value(scope, want)

    def _ptr_value(self, scope, want):
        """A pointer value for a `want` position (a `T*` where `const T*` is
        wanted is fine), claiming its root: a mutable one's root is then
        hidden for the rest of the statement, a const one's seen."""
        T, mut = want.inner, want.mutable
        if not self.types_visible([T]):
            return None
        kinds = ["vec", "addr"]
        self.pr.shuffle(kinds)
        kinds.insert(0 if self.pr.random() < 0.5 else 2, "var")
        for kind in kinds:
            e = getattr(self, "pv_" + kind)(scope, T, mut)
            if e is not None:
                root = TC.ptr_root(e)
                self.stmt_seen.add(root)
                if mut:
                    self.stmt_hidden.add(root)
                return e
        return None

    def pv_var(self, scope, T, mut):
        cands = []
        for name, t, w in self.vars_of(scope, lambda t, m: isinstance(t, K.PtrT) and t.inner == T):
            if mut and (not t.mutable or name in self.frozen or name in self.stmt_seen):
                continue
            cands.append(name)
        if not cands:
            return None
        name = self.pr.choice(cands)
        self.stmt_seen.add(name)
        if self.pr.random() < 0.3:
            start = self.index_for(scope, name) if mut else self.expr(K.USIZE, scope, 1)
            return K.Offset(K.Var(name), start)
        return K.Var(name)

    def pv_vec(self, scope, T, mut):
        if not K.elem_ok(T):
            return None
        pred = lambda tt: tt == K.VecT(T)  # noqa: E731
        pl = self.write_places(scope, pred, 1, ptr=True) if mut else self.places(scope, pred, 1, ptr=True)
        pl = [p for p in pl if not any(isinstance(n, (K.Call, K.MethodCall)) for n in K.walk(p))]
        if not pl:
            return None
        base = self.pr.choice(pl)
        root = TC.base_var(base)
        self.stmt_seen.add(root)
        if self.pr.random() < 0.5:
            return K.Offset(base, None)
        return K.Offset(base, self.index_for(scope, root) if mut else self.expr(K.USIZE, scope, 1))

    def pv_addr(self, scope, T, mut):
        me = scope.lookup("self")
        if (me is not None and K.deref(me[0]) == T and self.visible("self") and self.pr.random() < 0.8
                and (not mut or (me[0].mutable and "self" not in self.frozen and "self" not in self.stmt_seen))):
            return K.AddrOf(K.Var("self"))  # `this`
        pred = lambda tt: tt == T  # noqa: E731
        pl = self.write_places(scope, pred, 1, ptr=True) if mut else self.places(scope, pred, 1, ptr=True)
        if not pl:
            return None
        p = self.pr.choice(pl)
        self.stmt_seen.add(TC.base_var(p))
        if isinstance(p, K.Index):
            return K.Offset(p.base, p.index)  # N17: `&v[i]` is the suffix
        return K.AddrOf(p)

    def ptr_stmt(self, scope):
        with self.ptr_stream():
            return self._ptr_stmt(scope)

    def _ptr_stmt(self, scope):
        """A pointer local (`T* q = ..;`), a re-slice (`p += k;`), a store
        through a pointer (`*p = e;` / `p[i] op= e;`), a push or a member
        call through one (`p->push_back(e);`, `p->m(..);`)."""
        k = self.pr.choice(("plet", "plet", "pslice", "pstore", "pstore", "pvec", "pvec", "pmcall", "pmcall",
                            "pmcall"))
        if k == "pmcall":
            for _ in range(4):
                try:
                    return K.ExprStmt(self.mcall(scope, 2, None, ptr="only"))
                except _NoCall:
                    self.begin_stmt()
            return None
        if k == "pvec":
            places = self.write_places(scope, lambda tt: isinstance(tt, K.VecT), 1, ptr="only")
            if not places:
                return None
            tgt = self.target(places)
            return K.Push(tgt, self.value(self.ty(tgt, scope).elem, scope, 2, "consume"))
        if k == "plet":
            e = None
            pool = self.scope_types(scope)
            me = scope.lookup("self")
            if me is not None and self.visible("self") and self.pr.random() < 0.5:
                pool = [K.deref(me[0])]  # `S* q = this;`
            for _ in range(4):
                if not pool:
                    break
                want = K.PtrT(self.pr.choice(pool), self.pr.random() < 0.5)
                e = self._ptr_value(scope, want)
                if e is not None:
                    break
            if e is None:
                return None
            root = TC.ptr_root(e)
            roots = {root} | self.roots_of.get(root, set())
            self.pcounter += 1
            name = "q%d" % self.pcounter  # (its own counter: the main names stay put)
            self.roots_of[name] = roots
            # R2: a mutable pointer local hides its roots for the rest of the
            # block, a const one freezes them
            if want.mutable:
                self.hidden |= roots
            else:
                self.frozen |= roots
            mut = self.pr.random() < 0.6
            self.declare(scope, name, want, mut)
            return K.Let(name, want, mut, e)
        if k == "pslice":
            cands = [(n, t) for n, t, w in self.vars_of(scope, lambda t, m: isinstance(t, K.PtrT), write=True)
                     if n not in self.stmt_seen]
            if not cands:
                return None
            name, t = self.pr.choice(cands)
            self.stmt_seen.add(name)
            self.stmt_hidden.add(name)
            if self.pr.random() < 0.4:
                return K.CompoundAssign("add", name, K.IntLit(1, K.USIZE))  # `++p;`
            return K.CompoundAssign("add", name, self.index_for(scope, name) if t.mutable else self.expr(K.USIZE, scope, 1))
        places = [p for p in self.write_places(scope, lambda tt: True, 1, ptr="only")
                  if isinstance(p, (K.Index, K.Field))]
        if not places:
            return None
        tgt = self.choice(places)
        self.stmt_seen.add(TC.base_var(tgt))
        self.stmt_hidden.add(TC.base_var(tgt))
        el = self.ty(tgt, scope)
        if el in K.ARITH_INT and self.pr.random() < 0.4 and not any(isinstance(n, K.Call) for n in K.walk(tgt)):
            return K.CompoundStore("add", tgt, K.IntLit(1, el))
        return K.Store(tgt, self.value(el, scope, 2, "consume"))

    def count_call(self):
        if self.ncalls:
            raise _NoCall()
        self.ncalls = 1

    def mcall(self, scope, d, want, ptr=False):
        """FR-257: `r.m(args)` on a place, obeying the receiver alias rules:
        a non-const member function's receiver variable is not mentioned
        anywhere else in the statement (and not by the arguments, which also
        rules out `v[0].m(v[1])` E0502 and `s.m(&s)`); a const one's is not
        MODIFIED by the arguments (they may read it)."""
        cands = [m for m in self.msigs if m[3] in ("ref", "mut") and (want is None or m[2] == want)
                 and self.types_visible([K.StructT(m[4])] + [p.ty for p in m[1]] + [m[2]])]
        if not cands:
            raise _NoCall()
        self.count_call()
        mp, params, ret, recv, owner = self.choice(cands)
        st = K.StructT(owner)
        if recv == "mut":
            places = self.write_places(scope, lambda tt: tt == st, 1, ptr=ptr)
        else:
            places = self.places(scope, lambda tt: tt == st, 1, hide_root=True, ptr=ptr)
        if not places:
            raise _NoCall()
        fields = [p for p in places if isinstance(p, K.Field)]
        rv = self.choice(fields) if fields and self.r.random() < 0.5 else self.choice(places)
        b = TC.base_var(rv)
        self.stmt_seen.add(b)
        if recv == "mut":
            self.stmt_hidden.add(b)
        c = self.call(mp, params, scope, d, counted=True)
        return K.MethodCall(rv, mp.rsplit("::", 1)[1], c.args)

    def value(self, t, scope, depth, pos, is_return=False):
        """An expression of type t for a position: 'consume' (moved into a
        new owner), 'copy' (C++ copies from const T&), 'borrow' (read)."""
        if K.is_copy(t):
            return self.expr(t, scope, depth)
        r = self.r
        roll = r.random()
        if pos == "consume":
            movable = [v for v in self.vars_of(scope, lambda tt, m: tt == t, write=True)
                       if not self.is_ref(scope, v[0]) and v[0] not in self.stmt_seen
                       and (is_return or self.decl_depth.get(v[0], 0) == self.loop_depth)]
            if movable and roll < 0.4:
                name = self.choice(movable)[0]
                self.dead.add(name)
                self.stmt_seen.add(name)
                return K.Var(name)
            if roll < 0.65:
                pl = self.places(scope, lambda tt: tt == t, 1)
                if pl:
                    return K.Clone(self.choice(pl))
            return self.temp(t, scope, depth)
        if roll < 0.6:
            pl = self.places(scope, lambda tt: tt == t, 1)
            if pl:
                return self.choice(pl)
        return self.temp(t, scope, depth)

    def temp(self, t, scope, depth):
        r = self.r
        d = depth - 1
        fs = [s for s in self.callable_sigs() if s[2] == t and not s[0].endswith("::new")]
        if fs and depth > 1 and r.random() < 0.2:
            try:
                name, params, _ = self.choice(fs)
                return self.call(name, params, scope, d)
            except _NoCall:
                pass
        if isinstance(t, K.StructT):
            if depth > 1 and r.random() < 0.15:
                try:
                    return self.mcall(scope, d, t)
                except _NoCall:
                    pass
            sd = self.sd_by[t.path]
            if self.has_ctor(t.path):
                ctor = [m for m in self.msigs if m[3] == "ctor" and m[4] == t.path][0]
                try:
                    return self.call(ctor[0], ctor[1], scope, max(d, 0))
                except _NoCall:
                    pass
            return K.StructLit(t.path, tuple(self.value(f.ty, scope, max(d, 0), "consume") for f in sd.fields))
        if t == K.STRING:
            k = self.choice(("new", "from", "from", "to_string", "substr", "add", "add") if depth > 0
                            else ("new", "from"))
            if k == "new":
                return K.StrNew()
            if k == "from":
                return K.StrFrom(self.ascii())
            if k == "to_string":
                return K.ToStr(self.expr(self.choice(K.ARITH_INT), scope, d))
            if k == "substr":
                return K.Substr(self.value(K.STRING, scope, d, "borrow"), self.expr(K.USIZE, scope, 1),
                                self.expr(K.USIZE, scope, 1) if r.random() < 0.5 else None)
            lhs = self.value(K.STRING, scope, d, "consume")
            rhs = K.StrLit(self.ascii()) if r.random() < 0.3 else self.value(K.STRING, scope, d, "borrow")
            return K.Binary("add", lhs, rhs)
        el = t.elem
        k = self.choice(("lit", "lit", "fill", "empty") if depth > 0 else ("empty",))
        if k == "empty":
            return K.VecLit(el, ())
        if k == "fill":
            return K.VecFill(self.value(el, scope, d, "copy"), self.expr(K.USIZE, scope, 1))
        return K.VecLit(el, tuple(self.value(el, scope, d, "copy") for _ in range(r.randint(1, 3))))

    def expr(self, t, scope, depth):
        r = self.r
        if self.fr259 and t in K.ALL_PRIMS and depth > 0 and self.fr.random() < FR259_RATE:
            # FR-259: `c ? a : b` / an increment used as a value (own stream)
            with self.fr259_stream():
                e = self.fr259_expr(t, scope, depth - 1)
                if e is not None:
                    return e
        if self.pointers and t in K.ALL_PRIMS and self.pr.random() < 0.07:
            # FR-261: a read through a pointer, `*p` / `p[i]` / `p->f`
            with self.ptr_stream():
                pl = self.places(scope, lambda tt: tt == t, 1, ptr="only")
                if pl:
                    return self.pr.choice(pl)
        if depth <= 0 or r.random() < 0.25:
            vs = self.vars_of(scope, lambda tt, m: tt == t)
            if vs and r.random() < 0.6:
                return self.var(self.choice(vs)[0])
            return self.lit(t)
        d = depth - 1
        opts = ["cast", "call"]
        if t in K.ARITH:
            opts += ["neg", "arith", "arith"]
        if t in K.ARITH_INT:
            opts += ["not", "bit", "shift", "rem"]
        if t == K.BOOL:
            opts += ["cmp", "cmp", "logic", "notb", "ncmp", "empty"]
        if t == K.USIZE:
            opts += ["len", "len"]
        if t == K.I8:
            opts += ["byte"]
        if K.elem_ok(t):
            opts += ["index"]
        if self.sdefs:
            opts += ["field", "mcall"]
            if t == K.BOOL:
                opts += ["slcmp"]  # keeps FR-251's string-literal comparison coverage undiluted
        k = self.choice(opts)
        if k == "slcmp":
            a = self.value(K.STRING, scope, d, "borrow")
            lit = K.StrLit(self.ascii())
            op = self.choice(("eq", "ne"))
            return K.Binary(op, lit, a) if r.random() < 0.5 else K.Binary(op, a, lit)
        if k == "field":
            pl = [p for p in self.places(scope, lambda tt: tt == t, 1) if isinstance(p, K.Field)]
            if pl:
                return self.choice(pl)
            return self.lit(t)
        if k == "mcall":
            try:
                return self.mcall(scope, d, t)
            except _NoCall:
                return self.lit(t)
        if k == "cast":
            if t == K.BOOL:
                return self.expr(t, scope, d)
            srcs = [s for s in self.prims if s != t and (s != K.BOOL or K.is_int(t))]
            return K.Cast(t, self.expr(self.choice(srcs), scope, d))
        if k == "call":
            fs = [s for s in self.callable_sigs() if s[2] == t]
            if not fs:
                return self.lit(t)
            name, params, _ = self.choice(fs)
            try:
                return self.call(name, params, scope, d)
            except _NoCall:
                return self.lit(t)
        if k == "neg":
            return K.Unary("neg", self.expr(t, scope, d))
        if k in ("not", "notb"):
            return K.Unary("not", self.expr(t, scope, d))
        if k == "arith":
            op = self.choice(("add", "sub", "mul", "div"))
            return K.Binary(op, self.expr(t, scope, d), self.expr(t, scope, d))
        if k == "rem":
            return K.Binary("rem", self.expr(t, scope, d), self.expr(t, scope, d))
        if k == "bit":
            return K.Binary(self.choice(K.BIT_OPS), self.expr(t, scope, d), self.expr(t, scope, d))
        if k == "shift":
            return K.Binary(self.choice(K.SHIFT_OPS), self.expr(t, scope, d),
                            self.expr(self.choice(K.ARITH_INT), scope, d))
        if k == "cmp":
            ot = self.choice(K.ARITH)
            return K.Binary(self.choice(K.CMP_OPS), self.expr(ot, scope, d), self.expr(ot, scope, d))
        if k == "logic":
            return K.Binary(self.choice(K.LOGIC_OPS), self.expr(t, scope, d), self.expr(t, scope, d))
        if k == "ncmp":
            if r.random() < 0.6:
                op = self.choice(K.CMP_OPS)
                a = self.value(K.STRING, scope, d, "borrow")
                if op in ("eq", "ne") and r.random() < 0.6:
                    lit = K.StrLit(self.ascii())
                    return K.Binary(op, lit, a) if r.random() < 0.5 else K.Binary(op, a, lit)
                return K.Binary(op, a, self.value(K.STRING, scope, d, "borrow"))
            vt = self.choice(NC_TYPES[2:])
            return K.Binary(self.choice(("eq", "ne")), self.value(vt, scope, d, "borrow"),
                            self.value(vt, scope, d, "borrow"))
        if k in ("len", "empty"):
            bt = self.choice(NC_TYPES)
            b = self.value(bt, scope, d, "borrow")
            return K.Len(b) if k == "len" else K.IsEmpty(b)
        if k == "byte":
            return K.ByteAt(self.value(K.STRING, scope, d, "borrow"), self.expr(K.USIZE, scope, 1))
        if k == "index":
            return K.Index(self.value(K.VecT(t), scope, d, "borrow"), self.expr(K.USIZE, scope, 1))
        raise AssertionError(k)


    # ------------------------------------------------ FR-259
    @contextlib.contextmanager
    def fr259_stream(self):
        saved, self.r = self.r, self.fr
        try:
            yield
        finally:
            self.r = saved

    def fr259_stmt(self, scope):
        """`T y = <x++ / --x ..> op e;` on a writable integer variable, or (none
        yet) a mutable counter `T c = lit;` for later statements to use."""
        r = self.r
        t = r.choice(K.ARITH_INT)
        vs = [v for v in self.vars_of(scope, lambda tt, m: tt == t, write=True) if v[0] not in self.stmt_seen]
        name = self.fresh()
        if not vs:
            self.declare(scope, name, t, True)
            return K.Let(name, t, True, self.lit(t))
        x = r.choice(vs)[0]
        self.stmt_seen.add(x)
        self.stmt_hidden.add(x)
        e = K.IncDec(K.Var(x), r.choice(K.INCDEC_OPS), r.random() < 0.5)
        if r.random() < 0.6:
            e = K.Binary(r.choice(("add", "mul", "bxor")), e, self.expr(t, scope, 2))
        mut = r.random() < 0.5
        self.declare(scope, name, t, mut)
        return K.Let(name, t, mut, e)

    def fr259_expr(self, t, scope, d):
        """A Cond of type ``t``, or (integer ``t``) an IncDec of a writable
        variable the statement has not mentioned yet -- then hidden for the
        rest of it -- or None."""
        r = self.r
        if t in K.ARITH_INT and r.random() < 0.8:
            vs = [v for v in self.vars_of(scope, lambda tt, m: tt == t, write=True) if v[0] not in self.stmt_seen]
            if vs:
                name = r.choice(vs)[0]
                self.stmt_seen.add(name)
                self.stmt_hidden.add(name)
                return K.IncDec(K.Var(name), r.choice(K.INCDEC_OPS), r.random() < 0.5)
        return K.Cond(self.expr(K.BOOL, scope, d), self.expr(t, scope, d), self.expr(t, scope, d))


    # ------------------------------------------------ FR-252 closures
    @contextlib.contextmanager
    def closure_stream(self):
        saved, self.r = self.r, self.cr
        try:
            yield
        finally:
            self.r = saved

    def closure_group(self, scope, depth):
        r = self.r
        self.begin_stmt()
        move = r.random() < 0.5
        cands = [v for v in self.vars_of(scope, lambda t, m: t in self.prims)
                 if not (move and self.is_ref(scope, v[0]))]
        caps = r.sample(cands, min(len(cands), r.randint(0, 2)))
        cs = TC.Scope()
        for name, _, _ in caps:
            t, m = scope.lookup(name)
            cs.declare(name, t, m)
        params = []
        for _ in range(r.randint(0, 2)):
            pn = self.fresh()
            pt = self.choice(self.prims)
            pm = r.random() < 0.5
            params.append(K.Param(pn, pt, pm))
            cs.declare(pn, pt, pm)
            self.decl_depth[pn] = 0
        ret = self.choice(self.prims + (K.UNIT, K.UNIT))
        saved = (self.ret, self.loop_depth, self.in_closure)
        self.ret, self.loop_depth, self.in_closure = ret, 0, True
        try:
            tail = None
            if ret != K.UNIT:
                def tail():
                    self.begin_stmt()
                    return K.Return(self.value(ret, cs, 3, "consume", is_return=True))
            bs = cs.child()
            pre = []
            for cname, dt, w in caps:
                # use every capture: modify it (FnMut; C++ `mutable` when by
                # copy) or read it into a local
                if w and dt in K.ARITH and r.random() < 0.5:
                    pre.append(K.CompoundAssign(self.choice(("add", "sub", "mul")), cname, self.lit(dt)))
                else:
                    v = self.fresh()
                    self.declare(bs, v, dt, False)
                    pre.append(K.Let(v, dt, False, K.Var(cname)))
            body = pre + self.stmts(bs, min(depth + 1, 2), False, r.randint(0, 3), tail=tail)
        finally:
            self.ret, self.loop_depth, self.in_closure = saved
        name = self.fresh()
        clo = K.Closure(name, move, tuple(params), ret, K.Block(tuple(body)))
        out = [clo]
        ft = K.FnT(tuple(p.ty for p in params), ret)
        scope.declare(name, ft, False)
        self.dead.add(name)  # only the calls below mention it (its live range ends with them)
        hide = set() if move else {c[0] for c in caps} - self.hidden
        self.hidden |= hide
        try:
            for _ in range(r.randint(1, 3)):
                self.begin_stmt()
                call = K.ClosureCall(name, tuple(self.expr(p.ty, scope, 2) for p in params))
                if ret == K.UNIT or r.random() < 0.3:
                    out.append(K.ExprStmt(call))
                else:
                    v = self.fresh()
                    m = r.random() < 0.5
                    self.declare(scope, v, ret, m)
                    out.append(K.Let(v, ret, m, call))
        finally:
            self.hidden -= hide
        return out


class _NoCall(Exception):
    pass


def gen_program(seed, nfuncs, prefix="f", namespaces=True, pointers=False, fr259=False, closures=False):
    return Gen(seed, namespaces, pointers=pointers, fr259=fr259, closures=closures).program(nfuncs, prefix)
