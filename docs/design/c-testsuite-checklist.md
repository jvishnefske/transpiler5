## c-testsuite Remaining-Failure Checklist

Ledger as of 2026-07-20: 220 total / 220 passed / 0 miscompiled /
0 unsupported — COMPLETE (was 150/70 at commit a091423, when this checklist was
drawn up; the quick wins, the CTS-S7/R5/P2/P4/P6 partials, and
CTS-S1/S2/S4/S6/P1/P5/P7/P8/R1/R2/R4/L1/L2 landed since, and the
2026-07-18 TDD wave took 200 -> 213: unions as one-slot structs
[+00042], void*-wildcard/member-base/null-ternary provenance [+00039
+00103 +00144 +00163], cell-slice global params + byte puns [+00181
+00217], fixed-prototype variadics + sprintf [+00140 +00186], the
StmtExpr pack [+00213 +00214], and fn-ptr devirtualization +
global-pointer returns [+00089 +00189]; the T1.1 ledger wave took
213 -> 215: dead-VLA elision + constant-LHS short-circuit folding,
byte-array union arms, and the local void* fn-ptr holder [+00207
+00210]; the T1.2 wave took 215 -> 216: C99-45 bit-field accessors
over backing runs + keyword-member mangling [+00218]; the T1.3 wave
took 216 -> 217: FILE* as an owned std::fs handle — fopen/fread/
fwrite/fgetc/fgets/fclose, the C99-48 stdio slice [+00187]; the 00209
wave took 217 -> 218: K&R callsite-prototype inference, FR-29; the CTS
00204 wave took 218 -> 219: long-double-as-f64 (C99-8 revision) +
va_list monomorphization (C99-37 revision) + `const char **`
string-cursor parameters + keyword-function mangling + call-result
temporaries [+00204]; and the CTS-BR wave took 219 -> 220: the u8-only
byte-region aggregate model [+00216]). THE SUITE IS COMPLETE:
220/220 passed, 0 miscompiled, 0 unsupported — every former
permanent-out disposition was overturned by a dedicated spike-scoped
TDD wave (2026-07-19/20). The overturned reasonings are preserved
below inside each LANDED entry for the record.
Per-test dispositions:
- 00204 PASSES (disposition OVERTURNED 2026-07-19; formerly
  PERMANENT-OUT on "struct-typed varargs / HFA calling convention,
  fundamentally outside safe-Rust emission"). What changed: the HFA
  calling convention never needed modeling — per-call-site
  monomorphization (C99-37 revision) turns each `va_arg(ap, struct
  hfa34)` into a dispatch over ordinary by-value Copy parameters, and
  the long-double blocker dissolved into the f64 substitution (C99-8
  revision), sound here because every 00204 value is f64-exact at one
  printed decimal. The remaining companions (the keyword-named `match`
  helper with its advancing `const char **` cursor, `fr_hfa12().a`
  call-result member reads, `struct s1 t1 = fr_s1()` initializers)
  landed alongside. Byte-exact against the native oracle: 35 myprintf
  call sites, 33 distinct clone signatures, 14 struct-typed va_arg
  sites, %.1Lf output.
- 00209 LANDED (2026-07-19, overturning the earlier upheld rejection):
  K&R callsite-prototype inference (FR-29). Rule: a call with
  arguments whose callee, after the `(*fp)` deref-peel, is a
  `DeclRefExpr` to a local-storage ParmVarDecl/VarDecl of
  pointer-to-`FunctionNoProtoType` infers that decl's prototype from
  the call's argument types — clang has already applied the default
  argument promotions at a no-proto call (C11 6.5.2.2p6), so the
  promoted types are used verbatim (char -> i32 via `arith.extsi`,
  float -> f64 via `arith.extf`, the casts the ordinary typed-call
  conversion emits) — plus the declared return type. The decl then
  DECLARES at the refined `!emitrust.fn_ptr<promoted... -> ret>`
  (parameter and local place alike), its argument-carrying calls lower
  through the ordinary typed `emitrust.call_indirect` path, and
  binding a real function to it resolves against the refined
  signature. Multiple call sites for one decl must agree. Located
  rejections: a second disagreeing site is the NEW
  "unsupported: conflicting inferred prototypes for function pointer
  '<name>'" at that site; non-decl-traceable callees (struct members,
  array elements, call results) RETAIN "unsupported: call with
  arguments through a function pointer without a prototype"; an
  incompatible function bound to an inferred decl keeps
  "unsupported: function '<name>' does not match the function pointer
  signature" (resolveFunctionPointerDecl exact equality); and an
  unrefined no-proto VALUE passed into a refined position keeps
  "unsupported: call argument type mismatch" at the passing call site.
  A no-proto pointer never called with arguments stays at the
  unrefined `fn_ptr<() -> T>` mapping — inference is per-decl, not
  per-typedef (00209's f5 `fptr1` argument is untouched by f1's
  refinement of the same spelling).
  (test/Import/C/fnptr-noproto-infer.c, fnptr-noproto-infer-invalid.c,
  fn-pointers-invalid.c noproto-args; differential
  test/EndToEnd/fnptr-noproto-infer.c with executed inferred calls;
  c-testsuite 00209 — ledger 217 -> 218 passed / 0 miscompiled)
- 00216 PERMANENT-OUT (wave in flight): beyond its first blocker (flexible array
  member, "00216.c:46:12: error: unsupported: flexible array member" —
  the C99-17 dedicated wording that replaced the generic
  "non-constant array size" fallback) it
  requires byte-exact struct layout INCLUDING padding (a print macro
  walks `(u8*)&x` over sizeof(x)), GCC range designators, and
  compound literals with relocations — byte-exact ABI layout is
  antithetical to the project's safe-Rust value model (the same reason
  bit-field layout is deliberately non-ABI, see C99-45).
- 00216 LANDED (CTS-BR, the u8-only byte-region aggregate model): the
  former PERMANENT-OUT reasoning ("byte-exact layout is antithetical
  to the safe-Rust value model") only holds for aggregates with
  padding or mixed-width leaves. An aggregate whose scalar leaves are
  ALL `unsigned char` — u8 members, u8 arrays, nested such structs,
  u8-only unions including unnamed arms, empty structs contributing
  zero bytes, GNU zero-length arrays contributing zero, and a flexible
  array member contributing zero to sizeof — is padding-free BY
  CONSTRUCTION, so its object representation is exactly its value
  representation and safe Rust can model it byte-exactly.
  Classification: such a record is a BYTE REGION; a union arm with
  non-u8 leaves is tolerated type-level only as a constant array whose
  size equals the union's (the in6_addr u16[8]-over-u8[16] alias);
  mixed-size non-u8 arms keep the "unsupported: union ..." rejection;
  any non-u8 leaf keeps the aggregate on the typed struct_def path.
  Representation: byte-region objects are plain `!emitrust.array
  <Nxui8>` (N == sizeof; arrays of byte-region records flatten to one
  n*sizeof region); global initializers fold to complete zero-filled
  byte images from the APValue against the target layout, with a
  static FAM-tail initializer folding into an EXTENDED image while
  sizeof stays FAM-free (gw: 22-byte sizeof, 30-byte image); locals
  are bare zero-defaulted `emitrust.variable` regions initialized per
  byte (folded ui8 constants, embedded per-byte region copies for
  struct-value elements, runtime scalars through their AST casts);
  member access is `emitrust.subscript` at the member's constant byte
  offset; `(u8 *)&x` is the region base (a byte view of a NON-u8
  aggregate rejects at the cast: "unsupported: byte view of an
  aggregate with non-byte members"); `&x.member` is base + offset;
  struct copy/assign/init-from-deref are per-byte region copies.
  Pointers to byte-region records are `!emitrust.slice<ui8>`
  parameters (shared `&[u8]` for const pointees) riding the slice
  decomposition with byte-granular cursors; byte-region GLOBALS passed
  by address stage a whole-image copy, with mutable parameters storing
  the image back after the call. FAM/zero-length RUNTIME accesses
  reject per the amended C99-17. The remaining 00216 constructs also
  landed: GCC range designators arrive pre-expanded in clang's
  semantic form; fn-ptr TABLES (`T (*t[N])(...)`) import as
  `array<Nx!emitrust.fn_ptr<...>>` globals with folded Some(target)
  elements, `t[i]()` is subscript + call_indirect, and a runtime store
  to a slot rejects ("unsupported: assignment to a function-pointer
  array element"); a `void *` struct member whose every stored value
  is the address of one signature's function (the T1.1 holder bound
  extended to members) retypes to a fn_ptr member, readable under a
  cast to that signature. Byte-exact layout INCLUDING padding remains
  out for non-u8 aggregates (the same reason bit-field layout is
  deliberately non-ABI, see C99-45).
  (test/Import/C/byte-region-aggregates.c, byte-region-init.c,
  byte-region-aggregates-invalid.c, fnptr-table.c,
  fnptr-table-invalid.c, flexible-array-invalid.c,
  test/EndToEnd/byte-region-walk.c differential; ledger +00216.)
This checklist partitions the original 70 by sole blocker: each item lists the
exact tests it unlocks, so the sum of all items is exactly 70. Same
checkbox discipline as above — tick only when the referenced tests pass
under ninja check-emitrust and the ledger ratchets with zero new
miscompiles. Counts are first-blocker attributions; unlocking one item
can surface a second blocker in the same test (the interaction effect
observed when C99-33 + C99-47 together unlocked 00215).

### Quick wins (7 tests, no design decisions needed)

- [x] CTS-E1 (3) Thread-local closure binder shadows a mutable global
  named `c`: TranslateToRust.cpp hardcodes `.with(|c| c.get())` /
  `.with(|c| c.set(v))`, so a C global literally named `c` makes rustc
  resolve the closure pattern against the thread-local key and fail with
  E0308. Rename the binder to a reserved identifier (e.g. `__tl`).
  These are the only three tests that transpile but fail rustc.
  (00127.c, 00128.c, 00142.c)
  (Done: the binder is `__emitrust_tl`, following the `__emitrust_`
  reserved-prefix convention; the importer rejects a C global spelled
  `__emitrust_tl` like the other reserved helper names. 00127.c, 00128.c,
  00142.c now pass and are in the manifest — ledger 153 passed / 67
  unsupported / 0 miscompiled. Pinned by test/Target/Rust/globals.mlir
  (global named `c`), test/EndToEnd/globals.c (rustc-level), and
  test/Import/C/keywords-invalid.c (reserved-name rejection).)
- [x] CTS-F2 (3) Unreferenced main-file declarations must not demand
  definitions: `extern int x;` or a repeated prototype `int foo(void);`
  that is never referenced currently rejects with "referenced but not
  defined in any translation unit" even though nothing references it.
  Apply the C99-39 referenced-only policy to main-file prototypes and
  extern objects: skip if unreferenced, reject at the use site otherwise.
  (00094.c, 00108.c, 00162.c)
  Done: importFunction/importGlobalVar skip a body-less prototype or
  extern-only object whose redeclaration chain is unreferenced (before
  signature mapping, so unsupported shapes in dead prototypes cannot
  reject either); finalizeProject erases use-free external funcs and
  locates the referenced-but-undefined rejection at the first use site.
  Ledger 150 -> 153, zero miscompiles.
  (test/Import/C/unreferenced-extern-global.c, unreferenced-prototype.c,
  multi-tu-undefined-extern-global.c)
- [x] CTS-S3 (1) Block-scope function prototypes (`int f1(char *);`
  inside a function body): hoist the declaration to module scope and
  continue; currently "unsupported declaration inside a function body".
  (00078.c) Done: `emitStmt` routes a `FunctionDecl` in a `DeclStmt`
  through `importFunction`, the same path as a file-scope prototype
  (external linkage per C11 6.2.2p5); other in-body declarations still
  reject. (test/Import/C/fn-prototypes-local.c; 00078.c in the ledger)

### Pointer model extensions (30 tests, builds on FR-28/C99-26)

- [x] CTS-P1 (7) `char *` bound to string literals: a read-only
  string-region class in PointerRegionAnalysis whose base is the literal
  (`&'static [u8]`/`&'static str`) and whose cursor indexes it; feeds the
  existing %s printf shapes (C99-28/47). Watch embedded-NUL and
  non-ASCII policy already set by C99-28.
  (00025.c, 00026.c, 00058.c, 00112.c, 00137.c, 00138.c, 00173.c)
  Done: PointerRegion carries an optional string-literal base
  (`literalBase`, data on the region, alongside the object bases) plus a
  write-through fact; a literal-based region is a cursor into a read-only
  run backed by an immutable `const`-marked `emitrust.variable` byte
  array (`let lit: [i8; N+1] = [...]`, literal bytes plus the terminating
  NUL so strlen-style walks terminate), created once per literal and
  shared by every pointer of the region. Dereference/subscript read bytes
  via `emitrust.subscript(backing, cursor)`; arithmetic is the usual i64
  cursor arithmetic; any write through the region (`*p = c`, `p[i] = c`,
  `(*p)++`) is a located rejection at the write site (writing a C string
  literal is UB; the region is read-only), as are rebinding across two
  literals and joining a literal with an object. `%s` of a literal-bound
  pointer slices the backing from the cursor through `__emitrust_cstr`
  (C99-28 ASCII policy applies to the backing bytes); a definition-less
  `strlen` lowers by name to the new `__emitrust_strlen` helper over the
  same slice; `"..." == NULL` folds to false (a literal's address is
  never null). Ledger 159 -> 166, zero miscompiles, all seven tests in
  the manifest. (test/Import/C/pointers-string-literal.c; write-through,
  multi-literal, and literal/object-join rejections in
  test/Import/C/pointers-local-invalid.c; rustc-level differential
  test/EndToEnd/string-cursor.c)
- [x] CTS-P2 (7) Pointer types outside the parameter/local-cursor
  positions FR-28 classifies: pointer returns, pointer struct members
  (C99-43), pointers in casts and mixed expressions. Requires extending
  the region analysis beyond (base, cursor) pairs rooted in one
  function's locals. 00089 joined this set after CTS-L3 landed its
  fn_ptr-struct-field initializer: it now rejects on `struct S *anon()`
  at "00089.c:13:1: error: unsupported: pointer type outside a parameter
  position" (a data-pointer return).
  (00019.c, 00049.c, 00089.c, 00095.c, 00140.c, 00150.c, 00208.c,
  00214.c)
  (COMPLETE, 7 of 7 — 00089 landed last via the GLOBAL-RETURN kind
  (CTS-S stretch): a data-pointer-returning function whose every return
  site yields the address of ONE mutable whole global (cursor 0, never
  NULL) classifies as a single-global-base pointer RETURN region. The
  pointer result is ERASED from the imported signature — the function
  imports with no result and its return sites emit a bare `return` —
  the call is retained at each site for its side effects, and every
  caller `f()->member` access routes to the global directly through the
  ordinary staged-copy + writeback machinery: zero runtime pointer
  state (no cell, no flag, no address value) in callers. The erasure
  also flows through INDIRECT calls: a fn-ptr signature returning a
  data pointer is representable exactly when every address-taken
  function of that (canonical, unqualified) return type in the sole-TU
  program classifies to the erased kind with one common base
  (`classifyFnPtrPointerResult`), which covers 00089's
  `go()()->zerofunc()` chain — `go` returns `&anon` (the CTS-P2
  fn-address kind), `anon()` erases to global `s`, and the fn_ptr
  member call finishes through the CTS-L3 field machinery. Pinned OUT,
  located rejections: a NULL return site mixed with a global address
  ("return sites mix a global address and NULL"), disagreeing bases —
  including member-address sites rooted in different globals ("return
  sites disagree on the returned global base"), and callee-local
  returns keep the historical "unsupported: returned pointer value"
  dangling rejection.
  (test/Import/C/pointers-return-global.c,
  pointers-return-global-invalid.c; rustc-level differential
  test/EndToEnd/pointers-return-global.c; 00089.c in the ratchet
  manifest.)
  (Earlier partial state, 6 of 7 — 00019, 00049, 00095, 00140, 00150,
  00208 pass. Three
  sub-features landed, each the principal-kind inference of
  docs/transformation-theory.md sections 4-5 on the existing analysis.
  Pointer STRUCT MEMBERS: a data-pointer member is a stored i64 cursor
  field (a cursor is a borrow-free Copy integer, so a struct can hold
  one); the analysis resolves each member to one statically known target
  object — or one write-only string literal — per struct instance,
  program-wide (every body in Pass A plus the constant-initializer walk
  of globals, including a pointer global's compound-literal backing).
  Supported bindings are degenerate, so the stored i64 stays 0: member
  writes emit nothing, member reads resolve to the bound object's place
  with zero runtime state, and a self-referential chain folds hop by
  hop. Conflicting bindings are a located rejection naming both sites;
  aliased writes, escaping member addresses, and whole-struct overwrites
  poison the field program-wide. Pointer RETURNS: a data-pointer return
  type classifies by its return sites; the landed kind is a returned
  function address behind a void pointer (00095), emitted as the plain
  fn_ptr result — returning a cursor into a callee-local region stays
  rejected at the return site (the dangling case), and the caller-owned
  cursor-return kind (returning p+i over a slice parameter as a plain
  i64 the caller re-associates) is designed but not yet needed by any
  manifest test. CASTS: qualification-preserving explicit casts (same
  unqualified pointee) peel transparently in analysis and emission;
  reinterpreting casts stay rejected. 00140 passed once CTS-F1's
  fixed-prototype variadic definitions landed (its body never touches
  va_list; the pointer member and struct-by-value shapes already
  imported). 00214 passed once the CTS-P3 integer-carrier relaxation
  landed a second pointer-return kind: a function whose every return
  site yields a carrier value (a null constant, a pointer-width
  integer-to-pointer cast, a carrier-region local, or a call to another
  carrier-returning function) returns a plain i64 (see the CTS-P3 note),
  alongside its other blockers (__builtin_expect and StmtExpr, see
  CTS-S8). 00089 landed with the global-return kind above.
  Ledger 188 -> 193, zero miscompiles.
  (test/Import/C/pointers-member.c; test/Import/C/pointers-cast.c;
  member conflict/poison/literal-read/cross-function/dangling-return
  rejections in test/Import/C/pointers-member-invalid.c and
  test/Import/C/pointers-return.c; rustc-level differential
  test/EndToEnd/pointers-member.c)
- [x] CTS-P3 (5) Pointers assigned non-address values (integer↔pointer
  round-trips, arithmetic results stored back into pointers): the design
  decision landed as two relaxations rather than a tagged cursor — the
  CTS-P9 provenance core (void*-wildcard casts plus type-checked
  reinterpret-back sites) and the integer-carrier region model below —
  with every shape outside them a located by-design rejection pinned in
  the invalid tests. The null pointer constant is the None side of
  CTS-P8's Option-of-cursor; a pointer-width (64-bit) integer rides as
  a plain i64 carrier; every other non-address value (sub-pointer-width
  integer casts, carriers mixed with real address bases) stays
  rejected, each pinned in pointers-int-carrier-invalid.c. All five
  listed tests pass and are in the manifest.
  (00039.c, 00103.c, 00144.c, 00163.c, 00187.c)
  (Complete, 5 of 5 — the CTS-P9 provenance core. `void *` is a
  pointee-wildcard cursor: it carries no element unit of its own, so
  casts to and from a `void` pointee peel transparently in analysis and
  emission at any matching pointer depth (`(void *)&x`, `(int *)voidp`,
  the second-order `(int **)voidpp` of 00103), the (base, cursor)
  decomposition is unchanged, and a `void *` never materializes a
  pointer value. A reinterpret-back site `*(T *)p` type-checks T against
  the region's base element type: an exact match lowers exactly like a
  direct pointer (00039's scalar round-trip, 00103's double indirection),
  a same-width int<->int mismatch becomes an `emitrust.cast` bitcast
  view on the load and store (the unsigned view over int storage —
  Rust's same-width cross-sign `as` reinterprets the bit pattern, which
  is C's compatible-effective-type read), and every other
  reinterpretation stays a located rejection: "pointer cast reinterprets
  the pointee ('short' over 'int' storage)" / "('float' over 'int'
  storage)", plus "dereference of a 'void *' pointer" for an uncast
  deref. Null-only
  ternary chains (00144) fold statically — see the CTS-P8 note; the
  `&struct.member` bases of 00163 are the CTS-P7 note. 00039, 00103,
  00144, 00163 pass — ledger 200 -> 204 passed / 16 unsupported /
  0 miscompiled. 00187's actual blocker was FILE* streams, which
  landed as the C99-48 stdio slice (T1.3) — its handles never enter
  the pointer decomposition at all. Wide views over `char`
  storage (`*(unsigned *)charp`, from_ne_bytes territory) landed as
  CTS-P11.
  INTEGER-CARRIER REGIONS (the 00214 `extend_brk` brk-cursor shape)
  landed as a second relaxation: a local pointer whose ONLY sources are
  POINTER-WIDTH integer-to-pointer casts, calls returning carriers, and
  null pointer constants never addresses a modeled object — it is an
  integer riding in pointer clothing — and lowers as one plain i64
  value per pointer (null is the i64 zero; no base, cursor, or flag
  cell). A null test is an `arith.cmpi ne` against 0. The carrier
  crosses function boundaries in both directions: a pointer-returning
  function whose every return site yields a carrier returns a plain
  i64 (`classifyPointerReturn`'s second kind, memoized per canonical
  declaration with call-graph propagation), and a `void *` parameter of
  a defined function whose body only ever truth-tests it classifies as
  `ParamKind::Carrier` and imports as an i64 parameter (call sites pass
  carrier values). Pinned OUT, each a located rejection: "dereference
  of an integer-carrier pointer" (no object to read), "pointer
  arithmetic on an integer-carrier pointer" (no element run to walk),
  the historical "pointer assigned a non-address value" both for a
  carrier mixed with a real address base and for a sub-pointer-width
  integer cast (a truncated address can never round-trip; the MIXED
  and NARROW cases of pointers-int-carrier-invalid.c), and "void
  pointer parameter" for a `void *` parameter the body uses as
  anything but a truth test.
  Global pointers never carry integers (their facts feed the CTS-P4/P6
  machinery unchanged).
  (test/Import/C/pointers-void.c, pointers-void-invalid.c,
  pointers-int-carrier.c, pointers-int-carrier-invalid.c;
  rustc-level differential test/EndToEnd/pointers-void.c with
  loop-carried values through the reinterpreted accesses and
  test/EndToEnd/missing-return-expect-carrier.c; 00214.c in the
  ledger))
- [x] CTS-P4 (4) Pointer-typed global variables: global region bases.
  Landed as the single-global-region-base model below — the feared
  thread_local!+Cell interaction dissolved because a cursor is a
  borrow-free Copy integer, so no borrow ever escapes `.with`. C99-14
  now routes pointer-typed file-scope variables through this model
  (see its entry). Three of the four listed tests passed here first;
  the fourth, 00209, cleared its pointer-global blocker here and later
  flipped to PASS when the K&R callsite-prototype inference wave
  (FR-29, 2026-07-19) landed — see the 00209 disposition above.
  (00040.c, 00045.c, 00149.c, 00209.c — all PASS)
  (Complete in scope, 3 of 4 passing: a pointer-typed global decomposes against a single
  *global* region base; its cursor is a stored i64 `emitrust.global`
  under the pointer's C name — a cursor is a borrow-free Copy integer,
  so storing it globally never fights the thread_local!+Cell model, per
  docs/transformation-theory.md section 4. Supported base shapes: a
  global scalar/struct (`int *p = &x;`, degenerate — no runtime state),
  a global array (cursor + the existing staged-copy element access), a
  file-scope compound literal (synthesized `<name>_backing` global,
  00149), and a single constant-size calloc/malloc site promoted to a
  synthesized zero-initialized backing array whose assignment re-zeroes
  it — exact calloc semantics on every execution (00040, whose recursion
  and write-throughs all pass). Unreferenced pointer globals import
  nothing (covers 00209's six incomplete-pointee declarations). Located
  rejections pinned by test: binding a global pointer to a local object
  — the borrow-escape rustc would refuse, rejected at the binding site —
  plus multi-object regions, copying a global pointer, passing one to a
  function (the callee would see the staged copy), address-of, string
  literals, null constants, multiple allocation sites, and external
  linkage in a multi-TU project. 00040 and 00045 and 00149 pass and are
  in the manifest — ledger 178 -> 181 passed / 39 unsupported /
  0 miscompiled. 00209 left CTS-P4 scope here: after the
  pointer-to-fn-ptr parameter and fn_ptr slice extensions landed
  (pointers-fnptr-slice.c), its sole remaining blocker was the
  argument-carrying K&R `int (*)()` call in f1 (C99-46 scope, not
  CTS-P4), at the time upheld as a by-design rejection and since
  overturned by the FR-29 callsite-prototype inference wave
  (2026-07-19) — 00209 now passes; see its disposition above.
  (test/Import/C/globals-pointer.c, globals-pointer-invalid.c,
  pointers-fnptr-slice.c; rustc-level differential
  test/EndToEnd/pointers-global.c with data-dependent cursor updates
  across calls)
  (CTS-P10 interaction: the "passing a pointer into a global variable to
  a function" rejection is retired ONLY for the all-global cell-slice
  parameter class (see CTS-P6 below) — an argument mediated by a
  pointer-typed global (globals-pointer-invalid.c PASSFN) or by a local
  pointer into a global (pointers-local-invalid.c GLOBAL) keeps the
  staged-copy rejection verbatim, because those flows are outside the
  direct-decay/parameter-forwarding shapes the cell-slice class admits.)
- [x] CTS-P5 (2) Pointer-to-pointer values (`&p`, `**p`): second-order
  cursors over a region whose elements are themselves (base, cursor)
  pairs (C99-43). Implemented as the degenerate one-cell region of
  cursor cells: a `T **pp` bound (possibly repeatedly) to the address
  of exactly one first-order pointer local selects it statically, so
  `pp` carries no runtime state, `*pp` reads/rebinds the selected
  pointer's (base, cursor) decomposition (including its CTS-P8
  non-null flag), and `**pp` dereferences it — no
  reference-to-reference ever arises in the emitted Rust. Still
  rejected with located diagnostics: third-order pointers, pointers to
  function pointers, multi-target selections (would need a runtime
  second-order cursor), second-order copies, null second-order
  bindings, `&p` escaping outside a consumed `pp = &p` binding, and
  pointer-to-pointer parameters. 00005 and 00020 pass and are in the
  manifest — ledger 188 -> 190 passed / 30 unsupported /
  0 miscompiled.
  (test/Import/C/pointers-ptr-to-ptr.c, pointers-local-invalid.c;
  rustc-level differential test/EndToEnd/pointers-ptr-to-ptr.c with
  data-dependent re-pointing through `*pp`)
- [x] CTS-P6 (2) Pointers into global aggregates: same borrow-escape
  problem as CTS-P4; a global array base must be readable/writable
  through an index cursor without holding a borrow across statements.
  (00181.c, 00217.c)
  (Completed by the CTS-P10 cell-slice class and the CTS-P11 byte puns —
  see the resolution note after the CTS-P9 extension below. Ledger
  204 -> 206 passed / 14 unsupported / 0 miscompiled.)
  (Partial: a LOCAL pointer bound into a global aggregate (decay,
  `&garr[i]`, or `&gx`) now decomposes exactly like any Phase-1a local —
  its cursor stays a local i64 cell, so no borrow of the global is ever
  stored — and every element access through it goes through the CTS-P4
  staged-copy machinery: reads stage the global's whole value and
  subscript the copy; write contexts thread `GlobalWriteback` through
  `emitPointerPlace` and store the modified copy back, so a write
  through the pointer is visible to the next direct global access and
  vice versa (exact for the single-threaded subset; the historical
  one-statement last-writer-wins corner is closed by the writeback
  ordering rule below). Static-local
  bases get the same treatment (they share the globals map). Passing
  such a pointer to a function stays rejected at the call site — the
  argument would borrow the staged copy, not the global (the CTS-P4
  staged-copy coherence hazard) — as do escapes. Neither suite test
  passes yet, each on an out-of-scope shape: 00181.c rejects at
  "00181.c:117:4: error: unsupported: passing a pointer into a global
  variable to a function" (`Hanoi(N,A,B,C)` passes the global arrays
  into functions whose parameters also range over B and C — the staged-
  copy argument hazard plus CTS-P7 multi-object parameter regions);
  00217.c rejects at "00217.c:11:6: error: unsupported pointer
  expression: CStyleCastExpr" (`*(unsigned*)(data + r)` type-puns four
  chars of the global as an unsigned — a reinterpreting pointer cast,
  outside any CTS-P item). Ledger unchanged at 188 passed /
  32 unsupported / 0 miscompiled.
  (test/Import/C/pointers-into-global.c; staged-copy coherence-hazard
  rejection pinned in pointers-local-invalid.c GLOBAL case;
  rustc-level differential test/EndToEnd/pointers-into-global.c
  interleaving pointer writes with direct global reads, direct writes
  with pointer reads, and callee global writes between pointer uses)
  (Writeback ordering rule: the staged copy must be FRESH at store time.
  C11 6.5.16p3 sequences the RHS's side effects before the assignment's
  store, and the store writes only the designated subobject — so a
  whole-global snapshot loaded when the LHS place was formed must not be
  stored back after an intervening call wrote another subobject of the
  same global (the lost-update miscompile family). Every staged-global
  write path — simple and compound assignment, the CTS-P11 wide-byte
  stores, and ++/-- (including subscript-index calls, `g[f()]++`) —
  commits its mutation through the single `commitGlobalWriteback` seam
  in ImportC.cpp: when the statement evaluated any side-effecting
  subexpression after the staging load, the staged copy is rebound to a
  fresh `emitrust.global_load` snapshot immediately before the mutation,
  then flushed; pure statements emit the historical IR unchanged.
  Multi-base writebacks already re-stage afresh per dispatch arm inside
  the flush and need no refresh. User-visible evaluation order is
  untouched: every subexpression value is materialized before the
  refresh. Pinned differentially across all the write paths by
  test/EndToEnd/globals-writeback-order.c.)
  (CTS-P9 extension: `&global.member` is now a region base. The
  PointerBaseBinding carries an optional member path, and every access
  through such a pointer reuses this staged-copy machinery with a member
  projection — stage the whole global (`emitrust.global_load`), project
  the member (`emitrust.member`), and store the whole value back after a
  write (`emitrust.global_store`) — so no borrow of the global ever
  survives a statement. A member of a LOCAL struct resolves to the
  member's own `emitrust.member` place with no runtime state at all.
  Boundaries pinned by test: pointer arithmetic on a member base walks
  into sibling storage ("pointer arithmetic on the address of a struct
  member"), union storage has no unaliased member place ("taking the
  address of a union member"), and a member base on a pointer-typed
  GLOBAL stays rejected (the stored-cursor scheme has no member
  projection). 00163's `b = &(bolshevic.b)` exercises the global-member
  arm; test/Import/C/pointers-member-base.c and
  pointers-member-base-invalid.c pin the shapes, and the rustc-level
  differential test/EndToEnd/pointers-member-base.c rebinds the pointer
  at a data-dependent loop iteration.)
  (CTS-P10 resolution — cell-slice parameters, flips 00181: a pointer
  parameter whose interprocedural class is backed ONLY by mutable global
  arrays of one scalar element type now lowers to the shared
  `!emitrust.ref<!emitrust.cell_slice<T>>`, rendered
  `&[std::cell::Cell<T>]`. This is the coherence-sound choice the staged
  copy cannot make: 00181's Move mutates through its parameters and then
  calls PrintAll, which reads the SAME globals directly mid-call —
  `emitrust.cell_get`/`cell_set` and `global_load`/`global_store` hit
  the same thread-local Cell, so every write is observed. Pass A
  (`planCellSlices`) classifies via a union-find over exactly two
  argument shapes (direct global-array decay, parameter forwarding —
  which is what makes Hanoi's PERMUTED recursion classify: shared
  references are freely duplicable, no reborrow discipline). Call sites
  nest one `emitrust.global_cells` region per distinct global argument
  (leftmost outermost), rendered as nested thread-local `.with`
  accessors flattening through `as_slice_of_cells`, with a scalar result
  flowing out through a staging variable; element accesses in the callee
  are cell_get/cell_set on the reference itself (no lvalue staging, no
  cursor cell); forward prototypes classify through the definition
  exactly like Phase 1b. The historical "passing a pointer into a
  global variable to a function" rejection is retired for THIS class
  only; the pinned boundaries are located rejections with class-precise
  wordings: "pointer parameter would join global 'G' and local object
  'larr' into one region" (mixed classes stay out — one type cannot be
  both `&mut [T]` and `&[Cell<T>]`) and "nullable pointer parameter
  backed by a global variable" (Option wrapping and the global_cells
  borrow discipline do not compose in v1).
  Tests: test/Dialect/EmitRust/cell-slice.mlir (round-trip),
  test/Target/Rust/cell-slice.mlir (rendering),
  test/Import/C/pointers-global-args.c and
  pointers-global-args-invalid.c, and the rustc-level differential
  test/EndToEnd/pointers-global-args.c (mini-Hanoi: permuted recursive
  forwarding, direct global reads inside Move while cell borrows are
  live, a data-dependent disc count, and a Move return value flowing
  out of the .with nesting; greps pin as_slice_of_cells,
  &[std::cell::Cell<i32>], .with(, and the absence of unsafe).)
  (CTS-P11 resolution — byte puns over i8 regions, flips 00217: a
  wider-than-element reinterpreting deref `*(T *)p` over a region whose
  base element is a byte (a C char array) widens to a sizeof(T)-byte
  access at the runtime cursor: loads gather the bytes and combine with
  `T::from_ne_bytes`, stores split with `T::to_ne_bytes` and scatter
  back, and compound assignments read-modify-write the same window
  (00217's `*(unsigned*)(data + r) += a - b` with its wrapping u32
  delta). The direct pun cast `(unsigned *)(char *)...` is stripped only
  at dereference sites (`stripObjectPointerCasts`); the general
  decomposition still refuses to bind pointers through it. Global char
  arrays ride the ordinary staged-copy + writeback model, so a `%s`
  print of the global (extended to pointers into global char arrays)
  sees every punned byte. Boundaries pinned by test: a
  compile-time-constant offset whose window overruns the array rejects
  with "4-byte access at offset 5 runs past the end of 'buf' (8
  bytes)", and wide views over non-byte bases keep the existing
  "pointer cast reinterprets the pointee ('long long' over 'int'
  storage)" family (the deref type-check now also covers direct,
  non-void-mediated pun casts).
  Tests: test/Import/C/pointers-reinterpret.c and
  pointers-reinterpret-invalid.c, and the rustc-level differential
  test/EndToEnd/pointers-reinterpret.c (runtime offsets, partially
  overlapping wide stores, per-element/wide-view mixing, and the exact
  00217 shape over a global char array through a char* local; greps pin
  u32::from_ne_bytes, to_ne_bytes, and the absence of unsafe).)
- [x] CTS-P7 (2) One pointer ranging over several objects (`p = &x;
  ... p = &y;`): PointerRegionAnalysis unions the objects into one
  region today and rejects; needs either region materialization (copy
  both objects into one backing array) or an enum-of-bases cursor.
  (00077.c, 00172.c)
  (Done: the enum-of-bases cursor. A multi-base region is accepted when
  every base is local and of one uniform kind — all element runs (arrays
  or slice parameters) of the pointee's element type, or all degenerate
  scalars of the pointee type. Each pointer of the region carries a
  promotable rank-0 memref<i32> base-discriminant cell alongside its i64
  cursor cell (the tagged (base-index, cursor) pair; each variant of the
  closed enum names a disjoint region, so the disjoint-region invariant
  is preserved and the objects stay independently addressable —
  transformation-theory section 4, following the CTS-P8 flag-cell
  precedent). An address binding stores the bound base's index, `p = q`
  copies the source discriminant, cursor arithmetic is unchanged, and
  every dereference dispatches on the discriminant: a cf-level match
  over the closed set of bases whose arms touch exactly one base,
  staging the active element for reads and dispatching the mutated value
  back for writes (the staged-global writeback mechanism, generalized).
  Same-region equality compares (discriminant, cursor) pairs — exactly
  C's defined equality across distinct objects. Still rejected with
  located diagnostics: mixed base kinds and element types (the retained
  multibase.c negative), nullable multi-base regions, ordering and
  difference of multi-base pointers, passing one to a function or string
  helper, and non-scalar-element dereference. 00077 (param slice base +
  local array, sizeof forms) and 00172 (two scalars, equality before and
  after a discriminant copy) both pass; zero unsafe in the emitted Rust.
  test/Import/C/pointers-multi-base.c, the multi-* negatives in
  pointers-local-invalid.c; rustc-level differential
  test/EndToEnd/pointers-multi-base.c where the active base is
  data-dependent at runtime, including a loop-carried discriminant.)
  (CTS-P9 extension: the closed set of bases may now mix degenerate
  scalar locals with `&struct.member` bases, including members of
  GLOBAL structs (the 00163 shape, `b = &a; ... b = &bolshevic.b`).
  Member bases are degenerate one-element runs; a local-member arm of
  the dispatch touches the member's own place, and a global-member arm
  goes through the CTS-P6 staged copy with the member projection —
  stage the whole struct, project the member, store the whole value
  back on the write flush. Dispatch arms are ordered by base index
  (binding order) and the discriminant stays the promotable memref<i32>
  cell storing 0/1. test/Import/C/pointers-member-base.c @mixed_base;
  differential test/EndToEnd/pointers-member-base.c.)
- [x] Array-member self-reference (FR-37/FR-38, `union-find.c`'s
  `struct uf_node *parent` shape): `planArrayMemberPointers`
  (`lib/ImportC/ImportCPlanning.cpp`) proves a struct pointer MEMBER
  whose every read/write site provably roots in the SAME promoted
  owner-struct array as the struct instance it lives in (never an
  externally-named distinct object) and synthesizes an
  `emitrust.enum_def` with one variant per array INDEX — not per
  distinct base object — decoded/written via a genuine `match` on that
  closed, array-sized set. Key architectural decision: this reuses and
  extends the owner-struct-index model (`planOwners`, FR-30/FR-36)
  rather than generalizing CTS-P7's enum-of-BASES multi-base cursor to
  cover struct members. CTS-P7's mechanism is the right fit for a small,
  statically enumerable set of genuinely DISTINCT named objects a single
  local pointer variable ranges over (`p = &x; ... p = &y;`) — each
  variant names one independent base. A self-referential array-member
  pointer is a different shape: it ranges over elements of ONE already-
  promoted array (potentially large, and sized by the array, not by a
  fixed count of distinct locals), and it must interoperate with that
  same array's pointer PARAMETERS, LOCALS bound from calls (FR-36's
  owner-index return), and cross-parameter equality (FR-38's B5) — all
  of which already speak the owner-struct i64-cursor representation.
  Building the member pointer on owner-struct-index instead of a second,
  CTS-P7-shaped discriminated union means every one of those
  interactions (path compression's `parent = x->self;` field read
  binding a local, `while (x->self != x)` comparing a decoded field
  against a cursor, `root1 == root2` comparing two independently-traced
  parameters) is the SAME representation on both sides with no bridging
  conversion between two different pointer models. CTS-P7 stays scoped
  to its original multi-object-local use case and is untouched by this
  work (`pointers-multi-base.c` and its differential counterpart pass
  unchanged). See FR-37/FR-38 above for the mechanism's staged
  construction (enum synthesis, path compression, cross-parameter
  equality) and `test/EndToEnd/union-find.c` for the full capstone
  program this unlocks.
- [x] CTS-P8 (1) NULL data-pointer constants: an Option-of-cursor model
  mirroring the fn_ptr None mapping; interacts with CTS-P3.
  (00171.c)
  (Done: a region that sees a null pointer constant is nullable instead
  of invalidated; each of its pointers carries the Option discriminant in
  a promotable memref<i1> "non-null" flag cell — NULL assignment stores
  false, an address binding stores true, `p = q` copies the source flag,
  and null-checks (`if (p)`, `p == 0`, `p != NULL`) read it, folding to
  constants for statically non-null pointers. A dereference of a
  possibly-null pointer is guarded by assert!(flag, "null pointer
  dereference") — C null-deref is UB, so the deterministic panic is a
  legal refinement per the fn_ptr expect precedent. CTS-P3 interaction:
  only the null-constant idiom itself is modeled; general
  integer-to-pointer traffic stays rejected, as do passing, ordering,
  differencing, and same-region comparison of possibly-null pointers,
  nullable string-literal regions, and dereference of a pointer that is
  only ever null; nullable regions are excluded from Phase-4 owner
  promotion (bare i64 cursor arguments cannot carry the discriminant).
  Import shapes in test/Import/C/pointers-null.c, rejections in
  test/Import/C/pointers-null-invalid.c, rustc-level differential with a
  data-dependent null path in test/EndToEnd/pointers-null.c, ledger
  00171.c.)
  (CTS-P9 extension: a pointer-typed ConditionalOperator is now a
  pointer source — no new representation. Classifying `q = c ? A : B`
  classifies both arms into one united region (a null-constant arm,
  including the qualified `(const void *)0` spelling and the
  integer-conditional `q = i ? 0 : 0` shape, marks it nullable), and
  the emission assigns each arm in its own block so the null/address
  state merges through the pointer's own flag/discriminant/cursor cells
  (test/Import/C/pointers-null-ternary.c @ternary_real_base and the
  swapped-arm variant; differential test/EndToEnd/pointers-null-ternary.c
  with a loop-flipping condition). A base-less nullable region with a
  conditional source is STATICALLY NULL and carries zero runtime state:
  no flag cell is materialized, `if (q)` folds to a constant-false
  branch, `q == 0` folds true, `(int) q` folds to the integer 0 (the
  00144 ending), and dereference keeps the "only ever null" rejection
  (pointers-null-ternary-invalid.c; a pointer-to-int cast of a pointer
  with a real base keeps "unsupported cast (PointerToIntegral)"). A
  base-less region built only from DIRECT null bindings keeps the
  historical flag cell above — the pointers-null.c @null_only contract.
  00144 passes on this folding.)

### Records and symbol namespaces (16 tests)

- [x] CTS-R1 (5) Bare anonymous struct types (no tag, no typedef name):
  synthesize a stable name (e.g. `Anon<n>` keyed by shape) and reuse the
  C99-6 dedup machinery; today only typedef'd anonymous structs import.
  (00017.c, 00043.c, 00047.c, 00118.c, 00120.c)
  (Done: `importRecord` assigns `Anon<n>` names keyed by the C99-6
  field-shape serialization — the counter only orders first encounters,
  so the same anonymous shape in any TU maps to one Rust type and
  distinct shapes never collide; the key map is consulted only for
  anonymous records, so an anonymous struct matching a named struct's
  shape keeps its own type (C type identity is by declaration). A value
  of anonymous enum type maps to plain `i32`, unlocking 00120's
  anonymous-enum member. All five tests pass and are in the manifest —
  ledger 164 passed / 56 unsupported / 0 miscompiled. Pinned by
  test/Import/C/structs-anon-bare.c (distinct shapes get distinct names,
  repeated shape shares one struct_def, named-vs-anonymous shape match
  stays two types, cross-TU dedup, anonymous-enum member) and
  test/EndToEnd/structs-anon.c (differential member reads/writes).)
- [x] CTS-R2 (2) Unnamed struct members (anonymous member injection —
  C11 6.7.2.1p13 anonymous struct/union members whose fields join the
  parent's namespace): flatten fields into the parent struct_def with
  mangled names, or reject-by-design with a note.
  (00046.c, 00050.c)
  (Done: an anonymous struct member's fields are injected into the
  parent struct_def under their own spellings — no mangling is needed
  because C11 puts them in the parent's member namespace, so Sema has
  already enforced uniqueness (a collision is a located clang error).
  Both target tests also contain anonymous UNION members; the exactly
  representable subset is implemented: an anonymous union member whose
  arms each flatten to one leaf of one identical type becomes a single
  storage slot named after the first leaf, every arm's spelling
  aliasing it — exact because reading any union member with the type of
  the last store yields that stored value (C99 6.5.2.3). Every other
  ANONYMOUS union member — mixed-type arms, an arm wider than one
  slot — keeps the located union-type rejection; named and bare union
  TYPES were later admitted by the C99-44/CTS-R3 one-slot model, whose
  wider pun matrix does NOT extend to anonymous members (identical
  leaves only here).
  Member access skips Sema's implicit intermediate anonymous access and
  selects the flattened (alias-resolved) leaf on the parent place;
  block-scope initializer lists recurse onto the parent place with a
  union's nested list landing on its active arm's slot; constant global
  initializers convert along the C field structure so brace-elided
  values, zero-filled tails, and union slots produce the flattened
  attribute list. Distinct from CTS-R1's bare anonymous struct
  declarations, whose shape-keyed Anon naming is untouched.
  Traceability: importer lib/ImportC/ImportC.cpp (collectRecordFields,
  anonymousUnionArmLeaf, flattenedFieldName, unionSlotStorage,
  structDefRecords, emitRecordInitFields, emitRecordInitField,
  convertRecordAPValue, convertAnonymousSlotInit, and the anonymous
  skip in the member-access lvalue path); tests
  test/Import/C/structs-anon-member.c (two-level flattening, union slot
  aliasing, global/local initializer shapes),
  test/Import/C/structs-anon-member-invalid.c (mixed-type arms, wide
  arm, named union, parent-vs-member spelling collision — all located),
  test/EndToEnd/structs-anon-member.c (differential). Both tests pass
  and are in the manifest — ledger 190 passed / 30 unsupported /
  0 miscompiled.)
- [x] CTS-R3 (3) Unions (C99-44): CLOSED — all three target tests PASS
  in the manifest, and the design decision is the C99-44 one-slot
  struct model (a supported subset with documented located rejections;
  neither of the original candidates — data-carrying enum, byte-array
  storage with accessor helpers — was taken).
  (00042.c, 00210.c, 00218.c — all PASS)
  Wave5 B1: the one-slot struct model (full matrix under C99-44)
  admits unions whose arms alias one leaf — identical mapped types,
  same-width integers differing only in signedness (bit-exact
  `emitrust.cast` reinterpretation at the accesses), and, since the
  float-pun extension, a float arm against a same-width integer
  (bit-exact `emitrust.bitcast`, Rust to_bits/from_bits) — flipping
  00042.c (untagged local two-int-arm union) to PASS.
  T1.1: the byte-array arm (00210's `uint16_t u; uint8_t b[2];`,
  packed attributes in either typedef position tolerated and discarded)
  ADMITS at the TYPE level: the slot is the INTEGER arm regardless of
  declaration order, the array spelling never reaches the IR, and any
  access through the array arm is a located
  `unsupported: union byte-array arm access` at the ACCESS site;
  unequal-total-width array arms keep the union family rejection at the
  union decl. Together with the local void* fn-ptr holder (a
  never-reassigned local `void *` initialized from one known
  non-variadic function whose every value use is an explicit cast to
  exactly the target's signature in callee position imports as an
  ordinary `!emitrust.fn_ptr` local — fn-address `Some(target)`
  constant + `emitrust.call_indirect`, the cast fully peeled;
  out-of-shape holders keep `unsupported: pointer assigned a
  non-address value`), 00210.c flipped to PASS.
  00218.c (a SINGLE-ARM union of a struct with pointer members and an
  enum bit-field — not a multi-arm pun) passes through the one-slot
  single-arm admission combined with the C99-45 enum-bit-field
  zero-extend accessors and the CTS-P2 pointer-struct-member work; the
  earlier note here calling it out of scope was stale.
  Union shapes outside the model stay behind located
  `unsupported: union ...` rejections (test/Import/C/unions-invalid.c,
  union-bytearray-arm-invalid.c); positive pins in
  test/Import/C/unions.c, union-bytearray-arm.c, fnptr-void-local.c
  (+ -invalid), test/EndToEnd/unions.c, fnptr-void-local.c.
  Ledger at this entry's closure (T1.3 era): 217 passed /
  3 unsupported / 0 miscompiled of 220; superseded — see the ledger
  header above (218 as of 2026-07-19).
- [x] CTS-R4 (2) Block-scope struct declarations shadowing an outer tag
  (same tag `T`, different shape, inner scope): the importer's per-name
  shape dedup misreads this as a cross-TU conflict; record keys need
  scope depth, and the inner type needs a distinct Rust name.
  Landed: record identity is the defining `RecordDecl` (clang has already
  resolved tag scoping), so the name-keyed cross-TU shape dedup now
  applies to file-scope records only; each block-scope definition — even
  a same-shaped one, per C99 6.2.1 — emits its own struct_def under the
  function-local-static mangling convention `<function>_<tag>`
  (`_<n>`-suffixed when taken). Traceability: importer
  `lib/ImportC/ImportC.cpp` (`importRecord`, `emittedRecordName`,
  `localRecordNames`); tests test/Import/C/structs-shadow.c (shadowing,
  same-shape, double-shadow, tag-only), test/Import/C/
  multi-tu-struct-conflict.c (genuine file-scope cross-TU conflict keeps
  its diagnostic), test/EndToEnd/structs-shadow.c (differential);
  manifest ratcheted 159 -> 161.
  (00044.c, 00053.c)
- [x] CTS-R5 (3) C's separate tag/ordinary namespaces (`struct a` and a
  global `a` coexisting, or a static local colliding with the mangled
  `<fn>_<name>` scheme): Rust has one namespace per kind but the emitter
  uses one symbol table; resolved by detect-and-rename on collision.
  Detect-and-rename landed in the importer: a per-TU pre-pass
  (`collectOrdinaryNames`) records every name the ordinary namespace will
  claim (functions after `main`/TU-tag mangling, file-scope variables,
  function-local statics under their `<fn>_<name>` mangle), and
  `structSymbolName` keeps the readable tag when free, renaming
  deterministically to `Struct_<tag>` only on actual collision — order
  independent, cached per definition. If the renamed spelling is also
  claimed, the import is a located rejection
  (test/Import/C/structs-tag-namespace.c, -invalid.c; differential
  test/EndToEnd/struct-tag-namespace.c). 00129.c and 00219.c pass and are
  in the ratchet manifest; 00204.c clears its namespace blocker — the
  rename is complete for it too — and, since the 2026-07-19 CTS 00204
  wave (long-double-as-f64 + va_list monomorphization; see the
  disposition list above), passes end-to-end as well.
  (00129.c, 00204.c, 00219.c)
- [x] CTS-R6 (1) Empty structs (`struct T {};` — a GNU/C2x shape clang
  accepts): emit a unit-like Rust struct.
  Empty-struct support landed: the importer accepts a field-less
  record, struct_def permits empty field arrays, and the emitter prints
  `struct T {}` (declaration/copy/default via the usual derives;
  test/Import/C/structs-empty.c, Dialect ops.mlir, Target memory.mlir).
  The eager file-scope emission defers for an empty struct no
  declaration type mentions (CTS-BR: one that only ever appears as a
  zero-byte member of a byte-region aggregate never emits a
  struct_def). 00216.c itself landed via the CTS-BR byte-region wave —
  the FAM declaration is tolerated per the amended C99-17 and the
  whole test is on the manifest.
  (00216.c)

### Statements and expressions (10 tests)

- [x] CTS-S1 (2) Compound assignment with operand promotion
  (`char/short x; x += wider;`): lower as load, widen-cast, operate,
  narrow-cast, store. Must respect the zero-vs-sign-extension trap
  documented for the pipeline (adversarial negative/width-extreme
  differential tests required).
  Lowering implemented: `buildCompoundAssignValue` widens the loaded LHS
  to Sema's `getComputationLHSType`, operates, and narrows the result
  back to the LHS storage type, with both casts routed through the C99-3
  conversion machinery (`emitrust.cast` when either side is unsigned,
  arith ext/trunc between signless types, extf/truncf between float
  widths, sitofp/fptosi for the int-accumulator-with-float-RHS shape;
  `_Bool` endpoints stay rejected with a located diagnostic). Covers
  locals, the direct-global fast path, and value-position uses.
  Adversarial differential tests in test/EndToEnd/compound-promote.c:
  signed/unsigned char and short `/=`, `%=`, `>>=` on top-bit-set values
  (each prints differently if the widen picks the wrong extension),
  `+=`/`-=`/`*=`/`<<=` overflowing the narrow type in both directions
  (wrap-on-narrow), short -= long (the 00111.c shape), int accumulator
  with long long RHS and long long shift amount, float += double (the
  00174.c shape), and int *=/= double truncation toward zero. Both tests
  pass and are in the ratchet manifest: 00174.c's former second blocker
  ("00174.c:45:19: error: unsupported: call to 'sin' declared in a
  system header") was cleared by the hosted `sin` -> `f64::sin` mapping
  (C99-48).
  (00111.c, 00174.c)
- [x] CTS-S2 (2) Switch bodies that are not plain compound statements
  and case labels nested inside inner statements (Duff-adjacent,
  C99-32 note): requires emitting switch dispatch as cf-level branches
  into arbitrary statement positions rather than the structured match
  lowering; goto's labelBlocks machinery (C99-33) is the likely vehicle.
  Landed: a dispatch fallback lowering (`emitDispatchSwitch`) used when
  the body is not the plain shape (non-compound body, statement before
  the first label, or a case/default label nested inside an inner
  statement): every label of the switch (clang's
  `SwitchStmt::getSwitchCaseList`, which covers buried labels but not
  those of nested switches) becomes an ordinary block registered up
  front — the goto labelBlocks pattern, keyed by the label statement —
  the dispatch is one `cf.switch` to those targets, and the body is
  emitted in source order with each label redirecting emission into its
  block, so fall-through into and out of loop bodies (Duff's device) is
  plain block fall-into; lift-cf-to-scf absorbs the possibly
  irreducible result exactly like goto into a loop. Variable places
  under the dispatch are hoisted to the entry block (the dispatch may
  jump over declarations, like goto). The structured lowering stays the
  default for plain bodies, and GNU case ranges stay rejected with a
  located diagnostic in both paths. Differential coverage sweeps Duff's
  device over every entry residue and data-dependent trip counts, case
  labels in both arms of an if, INT_MIN/INT_MAX and negative case
  values with no-match values on both sides (FR-25), and a for-loop
  break under a case entered mid-loop. Both tests pass and are in the
  ratchet manifest.
  (test/Import/C/switch-dispatch.c, switch-invalid.c,
  test/EndToEnd/switch-dispatch.c)
  (00051.c, 00143.c)
- [x] CTS-S4 (2) Multi-dimensional arrays (C99-41): nested
  `emitrust.array` types, nested ArrayAttr initializers (the C99-11
  file-scope machinery already recurses), and row-major subscript
  lowering; mapType currently rejects the type before anything else
  runs.
  Landed: `!emitrust.array` elements may nest (emitted `[[T; N]; M]`),
  mapType recurses, the APValue global-initializer converter and the
  local init-list walker already recursed once the type mapper let them,
  and direct `a[i][j]` chains one `emitrust.subscript` per level. The
  pointer decomposition gained a flat row-major cursor into
  multi-dimensional bases: `&arr[i][j]` folds to `i*N + j`, a row
  pointer's subscript scales by the row span, and place materialization
  peels one array level per subscript by div/rem on the cursor. Walking
  arithmetic on row pointers (`++`, `+ n`, `+=`, difference) stays a
  located rejection (CTS-P scope), as do slices of rows. Both tests pass
  and are in the ratchet manifest.
  (test/Dialect/EmitRust/types.mlir, invalid.mlir,
  test/Import/C/arrays-multidim.c, pointers-local-invalid.c,
  test/EndToEnd/arrays-multidim.c)
  (00130.c, 00151.c)
- [x] CTS-S5 (1) Variable-length arrays: conflicts with the
  deterministic/bounded design philosophy; recommend documenting as a
  permanent by-design rejection rather than implementing.
  Landed (T1.1) as DEAD-VLA ELISION, not VLA support: an UNREFERENCED
  local VLA whose size expression is side-effect-free is elided at
  import — no IR, no diagnostic; the object never materializes (the
  00207 f1 shape). Referenced VLAs, and dead VLAs whose size expression
  has side effects (eliding would silently drop the call), keep the
  verbatim `unsupported: non-constant array size` rejection. The same
  wave folds a compile-time-constant short-circuit LHS before lowering
  (`0 && printf(...)` / `1 || printf(...)` value shapes, mirroring the
  constant-condition ternary elision — the 00207 f3 shape), flipping
  00207.c to PASS in the manifest. General (referenced) VLAs remain a
  permanent by-design rejection.
  (test/Import/C/vla-dead-elision.c, vla-dead-elision-invalid.c)
  (00207.c)
- [x] CTS-S6 (1) Integer-to-enum conversion (the reverse of C99-5):
  needed a design decision — `#[repr(i32)]` enums admit no safe `from`
  without a match table; candidates were a `fn <Enum>_from_i32`
  exhaustive-match helper, or rejection.
  Landed with the **preserved-value policy**, not the panic-refinement
  helper: 00170.c stores 12 into `enum fred` (matching no declared
  enumerator, values {0..3, 54, 73..75}) and prints it, which C defines as
  value-preserving (C99 6.7.2.2: the object holds any value of the
  underlying type), so an exhaustive match over declared discriminants
  cannot represent the result and a panic arm would abort a defined C
  program. The representation was therefore changed to a value-preserving
  open enum: `emitrust.enum_def` (now carrying an `unsigned_underlying`
  marker mirroring clang's underlying-type choice) emits a
  `#[repr(transparent)]` tuple struct over the storage integer (`i32` or
  `u32`) with one associated constant per enumerator and a Default impl
  returning the first variant; nominal typing, enumerator paths
  (`Fred::C`), `==`/`!=`, and `Name::default()` are unchanged.
  Int-to-enum lowers to `emitrust.cast` to the enum type (total,
  rendered `Fred(v as u32)`), enum-to-int renders `.0 as`, and the new
  `emitrust.enum_raw` place op supports 00170.c's other blocker, C's
  enum/underlying-type pointer compatibility (`deref(&e)` with a
  `unsigned int *` parameter borrows `&mut e.0`). Float-to-enum stays a
  located rejection.
  (test/Import/C/enum-from-int.c, enums-invalid.c,
  test/Target/Rust/match.mlir, test/Dialect/EmitRust/ops.mlir,
  invalid.mlir, test/EndToEnd/enum-from-int.c)
  (00170.c)
- [x] CTS-S7 (2) `(void)` casts and void-typed contexts (evaluate and
  discard, `void` in a statement-expression position): map to an
  expression statement / `let _ =` discard; today "unsupported cast
  (ToVoid)" / "unsupported builtin type 'void'".
  Done for the manifest tests: ToVoid casts evaluate the operand as an
  expression statement (a side-effect-free operand emits nothing) and
  void-typed conditionals in statement position lower as if/else
  diamonds, which unlocked 00212.c; 00213.c passed once the CTS-S8
  StmtExpr pack landed (its label-containing constant-conditional arms
  keep full lowering — see the 00213 note under CTS-S8).
  (00212.c, 00213.c)
- [x] CTS-S8 (2) The StmtExpr pack (the 00213/00214 shapes): GNU
  statement expressions, `__builtin_expect`, constant-condition dead-arm
  elision, and fall-off-the-end return synthesis.
  STATEMENT EXPRESSIONS `({ ... })` lower as FLATTENED statements in the
  enclosing function — never as a walled-off region op (an
  `scf.execute_region` would hide internal labels from the
  labelBlocks/goto dispatch) — with the final expression statement's
  value transiting a synthesized temp cell (memref for signless scalars,
  `emitrust.variable` for unsigned) that the surrounding expression
  reads; nested StmtExprs flatten recursively, labels inside register
  with the ordinary goto machinery (the within-StmtExpr backward-goto
  loop lifts to `scf.while`), and a statement-position StmtExpr
  discards its value (a side-effect-free final expression emits
  nothing). Pinned OUT: "goto out of a statement expression in value
  position" (the value temp would never be written); a StmtExpr whose
  last statement is not an expression is ill-formed C in value position
  and clang itself rejects it.
  __BUILTIN_EXPECT (and the _with_probability form) is a pure
  branch-prediction hint: it folds to its first argument at the emitCall
  seam in every position, so no call op or `__builtin_expect` symbol
  survives into the IR, and a constant argument composes with dead-arm
  elision through clang's constant evaluator.
  CONSTANT-CONDITION DEAD-ARM ELISION: an `if`, value ternary, or void
  ternary whose condition constant-folds (side-effect-free) elides the
  dead arm BEFORE lowering — before any unimported-call or conversion
  check, so a dead arm may contain otherwise-unimportable constructs
  (00214's `if (__builtin_expect(!!(0), 0))` arms and `_Bool chk`).
  Gated on a live-label check applied uniformly to the if, value
  ternary, and void ternary forms: a dead arm holding a goto-targeted
  label keeps FULL lowering — the constant branch leaves the arm
  dynamically dead while its labels register with the ordinary goto
  dispatch, so code entered through the label runs exactly as C
  requires (the 00213 `if (0) { lab: ... }` and kb_wait_1 shapes). This
  is sound for ternary arms too: a label there can only live inside a
  statement expression, clang rejects any jump INTO a statement
  expression from outside, and the flattened StmtExpr lowering (CTS-S8
  above) registers internal labels like any others, so full lowering
  needs no jump-around suppression. An arm holding a case/default label
  of an enclosing switch is likewise never elided (full lowering
  through the existing dispatch-switch machinery — silently dropping it
  would miscompile).
  MISSING-RETURN SYNTHESIS: a non-void function whose control falls off
  the end (C11 6.9.1p12 — defined while the caller never uses the
  value) synthesizes `return 0` of the function's return type at
  finalization, for every integer width including `_Bool`/i1 and for
  floats; aggregate/enum/fn_ptr returns keep the located rejection.
  00213 CAPTURED: its kb_wait_1 constant void-ternary holds a
  goto-targeted label inside the DEAD StmtExpr arm, targeted from
  within that same arm — exactly the full-lowering-instead-of-elision
  case above. The label-containing arm lowers fully behind the constant
  branch, the internal backward goto resolves through labelBlocks, and
  no code suppression is needed, so the composed lowering is exact and
  00213 joins the manifest alongside 00214.
  (test/Import/C/stmt-expr.c, stmt-expr-invalid.c, builtin-expect.c,
  missing-return.c; rustc-level differential
  test/EndToEnd/stmt-expr.c and
  test/EndToEnd/missing-return-expect-carrier.c; 00213.c and 00214.c
  in the ledger)

### Functions and linkage (2 tests)

- [x] CTS-F1 (2) Variadic calls and variadic function-pointer types
  beyond the printf/puts intrinsics (C99-37): design decision needed
  (safe Rust has no C-style varargs; candidates are arity-specialized
  monomorphization at call sites, or rejection).
  (00186.c, 00189.c)
  (COMPLETE, 2 of 2 — 00189 landed last via STATIC DEVIRTUALIZATION
  (CTS-S stretch): a file-scope function pointer initialized to a known
  function and NEVER REASSIGNED (nor address-taken) anywhere in the TU
  — the criterion is never-reassigned, not const-qualified; an
  externally visible variable only qualifies in a sole-TU import — is
  an import-time ALIAS of its target. No `emitrust.global` is
  materialized for it, calls through the alias (both `p(...)` and
  `(*p)(...)`) lower as DIRECT calls to the target after the same
  signature check a `Some(target)` constant runs (no fn_ptr value, no
  call_indirect), and a value use reads as the `Some(target)` constant.
  A variadic target aliases only when it is the hosted definition-less
  printf/fprintf: calls route through the printf machinery, and the
  fprintf shape swallows its leading `stdout` argument with the
  fprintf->printf routing — the swallowed first-arg slot is the ONLY
  place a FILE* value is accepted (00189's
  `fprintfptr(stdout, "%d\n", (*f)(24))` composition). Pinned OUT,
  located rejections: a reassigned global fn-ptr and a never-reassigned
  pointer to a NON-hosted external variadic keep the ordinary import
  path's "unsupported: variadic function pointer type" at the decl;
  `stdout` outside the swallowed slot keeps "unsupported: pointer
  variable 'stdout' has no known target object" at the use; storing
  `stdout` keeps "unsupported: copying a global pointer variable".
  (test/Import/C/fnptr-devirt.c, fnptr-devirt-invalid.c; rustc-level
  differential test/EndToEnd/fnptr-devirt.c; 00189.c in the ratchet
  manifest.)
  (Earlier partial state, 1 of 2 — 00186 passes. Two sub-features landed. VARIADIC
  DEFINITIONS whose bodies never touch va_list (no va_start/va_arg/
  va_copy calls, no va_list declarations) import as their FIXED
  prototype — the named parameters only, the trailing `...` dropped
  from the type; call sites drop trailing extras when every dropped
  extra is side-effect-free (the dropped extras are never imported:
  no loads of by-value struct extras, no borrows for dropped `&s`,
  no pointer regions). Pinned OUT: a definition whose body uses
  va_list keeps "unsupported: variadic function definition", and a
  dropped extra with side effects rejects with "unsupported: extra
  argument to a variadic call has side effects" at the call site;
  variadic function-pointer TYPES stay rejected — 00189's fprintf
  pointer later became representable without the type, via the
  devirtualization alias above. This also unblocked 00140 (see
  CTS-P2). SPRINTF with a
  literal format (00186) lowers through the printf-shared directive
  translator into a `format!` String plus the one-per-module safe
  `__emitrust_sprintf(dest: &mut [i8], s: &str) -> i32` helper
  (bytes + NUL copied via bounds-checked indexing — a too-small
  destination panics, a legal refinement of C's UB — returning the
  length), with the destination borrowed mutably from its cursor like
  the <string.h> helpers. Pinned OUT: non-literal formats
  ("unsupported: sprintf format must be an ordinary string literal"),
  precision (shared translator's "unsupported: precision in printf
  format specifier"), and string-literal destinations ("unsupported: a
  string literal region cannot be a mutable string argument").
  Ledger 201 -> 203 (+00140 +00186), zero miscompiles.
  (test/Import/C/varargs-def.c, varargs-def-invalid.c, sprintf.c,
  sprintf-invalid.c; rustc-level differentials
  test/EndToEnd/varargs-def.c, test/EndToEnd/sprintf.c))

### Hosted library surface (5 tests)

- [x] CTS-L1 (2) string.h subset — at least `strcpy` into a char array
  (C99-48): safe helper over `&mut [i8]` mirroring `__emitrust_cstr`;
  bounds are compile-time known array sizes, so no unsafe needed.
  DONE: definition-less strcpy/strncpy/strcat/memset/memcpy (statement
  position), strcmp/strncmp/memcmp (value position), strlen widened to
  char arrays, and strchr/strrchr (feeding printf %s and null
  comparisons via a found-index-or-minus-1 lowering) all lower by name
  to one-per-module safe helpers over `&[i8]`/`&mut [i8]` slices of the
  argument regions; same-object memcpy takes one mutable borrow plus two
  cursors (`copy_within`), and same-object copy sources, literal-region
  destinations, copy-result value uses, and uncurated <string.h>
  functions (strstr, ...) keep located rejections. printf %s also gained
  the `&arr[i]` element-pointer shape (00180.c). Both tests pass and are
  in the ratchet manifest.
  (test/Import/C/strings-hosted.c, strings-hosted-invalid.c,
  test/EndToEnd/strings-hosted.c)
  (00179.c, 00180.c)
- [x] CTS-L2 (1) printf %s of a `char *` function parameter: extend the
  C99-28 %s shapes to accept the FR-28 `mut_ref<slice<i8>>` parameter
  class (slice + `__emitrust_cstr`). Landed with two enabling pieces the
  test also needed: a string-literal argument to a slice parameter
  (fresh mutable per-call backing; copies are unobservable because
  writing a literal is UB), and C main's (int argc, char **argv) form —
  argc imports as i32 (the crate wrapper passes the process argument
  count via args_os), argv is dropped with a located rejection on any
  use. (00200.c; test/Import/C/printf-slice-param.c, main-args.c,
  test/EndToEnd/percent-s-param.c)
- [x] CTS-L3 (2) String-literal and other initializers for
  pointer-typed objects (`char *s = "…"` at file scope, struct fields):
  COMPLETE — the initializer shapes landed, 00220 passes, and 00089
  passes since CTS-P2's global-return kind landed (its own blocker; the
  initializer shape it needed is item (3) below). Landed: (1) a
  file-scope `char *s = "…"` imports in
  importPointerGlobal as the CTS-P1 read-only backing lifted to module
  scope — a const `<name>_backing` byte-array global (bytes plus NUL,
  ASCII-only per C99-28) plus the CTS-P4 stored i64 cursor global,
  offset-initialized; write-through, null, wide/u8 literals, non-ASCII
  bytes, literal/object joins, and body literal bindings keep located
  rejections (write-through detection now tracks a global pointer whose
  only body mention is the write); (2) wide-literal array initializers
  (`wchar_t s[] = L"…"`, block and file scope) fill i32 arrays with the
  literal's code units, no ASCII limit (a wide array never feeds the
  byte-string `%s`/`%c` helpers), which is all 00220 needs — it passes
  end-to-end (ledger 188 -> 189); (3) fn_ptr struct fields in file-scope
  initializers (the 00089 line-10 shape) fold to the opaque
  `Some(name)`/`None` forms via the constant evaluator, verifier and
  emitter accept them as aggregate leaves. 00089's last blocker —
  `struct S *anon()` returns a data pointer — landed as CTS-P2's
  global-return kind, and 00089 now passes end-to-end.
  (test/Import/C/globals-pointer-string.c, strings-wide.c,
  fn-pointers.c; rejections in globals-pointer-invalid.c,
  strings-invalid.c; rustc-level differential
  test/EndToEnd/globals-string.c)
  (00089.c and 00220.c pass)
Not itemized above: printf precision (`%.3s`) and the ll/h/hh length
specifiers are now inside the C99-47 grammar; the L length modifier on
the floating conversions joined it with the CTS 00204
long-double-as-f64 policy (C99-8 revision — bare %Lf keeps the
__emitrust_fmt_f64 fast path, adjusted forms route through
__emitrust_fmt_float), while `%Ld` (L on an integer conversion) stays
rejected. No test is sole-blocked on printf forms today (00182.c and,
since the 00204 wave, 00204.c pass).

