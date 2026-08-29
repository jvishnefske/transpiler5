// FR-108, the PLAIN-C instance of the record-name collision channel: no
// C++, no templates, no idiomatic rename needed. `typedef struct { int
// v; } Box;` beside `struct Box { int v; };` are two DISTINCT C types
// (C99 6.2.3 keeps the tag and ordinary namespaces separate), but both
// compute the emitted Rust name `Box` — the anonymous record takes its
// typedef's name — and the shape-keyed dedup in `importRecordUncached`
// silently merged them into ONE Rust type. Harmless while nothing has
// methods, but it is the same silent unification that miscompiles on the
// C++ side, and CLAUDE.md's safe-failure rule says the direction of
// failure must be a located diagnostic, never a quiet merge.
//
// The negative control matters as much as the rejection: the guard is
// gated on the record having a USER-WRITTEN name, because two same-shape
// ANONYMOUS file-scope records legitimately share one synthesized
// `Anon<hash>` type -- the uppercase hex content hash of the field shape
// (CTS-R1/FR-151, structs-anon-bare.c). Without that gate this guard
// rejects `struct { int v; } g1; struct { int v; } g2;` — measured.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/typedef-first.c 2>&1 | FileCheck %s --check-prefix=TYPEDEF
// RUN: not emitrust-import-c %t/tag-first.c 2>&1 | FileCheck %s --check-prefix=TAGFIRST
// RUN: emitrust-import-c %t/anon-pair.c | FileCheck %s --check-prefix=ANON

//--- typedef-first.c
// The typedef'd anonymous struct claims `Box` first.
// TYPEDEF: typedef-first.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'Box' collides with the emitted name of a different struct in this translation unit
typedef struct { int v; } Box;
struct Box { int v; };

int use(void) {
  Box a;
  struct Box b;
  a.v = 1;
  b.v = 2;
  return a.v + b.v;
}

//--- tag-first.c
// The mirror order: the tag is imported first. Same wording.
// TAGFIRST: tag-first.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'Box' collides with the emitted name of a different struct in this translation unit
struct Box { int v; };
typedef struct { int v; } Box;

int use(void) {
  struct Box b;
  Box a;
  b.v = 1;
  a.v = 2;
  return a.v + b.v;
}

//--- anon-pair.c
// NEGATIVE CONTROL. Two same-shape file-scope anonymous records still
// merge onto ONE anonymous type: they have no user-written name, the
// emitted name is the content hash of the field shape, and the merge is
// the documented CTS-R1 behavior. This is the regression the guard's
// `recordRustName(definition).empty()` gate exists to prevent. The name
// is captured, never spelled: FR-151 made it a hash, and the property
// pinned here is the SHARING, not the spelling.
struct { int v; } g1;
struct { int v; } g2;

int use(void) {
  g1.v = 1;
  g2.v = 2;
  return g1.v + g2.v;
}

// ANON: emitrust.struct_def @[[V:Anon[0-9A-F]+]] ["v"] [i32]
// ANON-NOT: emitrust.struct_def @Anon
// ANON-DAG: emitrust.global @g1 : !emitrust.struct<"[[V]]">
// ANON-DAG: emitrust.global @g2 : !emitrust.struct<"[[V]]">
