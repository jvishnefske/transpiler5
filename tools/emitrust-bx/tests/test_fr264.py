# Pins FR-264: namespace-scope scalar constants, `const T k = e;` <=>
# `const k: T = e;`.  The invariants, and why each is load-bearing:
#  * ONE normal form: `const`, `static const`, `constexpr` and
#    `inline constexpr` at namespace scope import to the same ConstItem and
#    print back as `const T k = e;` -- at namespace scope a const object
#    already has internal linkage and the subset has no constant-expression
#    context that `constexpr` would add (no array bounds, case labels or
#    template arguments), so laws 1-2 can only hold with one spelling;
#  * a use is a ConstRef, NOT a Var: a Var is a place, gets moved, and lands
#    in a lambda's capture list -- and C++ cannot capture a non-automatic
#    variable;
#  * the Rust spelling evaluates like C++: unsigned arithmetic keeps the
#    wrapping_* methods (const fn), a narrowing initializer prints its
#    conversion (`300i32 as u8`), and the Rust crate opts out of the
#    upper-case lint only when it has a constant;
#  * the shapes with no 1:1 spelling are LOCATED refusals, never silently
#    wrong or silently unbuildable: a binding of a constant's name (rustc
#    E0005/E0530 even for equal values), a use before the definition (C++
#    requires define-before-use; Rust accepts it), an internal constant used
#    outside its namespace (E0603), a non-kernel type, a non-constant
#    initializer, a duplicate, a collision with a function;
#  * mutable / extern / volatile / thread_local globals keep "global
#    variable", function-local statics keep "static local variable";
#  * every new wording is classified by scripts/bx-census.py (an
#    unclassified refusal lands in the `other` bucket and is invisible to
#    ranking);
#  * the OUTPUT behaves like the input: clang++ of the original, of the
#    printed C++ and of the printed Rust print the same bytes.
import importlib.util
import os
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
HEAD = "#include <cstdio>\n#include <cstdint>\n#include <cstddef>\n"
ALLOW = "#![allow(non_upper_case_globals)]\n"


def imp(tmp_path, src, name="t.cpp"):
    p = tmp_path / name
    p.write_text(src)
    return cli.load_cpp(str(p), "clang++")


def located(errs):
    return [("%s" % loc, m) for loc, m in errs]


def consts(prog):
    return [(c.path, str(c.ty), c.internal) for c in prog.consts()]


# ------------------------------------------------------------------ accepted

SPELLINGS = HEAD + (
    "static const int kA = 7;\n"
    "constexpr int kB = kA * 2;\n"
    "inline constexpr int kC = 3;\n"
    "const int kD = kB + kC;\n"
    "namespace ns {\n"
    "static const std::uint32_t kE = 5u;\n"
    "namespace {\n"
    "const int kF = 9;\n"
    "}\n"
    "int g() {\n"
    "    return kF + (int)kE;\n"
    "}\n"
    "}  // namespace ns\n"
    "int main() {\n"
    "    std::printf(\"%d %d %u\\n\", kD, ns::g(), ns::kE);\n"
    "    return 0;\n"
    "}\n")


@needs_clang
def test_every_spelling_is_one_normal_form_and_both_laws_hold(tmp_path):
    prog, errs = imp(tmp_path, SPELLINGS)
    assert errs == [], errs
    assert consts(prog) == [("kA", "i32", False), ("kB", "i32", False), ("kC", "i32", False),
                            ("kD", "i32", False), ("ns::kE", "u32", False), ("ns::kF", "i32", True)]
    assert cli.check_roundtrip(prog, "clang++") == []
    cxx = CP.cxx_print(prog)
    for line in ("const int kA = 7;", "const int kB = kA * 2;", "const int kC = 3;", "const int kD = kB + kC;",
                 "namespace ns {\nconst unsigned kE = 5U;\nnamespace {\nconst int kF = 9;\n}  // namespace"):
        assert line + "\n" in cxx, cxx
    assert "static const" not in cxx and "constexpr" not in cxx
    # every spelling is the same Kernel: rewrite all four to plain `const`
    plain = SPELLINGS.replace("static const", "const").replace("inline constexpr", "const")
    plain = plain.replace("constexpr", "const")
    prog2, errs2 = imp(tmp_path, plain, "plain.cpp")
    assert errs2 == [] and prog2 == prog
    rs = RSP.rs_print(prog)
    assert rs.startswith(ALLOW + "\n"), rs
    for line in ("const kA: i32 = 7;", "const kB: i32 = kA * 2;", "const kD: i32 = kB + kC;",
                 "    pub const kE: u32 = 5u32;", "    const kF: i32 = 9;"):
        assert line + "\n" in rs, rs
    assert "print!(\"{} {} {}\\n\", kD, crate::ns::g(), crate::ns::kE);" in rs
    assert RP.rs_parse(rs) == prog


@needs_clang
def test_no_constant_no_lint_attribute(tmp_path):
    prog, errs = imp(tmp_path, HEAD + "int main() {\n    std::printf(\"%d\\n\", 1);\n    return 0;\n}\n")
    assert errs == []
    assert "allow(non_upper_case_globals)" not in RSP.rs_print(prog)


@needs_clang
def test_kernel_dump_names_constants(tmp_path):
    prog, errs = imp(tmp_path, SPELLINGS)
    d = K.dump(prog)
    assert "const kB: i32 = (mul (const kA) 2:i32)\n" in d, d
    assert "const internal ns::kF: i32 = 9:i32\n" in d, d


WRAP = HEAD + (
    "const std::uint32_t kB = 0xFFFFFFFFu + 2u;\n"
    "const std::uint32_t kSub = 1u - 3u;\n"
    "const std::uint32_t kNeg = -5u;\n"
    "const std::uint8_t kN = 300;\n"
    "const std::int8_t kM = 200;\n"
    "const std::uint8_t kBig = kN + 300;\n"
    "static const double kS2 = 1.414213562;\n"
    "const int kPrec = 1 << 13;\n"
    "static const int kFP = static_cast<int>(kS2 * kPrec);\n"
    "const std::size_t kUsz = std::size_t(1) << (std::size_t(15) - std::size_t(9));\n"
    "const bool kCmp = kPrec < kFP;\n"
    "const int kTern = kFP > 3 ? kFP : -kFP;\n"
    "int main() {\n"
    "    std::printf(\"%u %u %u %d %d %d\\n\", kB, kSub, kNeg, (int)kN, (int)kM, (int)kBig);\n"
    "    std::printf(\"%d %zu %d %d %.3f\\n\", kFP, kUsz, (int)kCmp, kTern, kS2);\n"
    "    return 0;\n"
    "}\n")


@needs_clang
def test_wrapping_and_narrowing_spellings(tmp_path):
    prog, errs = imp(tmp_path, WRAP)
    assert errs == [], errs
    rs = RSP.rs_print(prog)
    for line in ("const kB: u32 = 4294967295u32.wrapping_add(2u32);",
                 "const kSub: u32 = 1u32.wrapping_sub(3u32);",
                 "const kNeg: u32 = 5u32.wrapping_neg();",
                 "const kN: u8 = 300i32 as u8;",
                 "const kM: i8 = 200i32 as i8;",
                 "const kBig: u8 = ((kN as i32) + 300i32) as u8;",
                 "const kFP: i32 = (kS2 * (kPrec as f64)) as i32;",
                 "const kUsz: usize = 1usize << 15usize.wrapping_sub(9usize);",
                 "const kTern: i32 = if kFP > 3 { kFP } else { -kFP };"):
        assert line + "\n" in rs, rs
    assert cli.check_roundtrip(prog, "clang++") == []


@needs_clang
def test_constant_is_never_captured_by_a_lambda(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "const int kA = 7;\n"
        "int main() {\n"
        "    int y = 2;\n"
        "    auto g = [](int x) -> int { return x + kA; };\n"
        "    auto h = [y](int x) -> int { return x + y + kA; };\n"
        "    std::printf(\"%d %d\\n\", g(1), h(1));\n"
        "    return 0;\n"
        "}\n"))
    assert errs == [], errs
    closures = [n for n in K.walk(prog) if isinstance(n, K.Closure)]
    assert [list(K.closure_captures(c)) for c in closures] == [[], ["y"]]
    assert not any(isinstance(n, K.Var) and n.name == "kA" for n in K.walk(prog))
    cxx = CP.cxx_print(prog)
    assert "[](int x) -> int {" in cxx and "[y](int x) -> int {" in cxx, cxx
    assert cli.check_roundtrip(prog, "clang++") == []


@needs_clang
def test_constant_counting_loop_bound_and_reference_parameter(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "const int kN = 4;\n"
        "int r(const int& x) {\n"
        "    return x + 1;\n"
        "}\n"
        "int main() {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < kN; ++i) {\n"
        "        s += i;\n"
        "    }\n"
        "    std::printf(\"%d %d\\n\", s + r(kN), kN);\n"
        "    return 0;\n"
        "}\n"))
    assert errs == [], errs
    assert "for i in 0..kN {" in RSP.rs_print(prog)
    assert cli.check_roundtrip(prog, "clang++") == []


@needs_clang
def test_qualified_spellings_across_namespaces_and_members(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "const int kA = 3;\n"
        "namespace a {\n"
        "const int kB = kA + 1;\n"
        "int f(int kA) {\n"  # a parameter may share the name of ANOTHER namespace's constant
        "    return kA + kB + ::kA;\n"
        "}\n"
        "}  // namespace a\n"
        "struct S {\n"
        "    int kA;\n"
        "    int get() const { return kA + ::kA + a::kB; }\n"
        "};\n"
        "int main() {\n"
        "    S s{4};\n"
        "    std::printf(\"%d %d\\n\", s.get(), a::f(1));\n"
        "    return 0;\n"
        "}\n"))
    assert errs == [], errs
    cxx = CP.cxx_print(prog)
    assert "const int kA = 3;\nnamespace a {\nconst int kB = ::kA + 1;\n}  // namespace a\n" in cxx, cxx
    assert "return kA + kB + ::kA;" in cxx and "return this->kA + ::kA + ::a::kB;" in cxx, cxx
    rs = RSP.rs_print(prog)
    assert "    pub const kB: i32 = crate::kA + 1;\n" in rs, rs
    assert "return kA + kB + crate::kA;" in rs, rs
    assert cli.check_roundtrip(prog, "clang++") == []


# ------------------------------------------------------------------ Rust side

RUST = ALLOW + """
const kA: i32 = 7;

mod a {
    pub const kB: u32 = 4294967295u32.wrapping_add(2u32);

    const kC: i32 = crate::kA * 2;

    pub fn g() -> i32 {
        let kA: i32 = 1;
        return kA + kC + crate::kA;
    }
}

fn main() {
    print!("{} {} {}\\n", kA, crate::a::kB, crate::a::g());
}
"""


@needs_clang
def test_rust_constants_parse_and_print_to_cpp():
    prog, errs = RP.rs_parse_all(RUST)
    assert errs == [], errs
    assert consts(prog) == [("kA", "i32", False), ("a::kB", "u32", False), ("a::kC", "i32", True)]
    assert RSP.rs_print(prog) == RUST
    cxx = CP.cxx_print(prog)
    assert ("const int kA = 7;\nnamespace a {\nconst unsigned kB = 4294967295U + 2U;\nnamespace {\n"
            "const int kC = ::kA * 2;\n}  // namespace\n}  // namespace a\n") in cxx, cxx
    assert "return kA + kC + ::kA;" in cxx, cxx
    assert cli.check_roundtrip(prog, "clang++") == []


def test_rust_function_may_use_a_later_constant():
    prog, errs = RP.rs_parse_all("fn f() -> i32 {\n    return kB;\n}\n\nconst kB: i32 = 2;\n\n"
                                 "fn main() {\n    print!(\"{}\\n\", f());\n}\n")
    assert errs == [], errs
    cxx = CP.cxx_print(prog)
    assert cxx.index("const int kB = 2;") < cxx.index("int f()"), cxx


@pytest.mark.parametrize("src,expected", [
    # Rust accepts a forward reference between constants; C++ does not
    (ALLOW + "\nconst kA: i32 = kB + 1;\n\nconst kB: i32 = 2;\n",
     ("3:17", "constant 'kB' used before its definition (C++ defines a constant before its use)")),
    (ALLOW + "\nconst kA: i32 = 1;\n\nconst kA: i32 = 2;\n",
     ("5:7", "constant 'kA' defined twice")),
    (ALLOW + "\nconst f: i32 = 1;\n\nfn f() -> i32 {\n    return 2;\n}\n",
     ("3:7", "constant 'f' has the name of a function")),
    (ALLOW + "\nconst a: i32 = 1;\n\nmod a {\n    pub fn f() {\n    }\n}\n",
     ("3:7", "constant 'a' has the name of a namespace declared in the same scope")),
    (ALLOW + "\nconst kA: i32 = 7;\n\nfn g(x: i32) -> i32 {\n    let kA: i32 = 1;\n    return kA + x;\n}\n",
     ("6:5", "variable 'kA' has the name of a constant")),
    (ALLOW + "\nconst kA: i32 = 7;\n\nfn g(kA: i32) -> i32 {\n    return kA;\n}\n",
     ("5:6", "variable 'kA' has the name of a constant")),
    (ALLOW + "\nconst kA: i32 = g();\n\nfn g() -> i32 {\n    return 1;\n}\n",
     ("3:17", "constant initializer is not a constant expression")),
    (ALLOW + "\nconst kA: String = String::new();\n",
     ("3:7", "constant of non-kernel type 'String'")),
    (ALLOW + "\nmod ns {\n    const kF: i32 = 9;\n}\n\nfn main() {\n    print!(\"{}\\n\", crate::ns::kF);\n}\n",
     ("8:20", "unsupported: 'ns::kF' is internal to namespace 'ns' (a C++ anonymous-namespace member, a private "
              "Rust `const`) and is referenced from outside it")),
])
def test_rust_constant_refusals_are_located(src, expected):
    prog, errs = RP.rs_parse_all(src)
    assert prog is None
    assert expected in located(errs), errs


def test_rust_unknown_value_path_stays_refused():
    prog, errs = RP.rs_parse_all("fn main() {\n    print!(\"{}\\n\", crate::nope);\n}\n")
    assert prog is None
    assert located(errs) == [("2:20", "unsupported: path expression `crate::nope`")], errs


# ------------------------------------------------------------------ C++ refusals

@needs_clang
def test_cxx_declaration_refusals_are_located(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "int g = 3;\n"                                  # 4
        "const int kF = g;\n"                           # 5
        "int h() {\n"                                   # 6
        "    return 4;\n"
        "}\n"
        "const int kH = h();\n"                         # 9
        "static const std::uint8_t kSig[] = {1, 2};\n"  # 10
        "extern const int kE;\n"                        # 11
        "const int* const kP = &g;\n"                   # 12
        "const float kFl = 1.5f;\n"                     # 13
        "const volatile int kV = 1;\n"                  # 14
        "thread_local const int kT = 2;\n"              # 15
        "int main() {\n"
        "    std::printf(\"%d %d\\n\", kF, kH);\n"
        "    return 0;\n"
        "}\n"))
    assert prog is None
    assert located(errs) == [
        ("4:1", "unsupported: global variable"),
        ("5:16", "unsupported: global variable"),
        ("9:16", "unsupported: constant initializer is not a constant expression"),
        ("10:1", "unsupported: constant of non-kernel type 'const std::uint8_t[2]'"),
        ("11:1", "unsupported: global variable"),
        ("12:1", "unsupported: constant of non-kernel type 'const int *const'"),
        ("13:1", "unsupported: constant of non-kernel type 'const float'"),
        ("14:1", "unsupported: global variable"),
        ("15:1", "unsupported: global variable"),
    ], errs


@needs_clang
def test_cxx_use_refusals_are_located(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "const int kOk = 5;\n"                          # 4
        "static const std::uint8_t kSig[] = {1, 2};\n"  # 5
        "int addr(const int* p) {\n"                    # 6
        "    return p[0];\n"
        "}\n"
        "int f(int x) {\n"                              # 9
        "    return x + addr(&kOk);\n"                  # 10
        "}\n"
        "int k(int x) {\n"                              # 12
        "    return x + kSig[1];\n"                     # 13
        "}\n"
        "int main() {\n"
        "    std::printf(\"%d %d\\n\", f(1), k(1));\n"
        "    return 0;\n"
        "}\n"))
    assert prog is None
    got = located(errs)
    assert ("10:21", "unsupported: address of something other than a variable or data member") in got, got
    assert ("5:1", "unsupported: constant of non-kernel type 'const std::uint8_t[2]'") in got, got
    assert any(l.startswith("13:") for l, _ in got), got


@needs_clang
@pytest.mark.parametrize("body,expected", [
    ("int f(int x) {\n    int kA = 5;\n    return x + kA;\n}\n", ("6:5", "variable 'kA' has the name of a constant")),
    ("int f(int kA) {\n    return kA;\n}\n", ("5:11", "variable 'kA' has the name of a constant")),
    ("int f(int x) {\n    for (int kA = 0; kA < 2; ++kA) {\n        x += kA;\n    }\n    return x;\n}\n",
     ("6:5", "variable 'kA' has the name of a constant")),
    ("int f(int x) {\n    auto g = [](int kA) -> int { return kA; };\n    return g(x);\n}\n",
     ("6:21", "variable 'kA' has the name of a constant")),
])
def test_cxx_binding_with_a_constants_name_is_refused(tmp_path, body, expected):
    prog, errs = imp(tmp_path, HEAD + "const int kA = 7;\n" + body +
                     "int main() {\n    std::printf(\"%d\\n\", f(1));\n    return 0;\n}\n")
    assert prog is None
    assert located(errs) == [(expected[0], "unsupported: " + expected[1])], errs


@needs_clang
def test_cxx_out_of_order_after_namespace_merge_is_refused(tmp_path):
    # legal C++; N13 merges the reopened `a` before `k0`, and C++ needs k0 first
    prog, errs = imp(tmp_path, HEAD + (
        "namespace a {\nconst int k1 = 1;\n}\n"
        "const int k0 = 2;\n"
        "namespace a {\nconst int k3 = k0 + k1;\n}\n"
        "int main() {\n    std::printf(\"%d\\n\", a::k3);\n    return 0;\n}\n"))
    assert prog is None
    assert located(errs) == [("9:16", "unsupported: constant 'k0' used before its definition (C++ defines a "
                                      "constant before its use)")], errs


@needs_clang
def test_cxx_internal_constant_used_outside_is_refused(tmp_path):
    prog, errs = imp(tmp_path, HEAD + "namespace ns {\nnamespace {\nconst int kF = 9;\n}\n}\n"
                     "int main() {\n    std::printf(\"%d\\n\", ns::kF);\n    return 0;\n}\n")
    assert prog is None
    assert located(errs) == [("10:25", "unsupported: 'ns::kF' is internal to namespace 'ns' (a C++ "
                                       "anonymous-namespace member, a private Rust `const`) and is referenced "
                                       "from outside it")], errs


@needs_clang
def test_function_locals_are_unchanged(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "int f(int x) {\n    static const int kL = 5;\n    return x + kL;\n}\n"
        "int g(int x) {\n    constexpr int kC = 5;\n    return x + kC;\n}\n"
        "int h(int x) {\n    const int kLoc = 6;\n    return x + kLoc;\n}\n"
        "int main() {\n    std::printf(\"%d %d %d\\n\", f(1), g(1), h(1));\n    return 0;\n}\n"))
    assert prog is None
    msgs = [m for _, m in errs]
    assert "unsupported: static local variable" in msgs and "unsupported: constexpr variable" in msgs, errs
    assert not any(l.startswith("13:") or l.startswith("14:") for l, _ in located(errs)), errs


# ------------------------------------------------------------------ header constants (R4)

CONSTS_H = """#pragma once
#include <cstddef>
#include <cstdint>
namespace brunsli {
static const std::size_t kMaxHuffmanBits = 15u;
static const int kPrecBits = 13;
static const int kPrec = 1 << kPrecBits;
static const int kUnused = 99;
static const std::uint8_t kArr[] = {1, 2, 3};
inline int Scale(int x) {
    return x * kPrec;
}
}  // namespace brunsli
"""

CONSTS_MAIN = """#include <cstdio>
#include "inc/consts.h"
namespace brunsli {
static const double kSqrt2 = 1.414213562;
static const int kFixed = static_cast<int>(kSqrt2 * kPrec);
int Mul(int q) {
    return q * kFixed + Scale(1) + static_cast<int>(kMaxHuffmanBits);
}
}  // namespace brunsli
int main() {
    std::printf("%d\\n", brunsli::Mul(3));
    return 0;
}
"""


def project(tmp_path, main=CONSTS_MAIN, header=CONSTS_H):
    (tmp_path / "inc").mkdir(exist_ok=True)
    (tmp_path / "inc" / "consts.h").write_text(header)
    p = tmp_path / "main.cpp"
    p.write_text(main)
    return p


def where(loc):
    return (os.path.basename(loc.file) if loc.file else "main", loc.line, loc.col)


@needs_clang
def test_header_constants_are_imported_lazily_and_transitively(tmp_path):
    notes = []
    prog, errs = cli.load_cpp(str(project(tmp_path)), "clang++", True, notes, str(tmp_path))
    assert errs == [], errs
    # anchored before the first top-level main-file declaration that reaches
    # them (here `namespace brunsli`), dependencies first
    assert [c.path for c in prog.consts()] == ["brunsli::kPrecBits", "brunsli::kPrec", "brunsli::kMaxHuffmanBits",
                                               "brunsli::kSqrt2", "brunsli::kFixed"]
    hdr = str(tmp_path / "inc" / "consts.h")
    got = sorted((where(l), m) for l, m in notes if "header constant" in m)
    assert got == [(("consts.h", 5, 26), "normalized: header constant 'kMaxHuffmanBits' from " + hdr),
                   (("consts.h", 6, 18), "normalized: header constant 'kPrecBits' from " + hdr),
                   (("consts.h", 7, 18), "normalized: header constant 'kPrec' from " + hdr)], got
    cxx = CP.cxx_print(prog)
    assert "kUnused" not in cxx and "kArr" not in cxx
    assert cli.check_roundtrip(prog, "clang++") == []
    # OFF: without --project-root a header constant is a library global
    p = project(tmp_path)
    prog, errs = cli.load_cpp(str(p), "clang++", True, [], None)
    assert prog is None and "unsupported: global variable" in [m for _, m in errs]


@needs_clang
def test_header_constant_collision_and_rejected_header_constant(tmp_path):
    hdr = "#pragma once\nnamespace n {\nstatic const int kA = 1;\n}\nstatic const int kBad = sizeof(int);\n"
    main = ('#include <cstdio>\n#include "inc/consts.h"\nnamespace m {\nconst int kA = 2;\n'
            'int f() {\n    return kA + n::kA;\n}\n}\nint g() {\n    return kBad;\n}\n'
            'int main() {\n    std::printf("%d %d\\n", m::f(), g());\n    return 0;\n}\n')
    prog, errs = cli.load_cpp(str(project(tmp_path, main, hdr)), "clang++", True, [], str(tmp_path))
    assert prog is None
    got = [(where(l), m) for l, m in errs]
    # n::kA does NOT collide with m::kA (different paths); kBad's initializer is
    # refused in the header AND at its main-file use, so the caller is not clean
    assert (("main", 10, 12), "unsupported: use of a rejected header constant 'kBad'") in got, got
    assert any(w[0] == "consts.h" and w[1] == 5 for w, _ in got), got
    assert not any("collides" in m for _, m in got), got
    # a TRUE collision needs an anonymous namespace (same path after N14);
    # binding by path would silently read the main file's value
    hdr2 = "#pragma once\nnamespace {\nconst int kA = 1;\n}\ninline int H() {\n    return kA;\n}\n"
    main2 = ('#include <cstdio>\n#include "inc/consts.h"\nconst int kA = 2;\n'
             'int main() {\n    std::printf("%d %d\\n", H(), ::kA);\n    return 0;\n}\n')
    prog, errs = cli.load_cpp(str(project(tmp_path, main2, hdr2)), "clang++", True, [], str(tmp_path))
    assert prog is None
    assert [(where(l), m) for l, m in errs] == [
        (("main", 5, 28), "unsupported: call of a rejected header function 'H'"),
        (("consts.h", 3, 11), "unsupported: normalize: header constant 'kA' collides with a main-file name"),
        (("consts.h", 6, 12), "unsupported: global variable")], errs


# ------------------------------------------------------------------ census

def _census():
    spec = importlib.util.spec_from_file_location("bx_census_fr264", os.path.join(REPO, "scripts", "bx-census.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_census_classifies_the_new_wordings():
    c = _census()
    cases = {
        "unsupported: constant of non-kernel type 'const std::uint8_t[2]'": "constant of non-kernel type",
        "unsupported: constant initializer is not a constant expression": "constant: non-constant initializer",
        "unsupported: constant 'k0' used before its definition (C++ defines a constant before its use)":
            "constant: used before definition",
        "unsupported: constant 'kA' defined twice": "constant: name collision",
        "unsupported: constant 'f' has the name of a function": "constant: name collision",
        "unsupported: constant 'a' has the name of a namespace declared in the same scope": "constant: name collision",
        "unsupported: constant 'S' has the name of a struct": "constant: name collision",
        "unsupported: variable 'kA' has the name of a constant": "typing: variable named like a constant",
        "unsupported: normalize: header constant 'kA' collides with a main-file name":
            "normalize: header constant collision",
        "unsupported: use of a rejected header constant 'kBad'": "normalize: header constant rejected",
        "unsupported: use of undeclared constant 'kX'": "typing: use of undeclared constant (cascade)",
    }
    for msg, tag in cases.items():
        got = c.classify(msg)
        assert got[0] == tag and got[2], (msg, got)
    assert c.classify("unsupported: global variable")[0] == "global variable"


# ------------------------------------------------------------------ behaviour

def _run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


@needs_clang
@needs_rustc
def test_original_printed_cpp_and_rust_print_the_same_bytes(tmp_path):
    src = WRAP.replace("int main() {\n", "const int kLoop = kN % 7;\nint f(int x) {\n    int s = 0;\n"
                       "    for (int i = 0; i < kLoop; ++i) {\n"
                       "        s += x * i + kPrec;\n    }\n    return s;\n}\nint main() {\n"
                       "    std::printf(\"%d\\n\", f(kTern));\n")
    p = tmp_path / "w.cpp"
    p.write_text(src)
    prog, errs = cli.load_cpp(str(p), "clang++")
    assert errs == [], errs
    _run(["clang++", "-std=c++17", "-w", str(p), "-o", str(tmp_path / "orig")])
    want = _run([str(tmp_path / "orig")])
    cpp = tmp_path / "out.cpp"
    cpp.write_text(CP.cxx_print(prog))
    _run(["clang++", "-std=c++17", "-w", str(cpp), "-o", str(tmp_path / "printed")])
    assert _run([str(tmp_path / "printed")]) == want
    rs = tmp_path / "out.rs"
    rs.write_text(RSP.rs_print(prog))
    _run(["rustc", "-O", "--edition", "2021", str(rs), "-o", str(tmp_path / "rs")])
    assert _run([str(tmp_path / "rs")]) == want
