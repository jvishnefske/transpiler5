#!/usr/bin/env python3
"""Clippy evaluator + ratchet for the emitter's idiomatic-output quality.

Transpiles a pinned C/C++ corpus, runs DEFAULT clippy (`clippy::all`) on each
emitted crate, and tallies the warnings by lint. The ranked tally is the
emitter's idiomatic-Rust debt and the work queue for the auto-improvement loop
(LOOP.md); `total_warnings` is the ratcheted metric -- it must never rise.

  clippy_eval.py [corpus] [--baseline FILE] [--update] [--top N]
                 [--file-list FILE] [--json FILE]

THE RATCHET IS A PAIRED COMPARISON (FR-144). A delta between two emitter
revisions is attributable to the emitter only if the POPULATION is identical
on both sides. It was not: the committed baseline recorded 164 linted crates
over a 180-file corpus while the tree had grown to 186 `.c` files, so the
`68 -> 72` the ratchet was firing on was partly that day's new tests and was
attributable to nothing. The default baseline is therefore now PINNED to an
epoch (`nix/harness/epoch-N.json`): the measured file list comes from the
epoch document, not from whatever `.c` happens to be in the corpus directory,
and before comparing anything the tool re-hashes those files and REFUSES
(exit 2) if the epoch has drifted, has been closed, or does not match the
hash the baseline was measured under. An UNPINNED baseline is refused for the
same reason -- it cannot prove which population it measured.

Exit codes are Result-style: 0 = at or below baseline, 1 = the ratchet fired
(debt rose), 2 = the comparison is not valid (drift / closed epoch / unpinned
baseline / a pinned file that stopped transpiling). A crate that does not
transpile standalone is skipped, and over a pinned population a skip is an
ERROR, not a shrug: the epoch's files were measurable when it was frozen, so a
new skip silently removes a file from the denominator.

``--file-list FILE`` restricts the measurement to an explicit, newline-
separated set of paths instead of the pinned list. This is what makes the
FR-63.3 held-out split possible: the harness measures the train slice and the
held-out slice as two independent file lists over the frozen epoch corpus, so
the controller can reject a change that lowers train warnings while regressing
held-out. An explicit file list that is not the baseline's whole population is
a MEASUREMENT, not a ratchet run -- it reports and says so rather than
comparing across populations. ``--json FILE`` also dumps the full report (used
by the controller's score step) without touching the baseline.
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.abspath(os.path.join(HERE, "..", "harness"))
if HARNESS not in sys.path:
    sys.path.insert(0, HARNESS)
import epoch as epoch_mod  # noqa: E402  (the epoch/drift authority is shared)

EMITRUST_CC = os.environ.get("EMITRUST_CC", "./build/tools/emitrust-cc")

# The authoritative ratchet baseline. Pinned to epoch-7 (the union corpus
# test/EndToEnd x {.c,.cpp}, measurability-filtered, re-probed at the FR-188
# rev: 268 files, 30 exclusions).
# Epoch-5 was CLOSED when FR-188 legitimately edited a pinned EndToEnd test
# (stl-unique-ptr.cpp) and FR-185's deferred comment fix touched another
# (cpp-inheritance.cpp); like epoch-4 before it, its baseline document stays
# byte-untouched as history and the totals are NOT a trajectory (the
# population grew 244 -> 268, and the compiler change across the boundary was
# separately measured NEUTRAL). THREE consumers must
# name this same document -- this default, signals.py's DEFAULT_CLIPPY, and
# controller.py's CLIPPY_BASELINE, which `--update`s and COMMITS it. If they
# diverge, the controller commits a file the ratchet never reads.
DEFAULT_BASELINE = os.path.join(HERE, "clippy-baseline-epoch7.json")


class PinError(Exception):
    """The measurement cannot be a paired comparison (drift/closure/mismatch)."""


def is_pinned(baseline):
    """A baseline is pinned iff it names the epoch AND the corpus hash it was
    measured under. `clippy-baseline.json` (pre-FR-144) names neither, which
    is precisely why nobody could tell its population had moved."""
    return bool(baseline) and "epoch_id" in baseline and "corpus_hash" in baseline


def pinned_population(baseline):
    """(epoch document, file list) for a pinned baseline, or raise PinError.

    Verifies, in order: the epoch document exists; it is the same sample the
    baseline was measured over (corpus_hash); and the pinned files still hold
    their pinned bytes and the epoch is not closed history
    (`epoch.assert_comparable`).
    """
    eid = baseline["epoch_id"]
    try:
        doc = epoch_mod.load_epoch(eid)
    except SystemExit as e:
        raise PinError(str(e))
    if doc["corpus_hash"] != baseline["corpus_hash"]:
        raise PinError(
            f"baseline corpus_hash {baseline['corpus_hash']} != epoch-{eid} "
            f"corpus_hash {doc['corpus_hash']} -- the baseline was measured "
            "over a different sample; re-base it or freeze a new epoch.")
    try:
        epoch_mod.assert_comparable(eid)
    except SystemExit as e:
        raise PinError(str(e))
    return doc, [e["path"] for e in doc["files"]]


def crate_name(cfile):
    """The emitted crate's NAME -- the basename stem, snake-cased."""
    return os.path.splitext(os.path.basename(cfile))[0].replace("-", "_")


def crate_dir_name(cfile):
    """The crate's DIRECTORY inside the shared tempdir.

    Distinct from the crate name, and carrying the extension, because a union
    corpus can hold `foo.c` and `foo.cpp`: keyed by stem alone the second
    would overwrite the first in the shared workdir and one file's warnings
    would vanish from the tally with nothing to show for it. (Measured 0 stem
    collisions across epoch-4's 238 files and epoch-5's 244, so this is a
    latent hazard closed
    before it fires, not a live bug fix. The crate NAME is unchanged, so no
    emitted byte moves.)
    """
    ext = os.path.splitext(os.path.basename(cfile))[1].lstrip(".").lower()
    return f"{crate_name(cfile)}__{ext or 'noext'}"


def crate_lints(cfile, workdir):
    stem = crate_name(cfile)
    crate = os.path.join(workdir, crate_dir_name(cfile))
    if subprocess.run([EMITRUST_CC, "--emit=crate", cfile, "-o", crate,
                       "--crate-name", stem],
                      capture_output=True, text=True).returncode != 0:
        return None
    proc = subprocess.run(
        ["cargo", "clippy", "--offline", "--message-format=json", "--quiet"],
        cwd=crate, capture_output=True, text=True)
    lints = collections.Counter()
    for line in proc.stdout.splitlines():
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        if msg.get("reason") != "compiler-message":
            continue
        m = msg.get("message", {})
        if m.get("level") not in ("warning", "error"):
            continue
        code = (m.get("code") or {}).get("code") or ""
        if code.startswith("clippy::"):
            lints[code] += 1
    return lints


def measure(corpus, file_list=None, per_file=None):
    """Tally clippy over a file list. ``per_file`` (a dict, if given) is filled
    with path -> Counter so a caller can partition the SAME measurement into
    train/held-out slices instead of re-running the corpus per slice."""
    if file_list is not None:
        files = sorted(file_list)
    else:
        files = sorted(os.path.join(corpus, x) for x in os.listdir(corpus)
                       if x.endswith(".c"))
    total = collections.Counter()
    linted = skipped = 0
    with tempfile.TemporaryDirectory() as wd:
        for cfile in files:
            lints = crate_lints(cfile, wd)
            if lints is None:
                skipped += 1
                continue
            linted += 1
            total.update(lints)
            if per_file is not None:
                per_file[cfile] = lints
    return {
        "corpus": corpus,
        "crates_linted": linted,
        "crates_skipped": skipped,
        "total_warnings": sum(total.values()),
        "by_lint": dict(total.most_common()),
    }


def _slice_subtotals(doc, per_file):
    """Train / held-out subtotals of a single full-population measurement,
    using the epoch's committed split lists. Returns None when the epoch has
    no split on disk (nothing to partition by)."""
    eid = doc["epoch_id"]
    out = {}
    for tag, fname in (("train", f"epoch-{eid}.train.txt"),
                       ("held_out", f"epoch-{eid}.heldout.txt")):
        path = os.path.join(HARNESS, fname)
        if not os.path.exists(path):
            return None
        members = set(epoch_mod.read_path_list(path))
        tally = collections.Counter()
        linted = 0
        for f, lints in per_file.items():
            if f in members:
                linted += 1
                tally.update(lints)
        out[tag] = {"crates_linted": linted,
                    "crates_skipped": len(members) - linted,
                    "total_warnings": sum(tally.values()),
                    "by_lint": dict(tally.most_common())}
    return out


def _updated_baseline(baseline, report, doc, slices):
    """The refreshed baseline document for `--update`.

    Carries the pin (epoch id, corpus hash, split, provenance comment) forward
    unchanged and replaces only the measurement, so an update can never quietly
    drop the evidence of WHICH population the number came from.
    """
    fresh = dict(baseline or {})
    fresh.update({
        "epoch_id": doc["epoch_id"],
        "corpus": doc["corpus_root"],
        "corpus_roots": doc.get("corpus_roots", [doc["corpus_root"]]),
        "extensions": doc.get("extensions", [".c"]),
        "corpus_hash": doc["corpus_hash"],
        "emitter_rev": epoch_mod._git_rev(),
        "crates_linted": report["crates_linted"],
        "crates_skipped": report["crates_skipped"],
        "total_warnings": report["total_warnings"],
        "by_lint": report["by_lint"],
    })
    if slices:
        fresh["train"] = slices["train"]
        fresh["held_out"] = slices["held_out"]
    return fresh


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus", nargs="?", default=None,
                    help="corpus root; ignored when the baseline is pinned to "
                         "an epoch (the epoch's file list wins)")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--top", type=int, default=20)
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

    # Resolve the population. A pinned baseline supplies it from the epoch
    # document -- and the epoch is re-hashed BEFORE anything is measured, so a
    # drifted or closed population fails loudly instead of producing a number
    # that is attributable to nothing.
    # An explicit --file-list is a SLICE measurement (the train/held-out split);
    # it is never ratcheted, so it needs no pin check -- and must not be
    # coupled to the state of whichever epoch the default baseline names.
    doc = None
    if file_list is None and is_pinned(baseline):
        try:
            doc, file_list = pinned_population(baseline)
        except PinError as e:
            print(f"PIN CHECK FAILED for {args.baseline}:")
            print(f"  {e}")
            return 2
    corpus = args.corpus or (doc["corpus_root"] if doc else "test/EndToEnd")

    # Paired iff we measured exactly the population the baseline recorded.
    paired = doc is not None

    per_file = {} if paired else None
    report = measure(corpus, file_list=file_list, per_file=per_file)
    print(f"crates linted: {report['crates_linted']}  "
          f"(skipped {report['crates_skipped']})")
    print(f"total clippy warnings: {report['total_warnings']}")
    if paired:
        print(f"population: epoch-{doc['epoch_id']} pinned, "
              f"{doc['file_count']} files, {doc['corpus_hash']}")
    print("ranked lints (the work queue):")
    for lint, n in list(report["by_lint"].items())[:args.top]:
        print(f"  {n:5d}  {lint}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
        print(f"report written: {args.json}")

    slices = _slice_subtotals(doc, per_file) if paired else None

    if args.update or baseline is None:
        if doc is not None and (paired or baseline is None):
            fresh = _updated_baseline(baseline, report, doc, slices)
        elif is_pinned(baseline):
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

    if not is_pinned(baseline):
        print(f"REFUSING TO RATCHET against {args.baseline}: the baseline is "
              "UNPINNED -- it records no epoch_id/corpus_hash, so there is no "
              "evidence it measured the same population and the delta would "
              "be attributable to nothing (FR-144). Use the epoch-pinned "
              "baseline, or freeze an epoch and --update a new one.")
        return 2
    if not paired:
        print("measurement only: the file list is not the pinned population, "
              "so this is NOT a paired comparison and the ratchet was not "
              "evaluated.")
        return 0
    if report["crates_skipped"] or (report["crates_linted"]
                                    != baseline["crates_linted"]):
        print(f"POPULATION CHANGED: {baseline['crates_linted']} crates linted "
              f"at baseline, {report['crates_linted']} now "
              f"({report['crates_skipped']} skipped). Every pinned file was "
              "measurable when the epoch was frozen, so a file that stopped "
              "producing a crate is an emitter regression AND makes the "
              "totals incomparable. Refusing to ratchet.")
        return 2

    prev = baseline["total_warnings"]
    cur = report["total_warnings"]
    delta = cur - prev
    print(f"ratchet: {prev} -> {cur} ({delta:+d})")
    if delta > 0:
        print("RATCHET FIRED: clippy debt rose. Fix or --update deliberately.")
        return 1
    if delta < 0:
        print(f"improvement: -{-delta}. Run with --update to lower the baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
