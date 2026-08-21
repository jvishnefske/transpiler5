// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/elem-write.c 2>&1 | FileCheck %s --check-prefix=WRITE
// RUN: not emitrust-import-c %t/elem-read.c 2>&1 | FileCheck %s --check-prefix=READ
// RUN: not emitrust-import-c %t/elem-deref.c 2>&1 | FileCheck %s --check-prefix=DEREF
// RUN: not emitrust-import-c %t/elem-arrow.c 2>&1 | FileCheck %s --check-prefix=ARROW
// RUN: not emitrust-import-c %t/elem-nonconst.c 2>&1 | FileCheck %s --check-prefix=NONCONST
// RUN: not emitrust-import-c %t/elem-addrof.c 2>&1 | FileCheck %s --check-prefix=ADDROF
// RUN: not emitrust-import-c %t/elem-arg.c 2>&1 | FileCheck %s --check-prefix=ARG
// RUN: not emitrust-import-c %t/whole-member-arg.c 2>&1 | FileCheck %s --check-prefix=WHOLEARG
// RUN: not emitrust-import-c %t/elem-arith.c 2>&1 | FileCheck %s --check-prefix=ARITH
// RUN: not emitrust-import-c %t/elem-incdec.c 2>&1 | FileCheck %s --check-prefix=INCDEC
// RUN: not emitrust-import-c %t/elem-return.c 2>&1 | FileCheck %s --check-prefix=RETURN
// RUN: not emitrust-import-c %t/elem-nulltest.c 2>&1 | FileCheck %s --check-prefix=NULLTEST
// RUN: not emitrust-import-c %t/elem-booltest.c 2>&1 | FileCheck %s --check-prefix=BOOLTEST
// RUN: not emitrust-import-c %t/elem-compare.c 2>&1 | FileCheck %s --check-prefix=COMPARE
// RUN: not emitrust-import-c %t/elem-toint.c 2>&1 | FileCheck %s --check-prefix=TOINT
// RUN: not emitrust-import-c %t/elem-printp.c 2>&1 | FileCheck %s --check-prefix=PRINTP
// RUN: not emitrust-import-c %t/elem-printstr.c 2>&1 | FileCheck %s --check-prefix=PRINTSTR
// RUN: not emitrust-import-c %t/elem-callfn.c 2>&1 | FileCheck %s --check-prefix=CALLFN
// RUN: not emitrust-import-c %t/memset-member.c 2>&1 | FileCheck %s --check-prefix=MEMSETMEMBER
// RUN: not emitrust-import-c %t/memset-record.c 2>&1 | FileCheck %s --check-prefix=MEMSETRECORD
// RUN: not emitrust-import-c %t/struct-copy.c 2>&1 | FileCheck %s --check-prefix=COPY
// RUN: not emitrust-import-c %t/global-read.c 2>&1 | FileCheck %s --check-prefix=GLOBALREAD
// RUN: not emitrust-import-c %t/local-addr-init.c 2>&1 | FileCheck %s --check-prefix=LOCALADDRINIT
// RUN: not emitrust-import-c %t/local-scalar-init.c 2>&1 | FileCheck %s --check-prefix=LOCALSCALARINIT

// FR-107 admits the TYPE of a pointer-ARRAY struct member (`T *m[N]` is
// `[i64; N]`, see pointers-member-array.c) and NOTHING ELSE. Rejection is
// a feature: the whole design rests on the invariant that no element USE
// may reach emission, because no element is ever bound to a target object
// and the stored slots are all-zero regardless of what the source wrote.
// If any arm below stopped rejecting, the emitted crate would read a
// literal 0 as if it were a pointer — a silent miscompile.
//
// Each file pins one element-use shape, with the EXACT wording the built
// tool produces today. Several land on pre-existing residual rejections
// rather than an FR-107-specific message; that is deliberate — this file
// exists to prove they are REACHED, so a later change that routes an
// element use somewhere else cannot lose the diagnostic silently.
//
// The GLOBALREAD arm is the load-bearing one. `struct H g = {{&a,&b}}`
// IMPORTS, folding both elements to literal 0 with no binding recorded
// (pinned in pointers-member-array.c). That image is only correct
// because reading it back rejects; this arm is the other half of that
// pair and must never be weakened.

//--- elem-write.c
// Storing an address into a slot: nothing records the binding, so the
// assignment must not be dropped.
struct H { int *m[4]; };
struct H g;
int a = 1;
int main(void) { g.m[0] = &a; return 0; }
// WRITE: elem-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: assignment to this pointer expression

//--- elem-read.c
// Copying a slot out into a pointer local.
struct H { int *m[4]; };
struct H g;
int main(void) { int *p = g.m[1]; return *p; }
// READ: elem-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- elem-deref.c
// The direct dereference of a constant-index element.
struct H { int *m[4]; };
struct H g;
int main(void) { return *g.m[0]; }
// DEREF: elem-deref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-arrow.c
// The same through a struct-POINTER instance path, which the per-instance
// model cannot key at all.
struct H { int *m[4]; };
int f(struct H *h) { return *h->m[0]; }
// ARROW: elem-arrow.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-nonconst.c
// A NON-CONSTANT index selects a target SET, not a target: exactly the
// shape lwIP's `lwip_stats.memp[i]` writes, and it must stay loud.
struct H { int *m[4]; };
struct H g;
int main(int argc, char **argv) { return *g.m[argc]; }
// NONCONST: elem-nonconst.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-addrof.c
// Taking the address OF a slot hands out a pointer to the inert i64.
struct H { int *m[4]; };
struct H g;
int main(void) { int **q = &g.m[0]; return **q; }
// ADDROF: elem-addrof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer variable assigned a value that is not the address of a local pointer variable

//--- elem-arg.c
// Passing a slot as a pointer argument.
struct H { int *m[4]; };
struct H g;
int use(int *p) { return *p; }
int main(void) { return use(g.m[0]); }
// ARG: elem-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- whole-member-arg.c
// Passing the WHOLE slot run as an `int **`: the decay hands out the
// address of a run of inert i64s.
struct H { int *m[4]; };
struct H g;
int use(int **p) { return **p; }
int main(void) { return use(g.m); }
// WHOLEARG: whole-member-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a cursor argument must be the address of a decomposed pointer local

//--- elem-arith.c
// Pointer arithmetic on a slot value.
struct H { int *m[4]; };
struct H g;
int main(void) { return *(g.m[0] + 1); }
// ARITH: elem-arith.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-incdec.c
// Incrementing a slot in place.
struct H { int *m[4]; };
struct H g;
int main(void) { g.m[0]++; return 0; }
// INCDEC: elem-incdec.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ++/-- on this pointer expression

//--- elem-return.c
// Returning a slot value.
struct H { int *m[4]; };
struct H g;
int *f(void) { return g.m[0]; }
// RETURN: elem-return.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- elem-nulltest.c
// A null test would read `0` and answer FALSE for a slot the source set
// to a real address — the exact silent-miscompile this file guards.
struct H { int *m[4]; };
struct H g;
int main(void) { return g.m[0] != 0; }
// NULLTEST: elem-nulltest.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-booltest.c
// The same in condition position.
struct H { int *m[4]; };
struct H g;
int main(void) { if (g.m[0]) return 1; return 0; }
// BOOLTEST: elem-booltest.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-compare.c
// Comparing two slots would answer EQUAL for two different targets.
struct H { int *m[4]; };
struct H g;
int main(void) { return g.m[0] == g.m[1]; }
// COMPARE: elem-compare.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- elem-toint.c
// Converting a slot to an integer would expose the cursor encoding.
struct H { int *m[4]; };
struct H g;
int main(void) { return (int)(long)g.m[0]; }
// TOINT: elem-toint.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (PointerToIntegral)

//--- elem-printp.c
// Printing a slot with %p.
int printf(const char *, ...);
struct H { void *m[4]; };
struct H g;
int main(void) { printf("%p\n", g.m[0]); return 0; }
// PRINTP: elem-printp.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf format specifier '%p'

//--- elem-printstr.c
// A `const char *` slot run is not a string region either.
int puts(const char *);
struct H { const char *n[2]; };
struct H g;
int main(void) { puts(g.n[0]); return 0; }
// PRINTSTR: elem-printstr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: printf '%s' argument must be a string literal or a char array

//--- elem-callfn.c
// Calling a `void *` slot through a function-pointer cast.
struct H { void *m[4]; };
struct H g;
int main(void) { ((void (*)(void))g.m[0])(); return 0; }
// CALLFN: elem-callfn.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (BitCast)

//--- memset-member.c
// A whole-member byte splat over the slot run — lwIP's
// `memset(netif->client_data, 0, sizeof(netif->client_data))` shape. The
// FR-97 typed-splat image must not claim a run of cursor slots.
#include <string.h>
struct H { int *m[4]; int n; };
struct H g;
int main(void) { memset(g.m, 0, sizeof(g.m)); return g.n; }
// MEMSETMEMBER: memset-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- memset-record.c
// A byte splat over the WHOLE record, which spans the slot run.
#include <string.h>
struct H { int *m[4]; int n; };
struct H g;
int main(void) { memset(&g, 0, sizeof(g)); return g.n; }
// MEMSETRECORD: memset-record.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object is not a string region

//--- struct-copy.c
// A whole-record copy carries no per-instance facts; the copy's slots are
// as unresolvable as the source's.
struct H { int *m[2]; };
int main(void) { struct H s = {0}; struct H d = s; return *d.m[0]; }
// COPY: struct-copy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- global-read.c
// THE LOAD-BEARING ARM. This global imports (pointers-member-array.c pins
// the folded `[0, 0]` image for exactly this initializer); reading an
// element of it back must reject, because the fold discarded `&a`.
struct H { int *m[2]; int n; };
int a = 1, b = 2;
struct H g = { { &a, &b }, 7 };
int main(void) { return *g.m[0]; }
// GLOBALREAD: global-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (LValueToRValue)

//--- local-addr-init.c
// A LOCAL brace initializer naming an address: FR-107 accepts only an
// all-null element list for the slot run (the `{0}` real declarations
// write), and an address element is a binding nothing records.
struct H { int *m[2]; int n; };
int main(void) { int a = 1; struct H l = { { &a }, 7 }; return l.n; }
// LOCALADDRINIT: local-addr-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-array struct member initializer

//--- local-scalar-init.c
// A non-list initializer for the slot run (here a compound literal) is
// outside the accepted all-null brace shape and names the position.
struct H { int *m[2]; int n; };
int a = 1;
int main(void) { struct H l = { (int *[2]){ &a, 0 }, 7 }; return l.n; }
// LOCALSCALARINIT: local-scalar-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-array struct member initializer
