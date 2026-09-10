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

EVERY DEPTH THIS TOOL REPORTS IS A LOWER BOUND, and the instrument cannot be
made exact. `--recover` drops whole TOP-LEVEL ITEMS, so every blocker INSIDE a
rejected function is invisible: the importer bails at the first error in a
body, recovery discards the whole function, and nothing further in it is ever
attempted. Measured instance -- B01_synthetic/004_nineality_sieve reports
depth 1 (`argv`) and actually needs at least four (argv, the strtol family
with an `endptr` out-parameter, `fprintf` to stderr, and a pointer-identity
test of `endptr` against `argv[1]`), none of which can surface while `main` is
dropped. Cases with a non-zero `dropped_items` count are therefore ranked on
partial information and are flagged as such in the report.

TWO CLASSES OF CASE YIELD NO INFORMATION AT ALL, and they are distinguished in
`notes` because the remedies differ:
  * `dropped-main`  -- recovery dropped `c_main`, so a BIN crate cannot be
    emitted ("the input does not define a 'main' function") and no progress
    JSON is written. Retried automatically as `--crate-type=lib`, which does
    produce one; the depth is still a lower bound per the paragraph above.
  * `no-progress-json` -- the case never reached the importer, because clang
    failed to PARSE it (a missing header, an unset build-system macro). No
    importer-side remedy exists; the dependency has to be present first.

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
import re
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

    def run(argv):
        try:
            return subprocess.run(argv, cwd=str(case.root), capture_output=True,
                                  text=True, timeout=RECOVER_TIMEOUT)
        except subprocess.TimeoutExpired:
            return None

    proc = run(cmd)
    if proc is None:
        return set(), "recover timed out", 0

    prog = crate / "emitrust-progress.json"
    note = ""
    if not prog.exists() and "does not define a 'main' function" in (
            proc.stderr or ""):
        # THE DROPPED-MAIN BLIND SPOT. Recovery dropped `c_main`, so the bin
        # crate cannot be emitted and NOTHING is written -- not even the
        # progress JSON this tool reads. The census then scored the case
        # depth 0, i.e. reported no information for exactly the cases whose
        # single function failed. emitrust-cc's own diagnostic names the
        # remedy ("Drop --crate-type=bin to emit a library crate instead"),
        # so take it: a lib crate emits the surviving items and their
        # progress record. The depth stays a LOWER BOUND -- whatever `main`
        # needed beyond its first blocker is still unreachable -- but a lower
        # bound of 1 with a dropped item flagged beats a silent 0.
        shutil.rmtree(crate, ignore_errors=True)
        retry = [("--crate-type=lib" if a.startswith("--crate-type=") else a)
                 for a in cmd]
        retry = [a for a in retry if a != "--c-abi-exports"]
        proc = run(retry) or proc
        note = "dropped-main"

    # How many top-level items recovery REJECTED -- stubbed OR dropped.
    #
    # THIS COUNTED ONLY `dropped` UNTIL 2026-09-09 AND THAT WAS A REAL DEFECT
    # IN THIS TOOL. `RejectionLedger.cpp` prints
    # `item.stubbed ? "stubbed" : "dropped"`, and a STUBBED item hides its
    # interior exactly as a dropped one does -- the importer bails at the
    # first error in a body either way, so nothing further inside it is ever
    # attempted. Corpus-wide the counts are 138 stubbed against 12 dropped, so
    # the old regex saw almost nothing: 36 of 39 cases this tool called
    # "SOLE BLOCKER, CONFIRMED" contained a stubbed item, i.e. that column was
    # ~92% unverified. Read the per-item `status` out of the progress JSON
    # below instead of regexing stderr, which is why this returns 0 here and
    # is filled in from the JSON.
    dropped = 0

    if not prog.exists():
        # The case never reached the importer at all -- clang failed to parse
        # it (missing header, unset build-system macro). Fall back to the
        # diagnostics, deduped by wording, and record a DISTINCT note so a
        # reader never mistakes it for a progress-derived set nor for the
        # dropped-main class above, which has a different remedy.
        got = set()
        for line in (proc.stderr or "").splitlines():
            if "unsupported:" in line:
                got.add(line.split("unsupported:", 1)[1].strip()[:110])
        return got, (note or "no-progress-json"), dropped

    try:
        data = json.loads(prog.read_text())
    except (OSError, json.JSONDecodeError):
        return set(), "progress unreadable", dropped

    blockers = set()
    items = data.get("items") or data.get("entries") or []
    totals = data.get("totals") or {}
    dropped = int(totals.get("stubbed", 0)) + int(totals.get("dropped", 0))
    if not totals:
        dropped = sum(1 for it in items if isinstance(it, dict)
                      and (it.get("status") or "") in ("stubbed", "dropped"))
    for it in items:
        if not isinstance(it, dict):
            continue
        if (it.get("status") or "") in ("ported", "ok", "clean"):
            continue
        # PREFER THE FULL DIAGNOSTIC OVER THE COARSE TAG. Each item carries
        # both: `blocker` is a short bucket ("other", "libc:sqrtf",
        # "unreached-by-import") and `diagnostic` is the sentence the user
        # sees. Ranking on the tag collapsed 124 of 211 non-passing cases into
        # a single "other" bucket, which makes the set-cover below meaningless
        # for exactly the cases that matter most -- an instrument that reports
        # "fix `other` for +36" has told you nothing. `normalize` then folds
        # `object 'buffer'` and `object 'p'` into one class, so preferring the
        # sentence does not re-inflate the count the way a raw histogram would.
        b = (it.get("diagnostic") or it.get("blocker") or it.get("reason")
             or it.get("detail") or "")
        if b:
            blockers.add(str(b).strip()[:110])
    return blockers, note, dropped


def normalize(b: str) -> str:
    """Collapse a diagnostic to its CLASS.

    Deliberately conservative: strip a trailing quoted identifier and any
    digits, because `object 'p'` and `object 'q'` are one blocker class and
    counting them apart would re-inflate exactly the way a first-failure
    histogram does.
    """
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
    kinds = {r.get("case"): r.get("kind") for r in rows}
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
    dropped = {}
    for name, blockers, note, ndropped in recs:
        sets[name] = {normalize(b) for b in blockers}
        if note:
            notes[name] = note
        if ndropped:
            dropped[name] = ndropped

    # --- stage 3: the analysis that the histograms could not give ----------
    todo = {k: v for k, v in sets.items() if k not in passing}
    unmatched = [k for k in sets if k not in scored]
    if unmatched:
        print(f"WARNING: {len(unmatched)} census cases have no scored row "
              f"(key mismatch); e.g. {unmatched[:2]}", file=sys.stderr)
    print(f"\nnon-passing cases: {len(todo)}", flush=True)

    depth = collections.Counter(len(v) for v in todo.values())
    print("\nBLOCKER-SET DEPTH -- A LOWER BOUND, NOT A COUNT.")
    print("  (--recover drops whole top-level items, so blockers INSIDE a")
    print("   rejected function never surface; see the module docstring.)")
    for d in sorted(depth):
        print(f"  {depth[d]:4} cases need AT LEAST {d} fix(es)")
    partial = sum(1 for k in todo if dropped.get(k))
    blind = sum(1 for k in todo if notes.get(k) == "no-progress-json")
    dm = sum(1 for k in todo if notes.get(k) == "dropped-main")
    print(f"\n  {partial} of {len(todo)} non-passing cases had >=1 DROPPED item,")
    print("  so their depth is partial by construction and they are ranked")
    print("  on incomplete information.")
    print(f"  {dm} recovered only as a lib crate (main was dropped).")
    print(f"  {blind} never reached the importer at all (clang could not parse")
    print("  them); no importer-side fix applies until the dependency exists.")

    freq = collections.Counter()
    for v in todo.values():
        freq.update(v)
    print("\nRAW FREQUENCY (the misleading number, shown for contrast):")
    for b, n in freq.most_common(12):
        print(f"  {n:4}  {b[:76]}")

    # THE NUMBER PEOPLE RANK ON, and there is no honest way to make it a
    # lower bound as well as an upper one.
    #
    # THIS TOOL ONCE PRINTED A "SOLE BLOCKER, CONFIRMED" COLUMN AND IT WAS
    # WRONG. The idea was that a case with no REJECTED item had a complete
    # blocker set -- but every non-passing case has at least one rejected
    # item, that being what makes it non-passing, and the importer bails at
    # the FIRST error inside any item it rejects. So a case reporting one
    # blocker may need one fix or five, and nothing short of making the fix
    # distinguishes them. Every set below is a LOWER BOUND and every yield an
    # UPPER BOUND, always.
    #
    # MEASURED CALIBRATION, one data point: this tool predicted +14 EMIT for
    # the system-header fix; FR-224 implemented it and delivered +7. Roughly
    # HALF, because three of the fourteen had a second blocker behind the stub
    # and one was refused on policy. Discount accordingly until there are more
    # data points.
    solo = collections.Counter()
    for name, v in todo.items():
        if len(v) == 1:
            solo.update(v)
    print("\nSINGLE-BLOCKER CASES -- AN UPPER BOUND on what one fix clears at"
          "\nEMIT, never a promise. The interior of every rejected item is"
          "\nunimported, so a second blocker can hide behind any of these:")
    if not solo:
        print("  (none)")
    for b, n in solo.most_common(14):
        print(f"  <={n:3}  {b[:76]}")
    print("\nAND AT THE EMIT STAGE ONLY. A `lib` case that clears emit must "
          "still\nexport a dlsym-able symbol before it counts as PASS; check "
          "`kind` in the\nrubric's results.json before quoting any of these "
          "as a yield.")
    ri = collections.Counter(dropped.get(k, 0) for k in todo)
    print("\nREJECTED ITEMS PER CASE (how much code is hidden behind the "
          "stubs --\nmore items means a looser bound):")
    for k in sorted(ri):
        print(f"  {ri[k]:4} cases have {k} rejected item(s)")

    # SET COVER. Two exclusions, both of which changed the answer materially
    # when they were added, so neither is cosmetic:
    #
    #   * NON-ACTIONABLE MARKERS. `unreached-by-import` is not a blocker
    #     anybody can fix -- it means clang never parsed the case. Leaving it
    #     in the candidate pool let the optimiser "spend" a fix on it and
    #     report a 4-fix set of +34, which no amount of engineering could
    #     deliver.
    # The result is EMIT-only and an UPPER BOUND on both counts: a `lib` case
    # that clears emit must then export a dlsym-able symbol (hence the
    # exec/lib split), and every case's blocker set is a lower bound because
    # the interior of its rejected items was never imported. An earlier
    # version excluded "partial" cases here in the belief that the rest were
    # verified; there is no such set, so nothing is excluded on that basis
    # now. Measured calibration: predicted +14 EMIT for the system-header fix,
    # delivered +7.
    NON_ACTIONABLE = {"unreached-by-import", ""}
    solid = {k: v - NON_ACTIONABLE for k, v in todo.items()}
    solid = {k: v for k, v in solid.items() if v}
    cands = [b for b, _ in collections.Counter(
        b for v in solid.values() for b in v).most_common(20)]
    print(f"\nSET COVER over the top {len(cands)} ACTIONABLE blockers "
          f"(combinations up to {args.max_combo});\n{len(solid)} of "
          f"{len(todo)} non-passing cases have an actionable blocker.\n"
          f"EVERY FIGURE IS AN UPPER BOUND -- see the note above the "
          f"single-blocker table.")
    best_by_size = {}
    for k in range(1, args.max_combo + 1):
        best = (0, None)
        for combo in itertools.combinations(cands, k):
            s = set(combo)
            cleared = sum(1 for v in solid.values() if v <= s)
            if cleared > best[0]:
                best = (cleared, combo)
        best_by_size[k] = best
        if best[1]:
            got = [c for c, v in solid.items() if v <= set(best[1])]
            nexec = sum(1 for c in got if kinds.get(c) == "exec")
            print(f"  {k} fix(es) -> +{best[0]} EMIT   "
                  f"(exec {nexec}, lib {len(got) - nexec})")
            for b in best[1]:
                print(f"        {b[:72]}")
        else:
            print(f"  {k} fix(es) -> +0 cases")

    report = {
        "cases": len(cases),
        "pass": sorted(passing),
        "blocker_sets": {k: sorted(v) for k, v in sets.items()},
        "notes": notes,
        "dropped_items": dropped,
        "depth_is_lower_bound": True,
        "depth": {str(k): v for k, v in depth.items()},
        "frequency": dict(freq.most_common()),
        "single_blocker_upper_bound": dict(solo.most_common()),
        "rejected_items": dropped,
        "set_cover": {str(k): {"cleared": v[0], "fixes": list(v[1] or [])}
                      for k, v in best_by_size.items()},
    }
    (out / "census.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nreport: {out / 'census.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
