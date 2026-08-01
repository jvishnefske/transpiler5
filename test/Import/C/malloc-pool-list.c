// RUN: emitrust-import-c %s | FileCheck %s

// W4.2e Part B (FR-39): the RFC index-handle node pool. A singly-linked
// list built from malloc'd nodes inside a foldable-trip-count loop, whose
// self-referential `next` field never escapes and is only null-checked,
// promotes to a fixed [Node; CAP] pool + free cursor. Each node pointer is
// a nullable pool index handle (an i64 index cell + an i1 non-null cell);
// the `next` field renders as `Option<usize>`, built and destructured
// through the __emitrust_pool_* helpers.
#include <stdlib.h>

struct Node { int val; struct Node *next; };

int sum_list(void) {
  struct Node *head = NULL;
  for (int i = 0; i < 5; i++) {
    struct Node *n = malloc(sizeof(struct Node));
    n->val = i;
    n->next = head;
    head = n;
  }
  int sum = 0;
  for (struct Node *c = head; c; c = c->next)
    sum += c->val;
  return sum;
}
// The node record's self-ref field is the nullable pool index.
// CHECK: emitrust.struct_def @Node ["val", "next"] [i32, !emitrust.opaque<"Option<usize>">]
// CHECK-LABEL: func.func @sum_list
// The importer emits the pool as the high-level, backend-agnostic collection
// place (element type + folded capacity 5); emitrust-lower-containers turns it
// back into a fixed [Node; 5] array + i64 cursor before the rest of the
// pipeline runs. emitrust-import-c runs no passes, so the raw fat ops show.
// CHECK: emitrust.collection {capacity = 5 : i64, element_type = !emitrust.struct<"Node">} : !emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>
// `n = malloc(...)` appends a defaulted slot and takes its index.
// CHECK: emitrust.collection_push %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>) -> i64
// A handle's `n->field` projection is a subscript into the shared pool.
// CHECK: emitrust.collection_at %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>, i64) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// `n->next = head` builds the Option<usize> field from the handle pair.
// CHECK: emitrust.call_opaque "__emitrust_pool_opt"(%{{.*}}, %{{.*}}) : (i1, i64) -> !emitrust.opaque<"Option<usize>">
// CHECK: emitrust.assign %{{.*}} = %{{.*}} : !emitrust.lvalue<!emitrust.opaque<"Option<usize>">>
// `c = c->next` destructures the field read back into (non-null, index).
// CHECK: emitrust.call_opaque "__emitrust_pool_unpack"(%{{.*}}) : (!emitrust.opaque<"Option<usize>">) -> (i1, i64)
