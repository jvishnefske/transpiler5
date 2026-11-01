# Pins the first FR-254 normalizer rules (opt-in `--normalize`): R1
# for->while and R2 do-while. The invariants, and why each is load-bearing:
#  * DEFAULT OFF is byte-identical: without --normalize every non-canonical
#    `for` and every do-while keeps its existing refusal (the Reject goldens
#    and the census needles key on that wording);
#  * a rule only fires where the subset refuses: a canonical counting `for`
#    stays a Kernel For (`for i in a..b`), so the rules never move a golden;
#  * the rewrite is SOUND, and each guard exists because the naive rewrite is
#    not: a `continue` bound to the rewritten loop is refused (C++ runs the
#    step / tests the condition on `continue`, the while form would skip
#    them), a `continue` bound to a NESTED loop is not; a step or condition
#    that mentions a name the body declares at its top level keeps the body
#    as a nested block `{ B } S;` (splicing would capture the inner name --
#    the generator below contains exactly such loops, so a wrong splice
#    changes the printed output); `break` / `return` need no guard;
#  * the OUTPUT obeys the bijection laws: the normalized program's C++
#    re-imports WITHOUT --normalize to the same Kernel (cpp->cpp fixed
#    point) and rust->kernel->rust is identical (cli.check_roundtrip);
#  * DOUBLE VALIDATION (FR-254's contract for every rule): a seeded
#    generator of non-canonical loop shapes in ONE TU, compiled natively
#    BEFORE and AFTER normalization and as the emitted Rust, stdout
#    byte-identical across all three. Inputs are UB-free by construction
#    (accumulators reduced mod 10007, bounded counters, shifts < 32).
import os
import random
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import normalize as NZ
from emitrust_bx import rs_print as RSP

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
HEAD = "#include <cstdio>\n"
FOR_BAD = "unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`"


def imp(tmp_path, src, normalize=True, name="t.cpp"):
    p = tmp_path / name
    p.write_text(HEAD + src)
    notes = []
    prog, es = cli.load_cpp(str(p), "clang++", normalize, notes)
    n = HEAD.count("\n")
    return prog, [(loc.line - n, loc.col, m) for loc, m in es], [(loc.line - n, loc.col, m) for loc, m in notes]


def ok(tmp_path, src):
    prog, es, notes = imp(tmp_path, src)
    assert es == [], es
    assert cli.check_roundtrip(prog, "clang++") == []
    return prog, notes


def body_of(prog, name="f"):
    return [f for f in prog.funcs() if f.path == name][0].body


def cxx_lines(prog):
    return [ln.strip() for ln in CP.cxx_print(prog).splitlines()]


# ------------------------------------------------------------- default off
SHAPES = """int f(int n) {
    int s = 0;
    for (int i = 0; i < n; i += 4) {
        s += i;
    }
    for (int i = n; i != 0; --i) {
        s += i;
    }
    for (int i = 0; i < n;) {
        s += i;
        i += 3;
    }
    for (int c = 1;; c *= 2) {
        if (c > n) {
            break;
        }
        s += c;
    }
    unsigned u = 100000U;
    for (; u >= 128U; u >>= 7) {
        s += 1;
    }
    do {
        s += n % 10;
        n /= 10;
    } while (n != 0);
    return s;
}
"""


@needs_clang
def test_default_off_keeps_every_refusal(tmp_path):
    _, es, notes = imp(tmp_path, SHAPES, normalize=False)
    assert notes == []
    assert es == [(3, 5, FOR_BAD), (6, 5, FOR_BAD), (9, 5, FOR_BAD), (13, 5, FOR_BAD), (20, 5, FOR_BAD),
                  (23, 5, "unsupported: do-while loop")]


@needs_clang
def test_all_six_shapes_normalize_with_located_notes(tmp_path):
    prog, notes = ok(tmp_path, SHAPES)
    assert notes == [(3, 5, "normalized: for->while"), (6, 5, "normalized: for->while"),
                     (9, 5, "normalized: for->while"), (13, 5, "normalized: for->while"),
                     (20, 5, "normalized: for->while"), (23, 5, "normalized: do-while")]
    assert not any(isinstance(x, K.For) for x in K.walk(prog))
    lines = cxx_lines(prog)
    assert lines.count("while (true) {") == 2  # `for (..;; c *= 2)` and the do-while
    assert "if (!(n != 0)) {" in lines
    assert "while (u >= 128U) {" in lines


@needs_clang
def test_canonical_for_is_not_rewritten(tmp_path):
    prog, notes = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = 0; i < n; ++i) {\n"
                               "        s += i;\n    }\n    return s;\n}\n")
    assert notes == []
    assert [type(s) for s in body_of(prog).stmts] == [K.Let, K.For, K.Return]


# ------------------------------------------------------------- R1 shape
@needs_clang
def test_r1_declaring_init_is_scoped_by_an_outer_block(tmp_path):
    prog, _ = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = n; i > 0; i -= 2) {\n"
                           "        s += i;\n    }\n    int i = 7;\n    return s + i;\n}\n")
    st = body_of(prog).stmts
    assert [type(s) for s in st] == [K.Let, K.Block, K.Let, K.Return]
    blk = st[1]
    assert [type(s) for s in blk.stmts] == [K.Let, K.While]
    assert [type(s) for s in blk.stmts[1].body.stmts] == [K.CompoundAssign, K.CompoundAssign]


@needs_clang
def test_r1_expression_init_and_missing_cond_need_no_block(tmp_path):
    prog, _ = ok(tmp_path, "int f(int n) {\n    int i = 0;\n    for (i = n;; i -= 3) {\n"
                           "        if (i < 0) {\n            break;\n        }\n    }\n    return i;\n}\n")
    st = body_of(prog).stmts
    assert [type(s) for s in st] == [K.Let, K.Assign, K.While, K.Return]
    assert st[2].cond == K.BoolLit(True)


@needs_clang
def test_r1_multi_declaration_init_is_sequential_lets(tmp_path):
    prog, _ = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = 0, j = n; i < j; ++i) {\n"
                           "        s += j;\n        j -= 1;\n    }\n    return s;\n}\n")
    blk = body_of(prog).stmts[1]
    assert [(type(s), getattr(s, "name", None)) for s in blk.stmts] == [(K.Let, "i"), (K.Let, "j"), (K.While, None)]


@needs_clang
def test_r1_step_capture_keeps_the_body_nested(tmp_path):
    # `i += t` is the OUTER t; splicing it after `int t = ...` would read the inner one
    prog, _ = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    int t = 2;\n    for (int i = 0; i < n; i += t) {\n"
                           "        int t = i % 3;\n        s += t;\n    }\n    return s;\n}\n")
    w = body_of(prog).stmts[2].stmts[1]
    assert isinstance(w, K.While)
    assert [type(s) for s in w.body.stmts] == [K.Block, K.CompoundAssign]
    assert [ln for ln in cxx_lines(prog)][-8:-3] == ["int t = i % 3;", "s += t;", "}", "i += t;", "}"]


@needs_clang
def test_r1_no_capture_splices(tmp_path):
    prog, _ = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = 0; i < n; i += 2) {\n"
                           "        int t = i % 3;\n        s += t;\n    }\n    return s;\n}\n")
    w = body_of(prog).stmts[1].stmts[1]
    assert [type(s) for s in w.body.stmts] == [K.Let, K.CompoundAssign, K.CompoundAssign]


@needs_clang
def test_r1_break_and_return_need_no_guard(tmp_path):
    prog, notes = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = 0; i < n; i += 2) {\n"
                                "        if (s > 50) {\n            return s;\n        }\n"
                                "        if (i == 9) {\n            break;\n        }\n        s += i;\n    }\n"
                                "    return s;\n}\n")
    assert len(notes) == 1


@needs_clang
def test_r1_nested_rewrites_and_inner_continue(tmp_path):
    # the `continue`s are bound to a canonical For and to a plain while: fine
    prog, notes = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = n; i > 0; i -= 3) {\n"
                                "        for (int j = 0; j < 4; ++j) {\n            if (j == 2) {\n"
                                "                continue;\n            }\n            s += j;\n        }\n"
                                "        int k = 0;\n        while (k < 2) {\n            k += 1;\n"
                                "            if (k == 1) {\n                continue;\n            }\n"
                                "            s += k;\n        }\n"
                                "        for (int j = i; j > 0; j /= 2) {\n            s += 1;\n        }\n    }\n"
                                "    return s;\n}\n")
    assert notes == [(3, 5, "normalized: for->while"), (18, 9, "normalized: for->while")]


@needs_clang
def test_r1_continue_bound_to_the_loop_is_refused(tmp_path):
    _, es, notes = imp(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = 0; i < n; i += 2) {\n"
                                 "        if (i == 4) {\n            continue;\n        }\n        s += i;\n    }\n"
                                 "    return s;\n}\n")
    assert notes == []
    assert es == [(3, 5, "unsupported: " + NZ.FOR_CONTINUE)]


@needs_clang
def test_r1_condition_variable_is_refused(tmp_path):
    _, es, _ = imp(tmp_path, "int f(int n) {\n    int s = 0;\n    for (; int r = n - s;) {\n"
                             "        s += r;\n    }\n    return s;\n}\n")
    assert es == [(3, 5, "unsupported: " + NZ.FOR_CVAR)]


@needs_clang
def test_r1_unrepresentable_step_keeps_its_own_located_refusal(tmp_path):
    _, es, notes = imp(tmp_path, "int f(int n) {\n    int s = 0;\n    for (int i = 0; i < n; i += 1, s += 2) {\n"
                                 "        s += i;\n    }\n    return s;\n}\n")
    assert notes == [(3, 5, "normalized: for->while")]
    assert es == [(3, 28, "unsupported: comma operator")]


# ------------------------------------------------------------- R2
@needs_clang
def test_r2_shape(tmp_path):
    prog, notes = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    do {\n        s += n;\n        n -= 3;\n"
                                "    } while (n > 0);\n    return s;\n}\n")
    w = body_of(prog).stmts[1]
    assert isinstance(w, K.While) and w.cond == K.BoolLit(True)
    last = w.body.stmts[-1]
    assert isinstance(last, K.If) and last.then.stmts == (K.Break(),) and last.els is None
    assert isinstance(last.cond, K.Unary) and last.cond.op == "not"


@needs_clang
def test_r2_int_condition_is_truthy(tmp_path):
    prog, _ = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    do {\n        s += 1;\n        n /= 2;\n"
                           "    } while (n);\n    return s;\n}\n")
    assert "if (!(n != 0)) {" in cxx_lines(prog)


@needs_clang
def test_r2_condition_capture_keeps_the_body_nested(tmp_path):
    prog, _ = ok(tmp_path, "int f(int n) {\n    int s = 0;\n    int k = 3;\n    do {\n        int k = 1;\n"
                           "        s += k;\n    } while (s < n + k);\n    return s;\n}\n")
    w = body_of(prog).stmts[2]
    assert [type(s) for s in w.body.stmts] == [K.Block, K.If]


@needs_clang
def test_r2_continue_bound_to_the_loop_is_refused(tmp_path):
    _, es, notes = imp(tmp_path, "int f(int n) {\n    int s = 0;\n    do {\n        n -= 1;\n"
                                 "        if (n == 2) {\n            continue;\n        }\n        s += n;\n"
                                 "    } while (n > 0);\n    return s;\n}\n")
    assert notes == []
    assert es == [(3, 5, "unsupported: " + NZ.DO_CONTINUE)]


def test_bound_continue_sees_through_switch_not_nested_loops_or_lambdas():
    cont = {"kind": "ContinueStmt"}
    assert NZ.bound_continue({"kind": "CompoundStmt", "inner": [{"kind": "SwitchStmt", "inner": [cont]}]}) is cont
    for k in ("ForStmt", "WhileStmt", "DoStmt", "CXXForRangeStmt", "LambdaExpr"):
        assert NZ.bound_continue({"kind": "CompoundStmt", "inner": [{"kind": k, "inner": [cont]}]}) is None


# ------------------------------------------------------------- CLI
@needs_clang
def test_cli_normalize_notes_and_rust_input(tmp_path, capsys):
    p = tmp_path / "t.cpp"
    p.write_text(HEAD + SHAPES + "int main() {\n    std::printf(\"%d\\n\", f(37));\n    return 0;\n}\n")
    assert cli.main(["--normalize", "--emit=cpp", str(p)]) == 0
    out, err = capsys.readouterr()
    assert err.splitlines()[0] == "%s:4:5: note: normalized: for->while" % p
    assert len(err.splitlines()) == 6
    q = tmp_path / "n.cpp"
    q.write_text(out)
    assert cli.main(["--emit=cpp", str(q)]) == 0  # no --normalize: already in the subset
    assert capsys.readouterr()[0] == out  # cpp -> cpp fixed point
    r = tmp_path / "t.rs"
    r.write_text("fn main() {\n}\n")
    assert cli.main(["--normalize", str(r)]) == 1
    assert capsys.readouterr()[1] == "error: --normalize applies to C++ input only\n"


# ------------------------------------------------------------- double validation
def _loop(rng, k, depth=0):
    """(C++ statements updating `acc` with one non-canonical loop) -- UB-free:
    acc stays in (-10007, 10007), counters are bounded by n <= 40."""
    M = rng.randint(2, 9)
    S = rng.randint(2, 5)
    A = rng.randint(-5, 5)

    def upd(x):
        return "acc = (acc * %d + %s) %% 10007;" % (M, x)
    v = "v%d" % k
    shape = rng.randrange(17 if depth == 0 else 11)
    if shape == 0:
        return ["for (int %s = %d; %s < n + %d; %s += %d) {" % (v, A, v, rng.randint(0, 5), v, S), "  " + upd(v), "}"]
    if shape == 1:
        return ["for (int %s = n; %s != 0; --%s) {" % (v, v, v), "  " + upd(v), "}"]
    if shape == 2:
        return ["for (int %s = n + %d; %s > %d; %s--) {" % (v, A, v, A - 3, v), "  " + upd(v), "}"]
    if shape == 3:
        return ["for (int %s = %d; %s < n;) {" % (v, A, v), "  " + upd(v), "  %s += %d;" % (v, S), "}"]
    if shape == 4:
        return ["for (int %s = 1;; %s *= 2) {" % (v, v), "  if (%s > n * %d + 1) {" % (v, M), "    break;", "  }",
                "  acc = (acc + %s) %% 10007;" % v, "}"]
    if shape == 5:
        return ["unsigned %s = %dU;" % (v, rng.randint(1000, 4000000)),
                "for (; %s >= %dU; %s >>= %d) {" % (v, rng.randint(2, 200), v, S),
                "  acc = (acc + static_cast<int>(%s %% 97U) + n) %% 10007;" % v, "}"]
    if shape == 6:
        return ["for (unsigned %s = %dU; %s > 0U; %s /= %dU) {" % (v, rng.randint(1, 90000), v, v, S),
                "  acc = (acc * %d + static_cast<int>(%s %% 13U)) %% 10007;" % (M, v), "}"]
    if shape == 7:
        return ["int %s = n * %d + %d;" % (v, M, rng.randint(0, 99)), "do {",
                "  acc = (acc * 10 + %s %% 10) %% 10007;" % v, "  %s /= 10;" % v, "} while (%s != 0);" % v]
    if shape == 8:
        return ["for (int %s = 0; %s < n; %s += %d) {" % (v, v, v, S), "  if (acc %% 7 == %d) {" % rng.randint(0, 6),
                "    break;", "  }", "  " + upd(v), "}"]
    if shape == 9:
        return ["int %s = %d;" % (v, A), "do {", "  if (%s > n) {" % v, "    break;", "  }",
                "  acc = (acc + %s) %% 10007;" % v, "  %s += %d;" % (v, S), "} while (%s < n * 2 + 5);" % v]
    if shape == 10:
        return ["for (int %s = 0, %s_e = n; %s < %s_e; ++%s) {" % (v, v, v, v, v), "  " + upd(v + "_e"),
                "  %s_e -= 1;" % v, "}"]
    if shape == 11:  # nested non-canonical loops
        inner = _loop(rng, k * 10 + 1, depth + 1)
        return ["for (int %s = n; %s > 0; %s -= %d) {" % (v, v, v, S)] + ["  " + x for x in inner] + \
            ["  acc = (acc + %s) %% 10007;" % v, "}"]
    if shape == 12:  # continue bound to a NESTED canonical for
        return ["for (int %s = n; %s > 0; %s -= %d) {" % (v, v, v, S), "  for (int j = 0; j < 4; ++j) {",
                "    if (j == %d) {" % rng.randint(0, 3), "      continue;", "    }", "    " + upd("j"), "  }", "}"]
    if shape == 13:  # step capture: `+= t` is the OUTER t
        return ["int %s_t = %d;" % (v, S), "for (int %s = 0; %s < n; %s += %s_t) {" % (v, v, v, v),
                "  int %s_t = %s %% 3 + 4;" % (v, v), "  " + upd(v + "_t"), "}"]
    if shape == 14:  # condition capture: `< n + k` is the OUTER k
        return ["int %s_k = %d;" % (v, S), "int %s = 0;" % v, "do {", "  int %s_k = %s %% 4;" % (v, v),
                "  " + upd(v + "_k"), "  %s += 1;" % v, "} while (%s < n + %s_k);" % (v, v)]
    if shape == 15:  # expression init, the counter read after the loop
        return ["int %s = 0;" % v, "for (%s = n; %s > 0; %s -= %d) {" % (v, v, v, S), "  " + upd(v), "}",
                "acc = (acc + %s) %% 10007;" % v]
    # shape 16: return from inside an infinite loop
    return ["for (int %s = %d;; %s += %d) {" % (v, A, v, S), "  if (%s > n) {" % v,
            "    return (acc + %s) %% 10007;" % v, "  }", "  " + upd(v), "}"]


def generate(seed, count):
    rng = random.Random(seed)
    out = [HEAD]
    for k in range(count):
        body = _loop(rng, k)
        out.append("int g%d(int n) {\n    int acc = %d;\n%s\n    return acc;\n}\n\n" % (
            k, rng.randint(0, 50), "\n".join("    " + ln for ln in body)))
    out.append("int main() {\n")
    for k in range(count):
        for n in (0, 1, 5, 17, 40):
            out.append("    std::printf(\"g%d(%d) = %%d\\n\", g%d(%d));\n" % (k, n, k, n))
    out.append("    return 0;\n}\n")
    return "".join(out)


def _run_cxx(tmp_path, name, text):
    src = tmp_path / (name + ".cpp")
    src.write_text(text)
    exe = tmp_path / name
    subprocess.run(["clang++", "-std=c++17", "-O1", str(src), "-o", str(exe)], check=True, capture_output=True)
    return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


@needs_clang
@needs_rustc
def test_generated_loops_byte_identical_before_and_after(tmp_path):
    count = 40
    text = generate(254, count)
    orig = tmp_path / "orig.cpp"
    orig.write_text(text)
    _, es0 = cli.load_cpp(str(orig), "clang++")
    assert es0 and all("for` loop outside" in m or "do-while" in m for _, m in es0)  # outside the subset
    notes = []
    prog, es = cli.load_cpp(str(orig), "clang++", True, notes)
    assert es == [], es
    assert len(notes) >= count
    assert cli.check_roundtrip(prog, "clang++") == []
    want = _run_cxx(tmp_path, "orig", text)
    assert want.count("\n") == count * 5
    assert _run_cxx(tmp_path, "norm", CP.cxx_print(prog)) == want
    rs = tmp_path / "norm.rs"
    rs.write_text(RSP.rs_print(prog))
    assert "unsafe" not in rs.read_text()
    exe = tmp_path / "norm_rs"
    subprocess.run(["rustc", "-O", "--edition", "2021", "-A", "warnings", str(rs), "-o", str(exe)], check=True,
                   capture_output=True)
    assert subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout == want


def test_generator_covers_every_shape():
    text = generate(254, 40)
    for needle in ("--", ";; ", ">>=", "/= ", "do {", "break;", "continue;", "_t = ", "_k = ", "return (acc",
                   "_e = n", "unsigned "):
        assert needle in text, needle
