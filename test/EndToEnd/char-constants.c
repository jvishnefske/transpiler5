// REQUIRES: cargo
// C99-4: differential regression test for the plain-char signedness
// policy, character constants, and escape sequences. Plain char is
// signed (the x86-64 Linux / clang default): '\xff' is the int -1 and a
// char holding it prints as -1 through %d. Character constants cover
// simple escapes, octal, and hex, in expressions, comparisons, switch
// case labels, and array subscripts; string literals exercise every
// standard escape the ASCII policy admits. Byte-identical stdout and
// exit codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name char_constants_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/char_constants_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int puts(const char *);
int putchar(int);

int classify(char c) {
  switch (c) {
  case 'a':
    return 1;
  case '\n':
    return 2;
  case '\0':
    return 3;
  case '\x7f':
    return 4;
  default:
    return 0;
  }
}

int main(void) {
  // Character constants have type int; print their evaluated values.
  printf("%d %d %d %d %d %d %d %d %d\n", 'a', '\n', '\t', '\0', '\\',
         '\'', '\"', '\012', '\x41');
  // Signed plain-char policy: '\xff' is -1 as an int constant, a char
  // object holding it prints -1 through the int promotion, and it
  // compares below zero and below 'A'.
  printf("%d\n", '\xff');
  char hi = '\xff';
  printf("%d %d %d\n", hi, hi < 0, hi < 'A');
  // More boundary bytes: '\x7f' stays positive, '\x80' wraps negative.
  char top = '\x7f';
  char wrap = '\x80';
  printf("%d %d\n", top, wrap);
  // Constants in arithmetic and comparisons.
  char c = 'q';
  printf("%d %d %d %d\n", c == 'q', c != 'q', 'z' - 'a', 'a' + 1);
  // Constants as switch scrutinee values and case labels.
  printf("%d %d %d %d %d\n", classify('a'), classify('\n'), classify(0),
         classify('\x7f'), classify('b'));
  // Constants as array subscripts.
  int table[4] = {10, 20, 30, 40};
  printf("%d %d\n", table['b' - 'a'], table['d' - 'a']);
  // Every standard string-literal escape the ASCII policy admits,
  // printed via %s and puts (\a \b \f \v \r included; they are bytes
  // like any other and must round-trip identically).
  printf("%s|\n", "tab\there");
  printf("%s|\n", "q\'q d\"d b\\b");
  printf("%s|\n", "oct\101 hex\x42 nul-next");
  char esc[] = "A\aB\bC\fD\vE\rF";
  printf("%s|\n", esc);
  puts("line1\nline2");
  // Character constants through putchar (%c-style byte output).
  putchar('\x41');
  putchar('0' + 5);
  putchar('\t');
  putchar('!');
  putchar('\n');
  return classify('\0') - 3;
}
