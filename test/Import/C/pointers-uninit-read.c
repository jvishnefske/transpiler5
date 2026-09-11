// RUN: split-file %s %t
// RUN: emitrust-import-c %t/charptr.c | FileCheck %s --check-prefix=MLIR
// RUN: emitrust-cc --emit=rust %t/charptr.c -o - | FileCheck %s --check-prefix=CHARPTR --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/intptr.c -o - | FileCheck %s --check-prefix=INTPTR --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/guarded.c -o - | FileCheck %s --check-prefix=GUARDED --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/initialized.c -o - | FileCheck %s --check-prefix=INIT --implicit-check-not=panic!
// RUN: emitrust-cc --emit=rust %t/unevaluated.c -o - | FileCheck %s --check-prefix=UNEVAL --implicit-check-not=panic!

// FR-238. INVARIANT PINNED HERE: reading a pointer local that is NEVER
// written anywhere in its function is C undefined behaviour -- the
// lvalue-to-rvalue conversion of an indeterminate pointer (C11 6.3.2.1p2,
// Annex J.2) -- so this tree refines it into a DETERMINISTIC PANIC at the
// offending expression statement rather than refusing the whole
// translation unit. It NEVER invents a value for the pointer: there is no
// decomposition, no cursor, no base, and nothing downstream of the panic
// is emitted.
//
// WHY THE STATEMENT AND NOT THE FUNCTION: a function whose uninitialized
// read sits under a branch has DEFINED paths (see `guarded.c`, whose
// `else` arm prints and must keep printing). Panicking at function entry
// would miscompile those. The statement is the smallest unit this dialect
// can replace wholesale, and entering an expression statement with no
// short-circuit or conditional operator in it is exactly "the read is
// evaluated", so the refinement has the same reachability as the UB.
//
// WHAT STAYS REFUSED is pinned next door in pointers-uninit-read-invalid.c.

//--- charptr.c
// The corpus shape (PUBLIC-Test-Corpus 011_uninit_char_ptr): `bad` reads
// an indeterminate `char *`, `good` assigns a string literal first. Only
// `bad` panics; `good` keeps its ordinary string-literal region.
int printf(const char *, ...);

void printLine(const char *line) {
  if (line != 0) {
    printf("%s\n", line);
  }
}

void bad(void) {
  char *data;
  printLine(data);
}

void good(void) {
  char *data;
  data = "string";
  printLine(data);
}

// The uninitialized pointer materializes NO runtime state at all: no
// cursor cell, no non-null flag, no base. The body is the panic and
// nothing else.
// MLIR-LABEL: func.func @bad
// MLIR-NOT: memref.alloca
// MLIR: emitrust.call_opaque "panic!"
// MLIR-SAME: read of uninitialized pointer 'data'
// MLIR-NOT: call @printLine
// MLIR: return

// `good` is untouched by any of this: it still binds the literal backing
// and calls through.
// MLIR-LABEL: func.func @good
// MLIR: call @printLine

// CHARPTR: fn bad() {
// CHARPTR-NEXT: panic!("read of uninitialized pointer 'data'");
// CHARPTR-NEXT: }
// CHARPTR: fn good() {
// CHARPTR: print_line(

//--- intptr.c
// PUBLIC-Test-Corpus 012_uninit_int_ptr: the callee DEREFERENCES its
// argument, so there is not even a null test to fold. Same refinement --
// the panic precedes argument evaluation, so no address is ever fabricated
// to pass.
int printf(const char *, ...);

void printIntPtrLine(const int *intNumber) {
  printf("%d\n", *intNumber);
}

void bad(void) {
  int *data;
  printIntPtrLine(data);
}

void good(void) {
  int data;
  data = 5;
  int *data_addr;
  data_addr = &data;
  printIntPtrLine(data_addr);
}

// INTPTR: fn bad() {
// INTPTR-NEXT: panic!("read of uninitialized pointer 'data'");
// INTPTR-NEXT: }
// INTPTR: fn good() {
// INTPTR: print_int_ptr_line(

//--- guarded.c
// THE LOAD-BEARING CASE. The read is inside an `if`, so the `else` arm is
// a perfectly defined path. The panic lands inside the `if` body and
// NOWHERE ELSE; `defined()` must still print. This is what rules out
// hoisting the panic to function entry.
int printf(const char *, ...);

void printLine(const char *line) {
  if (line != 0) {
    printf("%s\n", line);
  }
}

void pick(int c) {
  char *data;
  if (c) {
    printLine(data);
  } else {
    printf("defined\n");
  }
}

// GUARDED: fn pick(c: i32) {
// GUARDED: if c != 0i32 {
// GUARDED-NEXT: panic!("read of uninitialized pointer 'data'");
// GUARDED: } else {
// GUARDED: println!("defined");

//--- initialized.c
// A program in which every pointer local is written before use must be
// COMPLETELY unaffected: no panic anywhere (`--implicit-check-not=panic!`
// on this run enforces that globally, not just inside these functions).
int printf(const char *, ...);

void printLine(const char *line) {
  if (line != 0) {
    printf("%s\n", line);
  }
}

void always(int c) {
  char *data;
  data = "yes";
  if (c) {
    printLine(data);
  }
  printLine(data);
}

int scalar(void) {
  int x = 41;
  int *p = &x;
  return *p;
}

// THE ADVERSARIAL ONE. `data` is written only THROUGH a second-order alias
// (CTS-P5), and the address-of that creates the alias is CONSUMED by the
// binding -- so it does not invalidate `data`'s region. This is the shape
// where the region analysis alone would wrongly report "never written", and
// the `--implicit-check-not=panic!` on this run is the proof that the
// redundant address-of scan stops it. (The variant whose alias is never
// written back is refused outright; see pointers-uninit-read-invalid.c.)
void via_alias(void) {
  char *data;
  char **pp = &data;
  *pp = "aliased";
  printLine(data);
}

// INIT: fn always(c: i32) {
// INIT: fn scalar() -> i32 {
// INIT: fn via_alias() {

//--- unevaluated.c
// AN UNEVALUATED OPERAND IS NOT A READ, and this is a REGRESSION TEST, not
// a hypothetical: the first cut of this refinement panicked c-testsuite
// 00219, which declares `const int * const ptr;`, never initializes it, and
// asks `_Generic(ptr, ...)`. C11 6.5.1.1p3 says the controlling expression
// of a generic selection IS NOT EVALUATED -- the program is well-defined
// and prints 20 -- but clang's AST still spells the operand with an
// lvalue-to-rvalue cast, because the selection is made on the CONVERTED
// type. `sizeof` and `_Alignof` operands are the same shape. The byte-diff
// oracle caught it; this pins it.
int printf(const char *, ...);

int unevaluated(void) {
  int i;
  const int *ptr;
  const char *ti;
  i = _Generic(ptr, int *: 1, const int *: 2, default: 20);
  printf("%d\n", i);
  i = _Generic(ti, const char *: 4, char *: 3, default: 9);
  printf("%d\n", i);
  i = (int)sizeof(ptr) > 0;
  printf("%d\n", i);
  return i;
}

// The whole body folds to constants: no panic, and no pointer state either.
// UNEVAL: fn unevaluated() -> i32 {
// UNEVAL: println!("{}", 2i32);
// UNEVAL: println!("{}", 4i32);
