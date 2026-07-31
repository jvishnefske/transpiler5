// RUN: emitrust-import-c %s | FileCheck %s

// FR-55: the CTS-BR const rule in the SCALAR-REFERENCE position. A walked
// `const unsigned char *` parameter has always mapped to a shared byte
// slice (`!emitrust.ref<!emitrust.slice<ui8>>`); a deref-only one used to
// map to `!emitrust.mut_ref<ui8>` regardless of the const, and the
// disagreement between the two halves of one rule emitted Rust that does
// not compile: a caller holding the shared slice and handing ONE element to
// such a parameter had to reborrow a `&` as `&mut` (`error[E0596]`), which
// is the shape every `set_key(ctx, const uint8_t *k)` wrapper in real
// crypto code has. A deref-only `const unsigned char *` is now
// `!emitrust.ref<ui8>` and its argument borrow is shared to match.

int take_const_byte(const unsigned char *p);
int take_byte(unsigned char *p);
int take_const_int(const int *p);
// CHECK-DAG: func.func private @take_const_byte(!emitrust.ref<ui8>)
// CHECK-DAG: func.func private @take_byte(!emitrust.mut_ref<ui8>)
// CHECK-DAG: func.func private @take_const_int(!emitrust.mut_ref<i32>)

// The const byte pointee borrows shared, in the parameter AND in the
// element borrow the call site builds out of the shared slice.
int walk(const unsigned char *k, int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += take_const_byte(&k[i]);
  return t;
}
// CHECK-LABEL: func.func @walk
// CHECK-SAME: (%{{.*}}: !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: %[[ELT:.*]] = emitrust.subscript
// CHECK-NEXT: emitrust.addr_of %[[ELT]] : (!emitrust.lvalue<ui8>) -> !emitrust.ref<ui8>
// CHECK-NOT: addr_of mut

// The address-of form of the same argument (`&x` on a local) also borrows
// at the PARAMETER's mutability, not unconditionally mutably.
int local_arg(void) {
  unsigned char x = 3;
  unsigned char y = 4;
  int i = 5;
  return take_const_byte(&x) + take_byte(&y) + take_const_int(&i);
}
// CHECK-LABEL: func.func @local_arg
// CHECK: %[[X:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<ui8>) -> !emitrust.ref<ui8>
// CHECK-NEXT: call @take_const_byte(%[[X]])
// A non-const byte pointee is unchanged: still a mutable borrow.
// CHECK: %[[Y:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<ui8>) -> !emitrust.mut_ref<ui8>
// CHECK-NEXT: call @take_byte(%[[Y]])
// A const NON-byte pointee is deliberately unchanged too: its walked form
// is mutable as well, so the two halves of the rule already agree there and
// nothing forces a shared borrow.
// CHECK: %[[I:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
// CHECK-NEXT: call @take_const_int(%[[I]])

// The callee side of the same signature: the shared reference is
// dereferenced into a place and read exactly like the mutable one.
int deref_only(const unsigned char *p) { return (int)*p; }
// CHECK-LABEL: func.func @deref_only
// CHECK-SAME: (%[[P:.*]]: !emitrust.ref<ui8>)
// CHECK: emitrust.deref %[[P]] : (!emitrust.ref<ui8>) -> !emitrust.lvalue<ui8>
