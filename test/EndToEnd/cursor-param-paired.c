// REQUIRES: cargo
// C99-43 slice 1 differential: both generalized cursor-parameter shapes
// in one runnable program, byte-diffed against the clang-built native
// binary (THE oracle for the paired-writeback arithmetic).
//
// Shape P (paired out-cursor, strtol/endp family): a parser walks
// "12 34 -7"-style text via repeated `parse_int(cur, &end); cur = end;`.
// The SECOND and later rounds pass a decomposed pointer local (`cur`,
// cursor != 0) as the co-argument, which is the case that would
// miscompile if the caller-side writeback forgot the reslice coordinate
// correction: the callee's `*endp = s + i` speaks coordinates relative
// to the co-argument's cursor at the call, so the caller must add that
// cursor back before storing into `end`'s cell. `end - text` at the end
// crosses the total distance through all three writebacks.
//
// Shape S (self-walking cursor) over a non-char element: an `int **`
// summer advancing with `(*p)++`, covering slice 1a's runnable half,
// plus `ip - nums` observing the callee's cursor advancement.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name cursor_param_paired --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cursor_param_paired > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

long parse_int(const char *s, const char **endp) {
  long value = 0;
  long sign = 1;
  long i = 0;
  while (s[i] == ' ')
    i = i + 1;
  if (s[i] == '-') {
    sign = -1;
    i = i + 1;
  }
  while (s[i] >= '0' && s[i] <= '9') {
    value = value * 10 + (s[i] - '0');
    i = i + 1;
  }
  *endp = s + i;
  return sign * value;
}

int sum_step(int **p) {
  int v = **p;
  (*p)++;
  return v;
}

int main(void) {
  char text[9] = "12 34 -7";
  const char *cur = text;
  const char *end;
  long total = 0;
  int rounds = 0;
  while (rounds < 3) {
    long v = parse_int(cur, &end);
    printf("round %d value %ld\n", rounds, v);
    total = total + v;
    cur = end;
    rounds = rounds + 1;
  }
  printf("total %ld consumed %ld\n", total, (long)(end - text));

  int nums[4] = {2, 4, 6, 8};
  int *ip = nums;
  int s1 = 0;
  int k = 0;
  while (k < 4) {
    s1 = s1 + sum_step(&ip);
    k = k + 1;
  }
  printf("sum %d advanced %ld\n", s1, (long)(ip - nums));
  return 0;
}
