"""printf <=> print! mapping (FR-250).

PROBE RESULTS (2026-11-01, glibc in the nix flake vs rustc 1.96.1; the
mandatory probe the FR-250 spec requires before committing to a mapping):

  value        %.2f / {:.2}   %.1f / {:.1}  %.0f / {:.0}   %f / {:.6}
  0.125        0.12 = 0.12    0.1 = 0.1     0 = 0          same
  0.25         0.25 = 0.25    0.2 = 0.2     0 = 0          same
  2.5          same           same          2 = 2          same
  0.5          same           same          0 = 0          same
  1.5 / 3.5 / -2.5            ties to even in BOTH (exact-decimal rounding)
  1e300        identical 301-digit expansions in both
  inf / -inf   inf / -inf     IDENTICAL
  nan          "nan"   vs Rust "NaN"            DIFFERS
  -nan         "-nan"  vs Rust "NaN"            DIFFERS
  -0.0         -0.00 = -0.00                    same
  1e-320 / -1e-5  0.00 / -0.00 identical

  integers: %5d %-5d %05d (incl. -42 -> "-0042") %-5u %3d (overflowing
  width) %5lld (-9) %05zu all byte-identical to {:5} {:<5} {:05}.
  %c with a byte >= 0x80: C writes the raw byte; a Rust `char` would write
  its 2-byte UTF-8 encoding -- DIFFERS.

Consequences (the canonical forms):

* %.Nf goes through ``bx::fmt_f(x, N)`` (a `{}` argument), a safe helper
  that spells NaN the glibc way ("nan"/"-nan" by sign bit) and otherwise
  defers to ``format!("{:.*}", N, x)``, which matched glibc everywhere else.
  %f is %.6f.
* %c goes through ``bx::chr(x)``: `(x as u8) as char` for ASCII; for a
  byte >= 0x80 it PANICS (exit 101) instead of writing a different byte
  sequence -- a loud failure, never a silent mismatch.  (Exact non-ASCII %c
  needs byte-level stdout and is left to a later FR.)
* integer conversions map to `{}` with the probed width/flag forms; the
  conversion letter is NOT stored in the Kernel, it is derived from the
  argument type: i32 %d, i64 %lld, u32 %u, u64 %llu, usize %zu.  On
  import %i == %d, %ld == %lld (long and long long are both i64) and
  %lu == %zu (`unsigned long` IS size_t on LP64; cxx_import maps it to
  usize, keeping u64 for `unsigned long long`).
* %s takes a string LITERAL argument only (FR-250).
* only the '-' and '0' flags, only on integer conversions, never both.
"""

from . import kernel as K

C_CONV_FOR = {"i32": "d", "i64": "lld", "u32": "u", "u64": "llu", "usize": "zu"}

# conv letter + length modifier -> required Kernel argument type
C_ARG_TYPE = {
    ("d", ""): K.I32, ("i", ""): K.I32,
    ("d", "l"): K.I64, ("i", "l"): K.I64, ("d", "ll"): K.I64, ("i", "ll"): K.I64,
    ("u", ""): K.U32, ("u", "l"): K.USIZE, ("u", "ll"): K.U64, ("u", "z"): K.USIZE,
    ("f", ""): K.F64, ("f", "l"): K.F64,
    ("c", ""): K.I32,
    ("s", ""): K.STR,
}

BX_HELPERS = {
    "fmt_f": (
        "    pub fn fmt_f(x: f64, prec: usize) -> String {\n"
        "        if x.is_nan() {\n"
        "            if x.is_sign_negative() {\n"
        "                return String::from(\"-nan\");\n"
        "            }\n"
        "            return String::from(\"nan\");\n"
        "        }\n"
        "        format!(\"{:.*}\", prec, x)\n"
        "    }\n"
    ),
    "chr": (
        "    pub fn chr(x: i32) -> char {\n"
        "        let b: u8 = x as u8;\n"
        "        assert!(b < 128, \"bx::chr: printf %c of a non-ASCII byte is outside the subset\");\n"
        "        b as char\n"
        "    }\n"
    ),
}
BX_ORDER = ("fmt_f", "chr")


def bx_module(used):
    """The canonical `mod bx` text holding exactly the helpers in ``used``."""
    names = [n for n in BX_ORDER if n in used]
    if not names:
        return ""
    return "mod bx {\n" + "\n".join(BX_HELPERS[n] for n in names) + "}\n"


# ---------------------------------------------------------------- C side

class FormatError(Exception):
    pass


def parse_c_format(text):
    """Split a printf format into Text strings and spec tuples.

    Returns a list whose items are either ``str`` (literal text) or
    ``(flag, width, prec, length, conv)``.  Raises FormatError(reason) for
    anything outside the subset, the reason phrased in C++ terms.
    """
    out = []
    buf = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c != "%":
            buf.append(c)
            i += 1
            continue
        i += 1
        if i < n and text[i] == "%":
            buf.append("%")
            i += 1
            continue
        start = i - 1
        flags = ""
        while i < n and text[i] in "-0+ #'":
            flags += text[i]
            i += 1
        width = ""
        while i < n and text[i].isdigit():
            width += text[i]
            i += 1
        if i < n and text[i] == "*":
            raise FormatError("printf '*' width")
        prec = None
        if i < n and text[i] == ".":
            i += 1
            p = ""
            while i < n and text[i].isdigit():
                p += text[i]
                i += 1
            if i < n and text[i] == "*":
                raise FormatError("printf '*' precision")
            prec = int(p) if p else 0
        length = ""
        while i < n and text[i] in "hlLqjzt":
            length += text[i]
            i += 1
        if i >= n:
            raise FormatError("printf format ends inside a conversion")
        conv = text[i]
        i += 1
        spec = text[start:i]
        if (conv, length) not in C_ARG_TYPE:
            raise FormatError("printf conversion '%s'" % spec)
        if flags not in ("", "-", "0"):
            raise FormatError("printf flags '%s' in '%s'" % (flags, spec))
        if conv in "fcs" and (flags or width):
            raise FormatError("printf width/flags on '%s'" % spec)
        if conv not in "f" and prec is not None:
            raise FormatError("printf precision on '%s'" % spec)
        if flags and not width:
            raise FormatError("printf flag without width in '%s'" % spec)
        w = int(width) if width else None
        if w is not None and not (1 <= w <= 99):
            raise FormatError("printf width in '%s'" % spec)
        if conv == "f" and prec is None:
            prec = 6
        if prec is not None and prec > 30:
            raise FormatError("printf precision in '%s'" % spec)
        if buf:
            out.append("".join(buf))
            buf = []
        out.append((flags, w, prec, length, conv))
    if buf:
        out.append("".join(buf))
    return out


def c_spec_text(sg, argty):
    if sg.kind == "s":
        return "%s"
    if sg.kind == "c":
        return "%c"
    if sg.kind == "f":
        return "%f" if sg.prec == 6 else "%%.%df" % sg.prec
    return "%" + sg.flag + ("" if sg.width is None else str(sg.width)) + C_CONV_FOR[argty.name]


def c_escape(text, fmt):
    """Escape ``text`` for a C++ string literal (``fmt``: double '%')."""
    out = []
    for c in text:
        o = ord(c)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "%" and fmt:
            out.append("%%")
        elif 0x20 <= o <= 0x7E:
            out.append(c)
        else:
            out.append("\\%03o" % o)
    return "".join(out)


def c_unescape_literal(spelling):
    """Decode clang's JSON StringLiteral ``value`` (C++ source spelling).

    Returns the string, or raises FormatError for prefixes/encodings outside
    the subset.  Handles adjacent-literal concatenation ("a" "b").
    """
    s = spelling.strip()
    out = []
    i = 0
    n = len(s)
    while i < n:
        if s[i].isspace():
            i += 1
            continue
        if s[i] != '"':
            raise FormatError("string literal with an encoding prefix")
        i += 1
        while i < n and s[i] != '"':
            c = s[i]
            if c != "\\":
                out.append(c)
                i += 1
                continue
            i += 1
            e = s[i]
            simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'",
                      "?": "?", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}
            if e in simple:
                out.append(simple[e])
                i += 1
            elif e in "01234567":
                j = i
                while j < n and j < i + 3 and s[j] in "01234567":
                    j += 1
                out.append(chr(int(s[i:j], 8)))
                i = j
            elif e == "x":
                j = i + 1
                while j < n and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(chr(int(s[i + 1:j], 16)))
                i = j
            else:
                raise FormatError("string escape '\\%s'" % e)
        i += 1
    return "".join(out)


# ---------------------------------------------------------------- Rust side

def rust_placeholder(sg):
    if sg.kind != "d" or sg.width is None:
        return "{}"
    return "{:" + {"": "", "-": "<", "0": "0"}[sg.flag] + str(sg.width) + "}"


def rust_escape(text, fmt):
    out = []
    for c in text:
        o = ord(c)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        elif fmt and c == "{":
            out.append("{{")
        elif fmt and c == "}":
            out.append("}}")
        elif 0x20 <= o <= 0x7E:
            out.append(c)
        else:
            out.append("\\x%02x" % o)
    return "".join(out)


def parse_rust_format(text):
    """Split a print! format into str text and (flag, width) placeholders."""
    out = []
    buf = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            if i + 1 < n and text[i + 1] == "{":
                buf.append("{")
                i += 2
                continue
            j = text.find("}", i)
            if j < 0:
                raise FormatError("unterminated `{` in print! format")
            inner = text[i + 1:j]
            i = j + 1
            flag, width = "", None
            if inner == "":
                pass
            elif inner.startswith(":") and len(inner) > 1:
                body = inner[1:]
                if body[0] == "<":
                    flag, body = "-", body[1:]
                elif body[0] == "0" and len(body) > 1:
                    flag, body = "0", body[1:]
                if not body.isdigit() or body.startswith("0"):
                    raise FormatError("print! placeholder `{%s}`" % inner)
                width = int(body)
                if not (1 <= width <= 99):
                    raise FormatError("print! placeholder `{%s}`" % inner)
            else:
                raise FormatError("print! placeholder `{%s}`" % inner)
            if buf:
                out.append("".join(buf))
                buf = []
            out.append((flag, width))
            continue
        if c == "}":
            if i + 1 < n and text[i + 1] == "}":
                buf.append("}")
                i += 2
                continue
            raise FormatError("unmatched `}` in print! format")
        buf.append(c)
        i += 1
    if buf:
        out.append("".join(buf))
    return out
