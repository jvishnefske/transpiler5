#!/usr/bin/env python3
"""Turn v3_raw_e1e5.json into the E1 and E5 CSVs (schemas frozen from the
prior run; see paper-data/e1_e5_README.md)."""
import csv
import json
import re
from collections import defaultdict
from pathlib import Path

SCRATCH = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
               "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
OUT = SCRATCH / "paper-data-v3"
OUT.mkdir(parents=True, exist_ok=True)

ORDER = {"cpp-realworld": 0, "c-realworld": 1, "endtoend": 2, "c-testsuite": 3}
rows = json.loads((SCRATCH / "v3_raw_e1e5.json").read_text())
rows.sort(key=lambda r: (ORDER.get(r["corpus"], 9), r["name"]))

joined, summary, skipped, unmatched, missing_audit = [], [], [], [], []
e5_direct, e5_root, e5_comp = [], [], []
confusion = defaultdict(lambda: defaultdict(int))
STATUSES = ["ported", "stubbed", "dropped", "missing", "declared"]


def audit(sym, kind, text):
    if not text:
        return "absent-from-crate"
    esc = re.escape(sym)
    if kind == "function":
        pat = r"\bfn\s+%s\b" % esc
    elif kind in ("record", "enum"):
        pat = r"\b(struct|enum|union)\s+%s\b" % esc
    else:
        pat = r"\b%s\b" % esc
    return "present-in-crate" if re.search(pat, text) else "absent-from-crate"


for r in rows:
    corpus, name = r["corpus"], r["name"]
    prog = r.get("progress")
    citems = r.get("coloring_items") or {}
    if prog is None:
        head = " ".join((r.get("incr_stderr_head") or "").split())
        skipped.append([corpus, name,
                        "incremental-no-progress-json (rc=%s): %s"
                        % (r["incr_rc"], head[:200])])
        continue
    if not citems and prog.get("items"):
        head = " ".join((r.get("coloring_stderr_head") or "").split())
        skipped.append([corpus, name,
                        "coloring-no-items (rc=%s): %s"
                        % (r["coloring_rc"], head[:200])])
        continue

    pitems = {i["symbol"]: i for i in prog.get("items", [])}
    s = dict(items=0, green=0, yellow=0, red=0, false_green=0, false_red=0)
    for st in STATUSES:
        s[st] = 0
    for sym in sorted(set(citems) | set(pitems)):
        c = citems.get(sym)
        p = pitems.get(sym)
        if c and p:
            kind = c["kind"] or p.get("kind", "")
            joined.append([corpus, name, sym, kind, c["color"], c["reason"],
                           p["status"], p.get("root_blocker", "")])
            s["items"] += 1
            s[c["color"]] = s.get(c["color"], 0) + 1
            s[p["status"]] = s.get(p["status"], 0) + 1
            confusion[c["color"]][p["status"]] += 1
            if c["color"] in ("green", "yellow") and p["status"] == "dropped":
                s["false_green"] += 1
            if c["color"] == "red" and p["status"] == "ported":
                s["false_red"] += 1
            if p["status"] == "missing":
                missing_audit.append([corpus, name, sym, kind,
                                      audit(sym, kind, r.get("crate_text", ""))])
        elif c:
            unmatched.append([corpus, name, sym, "coloring-only", c["kind"],
                              c["color"], "", ""])
        else:
            unmatched.append([corpus, name, sym, "progress-only",
                              p.get("kind", ""), "", p["status"],
                              p.get("root_blocker", "")])
    for og in prog.get("off_graph_items", []):
        unmatched.append([corpus, name, og["symbol"], "off-graph",
                          og.get("kind", ""), og.get("color", ""),
                          og.get("status", ""), og.get("root_blocker", "")])

    summary.append([corpus, name, s["items"], s["green"], s["yellow"], s["red"],
                    s["ported"], s["stubbed"], s["dropped"], s["missing"],
                    s["declared"], s["false_green"], s["false_red"]])

    # ---------------- E5 ----------------
    for b in prog.get("blockers", []):
        e5_direct.append([corpus, name, b["tag"], b["count"]])
    for b in prog.get("root_blockers", []):
        e5_root.append([corpus, name, b["tag"], b["count"]])
    rejected = [i for i in (prog.get("items", [])
                            + prog.get("off_graph_items", []))
                if i.get("blocker")]
    roots = set()
    for i in rejected:
        chain = i.get("blame_chain") or []
        roots.add(chain[-1] if chain else i["symbol"])
    e5_comp.append([corpus, name, len(rejected), len(prog.get("blockers", [])),
                    len(prog.get("root_blockers", [])), len(roots)])


def write(path, header, data):
    with (OUT / path).open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        w.writerows(data)


write("e1_coloring_vs_outcome.csv",
      ["corpus", "project", "symbol", "kind", "predicted_color",
       "predicted_reason", "actual_status", "root_blocker"], joined)
write("e1_summary.csv",
      ["corpus", "project", "items", "green", "yellow", "red", "ported",
       "stubbed", "dropped", "missing", "declared", "false_green",
       "false_red"], summary)
write("e1_skipped.csv", ["corpus", "project", "reason"], skipped)
write("e1_unmatched.csv",
      ["corpus", "project", "symbol", "side", "kind", "predicted_color",
       "actual_status", "root_blocker"], unmatched)
write("e1_missing_audit.csv",
      ["corpus", "project", "symbol", "kind", "audit"], missing_audit)
write("e1_confusion.csv",
      ["predicted_color"] + STATUSES,
      [[c] + [confusion[c][s] for s in STATUSES]
       for c in ("green", "yellow", "red")])
write("e5_direct.csv", ["corpus", "project", "tag", "count"], e5_direct)
write("e5_root.csv", ["corpus", "project", "tag", "count"], e5_root)
write("e5_compression.csv",
      ["corpus", "project", "rejected_items", "distinct_direct_tags",
       "distinct_root_tags", "root_items"], e5_comp)

# ---------------- console summary ----------------
agg = defaultdict(lambda: defaultdict(int))
for row in summary:
    a = agg[row[0]]
    a["projects"] += 1
    for i, k in enumerate(["items", "green", "yellow", "red", "ported",
                           "stubbed", "dropped", "missing", "declared",
                           "false_green", "false_red"]):
        a[k] += row[2 + i]
hdr = ("corpus", "projects", "items", "green", "yellow", "red", "ported",
       "stubbed", "dropped", "missing", "declared", "FG", "FR")
print(("%-14s" + "%9s" * 12) % hdr)
tot = defaultdict(int)
for k in sorted(agg, key=lambda x: ORDER.get(x, 9)):
    a = agg[k]
    vals = [a[c] for c in ("projects", "items", "green", "yellow", "red",
                           "ported", "stubbed", "dropped", "missing",
                           "declared", "false_green", "false_red")]
    for c, v in zip(("projects", "items", "green", "yellow", "red", "ported",
                     "stubbed", "dropped", "missing", "declared",
                     "false_green", "false_red"), vals):
        tot[c] += v
    print(("%-14s" + "%9d" * 12) % tuple([k] + vals))
print(("%-14s" + "%9d" * 12) % tuple(
    ["TOTAL"] + [tot[c] for c in ("projects", "items", "green", "yellow",
                                  "red", "ported", "stubbed", "dropped",
                                  "missing", "declared", "false_green",
                                  "false_red")]))
print("skipped=%d  unmatched=%d (off-graph=%d)"
      % (len(skipped), len(unmatched),
         sum(1 for u in unmatched if u[3] == "off-graph")))
print()
e5agg = defaultdict(lambda: defaultdict(int))
for c, n, rej, dt, rt, ri in e5_comp:
    a = e5agg[c]
    a["projects"] += 1
    a["with_rejects"] += int(rej > 0)
    a["rejected"] += rej
    a["root_items"] += ri
print("%-14s %9s %13s %9s %11s %7s"
      % ("corpus", "projects", "with_rejects", "rejected", "root_items",
         "ratio"))
T = defaultdict(int)
for k in sorted(e5agg, key=lambda x: ORDER.get(x, 9)):
    a = e5agg[k]
    for c in a:
        T[c] += a[c]
    ratio = (a["rejected"] / a["root_items"]) if a["root_items"] else float("nan")
    print("%-14s %9d %13d %9d %11d %7.2f"
          % (k, a["projects"], a["with_rejects"], a["rejected"],
             a["root_items"], ratio))
ratio = (T["rejected"] / T["root_items"]) if T["root_items"] else float("nan")
print("%-14s %9d %13d %9d %11d %7.2f"
      % ("TOTAL", T["projects"], T["with_rejects"], T["rejected"],
         T["root_items"], ratio))
