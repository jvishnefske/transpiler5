// FR-152 / FR-193 item 6: the same fence, reached through `emitrust.opaque`
// rather than through a struct_def carrying `emitrust.has_drop`.
//
// A `std::string` local is placed in an `emitrust.variable` whose value type is
// `!emitrust.opaque<"String">`. The dialect knows nothing about the Rust type
// behind that string, so the fence treats every opaque place as droppy: every
// opaque type this importer produces (`String`, `Vec<T>`) owns a heap
// allocation whose `Drop` runs per iteration. Guessing optimistically here
// would delete allocation/deallocation side effects exactly as the destructor
// case deletes `printf`s.
//
// THE FENCE FOR THIS PROGRAM MOVED EARLIER, and the pin moved with it. The
// FR-152 rejection is a PASS-level one: it fires once `lift-cf-to-scf` has
// already relocated the loop's exit path, at which point the only thing it
// can name is the IR place. But the relocation IS the FR-193 item 6 defect,
// and the import-time gate that fences that now sees the same program one
// stage sooner and names the SOURCE construct instead -- the declaration
// whose drop the exit path crosses, plus a note at the effect that crosses
// it. That is strictly more information about the same refusal, so the pin
// here is the earlier diagnostic. The pass-level wording is still live and
// still pinned by its sibling test, loop-escaping-place-drop-reject.cpp,
// whose escaping use is a plain field read (`return r.id;`) and carries no
// side effect for the import-time gate to catch.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

#include <cstdio>
#include <string>

int f(int n) {
  for (;;) {
    // CHECK: loop-escaping-place-opaque-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: error: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    std::string s;
    s += "ab";
    n++;
    if (n > 3)
      // Every read of an opaque place is a CALL, which is why this shape can
      // no longer reach the pass: `s.size()` is an observable effect on a
      // path that only leaves the loop.
      // CHECK: loop-escaping-place-opaque-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++, but it is on a path that only leaves the loop, so it is emitted after the loop -- past the drop
      return (int)s.size();
  }
}

// CHECK-NOT: failed to legalize operation 'scf.while'

int main() {
  printf("%d\n", f(1));
  return 0;
}
