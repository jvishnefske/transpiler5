#!/usr/bin/env python3
"""Curate the CppStdSuite candidate list from the llvm-test-suite submodule.

NEVER run by lit or CI.  This is the reproducible corpus-selection step
(FR-244): it enumerates the C++ single-source programs of the pinned
third_party/llvm-test-suite checkout, keeps exactly those that fit the
conformance-ledger contract, and writes candidates.txt (and, with
--emit-expected, the reference stdout files).  Re-run it only when
advancing the submodule pin, and land the regenerated candidates.txt,
expected/ tree, and re---update-ed expected-pass.txt in the SAME commit
as the new pin (see cppstd-suite.cpp for the full pin-advance procedure).

The ledger contract is a bare ``main``: no arguments, no stdin, exit 0,
deterministic byte-exact stdout.  Selection, in order:

1. every ``*.cpp`` under SingleSource/Regression/C++, SingleSource/UnitTests,
   and SingleSource/Benchmarks/Misc-C++ (``.cc``/``.C`` excluded --
   emitrust-cc selects the C++ frontend by extension, and .cpp-only is
   simpler than special-casing);
2. DROP files whose directory's CMakeLists.txt/Makefile mentions
   RUN_OPTIONS, STDIN_FILENAME, or HASH_PROGRAM_OUTPUT (conservative:
   the whole directory is dropped -- upstream harness knobs mean the
   program does not run bare there).  Extra sources and required defines
   need no token scan: they surface as standalone-build failures in 3;
3. DROP files that do not build standalone with the pinned dev-shell
   ``clang++ -std=c++17`` (run this script under ``nix develop`` so the
   compiler is the pinned one, clang 21.1.8 at FR-244 time);
4. DROP files whose native binary, run TWICE in two different fresh
   empty cwds with stdin=/dev/null, does not exit 0 both times with
   byte-identical stdout and a runtime under 2s per run (headroom under
   the runner's 10s RUN_TIMEOUT on a slow CI host).  The two distinct
   cwds catch path-dependent output; ASLR across the two runs catches
   address-printing programs.

The committed candidates.txt paths are relative to the submodule root
and ARE the ledger names (unique by construction; the runner keys
per-crate workdirs on the sanitized path because basenames collide
across directories).  The reference stdout deliberately comes from OUR
pinned clang++ build, NOT llvm-test-suite's ``*.reference_output``:
those are missing for some programs, may carry harness-appended
exit/timing lines, and upstream compares them with fpcmp tolerances --
none of which matches the byte-exact-stdout + exit-0 oracle.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOTS = [
    "SingleSource/Regression/C++",
    "SingleSource/UnitTests",
    "SingleSource/Benchmarks/Misc-C++",
]
HARNESS_TOKENS = ("RUN_OPTIONS", "STDIN_FILENAME", "HASH_PROGRAM_OUTPUT")
BUILD_TIMEOUT = 60
RUN_TIMEOUT = 5
MAX_RUNTIME = 2.0


def dir_uses_harness_knobs(dirpath):
    for name in ("CMakeLists.txt", "Makefile"):
        p = os.path.join(dirpath, name)
        if os.path.isfile(p):
            text = open(p, "r", encoding="utf-8", errors="replace").read()
            if any(tok in text for tok in HARNESS_TOKENS):
                return True
    return False


def probe(suite_root, rel, tmp_root):
    """Return (rel, verdict, detail, stdout_bytes)."""
    src = os.path.join(suite_root, rel)
    workdir = os.path.join(tmp_root, rel.replace("/", "_"))
    os.makedirs(workdir, exist_ok=True)
    binary = os.path.join(workdir, "native")
    try:
        build = subprocess.run(
            ["clang++", "-std=c++17", "-w", src, "-o", binary],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=BUILD_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return rel, "no-build", "compile timeout", b""
    if build.returncode != 0:
        first = build.stderr.decode("utf-8", errors="replace").splitlines()
        return rel, "no-build", first[0] if first else "compile failed", b""

    outputs = []
    for i in (1, 2):
        cwd = os.path.join(workdir, "run%d" % i)
        os.makedirs(cwd, exist_ok=True)
        started = time.monotonic()
        try:
            run = subprocess.run(
                [binary],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=cwd,
                timeout=RUN_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            return rel, "run-timeout", "run %d exceeded %ss" % (i, RUN_TIMEOUT), b""
        elapsed = time.monotonic() - started
        if run.returncode != 0:
            return rel, "nonzero-exit", "run %d exited %d" % (i, run.returncode), b""
        if elapsed > MAX_RUNTIME:
            return rel, "too-slow", "run %d took %.2fs" % (i, elapsed), b""
        outputs.append(run.stdout)
    if outputs[0] != outputs[1]:
        return rel, "nondeterministic", "stdout differs between two runs/cwds", b""
    return rel, "keep", "", outputs[0]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument(
        "--suite",
        default=os.path.join(here, "..", "..", "third_party", "llvm-test-suite"),
        help="Root of the llvm-test-suite checkout.",
    )
    ap.add_argument(
        "--out",
        default=os.path.join(here, "candidates.txt"),
        help="candidates.txt to write.",
    )
    ap.add_argument(
        "--emit-expected",
        default=None,
        metavar="DIR",
        help="Also write <DIR>/<relpath>.expected reference stdout files"
        " from the (verified-deterministic) native runs.",
    )
    args = ap.parse_args(argv)
    suite_root = os.path.abspath(args.suite)
    if not os.path.isdir(os.path.join(suite_root, "SingleSource")):
        raise SystemExit(
            "error: %s does not look like llvm-test-suite; initialize the"
            " submodule first (git submodule update --init"
            " third_party/llvm-test-suite)" % suite_root
        )

    rels, dropped_dirs = [], set()
    for root in ROOTS:
        base = os.path.join(suite_root, root)
        if not os.path.isdir(base):
            raise SystemExit("error: missing corpus root: %s" % base)
        for dirpath, _dirnames, filenames in os.walk(base):
            knobs = dir_uses_harness_knobs(dirpath)
            for name in sorted(filenames):
                if not name.endswith(".cpp"):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), suite_root)
                if knobs:
                    dropped_dirs.add(os.path.relpath(dirpath, suite_root))
                else:
                    rels.append(rel)
    rels.sort()

    with tempfile.TemporaryDirectory(prefix="cppstd-curate-") as tmp_root:
        with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
            results = list(pool.map(lambda r: probe(suite_root, r, tmp_root), rels))

    kept = [(rel, out) for rel, verdict, _d, out in results if verdict == "keep"]
    by_verdict = {}
    for rel, verdict, detail, _out in results:
        by_verdict.setdefault(verdict, []).append((rel, detail))

    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(
            "# CppStdSuite candidate corpus: llvm-test-suite programs that fit\n"
            "# the conformance-ledger contract (bare main, no stdin, exit 0,\n"
            "# deterministic byte-exact stdout). Paths are relative to the\n"
            "# third_party/llvm-test-suite submodule root and ARE the ledger\n"
            "# names. Regenerated ONLY on a submodule pin advance, via:\n"
            "#   nix develop -c python3 test/CppStdSuite/curate_candidates.py \\\n"
            "#     --emit-expected test/CppStdSuite/expected\n"
        )
        for rel, _out in kept:
            handle.write(rel + "\n")

    if args.emit_expected:
        for rel, out in kept:
            dest = os.path.join(args.emit_expected, rel + ".expected")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as handle:
                handle.write(out)

    print("curated %d candidates -> %s" % (len(kept), args.out))
    for verdict in sorted(by_verdict):
        if verdict == "keep":
            continue
        entries = by_verdict[verdict]
        print("dropped %d (%s):" % (len(entries), verdict))
        for rel, detail in entries:
            print("  %s: %s" % (rel, detail))
    if dropped_dirs:
        print(
            "dropped %d director(ies) with harness knobs (%s):"
            % (len(dropped_dirs), "/".join(HARNESS_TOKENS))
        )
        for d in sorted(dropped_dirs):
            print("  " + d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
