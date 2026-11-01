# Pins the printf <=> print! mapping table (FR-250): which C conversions are
# in the subset (each needs a probed Rust equivalent), the exact wording of
# the out-of-subset reasons (they become `unsupported:` diagnostics), and
# that both literal escapers produce text their own language reads back.
import pytest

from emitrust_bx import fmt
from emitrust_bx import kernel as K
from emitrust_bx.rs_lex import unescape_rust_str


def test_parse_c_format_subset():
    parts = fmt.parse_c_format("a%d b%5u %-3lld %05zu %c %s %.2f %f %% %i %ld %lu %llu")
    specs = [p for p in parts if isinstance(p, tuple)]
    assert specs == [
        ("", None, None, "", "d"), ("", 5, None, "", "u"), ("-", 3, None, "ll", "d"),
        ("0", 5, None, "z", "u"), ("", None, None, "", "c"), ("", None, None, "", "s"),
        ("", None, 2, "", "f"), ("", None, 6, "", "f"), ("", None, None, "", "i"),
        ("", None, None, "l", "d"), ("", None, None, "l", "u"), ("", None, None, "ll", "u"),
    ]
    assert "".join(p for p in parts if isinstance(p, str)) == "a b" + " " * 7 + "%" + " " * 4


@pytest.mark.parametrize("f,reason", [
    ("%x", "printf conversion '%x'"),
    ("%+d", "printf flags '+' in '%+d'"),
    ("%-05d", "printf flags '-0' in '%-05d'"),
    ("%5s", "printf width/flags on '%5s'"),
    ("%.3d", "printf precision on '%.3d'"),
    ("%*d", "printf '*' width"),
    ("%hd", "printf conversion '%hd'"),
    ("%e", "printf conversion '%e'"),
    ("%5.2f", "printf width/flags on '%5.2f'"),
    ("%", "printf format ends inside a conversion"),
])
def test_parse_c_format_rejections(f, reason):
    with pytest.raises(fmt.FormatError) as ex:
        fmt.parse_c_format(f)
    assert str(ex.value) == reason


def test_arg_types():
    assert fmt.C_ARG_TYPE[("u", "l")] == K.USIZE  # unsigned long IS size_t on LP64
    assert fmt.C_ARG_TYPE[("u", "z")] == K.USIZE
    assert fmt.C_ARG_TYPE[("d", "l")] == K.I64
    assert fmt.C_ARG_TYPE[("c", "")] == K.I32  # int-promoted


def test_c_escape_roundtrip():
    s = 'a%b"c\\d\ne\tf\x01g{}'
    lit = '"' + fmt.c_escape(s, False) + '"'
    assert fmt.c_unescape_literal(lit) == s
    assert fmt.c_escape("%", True) == "%%"
    assert fmt.c_escape("\x01", False) == "\\001"  # octal: never swallows a following hex digit


def test_rust_escape_roundtrip():
    s = 'a%b"c\\d\ne\tf\x01g'
    assert unescape_rust_str(fmt.rust_escape(s, False), None) == s
    assert fmt.rust_escape("{x}", True) == "{{x}}"


def test_rust_placeholders():
    spec = lambda flag, w: K.Spec("d", K.Var("x"), flag, w, None)  # noqa: E731
    assert fmt.rust_placeholder(spec("", None)) == "{}"
    assert fmt.rust_placeholder(spec("", 5)) == "{:5}"
    assert fmt.rust_placeholder(spec("-", 5)) == "{:<5}"
    assert fmt.rust_placeholder(spec("0", 5)) == "{:05}"
    assert fmt.parse_rust_format("a{{{}}}{:<5}{:05}{:7}") == ["a{", ("", None), "}", ("-", 5), ("0", 5), ("", 7)]


@pytest.mark.parametrize("f", ["{:x}", "{:>5}", "{:+}", "{0}", "{:.2}", "{name}", "}"])
def test_rust_format_rejections(f):
    with pytest.raises(fmt.FormatError):
        fmt.parse_rust_format(f)


def test_bx_module_is_safe_and_minimal():
    assert fmt.bx_module(set()) == ""
    both = fmt.bx_module({"fmt_f", "chr"})
    assert "unsafe" not in both
    assert both.index("fn fmt_f") < both.index("fn chr")
    assert "fn chr" not in fmt.bx_module({"fmt_f"})
