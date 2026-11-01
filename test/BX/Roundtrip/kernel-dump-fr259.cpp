// FR-259: --emit=kernel shows the normal forms N20-N22 and the two new terms
// as the Kernel holds them: `static std::uint32_t a(..)` and `inline unsigned
// b(..)` have the SAME shape (no storage class, u32 -- N20, N21); in a
// namespace `static std::int16_t c` and the anonymous-namespace `short d` are
// both `func internal` (N21); `auto m = n;` and `unsigned long k = n;` are the
// same `let mut ..: usize` (N22, N20); `i++` is `(postinc i)`, `++i`
// `(preinc i)` and `n > 0 ? 1 : 2` is `(cond (gt n 0:usize) 1:i32 2:i32)` (the 0 is a usize literal, N2).
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      func a(mut x: u32) -> u32
// CHECK-NEXT:   block
// CHECK-NEXT:     return (add x 1:u32)
// CHECK-NEXT: func b(mut x: u32) -> u32
// CHECK-NEXT:   block
// CHECK-NEXT:     return (add x 1:u32)
// CHECK-NEXT: func internal ns::c(mut x: i16) -> i16
// CHECK-NEXT:   block
// CHECK-NEXT:     return x
// CHECK-NEXT: func internal ns::d(mut x: i16) -> i16
// CHECK-NEXT:   block
// CHECK-NEXT:     return x
// CHECK-NEXT: func ns::e(mut n: usize) -> i32
// CHECK-NEXT:   block
// CHECK-NEXT:     let mut m: usize = n
// CHECK-NEXT:     let mut k: usize = n
// CHECK-NEXT:     let mut i: i32 = 0:i32
// CHECK-NEXT:     let mut j: i32 = (add (postinc i) (cond (gt n 0:usize) 1:i32 2:i32))
// CHECK-NEXT:     let mut q: i32 = (preinc i)
// CHECK-NEXT:     return (add (add (add (add j q) (cast i32 (add m k))) (cast i32 (call ns::c 3:i16))) (cast i32 (call ns::d 4:i16)))
// CHECK-NEXT: func main() -> ()
// CHECK-NEXT:   block
// CHECK-NEXT:     print [spec(d flag='' width=None prec=None (call a 1:u32)), text ' ', spec(d flag='' width=None prec=None (call b 2:u32)), text ' ', spec(d flag='' width=None prec=None (call ns::e 5:usize)), text '\n']
#include <cstddef>
#include <cstdint>
#include <cstdio>

static std::uint32_t a(std::uint32_t x) { return x + 1U; }
inline unsigned b(unsigned x) { return x + 1U; }
namespace ns {
static std::int16_t c(std::int16_t x) { return x; }
namespace {
short d(short x) { return x; }
}  // namespace
int e(std::size_t n) {
    auto m = n;
    unsigned long k = n;
    int i = 0;
    int j = i++ + (n > 0 ? 1 : 2);
    int q = ++i;
    return j + q + static_cast<int>(m + k) + c(3) + d(4);
}
}  // namespace ns

int main() {
    std::printf("%u %u %d\n", a(1), b(2), ns::e(5));
    return 0;
}
