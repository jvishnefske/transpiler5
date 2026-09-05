// REQUIRES: cargo
// FR-195, the FUNCTION-POINTER IDENTITY half. FR-52 records the trap this
// re-opened: "a function ADDRESS `Some(f)` is not a symbol use", so nothing
// downstream can see it and the emitted constant has to name the right item
// at the moment it is built. Three C spellings the FR-53 idiomatic rename
// folds onto ONE symbol (`my_fn`, `myFn`, `MyFn` -> `tu0_my_fn`) all
// emitted `Some(tu0_my_fn)` under `--incremental`, so `a == b` was TRUE for
// pointers to two DIFFERENT functions and a dispatch table keyed on fn-ptr
// equality collapsed silently: native `1 1000 1000000 / 0 0 0`, emitted
// `1 1 1 / 1 1 1`, exit 0, clean cargo build.
//
// Each loser now carries its own reserved symbol, so the three constants
// stay DISTINCT and the equality line byte-diffs against the clang native.
// Calling through a loser's pointer then reaches its `unimplemented!()`
// stub and stops the program with exit 101 -- loud, which is the whole
// point: the C source named `myFn` and nothing may resolve that onto
// `my_fn`.
//
// The comparison line is emitted BEFORE the failing call so both halves are
// observable in one run: identity restored, then loud failure. Values
// derive from argc so constant folding cannot hide a miscompile.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --build \
// RUN:   --crate-name fn_symbol_collision_fnptr 2>%t.err
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: clang %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: sh -c '%t.crate/target/release/fn_symbol_collision_fnptr \
// RUN:   > %t.rust.out 2> %t.rust.err; echo rc=$? > %t.rc'
// RUN: FileCheck %s --check-prefix=RC --input-file=%t.rc
// RUN: FileCheck %s --check-prefix=PANIC --input-file=%t.rust.err
// RUN: head -n 2 %t.native.out > %t.native.prefix
// RUN: diff %t.native.prefix %t.rust.out
// The three constants really are three different items in the crate.
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs

int printf(const char *, ...);

typedef int (*Fn)(int);

static int my_fn(int x) { return x + 1; }
static int myFn(int x) { return x + 1000; }
static int MyFn(int x) { return x + 1000000; }

int main(int argc, char **argv) {
  int s = argc; // 1 on a bare run, but the compiler cannot know that.
  Fn a = my_fn, b = myFn, c = MyFn;
  printf("equal=%d %d %d\n", a == b, a == c, b == c);
  printf("survivor=%d\n", a(s));
  printf("dropped=%d\n", b(s));
  printf("unreachable=%d\n", c(s));
  return 0;
}

// DIAG: warning: unsupported: function 'myFn' emits as 'tu0_my_fn', which collides with 'my_fn' (the idiomatic rename folds both spellings onto one symbol) (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG: warning: unsupported: function 'MyFn' emits as 'tu0_my_fn', which collides with 'my_fn' (the idiomatic rename folds both spellings onto one symbol) (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG-DAG: stubbed 'tu0_my_fn_collision1' [other]
// DIAG-DAG: stubbed 'tu0_my_fn_collision2' [other]

// RUST-DAG: Some(tu0_my_fn);
// RUST-DAG: Some(tu0_my_fn_collision1)
// RUST-DAG: Some(tu0_my_fn_collision2)

// RC: rc=101
// PANIC: not implemented: unsupported: function 'myFn' emits as 'tu0_my_fn', which collides with 'my_fn' (the idiomatic rename folds both spellings onto one symbol)
