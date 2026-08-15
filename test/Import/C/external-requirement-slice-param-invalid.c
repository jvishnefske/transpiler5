// FR-75 frontier: a requirement-trait slice parameter needs a REGION view
// behind every call-site argument. A call site that cannot produce one —
// the address of a scalar object, whether the parameter is spelled
// `unsigned char *` or a consensus-admitted `void *` — keeps a LOCATED
// rejection, never a silent one-element borrow. NOTE the first case is a
// DELIBERATE coverage regression recorded in the FR-75 entry: before
// FR-75 the trait mode accepted `helper(&x, 1)` as a one-byte `&mut x`,
// which misrepresented the C region contract (fidelity over coverage).
// Two input files because only the PROJECT entry point applies the trait
// policy (a solo emitrust-import-c run keeps the historical import).
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/noregion.c %t/other.c 2>&1 \
// RUN:   | FileCheck --check-prefix=NOREGION %s
// RUN: not emitrust-import-c --externals-trait %t/voidscalar.c %t/other.c 2>&1 \
// RUN:   | FileCheck --check-prefix=VOIDSCALAR %s

// NOREGION: noregion.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- noregion.c
void helper(unsigned char *buf, unsigned int len);

int use_it(void) {
  unsigned char x = 7;
  helper(&x, 1u);
  return (int)x;
}

// The void* consensus scan admits `&x` as a byte VIEW (its type is a byte
// pointer), but the argument still has no region to reslice, so the
// call-site lowering keeps the same located rejection — over-admission is
// safe because nothing downstream can silently borrow one element.
// VOIDSCALAR: voidscalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- voidscalar.c
void vset(void *dst, unsigned char c, unsigned int n);

int use_it(void) {
  unsigned char x = 7;
  vset(&x, 0, 1u);
  return (int)x;
}

//--- other.c
// Defines nothing the first TU needs; present only so the run takes the
// project (trait-policy) path.
int er_invalid_dummy(int v) { return v + 1; }
