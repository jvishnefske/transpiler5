// FR-41: the stub side of the asymmetry. A function rejected for something in
// its BODY still has a writable signature, so FR-42's recovery replaces it
// with an `unimplemented!()` stub carrying that signature — and a caller
// compiles against the stub. So call-poisoning demotes the caller only to
// YELLOW, never to Red.
//
// The second thing this pins is that Yellow does NOT propagate: `outer` calls
// `caller`, `caller` is Yellow, and `outer` is GREEN. Yellow means "emits and
// compiles, but a callee is a stub", which is not a property that spreads —
// the caller of a compiling function is unaffected by what that function
// calls. Only RED spreads.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

// Inline asm has no representation in the emitted Rust at all, and it is a
// BODY construct: `blocked`'s `void(void)` signature maps perfectly well.
void blocked(void) { __asm__(""); }

void caller(void) { blocked(); }

void outer(void) { caller(); }

// Taking the address of a stubbable Red function is the same situation as
// calling it — the stub has the right signature, so `&blocked` still type
// checks — and demotes to Yellow along `TakesAddressOf` for the same reason.
void (*slot)(void) = blocked;

int main(void) {
  outer();
  return 0;
}

// CHECK:      item SLOT kind=global color=yellow reason=stub-callee via=blocked edge=TakesAddressOf chain=SLOT->blocked construct=inline-asm
// CHECK-NEXT: item blocked kind=function color=red reason=inadmissible construct=inline-asm
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: item caller kind=function color=yellow reason=stub-callee via=blocked edge=Calls chain=caller->blocked construct=inline-asm
// CHECK-NEXT: item outer kind=function color=green reason=admissible
// CHECK-NEXT: tally green=2 yellow=2 red=1
// CHECK-NOT:  item
