// REQUIRES: cargo
// C99-43 C3: differential regression for the admitted argv read shapes. The
// emitted crate collects argv from `args_os` as raw NUL-terminated `i8`
// runs (`&[Vec<i8>]`); a `%s` of a whole `argv[i]` and the `argv[i][j]`
// byte shapes (`%c`, a `%d` of a byte, and a NUL-scan `while(argv[1][n])`)
// must produce stdout byte-identical to the clang-built native binary when
// both are run with the SAME argument vector. argv[0] is deliberately never
// printed (the crate and native binaries live at different paths, so echoing
// argv[0] would diverge spuriously in lit); the `%s` echo loops from i=1.
// The vectors cover: no args, several args, an arg with spaces, and a UTF-8
// arg — the last two are why the raw-bytes bypass exists (the Latin-1
// Display funnel would double-encode the non-ASCII bytes).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name argv_echo_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/argv_echo_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a bc def > %t.native.out
// RUN: %t.crate/target/release/argv_echo_e2e a bc def > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native "two words" x > %t.native.out
// RUN: %t.crate/target/release/argv_echo_e2e "two words" x > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native héllo > %t.native.out
// RUN: %t.crate/target/release/argv_echo_e2e héllo > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(int argc, char **argv) {
  printf("argc=%d\n", argc);
  // Whole-value %s echo of every argument past the program name.
  for (int i = 1; i < argc; i++)
    printf("[%s]\n", argv[i]);
  // Byte-read shapes over the first real argument, guarded so a bare run
  // (argc==1) skips them.
  if (argc > 1) {
    // %c of a byte (raw byte output, no Latin-1 widening).
    printf("c0=%c\n", argv[1][0]);
    // %d of a byte (flows through the ordinary integer path, not bypassed).
    printf("d0=%d\n", argv[1][0]);
    // NUL-scan: the byte comparison reads through the slice place.
    int n = 0;
    while (argv[1][n])
      n++;
    printf("len=%d\n", n);
  }
  return 0;
}
