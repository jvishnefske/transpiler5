// FR-150 crate-root golden for the POSITIVE half, and the INDEPENDENCE pin: a
// crate may shadow more than one prelude name, and each is handled on its own.
//
// This crate names a C type `option` and another `string` -- both collide
// after `toUpperCamelCase` -- but names nothing `vec`. So `Option` and
// `String` must be written as fully qualified paths at every EMITTER-written
// site, while `Vec` stays bare in the very same file, because qualifying a
// name nothing shadows would shift bytes for no reason (see
// test/Driver/prelude-shadow-neutral.c, the byte-identity guard).
//
// The paths are `::std::...` and not `::core::...`/`::alloc::...` because
// emitted crates are std crates (`std::process::exit`, `println!`,
// `std::io::stdout`): `alloc` is NOT in the extern prelude of a std crate
// without an `extern crate alloc;`, so an `::alloc::boxed::Box` would not
// resolve. The leading `::` makes the path absolute, so an emitted item named
// `Std` (or a module of any name) cannot capture it.
//
// What must NOT move, and is pinned line for line below:
//   * the USER's own `Option` and `String` types keep their bare emitted
//     names in the struct_def, the `let` annotation and the struct literal --
//     the emitted name is part of the API surface (FR-139's `--c-abi-exports`
//     and the lib-crate story both depend on emitted names matching what a
//     caller expects), which is why this FR qualifies the emitter's uses
//     instead of renaming the user's type;
//   * `Vec` stays bare in `&[Vec<i8>]`, in the verbatim `__emitrust_cstr_out`
//     helper, and in the emitrust-cc DRIVER's `fn main()` wrapper.
//
// RUN: emitrust-cc --emit=rust %s -o - \
// RUN:   | FileCheck %s --strict-whitespace --match-full-lines
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s --check-prefix=NOVEC

#include <stdlib.h>
int printf(const char *, ...);
int puts(const char *);

typedef struct option { int id; } option;
typedef struct string { int n; } string;

struct ops { int (*apply)(int); int tag; };

static int inc(int x) { return x + 1; }
static int apply(int (*h)(int), int v) { return h(v); }

static void fill(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = 'z';
  a[len] = '\0';
  puts(a);
  free(a);
}

int main(int argc, char **argv) {
  option o;
  o.id = argc;
  string t;
  t.n = argc + 1;
  struct ops s;
  s.apply = inc;
  s.tag = 2;
  printf("a=%d b=%d n=%d\n", apply(inc, o.id), s.apply(o.id) + s.tag, t.n);
  fill((unsigned int)t.n);
  for (int i = 1; i < argc; i++)
    printf("[%s]\n", argv[i]);
  return 0;
}

// FR-220: the crate root's blanket `#![allow(dead_code)]` and the blank line
// after it are gone, replaced by a targeted `#[allow(dead_code)]` on each of
// the three `struct_def`s. Attribute POSITION only -- every qualified prelude
// path this file exists to pin (`::std::option::Option`, `::std::string::`,
// `::std::vec::`) is byte-identical, and the full-width CHECK-NEXT chain is
// what proves the qualification decision did not move with the attribute.
// CHECK:#[allow(dead_code)]
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct Option {
// CHECK-NEXT:    id: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[allow(dead_code)]
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct String {
// CHECK-NEXT:    n: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[allow(dead_code)]
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct Ops {
// CHECK-NEXT:    apply: ::std::option::Option<fn(i32) -> i32>,
// CHECK-NEXT:    tag: i32,
// CHECK-NEXT:}
// CHECK-NEXT:fn tu0_inc(x: i32) -> i32 {
// CHECK-NEXT:    x + 1i32
// CHECK-NEXT:}
// CHECK-NEXT:fn tu0_apply(v0: ::std::option::Option<fn(i32) -> i32>, v: i32) -> i32 {
// CHECK-NEXT:    let h: ::std::option::Option<fn(i32) -> i32> = v0;
// CHECK-NEXT:    h.expect("null function pointer")(v)
// CHECK-NEXT:}
// CHECK-NEXT:fn tu0_fill(v0: u32) {
// CHECK-NEXT:    let len: u32 = v0;
// CHECK-NEXT:    let a: ::std::string::String = "z".repeat(len as i64 as usize);
// CHECK-NEXT:    println!("{}", a);
// CHECK-NEXT:}
// CHECK-NEXT:fn c_main(argc: i32, argv: &[Vec<i8>]) -> i32 {
// CHECK-NEXT:    let o: Option = Option { id: argc, };
// CHECK-NEXT:    let t: String = String { n: argc + 1i32, };
// CHECK-NEXT:    let mut s: Ops = Ops::default();
// CHECK-NEXT:    let v4: ::std::option::Option<fn(i32) -> i32> = Some(tu0_inc);
// CHECK-NEXT:    s.apply = v4;
// CHECK-NEXT:    s.tag = 2i32;
// CHECK-NEXT:    let v5: ::std::option::Option<fn(i32) -> i32> = Some(tu0_inc);
// CHECK-NEXT:    let v7: i32 = tu0_apply(v5, o.id);
// CHECK-NEXT:    let v10: i32 = s.apply.expect("null function pointer")(o.id);
// CHECK-NEXT:    println!("a={} b={} n={}", v7, v10 + s.tag, t.n);
// CHECK-NEXT:    tu0_fill(t.n as u32);
// CHECK-NEXT:    for i in 1i32..argc {
// CHECK-NEXT:        print!("[");
// CHECK-NEXT:        let v16: &[i8] = &argv[i as usize][..];
// CHECK-NEXT:        __emitrust_cstr_out(v16);
// CHECK-NEXT:        println!("]");
// CHECK-NEXT:    }
// CHECK-NEXT:    0i32
// CHECK-NEXT:}
// CHECK-NEXT:fn __emitrust_cstr_out(s: &[i8]) {
// CHECK-NEXT:    use std::io::Write;
// CHECK-NEXT:    let end = s.iter().position(|&b| b == 0).unwrap_or(s.len());
// CHECK-NEXT:    let bytes: Vec<u8> = s[..end].iter().map(|&b| b as u8).collect();
// CHECK-NEXT:    std::io::stdout().write_all(&bytes).expect("stdout write failed");
// CHECK-NEXT:}
// CHECK-EMPTY:
// CHECK-NEXT:fn main() {
// CHECK-NEXT:    use std::os::unix::ffi::OsStrExt;
// CHECK-NEXT:    let __emitrust_argv: Vec<Vec<i8>> = std::env::args_os()
// CHECK-NEXT:        .map(|a| {
// CHECK-NEXT:            a.as_bytes().iter().map(|&b| b as i8).chain(std::iter::once(0i8)).collect()
// CHECK-NEXT:        })
// CHECK-NEXT:        .collect();
// CHECK-NEXT:    std::process::exit(c_main(__emitrust_argv.len() as i32, &__emitrust_argv));
// CHECK-NEXT:}

// `Vec` is shadowed by nothing in this crate, so it is never qualified --
// each prelude name is decided independently.
// NOVEC-NOT: ::std::vec::
// NOVEC-NOT: ::std::boxed::
