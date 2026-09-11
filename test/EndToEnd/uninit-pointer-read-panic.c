// REQUIRES: cargo
// FR-238. THE RUNTIME ORACLE for the uninitialized-pointer refinement.
//
// `char *data; printLine(data);` reads an indeterminate pointer. That is C
// undefined behaviour (C11 6.3.2.1p2, Annex J.2), so `emitrust-cc` used to
// refuse the whole translation unit -- and the refusal was CORRECT, because
// there is no value for `data` that this tree may invent. But refusing costs
// the DEFINED half of the same program, which is what these units pin: the
// UB is refined into a deterministic panic at the offending statement, and
// every path that does not reach that statement must be byte-identical to
// the clang native.
//
// THE UB PATH IS DELIBERATELY NOT BYTE-DIFFED. The native's behaviour there
// is undefined -- the corpus case this comes from ships that vector tagged
// `has_ub: "Program is killed after segmentation fault"` and the harness
// SKIPS it -- so no `diff` can hold it by construction, and the native is
// not run at all. The crate's exact panic text is pinned instead, which is
// the divergence made visible rather than merely asserted.
//
// Every choice is driven by `argc` (1 under lit), never by a literal, so no
// constant fold on either side can hide a miscompile by evaluating the
// choice at compile time.
//
// RUN: split-file %s %t
//
// 1: the defined half of PUBLIC-Test-Corpus 011_uninit_char_ptr. argc == 1,
//    so `good()` runs and prints; the panic in `bad()` is never reached.
// RUN: emitrust-cc --emit=crate %t/charptr.c -o %t/charptr.crate --crate-name uninit_charptr --build
// RUN: clang -std=c11 -w %t/charptr.c -o %t/charptr.native
// RUN: %t/charptr.native > %t/charptr.native.out
// RUN: %t/charptr.crate/target/release/uninit_charptr > %t/charptr.rust.out
// RUN: diff %t/charptr.native.out %t/charptr.rust.out
//
// 2: the defined half of PUBLIC-Test-Corpus 012_uninit_int_ptr, whose callee
//    DEREFERENCES the pointer, so there is not even a null test to fold.
// RUN: emitrust-cc --emit=crate %t/intptr.c -o %t/intptr.crate --crate-name uninit_intptr --build
// RUN: clang -std=c11 -w %t/intptr.c -o %t/intptr.native
// RUN: %t/intptr.native > %t/intptr.native.out
// RUN: %t/intptr.crate/target/release/uninit_intptr > %t/intptr.rust.out
// RUN: diff %t/intptr.native.out %t/intptr.rust.out
//
// 3: the UNREACHABILITY proof. `data` is read under `if (argc >= 2)`, which
//    is false, and the else arm plus everything after it is DEFINED. If the
//    panic had been hoisted to function entry this would abort; instead it
//    byte-diffs. The native is a legitimate oracle here precisely because
//    this execution never performs the indeterminate read.
// RUN: emitrust-cc --emit=crate %t/guarded.c -o %t/guarded.crate --crate-name uninit_guarded --build
// RUN: clang -std=c11 -w %t/guarded.c -o %t/guarded.native
// RUN: %t/guarded.native > %t/guarded.native.out
// RUN: %t/guarded.crate/target/release/uninit_guarded > %t/guarded.rust.out
// RUN: diff %t/guarded.native.out %t/guarded.rust.out
//
// 4: the UB path, reached because `argc >= 1` is true. The crate stops
//    loudly, at the offending statement, naming the variable. The native is
//    NOT run and NOT compared: it has no defined answer.
// RUN: emitrust-cc --emit=crate %t/reached.c -o %t/reached.crate --crate-name uninit_reached --build
// RUN: not %t/reached.crate/target/release/uninit_reached 2>&1 | FileCheck %s --check-prefix=PANIC
// PANIC: read of uninitialized pointer 'data'
//
// 5: the side effects BEFORE the UB statement still happen, and the ones
//    after it do not. A refinement that swallowed the whole function would
//    lose the first line.
// RUN: emitrust-cc --emit=crate %t/ordered.c -o %t/ordered.crate --crate-name uninit_ordered --build
// RUN: not %t/ordered.crate/target/release/uninit_ordered 2>&1 | FileCheck %s --check-prefix=ORDER
// ORDER: before
// ORDER-NOT: after
// ORDER: read of uninitialized pointer 'data'

//--- charptr.c
#include <stdio.h>

void printLine(const char *line) {
  if (line != NULL) {
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

int main(int argc, char **argv) {
  if (argc >= 2) {
    bad();
  } else {
    good();
  }
  printf("done %d\n", argc);
  return 0;
}

//--- intptr.c
#include <stdio.h>

void printIntPtrLine(const int *intNumber) {
  printf("%d\n", *intNumber);
}

void bad(void) {
  int *data;
  printIntPtrLine(data);
}

void good(int seed) {
  int data;
  data = 4 + seed;
  int *data_addr;
  data_addr = &data;
  printIntPtrLine(data_addr);
}

int main(int argc, char **argv) {
  if (argc >= 2) {
    bad();
  } else {
    good(argc);
  }
  printf("done %d\n", argc);
  return 0;
}

//--- guarded.c
#include <stdio.h>

void printLine(const char *line) {
  if (line != NULL) {
    printf("%s\n", line);
  }
}

int main(int argc, char **argv) {
  char *data;
  int i;
  printf("start %d\n", argc);
  for (i = 0; i < argc + 1; i++) {
    if (argc >= 2) {
      printLine(data);
    } else {
      printf("defined %d\n", i);
    }
  }
  printf("end %d\n", argc);
  return 0;
}

//--- reached.c
#include <stdio.h>

void printLine(const char *line) {
  if (line != NULL) {
    printf("%s\n", line);
  }
}

void bad(void) {
  char *data;
  printLine(data);
}

int main(int argc, char **argv) {
  if (argc >= 1) {
    bad();
  }
  return 0;
}

//--- ordered.c
#include <stdio.h>

void printLine(const char *line) {
  if (line != NULL) {
    printf("%s\n", line);
  }
}

void bad(int seed) {
  char *data;
  printf("before %d\n", seed);
  printLine(data);
  printf("after %d\n", seed);
}

int main(int argc, char **argv) {
  if (argc >= 1) {
    bad(argc);
  }
  return 0;
}
