"""Seeded random generator of well-typed, NORMAL Kernel programs (FR-250).

The property tests state the bijection laws over its output, so it must
reach every Kernel construct: all scalar types, every operator, casts,
calls (forward and recursive, hence prototypes), every statement form,
else-if chains, the counting loop, and printf with every spec kind, flag
and escape-worthy text character.  Output is passed through
``typecheck.normalize`` (laws are stated over normal forms) and checked.
"""

import random

from . import kernel as K
from . import typecheck as TC

FLOATS = ["0.0", "0.5", "1.0", "2.5", "0.125", "1e-05", "123.25", "1e+300", "3.0", "100.0",
          "0.1", "6.02e+23"]
TEXT_POOL = list("abcXYZ019 ,.:;=!?%{}\"\\'\n\t()[]#@$&*+-/<>|^~`_")


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def choice(self, xs):
        return self.r.choice(xs)

    # -------------------------------------------------------------- program
    def program(self, nfuncs, prefix="f"):
        r = self.r
        sigs = []
        for i in range(nfuncs):
            nparams = r.randint(0, 4)
            params = tuple(K.Param("p%d" % j, self.choice(K.ALL_PRIMS), r.random() < 0.6)
                           for j in range(nparams))
            ret = self.choice(K.ALL_PRIMS + (K.UNIT, K.UNIT))
            sigs.append(("%s%d" % (prefix, i), params, ret))
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
        scope = TC.Scope()
        for p in params:
            scope.declare(p.name, p.ty, p.mutable)
        stmts = self.stmts(scope, depth=0, loop=False, n=self.r.randint(1, 6))
        if ret != K.UNIT:
            stmts.append(K.Return(self.expr(ret, scope, 3)))
        return K.Func(name, params, ret, K.Block(tuple(stmts)))

    def fresh(self):
        self.counter += 1
        return "v%d" % self.counter

    # -------------------------------------------------------------- stmts
    def stmts(self, scope, depth, loop, n):
        return [self.stmt(scope, depth, loop) for _ in range(n)]

    def vars_of(self, scope, pred):
        out = []
        s = scope
        seen = set()
        while s is not None:
            for name, (t, m) in s.vars.items():
                if name not in seen:
                    seen.add(name)
                    if pred(t, m):
                        out.append((name, t, m))
            s = s.parent
        return sorted(out)

    def block(self, scope, depth, loop, n=None):
        inner = scope.child()
        if n is None:
            n = self.r.randint(0, 3)
        return K.Block(tuple(self.stmts(inner, depth + 1, loop, n)))

    def stmt(self, scope, depth, loop):
        r = self.r
        kinds = ["let", "let", "assign", "compound", "print", "call"]
        if depth < 3:
            kinds += ["if", "if", "while", "for", "block"]
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
            scope.declare(name, t, mut)
            return K.Let(name, t, mut, e)
        if k == "assign":
            vs = self.vars_of(scope, lambda t, m: m)
            if not vs:
                return self.stmt(scope, depth, loop) if r.random() < 0.5 else K.Break() if loop else self.print_(scope)
            name, t, _ = self.choice(vs)
            return K.Assign(name, self.expr(t, scope, 3))
        if k == "compound":
            vs = self.vars_of(scope, lambda t, m: m and t in K.ARITH)
            if not vs:
                return self.print_(scope)
            name, t, _ = self.choice(vs)
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
            return K.ExprStmt(K.Call(name, tuple(self.expr(p.ty, scope, 2) for p in params)))
        if k == "if":
            cond = self.expr(K.BOOL, scope, 3)
            then = self.block(scope, depth, loop)
            roll = r.random()
            if roll < 0.3:
                els = None
            elif roll < 0.6:
                els = K.Block((self.stmt(scope.child(), depth + 1, loop),))
                if not isinstance(els.stmts[0], K.If):
                    els = K.Block((K.If(self.expr(K.BOOL, scope, 2), self.block(scope, depth, loop),
                                        self.block(scope, depth, loop) if r.random() < 0.5 else None),))
            else:
                els = self.block(scope, depth, loop)
            return K.If(cond, then, els)
        if k == "while":
            return K.While(self.expr(K.BOOL, scope, 3), self.block(scope, depth, True))
        if k == "for":
            t = self.choice((K.I32, K.USIZE))
            lo = self.expr(t, scope, 2)
            var = self.fresh()
            inner = scope.child()
            inner.declare(var, t, False)
            body = K.Block(tuple(self.stmts(inner, depth + 1, True, r.randint(0, 3))))
            assigned = TC.assigned_names(body)
            cands = [v for v in self.vars_of(scope, lambda tt, m: tt == t) if v[0] not in assigned]
            if cands and r.random() < 0.5:
                hi = K.Var(self.choice(cands)[0])
            else:
                hi = self.lit(t)
            return K.For(var, lo, hi, body)
        if k == "block":
            return self.block(scope, depth, loop, self.r.randint(1, 3))
        if k == "break":
            return K.Break()
        if k == "continue":
            return K.Continue()
        if k == "return":
            if self.ret == K.UNIT:
                return K.Return(None)
            return K.Return(self.expr(self.ret, scope, 3))
        raise AssertionError(k)

    def print_(self, scope):
        r = self.r
        segs = []
        for _ in range(r.randint(0, 4)):
            if r.random() < 0.5:
                segs.append(K.Text("".join(self.choice(TEXT_POOL) for _ in range(r.randint(1, 5)))))
                continue
            kind = self.choice("dddfcs")
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
            else:
                s = "".join(self.choice(TEXT_POOL) for _ in range(r.randint(0, 4)))
                segs.append(K.Spec("s", K.StrLit(s), "", None, None))
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

    def expr(self, t, scope, depth):
        r = self.r
        if depth <= 0 or r.random() < 0.25:
            vs = self.vars_of(scope, lambda tt, m: tt == t)
            if vs and r.random() < 0.6:
                return K.Var(self.choice(vs)[0])
            return self.lit(t)
        d = depth - 1
        opts = ["cast", "call"]
        if t in K.ARITH:
            opts += ["neg", "arith", "arith"]
        if t in K.ARITH_INT:
            opts += ["not", "bit", "shift", "rem"]
        if t == K.BOOL:
            opts += ["cmp", "cmp", "logic", "notb"]
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
            return K.Call(name, tuple(self.expr(p.ty, scope, d - 1) for p in params))
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
        raise AssertionError(k)


def gen_program(seed, nfuncs, prefix="f"):
    return Gen(seed).program(nfuncs, prefix)
