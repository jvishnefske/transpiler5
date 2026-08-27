// FR-132 (clippy::needless_late_init): the LATE-INIT MERGE, byte-diffed. A
// deferred binding used to render at TWO program points -- `let s: i32;` at its
// declaration and `s = <rhs>;` at its initializing write. Those fold into ONE
// `let [mut] s: i32 = <rhs>;` at the WRITE, with the declaration emitting
// nothing.
//
// The fold SINKS a declaration past statements, and `cargo build` cannot see
// that going wrong: a dropped, duplicated or reordered initializer compiles
// perfectly and prints the wrong number. So every case below is byte-diffed
// against the clang-built native at several argument counts -- the seeds come
// from `argc` so nothing constant-folds, and a lost store shows up as a wrong
// number rather than as a compile error.
//
// The gaps deliberately contain PRINTING side effects: the declaration moves,
// the statements must not, and the interleaved stdout is the pin for that. The
// `--build` line is a second, weaker check: emitted crates deny(unused_mut), so
// a `mut` the fold got wrong in either direction fails the build outright.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native 0 > %t.n0.out && %t.crate/target/release/late_init_merge 0 > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/late_init_merge a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/late_init_merge a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

int helper(int v) { return v * 3 + 1; }
void twiddle(int *p) { *p = *p + 1; }

struct Pt {
  int x, y;
};

struct Pt mkpt(int n) {
  struct Pt p;
  p.x = n;
  p.y = n * 2;
  return p;
}

// THE MOTIVATING SHAPE: a `println!` and a `let` sit between the declaration
// and the initializing write. Both keep their position; only the declaration
// line disappears, reappearing as the `let` of the write.
// CHECK-LABEL: fn gapped_effect
// CHECK-NEXT:    println!("[gap {}]", n);
// CHECK-NEXT:    let t: i32 = helper(n + 1i32);
// CHECK-NEXT:    let mut s: i32 = t * 2i32;
int gapped_effect(int n) {
  int s;
  printf("[gap %d]\n", n);
  int t = helper(n + 1);
  s = t * 2;
  twiddle(&s);
  return s * 10 + s;
}

// The `mut` leg: the binding is mutated after its initializing write, so the
// merged form must keep `mut`.
// CHECK-LABEL: fn mut_merge
// CHECK:         let mut acc: i32 = seed + 7i32;
int mut_merge(int n) {
  int acc;
  int seed = helper(n);
  acc = seed + 7;
  printf("[acc %d]\n", acc);
  twiddle(&acc);
  return acc * 3;
}

// A dead store precedes the real initializing write: the scan steps over what
// renders nothing and merges at the SURVIVING write.
// CHECK-LABEL: fn dead_store_first
// CHECK-NOT:     99i32
// CHECK:         let mut x: i32 = v3 + 5i32;
int dead_store_first(int n) {
  int x;
  x = 99;
  x = helper(n) + 5;
  twiddle(&x);
  return x * 2 + x;
}

// Bindings declared INSIDE a loop body -- the shape that motivated FR-132
// (test/EndToEnd/switch-enum.c). Both the struct-typed `p` and the scalar `d`
// merge; each iteration re-declares, so a merge that hoisted or duplicated an
// initializer shows up as a wrong running sum.
// CHECK-LABEL: fn struct_loop
// CHECK:         let [[V:v[0-9]+]]: Pt = mkpt(i);
// CHECK-NEXT:    let p: Pt = [[V]];
// CHECK-NEXT:    let d: i32 = p.x + p.y;
int struct_loop(int n) {
  int acc = 0;
  for (int i = 0; i < n + 2; i++) {
    struct Pt p = mkpt(i);
    int d = p.x + p.y;
    printf("d=%d\n", d);
    acc += d * 2 + d;
  }
  return acc;
}

// REFUSES: every write is inside a `switch` arm while the read is after it.
// Sinking the declaration into an arm would move the binding out of scope for
// that read. The bare declaration must survive -- this residual shape is
// explicitly out of the fold's reach.
// CHECK-LABEL: fn region_write
// CHECK:         let {{v[0-9]+}}: i32;
// CHECK:         match
int region_write(int n) {
  int r;
  switch (n % 3) {
  case 0:
    r = 11;
    break;
  case 1:
    r = 22;
    break;
  default:
    r = 33;
    break;
  }
  return r * 2 + r;
}

// FR-61b keeps PRIORITY: a deferred binding whose only writes are the tails of
// both arms is already folded into an if-EXPRESSION binding, a strictly better
// rendering than the new merge. It must stay that way.
// CHECK-LABEL: fn if_expr_priority
// CHECK:         = if
int if_expr_priority(int n) {
  int r;
  if (n % 2) {
    r = helper(n);
  } else {
    r = helper(n + 1);
    r = r + 1;
  }
  return r * 2 + r;
}

int main(int argc, char **argv) {
  int n = argc;
  printf("%d %d %d %d %d %d\n", gapped_effect(n), mut_merge(n),
         dead_store_first(n), struct_loop(n), region_write(n),
         if_expr_priority(n));
  return 0;
}
