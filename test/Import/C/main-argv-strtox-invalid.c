// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/strlen.c 2>&1 | FileCheck %s --check-prefix=SIG
// RUN: not emitrust-import-c %t/fprintf.c 2>&1 | FileCheck %s --check-prefix=SIG
// RUN: not emitrust-import-c %t/arith.c 2>&1 | FileCheck %s --check-prefix=SIG
// RUN: not emitrust-import-c %t/ordered-argv.c 2>&1 | FileCheck %s --check-prefix=SIG
// RUN: not emitrust-import-c %t/nullcmp.c 2>&1 | FileCheck %s --check-prefix=SIG
// RUN: not emitrust-import-c %t/own-strtol.c 2>&1 | FileCheck %s --check-prefix=OWN
// RUN: not emitrust-import-c %t/deref.c 2>&1 | FileCheck %s --check-prefix=DEREF
// RUN: not emitrust-import-c %t/write.c 2>&1 | FileCheck %s --check-prefix=WRITE
// RUN: not emitrust-import-c %t/join.c 2>&1 | FileCheck %s --check-prefix=JOIN
// RUN: not emitrust-import-c %t/nullinit.c 2>&1 | FileCheck %s --check-prefix=NULLINIT
// RUN: not emitrust-import-c %t/ordered-end.c 2>&1 | FileCheck %s --check-prefix=ORDERED
// RUN: emitrust-cc --emit=rust --recover %t/ordered-end.c -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RECOVER

// FR-234 rung 3 pinned the frontier the widening STOPS at. The admission
// grammar (`argvUseAdmitted`) is ALL-OR-NOTHING: one unadmitted use
// abandons the whole table and the historical signature-time rejection
// stands. That makes every line below a correctness pin rather than a
// taste pin -- a use that slipped into the admitted set without a
// lowering would not be a located refusal, it would be a SILENT change to
// whether `main` imports at all and to what it imports as.
//
// TWO DISTINCT FAILURE ADDRESSES, and the split is the point:
//
//   A  AN ARGV USE OUTSIDE FORMS 1 AND 2 keeps the C99-43 C3 rejection at
//      the SIGNATURE, wording and ledger tag (`{"use of main's argv",
//      "argv"}`) byte-identical to before rung 3. `strlen(argv[i])`,
//      `fprintf(stderr, "%s", argv[i])`, `argv[i] + k` and an ORDERED
//      comparison against `argv[i]` are the four forms the argv survey
//      found in the corpus and rung 3 deliberately did not take; a
//      project-supplied `strtol` is the fifth (the admission is
//      definition-less-callee only, so a body-bearing `strtol` gets an
//      ordinary call to its own code).
//
//      `argv[i] == NULL` is here for a REASON, not for scope: `argv[argc]`
//      IS a null pointer (C 5.1.2.2.1p2), so the test has a real runtime
//      answer, while the argv decomposition carries no null state and
//      would fold it to a constant -- the WRONG constant at `i == argc`.
//      Admitting it would have been a miscompile, and it also holds
//      `test/Import/C/main-args.c`'s `argv[0] != 0` pin in place.
//
//   B  A POINTER INTO AN ARGV ARGUMENT used outside the endptr identity
//      test rejects AT THE USE, because such a program mentions argv only
//      in forms 1 and 2 and therefore DOES get an argv table. A
//      dereference, a write-through, a join with another object, a null
//      binding and an ordered comparison each get their own located
//      wording. These are a NEW refusal class and carry a new ledger
//      needle (`{"main's argv", "argv-pointer"}`, mirrored into
//      `test/RealWorld/run_realworld.py`) so the census ranks them as the
//      separate piece of work they are instead of dropping them into
//      `other`.
//
// The ordered-comparison case has both spellings on purpose: against
// `argv[i]` it is form A (the grammar never admits it), while between two
// endptrs it is form B (neither operand mentions argv, so the program is
// admitted and the refusal has to come from the comparison lowering).
// `--recover` on that one pins FR-52's marker contract for the new class.

// SIG: error: unsupported: use of main's argv parameter (command-line argument values are not modeled)

//--- strlen.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  char *end;
  long v = strtol(argv[1], &end, 10);
  return (int)v + (int)strlen(argv[1]);
}

//--- fprintf.c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  strtol(argv[1], &end, 10);
  fprintf(stderr, "bad: %s\n", argv[1]);
  return 1;
}

//--- arith.c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  return (int)strtol(argv[1] + 1, &end, 10);
}

//--- ordered-argv.c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  strtol(argv[1], &end, 10);
  return end < argv[1];
}

//--- nullcmp.c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  strtol(argv[1], &end, 10);
  return argv[1] == 0;
}

//--- own-strtol.c
// A project that supplies its own `strtol` gets an ordinary call to its
// own code, so `argv[1]` is not a hosted subject string and the table is
// never granted. The FIRST rejection is the one the own definition's
// `char **` parameter raises, which is the pre-rung-2 frontier and is
// exactly right: nothing about argv was ever admitted here.
#include <stdio.h>
long strtol(const char *s, char **e, int b);
long strtol(const char *s, char **e, int b) { (void)e; return (long)*s + b; }
int main(int argc, char **argv) {
  char *end;
  return (int)strtol(argv[1], &end, 10);
}
// OWN: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

//--- deref.c
// Reading the byte AT the endptr is representable (`argv[i][cursor]`) and
// is deliberately NOT admitted this rung. It must reject with ITS OWN
// reason and never fall into the only-ever-null rejection an argv-rooted
// pointer would otherwise match (it is base-less exactly like one).
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  strtol(argv[1], &end, 10);
  return *end;
}
// DEREF: error: unsupported: dereference of a pointer into main's argv

//--- write.c
// argv's storage is borrowed SHARED (`&[Vec<i8>]`); a write through it has
// no representation at all and is refused at the write site.
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  strtol(argv[1], &end, 10);
  *end = 0;
  return 0;
}
// WRITE: error: unsupported: write through a pointer into main's argv

//--- join.c
// An argv argument is not a modeled C object, so a region that also binds
// one has no single representation. Refused rather than approximated.
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char buf[4] = "a";
  char *end = buf;
  strtol(argv[1], &end, 10);
  return end == argv[1];
}
// JOIN: error: unsupported: pointer joins main's argv with another object into one region

//--- nullinit.c
// The argv decomposition has no null state (no CTS-P8 flag cell), so a
// null binding is refused at the binding rather than silently dropped.
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end = 0;
  strtol(argv[1], &end, 10);
  return end == argv[1];
}
// NULLINIT: error: unsupported: null pointer constant assigned to a pointer into main's argv

//--- ordered-end.c
// Form B of the ordered comparison: NEITHER operand mentions argv, so the
// program IS admitted and the refusal has to come from the comparison
// lowering. C defines ordered comparison only within one object and which
// argv argument each side walks is a runtime fact, so there is nothing to
// compare against.
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *e1;
  char *e2;
  strtol(argv[1], &e1, 10);
  strtol(argv[1], &e2, 10);
  return e1 < e2;
}
// ORDERED: error: unsupported: ordered comparison of pointers into main's argv
// FR-52's marker contract for the new class: a located warning plus a stub
// that fails loudly rather than an emitted wrong answer.
// RECOVER: warning: unsupported: ordered comparison of pointers into main's argv (recovered: emitted an unimplemented!() stub with the mapped signature)
// RECOVER: unimplemented!("unsupported: ordered comparison of pointers into main's argv")
