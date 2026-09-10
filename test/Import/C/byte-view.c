// FR-229 Wave 1: the materialized OBJECT-REPRESENTATION byte view. C's
// `(unsigned char *)&obj` names the object representation of `obj`, which
// the typed model never materializes -- so instead of exposing the typed
// object's storage (which would make the emitted crate depend on rustc
// laying the struct out the way clang did, the thing `transmute` gets
// wrong), the view is BUILT: a synthetic `[u8; sizeof obj]` local filled
// by `T::to_ne_bytes` per scalar component, each scattered at the
// component's C byte offset in C DECLARATION order, and borrowed as the
// slice argument. Rust's own layout is then irrelevant by construction.
//
// This file pins the four things a later refactor could silently break:
//
//  * THE OFFSET MAP. The `{int; int; double;}` case must write bytes 0..3
//    from `a`, 4..7 from `b` and 8..15 from `c`. The offsets are READ off
//    `emitrust.abi_layout` -- clang's OWN ASTRecordLayout, which is also
//    what the emitted const-assertions check rustc against -- and are
//    never re-derived, so there is no second layout model to disagree.
//  * THE ne_bytes CALLEE NAMES, including `f32`/`f64`. Those needed no
//    dialect change at all: they ride `emitrust.call_opaque` exactly as
//    the integer ones do, and only `neBytesTypeName` had to widen.
//  * ZERO `unsafe`. The whole point of the copy-in/copy-out shape is that
//    it discharges the object-representation obligation rather than
//    relocating it into an `unsafe` block; `--implicit-check-not=unsafe`
//    on every prefix is what keeps that honest.
//  * THE WRITE-BACK. A MUTABLE (`&mut [u8]`) byte-slice parameter means
//    the callee may write through the view; since the view is a COPY, the
//    object must be reconstituted with `from_ne_bytes` after the call or
//    the write is silently lost -- a MEASURED miscompile (native
//    -1431655766 against a naive Rust 1), from a crate that builds clean.
//    The runtime proof is test/EndToEnd/byte-view-writeback.c; what is
//    pinned here is that the reconstitution is emitted at all, and that
//    it reads each component back from its OWN offset.
//
// The negative space -- padded aggregates, global bases, escaping views
// and the non-scalar member -- is byte-view-invalid.c.
//
// RUN: split-file %s %t
// RUN: emitrust-cc --emit=rust %t/scalar.c -o - | FileCheck %s --check-prefix=SCALAR --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/floats.c -o - | FileCheck %s --check-prefix=FLOATS --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/aggregate.c -o - | FileCheck %s --check-prefix=AGG --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/writeback.c -o - | FileCheck %s --check-prefix=WB --implicit-check-not=unsafe
// RUN: emitrust-import-c %t/aggregate.c | FileCheck %s --check-prefix=LAYOUT

//--- scalar.c
// A SHARED (`const unsigned char *`) view of a by-value parameter: one
// `to_ne_bytes`, no write-back (C forbids writing through the pointer, so
// no valid program can observe a lost write).
int printf(const char *, ...);
static void ph(const unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
void view_int(int x) {
  ph((const unsigned char *)&x, sizeof(x));
}
// SCALAR: pub fn view_int(v0: i32) {
// SCALAR: let mut [[IMG:v[0-9]+]]: [u8; 4] = [0; 4];
// SCALAR: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(x);
// SCALAR: let [[SRC:v[0-9]+]]: [u8; 4] =
// SCALAR: [[IMG]][0i64 as usize] = [[SRC]][0i64 as usize];
// SCALAR: [[IMG]][3i64 as usize] = [[SRC]][3i64 as usize];
// SCALAR: let {{v[0-9]+}}: &[u8] = &[[IMG]][0i64 as usize..];
// A shared view has nothing to write back.
// SCALAR-NOT: from_ne_bytes

//--- floats.c
// FR-229 capability Af: `f32`/`f64::to_ne_bytes` are ordinary associated
// functions and need no dialect op of their own.
int printf(const char *, ...);
static void ph(const unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
void view_float(float f, double d) {
  ph((const unsigned char *)&f, sizeof(f));
  ph((const unsigned char *)&d, sizeof(d));
}
// FLOATS: let mut {{v[0-9]+}}: [u8; 4] = [0; 4];
// FLOATS: let {{v[0-9]+}}: [u8; 4] = f32::to_ne_bytes(f);
// FLOATS: let mut {{v[0-9]+}}: [u8; 8] = [0; 8];
// FLOATS: let {{v[0-9]+}}: [u8; 8] = f64::to_ne_bytes(d);

//--- aggregate.c
// `{int; int; double;}`: 4 + 4 + 8 == 16 == sizeof, offsets 0/4/8, no
// padding. The three components land at those offsets, in that order.
int printf(const char *, ...);
typedef struct { int a; int b; double c; } P;
static void ph(const unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
void view_struct(int x) {
  P p;
  p.a = x;
  p.b = 1;
  p.c = 2.0;
  ph((const unsigned char *)&p, sizeof(p));
}
// The gate is clang's own layout record, not a re-derivation.
// LAYOUT: emitrust.struct_def @P ["a", "b", "c"] [i32, i32, f64] {emitrust.abi_faithful, emitrust.abi_layout = {align = 8 : i64, offsets = [0, 4, 8], size = 16 : i64}}
//
// AGG: let mut [[IMG:v[0-9]+]]: [u8; 16] = [0; 16];
// AGG: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(p.a);
// AGG: let [[A:v[0-9]+]]: [u8; 4] =
// AGG: [[IMG]][0i64 as usize] = [[A]][0i64 as usize];
// AGG: [[IMG]][3i64 as usize] = [[A]][3i64 as usize];
// AGG: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(p.b);
// AGG: let [[B:v[0-9]+]]: [u8; 4] =
// AGG: [[IMG]][4i64 as usize] = [[B]][0i64 as usize];
// AGG: [[IMG]][7i64 as usize] = [[B]][3i64 as usize];
// AGG: let {{v[0-9]+}}: [u8; 8] = f64::to_ne_bytes(p.c);
// AGG: let [[C:v[0-9]+]]: [u8; 8] =
// AGG: [[IMG]][8i64 as usize] = [[C]][0i64 as usize];
// AGG: [[IMG]][15i64 as usize] = [[C]][7i64 as usize];
// AGG: let {{v[0-9]+}}: &[u8] = &[[IMG]][0i64 as usize..];

//--- writeback.c
// A MUTABLE byte-slice parameter: the object is read after the call, so
// the reconstitution must survive dead-store elimination and must put the
// bytes back into `x`.
int printf(const char *, ...);
static void zap(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = 0xaa;
}
void mutate(int x) {
  zap((unsigned char *)&x, sizeof(x));
  printf("%d\n", x);
}
// WB: let mut [[IMG:v[0-9]+]]: [u8; 4] = [0; 4];
// WB: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(x);
// WB: let {{v[0-9]+}}: &mut [u8] = &mut [[IMG]][0i64 as usize..];
// WB: tu0_zap(
// WB: let mut [[BACK:v[0-9]+]]: [u8; 4] = [0; 4];
// WB: [[BACK]][0i64 as usize] = [[IMG]][0i64 as usize];
// WB: [[BACK]][3i64 as usize] = [[IMG]][3i64 as usize];
// WB: let [[V:v[0-9]+]]: i32 = i32::from_ne_bytes([[BACK]]);
// WB: x = [[V]];
