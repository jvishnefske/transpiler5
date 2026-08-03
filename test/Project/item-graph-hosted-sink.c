// FR-62 (slice 1): hosted-sink visibility. The graph is CLOSED -- a call
// into a system header normally produces no edge -- with ONE deliberate
// exception this test pins: a call whose resolved callee is a
// definition-less SYSTEM-HEADER declaration of one of the importer's hosted
// OUTPUT SINKS (printf, puts, putchar, fprintf, fwrite, sprintf, snprintf
// -- the calls the importer lowers by name into Rust output effects)
// synthesizes a real function node (`def=0 linkage=extern`, the callee's
// real system-header location) and a real `Calls` edge, so output-effect
// attribution is total for FR-62's actor planner (E2 measured @stdout
// attribution working for only 7/131 kernel TUs under the closed graph).
// The control: strlen is hosted by the importer too, but it is NOT an
// output sink, so its system-header call still produces no node and no
// edge -- the exception is the sink set, not the whole hosted surface
// (--implicit-check-not proves strlen appears nowhere).
// RUN: emitrust-cc --emit=item-graph %s -o - \
// RUN:   | FileCheck %s --implicit-check-not=strlen

#include <stdio.h>
#include <string.h>

int emit(int n) {
  printf("%d\n", n);
  return n;
}

int width(const char *s) { return (int)strlen(s); }

int main(void) {
  emit(width("hi"));
  return 0;
}

// CHECK:      node c_main kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-hosted-sink.c:28:5
// CHECK-NEXT: node emit kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-hosted-sink.c:21:5
// The synthesized sink node: no definition anywhere in the project, extern
// linkage, and its LOCATION is the real system-header declaration's.
// CHECK-NEXT: node printf kind=function def=0 linkage=extern tu=0 loc={{.*}}stdio.h:{{[0-9]+}}:{{[0-9]+}}
// CHECK-NEXT: node width kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-hosted-sink.c:26:5

// CHECK-NEXT: edge c_main -> emit kind=Calls
// CHECK-NEXT: edge c_main -> width kind=Calls
// CHECK-NEXT: edge emit -> printf kind=Calls
// CHECK-NOT:  edge
