// RUN: %python %S/../../docs/plans/plan.py --repo %S/../.. check
//
// Pins the sync between the prose evidence ledger (design.md +
// docs/plans/entries/*.md + docs/design/*.md, joined in
// docs/plans/ledger.manifest order -- FR-242) and docs/plans/backlog.toml
// (the machine-readable queue over it). The two are edited together by
// protocol; this test is what makes the protocol enforceable: it fails
// when an indexed item's ledger entry has been deleted (a careless range
// edit did exactly that to FR-113..115 on 2026-08-22, back when the
// ledger was one 25k-line file), when a box is checked without the index
// status moving to landed, when an open FR is missing from the index
// entirely, when a blocked_by edge names an unknown id, when the
// dependency graph has a cycle, or -- since the split -- when the
// manifest and the entry files disagree (missing/orphan/misnamed member,
// an anchor that no longer resolves inside its own entry file). It runs
// in the fast tier so the pre-commit gate always sees it. This file is a
// .c only so lit discovers it; it contains no C.
