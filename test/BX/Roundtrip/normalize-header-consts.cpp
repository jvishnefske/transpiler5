// FR-264 + FR-254 R4 under --normalize --project-root: a main-file use of a
// constant DEFINED in a project header is no longer "global variable" -- the
// header constant is imported into ONE unity translation unit with the
// header functions, printed in the constants block. Pinned: the import is
// LAZY by reachability (`kUnused` and the array `kTable` are untouched) and
// TRANSITIVE through initializers (`kPrec` -> `kPrecBits`, dependencies
// first) and through imported header functions (`Scale` -> `kScaleBias`);
// each import is a `note: normalized: header constant` LOCATED IN THE
// HEADER; laws 1-2 hold on the output (--check-roundtrip re-imports the
// printed unity C++ without --normalize); the cpp -> cpp fixed point; and
// the OFF run keeps "global variable" for every header-constant use.
// RUN: %emitrust-bx --normalize --project-root=%S --check-roundtrip %s 2> %t.notes | FileCheck %s
// RUN: FileCheck --check-prefix=NOTE --input-file=%t.notes %s
// RUN: %emitrust-bx --normalize --project-root=%S --emit=cpp %s -o %t.cpp 2> /dev/null
// RUN: FileCheck --check-prefix=CPP --input-file=%t.cpp %s
// RUN: not grep -E "kUnused|kTable" %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp | diff %t.cpp -
// RUN: %emitrust-bx --normalize --project-root=%S --emit=rust %s -o %t.rs 2> /dev/null
// RUN: %emitrust-bx --emit=cpp %t.rs | diff %t.cpp -
// RUN: not %emitrust-bx --normalize %s 2>&1 | FileCheck --check-prefix=OFF %s
#include <cstdio>
#include <vector>

#include "Inputs/normalize-header-consts.h"

namespace brunsli {
static const double kSqrt2 = 1.414213562;
static const int kSqrt2FixedPoint = static_cast<int>(kSqrt2 * kPrec);
const std::size_t kMaxBypassHeaderSize = 5 * 6;

std::size_t GetBrunsliBypassSize(std::size_t jpg_size) {
    return jpg_size + kBrunsliSignatureSize + kMaxBypassHeaderSize;
}

int Mul(int q) {
    return q * kSqrt2FixedPoint + Scale(1);
}
}  // namespace brunsli

int main() {
    std::size_t bits = 0;
    while (bits < brunsli::kMaxHuffmanBits) {
        bits = bits + 4;
    }
    std::printf("%zu %zu %d\n", brunsli::GetBrunsliBypassSize(100), bits, brunsli::Mul(3));
    return 0;
}

// NOTE: {{.*}}Inputs{{/|\\}}normalize-header-consts.h:10:26: note: normalized: header constant 'kMaxHuffmanBits' from {{.*}}normalize-header-consts.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-consts.h:11:26: note: normalized: header constant 'kBrunsliSignatureSize' from {{.*}}normalize-header-consts.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-consts.h:12:18: note: normalized: header constant 'kPrecBits' from {{.*}}normalize-header-consts.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-consts.h:13:18: note: normalized: header constant 'kPrec' from {{.*}}normalize-header-consts.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-consts.h:16:18: note: normalized: header constant 'kScaleBias' from {{.*}}normalize-header-consts.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-consts.h:17:12: note: normalized: header-inline 'Scale' from {{.*}}normalize-header-consts.h
// NOTE-NOT: kUnused
// NOTE-NOT: kTable

// OFF-DAG: normalize-header-consts.cpp:28:63: error: unsupported: global variable
// OFF-DAG: normalize-header-consts.cpp:32:23: error: unsupported: global variable
// OFF-DAG: normalize-header-consts.cpp:36:35: error: unsupported: call to library function 'Scale'
// OFF-DAG: normalize-header-consts.cpp:42:19: error: unsupported: global variable

// CHECK:      #![allow(non_upper_case_globals)]
// CHECK-EMPTY:
// CHECK-NEXT: mod brunsli {
// CHECK-NEXT:     pub const kPrecBits: i32 = 13;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kPrec: i32 = 1 << kPrecBits;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kBrunsliSignatureSize: usize = 6usize;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kSqrt2: f64 = 1.414213562;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kSqrt2FixedPoint: i32 = (kSqrt2 * (kPrec as f64)) as i32;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kMaxBypassHeaderSize: usize = (5i32 * 6i32) as usize;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kScaleBias: i32 = 2;
// CHECK-EMPTY:
// CHECK-NEXT:     pub const kMaxHuffmanBits: usize = 15usize;
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn GetBrunsliBypassSize(mut jpg_size: usize) -> usize {
// CHECK-NEXT:         return jpg_size.wrapping_add(kBrunsliSignatureSize).wrapping_add(kMaxBypassHeaderSize);
// CHECK:          pub fn Scale(mut x: i32) -> i32 {
// CHECK-NEXT:         return x * kScaleBias;
// CHECK:      fn main() {
// CHECK-NEXT:     let mut bits: usize = 0usize;
// CHECK-NEXT:     while bits < crate::brunsli::kMaxHuffmanBits {
// CHECK-NOT:  kUnused
// CHECK-NOT:  kTable

// CPP:      namespace brunsli {
// CPP-NEXT: const int kPrecBits = 13;
// CPP-NEXT: const int kPrec = 1 << kPrecBits;
// CPP:      int Scale(int x) {
// CPP:      int main() {
