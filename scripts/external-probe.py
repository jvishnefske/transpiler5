#!/usr/bin/env python3
"""External-repo probe sweep for emitrust-cc: import demand signal AND a build oracle.

WHY THIS SCRIPT EXISTS (FR-145). The project has run this sweep by hand for
months and thrown the harness away with the scratchpad every time, recreating
it from a prose description in the session memory. That is exactly why FR-140
(``non_snake_case`` deny on a double-underscore C identifier), FR-141 (an
``impl`` for a type the crate never defines, E0425) and FR-142 (a deferred
``let mut`` that is never reassigned, ``unused_mut`` deny) were each found
ONE AT A TIME, by accident, as by-products of unrelated work -- and every one
of them passed the full lit suite first. They are three instances of ONE
class:

    THE EMITTER CAN PRODUCE CODE ITS OWN TOOLCHAIN REJECTS, AND NOTHING
    NOTICES.

Nothing in the committed gates can see that class. EndToEnd byte-diff builds
its crates, but only over ``test/EndToEnd`` inputs, which are written to be
supportable. The three defects all came from EXTERNAL C reached through
``--incremental``, a mode no gate exercises at scale. The progress-JSON loop
that DOES exercise it measures IMPORT only and is structurally blind to "the
emitted crate does not compile".

So this harness does two things in one pass, and keeps their verdicts apart:

  1. IMPORT -- ``emitrust-cc --emit=crate --crate-type=lib --incremental``
     per unit, and the per-item ``emitrust-progress.json`` it writes is the
     demand signal: which construct, ranked, is blocking the most code.
  2. BUILD  -- ``cargo build --release --offline`` in every emitted crate.
     THE INVARIANT: an emitted crate must COMPILE. Not "the importer exited
     0", not "the lit suite is green".

NO THIRD-PARTY CODE IS VENDORED AND NOTHING IS CLONED HERE. The corpus root
is an ARGUMENT, exactly as in ``scripts/tractor-eval.py``; the repos are
cloned separately and pinned by the operator. Per FR-144's epoch lesson, a
rise or fall in any number below is attributable to nothing unless the
population is pinned, so the report records the corpus root, the enumeration
rule, and each repo's git HEAD when it has one.

USAGE
    scripts/external-probe.py <corpus-root> [--out DIR] [-j N] [-m REGEX]

ENUMERATION, and the denominator every rate is over
---------------------------------------------------
A REPO is an immediate subdirectory of the corpus root. A UNIT is one ``.c``
file inside a repo -- solo-TU, which is what the loop measures, because a
solo-TU import routes undefined helpers through the FR-52 ``Externals`` trait
and therefore exercises declaration-only signatures too.

Directories named in ``SKIP_DIRS`` (tests, examples, fuzzers, build outputs,
vendored deps) are pruned: they are not the library under study and their
failures would drown the signal from the code that is. Pruning is by
DIRECTORY name only, deliberately -- a root-level ``tests.c`` is still a
unit, because a filename heuristic on top of a directory one is one more
place for the denominator to drift silently, and a test driver is real C the
emitter has to handle either way. A repo with no unit after pruning is
EXCLUDED WITH A REASON and reported, never silently dropped: a header-only
library (``jsmn``, ``uthash``) whose only ``.c`` files are its own tests
falls out here, and saying so is the point.

Include directories are derived per unit and are confined to the unit's own
repo: the unit's directory, its ``include``, its parent and its parent's
``include``, then the repo root and the repo's
``include``/``src``/``inc``/``lib``. The confinement matters -- ``<unit
dir>/..`` at the repo root would be the CORPUS root, and one repo's headers
would then satisfy another repo's ``#include`` and quietly change what is
being measured. ``lib`` earns its place in the list: lz4 keeps its public
headers there and its ``programs/`` drivers ``#include "lz4.h"`` unqualified.

No build system is configured for any repo. A unit that needs a generated
header (``config.h`` and friends) therefore fails to EMIT, and is reported as
an emit failure with its diagnostic rather than being hidden.

OUTCOMES, and why they are not merged
-------------------------------------
Conflating "did not emit" with "emitted and failed to build" is precisely
what hid this class for months, so the two are never summed:

  ``BUILT``              emitted a crate AND ``cargo build`` accepted it.
                         THE ONLY SUCCESS.
  ``BUILD_FAIL``         a crate was emitted and cargo REJECTED it. This is
                         the FR-145 class; each one is reported in full.
  ``BUILD_TIMEOUT``      cargo neither accepted nor rejected it in time. Not
                         a build failure -- no verdict was reached.
  ``BUILD_SKIPPED``      a crate was emitted but no build was attempted
                         (``--no-build``, or cargo is not installed). NEVER a
                         success; the count is printed next to the headline.
  ``EMIT_EMPTY``         ``emitrust-cc`` exited 0 but wrote no ``src/*.rs``.
                         Its own guard: an exit code alone cannot tell this
                         from success by inspection, and a zero-iteration
                         fallthrough of exactly this shape once inflated a
                         reported figure to 168/252 in this project.
  ``EMIT_FAIL``          ``emitrust-cc`` refused the input (a located
                         rejection is a FEATURE) or crashed.
  ``EMIT_TIMEOUT``       ``emitrust-cc`` did not finish.

RANKING, and the mistake it is written to avoid
-----------------------------------------------
RANK BY FUNCTION ITEMS, NOT TOTAL ITEMS. ``emitrust-progress.json`` counts
one item per (unit, symbol) across ALL kinds, so a record declared in a
header included by 18 translation units contributes EIGHTEEN ``ported``
items. Measured on this corpus's ancestor: of 858 ``ported`` items, 444 were
records and only 211 were functions -- about a quarter of the ledger is code.
Ranking the all-kinds tally once invalidated an entire session's conclusions
(the #1 blocker at 110 all-kinds was #6 at 20 function-only). The headline
ranking here is therefore over ``kind == "function"`` items, deduped by
(repo, symbol, diagnostic) so a header-driven multiplicity counts once --
deduped WITHIN a repo, because that is where the multiplicity comes from,
and never across repos, where two libraries may legitimately both define
``init``.

The all-kinds figure is reported ALONGSIDE it, never instead of it: it is the
number the older ledgers used and dropping it would make the two eras
incomparable.

Ranking is on the per-item ``diagnostic`` field, NOT ``blocker``, which is
mostly ``"other"`` on C input and separates nothing.

OUTPUTS (all under ``--out``; nothing outside it is written, and nothing in
the repo or the corpus root is modified)
  ``results.json``  the full per-unit record, including every rustc error.
  ``results.tsv``   one line per unit.
  ``crates/``       the emitted crates, kept for diagnosis.
  plus a human summary on stdout.

EXIT CODE
  0  every emitted crate compiled (or no build was attempted anywhere).
  1  at least one emitted crate did not compile, or emitted nothing at all.
  2  the harness could not run (no corpus, no driver, no units).

Modeled on ``scripts/tractor-eval.py`` (same argument style, same
machine-readable/human split) and ``test/RealWorld/run_realworld.py``.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

EMIT_TIME_LIMIT = 600
BUILD_TIME_LIMIT = 600

BUILT = "BUILT"
BUILD_FAIL = "BUILD_FAIL"
BUILD_TIMED_OUT = "BUILD_TIMEOUT"
BUILD_SKIPPED = "BUILD_SKIPPED"
EMIT_EMPTY = "EMIT_EMPTY"
EMIT_FAIL = "EMIT_FAIL"
EMIT_TIMEOUT = "EMIT_TIMEOUT"

#: Worst-first, for the outcome table. BUILT is the only success.
OUTCOME_ORDER = [BUILD_FAIL, EMIT_EMPTY, BUILD_TIMED_OUT, EMIT_TIMEOUT,
                 EMIT_FAIL, BUILD_SKIPPED, BUILT]

#: Directory names pruned from unit enumeration. These hold a repo's own
#: tests, examples, fuzz drivers, build outputs and vendored dependencies --
#: not the library under study. Pruning is part of the denominator and is
#: printed with it.
SKIP_DIRS = frozenset({
    ".git", ".github", "build", "builds", "cmake-build-debug", "out",
    "target", "test", "tests", "testing", "unittest", "unittests",
    "example", "examples", "demo", "demos", "sample", "samples",
    "bench", "benchmark", "benchmarks", "fuzz", "fuzzing", "fuzzer",
    "ossfuzz", "oss-fuzz",
    "doc", "docs", "third_party", "3rdparty", "vendor", "deps",
    "node_modules", "contrib",
})

#: ClangTool prints one of these per input before any diagnostic reaches
#: stderr, so a naive "first line" of a multi-input run is progress chatter.
_TOOL_PROGRESS_RX = re.compile(r"^\[\d+/\d+\] Processing file ")

#: Quoted identifiers inside a diagnostic ("the owning function 'sdsfromlonglong'
#: was rejected") make every instance of one construct look like a different
#: blocker. Ranking collapses them; the verbatim text is kept in the JSON and
#: one example is printed with each rank.
_QUOTED_RX = re.compile(r"'[^']*'")


@dataclass
class Unit:
    """One solo translation unit to measure."""

    repo: str
    rel: str            # path relative to the corpus root
    path: Path
    include_dirs: list
    #: Filesystem-safe name of this unit's crate directory. Assigned by
    #: ``discover``, which guarantees it is unique across the corpus --
    #: mangling ``/`` to ``_`` is not injective (``a/b.c`` and ``a_b.c``
    #: collide), and two units sharing a crate directory would have one
    #: overwrite the other's measurement.
    ident: str = ""


@dataclass
class Result:
    """One measured unit. ``outcome`` is the only verdict; nothing infers it."""

    unit: Unit
    outcome: str = EMIT_FAIL
    detail: str = ""
    emit_rc: Optional[int] = None
    emit_cmd: list = field(default_factory=list)
    emit_diagnostics: str = ""
    crate_dir: str = ""
    build_rc: Optional[int] = None
    rustc_errors: dict = field(default_factory=dict)   # code/lint -> count
    build_error_lines: list = field(default_factory=list)
    # emitrust-progress.json, when one was written and parsed.
    progress: Optional[dict] = None

    @property
    def emitted(self) -> bool:
        """True iff a crate with real Rust in it exists on disk."""
        return self.outcome not in (EMIT_FAIL, EMIT_TIMEOUT, EMIT_EMPTY)

    def as_dict(self) -> dict:
        p = self.progress or {}
        totals = p.get("totals", {})
        return {
            "repo": self.unit.repo,
            "unit": self.unit.rel,
            "outcome": self.outcome,
            "detail": self.detail,
            "emit_rc": self.emit_rc,
            "emit_cmd": self.emit_cmd,
            "emit_diagnostics": self.emit_diagnostics,
            "crate_dir": self.crate_dir,
            "build_rc": self.build_rc,
            "rustc_errors": self.rustc_errors,
            "build_error_lines": self.build_error_lines,
            "progress": {
                "denominator_source": p.get("denominator_source", ""),
                "all_kinds_ported": totals.get("ported", 0),
                "all_kinds_items": totals.get("graph_items", 0),
                "function_ported": self.function_ported,
                "function_items": self.function_items,
            } if self.progress else None,
        }

    # -- FR-44 per-item scoring, function-filtered (see module docstring) --

    @property
    def progress_items(self) -> list:
        return (self.progress or {}).get("items", [])

    @property
    def function_items(self) -> int:
        """Function items with something to port (``declared`` has nothing)."""
        return sum(1 for i in self.progress_items
                   if i.get("kind") == "function"
                   and i.get("status") != "declared")

    @property
    def function_ported(self) -> int:
        return sum(1 for i in self.progress_items
                   if i.get("kind") == "function"
                   and i.get("status") == "ported")


def normalize_diagnostic(text: str) -> str:
    """The ranking key for a diagnostic: quoted identifiers collapsed."""
    return _QUOTED_RX.sub("'*'", (text or "").strip())


def emit_diagnostics(stderr: str, limit: int = 12) -> str:
    """A unit's diagnostics, without ClangTool's per-file progress chatter.

    Errors first and in emitted order -- the FIRST located rejection explains
    the failure; a trailing "failed to parse one or more C inputs" is only its
    summary.
    """
    lines = [l.strip() for l in (stderr or "").splitlines()
             if l.strip() and not _TOOL_PROGRESS_RX.match(l.strip())]
    errors = [l for l in lines if re.search(r"\b(error|fatal error):", l)]
    if errors:
        return "\n".join(errors[:limit])
    return "\n".join(lines[-limit:])


# ---------------------------------------------------------------- discovery


def include_dirs_for(repo_root: Path, unit: Path) -> list:
    """Include directories for one unit, CONFINED to its own repo.

    Confinement is not a nicety: ``<unit dir>/..`` for a unit sitting at the
    repo root is the CORPUS root, and adding it would let a sibling repo's
    headers satisfy this repo's ``#include`` -- silently changing what is
    being measured, and differently depending on which repos happen to be
    cloned.
    """
    repo_root = repo_root.resolve()
    d = unit.parent
    candidates = [d, d / "include", d.parent, d.parent / "include",
                  repo_root, repo_root / "include", repo_root / "src",
                  repo_root / "inc", repo_root / "lib"]
    out, seen = [], set()
    for c in candidates:
        try:
            resolved = c.resolve()
        except OSError:
            continue
        if not resolved.is_dir() or str(resolved) in seen:
            continue
        # Inside the repo, or the repo itself.
        if resolved != repo_root and repo_root not in resolved.parents:
            continue
        seen.add(str(resolved))
        out.append(str(resolved))
    return out


def git_head(repo_root: Path) -> str:
    """The repo's pinned revision, or "" when it is not a git checkout.

    Recorded so a later run's numbers are attributable to a population rather
    than to "whatever was on disk that day" (the FR-144 epoch lesson).
    """
    try:
        proc = subprocess.run(["git", "-C", str(repo_root), "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return proc.stdout.strip() if proc.returncode == 0 else ""


def discover(corpus: Path) -> tuple:
    """Return (units, excluded), where ``excluded`` is [(repo, reason)].

    A repo with no unit after pruning is EXCLUDED WITH A REASON, never
    silently skipped -- an unexplained absence is indistinguishable from a
    harness bug.
    """
    units, excluded = [], []
    if not corpus.is_dir():
        sys.exit(f"error: {corpus} is not a directory")
    for repo_root in sorted(p for p in corpus.iterdir() if p.is_dir()):
        if repo_root.name in SKIP_DIRS:
            excluded.append((repo_root.name,
                             "directory name is in the prune list"))
            continue
        found = []
        for dirpath, dirnames, filenames in os.walk(repo_root):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
            for name in sorted(filenames):
                if name.endswith(".c"):
                    found.append(Path(dirpath) / name)
        if not found:
            excluded.append((repo_root.name,
                             "no .c file outside the pruned directories "
                             "(header-only, or C++/other language)"))
            continue
        for path in sorted(found):
            units.append(Unit(repo=repo_root.name,
                              rel=str(path.relative_to(corpus)),
                              path=path,
                              include_dirs=include_dirs_for(repo_root, path)))
    # Crate-directory names, made unique. The mangling below is not injective,
    # so a collision is disambiguated by position in the (already sorted) unit
    # list -- deterministic, and it never silently merges two units.
    taken = set()
    for i, unit in enumerate(units):
        base = re.sub(r"[^A-Za-z0-9_.-]", "_", unit.rel)
        ident = base if base not in taken else f"{base}__{i}"
        taken.add(ident)
        unit.ident = ident
    return units, excluded


# ------------------------------------------------------------- measurement


def measure(unit: Unit, cc: Path, crates_root: Path, cargo: Optional[str],
            skip_reason: Optional[str]) -> Result:
    """Emit one crate, then build it. The two verdicts stay separate.

    ``skip_reason`` is None when the build oracle runs; otherwise it is the
    reason it did not, recorded on every unit so a report can never be read
    as "these crates compiled" when nothing tried to compile them.
    """
    res = Result(unit=unit)
    crate = crates_root / unit.ident
    shutil.rmtree(crate, ignore_errors=True)
    cmd = [str(cc), "--emit=crate", "--crate-type=lib", "--incremental",
           str(unit.path), "-o", str(crate)]
    cmd += ["-I" + d for d in unit.include_dirs]
    res.emit_cmd = cmd
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=EMIT_TIME_LIMIT)
    except subprocess.TimeoutExpired:
        res.outcome = EMIT_TIMEOUT
        res.detail = f"emit exceeded {EMIT_TIME_LIMIT}s"
        return res
    except OSError as exc:
        res.outcome = EMIT_FAIL
        res.detail = f"could not run the driver: {exc!r}"
        return res
    res.emit_rc = proc.returncode
    res.emit_diagnostics = emit_diagnostics(proc.stderr)
    if proc.returncode != 0:
        res.outcome = EMIT_FAIL
        res.detail = (res.emit_diagnostics.splitlines() or [""])[0][:200]
        return res

    # GUARD: exit 0 is not evidence a crate exists. emitrust-cc exits 1
    # having done nothing on an unknown flag, and a recovering import can in
    # principle drop everything; either way an empty crate must never be
    # counted as a success.
    if not (crate / "src").is_dir() or not list((crate / "src").glob("*.rs")):
        res.outcome = EMIT_EMPTY
        res.detail = "the driver exited 0 but wrote no src/*.rs"
        return res
    res.crate_dir = str(crate)

    report = crate / "emitrust-progress.json"
    if report.is_file():
        try:
            res.progress = json.loads(report.read_text(encoding="utf-8"))
        except ValueError:
            res.progress = None

    if skip_reason is not None:
        res.outcome = BUILD_SKIPPED
        res.detail = skip_reason
        return res

    # `skip_reason` is set on BOTH paths that leave `cargo` None (--no-build and
    # cargo-not-on-PATH), so the guard above already returned; bind it to a
    # non-Optional name so that correlation is checkable rather than implied.
    cargo_exe = cargo
    assert cargo_exe is not None, "skip_reason must cover every None-cargo path"
    try:
        proc = subprocess.run(
            [cargo_exe, "build", "--release", "--offline",
             "--message-format=json"],
            cwd=str(crate), capture_output=True, text=True,
            timeout=BUILD_TIME_LIMIT)
    except subprocess.TimeoutExpired:
        res.outcome = BUILD_TIMED_OUT
        res.detail = f"cargo exceeded {BUILD_TIME_LIMIT}s and reached no verdict"
        return res
    except OSError as exc:
        res.outcome = BUILD_SKIPPED
        res.detail = f"could not run cargo: {exc!r}"
        return res
    res.build_rc = proc.returncode
    res.rustc_errors, res.build_error_lines = parse_cargo_json(proc.stdout)
    if proc.returncode == 0:
        res.outcome = BUILT
        return res
    res.outcome = BUILD_FAIL
    if res.rustc_errors:
        res.detail = ", ".join(
            f"{code} x{n}" for code, n in
            sorted(res.rustc_errors.items(), key=lambda kv: (-kv[1], kv[0])))
    else:
        # cargo failed with no compiler-message: a manifest error, a missing
        # dependency under --offline, a toolchain problem. Not the same fact
        # as "rustc rejected the emitted Rust", so say what happened.
        res.detail = "cargo failed with no rustc diagnostic: " + \
            "; ".join((proc.stderr or "").split("\n")[:3])[:300]
    return res


def parse_cargo_json(stdout: str) -> tuple:
    """(error code/lint -> count, verbatim error lines) from cargo's JSON.

    ``cargo --message-format=json`` puts machine-readable diagnostics on
    stdout and human chatter on stderr. A DENIED LINT arrives as an error
    whose ``code.code`` is the lint name (``non_snake_case``), and a hard
    error's is the rustc code (``E0425``) -- which is exactly the
    classification FR-140/141/142 needed and nobody had.
    """
    counts, lines = {}, []
    for line in (stdout or "").splitlines():
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if msg.get("reason") != "compiler-message":
            continue
        message = msg.get("message") or {}
        if message.get("level") != "error":
            continue
        code = ((message.get("code") or {}).get("code")
                or "uncoded: " + (message.get("message") or "")[:60])
        counts[code] = counts.get(code, 0) + 1
        if len(lines) < 20:
            lines.append(f"{code}: {(message.get('message') or '').strip()}")
    return counts, lines


# ---------------------------------------------------------------- ranking


def rank_diagnostics(results: list, functions_only: bool, dedupe: bool) -> list:
    """Ranked (count, normalized diagnostic, example verbatim) triples.

    The unported items are the demand signal: each is a place the importer
    gave up. ``functions_only`` restricts to ``kind == "function"`` (see the
    module docstring -- roughly three quarters of the ledger is type items
    replicated once per translation unit that includes the header), and
    ``dedupe`` collapses (repo, symbol, diagnostic) so that replication
    counts once.
    """
    counts, examples, seen = {}, {}, set()
    for res in results:
        for item in res.progress_items:
            if functions_only and item.get("kind") != "function":
                continue
            if item.get("status") in ("ported", "declared"):
                continue
            diagnostic = (item.get("diagnostic") or "").strip()
            if not diagnostic:
                continue
            key = normalize_diagnostic(diagnostic)
            if dedupe:
                dkey = (res.unit.repo, item.get("symbol", ""), key)
                if dkey in seen:
                    continue
                seen.add(dkey)
            counts[key] = counts.get(key, 0) + 1
            examples.setdefault(key, diagnostic)
    return [(n, key, examples[key])
            for key, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))]


# ---------------------------------------------------------------- reporting


def write_reports(out_dir: Path, results: list, excluded: list,
                  meta: dict) -> None:
    payload = {
        "corpus_root": meta["corpus"],
        "emitrust_cc": meta["cc"],
        "emitrust_cc_mtime": meta["cc_mtime"],
        "cargo": meta["cargo"] or "",
        "denominator": len(results),
        "denominator_note": (
            "one unit per .c file under an immediate subdirectory of the "
            "corpus root, excluding the directories named in SKIP_DIRS; "
            "every rate below is over this count"
        ),
        "repos": meta["repos"],
        "excluded_repos": [{"repo": r, "reason": why} for r, why in excluded],
        "outcome_counts": {o: sum(1 for r in results if r.outcome == o)
                           for o in OUTCOME_ORDER},
        "build_failure_count": sum(1 for r in results
                                   if r.outcome == BUILD_FAIL),
        "emit_failure_count": sum(1 for r in results
                                  if r.outcome in (EMIT_FAIL, EMIT_TIMEOUT,
                                                   EMIT_EMPTY)),
        "ranked_function_diagnostics": [
            {"count": n, "diagnostic": key, "example": ex}
            for n, key, ex in rank_diagnostics(results, True, True)],
        "ranked_all_kind_diagnostics": [
            {"count": n, "diagnostic": key, "example": ex}
            for n, key, ex in rank_diagnostics(results, False, False)],
        "units": [r.as_dict() for r in results],
    }
    (out_dir / "results.json").write_text(json.dumps(payload, indent=2) + "\n",
                                          encoding="utf-8")
    with (out_dir / "results.tsv").open("w", encoding="utf-8") as fh:
        fh.write("repo\tunit\toutcome\tfn_ported\tfn_items\tall_ported"
                 "\tall_items\tdetail\n")
        for r in results:
            totals = (r.progress or {}).get("totals", {})
            fh.write("\t".join([
                r.unit.repo, r.unit.rel, r.outcome,
                str(r.function_ported), str(r.function_items),
                str(totals.get("ported", 0)), str(totals.get("graph_items", 0)),
                r.detail.replace("\t", " ").replace("\n", " ")[:300],
            ]) + "\n")


def summarize(results: list, excluded: list, meta: dict, out_dir: Path,
              elapsed: float) -> None:
    n = len(results)
    emitted = [r for r in results if r.emitted]
    built = [r for r in results if r.outcome == BUILT]
    build_failed = [r for r in results if r.outcome == BUILD_FAIL]
    emit_failed = [r for r in results
                   if r.outcome in (EMIT_FAIL, EMIT_TIMEOUT, EMIT_EMPTY)]

    print()
    print("=" * 72)
    print("external-repo probe sweep -- import demand signal + build oracle")
    print("=" * 72)
    print(f"corpus:      {meta['corpus']}")
    print(f"driver:      {meta['cc']}  (mtime {meta['cc_mtime']})")
    print(f"cargo:       {meta['cargo'] or 'NOT AVAILABLE -- no crate was built'}")
    print(f"wall time:   {elapsed:.1f}s")
    print()
    print(f"DENOMINATOR: {n} units over {len(meta['repos'])} repos")
    print("  a unit is one .c file under an immediate subdirectory of the "
          "corpus root,")
    print("  excluding test/example/fuzz/build/vendor directories "
          "(SKIP_DIRS).")
    for repo in meta["repos"]:
        mine = [r for r in results if r.unit.repo == repo["repo"]]
        print(f"    {repo['repo']:<16} {len(mine):>4} units   "
              f"{repo['head'][:12] or '(not a git checkout)'}")
    if excluded:
        print(f"  EXCLUDED, with reason ({len(excluded)}):")
        for name, why in excluded:
            print(f"    {name:<16} {why}")
    print()
    print("outcomes (BUILT is the only success):")
    for outcome in OUTCOME_ORDER:
        k = sum(1 for r in results if r.outcome == outcome)
        if k:
            print(f"  {outcome:<16} {k:>4}")
    print()
    print(f"EMITTED A CRATE:   {len(emitted)}/{n}   "
          f"(emit failures: {len(emit_failed)})")
    if emitted:
        print(f"CRATE COMPILES:    {len(built)}/{len(emitted)} of the crates "
              f"that were emitted  ({len(built)}/{n} of all units)")
    print()
    print("!" * 72)
    print(f"!! BUILD FAILURES: {len(build_failed)}"
          + ("  -- the emitted Rust was rejected by rustc"
             if build_failed else "  (every emitted crate compiled)"))
    print("!" * 72)
    if build_failed:
        tally = {}
        for r in build_failed:
            for code, k in r.rustc_errors.items():
                tally[code] = tally.get(code, 0) + k
        print()
        print("  ranked rustc error codes / denied lints:")
        for code, k in sorted(tally.items(), key=lambda kv: (-kv[1], kv[0])):
            crates = sum(1 for r in build_failed if code in r.rustc_errors)
            print(f"    {k:>5}  {code:<28} in {crates} crate(s)")
        print()
        for r in build_failed:
            print(f"  {r.unit.rel}")
            print(f"    crate: {r.crate_dir}")
            print(f"    {r.detail}")
            for line in r.build_error_lines[:6]:
                print(f"      {line}")
    print()

    fn_ported = sum(r.function_ported for r in results)
    fn_items = sum(r.function_items for r in results)
    all_ported = sum((r.progress or {}).get("totals", {}).get("ported", 0)
                     for r in results)
    all_items = sum((r.progress or {}).get("totals", {}).get("graph_items", 0)
                    for r in results)
    ledger_only = [r for r in results
                   if (r.progress or {}).get("denominator_source") == "ledger-only"]
    print("IMPORT SCORE over the crates that emitted "
          f"({len([r for r in results if r.progress])} progress reports)")
    print(f"  FUNCTION items: {fn_ported}/{fn_items}"
          + (f" ({100.0 * fn_ported / fn_items:.1f}%)" if fn_items else ""))
    print(f"  all kinds:      {all_ported}/{all_items}"
          + (f" ({100.0 * all_ported / all_items:.1f}%)" if all_items else "")
          + "   <- inflated by type items replicated per TU; kept for "
            "comparability only")
    if ledger_only:
        print(f"  NOTE {len(ledger_only)} report(s) are 'ledger-only' (no item "
              f"graph): they contribute 0/0, NOT 0%.")
    off_graph = sum((r.progress or {}).get("totals", {}).get(
        "off_graph_rejected", 0) for r in results)
    if off_graph:
        print(f"  NOTE {off_graph} rejected item(s) are OFF-GRAPH (the FR-40 "
              f"graph does not model them, so they carry no 'kind'). They are "
              f"in neither figure above nor in the ranking below; stated, not "
              f"folded in.")
    print()
    print("TOP BLOCKING DIAGNOSTICS -- FUNCTION items only, deduped by "
          "(repo, symbol, diagnostic):")
    for i, (k, key, ex) in enumerate(rank_diagnostics(results, True, True)[:20], 1):
        print(f"  {i:>2}. {k:>5}  {key[:110]}")
    print()
    print("  the same ranking over ALL KINDS, undeduped (the older, inflated "
          "figure):")
    for i, (k, key, _) in enumerate(rank_diagnostics(results, False, False)[:10], 1):
        print(f"  {i:>2}. {k:>5}  {key[:110]}")

    if emit_failed:
        print()
        print(f"EMIT FAILURES ({len(emit_failed)}) -- ranked first diagnostic:")
        tally = {}
        for r in emit_failed:
            # Rank the MESSAGE, not the located line: the file:line:col
            # prefix differs per unit, so an unstripped key would report one
            # row per unit and rank nothing. Strip first, truncate after --
            # truncating first splits one reason across several rows purely
            # by how long its path happened to be.
            first = (r.emit_diagnostics.splitlines() or [r.detail])[0]
            key = normalize_diagnostic(
                re.sub(r"^.*?\b(error|fatal error): ", "", first))[:110]
            tally[key] = tally.get(key, 0) + 1
        for key, k in sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))[:15]:
            print(f"  {k:>4}  {key}")
    print()
    print(f"reports: {out_dir}/results.json, {out_dir}/results.tsv")
    print(f"crates:  {out_dir}/crates")


# --------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(__doc__ or "external probe sweep").splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus", type=Path,
                    help="root of a directory of SEPARATELY CLONED C repos; "
                         "nothing is cloned or modified here")
    ap.add_argument("--emitrust-cc", type=Path,
                    default=Path("build/tools/emitrust-cc"),
                    help="path to the emitrust-cc driver")
    ap.add_argument("--out", type=Path, default=None,
                    help="report directory (default <corpus>/../external-probe-out); "
                         "the ONLY thing this script writes to")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count(),
                    help="parallel emit+build jobs")
    ap.add_argument("-m", "--match", default=None,
                    help="only measure units whose corpus-relative path "
                         "matches this regex")
    ap.add_argument("--cargo", default="cargo", help="path to cargo")
    ap.add_argument("--no-build", action="store_true",
                    help="emit only, skipping the build oracle; every emitted "
                         "crate is then BUILD_SKIPPED, never a success")
    args = ap.parse_args()

    corpus = args.corpus.resolve()
    cc = args.emitrust_cc.resolve()
    if not cc.is_file() or not os.access(cc, os.X_OK):
        print(f"error: emitrust-cc not found or not executable at {cc}",
              file=sys.stderr)
        return 2
    out_dir = (args.out or (corpus.parent / "external-probe-out")).resolve()
    if corpus == out_dir or corpus in out_dir.parents:
        print(f"error: --out {out_dir} is inside the corpus root; this script "
              f"must not write into the corpus", file=sys.stderr)
        return 2
    out_dir.mkdir(parents=True, exist_ok=True)
    crates_root = out_dir / "crates"
    # Re-runnable and deterministic: a crate left by an earlier run over a
    # DIFFERENT unit selection would otherwise be measured again.
    shutil.rmtree(crates_root, ignore_errors=True)
    crates_root.mkdir(parents=True)

    cargo = None if args.no_build else shutil.which(args.cargo)
    skip_reason = None
    if args.no_build:
        skip_reason = "--no-build was given; no build was attempted"
    elif cargo is None:
        skip_reason = f"cargo ({args.cargo!r}) is not on PATH"
        print(f"warning: cargo ({args.cargo!r}) is not on PATH -- NO CRATE "
              f"WILL BE BUILT and the build oracle, which is the point of "
              f"this sweep, is not exercised. Every emitted crate is reported "
              f"BUILD_SKIPPED, never as a success.", file=sys.stderr)

    units, excluded = discover(corpus)
    if args.match:
        rx = re.compile(args.match)
        units = [u for u in units if rx.search(u.rel)]
    if not units:
        print("error: no units discovered", file=sys.stderr)
        return 2

    repo_names = sorted({u.repo for u in units})
    repos = [{"repo": name,
              "head": git_head(corpus / name),
              "units": sum(1 for u in units if u.repo == name)}
             for name in repo_names]
    print(f"{len(units)} units over {len(repos)} repos from {corpus}")
    if excluded:
        print(f"{len(excluded)} directory(ies) excluded with a reason "
              f"(see the summary)")

    start = time.time()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(
            lambda u: measure(u, cc, crates_root, cargo, skip_reason), units))
    elapsed = time.time() - start

    meta = {
        "corpus": str(corpus),
        "cc": str(cc),
        "cc_mtime": time.strftime("%Y-%m-%dT%H:%M:%S",
                                  time.localtime(cc.stat().st_mtime)),
        "cargo": cargo,
        "repos": repos,
    }
    write_reports(out_dir, results, excluded, meta)
    summarize(results, excluded, meta, out_dir, elapsed)

    bad = sum(1 for r in results
              if r.outcome in (BUILD_FAIL, EMIT_EMPTY))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
