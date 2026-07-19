// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/string-init.c 2>&1 | FileCheck %s --check-prefix=STRING
// RUN: not emitrust-import-c %t/enum-global.c 2>&1 | FileCheck %s --check-prefix=ENUMGLOBAL
// RUN: not emitrust-import-c %t/non-constant.c 2>&1 | FileCheck %s --check-prefix=NONCONST

// C99-11 boundaries: each file below exercises one located rejection around
// aggregate initializer lists. (Multi-dimensional arrays are supported
// since CTS-S4; see arrays-multidim.c. Compound-literal initializers are
// supported since C99-13; see compound-literals.c.)

// `char s[] = "..."` is supported (see strings.c), but only for plain and
// signed char arrays: an unsigned char array maps to u8 elements, which
// the string-init lowering does not cover.
//--- string-init.c
int main(void) {
  unsigned char s[4] = "abc";
  return s[0];
}
// STRING: string-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string literal initializer for this type

// An enum-typed global element has no typed-attribute representation.
//--- enum-global.c
enum E { RED, GREEN };
enum E palette[2] = {GREEN, RED};
int main(void) { return palette[0] == GREEN; }
// ENUMGLOBAL: enum-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global initializer for this type

// A non-constant element in a file-scope list is rejected by clang's own
// constant-initializer check (C11 6.7.9p4) before import.
//--- non-constant.c
int base;
int table[2] = {(int)(long)&base, 1};
int main(void) { return table[1]; }
// NONCONST: non-constant.c:{{[0-9]+}}:{{[0-9]+}}: error: initializer element is not a compile-time constant
