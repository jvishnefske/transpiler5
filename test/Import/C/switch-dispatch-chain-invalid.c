// RUN: split-file %s %t
// RUN: not timeout 120 emitrust-import-c %t/nested-chain.c 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not timeout 120 emitrust-import-c %t/duff-chain.c 2>&1 | FileCheck %s --check-prefix=DUFF
// RUN: not timeout 120 emitrust-import-c %t/prefix-chain.c 2>&1 | FileCheck %s --check-prefix=PREFIX
// RUN: timeout 120 emitrust-import-c %t/at-limit.c | FileCheck %s --check-prefix=LIMIT
// RUN: timeout 120 emitrust-import-c %t/wide-no-chain.c | FileCheck %s --check-prefix=WIDE

// FR-207 pins the fall-through-chain bound on the DISPATCH path -- the
// residual FR-198 left open. FR-198 bounded the plain-body path only, so a
// chain whose labels are not top-level children of the switch body (nested
// one compound down, or inside a loop as in Duff's device, or preceded by a
// statement) routed to `emitDispatchSwitch`, where the chain was never
// measured: no bound, no diagnostic, and the compile did not come back.
//
// The nesting changes the ROUTE, not the blowup. Re-measured on the shape in
// `nested-chain.c`, the dispatch path reproduces FR-197's plain-path curve
// almost constant for constant -- 438/2285/6565/14300/44236/100283/190635
// emitted lines and 0.11/0.11/0.31/1.11/9.72/48.26/192.70s at
// N=8/16/24/32/48/64/80, against FR-197's 445/2292/6572/14307/../44243 for
// the plain path -- and past N=80 it produces no output and no diagnostic at
// all. A compile that neither returns nor diagnoses is the one outcome this
// repo forbids unconditionally, so the same limit of 32 now applies here,
// with the same justification (32 costs 1.11s on this path, 0.86s on the
// plain one).
//
// The `timeout` on every RUN line is deliberate: a regression to hanging must
// fail this test loudly and quickly instead of wedging the suite until lit's
// 600s per-test cap.
//
// Each rejection carries a `note:` naming the clause that cost the body its
// structured lowering, because that -- not the size -- is what a user would
// have to change.

// NESTED: nested-chain.c:3:3: error: unsupported: switch with a fall-through chain of 33 cases (limit 32); structurizing it duplicates the tail into every arm and does not finish
// NESTED-NEXT: nested-chain.c:5:7: note: this label is nested inside a statement, so the switch takes the duplicating dispatch lowering

// DUFF: duff-chain.c:3:3: error: unsupported: switch with a fall-through chain of 33 cases (limit 32); structurizing it duplicates the tail into every arm and does not finish
// DUFF-NEXT: duff-chain.c:5:3: note: this label is nested inside a statement, so the switch takes the duplicating dispatch lowering

// PREFIX: prefix-chain.c:3:3: error: unsupported: switch with a fall-through chain of 33 cases (limit 32); structurizing it duplicates the tail into every arm and does not finish
// PREFIX-NEXT: prefix-chain.c:4:5: note: this statement precedes the first case label, so the switch takes the duplicating dispatch lowering

// The SAME nested shape one section shorter (32) still imports through the
// dispatch lowering: the bound is "> 32" and there is no off-by-one
// over-rejection at the boundary.
// LIMIT-LABEL: func.func @chain
// LIMIT: cf.switch %{{.*}} : i32, [
// LIMIT-NEXT: default: ^bb{{[0-9]+}},

// The bound measures the CHAIN, not the label count. This body nests TWICE as
// many labels as the rejected one, but every section ends in `break`, so
// there is no fall-through ladder to duplicate and it imports unchanged
// (measured 3N + 11 emitted lines, 0.11s flat out to N=128). Bounding the
// label count instead would have refused this, which is the over-rejection
// this clause exists to rule out.
// WIDE-LABEL: func.func @chain
// WIDE: cf.switch %{{.*}} : i32, [

//--- nested-chain.c
int chain(int x) {
  int acc = 0;
  switch (x) {
    if (x >= 0) {
      case 0: acc += 1;
      case 1: acc += 2;
      case 2: acc += 3;
      case 3: acc += 4;
      case 4: acc += 5;
      case 5: acc += 6;
      case 6: acc += 7;
      case 7: acc += 8;
      case 8: acc += 9;
      case 9: acc += 10;
      case 10: acc += 11;
      case 11: acc += 12;
      case 12: acc += 13;
      case 13: acc += 14;
      case 14: acc += 15;
      case 15: acc += 16;
      case 16: acc += 17;
      case 17: acc += 18;
      case 18: acc += 19;
      case 19: acc += 20;
      case 20: acc += 21;
      case 21: acc += 22;
      case 22: acc += 23;
      case 23: acc += 24;
      case 24: acc += 25;
      case 25: acc += 26;
      case 26: acc += 27;
      case 27: acc += 28;
      case 28: acc += 29;
      case 29: acc += 30;
      case 30: acc += 31;
      case 31: acc += 32;
      case 32: acc += 33;
    }
  }
  return acc;
}

//--- duff-chain.c
int chain(int x, int n) {
  int acc = 0;
  switch (x) {
  case 0: do { acc += 1;
  case 1:      acc += 2;
  case 2:      acc += 3;
  case 3:      acc += 4;
  case 4:      acc += 5;
  case 5:      acc += 6;
  case 6:      acc += 7;
  case 7:      acc += 8;
  case 8:      acc += 9;
  case 9:      acc += 10;
  case 10:      acc += 11;
  case 11:      acc += 12;
  case 12:      acc += 13;
  case 13:      acc += 14;
  case 14:      acc += 15;
  case 15:      acc += 16;
  case 16:      acc += 17;
  case 17:      acc += 18;
  case 18:      acc += 19;
  case 19:      acc += 20;
  case 20:      acc += 21;
  case 21:      acc += 22;
  case 22:      acc += 23;
  case 23:      acc += 24;
  case 24:      acc += 25;
  case 25:      acc += 26;
  case 26:      acc += 27;
  case 27:      acc += 28;
  case 28:      acc += 29;
  case 29:      acc += 30;
  case 30:      acc += 31;
  case 31:      acc += 32;
  case 32:      acc += 33;
             } while (--n > 0);
  }
  return acc;
}

//--- prefix-chain.c
int chain(int x) {
  int acc = 0;
  switch (x) {
    acc += 100;
  case 0: acc += 1;
  case 1: acc += 2;
  case 2: acc += 3;
  case 3: acc += 4;
  case 4: acc += 5;
  case 5: acc += 6;
  case 6: acc += 7;
  case 7: acc += 8;
  case 8: acc += 9;
  case 9: acc += 10;
  case 10: acc += 11;
  case 11: acc += 12;
  case 12: acc += 13;
  case 13: acc += 14;
  case 14: acc += 15;
  case 15: acc += 16;
  case 16: acc += 17;
  case 17: acc += 18;
  case 18: acc += 19;
  case 19: acc += 20;
  case 20: acc += 21;
  case 21: acc += 22;
  case 22: acc += 23;
  case 23: acc += 24;
  case 24: acc += 25;
  case 25: acc += 26;
  case 26: acc += 27;
  case 27: acc += 28;
  case 28: acc += 29;
  case 29: acc += 30;
  case 30: acc += 31;
  case 31: acc += 32;
  case 32: acc += 33;
  }
  return acc;
}

//--- at-limit.c
int chain(int x) {
  int acc = 0;
  switch (x) {
    if (x >= 0) {
      case 0: acc += 1;
      case 1: acc += 2;
      case 2: acc += 3;
      case 3: acc += 4;
      case 4: acc += 5;
      case 5: acc += 6;
      case 6: acc += 7;
      case 7: acc += 8;
      case 8: acc += 9;
      case 9: acc += 10;
      case 10: acc += 11;
      case 11: acc += 12;
      case 12: acc += 13;
      case 13: acc += 14;
      case 14: acc += 15;
      case 15: acc += 16;
      case 16: acc += 17;
      case 17: acc += 18;
      case 18: acc += 19;
      case 19: acc += 20;
      case 20: acc += 21;
      case 21: acc += 22;
      case 22: acc += 23;
      case 23: acc += 24;
      case 24: acc += 25;
      case 25: acc += 26;
      case 26: acc += 27;
      case 27: acc += 28;
      case 28: acc += 29;
      case 29: acc += 30;
      case 30: acc += 31;
      case 31: acc += 32;
    }
  }
  return acc;
}

//--- wide-no-chain.c
int chain(int x) {
  int acc = 0;
  switch (x) {
    if (x >= 0) {
      case 0: acc += 1; break;
      case 1: acc += 2; break;
      case 2: acc += 3; break;
      case 3: acc += 4; break;
      case 4: acc += 5; break;
      case 5: acc += 6; break;
      case 6: acc += 7; break;
      case 7: acc += 8; break;
      case 8: acc += 9; break;
      case 9: acc += 10; break;
      case 10: acc += 11; break;
      case 11: acc += 12; break;
      case 12: acc += 13; break;
      case 13: acc += 14; break;
      case 14: acc += 15; break;
      case 15: acc += 16; break;
      case 16: acc += 17; break;
      case 17: acc += 18; break;
      case 18: acc += 19; break;
      case 19: acc += 20; break;
      case 20: acc += 21; break;
      case 21: acc += 22; break;
      case 22: acc += 23; break;
      case 23: acc += 24; break;
      case 24: acc += 25; break;
      case 25: acc += 26; break;
      case 26: acc += 27; break;
      case 27: acc += 28; break;
      case 28: acc += 29; break;
      case 29: acc += 30; break;
      case 30: acc += 31; break;
      case 31: acc += 32; break;
      case 32: acc += 33; break;
      case 33: acc += 34; break;
      case 34: acc += 35; break;
      case 35: acc += 36; break;
      case 36: acc += 37; break;
      case 37: acc += 38; break;
      case 38: acc += 39; break;
      case 39: acc += 40; break;
      case 40: acc += 41; break;
      case 41: acc += 42; break;
      case 42: acc += 43; break;
      case 43: acc += 44; break;
      case 44: acc += 45; break;
      case 45: acc += 46; break;
      case 46: acc += 47; break;
      case 47: acc += 48; break;
      case 48: acc += 49; break;
      case 49: acc += 50; break;
      case 50: acc += 51; break;
      case 51: acc += 52; break;
      case 52: acc += 53; break;
      case 53: acc += 54; break;
      case 54: acc += 55; break;
      case 55: acc += 56; break;
      case 56: acc += 57; break;
      case 57: acc += 58; break;
      case 58: acc += 59; break;
      case 59: acc += 60; break;
      case 60: acc += 61; break;
      case 61: acc += 62; break;
      case 62: acc += 63; break;
      case 63: acc += 64; break;
    }
  }
  return acc;
}
