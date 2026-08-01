#!/usr/bin/env python3
"""Aggregate the raw per-commit JSON into the E4 CSVs."""
import csv
import json
import os

D = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(D, "raw")

# order, sha, subject, date, mode_used_for_series_A
COMMITS = [
    (1, "54fe0fe", "test(EndToEnd): widen C++ differential net with 3 in-subset byte-match tests", "2026-07-27", "plain"),
    (2, "c3fbb13", "W5.0: FR-46 C++ RealWorld demand corpus (4 projects) + runner C++ mode", "2026-07-28", "plain"),
    (3, "1f1dc8f", "W5.7: FR-47 implicit-`this` method-call receiver", "2026-07-28", "plain"),
    (4, "385efc3", "W5.4: FR-44 incremental crate output and progress ratchet", "2026-07-28", "incremental"),
    (5, "1a37f6c", "W5.3: FR-43 frontier tree search over admitted item sets", "2026-07-28", "incremental"),
    (6, "c665b20", "W5.9: FR-48 C++ reference types (rank-1 corpus blocker)", "2026-07-29", "incremental"),
    (7, "bc0be94", "W5.10: FR-50 --search never loses to --incremental, and a real miscompile fix", "2026-07-29", "incremental"),
    (8, "4c89af1", "W5.11: FR-51 library crates -- distinguish Rust lib from bin", "2026-07-29", "incremental"),
]

MODE_LABEL = {"plain": "--emit=crate",
              "incremental": "--emit=crate --incremental"}


def load(order, sha, mode):
    p = os.path.join(RAW, "e4_%d_%s_%s.json" % (order, sha, mode))
    with open(p, encoding="utf-8") as h:
        return json.load(h)


def emit(rows_path, totals_path, mode_of):
    rows, totals = [], []
    for order, sha, subject, date, default_mode in COMMITS:
        mode = mode_of(order, default_mode)
        recs = load(order, sha, mode)
        acc = {}
        for key in ("c", "cpp", "all"):
            acc[key] = {"projects": 0, "emitted": 0, "building": 0,
                        "gi": 0, "p": 0, "scored": False}
        for r in recs:
            gi = r["graph_items"]
            po = r["ported"]
            rows.append({
                "order": order, "sha": sha, "subject": subject, "date": date,
                "corpus": r["corpus"], "project": r["project"],
                "mode_used": MODE_LABEL[mode],
                "crate_emitted": r["crate_emitted"],
                "crate_builds": r["crate_builds"],
                "graph_items": "" if gi is None else gi,
                "ported": "" if po is None else po,
            })
            for key in (r["corpus"], "all"):
                a = acc[key]
                a["projects"] += 1
                a["emitted"] += r["crate_emitted"]
                a["building"] += r["crate_builds"]
                if gi is not None:
                    a["scored"] = True
                    a["gi"] += gi
                    a["p"] += po
        for key in ("c", "cpp", "all"):
            a = acc[key]
            totals.append({
                "order": order, "sha": sha, "subject": subject, "date": date,
                "corpus": key, "projects": a["projects"],
                "crates_emitted": a["emitted"],
                "crates_building": a["building"],
                "graph_items_total": a["gi"] if a["scored"] else "",
                "ported_total": a["p"] if a["scored"] else "",
            })
    with open(rows_path, "w", newline="", encoding="utf-8") as h:
        w = csv.DictWriter(h, fieldnames=[
            "order", "sha", "subject", "date", "corpus", "project",
            "mode_used", "crate_emitted", "crate_builds", "graph_items",
            "ported"])
        w.writeheader()
        w.writerows(rows)
    with open(totals_path, "w", newline="", encoding="utf-8") as h:
        w = csv.DictWriter(h, fieldnames=[
            "order", "sha", "subject", "date", "corpus", "projects",
            "crates_emitted", "crates_building", "graph_items_total",
            "ported_total"])
        w.writeheader()
        w.writerows(totals)
    return rows, totals


# Primary series: best mode each commit supports.
emit(os.path.join(D, "e4_longitudinal.csv"),
     os.path.join(D, "e4_totals.csv"),
     lambda order, default: default)

# Supplementary control series: plain --emit=crate at EVERY commit, so the
# Series-A staircase can be read without the mode switch at order 4.
emit(os.path.join(D, "e4_longitudinal_plainmode.csv"),
     os.path.join(D, "e4_totals_plainmode.csv"),
     lambda order, default: "plain")

print("ok")
