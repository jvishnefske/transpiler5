// A `///` or `/** */` doc comment is a FullComment child of the declaration
// it documents in clang's JSON AST. It is a comment -- dropped like any `//`
// comment -- not a class member, an initializer or a namespace item. Before
// this fix a doc comment on a struct rejected the struct ("FullComment inside
// a class", 2 woff2/brunsli sites), and one on a namespace-scope constant or
// on a namespace rejected those too. Every position is accepted here under
// --check-roundtrip (laws 1-2), and no diagnostic is printed.
// RUN: %emitrust-bx --check-roundtrip %s 2>&1 | FileCheck %s
// CHECK-NOT: error
// CHECK:      struct P {
// CHECK:      const kQ: i32 = 3;
// CHECK:      mod n {
// CHECK:      pub fn add(mut a: i32, mut b: i32) -> i32 {
// CHECK-NOT: error
#include <cstdio>
/// A point.
struct P {
  /** the x coordinate */
  int x;
  int y;
  /// their sum
  int sum() const { return x + y; }
};
/// a constant
const int kQ = 3;
/// a namespace
namespace n {
/// add two ints
int add(int a, int b) { return a + b; }
}  // namespace n
int main() {
  P p = {1, 2};
  printf("%d %d\n", p.sum(), n::add(kQ, 1));
  return 0;
}
