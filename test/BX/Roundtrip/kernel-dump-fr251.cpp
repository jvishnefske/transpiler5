// FR-251: --emit=kernel shows copy vs move as the Kernel holds them (Rust's
// value semantics): `(clone x)` is a C++ copy -- the copy constructor for
// `w = v` from a `const&`, `push_back(const T&)` for an lvalue -- and a bare
// variable in a consuming position is a MOVE (`push_back(std::move(s))`).
// Also pins the import desugarings `w.back()` -> `(index w (sub (len w)
// 1:usize))` and `w.front()` -> `(index w 0:usize)`, the reference
// parameter type `&Vec<String>`, the mutating range-for (`foreach x mut`),
// and that a returned element is a clone (moving out of an element is not
// in the subset).
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      func pick(v: &Vec<String>, mut s: String) -> String
// CHECK-NEXT:   block
// CHECK-NEXT:     let mut w: Vec<String> = (clone v)
// CHECK-NEXT:     push w (clone s)
// CHECK-NEXT:     push w s
// CHECK-NEXT:     let mut last: String = (clone (index w (sub (len w) 1:usize)))
// CHECK-NEXT:     foreach x mut in w
// CHECK-NEXT:       block
// CHECK-NEXT:         push_str x last
// CHECK-NEXT:     return (clone (index w 0:usize))
// CHECK-NEXT: func main() -> ()
// CHECK-NEXT:   block
#include <string>
#include <utility>
#include <vector>

std::string pick(const std::vector<std::string>& v, std::string s) {
    std::vector<std::string> w = v;
    w.push_back(s);
    w.push_back(std::move(s));
    std::string last = w.back();
    for (std::string& x : w) {
        x += last;
    }
    return w.front();
}

int main() {
    return 0;
}
