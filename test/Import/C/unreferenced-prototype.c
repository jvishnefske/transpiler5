// CTS-F2: body-less prototypes that nothing references — including repeated
// redeclarations of the same function — demand no definition and leave no
// body-less func.func behind (referenced-only policy, C99-39). The skip
// happens before signature mapping, so unsupported parameter shapes in an
// unreferenced prototype cannot reject the program either.
// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-empty.c | FileCheck %s

int foo(void);
int foo(void);
void bar(int *p);

int main(void) { return 0; }

// CHECK-NOT: @foo
// CHECK-NOT: @bar
// CHECK-LABEL: func.func @c_main() -> i32
// CHECK: return
// CHECK-NOT: @foo
// CHECK-NOT: @bar
