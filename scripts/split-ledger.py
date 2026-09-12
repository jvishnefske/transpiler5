#!/usr/bin/env python3
"""Split the monolithic design.md evidence ledger into the manifest layout
(FR-242): a slim design.md preamble, per-entry files under
docs/plans/entries/, chapter files under docs/design/, and
docs/plans/ledger.manifest listing the members in original order.

The split is an ORDERED PARTITION of the byte stream: concatenating the
manifest members reproduces the input byte-for-byte, asserted by sha256
here (and re-checkable any time with `plan.py join | sha256sum`). All
segmentation regexes are IMPORTED from plan.py so the two cannot drift.

This tool is committed permanently as the MERGE-REPLAY tool. To land a
branch that still edits the pre-split monolithic design.md:

    python3 docs/plans/plan.py join > design.md     # restore the monolith
    git apply <their design.md patch>               # or merge/rebase it
    python3 scripts/split-ledger.py --write         # re-split

Default is a dry run (compute, assert, report); --write applies. --write
refuses an input with no FR entries, so running it twice (i.e. against an
already-slimmed design.md) fails loudly instead of destroying the ledger.
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path
from typing import NoReturn

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "docs/plans"))
import plan  # noqa: E402  (segmentation regexes + ledger_files live there)

# Ordered chapter boundary table for the frozen back half. Keyed on EXACT
# heading text + occurrence index because `### Types` occurs twice (the
# dialect-contract one in the preamble, the C99-language one at the
# boundary). A heading not in this table does NOT open a chapter: inside
# the entries region it becomes an interstitial file, in the back half it
# stays inside the current chapter (e.g. `### Quick wins ...`).
CHAPTERS = [
    ("### Types", 2, "c99-language"),
    ("## C99 Feature Coverage (in progress)", 1, "c99-coverage"),
    ("## c-testsuite Remaining-Failure Checklist", 1, "c-testsuite-checklist"),
    ("## C++ input (subset)", 1, "cpp-subset"),
    ("### Roadmap: the C++ completion program (W2.15+)", 1, "cpp-completion-roadmap"),
    ("## Track 5 Third-party validation (external demand signal)", 1, "track5-third-party"),
    ("## Track 4 RealWorld corpus (demand signal)", 1, "track4-realworld"),
    ("## Non-Goals for the MVP", 1, "non-goals"),
    ("## RFC: Actor-model emission — deferred", 1, "rfc-actor-emission"),
    ("## Validation", 1, "validation"),
    ("## Performance baseline", 1, "performance-baseline"),
]


def die(msg) -> NoReturn:
    print(f"split-ledger: FATAL: {msg}", file=sys.stderr)
    raise SystemExit(1)


def slug(text, limit=40):
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return s[:limit].rstrip("-") or "interstitial"


def segment(lines):
    """Split lines into (first_line_no, [lines]) runs opened by a delimiter
    line (a checkbox or a #/##/### heading). Line 1 must be a delimiter."""
    if not plan.DELIM_RE.match(lines[0]):
        die("line 1 is not a heading/checkbox delimiter")
    segs, cur, start = [], [], 1
    for n, line in enumerate(lines, 1):
        if plan.DELIM_RE.match(line) and cur:
            segs.append((start, cur))
            cur, start = [], n
        cur.append(line)
    segs.append((start, cur))
    return segs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=REPO)
    ap.add_argument("--write", action="store_true", help="apply (default: dry run)")
    ap.add_argument(
        "--expect",
        default=None,
        help="comma-separated k=v inventory assertions, e.g. "
        "fr_lines=247,fr_ids=244,w2=28,t=1,headings=44,chapters=11,interstitials=1,preamble_lines=213",
    )
    args = ap.parse_args()
    repo = args.repo

    raw = (repo / "design.md").read_bytes()
    original_sha = hashlib.sha256(raw).hexdigest()
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as e:
        die(f"design.md is not valid UTF-8: {e}")
    if "\r" in text:
        die("design.md contains CR bytes; the splitter assumes LF-only")
    if not text.endswith("\n"):
        die("design.md does not end with a newline")
    lines = text[:-1].split("\n")  # drop the final newline; re-added per file

    # No delimiter-lookalike may sit inside a code fence, or joining would
    # still be lossless but per-entry semantics would silently differ from
    # the monolith's (a fenced "- [x] FR-…" would become a file boundary).
    fenced = False
    for n, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
        elif fenced and plan.DELIM_RE.match(line):
            die(f"line {n}: delimiter-lookalike inside a code fence: {line[:60]!r}")
    if fenced:
        die("unbalanced code fence")

    segs = segment(lines)

    # Walk the segments, assigning each to exactly one output file, in order.
    head_count = {}  # exact heading text -> occurrences seen
    chapter_key = {}  # (text, occurrence) -> chapter filename
    for t, occ, name in CHAPTERS:
        chapter_key[(t, occ)] = name
    outputs = []  # ordered (relpath, [lines])  -- the manifest, in order
    by_path = {}
    inventory = {"fr_lines": 0, "w2": 0, "t": 0, "headings": 0, "chapters": 0, "interstitials": 0}
    fr_ids, entry_names = set(), {}
    sink = "design.md"  # the preamble, until the first entry box
    in_preamble, current_chapter = True, None

    def emit(relpath, seg_lines):
        nonlocal sink
        if relpath in by_path:
            if sink != relpath:
                die(
                    f"{relpath} would receive non-contiguous segments "
                    f"(last sink was {sink}); the ordered-partition invariant breaks"
                )
            by_path[relpath].extend(seg_lines)
        else:
            by_path[relpath] = list(seg_lines)
            outputs.append(relpath)
        sink = relpath

    for _start, seg_lines in segs:
        first = seg_lines[0]
        if plan.DELIM_RE.match(first) and first.startswith("#"):
            inventory["headings"] += 1
            head_count[first] = head_count.get(first, 0) + 1
            chap = chapter_key.get((first, head_count[first]))
            if chap is not None:
                in_preamble, current_chapter = False, chap
                inventory["chapters"] += 1
                emit(f"{plan.CHAPTERS_REL}/{chap}.md", seg_lines)
                continue
        m = plan.ENTRY_ID_RE.match(first)
        if m:
            in_preamble = False
            eid = m.group(1)
            if eid.startswith("FR-"):
                inventory["fr_lines"] += 1
                fr_ids.add(eid)
            elif eid.startswith("W2."):
                inventory["w2"] += 1
            else:
                inventory["t"] += 1
            n_prev = entry_names.get(eid, 0) + 1
            entry_names[eid] = n_prev
            fname = eid if n_prev == 1 else f"{eid}--{n_prev}"
            emit(f"{plan.ENTRIES_REL}/{fname}.md", seg_lines)
        elif in_preamble:
            emit("design.md", seg_lines)
        elif current_chapter is not None:
            emit(f"{plan.CHAPTERS_REL}/{current_chapter}.md", seg_lines)
        else:
            inventory["interstitials"] += 1
            emit(f"{plan.ENTRIES_REL}/_{slug(first.lstrip('# '))}.md", seg_lines)

    inventory["fr_ids"] = len(fr_ids)
    inventory["preamble_lines"] = len(by_path.get("design.md", []))
    if inventory["fr_lines"] == 0:
        die("input has no FR entries -- refusing (already-split design.md?)")

    print("inventory:", " ".join(f"{k}={inventory[k]}" for k in sorted(inventory)))
    print(f"members: {len(outputs)} files")
    if args.expect:
        bad = []
        for kv in args.expect.split(","):
            k, v = kv.split("=")
            if inventory.get(k.strip()) != int(v):
                bad.append(f"{k}: expected {v}, measured {inventory.get(k.strip())}")
        if bad:
            die("inventory mismatch:\n  " + "\n  ".join(bad))

    # Lossless-partition proof, on the in-memory result before touching disk.
    joined = "".join("\n".join(ls) + "\n" for _, ls in ((p, by_path[p]) for p in outputs))
    joined_sha = hashlib.sha256(joined.encode()).hexdigest()
    if joined_sha != original_sha:
        die(f"lossless proof FAILED: joined sha {joined_sha} != original {original_sha}")
    print(f"lossless proof: sha256(join) == sha256(design.md) == {original_sha}")

    # Every backlog anchor must resolve inside its assigned file; items whose
    # id has no own entry file need an explicit anchor_file in backlog.toml.
    items = plan.load(repo)
    suggestions, anchor_errs = [], []
    for it in items:
        anchor = it.get("anchor", "")
        own = f"{plan.ENTRIES_REL}/{it['id']}.md"
        if own in by_path and anchor in "\n".join(by_path[own]) + "\n":
            if it.get("anchor_file"):
                suggestions.append(f"{it['id']}: anchor_file is redundant, own entry resolves")
            continue
        hits = [p for p in outputs if anchor in "\n".join(by_path[p]) + "\n"]
        if not hits:
            anchor_errs.append(f"{it['id']}: anchor resolves in NO single member: {anchor!r}")
        elif it.get("anchor_file") in hits:
            pass
        else:
            suggestions.append(f'{it["id"]}: needs anchor_file = "{hits[0]}"')
    if anchor_errs:
        die("anchor resolution:\n  " + "\n  ".join(anchor_errs))
    for s in suggestions:
        print("backlog:", s)

    if not args.write:
        print("dry run only; re-run with --write to apply")
        return 0

    entries_dir = repo / plan.ENTRIES_REL
    chapters_dir = repo / plan.CHAPTERS_REL
    entries_dir.mkdir(parents=True, exist_ok=True)
    chapters_dir.mkdir(parents=True, exist_ok=True)
    new_set = set(outputs)
    for d, rel in ((entries_dir, plan.ENTRIES_REL), (chapters_dir, plan.CHAPTERS_REL)):
        for p in sorted(d.glob("*.md")):
            if f"{rel}/{p.name}" not in new_set:
                print(f"removing stale member {rel}/{p.name}")
                p.unlink()
    for relpath in outputs:
        (repo / relpath).write_text("\n".join(by_path[relpath]) + "\n")
    manifest = [
        "# Ordered members of the evidence ledger (FR-242). plan.py joins these",
        "# byte-for-byte into the pre-split design.md stream; verify any time with",
        "#   python3 docs/plans/plan.py join | sha256sum",
        "# Regenerated by scripts/split-ledger.py -- edit entries, not this order.",
    ] + list(outputs)
    (repo / plan.MANIFEST_REL).write_text("\n".join(manifest) + "\n")

    # Re-verify from disk through plan.py's OWN reader.
    disk_sha = hashlib.sha256(plan.ledger_text(repo).encode()).hexdigest()
    if disk_sha != original_sha:
        die(f"post-write verification FAILED: {disk_sha} != {original_sha}")
    print(f"wrote {len(outputs)} members + manifest; disk join verified against {original_sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
