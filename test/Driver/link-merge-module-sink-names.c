// FR-159 phase 2: a sunk record's per-TU module is named after its shard's
// SOURCE FILE STEM, not after the shard's link-line ordinal.
//
// Phase 1 named the sink module after the shard's position on the link line
// (`mod tu314`). That name is correct but it is a build-system accident: it
// changes when the link line is reordered, it says nothing about which C file
// the type came from, and a diff of two builds that differ only in object
// order shows every sunk module renamed. Phase 2 derives the name from the
// shard's recorded source path instead, so the emitted crate is a function of
// the SOURCES and the reader can find the file a `mod` came from.
//
// A source stem is not automatically a legal, unambiguous Rust module name, so
// this file pins the four decisions that make the mapping total. The three
// fallback legs matter more than the happy one: each is a name that, emitted
// literally, is a rustc error on the whole crate -- exactly the silent
// degradation this project refuses -- and each falls back to the phase-1
// ordinal name, which is always available and always legal.
//   STEM     -- a plain unique stem becomes the module name, with every
//               character outside [A-Za-z0-9_] mapped to `_` so `beta-two.c`
//               is `mod beta_two` and not a Rust parse error.
//   DUP      -- two shards whose stems are IDENTICAL climb to one more
//               leading directory component (`d1/dup.c` -> `d1_dup`) rather
//               than collapsing onto one module or falling back. The two link
//               orders below emit the SAME two candidate names in either
//               direction: this is the position-independence the phase buys.
//   KEYWORD  -- a stem that is a Rust keyword (`loop.c`) falls back. `mod loop
//               { }` is `error: expected identifier, found keyword 'loop'`,
//               measured on rustc.
//   TUSHAPE  -- a stem of the form `tu<N>` falls back. That spelling is the
//               fallback's own namespace, so a file named `tu0.c` sitting at
//               link position 1 must NOT be allowed to claim `mod tu0` and
//               leave shard 0 with nowhere to fall back to.
//   ROOTTYPE -- a stem that collides with a crate-ROOT type falls back. A
//               module lives in the TYPE namespace, so root `struct Widget`
//               plus `mod Widget` is `error[E0428]: the name 'Widget' is
//               defined multiple times`, measured on rustc. The reserved set
//               is deliberately types-only: root `fn mkdir` next to `mod
//               mkdir` compiles fine (different namespaces), and systemd has
//               a real `mkdir.c` that must keep its stem.
//
// The byte-diff oracle for the naming is EndToEnd/link-module-sink-name-e2e.c;
// this file pins the names and the emitted item structure.

// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/alpha.c -o %t/alpha.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/beta-two.c -o %t/beta-two.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/d1/dup.c -o %t/d1/dup.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/d2/dup.c -o %t/d2/dup.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/kw-a.c -o %t/kw-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/loop.c -o %t/loop.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/tu0.c -o %t/tu0.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/rt-a.c -o %t/rt-a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/Widget.c -o %t/Widget.o

// STEM -- the later shard's `Buf` sinks into a module named for `beta-two.c`,
// and its uses inside its own shard are repathed to the ABSOLUTE stem path.
// RUN: emitrust-cc --link %t/alpha.o %t/beta-two.o --emit=rust -o %t.stem.rs
// RUN: FileCheck %s --check-prefix=STEM --strict-whitespace < %t.stem.rs
// FR-220: each record now leads with its own `#[allow(dead_code)]`, the
// targeted replacement for the crate root's retired blanket allow. Attribute
// POSITION only; the module NAMING this file exists to pin -- `beta_two` from
// the source stem, the absolute repathed uses, the absence of any ordinal
// `tu1::` -- is byte-identical.
//      STEM:#[allow(dead_code)]
// STEM-NEXT:#[derive(Clone, Copy, Default)]
// STEM-NEXT:struct Buf {
// STEM-NEXT:    a: i32,
// STEM-NEXT:}
//      STEM:fn tu1_local_use(v0: crate::beta_two::Buf) -> i32 {
//      STEM:let b: crate::beta_two::Buf = crate::beta_two::Buf { a:
//      STEM:mod beta_two {
// STEM-NEXT:    #[allow(dead_code)]
// STEM-NEXT:    #[derive(Clone, Copy, Default)]
// STEM-NEXT:    pub(crate) struct Buf {
// STEM-NEXT:        pub(crate) a: i32,
// STEM-NEXT:        pub(crate) b: i32,
// STEM-NEXT:    }
// STEM-NEXT:}
// The ordinal name is gone entirely -- not merely shadowed.
// RUN: FileCheck %s --check-prefix=STEMNOTU < %t.stem.rs
// STEMNOTU-NOT: tu1::
// STEMNOTU-NOT: mod tu

// DUP -- identical stems in different directories climb one rank each. The
// module is named for the file, so linking the two objects in the OTHER order
// moves which one sinks but keeps both names drawn from the sources.
// RUN: emitrust-cc --link %t/d1/dup.o %t/d2/dup.o --emit=rust -o %t.dup.rs
// RUN: FileCheck %s --check-prefix=DUPFWD --strict-whitespace < %t.dup.rs
//      DUPFWD:struct Rec {
// DUPFWD-NEXT:    a: i32,
// DUPFWD-NEXT:}
//      DUPFWD:fn tu1_use_rec(v0: crate::d2_dup::Rec) -> i32 {
//      DUPFWD:mod d2_dup {
// DUPFWD-NEXT:    #[allow(dead_code)]
// DUPFWD-NEXT:    #[derive(Clone, Copy, Default)]
// DUPFWD-NEXT:    pub(crate) struct Rec {
// DUPFWD-NEXT:        pub(crate) a: i32,
// DUPFWD-NEXT:        pub(crate) b: i32,
// DUPFWD-NEXT:    }
// DUPFWD-NEXT:}
// RUN: FileCheck %s --check-prefix=DUPFWDNOTU < %t.dup.rs
// DUPFWDNOTU-NOT: mod tu
//
// RUN: emitrust-cc --link %t/d2/dup.o %t/d1/dup.o --emit=rust -o %t.dupr.rs
// RUN: FileCheck %s --check-prefix=DUPREV --strict-whitespace < %t.dupr.rs
//      DUPREV:struct Rec {
// DUPREV-NEXT:    a: i32,
// DUPREV-NEXT:    b: i32,
// DUPREV-NEXT:}
//      DUPREV:fn tu1_use_rec(v0: crate::d1_dup::Rec) -> i32 {
//      DUPREV:mod d1_dup {
// DUPREV-NEXT:    #[allow(dead_code)]
// DUPREV-NEXT:    #[derive(Clone, Copy, Default)]
// DUPREV-NEXT:    pub(crate) struct Rec {
// DUPREV-NEXT:        pub(crate) a: i32,
// DUPREV-NEXT:    }
// DUPREV-NEXT:}
// RUN: FileCheck %s --check-prefix=DUPREVNOTU < %t.dupr.rs
// DUPREVNOTU-NOT: mod tu

// KEYWORD -- `loop` is not spellable as a module name, so the shard keeps the
// phase-1 ordinal name and the crate stays parseable.
// RUN: emitrust-cc --link %t/kw-a.o %t/loop.o --emit=rust -o %t.kw.rs
// RUN: FileCheck %s --check-prefix=KEYWORD --strict-whitespace < %t.kw.rs
//      KEYWORD:fn tu1_kw_use(v0: crate::tu1::Buf) -> i32 {
//      KEYWORD:mod tu1 {
// KEYWORD-NEXT:    #[allow(dead_code)]
// KEYWORD-NEXT:    #[derive(Clone, Copy, Default)]
// KEYWORD-NEXT:    pub(crate) struct Buf {
// KEYWORD-NEXT:        pub(crate) a: i32,
// KEYWORD-NEXT:        pub(crate) b: i32,
// KEYWORD-NEXT:    }
// KEYWORD-NEXT:}
// RUN: FileCheck %s --check-prefix=KEYWORDNOT < %t.kw.rs
// KEYWORDNOT-NOT: mod loop

// TUSHAPE -- `tu0.c` is the SECOND object on the link line, so its ordinal
// fallback is `tu1`. It gets exactly that: the stem `tu0` is refused, and the
// module it would have claimed stays available to shard 0.
// RUN: emitrust-cc --link %t/kw-a.o %t/tu0.o --emit=rust -o %t.ts.rs
// RUN: FileCheck %s --check-prefix=TUSHAPE --strict-whitespace < %t.ts.rs
//      TUSHAPE:fn tu1_kw_use(v0: crate::tu1::Buf) -> i32 {
//      TUSHAPE:mod tu1 {
// TUSHAPE-NEXT:    #[allow(dead_code)]
// TUSHAPE-NEXT:    #[derive(Clone, Copy, Default)]
// TUSHAPE-NEXT:    pub(crate) struct Buf {
// TUSHAPE-NEXT:        pub(crate) a: i32,
// TUSHAPE-NEXT:        pub(crate) b: i32,
// TUSHAPE-NEXT:    }
// TUSHAPE-NEXT:}
// RUN: FileCheck %s --check-prefix=TUSHAPENOT < %t.ts.rs
// TUSHAPENOT-NOT: mod tu0

// ROOTTYPE -- `struct Widget` is defined IDENTICALLY in both shards, so it
// dedups and keeps the crate root. `Widget.c`'s stem therefore names a type
// that already exists at the root and the shard falls back to its ordinal.
// RUN: emitrust-cc --link %t/rt-a.o %t/Widget.o --emit=rust -o %t.rt.rs
// RUN: FileCheck %s --check-prefix=ROOTTYPE --strict-whitespace < %t.rt.rs
//      ROOTTYPE:struct Widget {
// ROOTTYPE-NEXT:    w: i32,
// ROOTTYPE-NEXT:}
//      ROOTTYPE:fn tu1_rt_use(v0: crate::tu1::Buf) -> i32 {
//      ROOTTYPE:mod tu1 {
// ROOTTYPE-NEXT:    #[allow(dead_code)]
// ROOTTYPE-NEXT:    #[derive(Clone, Copy, Default)]
// ROOTTYPE-NEXT:    pub(crate) struct Buf {
// ROOTTYPE-NEXT:        pub(crate) a: i32,
// ROOTTYPE-NEXT:        pub(crate) b: i32,
// ROOTTYPE-NEXT:    }
// ROOTTYPE-NEXT:}
// Exactly one `Widget` -- the dedup is undisturbed -- and no module shadows it.
// RUN: FileCheck %s --check-prefix=ROOTCOUNT < %t.rt.rs
// ROOTCOUNT-COUNT-1: struct Widget {
// ROOTCOUNT-NOT: struct Widget {
// ROOTCOUNT-NOT: mod Widget

//--- alpha.c
struct Buf {
  int a;
};
static int local_use(struct Buf b) { return b.a; }
int f1(int seed) {
  struct Buf b;
  b.a = seed + 11;
  return local_use(b);
}

//--- beta-two.c
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

//--- d1/dup.c
struct Rec {
  int a;
};
static int use_rec(struct Rec r) { return r.a; }
int g1(int seed) {
  struct Rec r;
  r.a = seed + 5;
  return use_rec(r);
}

//--- d2/dup.c
struct Rec {
  int a;
  int b;
};
static int use_rec(struct Rec r) { return r.a - r.b; }
int g2(int seed) {
  struct Rec r;
  r.a = seed + 6;
  r.b = seed + 7;
  return use_rec(r);
}

//--- kw-a.c
struct Buf {
  int a;
};
static int kw_use(struct Buf b) { return b.a; }
int h1(int seed) {
  struct Buf b;
  b.a = seed + 8;
  return kw_use(b);
}

//--- loop.c
struct Buf {
  int a;
  int b;
};
static int kw_use(struct Buf b) { return b.a + b.b; }
int h2(int seed) {
  struct Buf b;
  b.a = seed + 9;
  b.b = seed + 10;
  return kw_use(b);
}

//--- tu0.c
struct Buf {
  int a;
  int b;
};
static int kw_use(struct Buf b) { return b.a - b.b; }
int h3(int seed) {
  struct Buf b;
  b.a = seed + 15;
  b.b = seed + 16;
  return kw_use(b);
}

//--- rt-a.c
struct Widget {
  int w;
};
struct Buf {
  int a;
};
int widen(struct Widget w);
int widen(struct Widget w) { return w.w; }
static int rt_use(struct Buf b) { return b.a; }
int k1(int seed) {
  struct Buf b;
  b.a = seed + 12;
  return rt_use(b);
}

//--- Widget.c
struct Widget {
  int w;
};
struct Buf {
  int a;
  int b;
};
int widen(struct Widget w);
static int rt_use(struct Buf b) { return b.a * b.b; }
int k2(int seed) {
  struct Widget w;
  struct Buf b;
  w.w = seed;
  b.a = seed + 13;
  b.b = seed + 14;
  return rt_use(b) + widen(w);
}
