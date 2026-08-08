// REQUIRES: cargo
// W2.10: ranged-for over vector and std::array, end to end. Builds a Rust
// crate through the full import + conversion + Rust-emission + `cargo
// build --release` pipeline and diffs its stdout against a `clang++
// -std=c++17` build of the identical source, byte for byte. Exercises:
// the by-value form over a vector (loop-variable writes must NOT leak
// into the container — pinned by printing an element after a body that
// reassigns its loop variable), the `&`-mutation form (element writes
// MUST land — printed after the loop), ranged-for over std::array,
// `break`/`continue` inside the desugared loop (the loop-stack wiring),
// and two independently-seeded vectors so a wiring bug cannot hide
// behind a repeated literal.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_ranged_for > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <vector>
#include <array>

int sum_scaled(int seed) {
  std::vector<int> v;
  v.push_back(seed);
  v.push_back(seed * 3);
  v.push_back(seed + 7);
  int total = 0;
  for (int x : v) {
    x = x * 2; // touches only the per-iteration copy
    total += x;
  }
  return total * 100 + v[0];
}

int main(void) {
  std::vector<int> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);
  v.push_back(4);
  for (int &x : v) {
    x = x * 10;
  }
  int picked = 0;
  for (int x : v) {
    if (x == 20) {
      continue;
    }
    if (x >= 40) {
      break;
    }
    picked += x;
  }
  std::array<int, 4> a = {3, 6, 9, 12};
  int arr_total = 0;
  for (int x : a) {
    arr_total += x * 2;
  }
  printf("%d %d %d %d %d %d\n", v[0], v[3], picked, arr_total,
         sum_scaled(2), sum_scaled(9));
  return 0;
}
