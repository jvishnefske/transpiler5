#!/usr/bin/env python3
"""Real-world C corpus runner for emitrust-cc (Track 4, W4.0).

Drives every program under a corpus directory through
``emitrust-cc --emit=crate --build`` and classifies the outcome:

  * REJECTED   -- the transpiler emitted a located diagnostic (rc != 0).
                  Tagged by BLOCKER category (see ``classify_blocker``); this
                  is the DEMAND SIGNAL the corpus exists to generate.
  * TRANSPILED -- the crate built and its stdout matched a clang-compiled
                  native build of the same sources (the differential oracle).
  * MISCOMPILE -- the crate built but crashed, exited non-zero, or produced
                  stdout that differs from the native build; also a build that
                  reports success with no binary. Always fails, quarantine
                  aside -- a successfully transpiled program must be correct.

Modeled on ``test/CTestSuite/run_c_testsuite.py`` (shared helpers copied so
this harness stays self-contained): per-crate ``target`` directories (a shared
CARGO_TARGET_DIR was measured and rejected there -- cargo's target-dir flock
serializes the pool), the two-way ratchet against a manifest of programs
EXPECTED to transpile, and an optional quarantine list.

A program is either a single top-level ``<name>.c`` (one TU) or an immediate
subdirectory ``<name>/`` whose ``*.c`` files compile together (multi-TU).
NOTE: ``argv`` VALUES are dropped at import (emitrust-cc's main wrapper passes
only ``argc``), so command-line-argument programs are expected to reject.
"""

import argparse
import difflib
import os
import re
import shutil
import signal
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

TRANSPILE_BUILD_TIMEOUT = 120
NATIVE_BUILD_TIMEOUT = 60
RUN_TIMEOUT = 10

REJECTED = "REJECTED"
TRANSPILED = "TRANSPILED"
MISCOMPILE = "MISCOMPILE"


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitrust-cc", required=True, dest="emitrust_cc",
                        help="Path to (or bare name of) the emitrust-cc binary.")
    parser.add_argument("--clang", default="clang",
                        help="C compiler for the native differential oracle.")
    parser.add_argument("--corpus", required=True,
                        help="Directory of corpus programs (*.c and <name>/ subdirs).")
    parser.add_argument("--manifest", required=True,
                        help="Manifest of program names EXPECTED to transpile.")
    parser.add_argument("--workdir", required=True,
                        help="Scratch directory for per-program crate output.")
    parser.add_argument("--known-miscompiles", default=None,
                        help="Optional quarantine list of known-miscompiling program names.")
    parser.add_argument("--update", action="store_true",
                        help="Rewrite the manifest with the current TRANSPILED set"
                             " instead of failing on drift.")
    return parser.parse_args(argv)


def resolve_tool(value):
    """Resolve a tool argument to an absolute executable, or fall back to PATH."""
    if os.sep in value:
        candidate = os.path.abspath(value)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
        raise SystemExit("error: tool not executable: %s" % candidate)
    found = shutil.which(value)
    if found is None:
        raise SystemExit("error: cannot find %r on PATH" % value)
    return found


def sanitize_crate_binary_name(stem):
    """Mirror emitrust-cc's crate-name sanitization (CrateEmitter sanitizeCrateName)."""
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
    names = set()
    if not os.path.isfile(path):
        if required:
            raise SystemExit("error: manifest not found: %s" % path)
        return names
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.split("#", 1)[0].strip()
            if stripped:
                names.add(stripped)
    return names


def run_command(cmd, timeout, cwd=None, env=None):
    """Run ``cmd`` with a hard timeout; SIGKILL the whole group on timeout."""
    full_env = None
    if env:
        full_env = dict(os.environ)
        full_env.update(env)
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, cwd=cwd, env=full_env, start_new_session=True,
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
    for line in data.decode("utf-8", errors="replace").splitlines():
        if line.strip():
            return line.strip()
    return ""


def output_diff_snippet(expected, actual, limit=12):
    diff = difflib.unified_diff(
        expected.decode("utf-8", errors="replace").splitlines(),
        actual.decode("utf-8", errors="replace").splitlines(),
        fromfile="native", tofile="crate", lineterm="",
    )
    lines = list(diff)[:limit]
    if not lines:
        return "(outputs differ only in trailing bytes/newlines)"
    return "\n".join("    " + line for line in lines)


# Blocker-tag heuristic over the first diagnostic line. Ordered; first match
# wins. free/realloc/malloc/calloc reject via the generic system-header path,
# so the symbol name is parsed out to separate dynamic-memory from other libc.
# The generic "pointer assigned a non-address value" / "no known target object"
# wordings are shared by local-malloc AND strchr-result binds, so those are
# refined by reading the cited source line (the diagnostic carries file:line).
_SYS_HEADER_RE = re.compile(r"call to '([^']+)' declared in a system header")
_LOC_RE = re.compile(r"^(.+?):(\d+):\d+: error:")
_DYNMEM_NAMES = {"malloc", "calloc", "realloc", "free", "aligned_alloc"}
_ALLOC_KEYWORDS = ("malloc", "calloc", "realloc", "aligned_alloc")
_BLOCKER_SUBSTRINGS = [
    ("use of main's argv", "argv"),
    ("pointer-to-pointer", "ptr-to-ptr"),
    ("returned pointer value", "returned-pointer"),
    ("pointer return type", "returned-pointer"),
    ("return sites disagree", "returned-pointer"),
    ("global pointer bound to a string literal", "global-string-cursor"),
    ("unsupported: allocation", "dynamic-memory"),
    ("pointer struct member of an externally", "pointer-member-cross-tu"),
    ("static-binding model", "self-ref-pointer-member"),
    ("variadic function", "variadic-cross-tu"),
]
# Wordings shared across blockers; refined by the cited source line's content.
_AMBIGUOUS_POINTER = ("pointer assigned a non-address value", "with no known target object")


def _cited_source_line(diagnostic):
    """Read the source line the diagnostic points at (best effort), or ''."""
    match = _LOC_RE.match(diagnostic)
    if not match:
        return ""
    path, line_no = match.group(1), int(match.group(2))
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for index, text in enumerate(handle, start=1):
                if index == line_no:
                    return text
    except OSError:
        pass
    return ""


def classify_blocker(diagnostic):
    """Map a first-line diagnostic to a coarse blocker tag for the survey."""
    if "PLEASE submit a bug report" in diagnostic or "Stack dump" in diagnostic:
        return "crash"
    match = _SYS_HEADER_RE.search(diagnostic)
    if match:
        name = match.group(1)
        return "dynamic-memory" if name in _DYNMEM_NAMES else "libc:" + name
    for needle, tag in _BLOCKER_SUBSTRINGS:
        if needle in diagnostic:
            return tag
    if any(needle in diagnostic for needle in _AMBIGUOUS_POINTER):
        source = _cited_source_line(diagnostic)
        if any(kw in source for kw in _ALLOC_KEYWORDS):
            return "dynamic-memory"
        if "strchr" in source or "strrchr" in source:
            return "strchr-result-bind"
        return "pointer-local-nonaddress"
    return "other"


def discover_programs(corpus_dir):
    """Return sorted (name, [source_paths]) pairs: top-level *.c = single TU,
    each immediate subdir with *.c = one multi-TU program."""
    programs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        if os.path.isfile(path) and entry.endswith(".c"):
            programs.append((entry[: -len(".c")], [path]))
        elif os.path.isdir(path):
            sources = sorted(
                os.path.join(path, f) for f in os.listdir(path) if f.endswith(".c")
            )
            if sources:
                programs.append((entry, sources))
    return programs


def run_single_program(tool, clang, name, sources, workdir):
    """Transpile+build, then differentially validate against a native build."""
    crate_dir = os.path.join(workdir, name)
    if os.path.isdir(crate_dir):
        shutil.rmtree(crate_dir)

    rc, _out, stderr, timed_out = run_command(
        [tool, "--emit=crate", *sources, "-o", crate_dir, "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    if timed_out:
        return name, REJECTED, "timeout", "transpile/build timed out after %ss" % TRANSPILE_BUILD_TIMEOUT
    if rc != 0:
        detail = first_line(stderr)
        return name, REJECTED, classify_blocker(detail), detail

    binary = os.path.join(crate_dir, "target", "release",
                          sanitize_crate_binary_name(name))
    if not os.path.isfile(binary):
        return name, MISCOMPILE, "", "build reported success but binary missing: %s" % binary

    # Native oracle: compile the same sources with clang and diff stdout.
    native = os.path.join(crate_dir, "native_oracle")
    rc, _o, nstderr, ntimed = run_command(
        [clang, "-std=c11", "-w", *sources, "-o", native], NATIVE_BUILD_TIMEOUT)
    if ntimed or rc != 0:
        return name, MISCOMPILE, "", "native clang build failed: " + first_line(nstderr)

    n_rc, n_out, _ne, n_timed = run_command([os.path.abspath(native)], RUN_TIMEOUT, cwd=crate_dir)
    if n_timed or n_rc != 0:
        return name, MISCOMPILE, "", "native oracle run failed (rc=%s)" % n_rc

    c_rc, c_out, c_err, c_timed = run_command([os.path.abspath(binary)], RUN_TIMEOUT, cwd=crate_dir)
    if c_timed:
        return name, MISCOMPILE, "", "crate binary timed out after %ss" % RUN_TIMEOUT
    if c_rc != 0:
        detail = "crate binary exited %s (expected %s)" % (c_rc, n_rc)
        line = first_line(c_err)
        if line:
            detail += "; stderr: " + line
        return name, MISCOMPILE, "", detail
    if c_out != n_out:
        return name, MISCOMPILE, "", "stdout mismatch vs native:\n" + output_diff_snippet(n_out, c_out)
    return name, TRANSPILED, "", ""


def write_manifest(path, transpiled):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# RealWorld corpus: program names emitrust-cc transpiles AND whose\n"
            "# crate output matches a clang-native build (Track 4, W4.0).\n"
            "# One program name per line. Regenerated via: run_realworld.py ... --update\n"
        )
        for name in sorted(transpiled):
            handle.write(name + "\n")


def main(argv):
    args = parse_args(argv)
    tool = resolve_tool(args.emitrust_cc)
    clang = resolve_tool(args.clang)

    if not os.path.isdir(args.corpus):
        raise SystemExit("error: corpus directory not found: %s" % args.corpus)
    programs = discover_programs(args.corpus)
    if not programs:
        raise SystemExit("error: no corpus programs found under %s" % args.corpus)

    os.makedirs(args.workdir, exist_ok=True)
    quarantined = load_name_list(args.known_miscompiles, required=False) if args.known_miscompiles else set()

    workers = min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(
            lambda p: run_single_program(tool, clang, p[0], p[1], args.workdir), programs))

    transpiled = {name for name, status, _t, _d in results if status == TRANSPILED}
    rejected = [(n, t, d) for n, s, t, d in results if s == REJECTED]
    miscompiles = [(n, d) for n, s, _t, d in results if s == MISCOMPILE]

    # Per-program report, sorted by name.
    for name, status, tag, detail in sorted(results):
        if status == TRANSPILED:
            print("TRANSPILED  %s" % name)
        elif status == REJECTED:
            print("REJECTED    %s  [%s]  %s" % (name, tag, detail))
        else:
            mark = "known, quarantined" if name in quarantined else "NEW"
            print("MISCOMPILE (%s): %s" % (mark, name))
            print("  " + detail.replace("\n", "\n  "))

    # Blocker-tag tabulation (the W4.1 survey signal).
    counts = {}
    for _name, tag, _detail in rejected:
        counts[tag] = counts.get(tag, 0) + 1
    if counts:
        print("\nblocker tabulation (rejected programs by tag):")
        for tag, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
            print("  %-24s %d" % (tag, count))

    failures = []
    new_miscompiles = [(n, d) for n, d in miscompiles if n not in quarantined]
    if new_miscompiles:
        failures.append(
            "%d unquarantined MISCOMPILE(s): %s -- a transpiled program produced"
            " wrong behavior; this always fails, manifest or not."
            % (len(new_miscompiles), ", ".join(n for n, _ in new_miscompiles)))

    if args.update:
        write_manifest(args.manifest, transpiled)
        print("manifest updated: %s (%d transpiled)" % (args.manifest, len(transpiled)))
    else:
        manifest = load_name_list(args.manifest, required=True)
        regressions = sorted(manifest - transpiled)
        improvements = sorted(transpiled - manifest)
        if regressions:
            failures.append("%d regression(s) -- in manifest but no longer transpiling: %s"
                            % (len(regressions), ", ".join(regressions)))
        if improvements:
            failures.append("%d improvement(s) -- transpiling but not in manifest: %s."
                            " Re-run with --update to ratchet forward."
                            % (len(improvements), ", ".join(improvements)))

    print("\nsummary: total=%d transpiled=%d rejected=%d miscompiled=%d (quarantined=%d)"
          % (len(results), len(transpiled), len(rejected), len(miscompiles),
             len(miscompiles) - len(new_miscompiles)))

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
