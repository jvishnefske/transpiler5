// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/utf16.c 2>&1 | FileCheck %s --check-prefix=UTF16
// RUN: not emitrust-import-c %t/utf32.c 2>&1 | FileCheck %s --check-prefix=UTF32
// RUN: not emitrust-import-c %t/multichar.c 2>&1 | FileCheck %s --check-prefix=MULTI
// RUN: not emitrust-import-c %t/nonascii.c 2>&1 | FileCheck %s --check-prefix=NONASCII

// C99-4 boundaries: ordinary and wide character constants are in the
// supported subset (both are int-typed rvalues on this target); Unicode
// constants carry charN_t types outside the model, and multi-character
// constants have implementation-defined values; each rejected shape
// keeps a located diagnostic.

//--- utf16.c
int main(void) { return u'a'; }
// UTF16: utf16.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: Unicode character constant

//--- utf32.c
int main(void) { return U'a'; }
// UTF32: utf32.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: Unicode character constant

//--- multichar.c
int main(void) { return 'ab'; }
// MULTI: multichar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: multi-character constant

//--- nonascii.c
// A multibyte source character ('é' is two UTF-8 bytes) is rejected by
// the clang frontend itself before import ("character too large for
// enclosing character literal type"), so no non-ASCII byte ever reaches
// the importer through an ordinary character constant.
int main(void) { return 'é'; }
// NONASCII: nonascii.c:{{[0-9]+}}:{{[0-9]+}}: error: character too large for enclosing character literal type
