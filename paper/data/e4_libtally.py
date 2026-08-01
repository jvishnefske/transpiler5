#!/usr/bin/env python3
"""Per-revision lib-vs-bin split of the E4 raw records (FR-51 only).

The E4 CSV schema is frozen, so this lives beside it rather than in it.  A
`lib` crate compiles but has no entry point: it is evidence of translatability,
never of behaviour, and is therefore never summed with `bin`.
"""
import json
import os

D = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(D, "raw")
COMMITS = [(1, "54fe0fe", "plain"), (2, "c3fbb13", "plain"),
           (3, "1f1dc8f", "plain"), (4, "385efc3", "incremental"),
           (5, "1a37f6c", "incremental"), (6, "c665b20", "incremental"),
           (7, "bc0be94", "incremental"), (8, "4c89af1", "incremental")]

print("%-6s %-9s %-12s %5s %8s %8s %8s %8s"
      % ("order", "sha", "mode", "proj", "emitted", "bin", "lib", "building"))
for order, sha, mode in COMMITS:
    p = os.path.join(RAW, "e4_%d_%s_%s.json" % (order, sha, mode))
    if not os.path.isfile(p):
        print("%-6d %-9s %-12s  (missing)" % (order, sha, mode))
        continue
    with open(p, encoding="utf-8") as h:
        recs = json.load(h)
    emitted = sum(r["crate_emitted"] for r in recs)
    building = sum(r["crate_builds"] for r in recs)
    lib = sum(1 for r in recs if r.get("crate_kind") == "lib")
    binc = sum(1 for r in recs if r.get("crate_kind") == "bin")
    print("%-6d %-9s %-12s %5d %8d %8d %8d %8d"
          % (order, sha, mode, len(recs), emitted, binc, lib, building))
    if lib:
        print("        lib crates: %s"
              % ", ".join("%s/%s" % (r["corpus"], r["project"])
                          for r in recs if r.get("crate_kind") == "lib"))
