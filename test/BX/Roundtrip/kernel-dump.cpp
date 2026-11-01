// FR-250: --emit=kernel is a stable, location-free dump of the Kernel IR --
// the shared meeting point of both languages. Pins that C++ implicit
// conversions are EXPLICIT casts in the Kernel, `if (x)` is `x != 0`,
// `++i` / `i += 1` / `i = i + 1` are one CompoundAssign (normal form N5),
// and the counting loop is one For node.
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      func f(mut a: i32, mut c: i8) -> i64
// CHECK-NEXT:   block
// CHECK-NEXT:     let mut r: i64 = (cast i64 (add a (cast i32 c)))
// CHECK-NEXT:     if (ne a 0:i32)
// CHECK-NEXT:       block
// CHECK-NEXT:         compound r add= 1:i64
// CHECK-NEXT:         compound r add= 1:i64
// CHECK-NEXT:         compound r add= 1:i64
// CHECK-NEXT:     for i in 0:i32 .. 3:i32
// CHECK-NEXT:       block
// CHECK-NEXT:         compound r mul= 2:i64
// CHECK-NEXT:     return r
// CHECK-NEXT: func main() -> ()
// CHECK-NEXT:   block
// CHECK-NEXT:     print [text 'r=', spec(d flag='' width=None prec=None (call f 7:i32 120:i8)), text '\n']
#include <cstdio>

long long f(int a, char c) {
    long long r = a + c;
    if (a) {
        ++r;
        r += 1;
        r = r + 1;
    }
    for (int i = 0; i < 3; ++i) {
        r *= 2;
    }
    return r;
}

int main() {
    std::printf("r=%lld\n", f(7, 'x'));
    return 0;
}
