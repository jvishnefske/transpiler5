# Pins FR-260 (C++ namespaces <=> Rust modules) at the unit level.  The
# invariants, and why each is load-bearing:
#  * every Kernel Call names an ABSOLUTE path, resolved on import -- by
#    clang's decl ids on the C++ side, by Rust's own module rules in
#    rs_parse.  A Kernel that kept the source SPELLING would change meaning
#    when a reopened namespace is merged (C++ lookup depends on declaration
#    order) or when a using-directive is dropped;
#  * the printers re-derive one spelling per reference: bare in the same
#    namespace, otherwise `::a::f` / `crate::a::f`, which neither language
#    resolves by searching enclosing scopes -- so "inner hides outer",
#    sibling namespaces with one function name, and calls into / out of
#    nested namespaces all resolve identically on both sides;
#  * normal forms N13 (a reopened namespace merges into its first block;
#    `namespace a::b` is `namespace a { namespace b`) and N14 (an anonymous
#    namespace at the root only changes linkage) make law 1 hold;
#  * the shapes that have no 1:1 spelling are LOCATED errors with exact
#    wording: a bare Rust call to an enclosing module's function (rustc
#    E0425; C++ would find it), an internal function referenced from outside
#    its namespace (rustc E0603; C++ accepts), inline namespaces, a named
#    namespace inside an anonymous one, reserved module names, a reopened
#    Rust module (E0428), restricted visibility and `use`.
import shutil

import pytest

from emitrust_bx import cli
from emitrust_bx import cxx_print as CP
from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP
from emitrust_bx import typecheck as TC

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
HEAD = "#include <cstdio>\n#include <string>\n"


def imp(tmp_path, src, name="t.cpp"):
    p = tmp_path / name
    p.write_text(src)
    return cli.load_cpp(str(p), "clang++")


def calls(prog):
    return [n.name for f in prog.funcs() for n in K.walk(f.body) if isinstance(n, K.Call)]


def errs_of(errs):
    return [("%s" % loc, m) for loc, m in errs]


def ret(v):
    return K.Block((K.Return(K.IntLit(v, K.I32)),))


def fn(name, ns=(), internal=False, body=None):
    return K.Func(name, (), K.I32, body or ret(1), None, ns, internal)


def call(path):
    return K.Block((K.Return(K.Call(path, ())),))


# ------------------------------------------------------------------ C++ side

@needs_clang
def test_cxx_lookup_is_resolved_to_absolute_paths(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "int f() { return 1; }\n"
        "namespace a {\n"
        "int f() { return 2; }\n"
        "namespace b {\n"
        "int f() { return 3; }\n"
        "int g() { return f() + a::f() + ::f(); }\n"  # inner hides outer
        "}\n"
        "}\n"
        "namespace c { int f() { return 4; } }\n"  # sibling, same leaf
        "int main() { std::printf(\"%d %d %d\\n\", a::b::g(), c::f(), f()); return 0; }\n"))
    assert errs == []
    assert [f.path for f in prog.funcs()] == ["f", "a::f", "a::b::f", "a::b::g", "c::f", "main"]
    assert calls(prog) == ["a::b::f", "a::f", "f", "a::b::g", "c::f", "f"]


@needs_clang
def test_declaration_order_lookup_survives_the_merge(tmp_path):
    # g's `f()` finds ::f because a::f is declared only in the REOPENED
    # block; merging the blocks (N13) must not retarget it.
    prog, errs = imp(tmp_path, HEAD + (
        "int f() { return 1; }\n"
        "namespace a { int g() { return f(); } }\n"
        "int k() { return 7; }\n"
        "namespace a { int f() { return 2; } int h() { return f(); } }\n"
        "int main() { std::printf(\"%d %d %d\\n\", a::g(), a::h(), k()); return 0; }\n"))
    assert errs == []
    assert [f.path for f in prog.funcs()] == ["f", "a::g", "a::f", "a::h", "k", "main"]
    assert calls(prog)[:2] == ["f", "a::f"]
    cxx = CP.cxx_print(prog)
    assert "int g() {\n    return ::f();\n}" in cxx
    assert "int h() {\n    return f();\n}" in cxx
    assert cxx.count("namespace a {") == 1
    assert cli.check_roundtrip(prog, "clang++") == []


@needs_clang
def test_nested_namespace_definition_and_out_of_line_member_normalize(tmp_path):
    a, e1 = imp(tmp_path, HEAD + (
        "namespace a::b { int f(); }\n"
        "int a::b::f() { return 5; }\n"
        "int main() { std::printf(\"%d\\n\", a::b::f()); return 0; }\n"), "a.cpp")
    b, e2 = imp(tmp_path, HEAD + (
        "namespace a {\nnamespace b {\nint f() { return 5; }\n}\n}\n"
        "int main() { std::printf(\"%d\\n\", ::a::b::f()); return 0; }\n"), "b.cpp")
    assert e1 == [] and e2 == []
    assert a == b
    assert a.funcs()[0].ns == ("a", "b")


@needs_clang
def test_using_and_aliases_of_user_namespaces_are_spelling_only(tmp_path):
    a, e1 = imp(tmp_path, HEAD + (
        "namespace a { namespace b { int f() { return 5; } int g() { return 6; } } }\n"
        "using namespace a;\n"
        "using a::b::g;\n"
        "namespace ab = a::b;\n"
        "int main() { std::printf(\"%d %d\\n\", ab::f(), g()); return 0; }\n"), "a.cpp")
    b, e2 = imp(tmp_path, HEAD + (
        "namespace a { namespace b { int f() { return 5; } int g() { return 6; } } }\n"
        "int main() { std::printf(\"%d %d\\n\", a::b::f(), a::b::g()); return 0; }\n"), "b.cpp")
    assert e1 == [] and e2 == []
    assert a == b


@needs_clang
def test_anonymous_namespace_is_internal_and_erased_at_the_root(tmp_path):
    prog, errs = imp(tmp_path, HEAD + (
        "namespace { int r() { return 1; } }\n"
        "namespace a {\nnamespace {\nint h() { return 2; }\n}\nint f() { return h(); }\n}\n"
        "int main() { std::printf(\"%d %d\\n\", r(), a::f()); return 0; }\n"))
    assert errs == []
    by = {f.path: f for f in prog.funcs()}
    assert by["r"].internal is False  # N14
    assert by["a::h"].internal is True
    assert by["a::f"].internal is False
    rs = RSP.rs_print(prog)
    assert "mod a {\n    fn h() -> i32 {" in rs
    assert "    pub fn f() -> i32 {\n        return h();" in rs
    cxx = CP.cxx_print(prog)
    assert "namespace a {\n\nnamespace {\n\nint h() {" in cxx
    assert "}  // namespace\n\nint f() {" in cxx
    assert cli.check_roundtrip(prog, "clang++") == []


@needs_clang
@pytest.mark.parametrize("body,expected", [
    ("inline namespace v1 { int f() { return 1; } }\n",
     [("4:1", "unsupported: inline namespace")]),
    ("namespace { namespace d { int f() { return 1; } } }\n",
     [("4:13", "unsupported: named namespace inside an anonymous namespace")]),
    ("namespace mod { int f() { return 1; } }\n",
     [("4:1", "unsupported: namespace name: identifier 'mod' is a Rust keyword")]),
    ("namespace Vec { int f() { return 1; } }\n",
     [("4:1", "unsupported: namespace name: identifier 'Vec' names a Rust prelude type")]),
    ("namespace bx { int f() { return 1; } }\n",
     [("4:1", "unsupported: namespace name: identifier 'bx' is reserved by the canonical spelling")]),
    ("namespace a { namespace { int h() { return 1; } } }\nint g() { return a::h(); }\n",
     [("5:18", "unsupported: 'a::h' is internal to namespace 'a' (a C++ anonymous-namespace member, a "
               "private Rust `fn`) and is referenced from outside it")]),
    ("namespace a { int main() { return 1; } }\n",
     [("4:19", "unsupported: function 'main' inside a namespace")]),
    ("using std::string;\n",
     [("4:1", "unsupported: using-declaration")]),
])
def test_cxx_namespace_rejections_are_located(tmp_path, body, expected):
    prog, errs = imp(tmp_path, HEAD + "#include <vector>\n" + body)
    assert prog is None
    assert errs_of(errs) == expected


# ------------------------------------------------------------------ Rust side

RS = """\
fn f() -> i32 {
    return 1;
}

mod a {
    fn h() -> i32 {
        return 10;
    }

    pub fn f() -> i32 {
        return 2;
    }

    pub mod b {
        pub fn f() -> i32 {
            return 3;
        }

        pub fn g() -> i32 {
            return f() + crate::a::f() + crate::a::h() + crate::f();
        }
    }
}

fn main() {
    print!("{}\\n", crate::a::b::g());
}
"""


def test_rust_modules_parse_to_resolved_paths_and_print_back():
    prog = RP.rs_parse(RS)
    assert [(f.path, f.internal) for f in prog.funcs()] == [
        ("f", False), ("a::h", True), ("a::f", False), ("a::b::f", False), ("a::b::g", False), ("main", False)]
    assert calls(prog) == ["a::b::f", "a::f", "a::h", "f", "a::b::g"]
    assert RSP.rs_print(prog) == RS


def test_rust_relative_spellings_normalize_to_the_absolute_path():
    canon = RP.rs_parse(RS)
    alt = RS.replace("crate::a::f()", "super::f()").replace("crate::a::h()", "super::h()") \
            .replace("crate::f()", "super::super::f()").replace("return f() +", "return self::f() +") \
            .replace("crate::a::b::g()", "a::b::g()")
    assert alt != RS
    assert RP.rs_parse(alt) == canon


def test_cxx_spelling_of_the_same_program():
    cxx = CP.cxx_print(RP.rs_parse(RS))
    assert "    return f() + ::a::f() + ::a::h() + ::f();" in cxx
    assert "std::printf(\"%d\\n\", ::a::b::g());" in cxx
    assert "}  // namespace b\n\n}  // namespace a\n" in cxx


def test_crate_bx_inside_modules():
    src = ("mod a {\n    pub fn s(t: &String) -> String {\n        return crate::bx::substr(t, 0usize, 1usize);\n"
           "    }\n}\n\nfn main() {\n    print!(\"{}\\n\", crate::a::s(&String::from(\"xy\")));\n}\n")
    prog = RP.rs_parse(src + "\n" + __import__("emitrust_bx.fmt", fromlist=["x"]).bx_module({"substr"}))
    out = RSP.rs_print(prog)
    assert "        return crate::bx::substr(t, 0usize, 1usize);" in out
    assert RP.rs_parse(out) == prog


@pytest.mark.parametrize("src,expected", [
    ("fn f() -> i32 {\n    return 1;\n}\n\nmod a {\n    pub fn g() -> i32 {\n        return f();\n    }\n}\n",
     ("7:16", "unsupported: `f` is not defined in module `a`: a Rust path does not search enclosing modules "
              "(C++ unqualified lookup would find `f`); spell it `crate::f`")),
    ("pub fn f() {\n}\n",
     ("1:1", "unsupported: `pub` at the crate root (the subset spells root items without it)")),
    ("mod a {\n    mod b {\n        pub fn f() {\n        }\n    }\n}\n",
     ("2:5", "unsupported: private nested module (the subset spells it `pub mod`: a private one is "
             "unreachable from the crate root)")),
    ("mod a {\n    pub(crate) fn f() {\n    }\n}\n",
     ("2:5", "unsupported: restricted visibility `pub(..)`")),
    ("mod a {\n    use super::*;\n}\n",
     ("2:5", "unsupported: item `use`")),
    ("mod a {\n    pub fn f() {\n    }\n}\n\nmod a {\n    pub fn g() {\n    }\n}\n",
     ("6:5", "unsupported: module 'a' defined twice (a Rust module cannot be reopened; the subset merges a "
             "reopened C++ namespace into one `mod`)")),
    ("mod a {\n    pub fn f() {\n        print!(\"{}\\n\", bx::chr(65));\n    }\n}\n",
     ("3:24", "unsupported: `bx::` inside a module resolves relative to it (the subset spells it "
              "`crate::bx::`)")),
    ("fn f() -> i32 {\n    return super::g();\n}\n",
     ("2:12", "unsupported: `super` above the crate root")),
    ("mod crate {\n}\n",
     ("1:5", "unsupported: module name: identifier 'crate' is a Rust keyword")),
])
def test_rust_module_rejections_are_located(src, expected):
    prog, errs = RP.rs_parse_all(src)
    assert prog is None
    assert ("%s" % errs[0][0], errs[0][1]) == expected


@pytest.mark.parametrize("src,expected", [
    ("mod a {\n    fn h() -> i32 {\n        return 1;\n    }\n}\n\nfn g() -> i32 {\n    return crate::a::h();\n}\n",
     ("8:12", "unsupported: 'a::h' is internal to namespace 'a' (a C++ anonymous-namespace member, a private "
              "Rust `fn`) and is referenced from outside it")),
    ("fn a() {\n}\n\nmod a {\n    pub fn f() {\n    }\n}\n",
     ("1:1", "function 'a' has the name of a namespace declared in the same scope")),
    ("mod a {\n    pub fn main() {\n    }\n}\n",
     ("2:9", "function 'main' inside a namespace")),
    ("mod a {\n    pub fn f() -> i32 {\n        return 1;\n    }\n\n    pub fn g() -> i32 {\n"
     "        let f: i32 = 2;\n        return f;\n    }\n}\n",
     ("7:9", "variable 'f' has the name of a function")),
])
def test_rust_module_wellformedness_errors(src, expected):
    prog, errs = RP.rs_parse_all(src)
    assert prog is None
    assert expected in [("%s" % l, m) for l, m in errs]


def test_variable_may_share_a_name_with_a_function_of_another_namespace():
    src = ("mod a {\n    pub fn f() -> i32 {\n        return 1;\n    }\n}\n\nfn g() -> i32 {\n"
           "    let f: i32 = crate::a::f();\n    return f;\n}\n")
    prog, errs = RP.rs_parse_all(src)
    assert errs == []


# ------------------------------------------------------------------ normal forms

def test_n13_tree_order_merges_reopened_namespaces():
    p = K.Program((fn("x", ("a",)), fn("r"), fn("y", ("a", "b")), fn("z", ("a",)), fn("w", ("c",))))
    n = TC.normalize(p)
    assert [f.path for f in n.funcs()] == ["a::x", "a::b::y", "a::z", "r", "c::w"]
    assert TC.normalize(n) == n


def test_n14_root_internal_is_erased():
    p = K.Program((fn("r", (), True), fn("h", ("a",), True)))
    n = TC.normalize(p)
    assert [f.internal for f in n.funcs()] == [False, True]


def test_kernel_dump_names_paths():
    p = TC.normalize(K.Program((fn("h", ("a",), True), fn("g", ("a", "b"), body=call("a::h")))))
    assert K.dump(p).splitlines()[0] == "func internal a::h() -> i32"
    assert "(call a::h)" in K.dump(p)


@needs_clang
def test_block_scope_using_is_spelling_only_and_header_redeclarations_resolve(tmp_path):
    # a call naming the HEADER declaration of a function this file defines
    # resolves to that function (clang's previousDecl link), and a
    # block-scope using-directive / alias is erased like a namespace-scope one
    (tmp_path / "h.h").write_text("namespace h { int f(int x); }\n")
    a, e1 = imp(tmp_path, HEAD + '#include "h.h"\n' + (
        "namespace h { int g(int x) { return f(x) + 1; } int f(int x) { return x * 5; } }\n"
        "int k() { using namespace h; namespace hh = h; return g(2) + hh::f(1); }\n"
        "int main() { std::printf(\"%d\\n\", k()); return 0; }\n"), "a.cpp")
    b, e2 = imp(tmp_path, HEAD + (
        "namespace h { int g(int x) { return f(x) + 1; } int f(int x) { return x * 5; } }\n"
        "int k() { return h::g(2) + h::f(1); }\n"
        "int main() { std::printf(\"%d\\n\", k()); return 0; }\n").replace(
        "namespace h { int g", "namespace h { int f(int x);\nint g"), "b.cpp")
    assert e1 == [] and e2 == []
    assert a == b
