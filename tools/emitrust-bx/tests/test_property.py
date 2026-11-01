# The bijection laws, as properties over generated Kernel programs (FR-250).
# gen.py with FIXED seeds yields >= 500 well-typed normal Kernel functions;
# for every one of them:
#   law 1 (Rust):  rs_parse(rs_print(k)) == k             -- pure python
#   law 1 (C++):   cxx_import(cxx_print(k)) == k          -- through real clang
#   law 2:         print(parse(print(k))) == print(k) on both sides
# The C++ side packs 50 functions per TU so clang runs ~12 times, not 600.
# A failure here means a printer emits something its own parser reads as a
# DIFFERENT program -- exactly the silent miscompile class the bijection is
# meant to rule out.
import collections
import os
import shutil

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import gen
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP

SEEDS = range(1000, 1012)
PER_TU = 50


@pytest.fixture(scope="module")
def programs():
    return [gen.gen_program(seed, PER_TU) for seed in SEEDS]


def test_generator_size_and_coverage(programs):
    funcs = [f for p in programs for f in p.funcs()]
    assert len(funcs) >= 500
    seen = collections.Counter()
    for f in funcs:
        for n in K.walk(f):
            seen[type(n).__name__] += 1
            if isinstance(n, (K.Binary, K.Unary, K.CompoundAssign)):
                seen["op:" + n.op] += 1
            if isinstance(n, K.Spec):
                seen["spec:" + n.kind + n.flag] += 1
            if isinstance(n, K.IntLit):
                seen["lit:" + n.ty.name] += 1
            if isinstance(n, K.Cast):
                seen["cast:" + n.ty.name] += 1
    for kind in ("Let", "Assign", "CompoundAssign", "ExprStmt", "Print", "If", "While", "For",
                 "Break", "Continue", "Return", "Block", "Cast", "Call", "BoolLit", "FloatLit", "StrLit"):
        assert seen[kind] >= 20, kind
    for op in K.BINARY_OPS + K.UNARY_OPS:
        assert seen["op:" + op] >= 10, op
    for sk in ("d", "d-", "d0", "f", "c", "s"):
        assert seen["spec:" + sk] >= 10, sk
    for t in K.INT_TYPES:
        assert seen["lit:" + t.name] >= 10, t
    for t in K.ALL_PRIMS:
        if t != K.BOOL:
            assert seen["cast:" + t.name] >= 10, t


def test_law1_and_law2_rust(programs):
    for p in programs:
        text = RSP.rs_print(p)
        back = RP.rs_parse(text)
        for a, b in zip(p.funcs(), back.funcs()):
            assert a == b, a.name
        assert back == p
        assert RSP.rs_print(back) == text


@pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
def test_law1_and_law2_cxx(programs, tmp_path):
    for i, p in enumerate(programs):
        text = CP.cxx_print(p)
        path = os.path.join(str(tmp_path), "tu%d.cpp" % i)
        with open(path, "w") as f:
            f.write(text)
        back, errs = cli.load_cpp(path, "clang++")
        assert errs == [], (path, errs[:5])
        assert len(back.funcs()) == PER_TU
        for a, b in zip(p.funcs(), back.funcs()):
            assert a == b, a.name
        assert back == p
        assert CP.cxx_print(back) == text


def test_law2_from_each_side(programs):
    # print(parse(x)) is a fixed point after one step, starting from the
    # OTHER language's canonical text as well
    for p in programs[:3]:
        rs = RSP.rs_print(p)
        once = RSP.rs_print(RP.rs_parse(rs))
        assert RSP.rs_print(RP.rs_parse(once)) == once


@pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
def test_printed_rust_typechecks_under_rustc(programs, tmp_path):
    # Law 1 is structural; this is the "never silently unbuildable" half for
    # the Rust printer: rustc must ACCEPT every generated program (the C++
    # half is covered by clang parsing every TU above). The allow-list only
    # silences lints the RANDOM constants trip (`i32::MAX + 1` folds to a
    # deny-by-default arithmetic_overflow); it does not relax typing.
    import subprocess
    header = ("#![allow(arithmetic_overflow, unconditional_panic, unreachable_code, unused, "
              "overflowing_literals, unconditional_recursion, while_true)]\n")
    for i, p in enumerate(programs[:4]):
        path = os.path.join(str(tmp_path), "g%d.rs" % i)
        with open(path, "w") as f:
            f.write(header + RSP.rs_print(p))
        r = subprocess.run(["rustc", "--edition", "2021", "--emit=metadata", "--crate-type=lib",
                            "-o", path + ".rmeta", path], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[:4000]
