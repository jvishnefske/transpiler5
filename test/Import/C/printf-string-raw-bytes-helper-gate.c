// The Display funnel must be neither CALLED nor EMITTED anywhere in this
// module -- the implicit-check-nots cover the WHOLE output, not just the tail
// after the last positive CHECK.
// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not='"__emitrust_cstr"' --implicit-check-not='fn __emitrust_cstr('

// FR-191 helper request gating: helpers are emitted only when requested, and
// the widened raw-bytes `%s` funnel must move the REQUEST too. A module whose
// every `%s` takes the bypass emits `__emitrust_cstr_out` and NO
// `__emitrust_cstr` -- a stale unused helper would be an `unused` deny in the
// emitted crate, which is a build failure, not a cosmetic issue.

int printf(const char *, ...);

int main(void) {
  char buf[4];
  buf[0] = 'h';
  buf[1] = 0;
  printf("%s\n", buf);
  return 0;
}

// CHECK: emitrust.call_opaque "__emitrust_cstr_out"
// CHECK: emitrust.verbatim "fn __emitrust_cstr_out(s: &[i8]) {
