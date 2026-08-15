// FR-75: a REQUIREMENT-trait pointer parameter carries a REGION, not one
// element. A declaration-only `unsigned char *` parameter under a trait
// policy used to classify as a scalar reference (`!emitrust.mut_ref<ui8>`)
// with call sites borrowing `&mut b[0]` — ONE byte — so no Externals
// implementor could faithfully implement a helper that touches buf[1..].
// This pins the FR-75 contract: the requirement signature is a slice
// (`!emitrust.mut_ref<!emitrust.slice<T>>`, shared for the const-u8
// flavor per the CTS-BR/FR-55 rule), and every call site lowers its
// argument as a REGION view (`emitrust.slice_of [mut]` at the argument's
// cursor). Two input files because requirement MARKING happens in
// `finalizeProject`, which only the multi-TU project entry point runs.
//
// RUN: split-file %s %t
// RUN: emitrust-import-c --externals-trait %t/lib.c %t/other.c \
// RUN:   | FileCheck %s
//
// The declarations survive, body-less, slice-typed, carrying the marker.
// CHECK-DAG: func.func private @helper(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) attributes {emitrust.external_requirement}
// CHECK-DAG: func.func private @csum(!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32 attributes {emitrust.external_requirement}
//
// Call sites are REGION views at the argument's cursor: the whole array
// reslices from cursor 0, `b + 1` from cursor 1, and the const flavor
// borrows shared.
// CHECK-LABEL: func.func @use_it
// CHECK: emitrust.slice_of mut %{{.*}}[%c0_i64{{[_0-9]*}}] : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK-NEXT: call @helper(%{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) -> ()
// CHECK: %[[CUR:.*]] = arith.addi %{{.*}}, %{{.*}} : i64
// CHECK-NEXT: emitrust.slice_of mut %{{.*}}[%[[CUR]]] : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: call @helper(%{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) -> ()
// CHECK: emitrust.slice_of %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @csum(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32
//
// Without the flag the historical whole-program rejection is unchanged,
// still located at the first USE.
// RUN: not emitrust-import-c %t/lib.c %t/other.c 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT %s
// REJECT: lib.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'helper' is referenced but not defined in any translation unit
//
// OWNER-LIFT FORWARDING (the one shape the FR-75 spike could not execute
// pre-implementation): a defined wrapper whose own pointer parameter is
// owner-lifted (Phase 4: `self.data` + i64 cursor) and forwarded to a
// requirement must reslice the owner's member array AT the lifted cursor
// — a region view again, never `&mut self.data[p]`'s one element.
// RUN: emitrust-import-c --externals-trait %t/fwd.c %t/other.c \
// RUN:   | FileCheck --check-prefix=FWD %s
// FWD: func.func private @helper(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) attributes {emitrust.external_requirement}
// FWD: func.func @wrap(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_use_it_b">>, %arg1: i64, %arg2: ui32) attributes {emitrust.method_of = "Owner_use_it_b"
// FWD: %[[MEM:.*]] = emitrust.member %{{.*}}["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_use_it_b">>) -> !emitrust.lvalue<!emitrust.array<4xui8>>
// FWD: %[[CUR:.*]] = memref.load
// FWD-NEXT: emitrust.slice_of mut %[[MEM]][%[[CUR]]] : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// FWD-NEXT: call @helper(%{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) -> ()

//--- lib.c
void helper(unsigned char *buf, unsigned int len);
int csum(const unsigned char *k, unsigned int n);

int use_it(void) {
  unsigned char b[4] = {1, 2, 3, 4};
  helper(b, 4u);
  helper(b + 1, 3u);
  return csum(b, 4u) + (int)b[0];
}

//--- fwd.c
void helper(unsigned char *buf, unsigned int len);

void wrap(unsigned char *p, unsigned int n) { helper(p, n); }

int use_it(void) {
  unsigned char b[4] = {0, 0, 0, 0};
  wrap(b, 4u);
  return (int)b[1];
}

//--- other.c
// Defines nothing lib.c needs, so both prototypes stay undefined
// project-wide and become requirements.
int er_slice_dummy(int v) { return v + 1; }
