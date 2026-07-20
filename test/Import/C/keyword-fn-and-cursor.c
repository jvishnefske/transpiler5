// RUN: emitrust-import-c %s | FileCheck %s

// CTS 00204 companions.
//
// (1) Function-name keyword policy REVISION: a function whose C
// spelling is a Rust keyword now mangles like a struct member — a
// single trailing underscore (`match` -> `match_`, `loop` -> `loop_`)
// — instead of rejecting. The mangled spelling is the symbol's identity
// everywhere: the func.func name and every call site. A collision the
// mangle introduces (an existing `match_` beside `match`) stays a
// located rejection (keyword-fn-and-cursor-invalid.c). Struct, enum,
// enumerator, and global names keep their keyword rejections.
//
// (2) `const char **` parameters with string-cursor semantics (the
// 00204 `match` helper): the callee reads the current position through
// `*s`/`**s` and advances it by writing `*s = p`; the caller passes
// `&s` where `s` walks a string. The lowering is GREEN's choice (an
// out-param cursor model or equivalent) — pinned here only loosely:
// the callee imports, call sites import, and the caller's loop keeps
// working. The runtime advancement observable is pinned differentially
// in test/EndToEnd/varargs-monomorph.c. A const char ** parameter used
// beyond this bounded shape stays rejected
// (keyword-fn-and-cursor-invalid.c).

int putchar(int c);

int loop(int x) { return x + 1; }
// CHECK-LABEL: func.func @loop_
// CHECK-SAME: (%{{[^,)]+}}: i32) -> i32

// The 00204 cursor helper, keyword name and all.
int match(const char **s, const char *f) {
  const char *p = *s;
  for (p = *s; *f && *f == *p; f++, p++)
    ;
  if (!*f) {
    *s = p - 1;
    return 1;
  }
  return 0;
}
// CHECK-LABEL: func.func @match_
// CHECK-SAME: -> i32

int scan(void) {
  const char *s;
  int hits = 0;
  for (s = "ab%dc%dd"; *s; s++) {
    if (match(&s, "%d"))
      hits = hits + 1;
    else
      putchar(*s);
  }
  putchar('\n');
  return hits + loop(1);
}
// CHECK-LABEL: func.func @scan
// CHECK: call @match_(
// CHECK: call @loop_(

int main(void) {
  return scan() - 4;
}
