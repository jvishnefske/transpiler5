// FR-251: canonical Rust -> C++ under --check-roundtrip (laws 1-2 on BOTH
// sides), the Rust-side half of the vector/string rows. Pins `&mut
// Vec<String>` / `&String` <=> `std::vector<std::string>&` / `const
// std::string&`; `v.resize(n, s.clone())` <=> `v.resize(n, s)` (a COPIED
// position: C++ takes `const T&`, so the clone is normal form N8's);
// `v.push(t)` of a mutable local <=> `push_back(std::move(t))`; an element
// receiver `v[last].push_str("#")` <=> `v[last] += "#"`; `.iter()` <=>
// `for (const std::string& w : words)`; `vec![Vec::new(); 2usize]` <=>
// `std::vector<std::vector<long long>> grid(n, std::vector<long long>{})`;
// `grid.clone()` <=> a copy and a bare `grid` <=> `std::move(grid)` -- the
// moved-from name never appears again; `bx::substr(&v[0], ..)` with an
// element argument; the `#include` set follows use (`<utility>` for
// std::move).
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// CHECK:      #include <cstdio>
// CHECK-NEXT: #include <string>
// CHECK-NEXT: #include <utility>
// CHECK-NEXT: #include <vector>
// CHECK-EMPTY:
// CHECK-NEXT: void fill(std::vector<std::string>& v, const std::size_t n, const std::string& s) {
// CHECK-NEXT:     v.resize(n, s);
// CHECK-NEXT:     std::string t = s;
// CHECK-NEXT:     t += "+";
// CHECK-NEXT:     v.push_back(std::move(t));
// CHECK-NEXT:     const std::size_t last = v.size() - static_cast<std::size_t>(1);
// CHECK-NEXT:     v[last] += "#";
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: std::vector<std::size_t> tally(const std::vector<std::string>& words) {
// CHECK-NEXT:     std::vector<std::size_t> lens;
// CHECK-NEXT:     for (const std::string& w : words) {
// CHECK-NEXT:         lens.push_back(w.size());
// CHECK-NEXT:     }
// CHECK-NEXT:     return lens;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: int main() {
// CHECK-NEXT:     std::vector<std::string> v;
// CHECK-NEXT:     const std::string s = "ab";
// CHECK-NEXT:     fill(v, static_cast<std::size_t>(2), s);
// CHECK-NEXT:     const std::vector<std::size_t> lens = tally(v);
// CHECK-NEXT:     std::vector<std::vector<long long>> grid(static_cast<std::size_t>(2), std::vector<long long>{});
// CHECK-NEXT:     grid[static_cast<std::size_t>(0)].push_back(7LL);
// CHECK-NEXT:     const std::vector<std::vector<long long>> copy = grid;
// CHECK-NEXT:     const std::vector<std::vector<long long>> moved = std::move(grid);
// CHECK-NEXT:     std::printf("%s %zu %d %s\n", v[static_cast<std::size_t>(2)].c_str(), lens[static_cast<std::size_t>(2)], static_cast<int>(copy == moved), v[static_cast<std::size_t>(0)].substr(static_cast<std::size_t>(1), static_cast<std::size_t>(5)).c_str());
// CHECK-NEXT:     return 0;
// CHECK-NEXT: }
fn fill(v: &mut Vec<String>, n: usize, s: &String) {
    v.resize(n, s.clone());
    let mut t: String = s.clone();
    t.push_str("+");
    v.push(t);
    let last: usize = v.len().wrapping_sub(1usize);
    v[last].push_str("#");
}

fn tally(words: &Vec<String>) -> Vec<usize> {
    let mut lens: Vec<usize> = Vec::new();
    for w in words.iter() {
        lens.push(w.len());
    }
    return lens;
}

fn main() {
    let mut v: Vec<String> = Vec::new();
    let s: String = String::from("ab");
    fill(&mut v, 2usize, &s);
    let lens: Vec<usize> = tally(&v);
    let mut grid: Vec<Vec<i64>> = vec![Vec::new(); 2usize];
    grid[0usize].push(7i64);
    let copy: Vec<Vec<i64>> = grid.clone();
    let moved: Vec<Vec<i64>> = grid;
    print!("{} {} {} {}\n", v[2usize], lens[2usize], (copy == moved) as i32, bx::substr(&v[0usize], 1usize, 5usize));
}
