#!/usr/bin/env python3
"""Side-by-side OLD (paper-data) vs NEW (paper-data-v3) headline numbers."""
import csv
import statistics as st
from collections import defaultdict
from pathlib import Path

S = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
         "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
OLD, NEW = S / "paper-data-v2", S / "paper-data-v3"
ORDER = {"cpp-realworld": 0, "c-realworld": 1, "endtoend": 2,
         "c-testsuite": 3, "synthetic": 4}


def rd(d, name):
    p = d / name
    if not p.exists():
        return None
    with p.open() as fh:
        return list(csv.DictReader(fh))


def num(x):
    return int(x) if x not in ("", None) else None


def sec(t):
    print("\n" + "=" * 78 + "\n" + t + "\n" + "=" * 78)


# ---------------- E1 ----------------
sec("E1 coloring precision")
for tag, d in (("OLD", OLD), ("NEW", NEW)):
    rows = rd(d, "e1_summary.csv")
    if rows is None:
        print(tag, "missing")
        continue
    agg = defaultdict(lambda: defaultdict(int))
    for r in rows:
        a = agg[r["corpus"]]
        a["projects"] += 1
        for k in ("items", "green", "yellow", "red", "ported", "stubbed",
                  "dropped", "missing", "declared", "false_green",
                  "false_red"):
            a[k] += int(r[k])
    T = defaultdict(int)
    print("%-4s %-14s %5s %6s %6s %6s %5s %6s %5s %5s %4s %4s"
          % (tag, "corpus", "proj", "items", "green", "red", "port", "stub",
             "drop", "miss", "FG", "FR"))
    for c in sorted(agg, key=lambda x: ORDER.get(x, 9)):
        a = agg[c]
        for k in a:
            T[k] += a[k]
        print("%-4s %-14s %5d %6d %6d %6d %5d %6d %5d %5d %4d %4d"
              % ("", c, a["projects"], a["items"], a["green"], a["red"],
                 a["ported"], a["stubbed"], a["dropped"], a["missing"],
                 a["false_green"], a["false_red"]))
    print("%-4s %-14s %5d %6d %6d %6d %5d %6d %5d %5d %4d %4d"
          % ("", "TOTAL", T["projects"], T["items"], T["green"], T["red"],
             T["ported"], T["stubbed"], T["dropped"], T["missing"],
             T["false_green"], T["false_red"]))
    print("%-4s skipped=%d unmatched=%d"
          % ("", len(rd(d, "e1_skipped.csv")), len(rd(d, "e1_unmatched.csv"))))

# ---------------- E2 ----------------
sec("E2 yield")
for tag, d in (("OLD", OLD), ("NEW", NEW)):
    rows = rd(d, "e2_yield.csv")
    if rows is None:
        print(tag, "missing")
        continue
    agg = defaultdict(lambda: defaultdict(int))
    for r in rows:
        a = agg[r["corpus"]]
        a["n"] += 1
        for k in ("strict_ok", "strict_builds", "incr_emitted", "incr_builds"):
            a[k] += int(r[k])
        for k in ("graph_items", "ported", "stubbed", "dropped", "missing",
                  "declared"):
            v = num(r[k])
            if v is not None:
                a[k] += v
    T = defaultdict(int)
    print("%-4s %-14s %4s %8s %9s %10s %10s %6s %6s"
          % (tag, "corpus", "n", "strictOK", "strictBLD", "incrEMIT",
             "incrBUILD", "items", "ported"))
    for c in sorted(agg, key=lambda x: ORDER.get(x, 9)):
        a = agg[c]
        for k in a:
            T[k] += a[k]
        print("%-4s %-14s %4d %8d %9d %10d %10d %6d %6d"
              % ("", c, a["n"], a["strict_ok"], a["strict_builds"],
                 a["incr_emitted"], a["incr_builds"], a["graph_items"],
                 a["ported"]))
    print("%-4s %-14s %4d %8d %9d %10d %10d %6d %6d"
          % ("", "TOTAL", T["n"], T["strict_ok"], T["strict_builds"],
             T["incr_emitted"], T["incr_builds"], T["graph_items"],
             T["ported"]))

# ---------------- E3 ----------------
sec("E3 search")
for tag, d in (("OLD", OLD), ("NEW", NEW)):
    rows = rd(d, "e3_search.csv")
    if rows is None:
        print(tag, "missing")
        continue
    by = defaultdict(dict)
    for r in rows:
        by[(r["corpus"], r["project"])][r["mode"]] = r
    improved = regressed = 0
    ratios = []
    rescues = []
    probes_gt1 = 0
    for k, m in by.items():
        off, on = m.get("off"), m.get("on")
        if not off or not on:
            continue
        po, pn = num(off["ported"]), num(on["ported"])
        bo, bn = int(off["builds"]), int(on["builds"])
        if (po is None and pn is not None) or \
           (po is not None and pn is not None and pn > po) or (bn > bo):
            improved += 1
            if bn > bo:
                rescues.append("%s/%s" % k)
        if (pn is not None and po is not None and pn < po) or (bn < bo):
            regressed += 1
        if float(off["wall_ms"]) > 0:
            ratios.append(float(on["wall_ms"]) / float(off["wall_ms"]))
        if num(on["probes"]) and num(on["probes"]) > 1:
            probes_gt1 += 1
    print("%-4s projects=%d improved=%d regressed=%d rescued(build 0->1)=%d"
          % (tag, len(by), improved, regressed, len(rescues)))
    print("     probes>1=%d  overhead on/off: median=%.2f mean=%.2f max=%.2f"
          % (probes_gt1, st.median(ratios), st.fmean(ratios), max(ratios)))
    print("     rescues: %s" % ", ".join(sorted(rescues)))

# ---------------- E4 ----------------
sec("E4 longitudinal (all rows)")
for tag, d in (("OLD", OLD), ("NEW", NEW)):
    rows = rd(d, "e4_totals.csv")
    if rows is None:
        print(tag, "missing")
        continue
    print(tag)
    for r in rows:
        if r["corpus"] != "all":
            continue
        print("   order=%s sha=%s emitted=%s/%s building=%s/%s ported=%s/%s"
              % (r["order"], r["sha"], r["crates_emitted"], r["projects"],
                 r["crates_building"], r["projects"], r["ported_total"],
                 r["graph_items_total"]))

# ---------------- E5 ----------------
sec("E5 attribution")
for tag, d in (("OLD", OLD), ("NEW", NEW)):
    rows = rd(d, "e5_compression.csv")
    if rows is None:
        print(tag, "missing")
        continue
    agg = defaultdict(lambda: defaultdict(int))
    for r in rows:
        a = agg[r["corpus"]]
        a["projects"] += 1
        a["with_rejects"] += int(int(r["rejected_items"]) > 0)
        a["rejected"] += int(r["rejected_items"])
        a["root_items"] += int(r["root_items"])
    T = defaultdict(int)
    print("%-4s %-14s %5s %13s %9s %10s %6s"
          % (tag, "corpus", "proj", "with_rejects", "rejected", "root_items",
             "ratio"))
    for c in sorted(agg, key=lambda x: ORDER.get(x, 9)):
        a = agg[c]
        for k in a:
            T[k] += a[k]
        rr = a["rejected"] / a["root_items"] if a["root_items"] else float("nan")
        print("%-4s %-14s %5d %13d %9d %10d %6.2f"
              % ("", c, a["projects"], a["with_rejects"], a["rejected"],
                 a["root_items"], rr))
    rr = T["rejected"] / T["root_items"] if T["root_items"] else float("nan")
    print("%-4s %-14s %5d %13d %9d %10d %6.2f"
          % ("", "TOTAL", T["projects"], T["with_rejects"], T["rejected"],
             T["root_items"], rr))

# ---------------- E6 ----------------
sec("E6 cost")
for tag, d in (("OLD", OLD), ("NEW", NEW)):
    rows = rd(d, "e6_cost.csv")
    if rows is None:
        print(tag, "missing")
        continue
    g = [float(r["graph_ms"]) for r in rows]
    co = [float(r["coloring_ms"]) for r in rows]
    inc = [float(r["incremental_ms"]) for r in rows]
    ca = [float(r["cargo_ms"]) for r in rows if r["cargo_ms"]]
    rg = [float(r["graph_ms"]) / float(r["incremental_ms"]) for r in rows]
    rc_ = [float(r["coloring_ms"]) / float(r["incremental_ms"]) for r in rows]
    marg = [(float(r["incremental_ms"]) - float(r["parse_floor_ms"]))
            / float(r["incremental_ms"]) for r in rows]
    print("%-4s n=%d  sum: graph=%.1fs coloring=%.1fs incr=%.1fs cargo=%.1fs"
          % (tag, len(rows), sum(g) / 1000, sum(co) / 1000, sum(inc) / 1000,
             sum(ca) / 1000))
    print("     median graph/incr=%.2f coloring/incr=%.2f "
          "marginal-translation=%.1f%%  tool/cargo=%.1f%%"
          % (st.median(rg), st.median(rc_), 100 * st.median(marg),
             100 * sum(inc) / sum(ca)))
