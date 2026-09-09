// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/bare-sizeof.c 2>&1 | FileCheck %s --check-prefix=BARE
// RUN: not emitrust-import-c %t/element-sizeof.c 2>&1 | FileCheck %s --check-prefix=ELEMENT
// RUN: not emitrust-import-c %t/deref-sizeof.c 2>&1 | FileCheck %s --check-prefix=DEREF
// RUN: not emitrust-import-c %t/alignof.c 2>&1 | FileCheck %s --check-prefix=ALIGN
// RUN: emitrust-import-c %t/elementsof.c | FileCheck %s --check-prefix=ELEMENTSOF

// FR-217 MISCOMPILE FENCE 2, and the idiom it must NOT break.
//
// A `const char *const` table keeps its C type after the padded lowering,
// so `sizeof(t)` still folds to N*sizeof(char *) -- a byte count that no
// longer describes anything in the emitted crate, whose storage is N*W.
// Any use that measures the table in BYTES (memcpy/memset/write extents,
// a manual byte walk) would be handed a number about a layout that is not
// there, so the fold refuses with a located diagnostic. `sizeof(t[0])` and
// `sizeof(*t)` are fenced on the same grounds: they promise
// sizeof(char *) where the emitted row is W bytes.
//
// The ELEMENTSOF idiom `sizeof(t)/sizeof(t[0])` is the common way such a
// table is walked, and it is PRESERVED EXACTLY -- the two sizeofs cancel,
// so the element count is the same before and after the change. It is
// folded to its constant ahead of the fence, and the last file pins that:
// rejecting it would gut the feature, which is why it is tested here
// beside the rejections rather than somewhere else.

// BARE: bare-sizeof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof of string table 'names' (its padded lowering has a different byte size; the element count is sizeof(t)/sizeof(t[0]))

//--- bare-sizeof.c
int printf(const char *, ...);
static const char *const names[3] = {"a", "bb", "ccc"};
int main(void) {
  printf("%d", (int)sizeof(names));
  return 0;
}

// ELEMENT: element-sizeof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof of string table 'names' (its padded lowering has a different byte size; the element count is sizeof(t)/sizeof(t[0]))

//--- element-sizeof.c
int printf(const char *, ...);
static const char *const names[3] = {"a", "bb", "ccc"};
int main(void) {
  printf("%d", (int)sizeof(names[0]));
  return 0;
}

// DEREF: deref-sizeof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof of string table 'names' (its padded lowering has a different byte size; the element count is sizeof(t)/sizeof(t[0]))

//--- deref-sizeof.c
int printf(const char *, ...);
static const char *const names[3] = {"a", "bb", "ccc"};
int main(void) {
  printf("%d", (int)sizeof(*names));
  return 0;
}

// _Alignof answers about the same absent layout.
// ALIGN: alignof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: alignof of string table 'names' (its padded lowering has a different byte size; the element count is sizeof(t)/sizeof(t[0]))

//--- alignof.c
int printf(const char *, ...);
static const char *const names[3] = {"a", "bb", "ccc"};
int main(void) {
  printf("%d", (int)_Alignof(names));
  return 0;
}

// The quotient survives, and folds to the element count directly -- no
// division op reaches the IR, because neither operand has a legal fold of
// its own.
// ELEMENTSOF-LABEL: func.func @c_main
// ELEMENTSOF: %[[N:.*]] = emitrust.constant <3 : ui64> : ui64
// ELEMENTSOF-NOT: emitrust.div
// ELEMENTSOF: emitrust.cast %[[N]] : ui64 to i32

//--- elementsof.c
int printf(const char *, ...);
static const char *const names[3] = {"a", "bb", "ccc"};
int main(void) {
  printf("%d", (int)(sizeof(names) / sizeof(names[0])));
  return 0;
}
