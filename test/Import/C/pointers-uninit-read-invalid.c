// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/if-cond.c 2>&1 | FileCheck %s --check-prefix=IFCOND
// RUN: not emitrust-import-c %t/null-test.c 2>&1 | FileCheck %s --check-prefix=NULLTEST
// RUN: not emitrust-import-c %t/short-circuit.c 2>&1 | FileCheck %s --check-prefix=SHORTC
// RUN: not emitrust-import-c %t/conditional.c 2>&1 | FileCheck %s --check-prefix=COND
// RUN: not emitrust-import-c %t/while-cond.c 2>&1 | FileCheck %s --check-prefix=WHILECOND
// RUN: not emitrust-import-c %t/return.c 2>&1 | FileCheck %s --check-prefix=RET
// RUN: not emitrust-import-c %t/decl-init.c 2>&1 | FileCheck %s --check-prefix=DECLINIT
// RUN: not emitrust-import-c %t/write-through.c 2>&1 | FileCheck %s --check-prefix=WRITETHRU
// RUN: not emitrust-import-c %t/second-order.c 2>&1 | FileCheck %s --check-prefix=SECONDORDER
// RUN: not emitrust-import-c %t/for-increment.c 2>&1 | FileCheck %s --check-prefix=FORINC

// FR-238 FRONTIER. The half of the uninitialized-pointer refinement that
// STAYS REFUSED, pinned by its exact wording so the boundary cannot drift
// silently in either direction.
//
// pointers-uninit-read.c admits ONE shape: an expression statement, free of
// any short-circuit/conditional/statement-expression construct, that reads a
// pointer local written nowhere in the function. Entering such a statement
// IS evaluating the read, so the deterministic panic that replaces it has
// exactly the reachability of the undefined behaviour.
//
// Every file below fails one clause of that. In each, a panic would either
// be REACHABLE ON A DEFINED PATH (a miscompile, strictly worse than today's
// refusal) or would have to invent a value to carry onward. A located
// rejection is the correct floor, so these keep the historical diagnostic.

//--- if-cond.c
// An `if` CONDITION is an expression, but the statement is an IfStmt, whose
// arms are not the read. The refinement is expression-STATEMENT scoped and
// does not claim control-flow statements.
void printf_stub(void);
void f(void) {
  char *data;
  if (data) {
    printf_stub();
  }
}
// IFCOND: if-cond.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- null-test.c
// The same for an explicit null comparison. Note this is NOT folded away:
// an uninitialized pointer is not a statically-null region (CTS-P9), it is
// nothing at all, and it must never be confused with one.
void printf_stub(void);
void f(void) {
  char *data;
  if (data != 0) {
    printf_stub();
  }
}
// NULLTEST: null-test.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- short-circuit.c
// `c && g(data)` IS an expression statement, but `g(data)` runs only when
// `c` is true -- and the `c == 0` path is perfectly defined. Panicking on
// entry to the statement would break it.
int g(const char *);
void f(int c) {
  char *data;
  (void)(c && g(data));
}
// SHORTC: short-circuit.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- conditional.c
// The conditional operator has the same defence, and reaches its own
// pre-existing pointer-expression rejection first.
void printLine(const char *line);
void f(int c) {
  char *data;
  printLine(c ? "a" : data);
}
// COND: conditional.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer expression: ConditionalOperator

//--- while-cond.c
// A loop condition is re-evaluated; the statement is a WhileStmt. Refused.
void printf_stub(void);
void f(void) {
  char *data;
  while (data) {
    printf_stub();
  }
}
// WHILECOND: while-cond.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- return.c
// A ReturnStmt is not an expression statement. Replacing it with a panic
// would also leave a non-void function with no terminator to yield, so the
// shape is left refused rather than half-handled.
int f(void) {
  int *data;
  return *data;
}
// RET: return.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- decl-init.c
// `char *q = data;` is a declaration, not an expression statement, and it
// UNITES `q` into `data`'s region -- so neither one is "written nowhere".
void printLine(const char *line);
void f(void) {
  char *data;
  char *q = data;
  printLine(q);
}
// DECLINIT: decl-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'q' has no known target object

//--- write-through.c
// `*data = 3;` IS an expression statement that reads `data`, but the write
// THROUGH it makes the region analysis record a write-through site, so a
// region exists and the "nothing is known about this pointer" precondition
// fails. Refused, not panicked -- the conservative direction.
void f(void) {
  int *data;
  *data = 3;
}
// WRITETHRU: write-through.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- second-order.c
// THE LOAD-BEARING NEGATIVE. `char **pp = &data;` is a CTS-P5 second-order
// binding: the address-of is CONSUMED by that binding, so it does NOT
// invalidate `data`'s region and `regionOf(data)` stays null -- the region
// analysis alone would wrongly call `data` never-written, because `*pp = q`
// elsewhere writes it. The redundant syntactic address-of scan is what
// refuses this, which is why the refinement requires BOTH checks to agree.
void printLine(const char *line);
void f(void) {
  char *data;
  char **pp = &data;
  printLine(data);
}
// SECONDORDER: second-order.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object

//--- for-increment.c
// A `for` increment is an expression, but it is emitted by the loop
// lowering, not by the expression-statement path. Refused.
int g(const char *);
void f(void) {
  char *data;
  int i;
  for (i = 0; i < 2; g(data)) {
    i = i + 1;
  }
}
// FORINC: for-increment.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'data' has no known target object
