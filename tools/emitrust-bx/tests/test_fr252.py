# Pins FR-252 (C++ lambdas <=> Rust closures) at the unit level.  The
# invariants, and why each is load-bearing:
#  * capture MODE is the closure's one bit: all by copy `[a, b]` <=> `move
#    |..|`, all by reference `[&a, &b]` <=> a non-move closure.  Rust spells
#    no capture list, so the Kernel stores none: the list is DERIVED from the
#    body (first use order, N23) and a closure that captures nothing is never
#    `move` (N24).  Mixed modes have no Rust spelling and are refused, NAMING
#    the rewrite (FR-254's normalizer); so are default captures, whose
#    message lists the explicit captures read off the closure record;
#  * whether a closure MODIFIES state is derived too: C++ `mutable` exactly
#    for a by-copy closure that modifies its copy, Rust `let mut f` exactly
#    when it modifies state or is passed as `&mut impl FnMut` -- one spelling
#    per meaning, so laws 1-2 hold;
#  * the alias discipline: a by-reference closure borrows its captures until
#    its LAST call.  A capture it modifies is not mentioned in between, one it
#    reads is not modified or moved -- an ASSIGNMENT TARGET counts (it is not a
#    Var node: FR-261's verification lesson).  A call of a closure counts, for
#    every analysis, as the modifications its body makes through by-reference
#    captures: without that a counting loop whose body calls a closure that
#    shrinks its bound would be a SILENT miscompile (C++ re-reads `i < n`,
#    Rust's range does not);
#  * a closure that modifies state is not called twice in one statement
#    without a sequence point (C++ leaves `cnt() * 10 + cnt()` unspecified;
#    `&&`, `||` and `?:` do sequence);
#  * callable parameters: `const F&` <=> `&impl Fn`, `F&` <=> `&mut impl
#    FnMut` -- BY REFERENCE, so a `mutable` lambda's state advanced inside the
#    callee is the caller's afterwards in both languages; a by-value `F f`
#    (a copy in C++) is refused, naming the rewrite;
#  * the generated closure population obeys laws 1-2 on both sides and rustc's
#    borrow checker accepts every printed program.
import collections
import os
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import gen
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
HEAD = "#include <cstdio>\n#include <string>\n#include <utility>\n#include <vector>\n"
V = K.Var
I = lambda v, t=K.I32: K.IntLit(v, t)  # noqa: E731


def imp(tmp_path, src, name="t.cpp"):
    p = tmp_path / name
    p.write_text(HEAD + src)
    return cli.load_cpp(str(p), "clang++")


def ok(tmp_path, src):
    prog, es = imp(tmp_path, src)
    assert es == [], es
    return prog


def errs(tmp_path, src):
    """(line relative to ``src``, col, message) of every located error."""
    _, es = imp(tmp_path, src)
    n = HEAD.count("\n")
    return [(loc.line - n, loc.col, m) for loc, m in es]


def body(prog, name):
    return list([f for f in prog.all_funcs() if f.name == name][0].body.stmts)


def closures(prog):
    return [n for n in K.walk(prog) if isinstance(n, K.Closure)]


# ------------------------------------------------------------------ import

@needs_clang
def test_import_rows(tmp_path):
    p = ok(tmp_path, "int f(int k) {\n"
                     "    int acc = 0;\n"
                     "    auto add = [k](int a) -> int { return a + k; };\n"
                     "    auto bump = [&acc](const int a) { acc = acc + a; };\n"
                     "    auto cnt = [k]() mutable -> int { k = k + 1; return k; };\n"
                     "    bump(add(1));\n"
                     "    return cnt() + acc;\n}\n")
    add, bump, cnt = closures(p)
    assert (add.name, add.move, add.ret, K.closure_captures(add)) == ("add", True, K.I32, ("k",))
    assert add.params == (K.Param("a", K.I32, True),)
    assert (bump.move, bump.ret, K.closure_captures(bump)) == (False, K.UNIT, ("acc",))
    assert bump.params == (K.Param("a", K.I32, False),)
    assert bump.body.stmts == (K.CompoundAssign("add", "acc", V("a")),)  # N5 inside a closure body
    assert (cnt.move, cnt.params, K.closure_captures(cnt)) == (True, (), ("k",))
    st = body(p, "f")
    assert st[4] == K.ExprStmt(K.ClosureCall("bump", (K.ClosureCall("add", (I(1),)),)))
    assert st[5] == K.Return(K.Binary("add", K.ClosureCall("cnt", ()), V("acc")))


@needs_clang
def test_capture_list_is_derived(tmp_path):
    # N23: written order and unused captures are spellings; N24: nothing
    # captured -> never `move`; `const auto` and `-> void` are spellings too
    p = ok(tmp_path, "int f() {\n"
                     "    int k = 2;\n    int n = 5;\n"
                     "    auto a = [n, k](int x) -> int { return x * k + n; };\n"
                     "    auto b = [k, n](int x) -> int { return x * k + n; };\n"
                     "    auto c = [n](int x) mutable { return x; };\n"
                     "    const auto d = [&k](int x) -> void { };\n"
                     "    d(1);\n"
                     "    return a(1) + b(2) + c(3);\n}\n")
    a, b, c, d = closures(p)
    assert K.closure_captures(a) == K.closure_captures(b) == ("k", "n")
    assert (a.move, a.params, a.ret, a.body) == (b.move, b.params, b.ret, b.body)
    assert (c.move, K.closure_captures(c)) == (False, ())
    assert (d.move, d.ret, K.closure_captures(d)) == (False, K.UNIT, ())
    cpp = CP.cxx_print(p)
    assert "auto a = [k, n](int x) -> int {" in cpp
    assert "auto c = [](int x) -> int {" in cpp
    assert "auto d = [](int x) {" in cpp


@needs_clang
def test_mutability_spellings_are_derived(tmp_path):
    p = ok(tmp_path, "template <typename F>\nvoid run(F& f) {\n    f();\n}\n"
                     "int h(int k) {\n"
                     "    int acc = 0;\n"
                     "    auto read = [k]() -> int { return k; };\n"
                     "    auto own = [k]() mutable -> int { k += 1; return k; };\n"
                     "    auto shared = [&acc]() { acc += 1; };\n"
                     "    auto plain = []() { };\n"
                     "    run(plain);\n"
                     "    shared();\n"
                     "    return read() + own() + acc;\n}\n")
    rs = RSP.rs_print(p)
    assert "    let read = move || -> i32 {" in rs
    assert "    let mut own = move || -> i32 {" in rs
    assert "    let mut shared = || {" in rs
    assert "    let mut plain = || {" in rs  # passed as `&mut impl FnMut`
    assert "run(&mut plain);" in rs
    cpp = CP.cxx_print(p)
    assert "auto read = [k]() -> int {" in cpp
    assert "auto own = [k]() mutable -> int {" in cpp
    assert "auto shared = [&acc]() {" in cpp  # a by-reference modification needs no `mutable`
    assert RP.rs_parse(rs) == p


def test_rust_let_mut_must_match():
    src = ("fn main() {\n    let mut f = || -> i32 {\n        return 1;\n    };\n"
           "    let mut k: i32 = 0;\n    let g = || {\n        k += 1;\n    };\n    g();\n"
           "    print!(\"{}\\n\", f());\n}\n")
    _, es = RP.rs_parse_all(src)
    msgs = [(str(loc), m) for loc, m in es]
    assert ("2:5", "unsupported: closure 'f' is not spelled the canonical way (`let f = ..`: `let mut` exactly "
                   "when it modifies state or is passed as `&mut impl FnMut`)") in msgs
    assert ("6:5", "unsupported: closure 'g' is not spelled the canonical way (`let mut g = ..`: `let mut` "
                   "exactly when it modifies state or is passed as `&mut impl FnMut`)") in msgs


def test_rust_closure_call_resolution():
    # a bare call of a closure in scope is the closure, inside nested blocks
    # too; after the block that declares it, the name is a function again
    src = ("fn g(x: i32) -> i32 {\n    return x;\n}\n\n"
           "fn main() {\n    let mut r: i32 = 0;\n    {\n        let h = |x: i32| -> i32 {\n"
           "            return x + 1;\n        };\n        if r == 0 {\n            r = h(2);\n        }\n    }\n"
           "    r = g(r);\n    print!(\"{}\\n\", r);\n}\n")
    p = RP.rs_parse(src)
    calls = [n for n in K.walk(p) if isinstance(n, (K.Call, K.ClosureCall))]
    assert [type(c).__name__ for c in calls] == ["ClosureCall", "Call"]
    assert RSP.rs_print(p) == src


# ------------------------------------------------------------------ refusals

@needs_clang
def test_default_captures_name_the_explicit_list(tmp_path):
    assert errs(tmp_path, "int f(int k, int& acc) {\n"
                          "    auto a = [=](int x) -> int { return x * k; };\n"
                          "    auto b = [&](int x) { acc = acc + x + k; };\n"
                          "    auto c = [k, &acc](int x) { acc = acc + k + x; };\n"
                          "    return 0;\n}\n") == [
        (2, 14, "unsupported: default capture `[=]` (normalizer: list the captures explicitly: `[k]`)"),
        (3, 14, "unsupported: default capture `[&]` (normalizer: list the captures explicitly: `[&acc, &k]`)"),
        (4, 14, "unsupported: mixed by-copy and by-reference captures `[k, &acc]` (normalizer: copy each by-copy "
                "capture into a local before the lambda and capture everything by reference)"),
    ]


@needs_clang
def test_assignment_target_is_a_mention(tmp_path):
    # FR-261's verification bug, closure edition: `k = 5;` names k as an
    # Assign target, not a Var -- it must still end a live borrow (E0506)
    assert errs(tmp_path, "int f(int k) {\n"
                          "    auto read = [&k]() -> int { return k; };\n"
                          "    k = 5;\n"
                          "    return read();\n}\n") == [
        (3, 5, "unsupported: 'k' is modified or moved while closure 'read', which captures it by reference, is "
               "still called later (a borrow conflict in Rust)"),
    ]
    # after the LAST call the borrow is over (NLL): accepted
    ok(tmp_path, "int f(int k) {\n    auto read = [&k]() -> int { return k; };\n    int r = read();\n"
                 "    k = 5;\n    return r + k;\n}\n")


@needs_clang
def test_closure_calls_count_as_modifications(tmp_path):
    # without closure effects this counting loop would be a SILENT
    # miscompile: C++ re-reads `i < n` after shrink(), Rust's `0..n` does not
    assert ("unsupported: counting loop body assigns or shadows the bound 'n'"
            in [m for _, _, m in errs(tmp_path, "int f(int n) {\n"
                                               "    auto shrink = [&n]() { n = n - 1; };\n"
                                               "    int t = 0;\n"
                                               "    for (int i = 0; i < n; ++i) {\n"
                                               "        shrink();\n        t += i;\n    }\n"
                                               "    return t;\n}\n")])
    blk = K.Block((K.ExprStmt(K.ClosureCall("shrink", ())),))
    sigs = TC.Sigs()
    sigs.closure_muts = {"shrink": {"n"}}
    assert "n" in TC.assigned_names(blk, sigs)
    assert "n" in TC.mutated_bases(blk, sigs)
    assert "n" not in TC.mutated_bases(blk, sigs, skip="shrink")


def test_unsequenced_uses():
    c = K.ClosureCall("cnt", ())
    assert TC.unsequenced_uses([K.Binary("add", K.Binary("mul", c, I(10)), c)], "cnt") is not None
    assert TC.unsequenced_uses([K.Binary("and", K.Binary("gt", c, I(3)), K.Binary("gt", c, I(4)))], "cnt") is None
    assert TC.unsequenced_uses([K.Cond(K.Binary("gt", c, I(3)), c, c)], "cnt") is None
    assert TC.unsequenced_uses([K.ClosureCall("cnt", (c,))], "cnt") is not None  # f(f(1)): E0499 besides
    assert TC.unsequenced_uses([c, c], "cnt") is not None  # two printf arguments
    assert TC.unsequenced_uses([K.Binary("add", c, K.ClosureCall("other", ()))], "cnt") is None


@needs_clang
def test_copy_capture_of_owned_values_is_a_move(tmp_path):
    assert errs(tmp_path, "int f() {\n"
                          "    std::vector<int> v = {1};\n"
                          "    auto peek = [v]() -> int { return v[0]; };\n"
                          "    return peek() + static_cast<int>(v.size());\n}\n") == [
        (3, 5, "unsupported: 'v' is used after closure 'peek' captures it by copy (C++ copies it, Rust moves it "
               "into the closure: the subset requires no later mention of 'v')"),
    ]
    p = ok(tmp_path, "int f() {\n    std::string s = \"abc\";\n"
                     "    auto n = [s]() -> int { return static_cast<int>(s.size()); };\n    return n();\n}\n")
    assert "let n = move || -> i32 {" in RSP.rs_print(p)


@needs_clang
def test_by_value_template_is_refused_naming_the_rewrite(tmp_path):
    es = errs(tmp_path, "template <typename F>\nint g(F f, int x) {\n    return f(x);\n}\n"
                        "int h(int k) {\n    auto add = [k](int a) -> int { return a + k; };\n"
                        "    return g(add, 1);\n}\n")
    assert es == [(1, 1, "unsupported: callable parameter taken by value `F f` (normalizer: take it as `const F& "
                         "f`, or `F& f` for a closure that modifies state -- C++ copies a by-value closure and its "
                         "state, Rust would borrow it)")]


@needs_clang
def test_template_signature_comes_from_the_instantiations(tmp_path):
    p = ok(tmp_path, "template <typename Op>\nlong long g(const Op& op, int x) {\n    return op(x) + 1LL;\n}\n"
                     "long long h(long long k) {\n"
                     "    auto a = [k](int y) -> long long { return y + k; };\n"
                     "    auto b = [](int y) -> long long { return y * 2LL; };\n"
                     "    return g(a, 1) + g(b, 2);\n}\n")
    g = [f for f in p.funcs() if f.name == "g"][0]
    assert g.params[0] == K.Param("op", K.RefT(K.FnT((K.I32,), K.I64), False), False)
    assert "fn g(op: &impl Fn(i32) -> i64, mut x: i32) -> i64 {" in RSP.rs_print(p)
    assert "template <typename F> long long g(const F& op, int x) {" in CP.cxx_print(p)


@needs_clang
@needs_rustc
def test_names_like_rust_items(tmp_path):
    # prelude names a local may shadow are accepted (and rustc agrees);
    # keywords and the canonical spelling's names are refused
    p = ok(tmp_path, "int f(int k) {\n"
                     "    auto drop = [k](int a) -> int { return a + k; };\n"
                     "    auto Box = [](int a) -> int { return a * 3; };\n"
                     "    auto Option = [&k](int a) { k = k + a; };\n"
                     "    Option(4);\n"
                     "    return drop(1) + Box(2) + k;\n}\n"
                     "int main() {\n    std::printf(\"%d\\n\", f(2));\n    return 0;\n}\n")
    rs = tmp_path / "n.rs"
    rs.write_text(RSP.rs_print(p))
    r = subprocess.run(["rustc", "--edition", "2021", "-A", "warnings", "-o", str(tmp_path / "n"), str(rs)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = subprocess.run([str(tmp_path / "n")], capture_output=True, text=True).stdout
    native = tmp_path / "t.native"
    subprocess.run(["clang++", "-std=c++17", str(tmp_path / "t.cpp"), "-o", str(native)], check=True)
    assert out == subprocess.run([str(native)], capture_output=True, text=True).stdout == "15\n"
    assert errs(tmp_path, "int f() {\n    auto loop = [](int a) -> int { return a; };\n"
                          "    auto print = [](int a) -> int { return a; };\n    return loop(1) + print(2);\n}\n") == [
        (2, 5, "unsupported: identifier 'loop' is a Rust keyword"),
        (3, 5, "unsupported: identifier 'print' is reserved by the canonical spelling"),
    ]


# ------------------------------------------------------------------ properties

FR252_SEEDS = range(6000, 6008)


@pytest.fixture(scope="module")
def closure_programs():
    return [gen.gen_program(seed, 50, closures=True) for seed in FR252_SEEDS]


def test_generator_covers_fr252(closure_programs):
    seen = collections.Counter()
    for p in closure_programs:
        sigs = TC.sigs_of(p)
        for f in p.all_funcs():
            fnmut = TC.closure_info(f.body, sigs)[1]
            for n in K.walk(f.body):
                if isinstance(n, K.Closure):
                    caps = K.closure_captures(n)
                    kind = "move" if n.move else ("ref" if caps else "none")
                    seen["closure-" + kind] += 1
                    if n.name in fnmut:
                        seen["fnmut-" + kind] += 1
                    seen["params-%d" % min(len(n.params), 2)] += 1
                    seen["ret-" + ("unit" if n.ret == K.UNIT else "value")] += 1
                    if len(caps) == 2:
                        seen["caps-2"] += 1
                    if any(isinstance(x, (K.If, K.While, K.For, K.ForEach)) for x in K.walk(n.body)):
                        seen["body-control"] += 1
                    if any(isinstance(x, K.Var) and x.name in caps for x in K.walk(n.body)):
                        seen["reads-capture"] += 1
                if isinstance(n, K.ExprStmt) and isinstance(n.expr, K.ClosureCall):
                    seen["call-stmt"] += 1
                if isinstance(n, K.Let) and isinstance(n.init, K.ClosureCall):
                    seen["call-let"] += 1
                if isinstance(n, (K.While, K.For, K.ForEach)) and any(
                        isinstance(x, K.Closure) for x in K.walk(n.body)):
                    seen["closure-in-loop"] += 1
    # measured 2026-11-01 on seeds 6000-6007: move 270, ref 283, none 409,
    # fnmut-move 81, fnmut-ref 88, caps-2 213, body-control 219,
    # closure-in-loop 155, call-stmt 808, call-let 1098, params 303/322/337,
    # ret unit 182 / value 780
    for k, lo in (("closure-move", 120), ("closure-ref", 120), ("closure-none", 150), ("fnmut-move", 30),
                  ("fnmut-ref", 30), ("caps-2", 80), ("body-control", 80), ("closure-in-loop", 50),
                  ("call-stmt", 300), ("call-let", 400), ("params-0", 100), ("params-1", 100),
                  ("params-2", 100), ("ret-unit", 60), ("ret-value", 300), ("reads-capture", 100)):
        assert seen[k] >= lo, (k, seen[k], sorted(seen.items()))


def test_law1_and_law2_rust_fr252(closure_programs):
    for p in closure_programs:
        text = RSP.rs_print(p)
        back = RP.rs_parse(text)
        assert back == p
        assert RSP.rs_print(back) == text


@needs_clang
def test_law1_and_law2_cxx_fr252(closure_programs, tmp_path):
    for i, p in enumerate(closure_programs):
        text = CP.cxx_print(p)
        path = os.path.join(str(tmp_path), "ctu%d.cpp" % i)
        with open(path, "w") as f:
            f.write(text)
        back, es = cli.load_cpp(path, "clang++")
        assert es == [], (path, es[:5])
        assert back == p
        assert CP.cxx_print(back) == text


@needs_rustc
def test_printed_fr252_rust_borrow_checks(closure_programs, tmp_path):
    header = ("#![allow(arithmetic_overflow, unconditional_panic, unreachable_code, unused, "
              "overflowing_literals, unconditional_recursion, while_true)]\n")
    for i, p in enumerate(closure_programs[:5]):
        path = os.path.join(str(tmp_path), "cg%d.rs" % i)
        with open(path, "w") as f:
            f.write(header + RSP.rs_print(p))
        r = subprocess.run(["rustc", "--edition", "2021", "--emit=metadata", "--crate-type=lib",
                            "-o", path + ".rmeta", path], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[:4000]


@needs_clang
def test_move_after_last_call_is_accepted(tmp_path):
    # a by-reference closure's borrow ends at its LAST call (NLL), so moving
    # the captured vector afterwards is fine -- before it, refused (E0505)
    p = ok(tmp_path, "std::size_t f(std::vector<int>& out) {\n"
                     "    std::vector<int> w = {4};\n"
                     "    auto len = [&w]() -> std::size_t { return w.size(); };\n"
                     "    std::size_t n = len();\n"
                     "    out = std::move(w);\n"
                     "    return n;\n}\n")
    assert "out = std::move(w);" in CP.cxx_print(p)
    assert errs(tmp_path, "std::size_t f(std::vector<int>& out) {\n"
                          "    std::vector<int> w = {4};\n"
                          "    auto len = [&w]() -> std::size_t { return w.size(); };\n"
                          "    out = std::move(w);\n"
                          "    return len();\n}\n") == [
        (4, 5, "unsupported: 'w' is modified or moved while closure 'len', which captures it by reference, is "
               "still called later (a borrow conflict in Rust)"),
    ]


def test_rust_generic_function_must_be_instantiated():
    # rustc compiles an uncalled `fn g(f: &impl Fn(i32) -> i32)`; its C++
    # template would never be instantiated, and only instantiations type it
    # (law 1 would fail on the way back) -- so it is refused, unless a call
    # with a closure reaches it (here through `outer` passing its own on)
    base = ("fn inner(f: &impl Fn(i32) -> i32) -> i32 {\n    return f(1);\n}\n\n"
            "fn outer(f: &impl Fn(i32) -> i32) -> i32 {\n    return inner(f);\n}\n\n")
    _, es = RP.rs_parse_all(base + "fn main() {\n}\n")
    assert [(str(loc), m) for loc, m in es] == [
        ("1:1", "unsupported: function 'inner' with a callable parameter is never called with a closure (its C++ "
                "template would never be instantiated, and is typed only by its instantiations)"),
        ("5:1", "unsupported: function 'outer' with a callable parameter is never called with a closure (its C++ "
                "template would never be instantiated, and is typed only by its instantiations)"),
    ]
    src = base + ("fn main() {\n    let k: i32 = 2;\n    let add = move |x: i32| -> i32 {\n        return x + k;\n"
                  "    };\n    print!(\"{}\\n\", outer(&add));\n}\n")
    p = RP.rs_parse(src)
    assert RSP.rs_print(p) == src
