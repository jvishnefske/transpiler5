#!/usr/bin/env python3
"""Derive each test/EndToEnd project's real input set from its own lit RUN line.

The v2 harness assumed every EndToEnd case was a single translation unit:

    for p in sorted(list(e2e.glob("*.c")) + list(e2e.glob("*.cpp"))):
        ps.append(dict(..., inputs=[str(p)], tus=1))

That is wrong for the deliberately multi-TU cases, whose companion source lives
in test/EndToEnd/Inputs/ and is named on the test's own RUN line, e.g.

    // RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-lib.c -o %t.crate ...

Feeding only half of a two-TU project makes a symbol undefined that is in fact
defined.  This module parses the FIRST `emitrust-cc` RUN line of each test and
returns exactly the source files it names, with %s -> the test file and
%S -> test/EndToEnd.  Nothing is hand-maintained, so the multi-TU set cannot
drift away from the tests.

A test whose RUN line cannot be parsed is reported in `skipped`, never silently
downgraded to single-TU.
"""
import re
import shlex
from pathlib import Path

SRC_SUFFIXES = (".c", ".cpp", ".cc", ".cxx", ".C")

# flags of emitrust-cc that take a separate value argument; the value must not
# be mistaken for an input file.
VALUE_FLAGS = {"-o", "-I", "-D", "-include", "--crate-name", "--search-trace",
               "--max-search-nodes", "-x", "-isystem"}

RUN_RE = re.compile(r"^\s*(?://|/\*|#)\s*RUN:\s?(.*?)\s*$")


def _run_lines(text):
    """Yield logical RUN commands, joining trailing-backslash continuations."""
    pending = None
    for raw in text.splitlines():
        m = RUN_RE.match(raw)
        if not m:
            if pending is not None:
                # a continuation was promised but the next line is not a RUN
                yield pending
                pending = None
            continue
        body = m.group(1)
        cont = body.endswith("\\")
        if cont:
            body = body[:-1]
        body = (pending + " " + body) if pending else body
        if cont:
            pending = body
        else:
            pending = None
            yield body
    if pending is not None:
        yield pending


def parse_inputs(test_path, s_dir):
    """-> (inputs, error).

    `inputs` is the list of source-file arguments of the first emitrust-cc
    command in the file, as absolute path strings, in RUN-line order.
    `error` is a string when the RUN line could not be parsed (inputs is then
    None).
    """
    test_path = Path(test_path)
    text = test_path.read_text(errors="replace")
    cmd = None
    for line in _run_lines(text):
        # stop at the first pipeline stage that is emitrust-cc
        for stage in line.split("|"):
            toks_probe = stage.split()
            if any(t.endswith("emitrust-cc") or t == "emitrust-cc"
                   for t in toks_probe):
                cmd = stage
                break
        if cmd:
            break
    if cmd is None:
        return None, "no emitrust-cc RUN line"
    # cut off shell redirections
    cmd = re.sub(r"\d?>&?\S*", " ", cmd)
    try:
        toks = shlex.split(cmd)
    except ValueError as exc:
        return None, "unlexable RUN line: %s" % exc
    # drop everything up to and including the emitrust-cc token
    try:
        i = next(k for k, t in enumerate(toks)
                 if t == "emitrust-cc" or t.endswith("/emitrust-cc"))
    except StopIteration:
        return None, "emitrust-cc token not found after lexing"
    toks = toks[i + 1:]

    inputs, k = [], 0
    while k < len(toks):
        t = toks[k]
        if t in VALUE_FLAGS:
            k += 2
            continue
        if t.startswith("-"):
            k += 1
            continue
        if t == "%s":
            inputs.append(str(test_path))
        elif t.startswith("%S/"):
            p = Path(s_dir) / t[len("%S/"):]
            if p.suffix not in SRC_SUFFIXES:
                k += 1
                continue          # e.g. a .rs consumer handed to rustc, not us
            if not p.exists():
                return None, "RUN line names a missing file: %s" % p
            inputs.append(str(p))
        elif t.startswith("%"):
            return None, "unresolvable lit substitution in input position: %s" % t
        elif t.endswith(SRC_SUFFIXES):
            p = Path(t)
            if not p.is_absolute():
                p = Path(s_dir) / t
            if not p.exists():
                return None, "RUN line names a missing file: %s" % p
            inputs.append(str(p))
        k += 1
    if not inputs:
        return None, "no source inputs found on the emitrust-cc RUN line"
    return inputs, ""


def endtoend_projects(root):
    """-> (projects, skipped).

    projects: list of dict(corpus, name, cwd, inputs, tus)
    skipped:  list of (name, reason)
    """
    e2e = Path(root) / "test/EndToEnd"
    ps, skipped = [], []
    for p in sorted(list(e2e.glob("*.c")) + list(e2e.glob("*.cpp"))):
        inputs, err = parse_inputs(p, e2e)
        if inputs is None:
            skipped.append((p.name, err))
            continue
        ps.append(dict(corpus="endtoend", name=p.name, cwd=str(e2e),
                       inputs=inputs, tus=len(inputs)))
    return ps, skipped


if __name__ == "__main__":
    import sys
    root = sys.argv[1]
    ps, sk = endtoend_projects(root)
    multi = [p for p in ps if p["tus"] > 1]
    print("%d EndToEnd cases, %d skipped, %d with >1 input"
          % (len(ps), len(sk), len(multi)))
    for p in multi:
        print("  %-46s tus=%d  %s" % (p["name"], p["tus"],
                                      " ".join(Path(i).name for i in p["inputs"])))
    for n, r in sk:
        print("  SKIP %s: %s" % (n, r))
