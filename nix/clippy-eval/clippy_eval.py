#!/usr/bin/env python3
"""Clippy evaluator + ratchet for the emitter's idiomatic-output quality.

Transpiles a C corpus, runs DEFAULT clippy (`clippy::all`) on each emitted
crate, and tallies the warnings by lint. The ranked tally is the emitter's
idiomatic-Rust debt and the work queue for the auto-improvement loop
(LOOP.md); `total_warnings` is the ratcheted metric -- it must never rise.

  clippy_eval.py [corpus] [--baseline FILE] [--update] [--top N]
                 [--file-list FILE] [--json FILE]

Default corpus is test/EndToEnd. Exit code is non-zero when the total exceeds
the baseline (the ratchet fired) unless --update rewrites the baseline. A
crate that does not transpile standalone is skipped (not clippy's concern);
the skip count is reported so the denominator is honest.

``--file-list FILE`` restricts the measurement to an explicit, newline-
separated set of ``.c`` paths instead of enumerating the corpus directory.
This is what makes the FR-63.3 held-out split possible: the harness measures
the train slice and the held-out slice as two independent file lists over the
frozen epoch corpus, so the controller can reject a change that lowers train
warnings while regressing held-out. ``--json FILE`` also dumps the full
report (used by the controller's score step) without touching the baseline.
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import tempfile

EMITRUST_CC = os.environ.get("EMITRUST_CC", "./build/bin/emitrust-cc")


def crate_lints(cfile, workdir):
    stem = os.path.splitext(os.path.basename(cfile))[0].replace("-", "_")
    crate = os.path.join(workdir, stem)
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


def measure(corpus, file_list=None):
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
    return {
        "corpus": corpus,
        "crates_linted": linted,
        "crates_skipped": skipped,
        "total_warnings": sum(total.values()),
        "by_lint": dict(total.most_common()),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus", nargs="?", default="test/EndToEnd")
    ap.add_argument("--baseline", default="nix/clippy-eval/clippy-baseline.json")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--file-list", default=None,
                    help="newline-separated .c paths to measure instead of "
                         "enumerating the corpus dir (held-out split)")
    ap.add_argument("--json", default=None,
                    help="dump the full report to this path (does not touch "
                         "the baseline)")
    args = ap.parse_args()

    file_list = None
    if args.file_list:
        with open(args.file_list) as f:
            file_list = [ln.strip() for ln in f if ln.strip()
                         and not ln.startswith("#")]

    report = measure(args.corpus, file_list=file_list)
    print(f"crates linted: {report['crates_linted']}  "
          f"(skipped {report['crates_skipped']})")
    print(f"total clippy warnings: {report['total_warnings']}")
    print("ranked lints (the work queue):")
    for lint, n in list(report["by_lint"].items())[:args.top]:
        print(f"  {n:5d}  {lint}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
        print(f"report written: {args.json}")

    baseline = None
    if os.path.exists(args.baseline):
        with open(args.baseline) as f:
            baseline = json.load(f)

    if args.update or baseline is None:
        with open(args.baseline, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
        print(f"baseline written: {args.baseline}")
        return 0

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
