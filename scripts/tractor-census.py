#!/usr/bin/env python3
"""Per-case FULL blocker sets for the TRACTOR corpus, then set-cover.

WHY THIS EXISTS. `tractor-eval.py` scores in STRICT mode, which is correct --
the rubric awards no partial credit. But strict mode stops at the FIRST
blocker, so its `detail` field is a first-failure histogram, and ranking work
by it has produced a wrong yield SEVEN times in this ledger (FR-61f x2,
FR-165, FR-138, FR-177, FR-178, FR-221). FR-177 tried per-case blocker SETS
and still got it wrong, because it measured only the EMIT stage of a
three-stage rubric.

This tool measures the thing those attempts wanted:

  * every case's COMPLETE emit blocker set, via `--recover --incremental`
    (which implies --recover and writes emitrust-progress.json), so a case
    with five blockers is counted as needing all five;
  * the STRICT outcome alongside it, so emit-clearable cases can be separated
    from ones that would still die at cargo build or dlsym;
  * a SET-COVER ranking: which combination of N fixes clears the most cases.

The last part is the point. A blocker appearing in 80 cases is worth ZERO if
every one of those cases also needs four other fixes. Conversely a blocker in
6 cases is worth 6 if it is the ONLY thing those 6 need. Marginal yield is a
property of the SET, not of the histogram, and no histogram can show it.

Usage:
    nix develop -c python3 scripts/tractor-census.py <corpus> \
        --emitrust-cc build/tools/emitrust-cc --out <dir> [-j N]

Reuses tractor-eval's own discovery so the population is identical by
construction rather than by agreement.
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import itertools
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load_eval():
    """Import tractor-eval.py despite the hyphen in its name."""
    spec = importlib.util.spec_from_file_location(
        "tractor_eval", HERE / "tractor-eval.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TE = _load_eval()

RECOVER_TIMEOUT = getattr(TE, "EMIT_TIMEOUT", 300)


def recover_blockers(case, cc: Path, out_dir: Path, extra_cflags: str):
    """The COMPLETE emit blocker set for one case.

    Runs the same command `tractor-eval.emit` builds, plus --incremental
    (which implies --recover), into a scratch crate dir so the scored crate
    is never touched. Returns (blockers, note).
    """
    crate = out_dir / "recover" / case.rel_name.replace("/", "__")
    shutil.rmtree(crate, ignore_errors=True)
    crate.parent.mkdir(parents=True, exist_ok=True)

    cmd = [str(cc), "--emit=crate", f"--crate-name={case.crate_name}",
           "--crate-type=" + ("lib" if case.is_library else "bin")]
    if case.is_library:
        cmd.append("--c-abi-exports")
    cmd += ["--recover", "--incremental", "-o", str(crate)]

    build, err = TE.cmake_configure(case, out_dir, extra_cflags)
    sources = None
    if build is not None:
        sources, err = TE.target_closure(build, case.crate_name)
    if sources is not None:
        db, err = TE.filtered_compdb(
            build, sources, case.crate_name,
            out_dir / "compdb" / (case.rel_name.replace("/", "__") + ".json"))
        if db is not None:
            cmd.append(f"--compdb={db}")
    if "--compdb" not in " ".join(cmd):
        for inc in case.glob_include_dirs:
            cmd += ["-I", inc]
        cmd += list(case.glob_sources)

    try:
        proc = subprocess.run(cmd, cwd=str(case.root), capture_output=True,
                              text=True, timeout=RECOVER_TIMEOUT)
    except subprocess.TimeoutExpired:
        return set(), "recover timed out"

    prog = crate / "emitrust-progress.json"
    if not prog.exists():
        # Recovery could not produce a crate at all: fall back to the
        # diagnostics, deduped by wording. Recorded as a distinct note so a
        # reader never mistakes it for a progress-derived set.
        got = set()
        for line in (proc.stderr or "").splitlines():
            if "unsupported:" in line:
                got.add(line.split("unsupported:", 1)[1].strip()[:90])
        return got, "no-progress-json"

    try:
        data = json.loads(prog.read_text())
    except (OSError, json.JSONDecodeError):
        return set(), "progress unreadable"

    blockers = set()
    items = data.get("items") or data.get("entries") or []
    for it in items:
        if not isinstance(it, dict):
            continue
        if (it.get("status") or "") in ("ported", "ok", "clean"):
            continue
        b = it.get("blocker") or it.get("reason") or it.get("detail") or ""
        if b:
            blockers.add(str(b).strip()[:90])
    return blockers, ""


def normalize(b: str) -> str:
    """Collapse a diagnostic to its CLASS.

    Deliberately conservative: strip a trailing quoted identifier and any
    digits, because `object 'p'` and `object 'q'` are one blocker class and
    counting them apart would re-inflate exactly the way a first-failure
    histogram does.
    """
    import re
    b = re.sub(r"'[^']*'", "'X'", b)
    b = re.sub(r"\b\d+\b", "N", b)
    return b.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--emitrust-cc", default="build/tools/emitrust-cc")
    ap.add_argument("--out", required=True)
    ap.add_argument("-j", "--jobs", type=int, default=6)
    ap.add_argument("--extra-cflags", default="")
    ap.add_argument("--results", help="tractor-eval results.json to join against")
    ap.add_argument("--max-combo", type=int, default=3,
                    help="largest fix-combination to score in the set cover")
    args = ap.parse_args()

    corpus = Path(args.corpus).resolve()
    cc = Path(args.emitrust_cc).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    cases, _ = TE.discover(corpus)
    print(f"cases discovered: {len(cases)}", flush=True)

    # --- stage 1: the scored outcome, taken from the RUBRIC's own runner --
    # Deliberately not reimplemented. `tractor-eval.py` owns the three-stage
    # scoring; duplicating it here would create a second definition of PASS
    # that could drift from the one that counts.
    res_path = Path(args.results) if args.results else None
    if res_path is None or not res_path.exists():
        print("ERROR: pass --results <tractor-eval results.json>. Run the "
              "scorer first; this tool joins against it rather than "
              "redefining PASS.", file=sys.stderr)
        return 2
    scored_doc = json.loads(res_path.read_text())
    rows = scored_doc["cases"] if isinstance(scored_doc, dict) and "cases" in scored_doc else scored_doc
    rows = list(rows.values()) if isinstance(rows, dict) else rows
    scored = {r.get("case"): (r.get("outcome"), r.get("detail") or "")
              for r in rows}
    passing = {k for k, (o, _) in scored.items() if o == TE.PASS}
    print(f"  scored: PASS {len(passing)} / {len(scored)}", flush=True)

    # --- stage 2: the COMPLETE blocker set, recover mode -------------------
    print("collecting full blocker sets (--recover --incremental) ...",
          flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        recs = list(ex.map(
            lambda c: (c.rel_name,
                       *recover_blockers(c, cc, out, args.extra_cflags)),
            cases))

    sets = {}
    notes = {}
    for name, blockers, note in recs:
        sets[name] = {normalize(b) for b in blockers}
        if note:
            notes[name] = note

    # --- stage 3: the analysis that the histograms could not give ----------
    todo = {k: v for k, v in sets.items() if k not in passing}
    unmatched = [k for k in sets if k not in scored]
    if unmatched:
        print(f"WARNING: {len(unmatched)} census cases have no scored row "
              f"(key mismatch); e.g. {unmatched[:2]}", file=sys.stderr)
    print(f"\nnon-passing cases: {len(todo)}", flush=True)

    depth = collections.Counter(len(v) for v in todo.values())
    print("\nBLOCKER-SET DEPTH (how many distinct fixes a case needs):")
    for d in sorted(depth):
        print(f"  {depth[d]:4} cases need {d} fix(es)")

    freq = collections.Counter()
    for v in todo.values():
        freq.update(v)
    print("\nRAW FREQUENCY (the misleading number, shown for contrast):")
    for b, n in freq.most_common(12):
        print(f"  {n:4}  {b[:76]}")

    solo = collections.Counter()
    for v in todo.values():
        if len(v) == 1:
            solo.update(v)
    print("\nSOLE BLOCKER (fixing this ALONE clears the case) -- the real "
          "marginal yield:")
    if not solo:
        print("  (none: every non-passing case needs two or more fixes)")
    for b, n in solo.most_common(12):
        print(f"  +{n:3}  {b[:76]}")

    # set cover over the top candidates
    cands = [b for b, _ in freq.most_common(24)]
    print(f"\nSET COVER over the top {len(cands)} blockers "
          f"(combinations up to {args.max_combo}):")
    best_by_size = {}
    for k in range(1, args.max_combo + 1):
        best = (0, None)
        for combo in itertools.combinations(cands, k):
            s = set(combo)
            cleared = sum(1 for v in todo.values() if v and v <= s)
            if cleared > best[0]:
                best = (cleared, combo)
        best_by_size[k] = best
        if best[1]:
            print(f"  {k} fix(es) -> +{best[0]} cases")
            for b in best[1]:
                print(f"        {b[:72]}")
        else:
            print(f"  {k} fix(es) -> +0 cases")

    report = {
        "cases": len(cases),
        "pass": sorted(passing),
        "blocker_sets": {k: sorted(v) for k, v in sets.items()},
        "notes": notes,
        "depth": {str(k): v for k, v in depth.items()},
        "frequency": dict(freq.most_common()),
        "sole_blocker": dict(solo.most_common()),
        "set_cover": {str(k): {"cleared": v[0], "fixes": list(v[1] or [])}
                      for k, v in best_by_size.items()},
    }
    (out / "census.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nreport: {out / 'census.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
