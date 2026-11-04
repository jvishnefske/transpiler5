# SPIKE: Kernel ENUM row (2026-11-03) -- scratch notes, NOT committed

## Step 0: frontier at 4f2a2f5 (front-head record)
- 33 functions carry `enumerator`; enumerator is the ONLY tag on 2 (woff2 Woff2Compress, TextCompress).
- Those 2 call woff2::Compress, which is rejected (param BrotliEncoderMode + call to library
  function BrotliEncoderCompress -- an external C library function, permanently out of bx).
  PROBE p2.cpp: bx emits NO diagnostic on a call to a rejected main-file function
  (cascade exists for structs/templates/header fns, NOT main-file fns). So the 2 would score
  EMIT strictly but are phantoms transitively (callee never emitted -> crate unbuildable).
- BRUNSLI_DCHECK: frontier compiles without NDEBUG, so platform.h:29 defines BRUNSLI_DEBUG and
  DCHECK(V) = BRUNSLI_CHECK(V) = `if (!(V)) { ::brunsli::BrunsliDumpAndAbort(...); while (true) {} }`.
  The 7 "DCHECK enumerator" sites are enumerators inside the condition (diag located at the
  macro expansion). Under NDEBUG it is empty. This is FR-267's "DCHECK abort row".

## Step 1: real uses on the frontier (source read, brunsli state_internal.h / status.h)
- BrunsliStatus = `typedef enum { BRUNSLI_OK = 0, ... }` (ANONYMOUS enum + typedef) in namespace brunsli,
  project header c/include/brunsli/status.h. Returned/passed/stored in locals, compared ==/!=.
- Class-nested state enums (VarintState::Stage, JpegInternalsState::Stage, MetadataState::Stage, ...)
  are stored in `size_t stage = READ_TAG;` / `Stage stage = INIT;` DATA MEMBERS, and used as
  BIT MASKS: `js.stage & JpegInternalsState::DECODE_HUFFMAN_MASK` (brunsli_decode.cc:1475,1495).
  i.e. enumerators used as named integer constants, not as a closed type.
- `enum struct Stage`, `enum struct SerializationStatus` (state.h:47,57): scoped, int underlying.
- woff2: `typedef enum BrotliEncoderMode {...}` in brotli/c/include/brotli/encode.h -- a dep include
  dir UNDER --project-root=/tmp/fr264/fsrc (so "project header" by path).

## Step 2: bijectivity probes (/tmp/enumspk/bij/e.cpp: anon+typedef, gaps, duplicate A=B=1,
## dependent C=A+B, scoped, bitmask into size_t, negative, s+1, s==0, static_cast<Stage>(42))
- native clang++ -fsanitize=address,undefined,enum and g++ -fsanitize=address,undefined: identical.
- (A) `#[repr] enum`: E0081 (duplicate discriminant), E0605 (`v as Stage` -- int->enum needs
  transmute = unsafe), plus mixed u32/i32 arithmetic errors. REFUSES 2 legal, frontier-relevant
  shapes (int->scoped cast; bitmask arithmetic needs `as` everywhere).
- (B) newtype `struct Status(pub u32)` + unscoped enumerators as module consts, scoped as
  associated consts `impl Stage { pub const ERROR: Stage = Stage(3); }`: byte-identical stdout.
- (C) erasure: enum type -> integer type, enumerators -> FR-264 consts: byte-identical stdout.

## Step 3: paired frontier (instrument BX_ENUM_SPIKE=1: enum types -> i32, enumerator -> IntLit
## of its value, namespace-scope EnumDecl/typedef-of-enum accepted). base 45f5a36 vs instrument.
- EMIT 35 -> 37 of 392, 0 lost. Gained: woff2::Woff2Compress, woff2::TextCompress ONLY.
- brunsli: 0 gained. 30 brunsli carriers keep >= 1 blocker; nearest are CheckBoundary and
  DecodeBase128 with residual {parameter of type 'State *'} only (State = class, FR-267).
- Exposed tags (were hidden): `may fall off end` x4 (switch-over-enum functions), `call to library
  function` x1 (Woff2Uncompress, BrotliDecoderDecompress), `pointer to pointer` x1 (WriteJpeg).
- PHANTOMS: both gains call woff2::Compress, which stays rejected (call to library function
  BrotliEncoderCompress -- brotli encoder, a separate C TU, never reachable by safe bx).
  Base already has 2 such phantoms (TransformAppMarker, WriteHuffmanTree). A call to a rejected
  main-file function is NOT a cascade in bx (probe p2.cpp) -> frontier EMIT over-counts.
- Records: /tmp/enumspk/front-base, /tmp/enumspk/front-enum, residuals /tmp/enumspk/residual.txt

## Step 4: usage census over all frontier TUs (clang JSON scan, /tmp/enumspk/scan/scan.py; functions
## deduped by file|name INCLUDING header-defined and class methods -- frequency, NOT yield)
  73 (664) enumerator | 33 (261) enum-typed data member access | 30 (222) implicit enum->int
  18 (35) enum-typed local | 10 (60) implicit enum->size_t | 9 (12) enum-typed parameter
  9 (11) switch over enum value | 7 (7) switch over int with enumerator labels | 5 (53) enum FIELDs
  1 (2) bitmask `&` on enumerator | 0 explicit cast TO enum | 0 explicit cast FROM enum | 0 printf
  20 project enum decls: 8 brotli C enums in extern "C" (LinkageSpecDecl), 5 class-nested, 0 with
  enumerator-dependent initializers.
## Step 5: out-of-range: `static_cast<Color>(42)` for unfixed `enum Color{R,G,B}` -> UBSan
  -fsanitize=enum "load of value 42, which is not a valid value for type 'Color'"; fixed `enum : int`
  clean. Rust->C++ round trips of (B) [fixed-type enum] and (C) [const int + namespace Stage] are
  byte-identical under g++ ASan/UBSan and clang++ ASan/UBSan/enum.
## Step 6: local `int OK = 3;` shadowing an enumerator: legal C++ (prints 3); Rust with
  `const OK: Status` in scope -> E0308 (pattern, not binding). FR-264's "variable 'k' has the
  name of a constant" refusal must cover enumerators.
