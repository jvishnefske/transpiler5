// REQUIRES: cargo
// FR-195, the RUNTIME oracle for the silent call-site rebind. `Compute` and
// `compute` are two distinct C identifiers whose emitted symbols the FR-53
// idiomatic rename folds onto ONE spelling (`tu0_compute`). Plain mode
// refuses them; under `--recover` (which `--incremental` implies) the loser
// used to be DROPPED with the survivor left owning the folded symbol, so
// every `compute(...)` call resolved onto `Compute`'s body -- emitrust-cc
// exit 0, cargo build clean with zero warnings, binary exit 0, WRONG ANSWER
// (native `dropped=7`, emitted `dropped=4`). That is precisely the
// contract CLAUDE.md and FR-53 state: a dropped or recovered item must
// never be silently observable.
//
// A FileCheck of the emitted Rust cannot tell a silently-rebound call from
// a correct one -- both are a well-typed call to an existing item -- so the
// pin here is the RUNTIME behavior, in three parts:
//   (a) the SURVIVOR still works: its two lines are byte-diffed against the
//       clang native's, which is the only oracle that can see a miscompile;
//   (b) the loser's call FAILS LOUDLY: exit code 101, with the original
//       located diagnostic carried into the panic payload;
//   (c) nothing is printed after the panic, i.e. the program really stopped
//       rather than continuing on a wrong value.
// Every printed value derives from argc, so constant folding cannot
// pre-compute the answers and hide a miscompile behind a clean build.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --build \
// RUN:   --crate-name fn_symbol_collision_recover 2>%t.err
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: clang %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: sh -c '%t.crate/target/release/fn_symbol_collision_recover \
// RUN:   > %t.rust.out 2> %t.rust.err; echo rc=$? > %t.rc'
// RUN: FileCheck %s --check-prefix=RC --input-file=%t.rc
// RUN: FileCheck %s --check-prefix=PANIC --input-file=%t.rust.err
// (a)+(c): the emitted stdout is EXACTLY the native's first two lines --
// not a prefix check, a byte diff, so an extra or altered line fails.
// RUN: head -n 2 %t.native.out > %t.native.prefix
// RUN: diff %t.native.prefix %t.rust.out

int printf(const char *, ...);

static int Compute(int x) { return x * 3 + 1; }
static int compute(int x) { return x * 5 + 2; }

int main(int argc, char **argv) {
  int s = argc; // 1 on a bare run, but the compiler cannot know that.
  printf("survivor=%d\n", Compute(s));
  printf("survivor2=%d\n", Compute(s + 41));
  printf("dropped=%d\n", compute(s));
  printf("unreachable=%d\n", Compute(s + 100));
  return 0;
}

// The loser is STUBBED under a symbol of its own, never dropped with the
// survivor left holding the folded name.
// DIAG: warning: unsupported: function 'compute' emits as 'tu0_compute', which collides with 'Compute' (the idiomatic rename folds both spellings onto one symbol) (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG: stubbed 'tu0_compute_collision1' [other]

// RC: rc=101
// PANIC: not implemented: unsupported: function 'compute' emits as 'tu0_compute', which collides with 'Compute' (the idiomatic rename folds both spellings onto one symbol)
