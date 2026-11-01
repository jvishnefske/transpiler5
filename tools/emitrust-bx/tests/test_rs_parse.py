# Pins the Rust side of the subset (FR-250): rs_parse DEFINES it.  It must
# accept exactly the canonical forms (plus the few documented equivalent
# spellings, which normalize to the same Kernel) and reject everything else
# with a LOCATED error -- never guess a meaning for Rust it does not model
# (inference, references, unsigned `+` that panics in debug builds).
import pytest

from emitrust_bx import kernel as K
from emitrust_bx import rs_parse as RP
from emitrust_bx import rs_print as RSP


def parse(src):
    return RP.rs_parse(src)


def err(src):
    prog, errs = RP.rs_parse_all(src)
    assert prog is None
    return [(str(l), m) for l, m in errs]


def test_canonical_function():
    p = parse("fn add(mut a: i32, b: i64) -> i64 {\n    return (a as i64) + b;\n}\n")
    f = p.funcs()[0]
    assert f.params == (K.Param("a", K.I32, True), K.Param("b", K.I64, False))
    assert f.body.stmts[0] == K.Return(K.Binary("add", K.Cast(K.I64, K.Var("a")), K.Var("b")))


def test_equivalent_spellings_normalize():
    a = parse('fn main() {\n    let mut x: i32 = 1;\n    x = x + 1;\n    println!("{}", ((x)));\n}\n')
    b = parse('fn main() {\n    let mut x: i32 = 1;\n    x += 1;\n    print!("{}\\n", x);\n}\n')
    assert a == b
    assert RSP.rs_print(a) == RSP.rs_print(b)


def test_cast_operand_literals_are_suffixed_but_call_args_need_not_be():
    p = parse("fn f(x: i32) -> i32 {\n    return x;\n}\n\nfn main() {\n"
              "    let a: i64 = (f(5) + 1i32) as i64;\n}\n")
    assert "(f(5) + 1i32) as i64" in RSP.rs_print(p)


def test_unsigned_wrapping_compound_is_canonical():
    p = parse("fn main() {\n    let mut u: u32 = 1u32;\n    u = u.wrapping_add(2u32);\n}\n")
    assert p.funcs()[0].body.stmts[1] == K.CompoundAssign("add", "u", K.IntLit(2, K.U32))
    assert "u = u.wrapping_add(2u32);" in RSP.rs_print(p)


@pytest.mark.parametrize("src,expected", [
    ("fn main() {\n    let x = 5;\n}\n",
     [("2:11", "unsupported: `let` without a type annotation (every declaration is explicitly typed)")]),
    # FR-251 moved this pin forward: `Vec<T>` is in the subset, other std
    # containers are its nearest out-of-subset neighbour
    ("fn main() {\n    let mut m: HashMap<i32, i32> = HashMap::new();\n}\n", [("2:16", "unsupported: type `HashMap`")]),
    ("fn main() {\n    loop {}\n}\n",
     [("2:5", "unsupported: `loop` (C++ has no direct canonical spelling; use `while`)")]),
    # FR-251 moved this pin forward: `&T` / `&mut T` parameters are in the
    # subset; a reference to a reference and a returned reference are not
    ("fn f(x: &&i32) {}\n", [("1:9", "unsupported: reference to a reference")]),
    ("fn f(x: &i32) -> &i32 {\n    return x;\n}\n", [("1:1", "unsupported: function returning a reference")]),
    ("fn main() {\n    let x: u32 = 1u32 + 2u32;\n}\n",
     [("2:23", "unsupported: unsigned `+` (panics on overflow in debug builds); "
               "the subset spells it `.wrapping_add(..)`")]),
    ("fn main() {\n    let x: i64 = 5;\n}\n", [("2:18", "expression has type i32, expected i64")]),
    ("fn main() {\n    for i in 0..=3 {}\n}\n", [("2:15", "unsupported: inclusive range `..=`")]),
    ("fn f() -> i32 {\n    7\n}\n", [("2:5", "unsupported: tail expression (the subset spells it `return e;`)")]),
    ("use std::io;\n", [("1:1", "unsupported: item `use`")]),
    ("fn main() {\n    let x: i32 = 1;\n    let x: i32 = 2;\n}\n",
     [("3:5", "redeclaration of 'x' in the same scope")]),
    ('fn main() {\n    print!("{:x}", 5);\n}\n', [("2:12", "unsupported: print! placeholder `{:x}`")]),
    ('fn main() {\n    print!("{}", 1.5);\n}\n', [("2:18", "printf integer conversion of a f64 argument")]),
    # FR-251 moved this pin forward: `.len()` is in the subset; methods
    # outside the rows (and iterator adaptors outside `for`) are not
    ("fn main() {\n    let v: Vec<i32> = Vec::new();\n    let s: usize = v.capacity();\n}\n",
     [("3:22", "unsupported: method call `.capacity()`")]),
    ("fn main() {\n    let v: Vec<i32> = vec![1, 2];\n    let n: usize = v.iter().count();\n}\n",
     [("3:29", "unsupported: method call `.count()`")]),
    # rustc would type these literals from the cast target (i64 / i8), not i32
    ("fn main() {\n    let s: i64 = (1 << 40i32) as i64;\n}\n",
     [("2:19", "unsupported: unsuffixed integer literal in the operand of `as` (rustc infers its "
               "type from the cast target); write `1i32`")]),
    ("fn main() {\n    let s: i8 = 200 as i8;\n}\n",
     [("2:17", "unsupported: unsuffixed integer literal in the operand of `as` (rustc infers its "
               "type from the cast target); write `200i32`")]),
])
def test_rejections_are_located(src, expected):
    assert err(src) == expected


def test_all_wellformedness_errors_reported():
    src = ("fn main() {\n    let a: i64 = 1;\n    let b: i32 = 1i64;\n    c = 3;\n}\n")
    assert err(src) == [("2:18", "expression has type i32, expected i64"),
                        ("3:18", "expression has type i64, expected i32"),
                        ("4:5", "assignment to undeclared 'c'")]


def test_bx_helpers_must_be_canonical():
    from emitrust_bx import fmt
    good = 'fn main() {\n    print!("{}", bx::chr(65));\n}\n\n' + fmt.bx_module({"chr"})
    assert parse(good)
    bad = good.replace("b < 128", "b < 255")
    assert err(bad) == [("6:5", "unsupported: `bx::chr` differs from the canonical helper")]
