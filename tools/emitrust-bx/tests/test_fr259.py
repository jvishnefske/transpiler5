# Pins FR-259 (cheap syntactic breadth) at the unit level.  The invariants,
# and why each is load-bearing:
#  * the <cstdint>/<cstddef> names are SPELLINGS of a Kernel type in every
#    position -- parameters, returns, locals, reference pointees and template
#    arguments -- not just pointees (N20): clang prints no desugared type for
#    a reference's pointee or a function's return, which is why `uint32_t
#    f(const std::vector<uint8_t>&)` was refused twice.  A TU that declares
#    one of those names as something ELSE turns the resolution off: a guess
#    there would silently change a type;
#  * `inline` / `static` on a free function and `auto` on a declaration are
#    spellings too (N21, N22): one Kernel term per meaning, so laws 1-2 hold;
#    `static` inside a namespace is internal linkage, exactly the anonymous-
#    namespace member FR-260 already maps to a private `fn`;
#  * `?:` <=> `if c { a } else { b }`: both lazy, arms of ONE scalar type
#    (clang's usual arithmetic conversions are explicit Casts), never a
#    place -- an lvalue or class-typed `?:` is a located refusal;
#  * an increment used as a value <=> a canonical Rust block, ONLY when the
#    statement mentions the variable nowhere else: `i++ + i` is undefined and
#    `f(i++, i)` unspecified in C++ while Rust fixes the order, so accepting
#    them would make the byte-diff oracle depend on luck.  It counts as an
#    assignment for the counting-loop, pointer and range-for analyses, and an
#    unsigned compound store through an incrementing index (Rust re-evaluates
#    the index in `v[i] = v[i].wrapping_add(e)`) is refused -- that one would
#    be a SILENT double increment;
#  * i16 / u16 have no arithmetic of their own (C++ promotes them, the Kernel
#    casts), exactly like i8 / u8.
import collections
import os
import shutil
import subprocess

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_import as CI
from emitrust_bx import cxx_print as CP
from emitrust_bx import gen
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
needs_rustc = pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
HEAD = "#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n#include <string>\n#include <vector>\n"
NHEAD = HEAD.count("\n")
SEQ = ("unsupported: increment/decrement of '%s' used as a value in a statement that also uses '%s' "
       "(C++ leaves the order unsequenced or unspecified)")

V = K.Var


def imp(tmp_path, src, name="t.cpp", head=HEAD):
    p = tmp_path / name
    p.write_text(head + src)
    return cli.load_cpp(str(p), "clang++")


def ok(tmp_path, src, name="t.cpp"):
    prog, errs = imp(tmp_path, src, name)
    assert errs == [], errs
    return prog


def errs(tmp_path, src, head=HEAD):
    """(line relative to ``src``, col, message) of every located error."""
    _, es = imp(tmp_path, src, head=head)
    n = head.count("\n")
    return [(loc.line - n, loc.col, m) for loc, m in es]


def rs_errs(src):
    _, es = RP.rs_parse_all(src)
    return [("%s" % loc, m) for loc, m in es]


def body(prog, name):
    return list([f for f in prog.all_funcs() if f.name == name][0].body.stmts)


def laws(tmp_path, prog):
    """Laws 1-2 on both sides for one program."""
    rs = RSP.rs_print(prog)
    back = RP.rs_parse(rs)
    assert back == prog
    assert RSP.rs_print(back) == rs
    cxx = CP.cxx_print(prog)
    p = tmp_path / "laws.cpp"
    p.write_text(cxx)
    again, es = cli.load_cpp(str(p), "clang++")
    assert es == [], es
    assert again == prog
    assert CP.cxx_print(again) == cxx
    return rs, cxx


# ------------------------------------------------------------------ N20

def test_n20_typedef_spellings():
    P = CI.parse_cxx_type
    assert P("uint32_t") == (K.U32, False)
    assert P("const std::vector<uint8_t> &") == (K.RefT(K.VecT(K.U8), False), False)
    assert P("std::vector<std::vector<std::uint16_t>> &") == (K.RefT(K.VecT(K.VecT(K.U16)), True), False)
    assert P("int64_t") == (K.I64, False) and P("std::ptrdiff_t") == (K.I64, False)
    assert P("uint64_t") == (K.USIZE, False) and P("size_t") == (K.USIZE, False)
    assert P("int8_t") == (K.I8, False) and P("signed char") == (K.I8, False)
    assert P("int16_t") == (K.I16, False) and P("unsigned short") == (K.U16, False)
    # deduced types are spelled east-const by clang
    assert P("unsigned char const &") == (K.RefT(K.U8, False), False)
    # look-alikes are not typedefs, and the resolution can be turned off
    assert P("my_uint8_t")[0] is None and P("uint8_t_t")[0] is None and P("a::uint8_t")[0] is None
    assert P("uint8_t", typedefs=False)[0] is None


@needs_clang
def test_n20_every_position_is_one_term(tmp_path):
    a = ok(tmp_path, "uint32_t f(const std::vector<uint8_t>& v, int16_t a, std::uint16_t b, int64_t c,\n"
                     "           std::size_t n, int8_t d) {\n"
                     "    uint32_t s = 0;\n"
                     "    for (std::size_t i = 0; i < n; ++i) { s = s + v[i]; }\n"
                     "    std::vector<int32_t> w = {a, b};\n"
                     "    return s + static_cast<uint32_t>(c + d + w[1]);\n"
                     "}\n", "a.cpp")
    b = ok(tmp_path, "unsigned f(const std::vector<unsigned char>& v, short a, unsigned short b, long c,\n"
                     "           unsigned long n, signed char d) {\n"
                     "    unsigned s = 0;\n"
                     "    for (std::size_t i = 0; i < n; ++i) { s = s + v[i]; }\n"
                     "    std::vector<int> w = {a, b};\n"
                     "    return s + static_cast<unsigned>(c + d + w[1]);\n"
                     "}\n", "b.cpp")
    assert a == b
    f = a.funcs()[0]
    assert [p.ty for p in f.params] == [K.RefT(K.VecT(K.U8), False), K.I16, K.U16, K.I64, K.USIZE, K.I8]
    assert f.ret == K.U32
    _, cxx = laws(tmp_path, a)
    assert "unsigned f(const std::vector<unsigned char>& v, short a, unsigned short b, long long c, " \
           "std::size_t n, char d) {" in cxx


@needs_clang
def test_n20_shadowed_name_is_not_resolved(tmp_path):
    # a TU that makes `uint8_t` mean something else must not get the <cstdint> type
    src = ("namespace a {\n"
           "typedef int uint8_t;\n"
           "int f(const uint8_t& x) { return x; }\n"
           "}\n")
    e = errs(tmp_path, src, head="")
    assert (2, 1, "unsupported: typedef") in e
    assert (3, 22, "unsupported: parameter of type 'const uint8_t &'") in e


# ------------------------------------------------------------------ N21

@needs_clang
def test_n21_static_and_inline_free_functions(tmp_path):
    a = ok(tmp_path, "static int g(int x) { return x + 1; }\n"
                     "inline int h(int x) { return g(x) * 2; }\n"
                     "static inline int k(int x) { return h(x); }\n"
                     "namespace n { static int q(int x) { return x - 1; } int r(int x) { return q(x); } }\n",
           "a.cpp")
    b = ok(tmp_path, "int g(int x) { return x + 1; }\n"
                     "int h(int x) { return g(x) * 2; }\n"
                     "int k(int x) { return h(x); }\n"
                     "namespace n { namespace { int q(int x) { return x - 1; } } int r(int x) { return q(x); } }\n",
           "b.cpp")
    assert a == b
    q = [f for f in a.funcs() if f.name == "q"][0]
    assert q.internal and q.ns == ("n",)
    assert not any(f.internal for f in a.funcs() if not f.ns)
    rs, cxx = laws(tmp_path, a)
    assert "    fn q(mut x: i32) -> i32 {" in rs
    assert "static" not in cxx and "inline" not in cxx and "namespace {" in cxx


@needs_clang
def test_n21_neighbours_stay_refused(tmp_path):
    src = ("namespace n { static int q(int x) { return x - 1; } }\n"
           "int r(int x) { return n::q(x); }\n"
           "extern int e(int x) { return x; }\n"
           "constexpr int c(int x) { return x; }\n")
    e = errs(tmp_path, src)
    assert (2, 23, "unsupported: 'n::q' is internal to namespace 'n' (a C++ anonymous-namespace member, a "
                   "private Rust `fn`) and is referenced from outside it") in e
    assert (3, 12, "unsupported: extern function") in e
    assert (4, 15, "unsupported: constexpr function") in e


# ------------------------------------------------------------------ N22

@needs_clang
def test_n22_auto_is_the_deduced_type(tmp_path):
    a = ok(tmp_path, "int f(const std::vector<uint8_t>& v, int x) {\n"
                     "    auto y = x + 1;\n    const auto b = v[0];\n    auto n = v.size();\n"
                     "    auto w = v;\n    auto s = std::string(\"ab\");\n    auto p = v.data();\n"
                     "    int t = 0;\n"
                     "    for (const auto& e : v) { t += e; }\n    for (auto e : w) { t += e; }\n"
                     "    for (auto& e : w) { e = 1; }\n"
                     "    return y + b + static_cast<int>(n + s.size()) + p[0] + t;\n}\n", "a.cpp")
    b = ok(tmp_path, "int f(const std::vector<unsigned char>& v, int x) {\n"
                     "    int y = x + 1;\n    const unsigned char b = v[0];\n    std::size_t n = v.size();\n"
                     "    std::vector<unsigned char> w = v;\n    std::string s = std::string(\"ab\");\n"
                     "    const unsigned char* p = v.data();\n"
                     "    int t = 0;\n"
                     "    for (const unsigned char& e : v) { t += e; }\n    for (unsigned char e : w) { t += e; }\n"
                     "    for (unsigned char& e : w) { e = 1; }\n"
                     "    return y + b + static_cast<int>(n + s.size()) + p[0] + t;\n}\n", "b.cpp")
    assert a == b
    laws(tmp_path, a)


@needs_clang
def test_n22_neighbours_stay_refused(tmp_path):
    src = ("int f(std::vector<int>& v, int x) {\n"
           "    auto& r = x;\n"
           "    auto it = v.begin();\n"
           "    auto lam = [](int q) { return q; };\n"
           "    auto il = {1, 2};\n"
           "    auto z{3};\n"
           "    decltype(auto) d = x;\n"
           "    return x;\n}\n")
    assert errs(tmp_path, src) == [
        (2, 5, "unsupported: variable of type 'int &'"),
        (3, 5, "unsupported: variable of type 'iterator'"),
        (4, 5, "unsupported: auto-typed declaration of a lambda"),
        (5, 5, "unsupported: variable of type 'std::initializer_list<int>'"),
        (6, 5, "unsupported: list-initialization (the subset spells it `T x = e;`)"),
        (7, 5, "unsupported: decltype-typed declaration"),
    ]


# ------------------------------------------------------------------ ?:

@needs_clang
def test_conditional_import_and_spellings(tmp_path):
    p = ok(tmp_path, "long long f(bool c, int i, unsigned u, char ch, int k) {\n"
                     "    long long r = c ? i : u;\n"
                     "    int q = k ? ch : 300;\n"
                     "    return r + q + (c ? 1 : k > 2 ? 2 : 3);\n}\n")
    s = body(p, "f")
    assert s[0].init == K.Cast(K.I64, K.Cond(V("c"), K.Cast(K.U32, V("i")), V("u")))
    assert s[1].init == K.Cond(K.Binary("ne", V("k"), K.IntLit(0, K.I32)), K.Cast(K.I32, V("ch")),
                               K.IntLit(300, K.I32))
    rs, cxx = laws(tmp_path, p)
    assert "let mut r: i64 = (if c { i as u32 } else { u }) as i64;" in rs
    assert "return (r + (q as i64)) + ((if c { 1i32 } else if k > 2 { 2i32 } else { 3i32 }) as i64);" in rs \
        or "if c { 1i32 } else if k > 2 { 2i32 } else { 3i32 }" in rs
    assert "long long r = static_cast<long long>(c ? static_cast<unsigned>(i) : u);" in cxx
    assert "c ? 1 : (k > 2 ? 2 : 3)" in cxx


@needs_clang
def test_conditional_neighbours_stay_refused(tmp_path):
    src = ("int f(int c, int x, int y, std::string a, std::string b) {\n"
           "    (c ? x : y) = 5;\n"
           "    int* p = &(c ? x : y);\n"
           "    std::string s = c ? a : b;\n"
           "    int z = x ?: y;\n"
           "    return *p + z + static_cast<int>(s.size());\n}\n")
    e = errs(tmp_path, src)
    assert (2, 6, "unsupported: conditional operator `?:` used as an lvalue") in e
    assert (3, 16, "unsupported: conditional operator `?:` used as an lvalue") in e
    assert (4, 21, "unsupported: conditional operator `?:` of type 'std::string' (the subset's `?:` yields "
                   "a scalar)") in e
    assert (5, 13, "unsupported: conditional operator `?:`") in e


# ------------------------------------------------------------------ x++ as a value

@needs_clang
def test_increment_as_a_value_import(tmp_path):
    p = ok(tmp_path, "int f(int n, std::vector<int>& out, unsigned& u) {\n"
                     "    std::size_t pos = 0;\n"
                     "    int i = 0;\n"
                     "    while (n-- > 0) { out[pos++] = ++i; }\n"
                     "    unsigned w = u++;\n"
                     "    return i + static_cast<int>(--pos) + static_cast<int>(w);\n}\n")
    s = body(p, "f")
    w = s[2]
    assert w.cond == K.Binary("gt", K.IncDec(V("n"), "sub", True), K.IntLit(0, K.I32))
    assert w.body.stmts[0] == K.Store(K.Index(V("out"), K.IncDec(V("pos"), "add", True)),
                                      K.IncDec(V("i"), "add", False))
    assert s[3].init == K.IncDec(V("u"), "add", True)
    rs, cxx = laws(tmp_path, p)
    assert "    while ({ n -= 1; n + 1 }) > 0 {" in rs
    assert "        out[{ pos = pos.wrapping_add(1usize); pos.wrapping_sub(1usize) }] = { i += 1; i };" in rs
    assert "    let mut w: u32 = { *u = (*u).wrapping_add(1u32); (*u).wrapping_sub(1u32) };" in rs
    assert "        out[pos++] = ++i;" in cxx and "unsigned w = u++;" in cxx


@needs_clang
def test_increment_as_a_value_refusals(tmp_path):
    src = ("int f(int i, std::vector<int>& v, std::vector<unsigned>& w, std::size_t k) {\n"
           "    int a = i++ + i;\n"
           "    v[i] = i++;\n"
           "    i = i++;\n"
           "    int b = v[static_cast<std::size_t>(i)] + v[static_cast<std::size_t>(i--)];\n"
           "    w[k++] += 1u;\n"
           "    for (int j = 0; j < 3; ++j) { a += j++; }\n"
           "    int x = 4;\n"
           "    int* p = &x;\n"
           "    int y = x++ + *p;\n"
           "    return a + b + y;\n}\n"
           "void g(char c, double d, int* p, unsigned char u) {\n"
           "    char c2 = c++;\n    double d2 = d--;\n    int* q = p++;\n    unsigned char u2 = ++u;\n"
           "    std::printf(\"%d %f %d %d\\n\", c2, d2, *q, u2);\n}\n")
    assert errs(tmp_path, src) == [
        (2, 13, SEQ % ("i", "i")),
        (3, 12, SEQ % ("i", "i")),
        (4, 9, SEQ % ("i", "i")),
        (5, 73, SEQ % ("i", "i")),
        (6, 5, "unsupported: unsigned compound assignment to an element whose index increments or decrements "
               "a variable"),
        (7, 5, "unsupported: counting loop body assigns the loop variable 'j'"),
        (10, 13, "unsupported: 'x' is used while 'p', a mutable pointer derived from it, is in scope (a second "
                 "borrow in Rust)"),
        (14, 15, "unsupported: increment/decrement used as a value"),
        (15, 17, "unsupported: increment/decrement used as a value"),
        (16, 14, "unsupported: increment/decrement used as a value"),
        (17, 24, "unsupported: increment/decrement used as a value"),
    ]


@needs_clang
def test_increment_is_an_assignment_for_loop_bounds(tmp_path):
    # C++ re-reads `i < n` every iteration, Rust's range does not: a bound
    # incremented inside the body (as a value) must be refused like `n += 1;`
    src = ("int f(int n) {\n"
           "    int s = 0;\n"
           "    for (int i = 0; i < n; ++i) { s += n++ > 100 ? 1 : 0; }\n"
           "    return s;\n}\n")
    assert errs(tmp_path, src) == [(3, 5, "unsupported: counting loop body assigns or shadows the bound 'n'")]


# ------------------------------------------------------------------ Rust surface

RS_OK = """fn f(mut n: i32, u: &mut u32, mut z: usize, c: bool) -> i64 {
    let a: i32 = { n += 1; n };
    let b: i32 = { n -= 1; n + 1 };
    let d: u32 = { *u = (*u).wrapping_add(1u32); (*u).wrapping_sub(1u32) };
    let e: usize = { z = z.wrapping_sub(1usize); z };
    let g: i64 = (if c { a } else { b }) as i64;
    let h: i16 = if c { 3i16 } else if n > 2 { (d as i32) as i16 } else { 7i16 };
    return g + ((h as i32) as i64) + (e as i64);
}
"""


def test_rust_surface_parses_to_the_kernel():
    p = RP.rs_parse(RS_OK)
    s = body(p, "f")
    assert s[0].init == K.IncDec(V("n"), "add", False)
    assert s[1].init == K.IncDec(V("n"), "sub", True)
    assert s[2].init == K.IncDec(V("u"), "add", True)
    assert s[3].init == K.IncDec(V("z"), "sub", False)
    assert s[4].init == K.Cast(K.I64, K.Cond(V("c"), V("a"), V("b")))
    assert isinstance(s[5].init, K.Cond) and isinstance(s[5].init.els, K.Cond)
    assert RSP.rs_print(p) == RS_OK


def test_rust_surface_refusals():
    def one(stmt, ret="0"):
        src = "fn f(mut n: i32, mut z: usize, c: bool) -> i32 {\n    %s\n    return %s;\n}\n" % (stmt, ret)
        return rs_errs(src)

    assert one("let a: i32 = { n += 2; n };") == [
        ("2:18", "unsupported: block expression other than the canonical increment `{ x += 1; x }` / "
                 "`{ x += 1; x - 1 }` (`{ x = x.wrapping_add(1u32); x }` when unsigned)")]
    assert one("let a: i32 = { n += 1; n - 2 };")[0][1].startswith("unsupported: block expression other than")
    assert one("let a: usize = { z += 1usize; z };") == [
        ("2:20", "unsupported: increment block not spelled the canonical way "
                 "(`{ x = x.wrapping_add(1usize); .. }`)")]
    assert one("let a: i32 = if c { 1 } else { 2 }.max(3);") == [
        ("2:39", "unsupported: `.` after an unparenthesized `if` expression (the subset parenthesizes it)")]
    assert one("let a: i32 = if c { 1 } else { 2 } + 3;") == [
        ("2:40", "unsupported: `+` after an unparenthesized `if` expression (the subset parenthesizes it)")]
    assert one("let a: i32 = { n += 1; n } * 2;") == [
        ("2:32", "unsupported: `*` after an unparenthesized block expression (the subset parenthesizes it)")]
    assert one("let a: i32 = if c { 1 };") == [("2:18", "unsupported: `if` expression without `else`")]
    assert one("let a: i32 = if c { let q: i32 = 1; q } else { 2 };") == [
        ("2:25", "unsupported: `let` expression")]
    assert one("let a: i32 = if c { 1 } else { 2i64 };") == [
        ("2:18", "arms of a conditional expression have different types i32 and i64")]
    assert one("let a: i32 = ({ n += 1; n }) + n;", "a") == [("2:19", SEQ % ("n", "n"))]


# ------------------------------------------------------------------ i16 / u16

def test_i16_u16_have_no_arithmetic_of_their_own():
    src = "fn f(a: i16, b: u16) -> i32 {\n    let c: i16 = a + a;\n    return (c as i32) + (b as i32);\n}\n"
    assert rs_errs(src) == [("2:20", "arithmetic add on i16")]
    p = RP.rs_parse("fn f(a: i16, b: u16) -> i32 {\n    let c: u16 = 65535u16;\n"
                    "    return ((a as i32) + (b as i32)) + (c as i32);\n}\n")
    assert [x.ty for x in p.funcs()[0].params] == [K.I16, K.U16]


@needs_clang
def test_i16_u16_laws(tmp_path):
    p = ok(tmp_path, "short f(short a, unsigned short b) {\n"
                     "    short c = a + b;\n    unsigned short d = 65535;\n    ++c;\n    d += 1;\n"
                     "    std::vector<short> v = {a, 7};\n"
                     "    return c + d + v[1];\n}\n")
    rs, cxx = laws(tmp_path, p)
    assert "fn f(mut a: i16, mut b: u16) -> i16 {" in rs
    assert "short f(short a, unsigned short b) {" in cxx
    assert "static_cast<unsigned short>(65535)" in cxx


# ------------------------------------------------------------------ the generator

FR259_SEEDS = range(3000, 3010)


@pytest.fixture(scope="module")
def fr259_programs():
    return [gen.gen_program(seed, 50, fr259=True) for seed in FR259_SEEDS]


def test_generator_covers_fr259(fr259_programs):
    seen = collections.Counter()
    for p in fr259_programs:
        for f in p.all_funcs():
            for prm in f.params:
                if prm.ty in (K.I16, K.U16):
                    seen["param-" + prm.ty.name] += 1
            for n in K.walk(f.body):
                if isinstance(n, K.Cond):
                    seen["cond"] += 1
                    seen["cond-" + ("elif" if isinstance(n.els, K.Cond) else "plain")] += 1
                if isinstance(n, K.IncDec):
                    seen["incdec-%s-%s" % (n.op, "post" if n.post else "pre")] += 1
                if isinstance(n, K.Index) and isinstance(n.index, K.IncDec):
                    seen["incdec-index"] += 1
                if isinstance(n, (K.Call, K.MethodCall)) and any(
                        isinstance(m, K.IncDec) for a in n.args for m in K.walk(a)):
                    seen["incdec-arg"] += 1
                if isinstance(n, K.IntLit) and n.ty in (K.I16, K.U16):
                    seen["lit-" + n.ty.name] += 1
                if isinstance(n, K.Cast) and n.ty in (K.I16, K.U16):
                    seen["cast-" + n.ty.name] += 1
                if isinstance(n, K.Let) and n.ty in (K.I16, K.U16):
                    seen["let-" + n.ty.name] += 1
    for k in ("cond", "cond-elif", "cond-plain", "incdec-add-pre", "incdec-add-post", "incdec-sub-pre",
              "incdec-sub-post", "lit-i16", "lit-u16", "let-i16", "let-u16", "param-i16", "param-u16"):
        assert seen[k] >= 10, (k, seen[k])
    # (rarer by construction: a small type is mostly a variable or a literal,
    # and an increment needs an unmentioned writable variable of its type)
    for k in ("cast-i16", "cast-u16", "incdec-index", "incdec-arg"):
        assert seen[k] >= 3, (k, seen[k])


def test_law1_and_law2_rust_fr259(fr259_programs):
    for p in fr259_programs:
        text = RSP.rs_print(p)
        back = RP.rs_parse(text)
        assert back == p
        assert RSP.rs_print(back) == text


@needs_clang
def test_law1_and_law2_cxx_fr259(fr259_programs, tmp_path):
    for i, p in enumerate(fr259_programs):
        text = CP.cxx_print(p)
        path = os.path.join(str(tmp_path), "ftu%d.cpp" % i)
        with open(path, "w") as f:
            f.write(text)
        back, es = cli.load_cpp(path, "clang++")
        assert es == [], (path, es[:5])
        assert back == p
        assert CP.cxx_print(back) == text


@needs_rustc
def test_printed_fr259_rust_borrow_checks(fr259_programs, tmp_path):
    header = ("#![allow(arithmetic_overflow, unconditional_panic, unreachable_code, unused, "
              "overflowing_literals, unconditional_recursion, while_true)]\n")
    for i, p in enumerate(fr259_programs[:5]):
        path = os.path.join(str(tmp_path), "fg%d.rs" % i)
        with open(path, "w") as f:
            f.write(header + RSP.rs_print(p))
        r = subprocess.run(["rustc", "--edition", "2021", "--emit=metadata", "--crate-type=lib",
                            "-o", path + ".rmeta", path], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[:4000]


def test_typecheck_counts_increment_as_assignment():
    blk = K.Block((K.Let("y", K.I32, True, K.IncDec(V("x"), "add", True)),))
    assert "x" in TC.assigned_names(blk)
