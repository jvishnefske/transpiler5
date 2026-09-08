// REQUIRES: cargo
// FR-197's last residual. This is the byte-diff oracle for the ADMITTED side
// of the `goto`-ladder fall-through bound: the bound refuses chains past 32,
// and this file pins that everything up to and around it still compiles AND
// still computes the right answers. `cargo build` succeeding is compile-only
// evidence and cannot see a miscompile, so every function's results are
// diffed against the clang-built native binary.
//
// Four shapes, chosen so that the bound's two controls are exercised at a
// length the bound would have refused if it counted the wrong thing:
//   `switch_ladder`   a 32-label `goto` ladder fed by a `switch` -- EXACTLY
//                     the boundary (the limit is "> 32"), and the shape whose
//                     blowup was measured: 14344 emitted lines / 1.11s here,
//                     83448 / 47.76s at a chain of 60, 190728 / 268.09s at
//                     80. It still lowers, so it still has to be right.
//   `cleanup_ladder`  the ordinary C error-cleanup idiom -- `if (...) goto
//                     Lk;` guards over sections that fall through into one
//                     another, with no `switch` anywhere. This is the shape
//                     that made the census necessary: it is common in real C,
//                     it blows up identically (13312 lines / 1.21s at a chain
//                     of 32), and the longest such chain found anywhere in
//                     the corpora is 8, which is the length used here.
//   `terminated`      33 labels and 33 gotos -- one PAST the bound -- with
//                     every section ending in `return`. Chain 1, so admitted.
//                     A label-count or goto-count bound would have refused
//                     this; measured 4N + 18 emitted lines, flat time.
//   `untargeted`      33 consecutive fall-through labels that no `goto`
//                     targets. Also past the bound by label count, also
//                     admitted, because a label nothing jumps to is not a
//                     join and nothing is duplicated into it.
//
// Every scrutinee is derived from `argc`, inside and outside the label range,
// so nothing folds at compile time and a miscompile cannot hide behind a
// constant.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/goto_ladder_chain > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int switch_ladder(int sel) {
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
L0: acc = acc * 3 + 1;
L1: acc = acc * 3 + 2;
L2: acc = acc * 3 + 3;
L3: acc = acc * 3 + 4;
L4: acc = acc * 3 + 5;
L5: acc = acc * 3 + 6;
L6: acc = acc * 3 + 7;
L7: acc = acc * 3 + 8;
L8: acc = acc * 3 + 9;
L9: acc = acc * 3 + 10;
L10: acc = acc * 3 + 11;
L11: acc = acc * 3 + 12;
L12: acc = acc * 3 + 13;
L13: acc = acc * 3 + 14;
L14: acc = acc * 3 + 15;
L15: acc = acc * 3 + 16;
L16: acc = acc * 3 + 17;
L17: acc = acc * 3 + 18;
L18: acc = acc * 3 + 19;
L19: acc = acc * 3 + 20;
L20: acc = acc * 3 + 21;
L21: acc = acc * 3 + 22;
L22: acc = acc * 3 + 23;
L23: acc = acc * 3 + 24;
L24: acc = acc * 3 + 25;
L25: acc = acc * 3 + 26;
L26: acc = acc * 3 + 27;
L27: acc = acc * 3 + 28;
L28: acc = acc * 3 + 29;
L29: acc = acc * 3 + 30;
L30: acc = acc * 3 + 31;
L31: acc = acc * 3 + 32;
  return acc;
}

int cleanup_ladder(int sel) {
  int acc = 0;
  if (sel == 0) goto C0;
  if (sel == 1) goto C1;
  if (sel == 2) goto C2;
  if (sel == 3) goto C3;
  if (sel == 4) goto C4;
  if (sel == 5) goto C5;
  if (sel == 6) goto C6;
  if (sel == 7) goto C7;
  return -1;
C0: acc = acc * 5 + 2; printf("c0 %d\n", acc);
C1: acc = acc * 5 + 3; printf("c1 %d\n", acc);
C2: acc = acc * 5 + 4; printf("c2 %d\n", acc);
C3: acc = acc * 5 + 5; printf("c3 %d\n", acc);
C4: acc = acc * 5 + 6; printf("c4 %d\n", acc);
C5: acc = acc * 5 + 7; printf("c5 %d\n", acc);
C6: acc = acc * 5 + 8; printf("c6 %d\n", acc);
C7: acc = acc * 5 + 9; printf("c7 %d\n", acc);
  return acc;
}

int terminated(int sel) {
  int acc = 0;
  switch (sel) {
  case 0: goto T0;
  case 1: goto T1;
  case 2: goto T2;
  case 3: goto T3;
  case 4: goto T4;
  case 5: goto T5;
  case 6: goto T6;
  case 7: goto T7;
  case 8: goto T8;
  case 9: goto T9;
  case 10: goto T10;
  case 11: goto T11;
  case 12: goto T12;
  case 13: goto T13;
  case 14: goto T14;
  case 15: goto T15;
  case 16: goto T16;
  case 17: goto T17;
  case 18: goto T18;
  case 19: goto T19;
  case 20: goto T20;
  case 21: goto T21;
  case 22: goto T22;
  case 23: goto T23;
  case 24: goto T24;
  case 25: goto T25;
  case 26: goto T26;
  case 27: goto T27;
  case 28: goto T28;
  case 29: goto T29;
  case 30: goto T30;
  case 31: goto T31;
  case 32: goto T32;
  default: return -1;
  }
T0: acc = acc * 7 + 3;
  return acc;
T1: acc = acc * 7 + 4;
  return acc;
T2: acc = acc * 7 + 5;
  return acc;
T3: acc = acc * 7 + 6;
  return acc;
T4: acc = acc * 7 + 7;
  return acc;
T5: acc = acc * 7 + 8;
  return acc;
T6: acc = acc * 7 + 9;
  return acc;
T7: acc = acc * 7 + 10;
  return acc;
T8: acc = acc * 7 + 11;
  return acc;
T9: acc = acc * 7 + 12;
  return acc;
T10: acc = acc * 7 + 13;
  return acc;
T11: acc = acc * 7 + 14;
  return acc;
T12: acc = acc * 7 + 15;
  return acc;
T13: acc = acc * 7 + 16;
  return acc;
T14: acc = acc * 7 + 17;
  return acc;
T15: acc = acc * 7 + 18;
  return acc;
T16: acc = acc * 7 + 19;
  return acc;
T17: acc = acc * 7 + 20;
  return acc;
T18: acc = acc * 7 + 21;
  return acc;
T19: acc = acc * 7 + 22;
  return acc;
T20: acc = acc * 7 + 23;
  return acc;
T21: acc = acc * 7 + 24;
  return acc;
T22: acc = acc * 7 + 25;
  return acc;
T23: acc = acc * 7 + 26;
  return acc;
T24: acc = acc * 7 + 27;
  return acc;
T25: acc = acc * 7 + 28;
  return acc;
T26: acc = acc * 7 + 29;
  return acc;
T27: acc = acc * 7 + 30;
  return acc;
T28: acc = acc * 7 + 31;
  return acc;
T29: acc = acc * 7 + 32;
  return acc;
T30: acc = acc * 7 + 33;
  return acc;
T31: acc = acc * 7 + 34;
  return acc;
T32: acc = acc * 7 + 35;
  return acc;
  return acc;
}

int untargeted(int sel) {
  int acc = sel;
U0: acc = acc * 2 + 1;
U1: acc = acc * 2 + 2;
U2: acc = acc * 2 + 3;
U3: acc = acc * 2 + 4;
U4: acc = acc * 2 + 5;
U5: acc = acc * 2 + 6;
U6: acc = acc * 2 + 7;
U7: acc = acc * 2 + 8;
U8: acc = acc * 2 + 9;
U9: acc = acc * 2 + 10;
U10: acc = acc * 2 + 11;
U11: acc = acc * 2 + 12;
U12: acc = acc * 2 + 13;
U13: acc = acc * 2 + 14;
U14: acc = acc * 2 + 15;
U15: acc = acc * 2 + 16;
U16: acc = acc * 2 + 17;
U17: acc = acc * 2 + 18;
U18: acc = acc * 2 + 19;
U19: acc = acc * 2 + 20;
U20: acc = acc * 2 + 21;
U21: acc = acc * 2 + 22;
U22: acc = acc * 2 + 23;
U23: acc = acc * 2 + 24;
U24: acc = acc * 2 + 25;
U25: acc = acc * 2 + 26;
U26: acc = acc * 2 + 27;
U27: acc = acc * 2 + 28;
U28: acc = acc * 2 + 29;
U29: acc = acc * 2 + 30;
U30: acc = acc * 2 + 31;
U31: acc = acc * 2 + 32;
U32: acc = acc * 2 + 33;
  return acc;
}

int main(int argc, char **argv) {
  int seed = argc;
  for (int i = -2; i < 36; ++i) {
    int x = i * seed;
    printf("sw %d -> %d\n", x, switch_ladder(x));
  }
  for (int i = -1; i < 10; ++i)
    printf("cl %d -> %d\n", i, cleanup_ladder(i * seed));
  for (int i = -1; i < 35; ++i)
    printf("tm %d -> %d\n", i, terminated(i * seed));
  for (int i = 0; i < 5; ++i)
    printf("un %d -> %d\n", i, untargeted(i + seed));
  return 0;
}
