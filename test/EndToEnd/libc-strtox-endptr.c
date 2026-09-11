// REQUIRES: cargo
// FR-234 rung 2: `strtol`/`strtoul`/`strtod` with a REAL `endptr`
// out-parameter, byte-diffed against the clang-built native.
//
// WHY THIS FILE EXISTS AND WHAT IT PINS. Rung 1 proved the VALUE half of
// the strto* family. The `endptr` half is a SECOND answer the same call
// has to produce, and it is the half a plausible implementation gets
// wrong silently: the emitted crate can return the right `long` and put
// the cursor one byte off, and `cargo build` sees nothing, no FileCheck
// over the emitted Rust sees it either, and every rejection test in the
// tree stays green. Only this diff can see it. The invariant is:
//
//   for every subject string and every base, the emitted crate's
//   `end - buf`, `*end`, `end[0]` and `end == buf` agree with glibc's
//   BYTE FOR BYTE.
//
// The rows below are chosen so that each is a place a from-scratch
// endptr gets it wrong:
//   1 NO CONVERSION is the one C nails down in the opposite direction
//     from everything else: when the subject sequence is empty, C 7.22.1.4p7
//     stores `nptr` ITSELF -- NOT the position after the whitespace the
//     scanner already ate, and not "unchanged". So `strtol("   ", &end, 10)`
//     leaves `end == buf` with offset 0, and `strtol("\t\n\v\f\r 42", ...)`
//     in a base where `4` is not a digit (base 2) ALSO leaves offset 0
//     even though six bytes were scanned. Rows 6/8/9/10/11/43/44/45 and the
//     base-2 column carry it; it is the case the join is most likely to
//     approximate, so it gets its own explicit `end == buf` column.
//   2 THE CONDITIONAL `0x` PREFIX moves the endptr, not just the value:
//     `strtol("0x", &end, 16)` converts the single `0` and leaves `end` on
//     the `x` (offset 1), where an unconditional two-character skip would
//     leave offset 2 with the same returned value 0 -- a divergence
//     INVISIBLE to rung 1's value-only diff. Rows 12 and 15.
//   3 OVERFLOW STILL ADVANCES. C's subject sequence is the whole run of
//     digits whatever the accumulator does, so `strtol("99999999999999999999999")`
//     saturates to LONG_MAX *and* leaves `end` at offset 23. An
//     implementation that bails out of the digit loop on overflow returns
//     the same value with the wrong cursor. Rows 33/34.
//   4 FULL CONSUMPTION lands `end` on the NUL, so `*end` is 0 and
//     `end - buf` is strlen. Rows 0/1/2/3.
//   5 TRAILING GARBAGE lands `end` mid-string (`42x` -> offset 2, `*end`
//     is 'x'). Rows 6/38/44.
//
// AN OUT-OF-RANGE BASE IS DELIBERATELY ABSENT FROM THIS TABLE, and the
// reason is a MEASUREMENT that corrected the plan for this file rather
// than a scoping preference. Rung 1's value-only table carries bases 1
// and 37 as a platform-refinement pin because glibc answers 0 for them.
// It does NOT answer anything for the endptr: probed directly, glibc's
// `strtol(s, &e, 1)` and `strtol(s, &e, 37)` leave `*endptr` COMPLETELY
// UNTOUCHED (an `e` preloaded with 0xdeadbeef still reads 0xdeadbeef
// afterwards), so with `end` reused across rows the native prints the
// PREVIOUS row's cursor and there is no defined answer to diff against.
// C 7.22.1.4p2 makes the whole call undefined, so neither side is wrong.
// The emitted crate stores `nptr` (offset 0) in that case, which is a
// deliberate REFINEMENT in the safe direction: glibc's non-write leaves
// an uninitialized `char *end;` uninitialized and readable, and the
// decomposed cursor can never be in that state.
//   7 strtod's endptr rides the SAME prefix scan atof already implements
//     (C 7.22.1.1p2 defines `atof(s)` as `strtod(s, NULL)`), so the `D`
//     rows pin that the float grammar's consumed length -- the exponent
//     BACKTRACK in particular (`1e` consumes just `1`, `3.5xyz` stops at
//     `x`) -- matches glibc too. Rows 14-19 are the `inf`/`nan` WORD
//     LENGTHS, the one part of that grammar whose cursor is neither the
//     mantissa scan nor a no-conversion zero: `infinity` consumes 8 and
//     `infin` consumes 3, so a lowering that advanced by the matched
//     prefix count would answer 5 for the second.
//
// ONE `char *end` SERVES ALL THREE FUNCTIONS on purpose: the region join
// that admits `&end` must survive the pointer being written by three
// different calls in one function, which is the shape the corpus actually
// uses.
//
// THE `Q` SECTION is the self-aliasing call `strtol(q, &q, 0)`, the only
// place here where the endptr's own cell is ALSO the subject string's
// cursor. It is the one shape whose answer depends on the lowering reading
// argument 0 before storing the result, and no table row can see that.
//
// TWO SMALL SECTIONS FOLLOW THE TABLES because nothing in the tables can
// reach them. The `O` rows parse from PART-WAY INTO a region, which is
// the only thing that can catch a lowering that stores the callee's
// answer raw instead of adding argument 0's own cursor -- that bug is
// byte-identical on every table row above. The `N` rows give `end` a NULL
// initializer, which gives the emitted pointer a runtime non-null flag
// that C requires to be true after the call even on no conversion.
//
// NOT HERE, and it cannot be: the hexadecimal-float and NaN-payload forms
// of strtod PANIC (FR-235), so no byte-diff can hold them; they stay in
// `libc-atof-loud-stop.c`. `errno` is not modelled at all, so the ERANGE
// half of C's overflow contract has no image -- what is observable is the
// returned value and the cursor, which is what rows 25-34 pin.
//
// Every subject string is copied out of the table into a local buffer at
// a row index derived from argc, and the base is read at an argc-derived
// index too, so neither compiler is handed a constant argument and no
// folded literal can stand in for a working parse or a working cursor.
// With argc == 1 both offsets are 0, so all 46 x 6 x 2 + 20 lines are
// still produced. Deterministic, with NO UB in the C at all; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name libc_strtox_endptr --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/libc_strtox_endptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <stdlib.h>

#define NFORMS 46
#define WIDTH 26
#define NBASES 6

/* NUL-padded rows: every row is WIDTH bytes, so copying the whole row
   always yields a NUL-terminated buffer. */
static const char forms[NFORMS][WIDTH] = {
    "0",
    "42",
    "-42",
    "+42",
    "  42",
    "\t\n\v\f\r 42",
    "42x",
    "x42",
    "",
    " ",
    "-",
    "+",
    "0x",
    "0x1f",
    "0X1F",
    "0xg",
    "010",
    "-010",
    "0b101",
    "z",
    "Z",
    "zz",
    "-0",
    "-0x10",
    "9223372036854775807",
    "9223372036854775808",
    "-9223372036854775808",
    "-9223372036854775809",
    "18446744073709551615",
    "18446744073709551616",
    "-1",
    "-18446744073709551615",
    "-18446744073709551616",
    "99999999999999999999999",
    "-99999999999999999999999",
    "7fffffffffffffff",
    "ffffffffffffffff",
    "1010",
    "zzzzzzzzzzzzz",
    "  \t+0X10p",
    "0777",
    "08",
    "  ",
    "1_000",
    "--1",
    "+-1"};

/* The DEFINED bases only -- see the out-of-range note above. */
static const int bases[NBASES] = {0, 2, 8, 10, 16, 36};

#define NDFORMS 20
static const char dforms[NDFORMS][WIDTH] = {
    "1e400",
    "-1e400",
    "1e-400",
    "5e-324",
    "-5e-324",
    "2.2250738585072011e-308",
    "1.7976931348623157e309",
    "0.1",
    "  -42.5abc",
    "3.5xyz",
    "1e308",
    "inf",
    "1e",
    "  zz",
    /* The `inf`/`nan` WORD LENGTHS, which nothing above reaches: the
       helper matches against `infinity` and stops at whatever prefix the
       input actually has, so `infinity` consumes 8 and `infin` consumes
       3 -- a scan that always advanced by the matched count would print 5
       for the second and 8 for `INFINITY` either way. `nanq` pins that a
       trailing letter is NOT part of the word (3, not 4), and the last
       two carry a sign and a case fold through the same arithmetic. */
    "infinity",
    "infin",
    "INFINITY",
    "nanq",
    " -infinityx",
    "+inf"};

/* One row each: the loops read them at an argc-derived index for the same
   reason the tables above do -- neither compiler may see a constant. */
static const char offs[1][WIDTH] = {"xx42y  -7z"};
static const char nulls[1][WIDTH] = {"zz9x"};
static const char walks[1][WIDTH] = {" 12 -3 0x1f 7q9"};

/* The endptr over a char* PARAMETER's region rather than a local array:
   `e` joins the PARAMETER's region, and `e - s` is a difference inside
   it. This is the shape a tokenizer helper actually has. */
static long parse_one(char *s, long *consumed) {
  char *e;
  long v = strtol(s, &e, 0);
  *consumed = (long)(e - s);
  return v;
}

int main(int argc, char **argv) {
  char buf[WIDTH];
  char *end;
  int i;
  int j;
  int k;
  int b;
  long lv;
  unsigned long uv;
  double dv;

  for (i = 0; i < NFORMS; i++) {
    for (j = 0; j < WIDTH; j++) {
      buf[j] = forms[i + argc - 1][j];
    }
    for (k = 0; k < NBASES; k++) {
      b = bases[(k + argc - 1) % NBASES];
      /* The signed column: value, cursor offset, the byte AT the cursor
         through both spellings (`*end` and `end[0]`), and the
         no-conversion predicate C defines as `end == nptr`. */
      lv = strtol(buf, &end, b);
      printf("L %02d %02d %ld %ld %d %d %d %d\n", i, k, lv, (long)(end - buf),
             (int)(unsigned char)*end, (int)(unsigned char)end[0],
             end == buf, end != buf);
      /* The unsigned column shares the subject sequence exactly -- the
         cursor must NOT differ from the signed one even where the values
         do (rows 25/27/29/32), which is what makes these two columns a
         cross-check rather than a repetition. */
      uv = strtoul(buf, &end, b);
      printf("U %02d %02d %lu %ld %d %d %d %d\n", i, k, uv, (long)(end - buf),
             (int)(unsigned char)*end, (int)(unsigned char)end[0],
             end == buf, end != buf);
    }
  }

  for (i = 0; i < NDFORMS; i++) {
    for (j = 0; j < WIDTH; j++) {
      buf[j] = dforms[i + argc - 1][j];
    }
    dv = strtod(buf, &end);
    printf("D %02d %.17g %ld %d %d\n", i, dv, (long)(end - buf),
           (int)(unsigned char)*end, end == buf);
  }

  /* THE COORDINATE CORRECTION, which nothing above can see: every row
     so far parses from the START of its region, so a lowering that
     stored the callee's answer RAW instead of adding argument 0's own
     cursor would be byte-identical on all 566 lines above. Here the
     subject starts part-way in, so `end - wide` and `end - p` differ and
     the raw-store bug prints 2 where C prints 4. `wend` is its own
     pointer: an `end` that walked two different regions in one function
     is a multi-base shape with no representation, and it is a LOCATED
     REJECTION (pinned in test/Import/C/libc-strtox-invalid.c), not
     something this file may quietly rely on. */
  {
    char wide[WIDTH];
    char *p;
    char *wend;
    for (j = 0; j < WIDTH; j++) {
      wide[j] = offs[argc - 1][j];
    }
    p = wide + 2 * (argc > 0);
    lv = strtol(p, &wend, 10);
    printf("O %ld %ld %ld %d\n", lv, (long)(wend - wide), (long)(wend - p),
           (int)(unsigned char)*wend);
    p = wide + 7 * (argc > 0);
    lv = strtol(p, &wend, 10);
    printf("O %ld %ld %ld %d\n", lv, (long)(wend - wide), (long)(wend - p),
           (int)(unsigned char)*wend);
  }

  /* A NULLABLE `end`: the region sees the null constant, so the emitted
     pointer carries a runtime non-null flag as well as a cursor. C
     7.22.1.4p7 stores a pointer INTO the subject string and never a null
     one, so `if (end)` must be true afterwards even when nothing was
     converted -- the case where a "did the callee write it?" flag would
     most plausibly be left false. */
  {
    char small[WIDTH];
    char *e2 = NULL;
    for (j = 0; j < WIDTH; j++) {
      small[j] = nulls[argc - 1][j];
    }
    lv = strtol(small, &e2, 10);
    printf("N %ld %d %d\n", lv, e2 != NULL, (int)(unsigned char)*e2);
    lv = strtol(small, &e2, 36);
    printf("N %ld %d %d\n", lv, e2 != NULL, (int)(unsigned char)*e2);
  }
  /* THE WALK LOOP -- `p = e` -- which is what an endptr is FOR and the
     only section here where the cursor feeds back into the next call.
     `end == p` is C's loop-termination test, and it is the no-conversion
     case reached at the end of the buffer rather than at its start, so it
     exercises a non-zero `nptr` for the very contract row 1 pins at zero.
     The literal-region call after it parses out of a STRING LITERAL's
     read-only backing instead of an array. */
  {
    char text[WIDTH];
    char *p;
    char *w;
    long total = 0;
    int steps = 0;
    for (j = 0; j < WIDTH; j++) {
      text[j] = walks[argc - 1][j];
    }
    p = text;
    for (;;) {
      long v = strtol(p, &w, 0);
      if (w == p) {
        break;
      }
      total += v;
      steps++;
      printf("W %d %ld %ld %d\n", steps, v, (long)(w - text),
             (int)(unsigned char)*w);
      p = w;
    }
    printf("W done %d %ld %ld\n", steps, total, (long)(p - text));
  }
  /* THE SELF-ALIASING SHAPE, `strtol(q, &q, 0)`, which is the walk loop
     written in one argument list instead of two statements. Nothing above
     reaches it, and it is the one shape where ORDER inside the lowering is
     observable: the subject string is read from `q`'s OLD cursor and the
     answer is stored into the SAME cell, so a lowering that stored before
     it read would re-parse from the new position and print a different
     total. Four steps, each starting where the last stopped. */
  {
    char q_text[WIDTH];
    char *q;
    long total = 0;
    int steps = 0;
    for (j = 0; j < WIDTH; j++) {
      q_text[j] = walks[argc - 1][j];
    }
    q = q_text;
    for (steps = 0; steps < 4; steps++) {
      long v = strtol(q, &q, 0);
      total += v;
      printf("Q %d %ld %ld\n", steps, v, (long)(q - q_text));
    }
    printf("Q done %ld\n", total);
  }
  {
    const char *lit = "  -8xyz";
    char *le;
    lv = strtol(lit + argc - 1, &le, 10);
    printf("S %ld %d %d\n", lv, (int)(unsigned char)*le, le == lit);
  }
  {
    long used = 0;
    char text[WIDTH];
    for (j = 0; j < WIDTH; j++) {
      text[j] = walks[argc - 1][j];
    }
    lv = parse_one(text + argc, &used);
    printf("P %ld %ld\n", lv, used);
  }
  return 0;
}
