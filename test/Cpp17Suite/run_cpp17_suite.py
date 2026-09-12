#!/usr/bin/env python3
"""Conformance-ledger runner for the in-repo C++17 feature corpus.

Runs every ``Inputs/NNNNN.cpp`` in test/Cpp17Suite through
``emitrust-cc --emit=crate --build`` and classifies each test as
PASS/MISCOMPILE/UNSUPPORTED against the expected-pass.txt two-way ratchet.
Corpus entries deliberately sit at and beyond the supported C++17 subset's
frontier, so UNSUPPORTED is a legal steady state -- the UNSUPPORTED count
IS the frontier measurement.

Since FR-243 the runner body lives in test/harness/ledger_runner.py, shared
with the other suite ledgers; this wrapper pins the Cpp17Suite
parameterization and keeps the historical invocation path byte-stable.  The
corpus conventions (printf-only output, deterministic, ``main`` exits 0,
committed ``.expected`` files generated once from a native
``clang++ -std=c++17`` build) are documented in cpp17-suite.cpp.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

from ledger_runner import SuiteSpec, main  # noqa: E402

SPEC = SuiteSpec(
    name="cpp17",
    description=__doc__,
    source_suffix=".cpp",
    suite_subdir=(),
    uses_candidates=False,
    uses_expected_dir=False,
    suite_help="Directory holding the corpus NNNNN.cpp/.expected files"
    " (test/Cpp17Suite/Inputs).",
    missing_suite_hint="error: corpus directory not found: %s",
    manifest_header=(
        "# Cpp17Suite conformance ledger for emitrust-cc.\n"
        "# One passing corpus filename per line.  Regenerated via:\n"
        "#   run_cpp17_suite.py ... --update\n"
        "# Feature work flips named corpus entries UNSUPPORTED->PASS\n"
        "# and ratchets this ledger forward in the same commit.\n"
    ),
)

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], SPEC))
