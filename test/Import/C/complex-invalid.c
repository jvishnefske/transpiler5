// C99-9: `_Complex` and `_Imaginary` are documented rejections — no Rust
// counterpart exists. `_Complex` reaches the importer's type mapper and is
// rejected with a located diagnostic naming the type; `_Imaginary` (the
// optional C99 Annex G type clang has never implemented) is a located
// rejection from the clang frontend before import even begins. Either
// way: a build-time error, never silent acceptance.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/complex.c 2>&1 | FileCheck %s --check-prefix=COMPLEX
// RUN: not emitrust-import-c %t/imaginary.c 2>&1 | FileCheck %s --check-prefix=IMAGINARY

//--- complex.c
void f(void) {
  _Complex double z;
  (void)z;
}
// COMPLEX: complex.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported type '_Complex double'

//--- imaginary.c
void g(void) {
  _Imaginary double z;
  (void)z;
}
// IMAGINARY: imaginary.c:{{[0-9]+}}:{{[0-9]+}}: error: imaginary types are not supported
