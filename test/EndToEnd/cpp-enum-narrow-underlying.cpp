// REQUIRES: cargo
// FR-113 C2 regression, for ALREADY-ADMITTED unscoped enums: a fixed narrow
// underlying type (`enum PT : unsigned char`) truncates conversions in C++,
// but the emitted open enum stores a fixed u32/i32, so on unpatched HEAD
// `static_cast<PT>(argc + 300)` printed 301 where the native binary prints
// 45 -- a measured pre-existing byte-diff MISCOMPILE, not a rejection. The
// importer now routes integer-to-enum conversions through an intermediate
// cast at the underlying width (`Pt(v as u8 as u32)`), for both unsigned
// (u8) and signed (i8, which must sign-extend back) underlying types. All
// values derive from argc so constant folding cannot hide the truncation.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_enum_narrow_underlying > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

enum PT : unsigned char { A = 10, B = 45 };
enum SB : signed char { SA = 1, SBB = 2 };

int main(int argc, char **argv) {
  PT p = static_cast<PT>(argc + 300); // 301 truncates to 45 via u8
  printf("u8 %d\n", (int)p);
  PT q = static_cast<PT>(argc + 9); // in range: 10 == A
  printf("u8in %d %d\n", (int)q, (int)(q == A));
  SB s = static_cast<SB>(argc + 200); // 201 wraps to -55 via i8
  printf("i8 %d\n", (int)s);
  SB t = static_cast<SB>(1 - argc - 130); // -130 wraps to 126 via i8
  printf("i8b %d\n", (int)t);
  return 0;
}
