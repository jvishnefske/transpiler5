// REQUIRES: cargo
// FR-166, differential end-to-end test for an enum whose UNDERLYING TYPE IS
// 64 BITS WIDE. This is systemd's `_SD_ENUM_FORCE_S64(X)` shape verbatim:
// the macro appends two enumerators at INT64_MIN/INT64_MAX purely to force
// clang to pick a 64-bit underlying type, and every flags enum in
// `sd-json.h` / `sd-varlink.h` carries one. Before FR-166 the whole enum was
// rejected ("enumerator value does not fit in i32") and the translation unit
// produced nothing.
//
// The emitted open enum's tuple-struct storage must now be `i64`, and EVERY
// site that used to force the discriminant to `i32` must follow the width:
// assignment, `==`/`!=`, `<`/`>`/`>=`, the truth test, `switch`, the `(int)`
// TRUNCATING cast, the `(long long)` widening cast, `int`->enum conversion,
// by-value enum parameters and `sizeof`. A wrong width compiles perfectly --
// `5000000000 as i32` is 705032704 and `INT64_MAX as i32` is -1, both
// legal Rust -- so `cargo build` CANNOT see the miscompile. The oracle is
// the stdout byte-diff against the clang-built native, and every runtime
// value derives from `argc` so no constant fold can hide a lost bit. The
// `as-int` line is the load-bearing one: it prints the truncations
// (a=705032704 m=-1 n=0) that a naive i32 storage would have produced for
// the WHOLE enum.
//
// Values above INT64_MAX stay rejected (DenseI64ArrayAttr cannot hold them)
// and a 32-bit UNSIGNED enumerator above INT32_MAX also stays rejected --
// both pinned in test/Import/C/enums-invalid.c.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_wide_underlying > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_wide_underlying a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_wide_underlying a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out

int printf(const char *, ...);

/* systemd's _SD_ENUM_FORCE_S64 shape verbatim: two extra enumerators at
   INT64_MIN/INT64_MAX force the enum to a signed 64-bit underlying type. */
enum JT {
  JT_NULL = 0,
  JT_BOOL = 1,
  JT_BIG = 5000000000LL,
  _JT_INT64_MIN = (-9223372036854775807LL - 1),
  _JT_INT64_MAX = 9223372036854775807LL,
};

static int classify(enum JT t) {
  switch (t) {
  case JT_NULL:
    return 10;
  case JT_BOOL:
    return 11;
  case JT_BIG:
    return 12;
  default:
    return 99;
  }
}

static long long raw(enum JT t) { return (long long)t; }

static int isMax(enum JT t) { return t == _JT_INT64_MAX ? 13 : 0; }

int main(int argc, char **argv) {
  enum JT a = JT_BIG;                         /* value needs 64 bits */
  enum JT m = _JT_INT64_MAX;
  enum JT n = _JT_INT64_MIN;
  enum JT z = (enum JT)(argc - 1);            /* int -> enum */
  enum JT w = (enum JT)(5000000000LL * argc); /* wide int -> enum */

  printf("a=%lld m=%lld n=%lld\n", (long long)a, (long long)m, (long long)n);
  /* The truncating cast: a naive i32 storage would print these for the
     whole enum, so this line is the miscompile detector. */
  printf("as-int a=%d m=%d n=%d\n", (int)a, (int)m, (int)n);
  printf("eq %d %d %d\n", a == JT_BIG, m == _JT_INT64_MAX, z == JT_NULL);
  printf("rel %d %d %d\n", a > JT_BOOL, n < JT_NULL, m >= a);
  printf("truth %d %d %d\n", !!a, !!z, !!n);
  printf("cls %d %d %d %d\n", classify(a), classify(m), classify(n),
         classify(z));
  printf("ismax %d %d\n", isMax(m), isMax(a));
  printf("w=%lld cls=%d\n", raw(w), classify(w));
  printf("sizeof=%d\n", (int)sizeof(enum JT));
  return 0;
}
