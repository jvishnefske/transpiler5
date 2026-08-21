// FR-76 frontier: fn-ptr component admission covers the arithmetic-pointee
// scalar-pointer subset (plus, since FR-102, a COMPLETE-record pointee),
// and everything outside it keeps a LOCATED rejection — nothing may
// silently emit wrong code. The pinned shapes: a `void *` component (no
// consensus source in a solo-TU import), a variadic signature, a NESTED
// fn-ptr whose inner signature names a pointer (the recursion is
// explicitly gated: a pointer-free nested fn_ptr is a valid component, so
// only the with-pointer flavor may reject), and a signature MISMATCH at
// the binding site — here the CellSlice case: a function directly called
// with a mutable GLOBAL array classifies `ref<cell_slice<ui8>>`
// (CTS-P10), which WINS over the address-taken slice forcing (a
// Cell-backed global has no `&mut [u8]` to lend), so binding it to a
// slice-typed fn-ptr fails loudly at the assignment with the FR-29
// wording rather than unifying. All wordings were probed against the
// built tool, never guessed.
//
// The STRUCTPTR arm below is a MOVED FRONTIER, not a deleted one. It
// pinned `struct Ops { void (*f)(struct S *p); }` as rejecting under
// FR-76; FR-102 admits exactly that shape, mapping the component through
// the same `mapParamType` ScalarRef branch an ordinary struct-pointer
// parameter takes, so the pin advanced to the POSITIVE spelling. The
// frontier itself did not loosen: an INCOMPLETE pointee, a `void *`
// pointee and a pointer-to-pointer still reject verbatim (see
// fnptr-struct-components-invalid.c).
//
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/struct-ptr.c | FileCheck %s --check-prefix=STRUCTPTR
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
// STRUCTPTR: emitrust.struct_def @Ops ["f"] [!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"S">>)>]

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
