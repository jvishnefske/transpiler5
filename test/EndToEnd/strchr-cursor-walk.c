// REQUIRES: cargo
// FR-230's strchr cursor bind, byte-diffed against the clang-built native.
// A FileCheck golden can only say the emitter produced the shape it was
// asked for; it cannot say the walk COUNTS THE SAME THINGS, and a cursor
// that is off by one or that restarts a search from the wrong index is a
// wrong ANSWER, not a build failure. So the oracle here is stdout.
//
// THE ENTROPY IS STANDARD INPUT, and it is load-bearing. Both the haystack
// and the NEEDLE are read from stdin, so neither clang nor rustc can fold
// any search: the whole walk has to run on both sides. (This is also the
// corpus case's own shape -- `028_strchr` fread()s its haystack.)
//
// WHAT THE VECTORS ARE CHOSEN TO CATCH:
//   - a match at index 0, which the flag-clamped payload would silently
//     confuse with "not found" if the clamp were applied without the flag;
//   - adjacent matches, which need the `s++` step to make progress;
//   - a match at the LAST byte, where the post-increment lands the cursor
//     exactly on the terminator;
//   - no match at all, an empty haystack, and an empty search region --
//     the three ways the -1 index reaches the arithmetic;
// DELIBERATELY ABSENT: walking on the needle 0. C's strchr matches the
// TERMINATOR, so `s = strchr(s, 0); s++` steps one past the end of the
// string on every iteration and runs off the object -- the clang native
// segfaults on it, which is the correct answer for undefined C and not a
// difference this oracle can compare. The counting rule for that shape is
// pinned by the deref guard in `strchr-cursor-bind.c` instead.
// The four loop spellings all compute the same count in C, so any disagreement
// between them is also caught -- one printf line carries all four.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name strchr_cursor_walk_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
//
// RUN: printf 'Aasdfaxxaasdxf' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// The corpus 028 vectors, with the needle byte prefixed.
// RUN: printf 'AasdfAXXAasdxf' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: printf 'xasdfAXXAasdxf\nanother line with an x' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// A match at index 0, and adjacent matches.
// RUN: printf 'AAAAB' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// A match at the very last byte.
// RUN: printf 'ZbcdZ' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// No match at all, and an empty haystack.
// RUN: printf 'qabcdef' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: printf 'q' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// Newlines and spaces as needles -- ordinary bytes to strchr.
// RUN: printf '\na\nb\nc\n' > %t.in
// RUN: %t.native < %t.in > %t.native.out
// RUN: %t.crate/target/release/strchr_cursor_walk_e2e < %t.in > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <string.h>

/* The corpus 028 loop, verbatim: the assignment IS the condition. */
static int count_for(const char *in, char c) {
  int res = 0;
  for (const char *s = in; s = strchr(s, c); s++)
    res++;
  return res;
}

/* The same walk with an explicit `!= NULL`. */
static int count_ne(const char *in, char c) {
  const char *s = in;
  int res = 0;
  while ((s = strchr(s, c)) != NULL) {
    res++;
    s++;
  }
  return res;
}

/* The same walk with a parenthesised truth test. */
static int count_while(const char *in, char c) {
  const char *s = in;
  int res = 0;
  while ((s = strchr(s, c))) {
    res++;
    s++;
  }
  return res;
}

/* The same walk with the assignment as a plain STATEMENT -- this one needs
   only the region source, not the assignment-as-rvalue. */
static int count_stmt(const char *in, char c) {
  const char *s = in;
  int res = 0;
  for (;;) {
    s = strchr(s, c);
    if (s == NULL)
      break;
    res++;
    s++;
  }
  return res;
}

/* strrchr: the last match, reported by the byte that FOLLOWS it so the
   answer depends on the cursor and not merely on "found or not". */
static int last_then_next(const char *in, char c) {
  const char *s = in;
  if ((s = strrchr(s, c)))
    return (int)(unsigned char)s[1];
  return -1;
}

/* The first match's own byte, through a single bind and a null test. */
static int first_byte(const char *in, char c) {
  const char *s = strchr(in, c);
  if (s == NULL)
    return -1;
  return (int)(unsigned char)*s;
}

int main(void) {
  char in[256];
  int needle;
  int n;

  memset(in, 0, sizeof(in));
  /* The needle is the FIRST byte of stdin; the haystack is the rest. */
  needle = getchar();
  if (needle < 0)
    return 1;
  n = (int)fread(in, 1, sizeof(in) - 1, stdin);
  in[n] = 0;

  printf("needle=%d len=%d\n", needle, n);
  printf("for=%d ne=%d while=%d stmt=%d\n", count_for(in, (char)needle),
         count_ne(in, (char)needle), count_while(in, (char)needle),
         count_stmt(in, (char)needle));
  printf("first=%d lastnext=%d\n", first_byte(in, (char)needle),
         last_then_next(in, (char)needle));
  return 0;
}
