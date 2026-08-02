// FR-40: the type and data edges of the item graph. Pins that nested
// records produce `Field` edges (a self-referential POINTER one a
// `FieldIndirect` self-edge), that an enum named by a field is a `Field` edge to an
// `kind=enum` node, that signature types are `SigType` while body-only
// mentions (local declarations, casts, `sizeof` operands) are `BodyType`,
// that a global read and a global written from the same function produce
// BOTH a `ReadsGlobal` and a `WritesGlobal` edge, that taking a function's
// address from a global initializer is a `TakesAddressOf` edge out of the
// GLOBAL's node, and that a global's own declared type is a `SigType` edge.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

enum Level { LOW, HIGH };

struct Leaf {
  int value;
};

struct Branch {
  struct Leaf leaf;
  enum Level level;
};

struct Chain {
  struct Chain *next;
};

int total;
static int scratch;

int measure(struct Branch *b);

int measure(struct Branch *b) {
  struct Leaf local;
  local.value = (int)sizeof(struct Chain);
  total += local.value;
  scratch = total;
  return (int)b->level;
}

int (*hook)(struct Branch *) = measure;

struct Branch origin;

int main(void) { return measure(&origin); }

// CHECK:      node Branch kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:18:8
// CHECK-NEXT: node Chain kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:23:8
// CHECK-NEXT: node HOOK kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:40:7
// CHECK-NEXT: node Leaf kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:14:8
// CHECK-NEXT: node Level kind=enum def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:12:6
// CHECK-NEXT: node ORIGIN kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:42:15
// CHECK-NEXT: node TOTAL kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:27:5
// CHECK-NEXT: node TU0_SCRATCH kind=global def=1 linkage=intern tu=0 loc={{.*}}item-graph-types.c:28:12
// CHECK-NEXT: node c_main kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:44:5
// CHECK-NEXT: node measure kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-types.c:32:5

// A record's field types are Field edges; a self-referential record's
// pointer field is a self-edge, not a dropped one -- but a `FieldIndirect`
// one, because a POINTER member is erased by the importer's
// pointer-struct-member models and the emitted Rust never names the pointee
// (FR-50; see item-graph-field-indirect.c for the whole split). The trailing
// `{{$}}` anchors are load-bearing: `kind=Field` is a prefix of
// `kind=FieldIndirect`, so a bare substring match would confuse the two.
// CHECK-NEXT: edge Branch -> Leaf kind=Field{{$}}
// CHECK-NEXT: edge Branch -> Level kind=Field{{$}}
// CHECK-NEXT: edge Chain -> Chain kind=FieldIndirect{{$}}

// A function-pointer global's initializer takes the target's address, and
// the pointee's parameter type is part of the global's own signature.
// CHECK-NEXT: edge HOOK -> Branch kind=SigType
// CHECK-NEXT: edge HOOK -> measure kind=TakesAddressOf

// CHECK-NEXT: edge ORIGIN -> Branch kind=SigType

// CHECK-NEXT: edge c_main -> measure kind=Calls
// CHECK-NEXT: edge c_main -> ORIGIN kind=ReadsGlobal

// Signature types versus body-only mentions, and the read/write split:
// `total += ...` is both, `scratch = total` writes scratch and reads total.
// CHECK-NEXT: edge measure -> Branch kind=SigType
// CHECK-NEXT: edge measure -> Chain kind=BodyType
// CHECK-NEXT: edge measure -> Leaf kind=BodyType
// CHECK-NEXT: edge measure -> TOTAL kind=ReadsGlobal
// CHECK-NEXT: edge measure -> TOTAL kind=WritesGlobal
// CHECK-NEXT: edge measure -> TU0_SCRATCH kind=WritesGlobal
// CHECK-NOT:  edge
