# Pins FR-257 (value-semantic structs and non-virtual member functions) at
# the unit level.  The invariants, and why each is load-bearing:
#  * a struct is a NAMED value type: its Kernel type is the ABSOLUTE path
#    (`a::P`), resolved on import (clang's canonical spelling, or C++'s own
#    innermost-namespace-first lookup for a written name) and by Rust's module
#    rules in rs_parse -- so `P` inside namespace a and `crate::a::P` are one
#    type, and a root `P` is another;
#  * structs are NOT Copy: a C++ copy is an explicit Clone, a bare variable in
#    a consuming position is a move (FR-251's rule, unchanged), and moving out
#    of a data member is refused;
#  * member functions keep their receiver kind (const <=> `&self`,
#    non-const <=> `&mut self`, static <=> associated fn, a
#    member-initializer-list constructor <=> `fn new`), and a call's receiver
#    obeys the alias rules measured on rustc 1.98's two-phase borrows;
#  * normal form N15 (struct definitions in topological-then-path order) makes
#    law 1 hold whatever order either source declares them in;
#  * a counting loop's `x.size()` bound is protected over ACCESS PATHS, which
#    closes an FR-251 miscompile (`i < v[0].size()` with `v[0].push_back`);
#  * every refused neighbour is a located error with its exact wording.
import shutil

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
HEAD = "#include <cstdio>\n#include <string>\n#include <utility>\n#include <vector>\n"


def imp(tmp_path, src, name="t.cpp"):
    p = tmp_path / name
    p.write_text(HEAD + src)
    return cli.load_cpp(str(p), "clang++")


def rs_errs(src):
    prog, errs = RP.rs_parse_all(src)
    return [("%s" % loc, m) for loc, m in errs]


def cxx_roundtrip(tmp_path, prog):
    text = CP.cxx_print(prog)
    p = tmp_path / "rt.cpp"
    p.write_text(text)
    back, errs = cli.load_cpp(str(p), "clang++")
    assert errs == [], errs
    return back, text


I32 = K.I32
SP = K.StructT("geo::P")


def point_program():
    """A hand-built Kernel: a struct in a namespace with every receiver kind."""
    me = K.Var("self")
    fields = (K.FieldDef("x", I32), K.FieldDef("tag", K.STRING))
    methods = (
        K.Func("new", (K.Param("x", I32, True),), SP,
               K.Block((K.Return(K.StructLit("geo::P", (K.Var("x"), K.StrNew()))),)), None, ("geo",), False,
               "P", "ctor"),
        K.Func("get", (), I32, K.Block((K.Return(K.Field(me, "x")),)), None, ("geo",), False, "P", "ref"),
        K.Func("bump", (K.Param("k", I32, True),), K.UNIT,
               K.Block((K.CompoundStore("add", K.Field(me, "x"), K.Var("k")),)), None, ("geo",), False, "P", "mut"),
        K.Func("zero", (), I32, K.Block((K.Return(K.IntLit(0, I32)),)), None, ("geo",), False, "P", "static"),
    )
    sd = K.StructDef("P", fields, methods, None, ("geo",))
    p = K.Var("p")
    main = K.Func("main", (), K.UNIT, K.Block((
        K.Let("p", SP, True, K.Call("geo::P::new", (K.IntLit(3, I32),))),
        K.ExprStmt(K.MethodCall(p, "bump", (K.MethodCall(p, "get", ()),))),
        K.Print((K.Spec("d", K.Binary("add", K.Field(p, "x"), K.Call("geo::P::zero", ()))), K.Text("\n"))),
    )))
    return TC.normalize(K.Program((sd, main)))


# ------------------------------------------------------------------ Kernel

def test_handbuilt_kernel_is_wellformed_and_rust_law1():
    prog = point_program()
    assert TC.check(prog) == []
    text = RSP.rs_print(prog)
    assert "pub fn new(mut x: i32) -> P {" in text
    assert "        return P { x, tag: String::new() };" in text
    assert "pub fn get(&self) -> i32 {" in text and "pub fn bump(&mut self, mut k: i32) {" in text
    assert "    p.bump(p.get());" in text  # two-phase borrow: the argument READS the receiver
    assert "crate::geo::P::zero()" in text
    back = RP.rs_parse(text)
    assert back == prog
    assert RSP.rs_print(back) == text


@needs_clang
def test_handbuilt_kernel_cxx_law1(tmp_path):
    prog = point_program()
    back, text = cxx_roundtrip(tmp_path, prog)
    assert back == prog
    assert "    P(const int x);" not in text and "    P(int x);" in text
    assert "P::P(int x) : x(x), tag(std::string{}) {}" in text
    assert "int P::get() const {" in text and "    return this->x;" in text
    assert "    static int zero();" in text
    assert "    ::geo::P p = ::geo::P(3);" in text
    assert CP.cxx_print(back) == text


def test_struct_is_not_copy_and_has_no_equality():
    assert not K.is_copy(SP) and K.is_owned_nc(SP) and K.elem_ok(SP)
    sigs = TC.sigs_of(point_program())
    sc = TC.Scope()
    sc.declare("p", SP, True)
    with pytest.raises(TC.KernelTypeError, match="operator eq on geo::P and geo::P"):
        TC.type_of(K.Binary("eq", K.Var("p"), K.Var("p")), sc, sigs)
    with pytest.raises(TC.KernelTypeError, match="operator eq"):
        TC.type_of(K.Binary("eq", K.VecLit(SP, ()), K.VecLit(SP, ())), sc, sigs)


def test_n15_struct_order_is_source_independent():
    a = K.StructDef("A", (K.FieldDef("b", K.StructT("B")),), ())
    b = K.StructDef("B", (K.FieldDef("n", I32),), ())
    c = K.StructDef("C", (K.FieldDef("v", K.VecT(K.StructT("A"))),), ())
    orders = [(a, b, c), (c, b, a), (b, c, a)]
    norm = [TC.normalize(K.Program(o)) for o in orders]
    assert all(n == norm[0] for n in norm)
    # B before A (A holds a B by value); C (a vector of A) needs nothing first
    assert [s.name for s in TC.struct_order([a, b, c])] == ["B", "A", "C"]


def test_by_value_recursion_is_refused_through_vec_is_not():
    selfish = K.StructDef("S", (K.FieldDef("me", K.StructT("S")),), ())
    tree = K.StructDef("T", (K.FieldDef("kids", K.VecT(K.StructT("T"))),), ())
    assert ("struct 'S' contains itself by value (infinite size)" in
            [m for _, m in TC.check(K.Program((selfish,)))])
    assert TC.check(K.Program((tree,))) == []


# ------------------------------------------------------------------ C++ side

@needs_clang
def test_cxx_struct_spellings_one_kernel(tmp_path):
    prog, errs = imp(tmp_path, (
        "struct P { int x; long long y; };\n"
        "int main() {\n"
        "    P a = {1, 2LL};\n"
        "    P b{1, 2LL};\n"
        "    P c = P{1, 2LL};\n"
        "    std::printf(\"%d\\n\", a.x + b.x + c.x);\n"
        "    return 0;\n"
        "}\n"))
    assert errs == []
    lets = [s for s in prog.funcs()[0].body.stmts if isinstance(s, K.Let)]
    assert [s.init for s in lets] == [K.StructLit("P", (K.IntLit(1, I32), K.IntLit(2, K.I64)))] * 3


@needs_clang
def test_cxx_ctor_spellings_one_kernel(tmp_path):
    prog, errs = imp(tmp_path, (
        "struct Q { int a; int b; Q(int x, int y) : b(y), a(x * 2) {} };\n"
        "int main() {\n"
        "    Q a(1, 2);\n"
        "    Q b{1, 2};\n"
        "    Q c = Q(1, 2);\n"
        "    Q d = {1, 2};\n"
        "    std::printf(\"%d\\n\", a.a + b.a + c.b + d.b);\n"
        "    return 0;\n"
        "}\n"))
    assert errs == []
    call = K.Call("Q::new", (K.IntLit(1, I32), K.IntLit(2, I32)))
    assert [s.init for s in prog.funcs()[0].body.stmts if isinstance(s, K.Let)] == [call] * 4
    ctor = prog.structs()[0].methods[0]
    # written `b(y), a(x * 2)`: clang lists (and C++ runs) them in DECLARATION order
    assert ctor.recv == "ctor" and ctor.body.stmts[0].value.values == (
        K.Binary("mul", K.Var("x"), K.IntLit(2, I32)), K.Var("y"))


@needs_clang
def test_cxx_methods_receivers_and_this(tmp_path):
    prog, errs = imp(tmp_path, (
        "namespace a {\n"
        "struct C {\n"
        "    int n;\n"
        "    int get() const;\n"
        "    void add(int k) { n += k; this->n += get(); }\n"
        "    static int unit() { return 1; }\n"
        "};\n"
        "}\n"
        "int a::C::get() const { return n; }\n"
        "int main() { a::C c = {0}; c.add(a::C::unit()); std::printf(\"%d\\n\", c.get()); return 0; }\n"))
    assert errs == []
    sd = prog.structs()[0]
    assert sd.path == "a::C" and [(m.name, m.recv) for m in sd.methods] == [
        ("get", "ref"), ("add", "mut"), ("unit", "static")]
    me = K.Var("self")
    assert sd.methods[1].body.stmts == (
        K.CompoundStore("add", K.Field(me, "n"), K.Var("k")),
        K.CompoundStore("add", K.Field(me, "n"), K.MethodCall(me, "get", ())))
    main = prog.funcs()[0]
    assert main.body.stmts[1] == K.ExprStmt(K.MethodCall(K.Var("c"), "add", (K.Call("a::C::unit", ()),)))


@needs_clang
def test_cxx_written_type_lookup_inner_hides_outer(tmp_path):
    # `const Q&` inside namespace a has no desugared spelling in clang's JSON;
    # C++ finds a::Q before ::Q, and so must the import
    prog, errs = imp(tmp_path, (
        "struct Q { int root; };\n"
        "namespace a {\n"
        "struct Q { int inner; };\n"
        "int f(const Q& q) { return q.inner; }\n"
        "Q g(int k) { return Q{k}; }\n"
        "}\n"
        "int main() { std::printf(\"%d\\n\", a::f(a::g(3))); return 0; }\n"))
    assert errs == []
    f = [x for x in prog.funcs() if x.path == "a::f"][0]
    g = [x for x in prog.funcs() if x.path == "a::g"][0]
    assert f.params[0].ty == K.RefT(K.StructT("a::Q"), False)
    assert g.ret == K.StructT("a::Q")


@needs_clang
def test_cxx_copy_vs_move_of_structs(tmp_path):
    prog, errs = imp(tmp_path, (
        "struct S { std::string s; };\n"
        "S keep(S x) { return x; }\n"
        "int main() {\n"
        "    S a = {std::string(\"a\")};\n"
        "    S b = a;\n"
        "    S c = std::move(a);\n"
        "    S d = keep(b);\n"
        "    std::printf(\"%s %s\\n\", c.s.c_str(), d.s.c_str());\n"
        "    return 0;\n"
        "}\n"))
    assert errs == []
    lets = [s.init for s in prog.funcs()[1].body.stmts if isinstance(s, K.Let)]
    assert lets[1] == K.Clone(K.Var("a"))
    assert lets[2] == K.Var("a")
    assert lets[3] == K.Call("keep", (K.Clone(K.Var("b")),))
    assert prog.funcs()[0].body.stmts[0] == K.Return(K.Var("x"))  # C++17 implicit move


@needs_clang
@pytest.mark.parametrize("body,expected", [
    ("std::string t = std::move(p.s);", "std::move of something other than a variable"),
    ("P q;", "struct variable without an initializer (default-initialized data members)"),
    ("P q = P();", "value-initialized or default-initialized struct (`S()`, `S s;`)"),
    # (an omitted std::string member is value-initialized -- `{1}` is
    # `{1, std::string{}}`, in the subset -- an omitted scalar is zero-filled)
    ("P q = {};", "aggregate initialization that omits data members"),
    ("int y = make().x;", "member access on a temporary"),
    ("int y = make().get();", "member function call on a temporary"),
    ("int y = p.zero();", "static member function called through an object"),
    ("p.take(p);", "'p' is passed by reference to 'take', a non-const member function called on it (aliasing)"),
    ("std::vector<P> v = {p}; v[0].set(v[0].get());",
     "non-const member function 'set' is called on an element of 'v' with an argument that reads 'v' "
     "(a second borrow in Rust)"),
])
def test_cxx_struct_use_rejections(tmp_path, body, expected):
    src = ("struct P {\n"
           "    int x;\n"
           "    std::string s;\n"
           "    int get() const { return x; }\n"
           "    void set(int k) { x = k; }\n"
           "    void take(const P& o) { x += o.x; }\n"
           "    static int zero() { return 0; }\n"
           "};\n"
           "P make() { P p = {1, \"m\"}; return p; }\n"
           "int main() {\n"
           "    P p = {2, \"p\"};\n"
           "    " + body + "\n"
           "    return 0;\n"
           "}\n")
    prog, errs = imp(tmp_path, src)
    assert prog is None
    assert ["unsupported: " + expected] == [m for _, m in errs]


@needs_clang
def test_cxx_len_bound_over_access_paths(tmp_path):
    # FR-251 accepted the first loop and the Rust printed `4` where C++ prints
    # `5`: the bound `v[0].size()` grows inside the body
    _, errs = imp(tmp_path, (
        "int main() {\n"
        "    std::vector<std::vector<int>> v = {{1, 2}, {3}};\n"
        "    for (std::size_t i = 0; i < v[0].size(); ++i) { if (i < 3) { v[0].push_back(5); } }\n"
        "    std::printf(\"%zu\\n\", v[0].size());\n"
        "    return 0;\n"
        "}\n"))
    assert [m for _, m in errs] == ["unsupported: counting loop body modifies or shadows 'v', whose size is the bound"]
    prog, errs = imp(tmp_path, (
        "struct B { std::vector<int> items; int n; };\n"
        "int main() {\n"
        "    B b = {{4, 5}, 0};\n"
        "    for (std::size_t i = 0; i < b.items.size(); ++i) { b.items[i] += 1; b.n += 1; }\n"
        "    std::printf(\"%d\\n\", b.n);\n"
        "    return 0;\n"
        "}\n"), "ok.cpp")
    assert errs == []


def test_ctor_call_for_effect_is_refused():
    sd = K.StructDef("Q", (K.FieldDef("a", I32),), (
        K.Func("new", (K.Param("v", I32, True),), K.StructT("Q"),
               K.Block((K.Return(K.StructLit("Q", (K.Var("v"),))),)), None, (), False, "Q", "ctor"),))
    main = K.Func("main", (), K.UNIT, K.Block((K.ExprStmt(K.Call("Q::new", (K.IntLit(1, I32),))),)))
    # C++ `Q(x);` would DECLARE a variable x (the most vexing parse)
    assert [m for _, m in TC.check(K.Program((sd, main)))] == [
        "unsupported: constructor call evaluated only for its effect"]


# ------------------------------------------------------------------ Rust side

S_DEF = "#[derive(Clone)]\nstruct S {\n    pub x: i32,\n}\n"


@pytest.mark.parametrize("src,expected", [
    ("struct S {\n    pub x: i32,\n}\n",
     [("1:1", "unsupported: struct without `#[derive(Clone)]` (the subset's structs are Clone, like every "
              "copyable C++ aggregate)")]),
    ("#[derive(Clone, Copy)]\nstruct S {\n    pub x: i32,\n}\n",
     [("1:1", "unsupported: attribute other than `#[derive(Clone)]` on a struct")]),
    ("#[derive(Clone)]\nstruct S {\n    x: i32,\n}\n",
     [("3:5", "unsupported: private field (the subset's data members are public: `pub x`)")]),
    ("#[derive(Clone)]\nstruct S(i32);\n", [("2:9", "unsupported: tuple or unit struct")]),
    ("mod a {\n    #[derive(Clone)]\n    struct S {\n        pub x: i32,\n    }\n}\n\n"
     "fn f(s: &a::S) -> i32 {\n    return s.x;\n}\n",
     [("8:1", "unsupported: 'a::S' is internal to namespace 'a' (a C++ anonymous-namespace member, a private "
              "Rust `struct`) and is named outside it")]),
    (S_DEF + "impl S {\n    pub fn m(self) -> i32 {\n        return 1;\n    }\n}\n",
     [("6:14", "unsupported: member function taking `self` by value (C++ has no by-value `this`)")]),
    (S_DEF + "impl S {\n    fn m(&self) -> i32 {\n        return 1;\n    }\n}\n",
     [("6:5", "unsupported: private member function (the subset spells it `pub fn`)")]),
    (S_DEF + "impl Clone for S {\n}\n", [("5:12", "unsupported: trait implementation (`impl Trait for S`)")]),
    (S_DEF + "impl S {\n}\nimpl S {\n}\n",
     [("7:1", "unsupported: second `impl S` block (the subset spells one per struct, like the one C++ class "
              "definition)")]),
    (S_DEF + "fn main() {\n    let s: S = S { x: 1, ..S { x: 2 } };\n}\n",
     [("6:26", "unsupported: struct update syntax `..base`")]),
    ("#[derive(Clone)]\nstruct S {\n    pub x: i32,\n    pub y: i32,\n}\n"
     "fn main() {\n    let s: S = S { y: 1, x: 2 };\n}\n",
     [("7:16", "unsupported: the literal of struct 'S' does not list every field in declaration order (Rust "
               "evaluates field initializers in written order, C++ in declaration order)")]),
    ("fn f(s: Missing) {}\n", [("1:9", "unsupported: type `Missing`")]),
    (S_DEF + "impl S {\n    pub fn len(&self) -> i32 {\n        return self.x;\n    }\n}\n",
     [("6:9", "identifier 'len' is reserved by the canonical Rust spelling (a method of Vec/String or of the "
              "derived Clone)")]),
    ("#[derive(Clone)]\nstruct Iterator {\n    pub x: i32,\n}\n",
     [("2:1", "struct name: identifier 'Iterator' names a Rust prelude item")]),
    (S_DEF + "impl S {\n    pub fn new(x: i32) -> S {\n        return S { x };\n    }\n}\n"
     "fn main() {\n    let s: S = S { x: 1 };\n}\n",
     [("11:16", "unsupported: aggregate initialization of 'S', which has a constructor (C++ would call the "
                "constructor)")]),
    ("mod a {\n    #[derive(Clone)]\n    pub struct S {\n        pub x: i32,\n    }\n\n    pub mod b {\n"
     "        pub fn f(s: &S) -> i32 {\n            return s.x;\n        }\n    }\n}\n",
     [("8:22", "unsupported: `S` is not defined in module `a::b`: a Rust path does not search enclosing "
               "modules (C++ unqualified lookup would find `a::S`); spell it `crate::a::S`")]),
])
def test_rust_rejections_are_located(src, expected):
    assert rs_errs(src) == expected


RECV = ("#[derive(Clone)]\nstruct C {\n    pub n: i32,\n    pub v: Vec<i32>,\n}\n\n"
        "impl C {\n"
        "    pub fn get(&self) -> i32 {\n        return self.n;\n    }\n\n"
        "    pub fn set(&mut self, k: i32) {\n        self.n = k;\n    }\n\n"
        "    pub fn take(&mut self, o: &C) {\n        self.n += o.n;\n    }\n\n"
        "    pub fn again(&mut self) {\n        self.set(self.n + 1);\n        self.set(self.get());\n    }\n"
        "}\n\n"
        "fn peek(c: &C) -> i32 {\n    return c.n;\n}\n\n"
        "fn bump(c: &mut C) -> i32 {\n    c.n += 1;\n    return c.n;\n}\n\n"
        "fn main() {\n    let mut c: C = C { n: 1, v: vec![2] };\n    let mut cs: Vec<C> = vec![c.clone()];\n"
        "    %s\n}\n")


@pytest.mark.parametrize("stmt", [
    "c.set(c.get());",            # measured: compiles (two-phase borrow, the argument reads)
    "c.set(peek(&c));",           # a shared borrow inside the argument ends before activation
    "c.set(c.v.len() as i32);",
    "c.again();",
    "cs[0usize].set(7);",
])
def test_rust_receiver_shapes_rustc_accepts(stmt):
    prog, errs = RP.rs_parse_all(RECV % stmt)
    assert errs == []
    text = RSP.rs_print(prog)
    assert RP.rs_parse(text) == prog


@pytest.mark.parametrize("stmt,expected", [
    ("c.take(&c);", "unsupported: 'c' is passed by reference to 'take', a non-const member function called on it "
                    "(aliasing)"),
    ("let k: i32 = c.get() + bump(&mut c);",
     "unsupported: 'c' is passed to 'bump' by non-const reference and also used elsewhere in the statement (C++ "
     "leaves the order unspecified)"),
    ("cs[0usize].set(cs[0usize].get());",
     "unsupported: non-const member function 'set' is called on an element of 'cs' with an argument that reads "
     "'cs' (a second borrow in Rust)"),
    ("let k: i32 = c.n + c.get() + { c.set(1); 0 };", None),
])
def test_rust_receiver_shapes_rustc_rejects(stmt, expected):
    errs = [m for _, m in rs_errs(RECV % stmt)]
    if expected is None:
        assert errs  # a block expression is not in the subset at all
    else:
        assert expected in errs


@needs_clang
def test_internal_struct_both_sides(tmp_path):
    # a struct in an anonymous namespace inside a named one <=> a private
    # `struct` in a `mod` (its fields and member functions stay `pub`); root
    # anonymity is erased (N14)
    prog, errs = imp(tmp_path, (
        "namespace codec {\n"
        "namespace {\n"
        "struct State { int pos; int next() { pos += 1; return pos; } };\n"
        "}\n"
        "int run(int k) { State s = {k}; return s.next(); }\n"
        "}\n"
        "namespace { struct Root { int r; }; }\n"
        "int main() { Root r = {2}; std::printf(\"%d\\n\", codec::run(r.r)); return 0; }\n"))
    assert errs == []
    by = {sd.path: sd for sd in prog.structs()}
    assert by["codec::State"].internal and not by["Root"].internal
    text = RSP.rs_print(prog)
    assert "    struct State {\n        pub pos: i32,\n    }" in text
    assert "        pub fn next(&mut self) -> i32 {" in text
    assert RP.rs_parse(text) == prog
    back, ctext = cxx_roundtrip(tmp_path, prog)
    assert back == prog
    assert "namespace codec {\n\nnamespace {\n\nstruct State {" in ctext
    assert "int State::next() {" in ctext  # defined from the enclosing namespace


def test_rust_struct_paths_resolve_like_calls():
    src = ("mod m {\n"
           "    #[derive(Clone)]\n    pub struct T {\n        pub n: i32,\n    }\n\n"
           "    pub mod k {\n"
           "        pub fn f(a: &super::T, b: &crate::m::T) -> i32 {\n            return a.n + b.n;\n        }\n"
           "    }\n"
           "}\n\n"
           "fn main() {\n"
           "    let t: m::T = m::T { n: 1 };\n"
           "    print!(\"{}\\n\", m::k::f(&t, &crate::m::T { n: 2 }));\n"
           "}\n")
    prog = RP.rs_parse(src)
    f = [x for x in prog.funcs() if x.path == "m::k::f"][0]
    assert [p.ty for p in f.params] == [K.RefT(K.StructT("m::T"), False)] * 2
    assert prog.funcs()[-1].body.stmts[0].ty == K.StructT("m::T")


def test_rust_struct_literal_in_condition_needs_parentheses():
    src = (S_DEF + "fn f(s: S) -> bool {\n    return s.x > 0;\n}\n\n"
           "fn main() {\n    if f(S { x: 1 }) {\n        print!(\"y\\n\");\n    }\n}\n")
    assert RP.rs_parse(src)
