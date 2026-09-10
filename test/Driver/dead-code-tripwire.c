// FR-220 TRIPWIRE, rustc half: a dead emitted `fn` is VISIBLE again, and the
// four covered item kinds are silent. Proven by running rustc, because that is
// the only thing that can say whether an attribute actually took effect.
//
// The invariant this file pins. Until FR-220 every emitted crate opened with
// `#![allow(dead_code)]`, which hid all dead items at once -- including the one
// kind that would signal an EMITTER DEFECT rather than faithful translation: a
// function the emitter wrote that nothing can reach. The allow now rides on
// records, enums (both the newtype and its associated-constant `impl`), data
// enums and inherent impls; a plain `fn` gets nothing. This file is the
// end-to-end proof of both halves at once, and it is deliberately a COUNT:
// `generated 1 warning` says the dead struct, the never-mentioned enumerator
// and the uncalled method produced ZERO diagnostics between them, so the
// targeted attributes are real and not merely present in the text.
//
// The emitted `fn` is NOT dropped. FR-220 attributes differently; it does not
// remove anything, and the FN line below says so against the crate that was
// actually compiled.
//
// Why this is a WARNING and not `dead_code = "deny"`. Denying it was measured
// and rejected twice: it regresses the c-testsuite ledger by 10 (an
// external-linkage C function uncalled in its own TU is not even a C warning
// -- the deadness is an artifact of the emitter privatizing it for a `bin`
// crate), and it breaks FR-44's headline guarantee, because recovery drops a
// rejected CALLER and orphans every function only that caller reached.
// RECOVERY STRUCTURALLY MANUFACTURES DEAD FUNCTIONS, and the RECOVER section
// below is that exact shape: `dropped` dies at its `void *` parameter, which
// orphans `only_reached_by_dropped`, and the crate must STILL BUILD. Under
// `deny` it did not. The gate against a regression is the clippy-eval ratchet
// (nix/clippy-eval/clippy_eval.py's TRACKED_RUSTC_LINTS), which counts
// `dead_code` over the frozen epoch corpus and holds it at zero.
//
// The emit-level half -- which items carry the attribute, and that the crate
// root and the deny table carry nothing -- is test/Driver/dead-code-allow-
// targeted.c, and needs no cargo.
//
// REQUIRES: cargo
// RUN: split-file %s %t
//
// The dead FUNCTION is seen. Exactly one warning comes out of a crate that
// also holds an unconstructed record, an unmentioned enumerator and an
// uncalled inherent method, and the build SUCCEEDS.
// RUN: emitrust-cc --emit=crate %t/dead.c -o %t.dead --build 2>%t.dead.err
// RUN: FileCheck %s --check-prefix=DEAD --input-file=%t.dead.err
// RUN: FileCheck %s --check-prefix=FN --input-file=%t.dead/src/main.rs
//
// FR-44: the recovery path still builds. This is the case `deny` broke.
// RUN: emitrust-cc --emit=crate --incremental %t/recover.c -o %t.rec --build \
// RUN:   2>%t.rec.err
// RUN: FileCheck %s --check-prefix=RECOVER --input-file=%t.rec.err
// RUN: FileCheck %s --check-prefix=RECFN --input-file=%t.rec/src/main.rs

//--- dead.c
int printf(const char *, ...);

// Covered by FR-220's targeted attributes -- none of these may warn.
struct never_built { int a; int b; };
enum code { CODE_OK, CODE_LAST_UNUSED };
static int g_total;
void bump(int n) { g_total += n; }

// NOT covered, on purpose: this is the tripwire.
static int never_called(int x) { return x + 1; }

int main(void) {
  bump(3);
  printf("%d %d\n", g_total, (int)CODE_OK);
  return 0;
}

//--- recover.c
int printf(const char *, ...);

// Reachable only from `dropped`, so recovery orphans it and it goes dead --
// through no fault of the emitter. It must warn, never fail the build.
static int only_reached_by_dropped(int x) { return x * 2; }

// Rejected at its signature: `void *` parameter.
void dropped(int v, void *sink) {
  only_reached_by_dropped(v);
  (void)sink;
}

int main(void) { printf("%d\n", 1); return 0; }

// The diagnostic, from rustc, naming the emitted symbol and the lint.
// DEAD:      warning: function `tu0_never_called` is never used
// DEAD:      = note: {{.*}}dead_code
// Exactly one: the record, the enumerator and the method are covered and
// silent. Reinstating a blanket crate-root allow makes this ZERO warnings and
// fails here; forgetting one of the four targeted allows makes it two or more
// and also fails here.
// DEAD:      warning: {{.*}} generated 1 warning
// DEAD-NEXT: {{ *}}Finished

// ... and the function is STILL EMITTED. Nothing was dropped.
// FN: fn tu0_never_called(x: i32) -> i32 {
// FN-NEXT:     x + 1i32

// Recovery: one located rejection, the orphan warns, and cargo finishes.
// RECOVER:      recover.c:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: void pointer parameter (recovered: item dropped)
// RECOVER:      warning: function `tu0_only_reached_by_dropped` is never used
// RECOVER:      warning: {{.*}} generated 1 warning
// RECOVER-NEXT: {{ *}}Finished

// The orphan is emitted too -- recovery drops the REJECTED item, not the
// items that merely became unreachable because of it.
// RECFN: fn tu0_only_reached_by_dropped(x: i32) -> i32 {
