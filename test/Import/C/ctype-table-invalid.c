// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: emitrust-cc --emit=import --recover %s -o - 2>&1 | FileCheck %s --check-prefix=RECOVER

// FR-129 half (a): glibc's <ctype.h> classifiers are MACROS that expand to
// a locale-table read through a pointer returned by a hosted extern
// function -- `isspace(c)` becomes `(*__ctype_b_loc())[(int)(c)] & _ISspace`.
// The table read is genuinely unsupported (that pointer has no region the
// model tracks), but before this pin it surfaced as the generic
// "unsupported pointer cast (LValueToRValue)" located on a source line
// whose text contains NO pointer and NO cast: a reader of `isspace(*s)`
// had no path from the wording to the cause, and the FR-42 ledger filed
// the item under `other`, the corpus's largest junk bucket. This pins that
// the rejection NAMES the ctype table and tabulates under its own tag.
//
// The shape is declared here rather than reached via `#include <ctype.h>`
// on purpose: the pin must not depend on which libc's macro expansion the
// test machine has, and this IS that expansion's AST -- a subscript into
// the dereference of a call-returned pointer. The accessor's own
// declaration is rejected first (a pointer return type), which is why the
// body-level wording is reachable only in recovery mode; strict mode stops
// at the declaration, and both directions are pinned.

// STRICT: ctype-table-invalid.c:[[#@LINE+2]]:{{[0-9]+}}: error: unsupported: pointer return type
// RECOVER: ctype-table-invalid.c:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: pointer return type
extern const unsigned short **__ctype_b_loc(void);

// RECOVER: ctype-table-invalid.c:[[#@LINE+2]]:{{[0-9]+}}: warning: unsupported: locale ctype table lookup through '__ctype_b_loc' (the <ctype.h> classifiers are macros over a locale table)
int my_isspace(int c) {
  return (*__ctype_b_loc())[c] & 8192;
}

// The ledger stops filing this under `other`.
// RECOVER: stubbed 'my_isspace' [ctype-table] unsupported: locale ctype table lookup through '__ctype_b_loc'
