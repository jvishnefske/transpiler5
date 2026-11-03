# Pins FR-254 normalizer rule R4, header-inline (opt-in `--normalize` PLUS
# `--project-root DIR`): a main-file function calling an `inline` /
# `constexpr` / `static` free function DEFINED in a project header (a header
# under DIR) no longer refuses "call to library function"; the header
# definition is imported into ONE unity translation unit. The invariants,
# and why each is load-bearing:
#  * DEFAULT OFF is byte-identical: without --normalize, and with --normalize
#    but no --project-root, every such call keeps "call to library function
#    'NAME'" (the census's libfn needle and every Reject golden key on it);
#    --project-root alone is a usage error, never silently ignored;
#  * the import is LAZY by reachability and TRANSITIVE: exactly the header
#    definitions reached by calls from the main file, through imported ones
#    (the FR-262 spike measured an eager import regressing a TU); a template,
#    a member, a non-inline extern definition and anything outside the root
#    stay library calls;
#  * an imported function sits right before the first main-file function
#    that reaches it, callees first, in its own namespace's `mod`, so the
#    unity C++ needs no prototype and obeys laws 1-2 (cli.check_roundtrip);
#  * an imported function is judged by the ORDINARY rules and its residuals
#    are LOCATED IN THE HEADER (kernel.Loc.file); every call of a header
#    function with a residual (its own, or transitively a callee's) is a
#    located refusal AT THE CALL SITE, because a per-function census
#    (bx-census / bx-frontier) only attributes main-file lines and would
#    otherwise count the caller clean;
#  * a reachable header function that cannot be imported under its name --
#    two reachable overloads, or a name a main-file function has -- is a
#    located refusal and its calls refuse too: binding the call by NAME
#    would silently call the other overload;
#  * the census sees only main-file functions (the frontier denominator does
#    not grow) and classifies the new wordings;
#  * the OUTPUT behaves like the ORIGINAL two-file program: clang++ of the
#    original, of the unity C++ and the emitted Rust print the same bytes.
import importlib.util
import os
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import rs_print as RSP

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
NOTE = "normalized: header-inline '%s' from %s"

HEADER = """#pragma once
#include <cstddef>
#include <cstdint>
inline std::size_t Put16(std::uint8_t* dst, std::size_t off, int x) {
    dst[off] = static_cast<std::uint8_t>(x >> 8);
    dst[off + 1] = static_cast<std::uint8_t>(x);
    return off + 2;
}
static int Sq(int a) {
    return a * a;
}
inline int SumSq(int a, int b) {
    return Sq(a) + Sq(b);
}
inline int Unused(int a) {
    return a + 1;
}
template <typename T>
inline T Id(T v) {
    return v;
}
namespace codec {
inline int Shift(int v) {
    return v << 2;
}
}  // namespace codec
"""

MAIN = """#include <cstdio>
#include <vector>
#include "inc/h.h"
namespace codec {
int Encode(int v) {
    return Shift(v) + 1;
}
}  // namespace codec
int main() {
    std::vector<std::uint8_t> b(4, 0);
    std::size_t o = Put16(b.data(), 0, 0x1234);
    std::printf("%zu %d %d\\n", o, b[0], b[1]);
    std::printf("%d %d\\n", SumSq(3, 4), codec::Encode(5));
    return 0;
}
"""


def project(tmp_path, main=MAIN, header=HEADER, hname="h.h"):
    (tmp_path / "inc").mkdir(exist_ok=True)
    (tmp_path / "inc" / hname).write_text(header)
    p = tmp_path / "main.cpp"
    p.write_text(main)
    return p


def load(p, root, normalize=True):
    notes = []
    prog, es = cli.load_cpp(str(p), "clang++", normalize, notes, root)
    return prog, es, notes


def where(loc):
    return (os.path.basename(loc.file) if loc.file else "main", loc.line, loc.col)


@needs_clang
def test_default_off_and_no_root_keep_the_library_refusal(tmp_path):
    p = project(tmp_path)
    for normalize, root in ((False, None), (True, None)):
        prog, es, notes = load(p, root, normalize)
        assert prog is None
        msgs = sorted(m for _, m in es)
        assert msgs == ["unsupported: call to library function 'Put16'",
                        "unsupported: call to library function 'Shift'",
                        "unsupported: call to library function 'SumSq'"], msgs
        assert all(loc.file is None for loc, _ in es)
        assert notes == []


@needs_clang
def test_project_root_needs_normalize(tmp_path, capsys):
    p = project(tmp_path)
    assert cli.main(["--project-root=%s" % tmp_path, str(p)]) == 1
    assert "error: --project-root needs --normalize" in capsys.readouterr().err


@needs_clang
def test_lazy_transitive_import_in_first_user_order(tmp_path):
    p = project(tmp_path)
    prog, es, notes = load(p, str(tmp_path))
    assert es == [], es
    assert [f.path for f in prog.funcs()] == ["codec::Shift", "codec::Encode", "Put16", "Sq", "SumSq", "main"]
    hdr = str(tmp_path / "inc" / "h.h")
    got = sorted((where(loc), m) for loc, m in notes)
    assert got == [(("h.h", 4, 20), NOTE % ("Put16", hdr)), (("h.h", 9, 12), NOTE % ("Sq", hdr)),
                   (("h.h", 12, 12), NOTE % ("SumSq", hdr)), (("h.h", 23, 12), NOTE % ("Shift", hdr))], got
    assert cli.check_roundtrip(prog, "clang++") == []
    cxx = CP.cxx_print(prog)
    assert "Unused" not in cxx and "Id" not in cxx and '#include "' not in cxx
    assert "unsafe" not in RSP.rs_print(prog)
    # imported functions carry header locations; main-file ones do not
    locs = {f.path: f.loc for f in prog.funcs()}
    assert locs["Put16"].file == hdr and locs["main"].file is None


@needs_clang
def test_header_outside_the_root_and_templates_stay_library(tmp_path):
    p = project(tmp_path)
    (tmp_path / "other").mkdir()
    prog, es, notes = load(p, str(tmp_path / "other"))
    assert prog is None and notes == []
    assert {m for _, m in es} == {"unsupported: call to library function '%s'" % n for n in ("Put16", "Shift", "SumSq")}
    main = MAIN.replace("SumSq(3, 4)", "Id(3)")
    prog, es, notes = load(project(tmp_path, main), str(tmp_path))
    assert [m for _, m in es] == ["unsupported: call to library function 'Id'"], es


@needs_clang
def test_non_inline_header_definition_is_not_eligible(tmp_path):
    hdr = "int Plain(int a) {\n    return a + 1;\n}\n"
    main = '#include <cstdio>\n#include "inc/h.h"\nint main() {\n    std::printf("%d\\n", Plain(1));\n    return 0;\n}\n'
    prog, es, notes = load(project(tmp_path, main, hdr), str(tmp_path))
    assert [(where(loc), m) for loc, m in es] == [(("main", 4, 25), "unsupported: call to library function 'Plain'")]


REJ_H = """#pragma once
inline int Counter() {
    static int n = 0;
    n = n + 1;
    return n;
}
inline int Inner(int v) {
    int t[2] = {v, v};
    return t[0];
}
inline int Outer(int v) {
    return Inner(v) + 1;
}
inline int Store16(int v) {
    return v;
}
inline int Store16(int v, int w) {
    return v + w;
}
inline int Scale(int v) {
    return v * 3;
}
constexpr int Twice(int v) {
    return 2 * v;
}
inline int Fine(int v) {
    return v - 1;
}
"""

REJ_MAIN = """#include <cstdio>
#include "inc/h.h"
double Scale(double v) {
    return v * 2.0;
}
int A() {
    return Counter();
}
int B() {
    return Outer(1);
}
int C() {
    return Store16(1) + Store16(1, 2);
}
int D() {
    return Scale(2);
}
int E() {
    return Twice(3);
}
int F() {
    return Fine(4);
}
int main() {
    std::printf("%d %d %d %d %d %d %f\\n", A(), B(), C(), D(), E(), F(), Scale(0.5));
    return 0;
}
"""

CALL = "unsupported: call of a rejected header function '%s'"
OVERLOAD = ("unsupported: header function 'Store16' is overloaded (normalize header-inline: the subset has one "
            "function per name)")
COLLIDE = ("unsupported: header function 'Scale' has the same name as a function of the main file (normalize "
           "header-inline: the subset has one function per name)")


@needs_clang
def test_refusals_located_in_the_header_and_at_every_call(tmp_path):
    prog, es, notes = load(project(tmp_path, REJ_MAIN, REJ_H), str(tmp_path))
    assert prog is None
    got = [(where(loc), m) for loc, m in es]
    assert got == [
        (("main", 7, 12), CALL % "Counter"),
        (("main", 10, 12), CALL % "Outer"),
        (("main", 13, 12), CALL % "Store16"),
        (("main", 13, 25), CALL % "Store16"),
        (("main", 16, 12), CALL % "Scale"),
        (("main", 19, 12), CALL % "Twice"),
        (("h.h", 3, 5), "unsupported: static local variable"),
        (("h.h", 4, 5), "unsupported: assignment to something other than a local variable"),
        (("h.h", 8, 5), "unsupported: variable of type 'int[2]'"),
        (("h.h", 9, 12), "unsupported: array subscript"),
        (("h.h", 12, 12), CALL % "Inner"),  # transitive: Outer's own call site, in the header
        (("h.h", 14, 12), OVERLOAD),
        (("h.h", 17, 12), OVERLOAD),
        (("h.h", 20, 12), COLLIDE),
        (("h.h", 23, 15), "unsupported: constexpr function"),
    ], got
    # `F` calls the clean `Fine`: no error at line 22
    assert not any(loc.file is None and loc.line == 22 for loc, _ in es)


@needs_clang
def test_collision_never_binds_the_call_to_the_main_file_function(tmp_path):
    # same signature shape as the main-file overload would be the dangerous case:
    # `Scale(2)` resolves to the HEADER's int overload; a by-name binding would
    # call the main file's double one and print 4.0 instead of 6
    hdr = "inline int Scale(int v) {\n    return v * 3;\n}\n"
    main = ('#include <cstdio>\n#include "inc/h.h"\nint Scale(int v, int w) {\n    return v * w;\n}\n'
            'int main() {\n    std::printf("%d\\n", Scale(2));\n    return 0;\n}\n')
    prog, es, _ = load(project(tmp_path, main, hdr), str(tmp_path))
    assert prog is None
    assert [(where(loc), m) for loc, m in es] == [(("main", 7, 25), CALL % "Scale"),
                                                  (("h.h", 1, 12), COLLIDE)], es


@needs_clang
def test_a_redeclaration_in_the_main_file_is_not_a_collision(tmp_path):
    hdr = "inline int Twice(int v) {\n    return v * 2;\n}\n"
    main = ('#include <cstdio>\n#include "inc/h.h"\ninline int Twice(int v);\n'
            'int main() {\n    std::printf("%d\\n", Twice(2));\n    return 0;\n}\n')
    prog, es, notes = load(project(tmp_path, main, hdr), str(tmp_path))
    assert not any("same name" in m for _, m in es), es


@needs_clang
def test_rust_keyword_name_in_the_header_cascades_to_the_caller(tmp_path):
    hdr = "inline int Ok(int v) {\n    return v;\n}\n"
    main = '#include <cstdio>\n#include "inc/h.h"\nint main() {\n    std::printf("%d\\n", Ok(2));\n    return 0;\n}\n'
    prog, es, _ = load(project(tmp_path, main, hdr), str(tmp_path))
    assert [(where(loc), m) for loc, m in es] == [
        (("main", 4, 25), CALL % "Ok"), (("h.h", 1, 12), "unsupported: identifier 'Ok' is a Rust keyword")], es


def _census():
    spec = importlib.util.spec_from_file_location("bx_census_r4", os.path.join(REPO, "scripts", "bx-census.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_census_classifies_the_new_wordings():
    c = _census()
    assert c.classify(CALL % "Store16") == ("normalize: header-inline callee rejected", "libfn:Store16", True)
    assert c.classify(OVERLOAD) == ("normalize: header-inline overload", None, True)
    assert c.classify(COLLIDE) == ("normalize: header-inline name collision", None, True)


@needs_clang
def test_census_counts_only_main_file_functions_and_callers_of_rejected_ones(tmp_path, monkeypatch):
    c = _census()
    p = project(tmp_path, REJ_MAIN, REJ_H)
    bx = os.path.join(REPO, "tools", "emitrust-bx", "emitrust-bx.py")
    monkeypatch.setenv("BX_EXTRA_ARGS", "--normalize --project-root=%s" % tmp_path)
    rec = c.analyze((str(p), bx, "clang++", 120))
    assert rec["status"] == "BLOCKED", rec
    fns = {f["name"]: f for f in rec["functions"]}
    assert sorted(fns) == ["A", "B", "C", "D", "E", "F", "Scale", "main"]  # no header function
    clean = sorted(n for n, f in fns.items() if not f["diags"] and not f["container_diags"])
    assert clean == ["F", "Scale", "main"], clean
    tags = {rec["diags"][i]["tag"] for i in fns["B"]["diags"]}
    assert tags == {"normalize: header-inline callee rejected"}


def _run_native(tmp_path, name, srcs):
    exe = tmp_path / name
    subprocess.run(["clang++", "-std=c++17", "-O1"] + [str(s) for s in srcs] + ["-o", str(exe)], check=True,
                   capture_output=True, cwd=str(tmp_path))
    return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


@needs_clang
@needs_rustc
def test_original_unity_cpp_and_rust_print_the_same_bytes(tmp_path):
    hdr = HEADER + """inline std::uint32_t Mix(std::uint32_t h, std::uint32_t v) {
    for (; v != 0; v >>= 3) {
        h = h * 31u + (v & 7u);
    }
    return h;
}
"""
    main = """#include <cstdio>
#include <vector>
#include "inc/h.h"
int next(int x) {
    return (x * 1103 + 12345) % 30011;
}
int main() {
    std::vector<std::uint8_t> b(64, 0);
    int s = 5;
    std::size_t o = 0;
    std::uint32_t h = 1u;
    for (int i = 0; i < 20; ++i) {
        s = next(s);
        o = Put16(b.data(), o, s + SumSq(i, s % 7));
        h = Mix(h, static_cast<std::uint32_t>(s));
    }
    for (int i = 0; i < 40; ++i) {
        std::printf("%d ", b[i]);
    }
    std::printf("\\n%zu %u %d\\n", o, h, codec::Shift(s));
    return 0;
}
"""
    p = project(tmp_path, main, hdr)
    prog, es, notes = load(p, str(tmp_path))
    assert es == [], es
    assert any(m == "normalized: for->while" and loc.file for loc, m in notes)  # R1 inside the header body
    assert cli.check_roundtrip(prog, "clang++") == []
    want = _run_native(tmp_path, "orig", [p])
    assert want.count(" ") >= 40
    unity = tmp_path / "unity.cpp"
    unity.write_text(CP.cxx_print(prog))
    assert _run_native(tmp_path, "unity", [unity]) == want
    rs = tmp_path / "norm.rs"
    rs.write_text(RSP.rs_print(prog))
    assert "unsafe" not in rs.read_text()
    exe = tmp_path / "norm_rs"
    subprocess.run(["rustc", "-O", "--edition", "2021", "-A", "warnings", str(rs), "-o", str(exe)], check=True,
                   capture_output=True)
    assert subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout == want


def test_loc_file_is_not_part_of_kernel_equality():
    a = K.Var("x", K.Loc(1, 2))
    b = K.Var("x", K.Loc(1, 2, "h.h"))
    assert a == b and K.Loc(1, 2) != K.Loc(1, 2, "h.h") and str(K.Loc(1, 2, "h.h")) == "1:2"
