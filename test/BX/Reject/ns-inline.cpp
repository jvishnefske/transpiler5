// FR-260: an inline namespace makes its members visible as members of the
// enclosing namespace AND under their own path -- two spellings for one
// function, which no Rust module layout reproduces 1:1. Refused, located.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
#include <cstdio>

namespace lib {
// CHECK: ns-inline.cpp:[[@LINE+1]]:1: error: unsupported: inline namespace
inline namespace v2 {
int f() { return 2; }
}
}

int main() {
    std::printf("%d\n", lib::f());
    return 0;
}
