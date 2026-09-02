// W2.17 on the FR-40 whole-program index. A destructor is a MEMBER, and
// members are not item-graph nodes (only records, functions and globals
// are), so admitting one must add exactly nothing to the index: the class
// keeps its single RECORD node and the destructor introduces no function
// node of its own. That is the additivity pin -- a spurious `r_dtor` node
// would silently inflate the `--incremental` PORTING.md /
// emitrust-progress.json DENOMINATOR by one item per RAII class, the same
// accounting failure W2.15/W2.16 closed from the other direction.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

extern "C" int printf(const char *, ...);

struct Tracer {
  int id;
  Tracer(int i) : id(i) { printf("ctor %d\n", id); }
  ~Tracer() { printf("dtor %d\n", id); }
};

int main() {
  Tracer a(3);
  printf("%d\n", a.id);
  return 0;
}

// CHECK-DAG: node Tracer kind=record def=1
// CHECK-DAG: node c_main kind=function def=1
// CHECK-DAG: edge c_main -> Tracer kind=BodyType

// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s --check-prefix=NOMEMBERS
// NOMEMBERS-NOT: node tracer_dtor
// NOMEMBERS-NOT: node tracer_ctor
// NOMEMBERS-NOT: node drop kind=
