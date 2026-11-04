// FR-264: --emit=kernel shows constants as the Kernel holds them. A constant
// is an ITEM `const [internal] PATH: T = e` in the constants block (after the
// structs, before every function), whatever its C++ spelling (`static const`
// here); a use is `(const PATH)` -- an absolute path, NOT a variable: it is
// no place, is never moved, and a closure's capture list (`[x]`) never names
// it. Unsigned `+` on constants is the Kernel `add` (printed wrapping_add in
// Rust), and a narrowing initializer is an explicit `(cast u8 300:i32)`.
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      const kA: i32 = 7:i32
// CHECK-NEXT: const kWrap: u32 = (add 4294967295:u32 2:u32)
// CHECK-NEXT: const kNarrow: u8 = (cast u8 300:i32)
// CHECK-NEXT: const internal geo::kHidden: i32 = (mul (const kA) 2:i32)
// CHECK-NEXT: func geo::Get(mut x: i32) -> i32
// CHECK-NEXT:   block
// CHECK-NEXT:     closure move add(mut y: i32) -> i32 [x]
// CHECK-NEXT:       block
// CHECK-NEXT:         return (add (add x y) (const geo::kHidden))
// CHECK-NEXT:     return (call_closure add (const kA))
// CHECK-NEXT: func main() -> ()
// CHECK-NEXT:   block
// CHECK-NEXT:     print [spec(d flag='' width=None prec=None (call geo::Get 1:i32)), text ' ', spec(d flag='' width=None prec=None (const kWrap)), text ' ', spec(d flag='' width=None prec=None (cast i32 (const kNarrow))), text '\n']
#include <cstdint>
#include <cstdio>

static const int kA = 7;
const std::uint32_t kWrap = 0xFFFFFFFFu + 2u;
const std::uint8_t kNarrow = 300;
namespace geo {
namespace {
const int kHidden = kA * 2;
}
int Get(int x) {
    auto add = [x](int y) -> int { return x + y + kHidden; };
    return add(::kA);
}
}  // namespace geo

int main() {
    std::printf("%d %u %d\n", geo::Get(1), kWrap, (int)kNarrow);
    return 0;
}
