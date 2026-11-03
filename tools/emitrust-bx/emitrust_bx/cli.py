"""emitrust-bx command line (FR-250).

    emitrust-bx [--from=cpp|rust] [--emit=kernel|rust|cpp|crate] [-o OUT]
                [--build] [--check-roundtrip] [--normalize] [--clang=clang++] SRC

Every construct outside the subset is reported as one located line
`SRC:line:col: error: unsupported: <construct>` on stderr (ALL of them,
sorted by location -- not the first), exit 1.  A program with any
residual never reaches a printer.

FR-254: ``--normalize`` (C++ input only, opt-in) applies the normalizer's
rewrite rules (normalize.RULES) instead of refusing their patterns; each
applied rule is a located `SRC:line:col: note: normalized: <rule>` line on
stderr.  The output is canonical subset C++/Rust: ``--emit=cpp`` prints
the rewritten program, which re-imports WITHOUT --normalize.

FR-254 R4: with ``--project-root DIR`` as well, an ``inline`` /
``constexpr`` / ``static`` free function DEFINED in a header under DIR and
reachable from the main file is imported into one unity translation unit
(`note: normalized: header-inline 'NAME' from HEADER`).  Its residuals are
located in the header, and every call of a header function with a residual
is itself refused at the call site, so the caller is never counted clean.
"""

import argparse
import difflib
import os
import sys
import tempfile

from . import crate
from . import cxx_print as CP
from . import kernel as K
from . import rs_parse as RP
from . import rs_print as RSP
from . import typecheck as TC
from .cxx_import import ClangError, cxx_import


def _key(item):
    loc = item[0]
    return (loc is None, (loc.file or "") if loc else "", loc.line if loc else 0, loc.col if loc else 0, item[1])


def header_cascade(prog, spans, errs):
    """FR-254 R4: the calls of imported header functions that have an error
    (their own, or a call of another such function), as located refusals at
    each call site -- in the main file this is what makes the CALLER
    non-clean (a per-function census attributes only main-file lines)."""
    if not spans:
        return []
    real = {}

    def rp(f):
        if f not in real:
            real[f] = os.path.realpath(f)
        return real[f]

    bad = set()
    for loc, _ in errs:
        if loc is None or loc.file is None:
            continue
        for path, (f, l0, l1) in spans.items():
            if rp(f) == rp(loc.file) and l0 <= loc.line <= l1:
                bad.add(path)
    calls = []  # (callee, caller path if a header function else None, loc)
    for fn in prog.all_funcs():
        caller = fn.path if (fn.loc is not None and fn.loc.file is not None and fn.path in spans) else None
        for n in K.walk(fn.body):
            if isinstance(n, K.Call) and n.name in spans:
                calls.append((n.name, caller, n.loc))
    changed = True
    while changed:
        changed = False
        for callee, caller, _ in calls:
            if callee in bad and caller is not None and caller not in bad:
                bad.add(caller)
                changed = True
    return [(loc, "unsupported: call of a rejected header function '%s'" % callee.rsplit("::", 1)[-1])
            for callee, _, loc in calls if callee in bad]


def load_cpp(path, clang, normalize=False, notes=None, project_root=None):
    """(normalized program or None, [(loc, msg)]).  FR-254: ``normalize``
    applies the rewrite rules, appending each application to ``notes``;
    R4: ``project_root`` imports reachable project-header functions."""
    spans = {}
    surf = cxx_import(path, clang, normalize, notes, project_root, spans)
    errs = [(r.loc, "unsupported: " + r.reason) for r in K.residuals(surf)]
    prog = TC.normalize(surf)
    for loc, msg in TC.check(prog):
        if msg.startswith("residual:"):
            continue
        errs.append((loc, msg if msg.startswith("unsupported:") else "unsupported: " + msg))
    if errs:
        errs += header_cascade(prog, spans, errs)
        return None, sorted(set(errs), key=_key)
    return prog, []


def load_rust(path):
    with open(path, encoding="utf-8") as f:
        src = f.read()
    prog, errs = RP.rs_parse_all(src)
    return prog, sorted(errs, key=_key)


def report_notes(path, notes):
    for loc, msg in sorted(set(notes), key=lambda x: (x[0] is None, (x[0].file or "") if x[0] else "",
                                                 x[0].line if x[0] else 0, x[0].col if x[0] else 0)):
        where = "%s:%d:%d" % (loc.file or path, loc.line, loc.col) if loc else "%s:1:1" % path
        sys.stderr.write("%s: note: %s\n" % (where, msg))


def report(path, errs):
    for loc, msg in errs:
        where = "%s:%d:%d" % (loc.file or path, loc.line, loc.col) if loc else "%s:1:1" % path
        sys.stderr.write("%s: error: %s\n" % (where, msg))


def _diff(a, b, an, bn):
    return "".join(difflib.unified_diff(a.splitlines(True), b.splitlines(True), an, bn))


def check_roundtrip(prog, clang):
    """Laws 1 and 2 for ``prog`` on both sides. Returns a list of failures."""
    fails = []
    rs = RSP.rs_print(prog)
    try:
        back = RP.rs_parse(rs)
    except RP.ParseError as ex:
        return ["rust: rs_parse(rs_print(k)) failed at %s: %s\n%s" % (ex.loc, ex.msg, rs)]
    if back != prog:
        fails.append("rust: rs_parse(rs_print(k)) != k\n" + _diff(K.dump(prog), K.dump(back), "k", "rs_parse(rs_print(k))"))
    elif RSP.rs_print(back) != rs:
        fails.append("rust: print not idempotent\n" + _diff(rs, RSP.rs_print(back), "1", "2"))
    cxx = CP.cxx_print(prog)
    fd, tmp = tempfile.mkstemp(suffix=".cpp", prefix="bx-roundtrip-")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(cxx)
        try:
            back2, errs = load_cpp(tmp, clang)
        except ClangError as ex:
            return fails + ["c++: clang rejected cxx_print(k):\n%s\n%s" % (ex, cxx)]
        if errs:
            fails.append("c++: cxx_import(cxx_print(k)) has errors:\n" +
                         "".join("  %s: %s\n" % (l, m) for l, m in errs) + cxx)
        elif back2 != prog:
            fails.append("c++: cxx_import(cxx_print(k)) != k\n" +
                         _diff(K.dump(prog), K.dump(back2), "k", "cxx_import(cxx_print(k))"))
        elif CP.cxx_print(back2) != cxx:
            fails.append("c++: print not idempotent\n" + _diff(cxx, CP.cxx_print(back2), "1", "2"))
    finally:
        os.unlink(tmp)
    return fails


def main(argv=None):
    ap = argparse.ArgumentParser(prog="emitrust-bx")
    ap.add_argument("--from", dest="src_lang", choices=["cpp", "rust"])
    ap.add_argument("--emit", choices=["kernel", "rust", "cpp", "crate"])
    ap.add_argument("-o", dest="out")
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--check-roundtrip", action="store_true")
    ap.add_argument("--normalize", action="store_true")
    ap.add_argument("--project-root", dest="project_root")
    ap.add_argument("--clang", default="clang++")
    ap.add_argument("src")
    a = ap.parse_args(argv)

    lang = a.src_lang
    if lang is None:
        lang = "rust" if a.src.endswith(".rs") else "cpp"
    emit = a.emit or ("cpp" if lang == "rust" else "rust")
    if a.build and emit != "crate":
        sys.stderr.write("error: --build is only valid with --emit=crate\n")
        return 1
    if a.normalize and lang != "cpp":
        sys.stderr.write("error: --normalize applies to C++ input only\n")
        return 1
    if a.project_root is not None and not a.normalize:
        sys.stderr.write("error: --project-root needs --normalize\n")
        return 1
    if emit == "crate" and not a.out:
        sys.stderr.write("error: --emit=crate needs -o DIR\n")
        return 1

    notes = []
    try:
        if lang == "cpp":
            prog, errs = load_cpp(a.src, a.clang, a.normalize, notes, a.project_root)
        else:
            prog, errs = load_rust(a.src)
    except ClangError as ex:
        sys.stderr.write(str(ex))
        sys.stderr.write("error: clang failed on %s\n" % a.src)
        return 1
    except OSError as ex:
        sys.stderr.write("error: %s\n" % ex)
        return 1
    report_notes(a.src, notes)
    if errs:
        report(a.src, errs)
        return 1

    if a.check_roundtrip:
        fails = check_roundtrip(prog, a.clang)
        if fails:
            for f in fails:
                sys.stderr.write("error: round-trip law failed: " + f + "\n")
            return 2

    if emit == "kernel":
        out = K.dump(prog)
    elif emit == "rust" or emit == "crate":
        out = RSP.rs_print(prog)
    else:
        out = CP.cxx_print(prog)

    if emit == "crate":
        if not any(f.path == "main" for f in prog.funcs()):
            sys.stderr.write("%s:1:1: error: --emit=crate needs a `main` function\n" % a.src)
            return 1
        crate.write_crate(a.out, a.src, out)
        if a.build:
            ok, msg = crate.build_crate(a.out)
            if not ok:
                sys.stderr.write("error: %s\n" % msg)
                return 1
        return 0

    if a.out and a.out != "-":
        with open(a.out, "w") as f:
            f.write(out)
    else:
        sys.stdout.write(out)
    return 0
