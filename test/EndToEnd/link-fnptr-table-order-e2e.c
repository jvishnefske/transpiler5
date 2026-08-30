// REQUIRES: cargo
// FR-156, the byte-diff half of the shard-retag order-independence pin: a
// wrong retag of an `emitrust.opaque` fn-ptr payload is a MISCOMPILE, not
// a compile error, once the names it confuses both exist -- `Some(tu1_p_a)`
// and `Some(tu1_p_ab)` are both well-typed `fn(i32) -> i32`, so a table
// entry pointed at the wrong one builds cleanly and returns the wrong
// number. Compile-clean evidence therefore cannot see this class of bug;
// only running it can. The SAME two shards are linked in BOTH link-line
// orders (table shard at position 0, where its own `tu0_` tags need no
// retag, and at position 1, where every tag is rewritten) and each built
// crate's stdout is diffed against the clang-linked native binary. The
// dispatch index and every argument derive from argc, so neither rustc nor
// clang can constant-fold the table away and hide a mis-wired entry, and
// the run is repeated with extra argv so a second argc value exercises a
// different permutation of the three targets.
//
// Per-TU shards through the FR-56 shim:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-fnptr-table-lib.c -o %t.lib.o
//
// The native oracle, at both argc values:
// RUN: clang -std=c11 %s %S/Inputs/link-fnptr-table-lib.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.native a b > %t.native2.out
//
// Order A -- table shard FIRST (no retag):
// RUN: emitrust-cc --link %t.lib.o %t.main.o -o %t.first --crate-name fnptr_table_first --build
// RUN: %t.first/target/release/fnptr_table_first > %t.first.out
// RUN: diff %t.native.out %t.first.out
// RUN: %t.first/target/release/fnptr_table_first a b > %t.first2.out
// RUN: diff %t.native2.out %t.first2.out
//
// Order B -- table shard SECOND (every `tu0_` tag retags to `tu1_`, the
// case that used to fail the link outright):
// RUN: emitrust-cc --link %t.main.o %t.lib.o -o %t.second --crate-name fnptr_table_second --build
// RUN: %t.second/target/release/fnptr_table_second > %t.second.out
// RUN: diff %t.native.out %t.second.out
// RUN: %t.second/target/release/fnptr_table_second a b > %t.second2.out
// RUN: diff %t.native2.out %t.second2.out

int printf(const char *, ...);
int run_tbl(int, int);
int run_local(int);
int run_hook(int);
int mode_of(int);

static int filler(int x) { return x + 7; }

int main(int argc, char **argv) {
  int seed = argc;
  for (int i = 0; i < 3; ++i)
    printf("tbl[%d]=%d\n", i, run_tbl((seed + i) % 3, seed + i));
  printf("local=%d hook=%d mode=%d filler=%d\n", run_local(seed),
         run_hook(seed), mode_of(seed), filler(seed));
  return 0;
}
