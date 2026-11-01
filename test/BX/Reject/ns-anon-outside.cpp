// FR-260: an anonymous-namespace member is INTERNAL: Rust spells it a private
// `fn` inside the module, which is reachable only from that module and its
// descendants (rustc E0603 otherwise). C++ accepts `geo::helper()` from the
// root (qualified lookup sees through the unnamed namespace), so this
// reference has no 1:1 Rust spelling and must be a LOCATED refusal, never a
// crate that fails to build. A reference from a nested namespace is fine.
// RUN: not %emitrust-bx %s -o %t.rs 2>&1 | FileCheck %s
// RUN: not test -e %t.rs
#include <cstdio>

namespace geo {
namespace {
int helper(int x) { return x + 1; }
}
namespace inner {
int ok(int x) { return helper(x) * 2; }
}
}

int main() {
    // CHECK: ns-anon-outside.cpp:[[@LINE+1]]:28: error: unsupported: 'geo::helper' is internal to namespace 'geo' (a C++ anonymous-namespace member, a private Rust `fn`) and is referenced from outside it
    std::printf("%d %d\n", geo::helper(1), geo::inner::ok(2));
    return 0;
}
// CHECK-NOT: error
