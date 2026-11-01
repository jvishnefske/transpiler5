// FR-260: C++ namespaces <=> Rust modules under --check-roundtrip (laws 1
// and 2 on both sides through real clang). Pins the canonical: one `mod`
// per namespace (the reopened `geo` and the C++17 `geo::inner` merge into
// the first block -- normal form N13), `pub mod` when nested, `pub fn` in a
// module and plain `fn` for an anonymous-namespace member; a call is bare
// only to the SAME namespace (`scale` inside geo::inner is the INNER one:
// inner hides outer on both sides) and absolute otherwise (`::geo::scale`
// <=> `crate::geo::scale`, `::scale` <=> `crate::scale`), so no reference
// depends on declaration order. The out-of-line `int geo::inner::depth()`,
// the using-directive and the namespace alias are spelling only (clang
// resolves every reference); sibling namespaces both define `scale`. C++
// prototypes are re-derived inside their namespace.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
#include <cstdio>

int scale(int x) {
    return x * 2;
}

namespace geo {

int scale(int x);

namespace {

int clampi(int x, int hi) {
    if (x > hi) {
        return hi;
    }
    return x;
}

}  // namespace

int area(int w, int h) {
    return clampi(scale(w) * h, 1000);
}

}  // namespace geo

namespace geo::inner {

int scale(int x) {
    return x * 4;
}

int depth();

int twice(int x) {
    return scale(x) + geo::scale(x) + ::scale(x);
}

}  // namespace geo::inner

namespace color {

int scale(int x) {
    return x + 7;
}

}  // namespace color

namespace geo {

int scale(int x) {
    return x * 3;
}

}  // namespace geo

int geo::inner::depth() {
    return 2;
}

using namespace color;
namespace gi = geo::inner;

int main() {
    std::printf("%d %d %d\n", geo::area(3, 5), gi::twice(6), color::scale(8));
    std::printf("%d %d\n", ::scale(9), geo::inner::depth());
    return 0;
}

// CHECK: fn scale(mut x: i32) -> i32 {
// CHECK-NEXT:     return x * 2;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod geo {
// CHECK-NEXT:     fn clampi(mut x: i32, mut hi: i32) -> i32 {
// CHECK-NEXT:         if x > hi {
// CHECK-NEXT:             return hi;
// CHECK-NEXT:         }
// CHECK-NEXT:         return x;
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn area(mut w: i32, mut h: i32) -> i32 {
// CHECK-NEXT:         return clampi(scale(w) * h, 1000);
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub mod inner {
// CHECK-NEXT:         pub fn scale(mut x: i32) -> i32 {
// CHECK-NEXT:             return x * 4;
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn twice(mut x: i32) -> i32 {
// CHECK-NEXT:             return scale(x) + crate::geo::scale(x) + crate::scale(x);
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn depth() -> i32 {
// CHECK-NEXT:             return 2;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn scale(mut x: i32) -> i32 {
// CHECK-NEXT:         return x * 3;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod color {
// CHECK-NEXT:     pub fn scale(mut x: i32) -> i32 {
// CHECK-NEXT:         return x + 7;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     print!("{} {} {}\n", crate::geo::area(3, 5), crate::geo::inner::twice(6), crate::color::scale(8));
// CHECK-NEXT:     print!("{} {}\n", scale(9), crate::geo::inner::depth());
// CHECK-NEXT: }

// CPP: #include <cstdio>
// CPP-EMPTY:
// CPP-NEXT: namespace geo {
// CPP-NEXT: int scale(int x);
// CPP-NEXT: }  // namespace geo
// CPP-EMPTY:
// CPP-NEXT: int scale(int x) {
// CPP-NEXT:     return x * 2;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: namespace geo {
// CPP-EMPTY:
// CPP-NEXT: namespace {
// CPP-EMPTY:
// CPP-NEXT: int clampi(int x, int hi) {
// CPP-NEXT:     if (x > hi) {
// CPP-NEXT:         return hi;
// CPP-NEXT:     }
// CPP-NEXT:     return x;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace
// CPP-EMPTY:
// CPP-NEXT: int area(int w, int h) {
// CPP-NEXT:     return clampi(scale(w) * h, 1000);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: namespace inner {
// CPP-EMPTY:
// CPP-NEXT: int scale(int x) {
// CPP-NEXT:     return x * 4;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int twice(int x) {
// CPP-NEXT:     return scale(x) + ::geo::scale(x) + ::scale(x);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int depth() {
// CPP-NEXT:     return 2;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace inner
// CPP-EMPTY:
// CPP-NEXT: int scale(int x) {
// CPP-NEXT:     return x * 3;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace geo
// CPP-EMPTY:
// CPP-NEXT: namespace color {
// CPP-EMPTY:
// CPP-NEXT: int scale(int x) {
// CPP-NEXT:     return x + 7;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace color
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::printf("%d %d %d\n", ::geo::area(3, 5), ::geo::inner::twice(6), ::color::scale(8));
// CPP-NEXT:     std::printf("%d %d\n", scale(9), ::geo::inner::depth());
// CPP-NEXT:     return 0;
// CPP-NEXT: }
