// RealWorld C++ corpus (FR-46), project `tokenizer`: driver TU.
//
// Exercises: two classes with constructors (member-initializer list and a
// body loop over an array member), mutating and `const` methods called across
// a TU boundary, a method call on a local object, a local `char` array
// initialized from a string literal, and an `enum` shared through a header.
// No virtual, no inheritance, no reference, no container, no template --
// the intended difficulty step above `fixed-stats`.
#include <cstdio>

#include "histogram.hpp"
#include "lexer.hpp"

int main(void) {
  const char text[] = "set gain=12, offset=-3; retry_count 4 times (max 250)";

  Lexer lex;
  Histogram hist;

  int run = 0;
  for (int i = 0; text[i] != '\0'; i = i + 1) {
    char c = text[i];
    lex.feed(c);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      run = run + 1;
    } else {
      if (run > 0)
        hist.add(run);
      run = 0;
    }
  }
  lex.finish();
  if (run > 0)
    hist.add(run);

  printf("words=%d numbers=%d punct=%d longest=%d\n", lex.words(),
         lex.numbers(), lex.punct(), lex.longest_word());
  printf("hist total=%d peak=%d bin3=%d bin7=%d\n", hist.total(),
         hist.peak_bin(), hist.bin(3), hist.bin(7));
  return 0;
}
