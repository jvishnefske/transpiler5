#!/usr/bin/env python3
"""Summarize a shim/sweep report.jsonl into the Track-5 outcome vocabulary.

Reads one JSONL record per TU (see cc-shim.py) and prints a per-corpus tally:
how many translation units the tool TRANSPILED in full versus REJECTED, split
from PARSE_FAIL (a configuration gap -- a header the include set does not
supply -- which is NOT a translation limit and must never be conflated with
one), plus the ranked reject blockers that are the actual demand signal.

Usage: tabulate.py <report.jsonl> [corpus-name]
"""
import collections
import json
import sys

# Diagnostic fragments that mean "the AST could not be built" -- a config/
# include gap, scored as PARSE_FAIL rather than a genuine translation REJECT.
PARSE_MARKERS = (
    "file not found", "unknown type name", "use of undeclared",
    "unknown register name", "expected", "redefinition of",
    "conflicting types", "typedef redefinition",
)


def classify(rec):
    if rec.get("outcome") == "TRANSPILED":
        return "TRANSPILED"
    blocker = (rec.get("blocker") or "").lower()
    if rec.get("real_cc_rc", 0) not in (0, None) or any(
            m in blocker for m in PARSE_MARKERS):
        return "PARSE_FAIL"
    return "REJECT"


def main():
    path = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "corpus"
    rows = [json.loads(line) for line in open(path, encoding="utf-8") if line.strip()]
    tally = collections.Counter(classify(r) for r in rows)
    blockers = collections.Counter(
        (r.get("blocker") or "").split("error:")[-1].strip()[:70]
        for r in rows if classify(r) == "REJECT" and r.get("blocker"))

    total = len(rows)
    print(f"=== {name}: {total} translation units ===")
    for k in ("TRANSPILED", "PARTIAL", "REJECT", "PARSE_FAIL"):
        if tally.get(k):
            pct = 100.0 * tally[k] / total if total else 0.0
            print(f"  {k:11s} {tally[k]:5d}  ({pct:5.1f}%)")
    if tally.get("PARSE_FAIL"):
        print("  NOTE: PARSE_FAIL is a config/include gap, not a translation "
              "limit -- extend this corpus's include set or config header.")
    if blockers:
        print("  top REJECT blockers (the demand signal):")
        for text, count in blockers.most_common(12):
            print(f"    {count:4d}  {text}")


if __name__ == "__main__":
    main()
