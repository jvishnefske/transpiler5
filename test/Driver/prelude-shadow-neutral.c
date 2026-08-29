// FR-150 BYTE-IDENTITY GUARD at the CRATE-ROOT level, and the load-bearing
// test of the whole change: a crate that shadows NO prelude name must emit
// byte-identical output -- bare `Option<..>`, bare `Vec<..>`, bare `String`,
// no qualification anywhere, not one shifted byte or line.
//
// FR-150 qualifies the emitter's own prelude spellings ONLY in a crate where
// an emitted item shadows that name. That condition is the entire safety
// argument: unconditional qualification would shift emitted bytes across the
// whole corpus (`--emit=crate` output is pinned byte-for-byte by golden tests,
// so that is a behavior change, not a cleanup) and would make every fn-ptr
// signature unreadable, for a defect that affects a handful of crates. This
// file states the byte-neutral half directly.
//
// It also pins the C-SPELLING boundary. Emitted type names go through
// `toUpperCamelCase`, which DROPS underscores, so `my_option` becomes
// `MyOption` and `my_string` becomes `MyString`: neither collides, and a
// detector that keyed off the C spelling (or off a substring) would qualify
// here and fail.
//
// Every prelude-writing site the emitter has is present and must stay bare:
//   * `Option<fn(..)>` in a struct FIELD, a fn-ptr PARAMETER, a `let`
//     annotation and a temporary,
//   * the FR-64 `String` binding,
//   * the C99-43 C3 argv table `&[Vec<i8>]`,
//   * the emitter's verbatim `__emitrust_cstr_out` helper (`Vec<u8>`),
//   * and the emitrust-cc DRIVER's own `fn main()` wrapper
//     (`let __emitrust_argv: Vec<Vec<i8>> = ..`), which is a SEPARATE
//     rendering site outside the emitter and needs its own neutrality.
//
// The whole crate root is pinned line by line under --strict-whitespace, and a
// second unanchored scan says outright that no qualified prelude path appears
// anywhere -- a CHECK-NOT only guards the span between its neighbours, so the
// full-line pin cannot say that on its own.
//
// RUN: emitrust-cc --emit=rust %s -o - \
// RUN:   | FileCheck %s --strict-whitespace --match-full-lines
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s --check-prefix=BARE

#include <stdlib.h>
int printf(const char *, ...);
int puts(const char *);

typedef struct my_option { int id; } my_option;
typedef struct my_string { int n; } my_string;

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
  my_option o;
  o.id = argc;
  my_string t;
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

// CHECK:#![allow(dead_code)]
// CHECK-EMPTY:
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct MyOption {
// CHECK-NEXT:    id: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct MyString {
// CHECK-NEXT:    n: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct Ops {
// CHECK-NEXT:    apply: Option<fn(i32) -> i32>,
// CHECK-NEXT:    tag: i32,
// CHECK-NEXT:}
// CHECK-NEXT:fn tu0_inc(x: i32) -> i32 {
// CHECK-NEXT:    x + 1i32
// CHECK-NEXT:}
// CHECK-NEXT:fn tu0_apply(v0: Option<fn(i32) -> i32>, v: i32) -> i32 {
// CHECK-NEXT:    let h: Option<fn(i32) -> i32> = v0;
// CHECK-NEXT:    h.expect("null function pointer")(v)
// CHECK-NEXT:}
// CHECK-NEXT:fn tu0_fill(v0: u32) {
// CHECK-NEXT:    let len: u32 = v0;
// CHECK-NEXT:    let a: String = "z".repeat(len as i64 as usize);
// CHECK-NEXT:    println!("{}", a);
// CHECK-NEXT:}
// CHECK-NEXT:fn c_main(argc: i32, argv: &[Vec<i8>]) -> i32 {
// CHECK-NEXT:    let o: MyOption = MyOption { id: argc, };
// CHECK-NEXT:    let t: MyString = MyString { n: argc + 1i32, };
// CHECK-NEXT:    let mut s: Ops = Ops::default();
// CHECK-NEXT:    let v4: Option<fn(i32) -> i32> = Some(tu0_inc);
// CHECK-NEXT:    s.apply = v4;
// CHECK-NEXT:    s.tag = 2i32;
// CHECK-NEXT:    let v5: Option<fn(i32) -> i32> = Some(tu0_inc);
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

// Not one qualified prelude path anywhere in the crate root.
// BARE-NOT: ::std::option::
// BARE-NOT: ::std::vec::
// BARE-NOT: ::std::string::
// BARE-NOT: ::std::boxed::
