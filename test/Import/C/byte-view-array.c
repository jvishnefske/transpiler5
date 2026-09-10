// FR-229 Wave 2: the memcpy OBJECT-REPRESENTATION SOURCE (capability C),
// the i8/u8 BYTE-ARRAY DOMAIN CROSSING (capability D), and the
// `(unsigned char *)&arr` spelling (capability E). Wave 1's scalar and
// padding-free-aggregate views are byte-view.c; the negative space for
// this wave is byte-view-array-invalid.c.
//
// This file pins the four things a later refactor could silently break:
//
//  * THE MEMCPY SCATTER GOES STRAIGHT INTO THE DESTINATION. C's
//    `memcpy(raw, &obj, sizeof obj)` has no source region for the typed
//    model to borrow -- the object representation is exactly what the
//    typed model never materializes -- so nothing is resolved as a source
//    at all. Each scalar component is split with `T::to_ne_bytes` and
//    written at the component's C byte offset PAST the destination
//    cursor: bytes 0..3 from `a`, 4..7 from `b`, 8..15 from `c` for
//    `{int; int; double;}`, and a `memcpy(w + 8, &x, 4)` lands at 8..11.
//    The offsets are READ off `emitrust.abi_layout` -- clang's OWN
//    ASTRecordLayout -- so nothing here depends on rustc laying the
//    struct out the way clang did, which is precisely why this is sound
//    where a `transmute` is not.
//
//  * THE DOMAIN CROSSING IS A COPY, AND ONLY WHEN IT HAS TO BE. `char
//    raw[4]` is `[i8; 4]` and the callee wants `&[u8]`;
//    `emitrust.slice_of` refuses to bridge those at the verifier ("result
//    slice element type 'ui8' does not match the base element type
//    'i8'"), so the bridge is a materialized `[u8; 4]` with a per-byte
//    `as u8`. An array that is ALREADY `[u8; N]` must NOT be copied --
//    `view_u8_addr` below borrows the region itself -- because a needless
//    copy would silently change what a mutating callee writes through.
//
//  * THE `&arr` SPELLING IS THE DECAY SPELLING. `view_decay` and
//    `view_addr` are the same C program written two ways and must lower
//    identically; the importer only ever had a lowering for the decayed
//    form, and `&arr` was a flat rejection whatever the element type was.
//
//  * THE WRITE-BACK. A MUTABLE (`&mut [u8]`) byte-slice parameter means
//    the callee may write through the view; since the view is a COPY the
//    array must be restored from it after the call or the write is
//    silently lost -- the same measured miscompile class Wave 1 pinned
//    (native -1431655766 against a naive Rust 1) from a crate that builds
//    clean. The runtime proof is test/EndToEnd/byte-view-array-domain.c;
//    what is pinned HERE is that the restore is emitted at all, and that
//    it is the exact inverse cast of the copy-in.
//
//  * ZERO `unsafe`. The whole point of the copy-in/copy-out shape is that
//    it DISCHARGES the object-representation obligation rather than
//    relocating it into an `unsafe` block; `--implicit-check-not=unsafe`
//    on every prefix is what keeps that honest.
//
// RUN: split-file %s %t
// RUN: emitrust-cc --emit=rust %t/memcpy-source.c -o - | FileCheck %s --check-prefix=MC --implicit-check-not=unsafe
// RUN: emitrust-cc --emit=rust %t/array-view.c -o - | FileCheck %s --check-prefix=AV --implicit-check-not=unsafe
// RUN: emitrust-import-c %t/memcpy-source.c | FileCheck %s --check-prefix=LAYOUT

//--- memcpy-source.c
#include <stdio.h>
#include <string.h>

/* 4 + 4 + 8 == 16 == sizeof: padding-free, so every byte of the image has
   a determinate value. */
struct Pair {
  int a;
  int b;
  double c;
};

static void ph(const unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}

void copy_int(int x) {
  unsigned char raw[4];
  memcpy(raw, &x, sizeof(x));
  ph(raw, sizeof(raw));
}
// MC-LABEL: pub fn copy_int(
// MC: let mut raw: [u8; 4] = [0; 4];
// MC: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(x);
// MC: let [[SRC:v[0-9]+]]: [u8; 4] =
// MC-NEXT: raw[0i64 as usize] = [[SRC]][0i64 as usize];
// MC-NEXT: raw[1i64 as usize] = [[SRC]][1i64 as usize];
// MC-NEXT: raw[2i64 as usize] = [[SRC]][2i64 as usize];
// MC-NEXT: raw[3i64 as usize] = [[SRC]][3i64 as usize];

void copy_pair(int a) {
  struct Pair p;
  unsigned char raw[16];
  p.a = a;
  p.b = 3;
  p.c = 2.5;
  memcpy(raw, &p, sizeof(p));
  ph(raw, sizeof(raw));
}
// Each field is split by its OWN type and lands at its OWN clang offset,
// in C declaration order: 0..3, 4..7, 8..15.
// MC-LABEL: pub fn copy_pair(
// MC: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(p.a);
// MC: let [[A:v[0-9]+]]: [u8; 4] =
// MC-NEXT: raw[0i64 as usize] = [[A]][0i64 as usize];
// MC: raw[3i64 as usize] = [[A]][3i64 as usize];
// MC: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(p.b);
// MC: let [[B:v[0-9]+]]: [u8; 4] =
// MC-NEXT: raw[4i64 as usize] = [[B]][0i64 as usize];
// MC: raw[7i64 as usize] = [[B]][3i64 as usize];
// MC: let {{v[0-9]+}}: [u8; 8] = f64::to_ne_bytes(p.c);
// MC: let [[C:v[0-9]+]]: [u8; 8] =
// MC-NEXT: raw[8i64 as usize] = [[C]][0i64 as usize];
// MC: raw[15i64 as usize] = [[C]][7i64 as usize];

void copy_window(int x) {
  unsigned char w[16];
  int i;
  for (i = 0; i < 16; i++)
    w[i] = 0;
  memcpy(w + 8, &x, sizeof(x));
  ph(w, 16);
}
// A NONZERO destination cursor: the image starts where the cursor points,
// not at 0.
// MC-LABEL: pub fn copy_window(
// MC: let {{v[0-9]+}}: [u8; 4] = i32::to_ne_bytes(x);
// MC: let [[W:v[0-9]+]]: [u8; 4] =
// MC-NEXT: w[8i64 as usize] = [[W]][0i64 as usize];
// MC-NEXT: w[9i64 as usize] = [[W]][1i64 as usize];
// MC-NEXT: w[10i64 as usize] = [[W]][2i64 as usize];
// MC-NEXT: w[11i64 as usize] = [[W]][3i64 as usize];

// The offsets the scatter uses are clang's own, parked on the struct_def
// by annotateAbiFaithfulness; there is no second layout model here to
// disagree with the one the emitted const-assertions check rustc against.
// LAYOUT: emitrust.abi_layout = {align = 8 : i64, offsets = [0, 4, 8], size = 16 : i64}

//--- array-view.c
#include <stdio.h>

static void ph(const unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}

static void zap(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = 0xaa;
}

/* Capability D: `char[4]` is `[i8; 4]` and the callee wants `&[u8]`. */
void view_decay(int s) {
  char raw[4];
  raw[0] = (char)s;
  raw[1] = 1;
  raw[2] = 2;
  raw[3] = 3;
  ph((const unsigned char *)raw, sizeof(raw));
}
// AV-LABEL: pub fn view_decay(
// AV: let mut raw: [i8; 4] = [0; 4];
// AV: let {{v[0-9]+}}: [u8; 4] = __emitrust_bytes_as_u8(raw);
// AV: let [[IMG:v[0-9]+]]: [u8; 4] =
// AV-NEXT: let {{v[0-9]+}}: &[u8] = &[[IMG]][0i64 as usize..];

/* Capability E: the SAME program with the `&arr` spelling. It must lower
   identically -- in C the two denote the same address. */
void view_addr(int s) {
  char raw[4];
  raw[0] = (char)s;
  raw[1] = 1;
  raw[2] = 2;
  raw[3] = 3;
  ph((const unsigned char *)&raw, sizeof(raw));
}
// AV-LABEL: pub fn view_addr(
// AV: let mut raw: [i8; 4] = [0; 4];
// AV: let {{v[0-9]+}}: [u8; 4] = __emitrust_bytes_as_u8(raw);
// AV: let [[IMG2:v[0-9]+]]: [u8; 4] =
// AV-NEXT: let {{v[0-9]+}}: &[u8] = &[[IMG2]][0i64 as usize..];

/* An array already in the u8 domain: `&arr` joins the decay spelling's
   lowering and NO copy is made. A needless copy here would change what a
   mutating callee writes through. */
void view_u8_addr(int s) {
  unsigned char raw[4];
  raw[0] = (unsigned char)s;
  raw[1] = 1;
  raw[2] = 2;
  raw[3] = 3;
  ph((const unsigned char *)&raw, sizeof(raw));
}
// AV-LABEL: pub fn view_u8_addr(
// AV: let mut raw: [u8; 4] = [0; 4];
// AV-NOT: __emitrust_bytes_as_u8
// AV: let {{v[0-9]+}}: &[u8] = &raw[0i64 as usize..];

/* The write-back. `zap` takes `&mut [u8]`, so the mutated image must be
   cast back into `raw` -- the exact inverse of the copy-in -- before the
   program reads `raw` again. */
void view_writeback(int s) {
  char raw[4];
  raw[0] = (char)s;
  raw[1] = 1;
  raw[2] = 2;
  raw[3] = 3;
  zap((unsigned char *)raw, sizeof(raw));
  printf("%d\n", (int)raw[0]);
}
// AV-LABEL: pub fn view_writeback(
// AV: let {{v[0-9]+}}: [u8; 4] = __emitrust_bytes_as_u8(raw);
// AV: let mut [[MUT:v[0-9]+]]: [u8; 4] =
// AV: let [[REF:v[0-9]+]]: &mut [u8] = &mut [[MUT]][0i64 as usize..];
// AV-NEXT: tu0_zap([[REF]],
// AV-NEXT: let [[BACK:v[0-9]+]]: [i8; 4] = __emitrust_bytes_as_i8([[MUT]]);
// AV-NEXT: raw = [[BACK]];

// The two bridge helpers are each other's exact inverse, and both are
// ordinary safe Rust: `as` between i8 and u8 is defined as the identity
// on the bit pattern, so the round trip is the identity on every byte the
// callee did not touch.
// AV: fn __emitrust_bytes_as_u8<const N: usize>(s: [i8; N]) -> [u8; N] {
// AV: o[i] = s[i] as u8;
// AV: fn __emitrust_bytes_as_i8<const N: usize>(s: [u8; N]) -> [i8; N] {
// AV: o[i] = s[i] as i8;
