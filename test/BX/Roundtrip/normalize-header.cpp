// FR-254 R4 under --normalize --project-root, C++ -> canonical subset C++ and
// Rust. A main-file function calling an `inline` / `static` free function
// DEFINED in a project header (a header under --project-root) is no longer a
// "call to library function": the header definition is imported into ONE
// unity translation unit, printed as an ordinary function before its first
// user. Pinned: the import is LAZY by reachability (`NeverCalled` is not
// imported; the FR-262 spike measured an eager import regressing a TU) and
// TRANSITIVE through imported functions (`SumSquares` -> `Square`, callees
// first); a header function inside a namespace lands in that namespace's
// `mod` (FR-260's law); each import is a `note: normalized: header-inline`
// LOCATED IN THE HEADER; laws 1-2 hold on the OUTPUT (--check-roundtrip
// re-imports the printed unity C++ WITHOUT --normalize, which can only work
// if the unity TU is self-contained); the cpp -> cpp fixed point; and the OFF
// runs: without --normalize, and with --normalize but no --project-root,
// every header call keeps its "call to library function" refusal.
// RUN: %emitrust-bx --normalize --project-root=%S --check-roundtrip %s 2> %t.notes | FileCheck %s
// RUN: FileCheck --check-prefix=NOTE --input-file=%t.notes %s
// RUN: %emitrust-bx --normalize --project-root=%S --emit=cpp %s -o %t.cpp 2> /dev/null
// RUN: FileCheck --check-prefix=CPP --input-file=%t.cpp %s
// RUN: %emitrust-bx --emit=cpp %t.cpp | diff %t.cpp -
// RUN: %emitrust-bx --normalize --project-root=%S --emit=rust %s -o %t.rs 2> /dev/null
// RUN: %emitrust-bx --emit=cpp %t.rs | diff %t.cpp -
// RUN: not %emitrust-bx %s 2>&1 | FileCheck --check-prefix=OFF %s
// RUN: not %emitrust-bx --normalize %s 2>&1 | FileCheck --check-prefix=OFF %s
// RUN: not %emitrust-bx --project-root=%S %s 2>&1 | FileCheck --check-prefix=FLAG %s
#include <cstdio>
#include <vector>

#include "Inputs/normalize-header.h"

namespace codec {
int Encode(int v) {
    return Shift(v) + 1;
}
}  // namespace codec

int main() {
    std::vector<std::uint8_t> b(4, 0);
    std::size_t o = Put16(b.data(), 0, 0x1234);
    std::printf("%zu %d %d\n", o, b[0], b[1]);
    std::printf("%d %d\n", SumSquares(3, 4), codec::Encode(5));
    return 0;
}

// NOTE: {{.*}}Inputs{{/|\\}}normalize-header.h:7:20: note: normalized: header-inline 'Put16' from {{.*}}normalize-header.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header.h:13:12: note: normalized: header-inline 'Square' from {{.*}}normalize-header.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header.h:17:12: note: normalized: header-inline 'SumSquares' from {{.*}}normalize-header.h
// NOTE-NEXT: {{.*}}Inputs{{/|\\}}normalize-header.h:26:12: note: normalized: header-inline 'Shift' from {{.*}}normalize-header.h
// NOTE-NOT: NeverCalled

// OFF-DAG: normalize-header.cpp:33:12: error: unsupported: call to library function 'Shift'
// OFF-DAG: normalize-header.cpp:39:21: error: unsupported: call to library function 'Put16'
// OFF-DAG: normalize-header.cpp:41:28: error: unsupported: call to library function 'SumSquares'

// FLAG: error: --project-root needs --normalize

// CHECK: mod codec {
// CHECK-NEXT:     pub fn Shift(mut v: i32) -> i32 {
// CHECK-NEXT:         return v << 2;
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn Encode(mut v: i32) -> i32 {
// CHECK-NEXT:         return Shift(v) + 1;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn Put16(mut dst: &mut [u8], mut off: usize, mut x: i32) -> usize {
// CHECK:      fn Square(mut a: i32) -> i32 {
// CHECK:      fn SumSquares(mut a: i32, mut b: i32) -> i32 {
// CHECK-NEXT:     return Square(a) + Square(b);
// CHECK:      fn main() {
// CHECK-NOT:  NeverCalled

// CPP:      namespace codec {
// CPP:      int Shift(int v) {
// CPP:      int Encode(int v) {
// CPP:      std::size_t Put16(unsigned char* dst, std::size_t off, int x) {
// CPP:      int Square(int a) {
// CPP:      int SumSquares(int a, int b) {
// CPP:      int main() {
// CPP-NOT:  NeverCalled
// CPP-NOT:  #include "
