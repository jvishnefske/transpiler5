// FR-260: a namespace becomes a Rust module of the SAME name, so its name must
// be a legal module name that cannot capture a canonical spelling: not a
// Rust keyword (`mod`), not the prelude type every `Vec<T>` names (a root
// `mod Vec` would shadow it), not the helper module `bx`. Every bad name is
// reported (not just the first), each located at its namespace.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
#include <cstdio>

// CHECK: ns-names.cpp:[[@LINE+1]]:1: error: unsupported: namespace name: identifier 'mod' is a Rust keyword
namespace mod {
int f() { return 1; }
}

// CHECK-NEXT: ns-names.cpp:[[@LINE+1]]:1: error: unsupported: namespace name: identifier 'Vec' names a Rust prelude type
namespace Vec {
int f() { return 2; }
}

// CHECK-NEXT: ns-names.cpp:[[@LINE+1]]:1: error: unsupported: namespace name: identifier 'bx' is reserved by the canonical spelling
namespace bx {
int f() { return 3; }
}

int main() {
    std::printf("%d\n", 0);
    return 0;
}
