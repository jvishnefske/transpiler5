#!/usr/bin/env python3
"""Conformance-ledger runner for the external c-testsuite single-exec suite.

Runs every ``tests/single-exec/NNNNN.c`` in the vendored c-testsuite through
``emitrust-cc --emit=crate --build`` and classifies each test as
PASS/MISCOMPILE/UNSUPPORTED against the expected-pass.txt two-way ratchet.

Since FR-243 the runner body lives in test/harness/ledger_runner.py, shared
with the other suite ledgers; this wrapper pins the c-testsuite
parameterization and keeps the historical invocation path byte-stable.  The
suite conventions (single .c files, ``main`` entry point, exit 0, the
``NNNNN.c.expected`` file as byte-exact reference stdout) and the outcome
model are documented in the shared module and in c-testsuite.c.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

from ledger_runner import (  # noqa: E402,F401 -- re-exports: test/Fuzz imports these from here
    MISCOMPILE,
    PASS,
    RUN_TIMEOUT,
    TRANSPILE_BUILD_TIMEOUT,
    UNSUPPORTED,
    SuiteSpec,
    first_line,
    load_name_list,
    main,
    output_diff_snippet,
    resolve_tool,
    run_command,
    sanitize_crate_binary_name,
)

SPEC = SuiteSpec(
    name="c-testsuite",
    description=__doc__,
    source_suffix=".c",
    suite_subdir=("tests", "single-exec"),
    uses_candidates=False,
    uses_expected_dir=False,
    suite_help="Root of the vendored c-testsuite checkout.",
    missing_suite_hint=(
        "error: %s not found; is the c-testsuite submodule initialized"
        " (git submodule update --init)?"
    ),
    manifest_header=(
        "# c-testsuite single-exec conformance ledger for emitrust-cc.\n"
        "# One passing test filename per line.  Regenerated via:\n"
        "#   run_c_testsuite.py ... --update\n"
        "# Generated against the current build/tools/emitrust-cc; the\n"
        "# integration pass will regenerate this after the in-flight\n"
        "# switch/enum features land.\n"
    ),
)

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], SPEC))
