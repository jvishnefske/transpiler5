// FR-159 phase 1: the link-time shape-conflict SINK, and the escape guard
// that bounds it.
//
// Two shards defining the same record name with DIFFERENT shapes has always
// been a hard link error (FR-58, link-merge-errors.c). For a record that is
// genuinely TRANSLATION-UNIT-LOCAL that error is too strong: C99 6.2.7 makes
// two file-scope tags in different TUs distinct types unless something makes
// them meet, so the honest Rust rendering is two PATHS, not one name. The
// later definition is sunk into `mod tu<N> { ... }` and every reference to it
// inside its own shard is repathed; the link succeeds.
//
// The sink is sound only while NOTHING outside the owning TU can name the
// record, so this file pins the guard as hard as it pins the feature. All
// four rejection legs below are records that a naive per-TU bucketing would
// have sunk and then handed to rustc as an E0308 with no source location --
// exactly the silent-degradation direction this project refuses. Every one
// of them keeps the FR-58 located rejection, now with a note naming the site
// that made the record escape:
//   PTR    -- reached through `struct Rec *` in an external-linkage
//             signature. The guard must therefore be TRANSITIVE through
//             `mut_ref`/`ref`/`slice`/`array`/`fn_ptr`, not a top-level
//             by-value check (the real systemd users are
//             `&mut [tu314::SwapEntries]`).
//   FIELD  -- reached through the FIELD of a struct that itself dedups
//             across the shards. That wrapper is ONE Rust type for the whole
//             crate, so its field cannot be repathed for one shard only.
//   GLOBAL -- reached through the type of an external-linkage global.
//   ODR    -- the header-divergence case (`#ifdef` in a shared header):
//             a genuine ODR violation, refused verbatim as before.
//
// And two non-regression legs:
//   DUP  -- two TUs with an IDENTICAL file-scope `struct P` shared through
//           an extern-by-value function. C makes these the SAME type and the
//           link works TODAY; bucketing them per TU would give rustc
//           `expected tu0::P, found tu1::P`. Records stay at crate root and
//           shape-dedup as they always have; ONLY a shape-CONFLICTING,
//           non-escaping record sinks.
//   SINK -- the positive: same-named TU-local records of different shapes,
//           used only from internal-linkage statics, link into two paths.
//
// The emitted-code oracle for SINK and DUP is the byte-diff in
// EndToEnd/link-module-sink-e2e.c; this file pins the link verdicts and the
// emitted item structure.

// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/sink-a.c -o %t/sink-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/sink-b.c -o %t/sink-b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/sink-main.c -o %t/sink-main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/ptr-a.c -o %t/ptr-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/ptr-b.c -o %t/ptr-b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/field-a.c -o %t/field-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/field-b.c -o %t/field-b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/glob-a.c -o %t/glob-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/glob-b.c -o %t/glob-b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/dup-a.c -o %t/dup-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/dup-b.c -o %t/dup-b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/odr-a.c -o %t/odr-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/odr-b.c -o %t/odr-b.o

// SINK -- the link succeeds and the LATER shard's `Buf` lands in that shard's
// module under its leaf name, `pub(crate)` on the struct and every field so
// the root can still construct it (a private field is rustc E0616).
// RUN: emitrust-cc --link %t/sink-a.o %t/sink-b.o %t/sink-main.o --emit=rust -o %t.sink.rs
// RUN: FileCheck %s --check-prefix=SINK --strict-whitespace < %t.sink.rs
//      SINK: #[derive(Clone, Copy, Default)]
// SINK-NEXT: struct Buf {
// SINK-NEXT:     a: i32,
// SINK-NEXT: }
// The sunk record's uses inside its OWN shard are repathed, at the ABSOLUTE
// path -- a relative `tu1::Buf` is module-relative and rustc E0433 the
// moment anything renders inside a module.
//      SINK: fn tu1_local_use(v0: crate::tu1::Buf) -> i32 {
//      SINK: let b: crate::tu1::Buf = crate::tu1::Buf { a:
//      SINK: mod tu1 {
// SINK-NEXT:     #[derive(Clone, Copy, Default)]
// SINK-NEXT:     pub(crate) struct Buf {
// SINK-NEXT:         pub(crate) a: i32,
// SINK-NEXT:         pub(crate) b: i32,
// SINK-NEXT:     }
// SINK-NEXT: }
// Exactly two `Buf` definitions come out of two conflicting shapes: one at
// the root, one in the module. Nothing merged, nothing duplicated.
// RUN: FileCheck %s --check-prefix=SINKCOUNT < %t.sink.rs
// SINKCOUNT-COUNT-2: struct Buf {
// SINKCOUNT-NOT: struct Buf {

// PTR -- the conflicting record reaches an external-linkage signature THROUGH
// A POINTER. It must stay a located rejection, never a silent sink that
// degrades into rustc E0308.
// RUN: not emitrust-cc --link %t/ptr-a.o %t/ptr-b.o --emit=rust -o %t.ptr.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=PTR
// PTR: ptr-b.c:{{[0-9]+}}:{{[0-9]+}}: error: conflicting definitions of 'Rec' at link: the shards disagree on its shape
// PTR: ptr-b.c:{{[0-9]+}}:{{[0-9]+}}: note: 'Rec' appears in the cross-translation-unit interface of 'consume_ptr', so it cannot be made translation-unit-local

// FIELD -- the conflicting record is a FIELD of a wrapper struct that dedups
// across the shards. The wrapper is one crate-wide Rust type, so the field
// cannot be repathed for one shard alone.
// RUN: not emitrust-cc --link %t/field-a.o %t/field-b.o --emit=rust -o %t.field.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FIELD
// FIELD: field-b.c:{{[0-9]+}}:{{[0-9]+}}: error: conflicting definitions of 'Rec' at link: the shards disagree on its shape
// FIELD: field-b.c:{{[0-9]+}}:{{[0-9]+}}: note: 'Rec' appears in the cross-translation-unit interface of 'Wrap', so it cannot be made translation-unit-local

// GLOBAL -- the conflicting record is the type of an external-linkage global.
// The note names the EMITTED item (`SHARED_REC` after the FR-53 idiomatic
// rename), which is the name the reader will find in the crate.
// RUN: not emitrust-cc --link %t/glob-a.o %t/glob-b.o --emit=rust -o %t.glob.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=GLOBAL
// GLOBAL: glob-b.c:{{[0-9]+}}:{{[0-9]+}}: error: conflicting definitions of 'Rec' at link: the shards disagree on its shape
// GLOBAL: glob-b.c:{{[0-9]+}}:{{[0-9]+}}: note: 'Rec' appears in the cross-translation-unit interface of 'SHARED_REC', so it cannot be made translation-unit-local

// ODR -- a header struct diverging by `#ifdef` keeps its rejection verbatim.
// RUN: not emitrust-cc --link %t/odr-a.o %t/odr-b.o --emit=rust -o %t.odr.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ODR
// ODR: odr.h:{{[0-9]+}}:{{[0-9]+}}: error: conflicting definitions of 'Cfg' at link: the shards disagree on its shape
// ODR: odr.h:{{[0-9]+}}:{{[0-9]+}}: note: 'Cfg' appears in the cross-translation-unit interface of 'use_cfg', so it cannot be made translation-unit-local

// DUP -- two IDENTICAL file-scope records shared through an extern-by-value
// function still dedup to ONE root struct and no module is created.
// RUN: emitrust-cc --link %t/dup-a.o %t/dup-b.o --emit=rust -o %t.dup.rs
// RUN: FileCheck %s --check-prefix=DUP < %t.dup.rs
// DUP-COUNT-1: struct P {
// DUP-NOT: struct P {
// DUP-NOT: mod tu

//--- sink-a.c
struct Buf {
  int a;
};
static int local_use(struct Buf b) { return b.a; }
int f1(int seed) {
  struct Buf b;
  b.a = seed + 11;
  return local_use(b);
}

//--- sink-b.c
struct Buf {
  int a;
  int b;
};
static int local_use(struct Buf b) { return b.a * b.b; }
int f2(int seed) {
  struct Buf b;
  b.a = seed + 3;
  b.b = seed + 4;
  return local_use(b);
}

//--- sink-main.c
int printf(const char *, ...);
int f1(int);
int f2(int);
int main(int argc, char **argv) {
  printf("%d %d\n", f1(argc), f2(argc));
  return 0;
}

//--- ptr-a.c
struct Rec {
  int a;
};
int consume_ptr(struct Rec *r);
int consume_ptr(struct Rec *r) { return r->a; }

//--- ptr-b.c
struct Rec {
  int a;
  int b;
};
int consume_ptr(struct Rec *r);
int ptr_b(int s) {
  struct Rec r;
  r.a = s;
  r.b = s + 1;
  return consume_ptr(&r);
}

//--- field-a.c
struct Rec {
  int a;
};
struct Wrap {
  struct Rec r;
};
int use_wrap(struct Wrap w);
int use_wrap(struct Wrap w) { return w.r.a; }

//--- field-b.c
struct Rec {
  int a;
  int b;
};
struct Wrap {
  struct Rec r;
};
static int local_w(struct Wrap w) { return w.r.a * w.r.b; }
int field_b(int s) {
  struct Wrap w;
  w.r.a = s;
  w.r.b = s + 1;
  return local_w(w);
}

//--- glob-a.c
struct Rec {
  int a;
};
extern struct Rec shared_rec;
struct Rec shared_rec;
int glob_a(void) { return shared_rec.a; }

//--- glob-b.c
struct Rec {
  int a;
  int b;
};
extern struct Rec shared_rec;
static int local_g(void) { return shared_rec.a + shared_rec.b; }
int glob_b(void) { return local_g(); }

//--- dup-a.c
struct P {
  int x;
  int y;
};
int consume(struct P p);
int consume(struct P p) { return p.x + p.y; }

//--- dup-b.c
struct P {
  int x;
  int y;
};
int consume(struct P p);
int dup_b(int s) {
  struct P p;
  p.x = s;
  p.y = s + 1;
  return consume(p);
}

//--- odr.h
struct Cfg {
  int a;
#ifdef WIDE
  int b;
#endif
};
int use_cfg(struct Cfg c);

//--- odr-a.c
#define WIDE 1
#include "odr.h"
int use_cfg(struct Cfg c) { return c.a + c.b; }

//--- odr-b.c
#include "odr.h"
int odr_b(int s) {
  struct Cfg c;
  c.a = s;
  return use_cfg(c);
}
