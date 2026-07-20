// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s --check-prefix=RUST

// C99-45 (c-testsuite 00218 uses a member named `type`): a struct member
// whose C spelling is a Rust keyword is no longer rejected; the importer
// mangles the member name deterministically by appending a single
// underscore (`type` -> `type_`, `match` -> `match_`). The mangled
// spelling is the member's identity everywhere: struct_def field lists,
// member accesses, and the emitted Rust field. Function names mangle the
// same way since CTS 00204 (keyword-fn-and-cursor.c); struct, enum, and
// global names that are Rust keywords stay rejected (keywords-invalid.c).

struct node {
  int type;
  int match;
};

int read_type(struct node n) {
  return n.type;
}

void bump(struct node *n) {
  n->match = n->match + n->type;
}

// CHECK: emitrust.struct_def @node ["type_", "match_"] [i32, i32]

// CHECK-LABEL: func.func @read_type
// CHECK: emitrust.member %{{.*}}["type_"] : (!emitrust.lvalue<!emitrust.struct<"node">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load
// CHECK: return

// CHECK-LABEL: func.func @bump
// CHECK: emitrust.deref
// CHECK-DAG: emitrust.member %{{.*}}["match_"]
// CHECK-DAG: emitrust.member %{{.*}}["type_"]
// CHECK: arith.addi
// CHECK: emitrust.assign

// The Rust rendering carries the mangled spellings verbatim.
// RUST: struct node {
// RUST-DAG: type_: i32,
// RUST-DAG: match_: i32,
// RUST: }
// RUST: .type_
