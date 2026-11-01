// FR-261: zero `unsafe` in anything emitrust-bx emits for pointers. A pointer
// is a SAFE slice of its allocation (`&[T]` / `&mut [T]`), `&x` is
// `std::slice::from_mut` (a safe std function, no raw pointer and no helper in
// `mod bx`), `this` is `from_mut(self)`, `v.data()` is `&mut v[..]`, `p + k` a
// re-slice -- the rubric scores unsafe, and lowering C++ pointers to raw
// pointers (or to a helper that hides `unsafe`) would relocate the obligation,
// not discharge it.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=SHAPE --input-file=%t.crate/src/main.rs %s
// RUN: not grep -q "mod bx" %t.crate/src/main.rs
// COUNT: {{^}}0{{$}}
// SHAPE: pub fn reset(&mut self) {
// SHAPE: clear_all(std::slice::from_mut(self), 0);
// SHAPE: fn clear_all(mut c: &mut [Counter], mut v: i32) {
// SHAPE: c[0usize].hits = v;
// SHAPE: fn sum(mut p: &[i32], mut n: usize) -> i32 {
// SHAPE: p = &p[1usize..];
// SHAPE: let mut c: Counter = Counter { hits: 3 };
// SHAPE: clear_all(std::slice::from_mut(&mut c), 9);
// SHAPE: print!("{} {}\n", sum(&v[1usize..], 2usize), c.hits);
#include <cstdio>
#include <vector>

struct Counter;
void clear_all(Counter* c, int v);

struct Counter {
    int hits;
    void reset() { clear_all(this, 0); }
};

void clear_all(Counter* c, int v) {
    c->hits = v;
}

int sum(const int* p, std::size_t n) {
    int t = 0;
    while (n > 0) {
        t += *p;
        ++p;
        --n;
    }
    return t;
}

int main() {
    std::vector<int> v = {1, 2, 3};
    Counter c = {3};
    c.reset();
    clear_all(&c, 9);
    std::printf("%d %d\n", sum(v.data() + 1, 2), c.hits);
    return 0;
}
