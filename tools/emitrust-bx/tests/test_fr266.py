# Pins FR-266: every USE of a rejected struct inside an otherwise-accepted
# function is a LOCATED residual at the use, worded "use of a rejected struct
# 'S'".  The invariants, and why each is load-bearing:
#  * the per-function census (scripts/bx-census.py / bx-frontier.py) counts a
#    function as EMIT when bx reports nothing inside it.  Before FR-266 a
#    function that takes a rejected struct as a parameter, calls one of its
#    methods and reads one of its fields had NO residual of its own, so it was
#    a PHANTOM EMIT: the whole run still failed (nothing is miscompiled), but
#    the measuring instrument counted a function that can never be emitted.
#    FR-262 closed the same hole for out-of-line DEFINITIONS; this closes it
#    for USES: parameters, locals and returns of a rejected struct type (by
#    value, by reference, or as a std::vector element), method calls (member
#    and static), data member reads and writes, and constructions;
#  * a struct that holds a rejected struct BY VALUE (directly or inside a
#    std::vector) is itself rejected, located at the field, so the rejection
#    propagates to the containing struct's users as well;
#  * a rejected struct's OWN member functions keep their existing class-level
#    attribution (no new wording inside them), and a rejected MEMBER of an
#    accepted struct keeps "use of a rejected data member" / "use of a
#    rejected member function";
#  * an ACCEPTED struct and its users are untouched: no new diagnostic on
#    accepted input, so no golden moves;
#  * every new wording is classified by the census as a CASCADE (not a new
#    root cause), and the old catch-all `struct: member` bucket is split into
#    one tag per reason so the frontier can rank struct rows.
import importlib.util
import os
import shutil

import pytest

from emitrust_bx import cli

needs_clang = pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not on PATH")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
USE = "unsupported: use of a rejected struct '%s'"


def imp(tmp_path, src, name="t.cpp"):
    p = tmp_path / name
    p.write_text(src)
    return cli.load_cpp(str(p), "clang++")


def lines(errs, msg=None):
    """[(line, col, message)] of the errors, optionally only one message."""
    return [(loc.line, loc.col, m) for loc, m in errs if msg is None or m == msg]


# A struct rejected for one member (`mutable`), with in-class methods.
BAD = """#include <cstdio>
#include <vector>
struct Bad {
    int x;
    mutable int m;
    int get() const { return x; }
    static int zero() { return 0; }
};
"""
# line 9 onwards is the test body


# ------------------------------------------------------------------ the defect

@needs_clang
def test_motivating_example_user_is_not_a_phantom(tmp_path):
    src = ("struct S { int get() const; int x; int* p; };\n"
           "int S::get() const { return x; }\n"
           "int user(const S& s) { return s.get() + s.x; }\n"
           "int main() { return 0; }\n")
    prog, errs = imp(tmp_path, src)
    assert prog is None
    # the parameter, the method call and the field read, each at its use
    assert lines(errs, USE % "S") == [(3, 19, USE % "S"), (3, 31, USE % "S"), (3, 41, USE % "S")], errs
    # the root causes are unchanged
    assert (1, 1, "unsupported: struct with members outside the subset") in lines(errs)
    assert (2, 1, "unsupported: member function of a struct with members outside the subset") in lines(errs)


# ------------------------------------------------------------------ use shapes

@needs_clang
@pytest.mark.parametrize("body, where", [
    # parameter by value / const& / & / vector element / vector&
    ("int f(Bad b) { return 0; }\n", [(9, 11)]),
    ("int f(const Bad& b) { return 0; }\n", [(9, 18)]),
    ("int f(Bad& b) { return 0; }\n", [(9, 12)]),
    ("int f(std::vector<Bad> v) { return 0; }\n", [(9, 24)]),
    ("int f(const std::vector<Bad>& v) { return 0; }\n", [(9, 31)]),
    # return type
    ("Bad f(int k) { Bad b = {k, k}; return b; }\n", [(9, 5), (9, 16), (9, 24)]),
    # local by value, initialized from an aggregate
    ("int f(int k) { Bad b = {k, k}; return k; }\n", [(9, 16), (9, 24)]),
    # local vector of the rejected struct
    ("int f(int k) { std::vector<Bad> v; return k; }\n", [(9, 16)]),
    # method call, field read, field write through a parameter
    ("int f(Bad& b) {\n    b.x = 3;\n    return b.get();\n}\n", [(9, 12), (10, 5), (11, 12)]),
    # static member function call
    ("int f(int k) { return Bad::zero() + k; }\n", [(9, 23)]),
])
def test_every_use_shape_is_located(tmp_path, body, where):
    prog, errs = imp(tmp_path, BAD + body + "int main() { return 0; }\n")
    assert prog is None
    got = lines(errs, USE % "Bad")
    assert [(l, c) for l, c, _ in got] == where, errs


@needs_clang
def test_own_members_of_a_rejected_struct_get_no_use_residual(tmp_path):
    # Bad::get / Bad::zero read their own struct; they are attributed by the
    # class-level residual, as before FR-266
    prog, errs = imp(tmp_path, BAD + "int main() { return 0; }\n")
    assert prog is None
    assert lines(errs) == [(3, 1, "unsupported: struct with members outside the subset"),
                           (5, 17, "unsupported: mutable data member")], errs


@needs_clang
def test_rejected_member_of_accepted_wording_is_kept(tmp_path):
    # a use of the rejected member itself keeps its existing wording first
    src = BAD + "int f(Bad& b) { return b.m; }\nint main() { return 0; }\n"
    prog, errs = imp(tmp_path, src)
    assert (9, 24, "unsupported: use of a rejected data member") in lines(errs), errs
    assert (9, 24, USE % "Bad") not in lines(errs), errs


@needs_clang
def test_use_residual_is_additive_and_hides_no_blocker(tmp_path):
    # the paired frontier showed a REPLACING residual hiding real blockers
    # (`std::vector::resize without a fill value` vanished from a woff2
    # function): the use is one MORE residual, the expression is imported
    # exactly as before
    src = ("#include <vector>\n"
           "struct Bad {\n    std::vector<int> v;\n    mutable int m;\n};\n"   # 2-5
           "void f(Bad& b) {\n"            # 6
           "    b.v.resize(3);\n"          # 7
           "    Bad c = {b.v, 1};\n"       # 8
           "}\n"
           "int main() { return 0; }\n")
    prog, errs = imp(tmp_path, src)
    got = lines(errs)
    for want in [(6, 13, USE % "Bad"),
                 (7, 5, "unsupported: std::vector::resize without a fill value"),
                 (7, 5, USE % "Bad"),
                 (8, 5, USE % "Bad"),
                 (8, 13, "unsupported: aggregate initialization of a struct with rejected data members"),
                 (8, 13, USE % "Bad")]:
        assert want in got, (want, errs)


# ------------------------------------------------------------------ containing structs

@needs_clang
def test_struct_holding_a_rejected_struct_by_value_is_rejected(tmp_path):
    src = BAD + ("struct Outer {\n"            # 9
                 "    Bad b;\n"                 # 10
                 "    int y;\n"                 # 11
                 "};\n"                         # 12
                 "struct Holder {\n"            # 13
                 "    std::vector<Bad> v;\n"    # 14
                 "};\n"                         # 15
                 "struct Deep {\n"              # 16
                 "    Outer o;\n"               # 17
                 "};\n"                         # 18
                 "int use(const Outer& o) { return o.y; }\n"  # 19
                 "int main() { return 0; }\n")
    prog, errs = imp(tmp_path, src)
    assert prog is None
    got = lines(errs)
    for want in [(9, 1, "unsupported: struct with members outside the subset"),
                 (10, 9, USE % "Bad"),
                 (13, 1, "unsupported: struct with members outside the subset"),
                 (14, 22, USE % "Bad"),
                 (16, 1, "unsupported: struct with members outside the subset"),
                 (17, 11, USE % "Outer"),
                 (19, 22, USE % "Outer"),
                 (19, 34, USE % "Outer")]:
        assert want in got, (want, errs)


# ------------------------------------------------------------------ accepted input untouched

@needs_clang
def test_accepted_struct_and_its_users_are_untouched(tmp_path):
    src = ("#include <cstdio>\n#include <vector>\n"
           "struct Good {\n    int x;\n    int get() const { return x; }\n};\n"
           "struct Wrap {\n    Good g;\n    std::vector<Good> gs;\n};\n"
           "int user(const Good& g, Wrap& w) { w.g.x = g.get(); return w.g.x + g.x; }\n"
           "int main() {\n    Good g = {1};\n    Wrap w = {g, {g}};\n"
           "    std::printf(\"%d\\n\", user(g, w));\n    return 0;\n}\n")
    prog, errs = imp(tmp_path, src)
    assert errs == [] and prog is not None


@needs_clang
def test_accepted_struct_next_to_a_rejected_one_is_untouched(tmp_path):
    src = BAD + ("struct Good {\n    int x;\n};\n"   # 9-11
                 "int user(const Good& g) { return g.x; }\n"  # 12
                 "int main() { return 0; }\n")
    prog, errs = imp(tmp_path, src)
    assert [l for l, _, _ in lines(errs)] == [3, 5], errs


# ------------------------------------------------------------------ census

def _census():
    spec = importlib.util.spec_from_file_location("bx_census_fr266", os.path.join(REPO, "scripts", "bx-census.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_census_classifies_the_use_as_a_cascade():
    c = _census()
    assert c.classify(USE % "Bad") == ("struct: use of a rejected struct (cascade)", "struct:Bad", True)
    assert c.classify(USE % "a::Bad") == ("struct: use of a rejected struct (cascade)", "struct:a::Bad", True)


SPLIT = {
    "private or protected member": "struct: private or protected member",
    "bit-field": "struct: bit-field",
    "default member initializer": "struct: default member initializer",
    "mutable data member": "struct: mutable data member",
    "unnamed data member": "struct: unnamed data member",
    "const data member": "struct: const data member",
    "static data member": "struct: static data member",
    "operator overload": "struct: operator overload",
    "virtual member function": "struct: virtual member function",
    "defaulted or deleted member function": "struct: defaulted or deleted member function",
    "volatile member function": "struct: volatile member function",
    "user-declared copy or move constructor": "struct: user-declared copy or move constructor",
    "overloaded constructor": "struct: overloaded constructor",
    "user-declared destructor": "struct: user-declared destructor",
    "friend declaration": "struct: friend declaration",
    "member function template": "struct: member function template",
    "nested class": "struct: nested class",
    "enum inside a class": "struct: enum inside a class",
    "member typedef": "struct: member typedef",
    "static_assert inside a class": "struct: static_assert inside a class",
    "using-declaration inside a class": "struct: using-declaration inside a class",
    "anonymous union or struct member": "struct: anonymous union or struct member",
    "conversion operator": "struct: conversion operator",
    "member function declared but not defined": "struct: member function declared but not defined",
    "use of a rejected data member": "struct: use of a rejected data member (cascade)",
    "use of a rejected member function": "struct: use of a rejected member function (cascade)",
    "member function used as a value": "struct: member function used as a value",
    "member access on a temporary": "struct: member access on a temporary",
    "member function call on a temporary": "struct: member function call on a temporary",
    "static member function called through an object": "struct: static member function called through an object",
    "aggregate initialization that omits data members": "struct: aggregate initialization that omits data members",
    "aggregate initialization of a struct with rejected data members":
        "struct: aggregate initialization of a struct with rejected data members (cascade)",
    "struct variable without an initializer (default-initialized data members)":
        "struct: struct variable without an initializer",
    "delegating or base-class constructor initializer": "struct: delegating or base-class constructor initializer",
    "ObjCIvarDecl inside a class": "struct: other member inside a class",
}


@pytest.mark.parametrize("reason, tag", sorted(SPLIT.items()))
def test_census_splits_struct_member_per_reason(reason, tag):
    c = _census()
    got = c.classify("unsupported: " + reason)
    assert got[0] == tag and got[2], (reason, got)
    assert got[0] != "struct: member"


@needs_clang
def test_census_does_not_count_the_user_as_emit(tmp_path):
    c = _census()
    p = tmp_path / "c.cpp"
    p.write_text("struct S { int get() const; int x; int* p; };\n"
                 "int S::get() const { return x; }\n"
                 "int user(const S& s) { return s.get() + s.x; }\n"
                 "int other(int k) { return k + 1; }\n"
                 "int main() { return 0; }\n")
    bx = os.path.join(REPO, "tools", "emitrust-bx", "emitrust-bx.py")
    rec = c.analyze((str(p), bx, "clang++", 120))
    assert rec["status"] == "BLOCKED", rec
    fns = {f["name"]: f for f in rec["functions"]}
    clean = sorted(n for n, f in fns.items() if not f["diags"] and not f["container_diags"])
    assert clean == ["main", "other"], clean
    tags = {rec["diags"][i]["tag"] for i in fns["user"]["diags"]}
    assert tags == {"struct: use of a rejected struct (cascade)"}, tags
