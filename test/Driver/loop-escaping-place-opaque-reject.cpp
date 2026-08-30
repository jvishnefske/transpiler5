// FR-152: the same fence, reached through `emitrust.opaque` rather than
// through a struct_def carrying `emitrust.has_drop`.
//
// A `std::string` local is placed in an `emitrust.variable` whose value type is
// `!emitrust.opaque<"String">`. The dialect knows nothing about the Rust type
// behind that string, so the fence treats every opaque place as droppy: every
// opaque type this importer produces (`String`, `Vec<T>`) owns a heap
// allocation whose `Drop` runs per iteration. Guessing optimistically here
// would delete allocation/deallocation side effects exactly as the destructor
// case deletes `printf`s.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

#include <cstdio>
#include <string>

int f(int n) {
  for (;;) {
    // CHECK: loop-escaping-place-opaque-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: error: unsupported: local 's' has a destructor, is declared inside a loop body, and escapes the loop
    std::string s;
    s += "ab";
    n++;
    if (n > 3)
      return (int)s.size();
  }
}

// CHECK-NOT: failed to legalize operation 'scf.while'

int main() {
  printf("%d\n", f(1));
  return 0;
}
