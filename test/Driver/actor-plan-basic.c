// FR-62 (slice 2): `--emit=actor-plan` default co-access planning. Pins the
// plan's pinned line format (`actor <name> globals=<sorted>` then `fn
// <symbol> role=<...> actor=<...>`, whole space-separated tokens, sorted)
// and the E2-measured seeding on a program with every default shape at
// once:
//  - a real multi-global cluster: bump/fill/counter_get co-access COUNTER,
//    SCALE and TABLE, so the three globals are ONE actor named by the
//    smallest owned symbol;
//  - the "@stdout" pseudo-global (slice 1's hosted-sink visibility made it
//    TOTAL): banner's `puts` call is a write of "@stdout", its own cluster
//    here because banner touches nothing else -- and c_main's own printf
//    does NOT drag "@stdout" anywhere, because the driver is exempt from
//    seeding;
//  - a singleton cluster (SOLO) and an untouched main-only global
//    (MAIN_ONLY), each its own actor;
//  - all four roles: c_main is the DRIVER; add (no footprint) is FREE;
//    peek's footprint spans COUNTER and SOLO through calls only, and since
//    its closure WRITES nothing the actors stay split and peek is a CROSS
//    message-passing client, not an arm (the writer rule fires on writes,
//    never on reads).
// No condensation fires, so the plan carries no `note` line. Two runs must
// be byte-identical (total determinism is part of the format contract).
//
// RUN: emitrust-cc --emit=actor-plan %s -o %t.plan
// RUN: emitrust-cc --emit=actor-plan %s -o %t.plan2
// RUN: diff %t.plan %t.plan2
// RUN: FileCheck %s --match-full-lines < %t.plan

#include <stdio.h>

int counter;
int scale = 3;
int table[4];
int solo;
int main_only;

int add(int a, int b) { return a + b; }

void bump(void) { counter += 1; }

void fill(void) {
  for (int i = 0; i < 4; ++i)
    table[i] = counter * scale;
}

int counter_get(void) { return counter; }

int solo_get(void) { return solo; }

void banner(void) { puts("start"); }

int peek(void) { return add(counter_get(), solo_get()); }

int main(void) {
  banner();
  bump();
  fill();
  main_only = peek();
  printf("%d\n", main_only);
  return 0;
}

// CHECK:      actor @stdout globals=@stdout
// CHECK-NEXT: actor COUNTER globals=COUNTER,SCALE,TABLE
// CHECK-NEXT: actor MAIN_ONLY globals=MAIN_ONLY
// CHECK-NEXT: actor SOLO globals=SOLO
// CHECK-NEXT: fn add role=free actor=-
// CHECK-NEXT: fn banner role=arm actor=@stdout
// CHECK-NEXT: fn bump role=arm actor=COUNTER
// CHECK-NEXT: fn c_main role=driver actor=-
// CHECK-NEXT: fn counter_get role=arm actor=COUNTER
// CHECK-NEXT: fn fill role=arm actor=COUNTER
// CHECK-NEXT: fn peek role=cross actor=-
// CHECK-NEXT: fn solo_get role=arm actor=SOLO
// CHECK-NOT:  {{.+}}
