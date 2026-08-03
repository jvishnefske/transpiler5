// REQUIRES: cargo
// FR-62 slice 4 (stage A) differential test, the CROSS shape: two actors
// (LEFT, RIGHT) whose reads are call-mediated, so observe/poke/robserve
// survive seeding as role=cross clients. Under --actor-lift each gains one
// &mut parameter per footprint actor in sorted order, main materializes a
// fresh &mut pair per call, the self-recursive arm steps() and the
// self-recursive cross robserve() both lean on Rust's implicit reborrow —
// and the byte-diff against the clang native proves the whole threading
// discipline preserves C's effect order (the SLICE-4 SPIKE's two-actor
// probe as an executable oracle).
// RUN: emitrust-cc --actor-lift --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_lift_cross > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int left;
int right;

int bump_left(void)  { left += 1; return left; }
int bump_right(void) { right += 3; return right; }
int get_left(void)   { return left; }
int get_right(void)  { return right; }

/* closure reads both actors, direct footprint empty: cross */
int observe(int k) { return get_left() + get_right() + k; }

/* closure writes LEFT, reads RIGHT: cross */
int poke(int k) { return bump_left() + get_right() + k; }

/* self-recursive, closure footprint {left}: arm of LEFT */
int steps(int n) {
  if (n > 0) { bump_left(); return steps(n - 1); }
  return left;
}

/* self-recursive cross */
int robserve(int n) {
  if (n <= 0) return 0;
  return observe(n) + robserve(n - 1);
}

int main(void) {
  int a = poke(5);
  printf("poke=%d\n", a);
  int b = observe(2);
  printf("obs=%d\n", b);
  int c = steps(3);
  printf("steps=%d\n", c);
  int d = robserve(2);
  printf("rob=%d\n", d);
  printf("l=%d r=%d\n", left, right);
  return 0;
}
