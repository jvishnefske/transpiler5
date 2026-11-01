// FR-252: --emit=kernel shows closures as the Kernel holds them. The capture
// LIST is derived, not stored: `[n, k]` and `[k, n]` over the same body are
// one term `closure move a(..) [k, n]` (first-use order, N23), a written but
// unused capture disappears and a closure that captures nothing is never
// `move` (`c`, written `[n] .. mutable`, is `closure c(..) []`: N24), a
// written `-> void` is the unit result, a call is `call_closure`, and a
// template's callable parameter `const G& g` is `g: &impl Fn(i32) -> i32`
// typed by its one instantiation (the template parameter's name is a
// spelling).
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      func twice(g: &impl Fn(i32) -> i32, mut x: i32) -> i32
// CHECK-NEXT:   block
// CHECK-NEXT:     return (call_closure g (call_closure g x))
// CHECK-NEXT: func main() -> ()
// CHECK-NEXT:   block
// CHECK-NEXT:     let mut k: i32 = 2:i32
// CHECK-NEXT:     let mut n: i32 = 5:i32
// CHECK-NEXT:     closure move a(mut x: i32) -> i32 [k, n]
// CHECK-NEXT:       block
// CHECK-NEXT:         return (add (mul x k) n)
// CHECK-NEXT:     closure move b(mut x: i32) -> i32 [k, n]
// CHECK-NEXT:       block
// CHECK-NEXT:         return (add (mul x k) n)
// CHECK-NEXT:     closure c(mut x: i32) -> i32 []
// CHECK-NEXT:       block
// CHECK-NEXT:         return x
// CHECK-NEXT:     closure d(mut x: i32) -> () []
// CHECK-NEXT:       block
// CHECK-NEXT:     let mut r: i32 = (add (add (call twice a 1:i32) (call_closure b 2:i32)) (call_closure c 3:i32))
// CHECK-NEXT:     expr (call_closure d r)
template <typename G>
int twice(const G& g, int x) {
    return g(g(x));
}

int main() {
    int k = 2;
    int n = 5;
    auto a = [n, k](int x) -> int { return x * k + n; };
    auto b = [k, n](int x) -> int { return x * k + n; };
    auto c = [n](int x) mutable { return x; };
    auto d = [](int x) -> void { };
    int r = twice(a, 1) + b(2) + c(3);
    d(r);
    return 0;
}
