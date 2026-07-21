// REQUIRES: cargo
// W3.4 G8 EndToEnd differential: an externally visible pointer-typed global
// `g`, defined and referenced in this TU (the companion is unrelated), bound
// project-wide to the single file-scope base `arr` with no reassignment. The
// W3.4 G8 relaxation reconstructs it as the CTS-P4 single-base cursor global;
// this differential proves the reconstructed lowering is runtime identical to
// the clang-linked native program.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g8-ptr-global-external-e2e-other.c -o %t.crate --crate-name g8_ptr --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-gate-g8-ptr-global-external-e2e-other.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/g8_ptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int arr[4] = {10, 20, 30, 40};
int *g = &arr[1];

int read_g(void) { return *g; }

// Defined in the companion TU.
int unrelated_g8(void);

int main(void) {
  int a = read_g();
  int b = unrelated_g8();
  printf("g=%d unrelated=%d\n", a, b);
  return 0;
}
