#!/usr/bin/env python3
"""Turn raw_*.json into the paper CSVs."""
import csv, json, sys
from pathlib import Path

SCRATCH = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
               "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
OUT = SCRATCH / "paper-data-v3"
OUT.mkdir(parents=True, exist_ok=True)

rows = []
for f in ["v3_raw_run_all.json", "v3_raw_synth.json"]:
    p = SCRATCH / f
    if p.exists():
        rows += json.loads(p.read_text())

# synthetic entries from the generic driver used wrong args; drop them in
# favour of v3_raw_synth.json (which carries the real multi-TU / -I command lines)
seen_synth = {r["name"] for r in rows
              if r["corpus"] == "synthetic" and r.get("_synth")}
bykey = {}
for r in rows:
    k = (r["corpus"], r["name"])
    bykey[k] = r  # later file (raw_synth) wins
rows = list(bykey.values())
ORDER = {"cpp-realworld": 0, "c-realworld": 1, "endtoend": 2,
         "c-testsuite": 3, "synthetic": 4}
rows.sort(key=lambda r: (ORDER.get(r["corpus"], 9), r["name"]))

# ---------------- e2_yield.csv ----------------
with (OUT / "e2_yield.csv").open("w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["corpus", "project", "tus", "strict_ok", "strict_builds",
                "incr_emitted", "incr_builds", "graph_items", "ported",
                "stubbed", "dropped", "missing", "declared",
                "ported_permille"])
    for r in rows:
        w.writerow([r["corpus"], r["name"], r["tus"], r["strict_ok"],
                    r["strict_builds"], r["incr_emitted"], r["incr_builds"],
                    r["graph_items"], r["ported"], r["stubbed"], r["dropped"],
                    r["missing"], r["declared"], r["ported_permille"]])

# ---------------- e3_search.csv ----------------
with (OUT / "e3_search.csv").open("w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["corpus", "project", "mode", "probes", "wall_ms",
                "graph_items", "ported", "builds"])
    for r in rows:
        w.writerow([r["corpus"], r["name"], "off", 0, r["incremental_ms"],
                    r["graph_items"], r["ported"], r["incr_builds"]])
        w.writerow([r["corpus"], r["name"], "on", r["probes"], r["search_ms"],
                    r["search_graph_items"], r["search_ported_total"],
                    r["search_builds"]])

# ---------------- e6_cost.csv ----------------
with (OUT / "e6_cost.csv").open("w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["corpus", "project", "tus", "nodes", "edges", "graph_ms",
                "coloring_ms", "incremental_ms", "cargo_ms",
                "parse_floor_ms"])
    for r in rows:
        floor = min(x for x in (r["graph_ms"], r["coloring_ms"]) if x)
        w.writerow([r["corpus"], r["name"], r["tus"], r["nodes"], r["edges"],
                    r["graph_ms"], r["coloring_ms"], r["incremental_ms"],
                    r["cargo_ms"], round(floor, 1)])

# ---------------- e3_bound_sweep.csv ----------------
sp = SCRATCH / "v3_raw_sweep.json"
if sp.exists():
    with (OUT / "e3_bound_sweep.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["project", "max_nodes", "probes", "wall_ms", "ported"])
        for r in json.loads(sp.read_text()):
            w.writerow([r["project"], r["max_nodes"], r["probes"],
                        r["wall_ms"], r["ported"]])

# ------------- e6_cost_recheck.csv -------------
tp = SCRATCH / "v3_raw_timing.json"
if tp.exists():
    with (OUT / "e6_cost_recheck.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["corpus", "project", "tus", "nodes", "edges", "graph_ms",
                    "coloring_ms", "incremental_ms", "search_ms", "probes",
                    "loadavg1"])
        for r in json.loads(tp.read_text()):
            w.writerow([r["corpus"], r["name"], r["tus"], r["nodes"],
                        r["edges"], r["graph_ms"], r["coloring_ms"],
                        r["incremental_ms"], r["search_ms"], r["probes"],
                        round(r["load"], 2)])

# ---------------- summary ----------------
from collections import defaultdict
agg = defaultdict(lambda: defaultdict(int))
for r in rows:
    c = agg[r["corpus"]]
    c["n"] += 1
    c["strict_ok"] += r["strict_ok"]
    c["strict_builds"] += r["strict_builds"]
    c["incr_emitted"] += r["incr_emitted"]
    c["incr_builds"] += r["incr_builds"]
    if r["graph_items"] != "":
        c["items"] += r["graph_items"]
        c["ported"] += r["ported"]
        c["stubbed"] += r["stubbed"]
        c["dropped"] += r["dropped"]
    if r["probes"] not in ("", None):
        c["probe_sum"] += r["probes"]
        c["probe_n"] += 1
        c["probe_gt1"] += int(r["probes"] > 1)
    c["improved"] += r.get("search_improved", 0)
    c["search_builds"] += r["search_builds"]
print(f"{'corpus':14s} {'n':>4} {'sOK':>4} {'sBLD':>5} {'iEMT':>5} {'iBLD':>5} "
      f"{'items':>6} {'ported':>6} {'stub':>5} {'drop':>5} {'prb>1':>6} {'impr':>5}")
for k, c in agg.items():
    print(f"{k:14s} {c['n']:4d} {c['strict_ok']:4d} {c['strict_builds']:5d} "
          f"{c['incr_emitted']:5d} {c['incr_builds']:5d} {c['items']:6d} "
          f"{c['ported']:6d} {c['stubbed']:5d} {c['dropped']:5d} "
          f"{c['probe_gt1']:6d} {c['improved']:5d}")
print()
# timing aggregates
import statistics as st
for k in agg:
    rs = [r for r in rows if r["corpus"] == k]
    g = [r["graph_ms"] for r in rs]
    co = [r["coloring_ms"] for r in rs]
    inc = [r["incremental_ms"] for r in rs]
    se = [r["search_ms"] for r in rs]
    ca = [r["cargo_ms"] for r in rs if r["cargo_ms"] != ""]
    print(f"{k:14s} median ms  graph={st.median(g):7.1f} color={st.median(co):7.1f} "
          f"incr={st.median(inc):7.1f} search={st.median(se):7.1f} "
          f"cargo={st.median(ca) if ca else float('nan'):7.1f}  "
          f"graph/incr={st.median(g)/st.median(inc):.2f} "
          f"search/incr={st.median(se)/st.median(inc):.2f}")
