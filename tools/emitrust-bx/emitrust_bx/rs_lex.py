"""Lexer for the canonical Rust subset (FR-250).

Exactly the tokens rs_print can emit, plus whitespace and comments.  A
character outside that set is a located error, not a guess.
"""

from .kernel import Loc


class LexError(Exception):
    def __init__(self, loc, msg):
        Exception.__init__(self, msg)
        self.loc = loc
        self.msg = msg


class Tok:
    __slots__ = ("kind", "text", "loc", "value", "suffix")

    def __init__(self, kind, text, loc, value=None, suffix=""):
        self.kind = kind  # 'ident' 'int' 'float' 'str' 'punct' 'eof'
        self.text = text
        self.loc = loc
        self.value = value
        self.suffix = suffix

    def __repr__(self):
        return "Tok(%s %r @%s)" % (self.kind, self.text, self.loc)


PUNCT = [
    "<<=", ">>=", "...", "..=",
    "::", "->", "=>", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "..",
    "{", "}", "(", ")", "[", "]", ",", ";", ":", "=", "<", ">", "+", "-",
    "*", "/", "%", "&", "|", "^", "!", ".", "#", "?", "@", "'", "$", "~",
]
INT_SUFFIXES = ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "i128", "u128",
                "isize", "usize")


def unescape_rust_str(body, loc):
    out = []
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= n:
            raise LexError(loc, "unterminated escape")
        e = body[i]
        simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0"}
        if e in simple:
            out.append(simple[e])
            i += 1
        elif e == "x":
            h = body[i + 1:i + 3]
            if len(h) != 2 or any(ch not in "0123456789abcdefABCDEF" for ch in h) or int(h, 16) > 0x7F:
                raise LexError(loc, "bad \\x escape")
            out.append(chr(int(h, 16)))
            i += 3
        elif e == "u":
            j = body.find("}", i)
            if body[i + 1:i + 2] != "{" or j < 0:
                raise LexError(loc, "bad \\u escape")
            out.append(chr(int(body[i + 2:j].replace("_", ""), 16)))
            i = j + 1
        elif e == "\n":
            # line continuation: skip the newline and leading whitespace
            i += 1
            while i < n and body[i] in " \t\n\r":
                i += 1
        else:
            raise LexError(loc, "unsupported string escape '\\%s'" % e)
    return "".join(out)


def lex(src):
    toks = []
    i = 0
    n = len(src)
    line = 1
    col = 1

    def adv(k):
        nonlocal i, line, col
        for _ in range(k):
            if src[i] == "\n":
                line += 1
                col = 1
            else:
                col += 1
            i += 1

    while i < n:
        c = src[i]
        if c in " \t\r\n":
            adv(1)
            continue
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                adv(1)
            continue
        if src.startswith("/*", i):
            depth = 0
            start = Loc(line, col)
            while True:
                if i >= n:
                    raise LexError(start, "unterminated block comment")
                if src.startswith("/*", i):
                    depth += 1
                    adv(2)
                elif src.startswith("*/", i):
                    depth -= 1
                    adv(2)
                    if depth == 0:
                        break
                else:
                    adv(1)
            continue
        loc = Loc(line, col)
        if c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(Tok("ident", src[i:j], loc))
            adv(j - i)
            continue
        if c.isdigit():
            j = i
            while j < n and (src[j].isdigit() or src[j] == "_"):
                j += 1
            is_float = False
            if j < n and src[j] == "." and j + 1 < n and src[j + 1].isdigit():
                is_float = True
                j += 1
                while j < n and (src[j].isdigit() or src[j] == "_"):
                    j += 1
            if j < n and src[j] in "eE":
                k = j + 1
                if k < n and src[k] in "+-":
                    k += 1
                if k < n and src[k].isdigit():
                    is_float = True
                    j = k
                    while j < n and (src[j].isdigit() or src[j] == "_"):
                        j += 1
            num = src[i:j]
            # suffix
            k = j
            while k < n and (src[k].isalnum() or src[k] == "_"):
                k += 1
            suffix = src[j:k]
            text = src[i:k]
            if src.startswith("0x", i) or src.startswith("0o", i) or src.startswith("0b", i):
                raise LexError(loc, "unsupported: non-decimal integer literal")
            if is_float or suffix in ("f64", "f32"):
                if suffix not in ("", "f64"):
                    raise LexError(loc, "unsupported: f32 literal")
                toks.append(Tok("float", text, loc, float(num.replace("_", "")), suffix))
            else:
                if suffix and suffix not in INT_SUFFIXES:
                    raise LexError(loc, "bad literal suffix '%s'" % suffix)
                toks.append(Tok("int", text, loc, int(num.replace("_", "")), suffix))
            adv(k - i)
            continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    j += 1
                j += 1
            if j >= n:
                raise LexError(loc, "unterminated string literal")
            body = src[i + 1:j]
            toks.append(Tok("str", src[i:j + 1], loc, unescape_rust_str(body, loc)))
            adv(j + 1 - i)
            continue
        for p in PUNCT:
            if src.startswith(p, i):
                toks.append(Tok("punct", p, loc))
                adv(len(p))
                break
        else:
            raise LexError(loc, "unexpected character %r" % c)
    toks.append(Tok("eof", "", Loc(line, col)))
    return toks
