# SPIKE-HC (header-class import), started 2026-11-03

status: setting up

## Population (baseline record 4f2a2f5 --normalize --project-root)
66 non-FunctionDecl / member-function-tagged functions; the target set = 49 whose ONLY tag is
`member function` (36), `CXXConstructorDecl` (9) or `CXXDestructorDecl` (4), in 9 TUs:
woff2 font.cc, woff2_out.cc; brunsli context.cc, ans_decode.cc, brunsli_decode.cc,
huffman_decode.cc, state.cc, brunsli_encode.cc, write_bits.cc.
(17 more are main-file-class members in transform.cc GlyfEncoder / jpeg_data_reader.cc
BitReaderState, already observed with struct rows.)

Anchor: cxx_import.py attach_definition (~L1268 at 0acec89): parent record not in
record_path -> `friendly(kind)` = "member function" / CXXConstructorDecl. This is the line.

## Instrument plan (BX_HC_SPIKE=1, worktree only)
lazy import of header records that parent a main-file out-of-line def; private members
reported but kept; bad-record early return skipped; header inline method bodies imported
in_file; class rows = header diags inside the class's range, attributed to every member def.

## Progress 1 (instrument works)
Instrument = BX_HC_SPIKE=1 patch in /tmp/wt-hc-spike/tools/emitrust-bx/emitrust_bx/cxx_import.py
(diff vs HEAD = the spike). Driver /tmp/hc/drive.py -> /tmp/hc/hc3.json (9 TUs).
INSTRUMENT DEFECT FOUND+FIXED in my own patch: clang JSON `type.decl` reference objects
({id,kind:CXXRecordDecl,name}) are walked by _FileTracker.visit, so decl_file for a record
id is overwritten by the file of its LAST USE; key header-record lookup on completeDefinition.
(Any implementer adding decl_file for records hits this.)
First look: EntropySource::AddCode has ZERO residuals once its class is imported; class rows
of EntropySource = private members only. But it calls Histogram::Add (array field) -> transitive.

## Progress 2
Field rows now report EVERY problem per field (bx's shell_field reports only the FIRST:
NSDMI hides the type, const hides pointer -- `uint8_t* const data` is const+pointer).
Enum-typed fields classified from the AST's EnumDecl names. Per-function sets: /tmp/hc/full5.json
(strict = own body + own class rows + rows of HC classes it holds by value; trans = + rows of
main-file callees, transitively). Instrument artifacts filtered: ctor ExprStmt/fall-off, dtor
`~X` identifier + implicit noexcept; `use of a rejected data member` / `return with a value in
a void function` dropped as cascades of a listed row.

## Progress 3 -- results so far (strict = frontier metric; all sets EXCLUDE the base
## increment "import the header class + its out-of-line methods", which all 49 need)
- 0 of 15 imported header classes is importable under FR-257 today. 1-row classes:
  Histogram (int[18] field), EntropySource (private; + Histogram by value), HuffmanDecodingData
  (vector<HuffmanCode>, HuffmanCode an unimported header POD).
- No single row unlocks any function (best 1-row: +0 after hand-correcting IsReused, a
  nested-class member the instrument could not open). Best 2: array field+subscript +2
  (Histogram::AddHistogram, Histogram::Merge). Best 4: + DCHECK abort + private = +5
  (adds Histogram::Add, EntropySource::AddCode, EntropySource::Merge). Same under the
  transitive metric.
- LEAD (private + arrays + enums >= 15) REFUTED: strict 3, transitive 2.
- DIFFERENTIAL PROBE /tmp/hc/probe2/hv.cpp: those 5 bodies VERBATIM with int[18] -> vector<int>
  and DCHECK dropped: today's bx accepts, --check-roundtrip OK, crate builds, byte-identical vs
  clang++ ASan/UBSan, 0 unsafe. So the 5-function set is MEASURED, not inferred.
- EMISSION PROBE /tmp/hc/probe1: hand-written [i32; 18] Rust for Histogram byte-identical vs native.

## RESULT (2026-11-03)
VERDICT: NO-GO on the lead; NO-GO for header-class import as a yield play on these 49.
Narrow GO-with-scope only if [T; N] is wanted for its own reasons (see report).

### Per-function full blocker sets (LOWER BOUNDS; every set additionally needs the base
### increment "lazy header-class import + out-of-line methods")
| TU | function | class | n | strict blocker set (own body + class rows) | extra rows via callees |
|---|---|---|--:|---|---|
| font.cc | FindTable | Font | 5 | ?: of non-scalar type, local non-kernel, return of non-kernel type, cls:map-field, cls:nested-class |  |
| font.cc | FindTable | Font | 6 | ?: of non-scalar type, local non-kernel, method-overloading, return of non-kernel type, cls:map-field, cls:nested-class |  |
| font.cc | OutputOrderedTags | Font | 5 | local non-kernel, other: std::vector::erase (iterators and pointers are outside the subset), other: std::vector::insert (iterators and pointers are outside the subset), cls:map-field, cls:nested-class |  |
| font.cc | IsReused | Font::Table (nested; hand-derived) | 4 | pointer: null (R6), cls:map-field, cls:nested-class, cls:pointer-field |  |
| woff2_out.cc | WOFF2StringOut() | WOFF2StringOut | 4 | ctor-body, cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | Write | WOFF2StringOut | 5 | method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private | member access through a pointer |
| woff2_out.cc | Write | WOFF2StringOut | 6 | member access through a pointer, method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | SetMaxSize | WOFF2StringOut | 3 | cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | WOFF2MemoryOut() | WOFF2MemoryOut | 4 | ctor-body, cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | Write | WOFF2MemoryOut | 5 | method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private | libcall:memcpy |
| woff2_out.cc | Write | WOFF2MemoryOut | 6 | libcall:memcpy, method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private |  |
| context.cc | InitAll | ComponentStateDC | 8 | typing: counting-loop canonical form, cls:ctor-body(inline), cls:field-of-unimported-class, cls:inline-body:member access through a pointer, cls:inline-body:object construction, cls:inline-body:other: std::vector::resize without a fill value, cls:private, cls:vector<non-kernel>-field |  |
| context.cc | InitAll | ComponentState | 12 | array-subscript, DCHECK-abort, local non-kernel, typing: counting-loop canonical form, cls:array-field, cls:ctor-body(inline), cls:inline-body:member access through a pointer, cls:inline-body:object construction, cls:inline-body:other: std::vector::resize without a fill value, cls:inline-body:sizeof/alignof, cls:private, cls:vector<non-kernel>-field |  |
| ans_decode.cc | Init | ANSDecodingData | 5 | comma operator, member access(unimported class), cls:array-field, cls:ctor-body(inline), cls:inline-body:object construction |  |
| brunsli_decode.cc | BrunsliDecoder() | BrunsliDecoder | 9 | ctor-body, member access through a pointer, object construction, param non-kernel, cls:ctor-overload/copy, cls:destructor, cls:enum-in-class, cls:private, cls:unique_ptr-field |  |
| brunsli_decode.cc | ~BrunsliDecoder() | BrunsliDecoder | 5 | cls:ctor-overload/copy, cls:destructor, cls:enum-in-class, cls:private, cls:unique_ptr-field |  |
| brunsli_decode.cc | Decode | BrunsliDecoder | 14 | dereference, enumerator, DCHECK-abort, local non-kernel, member access(unimported class), param non-kernel, pointer: null (R6), return of non-kernel type, switch statement, cls:ctor-overload/copy, cls:destructor, cls:enum-in-class, cls:private, cls:unique_ptr-field | 45 rows |
| huffman_decode.cc | ReadFromBitStream | HuffmanDecodingData | 16 | array-subscript, array-to-pointer decay, assignment to something other than a local variable, compound assignment to something other than a local variable, declaration without an initializer, default argument, hdr-fn-rejected, libcall:BrunsliBitReaderIsHealthy, libcall:BuildHuffmanTable, local non-kernel, member access(unimported class), member access through a pointer, param non-kernel, pointer: null (R6), static local variable, cls:vector<non-kernel>-field | libcall:memcpy, libcall:memset, switch statement, typing: counting-loop canonical form |
| huffman_decode.cc | ReadSymbol | HuffmanDecodingData | 8 | assignment to something other than a local variable, compound assignment to something other than a local variable, declaration without an initializer, hdr-fn-rejected, local non-kernel, member access(unimported class), param non-kernel, cls:vector<non-kernel>-field |  |
| state.cc | State() | State | 9 | ctor-body, object construction, cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | State() | State | 8 | struct: definition of a rejected member, cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | ~State() | State | 7 | cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | ApplyOptions | State | 8 | param non-kernel, cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | ~MetadataState() | MetadataState | 8 | libcall:BrotliDecoderDestroyInstance, pointer: null (R6), cls:NSDMI, cls:destructor, cls:enum-field, cls:enum-in-class, cls:field-of-unimported-class, cls:pointer-field |  |
| brunsli_encode.cc | Histogram() | Histogram | 3 | ctor-body, member access through a pointer, cls:array-field | libcall:memset |
| brunsli_encode.cc | Clear | Histogram | 2 | libcall:memset, cls:array-field |  |
| brunsli_encode.cc | AddHistogram | Histogram | 2 | array-subscript, cls:array-field |  |
| brunsli_encode.cc | Add | Histogram | 3 | array-subscript, DCHECK-abort, cls:array-field |  |
| brunsli_encode.cc | Merge | Histogram | 2 | array-subscript, cls:array-field |  |
| brunsli_encode.cc | Resize | EntropySource | 3 | other: std::vector::resize without a fill value, cls:array-field, cls:private |  |
| brunsli_encode.cc | AddCode | EntropySource | 2 | cls:array-field, cls:private | array-subscript, DCHECK-abort |
| brunsli_encode.cc | Finish | EntropySource | 5 | explicit cast to non-kernel type, other: std::vector::swap, return of non-kernel type, cls:array-field, cls:private |  |
| brunsli_encode.cc | Merge | EntropySource | 3 | DCHECK-abort, cls:array-field, cls:private | array-subscript |
| brunsli_encode.cc | EntropyCodes() | EntropyCodes | 7 | ctor-body, libcall:ClusterHistograms, object construction, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | EncodeContextMap | EntropyCodes | 6 | libcall:EncodeContextMap, param non-kernel, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | BuildAndStoreEntropyCodes | EntropyCodes | 6 | libcall:BuildAndStoreANSEncodingData, param non-kernel, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | GetANSTable | EntropyCodes | 5 | return of non-kernel type, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | DataStream() | DataStream | 6 | ctor-body, object construction, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | Resize | DataStream | 4 | cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | ResizeForBlock | DataStream | 6 | global variable, static local variable, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | AddCode | DataStream | 8 | DCHECK-abort, local non-kernel, member access(unimported class), normalize: incdec operand, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field | array-subscript, cls:array-field |
| brunsli_encode.cc | AddBits | DataStream | 6 | local non-kernel, member access(unimported class), cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | FlushArithmeticCoder | DataStream | 5 | member access(unimported class), cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | FlushBitWriter | DataStream | 5 | member access(unimported class), cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | AddBit | DataStream | 7 | member access(unimported class), member access through a pointer, param non-kernel, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | EncodeCodeWords | DataStream | 12 | hdr-fn-rejected, implicit conversion, local non-kernel, member access(unimported class), param non-kernel, reinterpret_cast, sizeof/alignof, typing: counting-loop canonical form, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field | return of non-kernel type, cls:array-field |
| write_bits.cc | Storage() | Storage | 6 | ctor-body, DCHECK-abort, member access through a pointer, cls:const-field, cls:destructor, cls:pointer-field |  |
| write_bits.cc | AppendBytes | Storage | 5 | DCHECK-abort, libcall:memcpy, cls:const-field, cls:destructor, cls:pointer-field |  |
| write_bits.cc | ~Storage() | Storage | 4 | DCHECK-abort, cls:const-field, cls:destructor, cls:pointer-field |  |

### Set cover over FUNCTIONS (strict = frontier metric; transitive adds callees' rows)
- best 1 row: +0 | best 2: array field + subscript +2 (Histogram::AddHistogram, ::Merge)
- best 3: + DCHECK abort +3 (adds Histogram::Add) | best 4: + private +5 (EntropySource::AddCode, ::Merge)
- array + private (no abort): strict +3, transitive +2
- greedy 12 rows -> 14 strict, 13 transitive
- lead set (private + arrays + enums): strict 3, transitive 2  -> REFUTED (>=15 claimed)
- row FREQUENCY is not yield: `private` is on 29 functions and unlocks 0 alone.

### Phantom-EMIT probe (paired frontier, frozen snapshot /tmp/hc/snap)
snapshot with flags off reproduces 35/392 exactly (EMIT set == baseline record).
BX_CASCADE_SPIKE=1 (a use of a field/method of a rejected struct is a residual): 35 -> 35, 0 lost.
Defect exists (/tmp/hc/probe3/c.cpp: `user(const S&)` calling a method of a rejected
struct is counted clean) but costs 0 today; it becomes live once header classes import.
