// RUN: split-file %s %t
// RUN: emitrust-import-c %t/lskip.c | FileCheck %s --check-prefix=LSKIP
// RUN: emitrust-import-c %t/alloc-token.c | FileCheck %s --check-prefix=ALLOC

// FR-104: a returned cursor into a PARAMETER region. The importer already
// decomposes every pointer into (region base, i64 cursor); when every
// return site of a function is proven to root in the SAME single
// slice-classified pointer parameter, only the CURSOR crosses the return:
// the function returns a plain i64 (RELATIVE to that slice parameter,
// whose own coordinates start at 0), and the caller re-slices the argument
// region it already holds by adding the result to the argument's cursor
// at the call. This generalizes the Stage-1 owner-index-return precedent
// (design.md FR-30 follow-on) to free functions over multiple caller
// regions — the case owner promotion cannot serve. Value-preserving
// pointer casts (`return (char *)s;` — verbatim inih ini_lskip) are
// transparent to the return-root proof. With a reachable `return NULL`
// (jsmn's jsmn_alloc_token), the result lifts to Option<i64> — `None` for
// the null sites, `Some(cursor)` otherwise — and the caller carries the
// discriminant in the existing CTS-P8 nullable-region flag cell.

//--- lskip.c
// TWO distinct caller regions force the parameter-rooted transform (a
// single region would be served by owner promotion instead); the
// (char *) cast crossing constness pins cast transparency. The caller
// mutability is the caller's own: the returned i64 carries no borrow.
char *lskip(const char *s) {
  while (*s && *s <= ' ')
    s++;
  return (char *)s;
}
int main(void) {
  char b1[8] = "  ab";
  char b2[8] = " z";
  return (*lskip(b1) == 'a' && *lskip(b2) == 'z') ? 0 : 1;
}
// The signature transform: one slice input, i64 result (no extra cursor
// parameter — slice-parameter coordinates are relative).
// LSKIP-LABEL: func.func @lskip
// LSKIP-SAME: (%{{[^,)]+}}: !emitrust.mut_ref<!emitrust.slice<i8>>) -> i64
// LSKIP-SAME: emitrust.param_names = ["s"]
// LSKIP: return %{{.+}} : i64
// The caller re-slices its own region at the argument cursor, calls, and
// adds the returned relative cursor to the argument cursor — once per
// region.
// LSKIP-LABEL: func.func @c_main
// LSKIP: %[[S1:.+]] = emitrust.slice_of mut %{{.+}}[%[[A1:.+]]] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// LSKIP: %[[R1:.+]] = call @lskip(%[[S1]]) : (!emitrust.mut_ref<!emitrust.slice<i8>>) -> i64
// LSKIP: arith.addi %[[A1]], %[[R1]] : i64
// LSKIP: %[[S2:.+]] = emitrust.slice_of mut %{{.+}}[%[[A2:.+]]] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// LSKIP: %[[R2:.+]] = call @lskip(%[[S2]]) : (!emitrust.mut_ref<!emitrust.slice<i8>>) -> i64
// LSKIP: arith.addi %[[A2]], %[[R2]] : i64

//--- alloc-token.c
// The jsmn_alloc_token shape: a reachable `return NULL` beside cursor
// returns rooted in the `tokens` parameter lifts the result to
// Option<i64> — zero new ops (the FR-99 Option op set with an i64
// payload). The second (struct) pointer parameter stays on its ordinary
// lowering; only the RETURN must root in one single parameter.
struct tok { int start; int end; int size; };
struct parser { unsigned int toknext; };
struct tok *alloc_token(struct parser *parser, struct tok *tokens,
                        unsigned int num_tokens) {
  struct tok *t;
  if (parser->toknext >= num_tokens)
    return 0;
  t = &tokens[parser->toknext++];
  t->start = -1;
  return t;
}
int main(void) {
  struct parser p;
  struct tok toks[3];
  p.toknext = 0;
  struct tok *t = alloc_token(&p, toks, 3u);
  if (!t)
    return 9;
  t->start = 5;
  return t->start - 5;
}
// ALLOC-LABEL: func.func @alloc_token
// ALLOC-SAME: -> !emitrust.opaque<"Option<i64>">
// The NULL site is the None literal; the cursor site wraps in Some.
// ALLOC: emitrust.literal "None" : !emitrust.opaque<"Option<i64>">
// ALLOC: emitrust.call_opaque "Some"(%{{.+}}) : (i64) -> !emitrust.opaque<"Option<i64>">
// The caller lands the Option in a temp, keeps the discriminant in the
// nullable-region flag cell (is_some), and re-slices at argument cursor +
// unwrap_or(0) — the payload is dead while the flag is false, so the 0 is
// never observable in a defined program.
// ALLOC-LABEL: func.func @c_main
// ALLOC: %[[OPT:.+]] = call @alloc_token(%{{.+}}) : ({{.*}}) -> !emitrust.opaque<"Option<i64>">
// ALLOC: emitrust.assign %[[TMP:.+]] = %[[OPT]]
// ALLOC: emitrust.method_call %[[TMP]]["is_some"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>) -> i1
// ALLOC: emitrust.method_call %[[TMP]]["unwrap_or"] (%{{.+}}) : (!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>, i64) -> i64
