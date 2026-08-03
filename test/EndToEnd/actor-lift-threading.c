// REQUIRES: cargo
// FR-62 slice 4 (stage A) differential test, the THREADING shape: helper()
// touches no global itself but sits on the call path to bump(), which
// writes `counter` — the plan roles helper arm-by-closure, so under
// --actor-lift it lifts INTO the CounterActor impl and its bump() calls
// become `(*self).bump()`. The byte-diff against the clang native pins
// that the threaded receiver reaches the same state the C global held
// (the SLICE-4 SPIKE's resolved threading question, as an executable
// oracle).
// RUN: emitrust-cc --actor-lift --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_lift_threading > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int counter;

int bump(void) {
  counter += 2;
  return counter;
}

int helper(int k) {
  int a = bump();
  int b = bump();
  return a + b + k;
}

int main(void) {
  int x = helper(1);
  printf("h1=%d\n", x);
  int y = helper(10);
  printf("h2=%d\n", y);
  printf("c=%d\n", counter);
  return 0;
}
