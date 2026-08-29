// REQUIRES: cargo
// FR-150, the `String` leg, byte-diffed. The FR-64 malloc-string-fill lowering
// binds an owned Rust `String`, so a C type named `string` shadows the prelude
// name in the very crate that needs it and the emitted
// `let a: String = "a".repeat(len as usize);` stops type-checking (rustc E0308,
// then E0277 "`String` doesn't implement `std::fmt::Display`" at the print).
// Exit 0, no diagnostic, unbuildable crate -- the FR-150 class.
//
// `toUpperCamelCase` drops underscores, so C's `string` collides and
// `my_string` does not; both are in this crate.
//
// The `String` spellings reached here: the FR-64 binding's type annotation and
// the `String` opaque flowing into `println!` by Display. The fill count comes
// from argc so nothing folds at import time, and the len == 0 case (the empty
// string) is covered because it is where an off-by-one in the fused lowering
// would show. The oracle is the byte-diff against the clang-built native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/prelude_shadow_string > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native one two > %t.native2.out
// RUN: %t.crate/target/release/prelude_shadow_string one two > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

#include <stdlib.h>

int puts(const char *);
int printf(const char *, ...);

// The colliding type. `string` -> `String` under toUpperCamelCase.
typedef struct string {
  int len;
  int cap;
} string;

// The underscore twin, which does NOT collide (`MyString`).
struct my_string {
  int len;
};

// The FR-64 constant-fill idiom: malloc(len+1), a canonical [0,len) counted
// fill loop of a compile-time-constant ASCII byte, a NUL terminator, a string
// consumer and a free. Lifts whole to `let a: String = "a".repeat(..)`.
static void alloc_string_a(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = 'a';
  a[len] = '\0';
  puts(a);
  free(a);
}

int main(int argc, char **argv) {
  string s;
  s.len = argc;
  s.cap = argc * 3;

  struct my_string m;
  m.len = s.cap - s.len;

  printf("len=%d cap=%d mlen=%d\n", s.len, s.cap, m.len);

  alloc_string_a((unsigned int)s.len);       // len = argc (>= 1)
  alloc_string_a((unsigned int)(s.cap + 1)); // a wider multi-byte fill
  alloc_string_a(0u);                        // len == 0: the empty string
  return 0;
}
