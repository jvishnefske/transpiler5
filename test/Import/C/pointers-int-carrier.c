// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P3 relaxation: int-carrier pointer regions. A pointer whose ONLY
// sources are integer-to-pointer casts and null pointer constants never
// addresses a modeled object: it is just an integer riding in pointer
// clothing (the 00214 `extend_brk` brk-cursor shape). Such a region lowers
// as a plain i64 value — no base, no cursor, no flag cell. A null test is
// an integer compare against 0, the value is returnable from functions,
// and a call whose callee returns a carrier propagates carrier-ness to the
// caller's pointer. Dereference or arithmetic on a carrier, and mixing a
// carrier with a real address base, stay located rejections (see
// pointers-int-carrier-invalid.c).

typedef unsigned long size_t;

void *make_ptr(size_t raw) {
  return (void *)raw;
}

// The carrier return type erases to i64.
// CHECK-LABEL: func.func @make_ptr
// CHECK-SAME: (%{{.*}}: ui64) -> i64
// CHECK: return %{{.*}} : i64

void *pick(size_t v, int use) {
  // Null constants and int-to-pointer casts may mix freely inside one
  // carrier region: null is just the i64 zero.
  void *ret = 0;
  if (use)
    ret = (void *)v;
  return ret;
}

// CHECK-LABEL: func.func @pick
// CHECK-SAME: -> i64
// CHECK: arith.constant 0 : i64
// CHECK: return %{{.*}} : i64

int consume(void *p) {
  // A carrier parameter (every call site passes a carrier) is an i64
  // parameter; its truth test is `!= 0` on the integer.
  if (p)
    return 1;
  return 0;
}

// CHECK-LABEL: func.func @consume
// CHECK-SAME: (%{{.*}}: i64) -> i32
// CHECK: arith.cmpi ne, %{{.*}}, %{{.*}} : i64
// CHECK: cf.cond_br

int main(void) {
  void *r;
  r = make_ptr(1024);
  if (!r)
    return 1;
  return consume(r);
}

// The call result carries the carrier region into `r`; the null test and
// the pass-along are plain i64 traffic.
// CHECK-LABEL: func.func @c_main
// CHECK: call @make_ptr
// CHECK: arith.cmpi
// CHECK: call @consume(%{{.*}}) : (i64) -> i32
