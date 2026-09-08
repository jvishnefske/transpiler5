// RUN: split-file %s %t
//
// FR-208: `--c-abi-exports` must export THE C PROGRAM'S OWN SYMBOL. The flag's
// entire contract is "there is a bare symbol to dlsym", but `#[no_mangle]` and
// `#[export_name]` were written from the MLIR symbol -- and the MLIR symbol is
// the FR-53 IDIOMATIC RENAME. So `int SPX_add(int, int)` shipped a cdylib
// exporting `spx_add`, a name that appears nowhere in the C source, and the
// host's `dlsym("SPX_add")` returned null.
//
// The dlopen/byte-diff oracle for this lives in
// test/EndToEnd/c-abi-exports-mixed-case-dlopen.c, which is what actually
// proves a C caller links. THIS file pins the emission RULES around it -- the
// cases that are cheap to state and easy to regress silently:
//
//  1. the C spelling reaches `#[export_name]` while the ITEM keeps the
//     idiomatic rename, so every internal call site and every other byte of
//     the crate is unchanged;
//  2. an already-snake_case name keeps the historical `#[no_mangle]`. The fix
//     is targeted, not a blanket switch to `#[export_name]`: if it were
//     blanket, every existing `--c-abi-exports` golden would have moved;
//  3. the FR-179 ACTOR WRAPPER path takes the same treatment -- it is a THIRD
//     emission site for the same defect and was equally wrong;
//  4. `main` is DELIBERATELY EXCLUDED and keeps exporting `c_main`. The
//     `main` -> `c_main` rename is not the FR-53 rename: it is unconditional,
//     `--preserve-c-names` performs it too, and a library crate has no
//     business defining a dynamic `main`. FR-208 moves the export name to
//     exactly what `--preserve-c-names` would have emitted, which is
//     `c_main`;
//  5. an internal-linkage function is untouched -- it was never exported and
//     still is not;
//  6. WITHOUT the flag nothing moves at all, checked by diff and not by
//     inspection, because the emitted bytes are this repo's byte-identity
//     invariant;
//  7. the cross-shard `--link` case: two C spellings that FOLD onto one
//     emitted symbol cannot reach the emitter, so no two items can ever
//     demand the same export name. That was already true and stays true; it
//     is pinned here because it is the one place FR-208 could have
//     introduced a new collision.
//
// RECORDED CAPABILITY COST: a C name that is a Rust KEYWORD still exports the
// mangled spelling (`match` -> `match_`). The keyword mangle is applied by
// `cFunctionSymbolName` in BOTH naming modes, so `--preserve-c-names` -- this
// FR's stated oracle for the target text -- produces `match_` too. Exporting
// the bare `match` would need a second, mode-independent rule and is a
// separate change; the current behavior is pinned below so it cannot drift
// unnoticed.

//--- names.c
static unsigned short Base[8] = {3, 5, 7, 11, 13, 17, 19, 23};

// The defect's own shape: an UPPER_ prefix folding to snake_case.
int SPX_add(int a, int b) { return a + b; }

// A leading-lowercase camel name, which folds differently.
int spxWiden(int x) { return x * 2; }

// THE CONTROL: already snake_case, so its emission may not move one byte.
int plain_add(int a, int b) { return a - b; }

// A Rust keyword: the mangle is mode-independent, so both spellings agree and
// the historical `#[no_mangle]` stays exact (see the cost note above).
int match(int x) { return x + 1; }

// Internal linkage: never exported, before or after.
static int Helper(int x) { return x * 3; }

int Visible(int x) { return Helper(x); }

// A library crate's `main`: `c_main` in every mode, and NOT `main`.
int main(void) { return SPX_add(1, 2) + Visible(3) + (int)Base[0]; }

// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   %t/names.c -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=NAMES

// The C spelling is the exported string; the item keeps the rename.
// NAMES:      #[export_name = "SPX_add"]
// NAMES-NEXT: pub extern "C" fn spx_add(a: i32, b: i32) -> i32 {
// NAMES:      #[export_name = "spxWiden"]
// NAMES-NEXT: pub extern "C" fn spx_widen(x: i32) -> i32 {
//
// The control and the keyword mangle both keep `#[no_mangle]`.
// NAMES:      #[no_mangle]
// NAMES-NEXT: pub extern "C" fn plain_add(a: i32, b: i32) -> i32 {
// NAMES:      #[no_mangle]
// NAMES-NEXT: pub extern "C" fn match_(x: i32) -> i32 {
//
// The `static` helper is a private item with no C ABI at all; the exported
// caller takes its own C spelling.
// NAMES:      fn tu0_helper(x: i32) -> i32 {
// NAMES:      #[export_name = "Visible"]
// NAMES-NEXT: pub extern "C" fn visible(x: i32) -> i32 {
//
// `main` stays `c_main`, deliberately.
// NAMES:      #[no_mangle]
// NAMES-NEXT: pub extern "C" fn c_main() -> i32 {

// Nothing exports the bare `main`, nothing exports the keyword's C spelling,
// and an internal-linkage function is not exported under any spelling.
// RUN: not grep 'export_name = "main"' %t.cabi/src/lib.rs
// RUN: not grep 'export_name = "match"' %t.cabi/src/lib.rs
// RUN: not grep 'export_name = "Helper"' %t.cabi/src/lib.rs

// THE BYTE-IDENTITY GUARD. The flag is OFF by default, so the whole feature
// must be invisible without it -- and the attribute the importer now carries
// is IR metadata only, never emitted text.
// RUN: emitrust-cc --emit=crate --crate-type=lib %t/names.c -o %t.plain
// RUN: not grep no_mangle %t.plain/src/lib.rs
// RUN: not grep export_name %t.plain/src/lib.rs
// RUN: not grep 'extern "C"' %t.plain/src/lib.rs

//--- actor.c
// The FR-179 actor-wrapper path: the same defect, a different emission site.
// `SPX_bump` and `peek` share one lifted owner, so the pair also pins that
// giving one of them an `#[export_name]` does not disturb the singleton.
static int counter;
static unsigned short Base[8] = {3, 5, 7, 11, 13, 17, 19, 23};

int SPX_bump(int by) { counter = counter + by; return counter; }
int peek(void) { return counter; }
unsigned short SPX_lookup(int i) { return Base[i & 7]; }

// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   %t/actor.c -o %t.actor 2>/dev/null
// RUN: cat %t.actor/src/lib.rs | FileCheck %s --check-prefix=ACTOR

// ACTOR:      #[export_name = "SPX_bump"]
// ACTOR-NEXT: pub extern "C" fn spx_bump(by: i32) -> i32 {
// ACTOR-NEXT:     __EMITRUST_ACTOR_TU0COUNTERACTOR.with(|__emitrust_actor| __emitrust_actor.borrow_mut().spx_bump(by))
// ACTOR-NEXT: }
// ACTOR-NEXT: #[no_mangle]
// ACTOR-NEXT: pub extern "C" fn peek() -> i32 {
// ACTOR-NEXT:     __EMITRUST_ACTOR_TU0COUNTERACTOR.with(|__emitrust_actor| __emitrust_actor.borrow_mut().peek())
// ACTOR-NEXT: }
// ACTOR-NEXT: #[export_name = "SPX_lookup"]
// ACTOR-NEXT: pub extern "C" fn spx_lookup(i: i32) -> u16 {

//--- fold-a.c
int SPX_alpha(int x) { return x * 2; }

//--- fold-b.c
// The cross-shard collision FR-208 could have introduced, and did not: two
// distinct C spellings fold onto one emitted symbol, so a merged module can
// never hold two items demanding two different export names. The merge
// refuses first, LOCATED, and emits no crate -- the pre-FR-208 behavior,
// pinned here because this is where a regression would show up.
int spx_alpha(int x) { return x + 100; }

// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/fold-a.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/fold-b.c -o %t.b.o
// RUN: not emitrust-cc --link %t.a.o %t.b.o --emit=crate --crate-type=lib \
// RUN:   --c-abi-exports --crate-name=foldlink -o %t.fold 2>%t.fold.err
// RUN: FileCheck %s --check-prefix=FOLD --input-file=%t.fold.err
// FOLD: fold-b.c:{{[0-9]+}}:{{[0-9]+}}: error: duplicate definition of 'spx_alpha' at link

//--- shard-a.c
int SPX_shard(int x) { return x * 7; }

//--- shard-b.c
// The attribute has to survive shard SERIALIZATION, or `--link` would silently
// fall back to the rename for every multi-file project -- which is what the
// TRACTOR corpus is made of.
int SPX_shard(int x);
int SPX_shard_user(int y) { return SPX_shard(y) + 1; }

// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/shard-a.c -o %t.sa.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/shard-b.c -o %t.sb.o
// RUN: emitrust-cc --link %t.sa.o %t.sb.o --emit=crate --crate-type=lib \
// RUN:   --c-abi-exports --crate-name=shardlink -o %t.shard 2>/dev/null
// RUN: cat %t.shard/src/lib.rs | FileCheck %s --check-prefix=SHARD
// SHARD:      #[export_name = "SPX_shard"]
// SHARD-NEXT: pub extern "C" fn spx_shard(
// SHARD:      #[export_name = "SPX_shard_user"]
// SHARD-NEXT: pub extern "C" fn spx_shard_user(
