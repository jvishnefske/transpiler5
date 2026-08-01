#!/usr/bin/env python3
"""E2: per-corpus yield and the strict-vs-incremental gap, v2 vs v3.

The "discriminating subset" is cpp-realworld + c-realworld + endtoend, i.e.
excluding the saturated c-testsuite and the purpose-built synthetic search
fixtures.  It is the subset the paper quotes for the partial-translation claim.
"""
import csv
from collections import defaultdict
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
ORDER = {"cpp-realworld": 0, "c-realworld": 1, "endtoend": 2,
         "c-testsuite": 3, "synthetic": 4}
SUBSET = {"cpp-realworld", "c-realworld", "endtoend"}


def num(x):
    return int(x) if x not in ("", None) else 0


for d in ("paper-data-v2", "paper-data-v3"):
    with (S / d / "e2_yield.csv").open() as fh:
        rows = list(csv.DictReader(fh))
    agg = defaultdict(lambda: defaultdict(int))
    for r in rows:
        a = agg[r["corpus"]]
        a["n"] += 1
        for k in ("strict_ok", "strict_builds", "incr_emitted", "incr_builds"):
            a[k] += int(r[k])
        for k in ("graph_items", "ported", "stubbed", "dropped", "missing",
                  "declared"):
            a[k] += num(r[k])
    print("\n### %s" % d)
    print("%-14s %4s %7s %7s %7s %7s %7s %7s %6s %6s %6s %6s %5s"
          % ("corpus", "n", "sOK", "sBLD", "iEMIT", "iBLD", "items", "ported",
             "stub", "drop", "miss", "decl", "permil"))
    T = defaultdict(int)
    for c in sorted(agg, key=lambda x: ORDER.get(x, 9)):
        a = agg[c]
        for k in a:
            T[k] += a[k]
        pm = round(1000 * a["ported"] / a["graph_items"]) if a["graph_items"] else 0
        print("%-14s %4d %7d %7d %7d %7d %7d %7d %6d %6d %6d %6d %5d"
              % (c, a["n"], a["strict_ok"], a["strict_builds"],
                 a["incr_emitted"], a["incr_builds"], a["graph_items"],
                 a["ported"], a["stubbed"], a["dropped"], a["missing"],
                 a["declared"], pm))
    pm = round(1000 * T["ported"] / T["graph_items"]) if T["graph_items"] else 0
    print("%-14s %4d %7d %7d %7d %7d %7d %7d %6d %6d %6d %6d %5d"
          % ("TOTAL", T["n"], T["strict_ok"], T["strict_builds"],
             T["incr_emitted"], T["incr_builds"], T["graph_items"],
             T["ported"], T["stubbed"], T["dropped"], T["missing"],
             T["declared"], pm))
    sub = [r for r in rows if r["corpus"] in SUBSET]
    n = len(sub)
    so = sum(int(r["strict_ok"]) for r in sub)
    ib = sum(int(r["incr_builds"]) for r in sub)
    print("discriminating subset: n=%d strict=%d (%.1f%%) incremental=%d "
          "(%.1f%%) converted=%d"
          % (n, so, 100 * so / n, ib, 100 * ib / n, ib - so))
    bad = [(r["corpus"], r["project"]) for r in rows
           if int(r["strict_ok"]) != int(r["strict_builds"])
           or int(r["incr_emitted"]) != int(r["incr_builds"])]
    print("rows where an emitted crate failed to compile: %d %s" % (len(bad), bad))
    conv = [(r["corpus"], r["project"]) for r in sub
            if not int(r["strict_ok"]) and int(r["incr_builds"])]
    print("strict-fail -> incremental-builds (%d):" % len(conv))
    for c, p in conv:
        print("   %s/%s" % (c, p))
