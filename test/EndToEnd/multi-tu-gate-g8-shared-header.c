// REQUIRES: cargo
// W3.2 COMMIT B: the EndToEnd differential for W3.1 predicted failure #1
// (shared `T *g;` header pointer global) once `deferExternGlobal`'s
// unconditional pointer-extern check is relaxed for the sound, sole,
// never-reassigned cross-TU binding shape (see
// test/Import/C/multi-tu-gate-g8-ptr-global-shared-header.c for the
// located-acceptance oracle). `g` is declared `extern` here and defined,
// bound to the companion TU's own `arr`, in Inputs/ -- the natural
// "shared header pointer global" shape. The crate build and the
// clang-linked native binary must produce byte-identical stdout.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g8-shared-header-def.c -o %t.crate --crate-name g8_shared --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-gate-g8-shared-header-def.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/g8_shared > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

extern int *g;

int read_g(void) { return *g; }

int main(void) {
  printf("g=%d\n", read_g());
  return 0;
}
