// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/volatile-local.c 2>&1 | FileCheck %s --check-prefix=VOLLOCAL
// RUN: not emitrust-import-c %t/volatile-global.c 2>&1 | FileCheck %s --check-prefix=VOLGLOBAL
// RUN: not emitrust-import-c %t/volatile-param-pointee.c 2>&1 | FileCheck %s --check-prefix=VOLPARAM
// RUN: not emitrust-import-c %t/volatile-field.c 2>&1 | FileCheck %s --check-prefix=VOLFIELD
// RUN: not emitrust-import-c %t/volatile-pointer-itself.c 2>&1 | FileCheck %s --check-prefix=VOLPTR
// RUN: not emitrust-import-c %t/volatile-return.c 2>&1 | FileCheck %s --check-prefix=VOLRET
// RUN: not emitrust-import-c %t/volatile-cast.c 2>&1 | FileCheck %s --check-prefix=VOLCAST
// RUN: not emitrust-import-c %t/atomic-local.c 2>&1 | FileCheck %s --check-prefix=ATOMICLOCAL
// RUN: not emitrust-import-c %t/atomic-global.c 2>&1 | FileCheck %s --check-prefix=ATOMICGLOBAL

// C99-7 type qualifiers, rejected side. volatile has no counterpart in
// the emitted single-threaded Rust model (no MMIO, no signal handlers,
// no setjmp), so every position where a volatile-qualified type reaches
// the importer — locals, globals, parameters (pointee or the pointer
// itself), struct fields, return types — is a located rejection rather
// than a silent qualifier drop. A cast that introduces volatile refuses
// the CTS-P2 qualification peel, so the site keeps a located rejection
// too. _Atomic is likewise rejected with a precise message.

//--- volatile-local.c
int main(void) {
  volatile int x = 1;
  return x;
}
// VOLLOCAL: volatile-local.c:2:16: error: unsupported: volatile-qualified type

//--- volatile-global.c
volatile int g;
int main(void) { return g; }
// VOLGLOBAL: volatile-global.c:1:14: error: unsupported: volatile-qualified type

//--- volatile-param-pointee.c
int f(volatile int *p) { return *p; }
int main(void) {
  int x = 2;
  return f(&x);
}
// VOLPARAM: volatile-param-pointee.c:1:21: error: unsupported: volatile-qualified type

//--- volatile-field.c
struct S {
  volatile int v;
};
int main(void) {
  struct S s;
  s.v = 1;
  return s.v;
}
// VOLFIELD: volatile-field.c:2:16: error: unsupported: volatile-qualified type

//--- volatile-pointer-itself.c
int main(void) {
  int x = 3;
  int *volatile p = &x;
  return *p;
}
// VOLPTR: volatile-pointer-itself.c:3:17: error: unsupported: volatile-qualified type

//--- volatile-return.c
volatile int f(void) { return 4; }
int main(void) { return f(); }
// VOLRET: volatile-return.c:1:14: error: unsupported: volatile-qualified type

//--- volatile-cast.c
int main(void) {
  int x = 1;
  *(volatile int *)&x = 5;
  return x;
}
// VOLCAST: volatile-cast.c:3:4: error: unsupported pointer expression

//--- atomic-local.c
int main(void) {
  _Atomic int x = 1;
  return x;
}
// ATOMICLOCAL: atomic-local.c:2:15: error: unsupported: _Atomic-qualified type

//--- atomic-global.c
_Atomic int g;
int main(void) { return g; }
// ATOMICGLOBAL: atomic-global.c:1:13: error: unsupported: _Atomic-qualified type
