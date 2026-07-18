// RUN: emitrust-import-c %s | FileCheck %s

// CTS-S (00189): static devirtualization of never-reassigned global
// function pointers. A global function pointer initialized to a known
// target and never reassigned anywhere in the TU is an import-time ALIAS:
// no `emitrust.global` is materialized for it, and every call through it
// lowers as a direct call to the target. When the aliased target is the
// hosted variadic `fprintf`, a call `fprintfptr(stdout, fmt, ...)` routes
// through the printf machinery exactly like `printf(fmt, ...)`: the
// `stdout` argument is swallowed with the fprintf->printf routing (that
// first-arg slot is the ONLY place a FILE* value is accepted; see
// fnptr-devirt-invalid.c), and no reference to `fprintf` or `stdout`
// survives in the module.

#include <stdio.h>

int add(int a, int b) { return a + b; }

// A const-qualified global fn-ptr to an in-TU function: devirtualized.
int (*const addptr)(int, int) = &add;

// The 00189 shape verbatim: non-const-qualified but never reassigned,
// aliasing the hosted variadic fprintf.
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;

// Neither alias materializes a global, and nothing from stdio leaks in.
// CHECK-NOT: emitrust.global @addptr
// CHECK-NOT: emitrust.global @fprintfptr
// CHECK-NOT: @stdout

// Calls through the alias — both the plain and the `(*p)(...)` spelling —
// are direct calls; no fn_ptr value and no indirect call is created.
int direct_devirt(void) { return addptr(2, 3) + (*addptr)(4, 1); }
// CHECK-LABEL: func.func @direct_devirt
// CHECK-NOT: emitrust.call_indirect
// CHECK: call @add(%{{.*}}, %{{.*}}) : (i32, i32) -> i32
// CHECK: call @add(%{{.*}}, %{{.*}}) : (i32, i32) -> i32
// CHECK: arith.addi
// CHECK-NOT: emitrust.call_indirect
// CHECK-NOT: !emitrust.fn_ptr

// A direct printf call and a devirtualized fprintfptr call mix in one
// function, and a devirtualized value call feeds the %d argument (the
// 00189 composition). Both print!s carry the translated Rust format
// string; the fprintfptr call swallowed its stdout argument.
int devirt_print(int x) {
  printf("direct %d\n", x);
  fprintfptr(stdout, "value %d\n", addptr(x, 1));
  return 0;
}
// CHECK-LABEL: func.func @devirt_print
// CHECK: emitrust.call_opaque "print!"(%{{.*}}) {args = ["direct {}\0A", 0 : index]} : (i32) -> ()
// CHECK: %[[R:.*]] = call @add(%{{.*}}, %{{.*}}) : (i32, i32) -> i32
// CHECK: emitrust.call_opaque "print!"(%[[R]]) {args = ["value {}\0A", 0 : index]} : (i32) -> ()
// CHECK-NOT: emitrust.call_indirect

// Multiple converted directives through the alias in one format.
void devirt_two(int a, int b) { fprintfptr(stdout, "a=%d b=%d\n", a, b); }
// CHECK-LABEL: func.func @devirt_two
// CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}) {args = ["a={} b={}\0A", 0 : index, 1 : index]} : (i32, i32) -> ()

// The aliases stay erased through the whole module.
// CHECK-NOT: emitrust.global @addptr
// CHECK-NOT: emitrust.global @fprintfptr
