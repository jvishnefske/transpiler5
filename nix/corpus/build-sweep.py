#!/usr/bin/env python3
"""Corpus BUILD sweep -- does every emitted crate actually COMPILE?

The Track-5 probe loop (cc-shim.py + tabulate.py) measures IMPORT: it records
TRANSPILED vs REJECT per translation unit from emitrust-cc's own diagnostics
and never once runs `cargo build` on what came out. That oracle is
STRUCTURALLY BLIND to a whole defect class -- a crate can import at 100% and
not compile -- and that blindness is exactly how design.md FR-105 shipped: a
loop-assigned deferred binding emitted without `mut`, three `error[E0384]`s in
tiny-AES-c's crate, invisible to every import-side measurement.

This is the missing half. It REGENERATES every crate from the tools as they
are right now, runs `cargo build --release --offline` in each, and classifies
every compiler ERROR by its rustc code (E0384, unused_assignments, ...).

  Regeneration is not optional. A crate left over from an earlier session
  lies in both directions: during FR-105 a STALE heatshrink decoder crate
  built clean while the same unit regenerated from current tools did not.

`cargo build`, not `cargo clippy`: clippy's own deny-by-default groups object
to faithfully transpiled code (`approx_constant` on tinyexpr's pi literal)
without anything being wrong with the build. Build failures are the defect
class this exists to catch; lint opinions are a separate conversation.

Known failures live in build-known-fail.txt as a RATCHET -- a crate off the
list that fails is a regression, and a crate on the list that starts passing
(or fails differently) is also reported, so a pinned defect can never be
silently fixed or silently replaced by another.

Usage:
  nix develop -c python3 nix/corpus/build-sweep.py <corpus-root> \
      [--emitrust-cc build/tools/emitrust-cc] [--work DIR] [--jobs N]

<corpus-root> is a directory of upstream clones laid out as
build-sweep-corpus.json describes. Exit status is 0 only when the sweep
matches the ledger exactly.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def load_specs(path):
    with open(path, encoding="utf-8") as f:
        cfg = json.load(f)
    return cfg["projects"], tuple(cfg.get("skip", ()))


def load_ledger(path):
    """crate name -> expected rustc error code."""
    known = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            crate, _, code = line.partition(" ")
            known[crate] = code.strip()
    return known


def emit(cc, src, crate_dir, includes):
    """Emit one lib crate from one .c file. Returns True if a crate appeared."""
    shutil.rmtree(crate_dir, ignore_errors=True)
    cmd = [cc, "--emit=crate", "--crate-type=lib", "--incremental", src,
           "-o", crate_dir]
    cmd += ["--extra-arg=-I" + inc for inc in includes]
    subprocess.run(cmd, capture_output=True, text=True)
    return os.path.isfile(os.path.join(crate_dir, "Cargo.toml"))


def build(crate_dir):
    """Run cargo on one crate; return a Counter of rustc error codes."""
    proc = subprocess.run(
        ["cargo", "build", "--release", "--offline", "--quiet",
         "--message-format=json"],
        cwd=crate_dir, capture_output=True, text=True)
    codes = collections.Counter()
    for line in proc.stdout.splitlines():
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if rec.get("reason") != "compiler-message":
            continue
        msg = rec.get("message", {})
        if msg.get("level") != "error":
            continue
        code = (msg.get("code") or {}).get("code")
        # An error with no code (a lint group, a link failure) is keyed by its
        # message so the ledger can still pin it exactly.
        codes[code or msg.get("message", "?")] += 1
    if not codes and proc.returncode != 0:
        codes["cargo-failed-without-a-diagnostic"] += 1
    return codes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--emitrust-cc",
                    default=os.path.join(REPO, "build", "tools", "emitrust-cc"))
    ap.add_argument("--work", default=None,
                    help="crate output dir (default: <corpus-root>/.build-sweep)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--specs", default=os.path.join(HERE, "build-sweep-corpus.json"))
    ap.add_argument("--ledger", default=os.path.join(HERE, "build-known-fail.txt"))
    args = ap.parse_args()

    if not os.path.isfile(args.emitrust_cc):
        sys.exit("no emitrust-cc at %s (build first)" % args.emitrust_cc)
    projects, skip = load_specs(args.specs)
    known = load_ledger(args.ledger)
    work = args.work or os.path.join(args.corpus, ".build-sweep")
    os.makedirs(work, exist_ok=True)

    units = []
    for spec in projects:
        srcdir = os.path.join(args.corpus, spec["sources"])
        if not os.path.isdir(srcdir):
            continue
        includes = [os.path.join(args.corpus, i) for i in spec["includes"]]
        for name in sorted(os.listdir(srcdir)):
            if not name.endswith(".c") or any(s in name for s in skip):
                continue
            crate = "%s_%s" % (spec["name"], name[:-2])
            units.append((crate, os.path.join(srcdir, name),
                          os.path.join(work, crate), includes))
    if not units:
        sys.exit("no translation units found under %s" % args.corpus)

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
    emitted = [u for u, ok in zip(
        units, pool.map(lambda u: emit(args.emitrust_cc, u[1], u[2], u[3]), units))
        if ok]
    results = dict(zip([u[0] for u in emitted],
                       pool.map(lambda u: build(u[2]), emitted)))

    ok = sorted(c for c, codes in results.items() if not codes)
    bad = {c: codes for c, codes in results.items() if codes}
    print("crates emitted=%d  BUILD OK=%d  FAIL=%d"
          % (len(emitted), len(ok), len(bad)))
    for crate in sorted(bad):
        print("  FAIL %-40s %s" % (crate, dict(bad[crate])))

    regressions, fixed, drifted = [], [], []
    for crate, codes in sorted(bad.items()):
        if crate not in known:
            regressions.append((crate, dict(codes)))
        elif known[crate] not in codes:
            drifted.append((crate, known[crate], dict(codes)))
    for crate in sorted(known):
        if crate in results and crate not in bad:
            fixed.append(crate)

    for crate, codes in regressions:
        print("REGRESSION: %s is not in the ledger and does not build: %s"
              % (crate, codes))
    for crate, want, got in drifted:
        print("LEDGER DRIFT: %s is pinned as %s but failed with %s"
              % (crate, want, got))
    for crate in fixed:
        print("LEDGER STALE: %s builds now -- remove it from the ledger "
              "in the same change that earns it" % crate)
    if regressions or drifted or fixed:
        return 1
    print("sweep matches the ledger (%d known-fail)" % len(known))
    return 0


if __name__ == "__main__":
    sys.exit(main())
