#!/usr/bin/env python3
"""FR-241: the rejection ledger's NEEDLE-LIVENESS check.

The blocker-tag heuristic exists twice -- `lib/ImportC/RejectionLedger.cpp`
(in-process, under recovering import) and `classify_blocker` in
`test/RealWorld/run_realworld.py` (a survey over subprocess stderr) -- and
both are hand-written substring tables keyed on DIAGNOSTIC WORDING. Nothing
compiled ties a needle to the `emitError` that raises it, so the two failure
modes are silent and were both measured live by FR-241's mechanical audit:

  * a needle ROTS -- the rejection it keyed on was admitted or reworded, the
    row outlived it, and a census reader is told a frontier still exists
    when nothing can raise it (two found: `written with a null pointer`,
    `explicit class template specialization`);
  * the two tables DRIFT -- a row lands in one mirror and not the other, so
    the survey reports `other` for a construct the in-process ledger tags
    correctly (two found: `has no bit-exact Rust mapping`, live since
    FR-224; `was not reached: sibling specialization`).

The root cause FR-241 measured: 54 of 89 tags are asserted NOWHERE in
`test/` except inside the Python mirror itself.

This check asserts three things, and DELIBERATELY does not assert a fourth.

  (a) LIVENESS. Every needle in `kBlockerSubstrings`, `kCxxBlockerSubstrings`
      and `kNodeNamedPrefixes` must be a substring of some string literal in
      `lib/ImportC` outside the ledger itself, with adjacent literals merged
      (the wordings are line-wrapped) and comments stripped (a comment
      QUOTING a retired wording must not keep its needle alive). A needle
      assembled around an interpolation is invisible to a literal scan and
      needs an explicit `LIVE_VIA` waiver below; the waiver is itself
      checked, so it fails when it becomes unnecessary.

  (b) MIRROR EQUALITY. The three tables must be row-for-row identical
      between the two files: same needles, same tags, same order. Pure
      equality, no exemptions -- the rows that legitimately live in only one
      mirror (`search-excluded`, `cxx-cascaded-method`,
      `owner-method-not-reached`) sit OUTSIDE these tables by construction,
      which is what makes the assertion free.

  (c) NO SHADOWING, in two forms. First-match-wins is the tables'
      mechanism, so an earlier row can silently steal a later row's
      traffic, and a wrong tag is WORSE than `other` -- `other` at least
      advertises that it knows nothing. Two sound (no-false-failure)
      structural rules:
        (c1) UNREACHABLE ROW BY NESTING: needle A is a substring of a later
             needle B with a different tag, so nothing can ever reach B.
        (c2) UNREACHABLE ROW BY WORDING: some row never WINS a single
             enumerated literal, even though (a) proved it occurs in one --
             i.e. every wording it exists for is claimed by an earlier row.
             This is how FR-241's SEVENTH defect surfaced, the one the
             audit's list of six did not contain:
             `stl-unrecognized-receiver` sat last, every wording it was
             minted for ends "... receiver is not a recognized STL type",
             and the generic type row above it took all five.
      (c2) is sound because a message always CONTAINS its literal, so a row
      that loses at literal level loses at message level too; it is
      incomplete in the other direction (a row winning a bare literal can
      still lose once the interpolated message is assembled), which is the
      right direction for a gate.

      NEITHER RULE CATCHES FR-241's DEFECT 6, and that is a real limit
      worth stating rather than papering over. Defect 6 -- the widened
      `global address` row filing ImportCTypes.cpp's returned-pointer
      refusals under the FR-62 cursor-parameter tag -- is MESSAGE-level
      ambiguity between two rows that BOTH still win other wordings, so no
      row is dead and no needle nests. Gating it would need a pinned list
      of the "deliberate" ambiguous orderings, which is the allowlist this
      check exists to avoid (the table is BUILT on deliberate ordering:
      argv/argv-pointer, the ostream operand family, the exception
      uncaught/closure pair). So the ambiguity inventory is PRINTED with
      its winner named -- 12 entries today, all deliberate, and defect 6's
      two rows now read `returned-pointer`, which is the correct answer.

NOT ASSERTED: anything about the `other` bucket. An allowlist was considered
and rejected -- 607 of 802 assembled messages classify `other` and MOST DO SO
LEGITIMATELY (internal consistency checks with zero corpus hits), so a gate
would need a line per new diagnostic and review pressure would push people to
add the line rather than think. Instead the wordings that classify `other`
AND appear in a real case's blocker set in the newest `census-export-*.json`
are PRINTED as a report. Empty most of the time; never a failure.
"""

import argparse
import glob
import json
import os
import re
import sys

REPO_DEFAULT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEDGER_CPP = "lib/ImportC/RejectionLedger.cpp"
MIRROR_PY = "test/RealWorld/run_realworld.py"
SCAN_DIR = "lib/ImportC"

# Needles that CANNOT appear as a literal because the wording is assembled
# around an interpolation. Each maps to the file:line list that builds it;
# the citation is verified (the file must exist and the line must still
# mention the interpolating call), and an unnecessary waiver is an error, so
# a waived needle that later becomes literal gets flagged rather than
# forgotten.
LIVE_VIA = {
    # LIVE-VIA: lib/ImportC/ImportCExpressions.cpp:540,557,613,720,976 --
    # `"unsupported cast (" << cast->getCastKindName() << ")"`, so the
    # UserDefinedConversion spelling only ever exists at runtime.
    "unsupported cast (UserDefinedConversion)": [
        ("lib/ImportC/ImportCExpressions.cpp", 540, "getCastKindName"),
        ("lib/ImportC/ImportCExpressions.cpp", 557, "getCastKindName"),
        ("lib/ImportC/ImportCExpressions.cpp", 613, "getCastKindName"),
        ("lib/ImportC/ImportCExpressions.cpp", 720, "getCastKindName"),
        ("lib/ImportC/ImportCExpressions.cpp", 976, "getCastKindName"),
    ],
}


# ---------------------------------------------------------------------------
# A minimal C++ lexer: enough to strip comments and recover string literals
# with adjacent-literal concatenation applied.
# ---------------------------------------------------------------------------

_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\",
    '"': '"', "'": "'", "a": "\a", "b": "\b", "f": "\f", "v": "\v",
    "?": "?",
}


def _decode(body):
    out = []
    index = 0
    while index < len(body):
        char = body[index]
        if char != "\\":
            out.append(char)
            index += 1
            continue
        index += 1
        if index >= len(body):
            break
        out.append(_ESCAPES.get(body[index], body[index]))
        index += 1
    return "".join(out)


def merged_string_literals(text):
    """Every string literal in `text`, comments stripped and runs of
    adjacent literals concatenated as the compiler concatenates them."""
    literals = []
    pending = None
    index = 0
    size = len(text)
    while index < size:
        char = text[index]
        if char == "/" and index + 1 < size and text[index + 1] == "/":
            end = text.find("\n", index)
            index = size if end < 0 else end
            continue
        if char == "/" and index + 1 < size and text[index + 1] == "*":
            end = text.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue
        if char == "'":
            index += 1
            while index < size and text[index] != "'":
                index += 2 if text[index] == "\\" else 1
            index += 1
            continue
        if char == '"':
            start = index + 1
            index = start
            while index < size and text[index] != '"':
                index += 2 if text[index] == "\\" else 1
            body = text[start:index]
            index += 1
            decoded = _decode(body)
            # Adjacent literals concatenate when only whitespace (and
            # comments, already consumed) separates them.
            probe = index
            while probe < size and text[probe] in " \t\r\n":
                probe += 1
            if probe < size and text[probe] == '"':
                pending = decoded if pending is None else pending + decoded
            else:
                literals.append(decoded if pending is None else pending + decoded)
                pending = None
            continue
        index += 1
    if pending is not None:
        literals.append(pending)
    return literals


def strip_comments(text):
    """`text` with // and /* */ comments blanked, string/char literals kept."""
    out = []
    index = 0
    size = len(text)
    while index < size:
        char = text[index]
        if char == "/" and index + 1 < size and text[index + 1] == "/":
            end = text.find("\n", index)
            index = size if end < 0 else end
            continue
        if char == "/" and index + 1 < size and text[index + 1] == "*":
            end = text.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue
        if char in "'\"":
            quote = char
            start = index
            index += 1
            while index < size and text[index] != quote:
                index += 2 if text[index] == "\\" else 1
            index += 1
            out.append(text[start:index])
            continue
        out.append(char)
        index += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# The C++ tables.
# ---------------------------------------------------------------------------

_LITERAL_RUN = re.compile(r'llvm::StringLiteral\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)')
_ONE_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def cpp_table(source, name):
    """The ordered (needle, tag) rows of `constexpr ... name[] = {...};`."""
    marker = name + "[] = {"
    start = source.find(marker)
    if start < 0:
        raise SystemExit("check-rejection-ledger: no table %s in %s" % (name, LEDGER_CPP))
    start += len(marker)
    end = source.find("\n};", start)
    if end < 0:
        raise SystemExit("check-rejection-ledger: unterminated table %s" % name)
    body = strip_comments(source[start:end])
    flat = []
    for run in _LITERAL_RUN.finditer(body):
        flat.append("".join(_decode(piece) for piece in _ONE_LITERAL.findall(run.group(1))))
    if len(flat) % 2:
        raise SystemExit("check-rejection-ledger: odd literal count in %s" % name)
    return [(flat[i], flat[i + 1]) for i in range(0, len(flat), 2)]


# ---------------------------------------------------------------------------
# Checks.
# ---------------------------------------------------------------------------


def literal_corpus(repo):
    corpus = []
    for root, _dirs, names in os.walk(os.path.join(repo, SCAN_DIR)):
        for name in sorted(names):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(root, name)
            if os.path.relpath(path, repo) == LEDGER_CPP:
                continue  # The tables themselves must not vouch for a needle.
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                corpus.extend(merged_string_literals(handle.read()))
    return corpus


def check_liveness(repo, rows, corpus, failures):
    """Checks (a) and (c2): every needle occurs in a real diagnostic literal
    (or carries an audited LIVE_VIA waiver), and every row actually WINS at
    least one of the literals it occurs in."""

    def literal_live(needle):
        return any(needle in literal for literal in corpus)

    # (c2): the row index that actually WINS each enumerated literal.
    winners = set()
    for literal in corpus:
        for index, (needle, _tag) in enumerate(rows):
            if needle in literal:
                winners.add(index)
                break

    for needle, tag in rows:
        waiver = LIVE_VIA.get(needle)
        if waiver is None:
            if not literal_live(needle):
                failures.append(
                    "DEAD NEEDLE %r (tag %r): no string literal in %s contains it, "
                    "and it has no LIVE_VIA waiver. Either the rejection it keyed "
                    "on is gone (delete the row from BOTH mirrors) or the wording "
                    "is interpolated (add a LIVE_VIA waiver)."
                    % (needle, tag, SCAN_DIR))
            continue
        if literal_live(needle):
            failures.append(
                "STALE WAIVER %r: a LIVE_VIA waiver claims the wording is "
                "interpolated, but a literal in %s now contains it. Delete the "
                "waiver." % (needle, SCAN_DIR))
            continue
        for path, line_no, anchor in waiver:
            full = os.path.join(repo, path)
            if not os.path.exists(full):
                failures.append("BROKEN WAIVER %r: %s does not exist" % (needle, path))
                continue
            with open(full, "r", encoding="utf-8", errors="replace") as handle:
                lines = handle.readlines()
            if line_no > len(lines) or anchor not in lines[line_no - 1]:
                failures.append(
                    "BROKEN WAIVER %r: %s:%d no longer mentions %r (the "
                    "interpolation the waiver cites moved; re-anchor it)."
                    % (needle, path, line_no, anchor))

    for index, (needle, tag) in enumerate(rows):
        # A needle with no literal at all is already reported by (a); saying
        # it is also unreachable is noise, not a second fact.
        if index in winners or needle in LIVE_VIA or not literal_live(needle):
            continue
        claimants = sorted({rows[other][1] for other in winners
                            for literal in corpus
                            if needle in literal and rows[other][0] in literal
                            and other < index})
        failures.append(
            "UNREACHABLE ROW %r (tag %r): the needle occurs in a wording, but "
            "an EARLIER row claims every wording it occurs in%s. The row can "
            "never produce its tag. Hoist it above the row that shadows it, "
            "or delete it from BOTH mirrors."
            % (needle, tag,
               " (shadowed by " + ", ".join(claimants) + ")" if claimants else ""))


def check_mirror(cpp_tables, py_tables, failures):
    for name, cpp_rows in cpp_tables.items():
        py_rows = py_tables[name]
        if cpp_rows == py_rows:
            continue
        failures.append(
            "MIRROR DRIFT in %s: %s has %d rows, %s has %d"
            % (name, LEDGER_CPP, len(cpp_rows), MIRROR_PY, len(py_rows)))
        # Report the SET difference first -- a single missing row shifts
        # every later index, and a positional dump of that is 90 lines of
        # noise around one fact.
        only_cpp = [row for row in cpp_rows if row not in py_rows]
        only_py = [row for row in py_rows if row not in cpp_rows]
        for row in only_cpp:
            failures.append("  only in %s: %r" % (LEDGER_CPP, row))
        for row in only_py:
            failures.append("  only in %s: %r" % (MIRROR_PY, row))
        if not only_cpp and not only_py:
            for index, (left, right) in enumerate(zip(cpp_rows, py_rows)):
                if left != right:
                    failures.append(
                        "  same rows, ORDER differs from row %d: cpp=%r py=%r"
                        % (index, left, right))
                    break


def check_shadowing(cpp_tables, failures):
    """Check (c1): a needle nested inside a LATER needle with a different tag
    makes that later row unreachable no matter what any wording says."""
    # classify_blocker consults the tables in this order, and first match
    # wins ACROSS them, so the shadowing question spans the concatenation.
    order = (cpp_tables["kBlockerSubstrings"]
             + cpp_tables["kCxxBlockerSubstrings"]
             + cpp_tables["kNodeNamedPrefixes"])
    for i, (needle, tag) in enumerate(order):
        for later_needle, later_tag in order[i + 1:]:
            if tag == later_tag:
                continue
            if needle in later_needle:
                failures.append(
                    "SHADOWED ROW: needle %r (tag %r) is a substring of the "
                    "later needle %r (tag %r), so nothing can ever reach the "
                    "later row. Reorder or narrow."
                    % (needle, tag, later_needle, later_tag))


def report_ambiguity(rows, corpus):
    """A REPORT, never a gate (see (c) in the module docstring): every
    enumerated wording that more than one row with DIFFERENT tags matches,
    with the row first-match-wins actually awards it. Deliberate orderings
    live here; so would a regression of FR-241 defect 6's shape."""
    entries = []
    for literal in sorted(set(corpus)):
        hits = [(index, needle, tag) for index, (needle, tag) in enumerate(rows)
                if needle in literal]
        if len({tag for _, _, tag in hits}) < 2:
            continue
        entries.append((literal, hits))
    print("needle-ambiguity report: %d wording(s) matched by rows with "
          "differing tags" % len(entries))
    for literal, hits in entries:
        print("  %s" % literal[:110])
        for rank, (index, needle, tag) in enumerate(hits):
            print("    %-5s row %-3d %-52s -> %s"
                  % ("WINS" if rank == 0 else "", index, repr(needle), tag))


def report_other(repo, classify):
    """A REPORT, never a gate: wordings from the newest census export that
    appear in a real case's blocker set and still classify `other`."""
    exports = sorted(glob.glob("/home/j/.cache/clippy-tmp/census-export-*.json"),
                     key=os.path.getmtime)
    if not exports:
        print("other-bucket report: no census-export-*.json found; skipped.")
        return
    newest = exports[-1]
    try:
        with open(newest, "r", encoding="utf-8", errors="replace") as handle:
            data = json.load(handle)
    except (OSError, ValueError) as exc:
        print("other-bucket report: %s unreadable (%s); skipped." % (newest, exc))
        return
    counts = {}
    for wordings in (data.get("blocker_sets") or {}).values():
        for wording in wordings:
            if classify(wording) == "other":
                counts[wording] = counts.get(wording, 0) + 1
    print("other-bucket report: %s, %d untagged wording(s) in real blocker sets"
          % (os.path.basename(newest), len(counts)))
    for wording, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print("  %3d case(s)  %s" % (count, wording))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=REPO_DEFAULT)
    args = parser.parse_args()
    repo = os.path.abspath(args.repo)

    with open(os.path.join(repo, LEDGER_CPP), "r", encoding="utf-8") as handle:
        source = handle.read()
    cpp_tables = {
        "kBlockerSubstrings": cpp_table(source, "kBlockerSubstrings"),
        "kCxxBlockerSubstrings": cpp_table(source, "kCxxBlockerSubstrings"),
        "kNodeNamedPrefixes": cpp_table(source, "kNodeNamedPrefixes"),
    }

    sys.path.insert(0, os.path.join(repo, "test", "RealWorld"))
    import run_realworld  # noqa: E402  (path is set up immediately above)

    py_tables = {
        "kBlockerSubstrings": [tuple(row) for row in run_realworld._BLOCKER_SUBSTRINGS],
        "kCxxBlockerSubstrings": [tuple(row) for row in run_realworld._CXX_BLOCKER_SUBSTRINGS],
        "kNodeNamedPrefixes": [tuple(row) for row in run_realworld._NODE_NAMED_PREFIXES],
    }

    failures = []
    all_rows = (cpp_tables["kBlockerSubstrings"]
                + cpp_tables["kCxxBlockerSubstrings"]
                + cpp_tables["kNodeNamedPrefixes"])
    corpus = literal_corpus(repo)
    check_liveness(repo, all_rows, corpus, failures)
    check_mirror(cpp_tables, py_tables, failures)
    check_shadowing(cpp_tables, failures)

    print("rejection ledger: %d C rows, %d C++ rows, %d node-named prefixes, "
          "%d LIVE_VIA waiver(s)"
          % (len(cpp_tables["kBlockerSubstrings"]),
             len(cpp_tables["kCxxBlockerSubstrings"]),
             len(cpp_tables["kNodeNamedPrefixes"]), len(LIVE_VIA)))
    report_ambiguity(all_rows, corpus)
    report_other(repo, run_realworld.classify_blocker)

    if failures:
        for failure in failures:
            print("error: " + failure, file=sys.stderr)
        return 1
    print("rejection ledger needle check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
