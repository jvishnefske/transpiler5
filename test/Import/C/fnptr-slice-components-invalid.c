// FR-76 frontier: fn-ptr component admission covers exactly the
// arithmetic-pointee scalar-pointer subset, and everything outside it
// keeps a LOCATED rejection — nothing may silently emit wrong code. The
// pinned shapes: a component naming a STRUCT pointer, a `void *`
// component (no consensus source in a solo-TU import), a variadic
// signature, a NESTED fn-ptr whose inner signature names a pointer (the
// recursion is explicitly gated: a pointer-free nested fn_ptr is a valid
// component, so only the with-pointer flavor may reject), and a
// signature MISMATCH at the binding site — here the CellSlice case: a
// function directly called with a mutable GLOBAL array classifies
// `ref<cell_slice<ui8>>` (CTS-P10), which WINS over the address-taken
// slice forcing (a Cell-backed global has no `&mut [u8]` to lend), so
// binding it to a slice-typed fn-ptr fails loudly at the assignment with
// the FR-29 wording rather than unifying. All wordings were probed
// against the built tool, never guessed.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/struct-ptr.c 2>&1 | FileCheck %s --check-prefix=STRUCTPTR
// RUN: not emitrust-import-c %t/void-component.c 2>&1 | FileCheck %s --check-prefix=VOIDCOMP
// RUN: not emitrust-import-c %t/variadic.c 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/nested.c 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not emitrust-import-c %t/cell-slice-mismatch.c 2>&1 | FileCheck %s --check-prefix=MISMATCH

//--- struct-ptr.c
struct S { int x; };
struct Ops { void (*f)(struct S *p); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// STRUCTPTR: struct-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- void-component.c
struct Ops { void (*g)(void *p, unsigned long n); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// VOIDCOMP: void-component.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- variadic.c
struct Ops { int (*h)(const char *fmt, ...); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// VARIADIC: variadic.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function pointer type

//--- nested.c
struct Ops { void (*k)(void (*inner)(unsigned char *b, unsigned n)); };
int main(void) {
  struct Ops o;
  (void)o;
  return 0;
}
// NESTED: nested.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position

//--- cell-slice-mismatch.c
unsigned char g[4];
void poke(unsigned char *buf, unsigned len) {
  for (unsigned i = 0; i < len; i++)
    buf[i] = (unsigned char)(i + len);
}
int main(void) {
  poke(g, 4u);
  void (*fp)(unsigned char *, unsigned) = poke;
  fp(g, 4u);
  return (int)g[0];
}
// MISMATCH: cell-slice-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'poke' does not match the function pointer signature
