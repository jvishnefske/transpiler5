// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/endptr-member.c 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/endptr-cast.c 2>&1 | FileCheck %s --check-prefix=CAST
// RUN: not emitrust-import-c %t/endptr-local.c 2>&1 | FileCheck %s --check-prefix=LOCAL
// RUN: not emitrust-import-c %t/endptr-param.c 2>&1 | FileCheck %s --check-prefix=PARAM
// RUN: not emitrust-import-c %t/strtoll.c 2>&1 | FileCheck %s --check-prefix=STRTOLL
// RUN: not emitrust-import-c %t/strtof.c 2>&1 | FileCheck %s --check-prefix=STRTOF
// RUN: not emitrust-import-c %t/strtold.c 2>&1 | FileCheck %s --check-prefix=STRTOLD

// FR-234 rung 1 admitted `strtol`/`strtoul`/`strtod` with a NULL `endptr`
// ONLY. This file is the boundary of that admission, and it exists for a
// reason sharper than completeness: without an explicit refusal the
// lowering would simply IGNORE a non-null second argument and emit a
// correct-looking parse that never wrote the caller's cursor -- a silent
// wrong answer, which is the one outcome this project forbids outright.
// Every shape below therefore has to REJECT, with a location.
//
// THE FINDING THIS FILE RECORDS, and it corrected the plan: the endptr
// out-parameter is NOT one frontier, it is FOUR, and only two of them
// reach the strto* lowering at all. `&end` on a local pointer and a
// `char **` parameter are both rejected by the POINTER REGION ANALYSIS
// before any call is lowered, and their wordings are older than this
// feature and unchanged by it -- which is also why this change moves no
// census tag for those shapes. Only a member address and a cast reach the
// new refusal. A reader ranking rung 2 needs all four, because fixing the
// call lowering alone would clear none of the first two.

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

//--- endptr-local.c
// THE COMMON SHAPE, and it never reaches the strto* lowering: taking the
// address of a local pointer sinks `end` in the pointer region analysis
// first. The wording is unchanged by FR-234 and is pinned here so a later
// rung that expects to see the endptr diagnostic knows where the refusal
// actually comes from.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42x";
  char *end;
  long v = strtol(buf, &end, 10);
  printf("%ld %c\n", v, *end);
  return 0;
}
// LOCAL: endptr-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a pointer variable

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
