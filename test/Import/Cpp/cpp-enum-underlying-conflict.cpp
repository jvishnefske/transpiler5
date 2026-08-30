// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/width1.cpp %t/width2.cpp 2>&1 | FileCheck %s --check-prefix=WIDTH
// RUN: not emitrust-import-c %t/sign1.cpp %t/sign2.cpp 2>&1 | FileCheck %s --check-prefix=SIGN
// RUN: emitrust-import-c %t/same1.cpp %t/same2.cpp | FileCheck %s --check-prefix=SAME

// FR-166 closes a LATENT hole in the cross-TU enum dedup key. The key is
// built from `name=value;` pairs only, on the since-falsified premise that
// "the underlying signedness is derived from the values". A C++ FIXED
// UNDERLYING TYPE breaks that premise outright: `enum G : int` and
// `enum G : unsigned int` carry identical enumerator values yet different
// storage, and before FR-166 the two translation units DEDUPED SILENTLY into
// a single `emitrust.enum_def @G` -- the second TU's storage was simply
// thrown away with no diagnostic at all.
//
// The hole was latent at HEAD because signed and unsigned 32-bit
// representations diverge only above 2^31 and such enumerators were rejected
// wholesale. FR-166 admits 64-bit underlying types, which makes both the
// WIDTH and the SIGNEDNESS mismatch observable, so both now join the key and
// both reject as a LOCATED "conflicting definition" -- the same wording and
// the same shape gate a differing VALUE LIST already uses
// (test/Import/C/enums-invalid.c's SHAPE leg). Rejection is the feature: the
// importer must never silently pick one TU's storage over the other's.
//
// The SAME leg is the additivity control: identical underlying type in both
// TUs still dedups to exactly one definition.

// WIDTH: width2.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of enum 'E' with a different shape in another translation unit
// SIGN: sign2.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of enum 'G' with a different shape in another translation unit

//--- width1.cpp
enum E : int { A = 1, B = 2 };
int f1(void) { E e = A; return (int)e; }

//--- width2.cpp
enum E : long long { A = 1, B = 2 };
int f2(void) { E e = B; return (int)e; }

//--- sign1.cpp
enum G : int { GA = 1, GB = 2 };
int g1(void) { G e = GA; return (int)e; }

//--- sign2.cpp
enum G : unsigned int { GA = 1, GB = 2 };
int g2(void) { G e = GB; return (int)e; }

//--- same1.cpp
enum H : unsigned int { HA = 1, HB = 2 };
int h1(void) { H e = HA; return (int)e; }

//--- same2.cpp
enum H : unsigned int { HA = 1, HB = 2 };
int h2(void) { H e = HB; return (int)e; }

// SAME: emitrust.enum_def @H ["HA", "HB"] [1, 2] {unsigned_underlying}
// SAME-NOT: emitrust.enum_def @H
