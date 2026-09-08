// RUN: split-file %s %t
// RUN: not timeout 120 emitrust-import-c %t/goto-chain.c 2>&1 | FileCheck %s --check-prefix=CHAIN
// RUN: not timeout 120 emitrust-import-c %t/if-chain.c 2>&1 | FileCheck %s --check-prefix=IFCHAIN
// RUN: timeout 120 emitrust-import-c %t/at-limit.c | FileCheck %s --check-prefix=LIMIT
// RUN: timeout 120 emitrust-import-c %t/terminated.c | FileCheck %s --check-prefix=TERM
// RUN: timeout 120 emitrust-import-c %t/untargeted.c | FileCheck %s --check-prefix=UNTARGETED
// RUN: timeout 120 emitrust-cc --emit=import --recover %t/goto-chain.c -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RECOVER

// FR-197's LAST residual, the `goto` ladder. FR-198 bounded the plain switch
// path and FR-207 the dispatch path; a `goto` ladder walked past both,
// because the switch that feeds it has a fall-through chain of ONE -- every
// arm is `goto Lk;`, which terminates the section -- while the ladder itself
// lives in the labels after the switch. FR-198 re-measured that residual at
// 29.7s / 83,383 emitted lines and left it open. This file pins the bound
// that closes it, and pins that the compiler TERMINATES.
//
// Measured on the shape in `goto-chain.c`, emitted output is CUBIC in the
// chain, not quadratic like the two switch paths: 44/76/120/178/252/344/456/
// 590/748 lines at N=2..10 has an exactly constant THIRD difference of 2
// (about N^3/3). Wall time is worse again -- 14344 lines / 1.11s at a chain
// of 32, 44296 / 8.92s at 48, 83448 / 47.76s at 60, 129788 / 133.27s at 70,
// 190728 / 268.09s at 80, and at 100 NO output and NO diagnostic at all in
// 400s (rc=124). A compile that neither returns nor diagnoses is the one
// outcome this repo forbids outright, and a 30-second compile with no
// diagnostic is already past what it tolerates.
//
// WHERE THE BOUND BITES, censused rather than guessed: the longest chain in
// EndToEnd/Import/Driver/Project/Conversion/Kernel/Fuzz (963 files) is 1, in
// c-testsuite (220) is 5, in Cpp17Suite + RealWorld (52) is 0, and in the
// TRACTOR corpus (184) is 2. The largest real chain anywhere is 5 against a
// bound of 32.
//
// TWO CONTROLS pin the bounded quantity as the CHAIN, and not the label count
// or the `goto` count. They are the reason this bound is not a capability
// regression for ordinary `goto` code, which is common in real C:
//   `terminated.c`  the SAME 33 labels, the SAME 33 gotos and the SAME
//                   switch, with every section ending in `return`. Zero
//                   fall-through edges between labels: measured exactly
//                   4N + 18 emitted lines, flat time out to N = 128.
//                   ADMITTED.
//   `untargeted.c`  33 consecutive fall-through labels that no `goto`
//                   targets, so not one of them is a join. Measured N + 12
//                   lines, flat to N = 128. ADMITTED.
// A bound on either quantity would have refused both of these.
//
// `if-chain.c` is the same ladder with NO switch at all, entered by the
// ordinary C error-cleanup idiom (`if (...) goto Lk;`). It blows up
// identically (13312 lines / 1.21s at a chain of 32), which is why the bound
// is measured over the whole function body rather than over any switch. The
// filing's lead -- that the driver is graph IRREDUCIBILITY -- is refuted by
// two further controls not kept here: a genuinely irreducible two-entry loop
// lowers to 52 lines flat in N, and closing this ladder with a back edge, so
// that it becomes an N-entry loop, makes it CHEAPER (2638 lines / 0.21s at
// N=32), not worse.
//
// The `timeout` on every RUN line is deliberate: a regression to hanging must
// fail loudly and fast instead of wedging the suite until lit's 600s cap.

// The error is located at the label that OPENS the chain and the note at the
// one that closes it, because the span is what a user has to break.
// CHAIN: goto-chain.c:39:1: error: unsupported: goto ladder of 33 labels that fall into one another (limit 32); structurizing it duplicates the tail into every entry and does not finish
// CHAIN-NEXT: goto-chain.c:71:1: note: the chain ends at this label; ending a section with 'return', 'break' or 'goto' splits it

// IFCHAIN: if-chain.c:37:1: error: unsupported: goto ladder of 33 labels that fall into one another (limit 32); structurizing it duplicates the tail into every entry and does not finish
// IFCHAIN-NEXT: if-chain.c:69:1: note: the chain ends at this label; ending a section with 'return', 'break' or 'goto' splits it

// The same shape one label shorter still imports: the bound is "> 32" and
// there is no off-by-one over-rejection at the boundary.
// LIMIT-LABEL: func.func @chain
// LIMIT: cf.switch %{{.*}} : i32, [

// TERM-LABEL: func.func @chain
// TERM: cf.switch %{{.*}} : i32, [

// UNTARGETED-LABEL: func.func @chain
// UNTARGETED: return

// Under --recover the rejection degrades to a warning plus an
// `unimplemented!()` stub carrying the same text, the note is preserved, and
// the compile TERMINATES. Nothing is silently truncated.
// RECOVER: goto-chain.c:39:1: warning: unsupported: goto ladder of 33 labels that fall into one another (limit 32); structurizing it duplicates the tail into every entry and does not finish (recovered: emitted an unimplemented!() stub with the mapped signature)
// RECOVER-NEXT: goto-chain.c:71:1: note: the chain ends at this label; ending a section with 'return', 'break' or 'goto' splits it
// RECOVER: emitrust.call_opaque "unimplemented!"

//--- goto-chain.c
int chain(int sel) {
  int acc = 0;
  switch (sel) {
  case 0: goto L0;
  case 1: goto L1;
  case 2: goto L2;
  case 3: goto L3;
  case 4: goto L4;
  case 5: goto L5;
  case 6: goto L6;
  case 7: goto L7;
  case 8: goto L8;
  case 9: goto L9;
  case 10: goto L10;
  case 11: goto L11;
  case 12: goto L12;
  case 13: goto L13;
  case 14: goto L14;
  case 15: goto L15;
  case 16: goto L16;
  case 17: goto L17;
  case 18: goto L18;
  case 19: goto L19;
  case 20: goto L20;
  case 21: goto L21;
  case 22: goto L22;
  case 23: goto L23;
  case 24: goto L24;
  case 25: goto L25;
  case 26: goto L26;
  case 27: goto L27;
  case 28: goto L28;
  case 29: goto L29;
  case 30: goto L30;
  case 31: goto L31;
  case 32: goto L32;
  default: return -1;
  }
L0: acc += 1;
L1: acc += 2;
L2: acc += 3;
L3: acc += 4;
L4: acc += 5;
L5: acc += 6;
L6: acc += 7;
L7: acc += 8;
L8: acc += 9;
L9: acc += 10;
L10: acc += 11;
L11: acc += 12;
L12: acc += 13;
L13: acc += 14;
L14: acc += 15;
L15: acc += 16;
L16: acc += 17;
L17: acc += 18;
L18: acc += 19;
L19: acc += 20;
L20: acc += 21;
L21: acc += 22;
L22: acc += 23;
L23: acc += 24;
L24: acc += 25;
L25: acc += 26;
L26: acc += 27;
L27: acc += 28;
L28: acc += 29;
L29: acc += 30;
L30: acc += 31;
L31: acc += 32;
L32: acc += 33;
  return acc;
}

//--- if-chain.c
int chain(int sel) {
  int acc = 0;
  if (sel == 0) goto L0;
  if (sel == 1) goto L1;
  if (sel == 2) goto L2;
  if (sel == 3) goto L3;
  if (sel == 4) goto L4;
  if (sel == 5) goto L5;
  if (sel == 6) goto L6;
  if (sel == 7) goto L7;
  if (sel == 8) goto L8;
  if (sel == 9) goto L9;
  if (sel == 10) goto L10;
  if (sel == 11) goto L11;
  if (sel == 12) goto L12;
  if (sel == 13) goto L13;
  if (sel == 14) goto L14;
  if (sel == 15) goto L15;
  if (sel == 16) goto L16;
  if (sel == 17) goto L17;
  if (sel == 18) goto L18;
  if (sel == 19) goto L19;
  if (sel == 20) goto L20;
  if (sel == 21) goto L21;
  if (sel == 22) goto L22;
  if (sel == 23) goto L23;
  if (sel == 24) goto L24;
  if (sel == 25) goto L25;
  if (sel == 26) goto L26;
  if (sel == 27) goto L27;
  if (sel == 28) goto L28;
  if (sel == 29) goto L29;
  if (sel == 30) goto L30;
  if (sel == 31) goto L31;
  if (sel == 32) goto L32;
  return -1;
L0: acc += 1;
L1: acc += 2;
L2: acc += 3;
L3: acc += 4;
L4: acc += 5;
L5: acc += 6;
L6: acc += 7;
L7: acc += 8;
L8: acc += 9;
L9: acc += 10;
L10: acc += 11;
L11: acc += 12;
L12: acc += 13;
L13: acc += 14;
L14: acc += 15;
L15: acc += 16;
L16: acc += 17;
L17: acc += 18;
L18: acc += 19;
L19: acc += 20;
L20: acc += 21;
L21: acc += 22;
L22: acc += 23;
L23: acc += 24;
L24: acc += 25;
L25: acc += 26;
L26: acc += 27;
L27: acc += 28;
L28: acc += 29;
L29: acc += 30;
L30: acc += 31;
L31: acc += 32;
L32: acc += 33;
  return acc;
}

//--- at-limit.c
int chain(int sel) {
  int acc = 0;
  switch (sel) {
  case 0: goto L0;
  case 1: goto L1;
  case 2: goto L2;
  case 3: goto L3;
  case 4: goto L4;
  case 5: goto L5;
  case 6: goto L6;
  case 7: goto L7;
  case 8: goto L8;
  case 9: goto L9;
  case 10: goto L10;
  case 11: goto L11;
  case 12: goto L12;
  case 13: goto L13;
  case 14: goto L14;
  case 15: goto L15;
  case 16: goto L16;
  case 17: goto L17;
  case 18: goto L18;
  case 19: goto L19;
  case 20: goto L20;
  case 21: goto L21;
  case 22: goto L22;
  case 23: goto L23;
  case 24: goto L24;
  case 25: goto L25;
  case 26: goto L26;
  case 27: goto L27;
  case 28: goto L28;
  case 29: goto L29;
  case 30: goto L30;
  case 31: goto L31;
  default: return -1;
  }
L0: acc += 1;
L1: acc += 2;
L2: acc += 3;
L3: acc += 4;
L4: acc += 5;
L5: acc += 6;
L6: acc += 7;
L7: acc += 8;
L8: acc += 9;
L9: acc += 10;
L10: acc += 11;
L11: acc += 12;
L12: acc += 13;
L13: acc += 14;
L14: acc += 15;
L15: acc += 16;
L16: acc += 17;
L17: acc += 18;
L18: acc += 19;
L19: acc += 20;
L20: acc += 21;
L21: acc += 22;
L22: acc += 23;
L23: acc += 24;
L24: acc += 25;
L25: acc += 26;
L26: acc += 27;
L27: acc += 28;
L28: acc += 29;
L29: acc += 30;
L30: acc += 31;
L31: acc += 32;
  return acc;
}

//--- terminated.c
int chain(int sel) {
  int acc = 0;
  switch (sel) {
  case 0: goto L0;
  case 1: goto L1;
  case 2: goto L2;
  case 3: goto L3;
  case 4: goto L4;
  case 5: goto L5;
  case 6: goto L6;
  case 7: goto L7;
  case 8: goto L8;
  case 9: goto L9;
  case 10: goto L10;
  case 11: goto L11;
  case 12: goto L12;
  case 13: goto L13;
  case 14: goto L14;
  case 15: goto L15;
  case 16: goto L16;
  case 17: goto L17;
  case 18: goto L18;
  case 19: goto L19;
  case 20: goto L20;
  case 21: goto L21;
  case 22: goto L22;
  case 23: goto L23;
  case 24: goto L24;
  case 25: goto L25;
  case 26: goto L26;
  case 27: goto L27;
  case 28: goto L28;
  case 29: goto L29;
  case 30: goto L30;
  case 31: goto L31;
  case 32: goto L32;
  default: return -1;
  }
L0: acc += 1;
  return acc;
L1: acc += 2;
  return acc;
L2: acc += 3;
  return acc;
L3: acc += 4;
  return acc;
L4: acc += 5;
  return acc;
L5: acc += 6;
  return acc;
L6: acc += 7;
  return acc;
L7: acc += 8;
  return acc;
L8: acc += 9;
  return acc;
L9: acc += 10;
  return acc;
L10: acc += 11;
  return acc;
L11: acc += 12;
  return acc;
L12: acc += 13;
  return acc;
L13: acc += 14;
  return acc;
L14: acc += 15;
  return acc;
L15: acc += 16;
  return acc;
L16: acc += 17;
  return acc;
L17: acc += 18;
  return acc;
L18: acc += 19;
  return acc;
L19: acc += 20;
  return acc;
L20: acc += 21;
  return acc;
L21: acc += 22;
  return acc;
L22: acc += 23;
  return acc;
L23: acc += 24;
  return acc;
L24: acc += 25;
  return acc;
L25: acc += 26;
  return acc;
L26: acc += 27;
  return acc;
L27: acc += 28;
  return acc;
L28: acc += 29;
  return acc;
L29: acc += 30;
  return acc;
L30: acc += 31;
  return acc;
L31: acc += 32;
  return acc;
L32: acc += 33;
  return acc;
  return acc;
}

//--- untargeted.c
int chain(int sel) {
  int acc = 0;
L0: acc += 1;
L1: acc += 2;
L2: acc += 3;
L3: acc += 4;
L4: acc += 5;
L5: acc += 6;
L6: acc += 7;
L7: acc += 8;
L8: acc += 9;
L9: acc += 10;
L10: acc += 11;
L11: acc += 12;
L12: acc += 13;
L13: acc += 14;
L14: acc += 15;
L15: acc += 16;
L16: acc += 17;
L17: acc += 18;
L18: acc += 19;
L19: acc += 20;
L20: acc += 21;
L21: acc += 22;
L22: acc += 23;
L23: acc += 24;
L24: acc += 25;
L25: acc += 26;
L26: acc += 27;
L27: acc += 28;
L28: acc += 29;
L29: acc += 30;
L30: acc += 31;
L31: acc += 32;
L32: acc += 33;
  return acc;
}
