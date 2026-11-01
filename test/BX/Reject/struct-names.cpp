// FR-257: struct NAMES with no 1:1 spelling, each a located residual. A
// struct named like a Rust prelude item would shadow it for the whole module
// (`Vec` breaks every `Vec<T>`, `Clone` the derive); a data member or a local
// named like a struct hides the type in C++ (`S S = ..;` makes a later `S{..}`
// an error); a member function named like a Vec/String method (`len`) would be
// read back by rs_parse as that method; a struct and a function of one name in
// one scope is legal C++ but `Leaf{..}` would then be ambiguous; and a type
// written relative to a using-directive (`const Item&` meaning
// `outer::inner::Item`) is not resolved by guessing.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}6{{$}}
#include <cstdio>

// CHECK: struct-names.cpp:[[@LINE+1]]:1: error: unsupported: struct name: identifier 'Vec' names a Rust prelude item
struct Vec {
    int x;
};

struct Node {
    // CHECK-NEXT: struct-names.cpp:[[@LINE+1]]:9: error: unsupported: data member 'Leaf' has the name of a struct
    int Leaf;
    int size;
    // CHECK-NEXT: struct-names.cpp:[[@LINE+1]]:9: error: unsupported: identifier 'len' is reserved by the canonical Rust spelling (a method of Vec/String or of the derived Clone)
    int len() const { return size; }
};

// CHECK-NEXT: struct-names.cpp:[[@LINE+1]]:1: error: unsupported: struct 'Leaf' has the name of a function declared in the same scope
struct Leaf {
    int v;
};

int Leaf(int k) {
    return k;
}

namespace outer {
struct Item {
    int a;
};
namespace inner {
struct Item {
    int b;
};
}  // namespace inner
}  // namespace outer

namespace other {
using namespace outer::inner;
// CHECK-NEXT: struct-names.cpp:[[@LINE+1]]:22: error: unsupported: parameter of type 'const Item &'
int peek(const Item& i) {
    return 1;
}
}  // namespace other

int main() {
    // CHECK-NEXT: struct-names.cpp:[[@LINE+1]]:5: error: unsupported: variable 'Node' has the name of a struct
    int Node = 3;
    std::printf("%d\n", Node);
    return 0;
}
