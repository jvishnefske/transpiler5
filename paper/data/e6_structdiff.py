#!/usr/bin/env python3
"""OLD vs NEW diff of e6_cost.csv restricted to the STRUCTURAL columns
(tus, nodes, edges).  The four *_ms columns are wall clock and are expected to
differ; a change in tus/nodes/edges would mean the item graph itself moved."""
import csv
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
COLS = ["tus", "nodes", "edges"]


def load(d):
    with (S / d / "e6_cost.csv").open() as fh:
        return {(r["corpus"], r["project"]): [r[c] for c in COLS]
                for r in csv.DictReader(fh)}


a, b = load("paper-data-v2"), load("paper-data-v3")
changed = [k for k in sorted(set(a) & set(b)) if a[k] != b[k]]
print("shared=%d  changed(tus/nodes/edges)=%d  only_new=%s"
      % (len(set(a) & set(b)), len(changed),
         sorted("/".join(k) for k in set(b) - set(a))))
for k in changed:
    print("  ~ %-45s %s -> %s" % ("/".join(k), a[k], b[k]))
