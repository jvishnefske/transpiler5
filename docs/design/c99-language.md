### Types

- [x] C99-1 Signed integer types char, short, int, long, long long as
  i8/i16/i32/i64, _Bool as bool, float and double as f32/f64.
  (test/Import/C/scalars.c, test/EndToEnd/float.c)
- [x] C99-2 Unsigned integer types mapping to u8/u16/u32/u64, including
  unsigned literals, wrap-around semantics, and the unsigned arithmetic,
  comparison, division, remainder, and shift operations end to end through
  the conversion layer. Dialect/emission layer: unsigned-typed
  emitrust.add/sub/mul render as the wrapping_add/wrapping_sub/wrapping_mul
  method calls (C wrap-around; Rust infix panics on debug overflow), while
  div/rem/cmp/bitwise/shift keep the infix operators whose uN semantics
  already match C, and getDefaultValueAttr covers unsigned types (0 : uN).
  The conversion layer guards the unsigned-semantics arith operations
  (divui/remui/shrui, cmpi ult/ule/ugt/uge) to unsigned IntegerType only —
  on signless types they stay illegal instead of miscompiling to the signed
  Rust operators. Importer side: unsigned C types map to MLIR unsigned
  ui8/ui16/ui32/ui64 (never signless); unsigned scalars live in
  emitrust.variable places rather than memref cells (mem2reg would
  materialize a signless arith.constant default); unsigned literals become
  emitrust.constant; all unsigned arithmetic, bitwise, shift, comparison,
  truth-test, ++/--, and compound-assignment forms lower to the emitrust
  ops; switch on an unsigned scrutinee reinterprets it to signless i64
  bit-exactly (case labels above i64::MAX included). Unary minus on an
  unsigned operand is C's modular negation (C99 6.2.5p9) and lowers to
  `0 - x` through the unsigned emitrust.sub, which renders as Rust's
  wrapping_sub on every width (u8/u16/u32/u64) — matching C at 0, 1,
  UINT_MAX, and the INT_MIN bit pattern instead of panicking on debug
  overflow.
  (test/Target/Rust/arith.mlir, test/Dialect/EmitRust/ops.mlir,
  test/Conversion/ArithToEmitRust/unsigned-invalid.mlir,
  test/Import/C/unsigned.c, switch-unsigned.c,
  switch-unsigned64.c, test/EndToEnd/unsigned.c)
- [x] C99-3 Integer promotions and the usual arithmetic conversions for
  mixed signed/unsigned and mixed-rank expressions: clang Sema supplies the
  implicit casts, and the importer lowers every integer-to-integer cast
  shape touching an unsigned type to emitrust.cast, whose Rust `as`
  semantics (truncation, zero-extension from unsigned, sign-extension from
  signed, same-width reinterpretation) match C's conversions exactly;
  signless-to-signless casts keep the existing arith ext/trunc fast path.
  Unary '-' interacts correctly with the promotions: on an operand that
  stays unsigned after promotion it lowers to the wrapping `0 - x` (see
  C99-2), while an unsigned char/short operand is promoted to signed int
  first (per C) and takes the signed negation path, converting back on
  any narrowing store.
  (test/Import/C/unsigned.c, test/EndToEnd/unsigned.c)
- [x] C99-4 Plain char signedness policy, character constants, and
  escape sequences. POLICY: plain `char` is signed — the x86-64 Linux /
  clang default the differential oracle uses — so Char_S maps to
  signless i8 exactly like `signed char` (Char_U/UChar map to ui8).
  Character constants have C type int and import as the i32 constant
  clang evaluated: simple escapes (\n \t \0 \\ \' \"), octal (\012) and
  hex (\x41) forms, and — under the signed-char policy — sign-extended
  high bytes ('\xff' is -1, '\x80' stored to a char reloads as -128).
  They work in expressions, comparisons, switch case labels (via
  clang's constant evaluator), array subscripts, and global
  initializers (via APValue). Wide constants (L'') are accepted as
  their code-point value — wchar_t is int on this target, matching the
  wide-string verbatim-code-unit policy and c-testsuite 00098 — while
  Unicode constants (u8''/u''/U'', charN_t types outside the model) and
  multi-character constants ('ab', implementation-defined value) are
  rejected with located diagnostics; a multibyte source character
  ('é') never reaches the importer — clang rejects it first. String
  literal escapes ride the existing C99-28 per-byte machinery (clang
  decodes them before import): all standard escapes including \a \b \f
  \v \r round-trip byte-identically; embedded NUL and the ASCII-only
  rejection of non-ASCII bytes are unchanged.
  (test/Import/C/char-constants.c, char-constants-invalid.c,
  test/EndToEnd/char-constants.c)
- [x] C99-5 Enumerations: enum definitions, enumerator constants in
  expressions and case labels, mapped to a distinct nominal Rust type per
  enum rather than bare integer constants; enum-to-int conversions
  (implicit promotions in mixed enum/int comparisons and arithmetic, and
  explicit casts) lower to `emitrust.cast` on the mapped destination type
  — signless i32 for signed underlying types, ui32 for unsigned ones, so
  C's unsigned comparison against negative ints is preserved — rendered as
  safe Rust `as` casts of the raw value; comparisons between distinct enum
  types stay rejected. Revised under CTS-S6: the emitted representation is
  a value-preserving open enum (a `#[repr(transparent)]` tuple struct over
  the storage integer with one associated constant per enumerator and a
  Default impl returning the first variant) rather than a fieldless
  `#[repr(i32)]` enum, because C enum objects hold any value of the
  underlying type; int-to-enum conversions are therefore supported and
  value-preserving (see CTS-S6).
  (test/Import/C/enums.c, enum-int.c, enums-invalid.c,
  test/EndToEnd/switch-enum.c, test/EndToEnd/enum-int.c)
- [x] C99-6 Typedefs of every supported type shape, including typedefs of
  pointers, arrays, and struct types, resolved through canonical types; a
  typedef naming an anonymous struct (`typedef struct { ... } T;`) gives
  the record its typedef name for import, mangling, and cross-TU shape
  dedup, while a bare anonymous struct stays rejected with a located
  diagnostic. (test/Import/C/typedefs.c, structs-anon-typedef.c,
  test/EndToEnd/fn-pointers.c)
- [x] C99-7 Type qualifiers. const maps positionally: a never-written
  const global (scalar or array) imports as a const-marked
  emitrust.global (an immutable Rust static; `static const` locals join
  via their mangled module global), string-literal backings are
  const-marked variables (immutable lets), const-qualified locals keep
  the ordinary variable/alloca lowering (writing through a const lvalue
  is already a clang frontend error, and mem2reg renders scalar SSA
  forms as immutable lets), a const value parameter is an ordinary
  by-value scalar, a const pointee (`const int *p`) classifies exactly
  like its unqualified spelling (mut_ref, slice, or cell-slice per the
  region analysis), and qualification-only pointer casts that add or
  drop const stay transparent (CTS-P2 peeling). volatile is rejected by
  policy with a located "unsupported: volatile-qualified type" wherever
  a declared type carries it at any level — locals, globals, parameter
  pointee chains, struct fields, return types (deep scan
  `hasVolatileQualifier`, plus checks in mapType, mapParamType,
  mapStructFieldType, emitLocalVar, and createGlobal) — and a cast that
  introduces a volatile pointee refuses the qualification peel so the
  site keeps a located rejection. Exception (c-testsuite 00162):
  qualifiers on a parameter OBJECT itself (`volatile int v`,
  `int x[volatile 5]`, which adjusts to `int * volatile x`) are
  body-local, never part of the function type, and accepted-and-ignored.
  restrict is accepted and ignored everywhere (a pure aliasing hint; the
  pointer region analysis is stricter than restrict), so
  restrict-qualified parameters import identically to unqualified ones.
  _Atomic is rejected with a located
  "unsupported: _Atomic-qualified type". Pre-existing const limits
  unchanged: struct- and fn_ptr-typed const globals keep the Cell
  representation (the GlobalOp const marker is scalar/array only), and a
  const global array argument to a pointer parameter keeps the CTS-P10
  located rejection (cell-slice classes require mutable global bases).
  (test/Import/C/qualifiers.c, qualifiers-invalid.c,
  test/EndToEnd/qualifiers.c)
- [x] C99-8 long double: maps to f64 — the same type as double — as a
  UB-refinement (REVISED for CTS 00204; the former PERMANENT rejection
  is withdrawn). Rationale: C requires long double to be at least as
  wide as double, every f64-exact value round-trips the substitution
  unchanged, and the supported shapes (L-suffixed literals, copies,
  parameters/returns/struct members, printf %Lf-family output of
  f64-exact values) perform no extended-precision arithmetic whose
  extra bits a defined program could rely on — the substitution only
  narrows evaluation precision, latitude C's FLT_EVAL_METHOD model
  already grants in the other direction, and the 00204 differential
  oracle is byte-exact under it. Consequences: double <-> long double
  conversions are identities (no arith.extf/arith.truncf); L-suffixed
  literals (decimal and hex) convert their x87 APFloat to IEEE double
  at import, correctly rounded; printf's L length modifier is accepted
  on the floating conversions exactly like the unmodified twins (bare
  %Lf keeps the __emitrust_fmt_f64 fast path, adjusted forms route
  through __emitrust_fmt_float) and stays a located rejection on the
  integer conversions ("unsupported: length modifier 'L' on printf
  '%d'"). Constructs that would OBSERVE the substitution stay
  rejected: sizeof/_Alignof over long double ("unsupported:
  sizeof/alignof of long double" — the C fold would promise the
  16-byte x86-64 ABI slot the emitted 8-byte f64 never keeps, the same
  reasoning as the bit-field sizeof rejection) and the %La/%LA
  hex-float conversions ("unsupported printf format specifier '%La'" —
  hex-float output renders the BITS, and an x87 80-bit value and the
  substituted f64 print different mantissas even when f64-exact).
  (test/Import/C/long-double-f64.c, long-double-f64-invalid.c,
  printf-extended-invalid.c; exercised end-to-end by c-testsuite 00204
  and test/EndToEnd/varargs-monomorph.c)
- [x] C99-9 _Complex and _Imaginary: documented rejection — no Rust
  counterpart. _Complex reaches the importer's type mapper and rejects
  with the located diagnostic "unsupported type '_Complex double'";
  _Imaginary (optional C99 Annex G, never implemented by clang) is a
  located clang frontend rejection ("imaginary types are not supported")
  before import begins. Both are pinned build-time errors, never silent
  acceptance. (test/Import/C/complex-invalid.c)

### Declarations and initializers

- [x] C99-10 Block-scoped declarations anywhere in a block and
  declarations in the for-init clause. (test/Import/C/scalars.c)
- [x] C99-11 Aggregate initializer lists for arrays and structs,
  including nested and partially explicit initializers with implicit
  zeroing.
  Implemented at both scopes. Block scope: the variable keeps its
  default-initialized `emitrust.variable` place (C99 zero-fill), then one
  `emitrust.assign` per explicitly initialized element through a
  constant-index `emitrust.subscript` (arrays) or `emitrust.member`
  (struct fields), recursing for nested lists — this uniformly covers
  partial initialization, designators, and non-constant elements, and
  also applies to owner-promoted arrays. File scope: clang's constant
  evaluator produces the complete APValue (designators resolved, holes
  zero-filled from the array filler), converted to a typed ArrayAttr
  element list on `emitrust.global` (nested ArrayAttr for aggregate
  elements); the GlobalOp verifier checks element count against the array
  size / struct_def field count and per-element types. Const arrays stay
  plain `static NAME: [T; N] = [e0, ...];`; mutable aggregate globals
  keep the `thread_local!` Cell path with `[e0, ...]` / `Name { f: e, ...
  }` literals. Accepted trade-off: a partially initialized large global
  renders its full element list (the 32-element `Default` derive cap only
  constrains struct fields of array type, which use literal lists here
  anyway). Rejected with located diagnostics: non-list aggregate
  initializers (whole-struct copies; compound literals are supported per
  C99-13; string literals on char arrays are supported per C99-28) and
  enum-typed global elements. Multi-dimensional arrays and their nested
  initializer lists ARE supported (each dimension renders its element list;
  see arrays-multidim tests).
  (test/Import/C/aggregate-init.c, aggregate-init-invalid.c,
  test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/EndToEnd/aggregate-init.c,
  c-testsuite 00048/00090/00092/00093/00115/00117/00146/00147/00148)
- [x] C99-12 Designated initializers for array indices and struct fields.
  Implemented with C99-11: the importer works on clang's semantic
  initializer-list form, where `[i] =` and `.field =` designators are
  already resolved to positional elements with implicit-value holes, so
  designated, partial, and overwriting-positional cases all reduce to the
  same per-element handling at block scope and the same APValue
  conversion at file scope. (test/Import/C/aggregate-init.c,
  test/EndToEnd/aggregate-init.c)
- [x] C99-13 Compound literals in expression position.
  Implemented for block-scope struct/union/array literals: the literal
  materializes as a fresh anonymous `emitrust.variable`
  (default-initialized — C99 zero fill — then the C99-11 per-element
  assigns; `(char[N]){"..."}` fills like a string-initialized array per
  C99-28) and from there behaves as an ordinary lvalue of that temp.
  Value uses load it whole (assignment right-hand side — including the
  self-referencing `s = (struct S){s.b, s.a}` swap, which reads the old
  values through the temp — by-value argument, return); member and
  subscript accesses resolve on the temp's place; a variable initializer
  or aggregate-element position initializes the target place directly
  through the literal's list (the copy source is immediately dead). A
  decayed or address-taken literal binds a synthesized backing
  declaration (one per literal, shared between the planning and emission
  analyses) as a pointer-region base like any named local: cursor walks,
  writes through the pointer, slice arguments, multi-base rebinding
  (CTS-P7), and the degenerate struct base all compose unchanged. A
  region-base temp is hoisted to the entry block (dereferences anywhere
  in the body must be dominated) and each evaluation first restores the
  type's pristine default value, so a literal bound inside a loop
  re-zeroes its holes exactly like C's fresh object per evaluation.
  Full expressions containing literals arrive wrapped in
  ExprWithCleanups, peeled as trivia (the "cleanup" is the temp's end of
  life). Owner promotion never keys on a literal base (no declaration
  statement to anchor the owner struct); such regions stay on the
  Phase-1b lowering. Rejected with located diagnostics: scalar compound
  literals, binding a global pointer to a literal (the borrow would
  outlive the block), returning a pointer into one (dangling); a
  block-scope `static` pointer initializer is rejected by clang's own
  constant-initializer check. File-scope literals keep their CTS-P4
  `<name>_backing` global path.
  (test/Import/C/compound-literals.c, compound-literals-invalid.c,
  test/EndToEnd/compound-literals.c)
- [x] C99-14 File-scope objects: global variables with constant
  initializers, tentative definitions, extern declarations across
  translation units (single-TU first), and static file-scope objects
  (Rust mapping: static items; mutable globals are a design decision —
  no unsafe rules out static mut, pointing at interior-mutability
  wrappers or parameter threading).
  Implemented single-TU via module-level `emitrust.global` with
  `emitrust.global_load`/`emitrust.global_store` access ops. Never-written
  const-qualified globals emit plain `static NAME: T = INIT;` read
  directly; every other global emits a `thread_local!`
  `std::cell::Cell<T>` (all imported types are Copy) accessed with
  `.with(|c| c.get()/c.set(v))` — no `unsafe`, no `static mut`, exact for
  the single-threaded subset. No initializer means the type's default
  (C zero-initialization); scalar initializers are clang
  constant-evaluated; aggregate initializer lists are typed ArrayAttr
  element lists (C99-11); element/field access to global aggregates is
  load-modify-store of the whole value. Rejected with located
  diagnostics: taking a global's address in value position, Rust-keyword
  names, `_Thread_local`, extern-only declarations, and block-scope
  extern. Pointer-typed file-scope variables import through the CTS-P4
  global region model (single global base plus a stored i64 cursor
  global; see the c-testsuite checklist); pointer-typed function-local
  statics stay rejected.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/Import/C/globals.c,
  globals-invalid.c, globals-keyword.c, globals-extern-only.c,
  aggregate-init.c, globals-thread-local.c, globals-pointer.c,
  globals-pointer-invalid.c,
  globals-extern-local.c, test/EndToEnd/globals.c)
- [x] C99-15 Static local variables preserving state across calls
  (design decision needed for a no-unsafe mapping).
  Implemented with the same `emitrust.global` machinery: a function-local
  static becomes a module-level global mangled `<function>_<name>`
  (collision with any existing module symbol is rejected), constant
  initializer required (C11 6.7.9p4, clang-enforced), initialized once at
  program start. (test/Import/C/globals.c,
  globals-static-collision.c, test/EndToEnd/globals.c)
- [x] C99-16 Variable-length arrays and variably modified types:
  documented rejection for LIVE VLAs — no fixed-size Rust counterpart,
  and VLAs are only conditionally supported in later C standards. A
  referenced VLA, or one whose size expression has side effects, rejects
  with the located diagnostic "unsupported: non-constant array size" at
  the declared variable. The sole carve-out is dead-VLA elision (landed
  2026-07-19 for 00207): an unreferenced VLA whose size expression is
  side-effect-free is elided at import and the rest of the function
  imports untouched. Both sides are pinned.
  (test/Import/C/vla-dead-elision-invalid.c rejection,
  vla-dead-elision.c acceptance)
- [x] C99-17 Flexible array members, AMENDED by CTS-BR (00216): a FAM
  (C99 6.7.2.1p16, `T tail[];`) is tolerated at the DECLARATION — the
  record imports with sizeof excluding the FAM, exactly C's sizeof. On
  a byte-region record a static FAM-tail initializer additionally
  folds into an EXTENDED byte image past sizeof; on a typed record the
  field is dropped (a non-empty typed FAM-tail constant stays
  rejected). GNU zero-length array members (`T r[0];`) get the same
  zero-size, field-less treatment. What remains rejected, with
  dedicated located wordings that never degrade to the generic
  "unsupported: non-constant array size" fallback, is RUNTIME access
  to the tail: "unsupported: flexible array member access" and
  "unsupported: zero-length array member access" at the access site.
  (test/Import/C/flexible-array-invalid.c; positive declaration-side
  pins in test/Import/C/byte-region-aggregates.c, typedfam and params
  splits; see the 00216 disposition below.)
- [x] C99-18 inline functions and the C99 inline linkage rules: a
  semantic no-op for the transpiler — the specifier is accepted and
  ignored, and every inline definition imports as an ordinary function
  with its body. The tricky C99 linkage case, a plain inline definition
  without extern (C99 6.7.4p7: an inline definition that provides no
  external definition), still hands clang's AST the full body, and in
  the merged whole-program module the single ordinary definition is the
  right shape; extern inline (the spelling that does provide the
  external definition) imports identically. static inline rides the
  ordinary internal-linkage path: bare name in a single-TU import,
  per-TU mangled in a multi-TU import so identically named static
  inline helpers in two units stay distinct (C99-38/FR-26).
  (test/Import/C/inline.c, inline-multi-tu.c, test/EndToEnd/inline.c)

### Expressions and operators

- [x] C99-19 Arithmetic, comparison, logical and/or/not with
  short-circuit evaluation, assignment, compound assignment, and
  statement-position increment/decrement on signed scalars. Unary '-'
  covers unsigned operands too: it lowers to `0 - x` via the unsigned
  emitrust.sub (Rust wrapping_sub), so negation in expressions,
  assignments, and comparisons matches C's modular semantics on all
  supported widths.
  (test/Import/C/scalars.c, test/Import/C/unsigned.c,
  test/EndToEnd/loops.c, test/EndToEnd/unsigned.c)
- [x] C99-20 Bitwise and, or, xor, complement, and shift operators.
  Dialect/emission layer: emitrust.and/or/xor/shl/shr render the Rust
  `&`/`|`/`^`/`<<`/`>>` operators (complement imports as xor with all-ones),
  with conversions from arith.andi/ori/xori/shli on any integer type and
  signedness-guarded shifts right (shrsi only on signless/signed types,
  where Rust `>>` is arithmetic; shrui only on unsigned types, where it is
  logical). Importer side: signless operands lower to
  arith.andi/ori/xori/shli/shrsi and `~x` to xor with all-ones; unsigned
  operands lower directly to the emitrust bitwise ops; a shift amount of a
  different width than the shifted operand is normalized to the operand's
  type (C promotes the two independently), including in `<<=`/`>>=`.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/arith.mlir,
  test/Conversion/ArithToEmitRust/arith-to-emitrust.mlir,
  unsigned-invalid.mlir, test/Import/C/bitwise.c, unsigned.c,
  test/EndToEnd/bitwise.c)
- [x] C99-21 The conditional operator in expression position, with
  short-circuit arm evaluation (only the selected arm's side effects run)
  through a result cell and a cf diamond; scalar (integer/float) results
  only, non-scalar operands rejected with a located diagnostic.
  (test/Import/C/conditional.c, conditional-invalid.c,
  test/EndToEnd/value-exprs.c)
- [x] C99-22 The comma operator, in value position (left operand for
  effects only, right operand's value) and in statement position (both for
  effects; a void right operand is allowed there).
  (test/Import/C/comma.c, test/EndToEnd/value-exprs.c)
- [x] C99-23 Increment, decrement, and assignment used as values inside
  larger expressions: assignments store and re-load the assigned place
  (the C value is the post-assignment value), postfix ++/-- yield the
  original value and prefix forms the updated one.
  (test/Import/C/value-position.c, test/EndToEnd/value-exprs.c)
- [x] C99-24 Explicit casts between all supported scalar types, including
  unsigned<->signed, unsigned<->float (Rust `as` saturates float-to-int
  where out-of-range C is undefined — a defined refinement), and the
  pre-existing signed/float pairs.
  (test/Import/C/unsigned.c, scalars.c, test/EndToEnd/unsigned.c, float.c)
- [x] C99-25 sizeof and _Alignof on types and expressions, constant-folded
  at import time from clang's target layout into a ui64 (size_t) constant;
  the operand stays unevaluated, and variable-length-array operands are
  rejected with a located diagnostic.
  (test/Import/C/sizeof.c, sizeof-invalid.c, test/EndToEnd/value-exprs.c)
- [x] C99-26 Pointer arithmetic, pointer subtraction, pointer
  comparisons, and array-to-pointer decay: decomposed into (base object,
  i64 cursor) pairs intra-function and slice parameters
  (&mut [T]) across calls — the safe-Rust mapping is slices plus indices
  rather than raw offsets. Multi-base rebinding, escaping pointers
  (&p, pointer struct fields, pointer returns), NULL
  data pointers, void* casts, and string-literal pointers stay located
  rejections by design (pointer globals are now the CTS-P4 global
  region model). See FR-28. Qualifying cross-function regions
  additionally promote to owner structs with &mut self methods (FR-30).
  (test/Import/C/pointers-local.c,
  pointers-param-slice.c, test/EndToEnd/pointers-local.c,
  pointer-params.c)
- [x] C99-27 Function pointers and calls through them: function pointers
  are ordinary Copy values of !emitrust.fn_ptr type rendered
  Option of fn (NULL is None, no sentinel), legal as locals, globals,
  struct fields, parameters, and results; function references become
  signature-checked Some(name) constants, indirect calls become
  emitrust.call_indirect with a deterministic panic refining the
  null-call UB, and truth tests and equality compare against None.
  Variadic pointers, void*/data-pointer components, and arrays of
  function pointers are located rejections. Argument-carrying calls
  through prototype-less K&R pointers refine the callee decl via
  callsite-prototype inference when it traces to a local-storage
  parameter/local (FR-29, CTS 00209); non-decl-traceable callees keep
  the located no-prototype rejection.
  (test/Import/C/fn-pointers.c, fn-pointers-invalid.c,
  fnptr-noproto-infer.c, fnptr-noproto-infer-invalid.c,
  test/EndToEnd/fn-pointers.c, fnptr-noproto-infer.c,
  test/Target/Rust/fn-pointers.mlir)
- [x] C99-28 String literals as char-array initializers and as pointer
  values, with the C escape set (beyond the original printf-format-only
  support). Array initializers: `char s[N] = "..."` / `char s[] = "..."`
  for plain, signed, and unsigned char arrays at both scopes (bytes are
  bytes; the unsigned elements live in the ui8 domain). Block scope
  lowers to per-element byte assigns over the default-zero place — the
  literal's bytes plus the terminating NUL when it fits (C99 6.7.8p14),
  remaining elements keeping the zero fill; file scope folds through the
  C99-11/14 APValue path to a typed i8/ui8 ArrayAttr on
  `emitrust.global`. Embedded NULs in the literal are ordinary data;
  wide (`L"..."`) literals fill wchar_t (i32) arrays one code unit per
  element (CTS-L3). Pointer values: `char *p = "..."` (and const char *)
  binds a read-only string-literal region (CTS-P1) — an i64 cursor into
  an immutable const-marked backing byte array holding the bytes plus
  the terminating NUL. Expression positions complete the entry: a
  subscript directly on a literal (`"abc"[i]`, constant or variable
  index) and the deref-of-arithmetic spelling (`*("abc" + n)`) read
  through the same cached backing, created on demand for anonymous
  decayed literals; `sizeof("...")` folds to length + 1 through clang's
  layout query (adjacent-literal operands included); a literal passed to
  a user-defined function's char-pointer (slice) parameter materializes
  a fresh mutable backing per call (FR-28, writes being UB makes the
  copy unobservable); string-helper arguments (strlen, ...) slice the
  backing; printf/puts %s shapes are unchanged; a literal compared
  against a null pointer constant folds to false. Adjacent string
  literals concatenate before import (clang folds translation phase 6),
  so every position above accepts the joined form. The full C escape
  set — simple escapes including \a \b \f \v \r \?, octal, and hex —
  arrives from clang already decoded to bytes, so every path above is
  spelling-blind (character constants share this machinery, C99-4).
  Located rejections, each pinned: non-ASCII bytes in any literal path
  (the ASCII-only policy keeping printed contents exact through the
  %s/%c helpers — unchanged and deliberate); u8/u/U literal kinds
  everywhere and wide literals outside the array-initializer position
  (a wide subscript has no byte backing); every write into a literal —
  plain and compound assignment, ++/--, and the deref spelling all
  funnel through one store guard on the const-marked backing (writing a
  C string literal is UB); literal-to-literal == (pointer identity is
  unspecified in C; each literal is its own backing, so the
  same-object comparison rule rejects the pair); %s literal arguments
  with embedded NUL or non-printable bytes; a literal bound through an
  unsigned-char pointer cast (CTS-P3 cast traffic); and `__func__`
  element access (C99-29).
  (test/Import/C/strings.c, strings-exprs.c, strings-invalid.c,
  pointers-string-literal.c, globals-pointer-string.c, strings-wide.c,
  printf-slice-param.c, char-constants.c, test/EndToEnd/strings.c,
  strings-exprs.c, string-cursor.c, char-constants.c)
- [x] C99-29 Float literal forms including hexadecimal float constants,
  and __func__. Every float literal spelling — decimal, leading/trailing
  dot, exponent, hexadecimal (C99 6.4.4.2), and the f/F suffixes —
  arrives as clang's evaluated APFloat, so only the value and type
  survive: float literals are f32 constants, double literals f64, and
  the file-scope APValue fold (C99-11/14) is spelling-blind too. Long
  double keeps its located builtin-type rejection under every spelling
  (decimal or hex L suffix), rejected at the literal before any
  narrowing cast. The `__func__`-family predefined identifiers
  (C99 6.4.2.2, plus __FUNCTION__ and __PRETTY_FUNCTION__) carry their
  function-name StringLiteral inside the PredefinedExpr and take the
  C99-28 literal paths unchanged: printf/puts %s arguments lower to the
  `emitrust.literal` string, a char-pointer binding is a read-only
  string-literal region over the name's bytes plus the NUL, and
  string-helper arguments (strlen, ...) slice the same backing. Located
  rejections: any use outside those positions (element access, plain
  value use) names the identifier, and the printf format position keeps
  the spelled-literal requirement.
  (test/Import/C/float-literals.c, float-literals-invalid.c,
  func-name.c, func-name-invalid.c, test/EndToEnd/float-literals.c,
  test/EndToEnd/func-name.c)

### Statements

- [x] C99-30 if/else, while, for, break, continue, return.
  (test/Import/C/scalars.c, test/EndToEnd/loops.c)
- [x] C99-31 do-while: the body block is entered unconditionally and the
  condition block branches back or exits, so lift-cf-to-scf recovers a
  loop with a trailing conditional break; break/continue target the exit
  and condition blocks through the existing loop stack.
  (test/Import/C/do-while.c, test/EndToEnd/value-exprs.c)
- [x] C99-32 switch, case, default, including fall-through, shared case
  labels, nested switches, and negative/sparse/64-bit case values, mapped
  through cf.switch and scf.index_switch onto a Rust match; Duff's device
  (case labels of an outer switch nested inside inner non-switch
  statements) and other non-plain bodies take the CTS-S2 dispatch
  fallback, and GNU case ranges are rejected with located diagnostics.
  (test/Import/C/switch.c, switch-dispatch.c, switch-invalid.c,
  test/EndToEnd/switch-enum.c, test/EndToEnd/switch-general.c,
  test/EndToEnd/switch-dispatch.c)
- [x] C99-33 goto and labels: each label maps to a dedicated block
  (created at first mention, so forward and backward gotos both resolve)
  and a goto is a plain cf.br; lift-cf-to-scf structures the resulting
  CFG, including irreducible shapes (a goto into a loop body lowers
  through the transformCFGToSCF multiplexer) and backward gotos that form
  loops. In a function containing labels, emitrust.variable places are
  hoisted to the entry block so a goto that jumps over a declaration
  cannot leave a later use undominated. Computed goto (GNU `goto *expr`)
  is rejected with a located diagnostic.
  (test/Import/C/goto.c, goto-invalid.c, test/EndToEnd/goto.c)

### Functions and program structure

- [x] C99-34 Function definitions and prototypes with scalar, struct
  by-value, and pointer parameters; forward declarations; main with
  implicit return. (test/Import/C/structs.c, pointers.c, scalars.c)
- [x] C99-35 Recursive and mutually recursive functions: pinned by a
  differential regression test — direct recursion at data-dependent
  depth plus a mutually recursive is_even/is_odd pair.
  (test/EndToEnd/recursion.c)
- [x] C99-36 Array parameters with decay semantics, including the C99
  static and qualifier forms inside the brackets. Every bracketed form —
  unsized, sized, the C99 minimum-length static form, and the const /
  volatile qualifier forms — adjusts to a pointer parameter in clang's
  AST (C99 6.7.5.3p7), so each rides the ordinary Phase-1b
  pointer-parameter classification: subscripted use classifies as a
  slice reference, deref-only use as a scalar reference. The bracket
  qualifiers qualify the decayed POINTER object itself, not the pointee,
  and the decomposition erases that object, so const and volatile inside
  the brackets are accepted and ignored — this leaves C99-7's policy on
  volatile OBJECTS untouched, since no volatile access ever survives to
  the emitted Rust. Exclusions keep their located pointer-parameter
  diagnostics: a multidimensional array parameter decays to a
  pointer-to-array with no slice shape, and an array-of-pointers
  parameter decays to a rejected pointer-to-pointer (CTS-P5).
  (test/Import/C/array-params.c, array-params-invalid.c,
  test/EndToEnd/array-params.c)
- [x] C99-37 Variadic function definitions and va_list: bounded va_list
  bodies MONOMORPHIZE per call site (REVISED for CTS 00204); everything
  the monomorphizer cannot see stays a located rejection. Rust has no
  stable variadic ABI or safe varargs access, so no va_list object ever
  survives into the emitted Rust; instead, a Pass-A planner
  (`planVaMonomorph`, before any declaration imports) claims every
  variadic DEFINITION whose body uses va_list and, when the body stays
  in the bounded shape, synthesizes one clone per distinct
  extras-signature over its direct call sites. Clone parameters are
  exactly (named parameters, that site's extra arguments BY VALUE in
  declared order) — no synthetic trailing parameter; the consumption
  cursor is an internal i64 local. Inside a clone va_start resets the
  cursor, va_end is a no-op, and each va_arg(ap, T) becomes a dispatch
  over the cursor selecting among the extras whose static type is T
  (the cursor increments per read); a cursor position with no matching
  extra — UB in the C call — is a deterministic panic. Struct-typed
  va_arg reads (the 00204 HFA shapes) fall out for free: extras are
  ordinary Copy values, so no calling-convention modeling is needed.
  Call sites rewrite to their clone; the original variadic symbol is
  never emitted, and an in-scope definition with zero call sites drops
  entirely. The bounded-shape scope checks are located rejections,
  raised BEFORE any callee prototype imports: "unsupported: va_copy"
  (a cloned cursor's lifetime is out of scope), "unsupported: va_list
  escapes variadic definition" (ap passed to ANY callee — the callee
  would consume varargs the monomorphizer cannot see; this beats the
  callee's own va_list-parameter type rejection), and "unsupported:
  address of variadic definition" (an escaping function address makes
  the call-site set non-enumerable). Outside variadic definitions the
  va_list TYPE keeps its rejection in every position — local,
  parameter, field, global — with the located "unsupported: va_list
  type", so a hand-rolled vprintf-style helper can never import as the
  target's register-save-area struct, and the v*printf family stays
  unreachable by construction. The older carve-outs stand unchanged:
  (1) the CTS-F1/CTS-P9 fixed-prototype import — a va_list-FREE
  variadic definition imports as its named parameters only and call
  sites drop effect-free extras (an extra with side effects is
  rejected); (2) printf-family CALL SITES route through the hosted
  printf/puts machinery (C99-47/48) when the project supplies no
  definition. (test/Import/C/varargs-monomorph.c,
  varargs-monomorph-invalid.c, varargs-def.c, varargs-def-invalid.c;
  differential test/EndToEnd/varargs-monomorph.c, varargs-def.c;
  exercised at scale by c-testsuite 00204 — 35 call sites, 33 distinct
  clone signatures, 14 struct-typed va_arg sites)
- [x] C99-38 Multiple translation units: several .c files are imported and
  merged into one flat crate with extern object and function resolution
  across units, and internal (static) linkage kept distinct by per-unit
  symbol mangling (a single flat module needs no pub/non-pub visibility).
  Undefined externs and conflicting external definitions are rejected with
  located diagnostics. (test/EndToEnd/multi-tu.c, test/Import/C/multi-tu.c,
  multi-tu-undefined-extern.c) See FR-26.
  W3.0 revision: a va_list-using variadic (C99-37) DEFINED in one TU and
  CALLED from another is a located rejection, not the silent breakage it
  was before. `planVaMonomorph` enumerates a va_list variadic's call
  sites WITHIN A SINGLE TU only (each TU is its own `clang::ASTContext`,
  so a caller in another TU is invisible to it), and a va_list-monomorphized
  definition is ALWAYS replaced by its per-site clones — it never keeps an
  ordinary symbol of its own name. Left alone, a cross-TU call site would
  reference a symbol that exists nowhere in the merged module: an
  unresolved call escaping with no located diagnostic. `importCProject` now
  pre-scans every parsed AST (`collectCrossTuVaListVariadics`, before any
  TU imports, so the scan is independent of which TU — caller or definer —
  appears first on the command line) for externally visible va_list-using
  variadic definitions and records their symbol names; `emitCall` consults
  this registry only when a variadic callee's definition is invisible in
  the current TU, raising "unsupported: call to a variadic function
  '<name>' defined in another translation unit". A NON-va_list
  (fixed-prototype, CTS-P9) variadic called cross-TU is unaffected: it
  keeps the historical generic "unsupported: call to a variadic function"
  wording (it was already rejected before this revision — cross-TU
  resolution for it is not yet attempted at all). A variadic defined AND
  called within the SAME TU is unaffected and still monomorphizes.

  W3.5 (real cross-TU va_list monomorphization) DEFERRED — the located W3.0
  rejection stands. A spike built the whole-program enumeration cleanly (a
  pre-pass over every AST, after `crossTuVaListVariadicNames` is complete,
  deduplicates each cross-TU all-scalar extras signature into a shared clone
  plan named `<symbol>__ctN`, so the defining TU and every calling TU agree
  on the clone name with no coordination) and definer-first import orders
  worked end to end. It foundered on the caller-BEFORE-definer order, which
  the differential requires (`emitrust-cc main.c def.c` imports the caller
  first): a monomorphization clone is a SYNTHESIZED symbol with no C
  prototype, so a caller TU imported before the definer has nothing to
  forward-declare it from, and `emitCall` rejects the reference to the
  not-yet-materialized `@<symbol>__ctN`. The three escape routes each cost
  more than the feature is worth: (i) reordering the import loop so definers
  precede callers perturbs the module's function EMISSION order (functions
  emit in import order — verified: reversing an existing multi-TU pair
  reorders the output), breaking every multi-TU byte-snapshot; (ii)
  pre-declaring the clone in the caller is impossible because the clone's
  signature (e.g. the `fmt` param's `&mut [i8]` cursor classification) is
  derivable ONLY from the definition's body, which lives in another
  `ASTContext` the caller cannot reach; (iii) an eager clone-materialization
  pass before the main loop needs the defining TU's full Pass-A, which the
  main loop then re-runs, risking double-planning on the completed 220/220 C
  ledger. Thin demand (a cross-TU va_list variadic is rare) plus this
  disproportionate, C-path-risking plumbing put it on the same
  conservative-but-sound footing as the G2/G7 retentions; reopening it is a
  dedicated future wave that would first refactor clone materialization to be
  decoupled from the defining TU's live import state. (test/Import/C/
  multi-tu-varargs.c, Inputs/multi-tu-varargs-def.c)
- [x] C99-39 Preprocessor-heavy sources: #include of project and system
  headers resolves — clang's builtin resource directory is wired in at
  configure time and -I/-isystem/--extra-arg are passed through, so macros,
  conditional compilation, and variadic macros are handled by clang once
  include paths resolve. Project-local headers (-I) are imported eagerly
  and whole-file validated, and are regression-tested. System-header
  declarations (angle-bracket includes / -isystem, gated solely on clang's
  isInSystemHeader at the declaration's expansion location) are skipped at
  the top level instead of imported, so including a real <stdio.h>
  succeeds even though its contents (glibc's anonymous structs in
  bits/types.h, variadic prototypes, FILE) fall outside the supported
  subset. A main-file use of a skipped declaration is rejected at the use
  site with a located diagnostic naming the symbol ("declared in a system
  header; not part of the supported C subset"); types are still imported
  on demand through mapType, and printf keeps its by-name lowering. The
  skip is per-TU and emits no symbols, so multi-TU mangling and cross-TU
  struct dedup are unaffected. (test/Import/C/include-path.c,
  system-headers.c, system-headers-invalid.c, system-headers-multi-tu.c,
  test/EndToEnd/stdio-include.c) See FR-27.

### Aggregates and memory

- [x] C99-40 Struct definitions, member and pointer-member access,
  by-value struct copies, address-of on locals and aggregates, and 1-D
  fixed-size arrays with integer indexing.
  (test/Import/C/structs.c, arrays.c, pointers.c, test/EndToEnd/structs.c)
- [x] C99-41 Multi-dimensional arrays and arrays of structs, with full
  place-chain access (the dialect's array and member ops already compose;
  the importer must emit the chains and tests must cover them).
  `!emitrust.array` elements nest (emitted `[[T; N]; M]`), `mapType`
  recurses, nested initializer lists (file-scope with designators and
  zero fill, and block-scope) recurse level by level, `a[i][j]` emits one
  `emitrust.subscript` per dimension, subscript chains compose with
  `emitrust.member` on arrays of structs, and the pointer decomposition
  reaches into multi-dimensional bases with a flat row-major cursor
  (`&arr[i][j]`, row-pointer subscripts, scalar dereference via div/rem
  peeling). Row-pointer walking arithmetic and slices of rows stay
  located rejections (CTS-P scope). See CTS-S4.
  (test/Dialect/EmitRust/types.mlir, invalid.mlir,
  test/Import/C/arrays-multidim.c, pointers-local-invalid.c,
  test/EndToEnd/arrays-multidim.c)
- [x] C99-42 Nested struct types and struct assignment as a whole:
  pinned — nested member types, whole-struct assignment (value/Copy
  semantics), member-of-nested writes, and assignment through a pointer
  deref, import-level and differential. (test/Import/C/structs-nested.c,
  test/EndToEnd/structs-nested.c)
- [x] C99-43 Pointers to pointers and pointer members inside structs
  (design decision needed alongside C99-26: reference-typed struct fields
  require Rust lifetimes, which the dialect deliberately does not model;
  candidate mappings are index-based handles or ownership restructuring).
  PARTIALLY RESOLVED for one closed subset: a self-referential pointer
  member of a promoted owner-struct array (`struct uf_node *parent`
  pointing at another element of the SAME array) is no longer a blanket
  rejection — see FR-37/FR-38 (`planArrayMemberPointers`, an
  `emitrust.enum_def` with one variant per array index, decoded/written
  via a genuine `match`). This resolves the member-pointer half of the
  shape; the RETURNED-pointer half is resolved only for the array-rooted
  case, where every return site provably roots in the same owner class as
  a data-pointer parameter of the same method (FR-36's owner-index
  return, Stage 1) — general arbitrary pointer returns (a pointer into a
  callee-local, non-array-backed object) remain rejected. Confirmed by
  direct test: `test/RealWorld/Inputs/binary-tree.c`'s `insert`, which
  returns a pointer to a freshly `malloc`'d node rather than a cursor into
  a caller-visible array, still rejects with "unsupported: returned
  pointer value (only a returned function address has a representation;
  a cursor into a callee-local region would dangle)" — the blocker there
  is dynamic memory (C99-46, out of scope for this plan), not the
  member-pointer or owner-index-return mechanism, and is left untouched.
  DESIGN SPIKE (2026-08-03) — candidate mappings measured; decision-ready,
  NOT an implementation. Probe artifacts (session scratchpad,
  c9943/probe.c + c9943/probe-rs, nothing lands in-tree): a C reference
  with three sections (kernel T** out-params; a list.h-style circular
  doubly-linked list over TWO bases; the RealWorld binary-tree) built
  with clang -std=c11 -Wall -Werror, and a hand-written Rust probe crate
  in emitted-crate style (forbid unsafe_code, deny warnings, deny
  unused_variables, Copy+Default node structs wherever the mapping
  permits) whose stdout is BYTE-IDENTICAL to the native build across all
  sections — every candidate's probe path reaches differential execution.
  CORPUS FREQUENCY, measured (kernel baseline
  test/Kernel/linux-6.6.94-allnoconfig/rejection-report.txt, 130 TUs /
  110,571 rejected items; c-testsuite ledger 220/220 — zero remaining
  demand; RealWorld 7 rejected):
  (a) T** locals/params — rank 7 `ptr-to-ptr`, 1,166 items in 89 TUs
  (1,143 "pointer-to-pointer parameter", the CTS-P5 pinned rejection at
  lib/ImportC/ImportCTypes.cpp:525, plus 23 string-cursor escapes).
  Behind the item count sit only EIGHT distinct T**-parameter functions
  in the shard TUs (skip_atoi, simple_strtoull/strtol, get_range,
  get_option, memparse, next_arg, check_cpu) plus the boot/string.h
  prototypes — the count multiplies through per-TU header re-rejection.
  Every measured write through such a parameter is either a cursor into
  a region a co-parameter already roots (`*endp = cp` x3, `*retptr`,
  `*param`, `*val`) or one global-or-NULL (`*err_flags_ptr = err ?
  err_flags : NULL`): 100% of observed shapes fall to candidate (iii).
  RealWorld's argv-echo (char** argv iteration) is the same family plus
  the W4.3 cursor-table gap.
  (b) pointer struct members at sibling/global objects —
  `self-ref-pointer-member`, 2,917 items in 67 TUs (first root
  include/linux/list.h:153, __list_add's next->prev writes), plus 781
  "pointer struct member assigned a non-address value" and 44
  nested-aggregate initializers under rank 1. A static census over the
  shard TUs + core headers shows the entire category is FOUR types:
  list_head (next/prev), rb_node (rb_left/rb_right), hlist_node (next),
  callback_head (next) — intrusive containers whose links cross
  heterogeneous containing objects via container_of.
  (c) T** struct members — ONE distinct field in the whole corpus slice,
  hlist_node's pprev, zero in the shard TUs themselves (and
  mapStructFieldType already admits T** fields as i64 at type level; the
  2026-07-31 premise-refutation in the Track-5 section stands).
  Adjacent, not C99-43 proper: returned-pointer 5,437 (the return half,
  FR-36 covers only array-rooted) and void-pointer-param 6,337.
  CANDIDATE (iii), T** OUT-PARAM AS MULTI-RETURN / &mut CURSOR — probe
  compiles clean, byte-matches native. simple_strtoull's `char **endp`
  erases into a second returned i64 cursor (the `*endp = cp` write is a
  cursor write into the region parameter `cp` already roots); skip_atoi's
  `const char **s` becomes backing + &mut i64 cursor (in-out, arity
  preserved); check_cpu's `u32 **err_flags_ptr` (one-global-or-NULL)
  collapses to a returned flag via the CTS-P2 GLOBAL-RETURN erasure
  applied to an out-param — callers read the global directly, zero
  runtime pointer state. IMPORT ANALYSIS: extend FR-28 parameter
  classification per T** parameter p — (1) every use of p is *p
  read/write or **p, p never reassigned/copied/subscripted/escaping;
  (2) every value written through *p resolves via resolveArgRoot, per
  call site exactly like slice admission, to ONE region class shared
  with a co-parameter, or to one global/NULL; (3) every caller passes
  &local of a decomposed pointer local (CTS-P5's consumed-&p shape,
  relaxed to cross the call boundary). Everything outside stays the
  located CTS-P5 rejection. Directly retires the 1,143-item rejection.
  CANDIDATE (i), INDEX-HANDLE REGION GENERALIZATION — probe compiles
  clean, byte-matches native, INCLUDING deletion and reverse iteration
  on a circular doubly-linked list whose links span TWO statically
  enumerable bases (head sentinel global + the embedded lh members of a
  4-element pool array): FR-37's per-index enum generalizes to one
  fieldless variant per REGION MEMBER with reads/writes decoding through
  a genuine match, node structs stay Copy+Default, and container_of
  COLLAPSES — the handle already names its containing element, so the C
  offset arithmetic has no emitted counterpart. OWNER IDENTIFICATION at
  import time: the region is the least closed set of statically
  enumerable objects (named globals, constant-size arrays, their
  members' embedded fields) reached by the fixpoint of every write into
  the field class program-wide — planArrayMemberPointers' proof shape
  (lib/ImportC/ImportCPlanning.cpp:484) with the single-ownerArray key
  widened to a base SET and resolveArgRoot extended to
  member-of-element addresses (&pool[i].lh). MEASURED LIMIT: kernel list
  users traverse via container_of/list_entry (char* offset casts);
  recognizing that idiom is a separate, large analysis, so the near-term
  admissible subset is pools/sentinels authored without container_of —
  a small share of the 2,917, consistent with the Track-5 6.4%
  ownership-census finding. The per-index enum also caps at
  kMaxOwnerArrayElements; a growable region needs the typed-index
  (newtype) form instead — an emitted-style question (Q4).
  CONTAINER_OF RECOGNITION SPIKE (census, NO-GO — the recorded C99-43
  design decision on this front): full census over the kernel slice's
  real include closure (131 shard TUs, 761 reachable files via kbuild
  .cmd dep lists; tarball sha256 matches the FR-60b record): 183
  textual sites, 26 real code sites (5 shard TUs + header inlines, 416
  per-TU materialized instances), every chain expanding through the one
  include/linux/container_of.h definition; open-coded (char*)-offsetof
  downcasts: ZERO in the slice. NO-GO on three measured blockers:
  (1) payoff ~zero — every containing type in the slice is
  independently rejected on fronts the recognizer does not touch
  (kref/refcount_t volatile atomics, the 7,135-item rank;
  function-pointer + incomplete-struct members, the 31,895-item
  cascade), so perfect recognition converts approximately no rejected
  items today; (2) the canonical shape falsifies the invariant — 8 of
  26 sites are for_each_entry loops whose exit-test downcast passes
  through a bare sentinel head embedded in NO container object, so a
  sound per-expression member-provenance proof must reject the idiom's
  most common form, and admitting it forces loop-level guarded-validity
  idiom recognition, the same fragile shape-specific machinery class
  the malloc-pool IR-rewrite NO-GO measured and rejected; (3) the
  closed-region precondition fails on the slice's actual lists — they
  are open cross-TU registration sets (i8253.c's global registered into
  clockevents.c's static heads; per-cpu registrants under real
  configs), which do not close per-TU and can never close over dynamic
  registrants. What would change the verdict: demand from a corpus with
  otherwise-admissible, statically-closed containers; candidate (i)'s
  widened multi-base fixpoint landed under FR-58 joint re-import; or
  the volatile/incomplete-struct fronts retiring first. DECISION: the
  container_of-free admissible subset (candidate (i)'s arena/index
  form, byte-matched above) stays sequenced behind that demand signal.
  CANDIDATE (ii), OWNERSHIP RESTRUCTURING (pointee moves into the
  struct, Option-of-Box tree) — compiles clean and its inorder walk
  cross-checks equal to the arena twin at runtime, BUT the node type
  stops being Copy, breaking the Copy+Default struct invariant the
  dialect rests on (the same axis on which the Track-5 census rejected
  lifetimes); it needs a unique-incoming-edge proof (exactly one live
  pointer per pointee — the doubly-linked list fails immediately on
  prev/next); and the only corpus shape it fits (binary-tree) is
  double-blocked on C99-46 malloc. NO-GO as a C99-43 mapping; revisit
  only inside C99-46 if non-Copy emitted structs are ever accepted. Its
  arena twin (Vec of nodes + Option<u32> handles, malloc->push,
  NULL->None) probes clean and byte-matches — the C99-46-era
  continuation of candidate (i), matching the recorded collection/
  index-handle preference from the C99-46 discussion.
  INTERACTIONS: (iii) is ROUTED on FR-58's signature-starvation axis
  (findSignatureStarvedDecls), but a verification probe MEASURED that
  the composition does not fire today for the T** family: the caller
  shard rejects a bare body-less T** prototype at import
  ("unsupported: pointer-to-pointer parameter", tag ptr-to-ptr), so no
  extern_decl func op exists for findSignatureStarvedDecls to compare
  (it scans kExternDeclAttrName ops only), the ledger axis
  (isFactStarvedDiagnostic) matches neither diagnostic produced, and a
  forced joint re-import hits the same rejection — a single-TU
  differential pins the root cause: even a redundant T** redeclaration
  AFTER an admitted Shape-S definition hard-errors, because Shape
  refinement is body-driven and the decl path has no admission for
  body-less T** signatures. The T* control (sum4-style slice
  refinement through a bare prototype) DOES rescue and byte-matches
  native, so the machinery itself is sound. Composition therefore
  needs two importer-side preconditions FR-58 does not provide:
  (a) admit body-less T** prototypes as extern_decls carrying the
  unrefined type, letting the existing disagreement trigger fire, and
  (b) tolerate T** redeclarations beside an admitted cursor-shape
  definition. The front REMAINS on FR-58's axis with those
  preconditions recorded — not a C99-43 box blocker. (iii) does not
  touch FR-59 (no shared state). (i)'s region
  facts are whole-program by construction — the same fact-starvation
  family as extern pointer globals, so FR-58's joint re-import of the
  definer group is the delivery vehicle — and a region whose member
  globals land in different FR-59 crates must CONDENSE exactly as
  cross-crate globals already do; a synthesized region struct grouping
  several globals also shifts emitted global names/layout, a
  byte-identity surface. (ii) composes with nothing (a new non-Copy
  type kind through every pass).
  RANKED RECOMMENDATION: 1st (iii) — smallest analysis burden, retires
  a measured 1,166 kernel items whose distinct shapes are 100% covered,
  and extends machinery that exists (FR-28 classification, CTS-P2
  erasure, CTS-P5's consumed-&p). 2nd (i) — mechanism proven by probe
  including the multi-base and reverse-iteration cases FR-37 lacks, but
  the admitted subset stays small until container_of recognition
  exists; sequence behind the demand signal, entangled with C99-46's
  arena. 3rd (ii) — NO-GO as stated. DECISION QUESTIONS only the
  project owner can answer: (Q1) may an out-param T** become a Rust
  multi-return, changing public signature arity (extern/FFI surface,
  FR-58 cross-shard shape), or must the arity-preserving &mut-i64-cursor
  form be the only admitted one? (Q2) is a synthesized region struct
  that GROUPS several globals acceptable emitted style, or must regions
  stay separate globals with match-routed accessors? (Q3) is a
  generation-checked (or plain index-checked, panic-on-stale) arena
  acceptable emitted style for the C99-46-era region — i.e. is a
  deterministic panic an acceptable refinement of C's stale-pointer UB,
  as it already is for null fn-ptr calls? (Q4) for handle-typed NULL,
  Option-of-index (probe form, Default = None) or an i64 sentinel —
  which is the FR-61 house style?
  DECISIONS (2026-08-03, owner):
  (Q1) &mut-i64-cursor only — NO multi-return anywhere. A T** out-param
  never changes the function's arity beyond the existing two-input
  (slice, &mut cursor) or one-input (&mut cursor) cursor forms; the
  public signature shape stays stable across the extern/FFI surface and
  FR-58 cross-shard comparison.
  (Q2) NO synthesized region structs. A `*p = <global address>` target
  (single or multiple, including check_cpu's global-or-NULL) stays a
  located rejection with its own tag (`ptr-to-ptr-global-target`); the
  multi-global front is the NEW FR-62 (actor/message decomposition,
  spike-first). The single-global-or-NULL case was verified against the
  probe and ALSO stays rejected in slice 1: the probe's CTS-P2-style
  erasure covers only the always-that-global write, while the measured
  kernel shape (`*err_flags_ptr = err ? err_flags : NULL`) needs a
  NULL-flag threaded back to the caller — a second in-out state cell
  beyond the existing global machinery, i.e. a new mechanism, exactly
  what slice 1 excludes.
  (Q3) Plain idiomatic indexing now: a deterministic panic on a stale or
  out-of-range index is an accepted refinement of C's UB, on the
  div-by-zero precedent. Generation-checked handles are recorded as a
  future OPT-IN optimization only, never the default emitted style.
  (Q4) Option<index> for NULL-able handles: None = NULL, on the fn-ptr
  (Option<fn>) precedent and the probe's Default = None form. No i64
  sentinel.
  SLICE 1 SCOPE (2026-08-03; the FR under implementation): generalize
  the existing CTS-00204 cursor-parameter machinery from `char **` to
  T** per the admitted-shape contract — per T** parameter of a DEFINED
  function (planning stays all-or-nothing per definition, main
  excluded): Shape S (self-walking cursor: every use under `*p`, content
  read-only, `*p = <self-rooted expr>` advancement) lowers to
  `(&[T], &mut i64)` for any slice-valid non-void, non-function element
  T; Shape P (paired out-cursor, the strtol/endp family: no `*p` reads,
  exactly one top-level unconditional `*p = <expr>` write whose RHS
  roots in exactly one same-element slice-classified co-parameter)
  lowers to ONE `&mut i64` input written directly, no cell and no
  writeback. Still rejected, each with a located diagnostic and a
  distinguishing ledger tag: shape escapes
  (`ptr-to-ptr-shape-escape`), `*p = NULL` (`ptr-to-ptr-null-write`),
  global targets (`ptr-to-ptr-global-target`, the FR-62 front),
  disagreeing/unrooted write sources (escape family), T*** and void**
  (generic `ptr-to-ptr` at mapParamType), and body-less
  prototypes/extern decls (generic `ptr-to-ptr`; a shard that sees only
  the prototype keeps rejecting — FR-58's signature-starvation axis is
  the recorded future path for cross-TU definitions).
  SLICE 1 LANDED (2026-08-03; box stays OPEN — the member-pointer,
  global-target, and cross-TU fronts remain). What is admitted now,
  validated by tests: Shape S over ANY slice-valid element
  (`isDataPointerPointerType` + `mapCursorParamSliceType`; int** pinned
  in test/Import/C/pointers-cursor-param-general.c beside a
  byte-behavior-unchanged char** twin; `(*p)++` advancement newly routes
  to the cursor cell), and Shape P, the strtol/endp paired out-cursor
  (planning classification in `planCursorParamsFor`, one-input
  `&mut i64` lowering, direct AssignOp write, call-site staged temp with
  the co-cursor reslice correction and caller-side region join via
  `pairedArgQuery`; pinned in pointers-cursor-param-paired.c and its
  -invalid split-file twin). Runnable proof:
  test/EndToEnd/cursor-param-paired.c byte-diffs a three-round
  strtol-style walk (rounds 2+ pass a nonzero-cursor pointer local as
  the co-argument — the shape that would miscompile without the
  coordinate correction) plus an int** self-walking summer against
  clang-native; stdout and exit byte-identical. New ledger tags (C++
  table + Python twin, rows above the generic `ptr-to-ptr`):
  `ptr-to-ptr-shape-escape` (escapes, content write-through,
  disagreeing/unrooted/conditional P writes), `ptr-to-ptr-null-write`,
  `ptr-to-ptr-global-target`. Wording note: the mapParamType
  "pointer-to-pointer parameter" rejection kept its generic wording and
  tag — the planned `ptr-to-ptr-no-def` split was measured off because
  class-scope C++ method definitions reach mapParamType without a
  cursor plan (planCursorParams walks only top-level decls), so the
  wording cannot claim "no visible definition". Cast on a P write RHS
  (`*endp = (char *)cp`) stays rejected (CStyleCastExpr) — recorded
  follow-up. KERNEL BASELINE REFRESHED (same corpus + FR-60b
  environment, shim rebuild `make LLVM=1 CC=emitrust-clang -j24` EXIT 0
  in 2m07s with unwrapped clang+lld on PATH and HOSTCC=gcc for host
  tools — the nix loader detail that FR-60b's host binaries embedded;
  queries ~1.5s): admitted-total 89,799 -> 89,939 (+140, tool-verified
  "ratchet improvement" against the committed baseline), rejected-total
  110,571 -> 110,433. The old rank-7 `ptr-to-ptr` 1,166 split into
  `ptr-to-ptr-shape-escape` 522 (54 TUs, definitions now entering
  cursor planning and rejecting at their located escape) +
  `ptr-to-ptr` 238 (89 TUs, prototypes/T***/void**) with the remaining
  ~400 items either imported or moved to more specific downstream
  wordings; zero kernel items hit the null-write/global-target tags
  (check_cpu's family rejects earlier, on shape). One shard was added:
  scripts/mod/empty.c (admitted=0, rejected=2 pre-existing
  compiler_types.h header items) now appears in the manifest — an
  object-enumeration delta from the FR-60 tolerant-query work, growth
  direction, gate-legal. Remaining fronts unchanged: intrusive
  member-pointer containers (rank 5), global out-param targets ->
  FR-62, prototypes/cross-TU -> FR-58 signature starvation, argv ->
  W4.3.
  FRONT C1 LANDED (2026-08-04; box stays OPEN — the orchestrator closes
  it after C3): single-global-or-NULL out-param cursors (Shape G),
  narrowing Q2's global-target rejection to the residual multi-global
  case. ADMITTED GRAMMAR: a `T **p` cursor parameter in an
  otherwise-admissible cursor-param function (single unconditional
  top-level write, all other uses of `p` under `*p`), whose write RHS is
  ONE whole statically-known file-scope global `g` (array decay or `&g`
  over a scalar, element types matching), a null pointer constant, or
  `cond ? g : NULL` in either arm order. THE MAPPING (spike-validated
  against the hand target, byte-diff-proved): the parameter lowers to
  ONE `&mut Option<i64>` in-out cell — Q1 arity preserved, Q4
  Option-form NULL (None = C NULL, Some(offset) = element offset into
  g's backing; always Some(0) under this grammar) — the callee's write
  assigns `Some(0i64)`/`None` (the ternary branches per arm), and the
  caller stages an Option temp (init None), passing `&mut`, then
  destructures it back into the pointer local's EXISTING CTS-P8/P6
  cells: non-null flag from `.is_some()`, cursor from `.unwrap_or(0)`
  (a never-null plan reads back with `.expect("null pointer read")`,
  the Q3 deterministic-panic spelling; an actual None-read still panics
  through the CTS-P8 `assert!(flag, "null pointer dereference")` guard
  at the read site — the existing precedent, kept over the spike's
  expect-at-read spelling because the read path is shared machinery).
  Later uses (`if (p)`, `p[i]`, `*p`) flow through the decomposed-
  pointer model unchanged, resolving against the plan's global — the
  region binds via the new `globalCursorArgQuery` seam in
  PointerRegionAnalysis, so no address ever escapes. Q2 NOTE: the
  recorded "needs a second in-out state cell" objection is OBSOLETE —
  the Option cell folds the NULL flag and the offset into one value;
  the synthesized-region PROHIBITION stands and is exactly what keeps
  multi-global rejected. ACTOR INTERPLAY (spike finding, implemented):
  the FR-40 item graph's assignment walk now classifies the admitted C1
  write as `ReadsGlobal` instead of `AddressOfGlobal` (the emitted
  Some-offset holds no address), so the callee JOINS the global's actor
  group and emits as a `&mut self` method with caller reads routed
  through the actor field — the spike's target shape; when the function
  fails import for any other reason the actor plan's missing-function
  rule still demotes, so the carve-out can never leak an address. The
  thread-local demoted form was probed byte-correct as well (a separate
  caller-side `ef = err_flags` decay binding still demotes and
  byte-matches). NARROWED REJECTIONS (both ledger tables updated in
  lock-step, needle widened to "global address", same
  `ptr-to-ptr-global-target` tag): "written with more than one global
  address" (two globals in one ternary, or across two write sites) and
  "written with a global address outside the single-global-or-NULL
  shape" (mixed global/local roots, derived addresses like `g + 1`,
  explicit-cast scalar puns, element-type mismatches, static locals).
  The pure-NULL write `*p = 0` is admitted as the degenerate
  empty-global-set grammar point, so the `ptr-to-ptr-null-write`
  wording is no longer raised (its table rows stay for old recorded
  ledgers). Variant B (`if (p) *p = ...;`) stays the
  `ptr-to-ptr-shape-escape` rejection: `p` outside `*p` has no
  representation under the `&mut Option<i64>` mapping; the spike's
  erased-guard lowering byte-matched but needs the all-sites proof
  recorded on FR-58's axis (body-less T** prototypes/redeclarations
  still hard-error — boundary unchanged). TESTS:
  test/Import/C/pointers-cursor-param-global.c (admitted grammar +
  caller flow goldens), pointers-cursor-param-global-invalid.c (the
  residual rejections, exact wordings), test/EndToEnd/
  cursor-param-global-or-null.c (check_cpu-shaped twin, both branches,
  mid-loop global mutation, byte-diff vs clang native; actor form),
  pointers-cursor-param-paired-invalid.c trimmed (its null-write and
  global-target cases moved to the admitted side). Suite 507 -> 510.
  KERNEL RATCHET: refreshed run (same corpus + FR-60b environment)
  measured NO DELTA — admitted-total 89,939 and rejected-total 110,433
  unchanged, zero items on the global-target tag before and after
  (check_cpu still rejects on shape, the variant-B guard), so the
  committed baseline stands.
  FRONT C3 LANDED (2026-08-04; box stays OPEN — the orchestrator closes
  it): the argv cursor table (retires the W4.3 gap). C `main`'s
  `char **argv` imports as the opaque `!emitrust.argv_table` parameter
  when EVERY use fits the admitted read grammar; the c_main signature
  then reads `c_main(argc: i32, argv: !emitrust.argv_table) -> i32`
  (rendered `fn c_main(v0: i32, v1: &[Vec<i8>]) -> i32`). THE WRAPPER
  (spike-validated against the hand target twin_a, byte-diff-proved): a
  3-way arity select in CrateEmitter (0/1/2 c_main inputs) adds the
  arity-2 entry shim, which collects the argument vector from
  `std::env::args_os()` as RAW BYTES — each OS argument's bytes widened
  to `i8` with a trailing NUL appended (`OsStrExt::as_bytes` +
  `chain(once(0i8))`), reproducing C's NUL-terminated `char*` strings
  byte-for-byte, so a non-Unicode argument round-trips where a `String`
  collection would panic or lossily replace — then calls
  `c_main(__emitrust_argv.len() as i32, &__emitrust_argv)`. The async
  twin drives it on the tokio current_thread runtime; the arity-0/1
  wrappers (`main(void)`, argc-only) stay BYTE-IDENTICAL to the pre-C3
  emitter (zero churn on every non-argv crate). ADMITTED READ GRAMMAR
  (planArgvUsesFor, admit-all-or-drop): (1) a whole-value `argv[i]` fed
  as a DIRECT `printf` `%s`/`%.Ns` argument with no field width — it
  borrows argument i's NUL-terminated byte run as an
  `emitrust.argv_arg` -> `!emitrust.ref<!emitrust.slice<i8>>`
  (`&table[i as usize][..]`); (2) `argv[i][j]` bytes consumed as VALUES
  — `%c` holes, comparisons/NUL-scans (`while (argv[1][n]) n++`),
  scalar reads, and `%d`/`%i` — resolving through the SHARED slice place
  (`emitrust.argv_arg` + deref + subscript) via a new emitLValue argv
  branch, so a byte read, a comparison, and a `%d` all read one way; and
  (3) argc used as the plain i32 loop/print scalar. EVERYTHING ELSE
  leaves the parameter unadmitted and keeps the historical located
  rejection ("use of main's argv parameter (command-line argument values
  are not modeled)", RejectionLedger `{"use of main's argv", "argv"}`,
  wording and tag byte-identical): stores of `argv`/`argv[i]` to a local,
  escapes to other calls, address-of (`&argv[i]`), pointer arithmetic
  (`argv++`, `*argv`), writes, pointer-value tests (`argv[0] != 0`), and
  a `%s` with an explicit field width (`%10s`, which the raw helper
  cannot pad). LATIN-1 BYPASS (the reason for the raw `*_out` helpers):
  an argv-fed `%s`/`%c` hole in the stdout `print!` context must NOT flow
  through the `__emitrust_cstr`/`__emitrust_fmt_c` Display funnels, whose
  byte-to-char widening emits two-byte UTF-8 for any argument byte
  128..=255 and would double-encode a non-ASCII argument (the spike's
  matrix column C, the `héllo` case). Instead translatePrintfFormat
  splits the format: the pending segment flushes as its own `print!`
  call, and the hole renders through the on-demand raw helpers —
  `__emitrust_cstr_out` (NUL-scan + `write_all` of the raw bytes),
  `__emitrust_cstr_n_out` (the `%.Ns` twin), `__emitrust_byte_out` (one
  raw byte) — all writing to the SAME globally buffered stdout handle
  `print!` locks, so segment ordering holds even on block-buffered pipes.
  A `%d`/`%i` of an argv byte is NOT bypassed: it flows through the exact
  integer path (i8 -> cast -> `{}`). HARNESS GATE (spike-measured): the
  native oracle and the crate binary live at different absolute paths, so
  a program echoing `argv[0]` (now importable) would diff on the path
  alone. run_realworld.py's `run_command` gained an optional `argv0`
  that runs `Popen([argv0]+cmd[1:], executable=cmd[0], ...)`; the SAME
  `argv0` ("./" + name) is passed to BOTH the native and crate runs,
  equalizing `argv[0]` — a no-op for every existing program (none read
  argv[0]). TESTS: test/Import/C/main-argv.c (admitted signature seat +
  the printf bypass split), main-argv-invalid.c (the five non-admitted
  shapes, exact wording twin), test/EndToEnd/argv-echo.c (byte-diff vs
  clang native across four arg vectors — no args, several, spaces, and a
  UTF-8 arg — echoing only argv[1..] to dodge the path difference),
  the dialect round-trip test/Dialect/EmitRust/argv-table{,-invalid}.mlir
  and target test/Target/Rust/argv-table.mlir, and RealWorld argv-echo
  promoted to expected-transpile.txt behind the argv0 gate. The FR-51
  dropped-main Driver test now STORES argv (still non-admitted) to keep
  exercising the recovery path, since a `%s` of argv is now admitted.
  Suite 517 -> 523.
  BOX CLOSED (2026-08-04, orchestrator). This was the C99 roadmap's LAST
  unchecked box; the design is now all-boxes-checked. The original
  acceptance ("pointers to pointers and pointer members inside structs")
  is not a single landing but a settled disposition across four measured
  fronts, each recorded above with its own verdict — the box closes on
  the union of them, NOT on a claim that every ptr-to-ptr shape now
  transpiles. Owner questions Q1-Q4 were answered first (2026-08-03):
  &mut-i64-cursor only (no multi-return), no synthesized region structs,
  plain idiomatic indexing with a deterministic panic on a stale handle,
  Option<index> for NULL-able handles. Against those, the fronts map:
  (member-pointer half) FR-37/FR-38 self-referential array-member
  pointers via `emitrust.enum_def` + `match`, and the array-rooted
  returned-pointer half via FR-36 — both pre-existing and unchanged.
  (C1) single-global-or-NULL out-param cursors (Shape G) LANDED, mapping
  a `T **p` write of one file-scope global / NULL / `cond ? g : NULL` to
  `&mut Option<i64>`. (C2) container_of intrusive-container recognition
  is a recorded NO-GO (three measured blockers; the four link types stay
  the located rejection). (C3) the argv cursor table LANDED (this
  record), retiring the W4.3 gap. (C4) the residual — body-less T**
  prototypes / signature-starved redeclarations — is NOT a ptr-to-ptr
  modelling gap but FR-58's signature-starvation axis, recorded there
  with its preconditions; it does not hold this box open. Everything
  outside these admitted subsets keeps its byte-identical located
  rejection: the box is closed on a PROVEN disposition (landed, NO-GO,
  or reassigned-and-recorded) for every front, with the safe failure
  direction — a loud rejection — preserved for all the rest.
- [x] C99-44 Unions. DECIDED and SHIPPED: the one-slot struct model —
  a supported subset with documented located rejections, not an enum
  mapping and not a blanket rejection. A named or untagged union
  RecordDecl imports as a ONE-FIELD struct whose storage field is the
  slot arm's leaf (name and type), generalizing the CTS-R2
  anonymous-union slot machinery — every arm's spelling aliases that
  slot, so no non-slot arm name reaches the IR. The slot is the FIRST
  arm, except in the byte-array mix (CTS-R3 T1.1) where it is the first
  non-array arm. SUPPORTED ARM MATRIX: arms that all map to one
  identical type (exact by C11 6.5.2.3); same-width integer arms
  differing only in signedness (accesses through the differently-signed
  arm wrap a bit-exact `emitrust.cast` reinterpret, reads slot->arm and
  stores arm->slot); a float arm paired with a same-width integer —
  float/32-bit and double/64-bit — whose accesses wrap an
  `emitrust.bitcast` (a dedicated dialect op rendered as Rust
  to_bits/from_bits, bit-exact by definition, deliberately distinct
  from the value-converting `as` of `emitrust.cast`); equal-total-width
  constant integer-array arms over an integer slot (type-level
  admission only, every access through the array arm rejects at the
  access site); single-arm unions; unions as struct members; union
  globals with constant initializers (the initializer lands on the
  slot; a float-pun arm's constant crosses the domain at compile time
  as its exact bit pattern); designated local initializers through pun
  arms (place takes the slot's type, value reinterprets onto it);
  compound assignment, increment/decrement, and value-position
  assignment through pun arms (load reinterprets slot->arm, the
  computation runs at the arm's type, the store reinterprets back).
  REJECTION MATRIX, all located: bit-field arms, unnamed/anonymous
  arms, and pointer arms (`unsupported: union with a ... arm`); empty
  unions (`unsupported: union with no members`); scalar arms of
  differing sizes — int/int, float/int, double/float
  (`unsupported: union arms of differing sizes`); aggregate or enum
  arms not identical to the slot's type
  (`unsupported: union arm cannot alias the storage slot`); access
  through a byte-array arm
  (`unsupported: union byte-array arm access`); taking the address of
  any union member (`unsupported: taking the address of a union
  member`); ++/-- through a float pun arm (the general non-integer
  ++/-- rejection, reached because the load reinterprets to the arm
  type first). Anonymous union MEMBERS (CTS-R2) keep the stricter
  identical-leaf-only admission — no puns through anonymous arms.
  Traceability: dialect `include/EmitRust/EmitRustOps.td` +
  `lib/EmitRust/EmitRustOps.cpp` (`emitrust.bitcast`, verifier:
  exactly one f32/f64 side and one same-width integer side) and
  `lib/Target/Rust/TranslateToRust.cpp` (`emitBitcast`); importer
  `lib/ImportC/ImportC.cpp` (`collectUnionSlot`,
  `flattenedFieldStorage`, `reinterpretScalarBits`,
  `reinterpretUnionArmRead`/`reinterpretUnionArmWrite`, pun hooks in
  `emitRecordInitField`, `emitCompoundAssignToPlace`,
  `emitIncDecValue`, `emitBinaryRValue`, cross-domain constants in
  `convertAnonymousSlotInit`, union routing in
  `mapType`/`importRecord`/`convertAPValueInit`); tests
  test/Dialect/EmitRust/ops.mlir + invalid.mlir (bitcast roundtrip and
  verifier pins), test/Target/Rust/arith.mlir (to_bits/from_bits
  rendering incl. signless `as` wrapping), test/Import/C/unions.c
  (alias, single-arm, struct member, global initializer, signedness
  pun, untagged local, float pun both slot orders, double pun,
  designated pun-arm local init — the last also pins the fixed
  arm-typed-store-into-slot-field defect),
  test/Import/C/unions-invalid.c (int size mismatch, float/long,
  double/float, aggregate arm, pointer arm, bit-field arm, empty
  union, float-arm ++ — all located; supersedes the deleted
  test/Import/C/union.c whose int/float rejection pin the float pun
  made stale), test/Import/C/union-bytearray-arm.c + -invalid.c,
  test/EndToEnd/unions.c (differential: both pun families in both
  directions, globals, designated inits, compound assign/++/value
  position), c-testsuite 00042.c, 00210.c, 00218.c (see CTS-R3).
- [x] C99-45 Bit-fields. DECIDED: mask-and-shift accessor synthesis over
  per-run backing integers (not a documented rejection). PARTIAL —
  landed with c-testsuite 00218 (T1.2). Layout: each maximal run of
  consecutively declared bit-field members packs LSB-first in
  declaration order into a synthesized backing field `__bits<n>` (n
  counts runs from 0 across the flattened record) of the smallest
  unsigned type (ui8/ui16/ui32/ui64) holding the run's total bits; runs
  split at any non-bit-field member, and the bit-field members' own
  names never appear in the struct_def. This layout is the project's
  OWN and deliberately NOT ABI-compatible with the C compiler's
  bit-field layout — which is why `sizeof`/`_Alignof` of any type
  containing a bit-field record is a located rejection
  (`unsupported: sizeof of a struct with bit-fields`): the C layout
  number would promise an ABI the emitted Rust does not keep. Reads
  load the backing field, `emitrust.shr` by the bit offset (always
  emitted, offset 0 included), `emitrust.and` with the width mask, then
  convert: unsigned/_Bool/enum-typed fields ZERO-extend from the
  unsigned backing (the 00218 core: an `enum : 8` field holding 152
  reads back 152, never -104, regardless of the enum's own underlying
  signedness), plain-int signed fields sign-extend from their declared
  width via `arith.shli`/`arith.shrsi` by (type width - field width).
  Writes are read-modify-writes: load, clear the window with the
  complement mask, cast the RHS to the backing type (from the enum type
  for enum RHS), truncate with the width mask (always a separate step),
  `emitrust.shl` by the offset (always emitted), `emitrust.or`, assign
  the backing field; a value-position assignment stages the truncated
  post-store field value (converted like a read). Alongside: struct
  MEMBER names that are Rust keywords now mangle with a trailing
  underscore (`type` -> `type_`, 00218 declares a member named `type`)
  instead of rejecting — members only; struct/enum/function/global
  keyword names keep their rejections — and a mangle collision (`type`
  next to `type_`) is a located rejection. Out of scope, all located
  rejections: zero-width and anonymous bit-fields, runs wider than 64
  bits, bit-field arms in unions (unchanged wording), compound
  assignment / increment on bit-fields, aggregate and global-constant
  initializers touching bit-field members. Traceability: importer
  `lib/ImportC/ImportC.cpp` (`collectRecordFields` run packing,
  `bitFieldAccessInfo`, `emitBitFieldRead`, `emitBitFieldAssign`,
  `convertBitFieldFieldValue`, `createBitFieldMask`,
  `emitMemberBasePlace`, `mangleMemberName`/`flattenedFieldName`,
  `typeContainsBitField` in `emitSizeofAlignof`); tests
  test/Import/C/bitfields.c (packing, read/write accessor shapes,
  signed extension, run splitting), bitfields-invalid.c (union arm,
  sizeof), struct-member-keyword.c (mangle end-to-end),
  keywords-invalid.c (non-member keyword rejections stay),
  test/EndToEnd/bitfields-zeroextend.c (00218 core, differential),
  test/EndToEnd/bitfields-flags.c (mixed runs, RMW isolation,
  signed truncation), c-testsuite 00218.c.
- [~] C99-46 Dynamic memory: malloc, calloc, realloc, free. Stage 1
  (W4.2e, FR-39): a LOCAL const-size flat buffer (`T *p =
  malloc(cap*sizeof(T)); p[i] = ...; free(p);`) synthesizes a mutable
  `[T; CAP]` backing + cursor (Part A); a fixed-CAP node pool with index
  handles (`malloc(sizeof(struct T))` in a foldable-trip-count loop
  building a self-referential linked structure) synthesizes a `[T; CAP]`
  pool + nullable `Option<usize>` index handles (Part B). `free` of a
  recognized allocation is a no-op (the backing/pool drops at scope end;
  a defined program never reads freed storage). OUT (still rejected,
  located): realloc (the fixed backing cannot resize); a RETURNED heap
  pointer (a callee-local backing would dangle -- keeps binary-tree out);
  an unbounded/non-foldable pool capacity; a heap allocation that escapes
  (global store, address-of, non-`free` call). `Vec<T>` is the documented
  future generalization for an unfoldable capacity.

### Hosted library surface (beyond the language)

- [x] C99-47 The full printf format language: %s, %c, %u, %x, %o, %e,
  %g, %p, field width, precision, flags, and length modifiers, mapped
  onto Rust format specifications. CLOSED as supported-subset plus
  documented rejections: every form below is either byte-exact against
  glibc or carries a located rejection pinned by a negative test —
  silent divergence is structurally excluded. The supported grammar is
  `%[flags][width][.precision][length]conv` with all five C99 flags
  (`-`, `0`, `+`, ` `, `#`), decimal width and precision (a bare `.`
  is precision 0), lengths `l`/`ll` (i64/u64) and `h`/`hh` (the
  promoted argument reduced to short/char range by an `as`-cast, C99
  7.19.6.1p7 masking semantics), and conversions d/i, u, x/X, o, c, s,
  f/F/e/E/g/G, and %%.
  Directives that map 1:1 onto Rust format specs keep the direct
  mapping (`%5d`→`{:5}`, `%-5d`→`{:<5}`, `%05d`→`{:05}`,
  `%04X`→`{:04X}`; Rust's zero pad is sign-aware like C's; on %c/%s a
  width is explicit alignment, `{:>5}`/`{:<5}`, because C right-aligns
  text where Rust's string formatting left-aligns). Everything beyond
  that subset routes through on-demand module-level helpers
  implementing the C99 rendering rules exactly, validated byte-exactly
  against glibc printf on a structured battery plus fuzzed batteries of
  4000 random f64 bit patterns and 4000 random integers across the
  flags/width/precision matrix (zero diffs): `__emitrust_fmt_int` (+
  `__emitrust_fmt_i64`/`__emitrust_fmt_u64` wrappers) renders integer
  precision (digits zero-padded after the sign; value 0 with precision
  0 prints nothing; `0` ignored next to a precision or `-`), the
  `+`/` ` sign slots, and `#` alternate forms (octal leading zero only
  when needed, 0x/0X on nonzero values, prefixes inside the `0` width
  padding); `__emitrust_fmt_float` (+ `__emitrust_fmt_edigits`/
  `__emitrust_fmt_exp`) renders f/e/g on Rust's exact correctly-rounded
  decimal conversion (`{:.*}`/`{:.*e}` round half-to-even on the exact
  binary value, matching glibc): %e with sign-always two-digit
  exponents, %g with the C99 f/e style switch on the post-rounding
  exponent, trailing-zero trimming, and glibc's `%#g` rounding-carry
  quirk (a carry landing exactly on the 10^P decade drops the mantissa
  fraction — `%#g` of 999999.5 is `1.e+06` — while an exact power of
  ten keeps it, `1.000e+04`; detected via the shortest-round-trip
  pre-rounding exponent); non-finite values print C's spellings
  (`inf`/`nan`, uppercase under F/E/G, `-nan` when the sign bit is
  set) and pad with spaces even under `0`, as glibc does. Bare
  %f/%lf keeps the `__emitrust_fmt_f64` fast path. %s gained precision
  (`%.Ns`): a string literal truncates at import time (only the
  retained prefix is validated), the slice shapes route through
  `__emitrust_cstr_n`, which stops at N bytes or the first NUL,
  whichever comes first (C99 7.19.6.1p8: no terminator needed when the
  precision bounds the read). u/x/X/o still `as`-cast the argument to
  the directive's unsigned type (`%x` of -1 is ffffffff) and
  mismatched integer widths truncate like C's x86-64 varargs read.
  Rejected by policy, each with a located diagnostic and a pinned
  negative test: %p (pointer provenance is compiled away by the FR-28
  decomposition, so no address exists to print), %n (writes through a
  pointer), %a/%A (hex float), `*` width/precision (runtime-supplied),
  lengths `L` (no long double representation) and `j`/`z`/`t`,
  undefined-by-C99 flag combinations rejected rather than silently
  dropped (`%#d`, `%+u`, `%0c`, `%010s`, `%.3c`), wide %lc/%ls,
  h/hh/ll on floating conversions, widths/precisions over nine digits,
  NaN floating-point *constants* at the Rust-emission boundary (Rust
  does not guarantee its NAN constant's sign/payload; infinity
  constants are emitted as `f64::INFINITY`/`f64::NEG_INFINITY`), and
  argument type mismatches. The char-path helpers stay ASCII-only by
  design (C99-48).
  Definition guard: the whole by-name printf lowering applies ONLY when
  the project supplies no printf definition of its own — the same
  `!callee->getDefinition()` guard puts/putchar, the string.h surface,
  and the fn-pointer alias planner already carry. A project-supplied
  printf (any signature; <stdio.h> is never imported) imports and is
  called like any user function in BOTH statement and value positions;
  a variadic va_list-free user printf flows through the fixed-prototype
  variadic machinery (C99-37/varargs-def), dropping effect-free
  call-site extras its body cannot observe. The "printf return value
  must be unused" rejection now applies only to the hosted
  (definition-less) lowering.
  (test/Import/C/printf.c, printf-extended.c, printf-precision.c,
  printf-float-forms.c, printf-extended-invalid.c, sprintf-invalid.c,
  strings.c, strings-invalid.c, test/Import/C/printf-user-defined.c,
  test/EndToEnd/printf-formats.c — differential against the clang-built
  native binary over every supported form with boundary values —
  test/EndToEnd/strings.c, test/EndToEnd/printf-user-defined.c,
  test/EndToEnd/printf-user-defined-nonvariadic.c)
- [x] C99-48 A curated stdio/stdlib/string/math subset mapped to Rust
  equivalents (putchar, puts, abs, string functions over the C99-28
  representation, math intrinsics onto f64 methods), each function
  individually tested differentially. Curation complete: every function
  below is either mapped to safe Rust over the decomposed representation
  (no libc linkage, no unsafe) or rejected with a located diagnostic and
  a recorded rationale; every intercept carries the `!getDefinition()`
  guard (a project-supplied function of a curated name imports as an
  ordinary call), and every UNCURATED libc function keeps the
  system-header use-site rejection ("declared in a system header; not
  part of the supported C subset"), pinned by negative tests (rand,
  strtok, strstr, tgamma). stdio: statement-position
  `puts(s)` and `putchar(c)` are lowered by name when the project
  supplies no definition of its own (a user-defined puts/putchar stays
  an ordinary call): puts to `println!` through the %s machinery (both
  %s shapes), putchar to `print!` of the argument through
  `__emitrust_fmt_c`, matching C's conversion to unsigned char. The
  char helpers are ASCII-only by design: C writes the raw byte where
  Rust would encode code points 128..=255 as two UTF-8 bytes, so
  non-ASCII string data is rejected at import (see C99-28/47) and the
  helpers are exact for everything that gets through. Value uses of the
  puts/putchar result keep located rejections.
  math.h (doubles are f64): definition-less fabs/sqrt/floor/ceil with
  the standard double(double) prototype lower to the matching f64
  methods (`f64::abs`/`f64::sqrt`/`f64::floor`/`f64::ceil`) — these are
  IEEE-754-exact operations (fabs/floor/ceil exact, sqrt correctly
  rounded), so every conforming implementation agrees bit for bit and
  the mapping needs no libm argument at all. `sin` lowers to `f64::sin`
  on the weaker "both sides resolve to the platform libm" argument,
  verified differentially. exp/log/pow are REJECTED by policy with the
  located "has no bit-exact Rust mapping" diagnostic — the recorded
  rationale: C imposes no accuracy requirement on them (C99 F.9 makes
  no correctness guarantee), libm implementations disagree in the last
  bits, and rustc may constant-fold a constant argument through a
  different libm than the differential oracle's glibc, so no
  bit-exactness argument can be made; widening the pinned sin exception
  was considered and declined. Every other math function keeps the
  system-header rejection (the `hostedMathCallee` table in ImportC.cpp
  is the extension point).
  stdlib.h: definition-less abs/labs lower to
  `i32::wrapping_abs`/`i64::wrapping_abs` — abs(INT_MIN)/labs(LONG_MIN)
  is C UB (7.20.6.1p2), refined to the deterministic two's-complement
  wrap the oracle's platform also produces (Rust's plain `abs` panics
  only in debug profiles and was rejected as profile-dependent). atoi
  parses its argument's char region (the strlen shapes: a char array, a
  literal backing, or a pointer into either) through the one-per-module
  `__emitrust_atoi` helper with C's exact 7.20.1.2 semantics: skip
  isspace bytes, one optional sign, decimal digits to the first
  non-digit, 0 when no digits exist (leading junk included); values out
  of range are C UB (7.20.1p1) refined to deterministic i32 wrapping,
  and a region ending before any terminator stops at the region end
  (reading past the array is C UB, refined — every helper access stays
  a bounds-checked slice index). Statement-position `exit(status)`
  lowers to `std::process::exit(status as i32)`, matching C's
  termination and exit-status semantics (both report the low byte on
  this target, pinned by the exit-status differential); exit returns
  void in C, so no value position exists. Every other stdlib function
  (rand, strtol, qsort, general malloc/free beyond the CTS-P4
  single-constant-size calloc/malloc carve-out, ...) keeps the
  system-header rejection.
  string.h (CTS-L1): definition-less strcpy/strncpy/strcat/memset/
  memcpy/memmove lower by name in statement position, strcmp/strncmp/
  memcmp and strlen in value position, and strchr/strrchr where a printf
  %s argument or a null-pointer comparison consumes the result — each to
  a one-per-module safe Rust helper over `&[i8]`/`&mut [i8]` slices of
  the argument's char region (a char array, a string-literal backing, or
  a pointer into either), so every access is a bounds-checked slice
  index with no unsafe. Comparison helpers compare as unsigned char per
  C; strchr/strrchr return the found index or -1 (C's NULL), which %s
  offsets into the region and null comparisons test directly;
  same-object memcpy borrows the array mutably once and passes both
  cursors (`copy_within`, refining C's undefined overlap). memmove
  shares memcpy's lowering EXACTLY, and the mapping is exact rather than
  a refinement: distinct char regions are distinct array objects and can
  never overlap, and the same-object shape is `copy_within`, which IS
  memmove's overlap-correct copy — no temporary needed. Copy results
  are statement-position only, a copy source sharing the destination's
  object, literal-region destinations, and uncurated functions (strstr,
  strtok, ...) keep located rejections; the helper namespace
  `__emitrust_*` is reserved.
  stdio FILE* streams (CTS-T1.3, 00187): a `FILE *` local declared
  uninitialized or fopen-initialized is an OWNED handle over std::fs —
  an `emitrust.variable` of the opaque `__EmitrustFile` type, a
  one-per-module enum over Null / Read(std::fs::File) /
  Write(std::fs::File) — and every stream operation borrows it `&mut`
  through a one-per-module `__emitrust_f*` safe-Rust helper (the
  sprintf/cstr helper convention; zero unsafe, every buffer access a
  bounds-checked slice index). Supported surface, sequential byte-wise
  I/O only: fopen(literal-path, "r"/"w") — literal-only paths, v1 —
  where "w" creates/truncates and a failed open is
  `__EmitrustFile::Null`, C's NULL, so `if (!f)` and null comparisons
  work through `__emitrust_file_ok`; fgetc/getc as an i32 byte with EOF
  the plain `arith.constant -1 : i32` met by `arith.cmpi` (an equality
  comparison folds a negated signed integer literal — EOF's `(-1)` — to
  a single negative constant; relational shapes keep the historical
  `0 - x` lowering pinned in enum-int.c); byte-wise
  fread/fwrite(ptr, 1, n, f) over char-region slices returning the
  size_t count through the strlen-style i64 helper + cast convention;
  fgets(buf, size, f) reading at most size-1 bytes, stopping after
  '\n', NUL-terminating, whose helper returns -1 for C's NULL so the
  pinned `while (fgets(...) != NULL)` and bare truth tests fold to an
  index comparison (the strchr convention); and fclose, which resets
  the handle to Null so the SAME variable is reassignable by a later
  fopen (00187's serial open/close cycles). UB-refinement stance: fopen
  failure takes C's NULL path; reading/writing a Null or
  wrong-direction handle, I/O errors beyond EOF, and out-of-range
  counts are C undefined behavior refined into deterministic panics.
  Located rejections pin the slice boundary: file positioning
  fseek/ftell/rewind (streams are sequential-only), fopen modes other
  than "r"/"w", fprintf to a real FILE* stream (only the devirtualized
  stdout form), fread/fwrite element sizes other than 1 (byte-wise
  only), FILE* crossing a user-defined function boundary (parameter or
  return — fired at the callee's first stream use, so a
  never-streaming FILE* parameter keeps its historical call-site
  rejection, pinned in fnptr-devirt-invalid.c), and FILE* anywhere but
  a function-local variable (struct member of a main-file record,
  global, array element). Non-handle FILE* shapes — `stdin`/`stdout`
  references, `FILE *g = stdout;` — keep the historical pointer
  machinery and its pinned wordings, and a bare `fopen(...)` statement
  keeps the system-header rejection.
  (test/Import/C/stdio-file.c, stdio-file-invalid.c,
  test/EndToEnd/stdio-file-roundtrip.c, 00187.c in the ledger)
  (test/Import/C/strings.c, strings-invalid.c, strings-hosted.c,
  strings-hosted-invalid.c, math.c, libc-subset.c,
  libc-subset-invalid.c, test/EndToEnd/strings.c, strings-hosted.c,
  math-sin.c, libc-subset.c)

Where an item above concludes in a documented rejection (varargs
definitions, irreducible goto, _Complex, and similar), that rejection with
a located diagnostic is itself the specified behavior and gets a negative
regression test, exactly like the current subset boundary (FR-19). The
Non-Goals section below describes what the MVP rejects today; items listed
there move out of it as their roadmap boxes are ticked.

