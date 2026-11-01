// FR-261 found (a pre-existing FR-257 hole): a member function named `into`
// or `try_into` was ACCEPTED and emitted unbuildable Rust. The 2021 prelude's
// `Into::into` / `TryInto::try_into` take `self` BY VALUE and are implemented
// for every type, so rustc's method probing finds them at the by-value step,
// before the inherent `&self` method (measured on rustc 1.96 and 1.98:
// `m.into(4)` is E0061, `m.try_into()` E0277). Both names are now reserved,
// located at the member function, like `len` / `clone`.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}2{{$}}
#include <cstdio>

struct Meter {
    int x;
    // CHECK: method-into.cpp:[[@LINE+1]]:9: error: unsupported: identifier 'into' is reserved by the canonical Rust spelling (a by-value prelude trait method, found before a `&self` one)
    int into(int k) const { return x + k; }
    // CHECK-NEXT: method-into.cpp:[[@LINE+1]]:9: error: unsupported: identifier 'try_into' is reserved by the canonical Rust spelling (a by-value prelude trait method, found before a `&self` one)
    int try_into() const { return x; }
};

int main() {
    Meter m = {3};
    std::printf("%d %d\n", m.into(4), m.try_into());
    return 0;
}
