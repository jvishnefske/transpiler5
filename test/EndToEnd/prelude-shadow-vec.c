// REQUIRES: cargo
// FR-150, the `Vec` leg, byte-diffed. `Option` is not the only prelude name at
// risk: the emitter writes `Vec<..>` too, so a C type named `vec` (or `VEC`,
// or `Vec`) shadows it exactly the same way and produces the same exit-0
// unbuildable crate.
//
// `toUpperCamelCase` is what decides, not the C spelling: it DROPS
// underscores, so C's `my_vec` is safe (`MyVec`) while `vec` becomes `Vec` and
// collides. The type below is spelled `vec` in C on purpose.
//
// The `Vec` spellings the emitter writes and this program reaches:
//   * the C99-43 C3 argv table parameter, `fn c_main(argc: i32, argv: &[Vec<i8>])`
//     -- a LITERAL in the emitter, not an opaque spelling,
//   * the crate's `fn main()` wrapper, which collects `let __emitrust_argv:
//     Vec<Vec<i8>> = std::env::args_os()...` -- and that text lives in the
//     emitrust-cc DRIVER, not in the emitter, so it is a second, separate site
//     the fix has to cover,
//   * the emitter's verbatim `__emitrust_cstr_out` runtime helper, whose
//     `let bytes: Vec<u8> = ..` is emitter-owned boilerplate.
//
// The oracle is the byte-diff, in four argument vectors including one with a
// space and one non-ASCII (the raw-bytes argv path exists for those). argv[0]
// is never echoed: the crate and the native binary live at different paths.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/prelude_shadow_vec > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native a bc def > %t.native3.out
// RUN: %t.crate/target/release/prelude_shadow_vec a bc def > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out
// RUN: %t.native "two words" x > %t.nativew.out
// RUN: %t.crate/target/release/prelude_shadow_vec "two words" x > %t.rustw.out
// RUN: diff %t.nativew.out %t.rustw.out
// RUN: %t.native héllo > %t.nativeu.out
// RUN: %t.crate/target/release/prelude_shadow_vec héllo > %t.rustu.out
// RUN: diff %t.nativeu.out %t.rustu.out

int printf(const char *, ...);

// The colliding type. `vec` -> `Vec` under toUpperCamelCase.
typedef struct vec {
  int x;
  int y;
} vec;

// A type whose C name contains an underscore and therefore does NOT collide;
// it shares the crate so a fix that keyed off the C spelling would show up.
struct my_vec {
  int z;
};

int main(int argc, char **argv) {
  vec v;
  v.x = argc * 2;
  v.y = argc + 11;

  struct my_vec m;
  m.z = v.x - v.y;

  printf("argc=%d x=%d y=%d z=%d\n", argc, v.x, v.y, m.z);

  // Whole-value %s echo of every argument past the program name: this is the
  // read that forces the argv table (`&[Vec<i8>]`) into the signature.
  for (int i = 1; i < argc; i++)
    printf("[%s]\n", argv[i]);

  // A byte read, guarded so a bare run skips it.
  if (argc > 1) {
    int n = 0;
    while (argv[1][n])
      n++;
    printf("len1=%d c0=%d\n", n, argv[1][0]);
  }
  return 0;
}
