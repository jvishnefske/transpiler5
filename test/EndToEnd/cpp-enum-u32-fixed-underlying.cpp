// REQUIRES: cargo
// FR-166 phase 2, the C++ side: a FIXED `unsigned int` underlying type may
// now carry enumerators across the whole u32 range.
//
// The importer's enumerator range gate is shared by C and C++, so opening
// it from `isInt<32>` to `isUInt<32>` for an unsigned underlying type opens
// `enum class Big : unsigned int { VMAX = 4294967295u }` at the same
// moment it opens the C shapes. That is a real widening of the admitted
// language, so it needs its own byte-diff rather than an argument by
// analogy: C++ reaches the enum-to-integer conversion through a DIFFERENT
// seam from C. C17 6.4.4.3 types an enumeration constant `int` (which is
// what FR-169 phase C had to fix), while in C++ an enumerator reference
// HAS the enum type and every conversion out of it is an explicit
// `static_cast`/IntegralCast node instead.
//
// The load-bearing columns are the `%lu` ones: 4294967295 widened through a
// lost signedness prints 18446744073709551615, which compiles perfectly and
// is invisible to `cargo build`. Scoped and unscoped run side by side, and
// `argc` picks the value so no constant fold can hide it.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/cpp_enum_u32_fixed_underlying > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/cpp_enum_u32_fixed_underlying a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/cpp_enum_u32_fixed_underlying a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/cpp_enum_u32_fixed_underlying a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

extern "C" int printf(const char *, ...);

enum class Big : unsigned int { V0 = 1, VMID = 2147483648u, VMAX = 4294967295u };
enum Un : unsigned int { U0 = 1, UMID = 2147483648u, UMAX = 4294967295u };
/* A fixed SIGNED underlying type keeps the i32 bound and must not move. */
enum class Sg : int { S0 = 1, SMAX = 2147483647 };

int main(int argc, char **argv) {
  unsigned seeds[4] = {1u, 2147483648u, 4294967295u, 7u};
  unsigned s = seeds[(argc - 1) & 3];
  Big b = static_cast<Big>(s);
  Un u = static_cast<Un>(s);
  Sg g = static_cast<Sg>(static_cast<int>(s));

  printf("b: %u %lu %d\n", static_cast<unsigned>(b),
         static_cast<unsigned long>(b), static_cast<int>(b));
  printf("k: %u %lu\n", static_cast<unsigned>(Big::VMAX),
         static_cast<unsigned long>(Big::VMAX));
  printf("u: %u %lu %d\n", (unsigned)u, (unsigned long)u, (int)u);
  printf("uk: %u %lu\n", (unsigned)UMAX, (unsigned long)UMAX);
  printf("g: %d %ld %u\n", static_cast<int>(g), static_cast<long>(g),
         static_cast<unsigned>(Sg::SMAX));
  printf("c: %d %d %d\n", b == Big::VMAX, u > UMID, g == Sg::SMAX);
  return 0;
}
