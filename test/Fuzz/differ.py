#!/usr/bin/env python3
"""One-iteration differential runner: native clang vs emitrust-cc crate.

``run_pair`` compiles one generated C program twice -- natively with
clang and through ``emitrust-cc --emit=crate --build`` -- runs both
binaries, and compares (exit code, stdout bytes).  Classification:

* PASS        -- both legs ran and the (rc, stdout) tuples match.
* UNSUPPORTED -- emitrust-cc rejected the program or timed out; counted,
  never a failure (the generator intentionally brushes subset edges).
* MISCOMPILE  -- emitrust-cc accepted and built, but the transpiled
  binary is missing, crashed, timed out, or its (rc, stdout) tuple
  differs from the native one.  Always a compiler bug to triage.
* HARNESS_BUG -- the NATIVE leg failed to compile or run (crash or
  timeout).  That is a generator defect; drivers must fail loudly.

Timeouts and process plumbing are imported from the conformance-ledger
runner (test/CTestSuite/run_c_testsuite.py) so the two harnesses cannot
drift apart.

Binary layout: emitrust-cc's cargo child normally honors an inherited
``CARGO_TARGET_DIR`` (the driver probes this once), in which case the
binary lands at ``$CARGO_TARGET_DIR/release/<crate>``; otherwise it
lands at ``<crate-dir>/target/release/<crate>``.  ``run_pair`` handles
both layouts, unlinking stale candidates before each build.
"""

import argparse
import os
import sys
from collections import namedtuple

_CTESTSUITE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "CTestSuite")
sys.path.insert(0, _CTESTSUITE_DIR)

from run_c_testsuite import (  # noqa: E402
    RUN_TIMEOUT,
    TRANSPILE_BUILD_TIMEOUT,
    first_line,
    output_diff_snippet,
    run_command,
    sanitize_crate_binary_name,
)

PASS = "PASS"
UNSUPPORTED = "UNSUPPORTED"
MISCOMPILE = "MISCOMPILE"
HARNESS_BUG = "HARNESS_BUG"

PairResult = namedtuple(
    "PairResult",
    ["status", "detail", "native_rc", "native_out", "rust_rc", "rust_out", "crate_dir"],
)


def _binary_candidates(crate_dir, crate_name):
    """Possible transpiled-binary paths for both cargo target-dir layouts."""
    binary = sanitize_crate_binary_name(crate_name)
    candidates = [os.path.join(crate_dir, "target", "release", binary)]
    shared = os.environ.get("CARGO_TARGET_DIR")
    if shared:
        candidates.append(os.path.join(shared, "release", binary))
    return candidates


def _run_binary(binary):
    """Run one produced binary; returns (rc, stdout, timed_out)."""
    rc, stdout, _stderr, timed_out = run_command([binary], RUN_TIMEOUT)
    return rc, stdout, timed_out


def compare_runs(native_binary, rust_binary):
    """Run both binaries and classify the (rc, stdout) comparison.

    The native leg is trusted ground truth: a crash or timeout there is a
    HARNESS_BUG.  Any divergence on the rust leg is a MISCOMPILE.  This is
    the single comparison path used by both run_pair and --self-test.
    """
    native_rc, native_out, timed_out = _run_binary(native_binary)
    if timed_out:
        return PairResult(HARNESS_BUG, "native binary timed out", None, b"", None, b"", None)
    if native_rc < 0:
        return PairResult(
            HARNESS_BUG, "native binary crashed with signal %d" % -native_rc,
            native_rc, native_out, None, b"", None,
        )

    rust_rc, rust_out, timed_out = _run_binary(rust_binary)
    if timed_out:
        return PairResult(
            MISCOMPILE, "transpiled binary timed out after %ss" % RUN_TIMEOUT,
            native_rc, native_out, None, b"", None,
        )
    if (native_rc, native_out) != (rust_rc, rust_out):
        detail = "rc native=%s rust=%s" % (native_rc, rust_rc)
        if native_out != rust_out:
            detail += "; stdout mismatch:\n" + output_diff_snippet(native_out, rust_out)
        return PairResult(MISCOMPILE, detail, native_rc, native_out, rust_rc, rust_out, None)
    return PairResult(PASS, "", native_rc, native_out, rust_rc, rust_out, None)


def run_pair(emitrust_cc, clang, source_path, workdir):
    """Run one source file through both legs and classify the outcome.

    ``workdir`` is a per-seed scratch directory; the crate name is derived
    from the source file stem (``fuzz_<seed>.c`` -> ``fuzz_<seed>``).
    """
    os.makedirs(workdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(source_path))[0]
    crate_name = sanitize_crate_binary_name(stem)

    native_binary = os.path.join(workdir, "native")
    rc, _stdout, stderr, timed_out = run_command(
        [clang, "-std=c11", "-w", source_path, "-o", native_binary],
        TRANSPILE_BUILD_TIMEOUT,
    )
    if timed_out or rc != 0:
        return PairResult(
            HARNESS_BUG,
            "native clang compile failed (rc=%s timeout=%s): %s"
            % (rc, timed_out, first_line(stderr)),
            None, b"", None, b"", None,
        )

    crate_dir = os.path.join(workdir, "crate")
    for candidate in _binary_candidates(crate_dir, crate_name):
        if os.path.isfile(candidate):
            os.unlink(candidate)

    rc, _stdout, stderr, timed_out = run_command(
        [emitrust_cc, "--emit=crate", source_path, "-o", crate_dir,
         "--crate-name", crate_name, "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    if timed_out:
        return PairResult(
            UNSUPPORTED, "transpile/build timed out after %ss" % TRANSPILE_BUILD_TIMEOUT,
            None, b"", None, b"", crate_dir,
        )
    if rc != 0:
        return PairResult(UNSUPPORTED, first_line(stderr), None, b"", None, b"", crate_dir)

    rust_binary = None
    for candidate in _binary_candidates(crate_dir, crate_name):
        if os.path.isfile(candidate):
            rust_binary = candidate
            break
    if rust_binary is None:
        return PairResult(
            MISCOMPILE,
            "build reported success but binary missing (looked in: %s)"
            % ", ".join(_binary_candidates(crate_dir, crate_name)),
            None, b"", None, b"", crate_dir,
        )

    result = compare_runs(native_binary, rust_binary)
    return result._replace(crate_dir=crate_dir)


def _self_test(clang, workdir):
    """Oracle self-test: a known-divergent pair must classify MISCOMPILE.

    Two hand-written native programs with different stdout and exit codes
    are pushed through the same compare_runs path used for real pairs; the
    divergent pairing must classify MISCOMPILE and the identical pairing
    must classify PASS.
    """
    os.makedirs(workdir, exist_ok=True)
    progs = {
        "sa": '#include <stdio.h>\nint main(void) { printf("alpha\\n"); return 3; }\n',
        "sb": '#include <stdio.h>\nint main(void) { printf("beta\\n"); return 4; }\n',
    }
    binaries = {}
    for name, text in sorted(progs.items()):
        src = os.path.join(workdir, name + ".c")
        with open(src, "w", encoding="utf-8") as handle:
            handle.write(text)
        binary = os.path.join(workdir, name)
        rc, _stdout, stderr, timed_out = run_command(
            [clang, "-std=c11", "-w", src, "-o", binary], TRANSPILE_BUILD_TIMEOUT
        )
        if timed_out or rc != 0:
            print("self-test FAIL: clang could not build %s: %s" % (name, first_line(stderr)))
            return 1
        binaries[name] = binary

    divergent = compare_runs(binaries["sa"], binaries["sb"])
    identical = compare_runs(binaries["sa"], binaries["sa"])
    ok = divergent.status == MISCOMPILE and identical.status == PASS
    print("self-test divergent pair -> %s (expected MISCOMPILE)" % divergent.status)
    print("self-test identical pair -> %s (expected PASS)" % identical.status)
    print("self-test %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    """CLI: run one pair (or the oracle self-test) and print the verdict."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitrust-cc", dest="emitrust_cc", default=None)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--source", default=None, help="C source to run through both legs.")
    parser.add_argument("--workdir", required=True)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run a known-divergent hand-written pair through the comparison"
        " logic; it must classify MISCOMPILE.",
    )
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test(args.clang, args.workdir)
    if not args.emitrust_cc or not args.source:
        parser.error("--emitrust-cc and --source are required unless --self-test")
    result = run_pair(args.emitrust_cc, args.clang, args.source, args.workdir)
    print("%s %s" % (result.status, result.detail))
    return 0 if result.status in (PASS, UNSUPPORTED) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
