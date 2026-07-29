// FR-41: the base case of the coloring — a project entirely inside the
// supported subset. Every item kind (function, record, enum, global) and
// every poison channel (Calls, SigType, BodyType, Field, ReadsGlobal,
// TakesAddressOf, and a self-recursive Calls edge) is present, so this pins
// that NONE of them colors anything on its own: a channel only carries poison
// when its target is Red, and here nothing is. It is the negative control for
// every other coloring test.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

enum Level { LOW, HIGH };

struct Leaf {
  int value;
};

struct Branch {
  struct Leaf leaf;
  enum Level level;
};

int counter;

static int depth(int n) { return n <= 0 ? 0 : depth(n - 1) + 1; }

int measure(struct Branch *b) {
  struct Leaf local;
  local.value = b->leaf.value + (int)b->level;
  counter += local.value;
  return depth(local.value);
}

int (*hook)(struct Branch *) = measure;

struct Branch origin;

int main(void) { return measure(&origin); }

// CHECK:      item Branch kind=record color=green reason=admissible
// CHECK-NEXT: item Leaf kind=record color=green reason=admissible
// CHECK-NEXT: item Level kind=enum color=green reason=admissible
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: item counter kind=global color=green reason=admissible
// CHECK-NEXT: item hook kind=global color=green reason=admissible
// CHECK-NEXT: item measure kind=function color=green reason=admissible
// CHECK-NEXT: item origin kind=global color=green reason=admissible
// CHECK-NEXT: item tu0_depth kind=function color=green reason=admissible
// CHECK-NEXT: tally green=9 yellow=0 red=0
// CHECK-NOT:  item
