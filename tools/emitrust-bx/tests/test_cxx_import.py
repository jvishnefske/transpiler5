# Pins the C++ side of the subset (FR-250): cxx_import over clang's JSON AST.
# Invariants: (1) main-file filtering is by the tracked `loc.file`, so
# <cstdio>'s own declarations never leak in; (2) printf is recognised by
# DECL IDENTITY (libc's, or a body-less extern "C" redeclaration), never by
# name -- a user function named printf must not become print!; (3) every
# implicit conversion becomes an explicit Kernel cast or `!= 0`; (4) the
# import does NOT stop at the first unsupported construct: every one is a
# located Residual, so the distance from the subset is a count (FR-254).
import shutil

import pytest

from emitrust_bx import cli
from emitrust_bx import kernel as K
from emitrust_bx.cxx_import import cxx_import

pytestmark = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")


def imp(tmp_path, src):
    p = tmp_path / "t.cpp"
    p.write_text(src)
    return cli.load_cpp(str(p), "clang++")


def ok(tmp_path, src):
    prog, errs = imp(tmp_path, src)
    assert errs == []
    return prog


def errs(tmp_path, src):
    prog, e = imp(tmp_path, src)
    assert prog is None
    return [(str(l), m) for l, m in e]


def test_cstdio_decls_do_not_leak(tmp_path):
    p = ok(tmp_path, "#include <cstdio>\nint main() {\n    std::printf(\"hi\\n\");\n    return 0;\n}\n")
    assert [f.name for f in p.funcs()] == ["main"]
    assert p.funcs()[0].body.stmts == (K.Print((K.Text("hi\n"),)),)


def test_printf_spellings_are_one_kernel(tmp_path):
    a = ok(tmp_path, '#include <cstdio>\nint main() {\n    printf("%i %ld\\n", 1, 2L);\n    return 0;\n}\n')
    b = ok(tmp_path, '#include <cstdio>\nint main() {\n    std::printf("%d %lld\\n", 1, 2LL);\n}\n')
    c = ok(tmp_path, 'extern "C" int printf(const char *, ...);\nint main() {\n    printf("%d %lld\\n", 1, 2LL);\n    return 0;\n}\n')
    assert a == b == c


def test_user_printf_is_not_libc(tmp_path):
    e = errs(tmp_path, "int printf(int x) { return x; }\nint main() {\n    printf(3);\n    return 0;\n}\n")
    assert e == [("1:5", "unsupported: identifier 'printf' is reserved by the canonical spelling")]


def test_implicit_conversions_become_explicit(tmp_path):
    p = ok(tmp_path, "long long f(int a, char c, unsigned u) {\n"
                     "    if (a) { return a + c; }\n    while (u) { u = u - 1; }\n    return u;\n}\n")
    body = p.funcs()[0].body.stmts
    assert body[0].cond == K.Binary("ne", K.Var("a"), K.IntLit(0, K.I32))
    assert body[0].then.stmts[0] == K.Return(K.Cast(K.I64, K.Binary("add", K.Var("a"), K.Cast(K.I32, K.Var("c")))))
    assert body[1].body.stmts[0] == K.CompoundAssign("sub", "u", K.IntLit(1, K.U32))
    assert body[2] == K.Return(K.Cast(K.I64, K.Var("u")))


def test_increment_forms(tmp_path):
    p = ok(tmp_path, "#include <cstdio>\nvoid f(int i, std::size_t z, char c, double d) {\n"
                     "    ++i; i++; i += 1; z++; c++; d++;\n}\n")
    s = p.funcs()[0].body.stmts
    one = K.CompoundAssign("add", "i", K.IntLit(1, K.I32))
    assert s[0] == s[1] == s[2] == one
    assert s[3] == K.CompoundAssign("add", "z", K.IntLit(1, K.USIZE))
    assert s[4] == K.Assign("c", K.Cast(K.I8, K.Binary("add", K.Cast(K.I32, K.Var("c")), K.IntLit(1, K.I32))))
    assert s[5] == K.CompoundAssign("add", "d", K.FloatLit("1.0"))


def test_every_residual_is_reported_not_first_failure(tmp_path):
    src = ("#include <cstdio>\n"
           "int g = 3;\n"
           "int f(int x) {\n"
           "    auto& y = x;\n"  # FR-259 accepts `auto y = x;` (N22); a reference stays out
           "    x = y;\n"
           "    int **p = nullptr;\n"  # FR-261 accepts `int *p = &x;`; a T** stays out
           "    do { x--; } while (x);\n"
           "    return x > 0 ? x++ : -x;\n"  # FR-259 accepts `?:`; `x++` beside `x` stays out
           "}\n"
           "int main() {\n"
           "    std::printf(\"%x\\n\", f(1));\n"
           "    return 0;\n"
           "}\n")
    assert errs(tmp_path, src) == [
        ("2:1", "unsupported: global variable"),
        ("4:5", "unsupported: variable of type 'int &'"),
        ("6:5", "unsupported: variable of type 'int **'"),
        ("7:5", "unsupported: do-while loop"),
        ("8:20", "unsupported: increment/decrement of 'x' used as a value in a statement that also uses 'x' "
                 "(C++ leaves the order unsequenced or unspecified)"),
        ("11:5", "unsupported: printf conversion '%x'"),
    ]


def test_non_canonical_for_is_residual(tmp_path):
    e = errs(tmp_path, "void f(int n) {\n    for (int i = 0; i <= n; ++i) {}\n    for (int i = 0; i < n; i += 2) {}\n}\n")
    assert e == [("2:5", "unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`"),
                 ("3:5", "unsupported: `for` loop outside the canonical counting form `for (int i = a; i < b; ++i)`")]


def test_counting_for_body_may_not_assign_bound(tmp_path):
    e = errs(tmp_path, "void f(int n) {\n    for (int i = 0; i < n; ++i) { n = 2; }\n}\n")
    assert e == [("2:5", "unsupported: counting loop body assigns or shadows the bound 'n'")]


def test_main_rules(tmp_path):
    e = errs(tmp_path, "int main(int argc, char **argv) {\n    if (argc) return 1;\n    return 0;\n}\n")
    assert e == [("1:5", "unsupported: main with parameters"),
                 ("1:27", "unsupported: parameter of type 'char **'"),
                 ("2:15", "unsupported: return inside main other than the final `return 0;`")]
