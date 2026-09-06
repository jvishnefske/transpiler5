// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/decl-chain.c 2>&1 | FileCheck %s --check-prefix=DECL
// RUN: not emitrust-import-c %t/label-chain.c 2>&1 | FileCheck %s --check-prefix=LABEL
// RUN: emitrust-import-c %t/at-limit.c | FileCheck %s --check-prefix=LIMIT

// FR-198 pins the FRONTIER of the fall-through-chain bound: the point past
// which a switch the guarded-ladder lowering cannot claim is REFUSED with a
// located diagnostic instead of handed to the duplicating `cf.switch`
// lowering.
//
// That lowering copies the tail of the chain into every arm, so a chain of N
// sections emits ~N^2/2 copies: FR-197 measured 0.86s at N=32, 2.87s at
// N=40, 7.93s at N=48 and >360s with NO output and NO diagnostic at N=100.
// A compile that neither returns nor diagnoses is the one outcome this repo
// forbids unconditionally, so the importer refuses past 32 -- the largest
// measured chain whose compile still finishes in about a second.
//
// Both refused bodies below are chains of 41 sections that the guarded form
// cannot express: the first declares a variable at switch-body scope (C
// keeps it live for later cases; a guarded `if` is its own Rust scope), the
// second carries a label, which is a `goto` target with no guarded-form
// equivalent. The note names the exact statement that cost the shape its
// linear lowering, so the diagnostic says what to change and not merely
// that the program is too big.
//
// `at-limit.c` is the SAME refused shape one section shorter (32) and must
// still import: the bound is "> 32", and a shape the ladder cannot claim
// keeps working right up to it.

// DECL: decl-chain.c:3:3: error: unsupported: switch with a fall-through chain of 41 cases (limit 32); structurizing it duplicates the tail into every arm and does not finish
// DECL-NEXT: decl-chain.c:6:5: note: this statement blocks the linear guarded lowering

// LABEL: label-chain.c:3:3: error: unsupported: switch with a fall-through chain of 41 cases (limit 32); structurizing it duplicates the tail into every arm and does not finish
// LABEL-NEXT: label-chain.c:15:3: note: this statement blocks the linear guarded lowering

// A 32-section chain the ladder cannot claim still gets the ordinary
// duplicating lowering: one `cf.switch` with one destination block per
// section and no index block argument.
// LIMIT-LABEL: func.func @chain
// LIMIT: cf.switch %{{.*}} : i32, [
// LIMIT-NEXT: default: ^bb{{[0-9]+}},

//--- decl-chain.c
int chain(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    ;
    int carried = 7;
    acc += 1;
  case 1:
    acc += 2;
  case 2:
    acc += 3;
  case 3:
    acc += 4;
  case 4:
    acc += 5;
  case 5:
    acc += 6;
  case 6:
    acc += 7;
  case 7:
    acc += 8;
  case 8:
    acc += 9;
  case 9:
    acc += 10;
  case 10:
    acc += 11;
  case 11:
    acc += 12;
  case 12:
    acc += 13;
  case 13:
    acc += 14;
  case 14:
    acc += 15;
  case 15:
    acc += 16;
  case 16:
    acc += 17;
  case 17:
    acc += 18;
  case 18:
    acc += 19;
  case 19:
    acc += 20;
  case 20:
    acc += 21;
  case 21:
    acc += 22;
  case 22:
    acc += 23;
  case 23:
    acc += 24;
  case 24:
    acc += 25;
  case 25:
    acc += 26;
  case 26:
    acc += 27;
  case 27:
    acc += 28;
  case 28:
    acc += 29;
  case 29:
    acc += 30;
  case 30:
    acc += 31;
  case 31:
    acc += 32;
  case 32:
    acc += 33;
  case 33:
    acc += 34;
  case 34:
    acc += 35;
  case 35:
    acc += 36;
  case 36:
    acc += 37;
  case 37:
    acc += 38;
  case 38:
    acc += 39;
  case 39:
    acc += 40;
  default:
    break;
  }
  return acc;
}

//--- label-chain.c
int chain(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
  case 1:
    acc += 2;
  case 2:
    acc += 3;
  case 3:
    acc += 4;
  case 4:
    acc += 5;
  case 5:
  again:
    if (acc < 0) goto again;
    acc += 6;
  case 6:
    acc += 7;
  case 7:
    acc += 8;
  case 8:
    acc += 9;
  case 9:
    acc += 10;
  case 10:
    acc += 11;
  case 11:
    acc += 12;
  case 12:
    acc += 13;
  case 13:
    acc += 14;
  case 14:
    acc += 15;
  case 15:
    acc += 16;
  case 16:
    acc += 17;
  case 17:
    acc += 18;
  case 18:
    acc += 19;
  case 19:
    acc += 20;
  case 20:
    acc += 21;
  case 21:
    acc += 22;
  case 22:
    acc += 23;
  case 23:
    acc += 24;
  case 24:
    acc += 25;
  case 25:
    acc += 26;
  case 26:
    acc += 27;
  case 27:
    acc += 28;
  case 28:
    acc += 29;
  case 29:
    acc += 30;
  case 30:
    acc += 31;
  case 31:
    acc += 32;
  case 32:
    acc += 33;
  case 33:
    acc += 34;
  case 34:
    acc += 35;
  case 35:
    acc += 36;
  case 36:
    acc += 37;
  case 37:
    acc += 38;
  case 38:
    acc += 39;
  case 39:
    acc += 40;
  default:
    break;
  }
  return acc;
}

//--- at-limit.c
int chain(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    ;
    int carried = 7;
    acc += 1;
  case 1:
    acc += 2;
  case 2:
    acc += 3;
  case 3:
    acc += 4;
  case 4:
    acc += 5;
  case 5:
    acc += 6;
  case 6:
    acc += 7;
  case 7:
    acc += 8;
  case 8:
    acc += 9;
  case 9:
    acc += 10;
  case 10:
    acc += 11;
  case 11:
    acc += 12;
  case 12:
    acc += 13;
  case 13:
    acc += 14;
  case 14:
    acc += 15;
  case 15:
    acc += 16;
  case 16:
    acc += 17;
  case 17:
    acc += 18;
  case 18:
    acc += 19;
  case 19:
    acc += 20;
  case 20:
    acc += 21;
  case 21:
    acc += 22;
  case 22:
    acc += 23;
  case 23:
    acc += 24;
  case 24:
    acc += 25;
  case 25:
    acc += 26;
  case 26:
    acc += 27;
  case 27:
    acc += 28;
  case 28:
    acc += 29;
  case 29:
    acc += 30;
  case 30:
    acc += 31;
  default:
    break;
  }
  return acc;
}
