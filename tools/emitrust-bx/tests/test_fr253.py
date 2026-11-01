# Pins FR-253 (<algorithm> / <numeric> over WHOLE vectors) at the unit level.
# The invariants, and why each is load-bearing:
#  * every row is a bijection: ONE canonical C++ spelling <=> ONE canonical
#    Rust spelling, laws 1-2 on both sides (``cli.check_roundtrip``); the two
#    C++ spellings with no Rust twin are normal forms, not second terms:
#    `std::none_of` is `!any_of` (N25) and `std::find(..) == v.end()` is
#    `!contains` (N26);
#  * the rows are EXACT, or refused located: std::accumulate runs in the INIT's
#    type (`0` over long longs truncates every step -- refused, `0LL` over ints
#    widens with `e as i64`); std::count is ptrdiff_t (i64), not usize; sort,
#    max_element and std::max refuse f64 (-0.0 == 0.0 print differently, NaN);
#    std::sort with a comparator is libstdc++'s introsort (bx::sort_by_less),
#    because sort_unstable_by orders distinguishable equivalents differently;
#  * a callable argument is a LOCAL closure (an inline lambda, a function or a
#    callable parameter is refused, naming the rewrite) and the algorithm call
#    IS a call of it for every FR-252 / FR-263 analysis: a closure that
#    modifies a counting loop's bound through a by-reference capture, called
#    via std::count_if, is a modification of the bound (else C++'s re-read
#    `i < n` and Rust's evaluate-once range silently differ); two algorithm
#    calls whose closures print are unsequenced; a closure that modifies the
#    container being iterated is refused; a `mutable` lambda (C++ copies it
#    into the algorithm) is refused;
#  * the comparator-sort test input really reaches introsort's depth-limit
#    (heapsort) branch -- checked here with an instrumented copy of the helper,
#    so the EndToEnd byte-diff that relies on it cannot silently stop covering
#    it;
#  * a user function named like an algorithm is never mistaken for it.
import os
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import fmt
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
HEAD = ("#include <algorithm>\n#include <cstdio>\n#include <iterator>\n#include <numeric>\n#include <string>\n"
        "#include <vector>\n")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


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


def rs_errs(src):
    _, es = RP.rs_parse_all(src)
    return [("%d:%d" % (loc.line, loc.col), m) for loc, m in es]


def algs(prog):
    return [n for n in K.walk(prog) if isinstance(n, (K.Alg, K.AlgStmt, K.MinMax))]


# ------------------------------------------------------------------ rows
# (C++ body of `void f(std::vector<int>& v, std::vector<long long>& w,
#  std::vector<std::string>& s, int x)`, canonical C++ line, canonical Rust line)
ROWS = [
    ("std::sort(v.begin(), v.end());", "std::sort(v.begin(), v.end());", "v.sort_unstable();"),
    ("std::stable_sort(s.begin(), s.end());", "std::stable_sort(s.begin(), s.end());", "s.sort();"),
    ("std::reverse(s.begin(), s.end());", "std::reverse(s.begin(), s.end());", "s.reverse();"),
    ("std::fill(w.begin(), w.end(), x);", "std::fill(w.begin(), w.end(), static_cast<long long>(x));",
     "w.fill(x as i64);"),
    ("std::fill(w.begin(), w.end(), 3);", "std::fill(w.begin(), w.end(), 3LL);", "w.fill(3i64);"),
    ("std::fill(s.begin(), s.end(), s[0]);", None, None),  # (refused: reads the vector it fills)
    ("bool b = std::find(v.begin(), v.end(), x) != v.end();",
     "bool b = std::find(v.begin(), v.end(), x) != v.end();", "let mut b: bool = v.contains(&x);"),
    ("bool b = std::find(w.begin(), w.end(), 7) == w.end();",
     "bool b = std::find(w.begin(), w.end(), 7LL) == w.end();", "let mut b: bool = !w.contains(&7i64);"),
    ("long long c = std::count(s.begin(), s.end(), s[1]);",
     "long long c = std::count(s.begin(), s.end(), s[static_cast<std::size_t>(1)]);",
     "let mut c: i64 = bx::count(s, &s[1usize]);"),
    ("int t = std::accumulate(v.begin(), v.end(), x);", "int t = std::accumulate(v.begin(), v.end(), x);",
     "let mut t: i32 = v.iter().fold(x, |a, &e| a + e);"),
    ("long long t = std::accumulate(v.begin(), v.end(), 0LL);",
     "long long t = std::accumulate(v.begin(), v.end(), 0LL);",
     "let mut t: i64 = v.iter().fold(0i64, |a, &e| a + e as i64);"),
    ("double t = std::accumulate(w.begin(), w.end(), 0.5);",
     "double t = std::accumulate(w.begin(), w.end(), 0.5);",
     "let mut t: f64 = w.iter().fold(0.5, |a, &e| a + e as f64);"),
    ("int m = *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());",
     "int m = *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());",
     "let mut m: i32 = *v.iter().max().unwrap() - *v.iter().min().unwrap();"),
    ("long long m = std::max(w[0], 4LL) + std::min(w[1], w[0]);",
     "long long m = std::max(w[static_cast<std::size_t>(0)], 4LL) + std::min(w[static_cast<std::size_t>(1)], "
     "w[static_cast<std::size_t>(0)]);",
     "let mut m: i64 = std::cmp::max(w[0usize], 4i64) + std::cmp::min(w[1usize], w[0usize]);"),
]


@needs_clang
@pytest.mark.parametrize("body,cxx,rust", [r for r in ROWS if r[1] is not None])
def test_no_callable_rows_are_bijections(tmp_path, body, cxx, rust):
    src = ("void f(std::vector<int>& v, std::vector<long long>& w, std::vector<std::string>& s, int x) {\n    %s\n}\n"
           % body)
    prog = ok(tmp_path, src)
    assert algs(prog), body
    assert ("    " + cxx) in CP.cxx_print(prog).splitlines()
    assert ("    " + rust) in RSP.rs_print(prog).splitlines()
    assert cli.check_roundtrip(prog, "clang++") == []


CALLABLE = """
void f(std::vector<int>& v, std::vector<long long>& w) {
    int k = 3;
    auto p = [k](int a) -> bool {
        return a > k;
    };
    int n = 0;
    auto bump = [&n](int a) {
        n += a;
    };
    auto sq = [](int a) -> int {
        return a * a;
    };
    auto wide = [](int a) -> long long {
        return static_cast<long long>(a);
    };
    auto add = [](long long acc, int a) -> long long {
        return acc + static_cast<long long>(a);
    };
    auto desc = [](int a, int b) -> bool {
        return a > b;
    };
    long long c = std::count_if(v.begin(), v.end(), p);
    bool any = std::any_of(v.begin(), v.end(), p);
    bool all = std::all_of(v.begin(), v.end(), p);
    bool none = std::none_of(v.begin(), v.end(), p);
    long long t = std::accumulate(v.begin(), v.end(), 0LL, add);
    std::for_each(v.begin(), v.end(), bump);
    std::transform(v.begin(), v.end(), v.begin(), sq);
    std::transform(v.begin(), v.end(), std::back_inserter(w), wide);
    std::sort(v.begin(), v.end(), desc);
    std::printf("%lld %d %d %d %lld %d\\n", c, any, all, none, t, n);
}
"""

CALLABLE_RUST = [
    "    let mut c: i64 = bx::count_if(v, &p);",
    "    let mut any: bool = v.iter().copied().any(&p);",
    "    let mut all: bool = v.iter().copied().all(&p);",
    "    let mut none: bool = !v.iter().copied().any(&p);",
    "    let mut t: i64 = v.iter().copied().fold(0i64, &add);",
    "    v.iter().copied().for_each(&mut bump);",
    "    bx::transform(v, &sq);",
    "    w.extend(v.iter().copied().map(&wide));",
    "    bx::sort_by_less(v, &desc);",
]
CALLABLE_CXX = [
    "    long long c = std::count_if(v.begin(), v.end(), p);",
    "    bool any = std::any_of(v.begin(), v.end(), p);",
    "    bool all = std::all_of(v.begin(), v.end(), p);",
    "    bool none = std::none_of(v.begin(), v.end(), p);",
    "    long long t = std::accumulate(v.begin(), v.end(), 0LL, add);",
    "    std::for_each(v.begin(), v.end(), bump);",
    "    std::transform(v.begin(), v.end(), v.begin(), sq);",
    "    std::transform(v.begin(), v.end(), std::back_inserter(w), wide);",
    "    std::sort(v.begin(), v.end(), desc);",
]


@needs_clang
def test_callable_rows_are_bijections(tmp_path):
    prog = ok(tmp_path, CALLABLE)
    rs = RSP.rs_print(prog).splitlines()
    for line in CALLABLE_RUST:
        assert line in rs, line
    # `bump` modifies state: `let mut bump` and `&mut bump`, nothing else is FnMut
    assert "    let mut bump = |mut a: i32| {" in rs
    cxx = CP.cxx_print(prog).splitlines()
    for line in CALLABLE_CXX:
        assert line in cxx, line
    assert "#include <iterator>" in cxx and "#include <numeric>" in cxx and "#include <algorithm>" in cxx
    assert cli.check_roundtrip(prog, "clang++") == []


@needs_clang
def test_none_of_and_find_end_are_normal_forms(tmp_path):
    # N25 / N26: each C++ spelling of a negation is ONE Kernel term
    a = ok(tmp_path, "bool f(const std::vector<int>& v, int x) {\n"
                     "    auto p = [x](int a) -> bool {\n        return a == x;\n    };\n"
                     "    return std::none_of(v.begin(), v.end(), p) && std::find(v.begin(), v.end(), x) == v.end();\n}\n")
    b = ok(tmp_path, "bool f(const std::vector<int>& v, int x) {\n"
                     "    auto p = [x](int a) -> bool {\n        return a == x;\n    };\n"
                     "    return !std::any_of(v.begin(), v.end(), p) && v.end() == std::find(v.begin(), v.end(), x);\n}\n")
    c = ok(tmp_path, "bool f(const std::vector<int>& v, int x) {\n"
                     "    auto p = [x](int a) -> bool {\n        return a == x;\n    };\n"
                     "    return !std::any_of(v.begin(), v.end(), p) && !(std::find(v.begin(), v.end(), x) != v.end());\n}\n")
    assert a == b == c
    ret = [n for n in K.walk(a) if isinstance(n, K.Return)][-1].value
    assert ret.lhs == K.Unary("not", K.Alg("any_of", K.Var("v"), (K.Var("p"),)))
    assert ret.rhs == K.Unary("not", K.Alg("contains", K.Var("v"), (K.Var("x"),)))
    cxx = CP.cxx_print(a)
    assert ("    return std::none_of(v.begin(), v.end(), p) && (std::find(v.begin(), v.end(), x) == v.end());"
            in cxx.splitlines())
    assert "    return !v.iter().copied().any(&p) && !v.contains(&x);" in RSP.rs_print(a).splitlines()
    assert cli.check_roundtrip(a, "clang++") == []


# ------------------------------------------------------------------ the leads
@needs_clang
def test_accumulate_runs_in_the_init_type(tmp_path):
    # LEAD 1, confirmed: `std::accumulate(b, e, 0)` over long longs sums in
    # INT (every step truncates) -- no exact fold spelling: refused, located
    assert errs(tmp_path, "int f(const std::vector<long long>& w) {\n"
                          "    return std::accumulate(w.begin(), w.end(), 0);\n}\n") == [
        (2, 48, "unsupported: std::accumulate of i64 elements from an initial value of type i32 (C++ sums in the "
                "initial value's type, converting every step; normalizer: give the initial value a type that holds "
                "every element, e.g. `0LL` / `0i64`)")]
    # widening is exact: `e as A`; unsigned wraps in both
    prog = ok(tmp_path, "unsigned f(const std::vector<unsigned char>& b, const std::vector<unsigned>& u) {\n"
                        "    unsigned long long s = std::accumulate(b.begin(), b.end(), 0ULL);\n"
                        "    return std::accumulate(u.begin(), u.end(), 7U) + static_cast<unsigned>(s);\n}\n")
    rs = RSP.rs_print(prog)
    assert "b.iter().fold(0u64, |a, &e| a.wrapping_add(e as u64))" in rs
    assert "u.iter().fold(7u32, |a, &e| a.wrapping_add(e))" in rs
    # mixed signedness into an unsigned accumulator is refused (not a widening)
    assert errs(tmp_path, "unsigned f(const std::vector<int>& v) {\n"
                          "    return std::accumulate(v.begin(), v.end(), 0U);\n}\n")[0][2].startswith(
        "unsupported: std::accumulate of i32 elements from an initial value of type u32")


def test_acc_ok_is_value_preserving():
    assert TC.acc_ok(K.I64, K.I32) and TC.acc_ok(K.I32, K.U16) and TC.acc_ok(K.F64, K.I64)
    assert TC.acc_ok(K.USIZE, K.U64) and TC.acc_ok(K.U64, K.U8)
    assert not TC.acc_ok(K.I32, K.I64) and not TC.acc_ok(K.U32, K.I32) and not TC.acc_ok(K.I64, K.U64)
    assert not TC.acc_ok(K.I32, K.F64) and not TC.acc_ok(K.I8, K.I8)  # (char arithmetic promotes: refused)


@needs_clang
def test_count_is_ptrdiff_t(tmp_path):
    # LEAD 4, confirmed: std::count returns ptrdiff_t (long) -> i64; an int
    # destination is an explicit cast on both sides
    prog = ok(tmp_path, "int f(const std::vector<int>& v) {\n    return std::count(v.begin(), v.end(), 2);\n}\n")
    ret = [n for n in K.walk(prog) if isinstance(n, K.Return)][0].value
    assert ret == K.Cast(K.I32, K.Alg("count", K.Var("v"), (K.IntLit(2, K.I32),)))
    assert "    return bx::count(v, &2) as i32;" in RSP.rs_print(prog).splitlines()


@needs_clang
def test_f64_rows_are_refused(tmp_path):
    # LEADS 2, 3 and 5, confirmed as refusals: the f64 rows are distinguishable
    es = errs(tmp_path, "double f(std::vector<double>& d) {\n"
                        "    std::sort(d.begin(), d.end());\n"
                        "    double m = std::max(d[0], d[1]);\n"
                        "    return m + *std::min_element(d.begin(), d.end());\n}\n")
    assert es == [
        (2, 5, "unsupported: std::sort of a vector of double without a comparator (-0.0 and 0.0 are equivalent "
               "but print differently, so the result depends on the algorithm; f64 has no total order in Rust)"),
        (3, 16, "unsupported: std::max of two doubles (it returns its first argument unless the second compares "
                "greater, which differs from f64::max on NaN and signed zero)"),
        (4, 17, "unsupported: std::min_element over a vector of double (it returns the FIRST extreme, and -0.0 / "
                "0.0 compare equal but print differently; f64 has no total order in Rust)"),
    ]


@needs_clang
def test_algorithm_call_is_a_call_of_its_closure(tmp_path):
    # LEAD 6: the closure's by-reference effects happen AT the algorithm call.
    # `n` is the counting loop's bound; C++ re-reads it every iteration, the
    # Rust range evaluates it once -- equal only if the body leaves n alone.
    es = errs(tmp_path, "int f(const std::vector<int>& v, int n) {\n"
                        "    int s = 0;\n"
                        "    auto shrink = [&n](int a) -> bool {\n        n -= 1;\n        return a > 0;\n    };\n"
                        "    for (int i = 0; i < n; ++i) {\n"
                        "        s += static_cast<int>(std::count_if(v.begin(), v.end(), shrink));\n    }\n"
                        "    return s;\n}\n")
    assert (7, 5, "unsupported: counting loop body assigns or shadows the bound 'n'") in es
    # ... and a closure that modifies the container it is iterating is refused
    es = errs(tmp_path, "void f(std::vector<int>& v) {\n"
                        "    auto grow = [&v](int a) {\n        v.push_back(a);\n    };\n"
                        "    std::for_each(v.begin(), v.end(), grow);\n}\n")
    assert es == [(5, 19, "unsupported: 'v' is used while closure 'grow', which modifies it through a by-reference "
                          "capture, is still called later (a second borrow in Rust)")]
    # a comparator that READS the vector being sorted
    es = errs(tmp_path, "void f(std::vector<int>& v) {\n"
                        "    auto peek = [&v](int a, int b) -> bool {\n        return a + v[0] < b;\n    };\n"
                        "    std::sort(v.begin(), v.end(), peek);\n}\n")
    assert es == [(5, 5, "unsupported: 'v' is modified or moved while closure 'peek', which captures it by "
                         "reference, is still called later (a borrow conflict in Rust)")]


@needs_clang
def test_printing_closures_make_an_algorithm_a_printing_call(tmp_path):
    # LEAD 7 (FR-263): two algorithm calls whose closure prints, unsequenced
    src = ("long long f(const std::vector<int>& v) {\n"
           "    auto loud = [](int a) -> bool {\n        std::printf(\"%d\\n\", a);\n        return a > 0;\n    };\n"
           "    return std::count_if(v.begin(), v.end(), loud) + std::count_if(v.begin(), v.end(), loud);\n}\n")
    assert errs(tmp_path, src) == [
        (6, 54, "unsupported: two calls that may print in one statement without a sequence point between them "
                "(C++ leaves their order unspecified; normalizer: evaluate each into a local first)")]
    # one per statement is fine; so is a quiet closure twice
    ok(tmp_path, src.replace(" + std::count_if(v.begin(), v.end(), loud)", ""))
    ok(tmp_path, src.replace("std::printf(\"%d\\n\", a);\n        ", ""))


# ------------------------------------------------------------------ callables
@needs_clang
def test_callable_argument_refusals(tmp_path):
    es = errs(tmp_path, "bool pos(int a) {\n    return a > 0;\n}\n"
                        "long long f(std::vector<int>& v) {\n"
                        "    long long a = std::count_if(v.begin(), v.end(), [](int x) -> bool { return x > 1; });\n"
                        "    long long b = std::count_if(v.begin(), v.end(), pos);\n"
                        "    int k = 0;\n"
                        "    auto step = [k](int x) mutable -> bool {\n        k += x;\n        return k > 3;\n    };\n"
                        "    long long c = std::count_if(v.begin(), v.end(), step);\n"
                        "    auto wide = [](int x) -> long long {\n        return x;\n    };\n"
                        "    std::transform(v.begin(), v.end(), v.begin(), wide);\n"
                        "    return a + b + c;\n}\n")
    assert es == [
        (5, 53, "unsupported: lambda expression as an argument of std::count_if (normalizer: bind it to a local "
                "closure first, `auto f = [..](..) {..};`, and pass `f`)"),
        (6, 53, "unsupported: function 'pos' passed to std::count_if (the subset passes a local closure)"),
        (12, 53, "unsupported: closure 'step' modifies its own copy of a capture (a `mutable` lambda) and is passed "
                 "to std::count_if, which copies it in C++ while Rust would borrow it (normalizer: capture the state "
                 "by reference)"),
        (16, 51, "unsupported: closure 'wide' has type impl Fn(i32) -> i64, but std::transform calls it as impl "
                 "Fn(i32) -> i32 (the subset has no implicit conversion of an argument or result)"),
    ]


@needs_clang
def test_callable_parameter_is_not_passed_to_an_algorithm(tmp_path):
    es = errs(tmp_path, "template <typename F>\nlong long via(const std::vector<int>& v, const F& f) {\n"
                        "    return std::count_if(v.begin(), v.end(), f);\n}\n"
                        "int main() {\n    std::vector<int> v = {1, 2};\n"
                        "    auto p = [](int a) -> bool {\n        return a > 1;\n    };\n"
                        "    std::printf(\"%lld\\n\", via(v, p));\n    return 0;\n}\n")
    assert es == [(3, 46, "unsupported: callable parameter 'f' passed to std::count_if (the subset passes a local "
                          "closure to an algorithm)")]


# ------------------------------------------------------------------ containers
@needs_clang
def test_whole_container_refusals(tmp_path):
    es = errs(tmp_path, "void f(std::vector<int>& v, std::vector<int>& u, std::string& s) {\n"
                        "    std::sort(v.begin() + 1, v.end());\n"
                        "    std::reverse(v.begin(), u.end());\n"
                        "    std::sort(s.begin(), s.end());\n"
                        "    std::stable_sort(v.begin(), v.end(), [](int a, int b) -> bool { return a < b; });\n"
                        "    std::max_element(v.begin(), v.end());\n}\n")
    assert es == [
        (2, 5, "unsupported: std::sort over something other than a whole container `v.begin(), v.end()` (free "
               "iterators are outside the subset)"),
        (3, 5, "unsupported: std::reverse over the begin() and end() of two different containers"),
        (4, 5, "unsupported: std::sort over a std::string (the subset's algorithms take a std::vector)"),
        (5, 5, "unsupported: std::stable_sort with a comparator"),
        (6, 5, "unsupported: std::max_element other than `*std::max_element(v.begin(), v.end())` (iterators are "
               "outside the subset)"),
    ]


@needs_clang
def test_value_type_and_alias_refusals(tmp_path):
    es = errs(tmp_path, "struct P {\n    std::vector<long long> items;\n    int tag;\n};\n"
                        "long long f(std::vector<int>& v, P& p, std::vector<long long>& w) {\n"
                        "    std::fill(v.begin(), v.end(), v[0]);\n"
                        "    std::fill(p.items.begin(), p.items.end(), p.tag);\n"
                        "    auto id = [](int a) -> long long {\n        return a;\n    };\n"
                        "    std::transform(v.begin(), v.end(), std::back_inserter(p.items), id);\n"
                        "    return std::count(w.begin(), w.end(), v[1]);\n}\n")
    # `p.items.fill(p.tag as i64)` and appending to p.items while reading v are fine
    # (disjoint access paths, measured on rustc); filling v with v[0] is E0502
    assert es == [
        (6, 35, "unsupported: the value of std::fill reads 'v', the vector it fills (a second borrow in Rust)"),
        (12, 43, "unsupported: std::count of a value of type i32 in a vector of i64 (C++ would compare after the "
                 "usual arithmetic conversions; normalizer: static_cast the value to the element type)"),
    ]


@needs_clang
def test_sort_keeps_a_size_bound_extend_does_not(tmp_path):
    # sort / fill / transform rewrite ELEMENTS: `i < v.size()` is still exact
    ok(tmp_path, "void f(std::vector<int>& v) {\n"
                 "    for (std::size_t i = 0; i < v.size(); ++i) {\n        std::sort(v.begin(), v.end());\n    }\n}\n")
    es = errs(tmp_path, "void f(std::vector<int>& v, std::vector<int>& w) {\n"
                        "    auto id = [](int a) -> int {\n        return a;\n    };\n"
                        "    for (std::size_t i = 0; i < w.size(); ++i) {\n"
                        "        std::transform(v.begin(), v.end(), std::back_inserter(w), id);\n    }\n}\n")
    assert es == [(5, 5, "unsupported: counting loop body modifies or shadows 'w', whose size is the bound")]


@needs_clang
def test_user_function_named_like_an_algorithm(tmp_path):
    prog = ok(tmp_path, "int count(int a) {\n    return a + 1;\n}\n"
                        "int f() {\n    return count(2);\n}\n")
    assert not algs(prog)
    assert [n for n in K.walk(prog) if isinstance(n, K.Call)] == [K.Call("count", (K.IntLit(2, K.I32),))]


# ------------------------------------------------------------------ Rust side
RS = """fn f(v: &mut Vec<i32>, w: &mut Vec<i64>, mut x: i32) -> i64 {
    v.sort_unstable();
    w.sort();
    v.reverse();
    w.fill(x as i64);
    let mut h: bool = v.contains(&x) && !w.contains(&5i64);
    let mut c: i64 = bx::count(v, &x);
    let mut s: i64 = v.iter().fold(0i64, |a, &e| a + e as i64);
    let mut m: i32 = *v.iter().max().unwrap() + std::cmp::min(x, 2);
    return c + s + (m as i64) + (h as i64);
}

mod bx {
""" + fmt.BX_HELPERS["count"] + "}\n"


def test_rust_rows_parse_to_the_kernel_and_print_back():
    prog = RP.rs_parse(RS)
    ops = [(n.op if not isinstance(n, K.MinMax) else "cmp_" + n.op) for n in algs(prog)]
    assert ops == ["sort", "stable_sort", "reverse", "fill", "contains", "contains", "count", "accumulate",
                   "max_element", "cmp_min"]
    assert RSP.rs_print(prog) == RS
    assert "    std::sort(v.begin(), v.end());" in CP.cxx_print(prog).splitlines()


@pytest.mark.parametrize("line,expected", [
    ("    let mut s: i32 = v.iter().fold(0, |a, &e| a.wrapping_add(e));",
     "unsupported: the fold of std::accumulate is not spelled the canonical way (`|a, &e| a + e` for a i32 "
     "accumulator over i32 elements)"),
    ("    let mut s: i64 = v.iter().fold(0i64, |a, &e| a + e);",
     "unsupported: the fold of std::accumulate is not spelled the canonical way (`|a, &e| a + e as i64` for a i64 "
     "accumulator over i32 elements)"),
    ("    let mut c: i64 = bx::count(v, x);",
     "unsupported: an argument of std::count is not spelled the canonical way (`&x`)"),
    ("    let mut h: bool = v.contains(x);",
     "unsupported: an argument of std::find is not spelled the canonical way (`&x`)"),
    ("    v.fill(v[0usize]);",
     "unsupported: the value of std::fill reads 'v', the vector it fills (a second borrow in Rust)"),
])
def test_rust_non_canonical_spellings_are_refused(line, expected):
    src = RS.replace("    return c + s", line + "\n    return c + s")
    assert expected in [m for _, m in rs_errs(src)]


@pytest.mark.parametrize("src,expected", [
    ("fn f(v: &Vec<i32>) -> i32 {\n    return v.iter().fold(0, |acc, &e| acc + e);\n}\n",
     [("2:29", "unsupported: fold closure other than `|a, &e| a + e` (the canonical std::accumulate step: "
               "`a.wrapping_add(e)` for an unsigned accumulator, `e as T` when the element type differs; any other "
               "operation is a closure variable `.copied().fold(init, &f)`)")]),
    ("fn f(v: &Vec<i32>) -> bool {\n    let p = |a: i32| -> bool {\n        return a > 0;\n    };\n"
     "    return v.iter().any(&p);\n}\n",
     [("5:21", "unsupported: iterator method `.any()` (the subset's iterator chains are `v.iter().fold(init, |a, &e| "
               "..)`, `v.iter().max().unwrap()` / `.min()`, and `v.iter().copied()` followed by `.fold(init, &f)`, "
               "`.any(&p)`, `.all(&p)`, `.for_each(&f)` or, inside `w.extend(..)`, `.map(&f)`)")]),
    ("fn f(v: &Vec<i32>) -> i32 {\n    let m: i32 = v.iter().max().unwrap();\n    return m;\n}\n",
     [("2:18", "unsupported: `v.iter().max().unwrap()` other than dereferenced at once "
               "(`*v.iter().max().unwrap()`)")]),
    ("fn f(v: &mut Vec<i32>, w: &Vec<i32>) {\n    v.extend(w.iter().copied());\n}\n",
     [("2:16", "unsupported: `.extend()` of something other than `v.iter().copied().map(&f)`")]),
    ("fn f(v: &mut Vec<i32>) {\n    let mut n: i32 = 0;\n    let mut cnt = |a: i32| -> bool {\n        n += 1;\n"
     "        return a > 0;\n    };\n    let mut c: i64 = bx::count_if(v, &cnt);\n}\n\nmod bx {\n"
     + fmt.BX_HELPERS["count_if"] + "}\n",
     None),
])
def test_rust_iterator_neighbours_are_located(src, expected):
    got = rs_errs(src)
    if expected is None:
        assert got and got[0][1].startswith("unsupported: closure argument 'cnt' of std::count_if is not spelled "
                                            "the canonical way (`&mut cnt`")
    else:
        assert got[:1] == expected


def test_helpers_are_safe_and_canonical():
    for name in ("count", "count_if", "transform", "sort_by_less"):
        assert "unsafe" not in fmt.BX_HELPERS[name]
        assert name in fmt.BX_ORDER
    # a printed `mod bx` with every FR-253 helper parses back token for token
    src = "fn main() {\n}\n\n" + fmt.bx_module({"count", "count_if", "transform", "sort_by_less"})
    RP.rs_parse(src)
    bad = src.replace("if n > 16 {", "if n > 15 {")
    line = src.splitlines().index("    pub fn sort_by_less<T: Copy>(v: &mut [T], mut less: impl FnMut(T, T) -> bool) {") + 1
    assert rs_errs(bad) == [("%d:5" % line, "unsupported: `bx::sort_by_less` differs from the canonical helper")]


# ------------------------------------------------------------------ introsort
KILLER = os.path.join(ROOT, "test", "EndToEnd", "bx-alg-sort.cpp")


def killer_input():
    """The 128-element McIlroy adversary input embedded in the EndToEnd test."""
    text = open(KILLER).read()
    body = text[text.index("std::vector<int> v = {") + len("std::vector<int> v = {"):]
    body = body[:body.index("}")]
    return [int(x) for x in body.replace("\n", " ").split(",")]


def test_killer_input_is_a_permutation():
    k = killer_input()
    assert sorted(k) == list(range(128))


@needs_rustc
def test_killer_drives_the_depth_limit_branch(tmp_path):
    """The EndToEnd byte-diff covers introsort's heapsort fallback only if
    its input actually reaches it: an instrumented helper reports it."""
    helper = fmt.BX_HELPERS["sort_by_less"].replace(
        "sbl_heap_sort(v, first, last, less);",
        'eprintln!("HEAP {} {}", first, last);\n                sbl_heap_sort(v, first, last, less);')
    assert helper != fmt.BX_HELPERS["sort_by_less"]
    data = ", ".join(str(x) for x in killer_input())
    src = ("mod bx {\n" + helper + "}\nfn main() {\n    let mut v: Vec<i32> = vec![%s];\n"
           "    let mut c: i64 = 0;\n    bx::sort_by_less(&mut v, |a: i32, b: i32| { c += 1; a / 2 < b / 2 });\n"
           "    println!(\"{} {}\", c, v.windows(2).all(|w| w[0] / 2 <= w[1] / 2));\n}\n" % data)
    rs = tmp_path / "k.rs"
    rs.write_text(src)
    exe = tmp_path / "k"
    subprocess.run(["rustc", "-O", "--edition", "2021", str(rs), "-o", str(exe)], check=True,
                   capture_output=True)
    r = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert "HEAP " in r.stderr
    assert r.stdout.split() == ["2518", "true"]  # 2518 comparisons: clang++'s count in bx-alg-sort.cpp
