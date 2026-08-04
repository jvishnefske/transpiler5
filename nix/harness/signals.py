#!/usr/bin/env python3
"""FR-63.4 -- signal merge + prioritizer for the improvement harness.

Three discovery sources emit signals about where the emitter should improve;
this module unifies them into ONE ranked, deduped queue so the controller can
pick a single top item per iteration. It is pure and deterministic: no LLM, no
emitter knowledge, no side effects -- it reads the sources' machine-readable
output and prints a stable ranked JSON queue.

  signal        source                                     yields
  ------------  -----------------------------------------  ---------------------
  correctness   nix/explore/explore.py --json              CRASH / HANG /
                                                           MISCOMPILE (bugs);
                                                           REJECT tags (demand)
  quality       nix/clippy-eval/clippy-baseline.json       ranked by_lint debt
  demand        REJECT tags (explore) + RealWorld blockers  unsupported
                                                           constructs

Scoring::

  priority = severity * leverage / risk

* severity -- CRASH/HANG > MISCOMPILE > new construct (REJECT) > clippy spelling.
* leverage -- how many items/warnings ONE fix retires (root-blame, not leaf):
  a clippy lint's by_lint count; a REJECT tag's file count; a crash signature's
  occurrence count. Fix the root, not the symptom.
* risk -- golden churn / blast radius / off-limits proximity. A spelling change
  is stdout-preserving (low); a codegen or construct change is high.

Excluded, never scored:
* Off-limits items (`clippy::needless_late_init`; cross-iteration loop liveness)
  -- a liveness change, not a spelling one, that miscompiled 3x. Reported under
  ``off_limits`` so they are visible but never picked.
* The long tail -- per-program clippy lints below --min-clippy and 1-of-N
  REJECT rarities below --min-demand. A bug (CRASH/HANG/MISCOMPILE) is never
  excluded by count; a single crash is still actionable.

Usage::

  signals.py [--clippy FILE] [--explore FILE ...] [--realworld FILE]
             [--min-clippy N] [--min-demand N] [--out FILE] [--top N]
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CLIPPY = os.path.join(HERE, "..", "clippy-eval", "clippy-baseline.json")

# Off-limits without a human + a new idea (CLAUDE.md, LOOP.md). needless_late_init
# folds a decl into its initializer -- a liveness change; cross-iteration loop
# liveness miscompiled three times. Keyed by clippy lint here; the loop-liveness
# bug class is enforced by the optimizer spec, not by a signature key.
OFF_LIMITS = frozenset({"clippy::needless_late_init"})

# severity tiers
SEV = {"CRASH": 100, "HANG": 100, "MISCOMPILE": 80, "REJECT": 40, "CLIPPY": 10}
# risk multipliers -- higher = more golden churn / blast radius
RISK = {"CRASH": 1.5, "HANG": 1.5, "MISCOMPILE": 2.0, "REJECT": 3.0, "CLIPPY": 1.0}


def _candidate(cid, signal, kind, leverage, detail, example, off_limits=False):
    severity = SEV[kind]
    risk = RISK[kind]
    priority = severity * leverage / risk
    return {
        "id": cid,
        "signal": signal,
        "kind": kind,
        "severity": severity,
        "leverage": leverage,
        "risk": risk,
        "priority": round(priority, 3),
        "off_limits": off_limits,
        "detail": detail,
        "example": example,
    }


def from_clippy(path, min_clippy):
    """Quality debt: each systematic lint is one candidate; count = leverage."""
    if not os.path.exists(path):
        return [], []
    with open(path) as f:
        report = json.load(f)
    picked, tail = [], []
    for lint, count in report.get("by_lint", {}).items():
        off = lint in OFF_LIMITS
        cand = _candidate(lint, "quality", "CLIPPY", count,
                          f"{count} emitted-Rust warnings of {lint}",
                          report.get("corpus", ""), off_limits=off)
        if off:
            tail.append(cand)
        elif count < min_clippy:
            tail.append({**cand, "reason": f"long tail (<{min_clippy})"})
        else:
            picked.append(cand)
    return picked, tail


def from_explore(paths, min_demand):
    """Correctness bugs + demand from one or more explore.py --json dumps."""
    picked, tail = [], []
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path) as f:
            data = json.load(f)
        for sig, d in data.get("signatures", {}).items():
            head = sig.split(":", 1)[0]
            occ = d.get("count", 0) + 1  # +1 for the exemplar (report convention)
            if head in ("CRASH", "HANG", "MISCOMPILE"):
                picked.append(_candidate(
                    sig, "correctness", head, occ,
                    d.get("detail", ""), d.get("file", "")))
            elif head == "REJECT":
                cand = _candidate(sig, "demand", "REJECT", occ,
                                  sig.split(":", 1)[1] if ":" in sig else sig,
                                  d.get("file", ""))
                (picked if occ >= min_demand else tail).append(
                    cand if occ >= min_demand
                    else {**cand, "reason": f"1-of-N rarity (<{min_demand})"})
            # TRANSPILED / PANIC / CRATE-EXIT are informational, not queue items.
    return picked, tail


def from_realworld(path, min_demand):
    """Demand from the RealWorld runner: REJECTED programs grouped by blocker
    tag. Parses the human report ('REJECTED  <name>  [<tag>]') -- the runner is
    text-first; this is a best-effort tally, skipped if the file is absent."""
    if not path or not os.path.exists(path):
        return [], []
    tags = {}
    line_re = re.compile(r"REJECTED\b.*?\[([^\]]+)\]")
    with open(path) as f:
        for line in f:
            m = line_re.search(line)
            if m:
                tags[m.group(1)] = tags.get(m.group(1), 0) + 1
    picked, tail = [], []
    for tag, count in tags.items():
        cand = _candidate(f"REALWORLD:{tag}", "demand", "REJECT", count,
                          f"{count} RealWorld programs blocked on {tag}", "")
        (picked if count >= min_demand else tail).append(cand)
    return picked, tail


def merge(candidates):
    """Dedup by id (max leverage wins), then rank SEVERITY-TIER FIRST and the
    leverage*(1/risk) product only WITHIN a tier.

    The plan's ``severity * leverage * (1/risk)`` is the intra-tier score, but
    severity is a strict ordering (CRASH/HANG > MISCOMPILE > new construct >
    clippy spelling): a correctness bug must outrank a quality lint no matter
    how many warnings the lint retires -- otherwise a 253-count spelling lint
    would bury a crash. So the queue is tiered by severity and ordered by the
    product inside each tier; ``priority`` is reported as the raw product."""
    by_id = {}
    for c in candidates:
        cur = by_id.get(c["id"])
        if cur is None or c["leverage"] > cur["leverage"]:
            by_id[c["id"]] = c
    return sorted(by_id.values(),
                  key=lambda c: (-c["severity"], -c["priority"], c["id"]))


def build_queue(clippy, explore, realworld, min_clippy, min_demand):
    picked, tail = [], []
    for src in (from_clippy(clippy, min_clippy),
                from_explore(explore, min_demand),
                from_realworld(realworld, min_demand)):
        picked += src[0]
        tail += src[1]
    return {"queue": merge(picked), "excluded": merge(tail)}


def main(argv=None):
    ap = argparse.ArgumentParser(description="merge harness signals into a "
                                             "ranked queue")
    ap.add_argument("--clippy", default=DEFAULT_CLIPPY)
    ap.add_argument("--explore", action="append", default=[],
                    help="explore.py --json dump (repeatable)")
    ap.add_argument("--realworld", default=None)
    ap.add_argument("--min-clippy", type=int, default=5)
    ap.add_argument("--min-demand", type=int, default=2)
    ap.add_argument("--out", default=None)
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args(argv)

    result = build_queue(args.clippy, args.explore, args.realworld,
                         args.min_clippy, args.min_demand)
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=2)
            f.write("\n")

    q = result["queue"]
    print(f"ranked queue: {len(q)} items "
          f"({len(result['excluded'])} excluded as off-limits / long tail)")
    print(f"{'#':>2}  {'priority':>9}  {'signal':11s} {'kind':11s} "
          f"{'lev':>4}  id")
    for i, c in enumerate(q[:args.top], 1):
        print(f"{i:>2}  {c['priority']:>9.1f}  {c['signal']:11s} "
              f"{c['kind']:11s} {c['leverage']:>4}  {c['id']}")
    if not q:
        print("  (queue empty -- nothing systematic to fix; a plateau signal)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
