// FR-179 (FR-139 follow-on): an FR-62 actor lift must not COST a C-ABI export.
//
// The defect this pins: `--c-abi-exports` refused a bare C symbol for a
// function that is all-scalar IN C purely because the lift had turned it into
// a `&mut self` method on a synthesized owner struct. The SYNTHETIC receiver,
// not the C signature, tripped the all-scalar gate, and the emitted cdylib
// contained zero `#[no_mangle]` -- every dlsym in a C host returned null. That
// is a correctness defect and not a policy: `unsigned short lookup(int)` has a
// perfectly good C ABI whether or not its lookup table became a struct field.
//
// "Do not lift it" is the WRONG repair, because the lifted state is REAL --
// the bodies below read and write it. So the method keeps its lifted shape,
// byte for byte, and gains a MODULE-scope `#[no_mangle] extern "C"` free
// function that borrows a per-owner thread_local singleton and delegates. No
// `unsafe`, and `thread_local` rather than a process-global Mutex or a
// `static mut` because that is already how this emitter renders every mutable
// C file-scope global (TranslateToRust.cpp's `emitGlobal`).
//
// Five invariants are pinned here.
//
// 1. The wrapper exists, at module scope, with the bare C symbol, the method's
//    own parameter spellings, and the C signature (receiver dropped).
// 2. ONE thread_local PER OWNER, shared by every wrapper on it: `bump` and
//    `peek` came from the same C `counter`, so they must see the SAME state.
//    Two singletons would be two copies of one C variable. Three owners exist
//    and only two are wrapped, so the count pins both halves at once.
// 3. FR-139's POINTER refusal is UNTOUCHED. `sum_into` is actor-lifted too and
//    is all-scalar in nothing: its `&[u8]` parameter is a two-register fat
//    pointer that rustc would compile across `extern "C"` with a mere warning
//    and then miscompile. It keeps its plain `pub fn` and its LOCATED warning.
// 4. An owner with no zero-argument `new()` -- an actor the FR-62 pass did not
//    export, constructed by its driver instead -- has nothing for a singleton
//    to be built from, and says so rather than naming a constructor that does
//    not exist. (Pinned in the sibling .cpp file, which has such an actor.)
// 5. WITHOUT the flag NOTHING moves. The wrappers are a pure module-scope
//    APPEND, so deleting everything from the first `thread_local! {` onward
//    must reproduce the default crate root BYTE FOR BYTE -- checked by diff,
//    not by inspection, because the emitted bytes are this repo's byte-identity
//    invariant.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// The wrapped set is exactly the two all-scalar owners' three methods, and the
// singleton set is exactly the two owners: never the pointer-taking method,
// never the synthesized `new`, never the unwrapped third owner.
// RUN: not grep 'extern "C" fn sum_into' %t.cabi/src/lib.rs
// RUN: not grep 'extern "C" fn new' %t.cabi/src/lib.rs
// RUN: not grep __EMITRUST_ACTOR_TU0SCRATCHACTOR %t.cabi/src/lib.rs
// RUN: grep -c no_mangle %t.cabi/src/lib.rs > %t.cabi.count
// RUN: grep -c 'thread_local! {' %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count
//
// The byte-identity guard. The flag is OFF by default and must move nothing.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep no_mangle %t.plain/src/lib.rs
// RUN: not grep 'extern "C"' %t.plain/src/lib.rs
// RUN: not grep __EMITRUST_ACTOR %t.plain/src/lib.rs
// RUN: not grep c-abi-exports %t.plain.err
// RUN: sed '/^thread_local! {$/,$d' %t.cabi/src/lib.rs > %t.cabi.head
// RUN: diff %t.plain/src/lib.rs %t.cabi.head

// The lifted state is read-only here, but it is still state: `lookup` reads
// `base` on every call, so the wrapper has to hand it a live owner.
static unsigned short base[8] = {3, 5, 7, 11, 13, 17, 19, 23};

// MUTATED lifted state, reached by two exported functions. This is the pair
// that a per-call `Tu0CounterActor::new()` would silently break.
static int counter;

static unsigned char scratch[8];

unsigned short lookup(int i) { return base[i & 7]; }
int bump(int by) { counter = counter + by; return counter; }
int peek(void) { return counter; }

// Actor-lifted AND pointer-taking: FR-139's refusal owns this one, unchanged.
// WARN: c-abi-exports-actor.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'sum_into': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int sum_into(const unsigned char *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i) {
    scratch[i & 7] = p[i];
    s += scratch[i & 7];
  }
  return s;
}

// The lifted methods keep exactly the shape they had: no attribute, no
// `extern "C"`, the receiver still first.
// CABI:      impl Tu0BaseActor {
// CABI:          pub fn lookup(&mut self, i: i32) -> u16 {
// CABI:      impl Tu0CounterActor {
// CABI:          pub fn bump(&mut self, by: i32) -> i32 {
// CABI:          pub fn peek(&mut self) -> i32 {
// CABI:      impl Tu0ScratchActor {
// CABI:          pub fn sum_into(&mut self, p: &[u8], n: i32) -> i32 {

// One singleton per wrapped owner, then one wrapper per exported method,
// spelled with the bare C symbol and the method's own parameter names.
// CABI:      thread_local! {
// CABI-NEXT:     static __EMITRUST_ACTOR_TU0BASEACTOR: std::cell::RefCell<Tu0BaseActor> = std::cell::RefCell::new(Tu0BaseActor::new());
// CABI-NEXT: }
// CABI-NEXT: thread_local! {
// CABI-NEXT:     static __EMITRUST_ACTOR_TU0COUNTERACTOR: std::cell::RefCell<Tu0CounterActor> = std::cell::RefCell::new(Tu0CounterActor::new());
// CABI-NEXT: }
// CABI-NEXT: #[no_mangle]
// CABI-NEXT: pub extern "C" fn lookup(i: i32) -> u16 {
// CABI-NEXT:     __EMITRUST_ACTOR_TU0BASEACTOR.with(|__emitrust_actor| __emitrust_actor.borrow_mut().lookup(i))
// CABI-NEXT: }
// CABI-NEXT: #[no_mangle]
// CABI-NEXT: pub extern "C" fn bump(by: i32) -> i32 {
// CABI-NEXT:     __EMITRUST_ACTOR_TU0COUNTERACTOR.with(|__emitrust_actor| __emitrust_actor.borrow_mut().bump(by))
// CABI-NEXT: }
// CABI-NEXT: #[no_mangle]
// CABI-NEXT: pub extern "C" fn peek() -> i32 {
// CABI-NEXT:     __EMITRUST_ACTOR_TU0COUNTERACTOR.with(|__emitrust_actor| __emitrust_actor.borrow_mut().peek())
// CABI-NEXT: }

// Three wrappers; two singletons for the three owners that exist.
// COUNT:      3
// COUNT-NEXT: 2
