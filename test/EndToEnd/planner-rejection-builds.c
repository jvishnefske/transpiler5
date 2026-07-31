// REQUIRES: cargo
// FR-53 end-to-end: the crate a recovered PLANNER rejection produces actually
// compiles and runs, and runs the same as the C.
//
// This is the claim that matters to a user, and it is the one the Driver test
// cannot make: `--incremental` writing a `src/main.rs` is worth nothing if
// cargo will not build it. The dropped item here is a `char **` writer outside
// the string-cursor shape -- before FR-53 the whole translation unit was
// rejected before the declaration walk began, so `emit_row`, `total` and
// `main` were lost with it and no crate existed to build.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --build 2>%t.err
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/planner_rejection_builds > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// The dropped item is named, and nothing else is.
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err

#include <stddef.h>

int printf(const char *, ...);

static int total(const int *values, int count) {
  int sum = 0;
  for (int index = 0; index < count; ++index)
    sum += values[index];
  return sum;
}

// Out of subset: the parameter is written through, which the cursor plan
// cannot represent. Never called, so dropping it costs the program nothing --
// which is exactly the situation that used to cost the program everything.
static void fill(unsigned char **out, size_t n) { (*out)[0] = (unsigned char)n; }

static void emit_row(const char *label, int value) {
  printf("%s=%d\n", label, value);
}

int main(void) {
  int values[4];
  values[0] = 3;
  values[1] = 5;
  values[2] = 7;
  values[3] = 11;
  emit_row("sum", total(values, 4));
  emit_row("first", values[0]);
  return 0;
}

// WARN: warning: unsupported: pointer-to-pointer parameter escapes the string-cursor shape (recovered: item dropped)
// WARN: recovered 1 rejected top-level item:
// WARN: dropped 'fill' [ptr-to-ptr]
