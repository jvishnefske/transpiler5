// FR-179 (FR-139 follow-on), the CONTAINMENT half: the new actor-lift
// C-ABI wrapper must reach the Phase-4 C owner method and NOTHING else.
//
// Two impl members can look alike at Rust emission time -- both are `&mut
// self` functions inside an `emitrust.impl` -- and only one of them ever had
// a C symbol. Exporting the other under a bare name would invent a C ABI for
// a C++ member that never had one, and would collide the moment two classes
// each have a `step`. The discriminator is ATTRIBUTE-keyed and never
// prefix-parsed (FR-110): a genuine C++ member carries
// `emitrust.method_rust_name` and its symbol was struct-mangled
// (`box_step`), while a Phase-4 C owner method carries neither and keeps the
// bare C spelling it always had.
//
// `Box::step` is the adversarial case on purpose: `int step(int)` IS
// all-scalar once the receiver is dropped, so only the attribute -- not the
// shape -- keeps it out. It must keep FR-139's plain `pub fn` and its located
// refusal.
//
// The second pin is the OTHER precondition, verified rather than assumed: the
// wrapper borrows a `thread_local` singleton it constructs with the owner's
// zero-argument `new()`, and the FR-62 pass synthesizes that constructor only
// for an EXPORTED owner. This input has a `main`, so `Tu0TallyActor` is
// driver-constructed and has no `new()` at all -- naming one would be a rustc
// E0599 in the emitted crate. It is refused, with its own wording, at its own
// location.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// Neither impl member reaches the C ABI, under any spelling of its name, and
// no singleton is created for either owner.
// RUN: not grep 'extern "C" fn step' %t.cabi/src/lib.rs
// RUN: not grep 'extern "C" fn box_step' %t.cabi/src/lib.rs
// RUN: not grep 'extern "C" fn add_to_tally' %t.cabi/src/lib.rs
// RUN: not grep __EMITRUST_ACTOR %t.cabi/src/lib.rs
// RUN: not grep thread_local %t.cabi/src/lib.rs
//
// Exactly one export survives: the entry point, which was never a method.
// RUN: grep -c no_mangle %t.cabi/src/lib.rs > %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count

extern "C" int printf(const char *, ...);

struct Box {
  int v;
  // WARN-DAG: c-abi-exports-actor-cxx.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'box_step': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
  int step(int d) { return v + d; }
};

static int tally;

// All-scalar in C and genuinely actor-lifted, but its owner is constructed by
// the driver (this input has a `main`), so the FR-62 pass synthesized no
// `new()` for it and the singleton has nothing to be built from.
// WARN-DAG: c-abi-exports-actor-cxx.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'add_to_tally': its FR-62 owner struct has no zero-argument new(), so a C-ABI singleton has nothing to construct from; it stays a plain 'pub fn' and is not reachable by dlsym
int add_to_tally(int d) { tally = tally + d; return tally; }

int main(int argc, char **) {
  Box b;
  b.v = argc;
  printf("%d %d\n", b.step(argc), add_to_tally(argc));
  return 0;
}

// The lifted C owner method and the C++ member both keep the plain shape.
// CABI:      impl Tu0TallyActor {
// CABI-NEXT:     pub fn add_to_tally(&mut self, d: i32) -> i32 {
// CABI:      #[no_mangle]
// CABI-NEXT: pub extern "C" fn c_main(argc: i32) -> i32 {
// CABI:      impl Box {
// CABI-NEXT:     pub fn step(&mut self, d: i32) -> i32 {

// COUNT: 1
