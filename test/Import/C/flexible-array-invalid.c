// C99-17, AMENDED by CTS-BR (00216): a flexible array member
// (C99 6.7.2.1p16, `T tail[];`) no longer rejects at the DECLARATION —
// the record imports with sizeof excluding the FAM (byte-region records
// additionally fold static FAM-tail initializers into an extended
// image; typed records drop the field), see byte-region-aggregates.c
// for the positive pins. FR-94/95 AMEND again: a gap-free u8 or
// Vec-mappable tail (u8 by FR-94; i16/i32/i64/u16/u32/u64/f32/f64 by
// FR-95) is ADMITTED as an owned Vec member — see
// flexible-array-owned-tail.c / flexible-array-owned-typed-tail.c —
// so these arms pin the residue OUTSIDE the admission: an element with
// no Vec mapping (long double) keeps every RUNTIME tail access on the
// dedicated located wording, never degrading to the generic array
// fallback (`unsupported: non-constant array size`). The same policy
// covers GNU zero-length array members (`T r[0];`): tolerated as a
// zero-size, field-less contribution when unaccessed; accessing one is
// a located rejection.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/fam-read.c 2>&1 | FileCheck %s --check-prefix=FAMREAD
// RUN: not emitrust-import-c %t/fam-write.c 2>&1 | FileCheck %s --check-prefix=FAMWRITE
// RUN: not emitrust-import-c %t/zero-read.c 2>&1 | FileCheck %s --check-prefix=ZEROREAD

//--- fam-read.c
// Reading the tail through a pointer to the record: rejected at the
// access site, not at the declaration (long double has no Vec mapping,
// so the FR-94/95 admission never claims the record).
struct S {
  int n;
  long double tail[];
};

long double g(struct S *p) {
  return p->tail[0];
}
// FAMREAD: fam-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member access

//--- fam-write.c
// Writing the tail is the same rejection: the non-admitted tail has no
// storage behind sizeof, so neither direction of runtime access is
// modeled.
struct S {
  int n;
  long double tail[];
};

void h(struct S *p) {
  p->tail[1] = 7;
}
// FAMWRITE: fam-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member access

//--- zero-read.c
// A GNU zero-length array member is tolerated in the record shape but
// has no elements: any access rejects at the access site.
struct Z {
  int n;
  int r[0];
};
struct Z gz;

int main(void) {
  return gz.r[0];
}
// ZEROREAD: zero-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: zero-length array member access
