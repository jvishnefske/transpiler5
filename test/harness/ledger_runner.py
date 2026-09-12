#!/usr/bin/env python3
"""Shared conformance-ledger runner (FR-243).

One runner body for the three suite ledgers -- CTestSuite (external
c-testsuite), Cpp17Suite (in-repo C++17 corpus), and CppStdSuite
(llvm-test-suite submodule) -- parameterized by a frozen ``SuiteSpec``.
The per-suite entry points stay at their historical paths
(test/CTestSuite/run_c_testsuite.py etc.) as thin wrappers, so lit RUN
lines and every documented invocation are unchanged; each wrapper's
behavior is byte-identical to the clone it replaced (verified by paired
--update-into-scratch manifests and non-update stdout diffs when this
module landed).

NOT covered: test/RealWorld/run_realworld.py looks like a fourth clone
but is a different species -- scripts/check-rejection-ledger.py asserts
against its classifier tables -- and deliberately stays standalone.

Classification model shared by all suites:

* UNSUPPORTED -- emitrust-cc rejected the file (or the transpile+build
  step timed out).  Suites deliberately contain material beyond the
  supported subset, so this is a legal steady state and never a failure
  by itself.
* PASS        -- the transpiled binary exited 0 and its stdout matched
  the reference ``.expected`` bytes exactly.
* MISCOMPILE  -- emitrust-cc accepted and built the test, but the binary
  crashed, timed out, exited nonzero, or printed the wrong output.
  Silent wrong code must never pass CI, so any MISCOMPILE not explicitly
  quarantined in the known-miscompiles file fails the run regardless of
  the manifest.

The PASS set is compared against a manifest (one test name per line,
``#`` comments allowed).  A mismatch in either direction is an error:
tests in the manifest that no longer pass are regressions; tests that
newly pass are improvements and the developer is told to re-run with
``--update`` to ratchet the ledger forward.

Summary side-channel: when ``EMITRUST_LEDGER_SUMMARY_DIR`` is set, the
report this runner prints (per-miscompile lines, the ``summary:`` line,
any ``FAIL:`` lines) is ALSO written to ``$DIR/<suite name>.txt``.  lit
swallows passing-test stdout, which is why CI used to duplicate the
c-testsuite run just to see its numbers; test/lit.cfg.py forwards the
variable into lit's curated child environment so CI can collect the
reports from the gating run itself.
"""

import argparse
import difflib
import os
import shutil
import signal
import subprocess
from collections import namedtuple
from concurrent.futures import ThreadPoolExecutor

# Hard ceilings, in seconds.  Transpile+cargo-build is the slow step; the
# produced binaries are tiny deterministic programs.
TRANSPILE_BUILD_TIMEOUT = 60
RUN_TIMEOUT = 10

# Classification labels.
PASS = "PASS"
MISCOMPILE = "MISCOMPILE"
UNSUPPORTED = "UNSUPPORTED"

# Frozen per-suite parameterization.  Fields:
#   name              -- short suite name; also the summary side-channel
#                        filename stem ("c-testsuite" -> c-testsuite.txt).
#   description       -- argparse description (the wrapper's __doc__).
#   source_suffix     -- ".c" or ".cpp"; also selects the glob.
#   suite_subdir      -- path components appended to --suite to reach the
#                        test sources (() = --suite itself).
#   uses_candidates   -- adds a required --candidates flag: enumerate from
#                        a committed relative-path list instead of globbing
#                        the suite directory (llvm-test-suite is far too
#                        big to glob, and the curated list IS the corpus).
#   uses_expected_dir -- adds a required --expected-dir flag: reference
#                        stdout lives at <dir>/<name>.expected instead of
#                        next to the source (submodule sources are
#                        read-only to us).
#   suite_help        -- help text for --suite.
#   missing_suite_hint-- SystemExit message (one %s: the missing path).
#   manifest_header   -- verbatim header block written by --update.
SuiteSpec = namedtuple(
    "SuiteSpec",
    [
        "name",
        "description",
        "source_suffix",
        "suite_subdir",
        "uses_candidates",
        "uses_expected_dir",
        "suite_help",
        "missing_suite_hint",
        "manifest_header",
    ],
)


def parse_args(argv, spec):
    """Parse command-line arguments into an argparse Namespace."""
    parser = argparse.ArgumentParser(description=spec.description)
    parser.add_argument(
        "--emitrust-cc",
        required=True,
        dest="emitrust_cc",
        help="Path to (or bare name of) the emitrust-cc binary.",
    )
    parser.add_argument(
        "--suite",
        required=True,
        help=spec.suite_help,
    )
    if spec.uses_candidates:
        parser.add_argument(
            "--candidates",
            required=True,
            help="Committed list of suite-relative source paths to run;"
            " the curated list is the corpus, the suite is never globbed.",
        )
    if spec.uses_expected_dir:
        parser.add_argument(
            "--expected-dir",
            required=True,
            dest="expected_dir",
            help="Directory holding <relative-path>.expected reference"
            " stdout files.",
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


def run_command(cmd, timeout, cwd=None, env=None):
    """Run ``cmd`` with a hard timeout, returning (rc, stdout, stderr, timed_out).

    Uses a fresh process group and SIGKILLs the whole group on timeout so
    that lingering grandchildren (e.g. rustc under cargo) cannot keep the
    output pipes open and hang the runner.

    ``env`` is an optional dict of extra environment variables merged over
    the inherited ``os.environ`` (e.g. ``CARGO_TARGET_DIR`` for the cargo
    child spawned by emitrust-cc).
    """
    full_env = None
    if env:
        full_env = dict(os.environ)
        full_env.update(env)
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=cwd,
        env=full_env,
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


def run_single_test(tool, source_path, name, workdir, spec, expected_dir=None):
    """Transpile, build, and run one suite test.

    Each test builds into its own crate-local ``target`` directory.  A
    shared ``CARGO_TARGET_DIR`` (as test/Fuzz uses for its sequential
    builds) was measured and rejected for this runner: the 8 concurrent
    cargo builds serialize on cargo's target-dir flock, and the full
    220-test ledger went from a 19.9s median (3 runs: 19.9/19.6/19.9) to a
    109.8s median (3 runs: 109.8/109.8/109.7) -- identical total CPU, 5.5x
    the wall clock.  Per-crate target dirs keep the pool actually parallel.
    (Crate names would not have collided: the sanitized NNNNN stems are
    unique; the flock alone kills the idea.)

    ``name`` is the test's ledger name: the bare filename for glob-mode
    suites (``00001.c``), the suite-relative path for candidates-mode
    suites.  Candidates-mode per-crate workdirs are keyed on the sanitized
    relative path because basenames collide across suite directories.

    Returns ``(name, status, detail)`` where ``status`` is
    PASS/MISCOMPILE/UNSUPPORTED and ``detail`` is a human-readable reason
    for anything that is not a PASS.
    """
    stem = os.path.basename(name)[: -len(spec.source_suffix)]
    if spec.uses_expected_dir:
        workdir_key = sanitize_crate_binary_name(name[: -len(spec.source_suffix)])
    else:
        workdir_key = stem
    crate_dir = os.path.join(workdir, workdir_key)
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

    if spec.uses_expected_dir:
        expected_path = os.path.join(expected_dir, name + ".expected")
    else:
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


def write_manifest(path, passing, spec):
    """Rewrite the manifest at ``path`` with the sorted PASS set."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(spec.manifest_header)
        for name in sorted(passing):
            handle.write(name + "\n")


def collect_sources(args, spec, suite_dir):
    """Return the ordered [(source_path, ledger_name), ...] work list."""
    if spec.uses_candidates:
        rels = sorted(load_name_list(args.candidates, required=True))
        if not rels:
            raise SystemExit("error: no candidates listed in %s" % args.candidates)
        missing = [rel for rel in rels if not os.path.isfile(os.path.join(suite_dir, rel))]
        if missing:
            raise SystemExit(
                "error: %d candidate(s) missing under %s (stale candidates"
                " list or submodule pin?): %s"
                % (len(missing), suite_dir, ", ".join(missing[:5]))
            )
        keys = [sanitize_crate_binary_name(r[: -len(spec.source_suffix)]) for r in rels]
        if len(set(keys)) != len(keys):
            dupes = sorted({k for k in keys if keys.count(k) > 1})
            raise SystemExit(
                "error: candidate paths collide after workdir sanitization: %s"
                % ", ".join(dupes)
            )
        return [(os.path.join(suite_dir, rel), rel) for rel in rels]
    sources = sorted(
        entry for entry in os.listdir(suite_dir) if entry.endswith(spec.source_suffix)
    )
    if not sources:
        raise SystemExit("error: no tests found under %s" % suite_dir)
    return [(os.path.join(suite_dir, entry), entry) for entry in sources]


def main(argv, spec):
    """Entry point: run the suite, enforce the ledger, and report."""
    args = parse_args(argv, spec)
    tool = resolve_tool(args.emitrust_cc)

    suite_dir = os.path.join(args.suite, *spec.suite_subdir)
    if not os.path.isdir(suite_dir):
        raise SystemExit(spec.missing_suite_hint % suite_dir)
    work = collect_sources(args, spec, suite_dir)
    expected_dir = args.expected_dir if spec.uses_expected_dir else None

    os.makedirs(args.workdir, exist_ok=True)
    quarantined = set()
    if args.known_miscompiles:
        quarantined = load_name_list(args.known_miscompiles, required=False)

    workers = min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(
            pool.map(
                lambda item: run_single_test(
                    tool, item[0], item[1], args.workdir, spec, expected_dir
                ),
                work,
            )
        )

    # Everything below reports through say() so the identical text can be
    # teed into the EMITRUST_LEDGER_SUMMARY_DIR side-channel file.
    report = []

    def say(msg):
        print(msg)
        report.append(msg)

    passing = {name for name, status, _ in results if status == PASS}
    miscompiles = [(name, detail) for name, status, detail in results if status == MISCOMPILE]
    unsupported = [(name, detail) for name, status, detail in results if status == UNSUPPORTED]

    failures = []

    new_miscompiles = [(n, d) for n, d in miscompiles if n not in quarantined]
    old_miscompiles = [(n, d) for n, d in miscompiles if n in quarantined]
    for name, detail in miscompiles:
        tag = "known, quarantined" if name in quarantined else "NEW"
        say("MISCOMPILE (%s): %s" % (tag, name))
        say("  " + detail.replace("\n", "\n  "))
    if new_miscompiles:
        failures.append(
            "%d unquarantined MISCOMPILE(s): %s -- a successfully transpiled"
            " test produced wrong behavior; this is a compiler bug and always"
            " fails, manifest or not."
            % (len(new_miscompiles), ", ".join(n for n, _ in new_miscompiles))
        )

    if args.update:
        write_manifest(args.manifest, passing, spec)
        say("manifest updated: %s (%d tests)" % (args.manifest, len(passing)))
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

    say(
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

    rc = 0
    if failures:
        for failure in failures:
            say("FAIL: " + failure)
        rc = 1

    summary_dir = os.environ.get("EMITRUST_LEDGER_SUMMARY_DIR")
    if summary_dir:
        os.makedirs(summary_dir, exist_ok=True)
        out = os.path.join(summary_dir, spec.name + ".txt")
        with open(out, "w", encoding="utf-8") as handle:
            handle.write("\n".join(report) + "\n")
    return rc


if __name__ == "__main__":
    raise SystemExit(
        "ledger_runner.py is a library; run a suite wrapper instead"
        " (test/CTestSuite/run_c_testsuite.py, test/Cpp17Suite/"
        "run_cpp17_suite.py, test/CppStdSuite/run_cppstd_suite.py)"
    )
