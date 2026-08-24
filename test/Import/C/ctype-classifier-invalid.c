// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/value.c 2>&1 | FileCheck %s --check-prefix=VALUE
// RUN: not emitrust-import-c %t/printed.c 2>&1 | FileCheck %s --check-prefix=PRINTED
// RUN: not emitrust-import-c %t/compared.c 2>&1 | FileCheck %s --check-prefix=COMPARED
// RUN: emitrust-cc --emit=import --recover %t/combined-mask.c -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=COMBINED
// RUN: not emitrust-import-c %t/tolower.c 2>&1 | FileCheck %s --check-prefix=TOLOWER
// RUN: not emitrust-import-c %t/toupper.c 2>&1 | FileCheck %s --check-prefix=TOUPPER
// RUN: emitrust-cc --emit=import --recover %t/tolower-table.c -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=TOLOWER-TABLE
// RUN: not emitrust-import-c %t/setlocale.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SETLOCALE
// RUN: not emitrust-import-c %t/uselocale.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=USELOCALE
// RUN: emitrust-cc --emit=import --recover %t/setlocale.c -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LEDGER

// FR-129 half (b): the FRONTIER of the admitted <ctype.h> classifiers.
// Half (b) admits the classifier family in BOOLEAN CONTEXT ONLY, because
// glibc's macro yields the `_IS*` MASK and C promises only "nonzero if
// true" -- a program that prints `isspace(' ')` prints 8192, so an image
// returning `bool` would be a MISCOMPILE outside boolean context. This file
// pins that everything outside the admitted set still fails LOUDLY and
// LOCATED, so nothing can silently emit wrong code:
//
//   * a classifier whose VALUE is used (an initializer, a printf argument,
//     an operand of `==`),
//   * a mask that is not exactly one classifier bit,
//   * `tolower`/`toupper`, which do not have this shape at all,
//   * ANY classifier in a translation unit that installs a locale, since
//     the ASCII image was measured only in the "C" locale.
//
// Every wording here was probed from the built tool, not guessed. The two
// hand-written table shapes run under `--recover` for the reason the
// half-(a) test records: the accessor's own declaration is rejected first
// (a pointer return type), so the body-level wording is reachable only
// under recovery.

//--- value.c
#include <ctype.h>
// The mask escapes into an int: this is the case the admission is narrowed
// to exclude, and it must stay rejected on the CAUSE, not on a generic
// pointer-cast mechanism.
// VALUE: value.c:7:11: error: unsupported: locale ctype table lookup through '__ctype_b_loc' (the <ctype.h> classifiers are macros over a locale table)
int value_use(int c) {
  int x = isspace(c);
  return x;
}

//--- printed.c
#include <ctype.h>
int printf(const char *, ...);
// PRINTED: printed.c:4:42: error: unsupported: locale ctype table lookup through '__ctype_b_loc' (the <ctype.h> classifiers are macros over a locale table)
void printed_use(int c) { printf("%d\n", isdigit(c)); }

//--- compared.c
#include <ctype.h>
// A COMPARISON is not one of the admitted boolean contexts. `== 0` happens
// to be truth-preserving, but `== 8192` would silently become false and
// `== 1` silently true; the admission does not try to tell those apart, and
// rejection is the safe direction.
// COMPARED: compared.c:8:7: error: unsupported: locale ctype table lookup through '__ctype_b_loc' (the <ctype.h> classifiers are macros over a locale table)
int compared_use(int c) {
  if (isspace(c) == 0)
    return 1;
  return 0;
}

//--- combined-mask.c
// A mask that is not exactly one of glibc's twelve classifier bits has no
// measured Rust image, so the shape is NOT admitted -- even in boolean
// context. Written by hand because no <ctype.h> macro spells it.
extern const unsigned short **__ctype_b_loc(void);
// COMBINED: combined-mask.c:7:7: warning: unsupported: locale ctype table lookup through '__ctype_b_loc' (the <ctype.h> classifiers are macros over a locale table)
int upper_or_lower(int c) {
  if ((*__ctype_b_loc())[c] & (256 | 512))
    return 1;
  return 0;
}

//--- tolower.c
#include <ctype.h>
// `tolower` is NOT a classifier and does not have the `table & MASK` shape:
// glibc declares it as a real function (and, under __USE_EXTERN_INLINES,
// wraps it in a statement expression that CALLS that function). It stays
// rejected, located, on the system-header front.
// TOLOWER: tolower.c:7:30: error: unsupported: call to 'tolower' declared in a system header; not part of the supported C subset
int lower_it(int c) { return tolower(c); }

//--- toupper.c
#include <ctype.h>
// TOUPPER: toupper.c:3:30: error: unsupported: call to 'toupper' declared in a system header; not part of the supported C subset
int upper_it(int c) { return toupper(c); }

//--- tolower-table.c
// The tolower TABLE read, hand-written (this is what the -O2 statement-
// expression expansion contains): a different table, no mask, and no
// admitted image -- the half-(a) wording still names it.
extern const int **__ctype_tolower_loc(void);
// TOLOWER-TABLE: tolower-table.c:7:7: warning: unsupported: locale ctype table lookup through '__ctype_tolower_loc' (the <ctype.h> classifiers are macros over a locale table)
int table_tolower(int c) {
  if ((*__ctype_tolower_loc())[c])
    return 1;
  return 0;
}

//--- setlocale.c
#include <ctype.h>
#include <locale.h>
// The C locale is what the ASCII equivalence was measured in. A translation
// unit that INSTALLS a locale invalidates it, so every classifier in the TU
// is fenced back to a located rejection -- including this one, which sits in
// a different function from the setlocale call and is a perfectly ordinary
// boolean context.
// SETLOCALE: setlocale.c:11:7: error: unsupported: <ctype.h> classifier in a translation unit that calls 'setlocale' (the ASCII image is measured only in the "C" locale)
// LEDGER: [ctype-locale] unsupported: <ctype.h> classifier in a translation unit that calls 'setlocale'
int fenced_space(int c) {
  if (isspace(c))
    return 1;
  return 0;
}
void install(void) { setlocale(LC_ALL, ""); }

//--- uselocale.c
#define _GNU_SOURCE
#include <ctype.h>
#include <locale.h>
// `uselocale` installs a locale for the calling thread; same fence.
// USELOCALE: uselocale.c:7:7: error: unsupported: <ctype.h> classifier in a translation unit that calls 'uselocale' (the ASCII image is measured only in the "C" locale)
int fenced_digit(int c) {
  if (isdigit(c))
    return 1;
  return 0;
}
void install_thread(locale_t l) { uselocale(l); }
