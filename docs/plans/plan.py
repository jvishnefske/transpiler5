#!/usr/bin/env python3
"""Query tool over docs/plans/backlog.toml, the machine-readable index of
design.md's open items.

design.md stays the authoritative evidence ledger (prose entries, spike
verdicts, measured evidence). This tool exists so an agent or a human can
answer "what next?" in ~10 lines of output instead of reading 12k lines,
and so the index cannot silently drift from the ledger:

  plan.py next [-n N]     the top N unresolved items whose deps have landed
  plan.py show ID         one item's full index record
  plan.py brief ID        extract the item's PROSE entry from design.md
  plan.py json            full index as JSON (for jq)
  plan.py check           validate index integrity + sync with design.md
  plan.py stats           counts by status/kind

`check` is wired into the fast lit tier (test/Driver/backlog-check.c), so
the pre-commit gate fails when the two files drift -- e.g. a design.md
entry deleted by a bad edit, or a box checked without a status update.
"""

import argparse
import json
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OPEN_STATUSES = {"open", "spiked", "deferred"}
ALL_STATUSES = OPEN_STATUSES | {"landed", "no-go"}


def load(repo: Path):
    with open(repo / "docs/plans/backlog.toml", "rb") as f:
        return tomllib.load(f)["items"]


def by_id(items):
    return {it["id"]: it for it in items}


def unblocked(item, idx):
    return all(
        idx.get(dep, {}).get("status") in ("landed", "no-go")
        for dep in item.get("blocked_by", ())
    )


def cmd_next(items, args):
    idx = by_id(items)
    ready = [
        it
        for it in items
        if it["status"] in ("open", "spiked") and unblocked(it, idx)
    ]
    ready.sort(key=lambda it: it.get("rank", 10**6))
    for it in ready[: args.n]:
        spike = "spiked" if it["status"] == "spiked" else "needs-spike"
        print(
            f"{it['id']:<10} r{it.get('rank', '?'):<3} "
            f"[{it.get('kind', '?')}/{it.get('size', '?')}/{spike}] {it['title']}"
        )
        if args.verbose and it.get("summary"):
            print(f"           {it['summary']}")
    if not ready:
        print("backlog: nothing unblocked", file=sys.stderr)
        return 1
    return 0


def cmd_show(items, args):
    it = by_id(items).get(args.id)
    if not it:
        print(f"no such item: {args.id}", file=sys.stderr)
        return 1
    print(json.dumps(it, indent=2))
    return 0


def entry_lines(design: str, item_id: str):
    """Yield the design.md prose entry for item_id: from its checkbox (or
    heading) line to the next same-level checkbox or heading."""
    lines = design.split("\n")
    esc = re.escape(item_id)
    start_re = re.compile(rf"^- \[[ x]\] {esc}\b")
    head_re = re.compile(rf"^#+ .*\b{esc}\b|^### .*{esc}")
    end_re = re.compile(r"^- \[[ x]\] |^#{1,3} ")
    start = None
    for i, line in enumerate(lines):
        if start_re.match(line) or head_re.match(line):
            start = i
            break
    if start is None:
        return None
    out = [lines[start]]
    for line in lines[start + 1 :]:
        if end_re.match(line):
            break
        out.append(line)
    return out


def cmd_brief(items, args):
    it = by_id(items).get(args.id)
    if not it:
        print(f"no such item: {args.id}", file=sys.stderr)
        return 1
    design = (REPO / "design.md").read_text()
    entry = entry_lines(design, args.id)
    if entry is None:
        # Fall back to the anchor (items like W2.19a share a parent entry).
        anchor = it.get("anchor", args.id)
        pos = design.find(anchor)
        if pos < 0:
            print(f"anchor not found in design.md: {anchor}", file=sys.stderr)
            return 1
        line_no = design.count("\n", 0, pos)
        entry = entry_lines(design, anchor.split()[0]) or [
            f"(anchor '{anchor}' at design.md:{line_no + 1}; no checkbox entry)"
        ]
    print("\n".join(entry).rstrip())
    return 0


def cmd_json(items, _args):
    idx = by_id(items)
    for it in items:
        it["unblocked"] = unblocked(it, idx)
    print(json.dumps(items, indent=1))
    return 0


def cmd_stats(items, _args):
    from collections import Counter

    print("by status:", dict(Counter(it["status"] for it in items)))
    print(
        "by kind:  ",
        dict(Counter(it.get("kind", "?") for it in items if it["status"] in OPEN_STATUSES)),
    )
    return 0


def cmd_check(items, _args):
    errs, warns = [], []
    idx = by_id(items)
    if len(idx) != len(items):
        seen, dupes = set(), set()
        for it in items:
            (dupes if it["id"] in seen else seen).add(it["id"])
        errs.append(f"duplicate ids: {sorted(dupes)}")

    ranks = {}
    for it in items:
        if it["status"] not in ALL_STATUSES:
            errs.append(f"{it['id']}: bad status {it['status']!r}")
        for dep in it.get("blocked_by", ()):
            if dep not in idx:
                errs.append(f"{it['id']}: blocked_by unknown id {dep!r}")
        if it["status"] in OPEN_STATUSES:
            r = it.get("rank")
            if r is None:
                errs.append(f"{it['id']}: open item with no rank")
            elif r in ranks:
                warns.append(f"rank {r} shared by {ranks[r]} and {it['id']}")
            else:
                ranks[r] = it["id"]

    # Cycle detection over blocked_by.
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {i: WHITE for i in idx}

    def visit(node, stack):
        color[node] = GRAY
        for dep in idx[node].get("blocked_by", ()):
            if dep not in idx:
                continue
            if color[dep] == GRAY:
                errs.append(f"dependency cycle: {' -> '.join(stack + [dep])}")
            elif color[dep] == WHITE:
                visit(dep, stack + [dep])
        color[node] = BLACK

    for node in idx:
        if color[node] == WHITE:
            visit(node, [node])

    design = (REPO / "design.md").read_text()

    # Every anchor must exist in design.md. This is the tripwire that
    # catches an entry deleted from the ledger by a careless range edit.
    for it in items:
        anchor = it.get("anchor")
        if not anchor:
            errs.append(f"{it['id']}: no anchor")
        elif anchor not in design:
            errs.append(f"{it['id']}: anchor not found in design.md: {anchor!r}")

    # Sync, both directions, over FR-* ids (waves share prose entries so
    # only their parent is box-checked; FR boxes map 1:1).
    open_boxes = set(re.findall(r"^- \[ \] (FR-\d+)\b", design, re.M))
    done_boxes = set(re.findall(r"^- \[x\] (FR-\d+)\b", design, re.M))
    indexed_open = {
        it["id"] for it in items if it["id"].startswith("FR-") and it["status"] in OPEN_STATUSES
    }
    indexed_landed = {
        it["id"] for it in items if it["id"].startswith("FR-") and it["status"] == "landed"
    }
    for fr in sorted(open_boxes - indexed_open):
        # Pre-index FRs (< FR-82) are grandfathered; everything the index
        # era touches must be present.
        if int(fr.split("-")[1]) >= 82:
            errs.append(f"design.md has open box {fr} but backlog.toml has no open entry")
    for fr in sorted(indexed_open & done_boxes):
        errs.append(f"backlog.toml says {fr} is open but design.md's box is checked")
    for fr in sorted(indexed_landed - done_boxes):
        errs.append(f"backlog.toml says {fr} landed but design.md has no checked box")

    for w in warns:
        print(f"warning: {w}", file=sys.stderr)
    for e in errs:
        print(f"error: {e}", file=sys.stderr)
    if not errs:
        n_open = sum(1 for it in items if it["status"] in OPEN_STATUSES)
        print(f"backlog check OK: {len(items)} items, {n_open} open, in sync with design.md")
    return 1 if errs else 0


def main():
    global REPO
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=REPO, help="repo root")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("next")
    p.add_argument("-n", type=int, default=3)
    p.add_argument("-v", "--verbose", action="store_true")
    sub.add_parser("show").add_argument("id")
    sub.add_parser("brief").add_argument("id")
    sub.add_parser("json")
    sub.add_parser("check")
    sub.add_parser("stats")
    args = ap.parse_args()
    REPO = args.repo
    items = load(REPO)
    return {
        "next": cmd_next,
        "show": cmd_show,
        "brief": cmd_brief,
        "json": cmd_json,
        "check": cmd_check,
        "stats": cmd_stats,
    }[args.cmd](items, args)


if __name__ == "__main__":
    sys.exit(main())
