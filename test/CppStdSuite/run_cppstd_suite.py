#!/usr/bin/env python3
"""Conformance-ledger runner for the llvm-test-suite standard-C++ corpus.

Runs every candidates.txt entry (suite-relative paths into the
third_party/llvm-test-suite submodule, curated by curate_candidates.py)
through ``emitrust-cc --emit=crate --build`` and classifies each as
PASS/MISCOMPILE/UNSUPPORTED against the expected-pass.txt two-way
ratchet.  Reference stdout lives under expected/<relpath>.expected,
generated once per submodule pin from the pinned dev-shell clang++
(NOT llvm-test-suite's *.reference_output; see curate_candidates.py).

The runner body is test/harness/ledger_runner.py (FR-243); this wrapper
pins the CppStdSuite parameterization.  See cppstd-suite.cpp for the
outcome model and the pin-advance procedure.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

from ledger_runner import SuiteSpec, main  # noqa: E402

SPEC = SuiteSpec(
    name="cppstd",
    description=__doc__,
    source_suffix=".cpp",
    suite_subdir=(),
    uses_candidates=True,
    uses_expected_dir=True,
    suite_help="Root of the llvm-test-suite submodule checkout.",
    missing_suite_hint=(
        "error: %s not found; is the llvm-test-suite submodule initialized"
        " (git submodule update --init third_party/llvm-test-suite)?"
    ),
    manifest_header=(
        "# CppStdSuite conformance ledger for emitrust-cc over the pinned\n"
        "# llvm-test-suite submodule (candidates.txt is the corpus).\n"
        "# One passing suite-relative path per line.  Regenerated via:\n"
        "#   run_cppstd_suite.py ... --update\n"
        "# Advance ONLY together with the submodule pin procedure in\n"
        "# cppstd-suite.cpp (re-curate, regenerate expected/, re---update).\n"
    ),
)

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], SPEC))
