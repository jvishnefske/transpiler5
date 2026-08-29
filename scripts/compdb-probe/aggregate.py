#!/usr/bin/env python3
"""Deduped aggregation over a measure.py run.  The per-TU item graph counts one
item per (unit, symbol), so a header record imported in 400 TUs is 400 items:
ranking on the raw totals measures header fan-out, not porting progress.  This
collapses to distinct (symbol, file, line, kind) and reports DEFINED functions
separately from extern declarations."""
import json, collections, sys
d = json.load(open(sys.argv[1]))
order = {"ported": 3, "stubbed": 2, "dropped": 1, "declared": 0, "missing": 0}
best = {}
for r in d:
    j = r["progress"]
    if not j: continue
    for it in j["items"]:
        k = (it["symbol"], it["file"], it["line"], it["kind"])
        cur = best.get(k)
        if cur is None or order.get(it["status"], 0) > order.get(cur["status"], 0):
            best[k] = it
print(f"units with a crate: {sum(1 for r in d if r['progress'])}/{len(d)}")
print(f"distinct items: {len(best)}")
k = collections.Counter((it["kind"], it["status"]) for it in best.values())
for kind in sorted({x[0] for x in k}):
    row = {s: k[(kind, s)] for s in order if k[(kind, s)]}
    tot, p = sum(row.values()), row.get("ported", 0)
    print(f"  {kind:9s} {p:6d}/{tot:6d} {100*p/tot:5.1f}%  {row}")
fn = [it for it in best.values() if it["kind"] == "function" and it["status"] != "declared"]
p = sum(1 for it in fn if it["status"] == "ported")
print(f"\nDEFINED FUNCTIONS: {p}/{len(fn)} = {100*p/len(fn):.1f}%")
c = collections.Counter(it["diagnostic"][:100] for it in fn if it["status"] != "ported")
for x, n in c.most_common(20): print(f"  {n:6d} {x}")
# per-directory
per = collections.defaultdict(lambda: [0, 0])
for it in fn:
    dirn = "/".join(it["file"].replace("../", "").split("/")[:2])
    per[dirn][1] += 1; per[dirn][0] += it["status"] == "ported"
print("\nby source dir (defined functions):")
for dn, (pp, tt) in sorted(per.items(), key=lambda x: -x[1][1])[:20]:
    print(f"  {dn:28s} {pp:5d}/{tt:5d} {100*pp/tt:5.1f}%")
