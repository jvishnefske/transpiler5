// FR-62 (slice 1): the `AddressOfGlobal` edge kind. Pins the compatibility
// contract: taking a global's address KEEPS recording `ReadsGlobal` exactly
// as before AND additionally records `AddressOfGlobal`, so an existing
// consumer sees no change while a new one can compute "read-only,
// address-never-taken" as ReadsGlobal-minus-AddressOfGlobal. The shapes and
// what each must produce:
//  - a plain scalar read is `ReadsGlobal` only;
//  - `&g` is BOTH edges, from a function body and from a global initializer;
//  - array-to-pointer decay of a global array USED AS A VALUE (returned,
//    passed as an argument, or initializing a pointer global) is BOTH edges
//    -- the decayed pointer is the array's address escaping;
//  - a subscript READ `g[i]` is `ReadsGlobal` only: the decay in its base
//    feeds the subscript and the emitted Rust indexes the global directly,
//    so no address escapes;
//  - `&g[i]` (address of a SUBOBJECT) is BOTH edges -- the element address
//    escapes the global's storage;
//  - a compound assignment stays `ReadsGlobal` + `WritesGlobal`, no
//    `AddressOfGlobal`.
// The kind is appended to the enumeration, so it sorts LAST in each item's
// edge block and every pre-existing edge line is byte-identical.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

int watched;
int cell;
int table[4];
int grid[3];
int *tip = table;

int plain_read(void) { return watched; }

void bump(void) { watched += 2; }

int *take_address(void) { return &cell; }

int *decay_value(void) { return table; }

int subscript_read(int i) { return grid[i]; }

int *elem_address(void) { return &table[1]; }

int consume(int *p) { return p[0]; }

int pass_arg(void) { return consume(table); }

// CHECK:      node CELL kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:24:5
// CHECK-NEXT: node GRID kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:26:5
// CHECK-NEXT: node TABLE kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:25:5
// CHECK-NEXT: node TIP kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:27:6
// CHECK-NEXT: node WATCHED kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:23:5
// CHECK-NEXT: node bump kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:31:6
// CHECK-NEXT: node consume kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:41:5
// CHECK-NEXT: node decay_value kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:35:6
// CHECK-NEXT: node elem_address kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:39:6
// CHECK-NEXT: node pass_arg kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:43:5
// CHECK-NEXT: node plain_read kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:29:5
// CHECK-NEXT: node subscript_read kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:37:5
// CHECK-NEXT: node take_address kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-address-of.c:33:6

// A pointer global initialized by the decay of a global array both reads
// the array and takes its address, attributed to the GLOBAL's own node.
// CHECK-NEXT: edge TIP -> TABLE kind=ReadsGlobal
// CHECK-NEXT: edge TIP -> TABLE kind=AddressOfGlobal

// Compound assignment: read + write, and NO AddressOfGlobal (the final
// CHECK-NOT proves its absence, since bump's block would have to carry it).
// CHECK-NEXT: edge bump -> WATCHED kind=ReadsGlobal
// CHECK-NEXT: edge bump -> WATCHED kind=WritesGlobal

// CHECK-NEXT: edge decay_value -> TABLE kind=ReadsGlobal
// CHECK-NEXT: edge decay_value -> TABLE kind=AddressOfGlobal

// CHECK-NEXT: edge elem_address -> TABLE kind=ReadsGlobal
// CHECK-NEXT: edge elem_address -> TABLE kind=AddressOfGlobal

// Decay in argument position escapes the address exactly like a returned
// decay; the call edge itself is unchanged.
// CHECK-NEXT: edge pass_arg -> consume kind=Calls
// CHECK-NEXT: edge pass_arg -> TABLE kind=ReadsGlobal
// CHECK-NEXT: edge pass_arg -> TABLE kind=AddressOfGlobal

// CHECK-NEXT: edge plain_read -> WATCHED kind=ReadsGlobal

// A subscript read stays ReadsGlobal only: its base decay feeds the
// subscript, no pointer value survives it.
// CHECK-NEXT: edge subscript_read -> GRID kind=ReadsGlobal

// CHECK-NEXT: edge take_address -> CELL kind=ReadsGlobal
// CHECK-NEXT: edge take_address -> CELL kind=AddressOfGlobal
// CHECK-NOT:  edge
