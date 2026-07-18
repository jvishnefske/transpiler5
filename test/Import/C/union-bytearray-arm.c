// CTS-F (00210): a union mixing an integer arm with an integer-ARRAY arm
// of equal total width (uint16_t u; uint8_t b[2];) now ADMITS into the
// established one-slot struct model (B1): the slot is the INTEGER arm —
// its name and its mapped type — regardless of declaration order, and the
// byte-array arm's spelling never reaches the IR. __attribute__((packed))
// in either 00210 typedef position is tolerated and discarded. Accesses
// THROUGH the byte-array arm stay rejected at the access site, and
// unequal-total-width array arms keep the union family rejection (see
// union-bytearray-arm-invalid.c).
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/typedefs.c | FileCheck %s --check-prefix=TYPEDEFS
// RUN: emitrust-import-c %t/used.c | FileCheck %s --check-prefix=USED
// RUN: emitrust-import-c %t/order.c | FileCheck %s --check-prefix=ORDER

//--- typedefs.c
// The 00210 typedefs verbatim (packed after the body, packed after the
// union keyword) plus an unattributed spelling: all three admit with the
// integer slot even though no object of any of them is ever declared.
// The exact `["u"] [ui16]` pins exclude any second field.
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

typedef union Unaligned16a {
  uint16_t u;
  uint8_t b[2];
} __attribute__((packed)) Unaligned16a;

typedef union __attribute__((packed)) Unaligned16b {
  uint16_t u;
  uint8_t b[2];
} Unaligned16b;

typedef union Plain16 {
  uint16_t u;
  uint8_t b[2];
} Plain16;

int main(void) { return 0; }

// TYPEDEFS-DAG: emitrust.struct_def @{{([A-Za-z0-9_]+_)?}}Unaligned16a ["u"] [ui16]
// TYPEDEFS-DAG: emitrust.struct_def @{{([A-Za-z0-9_]+_)?}}Unaligned16b ["u"] [ui16]
// TYPEDEFS-DAG: emitrust.struct_def @{{([A-Za-z0-9_]+_)?}}Plain16 ["u"] [ui16]
// TYPEDEFS-LABEL: func.func @c_main
// TYPEDEFS-NOT: ["b"]

//--- used.c
// An object declared and used through the integer arm ONLY keeps the
// ordinary one-slot access model: every access selects the "u" slot.
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

union U16 {
  uint16_t u;
  uint8_t b[2];
};

int roundtrip(void) {
  union U16 x;
  x.u = 41;
  x.u = x.u + 1;
  return x.u;
}

// USED-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?U16]] ["u"] [ui16]
// USED-LABEL: func.func @roundtrip
// USED: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"[[U]]">>
// USED: emitrust.member %{{.*}}["u"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<ui16>
// USED-NOT: ["b"]

//--- order.c
// Declaration order does not pick the slot: with the byte-array arm
// FIRST, the slot is still the integer arm's name and type.
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

union Rev {
  uint8_t b[2];
  uint16_t u;
};

int read_rev(void) {
  union Rev x;
  x.u = 7;
  return x.u;
}

// ORDER-DAG: emitrust.struct_def @[[R:([A-Za-z0-9_]+_)?Rev]] ["u"] [ui16]
// ORDER-LABEL: func.func @read_rev
// ORDER: emitrust.member %{{.*}}["u"] : (!emitrust.lvalue<!emitrust.struct<"[[R]]">>) -> !emitrust.lvalue<ui16>
// ORDER-NOT: ["b"]
