#!/usr/bin/env python3
"""Binding-hygiene evaluator + Pareto ratchet for the emitted Rust.

Clippy measures LINT cleanliness. It says almost nothing about the single
most obvious tell that a human did not write this code: chains of single-use
SSA temporaries.

    let v2: &'static str = "emit";
    let mut v3: [i8; 5] = [101, 109, 105, 116, 0];
    let v4: &mut [i8] = &mut v3[0i64 as usize..];
    let v5: u32 = tu0_fnv1a(v4);
    println!("{:<6} {:08x}", v2, v5);

Five statements at depth 1 where a human writes two at depth 2. This tool
measures exactly that, on the SAME epoch-pinned population as `clippy_eval.py`
and with the same flags, so that someone reading one recognises the other.

  binding_eval.py [corpus] [--baseline FILE] [--update] [--top N]
                  [--file-list FILE] [--json FILE] [--sites N]

TWO AXES, DELIBERATELY NEVER SUMMED.

  Axis A  FREE TEMPORARIES PER 100 STATEMENTS. A `let` counts only when it
          has the emitter's generated name (`v<N>`), has exactly one use in
          scope, that use is in the IMMEDIATELY following statement, and
          INLINING IT WOULD CROSS NO SIDE EFFECT.

  Axis B  EXPRESSION NESTING DEPTH, p95 and max per statement. The
          anti-cramming guard: it stops anyone "improving" axis A by welding
          statements together.

Any single scalar over the two is gameable in one direction, which is the
entire reason the metric is two-dimensional. THE RATCHET IS PARETO: neither
axis may rise. There is no combined score and there must never be one.

CONDITION 4 IS THE WHOLE DESIGN. It is what makes axis A both non-gameable
and CORRECT: a temporary that cannot be inlined without crossing a side
effect is not noise, it is load-bearing evaluation order, and "fixing" it
would be an actual miscompile. It is the same question `laterArgSideEffects`
(lib/ImportC/ImportCStatements.cpp, FR-192) asks importer-side, run in the
opposite direction -- there, "do LATER arguments have effects, so must this
read be DEFERRED?"; here, "does anything evaluated BEFORE the use site have
effects, so must this binding STAY PUT?".

A REAL PARSER, NOT A REGEX. The `binding-probe` helper next door parses the
emitted crate with `syn`. A regex has no scope awareness -- it cannot see
shadowing, closure capture, macro bodies or evaluation order -- so it
necessarily OVERCOUNTS. Everything the probe cannot establish (an unmodelled
expression form, an unparseable macro body, a shadowed name, a use in a
conditionally-evaluated position) REJECTS the candidate and lands in its own
reported bucket, so axis A is a LOWER bound and the buckets show the size of
what is being left on the table.

THE PIN IS SHARED WITH clippy_eval (FR-144). The population comes from the
epoch document, not from whatever happens to be in the corpus directory, and
the epoch is re-hashed BEFORE anything is measured; a drifted or closed
epoch, an unpinned baseline, or a pinned file that stopped transpiling all
refuse to ratchet rather than produce a number attributable to nothing. The
pin helpers are IMPORTED from clippy_eval rather than copied, so the two
tools cannot drift apart on what "the same population" means.

Exit codes are Result-style: 0 = at or below baseline on BOTH axes,
1 = the ratchet fired (an axis rose), 2 = the comparison is not valid.

NOT WIRED INTO THE MESON GATE. Deliberately: the numbers want a few epochs of
observation before they become blocking.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.abspath(os.path.join(HERE, "..", "harness"))
if HARNESS not in sys.path:
    sys.path.insert(0, HARNESS)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import epoch as epoch_mod  # noqa: E402
# The pin/drift authority and the crate-naming rules are SHARED, not copied:
# if these two tools disagreed about which population they measured, the
# whole point of pairing them on one epoch would be lost.
import clippy_eval as ce  # noqa: E402

EMITRUST_CC = os.environ.get("EMITRUST_CC", "./build/tools/emitrust-cc")
PROBE_DIR = os.path.join(HERE, "binding-probe")
PROBE_BIN = os.environ.get(
    "BINDING_PROBE", os.path.join(PROBE_DIR, "target", "release", "binding-probe"))

# CLAUDE.md's epoch discipline: THREE consumers name the baseline document,
# and if they diverge the controller commits a file the ratchet never reads.
# This one was the odd consumer out -- it stayed pinned to epoch-6 after that
# epoch CLOSED, so a bare invocation exited 2 with "refusing to measure" and
# every caller had to pass --baseline by hand. Found 2026-09-10 by the FR-228
# implementation, which had to work around it.
DEFAULT_BASELINE = os.path.join(HERE, "binding-baseline-epoch7.json")

# The buckets the probe reports for candidates it refused to count. Named
# here so the report and the baseline agree on the vocabulary.
BUCKETS = ("blocked_effectful_prefix", "blocked_panicking_prefix",
           "blocked_place", "blocked_unordered", "blocked_maybe_drop",
           "skipped_shadowed", "skipped_inline_fmt", "skipped_mut")
COUNTERS = ("statements", "generated_lets", "unread_generated_lets",
            "single_use_next_stmt", "free_temps") + BUCKETS


def ensure_probe():
    """Build the syn-based probe if it is not already there."""
    if os.path.exists(PROBE_BIN):
        return PROBE_BIN
    r = subprocess.run(
        ["cargo", "build", "--offline", "--release",
         "--manifest-path", os.path.join(PROBE_DIR, "Cargo.toml")],
        capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(PROBE_BIN):
        print("could not build binding-probe:\n" + r.stderr, file=sys.stderr)
        sys.exit(2)
    return PROBE_BIN


def crate_sources(cfile, workdir):
    """Emit the crate and return its `.rs` sources, or None if it did not
    transpile standalone (which over a pinned population is an ERROR, exactly
    as in clippy_eval)."""
    stem = ce.crate_name(cfile)
    crate = os.path.join(workdir, ce.crate_dir_name(cfile))
    if subprocess.run([EMITRUST_CC, "--emit=crate", cfile, "-o", crate,
                       "--crate-name", stem],
                      capture_output=True, text=True).returncode != 0:
        return None
    src = os.path.join(crate, "src")
    if not os.path.isdir(src):
        return None
    return sorted(os.path.join(src, f) for f in os.listdir(src)
                  if f.endswith(".rs"))


def probe(binary, sources):
    r = subprocess.run([binary] + sources, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return json.loads(r.stdout)


def percentile_and_max(hist):
    """p95 and max over a {depth: count} histogram, computed on the WHOLE
    population at once -- a per-crate p95 averaged afterwards is a different
    (and meaningless) statistic."""
    items = sorted((int(k), v) for k, v in hist.items())
    total = sum(v for _, v in items)
    if not total:
        return 0, 0, 0.0
    acc = 0
    p95 = items[-1][0]
    for d, n in items:
        acc += n
        if acc * 100 >= total * 95:
            p95 = d
            break
    mean = sum(d * n for d, n in items) / total
    return p95, items[-1][0], mean


def measure(corpus, file_list=None, per_file=None, sites_cap=400):
    binary = ensure_probe()
    files = sorted(file_list) if file_list is not None else sorted(
        os.path.join(corpus, x) for x in os.listdir(corpus) if x.endswith(".c"))
    tot = {k: 0 for k in COUNTERS}
    hist = {}
    measured = skipped = parse_errors = 0
    per_crate = []
    sites = []
    deep = []
    with tempfile.TemporaryDirectory() as wd:
        for cfile in files:
            srcs = crate_sources(cfile, wd)
            if not srcs:
                skipped += 1
                continue
            rep = probe(binary, srcs)
            if rep is None:
                skipped += 1
                continue
            measured += 1
            parse_errors += rep["parse_errors"]
            for k in COUNTERS:
                tot[k] += rep[k]
            for d, n in rep["depth_hist"].items():
                hist[d] = hist.get(d, 0) + n
            for s in rep["sites"]:
                s = dict(s)
                s["crate"] = cfile
                s["file"] = os.path.basename(s["file"])
                sites.append(s)
            for s in rep["deep_sites"]:
                s = dict(s)
                s["crate"] = cfile
                s["file"] = os.path.basename(s["file"])
                deep.append(s)
            entry = {"file": cfile,
                     "statements": rep["statements"],
                     "free_temps": rep["free_temps"],
                     "single_use_next_stmt": rep["single_use_next_stmt"],
                     "generated_lets": rep["generated_lets"],
                     "depth_max": rep["depth_max"]}
            per_crate.append(entry)
            if per_file is not None:
                per_file[cfile] = entry

    # The refusal buckets must PARTITION the gap between conditions 1-3 and
    # conditions 1-4. If they ever stop summing, a candidate is being counted
    # twice or dropped silently, and the axis-A numerator is not what the
    # docstring says it is.
    gap = tot["single_use_next_stmt"] - tot["free_temps"]
    partition = sum(tot[k] for k in BUCKETS
                    if k not in ("skipped_shadowed", "skipped_inline_fmt"))
    assert gap == partition, (
        f"refusal buckets do not partition the axis-A gap: {gap} != "
        f"{partition}")

    p95, dmax, dmean = percentile_and_max(hist)
    stmts = tot["statements"]
    free_per_100 = round(100.0 * tot["free_temps"] / stmts, 4) if stmts else 0.0
    raw_per_100 = (round(100.0 * tot["single_use_next_stmt"] / stmts, 4)
                   if stmts else 0.0)
    free_sites = [s for s in sites if s["verdict"] == "free"]
    return {
        "corpus": corpus,
        "crates_measured": measured,
        "crates_skipped": skipped,
        "parse_errors": parse_errors,
        # --- axis A -------------------------------------------------------
        "statements": stmts,
        "free_temps": tot["free_temps"],
        "free_per_100_stmts": free_per_100,
        # conditions 1-3 only, i.e. what the metric would say WITHOUT the
        # side-effect condition. Reported so the work condition 4 does is
        # visible rather than assumed.
        "single_use_next_stmt": tot["single_use_next_stmt"],
        "single_use_per_100_stmts": raw_per_100,
        "generated_lets": tot["generated_lets"],
        "unread_generated_lets": tot["unread_generated_lets"],
        "rejected": {k: tot[k] for k in BUCKETS},
        # --- axis B -------------------------------------------------------
        "depth_p95": p95,
        "depth_max": dmax,
        "depth_mean": round(dmean, 4),
        "depth_hist": {str(k): hist[k] for k in sorted(hist, key=int)},
        # --- the work queue ------------------------------------------------
        "worst_crates": sorted(per_crate, key=lambda e: (-e["free_temps"],
                                                         e["file"]))[:40],
        "free_sites": free_sites[:sites_cap],
        # Axis B's work queue: without "here is the statement", "depth rose"
        # is a scalar nobody can act on.
        "deepest_stmts": sorted(deep, key=lambda d: (-d["depth"],
                                                     d["crate"]))[:sites_cap],
    }


def _updated_baseline(baseline, report, doc):
    """Carry the pin forward unchanged and replace only the measurement, so
    an update can never quietly drop the evidence of WHICH population the
    number came from."""
    fresh = dict(baseline or {})
    fresh.update({
        "epoch_id": doc["epoch_id"],
        "corpus": doc["corpus_root"],
        "corpus_roots": doc.get("corpus_roots", [doc["corpus_root"]]),
        "extensions": doc.get("extensions", [".c"]),
        "corpus_hash": doc["corpus_hash"],
        "emitter_rev": epoch_mod._git_rev(),
    })
    for k in ("crates_measured", "crates_skipped", "parse_errors",
              "statements", "free_temps", "free_per_100_stmts",
              "single_use_next_stmt", "single_use_per_100_stmts",
              "generated_lets", "unread_generated_lets", "rejected",
              "depth_p95", "depth_max", "depth_mean", "depth_hist",
              "worst_crates"):
        fresh[k] = report[k]
    return fresh


def print_report(report, doc, top, sites_n):
    print(f"crates measured: {report['crates_measured']}  "
          f"(skipped {report['crates_skipped']})")
    if doc:
        print(f"population: epoch-{doc['epoch_id']} pinned, "
              f"{doc['file_count']} files, {doc['corpus_hash']}")
    print(f"statements: {report['statements']}")
    print()
    print("AXIS A -- free single-use temporaries (conditions 1-4)")
    print(f"  free temps                 : {report['free_temps']}")
    print(f"  per 100 statements         : {report['free_per_100_stmts']}")
    print(f"  (cond 1-3, no side-effect  : {report['single_use_next_stmt']}"
          f" = {report['single_use_per_100_stmts']} /100)")
    print(f"  generated `v<N>` lets      : {report['generated_lets']}"
          f"  (+{report['unread_generated_lets']} unread `_v<N>`)")
    print("  refused (condition 4 and the parser's own limits):")
    for k, v in sorted(report["rejected"].items(), key=lambda kv: -kv[1]):
        if v:
            print(f"    {v:5d}  {k}")
    print()
    print("AXIS B -- expression nesting depth (the anti-cramming guard)")
    print(f"  p95 : {report['depth_p95']}    max : {report['depth_max']}"
          f"    mean : {report['depth_mean']}")
    print(f"  histogram: {report['depth_hist']}")
    if report["deepest_stmts"]:
        print("  deepest statements (the anti-cramming work queue):")
        for d in report["deepest_stmts"][:top]:
            print(f"    d={d['depth']:2d}  "
                  f"{os.path.basename(d['crate'])}:{d['line']}  {d['text']}")
    print()
    print("worst crates (the work queue):")
    print(f"  {'free':>5} {'/100':>6} {'stmt':>5}  file")
    for e in report["worst_crates"][:top]:
        per = 100.0 * e["free_temps"] / e["statements"] if e["statements"] else 0
        print(f"  {e['free_temps']:5d} {per:6.1f} {e['statements']:5d}  "
              f"{e['file']}")
    if sites_n:
        print()
        print("worst sites (crate:line -- the binding that is free to inline):")
        for s in report["free_sites"][:sites_n]:
            print(f"  {os.path.basename(s['crate'])}:{s['line']}  {s['text']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus", nargs="?", default=None,
                    help="corpus root; ignored when the baseline is pinned to "
                         "an epoch (the epoch's file list wins)")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--sites", type=int, default=0,
                    help="also list this many individual free-temp sites")
    ap.add_argument("--sites-cap", type=int, default=400,
                    help="how many free-temp sites to carry in --json (raise "
                         "it to audit the whole population by hand)")
    ap.add_argument("--file-list", default=None,
                    help="newline-separated paths to measure instead of the "
                         "pinned list (held-out split)")
    ap.add_argument("--json", default=None,
                    help="dump the full report to this path (does not touch "
                         "the baseline)")
    args = ap.parse_args()

    baseline = None
    if os.path.exists(args.baseline):
        with open(args.baseline) as f:
            baseline = json.load(f)

    file_list = None
    if args.file_list:
        file_list = epoch_mod.read_path_list(args.file_list)

    doc = None
    if file_list is None and ce.is_pinned(baseline):
        try:
            doc, file_list = ce.pinned_population(baseline)
        except ce.PinError as e:
            print(f"PIN CHECK FAILED for {args.baseline}:")
            print(f"  {e}")
            return 2
    corpus = args.corpus or (doc["corpus_root"] if doc else "test/EndToEnd")
    paired = doc is not None

    # BINDING-ONLY exclusions: measurable files that belong in the clippy
    # epoch but are machine-generated stress inputs pinning a compiler bound,
    # not specimens of emitted style. At epoch-7 two such files were 64% of the
    # statements. See nix/clippy-eval/binding-exclude.txt for the rationale and
    # the bar for adding one.
    excl_path = os.path.join(HERE, "binding-exclude.txt")
    excluded = []
    if file_list is not None and os.path.exists(excl_path):
        drop = set(epoch_mod.read_path_list(excl_path))
        excluded = sorted(f for f in file_list if f in drop)
        if excluded:
            file_list = [f for f in file_list if f not in drop]
            print(f"binding-only exclusions: {len(excluded)} "
                  f"(machine-generated bound tests; see binding-exclude.txt)")
            for f in excluded:
                print(f"  excluded  {f}")

    report = measure(corpus, file_list=file_list,
                     sites_cap=args.sites_cap)
    print_report(report, doc, args.top, args.sites)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
        print(f"\nreport written: {args.json}")

    if args.update or baseline is None:
        if doc is not None and (paired or baseline is None):
            fresh = _updated_baseline(baseline, report, doc)
        elif ce.is_pinned(baseline):
            print("REFUSING TO --update a pinned baseline from a partial "
                  "population: measure the whole epoch (drop --file-list).")
            return 2
        else:
            fresh = report
        with open(args.baseline, "w") as f:
            json.dump(fresh, f, indent=2)
            f.write("\n")
        print(f"baseline written: {args.baseline}")
        return 0

    if not ce.is_pinned(baseline):
        print(f"REFUSING TO RATCHET against {args.baseline}: the baseline is "
              "UNPINNED -- it records no epoch_id/corpus_hash, so there is no "
              "evidence it measured the same population and the delta would "
              "be attributable to nothing (FR-144).")
        return 2
    if not paired:
        print("measurement only: the file list is not the pinned population, "
              "so this is NOT a paired comparison and the ratchet was not "
              "evaluated.")
        return 0
    if report["crates_skipped"] or (report["crates_measured"]
                                    != baseline["crates_measured"]):
        print(f"POPULATION CHANGED: {baseline['crates_measured']} crates "
              f"measured at baseline, {report['crates_measured']} now "
              f"({report['crates_skipped']} skipped). Every pinned file was "
              "measurable when the epoch was frozen, so a file that stopped "
              "producing a crate is an emitter regression AND makes the "
              "totals incomparable. Refusing to ratchet.")
        return 2

    # PARETO. Neither axis may rise; there is no trade between them and no
    # sum of them.
    print()
    fired = False
    # depth_max is REPORTED but deliberately NOT ratcheted. It is a
    # single-statement statistic (one statement carries the whole value), so as
    # a gate input it is a tripwire that any unrelated edit to that one
    # statement flips. Worse, it is currently not measuring the thing axis B
    # exists to prevent: the measured depth outliers are compound-assignment
    # expansion and bitfield masking, not cramming. p95 guards cramming across
    # the population and is stable within a pinned epoch. Keep max visible --
    # a jump in it is worth a look -- but do not let it fire the gate.
    # Axis A ratchets the ABSOLUTE count, not the per-100-statements ratio.
    # Measured the hard way: the FR-132 late-init narrowing merged 30
    # `let x; x = v;` pairs into `let x = v;`, which left free_temps at
    # exactly 1934 and dropped statements 16373 -> 16343 -- so the RATIO rose
    # (11.8121 -> 11.8338) and the ratchet fired on a change that improved the
    # code and did not touch binding hygiene at all. Removing a good statement
    # must never look like debt. The population is frozen by the epoch (268
    # files, pinned bytes), so the absolute count is directly comparable
    # within an epoch -- which is exactly why clippy_eval ratchets
    # total_warnings rather than warnings-per-statement. The ratio stays
    # REPORTED, because it is the number that carries meaning ACROSS epochs
    # where the populations differ.
    for label, key, fmt in (("A  free temps    ", "free_temps", "{}"),
                            ("B  depth p95     ", "depth_p95", "{}")):
        prev, cur = baseline[key], report[key]
        delta = cur - prev
        print(f"ratchet {label}: {fmt.format(prev)} -> {fmt.format(cur)} "
              f"({delta:+g})")
        if delta > 0:
            fired = True
    if fired:
        print("RATCHET FIRED: binding-hygiene debt rose on at least one axis. "
              "The axes are Pareto -- an improvement on the other one does "
              "NOT pay for this. Fix, or --update deliberately.")
        return 1
    # FR-228 inflated the DENOMINATOR, and a reader must not mistake that for
    # progress. Its stdout runtime is ~44 counted statements emitted into
    # EVERY crate, so `statements` jumped 19695 -> 32481 and this ratio fell
    # 11.71 -> 7.10 with no emitter change behind it. CLAUDE.md warns that a
    # per-100-statements ratio is gameable by REMOVING good statements; this
    # is the mirror image, gameable by ADDING neutral ones. Both ratchet axes
    # are absolute and were unaffected. Excluding runtime items from the count
    # needs a change to the syn-based probe and is not done.
    print("(NOTE: `statements` includes FR-228's per-crate stdout runtime, so "
          "the ratio below\n moved without an emitter change -- compare the "
          "ABSOLUTE axes above, not this.)")
    print(f"(free/100 stmts {baseline['free_per_100_stmts']:.4f} -> "
          f"{report['free_per_100_stmts']:.4f}; statements "
          f"{baseline['statements']} -> {report['statements']}; "
          f"depth max {baseline['depth_max']} -> {report['depth_max']}"
          " -- all reported only, not ratchet inputs)")
    print("both axes at or below baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
