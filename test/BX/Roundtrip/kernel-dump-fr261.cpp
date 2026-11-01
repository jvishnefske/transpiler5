// FR-261: --emit=kernel shows pointers as the Kernel holds them: a pointer
// type is the slice type (`&[i32]` for `const int*`, `&mut [Cell]` for
// `Cell*`); `p[i]`, `*out` and `c->v` / `c[0].v` are Index / Field terms with
// a usize index (`(index out 0:usize)`, `(field (index c 0:usize) v)` -- N16
// makes `c->v` and `c[0].v` one term); `&c` / `&got` are `(addr_of ..)`;
// `xs.data() + 1` and `&xs[1]` are the SAME `(offset xs 1:usize)` (N17) and
// `xs.data()` is `(offset xs)` with no start; a pointer local `int* w = &got;`
// is a `let` of slice type (in a block: `got` is read after it).
// RUN: %emitrust-bx --emit=kernel %s | FileCheck %s
// CHECK:      struct Cell
// CHECK-NEXT:   field v: i32
// CHECK-NEXT: func peek(mut p: &[i32], mut i: usize) -> i32
// CHECK-NEXT:   block
// CHECK-NEXT:     return (add (index p i) (index p 0:usize))
// CHECK-NEXT: func touch(mut c: &mut [Cell], mut out: &mut [i32]) -> ()
// CHECK-NEXT:   block
// CHECK-NEXT:     store (field (index c 0:usize) v) add= 1:i32
// CHECK-NEXT:     store (index out 0:usize) = (field (index c 0:usize) v)
// CHECK-NEXT: func main() -> ()
// CHECK-NEXT:   block
// CHECK-NEXT:     let mut xs: Vec<i32> = (vec i32 [1:i32, 2:i32, 3:i32])
// CHECK-NEXT:     let mut c: Cell = (struct Cell [4:i32])
// CHECK-NEXT:     let mut got: i32 = 0:i32
// CHECK-NEXT:     block
// CHECK-NEXT:       let mut w: &mut [i32] = (addr_of got)
// CHECK-NEXT:       expr (call touch (addr_of c) w)
// CHECK-NEXT:     let mut q: &[i32] = (offset xs 1:usize)
// CHECK-NEXT:     print [spec(d flag='' width=None prec=None (add (add (call peek (offset xs) 2:usize) (call peek q 0:usize)) (call peek (offset xs 1:usize) 1:usize))), text ' ', spec(d flag='' width=None prec=None got), text '\n']
#include <cstddef>
#include <cstdio>
#include <vector>

struct Cell {
    int v;
};

int peek(const int* p, std::size_t i) {
    return p[i] + p[0];
}

void touch(Cell* c, int* out) {
    c->v += 1;
    *out = c[0].v;
}

int main() {
    std::vector<int> xs = {1, 2, 3};
    Cell c = {4};
    int got = 0;
    {
        int* w = &got;
        touch(&c, w);
    }
    const int* q = xs.data() + 1;
    std::printf("%d %d\n", peek(xs.data(), 2) + peek(q, 0) + peek(&xs[1], 1), got);
    return 0;
}
