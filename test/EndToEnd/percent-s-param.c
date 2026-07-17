// REQUIRES: cargo
// CTS-L2 (design.md): differential regression test for printf %s of a
// `char *` function parameter (the FR-28 slice-parameter class). String
// literals and char arrays are passed through functions to printf %s and
// puts: straight pass-through, a cursor-advanced parameter (printing the
// suffix), an `&arr[i]` argument (nonzero call-site cursor), a literal
// with an embedded NUL (C stops printing there), repeated literal call
// sites, and C main's (argc, argv) form with argv unused — argc is
// printed and must match the native binary's (both run with no
// arguments). Byte-identical stdout and exit codes against the
// clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name percent_s_param_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/percent_s_param_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int puts(const char *);

static void show(const char *s) {
  printf("[%s]\n", s);
}

static void tail(const char *s) {
  s++;
  s++;
  printf("tail=%s\n", s);
  puts(s);
}

int main(int argc, char **argv) {
  char buf[8] = "abcdef";
  char cut[6] = "pq";
  show("literal");
  show("literal");
  show(buf);
  show(&buf[3]);
  tail("wxyz");
  tail(buf);
  show("nul\0hidden");
  show(cut);
  printf("argc=%d\n", argc);
  return 0;
}
