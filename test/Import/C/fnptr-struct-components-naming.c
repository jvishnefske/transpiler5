// FR-102 regression pin for the NAME-RESOLUTION ORDERING defect the
// struct-pointer component admission is the first feature to reach.
//
// `importRecordUncached` used to decide the emitted Rust name AFTER the
// field walk: the block-scope `<fn>_<tag>` mangling and the C99 6.2.3
// tag-versus-ordinary collision rename (`Struct_<tag>`) both ran below
// `collectRecordFields`. A SELF-REFERENTIAL fn-ptr component resolves its
// own record type THROUGH `emittedRecordName` DURING that walk, so it
// missed both maps and fell back to the raw tag — emitting a struct_def
// named `c_main_loc` / `Struct_ops` whose own field named a type
// `loc` / `ops` that does not exist. That is a dangling type reference
// with NO located diagnostic (rustc E0412, or E0573 when the colliding
// ordinary name is a function), and emitrust-opt cannot see it because
// struct types are opaque by name and nothing cross-checks a struct_def's
// field types against the module's definitions.
//
// The fix hoists both name decisions above the field walk (and erases
// them again on every field-walk failure path, so a REJECTED record never
// leaves a claimed name behind and cascade attribution is unchanged).
// These arms pin that the field type and the struct_def symbol are one
// decision, in the IR and in the emitted crate, in BOTH naming modes.
//
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/block-scope.c | FileCheck %s --check-prefix=LOCAL
// RUN: emitrust-import-c %t/tag-collision.c | FileCheck %s --check-prefix=COLLIDE
// RUN: emitrust-cc --emit=crate %t/tag-collision.c -o %t.crate
// RUN: cat %t.crate/src/main.rs | FileCheck %s --check-prefix=CRATE
// RUN: emitrust-cc --preserve-c-names --emit=crate %t/tag-collision.c -o %t.verbatim.crate
// RUN: cat %t.verbatim.crate/src/main.rs | FileCheck %s --check-prefix=VERBATIM
// RUN: emitrust-cc --preserve-c-names --emit=crate %t/link-a.c %t/link-b.c -o %t.link.crate
// RUN: cat %t.link.crate/src/main.rs | FileCheck %s --check-prefix=LINK

//--- block-scope.c
// A block-scope record needs no collision to reach the defect: the
// `<function>_<tag>` mangling alone is enough, so the component must read
// `c_main_loc`, never `loc`.
int printf(const char *, ...);
int main(void) {
  struct loc { int val; int (*visit)(struct loc *l, int d); };
  struct loc x;
  x.val = 5;
  x.visit = 0;
  printf("%d\n", x.val);
  return 0;
}
// LOCAL: emitrust.struct_def @c_main_loc ["val", "visit"] [i32, !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"c_main_loc">>, i32) -> i32>]

//--- tag-collision.c
// `struct ops` and the function `ops` coexist in C (tag versus ordinary
// namespace), so the tag is renamed; the self-referential component must
// follow the rename.
int printf(const char *, ...);
void ops(void);
struct ops { int val; int (*visit)(struct ops *o, int d); };
static int dbl(struct ops *o, int d) { o->val = o->val * 2 + d; return o->val; }
void ops(void) { printf("side\n"); }
int main(void) {
  struct ops a;
  a.val = 5;
  a.visit = dbl;
  ops();
  int r = a.visit(&a, 3);
  printf("r=%d\n", r);
  return 0;
}
// COLLIDE: emitrust.struct_def @Struct_ops ["val", "visit"] [i32, !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Struct_ops">>, i32) -> i32>]
// COLLIDE: emitrust.constant <#emitrust.opaque<"Some(dbl)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Struct_ops">>, i32) -> i32>

// The emitted crate must name the SAME type in the field as it declares,
// in both naming modes. Under the default idiomatic rename the tag takes
// the type spelling `Ops` while the function keeps `ops`, so no rename is
// needed; under --preserve-c-names both want the verbatim `ops` and the
// tag becomes `Struct_ops` -- which is exactly the mode in which the
// unfixed ordering emitted a field typed `&mut ops`, naming a FUNCTION
// (rustc E0573).
// CRATE: struct Ops {
// CRATE: visit: Option<fn(&mut Ops, i32) -> i32>,
// VERBATIM: struct Struct_ops {
// VERBATIM: visit: Option<fn(&mut Struct_ops, i32) -> i32>,

//--- link-a.c
// MULTI-TU: the same collision-renamed self-referential record reached
// through two translation units must dedup to ONE struct_def whose field
// still names it. The name is now decided before the field walk, so the
// cross-TU shape key is computed from the renamed spelling in both TUs --
// if one TU disagreed the merge would report a conflicting definition.
int printf(const char *, ...);
void ops(void);
struct ops { int val; int (*visit)(struct ops *o, int d); };
int dbl(struct ops *o, int d) { o->val = o->val * 2 + d; return o->val; }
void ops(void) { printf("side\n"); }

//--- link-b.c
int printf(const char *, ...);
void ops(void);
struct ops { int val; int (*visit)(struct ops *o, int d); };
int dbl(struct ops *o, int d);
int main(void) {
  struct ops a;
  a.val = 5;
  a.visit = dbl;
  ops();
  int r = a.visit(&a, 3);
  printf("r=%d\n", r);
  return 0;
}
// LINK: struct Struct_ops {
// LINK: visit: Option<fn(&mut Struct_ops, i32) -> i32>,
// LINK-NOT: struct Struct_ops {
