// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conv.c 2>&1 | FileCheck %s --check-prefix=CONV
// RUN: not emitrust-import-c %t/keyword.c 2>&1 | FileCheck %s --check-prefix=KEYWORD

// Rust has no integer-to-enum cast, so any conversion with an enum
// destination other than an enumerator of that same enum is rejected; and
// enumerator spellings are emitted verbatim as Rust variant names, so a
// spelling that is a Rust keyword is rejected. Both diagnostics carry the
// file:line:col location.

// CONV: conv.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: integer to enum conversion
// KEYWORD: keyword.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator 'match' is a Rust keyword

//--- conv.c
enum Mode { Off, On };

int set(int raw) {
  enum Mode m = Off;
  m = raw;
  return m == On;
}

//--- keyword.c
enum Token { match, other };

int first(void) {
  return match;
}
