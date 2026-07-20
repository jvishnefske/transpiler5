// REQUIRES: cargo
// W2.3: std::string recognition, end to end. The real oracle for the
// string half of the wave: builds a Rust crate through the full pipeline
// and diffs its stdout against a `clang++ -std=c++17` build of the
// identical source, byte for byte. Exercises: the string-literal
// conversion constructor, default construction, `+=` over another
// std::string, `+=` over a char, `+=` over a string literal, size(),
// empty(), and c_str() fed directly to printf's '%s' — across two
// independently-parameterized calls (distinct literal/char arguments at
// each call site), so a wiring bug would surface as a mismatch. `+=` over
// a runtime (non-literal) `const char*` is out of this wave's recognized
// set (design.md's STL OUT list), so both calls append literals, not a
// parameter.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_string > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <string>

int use_string_a(void) {
  std::string s = "hello";
  std::string t;
  t += s;
  t += '!';
  t += " world";
  int n = t.size();
  int e = t.empty();
  int e2 = s.empty();
  printf("%s (%d %d %d)\n", t.c_str(), n, e, e2);
  return n;
}

int use_string_b(void) {
  std::string s = "greetings";
  std::string t;
  t += s;
  t += ',';
  t += " there";
  int n = t.size();
  int e = t.empty();
  int e2 = s.empty();
  printf("%s (%d %d %d)\n", t.c_str(), n, e, e2);
  return n;
}

int main(void) {
  int a = use_string_a();
  int b = use_string_b();
  printf("%d %d\n", a, b);
  return 0;
}
