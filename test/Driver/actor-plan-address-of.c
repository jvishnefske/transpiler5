// FR-62 (slice 2): the two semantic upgrades slice 1's AddressOfGlobal
// edge kind enables, closing the E5 prototype's recorded direction-blind
// gap:
//  (a) ADDRESS-TAKING IS A WRITE. aleft/aright only READ their globals
//      textually, but each takes an address a caller may store through, so
//      the planner counts the AddressOfGlobal edge as a write: `both`,
//      whose closure address-takes LEFT and RIGHT while its direct
//      footprint is empty, is a cross-actor WRITER and forces the two
//      actors to merge with a note -- under the E5 prototype (writes =
//      WritesGlobal only) they stayed split and `both` was mis-roled as a
//      harmless cross READER.
//  (b) A POINTER GLOBAL AND ITS POINTEE ARE ONE ACTOR (E1's measured
//      rule): the global-to-global AddressOfGlobal edge of Q's initializer
//      `arr + 2` seeds ARR and Q into one cluster silently. The second
//      invocation pins the OVERRIDE interaction: pinning Q away by
//      --actor-map cannot split the pair -- the R0 condensation merges the
//      pinned actor back with a note, so overrides stay incapable of
//      creating unsoundness.
//
// RUN: emitrust-cc --emit=actor-plan %s -o - 2>%t.err | FileCheck %s --match-full-lines
// RUN: FileCheck %s --check-prefix=WARN < %t.err
//
// RUN: echo "Q pinned_q" > %t.map
// RUN: emitrust-cc --emit=actor-plan --actor-map %t.map %s -o - 2>%t.err2 | FileCheck %s --match-full-lines
// RUN: FileCheck %s --check-prefix=PINNED < %t.err2

int arr[4];
int *q = arr + 2;

int left;
int right;

int walk(void) { return q[0] + arr[1]; }

int *aleft(void) { return &left; }

int *aright(void) { return &right; }

int *both(int c) { return c ? aleft() : aright(); }

// The owned-global partition is identical with and without the pin; only
// the note trail differs (checked on stderr per invocation below).
// CHECK:      actor ARR globals=ARR,Q
// CHECK-NEXT: actor LEFT globals=LEFT,RIGHT
// CHECK-NEXT: fn aleft role=arm actor=LEFT
// CHECK-NEXT: fn aright role=arm actor=LEFT
// CHECK-NEXT: fn both role=arm actor=LEFT
// CHECK-NEXT: fn walk role=arm actor=ARR

// The writer-rule condensation is reported as a driver warning.
// WARN: warning: actor plan: function 'both' closure-writes globals of actors {LEFT,RIGHT}; merged into 'LEFT'

// Pinning Q apart from ARR is condensed back, with the pointer rule named.
// PINNED: warning: actor plan: global 'Q' takes the address of global 'ARR'; actors {ARR,pinned_q} merged into 'ARR'
// PINNED: warning: actor plan: function 'both' closure-writes globals of actors {LEFT,RIGHT}; merged into 'LEFT'
