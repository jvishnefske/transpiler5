// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/diff.c 2>&1 | FileCheck %s --check-prefix=DIFF
// RUN: not emitrust-import-c %t/user-arg.c 2>&1 | FileCheck %s --check-prefix=USERARG
// RUN: not emitrust-import-c %t/global-bind.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/member-bind.c 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/return-search.c 2>&1 | FileCheck %s --check-prefix=RETURN
// RUN: not emitrust-import-c %t/strstr-bind.c 2>&1 | FileCheck %s --check-prefix=STRSTR
// RUN: not emitrust-import-c %t/discarded.c 2>&1 | FileCheck %s --check-prefix=DISCARD

// The frontier AROUND FR-230's strchr cursor bind. Admitting the bind moved
// the boundary; these pin exactly where it now sits, so a later widening has
// to move a pin rather than quietly acquire a case.
//
// The distinction every unit below turns on: the bind gives the pointer a
// (region, cursor, non-null flag) decomposition, and that decomposition can
// only be consumed where the flag can be honoured. A pointer DIFFERENCE and
// a call ARGUMENT both discard the flag -- the callee has nowhere to put it
// -- so they stay refused rather than silently treating a failed search as
// index 0. A global or a struct member has no flag cell at all.
//
// `strstr` is the NEGATIVE CONTROL and the most important unit here: it
// proves the new region source is `strchr`/`strrchr`-specific and did not
// widen the analysis into "any library call returning a pointer".

//--- diff.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  char *p = strchr(a, 'a');
  return (int)(p - a);
}
// DIFF: diff.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: difference of possibly-null pointers

//--- user-arg.c
#include <string.h>
static int use(const char *s) { return s ? 1 : 0; }
int main(void) {
  char a[8] = "ab";
  char *p = strchr(a, 'a');
  return use(p);
}
// USERARG: user-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: possibly-null pointer passed as a function argument

//--- global-bind.c
#include <string.h>
char *g;
int main(void) {
  char a[8] = "ab";
  g = strchr(a, 'a');
  return g == 0;
}
// GLOBAL: global-bind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer bound to local object 'a' (the borrow would outlive the object)

//--- member-bind.c
#include <string.h>
struct holder {
  char *p;
};
int main(void) {
  char a[8] = "ab";
  struct holder h;
  h.p = strchr(a, 'a');
  return h.p == 0;
}
// MEMBER: member-bind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member assigned a non-address value

//--- return-search.c
#include <string.h>
const char *find(const char *s, int c) { return strchr(s, c); }
int main(void) {
  char a[8] = "ab";
  return find(a, 'a') == 0;
}
// RETURN: return-search.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value

//--- strstr-bind.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  char *p = strstr(a, "b");
  return p == 0;
}
// STRSTR: strstr-bind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- discarded.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  strchr(a, 'a');
  return 0;
}
// DISCARD: discarded.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a strchr result must feed a pointer binding, a printf '%s' argument or a comparison against a null pointer
