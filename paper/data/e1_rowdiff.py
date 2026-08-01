#!/usr/bin/env python3
"""Per-project OLD vs NEW diff of e1_summary.csv and e5_compression.csv.

Any project present in both runs whose row changed is an unexplained move and
is printed; projects that appear or disappear are listed separately.
"""
import csv
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")


def load(d, f, keycols=2):
    with (S / d / f).open() as fh:
        rows = list(csv.reader(fh))
    hdr, rows = rows[0], rows[1:]
    return hdr, {tuple(r[:keycols]): r[keycols:] for r in rows}


for fname in ("e1_summary.csv", "e5_compression.csv", "e2_yield.csv",
              "e6_cost.csv"):
    try:
        h1, a = load("paper-data-v2", fname)
        h2, b = load("paper-data-v3", fname)
    except FileNotFoundError:
        print("%-22s (not yet produced)" % fname)
        continue
    only_old = sorted(set(a) - set(b))
    only_new = sorted(set(b) - set(a))
    changed = [k for k in sorted(set(a) & set(b)) if a[k] != b[k]]
    print("\n### %s   old=%d new=%d  only_old=%d only_new=%d changed=%d"
          % (fname, len(a), len(b), len(only_old), len(only_new), len(changed)))
    for k in only_old:
        print("  -only-old %s" % ("/".join(k),))
    for k in only_new:
        print("  +only-new %s  %s" % ("/".join(k), b[k]))
    cols = h1[2:]
    for k in changed:
        deltas = []
        for c, x, y in zip(cols, a[k], b[k]):
            if x != y:
                deltas.append("%s %s->%s" % (c, x, y))
        print("  ~changed  %-40s %s" % ("/".join(k), "; ".join(deltas)))
