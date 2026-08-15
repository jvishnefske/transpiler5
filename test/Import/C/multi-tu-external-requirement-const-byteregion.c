// FR-85: a CONST BYTE-REGION extern global that no TU defines -- the lwIP
// `extern const struct eth_addr ethbroadcast` shape, where `struct eth_addr
// { u8_t addr[6]; }` imports as a plain `!emitrust.array<6xui8>` with NO
// struct_def -- is, like FR-79's const struct, a requirement on the
// ENVIRONMENT expressible as a GETTER-ONLY trait item `fn g() -> [u8; N]`.
// The pin's load-bearing fact is the importer's byte-region image: EVERY use
// of the global -- `&g` at an argument position included -- is a staged
// whole-value copy (`global_load` + temporary + slice_of/subscript), never a
// `global_addr`, so the by-value getter matches the emitted IR exactly and
// pointer identity was never preserved for byte-region globals to begin
// with. Because the MLIR-type key erases record vs record-array vs plain
// byte array (all map to `!emitrust.array<Nxui8>`), the admission
// deliberately covers all three; this file pins the record shape (the
// etharp residual) plus the plain byte array. FR-70's addressTakenGlobals
// disqualifier is bypassed for the arm -- the AST-level `&g` is real, but
// the lowered image carries no address -- while the surviving-use scan is
// LOADS-ONLY: any store-shaped or otherwise non-load use keeps the verbatim
// rejection (multi-tu-external-requirement-const-byteregion-negative.c).
//
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-empty.c | FileCheck %s
//
// The whole module is global_addr-free: the byte-region requirement's image
// is by-value copies, never a carried address (checked as its own prefix so
// the NOT spans the entire output, not just the tail).
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-empty.c | FileCheck --check-prefix=NOADDR %s
// NOADDR-NOT: emitrust.global_addr

typedef unsigned char u8_t;
struct eth_addr {
  u8_t addr[6];
} __attribute__((packed));
extern const struct eth_addr ethbroadcast;
extern const u8_t key[4];

// Body-less under a trait policy: a prospective requirement whose
// byte-region-pointee parameter is the CTS-BR shared byte slice.
// CHECK-DAG: func.func private @ethernet_output(!emitrust.ref<!emitrust.slice<ui8>>) attributes {emitrust.external_requirement}
void ethernet_output(const struct eth_addr *dst);

// `&g` at the argument position of the body-less callee: a staged copy of
// the whole region, then a shared byte slice of the TEMPORARY -- no
// global_addr anywhere in the module (pinned by the CHECK-NOT at the end).
// CHECK-LABEL: func.func @direct_arg
// CHECK: %[[T:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<6xui8>>
// CHECK: %[[L:.*]] = emitrust.global_load @ethbroadcast : !emitrust.array<6xui8>
// CHECK: emitrust.assign %[[T]] = %[[L]] : !emitrust.lvalue<!emitrust.array<6xui8>>
// CHECK: %[[S:.*]] = emitrust.slice_of %[[T]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<6xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @ethernet_output(%[[S]]) : (!emitrust.ref<!emitrust.slice<ui8>>) -> ()
int direct_arg(void) {
  ethernet_output(&ethbroadcast);
  return 0;
}

// The same address at an argument to a DEFINED static callee (the
// etharp_raw arm): identical staged-copy image, so admission does not
// depend on whether the callee has a body.
// CHECK-LABEL: func.func @defined_arg
// CHECK: emitrust.global_load @ethbroadcast : !emitrust.array<6xui8>
// CHECK: emitrust.slice_of
// CHECK: call @tu0_sum6
static int sum6(const struct eth_addr *p) {
  int s = 0;
  for (int i = 0; i < 6; ++i)
    s += p->addr[i];
  return s;
}
int defined_arg(void) { return sum6(&ethbroadcast); }

// A cast + local pointer bound to `&g` and passed on (the etharp
// `dest = (const struct eth_addr *)&ethbroadcast` flow): the pointer plan
// resolves it to the same staged copy at the call.
// CHECK-LABEL: func.func @via_local
// CHECK: emitrust.global_load @ethbroadcast : !emitrust.array<6xui8>
// CHECK: call @ethernet_output
int via_local(void) {
  const struct eth_addr *dest = (const struct eth_addr *)&ethbroadcast;
  ethernet_output(dest);
  return 0;
}

// Element read: whole-value load into a temporary, then subscript on the
// temporary -- never a projection into the (environment-owned) global.
// CHECK-LABEL: func.func @elem_read
// CHECK: emitrust.global_load @ethbroadcast : !emitrust.array<6xui8>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<6xui8>>, i64) -> !emitrust.lvalue<ui8>
int elem_read(void) { return (int)ethbroadcast.addr[0]; }

// The plain const byte ARRAY rides the same type key (array<4xui8>) and the
// same loads-only image, so it qualifies too.
// CHECK-LABEL: func.func @key_read
// CHECK: emitrust.global_load @key : !emitrust.array<4xui8>
int key_read(void) { return (int)key[1]; }

// Both declarations survive at module end with the const marker AND the
// requirement marker, declaration-only (no `<init>`); the lowering pass
// rewrites every load against the by-value getter, and a survivor fails
// loudly at emission.
// CHECK-DAG: emitrust.global const @ethbroadcast {emitrust.external_requirement} : !emitrust.array<6xui8>
// CHECK-DAG: emitrust.global const @key {emitrust.external_requirement} : !emitrust.array<4xui8>

// Without the flag the historical whole-program rejection is byte-for-byte
// unchanged, and still located at a USE (the first in the pending map's
// order -- measured: 'key').
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-empty.c 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT %s
// REJECT: multi-tu-external-requirement-const-byteregion.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'key' is referenced but not defined in any translation unit
