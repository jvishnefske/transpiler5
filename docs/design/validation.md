## Validation

Every requirement above is validated exclusively by regression tests run by
the lit suite; no requirement is checked off based on manual inspection.
The suite must pass warning-free in the pinned dev shell before a
requirement's box may be ticked. CI (.github/workflows/ci.yml) enforces the
same gate on every push and pull request: the full check-emitrust suite in
the pinned Nix dev shell, plus an explicit c-testsuite conformance step in
which every test listed in test/CTestSuite/expected-pass.txt is required to
pass differentially and any MISCOMPILE or ratchet violation fails the run.

