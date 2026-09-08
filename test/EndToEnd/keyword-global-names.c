// REQUIRES: cargo
// Differential end-to-end test pinning that a C file-scope variable whose
// name is a RUST KEYWORD survives the FR-62 actor lowerings.
//
// The importer rejects a global whose EMITTED symbol is a Rust keyword
// (test/Import/C/globals-keyword.c pins that wording). Under the FR-53
// idiomatic rename that check never fires: `pub` emits as the global
// symbol `PUB`, which is no keyword. `emitrust-cc`'s actor-lift planner
// then derives the lowered spelling by snake_casing that symbol back --
// `PUB` -> `pub` -- and handed a bare Rust keyword to emission on two
// distinct paths:
//
//   (a) a driver-only global DEMOTED to a named `c_main` local
//       (`let mut pub: i32 = 5;`), and
//   (b) a multi-function global lifted to an actor struct FIELD
//       (`struct MatchActor { match: i32 }`, `self.match += n`,
//       `MatchActor { match: 7i32 }`, `match_actor.match`).
//
// Both made `emitrust-cc` exit 0 and the emitted crate fail to PARSE
// (measured: "error: expected identifier, found keyword `pub`"), which is
// the silently-unbuildable outcome the repo forbids. The fix mangles both
// with the single trailing underscore `mangleMemberName` already applies
// to struct members and keyword-spelled locals -- both spellings stay
// inside a namespace the emitted crate owns (the actor struct's fields,
// `c_main`'s bindings), so no cross-symbol naming policy moves.
//
// Two keyword globals per path (`pub`/`loop` demoted, `match`/`type`
// lifted) so a fix that special-cased one keyword is caught, plus a
// non-keyword global on each path (`plain` demoted, `total` lifted) as
// the negative control: their emitted spellings must not move, and any
// shift shows up as a stdout divergence here and as a golden failure in
// test/Driver/actor-lift-keyword-names.c.
//
// `cargo build` succeeding is only half the story -- the mangle must also
// keep every read and write on the SAME cell. Every value derives from
// argc so no constant fold can hide a crossed binding, and the second RUN
// pair re-seeds through extra argv words.
// --release is load-bearing: debug Rust panics on integer overflow where
// C wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/keyword_global_names > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/keyword_global_names a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

int printf(const char *, ...);

/* Driver-only globals: every use sits in main, so these demote to named
   `c_main` locals. Two are Rust keywords; `plain` is the control. */
int pub = 5;
int loop = 11;
int plain = 100;

/* Multi-function globals: touched by both `bump` and `main`, so these
   become fields of the lifted actor struct. Two are Rust keywords;
   `total` is the control. */
int match = 7;
int type = 13;
int total = 1000;

int bump(int n) {
  match += n;
  type += n * 2;
  total += match + type;
  return match + type + total;
}

int main(int argc, char **argv) {
  pub += argc;
  loop += argc * 3;
  plain += argc * 5;
  int b = bump(argc);
  /* Read every cell back: a mangle that crossed two bindings diverges
     here even though the crate builds. */
  printf("locals %d %d %d\n", pub, loop, plain);
  printf("actor  %d %d %d %d\n", match, type, total, b);
  return 0;
}
