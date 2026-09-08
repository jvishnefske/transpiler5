// REQUIRES: cargo
// FR-192: a `%s` argument is CONVERTED -- the pointee read -- when printf
// reaches the directive, not when the argument is evaluated. C17 6.5.2.2p10
// puts a sequence point before the call, and the array-to-pointer decay of a
// buffer does NOT access the array's stored value, so a LATER argument whose
// call writes that buffer is well defined and its store IS visible to the
// conversion: `printf("[%s] %d\n", buf, bump(buf))` prints `[Bb] 1`, not
// `[ab] 1`. (Contrast a `%d` argument, whose value IS read during argument
// evaluation -- a later argument writing THAT object is an unsequenced
// read/write conflict and plain UB, so no such shape is pinned here.)
// The importer used to materialize the `%s` String at argument-lowering time,
// which is before the later argument's call, so the write was lost -- a
// measured miscompile: native `5b42625d20310a`, emitted `5b61625d20310a`.
// This pins, as observable BYTES, every `%s` shape the importer admits in
// front of a side-effecting argument: a local buffer, a file-scope buffer, a
// mid-buffer `&buf[i]`, a `%.Ns` precision, two `%s` holes before one write,
// an `argv` element (whose raw-bytes bypass ALSO used to write its output
// before the later argument ran, inverting the two writes: native
// `<1>[hello] 1`, emitted `[hello<1>] 1`), an argv `%c` BYTE (the same
// unfenced bypass, same inversion: native `<1>[h] 1`, emitted `[h<1>] 1`),
// the `sprintf` buffer context, and the interleaving where a deferred `%s`
// and a raw `%c`/`%s` bypass share ONE call, so a segment flush lands while
// a deferred operand slot is still unfilled.
// Two controls ride along to pin that nothing moved where nothing may: a
// `%s` whose later argument is a call that does NOT write the buffer, and a
// `%s` with no later argument at all.
// A FileCheck of the IR cannot catch this class -- only the diff can.
// Every payload byte is derived from argc and the test runs with three
// different argument vectors, so neither clang nor rustc can constant-fold
// the buffer and hide a miscompile behind a folded literal.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_string_arg_write_order_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native hello > %t.native.out
// RUN: %t.crate/target/release/printf_string_arg_write_order_e2e hello > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native world x > %t.native.out
// RUN: %t.crate/target/release/printf_string_arg_write_order_e2e world x > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native zz a b c > %t.native.out
// RUN: %t.crate/target/release/printf_string_arg_write_order_e2e zz a b c > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int sprintf(char *, const char *, ...);

/* A file-scope char buffer: the staged-global-copy `%s` shape, whose
   snapshot happened one step earlier still (the whole global was copied
   out before the directive was even reached). */
char gbuf[8];

static int seq;

/* Writes into the caller's buffer and returns a value the format prints:
   the side effect C sequences BEFORE the conversion of an earlier `%s`. */
static int bump(char *p, int c) {
  p[0] = (char)c;
  return 1;
}

/* Writes the file-scope buffer instead. */
static int bump_global(int c) {
  gbuf[0] = (char)c;
  return 2;
}

/* A later argument that writes stdout itself: its output must land BEFORE
   this call's, because C evaluates every argument before printf writes a
   byte. */
static int noisy(void) {
  seq = seq + 1;
  printf("<%d>", seq);
  return seq;
}

/* A control: a later argument that is a call but writes no buffer. */
static int pure_count(int argc) { return argc * 10; }

int main(int argc, char **argv) {
  char buf[8];
  char two[8];
  char out[32];
  char ch;
  int i;

  /* argc-seeded ASCII payloads: 'a'+argc .. so nothing folds. */
  for (i = 0; i < 6; i++)
    buf[i] = (char)('a' + argc + i);
  buf[6] = '\0';
  for (i = 0; i < 4; i++)
    gbuf[i] = (char)('p' + argc + i);
  gbuf[4] = '\0';
  for (i = 0; i < 4; i++)
    two[i] = (char)('A' + argc + i);
  two[4] = '\0';
  ch = (char)('0' + argc);

  /* 1. The headline shape: a whole local array, then a call writing it. */
  printf("1[%s] %d\n", buf, bump(buf, 'Z' - argc));

  /* 2. The file-scope buffer, whose copy-out is even earlier. */
  printf("2[%s] %d\n", gbuf, bump_global('Q' - argc));

  /* 3. A mid-buffer `&buf[i]` argument. */
  printf("3[%s] %d\n", &buf[2], bump(&buf[2], 'Y' - argc));

  /* 4. `%.3s`: the precision-bounded raw run. */
  printf("4[%.3s] %d\n", buf, bump(buf, 'X' - argc));

  /* 5. Two `%s` holes in front of ONE write: both are converted after it. */
  printf("5[%s][%s] %d\n", buf, two, bump(buf, 'W' - argc));

  /* 6. An `argv` element in front of a stdout-writing argument: the two
        writes must not invert. */
  printf("6[%s] %d\n", argv[1], noisy());

  /* 7. Control -- a later call that writes no buffer. */
  printf("7[%s] %d\n", buf, pure_count(argc));

  /* 8. Control -- no later argument at all. */
  printf("8[%s]\n", buf);

  /* 9. The `sprintf` buffer context shares the directive grammar. */
  sprintf(out, "9[%s] %d", buf, bump(buf, 'V' - argc));
  printf("%s\n", out);

  /* 10. The argv `%c` byte bypass, which carried the same missing fence:
         `<n>` must precede this line's own output. */
  printf("10[%c] %d\n", argv[1][0], noisy());

  /* 11. The interleaving that stresses the deferred-slot bookkeeping: a
         deferred `%s` (a write is still to come) followed by a `%c` whose
         OWN raw bypass fires -- nothing side-effecting follows IT -- and so
         flushes the very format segment that still holds the deferred
         slot. The deferred read must be materialized before that flush, and
         after the write. */
  printf("11[%s] %d <%c>\n", buf, bump(buf, 'T' - argc), ch);

  /* 12. The same, plus a trailing `%s` past the write that still takes the
         raw bypass: one call mixing a deferred hole and a raw one. */
  printf("12[%s] %d <%c> [%s]\n", buf, bump(buf, 'S' - argc), ch, buf);

  return 0;
}
