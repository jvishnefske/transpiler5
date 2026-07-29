#!/usr/bin/env python3
"""FR-50 gate: ``--search`` may never port FEWER items than not searching.

Why this exists as a permanent gate rather than as one more assertion inside
run_realworld.py: the loss it guards against is SILENT. FR-43's search starts
from FR-41's Green-or-Yellow coloring and every child state admits strictly
less than its parent, so an item the coloring wrongly calls Red is absent from
the search's answer and no later stage can put it back. Nothing in the emitted
crate says a thing is missing -- it is simply not there. A coloring change that
reintroduced such a false Red would pass every other test in the tree, because
every other test is written against the coloring's own idea of what should be
in the crate.

So the reference here is deliberately NOT the coloring: it is the unrestricted
recovering import, the one ``--incremental`` performs with nothing excluded.
That import is a fact about the importer, not about the analysis, which is what
makes it a check on the analysis at all.

The measurement is FR-44's own per-item ledger (``emitrust-progress.json``'s
``totals.ported``), so "ported" means here exactly what it means everywhere
else in the project, and both sides are compared with the SAME denominator.

Usage:
  search_never_worse.py --emitrust-cc <path> --corpus <dir> --workdir <dir>
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_realworld import read_compile_commands  # noqa: E402

TIMEOUT_SECONDS = 900


def ported_count(tool, sources, include_flags, crate_dir, search):
    """Transpile once and return (ported, total) from the FR-44 ledger, or None.

    ``search`` selects the compared configuration. Everything else is held
    identical on purpose -- same sources, same include flags, same
    ``--incremental`` -- so the only difference between the two numbers this
    script compares is whether FR-43 ran.
    """
    if os.path.isdir(crate_dir):
        shutil.rmtree(crate_dir)
    argv = [tool, "--emit=crate", "--incremental", *sources, *include_flags,
            "-o", crate_dir]
    if search:
        argv.append("--search")
    try:
        subprocess.run(argv, timeout=TIMEOUT_SECONDS, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, check=False)
    except subprocess.TimeoutExpired:
        return None
    try:
        with open(os.path.join(crate_dir, "emitrust-progress.json"),
                  "r", encoding="utf-8") as handle:
            totals = json.load(handle)["totals"]
    except (OSError, ValueError, KeyError):
        return None
    return totals["ported"], totals["graph_items"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitrust-cc", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--workdir", required=True)
    args = parser.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    projects = sorted(
        name for name in os.listdir(args.corpus)
        if os.path.isfile(os.path.join(args.corpus, name,
                                       "compile_commands.json")))
    if not projects:
        print("FAIL: no corpus project found under %s" % args.corpus)
        return 1

    failures = []
    for name in projects:
        project_dir = os.path.join(args.corpus, name)
        sources, include_dirs = read_compile_commands(project_dir)
        include_flags = ["-I" + d for d in include_dirs]
        plain = ported_count(args.emitrust_cc, sources, include_flags,
                             os.path.join(args.workdir, name + ".plain"),
                             search=False)
        searched = ported_count(args.emitrust_cc, sources, include_flags,
                                os.path.join(args.workdir, name + ".searched"),
                                search=True)
        if plain is None or searched is None:
            failures.append("%s: a configuration produced no readable "
                            "emitrust-progress.json (plain=%s searched=%s)"
                            % (name, plain, searched))
            continue
        verdict = "ok" if searched[0] >= plain[0] else "REGRESSION"
        print("%-14s no-search %d/%d   --search %d/%d   %s"
              % (name, plain[0], plain[1], searched[0], searched[1], verdict))
        if searched[0] < plain[0]:
            failures.append(
                "%s: --search ported %d items, plain --incremental ported %d. "
                "The search can only ever REMOVE items from FR-41's root "
                "state, so this is an item the coloring called Red and the "
                "importer accepts -- a false Red, and it is unrecoverable."
                % (name, searched[0], plain[0]))

    if failures:
        print("")
        for failure in failures:
            print("FAIL: %s" % failure)
        return 1
    print("\nAll %d corpus projects: --search >= no-search." % len(projects))
    return 0


if __name__ == "__main__":
    sys.exit(main())
