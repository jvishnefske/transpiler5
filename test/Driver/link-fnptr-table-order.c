// FR-156: a shard's emitted output must not depend on its POSITION on the
// link line. `renameShardTags` alpha-renames a shard's per-TU tags to its
// link-line ordinal and rewrites two carriers of the tagged name: symbol
// uses (via `SymbolTable::replaceAllSymbolUses`) and `emitrust.call_opaque`
// callees (plain strings, invisible to the symbol table). It missed a
// third, same-class carrier: the `emitrust.opaque` payloads that spell a
// fn-ptr target as `Some(<symbol>)` -- both inside an `emitrust.global`'s
// init array (a file-static dispatch table, the systemd
// src/basic/rlimit-util.c:222 shape) and in a body-level
// `emitrust.constant`. Those strings kept the shard's own `tu0_` tag while
// the functions were retagged, so the same object files linked in one
// order emitted a crate and in the OTHER order died at the emitter's
// dangling-target check. The check is right; the missed rewrite was the
// bug. Pinned here: both orders succeed, the retagged table names the
// retagged functions, the prefix-related pair `p_a`/`p_ab` survives the
// rewrite un-corrupted, the NON-symbol opaque payloads (`None`, an enum
// path) pass through untouched, and the two orders' output is equal up to
// the ordinals themselves.
//
// Per-TU shards through the FR-56 shim, exactly as a real build makes them:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-fnptr-table-lib.c -o %t.lib.o
//
// Table shard FIRST (position 0: its own `tu0_` tags are already correct,
// so no retag happens and this order has always worked).
// RUN: emitrust-cc --link %t.lib.o %t.main.o --emit=rust -o %t.libfirst.rs
// RUN: FileCheck %s --check-prefix=LIBFIRST --implicit-check-not=tu1_p < %t.libfirst.rs
// LIBFIRST-DAG: fn tu0_p_a(
// LIBFIRST-DAG: fn tu0_p_ab(
// LIBFIRST-DAG: fn tu0_p_b(
// LIBFIRST-DAG: static TU0_TBL: [Option<fn(i32) -> i32>; 3] = [Some(tu0_p_a), Some(tu0_p_ab), Some(tu0_p_b)];
// LIBFIRST-DAG: let v0: fn(i32) -> i32 = tu0_p_ab;
// LIBFIRST-DAG: std::cell::Cell::new(None)
// LIBFIRST-DAG: Mode::MODE_MUL
//
// Table shard SECOND (position 1: every `tu0_`/`TU0_` tag retags to 1).
// This is the regression -- the SAME shards, reordered, used to fail with
// "dangling function pointer target 'tu0_p_a'".
// RUN: emitrust-cc --link %t.main.o %t.lib.o --emit=rust -o %t.swapped.rs
// RUN: FileCheck %s --check-prefix=SWAPPED --implicit-check-not=tu0_p < %t.swapped.rs
// SWAPPED-DAG: fn tu1_p_a(
// SWAPPED-DAG: fn tu1_p_ab(
// SWAPPED-DAG: fn tu1_p_b(
//
// The prefix-related pair is the point: a substring rewrite that confused
// `tu1_p_a` with `tu1_p_ab` would still compile and dispatch to the WRONG
// function, so the whole table is pinned as one line, in order.
// SWAPPED-DAG: static TU1_TBL: [Option<fn(i32) -> i32>; 3] = [Some(tu1_p_a), Some(tu1_p_ab), Some(tu1_p_b)];
//
// The body-level `emitrust.constant` carrier retags too...
// SWAPPED-DAG: let v0: fn(i32) -> i32 = tu1_p_ab;
//
// ...and the opaque payloads that are NOT symbols survive verbatim.
// SWAPPED-DAG: std::cell::Cell::new(None)
// SWAPPED-DAG: Mode::MODE_MUL
//
// Order independence itself: modulo the ordinals (which ARE positional by
// design) the two crates are the same program, item for item.
// RUN: sed -e 's/tu[0-9][0-9]*_/tuN_/g' -e 's/TU[0-9][0-9]*_/TUN_/g' %t.libfirst.rs | sort > %t.libfirst.norm
// RUN: sed -e 's/tu[0-9][0-9]*_/tuN_/g' -e 's/TU[0-9][0-9]*_/TUN_/g' %t.swapped.rs | sort > %t.swapped.norm
// RUN: diff %t.libfirst.norm %t.swapped.norm

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
