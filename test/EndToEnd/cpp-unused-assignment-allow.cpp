// REQUIRES: cargo
// FR-106 (C++ leg): the `#[allow(unused_assignments)]` must land on the
// METHOD inside its `impl`, at the method's own indentation.
//
// Same reduced heatshrink `find_longest_match` shape as
// test/EndToEnd/unused-assignment-allow.c, but as a struct method, so the
// item being prefixed is nested four spaces deep. The insertion machinery
// is FR-140's (render the item, then prefix it at its own indentation);
// this pins that it is indentation-correct for a nested item and that the
// attribute is honoured there -- an attribute placed on the enclosing
// `impl` instead would not suppress the lint on the method's body, and the
// FR-53 crate deny would still fail the build.
//
// The native leg is clang++ (the source is C++); `printf` is declared
// `extern "C"` so the native build links libc's printf. All seeds derive
// from argc so constant folding cannot pre-compute the scan; the stdout
// diff against the native is the correctness oracle, not `cargo build`.
// Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_unused_assignment_allow > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/cpp_unused_assignment_allow a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out
//
// Exactly one allow, on the method, indented with it.
// RUN: FileCheck %s < %t.crate/src/main.rs
// CHECK-NOT:      allow(unused_assignments)
// CHECK:          impl Matcher {
// CHECK:          {{^}}    #[allow(unused_assignments)]
// CHECK-NEXT:     {{^}}    fn scan(
// CHECK-NOT:      allow(unused_assignments)

extern "C" int printf(const char *, ...);

static unsigned char buf[64];
static short idx[32];

struct Matcher {
  unsigned short maxlen;
  unsigned short scan(unsigned short start, unsigned short end) {
    unsigned short match_maxlen = 0;
    unsigned short match_index = 0xFFFF;
    unsigned short len = 0;
    unsigned char *const needlepoint = &buf[end];
    short pos = idx[end];
    while (pos - (short)start >= 0) {
      unsigned char *const pospoint = &buf[pos];
      /* Dead on the `continue` path below, live on every other. */
      len = 0;
      if (pospoint[match_maxlen] != needlepoint[match_maxlen]) {
        pos = idx[pos];
        continue;
      }
      for (len = 1; len < maxlen; len++) {
        if (pospoint[len] != needlepoint[len]) break;
      }
      if (len > match_maxlen) {
        match_maxlen = len;
        match_index = pos;
        if (len == maxlen) break;
      }
      pos = idx[pos];
    }
    printf("maxlen=%u index=%u\n", match_maxlen, match_index);
    return match_index;
  }
};

int main(int argc, char **argv) {
  int i;
  /* 64 wide so `pospoint[len]` (pos < 32, len <= 8) stays in bounds. */
  for (i = 0; i < 64; i++) buf[i] = (unsigned char)(argc * 7u + i % 5u);
  for (i = 0; i < 32; i++) idx[i] = (short)(i - argc * 3);
  Matcher m;
  m.maxlen = 8;
  printf("r=%u\n", m.scan(0, (unsigned short)(16 + argc)));
  m.maxlen = 4;
  printf("r=%u\n", m.scan(1, (unsigned short)(20 - argc)));
  return 0;
}
