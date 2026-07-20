// REQUIRES: cargo
// W3.3 G3 EndToEnd differential: owner-struct promotion (FR-30) of
// externally visible functions in a MULTI-TU project. fill/sum/total are
// externally visible and referenced only by this TU (the companion only
// defines the unrelated `unrelated_g3`), so the W3.3 G3 relaxation promotes
// them to Owner_main_arr methods across the whole-program import. G3 is a
// suboptimality gate — the pre-FR-30 slice fallback was already correct —
// so this differential proves the promoted owner lowering is RUNTIME
// identical to the clang-linked native program (byte-identical stdout).
// Companion TU lives in Inputs/ (not discovered as a test). --release
// matches the other EndToEnd tests.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g3-owner-fallback-e2e-other.c -o %t.crate --crate-name g3_owner --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-gate-g3-owner-fallback-e2e-other.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/g3_owner > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

// Defined in the companion translation unit.
int unrelated_g3(void);

// A subscripting callee, a walking callee, and a sibling-calling callee —
// all unified into one Owner_main_arr class rooted at `main`'s `arr`.
void fill(int *p, int n) {
  for (int i = 0; i < n; i++)
    p[i] = i;
}
int sum(int *p, int n) {
  int s = 0;
  while (n > 0) {
    s = s + *p;
    p++;
    n--;
  }
  return s;
}
int total(int *p, int n) {
  return sum(p, n);
}

int main(void) {
  int arr[8];
  fill(arr, 8);
  int t = sum(&arr[2], 4);
  t = t + total(arr, arr[0]);
  printf("t=%d unrelated=%d\n", t, unrelated_g3());
  return 0;
}
