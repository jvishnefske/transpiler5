// FR-264: zero `unsafe` in anything emitrust-bx emits for constants. A C++
// namespace-scope constant is a Rust `const` item (no `static`, no
// `static mut`, no lazy initializer, no helper in `mod bx`); a use is a value
// path, and a constant reaching a `const int&` parameter is `&kLimit` of a
// const (a fresh temporary per use, which is what C++ guarantees observably
// for a scalar constant). The rubric scores unsafe, and a `static mut`
// lowering would relocate the obligation, not discharge it.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=SHAPE --input-file=%t.crate/src/main.rs %s
// RUN: not grep -q "mod bx" %t.crate/src/main.rs
// RUN: not grep -q "static" %t.crate/src/main.rs
// COUNT: {{^}}0{{$}}
// SHAPE: #![allow(non_upper_case_globals)]
// SHAPE: const kLimit: i32 = 4;
// SHAPE: pub const kStep: u32 = 3u32;
// SHAPE: fn bump(x: &i32) -> i32 {
// SHAPE: for i in 0..kLimit {
// SHAPE: s = s + bump(&kLimit) + (crate::step::kStep as i32);
#include <cstdint>
#include <cstdio>

const int kLimit = 4;
namespace step {
constexpr std::uint32_t kStep = 3u;
}

int bump(const int& x) {
    return x + 1;
}

int main() {
    int s = 0;
    for (int i = 0; i < kLimit; ++i) {
        s = s + bump(kLimit) + static_cast<int>(step::kStep);
    }
    std::printf("%d\n", s);
    return 0;
}
