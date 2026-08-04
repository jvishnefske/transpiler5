// RUN: emitrust-import-c %s | FileCheck %s

// W2.3: STL recognition for std::string USAGE. `std::string` is a typedef
// for `std::basic_string<char, char_traits<char>, allocator<char>>`, so by
// the time `mapType`'s canonical-type RecordType branch sees it, it is
// already the `basic_string` specialization; `mapStdLibraryType` checks
// the first template argument is exactly `char` and maps it to
// `!emitrust.opaque<"String">` (a `basic_string` over any other character
// type is a located rejection, stl-invalid.cpp). Pins:
//   * `std::string s = "literal";` -> `String::from("literal")` — the
//     SAME `emitrust.literal` escaping (`emitRustStrLiteral`) printf's
//     `%s` string-literal shape already used, now shared by construction
//     too. libstdc++'s converting constructor also declares a DEFAULTED
//     allocator parameter, which `CXXConstructExpr` always fills with a
//     `CXXDefaultArgExpr` even though the user wrote one argument; this is
//     accepted (only argument 0 need be the literal).
//   * default construction (`std::string t;`) -> `String::new()`.
//   * `t += s` (`s2` a std::string) -> `.push_str(&s2)`: deref coercion
//     `&String` -> `&str` applies at the method-call argument position.
//   * `s += '!'` -> `.push(c as char)`, reusing the printf `%c` char
//     conversion helper (`wrapCharFormat`/`__emitrust_fmt_c`) — the ASCII-
//     only policy is shared, not reinvented.
//   * `s += "literal"` -> `.push_str("literal")` (no borrow needed for a
//     `&'static str` literal operand).
//   * size()/length() -> `.len()` then a cast to the call's own mapped
//     type (`size_t` -> `ui64`), exactly like std::vector::size().
//   * empty() -> `.is_empty()`.
//   * `s.c_str()` fed DIRECTLY to printf's `%s` -> a shared borrow of the
//     String place (`&s`), reusing the FILE*/%s machinery
//     (`emitPrintfStringArg`) — c_str() is recognized in NO OTHER
//     position (stl-invalid.cpp: a `c_str()` result assigned to a local is
//     rejected).
// `std::string::operator[]` (bytes indexing) is OUT this wave, so is every
// other method (see stl-invalid.cpp).

#include <string>

extern "C" int printf(const char *, ...);

int use_string(void) {
  std::string s = "hi";
  std::string t;
  t += s;
  s += '!';
  s += " there";
  int n = s.size();
  int e = t.empty();
  printf("%s\n", s.c_str());
  return n + e;
}

// CHECK-LABEL: func.func @use_string
// `std::string s = "hi";`: `String::from("hi")`.
// CHECK: %[[S:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.opaque<"String">>
// CHECK: %[[LIT:.*]] = emitrust.literal "\22hi\22" : !emitrust.opaque<"&'static str">
// CHECK: %[[FROM:.*]] = emitrust.call_opaque "String::from"(%[[LIT]]) : (!emitrust.opaque<"&'static str">) -> !emitrust.opaque<"String">
// CHECK: emitrust.assign %[[S]] = %[[FROM]] : !emitrust.lvalue<!emitrust.opaque<"String">>

// `std::string t;`: `String::new()`.
// CHECK: %[[T:.*]] = emitrust.variable named "t" : !emitrust.lvalue<!emitrust.opaque<"String">>
// CHECK: %[[NEW:.*]] = emitrust.call_opaque "String::new"() : () -> !emitrust.opaque<"String">
// CHECK: emitrust.assign %[[T]] = %[[NEW]] : !emitrust.lvalue<!emitrust.opaque<"String">>

// `t += s;`: a shared borrow of `s`, then `.push_str(&s)` on `t`.
// CHECK: %[[SREF:.*]] = emitrust.addr_of %[[S]] : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> !emitrust.ref<!emitrust.opaque<"String">>
// CHECK: emitrust.method_call %[[T]]["push_str"] (%[[SREF]]) : (!emitrust.lvalue<!emitrust.opaque<"String">>, !emitrust.ref<!emitrust.opaque<"String">>) -> ()

// `s += '!';`: the printf '%c' char conversion, then `.push(c as char)`.
// CHECK: %[[BANG:.*]] = arith.constant 33 : i8
// CHECK: %[[CHAR:.*]] = emitrust.call_opaque "__emitrust_fmt_c"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
// CHECK: emitrust.method_call %[[S]]["push"] (%[[CHAR]]) : (!emitrust.lvalue<!emitrust.opaque<"String">>, !emitrust.opaque<"char">) -> ()

// `s += " there";`: `.push_str("literal")`, no borrow.
// CHECK: %[[LIT2:.*]] = emitrust.literal {{.*}} : !emitrust.opaque<"&'static str">
// CHECK: emitrust.method_call %[[S]]["push_str"] (%[[LIT2]]) : (!emitrust.lvalue<!emitrust.opaque<"String">>, !emitrust.opaque<"&'static str">) -> ()

// size(): `.len()` -> index, cast to the call's own mapped type (ui64).
// CHECK: %[[LEN:.*]] = emitrust.method_call %[[S]]["len"] () : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> index
// CHECK: emitrust.cast %[[LEN]] : index to ui64

// empty(): `.is_empty()` -> i1.
// CHECK: emitrust.method_call %[[T]]["is_empty"] () : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> i1

// `s.c_str()` fed to printf '%s': a shared borrow of `s`, no helper call.
// CHECK: %[[CSTRREF:.*]] = emitrust.addr_of %[[S]] : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> !emitrust.ref<!emitrust.opaque<"String">>
// CHECK: emitrust.call_opaque "println!"(%[[CSTRREF]])
