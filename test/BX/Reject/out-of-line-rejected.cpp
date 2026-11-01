// An out-of-line member definition is a separate item from its in-class
// declaration. When the declaration (a user-declared destructor) or its class
// (a struct with members outside the subset) is rejected, the rejection is
// repeated AT the definition, located. Before this, both definitions carried
// no residual, and a per-function census (bx-census / bx-frontier) counted
// `S::~S` and `S::get` as clean EMIT: the FR-262 spike measured +5 phantom
// EMIT functions from this alone once project headers are visible.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
#include <cstdio>
// CHECK: out-of-line-rejected.cpp:[[@LINE+1]]:1: error: unsupported: struct with members outside the subset
struct S {
  int x;
  // CHECK-NEXT: out-of-line-rejected.cpp:[[@LINE+1]]:3: error: unsupported: user-declared destructor
  ~S();
  int get() const;
};
// CHECK-NEXT: out-of-line-rejected.cpp:[[@LINE+1]]:1: error: unsupported: definition of a rejected member function
S::~S() { std::printf("bye\n"); }
// CHECK-NEXT: out-of-line-rejected.cpp:[[@LINE+1]]:1: error: unsupported: member function of a struct with members outside the subset
int S::get() const { return x; }
// CHECK-NOT: error
int main() {
  S s = {1};
  std::printf("%d\n", s.get());
  return 0;
}
