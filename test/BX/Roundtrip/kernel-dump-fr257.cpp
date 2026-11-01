// FR-257: --emit=kernel shows struct copy vs move as the Kernel holds them
// (Rust's value semantics, FR-251's rule): `Item copy = v[0];` is
// `(clone (index v ..))`, `std::move(extra)` a bare variable, `push_back(copy)`
// of an lvalue a clone, a member read `(field moved name)` in a copying
// position a clone (moving out of a data member is not in the subset), a
// member write `store (field (index v 1) n) add= ..`, member calls
// `(method recv name args..)` with the receiver an element, and an aggregate
// `Item{s, ..}` a `(struct Item [..])` whose initializer COPIES the lvalue `s`
// (`(clone s)`; `Item{std::move(s), ..}` would be a bare `s`).
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:       struct Item
// CHECK-NEXT:    field name: String
// CHECK-NEXT:    field n: i32
// CHECK-NEXT:    func ref Item::twice() -> i32
// CHECK-NEXT:      block
// CHECK-NEXT:        return (mul (field self n) 2:i32)
// CHECK-NEXT:    func mut Item::retitle(s: &String) -> ()
// CHECK-NEXT:      block
// CHECK-NEXT:        store (field self name) = (clone s)
// CHECK-NEXT:  func pick(v: &mut Vec<Item>, mut extra: Item) -> Item
// CHECK-NEXT:    block
// CHECK-NEXT:      let mut copy: Item = (clone (index v 0:usize))
// CHECK-NEXT:      let mut moved: Item = extra
// CHECK-NEXT:      push v (clone copy)
// CHECK-NEXT:      expr (method (index v 1:usize) retitle (field copy name))
// CHECK-NEXT:      store (field (index v 1:usize) n) add= (method moved twice)
// CHECK-NEXT:      let mut s: String = (clone (field moved name))
// CHECK-NEXT:      return (struct Item [(clone s), (field (index v 1:usize) n)])
#include <string>
#include <utility>
#include <vector>

struct Item {
    std::string name;
    int n;
    int twice() const {
        return n * 2;
    }
    void retitle(const std::string& s) {
        name = s;
    }
};

Item pick(std::vector<Item>& v, Item extra) {
    Item copy = v[0];
    Item moved = std::move(extra);
    v.push_back(copy);
    v[1].retitle(copy.name);
    v[1].n += moved.twice();
    std::string s = moved.name;
    return Item{s, v[1].n};
}
