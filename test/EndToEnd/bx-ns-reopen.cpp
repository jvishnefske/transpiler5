// REQUIRES: cargo
// FR-260: emitrust-bx C++ -> Rust, REOPENED namespaces end to end (byte-diff
// vs clang++). C++ name lookup depends on declaration order: in the first
// `calc` block `twice()` finds ::twice (calc::twice is declared only in the
// reopened block), in the second it finds calc::twice. The canonical merges
// both blocks into one `mod calc` (normal form N13) -- sound only because
// every reference is RESOLVED on import (Rust: `crate::twice` vs `twice`).
// Also pins the C++17 `namespace calc::fmt` form, an out-of-line
// `int calc::offset(int)` definition, and a using-directive and namespace
// alias that are spelling only. Distinct values per call site; UB-free.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ns_reopen > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>

int twice(int x) {
    return x * 2;
}

namespace calc {

int offset(int x);

int early(int x) {
    return twice(x) + offset(x);
}

}  // namespace calc

int unrelated(int x) {
    return x - 1;
}

namespace calc::fmt {

int width(int x) {
    return x % 7 + 3;
}

}  // namespace calc::fmt

namespace calc {

int twice(int x) {
    return x * 20;
}

int late(int x) {
    return twice(x) + fmt::width(x);
}

}  // namespace calc

int calc::offset(int x) {
    return x + 1000;
}

using namespace calc;
namespace cf = calc::fmt;

int main() {
    std::printf("%d %d\n", calc::early(3), calc::late(4));
    std::printf("%d %d %d\n", ::twice(5), unrelated(6), cf::width(11));
    std::printf("%d %d\n", late(8), offset(9));
    return 0;
}
