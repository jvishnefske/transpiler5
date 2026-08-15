// FR-85 boundaries: the const byte-region requirement admits ONLY the
// getter-only, loads-only image (multi-tu-external-requirement-const-
// byteregion.c). This file pins what stays REJECTED, with the wordings
// measured against the built importer:
//   * a NON-const byte-region extern -- a mutable byte region would need a
//     setter whose whole-region write the by-value model cannot license
//     while `&g` argument-passing also exists (the shared-borrow image and
//     the write image cannot both hold), so the arm is const-only and the
//     non-const extern keeps the generic located rejection;
//   * a STORE-shaped use -- casting away const and passing to a mutating
//     callee makes the pointer plan emit a copy-back `global_store`, which
//     is not a load, so the loads-only use scan refuses admission and the
//     generic rejection stands (nothing silently writes to a copy of
//     environment-owned storage);
//   * a const array with a NON-byte element type (`const int [4]`) -- the
//     gate is on the `ui8` element, not on array-ness; element access into
//     environment-owned non-byte storage still needs PLACES no trait item
//     yields (the FR-80 struct-array arm in multi-tu-external-requirement-
//     const-struct-negative.c pins the struct-element face of the same
//     boundary).
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/nonconst.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=NONCONST %s
// RUN: not emitrust-import-c --externals-trait %t/mutcast.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=MUTCAST %s
// RUN: not emitrust-import-c --externals-trait %t/intarray.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=INTARRAY %s

//--- empty.c
int unrelated(int v) { return v; }

//--- nonconst.c
// The NON-const byte-region extern: reads-only in this TU, but const-ness
// is what licenses the getter-only contract, so the generic rejection
// stands, located at the use.
typedef unsigned char u8_t;
struct eth_addr {
  u8_t addr[6];
} __attribute__((packed));
extern struct eth_addr cur;

int first(void) { return (int)cur.addr[0]; }
// NONCONST: nonconst.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'cur' is referenced but not defined in any translation unit

//--- mutcast.c
// Casting away const and handing the region to a mutating callee: the
// pointer plan's copy-back is a `global_store` symbol use, the loads-only
// scan refuses, and the generic rejection stands -- never a silent write
// into a staged copy.
typedef unsigned char u8_t;
struct eth_addr {
  u8_t addr[6];
} __attribute__((packed));
extern const struct eth_addr ethbroadcast;
void scribble(struct eth_addr *dst);

void use(void) { scribble((struct eth_addr *)&ethbroadcast); }

int first(void) { return (int)ethbroadcast.addr[0]; }
// MUTCAST: mutcast.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'ethbroadcast' is referenced but not defined in any translation unit

//--- intarray.c
// A const array of NON-byte elements: not a byte region, keeps the verbatim
// finalize rejection -- the frontier is decided on the element TYPE.
extern const int table[4];

int first(void) { return table[0]; }
// INTARRAY: intarray.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'table' is referenced but not defined in any translation unit
