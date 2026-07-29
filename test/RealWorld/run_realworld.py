#!/usr/bin/env python3
"""Real-world C / C++ corpus runner for emitrust-cc (Track 4, W4.0; FR-46).

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

Two corpus kinds share this one harness (``--corpus-kind``):

``c`` (the default, ``test/RealWorld/Inputs``)
    A program is either a single top-level ``<name>.c`` (one TU) or an
    immediate subdirectory ``<name>/`` whose ``*.c`` files compile together
    (multi-TU). The native oracle is ``clang -std=c11``. The manifest
    (``--manifest-format names``) is the flat set of program names EXPECTED
    to transpile. NOTE: ``argv`` VALUES are dropped at import (emitrust-cc's
    main wrapper passes only ``argc``), so command-line-argument programs are
    expected to reject.

``cpp`` (FR-46, ``test/RealWorld/Cpp/Inputs``)
    A program is an immediate subdirectory holding a ``compile_commands.json``
    that lists its ``.cpp`` translation units and their include directories.
    Paths inside that file are RELATIVE (so the corpus is checked in
    machine-independent) and are made absolute here, against the project
    directory. The native oracle is ``clang++ -std=c++17``. Because the C++
    input subset is young, most of this corpus is EXPECTED to reject, so the
    manifest records the OUTCOME of every project (``--manifest-format
    outcomes``) rather than only the transpiled set: that keeps the same
    two-way ratchet (a TRANSPILED project that stops transpiling is a
    regression; a REJECTED project that starts transpiling demands a
    ``--update``) while also making the checked-in manifest a readable,
    ranked backlog of C++ blockers.

Scoring in ``cpp`` mode is by OUTCOME only. Per-ITEM scoring (how MUCH of a
project ported, rather than whether all of it did) arrives with FR-44's
``--incremental`` mode, which is landing on a parallel track; see the
"FR-44 SEAM" block below for exactly where it plugs in. This runner
deliberately does NOT guess FR-44's report format.
"""

import argparse
import difflib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
from collections import namedtuple
from concurrent.futures import ThreadPoolExecutor

TRANSPILE_BUILD_TIMEOUT = 120
NATIVE_BUILD_TIMEOUT = 60
RUN_TIMEOUT = 10

REJECTED = "REJECTED"
TRANSPILED = "TRANSPILED"
MISCOMPILE = "MISCOMPILE"

#: One corpus program: a display name, its translation units, and the include
#: directories its compilation needs (always empty for the C corpus, which has
#: no out-of-directory headers).
Program = namedtuple("Program", "name sources include_dirs")

#: One measured outcome. ``items`` is the FR-44 per-item payload; it is
#: ``None`` until FR-44 lands (see the FR-44 SEAM block).
Result = namedtuple("Result", "name status tag detail items")


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitrust-cc", required=True, dest="emitrust_cc",
                        help="Path to (or bare name of) the emitrust-cc binary.")
    parser.add_argument("--clang", default="clang",
                        help="Native differential-oracle compiler. Defaults to"
                             " 'clang'; pass 'clang++' with --corpus-kind cpp.")
    parser.add_argument("--corpus-kind", choices=("c", "cpp"), default="c",
                        dest="corpus_kind",
                        help="Corpus layout and language: 'c' (top-level *.c"
                             " plus multi-TU subdirs, clang -std=c11 oracle) or"
                             " 'cpp' (compile_commands.json-described project"
                             " subdirs, clang++ -std=c++17 oracle).")
    parser.add_argument("--corpus", required=True,
                        help="Directory of corpus programs.")
    parser.add_argument("--manifest", required=True,
                        help="Expectation manifest; see --manifest-format.")
    parser.add_argument("--manifest-format", choices=("names", "outcomes"),
                        default="names", dest="manifest_format",
                        help="'names': a flat list of program names EXPECTED to"
                             " transpile (the C corpus). 'outcomes': one"
                             " '<name> <OUTCOME> [tag]' line per program (the"
                             " C++ corpus, most of which is expected to"
                             " reject).")
    parser.add_argument("--workdir", required=True,
                        help="Scratch directory for per-program crate output.")
    parser.add_argument("--known-miscompiles", default=None,
                        help="Optional quarantine list of known-miscompiling program names.")
    parser.add_argument("--update", action="store_true",
                        help="Rewrite the manifest with the current measured"
                             " outcomes instead of failing on drift.")
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


#: ClangTool's per-file progress chatter, printed on stderr ahead of any
#: diagnostic whenever emitrust-cc is handed more than one translation unit.
_TOOL_PROGRESS_RE = re.compile(r"^\[\d+/\d+\] Processing file ")


def first_diagnostic_line(data):
    """The first line of ``data`` that is actually a diagnostic.

    ``first_line`` alone is wrong for a MULTI-TU compile: ClangTool prints a
    "[k/n] Processing file ..." line per input before any diagnostic reaches
    stderr, so the naive first line is progress chatter and every multi-TU
    rejection would tag as "other". Progress lines are dropped, then the first
    line carrying "error:" wins; a compiler CRASH (which prints a stack dump
    and no "error:" line) falls back to the first remaining line, which is
    what ``classify_blocker``'s crash heuristic reads.
    """
    lines = [line.strip()
             for line in data.decode("utf-8", errors="replace").splitlines()
             if line.strip() and not _TOOL_PROGRESS_RE.match(line.strip())]
    for line in lines:
        if "error:" in line:
            return line
    return lines[0] if lines else ""


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

# C++-input blocker tags (FR-46). Every wording below is emitted only from a
# C++-only code path in lib/ImportC (a CXXRecordDecl walk, mapType's reference
# case, or the STL recognition table), so a C program can never match any of
# them -- the C corpus's tabulation is unchanged by this table's existence.
_CXX_BLOCKER_SUBSTRINGS = [
    ("base classes are not supported", "cxx-inheritance"),
    ("user-declared destructor", "cxx-destructor"),
    ("virtual or unresolved member call", "cxx-virtual-call"),
    ("unsupported: virtual method", "cxx-virtual"),
    ("overloaded operator", "cxx-operator-overload"),
    ("reference types are not yet supported", "cxx-references"),
    ("is not a recognized STL type", "stl-unrecognized-type"),
    ("is not a recognized STL method", "stl-unrecognized-method"),
    ("receiver is not a recognized STL", "stl-unrecognized-receiver"),
    ("unsupported top-level declaration", "unsupported-top-level-decl"),
]
# Generic dispatch fallbacks that name the offending AST node class. These are
# language-agnostic (a C program can reach them too); refining them from the
# catch-all "other" into a node-named tag makes the tabulation a directly
# actionable backlog. Tags are display-only for the C corpus, whose manifest
# records program names, not tags.
_NODE_NAMED_RE = [
    (re.compile(r"unsupported assignable expression: (\w+)"), "unsupported-assign-expr:"),
    (re.compile(r"unsupported expression: (\w+)"), "unsupported-expr:"),
    (re.compile(r"unsupported statement: (\w+)"), "unsupported-stmt:"),
]


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
    for needle, tag in _CXX_BLOCKER_SUBSTRINGS:
        if needle in diagnostic:
            return tag
    for pattern, prefix in _NODE_NAMED_RE:
        match = pattern.search(diagnostic)
        if match:
            return prefix + match.group(1)
    if any(needle in diagnostic for needle in _AMBIGUOUS_POINTER):
        source = _cited_source_line(diagnostic)
        if any(kw in source for kw in _ALLOC_KEYWORDS):
            return "dynamic-memory"
        if "strchr" in source or "strrchr" in source:
            return "strchr-result-bind"
        return "pointer-local-nonaddress"
    return "other"


def discover_c_programs(corpus_dir):
    """Return sorted Programs: top-level *.c = single TU, each immediate subdir
    with *.c = one multi-TU program."""
    programs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        if os.path.isfile(path) and entry.endswith(".c"):
            programs.append(Program(entry[: -len(".c")], [path], []))
        elif os.path.isdir(path):
            sources = sorted(
                os.path.join(path, f) for f in os.listdir(path) if f.endswith(".c")
            )
            if sources:
                programs.append(Program(entry, sources, []))
    return programs


def _entry_argv(entry):
    """The compile command of one compile_commands.json entry, as a list."""
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def _include_dirs_from_argv(argv, base):
    """Absolute include directories named by -I in ``argv`` (both the joined
    ``-Idir`` and the separated ``-I dir`` spellings), resolved against
    ``base``."""
    dirs = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        value = None
        if arg == "-I" and index + 1 < len(argv):
            value = argv[index + 1]
            index += 1
        elif arg.startswith("-I") and len(arg) > 2:
            value = arg[2:]
        if value is not None:
            dirs.append(os.path.normpath(os.path.join(base, value)))
        index += 1
    return dirs


def read_compile_commands(project_dir):
    """Parse ``project_dir/compile_commands.json`` into (sources, include_dirs).

    Every path in the checked-in file is RELATIVE (a machine-independent
    corpus); ``directory`` is resolved against ``project_dir`` and everything
    else against ``directory``, so absolute paths are produced here at run
    time and never stored in the repository. Absolute paths in the file, if a
    generator ever writes one, are honored as-is.
    """
    db_path = os.path.join(project_dir, "compile_commands.json")
    with open(db_path, "r", encoding="utf-8") as handle:
        entries = json.load(handle)

    sources = []
    include_dirs = []
    for entry in entries:
        directory = entry.get("directory", ".")
        if not os.path.isabs(directory):
            directory = os.path.join(project_dir, directory)
        directory = os.path.normpath(directory)

        source = entry["file"]
        if not os.path.isabs(source):
            source = os.path.join(directory, source)
        source = os.path.normpath(source)
        if not os.path.isfile(source):
            raise SystemExit("error: %s names a missing source: %s"
                             % (db_path, source))
        if source not in sources:
            sources.append(source)

        for include in _include_dirs_from_argv(_entry_argv(entry), directory):
            if include not in include_dirs:
                include_dirs.append(include)

    if not sources:
        raise SystemExit("error: %s lists no translation units" % db_path)
    return sorted(sources), include_dirs


def discover_cpp_programs(corpus_dir):
    """Return sorted Programs: each immediate subdirectory holding a
    compile_commands.json is one multi-TU C++ project."""
    programs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        if not os.path.isdir(path):
            continue
        if not os.path.isfile(os.path.join(path, "compile_commands.json")):
            continue
        sources, include_dirs = read_compile_commands(path)
        programs.append(Program(entry, sources, include_dirs))
    return programs


def discover_programs(corpus_dir, kind):
    """Dispatch discovery on the corpus kind."""
    if kind == "cpp":
        return discover_cpp_programs(corpus_dir)
    return discover_c_programs(corpus_dir)


# ---------------------------------------------------------------------------
# FR-44 SEAM -- per-item incremental scoring.
#
# The C++ corpus (FR-46) is graded by whole-project OUTCOME only: a project
# either transpiles end to end or it does not, and today essentially none of
# them do. That is a coarse signal -- it cannot distinguish "3 of 40 functions
# ported" from "0 of 40" -- and the finer one is FR-44's job: an
# `emitrust-cc --incremental` mode landing on a parallel track that reports
# per-ITEM (per function / per record) success.
#
# This is the ONE place that mode plugs in. When FR-44 lands:
#   * `collect_per_item_scores` runs the incremental invocation for a program
#     and returns its report, in WHATEVER shape FR-44 defines -- that format is
#     deliberately NOT invented here, and nothing below inspects the payload;
#   * the returned value rides along in `Result.items`, already threaded
#     through `run_single_program`, the thread pool, the report loop, and the
#     manifest writer;
#   * `write_outcome_manifest` grows a per-item section (its `items` argument
#     is already plumbed and currently always None), and `ratchet_outcomes`
#     grows the matching per-item comparison next to the existing per-project
#     one.
# No other part of this runner needs to change.
# ---------------------------------------------------------------------------
def collect_per_item_scores(tool, program, workdir):
    """FR-44 SEAM: return the per-item score report for ``program``, or None.

    Returns None unconditionally until FR-44's ``--incremental`` mode exists;
    every consumer treats None as "per-item scoring unavailable".
    """
    del tool, program, workdir  # Unused until FR-44 lands.
    return None


def run_single_program(tool, native_cc, native_std, program, workdir):
    """Transpile+build, then differentially validate against a native build."""
    name, sources, include_dirs = program
    crate_dir = os.path.join(workdir, name)
    if os.path.isdir(crate_dir):
        shutil.rmtree(crate_dir)
    include_flags = ["-I" + d for d in include_dirs]

    def result(status, tag, detail):
        return Result(name, status, tag, detail,
                      collect_per_item_scores(tool, program, workdir))

    rc, _out, stderr, timed_out = run_command(
        [tool, "--emit=crate", *sources, *include_flags, "-o", crate_dir,
         "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    if timed_out:
        return result(REJECTED, "timeout",
                      "transpile/build timed out after %ss" % TRANSPILE_BUILD_TIMEOUT)
    if rc != 0:
        detail = first_diagnostic_line(stderr)
        return result(REJECTED, classify_blocker(detail), detail)

    binary = os.path.join(crate_dir, "target", "release",
                          sanitize_crate_binary_name(name))
    if not os.path.isfile(binary):
        return result(MISCOMPILE, "",
                      "build reported success but binary missing: %s" % binary)

    # Native oracle: compile the same sources natively and diff stdout.
    native = os.path.join(crate_dir, "native_oracle")
    rc, _o, nstderr, ntimed = run_command(
        [native_cc, native_std, "-w", *include_flags, *sources, "-o", native],
        NATIVE_BUILD_TIMEOUT)
    if ntimed or rc != 0:
        return result(MISCOMPILE, "",
                      "native build failed: " + first_line(nstderr))

    n_rc, n_out, _ne, n_timed = run_command([os.path.abspath(native)], RUN_TIMEOUT, cwd=crate_dir)
    if n_timed or n_rc != 0:
        return result(MISCOMPILE, "", "native oracle run failed (rc=%s)" % n_rc)

    c_rc, c_out, c_err, c_timed = run_command([os.path.abspath(binary)], RUN_TIMEOUT, cwd=crate_dir)
    if c_timed:
        return result(MISCOMPILE, "", "crate binary timed out after %ss" % RUN_TIMEOUT)
    if c_rc != 0:
        detail = "crate binary exited %s (expected %s)" % (c_rc, n_rc)
        line = first_line(c_err)
        if line:
            detail += "; stderr: " + line
        return result(MISCOMPILE, "", detail)
    if c_out != n_out:
        return result(MISCOMPILE, "",
                      "stdout mismatch vs native:\n" + output_diff_snippet(n_out, c_out))
    return result(TRANSPILED, "", "")


def write_name_manifest(path, transpiled):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# RealWorld corpus: program names emitrust-cc transpiles AND whose\n"
            "# crate output matches a clang-native build (Track 4, W4.0).\n"
            "# One program name per line. Regenerated via: run_realworld.py ... --update\n"
        )
        for name in sorted(transpiled):
            handle.write(name + "\n")


def write_outcome_manifest(path, results):
    """Write the '<name> <OUTCOME> [tag]' manifest used by the C++ corpus."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# RealWorld C++ corpus (FR-46): the MEASURED outcome of every\n"
            "# project, one per line, as '<name> <OUTCOME> [blocker-tag]'.\n"
            "#\n"
            "# This corpus exists to GENERATE DEMAND for the C++ input subset,\n"
            "# not to be a conformance target: a REJECTED line is the expected,\n"
            "# honest baseline and a standing backlog item, not a failure. The\n"
            "# ratchet is two-way -- a TRANSPILED project that stops\n"
            "# transpiling fails the gate as a regression, and a REJECTED\n"
            "# project that starts transpiling fails until it is ratcheted\n"
            "# forward. The blocker tag is advisory: a changed tag is reported\n"
            "# loudly but does not fail the gate, since it is a heuristic over\n"
            "# diagnostic wording.\n"
            "#\n"
            "# Per-ITEM scoring (how much of a rejected project ported) is\n"
            "# FR-44's --incremental mode; see the FR-44 SEAM block in\n"
            "# run_realworld.py. No per-item section exists yet.\n"
            "#\n"
            "# Regenerated via: run_realworld.py ... --manifest-format outcomes"
            " --update\n"
        )
        for entry in sorted(results):
            line = "%s %s" % (entry.name, entry.status)
            if entry.tag:
                line += " " + entry.tag
            handle.write(line + "\n")
            # FR-44 SEAM: `entry.items`, when non-None, gets its per-item lines
            # written here under this project's line.


def load_outcome_manifest(path):
    """Parse an outcome manifest into {name: (status, tag)}."""
    if not os.path.isfile(path):
        raise SystemExit("error: manifest not found: %s" % path)
    expected = {}
    with open(path, "r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, start=1):
            stripped = line.split("#", 1)[0].strip()
            if not stripped:
                continue
            fields = stripped.split()
            if len(fields) < 2 or fields[1] not in (REJECTED, TRANSPILED, MISCOMPILE):
                raise SystemExit("error: %s:%d: expected '<name> <OUTCOME> [tag]'"
                                 % (path, lineno))
            expected[fields[0]] = (fields[1], fields[2] if len(fields) > 2 else "")
    return expected


def ratchet_outcomes(manifest_path, results):
    """Compare measured outcomes against the outcome manifest.

    Returns (failures, notices): failures fail the gate, notices are printed
    as advisory drift.
    """
    expected = load_outcome_manifest(manifest_path)
    measured = {entry.name: entry for entry in results}

    failures = []
    notices = []

    missing = sorted(set(expected) - set(measured))
    if missing:
        failures.append("%d manifest entr(ies) with no corpus project: %s."
                        " Re-run with --update after removing a project."
                        % (len(missing), ", ".join(missing)))

    unlisted = sorted(set(measured) - set(expected))
    if unlisted:
        failures.append("%d corpus project(s) missing from the manifest: %s."
                        " Re-run with --update to record the baseline."
                        % (len(unlisted), ", ".join(unlisted)))

    regressions = []
    improvements = []
    for name in sorted(set(expected) & set(measured)):
        want_status, want_tag = expected[name]
        got = measured[name]
        if got.status == want_status:
            if got.tag != want_tag:
                notices.append("%s: blocker tag drifted %r -> %r (advisory;"
                               " --update refreshes it)"
                               % (name, want_tag or "-", got.tag or "-"))
            continue
        if want_status == TRANSPILED:
            regressions.append("%s: %s -> %s" % (name, want_status, got.status))
        elif got.status == TRANSPILED:
            improvements.append("%s: %s -> %s" % (name, want_status, got.status))
        else:
            # REJECTED <-> MISCOMPILE. MISCOMPILE is separately fatal unless
            # quarantined; the reverse direction is progress worth ratcheting.
            improvements.append("%s: %s -> %s" % (name, want_status, got.status))

    if regressions:
        failures.append("%d regression(s) -- expected TRANSPILED, no longer: %s"
                        % (len(regressions), "; ".join(regressions)))
    if improvements:
        failures.append("%d outcome change(s) not in the manifest: %s."
                        " Re-run with --update to ratchet forward."
                        % (len(improvements), "; ".join(improvements)))
    return failures, notices


def main(argv):
    args = parse_args(argv)
    tool = resolve_tool(args.emitrust_cc)
    native_cc = resolve_tool(args.clang)
    native_std = "-std=c++17" if args.corpus_kind == "cpp" else "-std=c11"

    if not os.path.isdir(args.corpus):
        raise SystemExit("error: corpus directory not found: %s" % args.corpus)
    programs = discover_programs(args.corpus, args.corpus_kind)
    if not programs:
        raise SystemExit("error: no corpus programs found under %s" % args.corpus)

    os.makedirs(args.workdir, exist_ok=True)
    quarantined = load_name_list(args.known_miscompiles, required=False) if args.known_miscompiles else set()

    workers = min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(
            lambda p: run_single_program(tool, native_cc, native_std, p,
                                         args.workdir), programs))

    transpiled = {e.name for e in results if e.status == TRANSPILED}
    rejected = [e for e in results if e.status == REJECTED]
    miscompiles = [e for e in results if e.status == MISCOMPILE]

    # Per-program report, sorted by name.
    for entry in sorted(results):
        if entry.status == TRANSPILED:
            print("TRANSPILED  %s" % entry.name)
        elif entry.status == REJECTED:
            print("REJECTED    %s  [%s]  %s" % (entry.name, entry.tag, entry.detail))
        else:
            mark = "known, quarantined" if entry.name in quarantined else "NEW"
            print("MISCOMPILE (%s): %s" % (mark, entry.name))
            print("  " + entry.detail.replace("\n", "\n  "))

    # Blocker-tag tabulation (the W4.1 survey signal; for the C++ corpus, the
    # ranked backlog that sequences the next input-subset waves).
    counts = {}
    for entry in rejected:
        counts[entry.tag] = counts.get(entry.tag, 0) + 1
    if counts:
        print("\nblocker tabulation (rejected programs by tag):")
        for tag, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
            print("  %-28s %d" % (tag, count))

    failures = []
    new_miscompiles = [e for e in miscompiles if e.name not in quarantined]
    if new_miscompiles:
        failures.append(
            "%d unquarantined MISCOMPILE(s): %s -- a transpiled program produced"
            " wrong behavior; this always fails, manifest or not."
            % (len(new_miscompiles), ", ".join(e.name for e in new_miscompiles)))

    if args.update:
        if args.manifest_format == "outcomes":
            write_outcome_manifest(args.manifest, results)
            print("manifest updated: %s (%d project outcomes)"
                  % (args.manifest, len(results)))
        else:
            write_name_manifest(args.manifest, transpiled)
            print("manifest updated: %s (%d transpiled)" % (args.manifest, len(transpiled)))
    elif args.manifest_format == "outcomes":
        ratchet_failures, notices = ratchet_outcomes(args.manifest, results)
        for notice in notices:
            print("note: " + notice)
        failures.extend(ratchet_failures)
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
