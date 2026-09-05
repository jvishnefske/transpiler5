// FR-195: the three guards that refuse two C identifiers whose emitted
// symbols FOLD onto one spelling -- the Rust-keyword mangle (`match` ->
// `match_`), FR-73's leading-underscore fold (`_set` -> `tu0_set`), and
// FR-125's idiomatic case fold (`Foo` -> `tu0_foo`) -- all detected the
// clash perfectly in PLAIN mode and then threw the answer away under
// `--recover`: the loser was classified DROPPED, nothing rewrote its call
// sites, and because the SURVIVOR already occupied the very symbol the
// loser would have emitted as, every dangling call silently resolved onto
// the survivor. Exit 0, clean cargo build, wrong answer (FR-193 item 3).
//
// What this file pins:
//  1. the PLAIN-mode located rejections are byte-identical -- FR-195
//     changes recovery only, and the wording/location of all three guards
//     is the pre-existing one;
//  2. under `--recover` the loser is STUBBED (like every other recovered
//     rejection) under a symbol of its OWN, and its call site binds to
//     that stub, so the call panics instead of silently running the
//     survivor;
//  3. the SURVIVOR is untouched: same symbol, same body, still called.
// The RUNTIME consequence -- exit 101 rather than a wrong number -- is in
// test/EndToEnd/fn-symbol-collision-recover.c, because no FileCheck of the
// emitted code can tell a silently-rebound call from a correct one.
//
// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=crate %t/case-fold.c -o %t.a 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CASE
// RUN: not emitrust-cc --emit=crate %t/underscore.c -o %t.b 2>&1 \
// RUN:   | FileCheck %s --check-prefix=USCORE
// RUN: not emitrust-cc --emit=crate %t/keyword.c -o %t.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=KEYWORD
//
// RUN: emitrust-cc --recover --emit=rust %t/case-fold.c -o - 2>%t.case.err \
// RUN:   | FileCheck %s --check-prefix=CASE-RUST
// RUN: FileCheck %s --check-prefix=CASE-DIAG --input-file=%t.case.err
// RUN: emitrust-cc --recover --emit=rust %t/underscore.c -o - \
// RUN:   2>%t.us.err | FileCheck %s --check-prefix=USCORE-RUST
// RUN: FileCheck %s --check-prefix=USCORE-DIAG --input-file=%t.us.err
// RUN: emitrust-cc --recover --emit=rust %t/keyword.c -o - 2>%t.kw.err \
// RUN:   | FileCheck %s --check-prefix=KEYWORD-RUST
// RUN: FileCheck %s --check-prefix=KEYWORD-DIAG --input-file=%t.kw.err
//
// The DECL-FIRST ordering is the dangerous one and gets its own leg: the
// caller is imported BEFORE the loser's definition, so the redirect cannot
// be a property of import order. The prototype is what claims the reserved
// symbol; the definition then has nothing left to claim and is dropped,
// which is correct -- the stub already owns the item.
// RUN: emitrust-cc --recover --emit=rust %t/decl-first.c -o - 2>%t.df.err \
// RUN:   | FileCheck %s --check-prefix=DECLFIRST
// RUN: FileCheck %s --check-prefix=DECLFIRST-DIAG --input-file=%t.df.err
//
// FAIL-CLOSED leg. Not every loser can be stubbed: a signature the stub
// retry cannot rebuild (here a `_Complex double` parameter) leaves no item
// under the reserved symbol at all. Binding the call to `tu0_foo` anyway
// would be exactly the silent rebind, so the USE is refused where it
// appears and the CALLER is recovered instead -- loud, one level out.
// RUN: emitrust-cc --recover --emit=rust %t/no-stub.c -o - 2>%t.ns.err \
// RUN:   | FileCheck %s --check-prefix=NOSTUB
// RUN: FileCheck %s --check-prefix=NOSTUB-DIAG --input-file=%t.ns.err

//--- case-fold.c
int printf(const char *, ...);
static int Foo(void) { return 1; }
static int foo(void) { return 2; }
int main(void) { printf("%d %d\n", Foo(), foo()); return 0; }
// CASE: case-fold.c:3:12: error: unsupported: function 'foo' emits as 'tu0_foo', which collides with 'Foo' (the idiomatic rename folds both spellings onto one symbol)

// The survivor keeps `tu0_foo` and its body; the loser gets `_collision1`
// and an `unimplemented!()` carrying the verbatim rejection; `c_main`
// calls ONE of each.
// CASE-RUST:      fn tu0_foo() -> i32 {
// CASE-RUST-NEXT:     1
// CASE-RUST-NEXT: }
// CASE-RUST:      fn tu0_foo_collision1() -> i32 {
// CASE-RUST-NEXT: unimplemented!("unsupported: function 'foo' emits as 'tu0_foo', which collides with 'Foo' (the idiomatic rename folds both spellings onto one symbol)")
// CASE-RUST:      fn c_main() -> i32 {
// CASE-RUST:      = tu0_foo();
// CASE-RUST-NEXT: = tu0_foo_collision1();
// CASE-DIAG: warning: unsupported: function 'foo' emits as 'tu0_foo', which collides with 'Foo' (the idiomatic rename folds both spellings onto one symbol) (recovered: emitted an unimplemented!() stub with the mapped signature)
// CASE-DIAG: stubbed 'tu0_foo_collision1' [other]

//--- underscore.c
int printf(const char *, ...);
static int _set(int a) { return a + 1; }
static int set(int a) { return a + 2; }
int main(void) { printf("%d %d\n", _set(1), set(2)); return 0; }
// USCORE: underscore.c:3:12: error: unsupported: function name 'set' emits as 'tu0_set', which collides with '_set' (leading underscores fold into the symbol prefix)

// USCORE-RUST:      fn tu0_set(a: i32) -> i32 {
// USCORE-RUST-NEXT:     a + 1i32
// USCORE-RUST:      fn tu0_set_collision1(
// USCORE-RUST-NEXT: unimplemented!("unsupported: function name 'set' emits as 'tu0_set', which collides with '_set' (leading underscores fold into the symbol prefix)")
// USCORE-RUST:      fn c_main() -> i32 {
// USCORE-RUST:      = tu0_set(1i32);
// USCORE-RUST-NEXT: = tu0_set_collision1(2i32);
// USCORE-DIAG: stubbed 'tu0_set_collision1' [other]

//--- keyword.c
int printf(const char *, ...);
static int match_(int a) { return a + 1; }
static int match(int a) { return a + 2; }
int main(void) { printf("%d %d\n", match_(1), match(2)); return 0; }
// KEYWORD: keyword.c:3:12: error: unsupported: function name 'match' mangles to 'match_', which collides with an existing symbol

// KEYWORD-RUST:      fn tu0_match_(a: i32) -> i32 {
// KEYWORD-RUST-NEXT:     a + 1i32
// KEYWORD-RUST:      fn tu0_match__collision1(
// KEYWORD-RUST-NEXT: unimplemented!("unsupported: function name 'match' mangles to 'match_', which collides with an existing symbol")
// KEYWORD-RUST:      fn c_main() -> i32 {
// KEYWORD-RUST:      = tu0_match_(1i32);
// KEYWORD-RUST-NEXT: = tu0_match__collision1(2i32);
// KEYWORD-DIAG: stubbed 'tu0_match__collision1' [other]

//--- decl-first.c
int printf(const char *, ...);
static int Foo(void);
static int foo(void);
int main(void) { printf("%d %d\n", Foo(), foo()); return 0; }
static int Foo(void) { return 1; }
static int foo(void) { return 2; }
// DECLFIRST:      fn tu0_foo_collision1() -> i32 {
// DECLFIRST-NEXT: unimplemented!("unsupported: function 'foo' emits as 'tu0_foo', which collides with 'Foo' (the idiomatic rename folds both spellings onto one symbol)")
// DECLFIRST:      fn c_main() -> i32 {
// DECLFIRST:      = tu0_foo();
// DECLFIRST-NEXT: = tu0_foo_collision1();
// DECLFIRST:      fn tu0_foo() -> i32 {
// DECLFIRST-NEXT:     1
// The prototype is stubbed and the later definition is dropped -- both are
// ledgered under the RESERVED symbol, never under `tu0_foo`, which is the
// name the surviving item still answers to.
// DECLFIRST-DIAG: stubbed 'tu0_foo_collision1' [other]
// DECLFIRST-DIAG: dropped 'tu0_foo_collision1' [other]

//--- no-stub.c
int printf(const char *, ...);
static int Foo(int x) { return x + 1; }
static int foo(_Complex double z) { return 2; }
int main(void) { printf("%d %d\n", Foo(1), foo(1.0)); return 0; }
// The survivor is untouched; the caller carries the refusal.
// NOSTUB:      fn tu0_foo(x: i32) -> i32 {
// NOSTUB-NEXT:     x + 1i32
// NOSTUB:      fn c_main() -> i32 {
// NOSTUB-NEXT: unimplemented!("unsupported: use of function 'foo', whose emitted symbol 'tu0_foo' is owned by a different C spelling in this translation unit")
// NOSTUB-NOT:  tu0_foo(
// NOSTUB-DIAG: no-stub.c:3:12: warning: unsupported: function 'foo' emits as 'tu0_foo', which collides with 'Foo' (the idiomatic rename folds both spellings onto one symbol) (recovered: item dropped)
// NOSTUB-DIAG: no-stub.c:4:44: warning: unsupported: use of function 'foo', whose emitted symbol 'tu0_foo' is owned by a different C spelling in this translation unit (recovered: emitted an unimplemented!() stub with the mapped signature)
// NOSTUB-DIAG: dropped 'tu0_foo_collision1' [other]
// NOSTUB-DIAG: stubbed 'c_main' [other]
