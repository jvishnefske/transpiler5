// FR-61f-10: a range-eligible `for` body may touch a GLOBAL or a
// function-local `static`.
//
// This was recorded as needing "a separate mechanism -- module symbols and
// staged copies rather than placeBackedScalars". It needs neither, and for the
// same reason the aggregates in FR-61f-6 did not: the cell story never applied.
// A global is not a frame slot at all. A read is an `emitrust.global_load` and
// a write an `emitrust.global_store`, both plain SSA ops that sit inside a
// region as happily as anywhere else, and no `memref.alloca` is involved on any
// arm. A function-local `static` is module-level state that `emitLocalVar`
// routes to `createGlobal` under a `<function>_<name>` mangling, so it is the
// same op pair.
//
// The one exception kept out is an ADDRESS-TAKEN global: `&g` pulls in the
// `emitrust.global_addr` / `emitrust.global_cells` machinery, which stages real
// cells. That guard is NOT pinned here and cannot be yet -- the importer
// rejects `&g` outright today ("unsupported: taking the address of a global
// variable"), so no such input reaches the matcher at all. It is defensive
// against FR-80/FR-82 making the shape reachable; whoever lands those owns
// pinning it.
//
// A global written inside the region must still be observable AFTER the loop --
// that is what the byte-diff at several argument values is for, since a
// dropped store would compile perfectly cleanly.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/range_for_globals > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_globals a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/range_for_globals a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
//
// The emitter groups the global-touching functions into one actor impl and
// emits them in ITS order, not the source's, so the pins live here in one
// ordered block rather than beside each function.
// CHECK-LABEL: fn write_global
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-LABEL: fn read_global
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-LABEL: fn global_array_write
// CHECK:         for {{i|_i}} in 0i32..8i32
// CHECK:         for {{i_1|_i_1}} in 0i32..
// CHECK-LABEL: fn global_struct
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-LABEL: fn local_static
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while

int printf(const char *, ...);

int g_counter = 0;
static int s_table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static unsigned g_mask = 0xf;
struct Pt { int x, y; };
static struct Pt g_pt = {3, 4};

// Reading a global array and a global unsigned scalar.
int read_global(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += s_table[i & 7] + (int)g_mask;
  return s;
}

// WRITING a global from inside the region, and reading it after the loop: the
// store must survive.
int write_global(int n) {
  for (int i = 0; i < n; i++)
    g_counter += i;
  return g_counter;
}

// A global STRUCT, read and written per iteration.
int global_struct(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    g_pt.x = i;
    s += g_pt.x * g_pt.y;
  }
  return s + g_pt.x;
}

// A global ARRAY written per iteration, then read back by a second lifted loop.
int global_array_write(int n) {
  int s = 0;
  for (int i = 0; i < 8; i++)
    s_table[i] = i * n;
  for (int i = 0; i < n; i++)
    s += s_table[i & 7];
  return s;
}

// A FUNCTION-LOCAL `static`: module-level state under a `<fn>_<name>`
// mangling, carried across calls.
int local_static(int n) {
  static int seen = 100;
  int s = 0;
  for (int i = 0; i < n; i++) {
    seen += i;
    s += seen & 31;
  }
  return s + seen;
}

int main(int argc, char **argv) {
  int n = argc + 5;
  printf("%d\n", read_global(n));
  printf("%d\n", write_global(n));
  printf("%d\n", global_struct(n));
  printf("%d\n", global_array_write(n));
  printf("%d\n", local_static(n));
  printf("%d\n", local_static(n));
  printf("c=%d x=%d\n", g_counter, g_pt.x);
  return 0;
}
