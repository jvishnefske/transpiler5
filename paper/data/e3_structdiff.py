#!/usr/bin/env python3
"""OLD vs NEW diff of e3_search.csv and e3_bound_sweep.csv on the NON-timing
columns.  wall_ms is expected to move; probes/graph_items/ported/builds are
the behavioural result and must not."""
import csv
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")


def load(d, f, key, cols):
    with (S / d / f).open() as fh:
        return {tuple(r[k] for k in key): [r[c] for c in cols]
                for r in csv.DictReader(fh)}


for f, key, cols in (
        ("e3_search.csv", ["corpus", "project", "mode"],
         ["probes", "graph_items", "ported", "builds"]),
        ("e3_bound_sweep.csv", ["project", "max_nodes"],
         ["probes", "ported"])):
    a, b = load("paper-data-v2", f, key, cols), load("paper-data-v3", f, key, cols)
    shared = set(a) & set(b)
    changed = [k for k in sorted(shared) if a[k] != b[k]]
    print("\n### %s  shared=%d changed=%d only_old=%d only_new=%d"
          % (f, len(shared), len(changed), len(set(a) - set(b)),
             len(set(b) - set(a))))
    for k in sorted(set(a) - set(b)):
        print("  -only-old %s  %s" % ("/".join(k), a[k]))
    for k in sorted(set(b) - set(a)):
        print("  +only-new %s  %s" % ("/".join(k), b[k]))
    for k in changed:
        print("  ~changed  %-50s %s -> %s"
              % ("/".join(k), list(zip(cols, a[k])), list(zip(cols, b[k]))))
