// FR-253: zero `unsafe` in anything emitrust-bx emits for the algorithm rows,
// INCLUDING the four `mod bx` helpers they bring (bx::count, bx::count_if,
// bx::transform, and bx::sort_by_less -- libstdc++'s introsort with its
// heapsort fallback, written over safe slice indexing: an out-of-bounds index
// panics where C++ with a non-strict-weak-order comparator is undefined). A
// closure argument is `&f` / `&mut f` (an ordinary borrow rustc checks), the
// extreme is `*v.iter().max().unwrap()` (a panic on an empty vector, where
// C++ dereferences end(): UB on the C++ side only), and no `dyn`, raw pointer
// or `std::mem` appears anywhere. The rubric scores unsafe; a helper that hid
// it would relocate the obligation, not discharge it.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=SHAPE --input-file=%t.crate/src/main.rs %s
// RUN: not grep -q "dyn \|\*const\|\*mut\|std::mem" %t.crate/src/main.rs
// COUNT: {{^}}0{{$}}
// SHAPE: let mut n: i64 = bx::count(&v, &3) + bx::count_if(&v, &mut odd);
// SHAPE: bx::transform(&mut v, &twice);
// SHAPE: bx::sort_by_less(&mut v, &desc);
// SHAPE: let mut top: i32 = *v.iter().max().unwrap();
// SHAPE: mod bx {
// SHAPE: pub fn count<T: PartialEq>(v: &[T], x: &T) -> i64 {
// SHAPE: pub fn count_if<T: Copy>(v: &[T], mut p: impl FnMut(T) -> bool) -> i64 {
// SHAPE: pub fn transform<T: Copy>(v: &mut [T], mut f: impl FnMut(T) -> T) {
// SHAPE: pub fn sort_by_less<T: Copy>(v: &mut [T], mut less: impl FnMut(T, T) -> bool) {
// SHAPE: fn sbl_heap_sort<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], first: usize, last: usize, less: &mut F) {
#include <algorithm>
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {5, 8, 3, 3, 9, 1};
    int seen = 0;
    auto odd = [&seen](int a) -> bool {
        seen += 1;
        return a % 2 != 0;
    };
    auto twice = [](int a) -> int {
        return a * 2;
    };
    auto desc = [](int a, int b) -> bool {
        return a > b;
    };
    long long n = std::count(v.begin(), v.end(), 3) + std::count_if(v.begin(), v.end(), odd);
    std::transform(v.begin(), v.end(), v.begin(), twice);
    std::sort(v.begin(), v.end(), desc);
    int top = *std::max_element(v.begin(), v.end());
    std::printf("%lld %d %d %d\n", n, seen, top, v[5]);
    return 0;
}
