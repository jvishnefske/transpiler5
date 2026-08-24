// REQUIRES: cargo
// FR-129 half (b): the EXHAUSTIVE differential proof that the emitted Rust
// image of glibc's <ctype.h> classifiers answers exactly what glibc answers.
// The domain is finite -- 256 `unsigned char` values times twelve
// classifiers -- so the byte-diff oracle can cover ALL of it, and it does:
// every value is walked through every admitted classifier in boolean
// context, the twelve answers are packed into one bitmask per value, and
// the 256 masks are printed. `cargo build` success proves nothing here; the
// diff of this stdout against the clang-built native is the oracle, and it
// is what caught the one real trap in this mapping -- Rust's
// `is_ascii_whitespace` follows the WhatWG definition and EXCLUDES U+000B
// VERTICAL TAB, which C's `isspace` includes, so `isspace` must be spelled
// out rather than delegated to it.
//
// Also covered here, because each is an admitted boolean context whose Rust
// spelling differs: `!`, `&&`, `||`, the ternary condition, a `while`
// condition (the measured inih `ini_lskip` shape), and a `for` condition.
// EOF (-1) is walked too: C says the classifiers accept EOF and answer
// false, and the `as u8` image maps it to 255, which no ASCII predicate
// accepts.
//
// Every byte derives from argc so constant folding cannot hide a wrong
// mask-to-predicate pairing, a swapped table entry, or an off-by-one range;
// with argc == 1 the XOR is the identity, so all 256 values are still
// visited. Deterministic, no UB; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/ctype_classifiers > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <ctype.h>

int printf(const char *, ...);

// Every classifier in `if`-condition position. The result of each test is
// carried by a distinct bit, so a single wrong predicate shifts exactly one
// bit of exactly the values it disagrees on -- and the diff names them.
static unsigned classify(int c) {
  unsigned m = 0u;
  if (isspace(c))
    m |= 1u;
  if (isalpha(c))
    m |= 2u;
  if (isdigit(c))
    m |= 4u;
  if (isupper(c))
    m |= 8u;
  if (islower(c))
    m |= 16u;
  if (isalnum(c))
    m |= 32u;
  if (ispunct(c))
    m |= 64u;
  if (isxdigit(c))
    m |= 128u;
  if (isblank(c))
    m |= 256u;
  if (iscntrl(c))
    m |= 512u;
  if (isprint(c))
    m |= 1024u;
  if (isgraph(c))
    m |= 2048u;
  return m;
}

// The other admitted boolean contexts, each producing a bit of its own.
static unsigned contexts(int c) {
  unsigned m = 0u;
  if (!isspace(c))
    m |= 1u;
  if (isalpha(c) && islower(c))
    m |= 2u;
  if (isupper(c) || isdigit(c))
    m |= 4u;
  m |= isalnum(c) ? 8u : 0u;
  {
    int n = 0;
    for (; ispunct(c) && n < 3; ++n)
      ;
    m |= (unsigned)n << 4;
  }
  return m;
}

// The measured corpus shape: inih's `ini_lskip`, whose classifier sits in a
// `while` condition behind a `&&`.
static int lskip(const char *s) {
  int n = 0;
  while (*s && isspace((unsigned char)(*s))) {
    ++s;
    ++n;
  }
  return n;
}

int main(int argc, char **argv) {
  unsigned i;
  unsigned seed = (unsigned)argc - 1u;
  char buf[8];
  for (i = 0u; i < 256u; ++i) {
    int c = (int)(unsigned char)(i ^ seed);
    printf("%3d %4x %2x\n", c, classify(c), contexts(c));
  }
  // EOF is in the classifiers' C domain and every one of them answers
  // false; -1 seeded from argc so it is not a literal at the call.
  printf("eof %x %x\n", classify(-(int)argc), contexts(-(int)argc));
  // A leading run of every C whitespace byte, including the vertical tab
  // that Rust's `is_ascii_whitespace` would drop.
  buf[0] = (char)(9 + (int)argc - 1);
  buf[1] = (char)10;
  buf[2] = (char)11;
  buf[3] = (char)12;
  buf[4] = (char)13;
  buf[5] = (char)32;
  buf[6] = (char)('x' + (int)argc - 1);
  buf[7] = (char)0;
  printf("lskip %d\n", lskip(buf));
  return 0;
}
