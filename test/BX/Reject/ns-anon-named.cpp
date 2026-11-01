// FR-260: a NAMED namespace nested in an anonymous one would be a public Rust
// module inside a private scope, but an anonymous namespace has no module of
// its own to hold it (its members are private items of the enclosing
// module). Refused, located at the named namespace; the anonymous function
// beside it is fine.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
#include <cstdio>

namespace {
int one() { return 1; }
// CHECK: ns-anon-named.cpp:[[@LINE+1]]:1: error: unsupported: named namespace inside an anonymous namespace
namespace detail {
int two() { return 2; }
}
}

int main() {
    std::printf("%d\n", one());
    return 0;
}
// CHECK-NOT: error
