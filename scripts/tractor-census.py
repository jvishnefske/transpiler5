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

AND EMIT IS ONE STAGE OF THREE. A `_lib` case scores by `dlopen` plus `dlsym`
of ONE named symbol, so clearing every emit blocker in it buys nothing unless
that symbol acquires a C ABI. FR-233 measured the top 3-fix set-cover at +34
EMIT of which all 34 were `lib`, four distinct symbols across eight build
configurations, and probed all four with `--c-abi-exports`: none exported. The
instrument was therefore ranking importer work by a number the rubric could
not pay. So every `lib` case now also carries an EXPORT VERDICT, read out of
the same `--recover --incremental --c-abi-exports` run: either the target
symbol acquires `#[no_mangle]`/`#[export_name]`, or `emitrust-cc` names the
shape that stopped it (FR-139/FR-182/FR-202/FR-209/FR-226 each have their own
sentence, and the sentences are kept apart because a refusal's WORDING is this
instrument's key), or the symbol never reaches the emitted module at all.

THE EXPORT VERDICT IS AN OBSERVATION, NOT A PREDICTION. It says what THIS
binary does to THIS signature today. It is not a claim that the wall is
permanent -- an export-class widening is itself a fix somebody can make -- and
it is not a claim that a stubbed body's signature is the signature the same
function would have had if its body had imported. Two of the export classes
(FR-202's bounded slice, FR-226's unaccessed pointer) are decided FROM THE
BODY, so recovery's stub can move the verdict in either direction. What the
verdict is good for is the negative: a `lib` case counted as "cleared" by an
importer-only fix while its own symbol is refused today is a case whose EMIT
movement demonstrably is not yield, and the set-cover below now says so
instead of hiding it in a total. Calibration on the 50 `lib` cases that emit
in STRICT mode: the verdict names the target symbol for exactly the 13
SYMBOL_MISSING cases and for none of the 36 PASS ones.

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
    is never touched. Returns (blockers, note, rejected_items, export).

    `export` is the EXPORT-STAGE observation for a `_lib` case, read from the
    very same run -- `tractor-eval.emit` already passes `--c-abi-exports` for
    a library, so this costs no extra invocation and cannot disagree with the
    scorer about the flags. `None` for an `exec` case, which has no dlsym gate.
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
        return set(), "recover timed out", 0, (
            {"verdict": "unknown", "blocker": "", "why": "recover timed out"}
            if case.is_library else None)

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
        return (got, (note or "no-progress-json"), dropped,
                export_verdict(case, crate, proc.stderr or ""))

    try:
        data = json.loads(prog.read_text())
    except (OSError, json.JSONDecodeError):
        return (set(), "progress unreadable", dropped,
                export_verdict(case, crate, proc.stderr or ""))

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
    return blockers, note, dropped, export_verdict(case, crate,
                                                   proc.stderr or "")


_EXPORT_REFUSAL_RX = re.compile(
    r"no C-ABI export for '([^']+)': (.*?); it stays a plain 'pub fn' and is "
    r"not reachable by dlsym")

#: `#[export_name = "sym"]` -- FR-208's spelling, used whenever the emitted
#: item's name is NOT the C symbol (the FR-53 idiomatic rename), and by every
#: FR-182/FR-202/FR-226 delegating wrapper, whose item is
#: `__emitrust_cabi_<sym>`.
_EXPORT_NAME_RX = re.compile(r'#\[export_name\s*=\s*"([^"]+)"\]')

#: `#[no_mangle] pub extern "C" fn sym` -- the plain all-scalar export, where
#: the item's own name IS the symbol. Attributes between the two are tolerated.
_NO_MANGLE_RX = re.compile(
    r'#\[no_mangle\]\s*(?:#\[[^\]]*\]\s*)*'
    r'pub\s+(?:unsafe\s+)?extern\s+"C"\s+fn\s+([A-Za-z_]\w*)')


def _fold(name: str) -> str:
    """The identifier key that survives FR-53's idiomatic rename.

    MEASURED, NOT ASSUMED, and it is the reason this function exists. The
    SPHINCS+ harness dlsyms `SPX_prf_addr`; the importer renames the item to
    `spx_prf_addr`, and BOTH the refusal warning and `emitrust-progress.json`
    carry the RUST name -- only `#[export_name = "..."]`, which exists solely
    for symbols that DO export, carries the C spelling. So a refused symbol
    cannot be matched to its case by string equality, and a matcher that
    tried it reported 32 SPHINCS+ cases as `absent` (symbol not in the
    module) when the module in fact contains the function and emitrust-cc in
    fact printed its refusal.

    Case and underscores are exactly what the rename moves, so they are what
    this drops. It is a HEURISTIC and it is used only when the exact name
    misses AND the folded key is UNIQUE among the candidates -- an ambiguous
    fold is reported as ambiguous rather than guessed.
    """
    return name.lower().replace("_", "")


def _unique_fold_match(symbol: str, names) -> str:
    """`names`' member matching `symbol` after folding, if there is exactly one."""
    key = _fold(symbol)
    hits = [n for n in names if _fold(n) == key]
    return hits[0] if len(hits) == 1 else ""


def export_verdict(case, crate: Path, stderr: str) -> dict:
    """What the CURRENT binary does to this `_lib` case's dlsym'd symbol.

    Not a prediction and not a proof -- an OBSERVATION of one run, which is
    the only thing this file is allowed to claim (see the module docstring,
    and the deleted "CONFIRMED" column it warns about). Four ways to be
    unexported and they are kept apart because the remedies differ:

      * `refused`    -- emitrust-cc classified the signature and said, in that
        shape's own sentence, why it gets no C ABI. This is the export WALL:
        no importer fix moves it, only an export-class fix.
      * `unexported` -- the symbol IS in the emitted module and drew no
        refusal. FR-159 keeps a per-TU-module item out of the export path
        without a diagnostic, and a `static` C function is not public at all;
        either way there is nothing for the harness to dlsym.
      * `absent`     -- the symbol never reached the emitted module (its TU
        did not parse, or recovery dropped it outright).
      * `ambiguous`  -- the fold above matched more than one candidate. Not
        guessed; reported.

    EXPORTED IS DECIDED BY EXACT NAME AND NOTHING ELSE, because `dlsym` is.
    `#[export_name = "S"]` and `#[no_mangle] ... fn S` both put the literal
    `S` in the dynamic symbol table, so `S == case.symbol` or the harness
    does not find it. The fold is used ONLY to attribute a refusal or a
    presence, never to declare an export.

    WHICH SYMBOL. `case.symbol` -- tractor-eval's own answer, parsed from the
    case's cando2 harness (`parse_lib_naming`), never re-derived here. If the
    two files disagreed about the symbol the census would be wrong in a new
    way, so there is exactly one definition of it and this reads it.
    """
    if not case.is_library:
        return None
    symbol = case.symbol or ""
    refusals = {}
    for m in _EXPORT_REFUSAL_RX.finditer(stderr or ""):
        refusals.setdefault(m.group(1), m.group(2).strip())

    text = ""
    src = crate / "src"
    if src.is_dir():
        for rs in sorted(src.rglob("*.rs")):
            try:
                text += rs.read_text(encoding="utf-8", errors="replace")
            except OSError:
                pass
        # Keep the refusals next to the crate they describe, so this verdict
        # can be re-derived without a second six-minute corpus pass.
        try:
            (crate / "export-warnings.txt").write_text(
                "".join(f"{k}\t{v}\n" for k, v in sorted(refusals.items())),
                encoding="utf-8")
        except OSError:
            pass

    plain = set(_NO_MANGLE_RX.findall(text))
    wrapped = set(_EXPORT_NAME_RX.findall(text))
    exported_names = plain | wrapped
    emitted_fns = set(re.findall(r"\bfn\s+([A-Za-z_]\w*)", text))

    # HOW SOFT IS AN `exports` VERDICT? Two of the export classes are decided
    # FROM THE BODY -- FR-202 needs a must-access bound proven from it, FR-226
    # needs the body to never touch the pointer -- and a RECOVERY STUB has no
    # body, so it satisfies "never accesses" vacuously. An `exports` verdict on
    # a stubbed item that goes through a delegating wrapper is therefore the
    # one shape this tool can over-report, and it is counted rather than
    # smoothed over. `#[no_mangle]` on the function itself is FR-139's
    # all-scalar class, which reads the SIGNATURE only and is unaffected.
    status = ""
    prog = crate / "emitrust-progress.json"
    if prog.is_file():
        try:
            items = json.loads(prog.read_text()).get("items") or []
            hits = [i for i in items
                    if isinstance(i, dict) and _fold(i.get("symbol", ""))
                    == _fold(symbol)]
            if hits:
                status = str(hits[0].get("status") or "")
        except (OSError, json.JSONDecodeError):
            pass

    rec = {"symbol": symbol,
           "refused_symbols": len(refusals),
           "exported_symbols": len(exported_names),
           "item_status": status,
           "matched_as": "exact"}

    if symbol in exported_names:
        rec.update(verdict="exports", blocker="",
                   via="no_mangle" if symbol in plain else "wrapper",
                   body_derived_risk=bool(symbol in wrapped
                                          and status not in ("ported", "")))
        return rec
    if symbol in refusals:
        rec.update(verdict="refused", blocker=refusals[symbol])
        return rec
    if not text:
        rec.update(verdict="absent", blocker="", matched_as="none",
                   why="no crate was written by the recover run")
        return rec

    # The rename path. Try the refusals first -- a named refusal is strictly
    # more informative than a bare presence.
    hit = _unique_fold_match(symbol, refusals)
    if hit:
        rec.update(verdict="refused", blocker=refusals[hit],
                   matched_as=f"renamed to '{hit}'")
        return rec
    if [n for n in refusals if _fold(n) == _fold(symbol)]:
        rec.update(verdict="ambiguous", blocker="", matched_as="ambiguous",
                   why="more than one emitted item folds to this C symbol")
        return rec
    hit = _unique_fold_match(symbol, emitted_fns)
    if hit:
        rec.update(verdict="unexported", blocker="",
                   matched_as="exact" if hit == symbol else f"renamed to '{hit}'",
                   why="the item is emitted but takes no export path and drew "
                       "no refusal (FR-159 per-TU module, not public, or a "
                       "recovery stub the exporter skipped)")
        return rec
    rec.update(verdict="absent", blocker="", matched_as="none",
               why="the symbol is not in the emitted module")
    return rec


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
    exports = {}
    for name, blockers, note, ndropped, export in recs:
        sets[name] = {normalize(b) for b in blockers}
        if note:
            notes[name] = note
        if ndropped:
            dropped[name] = ndropped
        if export is not None:
            exports[name] = export

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

    # --- the EXPORT stage, for `lib` cases only ---------------------------
    # Printed BEFORE the set cover because it is what the set cover is now
    # allowed to say. Not a prediction: see the module docstring.
    walled = {k for k, e in exports.items()
              if e.get("verdict") != "exports"}
    print("\nEXPORT VERDICT for the {} `lib` cases (an OBSERVATION of what "
          "this\nbinary does to each case's dlsym'd symbol TODAY -- not a "
          "prediction, and\nnot a claim the wall is permanent; widening an "
          "export class is itself a fix):"
          .format(len(exports)))
    vd = collections.Counter(e.get("verdict") for e in exports.values())
    for v in sorted(vd, key=lambda x: -vd[x]):
        print(f"  {vd[v]:4}  {v}")
    soft = sum(1 for e in exports.values() if e.get("body_derived_risk"))
    stub = sum(1 for e in exports.values()
               if e.get("verdict") == "exports"
               and e.get("item_status") == "stubbed")
    print(f"\n  {stub} of the `exports` verdicts are for an item RECOVERY "
          f"STUBBED, and {soft} of\n  those reach the C ABI through a "
          f"delegating wrapper. Only the second number is\n  soft: FR-202's "
          f"bound and FR-226's unaccessed pointer are decided FROM THE BODY,\n"
          f"  and a stub has none, so it can satisfy them vacuously. "
          f"FR-139's plain\n  `#[no_mangle]` reads the signature only and is "
          f"unaffected by the stub.")
    renamed = sum(1 for e in exports.values()
                  if str(e.get("matched_as", "")).startswith("renamed"))
    print(f"\n  {renamed} of those verdicts were attributed through FR-53's "
          f"rename (the C symbol\n  `SPX_prf_addr` is the item `spx_prf_addr` "
          f"everywhere but `#[export_name]`),\n  by a UNIQUE case/underscore "
          f"fold. Ambiguous folds are reported, never guessed.")
    agree_ok = sum(1 for k, e in exports.items()
                   if k in passing and e.get("verdict") == "exports")
    agree_no = sum(1 for k, e in exports.items()
                   if scored.get(k, ("", ""))[0] == "SYMBOL_MISSING"
                   and e.get("verdict") != "exports")
    npass = sum(1 for k in exports if k in passing)
    nsym = sum(1 for k in exports
               if scored.get(k, ("", ""))[0] == "SYMBOL_MISSING")
    print(f"\n  CALIBRATION against the scored run, on the cases whose "
          f"outcome the\n  export stage already decided: {agree_ok}/{npass} "
          f"PASS cases read `exports`,\n  {agree_no}/{nsym} SYMBOL_MISSING "
          f"cases read otherwise. A disagreement here\n  means this parser "
          f"is wrong, not that the binary is.")

    exb = collections.Counter(
        normalize(e.get("blocker") or "") for e in exports.values()
        if e.get("verdict") == "refused")
    print("\nEXPORT REFUSAL WORDINGS -- kept APART on purpose. The census "
          "keys on the\ndiagnostic, and collapsing shape-specific refusals "
          "into one bucket is exactly\nwhat moves a real class into the "
          "`other` junk heap where nobody ranks it:")
    if not exb:
        print("  (none)")
    for b, n in exb.most_common(12):
        print(f"  {n:4}  {b[:100]}")
    print("\nNOT-EXPORTED `lib` cases that are ALSO non-passing at emit: "
          f"{len(walled & set(todo))}.\nAn importer fix that clears every "
          "emit blocker in one of those still scores\nZERO, because the "
          "rubric pays on dlsym.")

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
    #
    #   * THE EXPORT WALL, and this one is not an exclusion but a SPLIT.
    #     FR-233 read this table's "+34 EMIT (exec 0, lib 34)" as work worth
    #     doing; all 34 were `lib` cases whose own dlsym'd symbol is refused
    #     a C ABI by the binary that printed the number, so the honest EMIT
    #     figure was 34 and the honest YIELD figure was 0. Every row below
    #     therefore breaks its total into (exec / lib-exports / EXPORT-WALLED),
    #     and a second table re-optimises with the walled cases removed from
    #     the objective. Both remain UPPER BOUNDS, and "walled" is a statement
    #     about today's binary -- an export-class widening would move it.
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
    def split(got):
        """(exec, lib-that-exports, lib-that-is-export-walled)."""
        nexec = sum(1 for c in got if kinds.get(c) == "exec")
        nok = sum(1 for c in got if kinds.get(c) != "exec"
                  and exports.get(c, {}).get("verdict") == "exports")
        return nexec, nok, len(got) - nexec - nok

    def cover(objective):
        """Best `k`-combination for each k, maximising `objective(case)`."""
        table = {}
        for k in range(1, args.max_combo + 1):
            best = (0, None)
            for combo in itertools.combinations(cands, k):
                cs = set(combo)
                score = sum(1 for c, v in solid.items()
                            if v <= cs and objective(c))
                if score > best[0]:
                    best = (score, combo)
            table[k] = best
        return table

    def emit_table(table, label):
        for k, (score, combo) in sorted(table.items()):
            if not combo:
                print(f"  {k} fix(es) -> +0 cases")
                continue
            got = [c for c, v in solid.items() if v <= set(combo)]
            nexec, nok, nwall = split(got)
            print(f"  {k} fix(es) -> +{len(got)} EMIT   "
                  f"(exec {nexec}, lib-exports {nok}, EXPORT-WALLED {nwall})"
                  + (f"   [{label} {score}]" if label else ""))
            for b in combo:
                print(f"        {b[:72]}")

    best_by_size = cover(lambda c: True)
    emit_table(best_by_size, "")

    # The same optimisation with the export-walled cases taken OUT of the
    # objective. This is the table to rank importer work by; the one above is
    # the table that told FR-233 it had found +34 when the rubric would have
    # paid 0.
    def not_walled(c):
        return (kinds.get(c) == "exec"
                or exports.get(c, {}).get("verdict") == "exports")
    yield_by_size = cover(not_walled)
    npayable = sum(1 for c in solid if not_walled(c))
    print(f"\nSET COVER, EXPORT-AWARE -- the same search re-optimised over "
          f"only the\n{npayable} of {len(solid)} actionable cases whose "
          f"symbol can reach dlsym as this\nbinary stands. STILL AN UPPER "
          f"BOUND (every blocker set is a lower bound),\nand still not a "
          f"prediction: the walled cases are not unreachable, they need an\n"
          f"EXPORT-class fix in addition to the importer one.")
    emit_table(yield_by_size, "payable")

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
        "export_verdicts": exports,
        "export_verdict_is_an_observation_not_a_prediction": True,
        "export_refusal_wordings": dict(exb.most_common()),
        "set_cover_export_aware": {
            str(k): {"payable": v[0], "fixes": list(v[1] or [])}
            for k, v in yield_by_size.items()},
    }
    (out / "census.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nreport: {out / 'census.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
