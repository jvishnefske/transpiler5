#!/usr/bin/env python3
"""E3: which projects does `--search` genuinely improve, old (v2) vs new (v3)?

"Improved" = with the search on, the project either emits a compiling crate
where `--incremental` alone emitted none (`builds` 0 -> 1), or ports strictly
more items.  "Regressed" = the mirror image.  Both are computed from
e3_search.csv, which has one `mode=off` and one `mode=on` row per project.
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")


def num(x):
    return int(x) if x not in ("", None) else None


def analyse(d):
    by = defaultdict(dict)
    with (S / d / "e3_search.csv").open() as fh:
        for r in csv.DictReader(fh):
            by[(r["corpus"], r["project"])][r["mode"]] = r
    improved, regressed = [], []
    for k, m in sorted(by.items()):
        off, on = m["off"], m["on"]
        po, pn = num(off["ported"]), num(on["ported"])
        bo, bn = int(off["builds"]), int(on["builds"])
        why = []
        if bn > bo:
            why.append("builds 0->1")
        if po is None and pn is not None:
            why.append("ported -/-> %s" % pn)
        elif po is not None and pn is not None and pn > po:
            why.append("ported %d->%d" % (po, pn))
        if why:
            improved.append((k, ", ".join(why), num(on["probes"])))
        why = []
        if bn < bo:
            why.append("builds 1->0")
        if po is not None and pn is not None and pn < po:
            why.append("ported %d->%d" % (po, pn))
        if why:
            regressed.append((k, ", ".join(why), num(on["probes"])))
    return by, improved, regressed


for d in (sys.argv[1:] or ["paper-data-v2", "paper-data-v3"]):
    by, imp, reg = analyse(d)
    print("\n### %s   projects=%d  improved=%d  regressed=%d"
          % (d, len(by), len(imp), len(reg)))
    for (c, p), why, probes in imp:
        print("  + %-14s %-46s %-22s probes=%s" % (c, p, why, probes))
    for (c, p), why, probes in reg:
        print("  - %-14s %-46s %-22s probes=%s" % (c, p, why, probes))
    nsyn = sum(1 for (c, _), _, _ in imp if c == "synthetic")
    print("  synthetic among improved: %d / %d" % (nsyn, len(imp)))
