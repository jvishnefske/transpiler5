// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/endptr-member.c 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/endptr-cast.c 2>&1 | FileCheck %s --check-prefix=CAST
// RUN: not emitrust-import-c %t/endptr-global.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/endptr-region.c 2>&1 | FileCheck %s --check-prefix=REGION
// RUN: not emitrust-import-c %t/endptr-param.c 2>&1 | FileCheck %s --check-prefix=PARAM
// RUN: not emitrust-import-c %t/strtoll.c 2>&1 | FileCheck %s --check-prefix=STRTOLL
// RUN: not emitrust-import-c %t/strtof.c 2>&1 | FileCheck %s --check-prefix=STRTOF
// RUN: not emitrust-import-c %t/strtold.c 2>&1 | FileCheck %s --check-prefix=STRTOLD

// FR-234 rung 2 admitted `strtol`/`strtoul`/`strtod` with a NULL `endptr`
// or with `&end` over a DECOMPOSED POINTER LOCAL. This file is the
// boundary of that admission, and it exists for a reason sharper than
// completeness: without an explicit refusal the lowering would simply
// IGNORE an endptr it cannot represent and emit a correct-looking parse
// that never wrote the caller's cursor -- a silent wrong answer, which is
// the one outcome this project forbids outright. Every shape below
// therefore has to REJECT, with a location.
//
// THE FINDING THIS FILE RECORDS, and it corrected the plan: the endptr
// out-parameter is NOT one frontier, it is FOUR, and only two of them
// reach the strto* lowering at all. Rung 1 pinned all four as rejections;
// rung 2 FLIPPED exactly one of them -- `&end` on a LOCAL pointer, which
// used to die in the POINTER REGION ANALYSIS at `taking the address of a
// pointer variable` and now imports (see
// `test/Import/C/libc-strtox-endptr.c` for the shape and
// `test/EndToEnd/libc-strtox-endptr.c` for the byte-diff). The pin moved
// FORWARD; it did not loosen. What is left is everything that still has
// nowhere to put the callee's answer:
//   - a GLOBAL `char *end`, which keeps the CTS-P4 refusal it always had;
//   - a `char **` PARAMETER, outside the cursor-parameter grammar;
//   - a MEMBER address and a CAST, which reach the strto* lowering;
//   - an `end` that walks a DIFFERENT REGION from the parsed string,
//     which is rung 2's own new refusal.

//--- endptr-member.c
// Reaches the strto* lowering: `&s.e` is not a null pointer constant, so
// the new refusal fires -- at the ARGUMENT's location, because that is
// where rung 2's work starts, not at the call's.
#include <stdio.h>
#include <stdlib.h>
struct S {
  char *e;
};
int main(void) {
  char buf[8] = "42x";
  struct S s;
  long v = strtol(buf, &s.e, 10);
  printf("%ld\n", v);
  return 0;
}
// MEMBER: endptr-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strtol with a non-null endptr argument

//--- endptr-cast.c
// The float form takes the same refusal, and a CAST is not a null pointer
// constant however pointer-shaped it looks.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42x";
  double d = strtod(buf, (char **)buf);
  printf("%f\n", d);
  return 0;
}
// CAST: endptr-cast.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strtod with a non-null endptr argument

//--- endptr-global.c
// A GLOBAL `char *end` has no decomposed cursor CELL to write -- a
// pointer-typed global's cursor is a module-level `emitrust.global`
// (CTS-P4) and its address escaping the region model is the refusal that
// has always covered it. Unchanged by rung 2, which widened the LOCAL
// shape only, and pinned here so the two are visibly different frontiers
// rather than one that half-works.
#include <stdio.h>
#include <stdlib.h>
char *end;
int main(void) {
  char buf[8] = "42x";
  long v = strtol(buf, &end, 10);
  printf("%ld %c\n", v, *end);
  return 0;
}
// GLOBAL: endptr-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a pointer variable

//--- endptr-region.c
// RUNG 2'S OWN NEW REFUSAL, and the one a reader is most likely to think
// is already handled. `end` here walks `other`, not `buf`: the cursor the
// callee hands back is an offset into the PARSED string's region, and
// storing it into a cursor over a different object would silently make
// `end` designate the wrong bytes -- `*end` would read `other[2]`. There
// is no representation for a pointer that changes region mid-function
// outside the CTS-P7 multi-base model, so this refuses at the call. Its
// wording carries the `strtox-endptr` census needle (verified by calling
// `classify_blocker`, not by reading it).
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42x";
  char other[8] = "7";
  char *end = other;
  long v = strtol(buf, &end, 10);
  printf("%ld %ld\n", v, (long)(end - buf));
  return 0;
}
// REGION: endptr-region.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strtol endptr must walk the parsed string's region

//--- endptr-param.c
// The forwarding shape, also rejected ahead of the call: a `char **`
// parameter is outside the admitted cursor-parameter grammar.
#include <stdlib.h>
long parse(char *buf, char **out) { return strtol(buf, out, 10); }
// PARAM: endptr-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

//--- strtoll.c
// THE FAMILY BOUNDARY. strtoll/strtoull/strtoimax/strtof/strtold are NOT
// admitted, and that is a decision rather than an oversight: each needs
// its own saturation constants or its own float width, and an unverified
// alias onto the long/double helpers would be exactly the silent
// divergence rung 1 exists to avoid. They keep the system-header refusal,
// which also keeps their `libc:<name>` census tag intact so a later
// increment can still find them.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42";
  long long v = strtoll(buf, NULL, 10);
  printf("%lld\n", v);
  return 0;
}
// STRTOLL: strtoll.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'strtoll' declared in a system header; not part of the supported C subset

//--- strtof.c
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42";
  float f = strtof(buf, NULL);
  printf("%f\n", f);
  return 0;
}
// STRTOF: strtof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'strtof' declared in a system header; not part of the supported C subset

//--- strtold.c
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42";
  long double g = strtold(buf, NULL);
  printf("%Lf\n", g);
  return 0;
}
// STRTOLD: strtold.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'strtold' declared in a system header; not part of the supported C subset
