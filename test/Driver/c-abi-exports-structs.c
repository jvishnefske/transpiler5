// FR-182: `--c-abi-exports` admits exactly TWO struct shapes across the C ABI,
// and gives every struct it lets across `#[repr(C)]` plus a build-time layout
// backstop. FR-139 exported only all-scalar signatures; this file pins the
// widening and, just as importantly, its edges.
//
// Why `#[repr(C)]` is a CORRECTNESS requirement and not a style choice: the
// default Rust repr is free to reorder fields and MEASURABLY does. The FR-181
// spike ran `flac_validate` across a dlopen boundary against the clang native
// on 6 corpus vectors -- with `#[repr(C)]` byte-identical, without it wrong
// and SILENT. Nothing catches that on its own: `struct tflac` is the same SIZE
// under both reprs, and `improper_ctypes_definitions` does not fire for a raw
// pointer to a non-`repr(C)` struct. Only the offsets differ.
//
// Five invariants are pinned here.
//
// 1. CLASS 0, by value. Every parameter and the result is a scalar or an
//    ABI-FAITHFUL struct passed/returned by value. Plain `#[no_mangle]
//    extern "C"`, ZERO `unsafe`: Rust's `extern "C"` lays a `#[repr(C)]`
//    struct argument out the way the platform C ABI says, so nothing needs
//    wrapping.
//
// 2. CLASS 1, one struct pointer. Exactly ONE parameter is a reference to an
//    ABI-faithful struct and everything else is a scalar. C has a POINTER
//    there where the translated function has a Rust reference, so the two
//    cannot be the same item: the function keeps its own name and its body
//    TEXTUALLY UNCHANGED, and a one-statement `#[export_name] unsafe
//    extern "C"` wrapper carries the bare C symbol. The `unsafe` is confined
//    to that generated item.
//
//    A HARD STRUCTURAL CAP rides this class: at most ONE reference parameter,
//    ever (pinned in c-abi-exports-structs-refused.c). Two are `noalias` to
//    LLVM and a C caller may legally alias them -- a measured miscompile.
//
// 3. The const-assert backstop. Every `#[repr(C)]` struct carries CLANG's own
//    size, alignment and per-field byte offsets, read from its ASTRecordLayout
//    at import and asserted at const-eval time. A layout rustc disagrees with
//    is `error[E0080]` at `cargo build` -- the repo's mandated hard-error
//    direction -- and never a wrong answer across the boundary.
//
// 4. TRANSITIVITY. An exported struct's layout depends on the layout of every
//    struct it CONTAINS, so `struct span { lm_vec2 lo, hi; }` being exported
//    means `lm_vec2` must be `#[repr(C)]` too. This is a correctness
//    requirement, not an optimisation: a non-`repr(C)` inner struct makes the
//    outer struct's asserted offsets meaningless.
//
// 5. WITHOUT the flag NOTHING moves. The flag defaults OFF precisely so every
//    `--emit=crate` golden stays byte-identical, so this is checked by DIFF
//    and not by inspection: strip the four additions (`#[repr(C)]`, the
//    const-asserts, `#[no_mangle]`/`extern "C"`, and the wrapper epilogue,
//    which is a pure module-scope APPEND) and the default crate root comes
//    back byte for byte.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
//
// Nothing is refused in this file, so the flagged run is silent.
// RUN: not grep . %t.cabi.err
//
// The exported set is exactly the five functions, and `unsafe` appears
// exactly three times -- once per class-1 wrapper, never in a class-0 export
// and never in a translated body.
// RUN: grep -c no_mangle %t.cabi/src/lib.rs > %t.cabi.count
// RUN: grep -c export_name %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: grep -c unsafe %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: grep -c 'repr(C)' %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count
//
// The class-1 functions themselves never take the C ABI: their Rust reference
// is not C's pointer, and giving them `extern "C"` is the FR-138 mismatch this
// whole feature exists to avoid.
// RUN: not grep 'extern "C" fn flac_validate' %t.cabi/src/lib.rs
// RUN: not grep 'extern "C" fn span_width' %t.cabi/src/lib.rs
//
// The byte-identity guard.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err
// RUN: not grep 'repr(C)' %t.plain/src/lib.rs
// RUN: not grep 'const _: ()' %t.plain/src/lib.rs
// RUN: not grep no_mangle %t.plain/src/lib.rs
// RUN: not grep export_name %t.plain/src/lib.rs
// RUN: not grep unsafe %t.plain/src/lib.rs
// RUN: not grep 'extern "C"' %t.plain/src/lib.rs
//
// ...and the flagged root differs from it in NOTHING but the four additions.
// RUN: sed '/^#\[export_name/,$d' %t.cabi/src/lib.rs \
// RUN:   | grep -v '^#\[repr(C)\]$' \
// RUN:   | grep -v '^const _: () = assert!' \
// RUN:   | grep -v no_mangle \
// RUN:   | sed 's/^pub extern "C" fn /pub fn /' > %t.cabi.stripped
// RUN: diff %t.plain/src/lib.rs %t.cabi.stripped

// A faithful two-float record: both members are scalars mapped to same-width
// Rust primitives, so the emitted field list IS the C one.
typedef struct { float x; float y; } lm_vec2;

// CLASS 0: struct in, struct out, by value.
lm_vec2 lm_add(lm_vec2 a, lm_vec2 b) {
  lm_vec2 r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

// CLASS 0 with a scalar result: the mixture is admitted in both directions.
float lm_dot(lm_vec2 a, lm_vec2 b) { return a.x * b.x + a.y * b.y; }

// The corpus shape FR-181 measured: mixed-width scalars with interior
// padding, so the offsets are not derivable by counting field sizes.
struct tflac {
  unsigned int blocksize;
  unsigned int samplerate;
  unsigned char channel_mode;
  unsigned char partition_order;
  unsigned int cur_blocksize;
};

// CLASS 1: one struct pointer beside a scalar, scalar result.
int flac_validate(struct tflac *t, int n) {
  if (t->blocksize < 16u)
    return -1;
  t->cur_blocksize = t->blocksize + (unsigned)n;
  return 0;
}

// CLASS 1 with no other parameter at all.
unsigned flac_peek(const struct tflac *t) {
  return t->cur_blocksize + t->channel_mode;
}

// TRANSITIVITY: exporting `span_width` reaches `struct span`, whose layout
// depends on `lm_vec2`'s, so BOTH are `#[repr(C)]`.
struct span { lm_vec2 lo; lm_vec2 hi; };
float span_width(struct span *s) { return s->hi.x - s->lo.x; }

// CABI:      #[repr(C)]
// CABI-NEXT: #[derive(Clone, Copy, Default)]
// CABI-NEXT: pub struct LmVec2 {
// CABI-NEXT:     pub x: f32,
// CABI-NEXT:     pub y: f32,
// CABI-NEXT: }
// CABI-NEXT: const _: () = assert!(core::mem::size_of::<LmVec2>() == 8);
// CABI-NEXT: const _: () = assert!(core::mem::align_of::<LmVec2>() == 4);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(LmVec2, x) == 0);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(LmVec2, y) == 4);

// Class 0: the C ABI on the item itself, and no `unsafe` anywhere near it.
// CABI-NEXT: #[no_mangle]
// CABI-NEXT: pub extern "C" fn lm_add(v0: LmVec2, v1: LmVec2) -> LmVec2 {
// CABI:      #[no_mangle]
// CABI-NEXT: pub extern "C" fn lm_dot(v0: LmVec2, v1: LmVec2) -> f32 {

// The padding really is clang's: `channel_mode` and `partition_order` are
// adjacent bytes and `cur_blocksize` starts at 12, not 10.
// CABI:      #[repr(C)]
// CABI-NEXT: #[derive(Clone, Copy, Default)]
// CABI-NEXT: pub struct Tflac {
// CABI:      const _: () = assert!(core::mem::size_of::<Tflac>() == 16);
// CABI-NEXT: const _: () = assert!(core::mem::align_of::<Tflac>() == 4);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Tflac, blocksize) == 0);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Tflac, samplerate) == 4);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Tflac, channel_mode) == 8);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Tflac, partition_order) == 9);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Tflac, cur_blocksize) == 12);

// Class 1: the translated function keeps the plain `pub fn` and the Rust
// reference it has always had.
// CABI-NEXT: pub fn flac_validate(t: &mut Tflac, n: i32) -> i32 {
// CABI:      pub fn flac_peek(t: &mut Tflac) -> u32 {

// TRANSITIVITY: `Span` is exported, so `LmVec2` above had to be repr(C) too.
// CABI:      #[repr(C)]
// CABI-NEXT: #[derive(Clone, Copy, Default)]
// CABI-NEXT: pub struct Span {
// CABI-NEXT:     pub lo: LmVec2,
// CABI-NEXT:     pub hi: LmVec2,
// CABI-NEXT: }
// CABI-NEXT: const _: () = assert!(core::mem::size_of::<Span>() == 16);
// CABI-NEXT: const _: () = assert!(core::mem::align_of::<Span>() == 4);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Span, lo) == 0);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Span, hi) == 8);
// CABI-NEXT: pub fn span_width(s: &mut Span) -> f32 {

// The wrapper epilogue: one statement each, the bare C symbol supplied by
// `#[export_name]` (the ITEM cannot be named for the symbol -- the translated
// function already holds that name and a second item of it is rustc E0428).
// CABI:      #[export_name = "flac_validate"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_flac_validate(t: *mut Tflac, n: i32) -> i32 {
// CABI-NEXT:     flac_validate(&mut *t, n)
// CABI-NEXT: }
// CABI-NEXT: #[export_name = "flac_peek"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_flac_peek(t: *mut Tflac) -> u32 {
// CABI-NEXT:     flac_peek(&mut *t)
// CABI-NEXT: }
// CABI-NEXT: #[export_name = "span_width"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_span_width(s: *mut Span) -> f32 {
// CABI-NEXT:     span_width(&mut *s)
// CABI-NEXT: }

// no_mangle: the two class-0 exports. export_name: the three class-1
// wrappers. unsafe: the same three wrappers and nothing else -- two
// occurrences per wrapper would mean an `unsafe` block leaked into a body.
// repr(C): the three reachable structs.
// COUNT:      2
// COUNT-NEXT: 3
// COUNT-NEXT: 3
// COUNT-NEXT: 3
