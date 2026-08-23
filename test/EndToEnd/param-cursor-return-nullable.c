// REQUIRES: cargo
// FR-104 differential, Option-of-cursor composition (the jsmn
// jsmn_alloc_token shape): a parameter-cursor-returning callee with a
// reachable `return NULL` lifts to Option<i64>. Byte-diffed against the
// clang-built native binary; the capacity derives from argc so the
// exhaustion point cannot constant-fold. The loop crosses the
// Some->None boundary mid-loop: the first `cap` iterations bind a live
// cursor into the caller's `toks` region (writes through it must land
// in the array), the rest observe the None discriminant through the
// caller's nullable-region flag cell — and the side effect on
// parser->toknext must advance EXACTLY on the Some iterations.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name param_cursor_return_nullable --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native 2>&1 > %t.native.out
// RUN: %t.crate/target/release/param_cursor_return_nullable > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct tok { int start; int end; int size; };
struct parser { unsigned int toknext; };

struct tok *alloc_token(struct parser *parser, struct tok *tokens,
                        unsigned int num_tokens) {
  struct tok *t;
  if (parser->toknext >= num_tokens)
    return 0;
  t = &tokens[parser->toknext++];
  t->start = -1;
  t->end = -1;
  t->size = 0;
  return t;
}

int main(int argc, char **argv) {
  struct parser p;
  struct tok toks[4];
  p.toknext = 0;
  unsigned int cap = 2u + (unsigned int)(argc - 1);
  int i;
  for (i = 0; i < 4; i++) {
    struct tok *t = alloc_token(&p, toks, cap);
    if (!t) {
      printf("i=%d null next=%d\n", i, (int)p.toknext);
      continue;
    }
    t->start = i * 7;
    printf("i=%d start=%d end=%d next=%d\n", i, t->start, t->end,
           (int)p.toknext);
  }
  return 0;
}
