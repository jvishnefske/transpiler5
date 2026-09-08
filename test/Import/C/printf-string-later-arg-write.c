// RUN: emitrust-import-c %s | FileCheck %s

// FR-192: a `%s` argument is CONVERTED -- the pointee read -- when printf
// reaches the directive, not when the argument is evaluated. C17 6.5.2.2p10
// puts a sequence point before the call, and the array-to-pointer decay of a
// buffer does not access the array's stored value, so a LATER argument whose
// call writes that buffer is well defined and its store IS visible to the
// conversion. Lowering the `%s` where it stands in the format string
// snapshotted the region BEFORE that store and lost it (measured: native
// `5b42625d20310a`, emitted `5b61625d20310a`).
//
// This pins the ORDER OF THE EMITTED OPS, which is what carries the
// semantics: the side-effecting argument's call is created FIRST, then the
// region borrow and its `__emitrust_cstr`, then the single `println!`. The
// same reordering is what makes the emitted Rust borrow-check -- the shared
// borrow of the buffer is now created after the mutable borrow the
// intervening call needs, rather than spanning it.
//
// It also pins the fence that FR-191 established for the char-region and
// `%c` bypasses onto the ARGV bypass, which never carried it: an argv `%s`
// or `%c` in front of a side-effecting argument used to flush and write its
// bytes before that argument had even been evaluated, inverting the two
// writes (measured: native `<1>[hello] 1`, emitted `[hello<1>] 1`). A
// declined argv `%s` is deferred through `__emitrust_cstr` rather than
// rejected, so the shape is kept; only the raw-byte property is given up,
// exactly the trade FR-191 already recorded for char regions.
//
// The controls matter as much as the cases: with no side-effecting argument
// to come, nothing moves and the raw bypasses still fire.

int printf(const char *, ...);
int sprintf(char *, const char *, ...);

char gbuf[8];
static int seq;

static int bump(char *p, int c) {
  p[0] = (char)c;
  return 1;
}

static int noisy(void) {
  seq = seq + 1;
  printf("<%d>", seq);
  return seq;
}

// CHECK-LABEL: func.func @deferred_region
void deferred_region(void) {
  char buf[8];
  char out[32];
  buf[0] = 'a';
  buf[1] = 0;

  // The headline shape. The write happens first in the emitted IR, the
  // region is borrowed and converted after it, and one `println!` prints
  // both holes -- no segment flush, because a raw write would land before
  // the very argument that is allowed to change what it writes.
  printf("[%s] %d\n", buf, bump(buf, 'B'));
  // CHECK: %[[N:.*]] = call @bump(
  // CHECK: %[[SL:.*]] = emitrust.slice_of
  // CHECK: %[[S:.*]] = emitrust.call_opaque "__emitrust_cstr"(%[[SL]])
  // CHECK: emitrust.call_opaque "println!"(%[[S]], %[[N]]) {args = ["[{}] {}", 0 : index, 1 : index]}

  // A `%.Ns` defers through the bounded `__emitrust_cstr_n` twin.
  printf("<%.2s> %d\n", buf, bump(buf, 'C'));
  // CHECK: %[[N2:.*]] = call @bump(
  // CHECK: emitrust.call_opaque "__emitrust_cstr_n"
  // CHECK: emitrust.call_opaque "println!"

  // TWO `%s` holes before ONE write: both are converted after it, and the
  // operand slots stay in directive order.
  printf("%s/%s %d\n", buf, gbuf, bump(buf, 'D'));
  // CHECK: %[[N3:.*]] = call @bump(
  // CHECK: %[[A:.*]] = emitrust.call_opaque "__emitrust_cstr"
  // CHECK: %[[B:.*]] = emitrust.call_opaque "__emitrust_cstr"
  // CHECK: emitrust.call_opaque "println!"(%[[A]], %[[B]], %[[N3]]) {args = ["{}/{} {}", 0 : index, 1 : index, 2 : index]}

  // The `sprintf` buffer context shares the directive grammar, so the same
  // deferral applies before the `format!`.
  sprintf(out, "{%s} %d", buf, bump(buf, 'E'));
  // CHECK: %[[N4:.*]] = call @bump(
  // CHECK: emitrust.call_opaque "__emitrust_cstr"
  // CHECK: emitrust.call_opaque "format!"
}

// CHECK-LABEL: func.func @c_main
int main(int argc, char **argv) {
  // An argv `%s` in front of a stdout-writing argument: the raw bypass is
  // declined (it would write before `noisy()` runs) and the element is
  // deferred through the Latin-1 funnel instead of being lost to a
  // rejection. The `noisy()` call is emitted before the argv borrow.
  printf("[%s] %d\n", argv[1], noisy());
  // CHECK: %[[M:.*]] = call @noisy()
  // CHECK: %[[AV:.*]] = emitrust.argv_arg
  // CHECK: %[[T:.*]] = emitrust.call_opaque "__emitrust_cstr"(%[[AV]])
  // CHECK: emitrust.call_opaque "println!"(%[[T]], %[[M]]) {args = ["[{}] {}", 0 : index, 1 : index]}

  // The argv `%c` byte bypass carried the same missing fence and takes the
  // same fix. Nothing is REORDERED here -- a `%c` reads its argument's VALUE
  // during argument evaluation, so the byte load may stay where it is; what
  // changes is that no raw write escapes ahead of `noisy()`, and both holes
  // land in one `println!`.
  printf("(%c) %d\n", argv[1][0], noisy());
  // CHECK: %[[C:.*]] = emitrust.call_opaque "__emitrust_fmt_c"
  // CHECK: %[[M2:.*]] = call @noisy()
  // CHECK: emitrust.call_opaque "println!"(%[[C]], %[[M2]]) {args = ["({}) {}", 0 : index, 1 : index]}

  // CONTROL -- no side-effecting argument to come, so both argv bypasses
  // still fire exactly as C99-43 C3 built them: a flushed segment, a raw
  // write, a fresh segment.
  printf("[%s]\n", argv[1]);
  // CHECK: emitrust.call_opaque "print!"() {args = ["["]}
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"
  // CHECK: emitrust.call_opaque "println!"() {args = ["]"]}
  printf("(%c)\n", argv[1][0]);
  // CHECK: emitrust.call_opaque "print!"() {args = ["("]}
  // CHECK: emitrust.call_opaque "__emitrust_byte_out"
  // CHECK: emitrust.call_opaque "println!"() {args = [")"]}

  return 0;
}
