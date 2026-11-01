# Pins FR-261 (C++ pointers <=> Rust suffix slices) at the unit level.  The
# invariants, and why each is load-bearing:
#  * a pointer is the SUFFIX SLICE of its allocation: `const T*` <=> `&[T]`,
#    `T*` <=> `&mut [T]`, for parameters and locals only.  Lengths stay
#    ordinary integers -- there is no bounds proof, so an index outside the
#    allocation panics in Rust where it is UB in C++ (a difference on UB
#    input only); every pointer VALUE therefore has to come from a real
#    allocation (`v.data()`, `&x`, a forward re-slice), and `&v[i]` must be
#    the suffix from i (N17), never a one-element slice C++ may index past;
#  * one Kernel term per meaning: `*p` / `p[0]` / `p->f` / `(*p).f` (N16),
#    `p = p + k` / `p += k` / `++p` (N18), and a usize index or offset built
#    from a non-negative literal, an unsigned value or a provably
#    non-negative signed one (N19) -- so laws 1 and 2 hold on both sides;
#  * the alias shapes rustc rejects (R1, R2, a mutable re-slice reading its
#    own root) and the shapes the model cannot express (R3 signed offsets and
#    backwards motion, R4 pointer comparison / difference, R6 null, pointer
#    members and returns, casts, libc) are LOCATED refusals with exact
#    wordings -- never rustc errors on accepted input, never silently wrong;
#  * a counting loop whose body makes a mutable pointer to its bound is
#    refused: C++ re-reads `i < n` every iteration, Rust's range does not, so
#    accepting it would be a SILENT miscompile;
#  * (a pre-existing FR-257 hole found here) a member function named `into`
#    or `try_into` loses to the prelude's by-value trait method in rustc's
#    method probe; both names are reserved.
import shutil

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_import as CI
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
HEAD = "#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n#include <string>\n#include <vector>\n"
NHEAD = HEAD.count("\n")

V = K.Var
U0 = K.IntLit(0, K.USIZE)


def imp(tmp_path, src, name="t.cpp"):
    p = tmp_path / name
    p.write_text(HEAD + src)
    return cli.load_cpp(str(p), "clang++")


def ok(tmp_path, src):
    prog, errs = imp(tmp_path, src)
    assert errs == [], errs
    return prog


def errs(tmp_path, src):
    """(line relative to ``src``, col, message) of every located error."""
    _, es = imp(tmp_path, src)
    return [(loc.line - NHEAD, loc.col, m) for loc, m in es]


def rs_errs(src):
    _, es = RP.rs_parse_all(src)
    return [("%s" % loc, m) for loc, m in es]


def body(prog, name):
    return list([f for f in prog.all_funcs() if f.name == name][0].body.stmts)


# ------------------------------------------------------------------ types

def test_pointer_spellings():
    P = CI.parse_cxx_type
    assert P("const uint8_t *") == (K.PtrT(K.U8, False), False)
    assert P("uint8_t *") == (K.PtrT(K.U8, True), False)
    assert P("int *const") == (K.PtrT(K.I32, True), True)
    assert P("const size_t *const") == (K.PtrT(K.USIZE, False), True)
    assert P("const unsigned char *") == (K.PtrT(K.U8, False), False)
    assert P("std::vector<int> *") == (K.PtrT(K.VecT(K.I32), True), False)
    # a pointer type has no desugared spelling: its typedefs are resolved here,
    # inside template arguments too (woff2's `std::vector<uint8_t>* out`)
    assert P("const std::vector<uint8_t> *") == (K.PtrT(K.VecT(K.U8), False), False)
    assert P("std::uint32_t *") == (K.PtrT(K.U32, True), False)
    assert P("int8_t *")[0] is None and P("my_size_t *")[0] is None
    # outside the model: T**, void*, a function pointer, a reference to a pointer
    for q in ("int **", "const int *const *", "void *", "int (*)(int)", "int *&"):
        assert P(q)[0] is None, q


def test_pointer_is_copy_and_qualification_conversion():
    assert K.is_copy(K.PtrT(K.I32, True)) and not K.is_owned_nc(K.PtrT(K.I32, True))
    assert K.ptr_fits(K.PtrT(K.I32, True), K.PtrT(K.I32, False))  # T* -> const T*
    assert not K.ptr_fits(K.PtrT(K.I32, False), K.PtrT(K.I32, True))
    assert not K.ptr_fits(K.PtrT(K.I32, True), K.PtrT(K.I64, True))
    assert str(K.PtrT(K.U8, False)) == "&[u8]" and str(K.PtrT(K.U8, True)) == "&mut [u8]"


# ------------------------------------------------------------------ Kernel

def handbuilt():
    """put(pos, out, b): out[*pos] = b; ++*pos;  walk(p, n): p += 1 ..."""
    pos, out = K.PtrT(K.USIZE, True), K.PtrT(K.U8, True)
    put = K.Func("put", (K.Param("pos", pos, False), K.Param("out", out, False), K.Param("b", K.U8, True)), K.UNIT,
                 K.Block((K.Store(K.Index(V("out"), K.Index(V("pos"), U0)), V("b")),
                          K.CompoundStore("add", K.Index(V("pos"), U0), K.IntLit(1, K.USIZE)))))
    walk = K.Func("walk", (K.Param("p", K.PtrT(K.U8, False), True),), K.I32,
                  K.Block((K.CompoundAssign("add", "p", K.IntLit(2, K.USIZE)),
                           K.Let("q", K.PtrT(K.U8, False), False, K.Offset(V("p"), K.IntLit(1, K.USIZE))),
                           K.Return(K.Cast(K.I32, K.Index(V("q"), U0))))))
    main = K.Func("main", (), K.UNIT, K.Block((
        K.Let("v", K.VecT(K.U8), True, K.VecFill(K.IntLit(0, K.U8), K.IntLit(6, K.USIZE))),
        K.Let("k", K.USIZE, True, K.IntLit(1, K.USIZE)),
        K.ExprStmt(K.Call("put", (K.AddrOf(V("k")), K.Offset(V("v"), None), K.IntLit(9, K.U8)))),
        K.ExprStmt(K.Call("put", (K.AddrOf(V("k")), K.Offset(V("v"), K.IntLit(1, K.USIZE)), K.IntLit(8, K.U8)))),
        K.Print((K.Spec("d", K.Call("walk", (K.Offset(V("v"), None),))), K.Text("\n"))),
    )))
    return TC.normalize(K.Program((put, walk, main)))


def test_handbuilt_rust_law1_and_spellings():
    prog = handbuilt()
    assert TC.check(prog) == []
    text = RSP.rs_print(prog)
    for line in ("fn put(pos: &mut [usize], out: &mut [u8], mut b: u8) {",
                 "    out[pos[0usize]] = b;",
                 "    pos[0usize] = pos[0usize].wrapping_add(1usize);",
                 "fn walk(mut p: &[u8]) -> i32 {",
                 "    p = &p[2usize..];",
                 "    let q: &[u8] = &p[1usize..];",
                 "    put(std::slice::from_mut(&mut k), &mut v[..], 9u8);",
                 "    put(std::slice::from_mut(&mut k), &mut v[1usize..], 8u8);",
                 # a `T*` value where a `const T*` is wanted: `&` (the PARAMETER decides)
                 "    print!(\"{}\\n\", walk(&v[..]));"):
        assert line in text.split("\n"), (line, text)
    back = RP.rs_parse(text)
    assert back == prog
    assert RSP.rs_print(back) == text


@needs_clang
def test_handbuilt_cxx_law1_and_spellings(tmp_path):
    prog = handbuilt()
    text = CP.cxx_print(prog)
    for line in ("void put(std::size_t* const pos, unsigned char* const out, unsigned char b) {",
                 "    out[*pos] = b;", "    ++*pos;",
                 "int walk(const unsigned char* p) {", "    p += 2;",
                 "    const unsigned char* const q = p + 1;", "    return static_cast<int>(*q);",
                 "    put(&k, v.data(), static_cast<unsigned char>(9));",
                 "    put(&k, v.data() + 1, static_cast<unsigned char>(8));"):
        assert line in text.split("\n"), (line, text)
    p = tmp_path / "rt.cpp"
    p.write_text(text)
    back, es = cli.load_cpp(str(p), "clang++")
    assert es == [] and back == prog
    assert CP.cxx_print(back) == text


# ------------------------------------------------------------------ normal forms

@needs_clang
def test_n16_deref_spellings_are_one_term(tmp_path):
    prog = ok(tmp_path, "struct S { int f; int get() const { return f; } };\n"
                        "int a(const int* p) { return *p; }\n"
                        "int b(const int* p) { return p[0]; }\n"
                        "int c(const S* s) { return s->f + (*s).f + s[0].f + s->get(); }\n")
    assert body(prog, "a") == body(prog, "b") == [K.Return(K.Index(V("p"), U0))]
    f = K.Field(K.Index(V("s"), U0), "f")
    assert body(prog, "c")[0] == K.Return(K.Binary("add", K.Binary("add", K.Binary("add", f, f), f),
                                                   K.MethodCall(K.Index(V("s"), U0), "get", ())))
    cxx = CP.cxx_print(prog)
    assert "    return *p;" in cxx and "    return s->f + s->f + s->f + s->get();" in cxx


@needs_clang
def test_n17_address_of_an_element_is_the_suffix(tmp_path):
    prog = ok(tmp_path, "int g(const int* p) { return p[1]; }\n"
                        "int f(std::vector<int>& v, const int* q) { return g(&v[1]) + g(&q[2]) + g(v.data() + 1); }\n")
    r = body(prog, "f")[0].value
    one = K.IntLit(1, K.USIZE)
    assert r.lhs.lhs.args[0] == K.Offset(V("v"), one) == r.rhs.args[0]
    assert r.lhs.rhs.args[0] == K.Offset(V("q"), K.IntLit(2, K.USIZE))


@needs_clang
def test_n18_reslice_spellings_are_one_term(tmp_path):
    prog = ok(tmp_path, "void f(const int* p, int* q) { p = p + 2; p += 2; ++p; p++; q += 3; }\n")
    two, one = K.IntLit(2, K.USIZE), K.IntLit(1, K.USIZE)
    assert body(prog, "f") == [K.CompoundAssign("add", "p", two), K.CompoundAssign("add", "p", two),
                               K.CompoundAssign("add", "p", one), K.CompoundAssign("add", "p", one),
                               K.CompoundAssign("add", "q", K.IntLit(3, K.USIZE))]
    rs = RSP.rs_print(prog)
    assert "    p = &p[2usize..];" in rs and "    q = &mut q[3usize..];" in rs
    # the Rust surface `p = &p[k..];` is the same term
    assert RP.rs_parse(rs) == prog
    cxx = CP.cxx_print(prog)
    assert "    p += 2;" in cxx and "    ++p;" in cxx and "    q += 3;" in cxx


@needs_clang
def test_n19_index_conversions(tmp_path):
    prog = ok(tmp_path, "int f(const int* p, unsigned u, unsigned char c, int n) {\n"
                        "    int t = p[3] + p[u] + p[c];\n"
                        "    for (int i = 0; i < n; ++i) { t += p[i]; }\n"
                        "    return t;\n}\n")
    let = body(prog, "f")[0]
    us = K.USIZE
    assert let.init == K.Binary("add", K.Binary("add", K.Index(V("p"), K.IntLit(3, us)),
                                                K.Index(V("p"), K.Cast(us, V("u")))),
                                K.Index(V("p"), K.Cast(us, V("c"))))  # (clang does not promote a subscript)
    loop = body(prog, "f")[1]
    assert loop.body.stmts[0] == K.CompoundAssign("add", "t", K.Index(V("p"), K.Cast(us, V("i"))))
    assert "t += p[i as usize];" in RSP.rs_print(prog)


# ------------------------------------------------------------------ refusals

R1_ARGS = ("unsupported: '%s' is passed to '%s' as a mutable pointer and also used by another argument of the "
           "same call (aliasing: a second borrow in Rust)")
R1_STMT = ("unsupported: '%s' is passed to '%s' as a mutable pointer and also used elsewhere in the statement "
           "(C++ leaves the order unspecified)")
R2_MUT = ("unsupported: '%s' is used while '%s', a mutable pointer derived from it, is in scope (a second borrow "
          "in Rust)")
R2_CONST = ("unsupported: '%s' is modified or moved while '%s', a pointer into it, is in scope (a borrow conflict "
            "in Rust)")
SELF_OFF = "unsupported: a mutable slice of '%s' starts at an offset that reads '%s' (a second borrow in Rust)"


@needs_clang
def test_r1_r2_alias_refusals(tmp_path):
    src = ("void cp(int* d, const int* s) { d[0] = s[0]; }\n"
           "int inc(int* p) { *p += 1; return *p; }\n"
           "void f(std::vector<int>& v, int x, int* p, std::size_t* sp) {\n"
           "    cp(v.data() + 1, v.data());\n"
           "    int y = inc(&x) + x;\n"
           "    inc(v.data() + (v.size() - 1));\n"
           "    sp += sp[0];\n"
           "    int* q = p + 1;\n"
           "    p[0] = y;\n"
           "    const int* w = v.data();\n"
           "    v.push_back(q[0] + w[0]);\n"
           "}\n")
    assert errs(tmp_path, src) == [
        (4, 5, R1_ARGS % ("v", "cp")),
        (5, 13, R1_STMT % ("x", "inc")),
        (6, 9, SELF_OFF % ("v", "v")),
        (7, 5, SELF_OFF % ("sp", "sp")),
        (9, 5, R2_MUT % ("p", "q")),
        (11, 5, R2_CONST % ("v", "w")),
    ]


@needs_clang
def test_counting_loop_bound_through_a_pointer_is_refused(tmp_path):
    # accepted, both would be SILENT miscompiles: C++ re-reads the bound
    src = ("int f(std::vector<std::vector<int>>& vv, int n) {\n"
           "    int t = 0;\n"
           "    for (std::size_t i = 0; i < vv[0].size(); ++i) {\n"
           "        std::vector<std::vector<int>>* pv = &vv;\n"
           "        (*pv)[0].push_back(1);\n"
           "    }\n"
           "    for (int i = 0; i < n; ++i) {\n"
           "        int* pn = &n;\n"
           "        *pn = 2;\n"
           "        t += 1;\n"
           "    }\n"
           "    return t;\n}\n")
    assert errs(tmp_path, src) == [
        (3, 5, "unsupported: counting loop body modifies or shadows 'vv', whose size is the bound"),
        (7, 5, "unsupported: counting loop body assigns or shadows the bound 'n'"),
    ]


@needs_clang
def test_r3_r4_r6_refusals(tmp_path):
    src = ("int a(const int* p, int k) { return p[k]; }\n"
           "int b(const int* p) { return *(p - 1); }\n"
           "void c(int* p) { --p; p -= 2; *p = 0; }\n"
           "int d(const int* p, const int* q) { return static_cast<int>(q - p) + (p == q); }\n"
           "int e(int* p) { if (p) { return 1; } return 0; }\n"
           "const int* g(const int* p) { return p; }\n"
           "void h(const int* p, const int* q) { p = q; }\n"
           "int m() { return e(nullptr); }\n")
    assert errs(tmp_path, src) == [
        (1, 39, "unsupported: " + CI.R3_SIGNED),
        (2, 32, "unsupported: " + CI.R3_MINUS),
        (3, 18, "unsupported: " + CI.R3_DEC),
        (3, 23, "unsupported: " + CI.R3_DEC),
        (4, 61, "unsupported: " + CI.R4_DIFF),
        (4, 71, "unsupported: " + CI.R4_CMP),
        (5, 21, "unsupported: " + CI.R6_NULL),
        (6, 12, "unsupported: return type 'const int *'"),
        (7, 38, "unsupported: assignment of a pointer (the subset re-slices a pointer only by `p += k;` / `++p;`)"),
        (8, 20, "unsupported: " + CI.R6_NULL),
    ]


def test_rust_surface_refusals():
    src = ("fn f(p: &[i32], q: &mut [i32]) {\n"
           "    q[0usize] = p[0usize];\n"
           "}\n"
           "fn main() {\n"
           "    let mut v: Vec<i32> = vec![1, 2];\n"
           "    let mut x: i32 = 3;\n"
           "    f(&mut v[..], std::slice::from_ref(&x));\n"
           "    let s: &[i32] = &v[..];\n"
           "    let mut t: &[i32] = &s[..];\n"
           "    t += 1;\n"
           "}\n")
    assert rs_errs(src) == [
        ("7:12", "unsupported: argument 1 of 'f' is not spelled the canonical way (`&x[..]`)"),
        ("7:19", "unsupported: argument 2 of 'f' is not spelled the canonical way (`std::slice::from_mut(&mut x)`)"),
        ("9:26", "unsupported: whole slice `&p[..]` of a pointer (the subset spells it `p`)"),
        ("10:5", "unsupported: `+=` on a slice (the subset spells a re-slice `t = &t[k..];`)"),
    ]
    assert rs_errs("fn main() { let v: Vec<i32> = vec![1]; let s: &[i32] = v[0usize..]; }\n") == [
        ("1:57", "unsupported: unsized slice place `x[k..]` (the subset spells a re-slice `&x[k..]` / "
                 "`&mut x[k..]`)")]


@needs_clang
def test_struct_method_named_into_is_reserved(tmp_path):
    src = "struct M {\n    int x;\n    int into(int k) const { return x + k; }\n    int try_into() const { return x; }\n};\n"
    wording = ("unsupported: identifier '%s' is reserved by the canonical Rust spelling (a by-value prelude trait "
               "method, found before a `&self` one)")
    assert errs(tmp_path, src) == [(3, 9, wording % "into"), (4, 9, wording % "try_into")]


@needs_clang
def test_accepted_pointer_local_from_address_of(tmp_path):
    # the shape test_cxx_import used to pin as `variable of type 'int *'`
    prog = ok(tmp_path, "int f(int x) {\n    int* p = &x;\n    *p = 3;\n    return 0;\n}\n")
    assert body(prog, "f")[0] == K.Let("p", K.PtrT(K.I32, True), True, K.AddrOf(V("x")))
    assert "    let mut p: &mut [i32] = std::slice::from_mut(&mut x);" in RSP.rs_print(prog)
