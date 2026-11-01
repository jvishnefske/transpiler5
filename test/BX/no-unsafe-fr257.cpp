// FR-257: zero `unsafe` in anything emitrust-bx emits for structs, member
// functions and constructors. A struct is a plain `#[derive(Clone)]` value,
// `this` is `&self` / `&mut self` (never a raw pointer), and every member
// access is a safe field path -- the rubric scores unsafe, and an emitter
// that reached a member through a pointer cast would not be a solution.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=SHAPE --input-file=%t.crate/src/main.rs %s
// COUNT: {{^}}0{{$}}
// SHAPE: #[derive(Clone)]
// SHAPE: struct Acc {
// SHAPE: impl Acc {
// SHAPE: pub fn new(start: i64) -> Acc {
// SHAPE: pub fn add(&mut self, k: i64) {
// SHAPE: pub fn get(&self) -> i64 {
#include <cstdio>
#include <vector>

struct Acc {
    std::vector<long long> log;
    long long sum;
    Acc(const long long start) : log({start}), sum(start) {}
    void add(const long long k) {
        log.push_back(k);
        sum += k;
    }
    long long get() const {
        return sum + static_cast<long long>(log.size());
    }
};

int main() {
    Acc a(5LL);
    a.add(7LL);
    std::printf("%lld\n", a.get());
    return 0;
}
