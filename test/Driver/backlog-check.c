// RUN: %python %S/../../docs/plans/plan.py --repo %S/../.. check
//
// Pins the sync between design.md (the prose evidence ledger) and
// docs/plans/backlog.toml (the machine-readable queue over it). The two
// are edited together by protocol; this test is what makes the protocol
// enforceable: it fails when an indexed item's design.md entry has been
// deleted (a careless range edit did exactly that to FR-113..115 on
// 2026-08-22), when a box is checked without the index status moving to
// landed, when an open FR is missing from the index entirely, when a
// blocked_by edge names an unknown id, or when the dependency graph has
// a cycle. It runs in the fast tier so the pre-commit gate always sees
// it. This file is a .c only so lit discovers it; it contains no C.
