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
"""

import random

from . import kernel as K
from . import typecheck as TC

FLOATS = ["0.0", "0.5", "1.0", "2.5", "0.125", "1e-05", "123.25", "1e+300", "3.0", "100.0",
          "0.1", "6.02e+23"]
TEXT_POOL = list("abcXYZ019 ,.:;=!?%{}\"\\'\n\t()[]#@$&*+-/<>|^~`_")
NC_TYPES = (K.STRING, K.STRING, K.VecT(K.I32), K.VecT(K.I64), K.VecT(K.U8), K.VecT(K.F64), K.VecT(K.I8),
            K.VecT(K.USIZE), K.VecT(K.STRING), K.VecT(K.VecT(K.I32)))
VALUE_TYPES = K.ALL_PRIMS + NC_TYPES


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def choice(self, xs):
        return self.r.choice(xs)

    # -------------------------------------------------------------- program
    def param_type(self):
        r = self.r
        roll = r.random()
        if roll < 0.55:
            return self.choice(K.ALL_PRIMS), r.random() < 0.6
        if roll < 0.75:
            return self.choice(NC_TYPES), r.random() < 0.6
        inner = self.choice(NC_TYPES + (K.I32, K.I64, K.U32, K.F64, K.BOOL))
        return K.RefT(inner, r.random() < 0.5), False

    def program(self, nfuncs, prefix="f"):
        r = self.r
        sigs = []
        for i in range(nfuncs):
            nparams = r.randint(0, 4)
            params = []
            for j in range(nparams):
                t, m = self.param_type()
                params.append(K.Param("p%d" % j, t, m))
            ret = self.choice(K.ALL_PRIMS + (K.UNIT, K.UNIT) + NC_TYPES[:4])
            sigs.append(("%s%d" % (prefix, i), tuple(params), ret))
        self.sigs = sigs
        funcs = []
        for name, params, ret in sigs:
            funcs.append(self.func(name, params, ret))
        prog = TC.normalize(K.Program(tuple(funcs)))
        errs = TC.check(prog)
        assert not errs, errs[:3]
        return prog

    def func(self, name, params, ret):
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
        for p in params:
            scope.declare(p.name, p.ty, p.mutable)
            self.decl_depth[p.name] = 0
        stmts = self.stmts(scope, depth=0, loop=False, n=self.r.randint(1, 6))
        if ret != K.UNIT:
            self.begin_stmt()
            stmts.append(K.Return(self.value(ret, scope, 3, "consume", is_return=True)))
        return K.Func(name, params, ret, K.Block(tuple(stmts)))

    def fresh(self):
        self.counter += 1
        return "v%d" % self.counter

    def begin_stmt(self):
        self.stmt_seen = set()
        self.stmt_hidden = set()

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
    def stmts(self, scope, depth, loop, n):
        return [self.stmt(scope, depth, loop) for _ in range(n)]

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
            t = self.choice(K.ALL_PRIMS)
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
            vs = self.vars_of(scope, lambda t, m: True, write=True)
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
            name, params, ret = self.choice(self.sigs)
            try:
                return K.ExprStmt(self.call(name, params, scope, 2))
            except _NoCall:
                return self.print_(scope)
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
                els = K.Block((self.stmt(scope.child(), depth + 1, loop),))
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
                                             index_ok=False) if p.name not in lo_hidden]
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
        return {n: (tuple(p.ty for p in ps), ret) for n, ps, ret in self.sigs}

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
    def write_places(self, scope, pred, depth):
        """Writable places (Var or element) whose type satisfies pred."""
        out = []
        for name, t, w in self.vars_of(scope, lambda t, m: True, write=True):
            if name in self.stmt_seen:
                continue
            if pred(t):
                out.append(K.Var(name))
            if depth > 0 and isinstance(t, K.VecT) and pred(t.elem):
                out.append(K.Index(K.Var(name), self.index_for(scope, name)))
        return out

    def index_for(self, scope, base):
        """A usize index expression that does not read ``base``."""
        added = base not in self.stmt_hidden
        self.stmt_hidden.add(base)
        try:
            return self.expr(K.USIZE, scope, 1)
        finally:
            if added:
                self.stmt_hidden.discard(base)

    def places(self, scope, pred, depth, index_ok=True):
        out = []
        for name, t, w in self.vars_of(scope, lambda t, m: True):
            if pred(t):
                out.append(K.Var(name))
            if index_ok and depth > 0 and isinstance(t, K.VecT) and pred(t.elem):
                out.append(K.Index(K.Var(name), self.expr(K.USIZE, scope, 1)))
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
        places = [p for p in self.write_places(scope, lambda tt: True, 1) if isinstance(p, K.Index)]
        if not places:
            return self.print_(scope)
        tgt = self.choice(places)
        self.stmt_seen.add(TC.base_var(tgt))
        el = self.ty(tgt, scope)
        if el in K.ARITH and self.r.random() < 0.5:
            op = self.choice(("add", "sub", "mul") + (("band", "bor", "bxor") if el in K.ARITH_INT else ()))
            self.stmt_hidden.add(TC.base_var(tgt))
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

    def call(self, name, params, scope, d):
        """A call obeying the alias rules: a `T&` argument is a variable
        not mentioned anywhere else in the statement."""
        args = [None] * len(params)
        for i, p in enumerate(params):
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
            if isinstance(p.ty, K.RefT):
                args[i] = self.value(p.ty.inner, scope, d - 1, "borrow")
            elif K.is_copy(p.ty):
                args[i] = self.expr(p.ty, scope, d - 1)
            else:
                args[i] = self.value(p.ty, scope, d - 1, "consume")
        return K.Call(name, tuple(args))

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
        fs = [s for s in self.sigs if s[2] == t]
        if fs and depth > 1 and r.random() < 0.2:
            try:
                name, params, _ = self.choice(fs)
                return self.call(name, params, scope, d)
            except _NoCall:
                pass
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
        k = self.choice(opts)
        if k == "cast":
            if t == K.BOOL:
                return self.expr(t, scope, d)
            srcs = [s for s in K.ALL_PRIMS if s != t and (s != K.BOOL or K.is_int(t))]
            return K.Cast(t, self.expr(self.choice(srcs), scope, d))
        if k == "call":
            fs = [s for s in self.sigs if s[2] == t]
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


class _NoCall(Exception):
    pass


def gen_program(seed, nfuncs, prefix="f"):
    return Gen(seed).program(nfuncs, prefix)
