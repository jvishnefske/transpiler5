# Pins FR-251 (std::vector + std::string, references, range-for) at the unit
# level.  The invariants, and why each is load-bearing:
#  * copy vs move is read from clang's AST (the constructor / overload the
#    compiler chose), never guessed -- a copy that became a move would
#    silently empty a vector the C++ program goes on reading;
#  * a move is accepted only with NO later mention, never inside a loop that
#    re-runs it, never out of a reference or an element -- Rust would refuse
#    to compile those, and C++ would read a moved-from object;
#  * aliasing a `T&` argument (with another argument, or elsewhere in the
#    statement), mutating the vector a range-for iterates, and writing an
#    element through an index that reads the same vector are LOCATED
#    residuals -- never rustc errors on accepted input;
#  * the String ASCII invariant: non-ASCII literals are residuals, so byte
#    indexing and substr never split a character;
#  * the normal forms N7-N11 make parse(print(k)) == k hold for every
#    spelling the printers emit (`&x`, `*x`, `.clone()` in copied positions,
#    `{}` of a String, `Vec::new()`), and each exact rejection wording is
#    pinned because a refusal's wording is part of the ranking instrument.
import shutil

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

V = K.Var
I = lambda v, t=K.I32: K.IntLit(v, t)  # noqa: E731
VI = K.VecT(K.I32)
VS = K.VecT(K.STRING)
needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")


def prog(*stmts, params=(), ret=K.UNIT, extra=()):
    return K.Program((K.Func("f", params, ret, K.Block(tuple(stmts))),) + tuple(extra))


def msgs(p, **kw):
    return [m for _, m in TC.check(p, **kw)]


# ------------------------------------------------------------------ typing

def test_reference_variables_have_the_referent_type():
    p = prog(K.Let("n", K.USIZE, False, K.Len(V("v"))),
             K.Assign("k", K.Binary("add", V("k"), I(1))),
             params=(K.Param("v", K.RefT(VI, False), False), K.Param("k", K.RefT(K.I32, True), False)))
    assert msgs(p) == []


def test_copy_is_explicit_and_a_bare_owned_variable_moves():
    ok = prog(K.Let("a", VI, True, K.VecLit(K.I32, (I(1),))),
              K.Let("b", VI, False, K.Clone(V("a"))),
              K.Let("c", VI, False, V("a")))
    assert msgs(ok) == []
    used = prog(K.Let("a", VI, True, K.VecLit(K.I32, ())),
                K.Let("c", VI, False, V("a")),
                K.Push(V("a"), I(1)))
    assert msgs(used) == ["unsupported: 'a' is used after it is moved (std::move(a) <=> a requires no later "
                          "mention of a)"]


def test_move_rules():
    imm = prog(K.Let("a", VI, False, K.VecLit(K.I32, ())), K.Let("c", VI, False, V("a")))
    assert msgs(imm) == ["unsupported: move of immutable 'a' (C++ cannot move a const object; it would copy)"]
    ref = prog(K.Let("c", VI, False, V("p")), params=(K.Param("p", K.RefT(VI, True), False),))
    assert msgs(ref) == ["unsupported: move out of reference 'p' (the subset copies it: C++ copy, Rust "
                         "`.clone()`)"]
    elem = prog(K.Let("w", K.VecT(VI), True, K.VecLit(VI, ())),
                K.Let("c", VI, False, K.Index(V("w"), I(0, K.USIZE))))
    assert msgs(elem) == ["unsupported: move out of an indexed element (the subset copies it: C++ copy, Rust "
                          "`.clone()`)"]
    loop = prog(K.Let("a", VI, True, K.VecLit(K.I32, ())),
                K.Let("all", K.VecT(VI), True, K.VecLit(VI, ())),
                K.For("i", I(0), I(3), K.Block((K.Push(V("all"), V("a")),))))
    assert msgs(loop) == ["unsupported: move of 'a' inside a loop it was declared outside of"]


def test_return_move_ends_the_path():
    # `if (c) return v; v.push_back(1); return v;` is fine in both languages
    p = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())),
             K.If(V("c"), K.Block((K.Return(V("v")),))),
             K.Push(V("v"), I(1)),
             K.Return(V("v")),
             params=(K.Param("c", K.BOOL, False),), ret=VI)
    assert msgs(p) == []


def test_reference_argument_aliasing():
    g = K.Func("g", (K.Param("a", K.RefT(VI, True), False), K.Param("b", K.RefT(VI, False), False)), K.UNIT,
               K.Block(()))
    p = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())), K.ExprStmt(K.Call("g", (V("v"), V("v")))),
             extra=(g,))
    assert msgs(p) == ["unsupported: 'v' is passed to 'g' by non-const reference and also used by another "
                       "argument of the same call (aliasing)"]
    h = K.Func("h", (K.Param("k", K.RefT(K.I32, True), False),), K.I32, K.Block((K.Return(I(1)),)))
    q = prog(K.Let("k", K.I32, True, I(1)),
             K.Let("r", K.I32, False, K.Binary("add", K.Call("h", (V("k"),)), V("k"))), extra=(h,))
    assert msgs(q) == ["unsupported: 'k' is passed to 'h' by non-const reference and also used elsewhere in "
                       "the statement (C++ leaves the order unspecified)"]


def test_range_for_alias_and_self_index():
    push = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())),
                K.ForEach("x", False, "copy", V("v"), K.Block((K.Push(V("v"), V("x")),))))
    assert msgs(push) == ["unsupported: the body of a range-for over 'v' modifies 'v' (iterator invalidation in "
                          "C++, a borrow conflict in Rust)"]
    mut = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())),
               K.ForEach("x", False, "mut", V("v"),
                         K.Block((K.Assign("x", K.Cast(K.I32, K.Len(V("v")))),))))
    assert msgs(mut) == ["unsupported: the body of a mutating range-for over 'v' uses 'v' (iterator "
                         "invalidation in C++, a second borrow in Rust)"]
    back = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())),
                K.Store(K.Index(V("v"), K.Binary("sub", K.Len(V("v")), I(1, K.USIZE))), I(5)))
    assert msgs(back) == ["unsupported: an element of 'v' is written through an index that reads 'v' (a second "
                          "borrow in Rust)"]


def test_string_rows_typing():
    s = K.STRING
    p = prog(K.Let("a", s, True, K.StrFrom("ab")),
             K.Let("b", s, False, K.Binary("add", K.Clone(V("a")), K.StrLit("c"))),
             K.Let("e", K.BOOL, False, K.Binary("eq", V("b"), K.StrLit("abc"))),
             K.Let("l", K.BOOL, False, K.Binary("lt", V("a"), V("b"))),
             K.Let("c", K.I8, False, K.ByteAt(V("b"), I(0, K.USIZE))),
             K.Let("t", s, False, K.Substr(V("b"), I(1, K.USIZE), None)),
             K.Let("n", s, False, K.ToStr(I(42, K.I64))),
             K.PushStr(V("a"), V("t")),
             K.PushByte(V("a"), I(65, K.I8)))
    assert msgs(p) == []
    bad = prog(K.Let("a", s, False, K.StrFrom("café")),
               K.Let("l", K.BOOL, False, K.Binary("lt", V("a"), K.StrLit("b"))),
               K.Let("t", s, False, K.ToStr(K.FloatLit("1.5"))))
    assert msgs(bad) == [
        "unsupported: non-ASCII string literal (every String is ASCII so byte indexing never splits a character)",
        "operator lt on String and &str",
        "to_string of f64 (only int, long long, unsigned, unsigned long long, std::size_t)"]


def test_vector_of_bool_is_excluded():
    assert msgs(prog(K.Let("v", K.VecT(K.BOOL), True, K.VecLit(K.BOOL, ())))) == [
        "variable type Vec<bool>", "vector of bool"]


def test_len_bound_requires_an_unmodified_vector():
    p = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())),
             K.For("i", I(0, K.USIZE), K.Len(V("v")), K.Block((K.Push(V("v"), I(1)),))))
    assert msgs(p) == ["unsupported: counting loop body modifies or shadows 'v', whose size is the bound"]


# ------------------------------------------------------------------ normal forms

def test_n8_clone_in_a_copied_position_is_dropped():
    surf = prog(K.Let("s", K.STRING, False, K.StrFrom("x")),
                K.Let("v", VS, False, K.VecLit(None, (K.Clone(V("s")),))))
    n = TC.normalize(surf)
    assert n.funcs()[0].body.stmts[1].init == K.VecLit(K.STRING, (V("s"),))  # N8 + N11


def test_n9_returning_a_clone_of_a_mutable_local_is_a_move():
    surf = prog(K.Let("v", VI, True, K.VecLit(K.I32, ())), K.Return(K.Clone(V("v"))), ret=VI)
    assert TC.normalize(surf).funcs()[0].body.stmts[1] == K.Return(V("v"))
    imm = prog(K.Let("v", VI, False, K.VecLit(K.I32, ())), K.Return(K.Clone(V("v"))), ret=VI)
    assert TC.normalize(imm).funcs()[0].body.stmts[1] == K.Return(K.Clone(V("v")))


def test_n12_returning_an_immutable_local_copies():
    p = RP.rs_parse("fn f() -> String {\n    let t: String = String::new();\n    return t;\n}\n")
    assert p.funcs()[0].body.stmts[1] == K.Return(K.Clone(V("t")))
    assert "return t.clone();" in RSP.rs_print(p)


def test_n10_print_of_a_string_is_percent_s():
    p = RP.rs_parse('fn main() {\n    let s: String = String::from("a");\n    print!("{}\\n", s);\n}\n')
    assert p.funcs()[0].body.stmts[1].segs[0] == K.Spec("s", V("s"))


# ------------------------------------------------------------------ Rust side

def test_canonical_rust_spellings():
    src = ("fn f(v: &mut Vec<i32>, s: &String, k: &mut u32) -> String {\n"
           "    v.push((*k) as i32);\n"
           "    *k = (*k).wrapping_add(1u32);\n"
           "    let mut t: String = s.clone() + &v.len().to_string();\n"
           "    return t;\n"
           "}\n\n"
           "fn main() {\n"
           "    let mut v: Vec<i32> = Vec::new();\n"
           "    let s: String = String::from(\"ab\");\n"
           "    let mut k: u32 = 1u32;\n"
           "    let r: String = f(&mut v, &s, &mut k);\n"
           "    let n: usize = Vec::<i32>::new().len();\n"
           "    print!(\"{} {}\\n\", r, n);\n"
           "}\n")
    p = RP.rs_parse(src)
    assert RSP.rs_print(p) == src
    f = p.funcs()[0]
    assert f.body.stmts[1] == K.CompoundAssign("add", "k", I(1, K.U32))
    assert f.body.stmts[2].init == K.Binary("add", K.Clone(V("s")), K.ToStr(K.Len(V("v"))))


@pytest.mark.parametrize("src,expected", [
    ("fn g(v: &Vec<i32>) {\n}\n\nfn main() {\n    let v: Vec<i32> = Vec::new();\n    g(v);\n}\n",
     [("6:7", "unsupported: argument 1 of 'g' binds a reference; the subset spells it `&x`")]),
    ("fn g(v: &mut Vec<i32>) {\n}\n\nfn main() {\n    let mut v: Vec<i32> = Vec::new();\n    g(&v);\n}\n",
     [("6:7", "unsupported: argument 1 of 'g' is not spelled the canonical way (`&mut x`)")]),
    ("fn main() {\n    let x: i32 = 1;\n    let y: i32 = *x;\n}\n",
     [("3:19", "unsupported: `*` of 'x', which is not a reference")]),
    ("fn main() {\n    let s: String = String::new();\n    let c: u8 = s.as_bytes()[0usize];\n}\n",
     [("3:29", "unsupported: byte read other than `s.as_bytes()[i] as i8` (std::string's char is signed)")]),
    ("fn main() {\n    let mut v: Vec<i32> = Vec::new();\n    let n: usize = v.push(1);\n}\n",
     [("3:22", "unsupported: `.push()` used as a value")]),
    ("fn main() {\n    let v: Vec<i32> = vec![];\n}\n",
     [("2:23", "unsupported: `vec![]` (the subset spells the empty vector `Vec::new()`)")]),
    ("fn main() {\n    let v: Vec<i32> = vec![1];\n    let w: Vec<i32> = v;\n}\n",
     [("3:23", "unsupported: move of immutable 'v' (C++ cannot move a const object; it would copy)")]),
])
def test_rust_side_rejections_are_located(src, expected):
    prog_, errs = RP.rs_parse_all(src)
    assert prog_ is None
    assert [(str(l), m) for l, m in errs] == expected


# ------------------------------------------------------------------ C++ side

def imp(tmp_path, src):
    p = tmp_path / "t.cpp"
    p.write_text(src)
    return cli.load_cpp(str(p), "clang++")


HEAD = "#include <cstdio>\n#include <string>\n#include <utility>\n#include <vector>\n"


@needs_clang
def test_copy_and_move_are_read_from_the_ast(tmp_path):
    prog_, errs = imp(tmp_path, HEAD + (
        "std::vector<int> pass(std::vector<int> v) {\n    return v;\n}\n"
        "std::vector<int> copy(const std::vector<int>& v) {\n    return v;\n}\n"
        "int main() {\n"
        "    std::vector<int> a = {1, 2};\n"
        "    std::vector<int> b = a;\n"
        "    std::vector<int> c = std::move(a);\n"
        "    std::vector<std::vector<int>> all;\n"
        "    all.push_back(b);\n"
        "    all.push_back(std::move(c));\n"
        "    return 0;\n}\n"))
    assert errs == []
    pass_, copy_, main = prog_.funcs()
    assert pass_.body.stmts == (K.Return(V("v")),)
    assert copy_.body.stmts == (K.Return(K.Clone(V("v"))),)
    st = main.body.stmts
    assert st[1].init == K.Clone(V("a")) and st[2].init == V("a")
    assert st[4] == K.Push(V("all"), K.Clone(V("b"))) and st[5] == K.Push(V("all"), V("c"))


@needs_clang
def test_cxx_desugarings(tmp_path):
    prog_, errs = imp(tmp_path, HEAD + (
        "int main() {\n"
        "    std::vector<int> v = {1, 2};\n"
        "    std::string s = \"ab\";\n"
        "    int x = v.front() + v.back();\n"
        "    std::string t = \"<\" + s;\n"
        "    t += '>';\n"
        "    std::size_t n = s.length();\n"
        "    std::vector<std::vector<int>> g = {{1}, {2, 3}};\n"
        "    return 0;\n}\n"))
    assert errs == []
    st = prog_.funcs()[0].body.stmts
    z = I(0, K.USIZE)
    last = K.Binary("sub", K.Len(V("v")), I(1, K.USIZE))
    assert st[2].init == K.Binary("add", K.Index(V("v"), z), K.Index(V("v"), last))
    assert st[3].init == K.Binary("add", K.StrFrom("<"), V("s"))
    assert st[4] == K.PushByte(V("t"), I(62, K.I8))
    assert st[5].init == K.Len(V("s"))
    assert st[6].init == K.VecLit(VI, (K.VecLit(K.I32, (I(1),)), K.VecLit(K.I32, (I(2), I(3)))))


CXX_REJECTS = [
    ("int main() {\n    std::vector<int> a = {1};\n    std::vector<int> b = std::move(a);\n"
     "    a.push_back(2);\n    return 0;\n}\n",
     [("7:26", "unsupported: 'a' is used after it is moved (std::move(a) <=> a requires no later mention of a)")]),
    ("int main() {\n    const std::vector<int> a = {1};\n    std::vector<int> b = std::move(a);\n    return 0;\n}\n",
     [("7:26", "unsupported: std::move of a const object (it copies)")]),
    ("int main() {\n    std::string s = \"ab\";\n    s[0] = 'x';\n    return 0;\n}\n",
     [("7:5", "unsupported: assignment to a std::string element (Rust has no safe String byte write)")]),
    ("int main() {\n    std::vector<int> v = {1};\n    int x = v.at(0);\n    return 0;\n}\n",
     [("7:13", "unsupported: std::vector::at (bounds-checked access that throws; the subset has operator[])")]),
    ("int main() {\n    std::vector<bool> v = {true};\n    return 0;\n}\n",
     [("6:5", "unsupported: variable of type 'std::vector<bool>' (std::vector<bool> is a bit-packed proxy "
              "container)")]),
    ("int main() {\n    std::string s = std::to_string(1.5);\n    return 0;\n}\n",
     [("6:21", "unsupported: std::to_string of 'double' (only int, long long, unsigned, unsigned long long, "
               "std::size_t)")]),
    ("int main() {\n    std::vector<int> v;\n    v.resize(3);\n    return 0;\n}\n",
     [("7:5", "unsupported: std::vector::resize without a fill value")]),
    ("int main() {\n    std::string s = \"ab\";\n    int n = 0;\n    for (char c : s) {\n        n += c;\n    }\n"
     "    return 0;\n}\n",
     [("8:5", "unsupported: range-based for over a std::string")]),
    ("int main() {\n    std::string s = \"ab\";\n    bool b = s < \"b\";\n    return 0;\n}\n",
     [("7:18", "unsupported: ordering comparison of a std::string with a string literal")]),
    ("int main() {\n    std::vector<int> a = {1};\n    std::vector<int> b = {2};\n    bool x = a < b;\n"
     "    return 0;\n}\n",
     [("8:14", "unsupported: ordering comparison of vectors")]),
    ("std::vector<int> take(std::vector<int>& v) {\n    return std::move(v);\n}\nint main() {\n    return 0;\n}\n",
     [("6:12", "unsupported: move out of reference 'v' (the subset copies it: C++ copy, Rust `.clone()`)")]),
    ("int main() {\n    std::string s = \"ab\";\n    s += s;\n    return 0;\n}\n",
     [("7:5", "unsupported: appending 's' to itself")]),
]


@needs_clang
@pytest.mark.parametrize("body,expected", CXX_REJECTS)
def test_cxx_rejections_are_located(tmp_path, body, expected):
    prog_, errs = imp(tmp_path, HEAD + body)
    assert prog_ is None
    assert [(str(l), m) for l, m in errs] == expected


# ------------------------------------------------------------------ printers

def test_cxx_includes_follow_use():
    p = prog(K.Let("a", VI, True, K.VecLit(K.I32, ())), K.Let("b", VI, False, V("a")))
    text = CP.cxx_print(K.Program((K.Func("main", (), K.UNIT, p.funcs()[0].body),)))
    assert text.startswith("#include <cstdio>\n#include <utility>\n#include <vector>\n")
    assert "const std::vector<int> b = std::move(a);" in text


def test_bx_helpers_are_safe_and_only_when_used():
    p = prog(K.Let("s", K.STRING, True, K.StrNew()), K.PushByte(V("s"), I(65, K.I8)),
             K.Let("t", K.STRING, False, K.Substr(V("s"), I(0, K.USIZE), I(1, K.USIZE))))
    text = RSP.rs_print(K.Program((K.Func("main", (), K.UNIT, p.funcs()[0].body),)))
    assert "pub fn push_byte(" in text and "pub fn substr(" in text
    assert "substr_from" not in text and "fmt_f" not in text
    assert "unsafe" not in text
