#!/usr/bin/env python3
"""E4: (a) OLD vs NEW per (order, corpus, project) row diff, (b) monotonicity
check -- no project may lose crate_emitted / crate_builds / ported as order
increases, in either series."""
import csv
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
COLS = ["mode_used", "crate_emitted", "crate_builds", "graph_items", "ported"]


def load(d, f):
    with (S / d / f).open() as fh:
        return {(r["order"], r["corpus"], r["project"]): [r[c] for c in COLS]
                for r in csv.DictReader(fh)}


for f in ("e4_longitudinal.csv", "e4_longitudinal_plainmode.csv"):
    a, b = load("paper-data", f), load("paper-data-v2", f)
    shared = set(a) & set(b)
    changed = [k for k in sorted(shared) if a[k] != b[k]]
    print("\n### %s  shared=%d changed=%d only_old=%d only_new=%d"
          % (f, len(shared), len(changed), len(set(a) - set(b)),
             len(set(b) - set(a))))
    for k in sorted(set(b) - set(a)):
        print("  +only-new order=%s %s/%s  %s" % (k + (b[k],)))
    for k in changed:
        print("  ~changed  %s  %s -> %s" % (k, a[k], b[k]))

    # monotonicity over the NEW series
    proj = {}
    for (o, c, p), v in b.items():
        proj.setdefault((c, p), {})[int(o)] = v
    bad = []
    for key, series in sorted(proj.items()):
        prev = None
        for o in sorted(series):
            em, bl, gi, po = (int(series[o][1]), int(series[o][2]),
                              series[o][3], series[o][4])
            cur = (em, bl, int(po) if po else None)
            if prev:
                if cur[0] < prev[0] or cur[1] < prev[1]:
                    bad.append("%s/%s order %d: emitted/builds went %s -> %s"
                               % (key[0], key[1], o, prev[:2], cur[:2]))
                if cur[2] is not None and prev[2] is not None \
                        and cur[2] < prev[2]:
                    bad.append("%s/%s order %d: ported %d -> %d"
                               % (key[0], key[1], o, prev[2], cur[2]))
            prev = cur
    print("  monotonicity violations (NEW series): %d" % len(bad))
    for line in bad:
        print("    !! " + line)
