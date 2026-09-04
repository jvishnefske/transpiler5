// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/string-push.cpp 2>&1 | FileCheck %s --check-prefix=PUSHADD
// RUN: not emitrust-import-c %t/string-pushback.cpp 2>&1 | FileCheck %s --check-prefix=PUSHBACK
// RUN: emitrust-import-c %t/string-runtime.cpp | FileCheck %s --check-prefix=GUARD

// FR-194, the THIRD entry point of the `(x as u8) as char` defect -- and the
// one that is NOT a rendering bug and therefore does NOT get a rendering fix.
//
// `printf("%c")` and `putchar` write bytes, so they were repaired by writing
// the raw byte (test/Import/C/printf-char-raw-bytes.c); `sprintf` formats
// into a byte buffer, so it was repaired by decoding the formatted string one
// `char` per byte. `std::string` is neither. A C++ `std::string` is a byte
// sequence that may hold ANY byte; a Rust `String` is UTF-8 BY INVARIANT and
// physically cannot hold a lone byte >= 0x80. Pushing the Latin-1 `char` for
// one stores its two-byte UTF-8 encoding, so `size()` (a `len()` over that
// encoding) reports 2 where C++ reports 1 and every subsequent byte offset
// shifts -- measured on `s += (char)0xc8`, where the native binary printed 1
// and the emitted crate printed 2. There is no alternative encoding inside
// the current model, so the only sound outcomes are rejection and a loud
// failure; silence-and-wrong is the option that is not available.
//
// This file pins the split. A CONSTANT operand >= 0x80 is decidable at
// import and gets the located rejection this repo prefers. A RUNTIME operand
// is not decidable there, and refusing every runtime push would refuse the
// entire ASCII world of character-at-a-time string building for the sake of
// a value that may never occur -- so it is decided AT THE PUSH instead, by
// `__emitrust_ascii_char`, whose assert aborts the crate rather than letting
// it store two bytes for one. Lateness is acceptable; silence is not. An
// ASCII constant is decided at import and keeps its unguarded emission, so
// the common `s += '!'` spelling is byte-identical to before (stl-string.cpp).

//--- string-push.cpp
#include <string>
// A Rust `String` is UTF-8 by invariant: it cannot hold a lone byte >= 0x80
// at all, so `s += '\xc8'` would store the TWO-byte UTF-8 encoding and
// `size()` would report 2 where C++ reports 1. That is a representation
// limit, not a rendering bug -- there is no encoding that fixes it -- so a
// constant operand outside 0..0x7f is a located rejection.
void push_high(std::string &s) { s += '\xc8'; }
// PUSHADD: string-push.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string::operator+= of a byte >= 0x80: a Rust String is UTF-8 and cannot hold it

//--- string-pushback.cpp
#include <string>
// The method spelling of the same store, rejected in the same words.
void pushback_high(std::string &s) { s.push_back('\xff'); }
// PUSHBACK: string-pushback.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string::push_back of a byte >= 0x80: a Rust String is UTF-8 and cannot hold it

//--- string-runtime.cpp
#include <string>
// A RUNTIME operand cannot be decided at import, and refusing every one of
// them would refuse the whole ASCII world of character-at-a-time string
// building for the sake of a value that may never occur. It is decided at
// the push instead: `__emitrust_ascii_char` aborts the crate rather than
// letting it store two bytes for one. Lateness is acceptable here; silence
// is what is not.
// GUARD-LABEL: func.func @push_runtime
void push_runtime(std::string &s, char c) {
  s += c;
  // GUARD: %[[G0:.*]] = emitrust.call_opaque "__emitrust_ascii_char"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // GUARD: emitrust.method_call %{{.*}}["push"] (%[[G0]])
  s.push_back(c);
  // GUARD: %[[G1:.*]] = emitrust.call_opaque "__emitrust_ascii_char"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // GUARD: emitrust.method_call %{{.*}}["push"] (%[[G1]])
}
// An ASCII CONSTANT needs no guard: it is decided at import and takes the
// plain Latin-1 encoder, so the common `s += '!'` spelling is unchanged.
// GUARD-LABEL: func.func @push_ascii_constant
void push_ascii_constant(std::string &s) {
  s += '!';
  // GUARD: emitrust.call_opaque "__emitrust_fmt_c"
}
// GUARD: emitrust.verbatim "fn __emitrust_ascii_char(x: i32) -> char {
// GUARD-SAME: assert!(b < 0x80
