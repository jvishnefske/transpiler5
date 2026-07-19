#!/usr/bin/env python3
"""Conformance-ledger runner for the external c-testsuite single-exec suite.

Runs every ``tests/single-exec/NNNNN.c`` in the vendored c-testsuite through
``emitrust-cc --emit=crate --build`` and classifies each test:

* UNSUPPORTED -- emitrust-cc rejected the file (or the transpile+build step
  timed out).  Most of the ~220 tests use constructs outside the supported
  C11 subset, so this is the expected common case and never a failure.
* PASS        -- the transpiled binary exited 0 and its stdout matched the
  suite's ``NNNNN.c.expected`` file byte-for-byte.
* MISCOMPILE  -- emitrust-cc accepted and built the test, but the binary
  crashed, timed out, exited nonzero, or printed the wrong output.  Silent
  wrong code must never pass CI, so any MISCOMPILE not explicitly
  quarantined in the known-miscompiles file fails the run regardless of the
  manifest.

The PASS set is compared against a manifest (one test filename per line,
``#`` comments allowed).  A mismatch in either direction is an error: tests
in the manifest that no longer pass are regressions; tests that newly pass
are improvements and the developer is told to re-run with ``--update`` to
ratchet the ledger forward.

Suite conventions (from the c-testsuite README): tests are single .c files,
``main`` is the entry point, programs exit 0 on success, and the
``.expected`` file holds the reference stdout+stderr.  emitrust-cc's printf
subset writes only to stdout, so this runner compares stdout alone; a
transpiled binary that writes to stderr would be a bug surfaced by the
byte-exact stdout comparison or the exit code.
"""

import argparse
import difflib
import os
import shutil
import signal
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# Hard ceilings, in seconds.  Transpile+cargo-build is the slow step; the
# produced binaries are tiny deterministic programs.
TRANSPILE_BUILD_TIMEOUT = 60
RUN_TIMEOUT = 10

# Classification labels.
PASS = "PASS"
MISCOMPILE = "MISCOMPILE"
UNSUPPORTED = "UNSUPPORTED"


def parse_args(argv):
    """Parse command-line arguments into an argparse Namespace."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emitrust-cc",
        required=True,
        dest="emitrust_cc",
        help="Path to (or bare name of) the emitrust-cc binary.",
    )
    parser.add_argument(
        "--suite",
        required=True,
        help="Root of the vendored c-testsuite checkout.",
    )
    parser.add_argument(
        "--manifest",
        required=True,
        help="Manifest of expected-passing test filenames.",
    )
    parser.add_argument(
        "--workdir",
        required=True,
        help="Scratch directory for per-test crate output.",
    )
    parser.add_argument(
        "--known-miscompiles",
        default=None,
        help="Optional quarantine list of known-miscompiling test filenames;"
        " a missing file means the quarantine is empty.",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Rewrite the manifest with the current PASS set instead of"
        " failing on manifest drift.",
    )
    return parser.parse_args(argv)


def resolve_tool(value):
    """Resolve the emitrust-cc argument to an absolute executable path.

    lit's tool substitution normally rewrites the bare token ``emitrust-cc``
    in the RUN line to an absolute path, but if a bare name reaches us
    anyway (e.g. when invoked by hand), fall back to PATH lookup --
    test/lit.cfg.py puts the tool directory on PATH.
    """
    if os.sep in value:
        candidate = os.path.abspath(value)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
        raise SystemExit("error: emitrust-cc not executable: %s" % candidate)
    found = shutil.which(value)
    if found is None:
        raise SystemExit("error: cannot find %r on PATH" % value)
    return found


def sanitize_crate_binary_name(stem):
    """Mirror emitrust-cc's crate-name sanitization for a source file stem.

    Lowercase the stem, map every character outside [a-z0-9_] to '_',
    prefix '_' if the result starts with a digit, and use 'transpiled' if
    the result is empty.
    """
    lowered = stem.lower()
    sanitized = "".join(
        ch if (ch.isascii() and (ch.isdigit() or "a" <= ch <= "z" or ch == "_")) else "_"
        for ch in lowered
    )
    if not sanitized:
        return "transpiled"
    if sanitized[0].isdigit():
        return "_" + sanitized
    return sanitized


def load_name_list(path, required):
    """Read a set of test filenames from ``path``.

    Blank lines and ``#`` comments are ignored.  If the file is missing and
    ``required`` is False, return the empty set; otherwise error out.
    """
    if not os.path.isfile(path):
        if required:
            raise SystemExit("error: missing file: %s" % path)
        return set()
    names = set()
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.split("#", 1)[0].strip()
            if stripped:
                names.add(stripped)
    return names


def run_command(cmd, timeout, cwd=None):
    """Run ``cmd`` with a hard timeout, returning (rc, stdout, stderr, timed_out).

    Uses a fresh process group and SIGKILLs the whole group on timeout so
    that lingering grandchildren (e.g. rustc under cargo) cannot keep the
    output pipes open and hang the runner.
    """
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=cwd,
        start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        return proc.returncode, stdout, stderr, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass
        stdout, stderr = proc.communicate()
        return proc.returncode, stdout, stderr, True


def first_line(data):
    """Return the first non-empty line of bytes ``data`` as text, for reports."""
    for line in data.decode("utf-8", errors="replace").splitlines():
        if line.strip():
            return line.strip()
    return ""


def output_diff_snippet(expected, actual, limit=12):
    """Build a short unified diff between expected and actual output bytes."""
    diff = difflib.unified_diff(
        expected.decode("utf-8", errors="replace").splitlines(),
        actual.decode("utf-8", errors="replace").splitlines(),
        fromfile="expected",
        tofile="actual",
        lineterm="",
    )
    lines = list(diff)[:limit]
    if not lines:
        return "(outputs differ only in trailing bytes/newlines)"
    return "\n".join("    " + line for line in lines)


def run_single_test(tool, source_path, workdir):
    """Transpile, build, and run one suite test.

    Returns ``(name, status, detail)`` where ``name`` is the test filename
    (e.g. ``00001.c``), ``status`` is PASS/MISCOMPILE/UNSUPPORTED, and
    ``detail`` is a human-readable reason for anything that is not a PASS.
    """
    name = os.path.basename(source_path)
    stem = name[: -len(".c")]
    crate_dir = os.path.join(workdir, stem)
    if os.path.isdir(crate_dir):
        shutil.rmtree(crate_dir)

    rc, _stdout, stderr, timed_out = run_command(
        [tool, "--emit=crate", source_path, "-o", crate_dir, "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    if timed_out:
        return name, UNSUPPORTED, "transpile/build timed out after %ss" % TRANSPILE_BUILD_TIMEOUT
    if rc != 0:
        return name, UNSUPPORTED, first_line(stderr)

    binary = os.path.join(
        crate_dir, "target", "release", sanitize_crate_binary_name(stem)
    )
    if not os.path.isfile(binary):
        return name, MISCOMPILE, "build reported success but binary missing: %s" % binary

    expected_path = source_path + ".expected"
    expected = b""
    if os.path.isfile(expected_path):
        with open(expected_path, "rb") as handle:
            expected = handle.read()

    # Run inside the per-test crate directory so tests that create scratch
    # files (e.g. 00187.c's fred.txt) stay hermetic instead of writing into
    # the invoker's working directory. The binary path must be absolute
    # because the child's working directory is no longer the invoker's.
    rc, stdout, stderr, timed_out = run_command([os.path.abspath(binary)],
                                                RUN_TIMEOUT, cwd=crate_dir)
    if timed_out:
        return name, MISCOMPILE, "binary timed out after %ss" % RUN_TIMEOUT
    if rc != 0:
        detail = "binary exited %s (expected 0)" % rc
        stderr_line = first_line(stderr)
        if stderr_line:
            detail += "; stderr: " + stderr_line
        return name, MISCOMPILE, detail
    if stdout != expected:
        return name, MISCOMPILE, "stdout mismatch:\n" + output_diff_snippet(expected, stdout)
    return name, PASS, ""


def write_manifest(path, passing):
    """Rewrite the manifest at ``path`` with the sorted PASS set."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# c-testsuite single-exec conformance ledger for emitrust-cc.\n"
            "# One passing test filename per line.  Regenerated via:\n"
            "#   run_c_testsuite.py ... --update\n"
            "# Generated against the current build/bin/emitrust-cc; the\n"
            "# integration pass will regenerate this after the in-flight\n"
            "# switch/enum features land.\n"
        )
        for name in sorted(passing):
            handle.write(name + "\n")


def main(argv):
    """Entry point: run the suite, enforce the ledger, and report."""
    args = parse_args(argv)
    tool = resolve_tool(args.emitrust_cc)

    suite_dir = os.path.join(args.suite, "tests", "single-exec")
    if not os.path.isdir(suite_dir):
        raise SystemExit(
            "error: %s not found; is the c-testsuite submodule initialized"
            " (git submodule update --init)?" % suite_dir
        )
    sources = sorted(
        os.path.join(suite_dir, entry)
        for entry in os.listdir(suite_dir)
        if entry.endswith(".c")
    )
    if not sources:
        raise SystemExit("error: no tests found under %s" % suite_dir)

    os.makedirs(args.workdir, exist_ok=True)
    quarantined = set()
    if args.known_miscompiles:
        quarantined = load_name_list(args.known_miscompiles, required=False)

    workers = min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(
            pool.map(lambda src: run_single_test(tool, src, args.workdir), sources)
        )

    passing = {name for name, status, _ in results if status == PASS}
    miscompiles = [(name, detail) for name, status, detail in results if status == MISCOMPILE]
    unsupported = [(name, detail) for name, status, detail in results if status == UNSUPPORTED]

    failures = []

    new_miscompiles = [(n, d) for n, d in miscompiles if n not in quarantined]
    old_miscompiles = [(n, d) for n, d in miscompiles if n in quarantined]
    for name, detail in miscompiles:
        tag = "known, quarantined" if name in quarantined else "NEW"
        print("MISCOMPILE (%s): %s" % (tag, name))
        print("  " + detail.replace("\n", "\n  "))
    if new_miscompiles:
        failures.append(
            "%d unquarantined MISCOMPILE(s): %s -- a successfully transpiled"
            " test produced wrong behavior; this is a compiler bug and always"
            " fails, manifest or not."
            % (len(new_miscompiles), ", ".join(n for n, _ in new_miscompiles))
        )

    if args.update:
        write_manifest(args.manifest, passing)
        print("manifest updated: %s (%d tests)" % (args.manifest, len(passing)))
    else:
        manifest = load_name_list(args.manifest, required=True)
        regressions = sorted(manifest - passing)
        improvements = sorted(passing - manifest)
        if regressions:
            failures.append(
                "%d regression(s) -- in manifest but not passing: %s"
                % (len(regressions), ", ".join(regressions))
            )
        if improvements:
            failures.append(
                "%d improvement(s) -- passing but not in manifest: %s."
                " Re-run with --update to ratchet the ledger forward."
                % (len(improvements), ", ".join(improvements))
            )

    print(
        "summary: total=%d transpiled=%d passed=%d miscompiled=%d"
        " (quarantined=%d) unsupported=%d"
        % (
            len(results),
            len(passing) + len(miscompiles),
            len(passing),
            len(miscompiles),
            len(old_miscompiles),
            len(unsupported),
        )
    )

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
