// FR-252: zero `unsafe` in anything emitrust-bx emits for closures. A
// by-reference capture is an ordinary borrow the closure holds (rustc checks
// it; the subset's alias rules keep it unbuildable-free), a `mutable` lambda's
// copy is a `move` closure's own field, a callable parameter is `&impl Fn` /
// `&mut impl FnMut` -- no `Box<dyn Fn>`, no raw pointer to the captured
// variables, no `std::mem::transmute` of a closure, and no helper in `mod bx`
// (the rubric scores unsafe; a helper that hid it would relocate the
// obligation, not discharge it).
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=SHAPE --input-file=%t.crate/src/main.rs %s
// RUN: not grep -q "mod bx" %t.crate/src/main.rs
// RUN: not grep -q "dyn " %t.crate/src/main.rs
// COUNT: {{^}}0{{$}}
// SHAPE: fn run(f: &mut impl FnMut(i32) -> i32, mut n: i32) -> i32 {
// SHAPE: let mut keep = |mut x: i32| -> i32 {
// SHAPE: let mut step = move |mut x: i32| -> i32 {
// SHAPE: let mut a: i32 = run(&mut keep, 3);
// SHAPE: let mut b: i32 = run(&mut step, 4);
#include <cstdio>
#include <vector>

template <typename F>
int run(F& f, int n) {
    int last = 0;
    for (int i = 0; i < n; ++i) {
        last = f(i);
    }
    return last;
}

int main() {
    std::vector<int> v = {3, 1};
    int k = 4;
    auto keep = [&v](int x) -> int {
        v.push_back(x);
        return static_cast<int>(v.size());
    };
    auto step = [k](int x) mutable -> int {
        k = k + x;
        return k;
    };
    int a = run(keep, 3);
    int b = run(step, 4);
    std::printf("%d %d %d\n", a, b, v[4]);
    return 0;
}
