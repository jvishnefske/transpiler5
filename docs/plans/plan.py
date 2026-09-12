#!/usr/bin/env python3
"""Query tool over docs/plans/backlog.toml, the machine-readable index of
design.md's open items.

design.md stays the authoritative evidence ledger (prose entries, spike
verdicts, measured evidence). This tool exists so an agent or a human can
answer "what next?" in ~10 lines of output instead of reading 12k lines,
and so the index cannot silently drift from the ledger:

  plan.py next [-n N]     the top N unresolved items whose deps have landed
  plan.py show ID         one item's full index record
  plan.py brief ID        extract the item's PROSE entry from the ledger
  plan.py json            full index as JSON (for jq)
  plan.py check           validate index integrity + sync with the ledger
  plan.py stats           counts by status/kind
  plan.py join            print the joined ledger to stdout (grep archaeology,
                          merge replay via scripts/split-ledger.py)

The LEDGER is design.md plus, once docs/plans/ledger.manifest exists, the
ordered member files it lists (docs/plans/entries/*.md per-entry files and
docs/design/*.md chapter files). The manifest is an ORDERED PARTITION of
the original design.md byte stream: joining the members reproduces it
byte-for-byte, so every regex semantic below (entry delimiters, sync sets,
first-match on duplicate ids) is unchanged by the split (FR-242). Without
a manifest this tool reads plain design.md, so both layouts work.

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
OPEN_STATUSES = {"open", "spiked", "deferred", "in-flight"}
ALL_STATUSES = OPEN_STATUSES | {"landed", "no-go"}

MANIFEST_REL = "docs/plans/ledger.manifest"
ENTRIES_REL = "docs/plans/entries"
CHAPTERS_REL = "docs/design"

# Ledger segmentation delimiters. entry_lines() and scripts/split-ledger.py
# must agree on these, so they live here and the splitter imports them.
BOX_RE = re.compile(r"^- \[[ x]\] ")
DELIM_RE = re.compile(r"^- \[[ x]\] |^#{1,3} ")
# A box line that opens a per-entry ledger item (vs. the C99-*/CTS-* checklist
# boxes that stay inside chapter files).
ENTRY_ID_RE = re.compile(r"^- \[[ x]\] (FR-\d+[a-z]*|W2\.\d+|T-SYSTEMD)\b")


def manifest_members(repo: Path):
    """Repo-relative member paths from the manifest, or None if no manifest
    (pre-split layout: the ledger is plain design.md)."""
    mf = repo / MANIFEST_REL
    if not mf.exists():
        return None
    out = []
    for raw in mf.read_text().splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


def ledger_files(repo: Path):
    """Ordered absolute paths of the ledger's member files."""
    members = manifest_members(repo)
    if members is None:
        return [repo / "design.md"]
    return [repo / m for m in members]


def ledger_text(repo: Path) -> str:
    """The joined ledger. With a manifest this is byte-identical to the
    pre-split design.md (the lossless-partition invariant that cmd_check's
    manifest checks and the splitter's sha256 assertion protect)."""
    return "".join(p.read_text() for p in ledger_files(repo))


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
        # in-flight = claimed by a live session's wave/spike; skip so a
        # second session pulling from `next` cannot double-pick it.
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
    """Yield the ledger prose entry for item_id: from its checkbox (or
    heading) line to the next same-level checkbox or heading."""
    lines = design.split("\n")
    esc = re.escape(item_id)
    start_re = re.compile(rf"^- \[[ x]\] {esc}\b")
    head_re = re.compile(rf"^#+ .*\b{esc}\b|^### .*{esc}")
    end_re = DELIM_RE
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
    design = ledger_text(REPO)
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


def check_manifest(items):
    """Manifest-mode structural checks (FR-242). Inert (returns []) until
    docs/plans/ledger.manifest exists; strictly STRONGER than the whole-file
    anchor check once it does: every anchor must resolve inside its OWN
    entry file (or the explicit anchor_file for items that share a parent
    entry), so a clobbered neighbouring file can no longer satisfy it."""
    members = manifest_members(REPO)
    if members is None:
        return []
    errs = []

    # Manifest hygiene: members exist, no duplicates, design.md is #1,
    # and every entry/chapter .md on disk is listed (no orphans).
    seen = set()
    for m in members:
        if m in seen:
            errs.append(f"manifest: duplicate member {m}")
        seen.add(m)
        if not (REPO / m).is_file():
            errs.append(f"manifest: missing member file {m}")
    if not members or members[0] != "design.md":
        errs.append("manifest: design.md must be member #1")
    for rel_dir in (ENTRIES_REL, CHAPTERS_REL):
        d = REPO / rel_dir
        if not d.is_dir():
            continue
        for p in sorted(d.glob("*.md")):
            rel = f"{rel_dir}/{p.name}"
            if rel not in seen:
                errs.append(f"manifest: orphan file not listed: {rel}")

    # Per-entry files: the first line must be the box line of the id the
    # filename claims (duplicate staged entries carry a --N suffix), and no
    # entry box line may appear in a NON-entry member (containment).
    for m in members:
        try:
            text = (REPO / m).read_text()
        except OSError:
            continue  # already reported as missing
        if m.startswith(ENTRIES_REL + "/"):
            stem = Path(m).stem
            if stem.startswith("_"):
                continue  # interstitial prose, not an entry
            want = re.sub(r"--\d+$", "", stem)
            first = text.split("\n", 1)[0]
            if not re.match(rf"^- \[[ x]\] {re.escape(want)}\b", first):
                errs.append(f"{m}: first line is not the {want} box: {first!r}")
        else:
            for line in text.split("\n"):
                if ENTRY_ID_RE.match(line):
                    errs.append(f"{m}: entry box line outside {ENTRIES_REL}/: {line[:60]!r}")

    # Per-file anchor resolution.
    for it in items:
        anchor = it.get("anchor")
        if not anchor:
            continue  # reported by the caller
        target = it.get("anchor_file", f"{ENTRIES_REL}/{it['id']}.md")
        tp = REPO / target
        if not tp.is_file():
            errs.append(f"{it['id']}: no entry file {target} (set anchor_file?)")
        elif anchor not in tp.read_text():
            errs.append(f"{it['id']}: anchor not found in {target}: {anchor!r}")
    return errs


def cmd_join(_items, _args):
    sys.stdout.write(ledger_text(REPO))
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

    design = ledger_text(REPO)

    # Every anchor must exist in the ledger. This is the tripwire that
    # catches an entry deleted from the ledger by a careless range edit.
    for it in items:
        anchor = it.get("anchor")
        if not anchor:
            errs.append(f"{it['id']}: no anchor")
        elif anchor not in design:
            errs.append(f"{it['id']}: anchor not found in the ledger: {anchor!r}")

    errs += check_manifest(items)

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
    sub.add_parser("join")
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
        "join": cmd_join,
    }[args.cmd](items, args)


if __name__ == "__main__":
    sys.exit(main())
