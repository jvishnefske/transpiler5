# Pins FR-254 normalizer rule R3 (opt-in `--normalize`): a POSTFIX
# increment/decrement used as a value -- `dst[(*off)++] = v`, `*(p++) = v`,
# `return d[(*pos)++];` -- is rewritten to "read the old value in place, then
# a separate `++x;`" (a `return` goes through a fresh `const T bx_ret`). The
# invariants, and why each is load-bearing:
#  * DEFAULT OFF is byte-identical: without --normalize every such statement
#    keeps "increment/decrement used as a value" at the `++` (the Reject
#    goldens and the census needle key on it);
#  * R3 fires ONLY where the subset refuses: `a[i++] = v` on a local integer
#    is already a Kernel IncDec and stays one (no golden moves); a prefix
#    `++x` used as a value, a statement with two increments and an increment
#    in a condition keep the importer's own refusal and wording;
#  * the rewrite moves the side effect to the END of the statement, so it is
#    sound only when nothing in the statement can observe the difference;
#    each guard is a LOCATED refusal at the `++`, because each naive rewrite
#    is observable: another mention of the variable (FR-259's own rule and
#    wording), a closure call (a by-reference capture reaches the variable
#    without naming it), a site under `&&` / `||` / `?:` / `,` (it might not
#    run), an operand that is not `x` / `*p`, a non-scalar `return`;
#  * rules compose: R3 applies inside R1/R2-rewritten loops, including in a
#    `for` STEP that R1 turned into a body statement;
#  * the fresh `return` temporary cannot capture or collide: a name the file
#    already spells is skipped, and two rewritten returns get two names;
#  * the OUTPUT obeys the bijection laws (cli.check_roundtrip) and contains
#    no `unsafe`;
#  * DOUBLE VALIDATION (FR-254's contract for every rule): a seeded generator
#    of R3 shapes (index post-increment through a pointer parameter and a
#    pointer local, post-decrement, a pointer post-increment store, a
#    `return` of a post-increment read, a call on the right-hand side,
#    compound assignment, a declaration, all also inside R1/R2-rewritten
#    loops) compiled natively BEFORE and AFTER normalization and as the
#    emitted Rust, stdout byte-identical across all three. UB-free by
#    construction: every index stays inside a 96-element buffer.
import random
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import normalize as NZ
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
HEAD = "#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n#include <string>\n#include <vector>\n"
USED = "unsupported: increment/decrement used as a value"
NOTE = "normalized: post-incdec"


def imp(tmp_path, src, normalize=True):
    p = tmp_path / "t.cpp"
    p.write_text(HEAD + src)
    notes = []
    prog, es = cli.load_cpp(str(p), "clang++", normalize, notes)
    n = HEAD.count("\n")
    return prog, [(loc.line - n, loc.col, m) for loc, m in es], [(loc.line - n, loc.col, m) for loc, m in notes]


def ok(tmp_path, src):
    prog, es, notes = imp(tmp_path, src)
    assert es == [], es
    assert cli.check_roundtrip(prog, "clang++") == []
    assert "unsafe" not in RSP.rs_print(prog)
    return prog, notes


def cxx_lines(prog):
    return [ln.strip() for ln in CP.cxx_print(prog).splitlines()]


def rs_lines(prog):
    return [ln.strip() for ln in RSP.rs_print(prog).splitlines()]


# the four measured woff2/brunsli shapes (own reproductions)
SHAPES = """int ValueMarker(int tag) {
    return tag * 8 + 2;
}

void Pack(uint8_t* dst, size_t* offset, const std::vector<int>& v) {
    for (int x : v) {
        uint8_t packed_byte = static_cast<uint8_t>(x & 0xff);
        dst[(*offset)++] = packed_byte;
    }
}

void Marks(int* data, size_t* pos, int tag) {
    data[(*pos)++] = ValueMarker(tag);
}

void Store(uint8_t* data, int val, int len) {
    for (int i = 0; i < len; ++i) {
        *(data++) = (val & 0x7f) | (i + 1 < len ? 0x80 : 0);
        val >>= 7;
    }
}

int Read(const uint8_t* data, size_t* pos) {
    return data[(*pos)++];
}
"""


# ------------------------------------------------------------- default off
@needs_clang
def test_default_off_keeps_every_refusal(tmp_path):
    _, es, notes = imp(tmp_path, SHAPES, normalize=False)
    assert notes == []
    assert es == [(8, 13, USED), (13, 10, USED), (18, 11, USED), (24, 17, USED)]


@needs_clang
def test_four_shapes_normalize_with_located_notes(tmp_path):
    prog, notes = ok(tmp_path, SHAPES)
    assert notes == [(8, 13, NOTE), (13, 10, NOTE), (18, 11, NOTE), (24, 17, NOTE)]
    lines = cxx_lines(prog)
    for want in (["dst[*offset] = packed_byte;", "++*offset;"],
                 ["data[*pos] = ValueMarker(tag);", "++*pos;"],
                 ["*data = static_cast<unsigned char>((val & 127) | ((i + 1) < len ? 128 : 0));", "++data;"],
                 ["const int bx_ret = static_cast<int>(data[*pos]);", "++*pos;", "return bx_ret;"]):
        i = lines.index(want[0])
        assert lines[i:i + len(want)] == want
    rs = rs_lines(prog)
    assert "data = &mut data[1usize..];" in rs  # FR-261's forward re-slice


@needs_clang
def test_native_local_incdec_is_not_rewritten(tmp_path):
    prog, notes = ok(tmp_path, "int f(std::vector<int>& d, int n) {\n    int i = 0;\n    d[i++] = n;\n    return i;\n}\n")
    assert notes == []
    assert any(isinstance(x, K.IncDec) for x in K.walk(prog))


@needs_clang
def test_compound_declaration_call_argument_and_post_decrement(tmp_path):
    prog, notes = ok(tmp_path, "int f(const int* d, size_t* p, int* out) {\n    int acc = 0;\n"
                               "    acc += d[(*p)++];\n    int b = d[(*p)--];\n    out[0] = b;\n"
                               "    std::printf(\"%d\\n\", d[(*p)++]);\n    return acc;\n}\n")
    assert notes == [(3, 14, NOTE), (4, 15, NOTE), (6, 27, NOTE)]
    lines = cxx_lines(prog)
    i = lines.index("acc += d[*p];")
    assert lines[i:i + 7] == ["acc += d[*p];", "++*p;", "int b = d[*p];", "--*p;", "*out = b;",
                              "std::printf(\"%d\\n\", d[*p]);", "++*p;"]


@needs_clang
def test_pointer_local_and_other_pointer_reads(tmp_path):
    # `q` is a pointer LOCAL; reading another pointer `e` in the statement is fine
    prog, notes = ok(tmp_path, "void f(int* d, const int* e, size_t n) {\n    int* q = d;\n"
                               "    for (size_t i = 0; i < n; ++i) {\n        *(q++) = e[i] + 1;\n    }\n}\n")
    assert notes == [(4, 11, NOTE)]
    assert ["*q = e[i] + 1;", "++q;"] == cxx_lines(prog)[cxx_lines(prog).index("*q = e[i] + 1;"):][:2]


# ------------------------------------------------------------- fresh name
@needs_clang
def test_fresh_return_temporary_skips_spelled_names(tmp_path):
    prog, _ = ok(tmp_path, "int g(int bx_ret) {\n    return bx_ret;\n}\n\n"
                           "int f(const int* d, size_t* p) {\n    if (*p > 3) {\n        return d[(*p)++];\n    }\n"
                           "    return d[(*p)++] + 1;\n}\n")
    lines = cxx_lines(prog)
    assert "const int bx_ret1 = d[*p];" in lines and "return bx_ret1;" in lines
    assert "const int bx_ret2 = d[*p] + 1;" in lines and "return bx_ret2;" in lines


# ------------------------------------------------------------- composition
@needs_clang
def test_r3_inside_r1_body_and_step_and_r2(tmp_path):
    prog, notes = ok(tmp_path, "void f(int* d, size_t* p, int n) {\n"
                               "    for (int i = 0; i < n; i += 2) {\n        d[(*p)++] = i;\n    }\n"
                               "    for (int i = n; i > 0; d[(*p)++] = i) {\n        i -= 3;\n    }\n"
                               "    int k = n;\n    do {\n        d[(*p)++] = k;\n        k -= 5;\n"
                               "    } while (k > 0);\n}\n")
    assert notes == [(2, 5, "normalized: for->while"), (3, 11, NOTE), (5, 5, "normalized: for->while"),
                     (5, 30, NOTE), (9, 5, "normalized: do-while"), (10, 11, NOTE)]
    assert not any(isinstance(x, K.IncDec) for x in K.walk(prog))


# ------------------------------------------------------------- stays refused (existing wording)
@needs_clang
@pytest.mark.parametrize("stmt,col", [
    ("d[++(*p)] = 1;", 7),              # prefix used as a value
    ("d[(*p)++] = d[(*p)++];", 7),      # two increments
    ("int i = 0;\n    d[(*p)++] = i++;", 7),  # one refused, one native: still two
])
def test_prefix_and_two_increments_keep_the_importer_refusal(tmp_path, stmt, col):
    src = "void f(int* d, size_t* p) {\n    %s\n}\n" % stmt
    _, es, notes = imp(tmp_path, src)
    assert notes == []
    off = stmt.count("\n")
    assert es and es[0][2] == USED and es[0][0] == 2 + off


@needs_clang
def test_refused_local_keeps_the_importer_wording(tmp_path):
    # `int k;` is refused at its declaration; a `k++` on it is not R3's (measured on
    # brunsli's BuildHuffmanTable: the R3 operand wording there misnamed the cause)
    _, es, notes = imp(tmp_path, "void f(int* d) {\n    int k;\n    k = 0;\n    d[k++] = 1;\n}\n")
    assert notes == []
    assert (2, 5, "unsupported: declaration without an initializer") in es
    assert (4, 7, USED) in es
    assert not any("post-incdec" in m for _, _, m in es)


@needs_clang
def test_static_declaration_is_not_rewritten(tmp_path):
    # a `static` initializer runs once; a trailing `++*p;` would run every call
    _, es, notes = imp(tmp_path, "int f(const int* d, size_t* p) {\n    static int b = d[(*p)++];\n"
                                 "    return b;\n}\n")
    assert notes == []
    assert (2, 5, "unsupported: static local variable") in es


@needs_clang
def test_increment_in_a_condition_keeps_the_importer_refusal(tmp_path):
    _, es, notes = imp(tmp_path, "int f(const int* d, size_t* p) {\n    if (d[(*p)++] > 0) {\n        return 1;\n"
                                 "    }\n    while (d[(*p)++] != 0) {\n    }\n    return 0;\n}\n")
    assert notes == []
    assert es == [(2, 11, USED), (5, 14, USED)]


# ------------------------------------------------------------- guards
def guard(src):
    return "void f(int* d, size_t* p, int n) {\n%s\n}\n" % src


@needs_clang
@pytest.mark.parametrize("body,col,why", [
    ("    d[(*p)++] = static_cast<int>(*p);", 7, TC.INCDEC_ALSO_USES % ("p", "p")),
    ("    d[(*p)++ + p[1]] = n;", 7, TC.INCDEC_ALSO_USES % ("p", "p")),
    ("    *(d++) = d[0];", 7, TC.INCDEC_ALSO_USES % ("d", "d")),
    ("    n = n > 0 ? d[(*p)++] : 0;", 19, NZ.INC_COND),
    ("    n = n > 0 && d[(*p)++] > 0;", 20, NZ.INC_COND),
    ("    n = d[(*p)++] > 0 || n > 0;", 11, NZ.INC_COND),
    ("    int k = 1;\n    auto bump = [&k]() {\n        k += 1;\n        return k;\n    };\n"
     "    d[(*p)++] = bump();", 7, NZ.INC_CLOSURE % ("p", "p")),
    ("    std::vector<int> v = {1, 2};\n    n = v[1]++;", 9, NZ.INC_OPERAND),
    ("    n = d[0]++;", 9, NZ.INC_OPERAND),
])
def test_guard_refusals_are_located_at_the_increment(tmp_path, body, col, why):
    _, es, notes = imp(tmp_path, guard(body))
    assert notes == []
    line = 2 + body.count("\n")
    assert es == [(line, col, "unsupported: " + why)]


@needs_clang
def test_closure_guard_is_load_bearing_on_a_double_local(tmp_path):
    # `x++` on a double local is not a Kernel IncDec, so R3 matches; `peek`
    # reads `x` by reference without the statement naming it. Measured with
    # the guard disabled: clang++ and g++ -O1 print 203.0, the rewrite 103.0.
    src = ("double f(double x) {\n    auto peek = [&x]() {\n        return x * 100.0;\n    };\n"
           "    double s = x++ + peek();\n    return s + x;\n}\n")
    _, es, notes = imp(tmp_path, src)
    assert notes == []
    assert es == [(5, 16, "unsupported: " + NZ.INC_CLOSURE % ("x", "x"))]
    prog, notes = ok(tmp_path, src.replace(" + peek()", " + 1.0"))
    assert notes == [(5, 16, NOTE)]
    lines = cxx_lines(prog)
    i = lines.index("double s = x + 1.0;")
    assert lines[i + 1] == "x += 1.0;"  # the canonical spelling of `++x` on a double


@needs_clang
def test_non_scalar_return_is_refused(tmp_path):
    _, es, notes = imp(tmp_path, "std::string f(const std::vector<std::string>& names, size_t* p) {\n"
                                 "    return names[(*p)++];\n}\n")
    assert notes == []
    assert es == [(2, 18, "unsupported: " + NZ.INC_RETURN % "std::string")]


@needs_clang
def test_vector_index_through_a_pointer_increment(tmp_path):
    prog, notes = ok(tmp_path, "int f(const std::vector<int>& v, size_t* p) {\n    int n = v[(*p)++];\n"
                               "    return n;\n}\n")
    assert notes == [(2, 15, NOTE)]
    assert "++*p;" in cxx_lines(prog)


def test_indirect_call_sees_closures_lambdas_and_pointer_reads():
    fn = {"kind": "CallExpr", "inner": [{"kind": "ImplicitCastExpr", "castKind": "FunctionToPointerDecay",
                                         "inner": [{"kind": "DeclRefExpr",
                                                    "referencedDecl": {"kind": "FunctionDecl", "name": "g"}}]}]}
    assert NZ.indirect_call(fn) is None
    viaptr = {"kind": "CallExpr", "inner": [{"kind": "ImplicitCastExpr", "castKind": "LValueToRValue",
                                             "inner": [{"kind": "DeclRefExpr",
                                                        "referencedDecl": {"kind": "ParmVarDecl", "name": "g"}}]}]}
    assert NZ.indirect_call(viaptr) is viaptr
    clo = {"kind": "CXXOperatorCallExpr", "inner": [
        {"kind": "ImplicitCastExpr", "inner": [{"kind": "DeclRefExpr",
                                                "referencedDecl": {"kind": "CXXMethodDecl", "name": "operator()"}}]}]}
    assert NZ.indirect_call({"kind": "BinaryOperator", "inner": [clo]}) is clo
    lam = {"kind": "LambdaExpr"}
    assert NZ.indirect_call({"kind": "ParenExpr", "inner": [lam]}) is lam


def test_incdec_sites_mark_conditional_operands_and_skip_lambdas():
    u = {"kind": "UnaryOperator", "opcode": "++", "isPostfix": True, "inner": [{"kind": "DeclRefExpr"}]}
    for k, op in (("BinaryOperator", "&&"), ("BinaryOperator", "||"), ("BinaryOperator", ","),
                  ("ConditionalOperator", None)):
        assert NZ.incdec_sites({"kind": k, "opcode": op, "inner": [u]}) == [(u, True)]
    assert NZ.incdec_sites({"kind": "BinaryOperator", "opcode": "+", "inner": [u]}) == [(u, False)]
    assert NZ.incdec_sites({"kind": "LambdaExpr", "inner": [u]}) == []


# ------------------------------------------------------------- double validation
def _shape(rng, k):
    """(helper function text, its driver call): one R3 shape. The helper
    writes at most 40 elements of the 96-element buffer starting at a
    position <= 8, so every index is in bounds; values stay small."""
    M = rng.randint(2, 9)
    A = rng.randint(0, 50)
    h = "h%d" % k
    shape = rng.randrange(12)
    if shape == 0:  # index post-increment through a pointer parameter, canonical loop
        body = ["for (int i = 0; i < n; ++i) {", "  d[(*p)++] = (i * %d + %d) %% 101;" % (M, A), "}"]
    elif shape == 1:  # ... through a pointer LOCAL
        body = ["size_t* q = p;", "for (int i = 0; i < n; ++i) {", "  d[(*q)++] = i * %d - %d;" % (M, A), "}"]
    elif shape == 2:  # post-decrement, a declaration
        body = ["*p += static_cast<size_t>(n);", "for (int i = 0; i < n; ++i) {", "  int v = d[(*p)--];",
                "  d[*p + 1] = v + i * %d;" % M, "}"]
    elif shape == 3:  # pointer post-increment store, R1 stride loop
        body = ["int* w = d + *p;", "for (int i = 0; i < n; i += %d) {" % rng.randint(2, 4),
                "  *(w++) = (i + %d) %% 97;" % A, "  *p += 1;", "}"]
    elif shape == 4:  # call on the right-hand side
        body = ["for (int i = 0; i < n; ++i) {", "  d[(*p)++] = mix(i, %d);" % M, "}"]
    elif shape == 5:  # compound assignment through the index
        body = ["for (int i = 0; i < n; ++i) {", "  d[(*p)++] += i * %d;" % M, "}"]
    elif shape == 6:  # R1 step containing the R3 site
        body = ["for (int i = n; i > 0; d[(*p)++] = i * %d) {" % M, "  i -= %d;" % rng.randint(1, 3), "}"]
    elif shape == 7:  # R2 do-while body
        body = ["int k = n;", "do {", "  d[(*p)++] = k + %d;" % A, "  k -= %d;" % rng.randint(1, 4),
                "} while (k > 0);"]
    elif shape == 8:  # return of a post-increment read, called in a loop
        body = ["int acc = %d;" % A, "for (int i = 0; i < n; ++i) {",
                "  acc = (acc * %d + rd(d, p)) %% 10007;" % M, "}", "d[0] = acc;"]
    elif shape == 9:  # read through a post-increment, R1 descending loop
        body = ["int acc = 0;", "for (int i = n; i != 0; --i) {", "  acc += d[(*p)++] * %d;" % M, "}",
                "d[1] = acc % 10007;"]
    elif shape == 10:  # a const pointer walk read, through a local
        body = ["int acc = %d;" % A, "{", "  const int* r = d + 8;", "  for (int i = 0; i < n; ++i) {",
                "    acc = (acc * %d + *(r++)) %% 10007;" % M, "  }", "}", "d[2] = acc;"]
    else:  # printf argument
        body = ["for (int i = 0; i < n && i < 3; ++i) {", "  std::printf(\"%d \", d[(*p)++]);", "}",
                "std::printf(\"\\n\");"]
    text = "void %s(int* d, size_t* p, int n) {\n%s\n}\n\n" % (h, "\n".join("    " + ln for ln in body))
    return text


def generate(seed, count):
    rng = random.Random(seed)
    out = [HEAD, "int mix(int a, int b) {\n    return (a * 31 + b) % 113;\n}\n\n",
           "int rd(const int* d, size_t* p) {\n    return d[(*p)++];\n}\n\n"]
    for k in range(count):
        out.append(_shape(rng, k))
    for k in range(count):
        out.append("int g%d(int n) {\n    std::vector<int> buf(96, 3);\n    for (size_t i = 0; i < 96; ++i) {\n"
                   "        buf[i] = static_cast<int>(i %% 7) + 1;\n    }\n"
                   "    size_t pos = static_cast<size_t>(n %% 9);\n    h%d(buf.data(), &pos, n);\n"
                   "    int acc = static_cast<int>(pos);\n    for (size_t i = 0; i < 96; ++i) {\n"
                   "        acc = (acc * 3 + buf[i]) %% 10007;\n    }\n    return acc;\n}\n\n" % (k, k))
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
def test_generated_incdec_byte_identical_before_and_after(tmp_path):
    count = 36
    text = generate(2543, count)
    orig = tmp_path / "orig.cpp"
    orig.write_text(text)
    _, es0 = cli.load_cpp(str(orig), "clang++")
    assert es0 and any(m == USED for _, m in es0)  # outside the subset
    notes = []
    prog, es = cli.load_cpp(str(orig), "clang++", True, notes)
    assert es == [], es
    sites = sum(text.count(x) for x in ("(*p)++", "(*p)--", "(*q)++", "(w++)", "(r++)"))
    assert sites >= count * 3 // 4
    assert sum(1 for _, m in notes if m == NOTE) == sites  # every site rewritten, none refused
    assert cli.check_roundtrip(prog, "clang++") == []
    want = _run_cxx(tmp_path, "orig", text)
    assert want.count("g0(") == 5
    assert _run_cxx(tmp_path, "norm", CP.cxx_print(prog)) == want
    rs = tmp_path / "norm.rs"
    rs.write_text(RSP.rs_print(prog))
    assert "unsafe" not in rs.read_text()
    exe = tmp_path / "norm_rs"
    subprocess.run(["rustc", "-O", "--edition", "2021", "-A", "warnings", str(rs), "-o", str(exe)], check=True,
                   capture_output=True)
    assert subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout == want


def test_generator_covers_every_shape():
    text = generate(2543, 36)
    for needle in ("d[(*p)++] = (i", "d[(*q)++]", "d[(*p)--]", "*(w++)", "mix(i,", "d[(*p)++] +=",
                   "; d[(*p)++] = i", "} while (k > 0);", "rd(d, p)", "i != 0; --i", "*(r++)", "std::printf(\"%d \""):
        assert needle in text, needle
