// FR-84: an in-TU-defined struct global with an aggregate initializer,
// certified and EXPORTED as an owner handle in a library TU, must import —
// this pins the invariant that the aggregate-init verifier resolves the
// module-level emitrust.struct_def MODULE-FIRST. The actor plan stages the
// C initializer as a `const` emitrust.variable INSIDE the owner impl's
// synthesized new(), and `emitrust.impl` is itself a SymbolTable, so the
// old nearest-table lookup never saw the def and the verifier killed the
// whole crate ("requires a visible emitrust.struct_def" — the lwIP
// ip4_addr/ip6_addr whole-crate loss, ip_addr.h:398). The initializer must
// LAND: new() carries every C field value, including the nested array's
// implicit zero tail, the wide-unsigned field, and the double.
// RUN: emitrust-cc --emit=rust %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=NOTE < %t.err
// RUN: FileCheck %s --check-prefix=RUST < %t.rs

// The exported actor: external-linkage arms over one struct global whose
// aggregate initializer is the FR-84 trigger.
struct Cal {
  int base;
  unsigned int mask;
  double scale;
  int taps[3];
};

struct Cal cal = {-7, 4000000000u, -1.25, {2, -3}};

int shift_base(int by) {
  cal.base = cal.base + by;
  return cal.base;
}

unsigned int fold_mask(unsigned int m) {
  cal.mask = cal.mask ^ m;
  return cal.mask;
}

double scaled(int v) { return cal.scale * v; }

int tap_sum(void) { return cal.taps[0] + cal.taps[1] + cal.taps[2]; }

// The export note is the actor-plan surface pinned by actor-lift-lib.c;
// here it proves the struct global took the owner-handle route (the route
// whose staged initializer used to die in the verifier).
// NOTE: note: actor plan: exported CAL: owner handle 'CalActor' (construct with CalActor::new())

// The owner struct holds the C global as a private field, and new() stages
// the aggregate initializer in one `let` and assigns it whole — the
// staged variable is the op the old nearest-table lookup could not verify.
// RUST:      pub struct CalActor {
// RUST-NEXT:     cal: Cal,
// RUST-NEXT: }
// RUST:      impl CalActor {
// RUST-NEXT:     pub fn new() -> CalActor {
// RUST:          let v0: Cal = Cal { base: -7, mask: 4000000000, scale: -1.25, taps: [2, -3, 0], };
// RUST-NEXT:     owner.cal = v0;
// RUST:      pub fn shift_base(&mut self, by: i32) -> i32 {
// RUST:      pub fn tap_sum(&mut self) -> i32 {
