// FR-62 (slice 1): the record/global node-key collision fix E2 surfaced on
// test/EndToEnd/pointers-member.c. `struct G` and global `g` both used to
// key as `G` (UpperCamel tag == SCREAMING single-letter global), so ONE
// node line survived the merge while the other item's edges did -- edges
// whose endpoint was not a node, violating the stated closure invariant.
// The importer never had this collision: `CImporter::structSymbolName`
// renames the TAG to `Struct_<tag>` (emitted `StructG` under the idiomatic
// rename) exactly when the TU's ordinary identifier namespace claims the
// name, a decision reproducible from the per-TU pre-scan
// (`collectOrdinaryNames`: functions, file-scope variables, static-local
// mangles). The graph now runs the same pre-scan and the same rule, so this
// pins: the record node keyed `StructG` -- matching the emitted Rust type
// -- the global keyed `G`, and every edge endpoint a node again.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

struct G { int a; int *gp; };

int gx = 10;
struct G g = { .gp = &gx, .a = 4 };

int read_member(void) { return g.a; }

// Both items, distinct keys, the record under the importer's renamed
// spelling with its own definition location.
// CHECK:      node G kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-struct-rename.c:19:10
// CHECK-NEXT: node GX kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-struct-rename.c:18:5
// CHECK-NEXT: node StructG kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-struct-rename.c:16:8
// CHECK-NEXT: node read_member kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-struct-rename.c:21:5

// The global's declared type names the RENAMED record; its initializer
// takes gx's address (ReadsGlobal + AddressOfGlobal, FR-62 slice 1a).
// CHECK-NEXT: edge G -> StructG kind=SigType
// CHECK-NEXT: edge G -> GX kind=ReadsGlobal
// CHECK-NEXT: edge G -> GX kind=AddressOfGlobal

// CHECK-NEXT: edge read_member -> G kind=ReadsGlobal
// CHECK-NOT:  edge
