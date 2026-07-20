//===- ImportC.cpp - C-to-EmitRust importer -------------------------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the C importer: a syntax-directed translation from
/// the clang AST of a C11 translation unit to a hybrid MLIR module.
///
/// The translation strategy is:
///  - Scalars and control flow use core dialects. Every scalar local and
///    scalar parameter becomes a rank-0 `memref.alloca` cell in the entry
///    block; reads are `memref.load`, writes are `memref.store`. `if`,
///    `while`, and `for` become explicit blocks connected with `cf.br` and
///    `cf.cond_br`, and `switch` becomes a `cf.switch` over one block per
///    label position, so that `--mem2reg --lift-cf-to-scf` downstream
///    recovers clean structured IR.
///  - Plain `char` is signed (the x86-64 Linux / clang default the
///    differential oracle uses) and maps to signless i8 exactly like
///    `signed char` (C99-4). Ordinary and wide character constants are
///    int-typed rvalues importing as the value clang evaluated —
///    escapes included, with high bytes sign-extended ('\xff' is -1) —
///    while Unicode (u8''/u''/U'') and multi-character constants are
///    rejected with located diagnostics.
///  - Aggregates and pointers use EmitRust place operations, which are
///    opaque to upstream passes: struct and array locals are
///    `emitrust.variable`, field access is `emitrust.member`, indexing is
///    `emitrust.subscript`, pointer parameters are `!emitrust.mut_ref<T>`
///    arguments dereferenced with `emitrust.deref`, and reads/writes of any
///    such place use `emitrust.load`/`emitrust.assign`. A scalar local whose
///    address is taken is kept as an `emitrust.variable` so the reference
///    stays valid in the generated Rust. A block-scope compound literal in
///    expression position (C99-13) materializes as a fresh anonymous
///    `emitrust.variable` at its evaluation point — default-initialized
///    (C99 zero fill), then per-element assigns of its initializer list,
///    the same lowering as a `= {...}` declaration — and behaves as an
///    ordinary lvalue of that temp: `(struct S){...}` loads whole for
///    value uses (assignment, by-value argument, return, and
///    `struct S s = (struct S){...}`, which initializes `s` directly),
///    member/subscript accesses resolve on the temp's place, and a
///    decayed or address-taken literal binds a synthesized backing
///    declaration (`CompoundLiteralTemps`) as a pointer-region base like
///    any named local object. Regions based on a compound literal stay on
///    the Phase-1b lowering (never owner-promoted), scalar compound
///    literals are rejected, and a global pointer bound to one keeps the
///    borrow-would-outlive-the-object rejection.
///  - Unions import on the one-slot struct model (C99-44/CTS-R3): a
///    union type is a ONE-FIELD struct_def whose storage field is the
///    slot arm's leaf, every arm's spelling aliasing it. Identical-type
///    arms alias exactly; same-width scalar puns (int signedness via
///    `emitrust.cast`, float/int via `emitrust.bitcast`, i.e. Rust
///    to_bits/from_bits) reinterpret bit-exactly at every access site,
///    including compound assignment, ++/--, value-position assignment,
///    and designated initializers; constant initializers through a
///    float-pun arm cross the domain at compile time. Everything else —
///    bit-field/pointer/unnamed arms, differing-size scalars, aggregate
///    arms, empty unions, byte-array arm ACCESS, address-of any union
///    member — is a located rejection (see `collectUnionSlot`).
///  - Intra-function pointer locals never materialize as pointer values.
///    A Steensgaard-style union-find pre-pass (`PointerRegionAnalysis`)
///    resolves every local pointer variable to a single base object; the
///    pointer decomposes into that base plus an i64 element cursor held in
///    an ordinary rank-0 `memref<i64>` cell, which `--mem2reg` promotes
///    exactly like an int local. Dereference and subscript become
///    `emitrust.subscript(base, cursor)` (the address of a scalar has no
///    cursor and resolves to the scalar's own place), pointer arithmetic
///    becomes i64 cursor arithmetic, and same-object pointer difference and
///    comparison become plain i64 `arith` ops (C's ptrdiff_t is `long` on
///    the supported targets). Into a multi-dimensional array the cursor is
///    flat and row-major (it counts innermost elements, so `&arr[i][j]` on
///    `T arr[M][N]` is the cursor `i*N + j`), an index over a row scales by
///    the row's flat element count, and materializing the place peels one
///    `emitrust.subscript` per array level by dividing the cursor by the
///    level's span and continuing with the remainder; the walking
///    arithmetic forms on row pointers (`++`, `+ n`, `+=`, difference) are
///    rejected. A `char *` bound to a string literal is a cursor into a
///    read-only region backed by an immutable local byte array holding the
///    literal's bytes plus the terminating NUL; the same backing serves
///    literals in expression position (C99-28) — `"abc"[i]` and
///    `*("abc" + n)` create it on demand and read through it — and every
///    write into a literal (plain or compound assignment, ++/--, the
///    deref spelling) is rejected by one guard on the const-marked
///    backing (writing a C string literal is UB). The
///    `__func__`-family predefined identifiers (C99 6.4.2.2) carry their
///    function-name `StringLiteral` and take the same literal paths —
///    printf/puts `%s` arguments, char-pointer bindings, and string-helper
///    arguments — while any other value use is rejected. A region
///    that sees a null pointer constant is nullable (CTS-P8): each of its
///    pointers models an Option of its cursor, with the discriminant in a
///    promotable i1 "non-null" flag cell — `p = NULL` stores false, an
///    address binding stores true, and a null-check (`p == NULL`,
///    `if (p)`) reads the flag (statically non-null pointers fold their
///    null-checks to constants). A pointer-typed conditional operator is
///    a pointer source (CTS-P9): both arms classify into one region, the
///    emission assigns each arm in its own block, and a base-less
///    nullable region with a conditional source is STATICALLY NULL —
///    zero runtime state, folded null tests, `(int) p` folds to 0, and
///    dereference stays rejected. Dereferencing a possibly-null pointer
///    emits `assert!(flag, "null pointer dereference")` first — C
///    dereferencing null is UB, so the deterministic panic is a legal
///    refinement, mirroring the fn_ptr `expect`. Passing, differencing,
///    or ordering possibly-null pointers stays rejected, as does the
///    general integer-to-pointer traffic around the idiom (CTS-P3).
///    A second-order pointer (`T **pp`, CTS-P5) is a cursor into a region
///    of cursor cells — an index selecting WHICH first-order pointer to
///    operate on; the implemented shape is the degenerate one-cell region,
///    where `pp` is only ever bound to the address of one first-order
///    pointer local, so the selection is static, `pp` needs no runtime
///    state, `*pp` reads/rebinds the selected pointer's decomposition, and
///    `**pp` dereferences it (no reference-to-reference ever arises in the
///    emitted Rust). Third-order pointers, multi-target selections,
///    second-order copies, and null second-order bindings are rejected.
///    A pointer that rebinds across several distinct same-kind local
///    objects of one element type keeps one region under the
///    enum-of-bases model (CTS-P7): each pointer of the region adds a
///    promotable i32 base-discriminant cell naming its active base
///    (address bindings store the bound base's index, `p = q` copies the
///    discriminant), every dereference dispatches on the discriminant —
///    a match over the closed set of bases that stages the active
///    element and, for writes, dispatches the mutated value back — and
///    same-region equality compares (discriminant, cursor) pairs.
///    Difference, ordering, and passing multi-base pointers onward stay
///    rejected, as do regions mixing base kinds or element types.
///    Pointers whose address escapes outside a consumed second-order
///    binding are rejected with located diagnostics.
///  - Pointer parameters are classified per definition (Phase 1b): a
///    parameter that is only dereferenced or arrowed stays a scalar
///    reference `!emitrust.mut_ref<T>`; a parameter that is subscripted,
///    walked, compared, differenced, reassigned, or passed onward becomes a
///    slice reference `!emitrust.mut_ref<!emitrust.slice<T>>`, dereferenced
///    once in the entry block into the `!emitrust.lvalue<slice>` base place
///    of an ordinary (base, cursor) decomposition. Array parameters decay
///    to pointers in C and classify the same way (C99-36) — including the
///    C99 bracket forms `a[static N]`, `a[const]`, and `a[volatile]`,
///    whose static length is a caller-side guarantee and whose qualifiers
///    land on the decayed POINTER object itself, which the decomposition
///    erases, so they are accepted and ignored. At call sites every value
///    argument is materialized before any borrow-producing argument (C
///    leaves the order unspecified; this keeps loads out of the borrow/call
///    window), a slice argument reslices its region base with
///    `emitrust.slice_of`, a scalar-reference argument borrows the
///    designated element with `emitrust.addr_of`, and two borrow arguments
///    resolving to the same region base are rejected (Rust aliasing).
///  - Function pointers are ordinary `Copy` values of `!emitrust.fn_ptr`
///    type (rendered `Option<fn(...)>`), never decomposed by the pointer
///    region analysis: a function reference (`f`, `&f`) becomes an
///    `emitrust.constant` with an opaque `Some(name)` payload after the
///    referenced function's signature is checked against the pointer type,
///    the null pointer constant becomes `None`, indirect calls become
///    `emitrust.call_indirect` (argument/result types checked like direct
///    calls), and truth tests and `==`/`!=` compare against a `None`
///    constant with `emitrust.cmp`. Variadic targets, unimported targets,
///    signature mismatches, argument-carrying calls through prototype-less
///    pointers, and fn_ptr component types outside the verifier set are
///    located rejections. A file-scope function pointer initialized to a
///    known function and never reassigned (nor address-taken) anywhere in
///    the TU is statically devirtualized (CTS-S, 00189): it becomes an
///    import-time alias with no global of its own, calls through it lower
///    as direct calls to the target, and value uses read as the
///    `Some(target)` constant. A variadic target aliases only when it is
///    the hosted definition-less printf/fprintf, whose calls route
///    through the printf machinery — the fprintf shape swallows its
///    leading `stdout` argument (the only position accepting a FILE*).
///  - Owner structs / active objects (Phase 4): the per-TU import is
///    two-pass. Pass A (`planOwners`) is a pure AST analysis run before any
///    IR is built: per-function pointer regions are unified across call
///    sites (each pointer argument's root object with the callee
///    definition's parameter), and a class that resolves to exactly one
///    non-escaping local array of at most 32 elements (the struct_def
///    `Default` derive MVP limit), crosses at least one function boundary,
///    and whose unified functions are all defined — with every call site
///    visible — in this translation unit is promoted. The base variable
///    becomes a module-level `emitrust.struct_def @Owner_<fn>_<base>
///    ["data"]` owner struct, and each unified function becomes a method:
///    its signature trades every pointer parameter for an i64 element index
///    behind a leading `!emitrust.mut_ref<!emitrust.struct<...>>` receiver,
///    its `func.func` carries the `emitrust.method_of` attribute, and its
///    body decomposes each index parameter against the receiver's
///    `deref(arg0) -> member("data")` place with the ordinary cursor
///    machinery. Call sites lower pointer arguments to their i64 cursors
///    and borrow the owner place for exactly one `emitrust.method_call`-
///    tagged `func.call`, which `convert-func-to-emitrust` rewrites into an
///    `emitrust.method_call` on the place (no borrow survives). Any failed
///    promotion condition falls back silently to the Phase-1b slice
///    lowering; interprocedural multi-base classes are not errors. Pass B
///    is the historical import, consulting the plans in `importFunction`,
///    `emitLocalVar`, and `emitCall`.
///  - Complete named enums become module-level `emitrust.enum_def`
///    definitions; enum-typed values are opaque `!emitrust.enum` values held
///    in `emitrust.variable` places, and enumerator references become
///    `emitrust.constant` ops with an opaque `Name::Variant` payload.
///    Anonymous enums contribute plain `i32` constants only, and a value
///    of anonymous enum type is a plain `i32`.
///  - Variables with static storage duration (file-scope variables and
///    function-local statics, the latter mangled `<function>_<name>`)
///    become module-level `emitrust.global`s with constant-evaluated
///    initializers. Whole-value reads and writes use
///    `emitrust.global_load`/`emitrust.global_store`; element and field
///    accesses stage the whole value in a local copy and store it back
///    after a mutation, which is exact for the single-threaded subset.
///    Taking the address of a global in value position stays rejected.
///  - Pointer-typed globals (CTS-P4) decompose like pointer locals, but
///    against a *global* region base: the pointer's cursor is a stored
///    i64 `emitrust.global` under the pointer's C name (a cursor is a
///    plain Copy integer, so storing it globally carries no borrow —
///    which is what the `thread_local!`+Cell global model requires). The
///    base is exactly one of: a global scalar/struct object (degenerate,
///    no runtime state at all), a global array (cursor + staged element
///    access), a file-scope compound literal (synthesized
///    `<name>_backing` global), or a single constant-size
///    `calloc`/`malloc` site (synthesized zero-initialized backing
///    array; the assignment re-zeroes it, which is exact for calloc and
///    a legal refinement of malloc's indeterminate contents). Program-wide
///    facts are gathered by running the region analysis over every body in
///    Pass A. Located rejections: binding a global pointer to a local
///    object (the borrow would outlive the object — the exact program
///    rustc refuses), copying a global pointer into another pointer
///    variable, passing one to a function (the callee would see a borrow
///    of the staged copy), multiple bases, string-literal bases, null
///    constants, and external linkage in a multi-file project.
///  - Pointer struct members, pointer returns, and pointer casts
///    (CTS-P2): a data-pointer struct member is stored as a plain i64
///    cursor field (a cursor is a borrow-free Copy integer, so a struct
///    can hold one), and the analysis resolves each member to one
///    statically known target object — or one write-only string literal —
///    per struct instance, gathered program-wide from every body in Pass A
///    plus the constant-initializer walk of global objects. Supported
///    bindings are degenerate (the member designates one whole object), so
///    the stored i64 stays 0: member writes emit nothing and member reads
///    resolve to the bound object's place with no runtime state (a
///    `s.p->p->x` chain folds hop by hop). Conflicting bindings reject
///    naming both sites; writes through aliases, escaping member
///    addresses, and whole-struct overwrites poison the field program-wide
///    (every read rejects, located at the unresolvable site). A
///    data-pointer *return type* classifies by its return sites
///    (principal-kind inference): the supported kinds are a returned
///    function address behind a `void *` return, emitted as the plain
///    `!emitrust.fn_ptr` result, and the single-global-base return
///    (CTS-S, 00089) — every site returns the address of ONE mutable
///    whole global, so the pointer result is ERASED from the signature
///    (the call is retained for its side effects) and callers route
///    `f()->member` accesses to the global through the staged-copy +
///    writeback machinery with zero runtime pointer state; the erasure
///    also applies to a fn-ptr signature returning a data pointer when
///    every address-taken function of that return type erases to the
///    same base, so indirect calls route identically. Nullable returns,
///    disagreeing bases, and member-address sites are located
///    rejections; returning a cursor into a callee-local
///    region stays rejected at the return site (it would dangle).
///    Qualification-preserving explicit casts (same unqualified pointee)
///    peel transparently in analysis and emission. A `void *` is a
///    pointee-wildcard cursor (CTS-P9): casts to and from a `void`
///    pointee peel transparently at any matching pointer depth, a
///    `&struct.member` of a directly named local or global struct roots
///    a region at that scalar member (resolved through `emitrust.member`
///    on the object's place or its staged global copy; union members and
///    member-base arithmetic are rejected), and a reinterpret-back site
///    `*(T *)p` type-checks T against the region's base element type —
///    exact matches lower directly, same-width int<->int views wrap the
///    load/store in an `emitrust.cast` bitcast, and every other
///    reinterpretation (including `void *` parameters and uncast `void *`
///    dereference) stays rejected.
///  - Cell-slice parameters (CTS-P10): a pointer-parameter class whose
///    interprocedural bases are ALL mutable global arrays of one scalar
///    element type lowers to the shared `!emitrust.ref<!emitrust.cell_slice
///    <T>>` (`&[std::cell::Cell<T>]`) — the coherence-sound choice, since a
///    callee may mutate through the parameter while other code reads the
///    globals directly mid-call (a staged copy would be unsound; both
///    sides hit the same thread-local Cell). Pass A (`planCellSlices`)
///    classifies via a union-find over exactly two argument shapes —
///    direct global-array decay and parameter forwarding — element reads
///    and writes are `emitrust.cell_get`/`emitrust.cell_set` on the
///    reference itself (no lvalue staging, no cursor cell), unwalked
///    parameters forward as the same SSA value (permuted recursion
///    included), and call sites nest one `emitrust.global_cells` region
///    per distinct global argument (leftmost outermost) with a scalar
///    result flowing out through a staging variable. Mixing a global base
///    with a local object and null-checked global-backed parameters stay
///    located rejections with class-precise wordings.
///  - Byte puns over i8 regions (CTS-P11): a wider-than-element
///    reinterpreting deref `*(T *)p` is accepted when the region's base
///    element is a byte (a C char array) and sizeof(T) is 2/4/8 — the
///    access widens to sizeof(T) consecutive bytes at the runtime cursor,
///    loading via `T::from_ne_bytes` and storing via `T::to_ne_bytes`
///    (compound assignments read-modify-write the same window). Works
///    over local AND global char arrays (global regions ride the ordinary
///    staged-copy + writeback model, so a following direct or `%s` read
///    of the global sees the punned bytes). A compile-time-constant
///    offset whose window overruns the array is a located rejection, and
///    wide views over non-byte bases keep the reinterpret rejection
///    family.
///  - The `inline` specifier is a semantic no-op (C99-18): every inline
///    definition — plain C99 `inline` without extern (whose body clang
///    still supplies even though C99 6.7.4p7 makes it no external
///    definition), `extern inline`, and `static inline` — imports as an
///    ordinary function definition; in the merged whole-program module
///    one ordinary definition per external name is the right shape, and
///    `static inline` keeps the per-TU mangling of any other file-static.
///  - Type qualifiers (C99-7): const maps positionally — a never-written
///    const global (scalar or array) is a `const`-marked emitrust.global
///    (an immutable Rust static, `static const` locals included via their
///    mangled module global), string-literal backings are `const`-marked
///    variables, const locals keep the ordinary lowering (clang already
///    rejects writes through const lvalues; mem2reg renders scalar SSA as
///    immutable lets), and a const pointee classifies exactly like its
///    unqualified spelling. volatile is rejected by policy with a located
///    "unsupported: volatile-qualified type" wherever a declared type
///    carries it at any level (locals, globals, parameters' pointee
///    chains, struct fields, return types — `hasVolatileQualifier`), and
///    a cast that introduces volatile refuses the qualification peel; the
///    lone exception is a qualifier on a parameter OBJECT itself
///    (`volatile int v`, `int x[volatile 5]` adjusting to
///    `int * volatile x`), which is body-local, never part of the
///    function type, and accepted-and-ignored. restrict is accepted and
///    ignored everywhere (an aliasing hint; the region analysis is
///    stricter). _Atomic is rejected with a located
///    "unsupported: _Atomic-qualified type".
///  - Hosted libc subset (C99-48): a curated set of definition-less libc
///    calls lowers by name to safe Rust — never to libc linkage — and
///    every intercept carries a `!getDefinition()` guard, so a
///    user-defined function of the same name imports as an ordinary
///    call. stdio: printf/puts/putchar/sprintf through the shared format
///    grammar, plus the owned FILE* slice (fopen/fgetc/fread/fwrite/
///    fgets/fclose). string.h: strlen/strcmp/strncmp/memcmp/strchr/
///    strrchr in value position and strcpy/strncpy/strcat/memset/memcpy/
///    memmove in statement position, each a one-per-module
///    `__emitrust_*` helper over bounds-checked i8 slices of the
///    argument's char region (memmove shares memcpy's lowering: distinct
///    regions never overlap and the same-object shape is `copy_within`,
///    which IS memmove). stdlib.h: abs/labs onto `wrapping_abs` (the
///    INT_MIN wrap deterministically refines C UB), atoi as an exact C
///    parse over a char region (`__emitrust_atoi`), and
///    statement-position exit onto `std::process::exit`. math.h: the
///    IEEE-exact fabs/sqrt/floor/ceil onto the matching f64 methods,
///    plus the differentially pinned sin; exp/log/pow are rejected by
///    policy with a located diagnostic (no accuracy mandate in C, libm
///    implementations disagree). Every uncurated libc function keeps the
///    system-header use-site rejection.
///
/// The importer is a functional core (the `CImporter` class below, which
/// owns the builder and per-function symbol table) driven by the imperative
/// shell in `importC`, which runs clang LibTooling and verifies the result.
/// Every unsupported construct produces a located diagnostic and fails the
/// import; no silently wrong IR is ever produced.
//
//===----------------------------------------------------------------------===//


#include "EmitRust/ImportC.h"

#include "CImporterInternal.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// PointerRegionAnalysis
//===----------------------------------------------------------------------===//

void PointerRegionAnalysis::analyze(clang::ASTContext &astContext,
                                    const clang::Stmt *body) {
  context = &astContext;
  parent.clear();
  regions.clear();
  pointerVars.clear();
  secondOrderVars.clear();
  secondOrderRegions.clear();
  consumedAddrOf.clear();
  memberFacts.clear();
  poisonedFields.clear();
  visit(body);
  context = nullptr;
}

const PointerRegion *PointerRegionAnalysis::regionOf(const clang::VarDecl *var) {
  if (!pointerVars.contains(var))
    return nullptr;
  auto it = regions.find(findRoot(var));
  return it == regions.end() ? nullptr : &it->second;
}

const clang::VarDecl *
PointerRegionAnalysis::findRoot(const clang::VarDecl *decl) {
  auto [it, inserted] = parent.try_emplace(decl, decl);
  if (inserted)
    return decl;
  // Iterative find with full path compression. The chains are tiny (a
  // handful of pointers per function), so this is bounded in practice.
  const clang::VarDecl *root = decl;
  while (parent[root] != root)
    root = parent[root];
  while (parent[decl] != root) {
    const clang::VarDecl *next = parent[decl];
    parent[decl] = root;
    decl = next;
  }
  return root;
}

/// Merges the facts of `absorbed` into `target`: bases are deduplicated by
/// declaration, literal and allocation bases conflict when they differ (a
/// pointer cannot range over two of them), flag/location pairs keep the
/// first recorded site, and only the first invalidation is kept. Used both
/// by the per-function union-find (`unite`) and by the program-wide
/// aggregation of a global pointer's per-function regions (`planOwners`).
static void mergeRegionFacts(PointerRegion &target,
                             const PointerRegion &absorbed) {
  for (const PointerBaseBinding &binding : absorbed.bases) {
    bool known = llvm::any_of(target.bases,
                              [&](const PointerBaseBinding &existing) {
                                return existing.base == binding.base &&
                                       existing.member == binding.member;
                              });
    if (!known)
      target.bases.push_back(binding);
  }
  if (absorbed.literalBase) {
    if (!target.literalBase) {
      target.literalBase = absorbed.literalBase;
      target.literalLoc = absorbed.literalLoc;
    } else if (target.literalBase != absorbed.literalBase &&
               target.invalidReason.empty()) {
      target.invalidReason =
          "unsupported: pointer bound to multiple string literals";
      target.invalidLoc = absorbed.literalLoc;
    }
  }
  if (absorbed.allocSite) {
    if (!target.allocSite) {
      target.allocSite = absorbed.allocSite;
      target.allocLoc = absorbed.allocLoc;
      target.allocCount = absorbed.allocCount;
    } else if (target.allocSite != absorbed.allocSite &&
               target.invalidReason.empty()) {
      target.invalidReason =
          "unsupported: global pointer bound to multiple allocations";
      target.invalidLoc = absorbed.allocLoc;
    }
  }
  if (absorbed.hasArithmetic && !target.hasArithmetic) {
    target.hasArithmetic = true;
    target.arithmeticLoc = absorbed.arithmeticLoc;
  }
  if (absorbed.hasWriteThrough && !target.hasWriteThrough) {
    target.hasWriteThrough = true;
    target.writeThroughLoc = absorbed.writeThroughLoc;
  }
  if (absorbed.nullable && !target.nullable) {
    target.nullable = true;
    target.nullableLoc = absorbed.nullableLoc;
  }
  target.hasConditionalSource |= absorbed.hasConditionalSource;
  if (absorbed.hasCarrierSource && !target.hasCarrierSource) {
    target.hasCarrierSource = true;
    target.carrierLoc = absorbed.carrierLoc;
  }
  if (!absorbed.invalidReason.empty() && target.invalidReason.empty()) {
    target.invalidReason = absorbed.invalidReason;
    target.invalidLoc = absorbed.invalidLoc;
  }
  // A union that joins an integer-carrier source with a real address base
  // (or literal/allocation) straddles the two pointer models (CTS-P3); it
  // keeps the historical non-address rejection at the carrier site.
  if (target.hasCarrierSource &&
      (!target.bases.empty() || target.literalBase || target.allocSite) &&
      target.invalidReason.empty()) {
    target.invalidReason = "unsupported: pointer assigned a non-address value";
    target.invalidLoc = target.carrierLoc;
  }
}

void PointerRegionAnalysis::unite(const clang::VarDecl *a,
                                  const clang::VarDecl *b) {
  const clang::VarDecl *rootA = findRoot(a);
  const clang::VarDecl *rootB = findRoot(b);
  if (rootA == rootB)
    return;
  parent[rootB] = rootA;
  auto itB = regions.find(rootB);
  if (itB == regions.end())
    return;
  // Merge the absorbed root's facts into the surviving root's region.
  PointerRegion absorbed = std::move(itB->second);
  regions.erase(itB);
  mergeRegionFacts(regions[rootA], absorbed);
}

PointerRegion &PointerRegionAnalysis::regionFor(const clang::VarDecl *decl) {
  return regions[findRoot(decl)];
}

void PointerRegionAnalysis::addBase(const clang::VarDecl *ptr,
                                    const clang::VarDecl *base,
                                    clang::SourceLocation loc,
                                    const clang::FieldDecl *member) {
  bool ptrIsGlobal = !ptr->hasLocalStorage();
  bool baseIsGlobal = !base->hasLocalStorage();
  // A local pointer into a global object is a valid region base (CTS-P6):
  // its cursor is a plain Copy i64 and every element access stages the
  // global's whole value exactly like a direct global element access, so
  // no borrow is ever held across statements. A global pointer bound to a
  // local object is a borrow escaping the object's scope — the exact
  // program rustc would refuse — rejected here, at the binding site
  // (CTS-P4).
  if (ptrIsGlobal && !baseIsGlobal)
    return markInvalid(
        ptr, loc,
        ("unsupported: global pointer bound to local object '" +
         base->getName() + "' (the borrow would outlive the object)")
            .str());
  if (baseIsGlobal)
    base = base->getCanonicalDecl();
  // A member binding roots at the member's own place and never shares
  // cursor state with pointers into the whole object, so the base object
  // stays out of the union-find (CTS-P9); a whole-object binding joins it
  // so that two pointers into one object always share a region.
  if (!member)
    unite(ptr, base);
  PointerRegion &region = regionFor(ptr);
  // An address binding into a region already fed by an integer carrier
  // straddles the two pointer models (CTS-P3); the region keeps the
  // historical non-address rejection at the carrier site.
  if (region.hasCarrierSource)
    return markInvalid(ptr, region.carrierLoc,
                       "unsupported: pointer assigned a non-address value");
  bool known = llvm::any_of(region.bases,
                            [&](const PointerBaseBinding &existing) {
                              return existing.base == base &&
                                     existing.member == member;
                            });
  if (!known)
    region.bases.push_back(PointerBaseBinding{base, loc, member});
}

void PointerRegionAnalysis::addLiteralBase(const clang::VarDecl *ptr,
                                           const clang::StringLiteral *literal,
                                           clang::SourceLocation loc) {
  PointerRegion &region = regionFor(ptr);
  // A literal binding cannot join an integer-carrier region (CTS-P3).
  if (region.hasCarrierSource)
    return markInvalid(ptr, region.carrierLoc,
                       "unsupported: pointer assigned a non-address value");
  if (!region.literalBase) {
    region.literalBase = literal;
    region.literalLoc = loc;
    return;
  }
  if (region.literalBase != literal)
    markInvalid(ptr, loc,
                "unsupported: pointer bound to multiple string literals");
}

void PointerRegionAnalysis::recordArithmetic(const clang::VarDecl *ptr,
                                             clang::SourceLocation loc) {
  PointerRegion &region = regionFor(ptr);
  if (!region.hasArithmetic) {
    region.hasArithmetic = true;
    region.arithmeticLoc = loc;
  }
}

void PointerRegionAnalysis::recordNullable(const clang::VarDecl *ptr,
                                           clang::SourceLocation loc) {
  PointerRegion &region = regionFor(ptr);
  if (!region.nullable) {
    region.nullable = true;
    region.nullableLoc = loc;
  }
}

void PointerRegionAnalysis::recordCarrierSource(const clang::VarDecl *ptr,
                                                clang::SourceLocation loc) {
  // A global pointer never carries integers: its program-wide facts feed
  // the CTS-P4/P6 global machinery, which has no carrier lowering; the
  // historical rejection is kept unchanged.
  if (!ptr->hasLocalStorage())
    return markInvalid(ptr, loc,
                       "unsupported: pointer assigned a non-address value");
  PointerRegion &region = regionFor(ptr);
  // A carrier source into a region that already binds a real address
  // base, string literal, or allocation straddles the two models.
  if (!region.bases.empty() || region.literalBase || region.allocSite)
    return markInvalid(ptr, loc,
                       "unsupported: pointer assigned a non-address value");
  if (!region.hasCarrierSource) {
    region.hasCarrierSource = true;
    region.carrierLoc = loc;
  }
}

void PointerRegionAnalysis::recordWriteThrough(const clang::VarDecl *ptr,
                                               clang::SourceLocation loc) {
  PointerRegion &region = regionFor(ptr);
  if (!region.hasWriteThrough) {
    region.hasWriteThrough = true;
    region.writeThroughLoc = loc;
  }
}

void PointerRegionAnalysis::recordAllocBase(const clang::VarDecl *ptr,
                                            const clang::CallExpr *call,
                                            clang::SourceLocation loc) {
  clang::QualType pointee =
      ptr->getType().getCanonicalType()->getPointeeType();
  if (pointee->isIncompleteType() || pointee->isFunctionType())
    return markInvalid(ptr, loc,
                       "unsupported: allocation bound to a pointer with an "
                       "unsized element type");
  uint64_t elementBytes =
      context->getTypeSizeInChars(pointee).getQuantity();
  auto evalConstant = [&](const clang::Expr *arg, uint64_t &out) {
    clang::Expr::EvalResult result;
    if (!arg->EvaluateAsInt(result, *context) ||
        result.Val.getInt().isNegative())
      return false;
    out = result.Val.getInt().getZExtValue();
    return true;
  };
  llvm::StringRef callee = call->getDirectCallee()->getName();
  uint64_t totalBytes = 0;
  if (callee == "calloc") {
    uint64_t count = 0;
    uint64_t size = 0;
    if (call->getNumArgs() != 2 ||
        !evalConstant(call->getArg(0), count) ||
        !evalConstant(call->getArg(1), size))
      return markInvalid(ptr, loc,
                         "unsupported: allocation size is not a "
                         "compile-time constant");
    totalBytes = count * size;
  } else {
    if (call->getNumArgs() != 1 ||
        !evalConstant(call->getArg(0), totalBytes))
      return markInvalid(ptr, loc,
                         "unsupported: allocation size is not a "
                         "compile-time constant");
  }
  // The synthesized backing is a fixed-size Rust array; cap it so the
  // generated code stays reasonable (matching no real program in the
  // suite is expected to exceed this).
  if (elementBytes == 0 || totalBytes == 0 ||
      totalBytes % elementBytes != 0 ||
      totalBytes / elementBytes > 65536)
    return markInvalid(ptr, loc,
                       "unsupported: allocation size does not fit the "
                       "pointer's element type");
  PointerRegion &region = regionFor(ptr);
  if (region.allocSite && region.allocSite != call)
    return markInvalid(ptr, loc,
                       "unsupported: global pointer bound to multiple "
                       "allocations");
  region.allocSite = call;
  region.allocLoc = loc;
  region.allocCount = totalBytes / elementBytes;
}

const clang::VarDecl *
PointerRegionAnalysis::trackedWritePlaceRoot(const clang::Expr *place) {
  // Peel the dereference or subscript that designates the written element.
  const clang::Expr *pointerExpr = nullptr;
  const clang::Expr *e = stripTrivia(place);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
    if (unary->getOpcode() == clang::UO_Deref)
      pointerExpr = unary->getSubExpr();
  } else if (const auto *subscript =
                 llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
    if (isPointerType(subscript->getBase()->getType()))
      pointerExpr = subscript->getBase();
  }
  if (!pointerExpr)
    return nullptr;
  // Walk the pointer expression down to the tracked pointer local it reads
  // (through casts, ++/--, and +/- offset forms).
  const clang::Expr *cursor = stripTrivia(pointerExpr);
  while (true) {
    // Explicit decomposition-transparent casts (`*(int *)p = v` through a
    // `void *`, CTS-P9) peel exactly like the emission's peel.
    if (const clang::Expr *peeled = peelPointerCast(*context, cursor)) {
      cursor = stripTrivia(peeled);
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(cursor)) {
      cursor = stripTrivia(cast->getSubExpr());
      continue;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(cursor)) {
      if (unary->isIncrementDecrementOp()) {
        cursor = stripTrivia(unary->getSubExpr());
        continue;
      }
      if (unary->getOpcode() == clang::UO_Deref) {
        // `**pp` / `(*pp)[i]`: the written region is the one of the
        // first-order pointer the second-order pointer selects.
        if (const clang::VarDecl *pp = asSecondOrderDeref(unary)) {
          auto it = secondOrderRegions.find(pp);
          if (it != secondOrderRegions.end() && it->second.target &&
              tracks(it->second.target))
            return it->second.target;
        }
        return nullptr;
      }
      return nullptr;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(cursor)) {
      if (binary->getOpcode() == clang::BO_Add ||
          binary->getOpcode() == clang::BO_Sub) {
        cursor = stripTrivia(isPointerType(binary->getLHS()->getType())
                                 ? binary->getLHS()
                                 : binary->getRHS());
        continue;
      }
      return nullptr;
    }
    break;
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(cursor))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (tracks(var))
        return var;
  // A global data pointer at the root joins the analysis on first sight
  // (mirroring the arithmetic and rebinding forms), so the write-through
  // fact reaches the program-wide merge even when the write is the body's
  // only mention of the global.
  if (const clang::VarDecl *global = asGlobalDataPointerRef(cursor)) {
    pointerVars.insert(global);
    return global;
  }
  return nullptr;
}

void PointerRegionAnalysis::markInvalid(const clang::VarDecl *ptr,
                                        clang::SourceLocation loc,
                                        llvm::StringRef reason) {
  PointerRegion &region = regionFor(ptr);
  if (region.invalidReason.empty()) {
    region.invalidReason = reason.str();
    region.invalidLoc = loc;
  }
}

void PointerRegionAnalysis::markSecondOrderInvalid(const clang::VarDecl *ptr,
                                                   clang::SourceLocation loc,
                                                   llvm::StringRef reason) {
  SecondOrderRegion &region = secondOrderRegions[ptr];
  if (region.invalidReason.empty()) {
    region.invalidReason = reason.str();
    region.invalidLoc = loc;
  }
}

const clang::VarDecl *
PointerRegionAnalysis::asSecondOrderDeref(const clang::Expr *expr) const {
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(expr));
  if (!unary || unary->getOpcode() != clang::UO_Deref)
    return nullptr;
  // `*(int **)pp` on a `void **` peels the reinterpret-back cast exactly
  // like the emission (the pointee-wildcard rule, CTS-P9).
  const clang::Expr *sub = stripTrivia(unary->getSubExpr());
  while (const clang::Expr *peeled = peelPointerCast(*context, sub))
    sub = stripTrivia(peeled);
  const clang::VarDecl *var = asLoadedLocalVarRef(sub);
  return var && secondOrderVars.contains(var) ? var : nullptr;
}

void PointerRegionAnalysis::recordSecondOrderWrite(const clang::VarDecl *ptr,
                                                   const clang::Expr *rhs) {
  const clang::Expr *e = stripTrivia(rhs);
  clang::SourceLocation loc = e->getBeginLoc();

  // A nullable selection would need an Option over the cursor-cell region
  // (the CTS-P8 discriminant one order up); outside the degenerate scope.
  if (e->isNullPointerConstant(*context,
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return markSecondOrderInvalid(ptr, loc,
                                  "unsupported: null pointer constant "
                                  "assigned to a pointer-to-pointer variable");

  // `pp = &p`: the address of a tracked first-order pointer local binds
  // `p` as the selection target. The `&p` expression is consumed here, so
  // the generic escape check does not invalidate `p`'s region — the
  // pointer's cursor never escapes; `pp` merely selects its cells.
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_AddrOf)
      if (const clang::VarDecl *target =
              asLocalVarRef(unary->getSubExpr()))
        if (tracks(target)) {
          consumedAddrOf.insert(unary);
          SecondOrderRegion &region = secondOrderRegions[ptr];
          if (!region.invalidReason.empty())
            return;
          if (!region.target || region.target == target) {
            if (!region.target) {
              region.target = target;
              region.targetLoc = loc;
            }
            return;
          }
          if (!region.secondTarget) {
            region.secondTarget = target;
            region.secondTargetLoc = loc;
          }
          return;
        }

  // `pp = pp2` would alias two selections of cursor cells; a general
  // region of indices (CTS-P5 beyond the degenerate cell) is not modeled.
  if (const clang::VarDecl *source = asLoadedLocalVarRef(e))
    if (secondOrderVars.contains(source))
      return markSecondOrderInvalid(
          ptr, loc, "unsupported: copying a pointer-to-pointer variable");

  markSecondOrderInvalid(ptr, loc,
                         "unsupported: pointer-to-pointer variable assigned "
                         "a value that is not the address of a local pointer "
                         "variable");
}

void PointerRegionAnalysis::poisonMemberField(const clang::FieldDecl *field,
                                              clang::SourceLocation loc) {
  poisonedFields.try_emplace(field, loc);
}

void PointerRegionAnalysis::markMemberInvalid(const clang::VarDecl *instance,
                                              const clang::FieldDecl *field,
                                              clang::SourceLocation loc,
                                              llvm::StringRef reason) {
  MemberPointerFacts &facts =
      memberFacts[{instance->getCanonicalDecl(), field}];
  if (facts.invalidReason.empty()) {
    facts.invalidReason = reason.str();
    facts.invalidLoc = loc;
  }
}

void PointerRegionAnalysis::bindMemberPointer(
    const clang::VarDecl *instance, const clang::FieldDecl *field,
    const clang::VarDecl *base, const clang::StringLiteral *literal,
    clang::SourceLocation loc) {
  if (base)
    base = base->getCanonicalDecl();
  MemberPointerFacts &facts =
      memberFacts[{instance->getCanonicalDecl(), field}];
  if (!facts.base && !facts.literal) {
    facts.base = base;
    facts.literal = literal;
    facts.loc = loc;
    return;
  }
  if (facts.base == base && facts.literal == literal)
    return; // Idempotent rebinding to the same target.
  if (facts.invalidReason.empty()) {
    facts.invalidReason =
        ("unsupported: pointer struct member '" + field->getName() +
         "' bound to two different targets")
            .str();
    facts.invalidLoc = loc;
    facts.secondLoc = loc;
  }
}

void PointerRegionAnalysis::recordMemberPointerWrite(
    const clang::VarDecl *instance, const clang::FieldDecl *field,
    const clang::Expr *rhs) {
  const clang::Expr *e = stripTrivia(rhs);
  clang::SourceLocation loc = e->getBeginLoc();
  // The null pointer constant establishes no target; a member that only
  // ever holds null has no binding, and its reads (a null dereference in
  // C, undefined behavior) reject at the read site.
  if (e->isNullPointerConstant(*context,
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return markMemberInvalid(instance, field, loc,
                             "unsupported: null pointer constant assigned "
                             "to a pointer struct member");
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
    if (unary->getOpcode() == clang::UO_AddrOf) {
      const clang::Expr *sub = stripTrivia(unary->getSubExpr());
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub))
        if (const auto *target =
                llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
          if (isPointerType(target->getType()))
            return markMemberInvalid(
                instance, field, loc,
                "unsupported: taking the address of a pointer variable");
          // `s.f = &x`: a degenerate whole-object binding; the pointee
          // must be the object's own type (no element runs, no offsets).
          if (!context->hasSameUnqualifiedType(
                  field->getType().getCanonicalType()->getPointeeType(),
                  target->getType()))
            return markMemberInvalid(instance, field, loc,
                                     "unsupported: pointer struct member "
                                     "type does not match its target "
                                     "object");
          return bindMemberPointer(instance, field, target, nullptr, loc);
        }
      return markMemberInvalid(instance, field, loc,
                               "unsupported: pointer struct member bound "
                               "to this address expression");
    }
  }
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_ArrayToPointerDecay) {
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      // `s.f = "..."`: a write-only literal binding (reads stay rejected;
      // nothing in the supported subset can observe the member).
      if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(sub))
        return bindMemberPointer(instance, field, nullptr, literal, loc);
      return markMemberInvalid(instance, field, loc,
                               "unsupported: pointer struct member bound "
                               "to an array");
    }
  markMemberInvalid(instance, field, loc,
                    "unsupported: pointer struct member assigned a "
                    "non-address value");
}

void PointerRegionAnalysis::collectStructInitBindings(
    const clang::VarDecl *instance, const clang::InitListExpr *list) {
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  const clang::RecordDecl *record = recordOfType(list->getType());
  if (!record)
    return;
  unsigned index = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    if (index >= list->getNumInits())
      break;
    const clang::Expr *element = list->getInit(index++);
    if (llvm::isa<clang::ImplicitValueInitExpr>(element))
      continue; // Zero fill: a data-pointer member stays unbound (null).
    if (isDataPointer(field->getType())) {
      recordMemberPointerWrite(instance, field, element);
      continue;
    }
    // A data-pointer field inside a nested aggregate has an instance path
    // the per-instance model does not key; a non-implicit initializer for
    // one poisons the field.
    if (const auto *nested = llvm::dyn_cast<clang::InitListExpr>(element)) {
      const clang::RecordDecl *nestedRecord =
          recordOfType(nested->getType());
      if (nestedRecord && recordHasDataPointerField(nestedRecord))
        poisonRecordPointerFields(nestedRecord, element->getBeginLoc());
    }
  }
}

void PointerRegionAnalysis::poisonRecordPointerFields(
    const clang::RecordDecl *record, clang::SourceLocation loc) {
  if (!record)
    return;
  for (const clang::FieldDecl *field : record->fields()) {
    clang::QualType fieldType = field->getType();
    while (const auto *array = llvm::dyn_cast<clang::ArrayType>(
               fieldType.getCanonicalType().getTypePtr()))
      fieldType = array->getElementType();
    if (isDataPointer(fieldType)) {
      poisonMemberField(field, loc);
      continue;
    }
    poisonRecordPointerFields(recordOfType(fieldType), loc);
  }
}

void PointerRegionAnalysis::recordPointerWrite(const clang::VarDecl *ptr,
                                               const clang::Expr *rhs) {
  const clang::Expr *e = stripTrivia(rhs);
  clang::SourceLocation loc = e->getBeginLoc();

  // A null pointer constant does not invalidate the region: it marks the
  // region nullable, and the pointer's Option-of-cursor discriminant (an
  // i1 "non-null" flag cell) records the binding at emission (CTS-P8).
  if (e->isNullPointerConstant(*context,
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return recordNullable(ptr, loc);
  // `(const void *) 0`: only the UNQUALIFIED `void *` cast is a formal
  // null pointer constant (C11 6.3.2.3p3), but the qualified cast still
  // yields the null pointer value; clang models both as CK_NullToPointer.
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    if (cast->getCastKind() == clang::CK_NullToPointer)
      return recordNullable(ptr, loc);

  // `g = calloc(n, sizeof(T))` / `g = malloc(bytes)` on a global pointer:
  // a constant-size allocation binding, promoted to a synthesized global
  // backing array. Allocations bound to local pointers keep the historical
  // non-address rejection below.
  if (!ptr->hasLocalStorage())
    if (const clang::CallExpr *alloc = asAllocCall(e))
      return recordAllocBase(ptr, alloc, loc);

  // A decomposition-transparent pointer cast — a qualification adjustment
  // or a `void *`-mediated cast (the pointee-wildcard rule, CTS-P9) — is
  // transparent to the region facts (the emission peels it identically);
  // genuinely reinterpreting casts fall through to the non-address
  // rejection.
  if (const clang::Expr *peeled = peelPointerCast(*context, e))
    return recordPointerWrite(ptr, peeled);

  // A pointer-typed conditional is a pointer source (CTS-P9): both arms
  // classify into the region, uniting whatever they bind; a null-constant
  // arm marks the region nullable through the ordinary null-binding path
  // above.
  if (const auto *conditional = llvm::dyn_cast<clang::ConditionalOperator>(e)) {
    regionFor(ptr).hasConditionalSource = true;
    recordPointerWrite(ptr, conditional->getTrueExpr());
    recordPointerWrite(ptr, conditional->getFalseExpr());
    return;
  }

  // Integer-to-pointer traffic (explicit `(void *)v` casts and the
  // implicit conversions C89-era code relies on): an integer conditional
  // classifies each arm on its own (a 0 arm is a null pointer constant, so
  // an all-null-arm conditional just marks the region nullable, the 00144
  // shape); a POINTER-WIDTH integer source marks the region an integer
  // carrier (CTS-P3) — the pointer is just an i64 in pointer clothing —
  // and a narrower source (a truncated address can never round-trip)
  // keeps the historical non-address rejection below.
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    if (cast->getCastKind() == clang::CK_IntegralToPointer) {
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (llvm::isa<clang::ConditionalOperator>(sub))
        return recordPointerWrite(ptr, sub);
      if (context->getTypeSize(cast->getSubExpr()->getType()) == 64)
        return recordCarrierSource(ptr, loc);
      return markInvalid(ptr, loc,
                         "unsupported: pointer assigned a non-address value");
    }

  // `p = f(...)` on a callee whose pointer return classifies as an
  // integer carrier (CTS-P3) propagates carrier-ness into `p`'s region;
  // any other returned pointer keeps the non-address rejection below.
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e))
    if (const clang::FunctionDecl *callee = call->getDirectCallee())
      if (carrierReturnQuery && carrierReturnQuery(callee))
        return recordCarrierSource(ptr, loc);

  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    switch (cast->getCastKind()) {
    case clang::CK_NoOp:
      return recordPointerWrite(ptr, cast->getSubExpr());
    case clang::CK_LValueToRValue: {
      // `p = q`: copying a pointer joins the two into one region.
      if (const clang::VarDecl *source = asLocalVarRef(cast->getSubExpr()))
        if (tracks(source))
          return unite(ptr, source);
      // Copying a global pointer (in either direction) would couple a
      // local cursor cell to the global's stored cursor; the shape is
      // outside the CTS-P4 model.
      if (asGlobalDataPointerRef(cast->getSubExpr()))
        return markInvalid(ptr, loc,
                           "unsupported: copying a global pointer variable");
      // `p = param`: a pointer parameter (slice-classified by this very
      // use) becomes the region base; the local walks the parameter's
      // element run through its own cursor.
      if (const clang::ParmVarDecl *param =
              asPointerParamRef(cast->getSubExpr()))
        return addBase(ptr, param, loc);
      // `p = *s` on a planned string-cursor parameter (CTS 00204): the
      // parameter is the region base — the local walks the cursor
      // parameter's byte run exactly like a slice parameter's.
      if (cursorParamQuery)
        if (const clang::ParmVarDecl *cursorParam =
                asPointerPointerParamDeref(cast->getSubExpr()))
          if (cursorParamQuery(cursorParam))
            return addBase(ptr, cursorParam, loc);
      break;
    }
    case clang::CK_ArrayToPointerDecay: {
      // `p = arr`: the decayed array is the region base. `p = "..."`
      // binds the literal as the region's read-only base, and
      // `p = __func__` binds the predefined identifier's function-name
      // literal the same way (C99-29). A decayed row of a
      // multi-dimensional array (`q = arr[i]`) peels the subscripts to
      // the same root.
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (const clang::StringLiteral *literal = underlyingStringLiteral(sub))
        return addLiteralBase(ptr, literal, loc);
      // `p = (int[]){...}` (C99-13): the decayed block-scope compound
      // literal is a fresh anonymous local object; its synthesized
      // backing declaration is the region base, exactly like a decayed
      // named array.
      if (const auto *compound =
              llvm::dyn_cast<clang::CompoundLiteralExpr>(sub)) {
        if (literalTemps && !compound->isFileScope())
          return addBase(ptr, literalTemps->getOrCreate(*context, compound),
                         loc);
        return markInvalid(
            ptr, loc, "unsupported: pointer assigned a non-address value");
      }
      while (const auto *inner =
                 llvm::dyn_cast<clang::ArraySubscriptExpr>(sub))
        sub = inner->getBase()->IgnoreParenImpCasts();
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub))
        if (const auto *array =
                llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
          if (tracks(array)) {
            // A row of the base a tracked pointer walks (`q = p[i]`)
            // joins the two pointers and moves the cursor.
            recordArithmetic(array, loc);
            return unite(ptr, array);
          }
          return addBase(ptr, array, loc);
        }
      break;
    }
    default:
      break;
    }
    return markInvalid(ptr, loc,
                       "unsupported: pointer assigned a non-address value");
  }

  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
    if (unary->getOpcode() == clang::UO_AddrOf) {
      const clang::Expr *sub = stripTrivia(unary->getSubExpr());
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
        if (const auto *target =
                llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
          // `p = &x`: `x` becomes a (degenerate, for scalars) region base.
          if (isPointerType(target->getType()))
            return markInvalid(
                ptr, loc,
                "unsupported: taking the address of a pointer variable");
          return addBase(ptr, target, loc);
        }
      }
      if (const auto *memberExpr = llvm::dyn_cast<clang::MemberExpr>(sub)) {
        // `p = &s.b` / `p = &g.b`: the address of a member of a directly
        // named local or global struct roots the region at that member
        // (CTS-P9). Union storage aliases its arms in one slot, so a
        // member-path base rooted there could not keep the aliased names
        // coherent; taking the address of a union member stays rejected.
        MemberAddressTarget target = classifyMemberAddress(memberExpr);
        if (target.touchesUnion)
          return markInvalid(
              ptr, loc, "unsupported: taking the address of a union member");
        if (target.root)
          return addBase(ptr, target.root, loc, target.field);
        return markInvalid(
            ptr, loc, "unsupported: pointer assigned a non-address value");
      }
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(sub)) {
        // `p = &arr[i]` binds the array; `p = &q[i]` is `p = q + i`; and
        // `p = &param[i]` binds a slice-classified pointer parameter.
        // Nested subscripts (`p = &arr[i][j]`) peel to the same root.
        const clang::Expr *base =
            subscript->getBase()->IgnoreParenImpCasts();
        while (const auto *inner =
                   llvm::dyn_cast<clang::ArraySubscriptExpr>(base))
          base = inner->getBase()->IgnoreParenImpCasts();
        if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base))
          if (const auto *var =
                  llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
            if (tracks(var)) {
              recordArithmetic(var, loc);
              return unite(ptr, var);
            }
            if (llvm::isa<clang::ParmVarDecl>(var) &&
                isPointerType(var->getType()))
              return addBase(ptr, var, loc);
            if (!isPointerType(var->getType()))
              return addBase(ptr, var, loc);
          }
      }
      // `p = &(struct S){...}` (C99-13): the address of a block-scope
      // compound literal binds its synthesized backing declaration as a
      // (degenerate, for struct/scalar types) local region base.
      if (const auto *compound =
              llvm::dyn_cast<clang::CompoundLiteralExpr>(sub))
        if (literalTemps && !compound->isFileScope())
          return addBase(ptr, literalTemps->getOrCreate(*context, compound),
                         loc);
      return markInvalid(ptr, loc,
                         "unsupported: pointer assigned a non-address value");
    }
    if (unary->isIncrementDecrementOp()) {
      // `p = q++` and friends: cursor arithmetic on the copied pointer.
      if (const clang::VarDecl *source = asLocalVarRef(unary->getSubExpr()))
        if (tracks(source)) {
          recordArithmetic(source, loc);
          return unite(ptr, source);
        }
      return markInvalid(ptr, loc,
                         "unsupported: pointer assigned a non-address value");
    }
  }

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    clang::BinaryOperatorKind opcode = binary->getOpcode();
    if (opcode == clang::BO_Add || opcode == clang::BO_Sub) {
      // `p = q + n` / `p = q - n`: arithmetic on the pointer operand.
      const clang::Expr *pointerSide =
          isPointerType(binary->getLHS()->getType()) ? binary->getLHS()
                                                     : binary->getRHS();
      recordArithmetic(ptr, loc);
      return recordPointerWrite(ptr, pointerSide);
    }
  }

  markInvalid(ptr, loc,
              "unsupported: pointer assigned a non-address value");
}

void PointerRegionAnalysis::visit(const clang::Stmt *stmt) {
  if (!stmt)
    return;

  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    // Function pointers are ordinary Copy values, not decomposed pointers;
    // the analysis never tracks them. Second-order pointers (`T **pp`)
    // track separately: their targets are cursor cells, not objects.
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if (var->hasLocalStorage() && !llvm::isa<clang::ParmVarDecl>(var) &&
            isPointerType(var->getType()) &&
            !isFunctionPointer(var->getType())) {
          // An admitted `void *` fn-ptr holder (CTS-F, 00210) imports as
          // an ordinary fn_ptr local; the decomposition never tracks it.
          if (fnHolderQuery && fnHolderQuery(var))
            continue;
          // A FILE* handle local is an owned stream handle (C99-48), not
          // a decomposed pointer; the emission intercepts every use.
          // Non-handle FILE* locals (`FILE *g = stdout;`) keep the
          // historical tracking and its rejections.
          if (isFileHandleLocal(var))
            continue;
          if (isSecondOrderPointerType(var->getType())) {
            secondOrderVars.insert(var);
            if (const clang::Expr *init = var->getInit())
              recordSecondOrderWrite(var, init);
            continue;
          }
          pointerVars.insert(var);
          if (const clang::Expr *init = var->getInit())
            recordPointerWrite(var, init);
        }
        // A struct local whose initializer list covers data-pointer
        // fields establishes their per-instance member bindings here
        // (static locals bind through the constant-initializer walk at
        // their global import instead). A compound-literal initializer
        // (`struct S s = (struct S){...}`, C99-13) initializes the
        // variable directly, so its list binds the same way.
        if (var->hasLocalStorage() && !llvm::isa<clang::ParmVarDecl>(var)) {
          const clang::Expr *init = var->getInit();
          if (init)
            if (const auto *compound = llvm::dyn_cast<
                    clang::CompoundLiteralExpr>(init->IgnoreParenImpCasts()))
              init = compound->getInitializer();
          if (const auto *list =
                  llvm::dyn_cast_if_present<clang::InitListExpr>(init))
            if (recordOfType(var->getType()))
              collectStructInitBindings(var, list);
        }
      }
  } else if (const auto *compound =
                 llvm::dyn_cast<clang::CompoundAssignOperator>(stmt)) {
    // `p += n` / `p -= n` walk the pointer without rebinding it;
    // `*p += n` / `p[i] -= n` write through the pointer. A global pointer
    // (tracked on first mention) walks its stored global cursor.
    if (isPointerType(compound->getLHS()->getType())) {
      if (const clang::VarDecl *var = asLocalVarRef(compound->getLHS())) {
        if (tracks(var))
          recordArithmetic(var, compound->getOperatorLoc());
      } else if (const clang::VarDecl *global =
                     asGlobalDataPointerRef(compound->getLHS())) {
        pointerVars.insert(global);
        recordArithmetic(global, compound->getOperatorLoc());
      } else if (const clang::FieldDecl *field =
                     dataPointerFieldOf(compound->getLHS())) {
        // `s.f += n`: a walked member needs runtime cursor state, which
        // the degenerate member model does not carry.
        poisonMemberField(field, compound->getOperatorLoc());
      }
    } else if (const clang::VarDecl *var =
                   trackedWritePlaceRoot(compound->getLHS())) {
      recordWriteThrough(var, compound->getOperatorLoc());
    }
  } else if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
    if (binary->getOpcode() == clang::BO_Assign) {
      if (isPointerType(binary->getLHS()->getType())) {
        if (const clang::VarDecl *var = asLocalVarRef(binary->getLHS())) {
          if (tracks(var))
            recordPointerWrite(var, binary->getRHS());
          else if (secondOrderVars.contains(var))
            recordSecondOrderWrite(var, binary->getRHS());
        } else if (const clang::VarDecl *global =
                       asGlobalDataPointerRef(binary->getLHS())) {
          pointerVars.insert(global);
          recordPointerWrite(global, binary->getRHS());
        } else if (const clang::VarDecl *pp =
                       asSecondOrderDeref(binary->getLHS())) {
          // `*pp = rhs` re-points the selected first-order pointer: the
          // write lands on the target's region (CTS-P5). A dereference
          // before any binding has no target to forward to.
          auto it = secondOrderRegions.find(pp);
          if (it != secondOrderRegions.end() && it->second.target)
            recordPointerWrite(it->second.target, binary->getRHS());
          else
            markSecondOrderInvalid(pp, binary->getOperatorLoc(),
                                   "unsupported: dereference of a "
                                   "pointer-to-pointer variable before it "
                                   "is bound");
        } else if (const clang::ParmVarDecl *cursorParam =
                       cursorParamQuery
                           ? asPointerPointerParamDeref(binary->getLHS())
                           : nullptr;
                   cursorParam && cursorParamQuery(cursorParam)) {
          // `*s = rhs` advances a planned string-cursor parameter (CTS
          // 00204): the parameter joins the analysis as a pointer of the
          // region it also bases, so the right-hand side's facts (its
          // arithmetic, its source pointer) land on that region.
          pointerVars.insert(cursorParam);
          recordPointerWrite(cursorParam, binary->getRHS());
        } else if (const clang::FieldDecl *field =
                       dataPointerFieldOf(binary->getLHS())) {
          // `s.f = rhs` on a directly named instance binds the member;
          // a write through any other place (an arrow, a subscripted
          // element, a nested member) has an instance path outside the
          // model and poisons the field program-wide.
          const auto *member = llvm::cast<clang::MemberExpr>(
              stripTrivia(binary->getLHS()));
          const clang::VarDecl *instance = nullptr;
          if (!member->isArrow())
            if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
                    stripTrivia(member->getBase())))
              instance = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
          if (instance)
            recordMemberPointerWrite(instance, field, binary->getRHS());
          else
            poisonMemberField(field, binary->getOperatorLoc());
        }
      } else if (const clang::VarDecl *var =
                     trackedWritePlaceRoot(binary->getLHS())) {
        // `*p = v` / `p[i] = v`: a write through the region's pointers.
        recordWriteThrough(var, binary->getOperatorLoc());
      } else if (const clang::RecordDecl *record =
                     recordOfType(binary->getLHS()->getType())) {
        // A whole-struct overwrite replaces any data-pointer members
        // wholesale; no static per-instance binding survives it.
        if (recordHasDataPointerField(record))
          poisonRecordPointerFields(record, binary->getOperatorLoc());
      }
    }
  } else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
    if (unary->isIncrementDecrementOp() &&
        isPointerType(unary->getSubExpr()->getType())) {
      if (const clang::VarDecl *var = asLocalVarRef(unary->getSubExpr())) {
        if (tracks(var))
          recordArithmetic(var, unary->getOperatorLoc());
      } else if (const clang::VarDecl *global =
                     asGlobalDataPointerRef(unary->getSubExpr())) {
        pointerVars.insert(global);
        recordArithmetic(global, unary->getOperatorLoc());
      } else if (const clang::FieldDecl *field =
                     dataPointerFieldOf(unary->getSubExpr())) {
        // `s.f++`: a walked member needs runtime cursor state.
        poisonMemberField(field, unary->getOperatorLoc());
      }
    } else if (unary->isIncrementDecrementOp()) {
      // `(*p)++` / `--p[i]`: a write through the region's pointers.
      if (const clang::VarDecl *var =
              trackedWritePlaceRoot(unary->getSubExpr()))
        recordWriteThrough(var, unary->getOperatorLoc());
    } else if (unary->getOpcode() == clang::UO_AddrOf) {
      // `&p` outside a second-order binding (`pp = &p` consumes its
      // operand) would let the pointer escape the decomposition; a global
      // pointer's address escaping is the same shape.
      if (const clang::VarDecl *var = asLocalVarRef(unary->getSubExpr())) {
        if (tracks(var) && !consumedAddrOf.contains(unary))
          markInvalid(var, unary->getOperatorLoc(),
                      "unsupported: taking the address of a pointer variable");
        else if (secondOrderVars.contains(var))
          markSecondOrderInvalid(var, unary->getOperatorLoc(),
                                 "unsupported: taking the address of a "
                                 "pointer-to-pointer variable");
      } else if (const clang::VarDecl *global =
                     asGlobalDataPointerRef(unary->getSubExpr())) {
        pointerVars.insert(global);
        markInvalid(global, unary->getOperatorLoc(),
                    "unsupported: taking the address of a pointer variable");
      } else if (const clang::FieldDecl *field =
                     dataPointerFieldOf(unary->getSubExpr())) {
        // `&s.f` lets the member escape the static-binding model.
        poisonMemberField(field, unary->getOperatorLoc());
      }
    }
  } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    // A `&p` argument feeding a planned string-cursor parameter (CTS
    // 00204) is consumed by the call lowering — the callee receives the
    // region and an in-out cursor — so it must not invalidate `p`'s
    // region through the generic address-of handler below; the callee's
    // advancement of the cursor is ordinary pointer arithmetic.
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee && cursorArgQuery) {
      for (unsigned index = 0, count = call->getNumArgs(); index < count;
           ++index) {
        if (!cursorArgQuery(callee, index))
          continue;
        const clang::Expr *argument = stripTrivia(call->getArg(index));
        while (const auto *cast =
                   llvm::dyn_cast<clang::ImplicitCastExpr>(argument))
          argument = stripTrivia(cast->getSubExpr());
        const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(argument);
        if (!addrOf || addrOf->getOpcode() != clang::UO_AddrOf)
          continue;
        const clang::VarDecl *pointer = asLocalVarRef(addrOf->getSubExpr());
        if (!pointer || !tracks(pointer))
          continue;
        consumedAddrOf.insert(addrOf);
        recordArithmetic(pointer, addrOf->getOperatorLoc());
      }
    }
  }
  // Pointer call arguments no longer invalidate the region (Phase 1b):
  // `emitCall` reborrows the region base per target-parameter kind
  // (emitrust.slice_of / emitrust.addr_of), so no raw pointer ever escapes.

  for (const clang::Stmt *child : stmt->children())
    visit(child);
}

//===----------------------------------------------------------------------===//
// Owner planning (Phase-4 Pass A)
//===----------------------------------------------------------------------===//

/// Collects every call expression below `stmt`, in source order.
static void collectCallExprs(const clang::Stmt *stmt,
                             SmallVectorImpl<const clang::CallExpr *> &calls) {
  if (!stmt)
    return;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
    calls.push_back(call);
  for (const clang::Stmt *child : stmt->children())
    collectCallExprs(child, calls);
}

const clang::VarDecl *
CImporter::resolveArgRoot(PointerRegionAnalysis &regions,
                          const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  if (e->isNullPointerConstant(astContext(),
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return nullptr;

  // A read (or ++/--) of a pointer variable: a parameter is its own root,
  // and a pointer local resolves through its single-base region.
  auto rootOfVarRef = [&](const clang::VarDecl *var) -> const clang::VarDecl * {
    if (!var)
      return nullptr;
    if (llvm::isa<clang::ParmVarDecl>(var))
      return isPointerType(var->getType()) ? var : nullptr;
    if (regions.tracks(var)) {
      const PointerRegion *region = regions.regionOf(var);
      // Nullable regions are excluded: owner methods pass bare i64
      // cursors, which cannot carry the Option-of-cursor discriminant, so
      // such regions keep the Phase-1b lowering (where a possibly-null
      // argument is a located rejection).
      if (region && region->invalidReason.empty() &&
          region->bases.size() == 1 && !region->nullable)
        return region->bases.front().base;
    }
    return nullptr;
  };

  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    switch (cast->getCastKind()) {
    case clang::CK_NoOp:
      return resolveArgRoot(regions, cast->getSubExpr());
    case clang::CK_LValueToRValue:
      return rootOfVarRef(asVarRef(cast->getSubExpr()));
    case clang::CK_ArrayToPointerDecay: {
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub))
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          if (var->hasLocalStorage())
            return var;
      return nullptr;
    }
    default:
      return nullptr;
    }
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
    if (unary->getOpcode() == clang::UO_AddrOf) {
      const clang::Expr *sub = stripTrivia(unary->getSubExpr());
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
        // `&x` roots at the (non-pointer) object itself.
        const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
        if (var && var->hasLocalStorage() && !isPointerType(var->getType()))
          return var;
        return nullptr;
      }
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(sub))
        return resolveArgRoot(regions, subscript->getBase());
      return nullptr;
    }
    if (unary->isIncrementDecrementOp())
      return rootOfVarRef(asVarRef(unary->getSubExpr()));
    return nullptr;
  }
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (binary->getOpcode() == clang::BO_Add ||
        binary->getOpcode() == clang::BO_Sub) {
      const clang::Expr *pointerSide =
          isPointerType(binary->getLHS()->getType()) ? binary->getLHS()
                                                     : binary->getRHS();
      return resolveArgRoot(regions, pointerSide);
    }
  }
  return nullptr;
}

void CImporter::mergeMemberPointerFacts(const MemberPointerKey &key,
                                        const MemberPointerFacts &incoming) {
  MemberPointerFacts &target = memberPtrBindings[key];
  if (!target.invalidReason.empty())
    return; // First invalidation wins.
  if (!incoming.invalidReason.empty()) {
    target.invalidReason = incoming.invalidReason;
    target.invalidLoc = incoming.invalidLoc;
    target.secondLoc = incoming.secondLoc;
    if (target.loc.isInvalid())
      target.loc = incoming.loc;
    return;
  }
  if (!incoming.base && !incoming.literal)
    return;
  if (!target.base && !target.literal) {
    target.base = incoming.base;
    target.literal = incoming.literal;
    target.loc = incoming.loc;
    return;
  }
  if (target.base == incoming.base && target.literal == incoming.literal)
    return; // Idempotent: the same target bound from two sites.
  target.invalidReason =
      ("unsupported: pointer struct member '" + key.second->getName() +
       "' bound to two different targets")
          .str();
  target.invalidLoc = incoming.loc;
  target.secondLoc = incoming.loc;
}

void CImporter::collectGlobalMemberBindings(const clang::VarDecl *instance,
                                            const clang::APValue &value,
                                            clang::QualType type,
                                            clang::SourceLocation loc) {
  if (isDataPointer(type)) {
    // The instance walk only reaches this case for struct fields (the
    // top-level object of a data-pointer type imports through
    // `importPointerGlobal` instead), but the recursion itself is
    // type-directed and total.
    return;
  }
  if (const clang::ArrayType *array = astContext().getAsArrayType(type)) {
    if (!value.isArray())
      return;
    for (unsigned i = 0, n = value.getArrayInitializedElts(); i != n; ++i)
      collectGlobalMemberBindings(instance, value.getArrayInitializedElt(i),
                                  array->getElementType(), loc);
    if (value.hasArrayFiller())
      collectGlobalMemberBindings(instance, value.getArrayFiller(),
                                  array->getElementType(), loc);
    return;
  }
  const clang::RecordDecl *record = recordOfType(type);
  if (!record || !value.isStruct())
    return;
  unsigned index = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    if (index >= value.getStructNumFields())
      break;
    const clang::APValue &fieldValue = value.getStructField(index++);
    if (!isDataPointer(field->getType())) {
      collectGlobalMemberBindings(instance, fieldValue, field->getType(),
                                  loc);
      continue;
    }
    MemberPointerFacts incoming;
    incoming.loc = loc;
    if (fieldValue.isNullPointer() || !fieldValue.isLValue())
      continue; // Null (or absent): the member stays unbound.
    clang::APValue::LValueBase lvalueBase = fieldValue.getLValueBase();
    const auto *baseVar = llvm::dyn_cast_if_present<clang::VarDecl>(
        lvalueBase.dyn_cast<const clang::ValueDecl *>());
    const auto *literal = llvm::dyn_cast_if_present<clang::StringLiteral>(
        lvalueBase.dyn_cast<const clang::Expr *>());
    bool atStart = fieldValue.getLValueOffset().isZero();
    clang::QualType pointee =
        field->getType().getCanonicalType()->getPointeeType();
    if (baseVar && !baseVar->hasLocalStorage() && atStart &&
        astContext().hasSameUnqualifiedType(pointee, baseVar->getType())) {
      incoming.base = baseVar->getCanonicalDecl();
    } else if (literal && atStart) {
      incoming.literal = literal;
    } else {
      incoming.invalidReason =
          ("unsupported: pointer struct member '" + field->getName() +
           "' initializer")
              .str();
      incoming.invalidLoc = loc;
    }
    mergeMemberPointerFacts({instance->getCanonicalDecl(), field}, incoming);
  }
}

FailureOr<const MemberPointerFacts *>
CImporter::resolveMemberPointerBinding(const clang::MemberExpr *member,
                                       Location loc) {
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field) // Defensive; callers pre-check the field.
    return emitError(loc) << "unsupported member access";
  auto poisoned = poisonedPtrFields.find(field);
  if (poisoned != poisonedPtrFields.end()) {
    InFlightDiagnostic diag =
        emitError(loc) << "unsupported: pointer struct member '"
                       << field->getName()
                       << "' is used outside the static-binding model";
    diag.attachNote(translateLoc(poisoned->second))
        << "first unresolvable use is here";
    return diag;
  }
  // Resolve the struct instance: the directly named base variable of a
  // dot access, or the single degenerate object behind a decomposed
  // arrow base (a `&s`-bound pointer, a whole-object pointer global).
  const clang::VarDecl *instance = nullptr;
  if (!member->isArrow()) {
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            stripTrivia(member->getBase())))
      instance = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  } else {
    FailureOr<PtrExprValue> pointer = emitPointerRValue(member->getBase());
    if (failed(pointer))
      return failure();
    if (pointer->nonNull)
      // Mirror `emitPointerPlace`'s null guard: dereferencing null is UB
      // in C, so the deterministic panic is a legal refinement.
      builder.create<emitrust::CallOpaqueOp>(
          loc, TypeRange(), builder.getStringAttr("assert!"),
          builder.getArrayAttr(
              {builder.getIndexAttr(0),
               builder.getStringAttr("null pointer dereference")}),
          ValueRange{pointer->nonNull});
    if (pointer->base && !pointer->cursor && !pointer->literalBacking)
      instance = pointer->base;
  }
  if (!instance)
    return emitError(loc)
           << "unsupported: pointer struct member '" << field->getName()
           << "' of an unresolvable struct instance";
  if (!instance->hasLocalStorage()) {
    instance = instance->getCanonicalDecl();
    // In a multi-file project another TU could rebind the member of an
    // externally visible global instance behind this TU's facts.
    if (!currentSoleTU && instance->isExternallyVisible())
      return emitError(loc)
             << "unsupported: pointer struct member of an externally "
                "visible global in a multi-file project";
  }
  auto it = memberPtrBindings.find({instance, field});
  if (it == memberPtrBindings.end() ||
      (it->second.invalidReason.empty() && !it->second.base &&
       !it->second.literal))
    return emitError(loc)
           << "unsupported: pointer struct member '" << field->getName()
           << "' has no known target object";
  const MemberPointerFacts &facts = it->second;
  if (!facts.invalidReason.empty()) {
    InFlightDiagnostic diag = emitError(loc) << facts.invalidReason;
    if (facts.loc.isValid())
      diag.attachNote(translateLoc(facts.loc)) << "first bound here";
    if (facts.secondLoc.isValid())
      diag.attachNote(translateLoc(facts.secondLoc))
          << "conflicting binding here";
    else if (facts.invalidLoc.isValid())
      diag.attachNote(translateLoc(facts.invalidLoc))
          << "unsupported construct here";
    return diag;
  }
  if (facts.base && facts.base->hasLocalStorage() &&
      !symbols.contains(facts.base))
    return emitError(loc)
           << "unsupported: pointer struct member '" << field->getName()
           << "' bound to a local object of another function";
  return &facts;
}

LogicalResult CImporter::emitMemberPointerAssign(
    const clang::MemberExpr *member, const clang::Expr *rhs, Location loc) {
  FailureOr<const MemberPointerFacts *> binding =
      resolveMemberPointerBinding(member, loc);
  if (failed(binding))
    return failure();
  const MemberPointerFacts &facts = **binding;
  if (facts.literal) {
    // A literal rebinding is idempotent by the analysis (a second literal
    // clashes into an invalid fact), and a literal decay has no side
    // effects: nothing to emit.
    if (llvm::isa<clang::StringLiteral>(
            stripTrivia(rhs)->IgnoreParenImpCasts()))
      return success();
    return emitError(loc)
           << "unsupported: pointer struct member assigned this value";
  }
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  if (value->base != facts.base || value->member || value->cursor ||
      value->literalBacking)
    return emitError(loc) // Defensive; the analysis pins the binding.
           << "unsupported: pointer struct member assigned this value";
  // The degenerate binding is static: the stored i64 member stays 0 and
  // the write needs no runtime code.
  return success();
}

namespace {
/// Union-find over storage/parameter declarations, the shared
/// interprocedural machinery of the Pass-A planners (`planOwners`,
/// `planCellSlices`): nodes register on first touch, `find` compresses
/// paths, and `unite` links roots. Union-find transitively closes as
/// edges are added, so one walk over every body reaches the fixpoint.
/// `nodes` snapshots the touched set for an aggregation that keeps
/// calling `find` (which compresses the underlying map).
class VarDeclUnionFind {
public:
  /// Returns `decl`'s class root, registering an unseen node as its own
  /// root and compressing the path walked.
  const clang::VarDecl *find(const clang::VarDecl *decl) {
    parent.try_emplace(decl, decl);
    const clang::VarDecl *root = decl;
    while (parent[root] != root)
      root = parent[root];
    while (parent[decl] != root) {
      const clang::VarDecl *next = parent[decl];
      parent[decl] = root;
      decl = next;
    }
    return root;
  }

  /// Unites `b`'s class into `a`'s.
  void unite(const clang::VarDecl *a, const clang::VarDecl *b) {
    const clang::VarDecl *rootB = find(b);
    const clang::VarDecl *rootA = find(a);
    parent[rootB] = rootA;
  }

  /// Snapshots every node ever touched.
  SmallVector<const clang::VarDecl *> nodes() const {
    SmallVector<const clang::VarDecl *> result;
    result.reserve(parent.size());
    for (const auto &entry : parent)
      result.push_back(entry.first);
    return result;
  }

private:
  llvm::DenseMap<const clang::VarDecl *, const clang::VarDecl *> parent;
};
} // namespace

/// Walks every call in `body` whose callee resolves to a non-variadic
/// definition of matching arity and invokes `visit` once per data-pointer
/// (non-function-pointer) callee parameter with the argument bound to it —
/// the shared call-edge scaffold of the Pass-A planners. Callees without
/// a definition in this TU add no edge: passing a region to them stays on
/// the Phase-1b call lowering.
static void forEachDataPointerCallArg(
    const clang::Stmt *body,
    llvm::function_ref<void(const clang::ParmVarDecl *, const clang::Expr *)>
        visit) {
  SmallVector<const clang::CallExpr *> calls;
  collectCallExprs(body, calls);
  for (const clang::CallExpr *call : calls) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (!callee || callee->isVariadic())
      continue;
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || !definition->hasBody() ||
        call->getNumArgs() != definition->getNumParams())
      continue;
    for (auto [index, argument] : llvm::enumerate(call->arguments())) {
      const clang::ParmVarDecl *param = definition->getParamDecl(index);
      if (!isPointerType(param->getType()) ||
          isFunctionPointer(param->getType()))
        continue;
      visit(param, argument);
    }
  }
}

SmallVector<const clang::FunctionDecl *>
CImporter::collectPassAFunctionDefinitions(
    const clang::TranslationUnitDecl *unit) const {
  SmallVector<const clang::FunctionDecl *> definitions;
  for (const clang::Decl *decl : unit->decls())
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl))
      if (func->doesThisDeclarationHaveABody() && !func->isVariadic() &&
          !isSystemHeaderDecl(func))
        definitions.push_back(func);
  return definitions;
}

void CImporter::planOwners(const clang::TranslationUnitDecl *unit,
                           bool soleTranslationUnit) {
  // Interprocedural union-find over storage bases (local arrays and
  // scalars) and the data-pointer parameters of function definitions.
  VarDeclUnionFind unionFind;
  // Declarations whose class must not be promoted: unresolvable pointer
  // arguments, pointer-to-pointer parameters, invalidated (escaping)
  // regions. Membership is checked per node during aggregation, so a
  // poison mark survives later unions.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> poisoned;

  for (const clang::FunctionDecl *func :
       collectPassAFunctionDefinitions(unit)) {
    PointerRegionAnalysis analysis;
    analysis.literalTemps = &literalTemps;
    analysis.analyze(astContext(), func->getBody());

    // Data-pointer parameters are class nodes; a pointer-to-pointer
    // parameter poisons its class (it has no i64-index representation).
    for (const clang::ParmVarDecl *param : func->parameters()) {
      if (!isPointerType(param->getType()) ||
          isFunctionPointer(param->getType()))
        continue;
      (void)unionFind.find(param);
      if (param->getType()
              .getCanonicalType()
              ->getPointeeType()
              .getCanonicalType()
              ->isPointerType())
        poisoned.insert(param);
    }

    // Project each per-function region into the global union-find: all
    // bases of one region share a class, and an invalidated (escaping)
    // region poisons them.
    for (const clang::VarDecl *var : analysis.trackedVars()) {
      const PointerRegion *region = analysis.regionOf(var);
      if (!region)
        continue;
      const clang::VarDecl *first = nullptr;
      for (const PointerBaseBinding &binding : region->bases) {
        if (!first)
          first = binding.base;
        else
          unionFind.unite(first, binding.base);
        if (!region->invalidReason.empty())
          poisoned.insert(binding.base);
        // A compound-literal backing (C99-13) has no declaration
        // statement to anchor an owner struct at; any class containing
        // one stays on the Phase-1b slice lowering.
        if (literalTemps.isTemp(binding.base))
          poisoned.insert(binding.base);
      }
    }

    // Program-wide facts of pointer-typed globals (CTS-P4): merge this
    // body's region view of every tracked global pointer;
    // `importPointerGlobal` validates the union when the global itself is
    // imported (Pass B).
    for (const clang::VarDecl *var : analysis.trackedVars())
      if (!var->hasLocalStorage())
        if (const PointerRegion *region = analysis.regionOf(var))
          mergeRegionFacts(globalPtrFacts[var->getCanonicalDecl()], *region);

    // Program-wide member-pointer facts (CTS-P2): every body's bindings
    // and unresolvable field uses merge here; reads and writes of
    // data-pointer members consult the union at their use sites.
    for (const auto &entry : analysis.memberBindings())
      mergeMemberPointerFacts(entry.first, entry.second);
    for (const auto &entry : analysis.poisonedMemberFields())
      poisonedPtrFields.try_emplace(entry.first, entry.second);

    // Call edges: a pointer argument's root object unifies with the callee
    // definition's parameter; an unresolvable root poisons the parameter's
    // class. Callees without a definition in this TU add no edge — passing
    // a region to them stays on the Phase-1b call lowering, which composes
    // with a promoted base through its rewritten data place.
    forEachDataPointerCallArg(
        func->getBody(),
        [&](const clang::ParmVarDecl *param, const clang::Expr *argument) {
          if (const clang::VarDecl *root = resolveArgRoot(analysis, argument))
            unionFind.unite(root, param);
          else
            poisoned.insert(param);
        });
  }

  // Aggregate the classes. `find` compresses paths, so the node set is
  // snapshotted before aggregation.
  struct ClassInfo {
    SmallVector<const clang::VarDecl *, 2> storageBases;
    SmallVector<const clang::ParmVarDecl *, 4> params;
    bool poisoned = false;
  };
  llvm::DenseMap<const clang::VarDecl *, ClassInfo> classes;
  for (const clang::VarDecl *node : unionFind.nodes()) {
    ClassInfo &info = classes[unionFind.find(node)];
    if (poisoned.contains(node))
      info.poisoned = true;
    if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(node)) {
      if (isPointerType(param->getType()) &&
          !isFunctionPointer(param->getType()))
        info.params.push_back(param);
      continue;
    }
    if (!isPointerType(node->getType()) && node->hasLocalStorage())
      info.storageBases.push_back(node);
  }

  // Promote every class that satisfies the full rule; anything else is a
  // silent Phase-1b fallback.
  for (const auto &entry : classes) {
    const ClassInfo &info = entry.second;
    if (info.poisoned || info.params.empty() ||
        info.storageBases.size() != 1)
      continue;
    const clang::VarDecl *base = info.storageBases.front();
    const auto *owner = llvm::dyn_cast_if_present<clang::FunctionDecl>(
        base->getParentFunctionOrMethod());
    if (!owner)
      continue;
    const clang::ConstantArrayType *arrayType =
        astContext().getAsConstantArrayType(base->getType());
    // Local array of 1..32 elements: the struct_def `Default` derive MVP
    // limit. Scalar and oversized bases keep the Phase-1b lowering.
    if (!arrayType || arrayType->getSize().getZExtValue() == 0 ||
        arrayType->getSize().getZExtValue() > 32)
      continue;
    clang::QualType element = arrayType->getElementType();

    // Every unified parameter must belong to a defined function and point
    // at the base's element type.
    llvm::SmallPtrSet<const clang::FunctionDecl *, 4> methodFns;
    bool qualifies = true;
    for (const clang::ParmVarDecl *param : info.params) {
      const auto *fn =
          llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
      if (!fn || !fn->doesThisDeclarationHaveABody() ||
          !astContext().hasSameUnqualifiedType(
              element,
              param->getType().getCanonicalType()->getPointeeType())) {
        qualifies = false;
        break;
      }
      methodFns.insert(fn);
    }
    if (!qualifies)
      continue;
    for (const clang::FunctionDecl *fn : methodFns) {
      // All-or-nothing per function: every data-pointer parameter of the
      // function must resolve into this one class, the return type must be
      // a plain value, the function may not be the owner itself or C
      // `main`, and all of its call sites must be visible — an externally
      // visible function qualifies only when this TU is the whole program.
      if (fn == owner || fn->getName() == "main" ||
          (fn->isExternallyVisible() && !soleTranslationUnit) ||
          (isPointerType(fn->getReturnType()) &&
           !isFunctionPointer(fn->getReturnType()))) {
        qualifies = false;
        break;
      }
      for (const clang::ParmVarDecl *param : fn->parameters()) {
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()) &&
            unionFind.find(param) != entry.first) {
          qualifies = false;
          break;
        }
      }
      if (!qualifies)
        break;
    }
    if (!qualifies)
      continue;

    // The owner struct is named after the C spellings (`main`, not the
    // renamed `c_main`); an internal-linkage owning function takes the
    // per-TU tag so identically named statics never collide.
    std::string structName =
        (llvm::Twine("Owner_") +
         (owner->isExternallyVisible() ? "" : currentTuTag.c_str()) +
         owner->getName() + "_" + base->getName())
            .str();
    ownerPlans[base] = OwnerPlan{structName, /*structDefCreated=*/false};
    for (const clang::FunctionDecl *fn : methodFns)
      methodPlans[fn->getCanonicalDecl()] = base;
  }
}

//===----------------------------------------------------------------------===//
// Cell-slice planning (CTS-P10 Pass A)
//===----------------------------------------------------------------------===//

namespace {

/// Recursive body walk collecting the per-parameter facts the cell-slice
/// qualification needs: which data-pointer parameters are null-checked
/// (compared against a null pointer constant, logically negated, or truth
/// tested as a statement condition) and which are used in any shape other
/// than a subscript base, a dereference base, a null check, or an argument
/// to a defined non-variadic callee (such "escaping" uses poison the
/// parameter's class — the cell-slice emission only models the whitelisted
/// shapes). The walk is top-down: a consuming context skips the consumed
/// parameter read, so any parameter read reached raw is an escape.
class CellSliceBodyScan {
public:
  CellSliceBodyScan(
      clang::ASTContext &context,
      llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &nullChecked,
      llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &poisoned)
      : context(context), nullChecked(nullChecked), poisoned(poisoned) {}

  /// Walks `stmt` and its children.
  void visit(const clang::Stmt *stmt) {
    if (!stmt)
      return;

    // Statement conditions truth-test a directly named pointer parameter
    // (`if (p)`, `while (p)`): a null check.
    if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt))
      markConditionNullCheck(ifStmt->getCond());
    else if (const auto *whileStmt = llvm::dyn_cast<clang::WhileStmt>(stmt))
      markConditionNullCheck(whileStmt->getCond());
    else if (const auto *doStmt = llvm::dyn_cast<clang::DoStmt>(stmt))
      markConditionNullCheck(doStmt->getCond());
    else if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
      markConditionNullCheck(forStmt->getCond());

    if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt)) {
      visitExpr(expr);
      return;
    }
    for (const clang::Stmt *child : stmt->children())
      visit(child);
  }

private:
  /// Returns the data-pointer parameter `expr` reads, or null.
  const clang::ParmVarDecl *asParamRead(const clang::Expr *expr) const {
    if (!expr)
      return nullptr;
    const clang::Expr *e = stripTrivia(expr);
    while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (cast->getCastKind() != clang::CK_LValueToRValue &&
          cast->getCastKind() != clang::CK_NoOp)
        break;
      e = stripTrivia(cast->getSubExpr());
    }
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
    const auto *param =
        ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
    if (param && isPointerType(param->getType()) &&
        !isFunctionPointer(param->getType()))
      return param;
    return nullptr;
  }

  /// Returns whether `expr` is a null pointer constant.
  bool isNullConstant(const clang::Expr *expr) const {
    return expr->isNullPointerConstant(
               context, clang::Expr::NPC_NeverValueDependent) !=
           clang::Expr::NPCK_NotNull;
  }

  /// Marks a statement condition that is a bare parameter read (a truth
  /// test) as a null check and lets the regular walk handle the rest.
  void markConditionNullCheck(const clang::Expr *cond) {
    if (const clang::ParmVarDecl *param = asParamRead(cond)) {
      nullChecked.insert(param);
      conditionTested.insert(param);
    }
  }

  /// Expression walk with consuming-context dispatch.
  void visitExpr(const clang::Expr *expr) {
    const clang::Expr *e = stripTrivia(expr);
    // A subscript through a parameter consumes the base read.
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
      if (asParamRead(subscript->getBase())) {
        visitExpr(subscript->getIdx());
        return;
      }
    }
    // A dereference of a parameter consumes the read; `!p` is a null
    // check.
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      const clang::ParmVarDecl *param = asParamRead(unary->getSubExpr());
      if (param && unary->getOpcode() == clang::UO_Deref)
        return;
      if (param && unary->getOpcode() == clang::UO_LNot) {
        nullChecked.insert(param);
        return;
      }
    }
    // `p == NULL` / `p != NULL` is a null check; comparisons against
    // anything else fall through to the raw-read poison below.
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (binary->getOpcode() == clang::BO_EQ ||
          binary->getOpcode() == clang::BO_NE) {
        const clang::ParmVarDecl *lhsParam = asParamRead(binary->getLHS());
        const clang::ParmVarDecl *rhsParam = asParamRead(binary->getRHS());
        if (lhsParam && isNullConstant(binary->getRHS())) {
          nullChecked.insert(lhsParam);
          return;
        }
        if (rhsParam && isNullConstant(binary->getLHS())) {
          nullChecked.insert(rhsParam);
          return;
        }
      }
    }
    // A defined non-variadic direct callee consumes its parameter-read
    // arguments (the call-edge scan classifies them); any other call
    // escapes them.
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      const clang::FunctionDecl *definition =
          callee ? callee->getDefinition() : nullptr;
      bool visible = callee && !callee->isVariadic() && definition &&
                     definition->hasBody() &&
                     call->getNumArgs() == definition->getNumParams();
      for (const clang::Expr *argument : call->arguments()) {
        if (const clang::ParmVarDecl *param = asParamRead(argument)) {
          if (!visible)
            poisoned.insert(param);
          continue;
        }
        visitExpr(argument);
      }
      return;
    }
    // A raw parameter read outside every whitelisted context escapes the
    // class (walks, reassignments, copies, differences, address-taking,
    // returns, ...). A bare read already recorded as a statement-condition
    // truth test is consumed.
    if (const clang::ParmVarDecl *param = asParamRead(e)) {
      if (!conditionTested.contains(param))
        poisoned.insert(param);
      return;
    }
    // The address-of or ++/-- of a parameter never reads it
    // (no LValueToRValue), so catch the raw DeclRefExpr too.
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      if (const auto *param =
              llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()))
          poisoned.insert(param);
      return;
    }
    for (const clang::Stmt *child : e->children())
      visit(child);
  }

  clang::ASTContext &context;
  llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &nullChecked;
  llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &poisoned;
  /// Parameters truth-tested as a whole statement condition: their bare
  /// read is consumed, not an escape.
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> conditionTested;
};

} // namespace

const clang::VarDecl *
CImporter::asDecayedGlobalArrayArg(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *noop = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (noop->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(noop->getSubExpr());
  }
  const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
  if (!decay || decay->getCastKind() != clang::CK_ArrayToPointerDecay)
    return nullptr;
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(decay->getSubExpr()));
  const auto *var = ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl())
                        : nullptr;
  if (!var || var->hasLocalStorage() ||
      !astContext().getAsConstantArrayType(var->getType()))
    return nullptr;
  return var->getCanonicalDecl();
}

const clang::ParmVarDecl *
CImporter::asPointerParamRead(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        cast->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *param =
      ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
  if (param && isPointerType(param->getType()) &&
      !isFunctionPointer(param->getType()))
    return param;
  return nullptr;
}

void CImporter::planCellSlices(const clang::TranslationUnitDecl *unit,
                               bool soleTranslationUnit) {
  // Union-find over data-pointer parameters and global array bases,
  // mirroring `planOwners`' machinery (see `VarDeclUnionFind` for the
  // fixpoint argument).
  VarDeclUnionFind unionFind;

  llvm::SmallPtrSet<const clang::VarDecl *, 8> poisoned;
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 8> nullChecked;
  // The first local object a class's parameter also received (the Mixed
  // boundary), keyed by the callee parameter that received it.
  llvm::DenseMap<const clang::VarDecl *, std::string> localJoins;

  for (const clang::FunctionDecl *func :
       collectPassAFunctionDefinitions(unit)) {
    // Body facts: null checks and escaping uses of this definition's own
    // data-pointer parameters.
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> bodyNullChecked;
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> bodyPoisoned;
    CellSliceBodyScan scan(astContext(), bodyNullChecked, bodyPoisoned);
    scan.visit(func->getBody());
    nullChecked.insert(bodyNullChecked.begin(), bodyNullChecked.end());
    for (const clang::ParmVarDecl *param : bodyPoisoned)
      poisoned.insert(param);

    // A pointer local bound to a parameter joins its region (Phase 1a
    // records the parameter as a base); the cell-slice emission has no
    // local-pointer form, so such a parameter's class stays on the
    // historical lowering.
    PointerRegionAnalysis analysis;
    analysis.literalTemps = &literalTemps;
    analysis.analyze(astContext(), func->getBody());
    for (const clang::VarDecl *var : analysis.trackedVars())
      if (const PointerRegion *region = analysis.regionOf(var))
        for (const PointerBaseBinding &binding : region->bases)
          if (llvm::isa<clang::ParmVarDecl>(binding.base))
            poisoned.insert(binding.base);

    // In a project import an externally visible function may be called
    // from an unseen TU with a local argument; its class must not turn
    // its parameters into cell-slices.
    if (!soleTranslationUnit && func->isExternallyVisible())
      for (const clang::ParmVarDecl *param : func->parameters())
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()))
          poisoned.insert(param);

    // Call edges: exactly two argument shapes bind into the class — the
    // direct decay of a global array and the forwarding of another
    // data-pointer parameter. A local array decay records the Mixed
    // boundary fact; everything else poisons the callee parameter.
    forEachDataPointerCallArg(
        func->getBody(), [&](const clang::ParmVarDecl *calleeParam,
                             const clang::Expr *argument) {
          (void)unionFind.find(calleeParam);
          if (calleeParam->getType()
                  .getCanonicalType()
                  ->getPointeeType()
                  .getCanonicalType()
                  ->isPointerType()) {
            poisoned.insert(calleeParam);
            return;
          }
          if (const clang::VarDecl *global =
                  asDecayedGlobalArrayArg(argument)) {
            unionFind.unite(calleeParam, global);
            return;
          }
          if (const clang::ParmVarDecl *forwarded =
                  asPointerParamRead(argument)) {
            unionFind.unite(calleeParam, forwarded);
            return;
          }
          // A directly decayed local array is the ordinary Phase-1b slice
          // argument; record it as the Mixed boundary witness in case the
          // class also picks up a global base.
          const clang::Expr *e = stripTrivia(argument);
          const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
          const auto *ref =
              decay && decay->getCastKind() == clang::CK_ArrayToPointerDecay
                  ? llvm::dyn_cast<clang::DeclRefExpr>(
                        stripTrivia(decay->getSubExpr()))
                  : nullptr;
          const auto *localVar =
              ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
          if (localVar && localVar->hasLocalStorage()) {
            localJoins.try_emplace(calleeParam, localVar->getName().str());
            return;
          }
          poisoned.insert(calleeParam);
        });
  }

  // Aggregate the classes (snapshotting the nodes: `find` compresses
  // paths).
  struct ClassInfo {
    SmallVector<const clang::VarDecl *, 2> globals;
    SmallVector<const clang::ParmVarDecl *, 4> params;
    std::string localName;
    bool poisoned = false;
    bool nullChecked = false;
  };
  llvm::DenseMap<const clang::VarDecl *, ClassInfo> classes;
  for (const clang::VarDecl *node : unionFind.nodes()) {
    ClassInfo &info = classes[unionFind.find(node)];
    if (poisoned.contains(node))
      info.poisoned = true;
    if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(node)) {
      info.params.push_back(param);
      if (nullChecked.contains(param))
        info.nullChecked = true;
      auto joined = localJoins.find(param);
      if (joined != localJoins.end() && info.localName.empty())
        info.localName = joined->second;
      continue;
    }
    if (!node->hasLocalStorage() && !isPointerType(node->getType()))
      info.globals.push_back(node);
  }

  // Qualify or record the located boundary per class. Classes without a
  // global base are ordinary Phase-1b classes; classes with one that hit
  // any other snag silently keep the historical staged-copy rejection.
  for (const auto &entry : classes) {
    const ClassInfo &info = entry.second;
    if (info.params.empty() || info.globals.empty() || info.poisoned)
      continue;
    if (!info.localName.empty()) {
      for (const clang::VarDecl *global : info.globals)
        cellSliceRejects.try_emplace(
            global, CellSliceReject{CellSliceReject::Kind::Mixed,
                                    global->getName().str(), info.localName});
      continue;
    }
    if (info.nullChecked) {
      for (const clang::VarDecl *global : info.globals)
        cellSliceRejects.try_emplace(
            global, CellSliceReject{CellSliceReject::Kind::NullableGlobal,
                                    global->getName().str(), std::string()});
      continue;
    }
    // Every base must be a mutable (non-const, and internal unless this
    // TU is the whole program) one-dimensional global array of one shared
    // supported scalar element type.
    bool qualifies = true;
    clang::QualType element;
    for (const clang::VarDecl *global : info.globals) {
      const clang::ConstantArrayType *arrayType =
          astContext().getAsConstantArrayType(global->getType());
      if (!arrayType || global->getType().isConstQualified() ||
          (!soleTranslationUnit && global->isExternallyVisible())) {
        qualifies = false;
        break;
      }
      clang::QualType elem = arrayType->getElementType();
      bool scalarElem =
          elem->isRealFloatingType() ||
          (elem->isIntegerType() && !elem->isEnumeralType() &&
           !elem->isBooleanType());
      if (elem.isConstQualified() || astContext().getAsArrayType(elem) ||
          !scalarElem) {
        qualifies = false;
        break;
      }
      if (element.isNull())
        element = elem;
      else if (!astContext().hasSameUnqualifiedType(element, elem))
        qualifies = false;
      if (!qualifies)
        break;
    }
    // Every parameter must belong to a defined (and internal, unless sole
    // TU) function and point at the shared element type.
    for (const clang::ParmVarDecl *param : info.params) {
      if (!qualifies)
        break;
      const auto *fn =
          llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
      if (!fn || !fn->doesThisDeclarationHaveABody() ||
          fn->getName() == "main" ||
          (!soleTranslationUnit && fn->isExternallyVisible()) ||
          !astContext().hasSameUnqualifiedType(
              element,
              param->getType().getCanonicalType()->getPointeeType()))
        qualifies = false;
    }
    if (!qualifies)
      continue;
    for (const clang::ParmVarDecl *param : info.params)
      cellSliceParams.insert(param);
  }
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

void CImporter::collectAddressTaken(const clang::Stmt *stmt) {
  if (!stmt)
    return;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (unary->getOpcode() == clang::UO_AddrOf)
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
              unary->getSubExpr()->IgnoreParens()))
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          addressTaken.insert(var);
  for (const clang::Stmt *child : stmt->children())
    collectAddressTaken(child);
}

void CImporter::planFnPtrAliases(const clang::TranslationUnitDecl *unit) {
  fnPtrGlobalsWritten.clear();
  addressTakenFunctions.clear();

  // Records a write (assignment, increment) or escape (address-of) of a
  // file-scope function-pointer variable: such a variable never aliases.
  auto markWritten = [&](const clang::Expr *expr) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
    if (!ref)
      return;
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (!var || !var->hasGlobalStorage() ||
        !var->getType().getCanonicalType()->isFunctionPointerType())
      return;
    fnPtrGlobalsWritten.insert(
        llvm::cast<clang::VarDecl>(var->getCanonicalDecl()));
  };

  // Walks one statement/expression subtree. A `DeclRefExpr` naming a
  // function ANYWHERE outside the callee position of a direct call is an
  // address-taking use (the C decay model: the reference becomes a
  // function pointer value), so it joins the candidate set consulted by
  // `classifyFnPtrPointerResult`.
  auto scanStmt = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      // The callee of a direct call is not a value use of the function;
      // a function-pointer callee expression is scanned like any value.
      if (!call->getDirectCallee())
        self(self, call->getCallee());
      for (const clang::Expr *argument : call->arguments())
        self(self, argument);
      return;
    }
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          self(self, var->getInit());
      return;
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
      if (const auto *fn =
              llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
        const auto *canonical =
            llvm::cast<clang::FunctionDecl>(fn->getCanonicalDecl());
        if (!llvm::is_contained(addressTakenFunctions, canonical))
          addressTakenFunctions.push_back(canonical);
      }
      return;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt))
      if (binary->isAssignmentOp())
        markWritten(binary->getLHS());
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
      if (unary->getOpcode() == clang::UO_AddrOf ||
          unary->isIncrementDecrementOp())
        markWritten(unary->getSubExpr());
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };

  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->hasBody() && func->getDefinition() == func)
        scanStmt(scanStmt, func->getBody());
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      if (const clang::Expr *init = var->getInit())
        scanStmt(scanStmt, init);
  }

  // Alias planning: a file-scope function pointer initialized to a known
  // function and never written anywhere in the TU. An externally visible
  // variable only aliases in a sole-TU import (another TU could rebind
  // it); a variadic target only when it is the hosted definition-less
  // printf/fprintf, whose calls route through the printf machinery.
  for (const clang::Decl *decl : unit->decls()) {
    const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
    if (!var || var->isImplicit() || isSystemHeaderDecl(var))
      continue;
    if (!var->getType().getCanonicalType()->isFunctionPointerType())
      continue;
    const auto *canonical =
        llvm::cast<clang::VarDecl>(var->getCanonicalDecl());
    if (fnPtrAliases.contains(canonical) ||
        fnPtrGlobalsWritten.contains(canonical))
      continue;
    if (!currentSoleTU && var->isExternallyVisible())
      continue;
    const clang::Expr *init = canonical->getAnyInitializer();
    const clang::Expr *fnExpr = init ? returnedFunctionExpr(init) : nullptr;
    if (!fnExpr)
      continue;
    const auto *target = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    if (target->isVariadic()) {
      if (target->getDefinition() || !target->getDeclName().isIdentifier())
        continue;
      llvm::StringRef name = target->getName();
      if (name != "printf" && name != "fprintf")
        continue;
    }
    fnPtrAliases[canonical] = target;
  }
}

LogicalResult CImporter::importGlobalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
  const clang::VarDecl *canonical = var->getCanonicalDecl();
  if (globals.contains(canonical))
    return success(); // Redeclaration of an already imported global.
  // A devirtualized function-pointer alias (CTS-S, 00189) materializes no
  // global at all; every use lowers against its target.
  if (fnPtrAliases.contains(canonical))
    return success();

  if (var->getTLSKind() != clang::VarDecl::TLS_None)
    return emitError(loc) << "unsupported: thread-local global variable";

  // Internal-linkage (`static`) globals are mangled with the per-TU tag so
  // identically named file-statics in different TUs stay distinct; external
  // globals keep their bare C name and unify across TUs. The tag is empty for
  // a single-TU import, preserving the historical bare name.
  bool internal = !var->isExternallyVisible();
  std::string symbolName =
      internal ? currentTuTag + var->getName().str() : var->getName().str();

  // C reconciliation of redeclarations: a variable that is only ever
  // `extern`-declared has no storage in this translation unit; a tentative
  // definition (`int g;`) behaves as a zero-initialized definition.
  if (var->hasDefinition() == clang::VarDecl::DeclarationOnly) {
    // Referenced-only import of main-file extern declarations (the same
    // policy system-header declarations follow, C99-39): an extern object
    // that nothing in this TU references demands no storage anywhere and
    // imports nothing.
    if (!var->isReferenced())
      return success();
    if (!deferExternGlobals)
      return emitError(loc) << "unsupported: extern global variable without a "
                               "definition in this translation unit";
    // Project import: another TU may define this external symbol. Defer the
    // existence check to `finalizeProject` after every TU is merged.
    return deferExternGlobal(canonical, symbolName, var->getType(), loc);
  }

  // The declaration carrying the initializer (if any) supplies the type;
  // otherwise the most recent declaration does, whose type is the merged
  // composite of all redeclarations.
  const clang::VarDecl *initDecl = nullptr;
  const clang::Expr *init = canonical->getAnyInitializer(initDecl);
  const clang::VarDecl *typeDecl =
      init ? initDecl : canonical->getMostRecentDecl();
  clang::QualType varType = typeDecl->getType().getCanonicalType();
  // An owned stream handle has no global model (C99-48): the check must
  // precede the pointer-global cursor decomposition below.
  if (isFilePtrType(varType))
    return emitError(loc)
           << "unsupported: FILE* is only supported as a function-local "
              "variable";
  if (varType->isPointerType() && !varType->isFunctionPointerType())
    return importPointerGlobal(canonical, typeDecl, symbolName, loc);
  return createGlobal(canonical, typeDecl, symbolName, loc);
}

LogicalResult CImporter::importPointerGlobal(const clang::VarDecl *key,
                                             const clang::VarDecl *decl,
                                             llvm::StringRef symbolName,
                                             Location loc) {
  // Referenced-only import: an unreferenced pointer global demands no
  // storage anywhere in the supported subset (nothing can observe it), so
  // declarations like `struct S *s;` — even with incomplete pointee
  // types — import nothing.
  if (!key->isReferenced())
    return success();
  // Program-wide facts are merged per TU by `planOwners`; an externally
  // visible pointer global in a multi-file project could be rebound by a
  // TU whose facts are not visible when this one imports.
  if (!currentSoleTU && key->isExternallyVisible())
    return emitError(loc) << "unsupported: pointer-typed global variable "
                             "with external linkage in a multi-file project";
  if (isRustKeyword(symbolName))
    return emitError(loc) << "unsupported: global variable name '"
                          << symbolName << "' is a Rust keyword";
  if (symbolName == "__emitrust_tl")
    return emitError(loc) << "unsupported: global variable name "
                             "'__emitrust_tl' is reserved for the "
                             "thread-local accessor binder";
  auto checkFreshSymbol = [&](llvm::StringRef name) -> LogicalResult {
    if (SymbolTable::lookupSymbolIn(module, name))
      return emitError(loc) << "unsupported: global variable '" << name
                            << "' collides with an existing symbol";
    return success();
  };

  clang::QualType pointee =
      decl->getType().getCanonicalType()->getPointeeType();

  // Start from the program-wide body facts and merge the file-scope
  // initializer's binding: static storage requires a constant initializer,
  // so clang's evaluator yields an lvalue APValue — a base (declaration or
  // compound literal) plus a byte offset.
  PointerRegion facts = globalPtrFacts.lookup(key);
  const clang::CompoundLiteralExpr *literalInit = nullptr;
  const clang::StringLiteral *stringInit = nullptr;
  int64_t initByteOffset = 0;
  Location initLoc = loc;
  // Converts the initializer's byte offset into the flat cursor unit: the
  // number of innermost (non-array) elements of `objectType` it spans.
  // Fails (nullopt) when the offset does not land on an element boundary.
  auto flatCursorOffset =
      [&](clang::QualType objectType) -> std::optional<int64_t> {
    clang::QualType innermost = astContext().getBaseElementType(objectType);
    int64_t innerBytes =
        astContext().getTypeSizeInChars(innermost).getQuantity();
    if (innerBytes <= 0 || initByteOffset < 0 ||
        initByteOffset % innerBytes != 0)
      return std::nullopt;
    return initByteOffset / innerBytes;
  };
  if (const clang::Expr *init = decl->getInit()) {
    initLoc = translateLoc(init->getBeginLoc());
    const clang::APValue *value = decl->evaluateValue();
    if (!value || !value->isLValue())
      return emitError(initLoc) << "unsupported: global pointer initializer";
    if (value->isNullPointer())
      return emitError(initLoc) << "unsupported: null pointer constant "
                                   "assigned to a pointer variable";
    initByteOffset = value->getLValueOffset().getQuantity();
    clang::APValue::LValueBase lvalueBase = value->getLValueBase();
    if (const auto *baseDecl =
            lvalueBase.dyn_cast<const clang::ValueDecl *>()) {
      const auto *baseVar = llvm::dyn_cast<clang::VarDecl>(baseDecl);
      if (!baseVar || baseVar->hasLocalStorage())
        return emitError(initLoc)
               << "unsupported: global pointer initializer";
      PointerRegion initBinding;
      initBinding.bases.push_back(PointerBaseBinding{
          baseVar->getCanonicalDecl(), init->getBeginLoc()});
      mergeRegionFacts(facts, initBinding);
    } else if (const auto *baseExpr =
                   lvalueBase.dyn_cast<const clang::Expr *>()) {
      literalInit = llvm::dyn_cast<clang::CompoundLiteralExpr>(baseExpr);
      stringInit = llvm::dyn_cast<clang::StringLiteral>(baseExpr);
      if (!literalInit && !stringInit)
        return emitError(initLoc)
               << "unsupported: global pointer initializer";
    } else {
      return emitError(initLoc) << "unsupported: global pointer initializer";
    }
  }

  // Region validation, mirroring `emitPointerLocal`: the first
  // invalidating construct (a binding to a local object, a copied global
  // pointer, an escaping address, ...) rejects at its own site.
  if (!facts.invalidReason.empty())
    return emitError(translateLoc(facts.invalidLoc)) << facts.invalidReason;
  if (facts.literalBase)
    return emitError(translateLoc(facts.literalLoc))
           << "unsupported: global pointer bound to a string literal";
  unsigned baseKinds = (facts.bases.empty() ? 0 : 1) +
                       (facts.allocSite ? 1 : 0) + (literalInit ? 1 : 0) +
                       (stringInit ? 1 : 0);
  if (baseKinds > 1 || facts.bases.size() >= 2) {
    if (facts.bases.size() >= 2) {
      const PointerBaseBinding &first = facts.bases[0];
      const PointerBaseBinding &second = facts.bases[1];
      InFlightDiagnostic diag = emitError(loc);
      diag << "unsupported: global pointer '" << symbolName
           << "' would join objects '" << first.base->getName() << "' and '"
           << second.base->getName() << "' into one region";
      diag.attachNote(translateLoc(first.loc))
          << "bound to '" << first.base->getName() << "' here";
      diag.attachNote(translateLoc(second.loc))
          << "bound to '" << second.base->getName() << "' here";
      return diag;
    }
    return emitError(loc) << "unsupported: global pointer '" << symbolName
                          << "' bound to multiple objects";
  }
  if (baseKinds == 0)
    return emitError(loc) << "unsupported: global pointer variable '"
                          << symbolName << "' has no known target object";
  // Member-rooted bases (CTS-P9) are a pointer-local shape: the stored
  // global cursor scheme has no member projection to store.
  if (!facts.bases.empty() && facts.bases.front().member)
    return emitError(translateLoc(facts.bases.front().loc))
           << "unsupported: global pointer bound to a struct member";

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  IntegerType i64Type = builder.getIntegerType(64);
  auto createCursorGlobal = [&](llvm::StringRef name,
                                int64_t start) -> LogicalResult {
    if (failed(checkFreshSymbol(name)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(name), TypeAttr::get(i64Type),
        moduleBuilder.getIntegerAttr(i64Type, start), UnitAttr());
    return success();
  };

  // Shape 1: a promoted constant-size allocation — a zero-initialized
  // backing array global plus the cursor global.
  if (facts.allocSite) {
    Location allocLoc = translateLoc(facts.allocLoc);
    FailureOr<Type> elementType = mapType(pointee, allocLoc);
    if (failed(elementType))
      return failure();
    Type backingType = emitrust::ArrayType::get(
        builder.getContext(), facts.allocCount, *elementType);
    std::string backingName = (symbolName + "_backing").str();
    if (failed(checkFreshSymbol(backingName)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(backingName),
        TypeAttr::get(backingType), Attribute(), UnitAttr());
    if (failed(createCursorGlobal(symbolName, 0)))
      return failure();
    pointerGlobals[key] =
        PointerGlobalInfo{key, backingName, backingType, symbolName.str()};
    return success();
  }

  // Shape 2: a file-scope compound literal — a synthesized
  // constant-initialized backing global; degenerate for scalar/struct
  // literals, cursor-carrying for array literals.
  if (literalInit) {
    Location initLoc = translateLoc(literalInit->getBeginLoc());
    clang::QualType literalType = literalInit->getType();
    FailureOr<Type> backingType = mapType(literalType, initLoc);
    if (failed(backingType))
      return failure();
    clang::Expr::EvalResult literalValue;
    if (!literalInit->getInitializer()->EvaluateAsRValue(literalValue,
                                                         astContext()) ||
        literalValue.HasSideEffects)
      return emitError(initLoc)
             << "unsupported: non-constant global initializer";
    FailureOr<Attribute> init =
        convertAPValueInit(literalValue.Val, *backingType, literalType,
                           initLoc);
    if (failed(init))
      return failure();
    // Data-pointer members of the backing bind through the pointer
    // global's own key (the backing is 1:1 with the pointer), so
    // `s->f`-style member reads resolve statically (CTS-P2).
    collectGlobalMemberBindings(key, literalValue.Val, literalType,
                                literalInit->getBeginLoc());
    std::string backingName = (symbolName + "_backing").str();
    if (failed(checkFreshSymbol(backingName)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(backingName),
        TypeAttr::get(*backingType), *init, UnitAttr());
    if (astContext().getAsConstantArrayType(literalType)) {
      std::optional<int64_t> start = flatCursorOffset(literalType);
      if (!start)
        return emitError(initLoc)
               << "unsupported: global pointer initializer";
      if (failed(createCursorGlobal(symbolName, *start)))
        return failure();
      pointerGlobals[key] = PointerGlobalInfo{key, backingName, *backingType,
                                              symbolName.str()};
      return success();
    }
    if (facts.hasArithmetic)
      return emitError(translateLoc(facts.arithmeticLoc))
             << "unsupported: arithmetic on the address of a scalar object";
    if (initByteOffset != 0 ||
        !astContext().hasSameUnqualifiedType(pointee, literalType))
      return emitError(initLoc)
             << "unsupported: pointer type does not match its target object";
    pointerGlobals[key] =
        PointerGlobalInfo{key, backingName, *backingType, std::string()};
    return success();
  }

  // Shape 2b: a file-scope string-literal initializer (`char *s = "...";`,
  // CTS-L3) — the CTS-P1 read-only literal backing lifted to module scope.
  // The literal's bytes plus the terminating NUL become an immutable
  // `<name>_backing` byte-array global (never written: any write through
  // the region is rejected below, since writing a C string literal is UB),
  // and the pointer becomes a stored i64 cursor global into it.
  if (stringInit) {
    Location bindLoc = translateLoc(stringInit->getBeginLoc());
    if (facts.hasWriteThrough)
      return emitError(translateLoc(facts.writeThroughLoc))
             << "unsupported: write through a pointer to a string literal "
                "(the literal is read-only)";
    // Nullable literal regions are outside the CTS-P8 scope, matching the
    // function-local literal-region policy.
    if (facts.nullable)
      return emitError(translateLoc(facts.nullableLoc))
             << "unsupported: null pointer constant assigned to a pointer "
                "into a string literal";
    if (!stringInit->isOrdinary())
      return emitError(bindLoc) << "unsupported: non-ordinary string "
                                   "literal bound to a pointer";
    FailureOr<Type> elementType = mapType(pointee, bindLoc);
    if (failed(elementType))
      return failure();
    Type byteType = builder.getIntegerType(8);
    if (*elementType != byteType)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "string literal";
    // The backing holds the bytes plus the terminating NUL, so a
    // strlen-style walk terminates inside the array. Non-ASCII bytes are
    // rejected so the region's contents stay exact through the ASCII-only
    // `%s`/`%c` printing helpers (the CTS-P1/C99-28 policy).
    uint64_t length = stringInit->getLength();
    SmallVector<Attribute> bytes;
    bytes.reserve(length + 1);
    for (uint64_t i = 0; i != length; ++i) {
      uint32_t byte = stringInit->getCodeUnit(i);
      if (byte > 127)
        return emitError(bindLoc) << "unsupported: non-ASCII byte in "
                                     "string literal bound to a pointer";
      bytes.push_back(
          IntegerAttr::get(byteType, static_cast<int64_t>(byte)));
    }
    bytes.push_back(IntegerAttr::get(byteType, 0));
    // The initializer's byte offset is the flat cursor directly (i8
    // elements); anything past one-past-the-end is not a constant C
    // pointer value, so the bound is defensive.
    if (initByteOffset < 0 ||
        static_cast<uint64_t>(initByteOffset) > length + 1)
      return emitError(initLoc) << "unsupported: global pointer initializer";
    auto backingType = emitrust::ArrayType::get(builder.getContext(),
                                                length + 1, byteType);
    std::string backingName = (symbolName + "_backing").str();
    if (failed(checkFreshSymbol(backingName)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(backingName),
        TypeAttr::get(backingType), builder.getArrayAttr(bytes),
        moduleBuilder.getUnitAttr());
    if (failed(createCursorGlobal(symbolName, initByteOffset)))
      return failure();
    pointerGlobals[key] =
        PointerGlobalInfo{key, backingName, backingType, symbolName.str()};
    return success();
  }

  // Shape 3: a real global object base. The base's own `emitrust.global`
  // is resolved at each access (it may be declared later in the TU); the
  // type compatibility check runs on the C types, mirroring
  // `emitPointerLocal`.
  const PointerBaseBinding &binding = facts.bases.front();
  const clang::VarDecl *base = binding.base;
  Location bindLoc = translateLoc(binding.loc);
  if (base->hasLocalStorage()) // Defensive; `addBase` rejects this first.
    return emitError(bindLoc)
           << "unsupported: global pointer bound to local object '"
           << base->getName() << "' (the borrow would outlive the object)";
  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(base->getType())) {
    bool matchesLevel = false;
    for (const clang::ConstantArrayType *level = array; level;
         level = astContext().getAsConstantArrayType(
             level->getElementType())) {
      if (astContext().hasSameUnqualifiedType(pointee,
                                              level->getElementType())) {
        matchesLevel = true;
        break;
      }
    }
    if (!matchesLevel)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target array";
    std::optional<int64_t> start = flatCursorOffset(base->getType());
    if (!start)
      return emitError(initLoc) << "unsupported: global pointer initializer";
    if (failed(createCursorGlobal(symbolName, *start)))
      return failure();
    pointerGlobals[key] =
        PointerGlobalInfo{base, std::string(), Type(), symbolName.str()};
    return success();
  }
  if (isPointerType(base->getType())) // Defensive; `addBase` forbids it.
    return emitError(bindLoc)
           << "unsupported: global pointer bound to a pointer object";
  if (facts.hasArithmetic)
    return emitError(translateLoc(facts.arithmeticLoc))
           << "unsupported: arithmetic on the address of a scalar object";
  if (initByteOffset != 0 ||
      !astContext().hasSameUnqualifiedType(pointee, base->getType()))
    return emitError(bindLoc)
           << "unsupported: pointer type does not match its target object";
  pointerGlobals[key] =
      PointerGlobalInfo{base, std::string(), Type(), std::string()};
  return success();
}

LogicalResult CImporter::deferExternGlobal(const clang::VarDecl *key,
                                           llvm::StringRef symbolName,
                                           clang::QualType qualType,
                                           Location loc) {
  if (qualType.getCanonicalType()->isPointerType() &&
      !qualType.getCanonicalType()->isFunctionPointerType())
    return emitError(loc) << "unsupported: pointer-typed global variable";
  FailureOr<Type> mlirType = mapType(qualType, loc);
  if (failed(mlirType))
    return failure();
  globals[key] = GlobalInfo{symbolName.str(), *mlirType};
  pendingExternGlobals.try_emplace(symbolName, loc);
  return success();
}

LogicalResult CImporter::createGlobal(const clang::VarDecl *key,
                                      const clang::VarDecl *decl,
                                      llvm::StringRef symbolName,
                                      Location loc) {
  if (symbolName.empty())
    return emitError(loc) << "unsupported: unnamed global variable";
  if (isRustKeyword(symbolName))
    return emitError(loc) << "unsupported: global variable name '"
                          << symbolName << "' is a Rust keyword";
  // Globals are emitted as `static` items (thread-local or plain), and Rust
  // identifier patterns cannot shadow statics, so a global spelled like the
  // thread-local accessor binder would break every mutable-global access.
  if (symbolName == "__emitrust_tl")
    return emitError(loc) << "unsupported: global variable name "
                             "'__emitrust_tl' is reserved for the "
                             "thread-local accessor binder";
  if (Operation *existing = SymbolTable::lookupSymbolIn(module, symbolName)) {
    auto existingGlobal = llvm::dyn_cast<emitrust::GlobalOp>(existing);
    if (!deferExternGlobals || !existingGlobal)
      return emitError(loc) << "unsupported: global variable '" << symbolName
                            << "' collides with an existing symbol";
    // Project import: a second file-scope definition of the same external
    // global. A tentative definition (no initializer) yields to a real one;
    // two real definitions are a duplicate-definition error.
    bool incomingHasInit = decl->getInit() != nullptr;
    if (!incomingHasInit) {
      globals[key] = GlobalInfo{symbolName.str(), existingGlobal.getType()};
      return success();
    }
    if (existingGlobal.getInitAttr())
      return emitError(loc)
             << "unsupported: conflicting definition of global variable '"
             << symbolName
             << "' (already defined in another translation unit)";
    existingGlobal.erase(); // Upgrade the tentative definition to this one.
  }

  clang::QualType qualType = decl->getType();
  // C99-7: scan the whole global's type (pointer globals bypass
  // `mapType`, and `int * volatile g` carries the qualifier on the
  // pointer itself).
  if (hasVolatileQualifier(astContext(), qualType))
    return emitError(loc) << "unsupported: volatile-qualified type";
  // Function pointers map to `!emitrust.fn_ptr` and are legal globals;
  // data pointers stay rejected.
  if (qualType.getCanonicalType()->isPointerType() &&
      !qualType.getCanonicalType()->isFunctionPointerType())
    return emitError(loc) << "unsupported: pointer-typed global variable";
  // CTS-BR (00216): a byte-region aggregate global is a byte-image
  // global — computed against the target layout, zero-filled, and
  // extended past sizeof by a static flexible-array-member tail.
  if (isByteRegionAggregate(qualType))
    return createByteRegionGlobal(key, decl, symbolName, loc);
  FailureOr<Type> mlirType = mapType(qualType, loc);
  if (failed(mlirType))
    return failure();

  Attribute initAttr;
  if (decl->getInit()) {
    FailureOr<Attribute> converted = convertGlobalInit(decl, *mlirType, loc);
    if (failed(converted))
      return failure();
    initAttr = *converted;
    // Record the static member bindings of any data-pointer fields the
    // initialized aggregate carries (CTS-P2); the stored i64 members
    // themselves converted to 0 above.
    if (const clang::APValue *value = decl->evaluateValue())
      collectGlobalMemberBindings(key, *value, decl->getType(),
                                  decl->getInit()->getBeginLoc());
  }

  // A const-qualified global is never written (clang rejects writes), so it
  // becomes an immutable Rust static. Struct- and fn_ptr-typed const
  // globals keep the mutable (Cell) representation: the GlobalOp `const`
  // marker is limited to scalar and array value types.
  bool isConst =
      qualType.isConstQualified() &&
      !llvm::isa<emitrust::StructType, emitrust::FnPtrType>(*mlirType);

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::GlobalOp>(
      loc, moduleBuilder.getStringAttr(symbolName), TypeAttr::get(*mlirType),
      initAttr, isConst ? moduleBuilder.getUnitAttr() : UnitAttr());
  globals[key] = GlobalInfo{symbolName.str(), *mlirType};
  return success();
}

FailureOr<Attribute> CImporter::convertGlobalInit(const clang::VarDecl *decl,
                                                  Type type, Location loc) {
  const clang::Expr *init = decl->getInit();
  Location initLoc = init ? translateLoc(init->getBeginLoc()) : loc;
  // A file-scope `char s[] = "..."` folds to a plain i8 element list
  // through the APValue path below, but non-ASCII bytes are rejected up
  // front (mirroring the block-scope string initializer) so the array's
  // contents stay exact through the ASCII-only `%s`/`%c` printing helpers.
  // A wide literal's code units fold to i32 elements that never feed those
  // byte-string helpers, so they carry no ASCII limit (matching the
  // block-scope `emitStringArrayInit` policy).
  if (init) {
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
            init->IgnoreParenImpCasts())) {
      if (!literal->isWide())
        for (unsigned i = 0, n = literal->getLength(); i != n; ++i)
          if (literal->getCodeUnit(i) > 127)
            return emitError(initLoc)
                   << "unsupported: non-ASCII byte in string literal "
                      "initializer";
    }
  }
  // A function-pointer global initializer is either the null constant
  // (`None`) or a direct function reference (`Some(name)`, after the
  // signature check); both are emitted as opaque attributes.
  if (auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(type)) {
    const clang::Expr *e = stripTrivia(init);
    if (e->isNullPointerConstant(astContext(),
                                 clang::Expr::NPC_NeverValueDependent) !=
        clang::Expr::NPCK_NotNull)
      return Attribute(
          emitrust::OpaqueAttr::get(builder.getContext(), "None"));
    // A fn-ptr-to-fn-ptr conversion (prototype-less pointer bound to a
    // prototyped function) is transparent here; the signature check below
    // runs against the global's own fn_ptr type.
    if (const auto *bitcast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
      if (bitcast->getCastKind() == clang::CK_BitCast &&
          isFunctionPointer(bitcast->getSubExpr()->getType()))
        e = stripTrivia(bitcast->getSubExpr());
    const clang::Expr *fnExpr = nullptr;
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_FunctionToPointerDecay)
        fnExpr = cast->getSubExpr();
    } else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      if (unary->getOpcode() == clang::UO_AddrOf)
        fnExpr = unary->getSubExpr();
    }
    if (!fnExpr)
      return emitError(initLoc)
             << "unsupported: global function pointer initializer";
    FailureOr<std::string> name =
        resolveFunctionPointerTarget(fnExpr, fnPtrType, initLoc);
    if (failed(name))
      return failure();
    return Attribute(emitrust::OpaqueAttr::get(
        builder.getContext(), (llvm::Twine("Some(") + *name + ")").str()));
  }
  // Static storage duration requires a constant initializer (C11 6.7.9p4);
  // clang's constant evaluator produces the folded value. For aggregates
  // it also resolves designators and zero-fills the uninitialized holes,
  // so the APValue is the complete element-by-element picture.
  clang::APValue *value = decl->evaluateValue();
  if (!value)
    return emitError(initLoc) << "unsupported: non-constant global initializer";
  return convertAPValueInit(*value, type, decl->getType(), initLoc);
}

FailureOr<Attribute> CImporter::convertAPValueInit(const clang::APValue &value,
                                                   Type type,
                                                   clang::QualType cType,
                                                   Location loc) {
  // A data-pointer struct member is stored as a plain i64 cursor field
  // whose degenerate binding carries no runtime information: the constant
  // initializer's lvalue (or null) converts to 0, and the binding itself
  // is recorded by `collectGlobalMemberBindings` at the object's import.
  // An admitted `void *` fn-ptr member (CTS-BR, 00216) folds like a
  // genuinely fn-ptr-typed field: its converted type is already the
  // fn_ptr, so it must not fall into the data-pointer i64 shortcut.
  if (!cType.isNull() && isDataPointer(cType) &&
      !llvm::isa<emitrust::FnPtrType>(type)) {
    auto intType = llvm::dyn_cast<IntegerType>(type);
    if (!intType || intType.getWidth() != 64 ||
        (!value.isLValue() && !value.isNullPointer()))
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    return Attribute(IntegerAttr::get(intType, 0));
  }
  if (auto intType = llvm::dyn_cast<IntegerType>(type)) {
    if (!value.isInt())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    if (intType.getWidth() == 1)
      return Attribute(builder.getBoolAttr(value.getInt().getBoolValue()));
    return Attribute(IntegerAttr::get(
        intType, value.getInt().extOrTrunc(intType.getWidth())));
  }
  if (auto floatType = llvm::dyn_cast<FloatType>(type)) {
    if (!value.isFloat())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    // A long double initializer arrives as an x87 APFloat; floatAttrFor
    // narrows it to the f64 the type policy substitutes (CTS 00204).
    return Attribute(floatAttrFor(floatType, value.getFloat()));
  }
  if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type)) {
    if (!value.isArray() || value.getArraySize() != arrayType.getSize())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::ArrayType *cArray =
        cType.isNull() ? nullptr : astContext().getAsArrayType(cType);
    clang::QualType cElement =
        cArray ? cArray->getElementType() : clang::QualType();
    Type elementType = arrayType.getElementType();
    SmallVector<Attribute> elements;
    elements.reserve(arrayType.getSize());
    for (unsigned i = 0, n = value.getArrayInitializedElts(); i != n; ++i) {
      FailureOr<Attribute> element = convertAPValueInit(
          value.getArrayInitializedElt(i), elementType, cElement, loc);
      if (failed(element))
        return failure();
      elements.push_back(*element);
    }
    // Elements beyond the explicitly initialized prefix share the filler
    // value (C99 zero-fill of partial and designated initialization).
    if (elements.size() < arrayType.getSize()) {
      if (!value.hasArrayFiller())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      FailureOr<Attribute> filler = convertAPValueInit(
          value.getArrayFiller(), elementType, cElement, loc);
      if (failed(filler))
        return failure();
      elements.append(arrayType.getSize() - elements.size(), *filler);
    }
    return Attribute(builder.getArrayAttr(elements));
  }
  if (auto structType = llvm::dyn_cast<emitrust::StructType>(type)) {
    auto structDef = llvm::dyn_cast_or_null<emitrust::StructDefOp>(
        SymbolTable::lookupSymbolIn(module, structType.getName()));
    if (!structDef)
      return emitError(loc)
             << "unsupported: global initializer for this type";
    // A union global carries a Union APValue, not a Struct one; it
    // initializes its single storage slot (the union imports as a
    // one-field struct, see `collectUnionSlot`) exactly like an
    // anonymous union member's slot, bit-exact across a signedness or
    // float pun.
    if (const clang::RecordDecl *unionRecord =
            structDefRecords.lookup(structType.getName());
        unionRecord && unionRecord->isUnion()) {
      ArrayAttr slotTypes = structDef.getFieldTypes();
      if (slotTypes.size() != 1)
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      Type slotType = llvm::cast<TypeAttr>(slotTypes[0]).getValue();
      FailureOr<Attribute> slot =
          convertAnonymousSlotInit(value, unionRecord, slotType, loc);
      if (failed(slot))
        return failure();
      return Attribute(builder.getArrayAttr({*slot}));
    }
    if (!value.isStruct())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::RecordDecl *record =
        cType.isNull() ? nullptr : recordOfType(cType);
    SmallVector<clang::QualType> cFields;
    if (record)
      for (const clang::FieldDecl *field : record->fields())
        cFields.push_back(field->getType());
    ArrayAttr fieldTypes = structDef.getFieldTypes();
    SmallVector<Attribute> fields;
    fields.reserve(fieldTypes.size());
    // An imported record converts field by field along the C structure,
    // which resolves flattened anonymous members; the struct_def's
    // flattened type list is consumed in step. Synthesized struct_defs
    // (owner structs, which never carry a C initializer in practice)
    // have no record and keep the positional conversion.
    if (const clang::RecordDecl *record =
            structDefRecords.lookup(structType.getName())) {
      unsigned typeIndex = 0;
      if (failed(convertRecordAPValue(value, record, fieldTypes, typeIndex,
                                      fields, loc)))
        return failure();
      if (typeIndex != fieldTypes.size() || fields.size() != fieldTypes.size())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return Attribute(builder.getArrayAttr(fields));
    }
    if (value.getStructNumFields() != fieldTypes.size())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    for (auto [i, fieldType] : llvm::enumerate(fieldTypes)) {
      FailureOr<Attribute> field = convertAPValueInit(
          value.getStructField(i),
          llvm::cast<TypeAttr>(fieldType).getValue(),
          i < cFields.size() ? cFields[i] : clang::QualType(), loc);
      if (failed(field))
        return failure();
      fields.push_back(*field);
    }
    return Attribute(builder.getArrayAttr(fields));
  }
  // A function-pointer element (a fn_ptr struct field, CTS-L3): the
  // evaluator yields an lvalue whose base is the target function
  // declaration (or the null constant, `None`). The signature check is the
  // same one every fn_ptr constant goes through.
  if (auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(type)) {
    if (!value.isLValue())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    if (value.isNullPointer())
      return Attribute(
          emitrust::OpaqueAttr::get(builder.getContext(), "None"));
    const auto *callee = llvm::dyn_cast_or_null<clang::FunctionDecl>(
        value.getLValueBase().dyn_cast<const clang::ValueDecl *>());
    if (!callee || !value.getLValueOffset().isZero())
      return emitError(loc)
             << "unsupported: global function pointer initializer";
    FailureOr<std::string> name =
        resolveFunctionPointerDecl(callee, fnPtrType, loc);
    if (failed(name))
      return failure();
    return Attribute(emitrust::OpaqueAttr::get(
        builder.getContext(), (llvm::Twine("Some(") + *name + ")").str()));
  }
  return emitError(loc) << "unsupported: global initializer for this type";
}

LogicalResult CImporter::convertRecordAPValue(
    const clang::APValue &value, const clang::RecordDecl *record,
    ArrayAttr fieldTypes, unsigned &typeIndex,
    SmallVectorImpl<Attribute> &fields, Location loc) {
  if (!value.isStruct())
    return emitError(loc)
           << "unsupported: global initializer does not match its type";
  unsigned valueIndex = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    // A bit-field member has no field of its own in the flattened
    // struct_def; constant initialization of one is out of the C99-45
    // scope (packing the APValue bits is unimplemented).
    if (field->isBitField())
      return emitError(loc)
             << "unsupported: global initializer for a struct with "
                "bit-fields";
    if (valueIndex >= value.getStructNumFields())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    // A flexible array member or GNU zero-length array member has no
    // field in the struct_def (CTS-BR, 00216); its APValue slot is
    // consumed without emitting anything. A non-empty FAM-tail constant
    // on a TYPED record would silently vanish, so it stays rejected.
    if (field->getType()->isIncompleteArrayType() ||
        isZeroLengthArrayType(field->getType())) {
      const clang::APValue &dropped = value.getStructField(valueIndex++);
      if (field->getType()->isIncompleteArrayType() && dropped.isArray() &&
          dropped.getArraySize() > 0)
        return emitError(loc)
               << "unsupported: flexible array member initializer";
      continue;
    }
    const clang::APValue &fieldValue = value.getStructField(valueIndex++);
    if (field->isAnonymousStructOrUnion()) {
      const clang::RecordDecl *member =
          field->getType()->getAsRecordDecl()->getDefinition();
      if (member->isUnion()) {
        if (typeIndex >= fieldTypes.size())
          return emitError(loc)
                 << "unsupported: global initializer does not match its type";
        Type slotType =
            llvm::cast<TypeAttr>(fieldTypes[typeIndex++]).getValue();
        FailureOr<Attribute> slot =
            convertAnonymousSlotInit(fieldValue, member, slotType, loc);
        if (failed(slot))
          return failure();
        fields.push_back(*slot);
        continue;
      }
      if (failed(convertRecordAPValue(fieldValue, member, fieldTypes,
                                      typeIndex, fields, loc)))
        return failure();
      continue;
    }
    if (typeIndex >= fieldTypes.size())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    Type fieldType = llvm::cast<TypeAttr>(fieldTypes[typeIndex++]).getValue();
    FailureOr<Attribute> attr =
        convertAPValueInit(fieldValue, fieldType, field->getType(), loc);
    if (failed(attr))
      return failure();
    fields.push_back(*attr);
  }
  return success();
}

FailureOr<Attribute>
CImporter::convertAnonymousSlotInit(const clang::APValue &value,
                                    const clang::RecordDecl *record,
                                    Type slotType, Location loc) {
  if (record->isUnion()) {
    if (!value.isUnion())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::FieldDecl *active = value.getUnionField();
    if (!active) {
      // No arm was initialized: the slot takes its zero value, matching
      // C's zero-fill of static storage.
      Attribute zero = builder.getZeroAttr(slotType);
      if (!zero)
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return zero;
    }
    if (active->isAnonymousStructOrUnion())
      return convertAnonymousSlotInit(
          value.getUnionValue(),
          active->getType()->getAsRecordDecl()->getDefinition(), slotType,
          loc);
    // A float-pun arm's constant crosses the domain at compile time: the
    // active arm's value lands on the slot as its exact bit pattern (the
    // constant counterpart of the `emitrust.bitcast` at access sites).
    // Same-width int arms need no special case — the IntegerAttr path of
    // `convertAPValueInit` is already bit-exact (extOrTrunc).
    const clang::APValue &armValue = value.getUnionValue();
    if (auto slotInt = llvm::dyn_cast<IntegerType>(slotType);
        slotInt && armValue.isFloat()) {
      llvm::APInt bits = armValue.getFloat().bitcastToAPInt();
      if (bits.getBitWidth() != slotInt.getWidth())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return Attribute(IntegerAttr::get(slotInt, bits));
    }
    if (auto slotFloat = llvm::dyn_cast<FloatType>(slotType);
        slotFloat && armValue.isInt()) {
      if (armValue.getInt().getBitWidth() != slotFloat.getWidth())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return Attribute(FloatAttr::get(
          slotFloat, llvm::APFloat(slotFloat.getFloatSemantics(),
                                   armValue.getInt())));
    }
    return convertAPValueInit(armValue, slotType, active->getType(), loc);
  }
  // A nested anonymous struct on the slot path has exactly one field
  // (`anonymousUnionArmLeaf` admitted the arm); descend into it.
  if (!value.isStruct() || value.getStructNumFields() == 0)
    return emitError(loc)
           << "unsupported: global initializer does not match its type";
  const clang::FieldDecl *only = *record->field_begin();
  const clang::APValue &fieldValue = value.getStructField(0);
  if (only->isAnonymousStructOrUnion())
    return convertAnonymousSlotInit(
        fieldValue, only->getType()->getAsRecordDecl()->getDefinition(),
        slotType, loc);
  return convertAPValueInit(fieldValue, slotType, only->getType(), loc);
}

const GlobalInfo *CImporter::lookupGlobal(const clang::ValueDecl *decl) const {
  const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
  if (!var)
    return nullptr;
  auto it = globals.find(var->getCanonicalDecl());
  return it == globals.end() ? nullptr : &it->second;
}

const clang::VarDecl *
CImporter::asDirectGlobalRef(const clang::Expr *expr) const {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParens());
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var)
    return nullptr;
  const clang::VarDecl *canonical = var->getCanonicalDecl();
  return globals.contains(canonical) ? canonical : nullptr;
}

bool CImporter::rootsAtGlobal(const clang::Expr *expr) const {
  const clang::Expr *e = expr->IgnoreParens();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    return lookupGlobal(ref->getDecl()) != nullptr;
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
    return !member->isArrow() && rootsAtGlobal(member->getBase());
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    return rootsAtGlobal(subscript->getBase()->IgnoreParenImpCasts());
  return false;
}

LogicalResult
CImporter::flushGlobalWriteback(Location loc,
                                const GlobalWriteback &writeback) {
  if (!writeback.place)
    return success();
  if (!writeback.multiBases.empty()) {
    // A staged multi-base element (CTS-P7): dispatch on the staged
    // discriminant and store the mutated element back into the active
    // base at the staged cursor. A global-member base's arm (CTS-P9)
    // stages the global's whole value afresh, assigns the projected
    // member, and stores the whole value back.
    Value value = loadPlace(loc, writeback.place);
    return emitMultiBaseDispatch(
        loc, writeback.multiBases, writeback.multiBaseIndex,
        [&](const PointerBaseKey &base) -> LogicalResult {
          if (base.var->hasLocalStorage()) {
            FailureOr<Value> element = materializeLocalElementPlace(
                loc, base, writeback.multiCursor,
                writeback.multiPointeeType);
            if (failed(element))
              return failure();
            return storeToPlace(loc, *element, value);
          }
          FailureOr<std::pair<Value, std::string>> staged =
              stageGlobalCopy(loc, base.var);
          if (failed(staged))
            return failure();
          Value place = staged->first;
          if (base.member) {
            FailureOr<Value> memberPlace =
                projectMemberPlace(loc, place, base.member);
            if (failed(memberPlace))
              return failure();
            place = *memberPlace;
          }
          FailureOr<Value> element = refineElementPlace(
              loc, place, writeback.multiCursor, writeback.multiPointeeType);
          if (failed(element))
            return failure();
          if (failed(storeToPlace(loc, *element, value)))
            return failure();
          auto lvalueType =
              llvm::cast<emitrust::LValueType>(staged->first.getType());
          Value full = builder
                           .create<emitrust::LoadOp>(
                               loc, lvalueType.getValueType(), staged->first)
                           .getResult();
          builder.create<emitrust::GlobalStoreOp>(
              loc, full, globalSymbol(staged->second));
          return success();
        });
  }
  auto lvalueType =
      llvm::cast<emitrust::LValueType>(writeback.place.getType());
  Value full = builder
                   .create<emitrust::LoadOp>(loc, lvalueType.getValueType(),
                                             writeback.place)
                   .getResult();
  builder.create<emitrust::GlobalStoreOp>(loc, full,
                                          globalSymbol(writeback.symbol));
  return success();
}

LogicalResult CImporter::commitGlobalWriteback(
    Location loc, const GlobalWriteback &writeback, bool refreshStaged,
    llvm::function_ref<LogicalResult()> mutate) {
  // Refresh a stale single-base staged copy: the staging load ran when
  // the LHS place was formed, and any global write emitted since (an RHS
  // call mutating another subobject of the same global) would be
  // reverted by flushing that stale whole-value snapshot. Rebinding the
  // staged place to a fresh snapshot immediately before the mutation
  // keeps the user-visible evaluation order identical — every
  // subexpression value was already materialized — while making the
  // store-back exact. Statements without side-effecting subexpressions
  // skip the refresh: nothing can have written the global since staging.
  if (refreshStaged && writeback.place && writeback.multiBases.empty() &&
      !writeback.symbol.empty()) {
    auto lvalueType =
        llvm::cast<emitrust::LValueType>(writeback.place.getType());
    Value fresh = builder
                      .create<emitrust::GlobalLoadOp>(
                          loc, lvalueType.getValueType(),
                          globalSymbol(writeback.symbol))
                      .getResult();
    builder.create<emitrust::AssignOp>(loc, writeback.place, fresh);
  }
  if (failed(mutate()))
    return failure();
  return flushGlobalWriteback(loc, writeback);
}

std::string CImporter::mlirFuncName(const clang::FunctionDecl *func) const {
  llvm::StringRef cName = func->getName();
  if (cName == "main")
    return "c_main";
  // A function whose C spelling is a Rust keyword mangles like a struct
  // member — one trailing underscore (`match` -> `match_`, CTS 00204).
  // The mangled spelling is the symbol's identity everywhere (definition
  // and call sites resolve through this same function); a collision with
  // an existing `match_` is rejected in `importFunction`.
  std::string base = mangleMemberName(cName);
  // Internal-linkage (`static`) functions are mangled with the per-TU tag so
  // identically named file-statics in different TUs never collide. The tag is
  // empty for a single-TU import, preserving the historical bare name.
  if (func->getStorageClass() == clang::SC_Static)
    return currentTuTag + base;
  return base;
}

/// Returns whether `later` differs from `earlier` only by refining
/// `!emitrust.mut_ref<T>` parameter positions into
/// `!emitrust.mut_ref<!emitrust.slice<T>>` — the shape change a
/// definition's pointer-parameter classification may introduce over a
/// prototype-only import from another translation unit.
static bool isSliceRefinementOf(FunctionType earlier, FunctionType later) {
  if (earlier.getNumInputs() != later.getNumInputs() ||
      earlier.getResults() != later.getResults())
    return false;
  for (auto [oldType, newType] :
       llvm::zip_equal(earlier.getInputs(), later.getInputs())) {
    if (oldType == newType)
      continue;
    auto oldRef = llvm::dyn_cast<emitrust::MutRefType>(oldType);
    auto newRef = llvm::dyn_cast<emitrust::MutRefType>(newType);
    if (!oldRef || !newRef)
      return false;
    auto newSlice = llvm::dyn_cast<emitrust::SliceType>(newRef.getPointee());
    if (!newSlice || newSlice.getElementType() != oldRef.getPointee())
      return false;
  }
  return true;
}

LogicalResult CImporter::importFunction(const clang::FunctionDecl *func) {
  Location loc = translateLoc(func->getLocation());
  llvm::StringRef cName = func->getName();

  if (func->isVariadic()) {
    const clang::FunctionDecl *definition = func->getDefinition();
    if (definition && definition->hasBody()) {
      // A variadic definition whose body never touches va_list (no
      // va_start/va_arg/va_copy, no va_list declarations) can never
      // observe its trailing arguments, so it imports as its fixed
      // prototype — the named parameters only (CTS-P9). Call sites drop
      // effect-free trailing extras in `emitCall`. A body that uses
      // va_list in the bounded shape monomorphizes per call site
      // (CTS 00204, planVaMonomorph); any other va_list-using body
      // keeps the blanket rejection.
      if (bodyUsesVaList(astContext(), definition->getBody())) {
        auto planIt = vaMonomorphPlans.find(definition->getCanonicalDecl());
        if (planIt == vaMonomorphPlans.end())
          return emitError(loc)
                 << "unsupported: variadic function definition";
        if (!func->isThisDeclarationADefinition())
          return success(); // Clones are emitted at the definition.
        return emitVaClones(definition, planIt->second);
      }
    } else {
      // Body-less variadic declarations (printf in particular) are
      // skipped; calls to them are handled specially or rejected at the
      // call site.
      return success();
    }
  }
  // Body-less puts/putchar declarations are skipped like printf's: their
  // statement-position calls are lowered by name (`emitPuts`/`emitPutchar`)
  // and never reference the symbol, and a body-less function would
  // otherwise be rejected by `finalizeProject`.
  if ((cName == "puts" || cName == "putchar") && !func->getDefinition())
    return success();

  // Referenced-only import of main-file prototypes (the same policy
  // system-header declarations follow, C99-39): a body-less prototype with
  // no definition in this TU that nothing in this TU references demands no
  // definition and imports nothing — not even its signature types. A
  // referenced prototype is still imported, and `finalizeProject` rejects
  // it at the use site if no translation unit supplies the body.
  if (!func->isThisDeclarationADefinition() && !func->getDefinition() &&
      !func->isReferenced())
    return success();

  bool isDefinition = func->isThisDeclarationADefinition();
  // C `main` is renamed so the driver can emit its own Rust `main` wrapper;
  // the replacement name is therefore reserved, and any other spelling that
  // Rust reserves cannot be emitted as a Rust function name.
  if (cName == "c_main")
    return emitError(loc) << "unsupported: function name 'c_main' is "
                             "reserved for the imported C main";
  if (cName == "__emitrust_fmt_f64")
    return emitError(loc) << "unsupported: function name '__emitrust_fmt_f64' "
                             "is reserved for the printf %f helper";
  if (cName == "__emitrust_fmt_c")
    return emitError(loc) << "unsupported: function name '__emitrust_fmt_c' "
                             "is reserved for the printf %c helper";
  if (cName == "__emitrust_cstr")
    return emitError(loc) << "unsupported: function name '__emitrust_cstr' "
                             "is reserved for the printf %s helper";
  if (cName == "__emitrust_strlen")
    return emitError(loc) << "unsupported: function name '__emitrust_strlen' "
                             "is reserved for the strlen helper";
  if (cName.starts_with("__emitrust_"))
    return emitError(loc) << "unsupported: function name '" << cName
                          << "' is in the reserved '__emitrust_' helper "
                             "namespace";
  // A function whose C spelling is a Rust keyword mangles with a trailing
  // underscore (mlirFuncName, CTS 00204) instead of rejecting. The mangle
  // must not silently merge two C symbols: a source declaration already
  // spelled with the mangled name rejects the keyword function where it
  // is declared.
  if (isRustKeyword(cName) &&
      ordinaryRawTuNames.contains((cName + "_").str()))
    return emitError(loc) << "unsupported: function name '" << cName
                          << "' mangles to '" << cName
                          << "_', which collides with an existing symbol";
  std::string name = mlirFuncName(func);

  // K&R callsite-prototype inference (FR-29, CTS 00209): the definition's
  // body refines argument-called prototype-less fn-ptr decls to their
  // callsite signatures BEFORE the signature is built, so a prototype
  // visited ahead of its later definition maps the identical refined
  // parameter types (the reconciliation below sees no conflict). The
  // result is staged locally and installed into `inferredFnPtrSigs` only
  // in the definition's own prologue: a block-scope prototype imported
  // MID-BODY must not clobber the enclosing function's live map.
  const clang::FunctionDecl *definition = func->getDefinition();
  llvm::DenseMap<const clang::VarDecl *, emitrust::FnPtrType> inferredSigs;
  if (definition && definition->doesThisDeclarationHaveABody() &&
      failed(inferNoProtoCallSignatures(definition, inferredSigs)))
    return failure();

  // Build the signature. Pointer-parameter kinds derive from the
  // definition's body (Phase 1b); a prototype whose definition appears
  // later in the same TU classifies identically because
  // `FunctionDecl::getDefinition` searches the whole redeclaration chain.
  // A method-planned function (Phase 4; prototypes consult the same plan,
  // keyed by the canonical declaration) instead trades every data-pointer
  // parameter for an i64 element index behind a leading owner receiver.
  const clang::VarDecl *methodOwner =
      methodPlans.lookup(func->getCanonicalDecl());
  emitrust::StructType ownerStructType;
  ArrayRef<ParamKind> paramKinds = classifyPointerParams(func);
  SmallVector<Type> inputTypes;
  if (methodOwner) {
    ownerStructType = emitrust::StructType::get(
        builder.getContext(), ownerPlans.find(methodOwner)->second.structName);
    inputTypes.push_back(emitrust::MutRefType::get(ownerStructType));
  }
  if (name == "c_main" && func->getNumParams() != 0) {
    // C `main`'s standard two-parameter form (C99 5.1.2.2.1): `argc`
    // imports as a plain i32 — the crate's `fn main` wrapper passes the
    // process argument count — and `argv`, whose `char **` shape has no
    // safe decomposition, is dropped from the imported signature. A body
    // that reads `argv` is rejected here with a located diagnostic, so
    // the dropped parameter can never be observed.
    if (func->getNumParams() != 2 ||
        !astContext().hasSameUnqualifiedType(
            func->getParamDecl(0)->getType().getCanonicalType(),
            astContext().IntTy))
      return emitError(loc) << "unsupported: main must take zero or two "
                               "(int, char **) parameters";
    const clang::ParmVarDecl *argvParam = func->getParamDecl(1);
    clang::QualType argvType = argvParam->getType().getCanonicalType();
    const auto *outer = argvType->getAs<clang::PointerType>();
    const auto *inner =
        outer ? outer->getPointeeType().getCanonicalType()
                    ->getAs<clang::PointerType>()
              : nullptr;
    if (!inner || !astContext().hasSameUnqualifiedType(
                      inner->getPointeeType(), astContext().CharTy))
      return emitError(loc) << "unsupported: main must take zero or two "
                               "(int, char **) parameters";
    if (argvParam->isReferenced() || argvParam->isUsed())
      return emitError(translateLoc(argvParam->getLocation()))
             << "unsupported: use of main's argv parameter (command-line "
                "argument values are not modeled)";
    inputTypes.push_back(builder.getIntegerType(32));
  } else {
    for (auto [index, param] : llvm::enumerate(func->parameters())) {
      if (methodOwner && isPointerType(param->getType()) &&
          !isFunctionPointer(param->getType())) {
        inputTypes.push_back(builder.getIntegerType(64));
        continue;
      }
      // A callsite-inferred prototype-less fn-ptr parameter (FR-29, CTS
      // 00209) refines to the inferred signature instead of the
      // zero-parameter no-proto mapping; keyed by the DEFINITION's decl
      // so a prototype visit maps identically.
      if (definition && index < definition->getNumParams())
        if (emitrust::FnPtrType refined =
                inferredSigs.lookup(definition->getParamDecl(index))) {
          inputTypes.push_back(refined);
          continue;
        }
      // A planned string-cursor parameter (CTS 00204) lowers to TWO
      // inputs: a shared byte-slice over the region and an in-out i64
      // cursor. The advancement `*s = p` becomes a cursor write the
      // caller observes through the reference. Mutually exclusive with
      // the inferred-fn-ptr class above (different parameter types).
      if (cursorParams.contains(param)) {
        inputTypes.push_back(emitrust::RefType::get(
            emitrust::SliceType::get(builder.getIntegerType(8))));
        inputTypes.push_back(
            emitrust::MutRefType::get(builder.getIntegerType(64)));
        continue;
      }
      FailureOr<Type> paramType =
          mapParamType(param->getType(), translateLoc(param->getLocation()),
                       paramKinds[index]);
      if (failed(paramType))
        return failure();
      inputTypes.push_back(*paramType);
    }
  }
  SmallVector<Type> resultTypes;
  clang::QualType returnType = func->getReturnType();
  if (!returnType->isVoidType()) {
    // Returning an owned stream handle would let it escape its function
    // (C99-48 v1: no escapes); checked before the data-pointer return
    // classification below.
    if (isFilePtrType(returnType))
      return emitError(loc)
             << "unsupported: FILE* cannot cross a user-defined function "
                "boundary";
    if (isDataPointer(returnType)) {
      // A data-pointer return classifies by its return sites (CTS-P2):
      // the fn-address kind returns the plain fn_ptr value, and the
      // single-global-base kind (CTS-S, 00089) ERASES the result — the
      // classification is the null `Type` and the function imports
      // without one.
      FailureOr<Type> kind = classifyPointerReturn(func, loc);
      if (failed(kind))
        return failure();
      if (*kind)
        resultTypes.push_back(*kind);
    } else {
      FailureOr<Type> mapped = mapType(returnType, loc);
      if (failed(mapped))
        return failure();
      resultTypes.push_back(*mapped);
    }
  }
  FunctionType functionType = builder.getFunctionType(inputTypes, resultTypes);

  // Reconcile with an earlier import of the same symbol. Across TUs an
  // external prototype in one file is satisfied by the definition in another;
  // a second definition of the same external symbol is a duplicate. (Internal
  // statics are mangled per-TU, so any collision here is a genuine external
  // clash — for a valid single TU clang has already merged redeclarations.)
  if (func::FuncOp existing = functions.lookup(name)) {
    if (!isDefinition)
      return success(); // Redundant declaration.
    if (!existing.isExternal())
      return emitError(loc)
             << "unsupported: conflicting definition of '" << name
             << "' (already defined in another translation unit)";
    if (existing.getFunctionType() != functionType) {
      // A definition may refine a prototype-only import's pointer
      // parameters from scalar references to slices (the prototype's TU
      // could not see the body). The refinement is only sound while no
      // call was imported against the scalar shape.
      if (!isSliceRefinementOf(existing.getFunctionType(), functionType))
        return emitError(loc)
               << "unsupported: conflicting redeclaration of '" << name
               << "'";
      if (!SymbolTable::symbolKnownUseEmpty(existing.getOperation(),
                                            module.getOperation()))
        return emitError(loc)
               << "unsupported: function '" << name << "' was called as "
               << existing.getFunctionType()
               << " before its definition refined the signature to "
               << functionType
               << " (cross-TU pointer-parameter classification)";
    }
    existing.erase();
    functions.erase(name);
  }

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, name, functionType);
  if (methodOwner)
    funcOp->setAttr(emitrust::kMethodOfAttrName,
                    builder.getStringAttr(ownerStructType.getName()));
  functions[name] = funcOp;
  if (!isDefinition) {
    funcOp.setPrivate();
    return success();
  }

  // Function prologue: reset per-function state, then materialize each
  // parameter as a place appropriate to its kind.
  symbols.clear();
  addressTaken.clear();
  fileLocals.clear();
  pointerLocals.clear();
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  inferredFnPtrSigs = std::move(inferredSigs);
  cursorWritebacks.clear();
  currentVaCloneActive = false;
  currentVaExtras.clear();
  currentVaCursorCell = Value();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentFunctionBody = func->getBody();
  currentReceiverPlace = Value();
  currentMethodOwner = nullptr;
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  // An erased single-global-base pointer return (CTS-S, 00089): return
  // sites emit a bare `return` instead of the classified `&global`.
  currentErasedReturnBase =
      globalReturnBases.lookup(func->getCanonicalDecl());
  currentFuncName = name;
  currentIsMain = name == "c_main";
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
  // Calls to carrier-returning functions are carrier sources of the
  // assigned pointer's region (CTS-P3).
  pointerRegions.carrierReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return isCarrierReturnFunction(callee);
      };
  // Admitted local `void *` fn-ptr holders (CTS-F, 00210) import as
  // ordinary fn_ptr locals; the pointer decomposition never tracks them.
  collectVoidFnPtrHolders(func->getBody());
  pointerRegions.fnHolderQuery = [this](const clang::VarDecl *var) {
    return voidFnPtrHolders.contains(var);
  };
  pointerRegions.literalTemps = &literalTemps;
  // String-cursor parameters (CTS 00204): the walk binds `p = *s` to the
  // parameter's region and lets `&p` arguments to cursor positions pass
  // without invalidation.
  pointerRegions.cursorParamQuery = [this](const clang::ParmVarDecl *param) {
    return cursorParams.contains(param);
  };
  pointerRegions.cursorArgQuery = [this](const clang::FunctionDecl *callee,
                                         unsigned index) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return false;
    return cursorParams.contains(definition->getParamDecl(index));
  };
  pointerRegions.analyze(astContext(), func->getBody());

  // Method prologue (Phase 4): the receiver dereferences once into the
  // owner struct place, whose "data" member is the region base every
  // pointer parameter (and every pointer local unified with one)
  // decomposes against.
  Value receiverDataPlace;
  if (methodOwner) {
    Value receiverArg = entryBlock->getArgument(0);
    Value receiverPlace =
        builder
            .create<emitrust::DerefOp>(
                loc, emitrust::LValueType::get(ownerStructType), receiverArg)
            .getResult();
    FailureOr<Type> ownedType = mapType(methodOwner->getType(), loc);
    if (failed(ownedType))
      return failure();
    receiverDataPlace = builder
                            .create<emitrust::MemberOp>(
                                loc, emitrust::LValueType::get(*ownedType),
                                receiverPlace, builder.getStringAttr("data"))
                            .getResult();
    currentReceiverPlace = receiverPlace;
    currentMethodOwner = methodOwner;
  }

  unsigned entryArgIndex = methodOwner ? 1 : 0;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    // main's `argv` was dropped from the imported signature (it has no
    // entry-block argument); its uses were rejected at signature time, so
    // no binding is needed.
    if (currentIsMain && isPointerType(param->getType()))
      continue;
    Location paramLoc = translateLoc(param->getLocation());
    // A string-cursor parameter (CTS 00204) owns TWO entry-block
    // arguments: the shared region slice and the in-out cursor.
    if (cursorParams.contains(param)) {
      Value baseArg = entryBlock->getArgument(entryArgIndex);
      Value cursorArg = entryBlock->getArgument(entryArgIndex + 1);
      entryArgIndex += 2;
      if (failed(bindCursorParam(param, baseArg, cursorArg, paramLoc)))
        return failure();
      continue;
    }
    Value blockArg = entryBlock->getArgument(entryArgIndex++);
    // An integer-carrier `void *` parameter (CTS-P3) is a plain i64
    // scalar; remember it so truth tests and carrier reads route to its
    // prologue cell (bound through the ordinary scalar path below).
    if (!methodOwner && isDataPointer(param->getType()) &&
        blockArg.getType() == builder.getIntegerType(64))
      carrierParams.insert(param);
    if (methodOwner && isPointerType(param->getType()) &&
        !isFunctionPointer(param->getType())) {
      // Owner-region pointer parameter: an i64 element index into the
      // receiver's array, decomposed exactly like a slice parameter with
      // the receiver's data member as region base and the index argument
      // as the initial cursor.
      Value cursorCell =
          createEntryAlloca(paramLoc, builder.getIntegerType(64));
      builder.create<memref::StoreOp>(paramLoc, blockArg, cursorCell);
      symbols[param] = receiverDataPlace;
      pointerLocals[param] = PointerLocalInfo{param, cursorCell};
      continue;
    }
    if (failed(bindOrdinaryParam(param, blockArg, paramLoc)))
      return failure();
  }

  if (failed(emitStmt(func->getBody())))
    return failure();
  return finalizeFunction(funcOp, loc);
}

LogicalResult CImporter::bindOrdinaryParam(const clang::ParmVarDecl *param,
                                           Value blockArg, Location paramLoc) {
  Type type = blockArg.getType();
  {
    Type refPointee;
    if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(type))
      refPointee = mutRef.getPointee();
    else if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(type))
      refPointee = sharedRef.getPointee();
    if (auto sliceType =
            llvm::dyn_cast_or_null<emitrust::SliceType>(refPointee)) {
      // Slice parameter (Phase 1b): one entry-block dereference
      // establishes the region base place, and the parameter itself
      // decomposes into (base, i64 cursor = 0) exactly like a decayed
      // local array; every element access renders `(*param)[i as usize]`
      // so no borrow is ever held across statements. A shared slice
      // (`&[u8]`, the const byte-region walkers) decomposes the same
      // way; writes through it were excluded by the const pointee.
      Value basePlace =
          builder
              .create<emitrust::DerefOp>(
                  paramLoc, emitrust::LValueType::get(sliceType), blockArg)
              .getResult();
      Value cursorCell =
          createEntryAlloca(paramLoc, builder.getIntegerType(64));
      Value zero = createIntConstant(paramLoc, builder.getIntegerType(64), 0);
      builder.create<memref::StoreOp>(paramLoc, zero, cursorCell);
      symbols[param] = basePlace;
      pointerLocals[param] = PointerLocalInfo{param, cursorCell};
      return success();
    }
  }
  if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(type)) {
    // Scalar-reference pointer parameter: used directly as a reference
    // SSA value.
    symbols[param] = blockArg;
    return success();
  }
  if (llvm::isa<emitrust::StructType, emitrust::EnumType,
                emitrust::FnPtrType>(type) ||
      isUnsignedInt(type) || addressTaken.contains(param)) {
    // By-value struct, enum, function pointer, or unsigned scalar, or an
    // address-taken scalar: copy into a Rust variable (dialect-typed
    // values must not become memref cells — a memref of a dialect type
    // is illegal — and unsigned cells must not either, because mem2reg
    // materializes its default value as an `arith.constant`, which
    // requires a signless type).
    Value place = builder
                      .create<emitrust::VariableOp>(
                          paramLoc, emitrust::LValueType::get(type))
                      .getResult();
    builder.create<emitrust::AssignOp>(paramLoc, place, blockArg);
    symbols[param] = place;
    return success();
  }
  if (llvm::isa<emitrust::ArrayType>(type))
    return emitError(paramLoc) << "unsupported: array parameter";
  // Plain scalar: promotable rank-0 memref cell (swept by
  // `finalizeFunction` when the parameter is never read).
  Value cell = createEntryAlloca(paramLoc, type);
  builder.create<memref::StoreOp>(paramLoc, blockArg, cell);
  symbols[param] = cell;
  paramCells.push_back(cell);
  return success();
}

LogicalResult CImporter::bindCursorParam(const clang::ParmVarDecl *param,
                                         Value baseArg, Value cursorArg,
                                         Location paramLoc) {
  // The shared byte-slice argument derefs once into the region base
  // place, exactly like a slice parameter's; reads render
  // `(*base)[i as usize]` and never hold a borrow across statements.
  auto sliceType = emitrust::SliceType::get(builder.getIntegerType(8));
  Value basePlace =
      builder
          .create<emitrust::DerefOp>(
              paramLoc, emitrust::LValueType::get(sliceType), baseArg)
          .getResult();
  // The in-out cursor copies into a local i64 cell at entry; `*s` reads
  // and `*s = p` writes go through the cell, and every return site
  // copies it back through the reference (emitCursorWritebacks).
  IntegerType i64Type = builder.getIntegerType(64);
  Value cursorPlace =
      builder
          .create<emitrust::DerefOp>(
              paramLoc, emitrust::LValueType::get(i64Type), cursorArg)
          .getResult();
  Value initial =
      builder.create<emitrust::LoadOp>(paramLoc, i64Type, cursorPlace)
          .getResult();
  Value cell = createEntryAlloca(paramLoc, i64Type);
  builder.create<memref::StoreOp>(paramLoc, initial, cell);
  symbols[param] = basePlace;
  pointerLocals[param] = PointerLocalInfo{param, cell};
  cursorWritebacks.push_back({cell, cursorPlace});
  return success();
}

void CImporter::emitCursorWritebacks(Location loc) {
  for (auto &[cell, place] : cursorWritebacks) {
    Value value = loadPlace(loc, cell);
    builder.create<emitrust::AssignOp>(loc, place, value);
  }
}

LogicalResult CImporter::emitVaClones(const clang::FunctionDecl *func,
                                      const VaMonomorphPlan &plan) {
  for (const VaClonePlan &clone : plan.clones)
    if (failed(emitVaClone(func, clone)))
      return failure();
  return success();
}

LogicalResult CImporter::emitVaClone(const clang::FunctionDecl *func,
                                     const VaClonePlan &clone) {
  Location loc = translateLoc(func->getLocation());

  // Signature: the named parameters keep their classified shapes, the
  // site's extras append as by-value parameters — no synthetic cursor
  // parameter; the consumption cursor is an internal local.
  ArrayRef<ParamKind> paramKinds = classifyPointerParams(func);
  SmallVector<Type> inputTypes;
  for (auto [index, param] : llvm::enumerate(func->parameters())) {
    if (cursorParams.contains(param)) {
      inputTypes.push_back(emitrust::RefType::get(
          emitrust::SliceType::get(builder.getIntegerType(8))));
      inputTypes.push_back(
          emitrust::MutRefType::get(builder.getIntegerType(64)));
      continue;
    }
    FailureOr<Type> paramType =
        mapParamType(param->getType(), translateLoc(param->getLocation()),
                     paramKinds[index]);
    if (failed(paramType))
      return failure();
    inputTypes.push_back(*paramType);
  }
  unsigned namedInputCount = inputTypes.size();
  for (Type extraType : clone.extraTypes)
    inputTypes.push_back(extraType);
  SmallVector<Type> resultTypes;
  clang::QualType returnType = func->getReturnType();
  if (!returnType->isVoidType()) {
    if (isDataPointer(returnType) || isFilePtrType(returnType))
      return emitError(loc)
             << "unsupported: pointer return from a variadic definition";
    FailureOr<Type> mapped = mapType(returnType, loc);
    if (failed(mapped))
      return failure();
    resultTypes.push_back(*mapped);
  }
  FunctionType functionType = builder.getFunctionType(inputTypes, resultTypes);
  if (functions.lookup(clone.name))
    return emitError(loc) // Defensive; the planner reserved the name.
           << "unsupported: monomorphization clone name '" << clone.name
           << "' collides with an existing symbol";

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, clone.name, functionType);
  functions[clone.name] = funcOp;

  // Function prologue: the same per-clone state reset importFunction
  // performs, with the va_start/va_arg/va_end lowerings armed.
  symbols.clear();
  addressTaken.clear();
  fileLocals.clear();
  pointerLocals.clear();
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  cursorWritebacks.clear();
  currentVaCloneActive = true;
  currentVaExtras.clear();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentFunctionBody = func->getBody();
  currentReceiverPlace = Value();
  currentMethodOwner = nullptr;
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  currentErasedReturnBase = nullptr;
  currentFuncName = clone.name;
  currentIsMain = false;
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
  pointerRegions.carrierReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return isCarrierReturnFunction(callee);
      };
  collectVoidFnPtrHolders(func->getBody());
  pointerRegions.fnHolderQuery = [this](const clang::VarDecl *var) {
    return voidFnPtrHolders.contains(var);
  };
  pointerRegions.literalTemps = &literalTemps;
  pointerRegions.cursorParamQuery = [this](const clang::ParmVarDecl *param) {
    return cursorParams.contains(param);
  };
  pointerRegions.cursorArgQuery = [this](const clang::FunctionDecl *callee,
                                         unsigned index) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return false;
    return cursorParams.contains(definition->getParamDecl(index));
  };
  pointerRegions.analyze(astContext(), func->getBody());

  // Named parameter binding, then the extras: the extra block arguments
  // stay raw SSA values (structs are Copy) selected by the va_arg
  // dispatch; the consumption cursor is an entry-block i64 cell.
  unsigned entryArgIndex = 0;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    Location paramLoc = translateLoc(param->getLocation());
    if (cursorParams.contains(param)) {
      Value baseArg = entryBlock->getArgument(entryArgIndex);
      Value cursorArg = entryBlock->getArgument(entryArgIndex + 1);
      entryArgIndex += 2;
      if (failed(bindCursorParam(param, baseArg, cursorArg, paramLoc)))
        return failure();
      continue;
    }
    Value blockArg = entryBlock->getArgument(entryArgIndex++);
    if (isDataPointer(param->getType()) &&
        blockArg.getType() == builder.getIntegerType(64))
      carrierParams.insert(param);
    if (failed(bindOrdinaryParam(param, blockArg, paramLoc)))
      return failure();
  }
  for (unsigned index = namedInputCount; index < inputTypes.size(); ++index)
    currentVaExtras.push_back(entryBlock->getArgument(index));
  currentVaCursorCell =
      createEntryAlloca(loc, builder.getIntegerType(64));

  if (failed(emitStmt(func->getBody())))
    return failure();
  return finalizeFunction(funcOp, loc);
}

FailureOr<Value> CImporter::emitVaArg(const clang::VAArgExpr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  if (!currentVaCloneActive)
    return emitError(loc)
           << "unsupported: va_arg outside a variadic definition";
  FailureOr<Type> mapped = mapType(expr->getType(), loc);
  if (failed(mapped))
    return failure();
  Type type = *mapped;
  IntegerType i64Type = builder.getIntegerType(64);

  // Consume one position: read the cursor, then bump it.
  Value cursor = loadPlace(loc, currentVaCursorCell);
  Value one = createIntConstant(loc, i64Type, 1);
  Value next = builder.create<arith::AddIOp>(loc, cursor, one).getResult();
  builder.create<memref::StoreOp>(loc, next, currentVaCursorCell);

  // The dispatch selects among the extras whose static type is T. A
  // cursor position with no matching extra would be UB in the C call
  // (va_arg with the wrong type), so a deterministic panic is a legal
  // refinement.
  Value result = createVariablePlace(loc, type);
  SmallVector<unsigned, 4> candidates;
  for (auto [index, extra] : llvm::enumerate(currentVaExtras))
    if (extra.getType() == type)
      candidates.push_back(static_cast<unsigned>(index));
  auto emitPanic = [&]() {
    Attribute message = builder.getStringAttr(
        "va_arg: no fixed argument of the requested type");
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr("panic!"),
        builder.getArrayAttr({message}), ValueRange());
  };
  if (candidates.empty()) {
    // No extra of this type exists in the clone at all (e.g. a
    // struct-typed va_arg inside a clone whose site passed only ints):
    // reaching this read at runtime is unconditionally UB in C.
    emitPanic();
    return loadPlace(loc, result);
  }
  Block *contBlock = createBlock();
  for (unsigned candidate : candidates) {
    Value expected = createIntConstant(loc, i64Type, candidate);
    Value matches = builder
                        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                               cursor, expected)
                        .getResult();
    Block *matchBlock = createBlock();
    Block *nextBlock = createBlock();
    builder.create<cf::CondBranchOp>(loc, matches, matchBlock, ValueRange(),
                                     nextBlock, ValueRange());
    builder.setInsertionPointToEnd(matchBlock);
    builder.create<emitrust::AssignOp>(loc, result,
                                       currentVaExtras[candidate]);
    builder.create<cf::BranchOp>(loc, contBlock);
    builder.setInsertionPointToEnd(nextBlock);
  }
  emitPanic();
  builder.create<cf::BranchOp>(loc, contBlock);
  builder.setInsertionPointToEnd(contBlock);
  return loadPlace(loc, result);
}

LogicalResult
CImporter::planCursorParams(const clang::TranslationUnitDecl *unit) {
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!func || !func->isThisDeclarationADefinition() || !func->hasBody())
      continue;
    // C main's `char **argv` has its own policy (dropped from the
    // imported signature; uses rejected) — never a string cursor.
    if (func->isMain())
      continue;
    SmallVector<const clang::ParmVarDecl *, 2> eligible;
    for (const clang::ParmVarDecl *param : func->parameters())
      if (isCharPointerPointerType(param->getType()))
        eligible.push_back(param);
    if (eligible.empty())
      continue;
    // The bounded shape: the parameter appears only under its own
    // dereference — reads and the `*s = p` advancement. Everything else
    // (stored, passed on, address-taken, content writes) escapes.
    for (const clang::ParmVarDecl *param : eligible)
      if (const clang::Expr *escape =
              findCursorParamEscape(func->getBody(), param))
        return emitError(translateLoc(escape->getBeginLoc()))
               << "unsupported: pointer-to-pointer parameter escapes the "
                  "string-cursor shape";
    // Region check: a write through a pointer DERIVED from the cursor
    // parameter (`p = *s; *p = c;`) writes region content the shared
    // slice lowering cannot accept.
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 2> candidates(
        eligible.begin(), eligible.end());
    PointerRegionAnalysis analysis;
    analysis.cursorParamQuery = [&](const clang::ParmVarDecl *param) {
      return candidates.contains(param);
    };
    analysis.analyze(astContext(), func->getBody());
    SmallVector<const clang::VarDecl *, 8> locals;
    collectLocalPointerDecls(func->getBody(), locals);
    for (const clang::VarDecl *var : locals) {
      const PointerRegion *region = analysis.regionOf(var);
      if (!region || !region->hasWriteThrough)
        continue;
      for (const PointerBaseBinding &binding : region->bases)
        if (const auto *param =
                llvm::dyn_cast_if_present<clang::ParmVarDecl>(binding.base);
            param && candidates.contains(param))
          return emitError(translateLoc(region->writeThroughLoc))
                 << "unsupported: write through a string-cursor parameter";
    }
    for (const clang::ParmVarDecl *param : eligible)
      cursorParams.insert(param);
  }
  return success();
}

LogicalResult
CImporter::planVaMonomorph(const clang::TranslationUnitDecl *unit) {
  // Gather this TU's va_list-using variadic definitions.
  SmallVector<const clang::FunctionDecl *, 4> targets;
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!func || !func->isThisDeclarationADefinition() || !func->hasBody() ||
        !func->isVariadic())
      continue;
    if (bodyUsesVaList(astContext(), func->getBody()))
      targets.push_back(func);
  }
  if (targets.empty())
    return success();

  // Scope checks per definition: no va_copy, and the va_list objects may
  // only ever feed va_start/va_end/va_arg — a va_list passed to any
  // callee escapes the definition (the callee would consume varargs the
  // monomorphizer cannot see). Checked BEFORE any declaration imports,
  // so this diagnostic beats the callee's va_list-parameter rejection.
  clang::QualType vaListType =
      astContext().getBuiltinVaListType().getCanonicalType();
  for (const clang::FunctionDecl *func : targets) {
    llvm::SmallPtrSet<const clang::Expr *, 8> consumed;
    SmallVector<const clang::Stmt *> worklist{func->getBody()};
    while (!worklist.empty()) {
      const clang::Stmt *current = worklist.pop_back_val();
      if (!current)
        continue;
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(current)) {
        switch (call->getBuiltinCallee()) {
        case clang::Builtin::BI__builtin_va_copy:
        case clang::Builtin::BI__builtin_ms_va_copy:
        case clang::Builtin::BIva_copy:
          return emitError(translateLoc(call->getBeginLoc()))
                 << "unsupported: va_copy";
        case clang::Builtin::BI__builtin_va_start:
        case clang::Builtin::BI__builtin_c23_va_start:
        case clang::Builtin::BI__builtin_ms_va_start:
        case clang::Builtin::BI__va_start:
        case clang::Builtin::BIva_start:
        case clang::Builtin::BI__builtin_va_end:
        case clang::Builtin::BI__builtin_ms_va_end:
        case clang::Builtin::BIva_end:
          if (call->getNumArgs() >= 1)
            consumed.insert(strippedImplicitRef(call->getArg(0)));
          break;
        default:
          break;
        }
      }
      if (const auto *vaArg = llvm::dyn_cast<clang::VAArgExpr>(current))
        consumed.insert(strippedImplicitRef(vaArg->getSubExpr()));
      for (const clang::Stmt *child : current->children())
        worklist.push_back(child);
    }
    worklist.push_back(func->getBody());
    while (!worklist.empty()) {
      const clang::Stmt *current = worklist.pop_back_val();
      if (!current)
        continue;
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(current)) {
        const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
        if (var &&
            astContext().hasSameType(var->getType().getCanonicalType(),
                                     vaListType) &&
            !consumed.contains(ref))
          return emitError(translateLoc(ref->getBeginLoc()))
                 << "unsupported: va_list escapes variadic definition";
      }
      for (const clang::Stmt *child : current->children())
        worklist.push_back(child);
    }
  }

  // Address-of scan and call-site enumeration over the whole TU, in
  // declaration order (pre-order within each body), so clone numbering is
  // deterministic. A reference to a monomorphized definition outside a
  // direct-callee position makes its call sites non-enumerable.
  llvm::DenseMap<const clang::FunctionDecl *, const clang::FunctionDecl *>
      canonicalTargets;
  for (const clang::FunctionDecl *func : targets)
    canonicalTargets[func->getCanonicalDecl()] = func;
  struct SiteRecord {
    const clang::CallExpr *call;
    const clang::FunctionDecl *target;
  };
  SmallVector<SiteRecord, 8> sites;
  llvm::SmallPtrSet<const clang::Expr *, 16> calleeRefs;
  std::function<LogicalResult(const clang::Stmt *)> scan =
      [&](const clang::Stmt *stmt) -> LogicalResult {
    if (!stmt)
      return success();
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      const clang::FunctionDecl *target =
          callee ? canonicalTargets.lookup(callee->getCanonicalDecl())
                 : nullptr;
      if (target) {
        sites.push_back({call, target});
        calleeRefs.insert(strippedImplicitRef(call->getCallee()));
      }
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
      if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
        if (canonicalTargets.count(fn->getCanonicalDecl()) &&
            !calleeRefs.contains(ref))
          return emitError(translateLoc(ref->getBeginLoc()))
                 << "unsupported: address of variadic definition";
    for (const clang::Stmt *child : stmt->children())
      if (failed(scan(child)))
        return failure();
    return success();
  };
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->isThisDeclarationADefinition() && func->hasBody() &&
          failed(scan(func->getBody())))
        return failure();
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      if (var->hasInit() && failed(scan(var->getInit())))
        return failure();
  }

  // One clone per distinct extras signature; call sites record their
  // clone index. Every target gets a plan entry — a site-less definition
  // plans zero clones and drops entirely at import.
  for (const clang::FunctionDecl *func : targets)
    (void)vaMonomorphPlans[func->getCanonicalDecl()];
  for (const SiteRecord &site : sites) {
    VaMonomorphPlan &plan =
        vaMonomorphPlans[site.target->getCanonicalDecl()];
    unsigned named = site.target->getNumParams();
    if (site.call->getNumArgs() < named)
      return emitError(translateLoc(site.call->getBeginLoc()))
             << "unsupported: call argument count mismatch";
    SmallVector<Type, 4> extraTypes;
    for (unsigned index = named; index < site.call->getNumArgs(); ++index) {
      const clang::Expr *argument = site.call->getArg(index);
      Location argLoc = translateLoc(argument->getBeginLoc());
      // Extras pass BY VALUE (clang has already applied the default
      // argument promotions); a data-pointer extra has no by-value
      // representation under the decomposition.
      if (isDataPointer(argument->getType()))
        return emitError(argLoc)
               << "unsupported: pointer argument to a variadic call";
      FailureOr<Type> mapped = mapType(argument->getType(), argLoc);
      if (failed(mapped))
        return failure();
      extraTypes.push_back(*mapped);
    }
    unsigned cloneIndex = plan.clones.size();
    for (auto [index, clone] : llvm::enumerate(plan.clones))
      if (clone.extraTypes == extraTypes) {
        cloneIndex = static_cast<unsigned>(index);
        break;
      }
    if (cloneIndex == plan.clones.size()) {
      // Clone names carry the original symbol plus a per-signature
      // suffix; the original bare symbol is never emitted.
      std::string name = mlirFuncName(site.target) + "__" +
                         std::to_string(plan.clones.size() + 1);
      while (ordinaryNameTaken(name) || functions.lookup(name))
        name += "_";
      plan.clones.push_back(VaClonePlan{name, extraTypes});
    }
    vaCallSiteClones[site.call] = cloneIndex;
  }
  return success();
}

LogicalResult CImporter::importTranslationUnit(clang::ASTContext &context,
                                               llvm::StringRef tuTag,
                                               bool deferExtern,
                                               bool soleTranslationUnit) {
  astContextPtr = &context;
  currentTuTag = tuTag.str();
  deferExternGlobals = deferExtern;
  currentSoleTU = soleTranslationUnit;
  const clang::TranslationUnitDecl *unit = astContext().getTranslationUnitDecl();
  // Namespace pre-pass: record every module-symbol name this TU's ordinary
  // identifier namespace will claim, so struct tag naming
  // (`structSymbolName`) is independent of declaration order.
  collectOrdinaryNames(unit);
  // Phase-4 Pass A: pure-AST owner planning over every function definition
  // before any IR is built; Pass B below consults the plans.
  planOwners(unit, soleTranslationUnit);
  // CTS-P10 Pass A: cell-slice classification of pointer-parameter
  // classes whose bases are all mutable global arrays.
  planCellSlices(unit, soleTranslationUnit);
  // CTS-S Pass A: per-TU fn-ptr facts (written globals, address-taken
  // functions) and the devirtualization aliases of never-reassigned
  // global function pointers.
  planFnPtrAliases(unit);
  // CTS 00204 Pass A: string-cursor parameter plans and va_list
  // monomorphization plans. Both run BEFORE any declaration imports so
  // their located rejections (escape shapes, va_copy, address-of) beat
  // the type rejections importing a callee prototype would raise.
  if (failed(planCursorParams(unit)))
    return failure();
  if (failed(planVaMonomorph(unit)))
    return failure();
  // CTS-BR (00216) Pass A: `void *` struct members whose every stored
  // value is the address of a function of one signature retype to
  // fn_ptr members; declaration-type record uses gate the eager import
  // of empty structs.
  planFnPtrMembers(unit);
  collectDeclTypeRecords(unit);
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit())
      continue;
    // System-header declarations (angle-bracket includes, `-isystem`) are
    // skipped instead of imported eagerly: real libc headers are full of
    // constructs outside the supported subset (anonymous structs in
    // bits/types.h, variadic prototypes, ...), and a program that never
    // touches them must not be rejected for their sake. A main-file use of
    // a skipped declaration is rejected at the use site (see
    // `rejectSystemHeaderUse`); types are still imported on demand through
    // `mapType`. Project headers included via `-I` are not system headers
    // and keep the whole-file fail-fast import.
    if (isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (failed(importFunction(func)))
        return failure();
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
      // An EMPTY struct that no declaration type mentions is skipped: it
      // may only ever appear as a zero-byte member of a byte-region
      // aggregate (CTS-BR, 00216), which never materializes the record
      // type at all. One that IS declared with keeps the eager import.
      if (const clang::RecordDecl *definition = record->getDefinition();
          definition && definition->isStruct() && definition->field_empty() &&
          !declTypeUsedRecords.contains(definition))
        continue;
      if (failed(importRecord(record, translateLoc(record->getBeginLoc()))))
        return failure();
      continue;
    }
    if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
      if (failed(importEnum(enumDecl, translateLoc(enumDecl->getBeginLoc()))))
        return failure();
      continue;
    }
    if (llvm::isa<clang::TypedefDecl>(decl) || llvm::isa<clang::EmptyDecl>(decl))
      continue;
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      if (failed(importGlobalVar(var)))
        return failure();
      continue;
    }
    return emitError(translateLoc(decl->getBeginLoc()))
           << "unsupported top-level declaration";
  }
  if (needsFloatFormatHelper && !floatFormatHelperEmitted) {
    floatFormatHelperEmitted = true;
    // C-compatible `%f` rendering: `{:.6}` matches C for finite values and
    // infinities, but Rust spells NaN as "NaN" where C prints "nan" with a
    // leading '-' when the sign bit is set. Emitted once per module, after
    // all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_f64(x: f64) -> String {\n"
            "    if x.is_nan() {\n"
            "        if x.is_sign_negative() { String::from(\"-nan\") } "
            "else { String::from(\"nan\") }\n"
            "    } else {\n"
            "        format!(\"{:.6}\", x)\n"
            "    }\n"
            "}"));
  }
  if (needsCharFormatHelper && !charFormatHelperEmitted) {
    charFormatHelperEmitted = true;
    // C-compatible `%c`/putchar rendering: C converts the int argument to
    // unsigned char and writes that byte; `(x as u8) as char` emits the
    // identical byte for every ASCII value (0..=127). Values 128..=255
    // would render as two-byte UTF-8 and are documented as out of scope
    // (design.md C99-48). Emitted once per module, after all imported
    // items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr("fn __emitrust_fmt_c(x: i32) -> char {\n"
                                    "    (x as u8) as char\n"
                                    "}"));
  }
  if (needsCStrHelper && !cStrHelperEmitted) {
    cStrHelperEmitted = true;
    // C-compatible `%s` rendering of a char array: C prints bytes up to
    // (not including) the first NUL, which `take_while` mirrors; the
    // per-byte `u8 as char` conversion is exact for ASCII contents (the
    // importer rejects non-ASCII string data, design.md C99-47). Emitted
    // once per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_cstr(s: &[i8]) -> String {\n"
            "    s.iter().take_while(|&&b| b != 0).map(|&b| (b as u8) as "
            "char).collect()\n"
            "}"));
  }
  if (needsCStrNHelper && !cStrNHelperEmitted) {
    cStrNHelperEmitted = true;
    // C-compatible `%.Ns` rendering of a char region: C writes at most N
    // bytes and stops earlier at a NUL; under a bounding precision the
    // region may legally lack a terminator (C99 7.19.6.1p8), which
    // `take(n)` mirrors by stopping at the slice end. ASCII-only like
    // `__emitrust_cstr`. Emitted once per module, after all imported
    // items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_cstr_n(s: &[i8], n: i64) -> String {\n"
            "    s.iter().take(n as usize).take_while(|&&b| b != 0)"
            ".map(|&b| (b as u8) as char).collect()\n"
            "}"));
  }
  if ((needsIntFormatSignedHelper || needsIntFormatUnsignedHelper) &&
      !intFormatCoreHelperEmitted) {
    intFormatCoreHelperEmitted = true;
    // C99 7.19.6.1 integer directive rendering, shared by the signed and
    // unsigned wrappers: precision zero-pads the digits (and a zero value
    // with precision zero prints nothing), '#' forces the leading octal
    // zero or the 0x/0X prefix, the sign/prefix sit inside the '0' width
    // padding, '0' is ignored next to '-' or a precision — all exactly
    // C's rules (validated byte-exactly against glibc). Flag bits:
    // '-'=1, '0'=2, '+'=4, ' '=8, '#'=16, uppercase=32.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_int(neg: bool, mag: u64, base: i32, "
            "prec: i32,\n"
            "                      width: i32, flags: i32) -> String {\n"
            "    let minus = flags & 1 != 0;\n"
            "    let zero = flags & 2 != 0 && prec < 0 && !minus;\n"
            "    let plus = flags & 4 != 0;\n"
            "    let space = flags & 8 != 0;\n"
            "    let alt = flags & 16 != 0;\n"
            "    let upper = flags & 32 != 0;\n"
            "    let mut digits = if mag == 0 && prec == 0 {\n"
            "        String::new()\n"
            "    } else if base == 8 {\n"
            "        format!(\"{:o}\", mag)\n"
            "    } else if base == 16 && upper {\n"
            "        format!(\"{:X}\", mag)\n"
            "    } else if base == 16 {\n"
            "        format!(\"{:x}\", mag)\n"
            "    } else {\n"
            "        format!(\"{}\", mag)\n"
            "    };\n"
            "    if prec > digits.len() as i32 {\n"
            "        digits = \"0\".repeat(prec as usize - digits.len()) "
            "+ &digits;\n"
            "    }\n"
            "    if alt && base == 8 && !digits.starts_with('0') {\n"
            "        digits.insert(0, '0');\n"
            "    }\n"
            "    let prefix = if alt && base == 16 && mag != 0 {\n"
            "        if upper { \"0X\" } else { \"0x\" }\n"
            "    } else {\n"
            "        \"\"\n"
            "    };\n"
            "    let sign = if neg { \"-\" } else if plus { \"+\" }\n"
            "               else if space { \" \" } else { \"\" };\n"
            "    let used = sign.len() + prefix.len() + digits.len();\n"
            "    let pad = if width > used as i32 { width as usize - used "
            "} else { 0 };\n"
            "    if minus {\n"
            "        format!(\"{}{}{}{}\", sign, prefix, digits, "
            "\" \".repeat(pad))\n"
            "    } else if zero {\n"
            "        format!(\"{}{}{}{}\", sign, prefix, "
            "\"0\".repeat(pad), digits)\n"
            "    } else {\n"
            "        format!(\"{}{}{}{}\", \" \".repeat(pad), sign, "
            "prefix, digits)\n"
            "    }\n"
            "}"));
  }
  if (needsIntFormatSignedHelper && !intFormatSignedHelperEmitted) {
    intFormatSignedHelperEmitted = true;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_i64(x: i64, prec: i32, width: i32, "
            "flags: i32) -> String {\n"
            "    __emitrust_fmt_int(x < 0, x.unsigned_abs(), 10, prec, "
            "width, flags)\n"
            "}"));
  }
  if (needsIntFormatUnsignedHelper && !intFormatUnsignedHelperEmitted) {
    intFormatUnsignedHelperEmitted = true;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_u64(x: u64, base: i32, prec: i32, "
            "width: i32,\n"
            "                      flags: i32) -> String {\n"
            "    __emitrust_fmt_int(false, x, base, prec, width, flags)\n"
            "}"));
  }
  if (needsFloatFormatExtHelper && !floatFormatExtHelperEmitted) {
    floatFormatExtHelperEmitted = true;
    // Exact C99 f/e/g floating rendering over Rust's correctly-rounded
    // decimal conversion ({:.*} and {:.*e} are exact for every finite
    // f64, and round half-to-even on the exact value like glibc).
    // `__emitrust_fmt_edigits` extracts correctly-rounded e-style digits
    // and reports whether rounding carried into the next decade (glibc's
    // %#g drops the mantissa fraction exactly when that carry lands on
    // ev == P; an exact power of ten keeps it). Non-finite values pad
    // with spaces even under '0', as glibc does. Validated byte-exactly
    // against glibc across structured and fuzzed batteries. Flag bits as
    // in `__emitrust_fmt_int`; conv: 0=f, 1=e, 2=g.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_exp(m: &str, ev: i32, alt: bool, "
            "upper: bool) -> String {\n"
            "    let mut m = String::from(m);\n"
            "    if alt && !m.contains('.') {\n"
            "        m.push('.');\n"
            "    }\n"
            "    format!(\"{}{}{}{:02}\", m, if upper { 'E' } else "
            "{ 'e' },\n"
            "            if ev < 0 { '-' } else { '+' }, ev.abs())\n"
            "}\n"
            "\n"
            "fn __emitrust_fmt_edigits(mag: f64, prec: usize) -> "
            "(String, i32, bool) {\n"
            "    let s = format!(\"{:.*e}\", prec, mag);\n"
            "    let (m, e) = match s.split_once('e') {\n"
            "        Some(t) => t,\n"
            "        None => (s.as_str(), \"0\"),\n"
            "    };\n"
            "    let ev: i32 = match e.parse() {\n"
            "        Ok(v) => v,\n"
            "        Err(_) => 0,\n"
            "    };\n"
            "    let pre: i32 = {\n"
            "        let t = format!(\"{:e}\", mag);\n"
            "        match t.split_once('e') {\n"
            "            Some((_, te)) => te.parse().unwrap_or(0),\n"
            "            None => 0,\n"
            "        }\n"
            "    };\n"
            "    let int_len = m.find('.').unwrap_or(m.len());\n"
            "    if int_len == 1 {\n"
            "        return (String::from(m), ev, ev != pre);\n"
            "    }\n"
            "    let mut out = String::from(\"1\");\n"
            "    if prec > 0 {\n"
            "        out.push('.');\n"
            "        out.push_str(&\"0\".repeat(prec));\n"
            "    }\n"
            "    (out, ev + 1, true)\n"
            "}\n"
            "\n"
            "fn __emitrust_fmt_float(x: f64, conv: i32, prec: i32, "
            "width: i32,\n"
            "                        flags: i32) -> String {\n"
            "    let minus = flags & 1 != 0;\n"
            "    let zero = flags & 2 != 0 && !minus;\n"
            "    let plus = flags & 4 != 0;\n"
            "    let space = flags & 8 != 0;\n"
            "    let alt = flags & 16 != 0;\n"
            "    let upper = flags & 32 != 0;\n"
            "    let p = if prec < 0 { 6usize } else { prec as usize };\n"
            "    let sign = if x.is_sign_negative() { \"-\" } else if "
            "plus { \"+\" }\n"
            "               else if space { \" \" } else { \"\" };\n"
            "    let (body, numeric) = if x.is_nan() {\n"
            "        (String::from(if upper { \"NAN\" } else { \"nan\" })"
            ", false)\n"
            "    } else if x.is_infinite() {\n"
            "        (String::from(if upper { \"INF\" } else { \"inf\" })"
            ", false)\n"
            "    } else {\n"
            "        let mag = x.abs();\n"
            "        let text = if conv == 0 {\n"
            "            let mut t = format!(\"{:.*}\", p, mag);\n"
            "            if alt && p == 0 {\n"
            "                t.push('.');\n"
            "            }\n"
            "            t\n"
            "        } else if conv == 1 {\n"
            "            let (m, ev, _) = __emitrust_fmt_edigits(mag, p);\n"
            "            __emitrust_fmt_exp(&m, ev, alt, upper)\n"
            "        } else {\n"
            "            let pp = if p == 0 { 1 } else { p };\n"
            "            let (m, ev, carried) = "
            "__emitrust_fmt_edigits(mag, pp - 1);\n"
            "            if ev < -4 || ev >= pp as i32 {\n"
            "                let mut m = if carried && ev == pp as i32 {\n"
            "                    String::from(\"1\")\n"
            "                } else {\n"
            "                    m\n"
            "                };\n"
            "                if !alt && m.contains('.') {\n"
            "                    m = String::from(\n"
            "                        m.trim_end_matches('0')"
            ".trim_end_matches('.'));\n"
            "                }\n"
            "                __emitrust_fmt_exp(&m, ev, alt, upper)\n"
            "            } else {\n"
            "                let digits: String =\n"
            "                    m.chars().filter(|c| *c != '.')"
            ".collect();\n"
            "                let mut t = if ev >= 0 {\n"
            "                    let ip = ev as usize + 1;\n"
            "                    if digits.len() > ip {\n"
            "                        format!(\"{}.{}\", &digits[..ip], "
            "&digits[ip..])\n"
            "                    } else {\n"
            "                        String::from(&digits[..ip])\n"
            "                    }\n"
            "                } else {\n"
            "                    format!(\"0.{}{}\", "
            "\"0\".repeat((-ev - 1) as usize), digits)\n"
            "                };\n"
            "                if !alt && t.contains('.') {\n"
            "                    t = String::from(\n"
            "                        t.trim_end_matches('0')"
            ".trim_end_matches('.'));\n"
            "                }\n"
            "                if alt && !t.contains('.') {\n"
            "                    t.push('.');\n"
            "                }\n"
            "                t\n"
            "            }\n"
            "        };\n"
            "        (text, true)\n"
            "    };\n"
            "    let used = sign.len() + body.len();\n"
            "    let pad = if width > used as i32 { width as usize - used "
            "} else { 0 };\n"
            "    if minus {\n"
            "        format!(\"{}{}{}\", sign, body, \" \".repeat(pad))\n"
            "    } else if zero && numeric {\n"
            "        format!(\"{}{}{}\", sign, \"0\".repeat(pad), body)\n"
            "    } else {\n"
            "        format!(\"{}{}{}\", \" \".repeat(pad), sign, body)\n"
            "    }\n"
            "}"));
  }
  if (needsSprintfHelper && !sprintfHelperEmitted) {
    sprintfHelperEmitted = true;
    // C-compatible sprintf tail: copies the formatted ASCII bytes plus the
    // terminating NUL into the destination char region and returns the
    // written length (excluding the NUL), C's sprintf result. Every write
    // is a bounds-checked slice index, so a destination too small for the
    // bytes plus the NUL panics — C leaves that overflow undefined, and
    // the deterministic panic is a legal refinement. Emitted once per
    // module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_sprintf(dest: &mut [i8], s: &str) -> i32 {\n"
            "    let bytes = s.as_bytes();\n"
            "    for (i, &b) in bytes.iter().enumerate() {\n"
            "        dest[i] = b as i8;\n"
            "    }\n"
            "    dest[bytes.len()] = 0;\n"
            "    bytes.len() as i32\n"
            "}"));
  }
  // Hosted <string.h> helpers (design.md C99-48, CTS-L1): each requested
  // helper is emitted once per module, in this fixed order, as safe Rust
  // over i8 slices. Every access is a bounds-checked slice index — the
  // borrowed regions are compile-time-sized char arrays (or literal
  // backings, which always end in a NUL) — so a C program whose behavior
  // is undefined (a missing terminator, an out-of-range count) panics
  // instead of reading out of bounds. Comparisons compare as unsigned
  // char, exactly C's rule.
  static const struct {
    llvm::StringRef name;
    llvm::StringRef source;
  } kStringHelpers[] = {
      {"__emitrust_strcpy",
       "fn __emitrust_strcpy(dst: &mut [i8], src: &[i8]) {\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        let b = src[i];\n"
       "        dst[i] = b;\n"
       "        if b == 0 { break; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strncpy",
       "fn __emitrust_strncpy(dst: &mut [i8], src: &[i8], n: i64) {\n"
       "    let mut ended = false;\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let b = if ended { 0 } else { src[i] };\n"
       "        if b == 0 { ended = true; }\n"
       "        dst[i] = b;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strcat",
       "fn __emitrust_strcat(dst: &mut [i8], src: &[i8]) {\n"
       "    let mut d = 0usize;\n"
       "    while dst[d] != 0 { d += 1; }\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        let b = src[i];\n"
       "        dst[d + i] = b;\n"
       "        if b == 0 { break; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strcmp",
       "fn __emitrust_strcmp(a: &[i8], b: &[i8]) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        let x = a[i] as u8;\n"
       "        let y = b[i] as u8;\n"
       "        if x != y || x == 0 { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strncmp",
       "fn __emitrust_strncmp(a: &[i8], b: &[i8], n: i64) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let x = a[i] as u8;\n"
       "        let y = b[i] as u8;\n"
       "        if x != y || x == 0 { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "    0\n"
       "}"},
      {"__emitrust_strchr",
       "fn __emitrust_strchr(s: &[i8], c: i32) -> i64 {\n"
       "    let c = c as u8 as i8;\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        if s[i] == c { return i as i64; }\n"
       "        if s[i] == 0 { return -1; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strrchr",
       "fn __emitrust_strrchr(s: &[i8], c: i32) -> i64 {\n"
       "    let c = c as u8 as i8;\n"
       "    let mut last: i64 = -1;\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        if s[i] == c { last = i as i64; }\n"
       "        if s[i] == 0 { return last; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memset",
       "fn __emitrust_memset(s: &mut [i8], c: i32, n: i64) {\n"
       "    let b = c as u8 as i8;\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        s[i] = b;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memcpy",
       "fn __emitrust_memcpy(dst: &mut [i8], src: &[i8], n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        dst[i] = src[i];\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memcpy_within",
       "fn __emitrust_memcpy_within(s: &mut [i8], dst: i64, src: i64, n: "
       "i64) {\n"
       "    s.copy_within(src as usize..(src + n) as usize, dst as usize);\n"
       "}"},
      {"__emitrust_atoi",
       // C's atoi (7.20.1.2): skip isspace bytes (space and 0x09..0x0D),
       // one optional sign, then decimal digits to the first non-digit;
       // no digits yields 0. The accumulator counts downward so INT_MIN
       // parses exactly, and out-of-range values — C UB (7.20.1p1) — are
       // refined to deterministic i32 wrapping. A region with neither a
       // NUL nor a non-digit before its end simply stops at the end
       // (reading past the array is C UB, refined).
       "fn __emitrust_atoi(s: &[i8]) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while i < s.len() {\n"
       "        let b = s[i] as u8;\n"
       "        if b != b' ' && (b < 9 || b > 13) { break; }\n"
       "        i += 1;\n"
       "    }\n"
       "    let mut neg = false;\n"
       "    if i < s.len() {\n"
       "        let b = s[i] as u8;\n"
       "        if b == b'+' || b == b'-' {\n"
       "            neg = b == b'-';\n"
       "            i += 1;\n"
       "        }\n"
       "    }\n"
       "    let mut acc: i32 = 0;\n"
       "    while i < s.len() {\n"
       "        let b = s[i] as u8;\n"
       "        if b < b'0' || b > b'9' { break; }\n"
       "        acc = acc.wrapping_mul(10).wrapping_sub((b - b'0') as "
       "i32);\n"
       "        i += 1;\n"
       "    }\n"
       "    if neg { acc } else { acc.wrapping_neg() }\n"
       "}"},
      {"__emitrust_memcmp",
       "fn __emitrust_memcmp(a: &[i8], b: &[i8], n: i64) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let x = a[i] as u8;\n"
       "        let y = b[i] as u8;\n"
       "        if x != y { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "    0\n"
       "}"},
  };
  for (const auto &helper : kStringHelpers) {
    if (!neededStringHelpers.contains(helper.name) ||
        emittedStringHelpers.contains(helper.name))
      continue;
    emittedStringHelpers.insert(helper.name);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(helper.source));
  }
  if (needsStrlenHelper && !strlenHelperEmitted) {
    strlenHelperEmitted = true;
    // C-compatible strlen over a string-literal region: counts bytes up to
    // (not including) the first NUL. The backing of every string-literal
    // region includes the terminating NUL, so `position` always finds one;
    // the `unwrap_or` fallback merely keeps the helper total. Emitted once
    // per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_strlen(s: &[i8]) -> i64 {\n"
            "    s.iter().position(|&b| b == 0).unwrap_or(s.len()) as i64\n"
            "}"));
  }
  // Hosted <stdio.h> FILE* helpers (design.md C99-48, CTS-T1.3): the
  // owned-handle enum plus one safe-Rust helper per lowered stream
  // operation, each requested by `requestFileHelper` and emitted once per
  // module in this fixed order (the enum first; fread/fgets pull in the
  // fgetc primitive they call). fopen failure is C's NULL path
  // (`__EmitrustFile::Null`); every use of a Null or wrong-direction
  // handle, an I/O error beyond end-of-file, and an out-of-range count
  // is C undefined behavior refined into a deterministic panic (the
  // slice indexing is bounds-checked).
  static const struct {
    llvm::StringRef name;
    llvm::StringRef source;
  } kFileHelpers[] = {
      {"__EmitrustFile",
       "/// An owned C `FILE*` stream handle over std::fs (design.md\n"
       "/// C99-48). `Null` is C's NULL: a failed fopen, or the state\n"
       "/// fclose leaves so the same variable can be reopened. Streams\n"
       "/// are sequential-only and byte-wise; using a Null or\n"
       "/// wrong-direction handle is C UB, refined into a deterministic\n"
       "/// panic by the helpers below.\n"
       "enum __EmitrustFile {\n"
       "    Null,\n"
       "    Read(std::fs::File),\n"
       "    Write(std::fs::File),\n"
       "}"},
      {"__emitrust_fopen_r",
       "/// `fopen(path, \"r\")`: opens an existing file for sequential\n"
       "/// reading; `Null` is C's NULL result when the open fails.\n"
       "fn __emitrust_fopen_r(path: &str) -> __EmitrustFile {\n"
       "    match std::fs::File::open(path) {\n"
       "        Ok(f) => __EmitrustFile::Read(f),\n"
       "        Err(_) => __EmitrustFile::Null,\n"
       "    }\n"
       "}"},
      {"__emitrust_fopen_w",
       "/// `fopen(path, \"w\")`: creates or truncates a file for\n"
       "/// sequential writing; `Null` is C's NULL result when the open\n"
       "/// fails.\n"
       "fn __emitrust_fopen_w(path: &str) -> __EmitrustFile {\n"
       "    match std::fs::File::create(path) {\n"
       "        Ok(f) => __EmitrustFile::Write(f),\n"
       "        Err(_) => __EmitrustFile::Null,\n"
       "    }\n"
       "}"},
      {"__emitrust_file_ok",
       "/// The truth of a C `FILE*` handle: false exactly when it is\n"
       "/// NULL (the `if (!f)` check after fopen).\n"
       "fn __emitrust_file_ok(f: &__EmitrustFile) -> bool {\n"
       "    !matches!(f, __EmitrustFile::Null)\n"
       "}"},
      {"__emitrust_fgetc",
       "/// `fgetc`/`getc`: one byte as 0..=255, or -1 (C's EOF) at end\n"
       "/// of file. Reading a NULL or write-mode stream is C UB, and an\n"
       "/// I/O error beyond end-of-file has no C-visible result either;\n"
       "/// both panic deterministically.\n"
       "fn __emitrust_fgetc(f: &mut __EmitrustFile) -> i32 {\n"
       "    match f {\n"
       "        __EmitrustFile::Read(file) => {\n"
       "            let mut byte = [0u8; 1];\n"
       "            match std::io::Read::read(file, &mut byte) {\n"
       "                Ok(0) => -1,\n"
       "                Ok(_) => byte[0] as i32,\n"
       "                Err(e) => panic!(\"fgetc: {}\", e),\n"
       "            }\n"
       "        }\n"
       "        _ => panic!(\"fgetc on a stream not open for reading\"),\n"
       "    }\n"
       "}"},
      {"__emitrust_fread",
       "/// Byte-wise `fread(ptr, 1, n, f)`: reads up to `n` bytes into\n"
       "/// the destination's prefix and returns the count actually read\n"
       "/// (short at end of file). A count exceeding the destination\n"
       "/// panics on the bounds-checked index (C UB, refined).\n"
       "fn __emitrust_fread(f: &mut __EmitrustFile, buf: &mut [i8], n: i64) "
       "-> i64 {\n"
       "    let mut count = 0i64;\n"
       "    while count < n {\n"
       "        let c = __emitrust_fgetc(f);\n"
       "        if c < 0 {\n"
       "            break;\n"
       "        }\n"
       "        buf[count as usize] = c as i8;\n"
       "        count += 1;\n"
       "    }\n"
       "    count\n"
       "}"},
      {"__emitrust_fwrite",
       "/// Byte-wise `fwrite(ptr, 1, n, f)`: writes the source's first\n"
       "/// `n` bytes and returns `n`, C's full-success result. A write\n"
       "/// error, a stream not open for writing, and a count exceeding\n"
       "/// the source all panic deterministically (C UB, refined).\n"
       "fn __emitrust_fwrite(f: &mut __EmitrustFile, buf: &[i8], n: i64) "
       "-> i64 {\n"
       "    match f {\n"
       "        __EmitrustFile::Write(file) => {\n"
       "            let bytes: Vec<u8> =\n"
       "                buf[..n as usize].iter().map(|&b| b as u8).collect();\n"
       "            std::io::Write::write_all(file, &bytes)\n"
       "                .expect(\"fwrite failed\");\n"
       "            n\n"
       "        }\n"
       "        _ => panic!(\"fwrite on a stream not open for writing\"),\n"
       "    }\n"
       "}"},
      {"__emitrust_fgets",
       "/// `fgets(buf, size, f)`: reads at most `size - 1` bytes,\n"
       "/// stopping after a newline, and NUL-terminates what was read.\n"
       "/// Returns -1 for C's NULL result (end of file with nothing\n"
       "/// read), else the count of bytes stored before the NUL.\n"
       "fn __emitrust_fgets(f: &mut __EmitrustFile, buf: &mut [i8], n: i64) "
       "-> i64 {\n"
       "    if n < 1 {\n"
       "        return -1;\n"
       "    }\n"
       "    let mut i = 0i64;\n"
       "    while i + 1 < n {\n"
       "        let c = __emitrust_fgetc(f);\n"
       "        if c < 0 {\n"
       "            break;\n"
       "        }\n"
       "        buf[i as usize] = c as i8;\n"
       "        i += 1;\n"
       "        if c == 10 {\n"
       "            break;\n"
       "        }\n"
       "    }\n"
       "    if i == 0 && n > 1 {\n"
       "        return -1;\n"
       "    }\n"
       "    buf[i as usize] = 0;\n"
       "    i\n"
       "}"},
      {"__emitrust_fclose",
       "/// `fclose(f)`: drops the handle (closing the file) and leaves\n"
       "/// the variable NULL, so the same C variable can be reopened by\n"
       "/// a later fopen (the 00187 serial-reuse shape). Closing NULL is\n"
       "/// C UB; leaving it NULL is a benign refinement.\n"
       "fn __emitrust_fclose(f: &mut __EmitrustFile) {\n"
       "    *f = __EmitrustFile::Null;\n"
       "}"},
  };
  for (const auto &helper : kFileHelpers) {
    if (!neededFileHelpers.contains(helper.name) ||
        emittedFileHelpers.contains(helper.name))
      continue;
    emittedFileHelpers.insert(helper.name);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(helper.source));
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Function-body plumbing
//===----------------------------------------------------------------------===//

Value CImporter::createEntryAlloca(Location loc, Type elementType) {
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(entryBlock);
  auto memrefType = MemRefType::get({}, elementType);
  return builder.create<memref::AllocaOp>(loc, memrefType).getResult();
}

Block *CImporter::createBlock() {
  OpBuilder::InsertionGuard guard(builder);
  return builder.createBlock(bodyRegion, bodyRegion->end());
}

Block *CImporter::getLabelBlock(const clang::LabelDecl *label) {
  Block *&block = labelBlocks[label];
  if (!block)
    block = createBlock();
  return block;
}

Value CImporter::createVariablePlace(Location loc, Type type) {
  OpBuilder::InsertionGuard guard(builder);
  if (currentHasLabels)
    builder.setInsertionPointToStart(entryBlock);
  return builder
      .create<emitrust::VariableOp>(loc, emitrust::LValueType::get(type))
      .getResult();
}

bool CImporter::isTerminated(Block *block) {
  return !block->empty() && block->back().hasTrait<OpTrait::IsTerminator>();
}

Value CImporter::createIntConstant(Location loc, Type type, int64_t value) {
  return builder
      .create<arith::ConstantOp>(loc, builder.getIntegerAttr(type, value))
      .getResult();
}

Value CImporter::createBoolConstant(Location loc, bool value) {
  return builder.create<arith::ConstantOp>(loc, builder.getBoolAttr(value))
      .getResult();
}

Value CImporter::createScalarIntConstant(Location loc, Type type,
                                         int64_t value) {
  if (isUnsignedInt(type))
    return builder
        .create<emitrust::ConstantOp>(loc, type,
                                      IntegerAttr::get(type, value))
        .getResult();
  return createIntConstant(loc, type, value);
}

LogicalResult CImporter::finalizeFunction(func::FuncOp funcOp, Location loc) {
  Region &region = funcOp.getBody();

  // Erase blocks unreachable from the entry block (dead code after returns
  // and empty merge blocks). Cross-block SSA uses only ever reference
  // entry-block values, so dropping the dead blocks' defs and references
  // first makes erasure safe in any order.
  llvm::SmallPtrSet<Block *, 16> reachable;
  SmallVector<Block *> worklist{&region.front()};
  while (!worklist.empty()) {
    Block *block = worklist.pop_back_val();
    if (!reachable.insert(block).second)
      continue;
    for (Block *successor : block->getSuccessors())
      worklist.push_back(successor);
  }
  for (Block &block : region) {
    if (reachable.contains(&block))
      continue;
    block.dropAllDefinedValueUses();
    block.dropAllReferences();
  }
  for (Block &block : llvm::make_early_inc_range(region))
    if (!reachable.contains(&block))
      block.erase();

  // Sweep write-only parameter cells: a prologue cell whose every
  // remaining use is a store belongs to a parameter that is never read on
  // any surviving path (e.g. its only uses folded away with a statically
  // null pointer, CTS-P9). Dropping the stores and the cell is exact —
  // the stored values' computations stay behind as pure ops — and keeps
  // fully folded functions free of runtime state.
  for (Value cell : paramCells) {
    Operation *alloca = cell.getDefiningOp();
    if (!alloca)
      continue;
    SmallVector<Operation *> users(cell.getUsers().begin(),
                                   cell.getUsers().end());
    if (!llvm::all_of(users, [](Operation *user) {
          return llvm::isa<memref::StoreOp>(user);
        }))
      continue;
    for (Operation *user : users)
      user->erase();
    alloca->erase();
  }

  // Terminate the fall-through block, if any.
  for (Block &block : region) {
    if (isTerminated(&block))
      continue;
    builder.setInsertionPointToEnd(&block);
    if (!currentReturnType) {
      emitCursorWritebacks(loc);
      builder.create<func::ReturnOp>(loc);
      continue;
    }
    // C11 5.1.2.2.3: falling off the end of main returns 0. For any other
    // non-void function, C11 6.9.1p12 leaves the behavior defined as long
    // as the caller never uses the missing value; the importer synthesizes
    // a `return 0` of the function's return type (Rust has no
    // fall-off-the-end for value-returning functions, and the zero is only
    // observable on executions that were undefined reads in C anyway).
    if (llvm::isa<IntegerType>(currentReturnType)) {
      Value zero = createScalarIntConstant(loc, currentReturnType, 0);
      emitCursorWritebacks(loc);
      builder.create<func::ReturnOp>(loc, zero);
      continue;
    }
    if (auto floatType = llvm::dyn_cast<FloatType>(currentReturnType)) {
      Value zero = builder
                       .create<arith::ConstantOp>(
                           loc, FloatAttr::get(floatType, 0.0))
                       .getResult();
      emitCursorWritebacks(loc);
      builder.create<func::ReturnOp>(loc, zero);
      continue;
    }
    // Aggregate, enum, and fn_ptr returns have no meaningful zero.
    return emitError(loc)
           << "unsupported: control reaches the end of non-void function '"
           << funcOp.getSymName() << "'";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

LogicalResult CImporter::emitStmt(const clang::Stmt *stmt) {
  Location loc = translateLoc(stmt->getBeginLoc());

  if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    for (const clang::Stmt *child : compound->body())
      if (failed(emitStmt(child)))
        return failure();
    return success();
  }
  if (llvm::isa<clang::NullStmt>(stmt))
    return success();
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *decl : declStmt->decls()) {
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if (failed(emitLocalVar(var)))
          return failure();
        continue;
      }
      if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
        if (failed(importRecord(record, translateLoc(record->getBeginLoc()))))
          return failure();
        continue;
      }
      if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        if (failed(
                importEnum(enumDecl, translateLoc(enumDecl->getBeginLoc()))))
          return failure();
        continue;
      }
      if (const auto *funcDecl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        // A block-scope function declaration has external linkage
        // (C11 6.2.2p5), so it is hoisted to module scope and imported
        // through the same path as a file-scope prototype (including the
        // body-less-function check in `finalizeProject`). It is always a
        // prototype: clang rejects nested function definitions before the
        // importer runs. `importFunction` guards the builder's insertion
        // point, so emission resumes in the current block afterwards.
        if (failed(importFunction(funcDecl)))
          return failure();
        continue;
      }
      if (llvm::isa<clang::TypedefDecl>(decl))
        continue;
      return emitError(translateLoc(decl->getBeginLoc()))
             << "unsupported declaration inside a function body";
    }
    return success();
  }
  if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt))
    return emitReturnStmt(ret);
  if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt))
    return emitIfStmt(ifStmt);
  if (const auto *whileStmt = llvm::dyn_cast<clang::WhileStmt>(stmt))
    return emitWhileStmt(whileStmt);
  if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
    return emitForStmt(forStmt);
  if (const auto *switchStmt = llvm::dyn_cast<clang::SwitchStmt>(stmt))
    return emitSwitchStmt(switchStmt);
  if (llvm::isa<clang::BreakStmt>(stmt)) {
    if (loopStack.empty())
      return emitError(loc)
             << "unsupported: 'break' outside of a loop or switch";
    builder.create<cf::BranchOp>(loc, loopStack.back().breakDest);
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (llvm::isa<clang::ContinueStmt>(stmt)) {
    // A switch inherits the continue target of its enclosing loop; a null
    // target means the innermost switch has no enclosing loop.
    if (loopStack.empty() || !loopStack.back().continueDest)
      return emitError(loc) << "unsupported: 'continue' outside of a loop";
    builder.create<cf::BranchOp>(loc, loopStack.back().continueDest);
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (const auto *doStmt = llvm::dyn_cast<clang::DoStmt>(stmt))
    return emitDoStmt(doStmt);
  if (llvm::isa<clang::IndirectGotoStmt>(stmt))
    return emitError(loc) << "unsupported: computed goto";
  if (const auto *gotoStmt = llvm::dyn_cast<clang::GotoStmt>(stmt)) {
    builder.create<cf::BranchOp>(loc, getLabelBlock(gotoStmt->getLabel()));
    // Continue in a fresh block; if it stays unreachable it is erased later.
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (const auto *labelStmt = llvm::dyn_cast<clang::LabelStmt>(stmt)) {
    Block *block = getLabelBlock(labelStmt->getDecl());
    if (!isTerminated(builder.getInsertionBlock()))
      builder.create<cf::BranchOp>(loc, block); // Fall into the label.
    builder.setInsertionPointToEnd(block);
    return emitStmt(labelStmt->getSubStmt());
  }
  if (const auto *switchCase = llvm::dyn_cast<clang::SwitchCase>(stmt)) {
    // Reached only under a dispatch-lowered switch (`emitDispatchSwitch`
    // pre-registers every label of the switch before walking its body; the
    // structured lowering peels its labels itself and never routes them
    // here). The label is an ordinary block boundary: fall into its
    // pre-created dispatch target, exactly like a C label.
    Block *block = switchCaseBlocks.lookup(switchCase);
    if (!block)
      return emitError(loc)
             << "unsupported: case label outside of an enclosing switch";
    if (!isTerminated(builder.getInsertionBlock()))
      builder.create<cf::BranchOp>(loc, block); // Fall into the label.
    builder.setInsertionPointToEnd(block);
    return emitStmt(switchCase->getSubStmt());
  }
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt))
    return emitExprStmt(expr);
  return emitError(loc) << "unsupported statement: "
                        << stmt->getStmtClassName();
}

/// Returns whether any `DeclRefExpr` under `stmt` references `var`.
/// Drives dead-VLA elision (CTS-F, 00207): "unreferenced" means no use
/// anywhere in the function body, including unevaluated contexts such as
/// `sizeof` (whose operand is a child of the trait expression), so any
/// mention at all keeps the existing rejection.
static bool referencesVar(const clang::Stmt *stmt,
                          const clang::VarDecl *var) {
  if (!stmt)
    return false;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (ref->getDecl()->getCanonicalDecl() == var->getCanonicalDecl())
      return true;
  for (const clang::Stmt *child : stmt->children())
    if (referencesVar(child, var))
      return true;
  return false;
}

LogicalResult CImporter::emitLocalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
  // A `va_list` local inside a monomorphization clone (CTS 00204) has no
  // storage of its own: the consumption cursor is the clone's internal
  // cell, and every reference to the object is consumed by the
  // va_start/va_arg/va_end lowerings (the planner verified this).
  // Outside a clone the type keeps its C99-37 rejection via mapType.
  if (currentVaCloneActive &&
      astContext().hasSameType(
          var->getType().getCanonicalType(),
          astContext().getBuiltinVaListType().getCanonicalType()))
    return success();
  // C99-7: pointer locals divert into the decomposition before `mapType`
  // runs, so the volatile scan happens up front for every local shape
  // (including the pointer's own qualifier, `int * volatile p`).
  if (hasVolatileQualifier(astContext(), var->getType()))
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (!var->hasLocalStorage()) {
    if (var->isStaticLocal()) {
      // A function-local static is module-level state initialized once at
      // program start (its C initializer must be a constant expression).
      // It is mangled as <function>_<name>; createGlobal rejects the
      // mangled name if it collides with an existing module symbol.
      std::string mangled =
          (llvm::Twine(currentFuncName) + "_" + var->getName()).str();
      return createGlobal(var->getCanonicalDecl(), var, mangled, loc);
    }
    return emitError(loc) << "unsupported: extern local variable";
  }
  // An owner-promoted array (Phase 4) declares the owner struct variable
  // instead; every direct access rewrites to the struct's "data" member.
  if (ownerPlans.contains(var))
    return emitOwnerLocal(var, loc);
  // Dead-VLA elision (CTS-F, 00207): an UNREFERENCED local VLA whose
  // size expression is side-effect-free is elided entirely — no IR, no
  // diagnostic. The object never materializes, and dropping the (pure)
  // size expression loses nothing. A referenced VLA — and a dead one
  // whose size expression has side effects (eliding it would silently
  // lose the effect) — keeps the `unsupported: non-constant array size`
  // rejection `mapType` emits below.
  {
    clang::QualType probe = var->getType();
    bool isVla = false;
    bool sizeSideEffectFree = true;
    while (const clang::ArrayType *array = astContext().getAsArrayType(probe)) {
      if (const auto *vla = llvm::dyn_cast<clang::VariableArrayType>(array)) {
        isVla = true;
        if (vla->getSizeExpr() &&
            vla->getSizeExpr()->HasSideEffects(astContext()))
          sizeSideEffectFree = false;
      }
      probe = array->getElementType();
    }
    if (isVla && sizeSideEffectFree &&
        !referencesVar(currentFunctionBody, var))
      return success();
  }
  // An admitted local `void *` fn-ptr holder (CTS-F, 00210) imports
  // exactly like a directly-typed local fn-ptr; the pointer
  // decomposition never sees it (`fnHolderQuery`).
  if (const clang::FunctionDecl *target = voidFnPtrHolders.lookup(var))
    return emitFnHolderLocal(var, target, loc);
  clang::QualType type = var->getType().getCanonicalType();
  // A FILE* handle local (uninitialized or fopen-initialized) is an owned
  // stream handle over std::fs (C99-48), never a decomposed pointer;
  // other FILE* initializers (stdout, ...) keep the historical pointer
  // path and its located rejections.
  if (isFileHandleLocal(var))
    return emitFileLocal(var, loc);
  // Function pointers are ordinary `!emitrust.fn_ptr` values and take the
  // plain variable path below, bypassing the pointer decomposition.
  if (type->isPointerType() && !type->isFunctionPointerType())
    return emitPointerLocal(var, loc);
  FailureOr<Type> mlirType = mapType(type, loc);
  if (failed(mlirType))
    return failure();
  // A callsite-inferred prototype-less fn-ptr local (FR-29, CTS 00209)
  // declares at its refined signature instead of the zero-parameter
  // no-proto mapping; its initializer binds against the refinement below.
  if (emitrust::FnPtrType refined = inferredFnPtrSigs.lookup(var))
    mlirType = Type(refined);

  bool isAggregate =
      llvm::isa<emitrust::StructType, emitrust::ArrayType>(*mlirType);
  // Enums, function pointers, and unsigned scalars live in
  // `emitrust.variable` places rather than memref cells: a memref of a
  // dialect type is illegal, and mem2reg materializes an unsigned cell's
  // default value as an `arith.constant`, which requires a signless type.
  bool isPlaceOnly =
      llvm::isa<emitrust::EnumType, emitrust::FnPtrType>(*mlirType);
  if (isAggregate || isPlaceOnly || isUnsignedInt(*mlirType) ||
      addressTaken.contains(var)) {
    Value place = createVariablePlace(loc, *mlirType);
    symbols[var] = place;
    if (const clang::Expr *init = var->getInit()) {
      if (isAggregate) {
        // CTS-BR (00216): byte-region locals initialize per byte —
        // folded constants at their layout offsets, embedded region
        // copies for struct-value elements and whole-copy initializers,
        // runtime scalars through their AST conversion casts.
        if (isByteRegionAggregate(var->getType()))
          return emitByteRegionInit(place, 0, var->getType(), init);
        // `= {...}` lists and `char s[] = "..."` string initializers are
        // supported; a whole-aggregate copy initializer stays rejected.
        // A compound-literal initializer (`struct S s = (struct S){...}`,
        // C99-13) copies a temp that is immediately dead, so it
        // initializes the variable directly through its own list.
        const clang::Expr *unwrapped = init->IgnoreParenImpCasts();
        if (const auto *compound =
                llvm::dyn_cast<clang::CompoundLiteralExpr>(unwrapped))
          unwrapped = compound->getInitializer()->IgnoreParenImpCasts();
        if (const auto *literal =
                llvm::dyn_cast<clang::StringLiteral>(unwrapped))
          return emitStringArrayInit(place, *mlirType, literal);
        // `struct T x = f();` initializes from the call's struct value
        // exactly like the assignment form `x = f();` (CTS 00204), and
        // `struct T x = va_arg(ap, struct T)` from the monomorphized
        // va_arg dispatch the same way.
        if (llvm::isa<clang::CallExpr, clang::VAArgExpr>(unwrapped)) {
          FailureOr<Value> value = emitRValue(unwrapped);
          if (failed(value))
            return failure();
          if ((*value).getType() != *mlirType)
            return emitError(loc)
                   << "unsupported: initializer type does not match the "
                      "variable";
          return storeToPlace(loc, place, *value);
        }
        const auto *list = llvm::dyn_cast<clang::InitListExpr>(unwrapped);
        if (!list)
          return emitError(loc) << "unsupported: aggregate initializer";
        return emitAggregateInitList(place, *mlirType, list, var);
      }
      FailureOr<Value> value = emitPositionedRValue(*mlirType, init);
      if (failed(value))
        return failure();
      return storeToPlace(loc, place, *value);
    }
    return success();
  }

  Value cell = createEntryAlloca(loc, *mlirType);
  symbols[var] = cell;
  if (const clang::Expr *init = var->getInit()) {
    FailureOr<Value> value = emitRValue(init);
    if (failed(value))
      return failure();
    return storeToPlace(loc, cell, *value);
  }
  return success();
}

void CImporter::collectVoidFnPtrHolders(const clang::Stmt *body) {
  voidFnPtrHolders.clear();
  if (!body)
    return;

  // Candidate pass: a local `void *` initialized with (an implicit cast
  // of) `&f` or the decayed `f` for a known non-variadic function.
  llvm::DenseMap<const clang::VarDecl *, const clang::FunctionDecl *>
      candidates;
  auto collectCandidates = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
          clang::QualType type = var->getType().getCanonicalType();
          if (!var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var) ||
              !type->isPointerType() ||
              !type->getPointeeType()->isVoidType() || !var->getInit())
            continue;
          const clang::Expr *init = var->getInit()->IgnoreParenImpCasts();
          if (const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(init))
            if (addrOf->getOpcode() == clang::UO_AddrOf)
              init = addrOf->getSubExpr()->IgnoreParenImpCasts();
          const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(init);
          const auto *target =
              ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())
                  : nullptr;
          // A prototype-less K&R target (the 00210 `int f()` shape) is
          // admitted like a directly-typed K&R fn-ptr local: it maps to
          // the zero-parameter form, and the emission's signature check
          // (`resolveFunctionPointerDecl`) still guards the binding.
          if (target && !target->isVariadic())
            candidates[var] = target;
        }
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  collectCandidates(collectCandidates, body);
  if (candidates.empty())
    return;

  // Consumption pass: mark every holder read that is an explicit cast to
  // EXACTLY the target's signature in callee position. Attributes inside
  // the cast type were already discarded by clang, so the canonical-type
  // comparison sees the plain signature.
  llvm::SmallPtrSet<const clang::DeclRefExpr *, 8> consumed;
  auto consumeCastCalls = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
      if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(
              call->getCallee()->IgnoreParens())) {
        const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            cast->getSubExpr()->IgnoreParenImpCasts());
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        auto candidate = var ? candidates.find(var) : candidates.end();
        if (candidate != candidates.end()) {
          clang::QualType castType = cast->getType().getCanonicalType();
          // "Exactly f's signature" is C type compatibility (C11
          // 6.2.7): it equates the cast's prototype with a
          // prototype-less target declaration (the 00210 shape) while
          // rejecting any diverging parameter or result spelling.
          if (castType->isFunctionPointerType() &&
              astContext().typesAreCompatible(
                  castType->getPointeeType(),
                  candidate->second->getType()))
            consumed.insert(ref);
        }
      }
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  consumeCastCalls(consumeCastCalls, body);

  // Disqualification pass: any other mention of the holder — a
  // reassignment's left-hand side, an escaping argument, a mismatched
  // cast, its address taken — keeps the existing pointer-region
  // rejection.
  auto disqualify = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
      if (!consumed.contains(ref))
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          candidates.erase(var);
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  disqualify(disqualify, body);

  for (const auto &[var, target] : candidates)
    voidFnPtrHolders.try_emplace(var, target);
}

LogicalResult CImporter::inferNoProtoCallSignatures(
    const clang::FunctionDecl *definition,
    llvm::DenseMap<const clang::VarDecl *, emitrust::FnPtrType> &inferred) {
  auto walk = [&](auto &&self, const clang::Stmt *stmt) -> LogicalResult {
    if (!stmt)
      return success();
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      const clang::Expr *calleeExpr = call->getCallee()->IgnoreParens();
      // `(*fp)(...)`: the decay/deref pair cancels out (see
      // emitIndirectCall).
      if (const auto *decay =
              llvm::dyn_cast<clang::ImplicitCastExpr>(calleeExpr))
        if (decay->getCastKind() == clang::CK_FunctionToPointerDecay) {
          const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(
              decay->getSubExpr()->IgnoreParens());
          if (deref && deref->getOpcode() == clang::UO_Deref &&
              isFunctionPointer(deref->getSubExpr()->getType()))
            calleeExpr = deref->getSubExpr()->IgnoreParens();
        }
      const clang::FunctionNoProtoType *noProto = nullptr;
      const clang::VarDecl *var = nullptr;
      if (call->getNumArgs() > 0 && isFunctionPointer(calleeExpr->getType())) {
        noProto = llvm::dyn_cast<clang::FunctionNoProtoType>(
            calleeExpr->getType()
                .getCanonicalType()
                ->getPointeeType()
                .getTypePtr());
        const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            calleeExpr->IgnoreParenImpCasts());
        const auto *decl =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        // Only local-storage decls (parameters and locals): a refined
        // type is a per-function fact, and a global's module-level type
        // must not depend on one body's call sites.
        if (decl && decl->hasLocalStorage())
          var = decl;
      }
      if (noProto && var) {
        Location loc = translateLoc(call->getBeginLoc());
        // Clang already applied the default argument promotions to the
        // arguments of a call through a no-proto type (C11 6.5.2.2p6),
        // so the promoted argument types are used verbatim.
        SmallVector<Type> inputs;
        for (const clang::Expr *argument : call->arguments()) {
          FailureOr<Type> mapped = mapType(argument->getType(), loc);
          if (failed(mapped))
            return failure();
          if (!emitrust::FnPtrType::isValidComponentType(*mapped))
            return emitError(loc)
                   << "unsupported: function pointer parameter type";
          inputs.push_back(*mapped);
        }
        SmallVector<Type> results;
        clang::QualType returnType = noProto->getReturnType();
        if (!returnType->isVoidType()) {
          FailureOr<Type> mapped = mapType(returnType, loc);
          if (failed(mapped))
            return failure();
          if (!emitrust::FnPtrType::isValidComponentType(*mapped))
            return emitError(loc)
                   << "unsupported: function pointer result type";
          results.push_back(*mapped);
        }
        auto signature =
            emitrust::FnPtrType::get(builder.getContext(), inputs, results);
        auto [existing, isNew] = inferred.try_emplace(var, signature);
        if (!isNew && existing->second != signature)
          return emitError(loc)
                 << "unsupported: conflicting inferred prototypes for "
                    "function pointer '"
                 << var->getName() << "'";
      }
    }
    for (const clang::Stmt *child : stmt->children())
      if (failed(self(self, child)))
        return failure();
    return success();
  };
  return walk(walk, definition->getBody());
}

LogicalResult CImporter::emitFnHolderLocal(const clang::VarDecl *var,
                                           const clang::FunctionDecl *target,
                                           Location loc) {
  FailureOr<Type> mapped =
      mapType(astContext().getPointerType(target->getType()), loc);
  if (failed(mapped))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*mapped);
  if (!fnPtrType)
    return emitError(loc) << "unsupported function pointer type";
  // The same import + signature check a `Some(target)` constant runs.
  FailureOr<std::string> name =
      resolveFunctionPointerDecl(target, fnPtrType, loc);
  if (failed(name))
    return failure();
  Value place = createVariablePlace(loc, fnPtrType);
  symbols[var] = place;
  auto some = emitrust::OpaqueAttr::get(
      builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
  Value constant =
      builder.create<emitrust::ConstantOp>(loc, fnPtrType, some).getResult();
  return storeToPlace(loc, place, constant);
}

LogicalResult CImporter::emitOwnerLocal(const clang::VarDecl *var,
                                        Location loc) {
  OwnerPlan &plan = ownerPlans.find(var)->second;
  FailureOr<Type> ownedType = mapType(var->getType(), loc);
  if (failed(ownedType))
    return failure();

  // Synthesize the module-level owner struct on first need; the name is
  // derived from C spellings, so a collision with any existing module
  // symbol is a located rejection (mirroring createGlobal).
  if (!plan.structDefCreated) {
    if (SymbolTable::lookupSymbolIn(module, plan.structName))
      return emitError(loc)
             << "unsupported: owner struct name '" << plan.structName
             << "' collides with an existing symbol";
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::StructDefOp>(
        loc, moduleBuilder.getStringAttr(plan.structName),
        moduleBuilder.getStrArrayAttr(llvm::StringRef("data")),
        moduleBuilder.getTypeArrayAttr(*ownedType));
    plan.structDefCreated = true;
  }

  auto ownerStructType =
      emitrust::StructType::get(builder.getContext(), plan.structName);
  Value ownerPlace = createVariablePlace(loc, ownerStructType);
  ownerStructPlaces[var] = ownerPlace;
  // Every direct access to the array — and every decomposed pointer whose
  // region base it is — routes through the data member place registered
  // here. Nothing ever loads the owner struct whole: the struct place is
  // only borrowed at method call sites (C arrays are not assignable, so no
  // syntax reaches a whole-owner load).
  Value dataPlace = builder
                        .create<emitrust::MemberOp>(
                            loc, emitrust::LValueType::get(*ownedType),
                            ownerPlace, builder.getStringAttr("data"))
                        .getResult();
  symbols[var] = dataPlace;
  if (const clang::Expr *init = var->getInit()) {
    // The owned array's initializer (a list or a `char s[] = "..."`
    // string) assigns through the data member place, exactly like a plain
    // local array's.
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
            init->IgnoreParenImpCasts()))
      return emitStringArrayInit(dataPlace, *ownedType, literal);
    const auto *list = llvm::dyn_cast<clang::InitListExpr>(init);
    if (!list)
      return emitError(loc) << "unsupported: aggregate initializer";
    return emitAggregateInitList(dataPlace, *ownedType, list);
  }
  return success();
}

LogicalResult
CImporter::emitAggregateInitList(Value place, Type type,
                                 const clang::InitListExpr *list,
                                 const clang::VarDecl *instance) {
  // Sema's semantic form has designators resolved to positional elements
  // and ImplicitValueInitExpr holes for everything left implicit.
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  Location loc = translateLoc(list->getBeginLoc());
  if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type)) {
    if (list->getNumInits() > arrayType.getSize()) // Defensive; Sema rejects.
      return emitError(loc)
             << "unsupported: excess elements in aggregate initializer";
    for (unsigned i = 0, n = list->getNumInits(); i != n; ++i) {
      const clang::Expr *element = list->getInit(i);
      // A hole keeps the place's default element value (C99 zero-fill).
      if (llvm::isa<clang::ImplicitValueInitExpr>(element))
        continue;
      Location elementLoc = translateLoc(element->getBeginLoc());
      Value index = createIntConstant(elementLoc, builder.getIntegerType(64),
                                      static_cast<int64_t>(i));
      Value elementPlace =
          builder
              .create<emitrust::SubscriptOp>(
                  elementLoc,
                  emitrust::LValueType::get(arrayType.getElementType()),
                  place, index)
              .getResult();
      if (failed(emitInitListElement(elementPlace,
                                     arrayType.getElementType(), element)))
        return failure();
    }
    return success();
  }
  if (llvm::isa<emitrust::StructType>(type)) {
    const clang::RecordDecl *record = list->getType()->getAsRecordDecl();
    if (!record) // Defensive; a struct-typed list always has a record.
      return emitError(loc) << "unsupported: aggregate initializer";
return emitRecordInitFields(place, record, list, instance);
  }
  return emitError(loc) << "unsupported: aggregate initializer";
}

LogicalResult
CImporter::emitRecordInitFields(Value place, const clang::RecordDecl *record,
                                const clang::InitListExpr *list,
                                const clang::VarDecl *instance) {
  // Nested lists reached through anonymous members arrive directly (not
  // via emitAggregateInitList), so normalize to the semantic form here
  // too; it is a no-op for a list that already is one.
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  if (record->isUnion()) {
    // A flattened anonymous union member: Sema records the single arm the
    // list initializes; its value lands on the aliased storage slot. A
    // list initializing no arm leaves the slot's default (zero) value.
    const clang::FieldDecl *active = list->getInitializedFieldInUnion();
    if (!active || list->getNumInits() == 0)
      return success();
    return emitRecordInitField(place, active, list->getInit(0), instance);
  }
  unsigned index = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    if (index >= list->getNumInits())
      break; // Remaining fields keep their default (zero) value.
    const clang::Expr *element = list->getInit(index++);
    if (!element || llvm::isa<clang::ImplicitValueInitExpr>(element) ||
        llvm::isa<clang::NoInitExpr>(element))
      continue;
    if (failed(emitRecordInitField(place, field, element, instance)))
      return failure();
  }
  return success();
}

LogicalResult CImporter::emitRecordInitField(Value place,
                                             const clang::FieldDecl *field,
                                             const clang::Expr *element,
                                             const clang::VarDecl *instance) {
  Location elementLoc = translateLoc(element->getBeginLoc());
  // A bit-field member has no field of its own in the flattened
  // struct_def (its storage is a window of a `__bits<n>` backing field);
  // aggregate initialization of one stays out of the C99-45 scope.
  if (field->isBitField())
    return emitError(elementLoc)
           << "unsupported: aggregate initializer for a bit-field member";
  // A flexible array member has no storage behind sizeof on a local
  // object; a GNU zero-length member has no elements, so its (empty)
  // brace initializer emits nothing (CTS-BR, 00216).
  if (field->getType()->isIncompleteArrayType())
    return emitError(elementLoc) << "unsupported: flexible array member access";
  if (isZeroLengthArrayType(field->getType()))
    return success();
  // An admitted `void *` fn-ptr member (CTS-BR, 00216) initializes from
  // the address of a function of its one signature: the member place is
  // the retyped fn_ptr field, the value the folded Some(target) (or None
  // for the null constant).
  if (clang::QualType retyped = fnPtrMemberTypes.lookup(field);
      !retyped.isNull()) {
    FailureOr<Type> fieldType = mapType(retyped, elementLoc);
    if (failed(fieldType))
      return failure();
    auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*fieldType);
    if (!fnPtrType)
      return emitError(elementLoc) << "unsupported function pointer type";
    Value fieldPlace =
        builder
            .create<emitrust::MemberOp>(
                elementLoc, emitrust::LValueType::get(fnPtrType), place,
                builder.getStringAttr(flattenedFieldName(field)))
            .getResult();
    if (element->isNullPointerConstant(astContext(),
                                       clang::Expr::NPC_NeverValueDependent) !=
        clang::Expr::NPCK_NotNull) {
      Value none = builder
                       .create<emitrust::ConstantOp>(
                           elementLoc, fnPtrType,
                           emitrust::OpaqueAttr::get(builder.getContext(),
                                                     "None"))
                       .getResult();
      builder.create<emitrust::AssignOp>(elementLoc, fieldPlace, none);
      return success();
    }
    const clang::Expr *target = element->IgnoreParenCasts();
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(target))
      if (unary->getOpcode() == clang::UO_AddrOf)
        target = unary->getSubExpr()->IgnoreParenCasts();
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(target);
    const auto *callee =
        ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
    if (!callee)
      return emitError(elementLoc)
             << "unsupported: pointer struct member initializer";
    FailureOr<std::string> name =
        resolveFunctionPointerDecl(callee, fnPtrType, elementLoc);
    if (failed(name))
      return failure();
    Value some = builder
                     .create<emitrust::ConstantOp>(
                         elementLoc, fnPtrType,
                         emitrust::OpaqueAttr::get(
                             builder.getContext(),
                             (llvm::Twine("Some(") + *name + ")").str()))
                     .getResult();
    builder.create<emitrust::AssignOp>(elementLoc, fieldPlace, some);
    return success();
  }
  if (isDataPointer(field->getType())) {
    // A data-pointer field's binding was recorded by the analysis walk of
    // this declaration; the stored i64 member keeps its default 0 (the
    // degenerate binding carries no runtime information), so a supported
    // initializer emits nothing (CTS-P2).
    if (!instance)
      return emitError(elementLoc)
             << "unsupported: pointer struct member initializer in a "
                "nested aggregate";
    MemberPointerKey key{instance->getCanonicalDecl(), field};
    auto it = memberPtrBindings.find(key);
    if (it == memberPtrBindings.end())
      return emitError(elementLoc)
             << "unsupported: pointer struct member initializer";
    if (!it->second.invalidReason.empty()) {
      InFlightDiagnostic diag =
          emitError(elementLoc) << it->second.invalidReason;
      if (it->second.secondLoc.isValid())
        diag.attachNote(translateLoc(it->second.secondLoc))
            << "conflicting binding here";
      return diag;
    }
    return success();
  }
  if (field->isAnonymousStructOrUnion()) {
    // The anonymous member's fields live inline in the parent place; its
    // nested list (the semantic form always materializes one) recurses
    // onto that same place.
    const auto *nested = llvm::dyn_cast<clang::InitListExpr>(element);
    if (!nested)
      return emitError(elementLoc)
             << "unsupported: aggregate initializer element";
    return emitRecordInitFields(
        place, field->getType()->getAsRecordDecl()->getDefinition(), nested,
        instance);
  }
  // A union pun arm stores into its slot's field: the member place takes
  // the SLOT's type and name, and the arm-typed initializer value
  // reinterprets bit-exactly onto it (the local-init counterpart of
  // `reinterpretUnionArmWrite`).
  const clang::FieldDecl *storage = flattenedFieldStorage(field);
  FailureOr<Type> fieldType = mapType(storage->getType(), elementLoc);
  if (failed(fieldType))
    return failure();
  Value fieldPlace = builder
                         .create<emitrust::MemberOp>(
                             elementLoc, emitrust::LValueType::get(*fieldType),
                             place,
                             builder.getStringAttr(flattenedFieldName(field)))
                         .getResult();
  if (storage == field)
    return emitInitListElement(fieldPlace, *fieldType, element);
  FailureOr<Value> value = emitRValue(element);
  if (failed(value))
    return failure();
  return storeToPlace(elementLoc, fieldPlace,
                      reinterpretScalarBits(elementLoc, *value, *fieldType));
}

LogicalResult CImporter::emitInitListElement(Value place, Type type,
                                             const clang::Expr *element) {
  if (const auto *nested = llvm::dyn_cast<clang::InitListExpr>(element))
    return emitAggregateInitList(place, type, nested);
  // A compound-literal element (`{(struct S){1, 2}, ...}`, C99-13) copies
  // a temp that is immediately dead; its list initializes the element
  // place directly, like a nested brace list.
  if (llvm::isa<emitrust::ArrayType, emitrust::StructType>(type))
    if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(
            element->IgnoreParenImpCasts()))
      if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(
              compound->getInitializer()->IgnoreParenImpCasts()))
        return emitAggregateInitList(place, type, list);
  Location loc = translateLoc(element->getBeginLoc());
  // A non-list initializer for an aggregate element (a string literal for
  // a char-array field, a whole-struct copy) is out of scope.
  if (llvm::isa<emitrust::ArrayType, emitrust::StructType>(type))
    return emitError(loc) << "unsupported: aggregate initializer element";
  FailureOr<Value> value = emitRValue(element);
  if (failed(value))
    return failure();
  return storeToPlace(loc, place, *value);
}

LogicalResult
CImporter::emitStringArrayInit(Value place, Type type,
                               const clang::StringLiteral *literal) {
  Location loc = translateLoc(literal->getBeginLoc());
  auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type);
  // An ordinary literal fills a byte array — signless i8 for plain/signed
  // char elements, unsigned ui8 for unsigned char elements (C99 6.7.8p14
  // admits all three; the bytes are identical since the ASCII policy
  // below keeps every value in 0..=127). A wide literal fills a `wchar_t`
  // (i32 on the supported targets) array, one code unit per element.
  // u8/u/U literals have no mapped element representation.
  if (!literal->isOrdinary() && !literal->isWide())
    return emitError(loc) << "unsupported: non-ordinary string literal "
                             "initializer";
  auto elementType =
      arrayType ? llvm::dyn_cast<IntegerType>(arrayType.getElementType())
                : IntegerType();
  bool elementMatches =
      elementType &&
      (literal->isOrdinary()
           ? elementType.getWidth() == 8 && !elementType.isSigned()
           : elementType.getWidth() == 32 && elementType.isSignless());
  if (!elementMatches)
    return emitError(loc)
           << "unsupported: string literal initializer for this type";
  // C99 6.7.8p14: successive code units of the literal (including the
  // terminating NUL if there is room) initialize the elements; Sema
  // guarantees the literal fits. Elements past the literal keep the
  // place's default zero value (matching C's zero fill), so only the
  // literal's code units plus the NUL are assigned.
  uint64_t length = literal->getLength();
  uint64_t count = std::min<uint64_t>(length + 1, arrayType.getSize());
  for (uint64_t i = 0; i != count; ++i) {
    uint32_t byte = i < length ? literal->getCodeUnit(i) : 0;
    // Non-ASCII bytes of an ordinary literal are rejected so the array's
    // contents stay exact through the ASCII-only `%s`/`%c` printing
    // helpers; a wide array never feeds those helpers.
    if (literal->isOrdinary() && byte > 127)
      return emitError(loc)
             << "unsupported: non-ASCII byte in string literal initializer";
    Value index =
        createIntConstant(loc, builder.getIntegerType(64),
                          static_cast<int64_t>(i));
    Value elementPlace =
        builder
            .create<emitrust::SubscriptOp>(
                loc, emitrust::LValueType::get(arrayType.getElementType()),
                place, index)
            .getResult();
    // createScalarIntConstant covers both the signless (arith) and
    // unsigned (emitrust.constant) element domains.
    Value value = createScalarIntConstant(loc, arrayType.getElementType(),
                                          static_cast<int64_t>(byte));
    if (failed(storeToPlace(loc, elementPlace, value)))
      return failure();
  }
  return success();
}

FailureOr<Value> CImporter::emitCompoundLiteralPlace(
    const clang::CompoundLiteralExpr *literal, bool hoistForRegion) {
  Location loc = translateLoc(literal->getBeginLoc());
  // A file-scope compound literal in a global initializer imports through
  // the constant-evaluator paths (CTS-P4); one reaching an expression
  // context here is defensive.
  if (literal->isFileScope())
    return emitError(loc)
           << "unsupported: file-scope compound literal in expression "
              "position";
  FailureOr<Type> type = mapType(literal->getType(), loc);
  if (failed(type))
    return failure();
  // The C99-13 subset covers aggregate (struct/union/array) literals; a
  // scalar compound literal has no aggregate-init lowering here.
  if (!llvm::isa<emitrust::StructType, emitrust::ArrayType>(*type))
    return emitError(loc)
           << "unsupported: compound literal of non-aggregate type";
  Value place;
  if (hoistForRegion) {
    // A region-base temp is dereferenced wherever the region's pointers
    // are used, so its place must dominate the whole body: create it in
    // the entry block and capture the pristine default value there, then
    // restore that default at each evaluation of the literal so re-runs
    // (a binding inside a loop) re-zero the holes exactly like C's fresh
    // object per evaluation.
    Value defaultValue;
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(entryBlock);
      place = builder
                  .create<emitrust::VariableOp>(
                      loc, emitrust::LValueType::get(*type))
                  .getResult();
      defaultValue =
          builder.create<emitrust::LoadOp>(loc, *type, place).getResult();
    }
    if (failed(storeToPlace(loc, place, defaultValue)))
      return failure();
  } else {
    place = createVariablePlace(loc, *type);
  }
  const clang::Expr *init = literal->getInitializer()->IgnoreParenImpCasts();
  if (const auto *string = llvm::dyn_cast<clang::StringLiteral>(init)) {
    if (failed(emitStringArrayInit(place, *type, string)))
      return failure();
    return place;
  }
  const auto *list = llvm::dyn_cast<clang::InitListExpr>(init);
  if (!list)
    return emitError(loc) << "unsupported: aggregate initializer";
  // `(char[]){"hi"}`: the braces hold the string as the list's sole
  // element (there is no bare-string spelling for a compound literal);
  // it fills the array like a `char s[] = "..."` declaration.
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  if (llvm::isa<emitrust::ArrayType>(*type) && list->getNumInits() == 1)
    if (const auto *string = llvm::dyn_cast<clang::StringLiteral>(
            list->getInit(0)->IgnoreParenImpCasts())) {
      if (failed(emitStringArrayInit(place, *type, string)))
        return failure();
      return place;
    }
  if (failed(emitAggregateInitList(place, *type, list)))
    return failure();
  return place;
}

FailureOr<const clang::VarDecl *> CImporter::materializeCompoundLiteralBase(
    const clang::CompoundLiteralExpr *literal) {
  const clang::VarDecl *backing =
      literalTemps.getOrCreate(astContext(), literal);
  FailureOr<Value> place =
      emitCompoundLiteralPlace(literal, /*hoistForRegion=*/true);
  if (failed(place))
    return failure();
  // Re-executing the binding (a loop around it) rebinds the backing to the
  // freshly initialized place of that evaluation, matching C's per-block
  // storage duration.
  symbols[backing] = *place;
  return backing;
}

FailureOr<Value>
CImporter::createLiteralBacking(const clang::StringLiteral *literal,
                                Location loc, bool isConst) {
  if (!literal->isOrdinary())
    return emitError(loc) << "unsupported: non-ordinary string literal "
                             "bound to a pointer";
  // The backing holds the literal's bytes plus the terminating NUL, so a
  // strlen-style walk terminates inside the array. Non-ASCII bytes are
  // rejected so the region's contents stay exact through the ASCII-only
  // `%s`/`%c` printing helpers (the emitStringArrayInit policy, C99-28).
  uint64_t length = literal->getLength();
  Type byteType = builder.getIntegerType(8);
  SmallVector<Attribute> bytes;
  bytes.reserve(length + 1);
  for (uint64_t i = 0; i != length; ++i) {
    uint32_t byte = literal->getCodeUnit(i);
    if (byte > 127)
      return emitError(loc) << "unsupported: non-ASCII byte in string "
                               "literal bound to a pointer";
    bytes.push_back(
        IntegerAttr::get(byteType, static_cast<int64_t>(byte)));
  }
  bytes.push_back(IntegerAttr::get(byteType, 0));
  auto arrayType = emitrust::ArrayType::get(builder.getContext(), length + 1,
                                            byteType);
  // Like createVariablePlace, hoist a cached (const) backing to the entry
  // block when the function contains labels so a goto jumping over the
  // declaration cannot leave a later use undominated. A mutable per-call
  // copy is used immediately in the same statement group, so it stays at
  // the current insertion point.
  OpBuilder::InsertionGuard guard(builder);
  if (isConst && currentHasLabels)
    builder.setInsertionPointToStart(entryBlock);
  return builder
      .create<emitrust::VariableOp>(loc, emitrust::LValueType::get(arrayType),
                                    builder.getArrayAttr(bytes), isConst)
      .getResult();
}

FailureOr<Value>
CImporter::getOrCreateLiteralBacking(const clang::StringLiteral *literal,
                                     Location loc) {
  if (Value existing = literalBackings.lookup(literal))
    return existing;
  FailureOr<Value> backing =
      createLiteralBacking(literal, loc, /*isConst=*/true);
  if (failed(backing))
    return failure();
  literalBackings[literal] = *backing;
  return *backing;
}

LogicalResult CImporter::emitPointerLocal(const clang::VarDecl *var,
                                          Location loc) {
  clang::QualType pointee =
      var->getType().getCanonicalType()->getPointeeType();
  if (pointee.getCanonicalType()->isPointerType())
    return emitPointerPointerLocal(var, loc);

  const PointerRegion *region = pointerRegions.regionOf(var);
  if (!region)
    return success(); // Declared but never used as a pointer; no code.
  if (!region->invalidReason.empty())
    return emitError(translateLoc(region->invalidLoc))
           << region->invalidReason;
  if (region->literalBase) {
    // Read-only string-literal region: the pointer is a cursor into the
    // literal's `'static` byte run, backed by an immutable local byte
    // array (bytes plus the terminating NUL). Nullable literal regions
    // are outside the CTS-P8 scope.
    if (region->nullable)
      return emitError(translateLoc(region->nullableLoc))
             << "unsupported: null pointer constant assigned to a pointer "
                "into a string literal";
    if (!region->bases.empty()) {
      const PointerBaseBinding &object = region->bases.front();
      InFlightDiagnostic diag = emitError(loc);
      diag << "unsupported: pointer '" << var->getName()
           << "' would join a string literal and object '"
           << object.base->getName() << "' into one region";
      diag.attachNote(translateLoc(region->literalLoc))
          << "bound to a string literal here";
      diag.attachNote(translateLoc(object.loc))
          << "bound to '" << object.base->getName() << "' here";
      return diag;
    }
    if (region->hasWriteThrough)
      return emitError(translateLoc(region->writeThroughLoc))
             << "unsupported: write through a pointer to a string literal "
                "(the literal is read-only)";
    Location bindLoc = translateLoc(region->literalLoc);
    FailureOr<Type> elementType = mapType(pointee, bindLoc);
    if (failed(elementType))
      return failure();
    if (*elementType != builder.getIntegerType(8))
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "string literal";
    FailureOr<Value> backing =
        getOrCreateLiteralBacking(region->literalBase, bindLoc);
    if (failed(backing))
      return failure();
    Value cell = createEntryAlloca(loc, builder.getIntegerType(64));
    pointerLocals[var] = PointerLocalInfo{nullptr, cell, *backing};
    if (const clang::Expr *init = var->getInit())
      return storePointerAssign(loc, var, init);
    return success();
  }
  if (region->bases.size() >= 2) {
    // A pointer rebound across distinct objects keeps one region under the
    // enum-of-bases model (CTS-P7): each pointer of the region carries a
    // promotable i32 discriminant cell naming its active base, and every
    // dereference dispatches on it — each variant of the closed enum names
    // a disjoint region, so the disjoint-region invariant is preserved
    // while the objects stay independently addressable. The model requires
    // every base to be local and of one uniform kind — all element runs
    // (arrays or slice parameters) whose element type is the pointee, or
    // all degenerate scalar objects of the pointee type. Anything else
    // (mixed kinds, mismatched element types, global bases, a nullable
    // region) keeps the historical join rejection naming both objects and
    // both binding sites.
    auto joinReject = [&]() -> LogicalResult {
      const PointerBaseBinding &first = region->bases[0];
      const PointerBaseBinding &second = region->bases[1];
      InFlightDiagnostic diag = emitError(loc);
      diag << "unsupported: pointer '" << var->getName()
           << "' would join objects '" << first.base->getName() << "' and '"
           << second.base->getName() << "' into one region";
      diag.attachNote(translateLoc(first.loc))
          << "bound to '" << first.base->getName() << "' here";
      diag.attachNote(translateLoc(second.loc))
          << "bound to '" << second.base->getName() << "' here";
      return diag;
    };
    if (region->nullable)
      return joinReject();
    bool anyCursored = false;
    bool anyDegenerate = false;
    bool anyMember = false;
    // A `void *` pointee is a pointee-wildcard cursor (CTS-P9): it carries
    // no element unit of its own, so the per-base element checks below do
    // not apply; each reinterpret-back deref site type-checks instead.
    bool wildcard = pointee.getCanonicalType()->isVoidType();
    for (const PointerBaseBinding &binding : region->bases) {
      const clang::VarDecl *base = binding.base;
      if (binding.member) {
        // A member base (CTS-P9) is a degenerate one-element run rooted
        // at the member's own place; a global root reuses the staged-copy
        // machinery per dispatch arm.
        if (!wildcard && !astContext().hasSameUnqualifiedType(
                             pointee, binding.member->getType()))
          return joinReject();
        anyDegenerate = true;
        anyMember = true;
        continue;
      }
      if (!base->hasLocalStorage())
        return joinReject();
      if (isPointerType(base->getType())) {
        // A slice-classified pointer parameter base: its deref'd slice
        // place was registered in the prologue with a cursor cell.
        auto baseInfo = pointerLocals.find(base);
        if (baseInfo == pointerLocals.end() ||
            !baseInfo->second.cursorCell ||
            (!wildcard &&
             !astContext().hasSameUnqualifiedType(
                 pointee,
                 base->getType().getCanonicalType()->getPointeeType())))
          return joinReject();
        anyCursored = true;
      } else if (const clang::ConstantArrayType *array =
                     astContext().getAsConstantArrayType(base->getType())) {
        bool matchesLevel = wildcard;
        for (const clang::ConstantArrayType *level = array;
             level && !matchesLevel;
             level = astContext().getAsConstantArrayType(
                 level->getElementType()))
          if (astContext().hasSameUnqualifiedType(pointee,
                                                  level->getElementType()))
            matchesLevel = true;
        if (!matchesLevel)
          return joinReject();
        anyCursored = true;
      } else {
        if (!wildcard &&
            !astContext().hasSameUnqualifiedType(pointee, base->getType()))
          return joinReject();
        anyDegenerate = true;
      }
    }
    if (anyCursored && anyDegenerate)
      return joinReject();
    if (anyDegenerate && region->hasArithmetic)
      return emitError(translateLoc(region->arithmeticLoc))
             << (anyMember ? "unsupported: pointer arithmetic on the "
                             "address of a struct member"
                           : "unsupported: arithmetic on the address of a "
                             "scalar object");
    PointerLocalInfo info;
    if (anyCursored)
      info.cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
    info.baseIndexCell = createEntryAlloca(loc, builder.getIntegerType(32));
    for (const PointerBaseBinding &binding : region->bases)
      info.multiBases.push_back(PointerBaseKey{binding.base, binding.member});
    pointerLocals[var] = info;
    if (const clang::Expr *init = var->getInit())
      return storePointerAssign(loc, var, init);
    return success();
  }
  if (region->bases.empty()) {
    // An integer-carrier region (CTS-P3): the pointer never addresses a
    // modeled object — its only sources are integer-to-pointer casts,
    // carrier-returning calls, and null constants — so its entire runtime
    // state is one plain i64 cell (null is 0). Walking or dereferencing a
    // carrier has nothing to resolve against and rejects here, at the
    // first offending site the analysis recorded.
    if (region->hasCarrierSource) {
      if (region->hasArithmetic)
        return emitError(translateLoc(region->arithmeticLoc))
               << "unsupported: pointer arithmetic on an integer-carrier "
                  "pointer";
      if (region->hasWriteThrough)
        return emitError(translateLoc(region->writeThroughLoc))
               << "unsupported: dereference of an integer-carrier pointer";
      Value cell = createEntryAlloca(loc, builder.getIntegerType(64));
      carrierLocals[var] = cell;
      if (const clang::Expr *init = var->getInit())
        return storePointerAssign(loc, var, init);
      return success();
    }
    // Never bound to any object. A base-less nullable region with a
    // conditional source is STATICALLY NULL (CTS-P9): it only ever unites
    // null constants and other null-only pointers, so it carries zero
    // runtime state — no flag cell is materialized, null tests fold to
    // constants, `(int) p` folds to 0, assignments are no-ops (see
    // `storePointerAssign`), and any dereference is rejected at its site.
    // A base-less nullable region built only from direct null bindings
    // keeps its CTS-P8 Option-of-cursor discriminant so null-checks read
    // the flag. A non-nullable unbound pointer needs no code.
    if (region->nullable && !isStaticallyNullRegion(region)) {
      Value nonNullCell = createEntryAlloca(loc, builder.getI1Type());
      pointerLocals[var] =
          PointerLocalInfo{nullptr, Value(), Value(), nonNullCell};
      if (const clang::Expr *init = var->getInit())
        return storePointerAssign(loc, var, init);
    }
    return success();
  }

  const PointerBaseBinding &binding = region->bases.front();
  // A global (or static-local) base is accepted (CTS-P6): every element
  // access through the pointer stages the global's whole value and writes
  // store the staged copy back, exactly like a direct global element
  // access, so the cursor cell below is the pointer's only runtime state
  // and no borrow of the global is ever held. The base's element/pointee
  // validation reads only the declared type and applies unchanged.
  const clang::VarDecl *base = binding.base;
  Location bindLoc = translateLoc(binding.loc);
  // A `void *` pointee is a pointee-wildcard cursor (CTS-P9): it names no
  // element unit, so the element checks below do not apply; each
  // reinterpret-back deref site (`*(T *)p`) type-checks `T` against the
  // base element type instead.
  bool wildcard = pointee.getCanonicalType()->isVoidType();

  Value cursorCell;
  if (binding.member) {
    // A `&struct.member` base (CTS-P9) is a degenerate one-element run
    // rooted at the member's own place: no cursor, and any pointer
    // arithmetic would walk past the member into sibling storage, which
    // the member-path binding cannot represent.
    if (region->hasArithmetic)
      return emitError(translateLoc(region->arithmeticLoc))
             << "unsupported: pointer arithmetic on the address of a "
                "struct member";
    if (!wildcard && !astContext().hasSameUnqualifiedType(
                         pointee, binding.member->getType()))
      return emitError(bindLoc)
             << "unsupported: pointer type does not match its target "
                "member";
  } else if (isPointerType(base->getType())) {
    // The base is a slice-classified pointer parameter (the only pointer
    // that can be a region base): the local walks the parameter's element
    // run through its own cursor. The binding registered at the function
    // prologue guarantees the base place is an lvalue<slice>.
    auto baseInfo = pointerLocals.find(base);
    if (baseInfo == pointerLocals.end() || !baseInfo->second.cursorCell)
      return emitError(bindLoc)
             << "unsupported: pointer variable bound to a non-slice "
                "pointer parameter"; // Defensive; classification forbids it.
    clang::QualType baseElement =
        base->getType().getCanonicalType()->getPointeeType();
    // A string-cursor parameter's element run is the pointee of its
    // POINTEE: `const char **s` walks the byte region `*s` points into
    // (CTS 00204).
    if (const auto *baseParam = llvm::dyn_cast<clang::ParmVarDecl>(base);
        baseParam && cursorParams.contains(baseParam))
      baseElement = baseElement.getCanonicalType()->getPointeeType();
    if (!wildcard && !astContext().hasSameUnqualifiedType(pointee, baseElement))
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target parameter";
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else if (const clang::ConstantArrayType *array =
                 astContext().getAsConstantArrayType(base->getType())) {
    // The pointee must be the element type of the base at some array
    // nesting depth: a row pointer (`char (*)[4]` into `char[2][4]`)
    // matches at the first level, a scalar pointer (`char *`) at the
    // innermost. Either way the cursor counts innermost elements in
    // row-major order.
    bool matchesLevel = wildcard;
    for (const clang::ConstantArrayType *level = array;
         level && !matchesLevel;
         level = astContext().getAsConstantArrayType(
             level->getElementType())) {
      if (astContext().hasSameUnqualifiedType(pointee,
                                              level->getElementType()))
        matchesLevel = true;
    }
    if (!matchesLevel)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target array";
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else {
    // Degenerate base: the pointer can only ever designate the whole
    // scalar (or struct) object, so it carries no cursor and supports no
    // arithmetic.
    if (region->hasArithmetic)
      return emitError(translateLoc(region->arithmeticLoc))
             << "unsupported: arithmetic on the address of a scalar object";
    if (!wildcard &&
        !astContext().hasSameUnqualifiedType(pointee, base->getType()))
      return emitError(bindLoc)
             << "unsupported: pointer type does not match its target object";
  }
  // A nullable region carries the Option-of-cursor discriminant in a
  // promotable i1 cell per pointer: address bindings store true, null
  // bindings store false, and null-checks load it (CTS-P8).
  Value nonNullCell;
  if (region->nullable)
    nonNullCell = createEntryAlloca(loc, builder.getI1Type());
  PointerLocalInfo info{base, cursorCell, Value(), nonNullCell};
  info.member = binding.member;
  pointerLocals[var] = info;
  if (const clang::Expr *init = var->getInit())
    return storePointerAssign(loc, var, init);
  return success();
}

LogicalResult CImporter::emitPointerPointerLocal(const clang::VarDecl *var,
                                                 Location loc) {
  clang::QualType pointee =
      var->getType().getCanonicalType()->getPointeeType();
  // The selected cell must hold a first-order object pointer: a function
  // pointer is an ordinary Copy value with no cursor cell to select, and a
  // third-order pointer would need a region of second-order selections.
  if (pointee.getCanonicalType()->isFunctionPointerType())
    return emitError(loc)
           << "unsupported: pointer to a function pointer variable";
  if (pointee.getCanonicalType()->getPointeeType()->isPointerType())
    return emitError(loc)
           << "unsupported: pointer-to-pointer-to-pointer variable";

  const SecondOrderRegion *region = pointerRegions.secondOrderRegionOf(var);
  if (!region)
    return success(); // Declared but never used as a pointer; no code.
  if (!region->invalidReason.empty())
    return emitError(translateLoc(region->invalidLoc))
           << region->invalidReason;
  if (region->secondTarget) {
    // A selection over two distinct pointer variables would need a real
    // region of cursor cells with a runtime second-order cursor; the
    // degenerate one-cell shape names both bindings and rejects.
    InFlightDiagnostic diag = emitError(loc);
    diag << "unsupported: pointer-to-pointer '" << var->getName()
         << "' would select between pointer variables '"
         << region->target->getName() << "' and '"
         << region->secondTarget->getName() << "'";
    diag.attachNote(translateLoc(region->targetLoc))
        << "bound to '" << region->target->getName() << "' here";
    diag.attachNote(translateLoc(region->secondTargetLoc))
        << "bound to '" << region->secondTarget->getName() << "' here";
    return diag;
  }
  if (!region->target)
    return success(); // Never bound; any dereference rejects at its site.
  // The degenerate one-cell region: the selection is static, so the
  // binding (and every later `pp = &p` of the same target) emits no code.
  pointerPointerLocals[var] = region->target;
  return success();
}

LogicalResult CImporter::storePointerAssign(Location loc,
                                            const clang::VarDecl *ptr,
                                            const clang::Expr *rhs) {
  // An integer-carrier pointer local (CTS-P3) rebinds by storing the
  // carrier's plain i64 value into its cell; no pointer state exists.
  if (Value cell = carrierLocals.lookup(ptr)) {
    FailureOr<Value> value = emitCarrierValue(rhs);
    if (failed(value))
      return failure();
    builder.create<memref::StoreOp>(loc, *value, cell);
    return success();
  }
  auto it = pointerLocals.find(ptr);
  if (it == pointerLocals.end()) {
    auto globalIt = pointerGlobals.find(ptr->getCanonicalDecl());
    if (globalIt != pointerGlobals.end())
      return storeGlobalPointerAssign(loc, ptr, globalIt->second, rhs);
    auto secondIt = pointerPointerLocals.find(ptr);
    if (secondIt != pointerPointerLocals.end()) {
      // `pp = &p`: the second-order selection is static (the analysis
      // accepted exactly one target), so the rebinding emits no code.
      // Defensively verify the operand is that target's address.
      const auto *unary =
          llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(rhs));
      const clang::VarDecl *target =
          unary && unary->getOpcode() == clang::UO_AddrOf
              ? asLocalVarRef(unary->getSubExpr())
              : nullptr;
      if (target != secondIt->second)
        return emitError(loc) // Defensive; the analysis forbids it.
               << "unsupported: pointer-to-pointer assignment would rebind "
                  "to a different pointer variable";
      return success();
    }
    // A statically-null pointer (a base-less nullable region, CTS-P9)
    // carries zero runtime state: every source the analysis admitted into
    // its region is a null constant or another statically-null pointer,
    // so the assignment is a no-op.
    if (ptr->hasLocalStorage() &&
        isStaticallyNullRegion(pointerRegions.regionOf(ptr)))
      return success();
    return emitError(loc) << "unsupported: assignment to pointer variable '"
                          << ptr->getName() << "' with no known target object";
  }
  // A pointer-typed conditional is a pointer source (CTS-P9): each arm
  // assigns in its own block, so the null/address state merges through the
  // pointer's own flag, discriminant, and cursor cells — no new
  // representation. The implicit `void *` bitcast Sema wraps a mixed-arm
  // conditional in peels first.
  {
    const clang::Expr *peeled = stripTrivia(rhs);
    while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
      peeled = stripTrivia(sub);
    if (const auto *conditional =
            llvm::dyn_cast<clang::ConditionalOperator>(peeled)) {
      FailureOr<Value> condition = emitCondition(conditional->getCond());
      if (failed(condition))
        return failure();
      Block *trueBlock = createBlock();
      Block *falseBlock = createBlock();
      Block *endBlock = createBlock();
      builder.create<cf::CondBranchOp>(loc, *condition, trueBlock,
                                       ValueRange(), falseBlock, ValueRange());
      builder.setInsertionPointToEnd(trueBlock);
      if (failed(storePointerAssign(loc, ptr, conditional->getTrueExpr())))
        return failure();
      builder.create<cf::BranchOp>(loc, endBlock);
      builder.setInsertionPointToEnd(falseBlock);
      if (failed(storePointerAssign(loc, ptr, conditional->getFalseExpr())))
        return failure();
      builder.create<cf::BranchOp>(loc, endBlock);
      builder.setInsertionPointToEnd(endBlock);
      return success();
    }
  }
  const PointerLocalInfo &info = it->second;
  // `p = NULL` selects the None side of the Option-of-cursor model: only
  // the discriminant cell changes (the stale cursor is dead while the
  // flag is false). A pointer without a flag cell cannot represent null;
  // the analysis marks every null-receiving local region nullable, so
  // this rejection covers only non-region pointers (e.g. parameters).
  if (isNullPointerConstantExpr(rhs)) {
    if (!info.nonNullCell)
      return emitError(loc) << "unsupported: null pointer constant assigned "
                               "to this pointer";
    Value none = createBoolConstant(loc, false);
    builder.create<memref::StoreOp>(loc, none, info.nonNullCell);
    return success();
  }
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  if (!info.multiBases.empty()) {
    // Multi-base region (CTS-P7): the assignment stores the enum-of-bases
    // discriminant alongside the cursor — the bound object's index for an
    // address binding, or the source pointer's own discriminant for
    // `p = q` (every pointer of the region shares the base order).
    Value index;
    if (value->baseIndex) {
      if (value->multiBases != info.multiBases) // Defensive; one region.
        return emitError(loc)
               << "unsupported: pointer assignment would rebind to a "
                  "different object";
      index = value->baseIndex;
    } else {
      const auto *found = llvm::find(
          info.multiBases, PointerBaseKey{value->base, value->member});
      if (!value->base || found == info.multiBases.end()) // Defensive.
        return emitError(loc)
               << "unsupported: pointer assignment would rebind to a "
                  "different object";
      index = createIntConstant(loc, builder.getIntegerType(32),
                                found - info.multiBases.begin());
    }
    builder.create<memref::StoreOp>(loc, index, info.baseIndexCell);
    if (!info.cursorCell)
      return success(); // All-degenerate bases: no element offset to track.
    Value multiCursor =
        value->cursor ? value->cursor
                      : createIntConstant(loc, builder.getIntegerType(64), 0);
    builder.create<memref::StoreOp>(loc, multiCursor, info.cursorCell);
    return success();
  }
  if (value->base != info.base || value->member != info.member ||
      value->literalBacking != info.literalBacking ||
      value->baseIndex) // A multi-base source cannot rebind a single-base
                        // pointer (defensive; regions would have unioned).
    return emitError(loc)
           << "unsupported: pointer assignment would rebind to a different "
              "object";
  // An address binding selects the Some side: the discriminant becomes
  // true (or copies the source pointer's flag on `p = q`).
  if (info.nonNullCell) {
    Value nonNull =
        value->nonNull ? value->nonNull : createBoolConstant(loc, true);
    builder.create<memref::StoreOp>(loc, nonNull, info.nonNullCell);
  }
  if (!info.cursorCell)
    return success(); // Degenerate: the target place is statically known.
  Value cursor = value->cursor
                     ? value->cursor
                     : createIntConstant(loc, builder.getIntegerType(64), 0);
  builder.create<memref::StoreOp>(loc, cursor, info.cursorCell);
  return success();
}

LogicalResult
CImporter::storeGlobalPointerAssign(Location loc, const clang::VarDecl *ptr,
                                    const PointerGlobalInfo &info,
                                    const clang::Expr *rhs) {
  IntegerType i64Type = builder.getIntegerType(64);
  // `g = calloc(...)` / `g = malloc(...)`: the region validation accepted
  // exactly one allocation site, so any allocation call reaching an
  // assignment to `g` is that site. Re-zero the synthesized backing by
  // storing a fresh default-initialized value (exact calloc semantics on
  // every execution of the statement; malloc's contents are indeterminate,
  // so zero-filling is a legal refinement) and reset the cursor.
  if (asAllocCall(rhs)) {
    if (info.backingSymbol.empty() || info.cursorSymbol.empty())
      return emitError(loc) // Defensive; the region validation forbids it.
             << "unsupported: allocation assigned to this pointer variable";
    Value fresh = createVariablePlace(loc, info.backingType);
    Value zeroed = builder
                       .create<emitrust::LoadOp>(loc, info.backingType, fresh)
                       .getResult();
    builder.create<emitrust::GlobalStoreOp>(loc, zeroed,
                                            globalSymbol(info.backingSymbol));
    builder.create<emitrust::GlobalStoreOp>(loc,
                                            createIntConstant(loc, i64Type, 0),
                                            globalSymbol(info.cursorSymbol));
    return success();
  }
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  if (value->base != info.base || value->literalBacking ||
      value->baseIndex) // Defensive; global regions are single-base.
    return emitError(loc)
           << "unsupported: pointer assignment would rebind to a different "
              "object";
  if (info.cursorSymbol.empty())
    return success(); // Degenerate: the target place is statically known.
  Value cursor =
      value->cursor ? value->cursor : createIntConstant(loc, i64Type, 0);
  builder.create<emitrust::GlobalStoreOp>(loc, cursor,
                                          globalSymbol(info.cursorSymbol));
  return success();
}

LogicalResult CImporter::emitPointerCompoundAssign(
    const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  if (opcode != clang::BO_Add && opcode != clang::BO_Sub)
    return emitError(loc) << "unsupported compound assignment on a pointer";
  // Walking a pointer to a whole row would need a row-scaled step (CTS-P
  // scope).
  if (pointsToArray(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: arithmetic on a pointer to an array";
  const clang::VarDecl *var = asVarRef(op->getLHS());
  const PointerGlobalInfo *globalInfo = nullptr;
  if (!var)
    if (const clang::VarDecl *global = asGlobalDataPointerRef(op->getLHS())) {
      auto globalIt = pointerGlobals.find(global->getCanonicalDecl());
      if (globalIt != pointerGlobals.end())
        globalInfo = &globalIt->second;
    }
  auto it = var ? pointerLocals.find(var) : pointerLocals.end();
  if (it == pointerLocals.end() && !globalInfo)
    return emitError(loc)
           << "unsupported: compound assignment to this pointer expression";
  if (!globalInfo && !it->second.cursorCell)
    // Defensive; the analysis rejects this at the decl.
    return emitError(loc)
           << "unsupported: arithmetic on the address of a scalar object";
  if (globalInfo && globalInfo->cursorSymbol.empty())
    return emitError(loc)
           << "unsupported: arithmetic on the address of a scalar object";
  IntegerType i64Type = builder.getIntegerType(64);
  Value current =
      globalInfo
          ? builder
                .create<emitrust::GlobalLoadOp>(
                    loc, i64Type, globalSymbol(globalInfo->cursorSymbol))
                .getResult()
          : loadPlace(loc, it->second.cursorCell);
  FailureOr<Value> amount = emitRValue(op->getRHS());
  if (failed(amount))
    return failure();
  auto amountType = llvm::dyn_cast<IntegerType>((*amount).getType());
  if (!amountType)
    return emitError(loc) << "unsupported pointer offset type";
  Value offset = castToIntType(loc, *amount, i64Type);
  Value next =
      opcode == clang::BO_Add
          ? builder.create<arith::AddIOp>(loc, current, offset).getResult()
          : builder.create<arith::SubIOp>(loc, current, offset).getResult();
  if (globalInfo)
    builder.create<emitrust::GlobalStoreOp>(
        loc, next, globalSymbol(globalInfo->cursorSymbol));
  else
    builder.create<memref::StoreOp>(loc, next, it->second.cursorCell);
  return success();
}

LogicalResult CImporter::emitIfStmt(const clang::IfStmt *stmt) {
  Location loc = translateLoc(stmt->getIfLoc());
  if (stmt->getConditionVariable() || stmt->getInit())
    return emitError(loc) << "unsupported: declaration in if condition";
  // A compile-time-constant, side-effect-free condition (a literal, a
  // folded `__builtin_expect(!!(0), 0)`, ...) elides the dead arm BEFORE
  // lowering, so a dead arm may contain constructs that could never lower
  // (an unimportable call, a `_Bool` conversion, a declaration). The
  // elision is gated on a live-label check: a goto-targeted label in the
  // dead arm keeps the arm reachable, and a case/default label of an
  // enclosing switch must not be dropped either — both shapes keep the
  // full lowering below (`containsLabelStmt` scans all descendants;
  // `findNestedSwitchLabel` skips nested switches, whose labels are their
  // own and are elided soundly with them).
  clang::Expr::EvalResult conditionValue;
  if (stmt->getCond()->EvaluateAsInt(conditionValue, astContext())) {
    bool truth = conditionValue.Val.getInt() != 0;
    const clang::Stmt *live = truth ? stmt->getThen() : stmt->getElse();
    const clang::Stmt *dead = truth ? stmt->getElse() : stmt->getThen();
    if (!dead || (!containsLabelStmt(dead) &&
                  !llvm::isa<clang::SwitchCase>(dead) &&
                  !findNestedSwitchLabel(dead))) {
      if (live)
        return emitStmt(live);
      return success();
    }
  }
  FailureOr<Value> condition = emitCondition(stmt->getCond());
  if (failed(condition))
    return failure();

  Block *thenBlock = createBlock();
  Block *elseBlock = stmt->getElse() ? createBlock() : nullptr;
  Block *contBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, *condition, thenBlock, ValueRange(),
                                   elseBlock ? elseBlock : contBlock,
                                   ValueRange());

  builder.setInsertionPointToEnd(thenBlock);
  if (failed(emitStmt(stmt->getThen())))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);

  if (const clang::Stmt *elseStmt = stmt->getElse()) {
    builder.setInsertionPointToEnd(elseBlock);
    if (failed(emitStmt(elseStmt)))
      return failure();
    if (!isTerminated(builder.getInsertionBlock()))
      builder.create<cf::BranchOp>(loc, contBlock);
  }

  builder.setInsertionPointToEnd(contBlock);
  return success();
}

LogicalResult CImporter::emitWhileStmt(const clang::WhileStmt *stmt) {
  Location loc = translateLoc(stmt->getWhileLoc());
  if (stmt->getConditionVariable())
    return emitError(loc) << "unsupported: declaration in while condition";

  Block *condBlock = createBlock();
  Block *bodyBlock = createBlock();
  Block *exitBlock = createBlock();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  FailureOr<Value> condition = emitCondition(stmt->getCond());
  if (failed(condition))
    return failure();
  builder.create<cf::CondBranchOp>(loc, *condition, bodyBlock, ValueRange(),
                                   exitBlock, ValueRange());

  builder.setInsertionPointToEnd(bodyBlock);
  loopStack.push_back({exitBlock, condBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitForStmt(const clang::ForStmt *stmt) {
  Location loc = translateLoc(stmt->getForLoc());
  if (stmt->getConditionVariable())
    return emitError(loc) << "unsupported: declaration in for condition";
  if (const clang::Stmt *init = stmt->getInit())
    if (failed(emitStmt(init)))
      return failure();

  Block *condBlock = createBlock();
  Block *bodyBlock = createBlock();
  Block *incBlock = createBlock();
  Block *exitBlock = createBlock();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  if (const clang::Expr *cond = stmt->getCond()) {
    FailureOr<Value> condition = emitCondition(cond);
    if (failed(condition))
      return failure();
    builder.create<cf::CondBranchOp>(loc, *condition, bodyBlock, ValueRange(),
                                     exitBlock, ValueRange());
  } else {
    builder.create<cf::BranchOp>(loc, bodyBlock);
  }

  builder.setInsertionPointToEnd(bodyBlock);
  loopStack.push_back({exitBlock, incBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, incBlock);

  builder.setInsertionPointToEnd(incBlock);
  if (const clang::Expr *inc = stmt->getInc())
    if (failed(emitExprStmt(inc)))
      return failure();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitDoStmt(const clang::DoStmt *stmt) {
  Location loc = translateLoc(stmt->getDoLoc());

  Block *bodyBlock = createBlock();
  Block *condBlock = createBlock();
  Block *exitBlock = createBlock();
  // The body runs at least once: enter it unconditionally.
  builder.create<cf::BranchOp>(loc, bodyBlock);

  builder.setInsertionPointToEnd(bodyBlock);
  loopStack.push_back({exitBlock, condBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  FailureOr<Value> condition = emitCondition(stmt->getCond());
  if (failed(condition))
    return failure();
  builder.create<cf::CondBranchOp>(loc, *condition, bodyBlock, ValueRange(),
                                   exitBlock, ValueRange());

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitSwitchStmt(const clang::SwitchStmt *stmt) {
  Location loc = translateLoc(stmt->getSwitchLoc());
  if (stmt->getConditionVariable() || stmt->getInit())
    return emitError(loc) << "unsupported: declaration in switch condition";

  // Evaluate the controlling expression to an integer flag. An enum
  // condition arrives behind its integral-promotion cast; it is peeled and
  // converted with an explicit `emitrust.cast` so that the (possibly
  // unsigned) promotion type never needs to be mapped.
  Value flag;
  if (std::optional<EnumOperand> component =
          classifyEnumOperand(stmt->getCond())) {
    FailureOr<Value> value = emitEnumOperand(*component, loc);
    if (failed(value))
      return failure();
    flag = castEnumToI32(loc, *value);
  } else {
    FailureOr<Value> value = emitRValue(stmt->getCond());
    if (failed(value))
      return failure();
    flag = *value;
  }
  // An unsigned condition (`unsigned int` and wider are their own promoted
  // types; narrower unsigned types promote to plain `int` and never reach
  // here unsigned) is reinterpreted to signless i64 with an `emitrust.cast`
  // (`as i64`): ui8/ui16/ui32 values zero-extend and ui64 values keep their
  // bit pattern. The case labels below extend to the flag width with the
  // same zero-extension of their APInt bits, so the flag and every label
  // agree bit for bit even for ui64 case values above i64::MAX.
  if (isUnsignedInt(flag.getType()))
    flag = builder
               .create<emitrust::CastOp>(loc, builder.getIntegerType(64),
                                         flag)
               .getResult();
  auto flagType = llvm::dyn_cast<IntegerType>(flag.getType());
  if (!flagType)
    return emitError(loc) << "unsupported: non-integer switch condition";

  const auto *body = llvm::dyn_cast_if_present<clang::CompoundStmt>(
      stmt->getBody());
  if (!body || !isPlainSwitchBody(body))
    return emitDispatchSwitch(stmt, flag, flagType, loc);

  // Partition the body into label sections: every top-level label chain
  // (consecutive case/default labels share one target) starts a section
  // holding the statements up to the next chain. Case values are constant
  // by C semantics; clang has already checked them.
  struct Section {
    Block *block;
    SmallVector<const clang::Stmt *, 4> stmts;
  };
  SmallVector<Section> sections;
  SmallVector<llvm::APInt> caseValues;
  SmallVector<Block *> caseBlocks;
  Block *defaultBlock = nullptr;
  for (const clang::Stmt *child : body->body()) {
    const clang::Stmt *statement = child;
    if (llvm::isa<clang::SwitchCase>(child)) {
      sections.push_back({createBlock(), {}});
      while (const auto *label = llvm::dyn_cast<clang::SwitchCase>(statement)) {
        Location labelLoc = translateLoc(label->getKeywordLoc());
        if (const auto *caseStmt = llvm::dyn_cast<clang::CaseStmt>(label)) {
          if (caseStmt->getRHS())
            return emitError(labelLoc) << "unsupported: GNU case range";
          llvm::APSInt value =
              caseStmt->getLHS()->EvaluateKnownConstInt(astContext());
          caseValues.push_back(value.extOrTrunc(flagType.getWidth()));
          caseBlocks.push_back(sections.back().block);
        } else {
          defaultBlock = sections.back().block;
        }
        statement = label->getSubStmt();
      }
    }
    // `isPlainSwitchBody` guaranteed the first child starts a label chain,
    // so `sections` is never empty here, and no label of this switch hides
    // inside `statement`.
    sections.back().stmts.push_back(statement);
  }

  Block *exitBlock = createBlock();
  SmallVector<ValueRange> caseOperands(caseBlocks.size(), ValueRange());
  builder.create<cf::SwitchOp>(
      loc, flag, defaultBlock ? defaultBlock : exitBlock, ValueRange(),
      llvm::ArrayRef<llvm::APInt>(caseValues), BlockRange(caseBlocks),
      llvm::ArrayRef<ValueRange>(caseOperands));

  // Emit the sections in source order. `break` targets the exit block;
  // `continue` keeps targeting the latch of the enclosing loop, if any. A
  // section that does not end in a terminator falls through to the next
  // section (or, for the last section, to the exit block).
  loopStack.push_back(
      {exitBlock, loopStack.empty() ? nullptr : loopStack.back().continueDest});
  for (auto [index, section] : llvm::enumerate(sections)) {
    builder.setInsertionPointToEnd(section.block);
    LogicalResult sectionResult = success();
    for (const clang::Stmt *statement : section.stmts)
      if (failed(sectionResult = emitStmt(statement)))
        break;
    if (failed(sectionResult)) {
      loopStack.pop_back();
      return failure();
    }
    if (!isTerminated(builder.getInsertionBlock())) {
      Block *next =
          index + 1 < sections.size() ? sections[index + 1].block : exitBlock;
      builder.create<cf::BranchOp>(loc, next);
    }
  }
  loopStack.pop_back();

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitDispatchSwitch(const clang::SwitchStmt *stmt,
                                            Value flag, IntegerType flagType,
                                            Location loc) {
  // Register one block per case/default label of this switch. Clang chains
  // a switch's own labels (wherever they nest inside the body) off
  // `getSwitchCaseList` in reverse source order; labels of nested switches
  // hang off their own SwitchStmt and never appear here. The list is
  // reversed so blocks and `cf.switch` case operands come out in source
  // order deterministically.
  SmallVector<const clang::SwitchCase *> labels;
  for (const clang::SwitchCase *label = stmt->getSwitchCaseList(); label;
       label = label->getNextSwitchCase())
    labels.push_back(label);
  std::reverse(labels.begin(), labels.end());

  SmallVector<llvm::APInt> caseValues;
  SmallVector<Block *> caseBlocks;
  Block *defaultBlock = nullptr;
  for (const clang::SwitchCase *label : labels) {
    Location labelLoc = translateLoc(label->getKeywordLoc());
    Block *block = createBlock();
    switchCaseBlocks[label] = block;
    if (const auto *caseStmt = llvm::dyn_cast<clang::CaseStmt>(label)) {
      if (caseStmt->getRHS())
        return emitError(labelLoc) << "unsupported: GNU case range";
      llvm::APSInt value =
          caseStmt->getLHS()->EvaluateKnownConstInt(astContext());
      caseValues.push_back(value.extOrTrunc(flagType.getWidth()));
      caseBlocks.push_back(block);
    } else {
      defaultBlock = block;
    }
  }

  Block *exitBlock = createBlock();
  SmallVector<ValueRange> caseOperands(caseBlocks.size(), ValueRange());
  builder.create<cf::SwitchOp>(
      loc, flag, defaultBlock ? defaultBlock : exitBlock, ValueRange(),
      llvm::ArrayRef<llvm::APInt>(caseValues), BlockRange(caseBlocks),
      llvm::ArrayRef<ValueRange>(caseOperands));

  // The body is emitted in source order, starting in a fresh block that is
  // reachable only if something branches into it (control enters the body
  // through the dispatch above, or through a goto). Each case/default
  // label reached during the walk redirects emission into its pre-created
  // block (the SwitchCase case of `emitStmt`), so fall-through between
  // labels — including into and out of loop bodies — is the ordinary
  // fall-into branch of an unterminated block. `break` targets the exit
  // block; `continue` keeps targeting the latch of the enclosing loop.
  // Variable places are hoisted to the entry block while the body is
  // emitted (`createVariablePlace`): the dispatch may jump over a
  // declaration, leaving the variable alive but uninitialized, exactly
  // like goto over a declaration (C11 6.2.4p6).
  builder.setInsertionPointToEnd(createBlock());
  loopStack.push_back(
      {exitBlock, loopStack.empty() ? nullptr : loopStack.back().continueDest});
  bool savedHasLabels = currentHasLabels;
  currentHasLabels = true;
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  currentHasLabels = savedHasLabels;
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, exitBlock);
  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitReturnStmt(const clang::ReturnStmt *stmt) {
  Location loc = translateLoc(stmt->getReturnLoc());
  if (const clang::Expr *retValue = stmt->getRetValue()) {
    if (!currentReturnType) {
      if (currentErasedReturnBase) {
        // A classified single-global-base pointer return (CTS-S, 00089):
        // the result was erased from the signature, and the classification
        // pinned every site to `&base` (side-effect free), so the site
        // emits a bare return — no address value, no runtime state.
        builder.create<func::ReturnOp>(loc);
        builder.setInsertionPointToEnd(createBlock());
        return success();
      }
      return emitError(loc)
             << "unsupported: return with a value in a void function";
    }
    FailureOr<Value> value = failure();
    if (isDataPointer(retValue->getType()) &&
        currentReturnType == builder.getIntegerType(64)) {
      // An integer-carrier pointer return (CTS-P3): the function's return
      // type classified to a plain i64, and every return site yields a
      // carrier value.
      value = emitCarrierValue(retValue);
    } else if (isDataPointer(retValue->getType()) &&
               llvm::isa<emitrust::FnPtrType>(currentReturnType)) {
      // A classified fn-address pointer return (CTS-P2): peel the
      // `void *` cast and emit the fn_ptr constant directly.
      const clang::Expr *fnExpr = returnedFunctionExpr(retValue);
      if (!fnExpr) // Defensive; classification pinned every return site.
        return emitError(loc) << "unsupported: returned pointer value";
      const auto *fn = llvm::cast<clang::FunctionDecl>(
          llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
      value = emitFunctionPointerConstant(
          fnExpr, astContext().getPointerType(fn->getType()), loc);
    } else {
      value = emitRValue(retValue);
    }
    if (failed(value))
      return failure();
    if ((*value).getType() != currentReturnType)
      return emitError(loc) << "unsupported: return value type mismatch";
    emitCursorWritebacks(loc);
    builder.create<func::ReturnOp>(loc, *value);
  } else {
    if (currentReturnType)
      return emitError(loc)
             << "unsupported: return without a value in a non-void function";
    emitCursorWritebacks(loc);
    builder.create<func::ReturnOp>(loc);
  }
  // Continue in a fresh block; if it stays unreachable it is erased later.
  builder.setInsertionPointToEnd(createBlock());
  return success();
}

LogicalResult CImporter::emitExprStmt(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  // A full expression containing a materialized temporary (e.g. the
  // `printf("%d\n", f().m)` shape, CTS 00204) is wrapped in
  // ExprWithCleanups; the "cleanup" is the end of the temp's lifetime,
  // which needs no code — unwrap so statement-position calls keep their
  // statement lowerings (the by-name printf intercept in particular).
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    return emitExprStmt(cleanups->getSubExpr());
  if (const auto *compound = llvm::dyn_cast<clang::CompoundAssignOperator>(e))
    return emitCompoundAssign(compound);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (binary->getOpcode() == clang::BO_Assign)
      return emitAssign(binary);
    // A comma in statement position evaluates both operands for their side
    // effects only, so a void-typed right operand is fine here.
    if (binary->getOpcode() == clang::BO_Comma) {
      if (failed(emitExprStmt(binary->getLHS())))
        return failure();
      return emitExprStmt(binary->getRHS());
    }
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->isIncrementDecrementOp())
      return emitIncDec(unary);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e))
    return emitCallStmt(call);
  // A cast to void evaluates its operand for its side effects and discards
  // the value (C11 6.3.2.2). A side-effect-free operand needs no code at
  // all; anything else is re-entered as an expression statement, so calls,
  // assignments, and ++/-- keep their statement-position lowerings. This
  // also covers implicit ToVoid casts, e.g. the non-void arm of a
  // void-typed conditional.
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    if (cast->getCastKind() == clang::CK_ToVoid) {
      if (!cast->getSubExpr()->HasSideEffects(astContext()))
        return success();
      return emitExprStmt(cast->getSubExpr());
    }
  // A void-typed conditional operator (a GNU shape: at least one arm has
  // void type) has no value to materialize, so emitConditionalOperator
  // cannot lower it; in statement position both arms are evaluated for
  // their side effects only, which is exactly an if/else.
  if (const auto *conditional = llvm::dyn_cast<clang::ConditionalOperator>(e))
    if (conditional->getType()->isVoidType())
      return emitVoidConditionalStmt(conditional);
  // A GNU statement expression in statement position (the 00214 `bla`
  // shape): the body statements run inline in the enclosing function and
  // the final expression's value is discarded — a side-effect-free final
  // expression needs no code at all, exactly like a cast to void.
  if (const auto *stmtExpr = llvm::dyn_cast<clang::StmtExpr>(e)) {
    const clang::CompoundStmt *body = stmtExpr->getSubStmt();
    const clang::Stmt *last = body->body_empty() ? nullptr : body->body_back();
    for (const clang::Stmt *child : body->body()) {
      if (child == last)
        if (const auto *lastExpr = llvm::dyn_cast<clang::Expr>(child)) {
          if (!lastExpr->HasSideEffects(astContext()))
            return success();
          return emitExprStmt(lastExpr);
        }
      if (failed(emitStmt(child)))
        return failure();
    }
    return success();
  }
  // Any other expression statement is evaluated and its value discarded.
  return success(succeeded(emitRValue(e)));
}

LogicalResult
CImporter::emitVoidConditionalStmt(const clang::ConditionalOperator *op) {
  Location loc = translateLoc(op->getQuestionLoc());
  // The constant-condition rule of `emitConditionalOperator` applies to
  // the void (statement-position) form identically: a label-free dead
  // arm is elided before lowering, while a goto-targeted label in the
  // dead arm (the 00213 kb_wait_1 shape) or a case/default label of an
  // enclosing switch keeps the FULL if/else lowering below — the
  // constant branch leaves the arm dynamically dead while its labels
  // register with the ordinary goto dispatch.
  clang::Expr::EvalResult conditionValue;
  if (op->getCond()->EvaluateAsInt(conditionValue, astContext())) {
    bool truth = conditionValue.Val.getInt() != 0;
    const clang::Expr *live = truth ? op->getTrueExpr() : op->getFalseExpr();
    const clang::Expr *dead = truth ? op->getFalseExpr() : op->getTrueExpr();
    if (!containsLabelStmt(dead) && !findNestedSwitchLabel(dead))
      return emitExprStmt(live);
  }
  FailureOr<Value> condition = emitCondition(op->getCond());
  if (failed(condition))
    return failure();

  Block *thenBlock = createBlock();
  Block *elseBlock = createBlock();
  Block *contBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, *condition, thenBlock, ValueRange(),
                                   elseBlock, ValueRange());

  builder.setInsertionPointToEnd(thenBlock);
  if (failed(emitExprStmt(op->getTrueExpr())))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);

  builder.setInsertionPointToEnd(elseBlock);
  if (failed(emitExprStmt(op->getFalseExpr())))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);

  builder.setInsertionPointToEnd(contBlock);
  return success();
}

LogicalResult CImporter::emitAssign(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // Reassigning a FILE* handle local (`f = fopen(...)` after fclose, the
  // serial-reuse shape of 00187) stores a fresh handle into its owned
  // place. Non-handle FILE* destinations fall through to the historical
  // pointer paths and their located rejections.
  if (isFilePtrType(op->getLHS()->getType()))
    if (const clang::VarDecl *var = asVarRef(op->getLHS()))
      if (Value place = fileLocals.lookup(var))
        return emitFileOpenInto(place, op->getRHS());
  // Rebinding a decomposed pointer local (or a slice-classified pointer
  // parameter) recomputes its cursor; no pointer value is ever
  // materialized. Function pointers are ordinary values and take the
  // plain place-assignment (or global-store) path below.
  if (isPointerType(op->getLHS()->getType()) &&
      !isFunctionPointer(op->getLHS()->getType())) {
    if (const clang::VarDecl *var = asVarRef(op->getLHS()))
      if (pointerLocals.contains(var) || pointerRegions.tracks(var) ||
          pointerPointerLocals.contains(var) ||
          pointerRegions.tracksSecondOrder(var))
        return storePointerAssign(loc, var, op->getRHS());
    // A pointer-typed global rebinds by storing its global cursor.
    if (const clang::VarDecl *global = asGlobalDataPointerRef(op->getLHS()))
      if (pointerGlobals.contains(global->getCanonicalDecl()))
        return storePointerAssign(loc, global, op->getRHS());
    // `*pp = rhs`: re-pointing through a second-order pointer is exactly
    // an assignment to the first-order pointer it selects (CTS-P5).
    if (const clang::VarDecl *pp = secondOrderDerefVar(op->getLHS())) {
      auto it = pointerPointerLocals.find(pp);
      if (it == pointerPointerLocals.end())
        return emitError(loc) << "unsupported: pointer-to-pointer variable '"
                              << pp->getName()
                              << "' has no bound pointer variable";
      return storePointerAssign(loc, it->second, op->getRHS());
    }
    // `*s = rhs` on a string-cursor parameter (CTS 00204): the
    // advancement writes the parameter's cursor cell; the return-site
    // writebacks make it visible to the caller.
    if (const clang::ParmVarDecl *cursorParam =
            asPointerPointerParamDeref(op->getLHS());
        cursorParam && pointerLocals.contains(cursorParam))
      return storePointerAssign(loc, cursorParam, op->getRHS());
    // A data-pointer struct member holds a statically resolved degenerate
    // binding; the write validates against it and emits nothing (CTS-P2).
    if (dataPointerFieldOf(op->getLHS()))
      return emitMemberPointerAssign(
          llvm::cast<clang::MemberExpr>(stripTrivia(op->getLHS())),
          op->getRHS(), loc);
    return emitError(loc)
           << "unsupported: assignment to this pointer expression";
  }
  // CTS-BR (00216): fn-ptr TABLE slots are never reassigned after their
  // initializer — the folded Some(target) element list is a static fact.
  if (isFunctionPointer(op->getLHS()->getType()))
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(
            stripTrivia(op->getLHS())))
      if (subscript->getBase()
              ->IgnoreParenImpCasts()
              ->getType()
              .getCanonicalType()
              ->isArrayType())
        return emitError(loc)
               << "unsupported: assignment to a function-pointer array "
                  "element";
  // CTS-BR (00216): whole-aggregate assignment over byte-region records
  // is a per-byte region copy when both sides are designators; other
  // right-hand sides (calls) keep the whole-value paths below.
  if (op->getLHS()->getType().getCanonicalType()->isRecordType() &&
      isByteRegionAggregate(op->getLHS()->getType()) &&
      isByteRegionDesignator(op->getLHS()) &&
      isByteRegionDesignator(op->getRHS()))
    return emitByteRegionAggregateAssign(op);
  // Whole-value store to a global in statement position: a direct
  // emitrust.global_store, no staging copy needed. Value-position uses go
  // through emitAssignToPlace, whose staged copy provides the place the
  // surrounding expression loads from.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getLHS())) {
    const GlobalInfo &global = globals.find(var)->second;
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    if ((*value).getType() != global.type)
      return emitError(loc)
             << "unsupported: assigned value type does not match the variable";
    builder.create<emitrust::GlobalStoreOp>(loc, *value,
                                            globalSymbol(global.symbol));
    return success();
  }
  // An element write through a cell-slice parameter (CTS-P10) is an
  // `emitrust.cell_set` on the reference itself; value-position uses keep
  // a located rejection in emitAssignToPlace.
  if (std::optional<CellSliceAccess> access =
          matchCellSliceAccess(op->getLHS()))
    return emitCellSliceAssign(*access, op->getRHS(), loc);
  // A simple store to a bit-field member is the C99-45 read-modify-write
  // accessor; statement position discards the field value.
  if (const auto *memberExpr =
          llvm::dyn_cast<clang::MemberExpr>(stripTrivia(op->getLHS())))
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
      if (field->isBitField())
        return success(succeeded(emitBitFieldAssign(
            memberExpr, op->getRHS(), loc, assignStalenessRisk(op),
            /*wantValue=*/false)));
  return success(succeeded(emitAssignToPlace(op)));
}

FailureOr<Value>
CImporter::emitAssignToPlace(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // A decomposed pointer has no place to re-load the assigned value from;
  // a function pointer is an ordinary value with an ordinary place.
  if (isPointerType(op->getLHS()->getType()) &&
      !isFunctionPointer(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: pointer assignment in value position";
  if (matchCellSliceAccess(op->getLHS()))
    return emitError(loc) << "unsupported: assignment through a cell-slice "
                             "parameter in value position";
  // A value-position store to a bit-field member: the C99-45
  // read-modify-write accessor also stages the truncated post-store field
  // value (C's value of an assignment), returned as a re-loadable place.
  if (const auto *memberExpr =
          llvm::dyn_cast<clang::MemberExpr>(stripTrivia(op->getLHS())))
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
      if (field->isBitField())
        return emitBitFieldAssign(memberExpr, op->getRHS(), loc,
                                  assignStalenessRisk(op), /*wantValue=*/true);
  // A store through a wider-than-element view over a byte region
  // (CTS-P11) widens to a `to_ne_bytes` store over sizeof(T) consecutive
  // bytes; the assignment's value is staged in a temporary so a value
  // position can re-load it.
  if (ByteViewDeref wide = classifyByteViewDeref(op->getLHS());
      wide.wideByte) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveWideByteAccess(wide, loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    Value stored = *value;
    if (stored.getType() != access->valueType) {
      FailureOr<Value> converted =
          convertScalarValue(loc, stored, access->valueType);
      if (failed(converted))
        return failure();
      stored = *converted;
    }
    if (failed(commitGlobalWriteback(
            loc, writeback, assignStalenessRisk(op), [&]() {
              return emitWideByteStore(*access, stored, loc);
            })))
      return failure();
    Value staged = createVariablePlace(loc, access->valueType);
    builder.create<emitrust::AssignOp>(loc, staged, stored);
    return staged;
  }
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  // The destination's value type positions the right-hand side: a
  // refined (callsite-inferred, FR-29 / CTS 00209) fn-ptr place rebinds
  // function references against its refined signature; every other
  // destination takes the ordinary rvalue path unchanged.
  Type assignedType;
  if (auto lvalueType =
          llvm::dyn_cast<emitrust::LValueType>((*place).getType()))
    assignedType = lvalueType.getValueType();
  FailureOr<Value> value = emitPositionedRValue(assignedType, op->getRHS());
  if (failed(value))
    return failure();
  // A store through a union pun arm lands the bit-exactly reinterpreted
  // (slot-typed) value on the slot.
  FailureOr<Value> stored =
      reinterpretUnionArmWrite(op->getLHS(), *value, loc);
  if (failed(stored))
    return failure();
  Value toStore = *stored;
  // A store through a same-width integer view (`*(unsigned int *)p = u`
  // over an int base, CTS-P9) casts the value back to the base element
  // type before assigning: the place is the base element's own place.
  if (classifyByteViewDeref(op->getLHS()).reinterpreted)
    if (auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>((*place).getType()))
      if (lvalueType.getValueType() != toStore.getType())
        toStore = builder
                      .create<emitrust::CastOp>(loc, lvalueType.getValueType(),
                                                toStore)
                      .getResult();
  if (failed(commitGlobalWriteback(
          loc, writeback, assignStalenessRisk(op),
          [&]() { return storeToPlace(loc, *place, toStore); })))
    return failure();
  return place;
}

LogicalResult
CImporter::emitCompoundAssign(const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // `p += n` / `p -= n` on a decomposed pointer local is cursor arithmetic.
  if (isPointerType(op->getLHS()->getType()))
    return emitPointerCompoundAssign(op);
  // Compound assignment to a whole global in statement position:
  // load-modify-store through the global access ops, no staging copy
  // needed. Value-position uses go through emitCompoundAssignToPlace.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getLHS())) {
    const GlobalInfo &global = globals.find(var)->second;
    Value current = builder
                        .create<emitrust::GlobalLoadOp>(
                            loc, global.type, globalSymbol(global.symbol))
                        .getResult();
    FailureOr<Value> result = buildCompoundAssignValue(loc, op, current);
    if (failed(result))
      return failure();
    builder.create<emitrust::GlobalStoreOp>(loc, *result,
                                            globalSymbol(global.symbol));
    return success();
  }
  return success(succeeded(emitCompoundAssignToPlace(op)));
}

FailureOr<Value>
CImporter::emitCompoundAssignToPlace(const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // A decomposed pointer has no place to re-load the assigned value from.
  if (isPointerType(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: pointer assignment in value position";
  if (matchCellSliceAccess(op->getLHS()))
    return emitError(loc) << "unsupported: compound assignment through a "
                             "cell-slice parameter";
  // A compound assignment through a wide byte view (CTS-P11) is a
  // read-modify-write over the same sizeof(T)-byte window: from_ne_bytes
  // load, computation, to_ne_bytes store (and, over a global byte region,
  // the staged copy's writeback).
  if (ByteViewDeref wide = classifyByteViewDeref(op->getLHS());
      wide.wideByte) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveWideByteAccess(wide, loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> current = emitWideByteLoad(*access, loc);
    if (failed(current))
      return failure();
    FailureOr<Value> result = buildCompoundAssignValue(loc, op, *current);
    if (failed(result))
      return failure();
    if (failed(commitGlobalWriteback(
            loc, writeback, assignStalenessRisk(op), [&]() {
              return emitWideByteStore(*access, *result, loc);
            })))
      return failure();
    Value staged = createVariablePlace(loc, access->valueType);
    builder.create<emitrust::AssignOp>(loc, staged, *result);
    return staged;
  }
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  // A union pun arm's place is its slot's: the computation happens on the
  // arm's own type, so the loaded slot value reinterprets to the arm
  // (bit-exact) and the computed result reinterprets back before the
  // store — without this a float arm over an integer slot would
  // VALUE-convert through `convertScalarValue` instead.
  FailureOr<Value> loaded = reinterpretUnionArmRead(op->getLHS(), current, loc);
  if (failed(loaded))
    return failure();
  FailureOr<Value> result = buildCompoundAssignValue(loc, op, *loaded);
  if (failed(result))
    return failure();
  FailureOr<Value> stored = reinterpretUnionArmWrite(op->getLHS(), *result, loc);
  if (failed(stored))
    return failure();
  if (failed(commitGlobalWriteback(
          loc, writeback, assignStalenessRisk(op),
          [&]() { return storeToPlace(loc, *place, *stored); })))
    return failure();
  return place;
}

FailureOr<Value> CImporter::buildCompoundAssignValue(
    Location loc, const clang::CompoundAssignOperator *op, Value current) {
  Type storedType = current.getType();
  FailureOr<Type> computeType = mapType(op->getComputationLHSType(), loc);
  if (failed(computeType))
    return failure();
  // `char/short x; x += wider;`: Sema records the promoted type the
  // operation happens at; widen the loaded LHS to it (a no-op when no
  // promotion applies).
  FailureOr<Value> widened = convertScalarValue(loc, current, *computeType);
  if (failed(widened))
    return failure();
  FailureOr<Value> rhs = emitRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  Value rhsValue = *rhs;
  // The shift amount's C type is independent of the shifted operand's, so
  // `<<=`/`>>=` normalize the right operand to the (widened) left
  // operand's width; every other compound assignment meets its RHS at the
  // computation type Sema already converted it to.
  auto lhsInt = llvm::dyn_cast<IntegerType>((*widened).getType());
  auto rhsInt = llvm::dyn_cast<IntegerType>(rhsValue.getType());
  if ((opcode == clang::BO_Shl || opcode == clang::BO_Shr) && lhsInt && rhsInt)
    rhsValue = castToIntType(loc, rhsValue, lhsInt);
  if ((*widened).getType() != rhsValue.getType())
    return emitError(loc)
           << "unsupported: compound assignment operand type mismatch";
  FailureOr<Value> result = buildBinaryArith(loc, opcode, *widened, rhsValue);
  if (failed(result))
    return failure();
  // C converts the computed value back to the LHS type before storing
  // (C99 6.5.16.2p3 via 6.5.16.1p2).
  return convertScalarValue(loc, *result, storedType);
}

FailureOr<Value> CImporter::convertScalarValue(Location loc, Value value,
                                               Type target) {
  Type source = value.getType();
  if (source == target)
    return value;
  auto sourceInt = llvm::dyn_cast<IntegerType>(source);
  auto targetInt = llvm::dyn_cast<IntegerType>(target);
  // C converts to `_Bool` by comparison against zero, not by truncation;
  // reject rather than lower it wrong.
  if ((sourceInt && sourceInt.getWidth() == 1) ||
      (targetInt && targetInt.getWidth() == 1))
    return emitError(loc) << "unsupported: _Bool conversion";
  if (sourceInt && targetInt)
    return castToIntType(loc, value, targetInt);
  auto sourceFloat = llvm::dyn_cast<FloatType>(source);
  auto targetFloat = llvm::dyn_cast<FloatType>(target);
  if (sourceFloat && targetFloat) {
    if (sourceFloat.getWidth() < targetFloat.getWidth())
      return builder.create<arith::ExtFOp>(loc, targetFloat, value)
          .getResult();
    return builder.create<arith::TruncFOp>(loc, targetFloat, value)
        .getResult();
  }
  if (sourceInt && targetFloat) {
    // Unsigned to float is an `emitrust.cast`: Rust's `u* as f*` performs
    // the same round-to-nearest conversion as C.
    if (sourceInt.isUnsigned())
      return builder.create<emitrust::CastOp>(loc, target, value).getResult();
    return builder.create<arith::SIToFPOp>(loc, target, value).getResult();
  }
  if (sourceFloat && targetInt) {
    // Float to unsigned is an `emitrust.cast`; Rust's `as` saturates where
    // C is undefined, an acceptable defined refinement (matching the
    // `CK_FloatingToIntegral` lowering).
    if (targetInt.isUnsigned())
      return builder.create<emitrust::CastOp>(loc, target, value).getResult();
    return builder.create<arith::FPToSIOp>(loc, target, value).getResult();
  }
  return emitError(loc) << "unsupported scalar conversion";
}

LogicalResult CImporter::emitIncDec(const clang::UnaryOperator *op) {
  // `p++` / `--p` on a decomposed pointer local walks its cursor; the
  // pointer value form of the expression is discarded in statement position.
  if (isPointerType(op->getSubExpr()->getType()))
    return success(succeeded(emitPointerRValue(op)));
  return success(succeeded(emitIncDecValue(op)));
}

FailureOr<Value> CImporter::emitIncDecValue(const clang::UnaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // Pointer ++/-- value forms are consumed by `emitPointerRValue`; a
  // pointer value reaching this scalar path has no representation.
  if (isPointerType(op->getSubExpr()->getType()))
    return emitError(loc) << "unsupported pointer expression in this context";
  // ++/-- on a whole global: load-modify-store through the global access
  // ops, no staging copy needed.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getSubExpr())) {
    const GlobalInfo &global = globals.find(var)->second;
    auto intType = llvm::dyn_cast<IntegerType>(global.type);
    if (!intType)
      return emitError(loc) << "unsupported: ++/-- on a non-integer operand";
    Value current = builder
                        .create<emitrust::GlobalLoadOp>(
                            loc, global.type, globalSymbol(global.symbol))
                        .getResult();
    // createScalarIntConstant/buildBinaryArith cover both the signless
    // (arith) and unsigned (emitrust) domains.
    Value one = createScalarIntConstant(loc, intType, 1);
    clang::BinaryOperatorKind opcode =
        op->isIncrementOp() ? clang::BO_Add : clang::BO_Sub;
    FailureOr<Value> next = buildBinaryArith(loc, opcode, current, one);
    if (failed(next))
      return failure();
    builder.create<emitrust::GlobalStoreOp>(loc, *next,
                                            globalSymbol(global.symbol));
    // C evaluates postfix forms to the original value and prefix forms to
    // the updated one.
    return op->isPostfix() ? current : *next;
  }
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getSubExpr(), &writeback);
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  // A union pun arm's place is its slot's: reinterpret the loaded slot
  // value to the arm's own type first, so ++/-- on a float arm over an
  // integer slot is the (already rejected) non-integer case rather than
  // raw arithmetic on the bit pattern; an integer pun arm computes at
  // its own signedness and reinterprets back before the store.
  FailureOr<Value> loaded =
      reinterpretUnionArmRead(op->getSubExpr(), current, loc);
  if (failed(loaded))
    return failure();
  current = *loaded;
  auto intType = llvm::dyn_cast<IntegerType>(current.getType());
  if (!intType)
    return emitError(loc) << "unsupported: ++/-- on a non-integer operand";
  Value one = createScalarIntConstant(loc, intType, 1);
  clang::BinaryOperatorKind opcode =
      op->isIncrementOp() ? clang::BO_Add : clang::BO_Sub;
  FailureOr<Value> next = buildBinaryArith(loc, opcode, current, one);
  if (failed(next))
    return failure();
  FailureOr<Value> stored =
      reinterpretUnionArmWrite(op->getSubExpr(), *next, loc);
  if (failed(stored))
    return failure();
  // The subexpression's own side effects (a subscript-index call,
  // `g[f()]++`) run after the staging load and force the pre-store
  // refresh of the staged copy.
  if (failed(commitGlobalWriteback(
          loc, writeback, op->getSubExpr()->HasSideEffects(astContext()),
          [&]() { return storeToPlace(loc, *place, *stored); })))
    return failure();
  // C evaluates postfix forms to the original value and prefix forms to
  // the updated one.
  return op->isPostfix() ? current : *next;
}

LogicalResult CImporter::emitCallStmt(const clang::CallExpr *call) {
  const clang::FunctionDecl *callee = call->getDirectCallee();
  // va_start/va_end inside a monomorphization clone (CTS 00204):
  // va_start resets the internal consumption cursor; va_end is a no-op.
  // Outside a clone both are unreachable (clang only admits them in
  // variadic definitions, and every va_list-using definition either
  // monomorphizes or rejects), so the guard is defensive.
  if (callee) {
    switch (callee->getBuiltinID()) {
    case clang::Builtin::BI__builtin_va_start:
    case clang::Builtin::BI__builtin_c23_va_start:
    case clang::Builtin::BI__va_start:
    case clang::Builtin::BIva_start: {
      Location loc = translateLoc(call->getBeginLoc());
      if (!currentVaCloneActive)
        return emitError(loc)
               << "unsupported: va_start outside a variadic definition";
      Value zero =
          createIntConstant(loc, builder.getIntegerType(64), 0);
      builder.create<memref::StoreOp>(loc, zero, currentVaCursorCell);
      return success();
    }
    case clang::Builtin::BI__builtin_va_end:
    case clang::Builtin::BIva_end:
      if (!currentVaCloneActive)
        return emitError(translateLoc(call->getBeginLoc()))
               << "unsupported: va_end outside a variadic definition";
      return success();
    default:
      break;
    }
  }
  if (callee && callee->getDeclName().isIdentifier()) {
    llvm::StringRef name = callee->getName();
    // printf/puts/putchar are intercepted by name only when the project
    // supplies no definition of its own; a user-defined printf (any
    // signature — <stdio.h> is not imported) or puts/putchar is an
    // ordinary call to the imported definition.
    if (name == "printf" && !callee->getDefinition())
      return emitPrintf(call);
    if (name == "puts" && !callee->getDefinition())
      return emitPuts(call);
    if (name == "putchar" && !callee->getDefinition())
      return emitPutchar(call);
    // Hosted <string.h> copy/fill functions (design.md C99-48, CTS-L1) are
    // lowered by name in statement position when the project supplies no
    // definition of its own; C's pointer result (the destination) has no
    // decomposed representation, so value uses keep located rejections in
    // emitCall.
    if (!callee->getDefinition()) {
      // A statement-position `fclose(f)` drops the owned handle (C99-48);
      // its int result has no representation, so value uses keep a
      // located rejection in emitCall.
      if (name == "fclose")
        return emitFileClose(call);
      if (name == "strcpy")
        return emitStringCopyCall(call, "strcpy", /*hasCount=*/false);
      if (name == "strncpy")
        return emitStringCopyCall(call, "strncpy", /*hasCount=*/true);
      if (name == "strcat")
        return emitStringCopyCall(call, "strcat", /*hasCount=*/false);
      if (name == "memset")
        return emitMemsetCall(call);
      if (name == "memcpy")
        return emitMemcpyCall(call, "memcpy");
      // memmove shares memcpy's lowering exactly: distinct char regions
      // never overlap, and the same-object shape already goes through
      // `copy_within`, which is memmove's overlap-correct copy.
      if (name == "memmove")
        return emitMemcpyCall(call, "memmove");
      // A statement-position `exit(status)` terminates the process with
      // C's exit-status semantics (design.md C99-48).
      if (name == "exit")
        return emitExitCall(call);
    }
  }
  // A statement-position call through a devirtualized alias of a hosted
  // variadic (CTS-S, 00189) routes through the printf machinery — the
  // fprintf shape swallows its leading `stdout` argument. Non-variadic
  // aliases fall through to emitCall's direct-call devirtualization.
  if (const clang::FunctionDecl *target = devirtualizedCallee(call))
    if (target->isVariadic())
      return emitAliasedPrintf(call, target);
  // Calls without a direct callee (function pointers) are handled by the
  // indirect path inside emitCall.
  return success(succeeded(emitCall(call)));
}

LogicalResult
CImporter::emitAliasedPrintf(const clang::CallExpr *call,
                             const clang::FunctionDecl *target) {
  Location loc = translateLoc(call->getBeginLoc());
  unsigned formatIndex = 0;
  if (target->getDeclName().isIdentifier() &&
      target->getName() == "fprintf") {
    if (call->getNumArgs() == 0)
      return emitError(loc) << "unsupported: fprintf without a stream "
                               "argument";
    // The swallowed stream slot (the fprintf->printf routing): the ONLY
    // position where a FILE* value is accepted, and only as the literal
    // `stdout`. Every other FILE* use keeps its located rejection.
    const auto *stream = llvm::dyn_cast<clang::DeclRefExpr>(
        call->getArg(0)->IgnoreParenImpCasts());
    const clang::NamedDecl *streamDecl =
        stream ? llvm::dyn_cast<clang::NamedDecl>(stream->getDecl())
               : nullptr;
    if (!streamDecl || !streamDecl->getDeclName().isIdentifier() ||
        streamDecl->getName() != "stdout")
      return emitError(translateLoc(call->getArg(0)->getBeginLoc()))
             << "unsupported: a devirtualized fprintf call requires the "
                "literal 'stdout' stream argument";
    formatIndex = 1;
  }
  if (call->getNumArgs() <= formatIndex)
    return emitError(loc) << "unsupported: printf without a format string";
  const clang::Expr *formatExpr =
      call->getArg(formatIndex)->IgnoreParenImpCasts();
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(formatExpr);
  if (!literal || !literal->isOrdinary())
    return emitError(loc)
           << "unsupported: printf format must be an ordinary string literal";

  SmallVector<Value> operands;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/formatIndex + 1, operands);
  if (failed(rustFormat))
    return failure();

  SmallVector<Attribute> callArguments;
  callArguments.push_back(builder.getStringAttr(*rustFormat));
  for (unsigned i = 0, e = operands.size(); i < e; ++i)
    callArguments.push_back(builder.getIndexAttr(i));
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("print!"),
      builder.getArrayAttr(callArguments), operands);
  return success();
}

LogicalResult CImporter::emitPrintf(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() == 0)
    return emitError(loc) << "unsupported: printf without a format string";
  const clang::Expr *formatExpr = call->getArg(0)->IgnoreParenImpCasts();
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(formatExpr);
  if (!literal || !literal->isOrdinary())
    return emitError(loc)
           << "unsupported: printf format must be an ordinary string literal";

  SmallVector<Value> operands;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/1, operands);
  if (failed(rustFormat))
    return failure();

  SmallVector<Attribute> callArguments;
  callArguments.push_back(builder.getStringAttr(*rustFormat));
  for (unsigned i = 0, e = operands.size(); i < e; ++i)
    callArguments.push_back(builder.getIndexAttr(i));
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("print!"),
      builder.getArrayAttr(callArguments), operands);
  return success();
}

FailureOr<std::string> CImporter::translatePrintfFormat(
    Location loc, const clang::CallExpr *call,
    const clang::StringLiteral *literal, unsigned firstArgIndex,
    SmallVectorImpl<Value> &operands) {
  // Translate the C format string into a Rust format string. The literal's
  // bytes already have C escapes decoded (a "\n" is a real newline byte);
  // the StringAttr printer re-escapes them for the textual assembly.
  llvm::StringRef format = literal->getString();
  std::string rustFormat;
  rustFormat.reserve(format.size());
  unsigned argIndex = firstArgIndex;
  for (size_t i = 0, n = format.size(); i < n; ++i) {
    char c = format[i];
    // C printf stops at an embedded NUL while Rust's print! would emit the
    // remaining bytes, and any byte outside printable ASCII (plus the
    // ordinary whitespace escapes) would reach the generated Rust source
    // verbatim and fail rustc's UTF-8 check; both are rejected rather than
    // silently diverging.
    if (c == '\0')
      return emitError(loc) << "unsupported: NUL byte in printf format";
    if ((c < 0x20 || c > 0x7e) && c != '\n' && c != '\t' && c != '\r')
      return emitError(loc)
             << "unsupported: non-printable or non-ASCII byte in printf "
                "format";
    if (c == '{') {
      rustFormat += "{{";
      continue;
    }
    if (c == '}') {
      rustFormat += "}}";
      continue;
    }
    if (c != '%') {
      rustFormat += c;
      continue;
    }
    if (++i >= n)
      return emitError(loc) << "unsupported: trailing '%' in printf format";
    if (format[i] == '%') {
      rustFormat += '%';
      continue;
    }
    // Parse `%[flags][width][.precision][length]conv` (C99 7.19.6.1). All
    // five C99 flags are recognized; width and precision are decimal
    // numbers ('*' forms consume a runtime argument and stay rejected);
    // lengths l/ll (64-bit) and h/hh (short/char range) are supported, L
    // is accepted on the floating conversions (long-double-as-f64, CTS
    // 00204), and j/z/t stay rejected.
    bool leftAlign = false;
    bool zeroPad = false;
    bool plusSign = false;
    bool spaceSign = false;
    bool altForm = false;
    while (i < n) {
      char flag = format[i];
      if (flag == '-')
        leftAlign = true;
      else if (flag == '0')
        zeroPad = true;
      else if (flag == '+')
        plusSign = true;
      else if (flag == ' ')
        spaceSign = true;
      else if (flag == '#')
        altForm = true;
      else
        break;
      ++i;
    }
    if (i < n && format[i] == '*')
      return emitError(loc)
             << "unsupported: '*' field width in printf format";
    std::string width;
    while (i < n && format[i] >= '0' && format[i] <= '9')
      width += format[i++];
    if (width.size() > 9)
      return emitError(loc) << "unsupported: printf field width too large";
    int precision = -1;
    if (i < n && format[i] == '.') {
      ++i;
      if (i < n && format[i] == '*')
        return emitError(loc)
               << "unsupported: '*' precision in printf format";
      std::string precisionDigits;
      while (i < n && format[i] >= '0' && format[i] <= '9')
        precisionDigits += format[i++];
      if (precisionDigits.size() > 9)
        return emitError(loc) << "unsupported: printf precision too large";
      // A '.' with no digits is precision zero (C99 7.19.6.1p4).
      precision = precisionDigits.empty() ? 0 : std::stoi(precisionDigits);
    }
    enum class Length { None, Long, LongLong, Short, Char, LongDouble };
    Length lengthMod = Length::None;
    if (i < n && format[i] == 'l') {
      lengthMod = Length::Long;
      ++i;
      if (i < n && format[i] == 'l') {
        lengthMod = Length::LongLong;
        ++i;
      }
    } else if (i < n && format[i] == 'h') {
      lengthMod = Length::Short;
      ++i;
      if (i < n && format[i] == 'h') {
        lengthMod = Length::Char;
        ++i;
      }
    } else if (i < n && format[i] == 'L') {
      // The long double length modifier (CTS 00204): accepted on the
      // floating conversions, where the long-double-as-f64 policy makes
      // it behave exactly like the unmodified twin; rejected on the
      // integer conversions (undefined in C99 7.19.6.1p7) below.
      lengthMod = Length::LongDouble;
      ++i;
    } else if (i < n && (format[i] == 'j' || format[i] == 'z' ||
                         format[i] == 't')) {
      return emitError(loc) << "unsupported printf length modifier '"
                            << llvm::Twine(std::string(1, format[i])) << "'";
    }
    if (i >= n)
      return emitError(loc) << "unsupported: trailing '%' in printf format";
    char spec = format[i];
    std::string specName(1, spec);
    // The unknown-conversion diagnostic names the directive as spelled:
    // %La (the long-double hex-float form, whose output would render the
    // bits of the native 80-bit value) reports '%La', not '%a'.
    std::string directiveName =
        (lengthMod == Length::LongDouble ? "L" : "") + specName;
    // Validate the conversion before consuming an argument so an unknown
    // conversion is always the diagnostic, even when arguments are short.
    // %p stays rejected by design: pointer provenance is compiled away by
    // the pointer decomposition, so no address exists to print.
    bool isSignedConv = spec == 'd' || spec == 'i';
    bool isUnsignedConv =
        spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o';
    bool isFloatConv = spec == 'f' || spec == 'F' || spec == 'e' ||
                       spec == 'E' || spec == 'g' || spec == 'G';
    if (!isSignedConv && !isUnsignedConv && !isFloatConv && spec != 'c' &&
        spec != 's')
      return emitError(loc) << "unsupported printf format specifier '%"
                            << directiveName << "'";
    // 'L' applies only to the floating conversions; on the integer ones
    // it is undefined in C99 and stays a located rejection (CTS 00204).
    if (lengthMod == Length::LongDouble && (isSignedConv || isUnsignedConv))
      return emitError(loc)
             << "unsupported: length modifier 'L' on printf '%" << specName
             << "'";
    // Flag and length validity (C99 7.19.6.1p6-7): '+'/' ' are defined
    // only for the signed and floating conversions, '#' only for x/X/o
    // and the floating conversions; both are undefined elsewhere and are
    // rejected rather than silently dropped. h/hh apply only to the
    // integer conversions; ll does not apply to the floating ones (l on a
    // floating conversion has no effect and is accepted, C99 7.19.6.1p7).
    if ((plusSign || spaceSign) && !isSignedConv && !isFloatConv)
      return emitError(loc) << "unsupported: '+' or ' ' flag on printf '%"
                            << specName << "'";
    if (altForm && !isFloatConv && spec != 'x' && spec != 'X' && spec != 'o')
      return emitError(loc)
             << "unsupported: '#' flag on printf '%" << specName << "'";
    if ((lengthMod == Length::Short || lengthMod == Length::Char) &&
        !isSignedConv && !isUnsignedConv)
      return emitError(loc)
             << "unsupported: length modifier 'h' on printf '%" << specName
             << "'";
    if (lengthMod == Length::LongLong && isFloatConv)
      return emitError(loc)
             << "unsupported: length modifier 'll' on printf '%" << specName
             << "'";
    if (lengthMod != Length::None && (spec == 'c' || spec == 's'))
      return emitError(loc) << "unsupported: length modifier on printf '%"
                            << specName << "'";
    if (zeroPad && (spec == 'c' || spec == 's'))
      return emitError(loc)
             << "unsupported: '0' flag on printf '%" << specName << "'";
    if (precision >= 0 && spec == 'c')
      return emitError(loc) << "unsupported: precision on printf '%c'";
    bool isLong =
        lengthMod == Length::Long || lengthMod == Length::LongLong;
    // Renders the Rust format placeholder for a numeric directive: the C
    // width maps 1:1 ("%5d" -> "{:5}"), '-' to left alignment ("%-5d" ->
    // "{:<5}"), '0' to Rust's sign-aware zero pad ("%05d" -> "{:05}"),
    // and x/X/o append their radix marker ("%04X" -> "{:04X}"). A flag
    // without a width is a no-op in C and is dropped. C ignores '0' when
    // '-' is present, so left alignment wins.
    auto placeholderFor = [&](llvm::StringRef radix) {
      if (width.empty() && radix.empty())
        return std::string("{}");
      std::string text = "{:";
      if (!width.empty()) {
        if (leftAlign)
          text += '<';
        else if (zeroPad)
          text += '0';
        text += width;
      }
      text += radix.str();
      text += '}';
      return text;
    };
    // Renders the placeholder for a %c/%s directive with a width: C
    // right-aligns text to the field by default where Rust's string
    // formatting left-aligns, so the alignment is always explicit.
    auto textPlaceholder = [&]() {
      if (width.empty())
        return std::string("{}");
      std::string text = "{:";
      text += leftAlign ? '<' : '>';
      text += width;
      text += '}';
      return text;
    };
    // The C99-flag bitmask shared by the `__emitrust_fmt_*` helpers
    // (kept in sync with the emitted helper sources): '-'=1, '0'=2,
    // '+'=4, ' '=8, '#'=16, uppercase conversion=32.
    int flagsMask = (leftAlign ? 1 : 0) | (zeroPad ? 2 : 0) |
                    (plusSign ? 4 : 0) | (spaceSign ? 8 : 0) |
                    (altForm ? 16 : 0);
    int widthValue = width.empty() ? 0 : std::stoi(width);
    auto i32Type = builder.getIntegerType(32);
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    if (argIndex >= call->getNumArgs())
      return emitError(loc) << "unsupported: too few arguments to printf";
    const clang::Expr *argExpr = call->getArg(argIndex);
    unsigned argNumber = argIndex++;

    if (spec == 's') {
      std::optional<unsigned> stringPrecision;
      if (precision >= 0)
        stringPrecision = static_cast<unsigned>(precision);
      FailureOr<Value> text = emitPrintfStringArg(argExpr, stringPrecision);
      if (failed(text))
        return failure();
      operands.push_back(*text);
      rustFormat += textPlaceholder();
      continue;
    }

    FailureOr<Value> argument = emitRValue(argExpr);
    if (failed(argument))
      return failure();
    Type argType = (*argument).getType();

    if (isFloatConv) {
      if (!llvm::isa<Float64Type>(argType))
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      bool plainF = spec == 'f' && precision < 0 && width.empty() &&
                    !leftAlign && !zeroPad && !plusSign && !spaceSign &&
                    !altForm;
      if (plainF) {
        // C's %f prints six decimals; Rust's {:.6} matches it for every
        // finite value and for infinities, but spells NaN as "NaN" where
        // C prints "nan"/"-nan". The argument is therefore routed through
        // the module-level `__emitrust_fmt_f64` helper (emitted once, on
        // demand) and printed with a plain `{}`. (`%lf` is identical to
        // `%f` in C99.)
        needsFloatFormatHelper = true;
        *argument = builder
                        .create<emitrust::CallOpaqueOp>(
                            loc, TypeRange{stringType},
                            builder.getStringAttr("__emitrust_fmt_f64"),
                            /*args=*/ArrayAttr(), ValueRange{*argument})
                        .getResult(0);
        operands.push_back(*argument);
        rustFormat += "{}";
        continue;
      }
      // Every other floating directive goes through the module-level
      // `__emitrust_fmt_float` helper, which implements the C99 f/e/g
      // algorithms (including the glibc %#g rounding-carry quirk) over
      // Rust's exact correctly-rounded decimal conversion; the directive's
      // compile-time parameters travel as i32 constants.
      int convCode = (spec == 'e' || spec == 'E') ? 1
                     : (spec == 'g' || spec == 'G') ? 2
                                                    : 0;
      if (spec == 'F' || spec == 'E' || spec == 'G')
        flagsMask |= 32;
      needsFloatFormatExtHelper = true;
      Value convValue = createIntConstant(loc, i32Type, convCode);
      Value precisionValue = createIntConstant(loc, i32Type, precision);
      Value widthConst = createIntConstant(loc, i32Type, widthValue);
      Value flagsValue = createIntConstant(loc, i32Type, flagsMask);
      Value formatted =
          builder
              .create<emitrust::CallOpaqueOp>(
                  loc, TypeRange{stringType},
                  builder.getStringAttr("__emitrust_fmt_float"),
                  /*args=*/ArrayAttr(),
                  ValueRange{*argument, convValue, precisionValue,
                             widthConst, flagsValue})
              .getResult(0);
      operands.push_back(formatted);
      rustFormat += "{}";
      continue;
    }

    auto argIntType = llvm::dyn_cast<IntegerType>(argType);
    bool isIntArgument = argIntType && argIntType.getWidth() > 1;

    if (spec == 'c') {
      // C converts the argument to unsigned char and prints that byte;
      // the i32 argument (chars arrive int-promoted) goes through the
      // `__emitrust_fmt_c` helper (ASCII-only, see design.md C99-48).
      if (!isIntArgument)
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      operands.push_back(wrapCharFormat(loc, *argument));
      rustFormat += textPlaceholder();
      continue;
    }

    // Integer conversions. d/i print signed; u/x/X/o print the value as
    // unsigned, so the argument is `as`-cast to the unsigned type of the
    // directive's width — a negative signed argument then prints its
    // two's-complement bit pattern ("%x" of -1 is ffffffff), exactly like
    // C. An argument of a different width is `as`-cast as well, which
    // truncates to the low bits just like C's varargs read on x86-64
    // (printf("%d", sizeof(x)) prints the low 32 bits of the size_t); the
    // h/hh lengths reuse the same cast to reduce the int-promoted
    // argument to short/char range (C99 7.19.6.1p7).
    llvm::StringRef radix;
    switch (spec) {
    case 'x':
      radix = "x";
      break;
    case 'X':
      radix = "X";
      break;
    case 'o':
      radix = "o";
      break;
    default:
      break;
    }
    if (!isIntArgument)
      return emitError(loc) << "unsupported: printf argument " << argNumber
                            << " does not match its format specifier";
    unsigned bits = isLong                      ? 64
                    : lengthMod == Length::Short ? 16
                    : lengthMod == Length::Char  ? 8
                                                 : 32;
    IntegerType target =
        isSignedConv
            ? builder.getIntegerType(bits)
            : IntegerType::get(builder.getContext(), bits,
                               IntegerType::Unsigned);
    Value narrowed = castToIntType(loc, *argument, target);
    if (precision < 0 && !plusSign && !spaceSign && !altForm) {
      // Flags/width-only directives map 1:1 onto Rust format specs.
      operands.push_back(narrowed);
      rustFormat += placeholderFor(radix);
      continue;
    }
    // Precision or the '+'/' '/'#' flags have no Rust format equivalent
    // with C semantics ('0' is ignored next to a precision, the sign and
    // 0x/0 prefixes sit inside the zero padding, ...); the directive goes
    // through the module-level `__emitrust_fmt_i64`/`__emitrust_fmt_u64`
    // helpers, which implement the C99 rules exactly over the value
    // widened to 64 bits (sign- or zero-extended per the conversion).
    if (spec == 'X')
      flagsMask |= 32;
    Value widened = castToIntType(
        loc, narrowed,
        isSignedConv ? builder.getIntegerType(64)
                     : IntegerType::get(builder.getContext(), 64,
                                        IntegerType::Unsigned));
    Value precisionValue = createIntConstant(loc, i32Type, precision);
    Value widthConst = createIntConstant(loc, i32Type, widthValue);
    Value flagsValue = createIntConstant(loc, i32Type, flagsMask);
    SmallVector<Value> helperArgs{widened};
    llvm::StringRef helperName = "__emitrust_fmt_i64";
    if (!isSignedConv) {
      helperName = "__emitrust_fmt_u64";
      int base = spec == 'o' ? 8 : spec == 'u' ? 10 : 16;
      helperArgs.push_back(createIntConstant(loc, i32Type, base));
      needsIntFormatUnsignedHelper = true;
    } else {
      needsIntFormatSignedHelper = true;
    }
    helperArgs.push_back(precisionValue);
    helperArgs.push_back(widthConst);
    helperArgs.push_back(flagsValue);
    Value formatted = builder
                          .create<emitrust::CallOpaqueOp>(
                              loc, TypeRange{stringType},
                              builder.getStringAttr(helperName),
                              /*args=*/ArrayAttr(), helperArgs)
                          .getResult(0);
    operands.push_back(formatted);
    rustFormat += "{}";
  }
  if (argIndex != call->getNumArgs())
    return emitError(loc) << "unsupported: too many arguments to printf";
  return rustFormat;
}

FailureOr<Value> CImporter::emitSprintf(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() < 2)
    return emitError(loc) << "unsupported: sprintf requires a destination "
                             "and a format string";
  const clang::Expr *formatExpr = call->getArg(1)->IgnoreParenImpCasts();
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(formatExpr);
  if (!literal || !literal->isOrdinary())
    return emitError(loc)
           << "unsupported: sprintf format must be an ordinary string literal";
  FailureOr<PtrExprValue> dst = emitCharRegionArg(call->getArg(0));
  if (failed(dst))
    return failure();

  // The format arguments materialize first (through the printf-shared
  // directive grammar) and collapse into a String, so no load intervenes
  // between the mutable destination borrow below and the helper call
  // consuming it.
  SmallVector<Value> operands;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/2, operands);
  if (failed(rustFormat))
    return failure();
  SmallVector<Attribute> callArguments;
  callArguments.push_back(builder.getStringAttr(*rustFormat));
  for (unsigned i = 0, e = operands.size(); i < e; ++i)
    callArguments.push_back(builder.getIndexAttr(i));
  auto stringType = emitrust::OpaqueType::get(builder.getContext(), "String");
  Value text = builder
                   .create<emitrust::CallOpaqueOp>(
                       loc, TypeRange{stringType},
                       builder.getStringAttr("format!"),
                       builder.getArrayAttr(callArguments), operands)
                   .getResult(0);

  // The helper's pinned `s: &str` parameter is fed a `&String` borrow
  // (deref coercion applies at the argument position): the String value
  // has no place of its own, so it is staged through a String variable
  // whose shared borrow is taken before the destination's mutable borrow
  // below (distinct objects, so the borrows coexist).
  Value stringPlace =
      builder
          .create<emitrust::VariableOp>(loc,
                                        emitrust::LValueType::get(stringType))
          .getResult();
  builder.create<emitrust::AssignOp>(loc, stringPlace, text);
  Value textRef = builder
                      .create<emitrust::AddrOfOp>(
                          loc, emitrust::RefType::get(stringType), stringPlace,
                          /*isMut=*/false)
                      .getResult();

  // The destination borrows mutably from its cursor, exactly like the
  // <string.h> copy helpers (a string-literal region rejects here).
  FailureOr<Value> dstSlice = emitCharRegionSlice(loc, *dst, /*isMut=*/true);
  if (failed(dstSlice))
    return failure();
  needsSprintfHelper = true;
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI32Type()},
          builder.getStringAttr("__emitrust_sprintf"),
          /*args=*/ArrayAttr(), ValueRange{*dstSlice, textRef})
      .getResult(0);
}

FailureOr<Value>
CImporter::emitPrintfStringArg(const clang::Expr *expr,
                               std::optional<unsigned> precision) {
  // The array-to-pointer decay wrapping both supported shapes is implicit;
  // strip it (and parentheses) to see the underlying literal or lvalue.
  // A `__func__`-family predefined identifier prints its function-name
  // literal through the same literal path (C99-29); the check runs before
  // the char-array branch below, which would otherwise claim the
  // predefined identifier's `const char[N]` lvalue type.
  const clang::Expr *arg = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(arg->getBeginLoc());
  if (const clang::StringLiteral *literal = underlyingStringLiteral(arg)) {
    if (!literal->isOrdinary())
      return emitError(loc)
             << "unsupported: non-ordinary string literal in printf '%s'";
    // The literal's decoded bytes become a Rust string literal emitted
    // verbatim into the generated source: an embedded NUL would diverge
    // from C (which stops printing there) and a non-ASCII byte would fail
    // rustc's UTF-8 check, so both are rejected; quote, backslash, and the
    // whitespace escapes are re-escaped for the Rust spelling.
    // A %.Ns precision truncates at import time: C never reads past the
    // Nth byte, so only the retained prefix is validated below.
    llvm::StringRef data = literal->getString();
    if (precision && *precision < data.size())
      data = data.take_front(*precision);
    std::string text = "\"";
    for (char c : data) {
      if (c == '\0')
        return emitError(loc)
               << "unsupported: NUL byte in printf '%s' string literal";
      if ((c < 0x20 || c > 0x7e) && c != '\n' && c != '\t' && c != '\r')
        return emitError(loc) << "unsupported: non-printable or non-ASCII "
                                 "byte in printf '%s' string literal";
      switch (c) {
      case '\n':
        text += "\\n";
        break;
      case '\t':
        text += "\\t";
        break;
      case '\r':
        text += "\\r";
        break;
      case '"':
        text += "\\\"";
        break;
      case '\\':
        text += "\\\\";
        break;
      default:
        text += c;
      }
    }
    text += '"';
    auto strType =
        emitrust::OpaqueType::get(builder.getContext(), "&'static str");
    return builder
        .create<emitrust::LiteralOp>(loc, strType, builder.getStringAttr(text))
        .getResult();
  }
  // Renders a borrowed i8 slice through the on-demand `__emitrust_cstr`
  // helper (stops at the first NUL, like C's %s) or, under a %.Ns
  // precision, through `__emitrust_cstr_n` (stops at N bytes or the first
  // NUL, whichever comes first; C99 7.19.6.1p8 allows the array to lack a
  // terminator when the precision bounds the read).
  auto wrapCStr = [&](Location loc, Value slice) -> Value {
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    if (precision) {
      needsCStrNHelper = true;
      Value count = createIntConstant(loc, builder.getIntegerType(64),
                                      static_cast<int64_t>(*precision));
      return builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{stringType},
              builder.getStringAttr("__emitrust_cstr_n"),
              /*args=*/ArrayAttr(), ValueRange{slice, count})
          .getResult(0);
    }
    needsCStrHelper = true;
    return builder
        .create<emitrust::CallOpaqueOp>(loc, TypeRange{stringType},
                                        builder.getStringAttr("__emitrust_cstr"),
                                        /*args=*/ArrayAttr(),
                                        ValueRange{slice})
        .getResult(0);
  };
  // A char-array lvalue is borrowed whole (`emitrust.slice_of` at index 0)
  // and rendered by the `__emitrust_cstr` helper, which — like C's %s —
  // stops at the first NUL.
  if (astContext().getAsConstantArrayType(arg->getType()) &&
      arg->isLValue()) {
    FailureOr<Value> place = emitLValue(arg);
    if (failed(place))
      return failure();
    auto lvalueType = llvm::cast<emitrust::LValueType>((*place).getType());
    auto arrayType =
        llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType());
    if (!arrayType || arrayType.getElementType() != builder.getIntegerType(8))
      return emitError(loc)
             << "unsupported: printf '%s' argument must be a string literal "
                "or a char array";
    Value zero = createIntConstant(loc, builder.getIntegerType(64), 0);
    auto sliceRefType = emitrust::RefType::get(
        emitrust::SliceType::get(arrayType.getElementType()));
    Value slice = builder
                      .create<emitrust::SliceOfOp>(loc, sliceRefType, *place,
                                                   zero, /*is_mut=*/false)
                      .getResult();
    return wrapCStr(loc, slice);
  }
  // A strchr/strrchr result prints the searched region's byte run from
  // the found index: the helper's i64 index (relative to the argument's
  // cursor) offsets the cursor, and the region is re-sliced there for
  // `__emitrust_cstr`. A not-found result is C's NULL, whose %s print is
  // undefined in C; the -1 index makes the slice borrow panic instead of
  // reading out of bounds.
  bool reverse = false;
  if (const clang::CallExpr *search = asHostedStrchrCall(arg, reverse)) {
    PtrExprValue region;
    FailureOr<Value> index = emitStrchrIndex(search, reverse, region);
    if (failed(index))
      return failure();
    Value found =
        builder.create<arith::AddIOp>(loc, region.cursor, *index)
            .getResult();
    PtrExprValue at{region.base, found, region.literalBacking};
    FailureOr<Value> slice = emitCharRegionSlice(loc, at, /*isMut=*/false);
    if (failed(slice))
      return failure();
    return wrapCStr(loc, *slice);
  }
  // `&arr[i]` (or `&p[i]` over a decomposed pointer) prints the region's
  // byte run from element i, through the same slice + `__emitrust_cstr`
  // lowering as the whole-array shape (CTS-L1; 00180.c prints &a[1]).
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(arg))
    if (unary->getOpcode() == clang::UO_AddrOf &&
        llvm::isa<clang::ArraySubscriptExpr>(
            stripTrivia(unary->getSubExpr()))) {
      FailureOr<PtrExprValue> pointer = emitCharRegionArg(arg);
      if (failed(pointer))
        return failure();
      FailureOr<Value> slice =
          emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
      if (failed(slice))
        return failure();
      return wrapCStr(loc, *slice);
    }
  // A decomposed `char *` prints the backing byte run from its cursor:
  // `emitrust.slice_of` of the region place at the cursor, rendered by
  // the same `__emitrust_cstr` helper as char arrays (both stop at the
  // first NUL, like C's %s). Two region shapes qualify: a pointer into a
  // string-literal region (its read-only backing array, CTS-P1) and the
  // FR-28 slice-parameter class (a slice-classified `char *` parameter,
  // whose base place is the deref'd `!emitrust.lvalue<!emitrust.slice<i8>>`,
  // CTS-L2). The decomposed-pointer gate keeps pointer-shaped arguments
  // without a decomposed pointer (casts of scalar addresses, ...) on the
  // generic rejection below.
  if (isPointerType(expr->getType()) && involvesDecomposedPointer(expr) &&
      isDecomposedPointerExpr(expr)) {
    FailureOr<PtrExprValue> pointer = emitPointerRValue(expr);
    if (failed(pointer))
      return failure();
    Value backingPlace = pointer->literalBacking;
    Type elementType;
    if (backingPlace) {
      auto lvalueType =
          llvm::cast<emitrust::LValueType>(backingPlace.getType());
      elementType = llvm::cast<emitrust::ArrayType>(lvalueType.getValueType())
                        .getElementType();
    } else if (pointer->base) {
      auto it = symbols.find(pointer->base);
      if (it != symbols.end()) {
        auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>(it->second.getType());
        auto sliceType =
            lvalueType ? llvm::dyn_cast<emitrust::SliceType>(
                             lvalueType.getValueType())
                       : emitrust::SliceType();
        if (sliceType &&
            sliceType.getElementType() == builder.getIntegerType(8)) {
          backingPlace = it->second;
          elementType = sliceType.getElementType();
        }
      } else if (!pointer->base->hasLocalStorage()) {
        // A pointer into a global char array (the 00217 shape) prints the
        // staged copy's byte run: the copy is taken fresh at the print,
        // so every earlier write — element, wide-byte, or cell — is
        // visible in it (CTS-P11).
        FailureOr<std::pair<Value, std::string>> staged =
            stageGlobalCopy(loc, pointer->base);
        if (failed(staged))
          return failure();
        auto lvalueType =
            llvm::cast<emitrust::LValueType>(staged->first.getType());
        auto arrayType =
            llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType());
        if (arrayType &&
            arrayType.getElementType() == builder.getIntegerType(8)) {
          backingPlace = staged->first;
          elementType = arrayType.getElementType();
        }
      }
    }
    if (!backingPlace)
      return emitError(loc)
             << "unsupported: printf '%s' argument must be a string literal, "
                "a char array, a char slice parameter, or a pointer into a "
                "string literal";
    Value cursor =
        pointer->cursor
            ? pointer->cursor
            : createIntConstant(loc, builder.getIntegerType(64), 0);
    auto sliceRefType =
        emitrust::RefType::get(emitrust::SliceType::get(elementType));
    Value slice = builder
                      .create<emitrust::SliceOfOp>(loc, sliceRefType,
                                                   backingPlace, cursor,
                                                   /*is_mut=*/false)
                      .getResult();
    return wrapCStr(loc, slice);
  }
  return emitError(loc) << "unsupported: printf '%s' argument must be a "
                           "string literal or a char array";
}

Value CImporter::wrapCharFormat(Location loc, Value value) {
  needsCharFormatHelper = true;
  Value promoted = castToIntType(loc, value, builder.getI32Type());
  auto charType = emitrust::OpaqueType::get(builder.getContext(), "char");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{charType}, builder.getStringAttr("__emitrust_fmt_c"),
          /*args=*/ArrayAttr(), ValueRange{promoted})
      .getResult(0);
}

LogicalResult CImporter::emitPuts(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc) << "unsupported: puts requires exactly one argument";
  // C's puts writes the string then a newline; println! of the %s-shaped
  // value matches byte-for-byte (both supported shapes reject the bytes
  // Rust could not reproduce).
  FailureOr<Value> text = emitPrintfStringArg(call->getArg(0));
  if (failed(text))
    return failure();
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("println!"),
      builder.getArrayAttr(
          {builder.getStringAttr("{}"), builder.getIndexAttr(0)}),
      ValueRange{*text});
  return success();
}

FailureOr<Value> CImporter::emitStrlenCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: strlen requires exactly one argument";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  needsStrlenHelper = true;
  Value count =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{builder.getIntegerType(64)},
              builder.getStringAttr("__emitrust_strlen"),
              /*args=*/ArrayAttr(), ValueRange{*slice})
          .getResult(0);
  // Convert the i64 count to the call's declared result type (`int` in the
  // K&R-style `int strlen(char *)` prototype, size_t otherwise), matching
  // C's conversion of the returned value.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: strlen result type";
  return castToIntType(loc, count, intType);
}

//===----------------------------------------------------------------------===//
// Hosted <stdio.h> FILE* streams (design.md C99-48, CTS-T1.3, 00187)
//===----------------------------------------------------------------------===//

emitrust::OpaqueType CImporter::fileHandleType() {
  return emitrust::OpaqueType::get(builder.getContext(), "__EmitrustFile");
}

void CImporter::requestFileHelper(llvm::StringRef name) {
  // Every helper mentions the handle enum, and the byte-loop helpers call
  // the fgetc primitive.
  neededFileHelpers.insert("__EmitrustFile");
  if (name == "__emitrust_fread" || name == "__emitrust_fgets")
    neededFileHelpers.insert("__emitrust_fgetc");
  neededFileHelpers.insert(name);
}

LogicalResult CImporter::emitFileLocal(const clang::VarDecl *var,
                                       Location loc) {
  // The owned handle lives in an `emitrust.variable` place; without an
  // initializer it renders as `__EmitrustFile::Null` (C's NULL), so the
  // enum definition is needed as soon as a handle local exists.
  requestFileHelper("__EmitrustFile");
  Value place = createVariablePlace(loc, fileHandleType());
  fileLocals[var] = place;
  if (const clang::Expr *init = var->getInit())
    return emitFileOpenInto(place, init);
  return success();
}

LogicalResult CImporter::emitFileOpenInto(Value place,
                                          const clang::Expr *init) {
  const clang::Expr *e = init->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  const auto *call = llvm::dyn_cast<clang::CallExpr>(e);
  const clang::FunctionDecl *callee =
      call ? call->getDirectCallee() : nullptr;
  if (!callee || !callee->getDeclName().isIdentifier() ||
      callee->getName() != "fopen" || callee->getDefinition())
    return emitError(loc) << "unsupported: a FILE* local may only be "
                             "initialized or assigned by fopen";
  if (call->getNumArgs() != 2)
    return emitError(loc)
           << "unsupported: fopen requires a path and a mode";
  // The mode selects the helper. Exactly "r" and "w" are in the slice:
  // append/update modes and the (POSIX no-op) binary suffix stay out.
  const auto *modeLiteral = llvm::dyn_cast<clang::StringLiteral>(
      call->getArg(1)->IgnoreParenImpCasts());
  if (!modeLiteral || !modeLiteral->isOrdinary())
    return emitError(translateLoc(call->getArg(1)->getBeginLoc()))
           << "unsupported: fopen mode must be a string literal";
  llvm::StringRef mode = modeLiteral->getString();
  if (mode != "r" && mode != "w")
    return emitError(translateLoc(call->getArg(1)->getBeginLoc()))
           << "unsupported: fopen mode \"" << mode
           << "\" (only \"r\" and \"w\" are supported)";
  // Literal-only paths (v1): the decoded bytes are re-emitted as a Rust
  // string literal, so they must be printable ASCII (an embedded NUL
  // would also diverge from C's NUL-terminated path).
  const auto *pathLiteral = llvm::dyn_cast<clang::StringLiteral>(
      call->getArg(0)->IgnoreParenImpCasts());
  Location pathLoc = translateLoc(call->getArg(0)->getBeginLoc());
  if (!pathLiteral || !pathLiteral->isOrdinary())
    return emitError(pathLoc)
           << "unsupported: fopen path must be an ordinary string literal";
  llvm::StringRef path = pathLiteral->getString();
  for (char c : path)
    if (c < 0x20 || c > 0x7e)
      return emitError(pathLoc) << "unsupported: non-printable or "
                                   "non-ASCII byte in fopen path";
  llvm::StringRef helper =
      mode == "r" ? "__emitrust_fopen_r" : "__emitrust_fopen_w";
  requestFileHelper(helper);
  Value handle =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{fileHandleType()}, builder.getStringAttr(helper),
              builder.getArrayAttr({builder.getStringAttr(path)}),
              ValueRange{})
          .getResult(0);
  builder.create<emitrust::AssignOp>(loc, place, handle);
  return success();
}

FailureOr<Value> CImporter::emitFileHandleArg(const clang::Expr *expr,
                                              bool isMut) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *var =
      ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  // A FILE* parameter means the stream crossed into this function — the
  // v1 no-escape rule, pinned by the escape rejection case.
  if (var && llvm::isa<clang::ParmVarDecl>(var) &&
      isFilePtrType(var->getType()))
    return emitError(loc) << "unsupported: FILE* cannot cross a "
                             "user-defined function boundary";
  Value place = var ? fileLocals.lookup(var) : Value();
  if (!place)
    return emitError(loc) << "unsupported: a FILE* stream argument must be "
                             "a function-local FILE* variable";
  Type refType = isMut ? Type(emitrust::MutRefType::get(fileHandleType()))
                       : Type(emitrust::RefType::get(fileHandleType()));
  return builder.create<emitrust::AddrOfOp>(loc, refType, place, isMut)
      .getResult();
}

FailureOr<Value> CImporter::emitFileGetc(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: fgetc requires exactly one argument";
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(0), /*isMut=*/true);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_fgetc");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI32Type()},
          builder.getStringAttr("__emitrust_fgetc"), /*args=*/ArrayAttr(),
          ValueRange{*handle})
      .getResult(0);
}

FailureOr<Value> CImporter::emitFileReadWrite(const clang::CallExpr *call,
                                              bool isWrite) {
  llvm::StringRef name = isWrite ? "fwrite" : "fread";
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 4)
    return emitError(loc) << "unsupported: " << name
                          << " requires four arguments";
  // Byte-wise only: the element size must be the integer constant 1. The
  // check precedes the buffer lowering so a wide element over a non-char
  // buffer still reports the pinned byte-wise wording.
  clang::Expr::EvalResult size;
  if (!call->getArg(1)->EvaluateAsInt(size, astContext()) ||
      size.Val.getInt() != 1)
    return emitError(loc)
           << "unsupported: " << name
           << " element size other than 1 (FILE* I/O is byte-wise)";
  // The count value materializes before the borrows below, so no load
  // intervenes between a borrow and the helper call consuming it.
  FailureOr<Value> count = emitRValue(call->getArg(2));
  if (failed(count))
    return failure();
  if (!llvm::isa<IntegerType>((*count).getType()))
    return emitError(loc) << "unsupported: " << name << " count type";
  Value countI64 = castToIntType(loc, *count, builder.getIntegerType(64));
  FailureOr<PtrExprValue> region = emitCharRegionArg(call->getArg(0));
  if (failed(region))
    return failure();
  // fread fills the destination (mutable borrow); fwrite only reads its
  // source, so a string-literal region is fine there.
  FailureOr<Value> slice = emitCharRegionSlice(loc, *region,
                                               /*isMut=*/!isWrite);
  if (failed(slice))
    return failure();
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(3), /*isMut=*/true);
  if (failed(handle))
    return failure();
  llvm::StringRef helper =
      isWrite ? "__emitrust_fwrite" : "__emitrust_fread";
  requestFileHelper(helper);
  Value bytes = builder
                    .create<emitrust::CallOpaqueOp>(
                        loc, TypeRange{builder.getIntegerType(64)},
                        builder.getStringAttr(helper), /*args=*/ArrayAttr(),
                        ValueRange{*handle, *slice, countI64})
                    .getResult(0);
  // Convert the i64 byte count to the call's declared C result type
  // (size_t), matching emitStrlenCall's convention.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: " << name << " result type";
  return castToIntType(loc, bytes, intType);
}

FailureOr<Value> CImporter::emitFileGetsIndex(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc) << "unsupported: fgets requires three arguments";
  // The size value materializes before the borrows (same discipline as
  // fread/fwrite).
  FailureOr<Value> sizeValue = emitRValue(call->getArg(1));
  if (failed(sizeValue))
    return failure();
  if (!llvm::isa<IntegerType>((*sizeValue).getType()))
    return emitError(loc) << "unsupported: fgets size type";
  Value sizeI64 =
      castToIntType(loc, *sizeValue, builder.getIntegerType(64));
  FailureOr<PtrExprValue> region = emitCharRegionArg(call->getArg(0));
  if (failed(region))
    return failure();
  FailureOr<Value> slice = emitCharRegionSlice(loc, *region, /*isMut=*/true);
  if (failed(slice))
    return failure();
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(2), /*isMut=*/true);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_fgets");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getIntegerType(64)},
          builder.getStringAttr("__emitrust_fgets"), /*args=*/ArrayAttr(),
          ValueRange{*handle, *slice, sizeI64})
      .getResult(0);
}

const clang::CallExpr *
CImporter::asHostedFgetsCall(const clang::Expr *expr) const {
  const auto *call =
      llvm::dyn_cast<clang::CallExpr>(expr->IgnoreParenImpCasts());
  const clang::FunctionDecl *callee =
      call ? call->getDirectCallee() : nullptr;
  if (!callee || !callee->getDeclName().isIdentifier() ||
      callee->getName() != "fgets" || callee->getDefinition())
    return nullptr;
  return call;
}

LogicalResult CImporter::emitFileClose(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: fclose requires exactly one argument";
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(0), /*isMut=*/true);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_fclose");
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("__emitrust_fclose"),
      /*args=*/ArrayAttr(), ValueRange{*handle});
  return success();
}

FailureOr<Value> CImporter::emitFileTruth(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  // A truth-tested fgets result asks "did a line arrive": the helper's
  // -1 is C's NULL, so the test is an index comparison (the strchr
  // convention).
  if (const clang::CallExpr *gets = asHostedFgetsCall(e)) {
    FailureOr<Value> index = emitFileGetsIndex(gets);
    if (failed(index))
      return failure();
    Value nullIndex =
        createIntConstant(loc, builder.getIntegerType(64), -1);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, *index,
                               nullIndex)
        .getResult();
  }
  // A handle's truth is its NULL check, read through a shared borrow.
  FailureOr<Value> handle = emitFileHandleArg(e, /*isMut=*/false);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_file_ok");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI1Type()},
          builder.getStringAttr("__emitrust_file_ok"), /*args=*/ArrayAttr(),
          ValueRange{*handle})
      .getResult(0);
}

FailureOr<PtrExprValue>
CImporter::emitCharRegionArg(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  Location loc = translateLoc(e->getBeginLoc());
  // The void* parameters of memset/memcpy/memcmp wrap their arguments in
  // implicit pointer bitcasts, and const-qualified parameters (strcpy's
  // source, ...) in no-op qualification casts; the region underneath is
  // byte-typed either way, so both cast kinds are stripped.
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if ((cast->getCastKind() != clang::CK_BitCast &&
         cast->getCastKind() != clang::CK_NoOp) ||
        !isPointerType(cast->getType()))
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  // A decayed string literal argument — or `__func__`, whose
  // function-name literal is the same shape (C99-29) — creates (or
  // reuses) the literal's read-only backing; unlike a literal bound to a
  // pointer variable, this shape may appear with no pointer region
  // referring to the literal.
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
      if (const clang::StringLiteral *literal =
              underlyingStringLiteral(stripTrivia(cast->getSubExpr()))) {
        FailureOr<Value> backing = getOrCreateLiteralBacking(literal, loc);
        if (failed(backing))
          return failure();
        return PtrExprValue{
            nullptr, createIntConstant(loc, builder.getIntegerType(64), 0),
            *backing};
      }
  FailureOr<PtrExprValue> pointer = emitPointerRValue(e);
  if (failed(pointer))
    return failure();
  // A multi-base pointer has no single char region to borrow (CTS-P7
  // scope).
  if (pointer->baseIndex)
    return emitError(loc) << "unsupported: passing a pointer bound to "
                             "multiple objects to a string function";
  if (!pointer->cursor)
    return emitError(loc) << "unsupported: the address of a scalar object "
                             "is not a string region";
  return *pointer;
}

FailureOr<Value> CImporter::emitCharRegionSlice(Location loc,
                                                const PtrExprValue &pointer,
                                                bool isMut) {
  Value place = pointer.literalBacking;
  if (place && isMut)
    return emitError(loc) << "unsupported: a string literal region cannot "
                             "be a mutable string argument";
  if (!place) {
    auto it = symbols.find(pointer.base);
    if (it == symbols.end())
      return emitError(loc)
             << "unsupported: pointer target '" << pointer.base->getName()
             << "' is not an importable place";
    place = it->second;
  }
  auto lvalueType = llvm::dyn_cast<emitrust::LValueType>(place.getType());
  emitrust::ArrayType arrayType =
      lvalueType
          ? llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType())
          : emitrust::ArrayType();
  if (!arrayType || arrayType.getElementType() != builder.getIntegerType(8))
    return emitError(loc) << "unsupported: string function argument must "
                             "designate a char array";
  auto sliceType = emitrust::SliceType::get(arrayType.getElementType());
  Type refType = isMut ? Type(emitrust::MutRefType::get(sliceType))
                       : Type(emitrust::RefType::get(sliceType));
  return builder
      .create<emitrust::SliceOfOp>(loc, refType, place, pointer.cursor,
                                   isMut)
      .getResult();
}

void CImporter::requestStringHelper(llvm::StringRef name) {
  neededStringHelpers.insert(name);
}

LogicalResult CImporter::emitStringCopyCall(const clang::CallExpr *call,
                                            llvm::StringRef name,
                                            bool hasCount) {
  Location loc = translateLoc(call->getBeginLoc());
  unsigned expected = hasCount ? 3 : 2;
  if (call->getNumArgs() != expected)
    return emitError(loc) << "unsupported: " << name << " requires exactly "
                          << expected << " arguments";
  FailureOr<PtrExprValue> dst = emitCharRegionArg(call->getArg(0));
  if (failed(dst))
    return failure();
  FailureOr<PtrExprValue> src = emitCharRegionArg(call->getArg(1));
  if (failed(src))
    return failure();
  if (dst->base && dst->base == src->base)
    return emitError(loc)
           << "unsupported: " << name
           << " source and destination point into the same object '"
           << dst->base->getName() << "'";
  Value count;
  if (hasCount) {
    FailureOr<Value> n = emitRValue(call->getArg(2));
    if (failed(n))
      return failure();
    if (!llvm::isa<IntegerType>((*n).getType()))
      return emitError(loc) << "unsupported: " << name << " count type";
    count = castToIntType(loc, *n, builder.getIntegerType(64));
  }
  // The mutable destination borrow and the shared source borrow are
  // created back to back, immediately before the call: no load of either
  // base intervenes, so rustc accepts the pair.
  FailureOr<Value> dstSlice = emitCharRegionSlice(loc, *dst, /*isMut=*/true);
  if (failed(dstSlice))
    return failure();
  FailureOr<Value> srcSlice =
      emitCharRegionSlice(loc, *src, /*isMut=*/false);
  if (failed(srcSlice))
    return failure();
  SmallVector<Value> operands{*dstSlice, *srcSlice};
  if (count)
    operands.push_back(count);
  requestStringHelper(("__emitrust_" + name).str());
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(),
      builder.getStringAttr(("__emitrust_" + name).str()),
      /*args=*/ArrayAttr(), operands);
  return success();
}

LogicalResult CImporter::emitMemsetCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc)
           << "unsupported: memset requires exactly 3 arguments";
  FailureOr<PtrExprValue> dst = emitCharRegionArg(call->getArg(0));
  if (failed(dst))
    return failure();
  FailureOr<Value> byte = emitRValue(call->getArg(1));
  if (failed(byte))
    return failure();
  FailureOr<Value> n = emitRValue(call->getArg(2));
  if (failed(n))
    return failure();
  if (!llvm::isa<IntegerType>((*byte).getType()) ||
      !llvm::isa<IntegerType>((*n).getType()))
    return emitError(loc) << "unsupported: memset argument type";
  Value fill = castToIntType(loc, *byte, builder.getI32Type());
  Value count = castToIntType(loc, *n, builder.getIntegerType(64));
  FailureOr<Value> dstSlice = emitCharRegionSlice(loc, *dst, /*isMut=*/true);
  if (failed(dstSlice))
    return failure();
  requestStringHelper("__emitrust_memset");
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("__emitrust_memset"),
      /*args=*/ArrayAttr(), ValueRange{*dstSlice, fill, count});
  return success();
}

LogicalResult CImporter::emitMemcpyCall(const clang::CallExpr *call,
                                        llvm::StringRef name) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc)
           << "unsupported: " << name << " requires exactly 3 arguments";
  FailureOr<PtrExprValue> dst = emitCharRegionArg(call->getArg(0));
  if (failed(dst))
    return failure();
  FailureOr<PtrExprValue> src = emitCharRegionArg(call->getArg(1));
  if (failed(src))
    return failure();
  FailureOr<Value> n = emitRValue(call->getArg(2));
  if (failed(n))
    return failure();
  if (!llvm::isa<IntegerType>((*n).getType()))
    return emitError(loc) << "unsupported: " << name << " count type";
  Value count = castToIntType(loc, *n, builder.getIntegerType(64));
  if (dst->base && dst->base == src->base) {
    // Both arguments point into the same object: two slice borrows would
    // alias a mutable borrow, so the whole array is borrowed mutably once
    // and the helper receives both element cursors (`copy_within`; its
    // memmove semantics refine C's undefined overlapping memcpy).
    PtrExprValue whole{dst->base,
                       createIntConstant(loc, builder.getIntegerType(64), 0),
                       Value()};
    FailureOr<Value> slice = emitCharRegionSlice(loc, whole, /*isMut=*/true);
    if (failed(slice))
      return failure();
    requestStringHelper("__emitrust_memcpy_within");
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(),
        builder.getStringAttr("__emitrust_memcpy_within"),
        /*args=*/ArrayAttr(),
        ValueRange{*slice, dst->cursor, src->cursor, count});
    return success();
  }
  FailureOr<Value> dstSlice = emitCharRegionSlice(loc, *dst, /*isMut=*/true);
  if (failed(dstSlice))
    return failure();
  FailureOr<Value> srcSlice =
      emitCharRegionSlice(loc, *src, /*isMut=*/false);
  if (failed(srcSlice))
    return failure();
  requestStringHelper("__emitrust_memcpy");
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("__emitrust_memcpy"),
      /*args=*/ArrayAttr(), ValueRange{*dstSlice, *srcSlice, count});
  return success();
}

FailureOr<Value>
CImporter::emitStringCompareCall(const clang::CallExpr *call,
                                 llvm::StringRef name, bool hasCount) {
  Location loc = translateLoc(call->getBeginLoc());
  unsigned expected = hasCount ? 3 : 2;
  if (call->getNumArgs() != expected)
    return emitError(loc) << "unsupported: " << name << " requires exactly "
                          << expected << " arguments";
  FailureOr<PtrExprValue> lhs = emitCharRegionArg(call->getArg(0));
  if (failed(lhs))
    return failure();
  FailureOr<PtrExprValue> rhs = emitCharRegionArg(call->getArg(1));
  if (failed(rhs))
    return failure();
  Value count;
  if (hasCount) {
    FailureOr<Value> n = emitRValue(call->getArg(2));
    if (failed(n))
      return failure();
    if (!llvm::isa<IntegerType>((*n).getType()))
      return emitError(loc) << "unsupported: " << name << " count type";
    count = castToIntType(loc, *n, builder.getIntegerType(64));
  }
  // Both borrows are shared, so even two arguments into the same object
  // coexist.
  FailureOr<Value> lhsSlice =
      emitCharRegionSlice(loc, *lhs, /*isMut=*/false);
  if (failed(lhsSlice))
    return failure();
  FailureOr<Value> rhsSlice =
      emitCharRegionSlice(loc, *rhs, /*isMut=*/false);
  if (failed(rhsSlice))
    return failure();
  SmallVector<Value> operands{*lhsSlice, *rhsSlice};
  if (count)
    operands.push_back(count);
  requestStringHelper(("__emitrust_" + name).str());
  Value result = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{builder.getI32Type()},
                         builder.getStringAttr(("__emitrust_" + name).str()),
                         /*args=*/ArrayAttr(), operands)
                     .getResult(0);
  // The declared result type is C's int (i32) for the standard
  // prototypes; convert defensively for K&R-style declarations.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: " << name << " result type";
  return castToIntType(loc, result, intType);
}

const clang::CallExpr *
CImporter::asHostedStrchrCall(const clang::Expr *expr, bool &reverse) const {
  const auto *call =
      llvm::dyn_cast<clang::CallExpr>(expr->IgnoreParenImpCasts());
  if (!call)
    return nullptr;
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee || !callee->getDeclName().isIdentifier() ||
      callee->getDefinition())
    return nullptr;
  if (callee->getName() == "strchr") {
    reverse = false;
    return call;
  }
  if (callee->getName() == "strrchr") {
    reverse = true;
    return call;
  }
  return nullptr;
}

FailureOr<Value> CImporter::emitStrchrIndex(const clang::CallExpr *call,
                                            bool reverse,
                                            PtrExprValue &region) {
  Location loc = translateLoc(call->getBeginLoc());
  llvm::StringRef name = reverse ? "strrchr" : "strchr";
  if (call->getNumArgs() != 2)
    return emitError(loc) << "unsupported: " << name
                          << " requires exactly 2 arguments";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  region = *pointer;
  FailureOr<Value> needle = emitRValue(call->getArg(1));
  if (failed(needle))
    return failure();
  if (!llvm::isa<IntegerType>((*needle).getType()))
    return emitError(loc) << "unsupported: " << name << " character type";
  Value byte = castToIntType(loc, *needle, builder.getI32Type());
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, region, /*isMut=*/false);
  if (failed(slice))
    return failure();
  llvm::StringRef helper =
      reverse ? "__emitrust_strrchr" : "__emitrust_strchr";
  requestStringHelper(helper);
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getIntegerType(64)},
          builder.getStringAttr(helper),
          /*args=*/ArrayAttr(), ValueRange{*slice, byte})
      .getResult(0);
}

LogicalResult CImporter::emitPutchar(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: putchar requires exactly one argument";
  FailureOr<Value> value = emitRValue(call->getArg(0));
  if (failed(value))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>((*value).getType());
  if (!intType || intType.getWidth() == 1)
    return emitError(loc) << "unsupported: putchar argument must be an "
                             "integer";
  // C's putchar writes the argument converted to unsigned char; the
  // `__emitrust_fmt_c` helper performs that conversion (ASCII-only, see
  // design.md C99-48).
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("print!"),
      builder.getArrayAttr(
          {builder.getStringAttr("{}"), builder.getIndexAttr(0)}),
      ValueRange{wrapCharFormat(loc, *value)});
  return success();
}

std::optional<llvm::StringRef>
CImporter::hostedMathCallee(llvm::StringRef name) {
  // fabs/sqrt/floor/ceil are IEEE-754-exact (fabs/floor/ceil are exact
  // operations, sqrt is correctly rounded), so every conforming
  // implementation — glibc, Rust's f64 methods, LLVM's constant folder —
  // agrees bit for bit. sin carries no such mandate; it is accepted on
  // the weaker "both sides resolve to the platform libm" argument,
  // pinned differentially (design.md C99-48). exp/log/pow stay rejected
  // (see emitCall) rather than widening that exception.
  return llvm::StringSwitch<std::optional<llvm::StringRef>>(name)
      .Case("sin", "f64::sin")
      .Case("fabs", "f64::abs")
      .Case("sqrt", "f64::sqrt")
      .Case("floor", "f64::floor")
      .Case("ceil", "f64::ceil")
      .Default(std::nullopt);
}

FailureOr<Value> CImporter::emitHostedMathCall(const clang::CallExpr *call,
                                               llvm::StringRef rustCallee) {
  Location loc = translateLoc(call->getBeginLoc());
  FailureOr<Value> argument = emitRValue(call->getArg(0));
  if (failed(argument))
    return failure();
  // The callee's prototype is `double f(double)` (checked at the call
  // site), so clang has already converted the argument to double; anything
  // else indicates an importer bug rather than an unsupported program.
  if (!llvm::isa<Float64Type>((*argument).getType()))
    return emitError(loc) << "unsupported: " << rustCallee
                          << " argument is not a double";
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getF64Type()},
          builder.getStringAttr(rustCallee),
          /*args=*/ArrayAttr(), ValueRange{*argument})
      .getResult(0);
}

FailureOr<Value> CImporter::emitAbsCall(const clang::CallExpr *call,
                                        bool isLong) {
  Location loc = translateLoc(call->getBeginLoc());
  llvm::StringRef name = isLong ? "labs" : "abs";
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: " << name << " requires exactly one argument";
  FailureOr<Value> value = emitRValue(call->getArg(0));
  if (failed(value))
    return failure();
  if (!llvm::isa<IntegerType>((*value).getType()))
    return emitError(loc)
           << "unsupported: " << name << " argument must be an integer";
  // The prototype has already converted the argument to int/long;
  // normalize the width anyway (a K&R-style declaration may differ).
  IntegerType intType = builder.getIntegerType(isLong ? 64 : 32);
  Value argument = castToIntType(loc, *value, intType);
  // wrapping_abs: abs(INT_MIN)/labs(LONG_MIN) is C UB (7.20.6.1p2),
  // refined to the deterministic two's-complement wrap the differential
  // oracle's platform also produces; Rust's plain `abs` would panic only
  // in debug builds and is rejected as non-deterministic across profiles.
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{intType},
          builder.getStringAttr(isLong ? "i64::wrapping_abs"
                                       : "i32::wrapping_abs"),
          /*args=*/ArrayAttr(), ValueRange{argument})
      .getResult(0);
}

FailureOr<Value> CImporter::emitAtoiCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: atoi requires exactly one argument";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  requestStringHelper("__emitrust_atoi");
  Value parsed =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{builder.getI32Type()},
              builder.getStringAttr("__emitrust_atoi"),
              /*args=*/ArrayAttr(), ValueRange{*slice})
          .getResult(0);
  // atoi returns int; convert to the call's declared result type in case
  // a K&R-style declaration says otherwise (matching emitStrlenCall).
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: atoi result type";
  return castToIntType(loc, parsed, intType);
}

LogicalResult CImporter::emitExitCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: exit requires exactly one argument";
  FailureOr<Value> status = emitRValue(call->getArg(0));
  if (failed(status))
    return failure();
  if (!llvm::isa<IntegerType>((*status).getType()))
    return emitError(loc)
           << "unsupported: exit status must be an integer";
  Value code = castToIntType(loc, *status, builder.getI32Type());
  // `std::process::exit` terminates with the given status like C's exit;
  // both report the low byte to the OS on this target. Its `!` result
  // needs no representation — the call is statement-position only.
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("std::process::exit"),
      /*args=*/ArrayAttr(), ValueRange{code});
  return success();
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

FailureOr<Value> CImporter::emitRValue(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  // Constant contexts (case values, enumerator initializers) are wrapped in
  // ConstantExpr; translate the wrapped expression. Full expressions
  // containing block-scope compound literals are wrapped in
  // ExprWithCleanups (C99-13); the "cleanup" is the end of the temp's
  // lifetime, which needs no code.
  if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(e))
    return emitRValue(constant->getSubExpr());
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    return emitRValue(cleanups->getSubExpr());
  // An enumerator used as a plain expression has type `int` in C: a named
  // enum's constant is rendered as its Rust variant cast to i32, while an
  // anonymous enum's constant is a plain i32 value.
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (const auto *enumerator =
            llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl())) {
      FailureOr<Value> constant = emitEnumConstant(enumerator, loc);
      if (failed(constant))
        return failure();
      if (llvm::isa<emitrust::EnumType>((*constant).getType()))
        return castEnumToI32(loc, *constant);
      return constant;
    }

  if (const auto *literal = llvm::dyn_cast<clang::IntegerLiteral>(e)) {
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    IntegerAttr attr = IntegerAttr::get(*type, literal->getValue());
    // Unsigned literals (42u and friends) cannot be arith constants (arith
    // requires signless types); they become emitrust constants instead.
    if (isUnsignedInt(*type))
      return builder.create<emitrust::ConstantOp>(loc, *type, attr)
          .getResult();
    return builder.create<arith::ConstantOp>(loc, attr).getResult();
  }
  if (const auto *literal = llvm::dyn_cast<clang::FloatingLiteral>(e)) {
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    // An L-suffixed literal's APFloat carries x87 extended semantics;
    // floatAttrFor narrows it to the f64 the long-double-as-f64 policy
    // substitutes (CTS 00204).
    return builder
        .create<arith::ConstantOp>(
            loc, floatAttrFor(llvm::cast<FloatType>(*type),
                              literal->getValue()))
        .getResult();
  }
  if (const auto *literal = llvm::dyn_cast<clang::CharacterLiteral>(e)) {
    // C99-4 plain-char policy: ordinary character constants have C type
    // int and import as the value clang evaluated; because plain char is
    // signed on the x86-64 Linux target the differential oracle uses, a
    // high-byte constant ('\xff') is stored sign-extended (0xFFFFFFFF,
    // i.e. -1). A wide constant (L'') is also just an int-typed rvalue —
    // wchar_t is int on this target — and imports as its code-point
    // value, matching the wide-string policy of carrying code units
    // verbatim. Unicode constants (u8''/u''/U'') carry charN_t types
    // outside the model and stay rejected.
    clang::CharacterLiteralKind kind = literal->getKind();
    if (kind != clang::CharacterLiteralKind::Ascii &&
        kind != clang::CharacterLiteralKind::Wide)
      return emitError(loc)
             << "unsupported: Unicode character constant (u8'', u'', U'')";
    uint32_t raw = literal->getValue();
    if (kind == clang::CharacterLiteralKind::Ascii) {
      // An ordinary constant must be a single byte: a value in [0,255]
      // or a sign-extended high byte. Anything else is a
      // multi-character constant ('ab'), whose value is
      // implementation-defined and rejected rather than pinned.
      bool singleByte = raw <= 0xFF || (raw & 0xFFFFFF00u) == 0xFFFFFF00u;
      if (!singleByte)
        return emitError(loc) << "unsupported: multi-character constant";
    }
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    // Materialize the (possibly negative) int-typed value, sign-extended
    // from the stored 32-bit pattern.
    return createIntConstant(loc, *type,
                             static_cast<int64_t>(static_cast<int32_t>(raw)));
  }
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    return emitCast(cast);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e))
    return emitBinaryRValue(binary);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    return emitUnaryRValue(unary);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
    FailureOr<Value> result = emitCall(call);
    if (failed(result))
      return failure();
    if (!*result)
      return emitError(loc) << "unsupported: value use of a void call";
    return result;
  }
  if (const auto *conditional = llvm::dyn_cast<clang::ConditionalOperator>(e))
    return emitConditionalOperator(conditional);
  if (const auto *stmtExpr = llvm::dyn_cast<clang::StmtExpr>(e))
    return emitStmtExpr(stmtExpr);
  // `va_arg(ap, T)` inside a monomorphization clone (CTS 00204): a
  // dispatch over the consumption cursor selecting among the clone's
  // extras of static type T.
  if (const auto *vaArg = llvm::dyn_cast<clang::VAArgExpr>(e))
    return emitVaArg(vaArg);
  if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(e))
    return emitSizeofAlignof(trait);
  if (llvm::isa<clang::StringLiteral>(e))
    return emitError(loc)
           << "unsupported: string literal outside a printf format";
  // `__func__` (and __FUNCTION__/__PRETTY_FUNCTION__) is modeled only in
  // the string-literal positions — printf/puts '%s' arguments,
  // char-pointer bindings, and string-helper arguments (C99-29); any
  // other value use keeps a located rejection naming the identifier.
  if (const auto *predefined = llvm::dyn_cast<clang::PredefinedExpr>(e))
    return emitError(loc) << "unsupported use of '"
                          << predefined->getIdentKindName()
                          << "' outside a string literal position";
  // A struct compound literal in value position (assignment right-hand
  // side, by-value argument, return value — contexts where clang does not
  // wrap the aggregate in an lvalue-to-rvalue cast) materializes its
  // anonymous temp and loads it whole (C99-13).
  if (const auto *literal = llvm::dyn_cast<clang::CompoundLiteralExpr>(e)) {
    FailureOr<Value> place = emitCompoundLiteralPlace(literal);
    if (failed(place))
      return failure();
    return loadPlace(loc, *place);
  }
  return emitError(loc) << "unsupported expression: " << e->getStmtClassName();
}

FailureOr<Value> CImporter::emitCast(const clang::CastExpr *cast) {
  Location loc = translateLoc(cast->getBeginLoc());
  const clang::Expr *sub = cast->getSubExpr();

  switch (cast->getCastKind()) {
  case clang::CK_NoOp:
    return emitRValue(sub);
  case clang::CK_FunctionToPointerDecay:
    // A function used as a value decays to a `Some(name)` fn_ptr constant.
    return emitFunctionPointerConstant(sub, cast->getType(), loc);
  case clang::CK_NullToPointer: {
    // The null constant of a function pointer is `None`. A data-pointer
    // null constant is modeled at its consuming sites (assignment,
    // equality comparison, truth test — the Option-of-cursor model,
    // CTS-P8); one reaching this generic value path has no modeled
    // consumer and keeps its located rejection.
    if (isFunctionPointer(cast->getType())) {
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      return createFnPtrNone(loc, *mapped);
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
  case clang::CK_BitCast: {
    // A conversion between function pointer types, e.g. binding a
    // prototyped function to a prototype-less `int (*)()` pointer. A
    // direct function reference re-resolves against the destination type;
    // any other value is legal only when both sides map to the identical
    // fn_ptr signature (Rust has no function pointer reinterpretation).
    if (isFunctionPointer(cast->getType()) &&
        isFunctionPointer(sub->getType())) {
      const clang::Expr *stripped = stripTrivia(sub);
      if (const auto *decay =
              llvm::dyn_cast<clang::ImplicitCastExpr>(stripped))
        if (decay->getCastKind() == clang::CK_FunctionToPointerDecay)
          return emitFunctionPointerConstant(decay->getSubExpr(),
                                             cast->getType(), loc);
      if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripped))
        if (unary->getOpcode() == clang::UO_AddrOf)
          return emitFunctionPointerConstant(unary->getSubExpr(),
                                             cast->getType(), loc);
      FailureOr<Value> value = emitRValue(sub);
      if (failed(value))
        return failure();
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      if ((*value).getType() != *mapped)
        return emitError(loc) << "unsupported: function pointer conversion "
                                 "changes the signature";
      return value;
    }
    // CTS-BR (00216): a read of an admitted `void *` fn-ptr member under
    // a cast to its one signature loads the retyped member place — no
    // cast op reaches the IR.
    if (isFunctionPointer(cast->getType())) {
      const auto *memberExpr = llvm::dyn_cast<clang::MemberExpr>(
          sub->IgnoreParenImpCasts());
      const auto *field =
          memberExpr
              ? llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl())
              : nullptr;
      if (field && fnPtrMemberTypes.count(field)) {
        FailureOr<Value> place = emitLValue(memberExpr, nullptr);
        if (failed(place))
          return failure();
        Value value = loadPlace(loc, *place);
        FailureOr<Type> mapped = mapType(cast->getType(), loc);
        if (failed(mapped))
          return failure();
        if (value.getType() != *mapped)
          return emitError(loc) << "unsupported: function pointer "
                                   "conversion changes the signature";
        return value;
      }
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
  case clang::CK_LValueToRValue: {
    if (const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(sub->IgnoreParens())) {
      // A pointer parameter read as a value yields its reference SSA value.
      auto it = symbols.find(ref->getDecl());
      if (it != symbols.end() &&
          llvm::isa<emitrust::MutRefType, emitrust::RefType>(
              it->second.getType()))
        return it->second;
      // A whole-value read of a global is a direct emitrust.global_load.
      if (const GlobalInfo *global = lookupGlobal(ref->getDecl()))
        return builder
            .create<emitrust::GlobalLoadOp>(loc, global->type,
                                            globalSymbol(global->symbol))
            .getResult();
    }
    // An element read through a cell-slice parameter (CTS-P10) is an
    // `emitrust.cell_get` on the reference itself — no place is staged.
    if (std::optional<CellSliceAccess> access = matchCellSliceAccess(sub))
      return emitCellSliceGet(*access, loc);
    // A wider-than-element view over a byte region (CTS-P11) widens to a
    // `from_ne_bytes` load over sizeof(T) consecutive bytes.
    if (ByteViewDeref wide = classifyByteViewDeref(sub); wide.wideByte) {
      FailureOr<WideByteAccess> access =
          resolveWideByteAccess(wide, loc, /*writeback=*/nullptr);
      if (failed(access))
        return failure();
      return emitWideByteLoad(*access, loc);
    }
    // A bit-field member read is the synthesized mask-and-shift accessor
    // over its backing field (C99-45); no lvalue of the member exists.
    if (const auto *memberExpr =
            llvm::dyn_cast<clang::MemberExpr>(stripTrivia(sub)))
      if (const auto *field =
              llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
        if (field->isBitField())
          return emitBitFieldRead(memberExpr, loc);
    FailureOr<Value> place = emitLValue(sub);
    if (failed(place))
      return failure();
    Value loaded = loadPlace(loc, *place);
    // A same-width integer view through a reinterpret-back site
    // (`u = *(unsigned int *)p` over an int base, CTS-P9) loads the base
    // element and bitcasts it to the viewed type: Rust's same-width
    // cross-sign `as` reinterprets the bit pattern, exactly C's
    // effective-type-compatible read.
    if (classifyByteViewDeref(sub).reinterpreted) {
      FailureOr<Type> viewed = mapType(cast->getType(), loc);
      if (failed(viewed))
        return failure();
      if (*viewed != loaded.getType())
        loaded = builder.create<emitrust::CastOp>(loc, *viewed, loaded)
                     .getResult();
    }
    // A union pun arm reads the slot and reinterprets the loaded value
    // as its own (differently-signed) type.
    return reinterpretUnionArmRead(sub, loaded, loc);
  }
  case clang::CK_PointerToIntegral: {
    // `(int) q` of a STATICALLY NULL pointer (a base-less nullable
    // region, CTS-P9) is the integer 0 — no address value ever exists to
    // materialize. Every other pointer-to-int cast keeps its located
    // rejection: a pointer with a real base object would need a genuine
    // address value.
    if (isDataPointer(sub->getType()) && isStaticallyNullPointerExpr(sub)) {
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      if (llvm::isa<IntegerType>(*mapped))
        return createScalarIntConstant(loc, *mapped, 0);
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
  case clang::CK_IntegralCast: {
    // A `sizeof` over a BYTE-REGION aggregate converted to a signed
    // 64-bit context (the `long size` walker parameter) folds directly
    // to the signless constant — the region size is the model's own
    // static fact, and `sizeof(struct W)` stays the FAM-free size
    // (CTS-BR, 00216). Other sizeof shapes keep the historical
    // ui64-constant-plus-cast pair.
    if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(
            sub->IgnoreParenImpCasts());
        trait && trait->getKind() == clang::UETT_SizeOf &&
        !trait->getTypeOfArgument()->isVariablyModifiedType() &&
        isByteRegionAggregate(trait->getTypeOfArgument())) {
      clang::Expr::EvalResult eval;
      Type destType;
      if (FailureOr<Type> mapped = mapType(cast->getType(), loc);
          succeeded(mapped))
        destType = *mapped;
      auto destInt = llvm::dyn_cast_or_null<IntegerType>(destType);
      if (destInt && destInt.isSignless() && destInt.getWidth() == 64 &&
          trait->EvaluateAsInt(eval, astContext()) && !eval.HasSideEffects) {
        // Materialize in the entry block: the region size is a
        // function-wide static fact, and the hoisted constant dominates
        // every use.
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(entryBlock);
        return createIntConstant(loc, destInt,
                                 eval.Val.getInt().getSExtValue());
      }
    }
    // Integer-to-enum: a reference to an enumerator of the destination
    // enum keeps the direct constant fast path (in C the enumerator itself
    // has type `int`, so even `enum Color c = Red;` arrives as this cast);
    // any other integer value converts through an `emitrust.cast` to the
    // enum type, rendered as the open enum's value-preserving tuple-struct
    // constructor — C preserves values outside the declared enumerators
    // (C99 6.7.2.2, the object holds any value of the underlying type),
    // and so does the emitted Rust.
    if (const clang::EnumDecl *target = namedEnumDeclOf(cast->getType())) {
      if (const auto *ref =
              llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(sub)))
        if (const auto *enumerator =
                llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl()))
          if (llvm::cast<clang::EnumDecl>(enumerator->getDeclContext())
                  ->getDefinition() == target)
            return emitEnumConstant(enumerator, loc);
      FailureOr<Value> value = emitRValue(sub);
      if (failed(value))
        return failure();
      auto sourceType = llvm::dyn_cast<IntegerType>((*value).getType());
      if (!sourceType || sourceType.getWidth() == 1)
        return emitError(loc) << "unsupported: integer to enum conversion";
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    }
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    // Enum-to-integer (Sema's integral promotion, an arithmetic use, or an
    // explicit cast): an `emitrust.cast` to the mapped destination type
    // (Rust's `as` casts a fieldless `#[repr(i32)]` enum to any integer
    // type via its discriminant, matching C's conversion). Mapping the
    // destination keeps unsigned promotions unsigned, so an enum with an
    // unsigned underlying type meets its comparison or arithmetic partner
    // at the same type and unsigned C semantics are preserved.
    if (llvm::isa<emitrust::EnumType>((*value).getType())) {
      if (!cast->getType().getCanonicalType()->isIntegerType())
        return emitError(loc) << "unsupported integral cast";
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      if (!llvm::isa<IntegerType>(*mapped))
        return emitError(loc) << "unsupported integral cast";
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    }
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    auto sourceType = llvm::dyn_cast<IntegerType>((*value).getType());
    auto targetType = llvm::dyn_cast<IntegerType>(*mapped);
    if (!sourceType || !targetType)
      return emitError(loc) << "unsupported integral cast";
    // A conversion touching an unsigned type on either side is an
    // `emitrust.cast`: Rust's `as` between integer types matches C's
    // conversion semantics exactly (truncation keeps the low bits,
    // widening zero-extends from unsigned and sign-extends from signed,
    // and same-width cross-sign casts reinterpret the bit pattern).
    if (!sourceType.isSignless() || !targetType.isSignless()) {
      if (sourceType == targetType)
        return *value;
      return builder.create<emitrust::CastOp>(loc, targetType, *value)
          .getResult();
    }
    if (sourceType.getWidth() == targetType.getWidth())
      return *value;
    if (sourceType.getWidth() < targetType.getWidth()) {
      if (sourceType.getWidth() == 1)
        return builder.create<arith::ExtUIOp>(loc, targetType, *value)
            .getResult();
      return builder.create<arith::ExtSIOp>(loc, targetType, *value)
          .getResult();
    }
    return builder.create<arith::TruncIOp>(loc, targetType, *value)
        .getResult();
  }
  case clang::CK_IntegralToFloating: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    // Unsigned to float is an `emitrust.cast`: Rust's `u* as f*` performs
    // the same round-to-nearest conversion as C.
    if (isUnsignedInt((*value).getType()))
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    return builder.create<arith::SIToFPOp>(loc, *mapped, *value).getResult();
  }
  case clang::CK_FloatingToIntegral: {
    // A floating value converted directly to an enum destination has no
    // integer intermediary in the AST; the emitrust.cast enum path
    // requires an integer source, so the shape stays rejected.
    if (namedEnumDeclOf(cast->getType()))
      return emitError(loc) << "unsupported: floating to enum conversion";
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    // Float to unsigned is an `emitrust.cast`. Where the value is in the
    // destination's range the Rust `as` result is identical to C's; out of
    // range C is undefined behavior while Rust `as` saturates, which is an
    // acceptable (defined) refinement of the undefined C behavior.
    if (isUnsignedInt(*mapped))
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    return builder.create<arith::FPToSIOp>(loc, *mapped, *value).getResult();
  }
  case clang::CK_FloatingCast: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    auto sourceType = llvm::dyn_cast<FloatType>((*value).getType());
    auto targetType = llvm::dyn_cast<FloatType>(*mapped);
    if (!sourceType || !targetType)
      return emitError(loc) << "unsupported floating-point cast";
    if (sourceType.getWidth() < targetType.getWidth())
      return builder.create<arith::ExtFOp>(loc, targetType, *value)
          .getResult();
    if (sourceType.getWidth() > targetType.getWidth())
      return builder.create<arith::TruncFOp>(loc, targetType, *value)
          .getResult();
    return *value;
  }
  case clang::CK_IntegralToBoolean: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    // An enum tested for truth compares its i32 discriminant against zero.
    Value truth = *value;
    if (llvm::isa<emitrust::EnumType>(truth.getType()))
      truth = castEnumToI32(loc, truth);
    Value zero = createScalarIntConstant(loc, truth.getType(), 0);
    // Unsigned truth tests use `emitrust.cmp` (arith.cmpi requires
    // signless operands; Rust's `!=` is type-directed and hence unsigned).
    if (isUnsignedInt(truth.getType()))
      return builder
          .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                   emitrust::CmpPredicate::ne, truth, zero)
          .getResult();
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, truth, zero)
        .getResult();
  }
  case clang::CK_FloatingToBoolean: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    auto floatType = llvm::cast<FloatType>((*value).getType());
    Value zero =
        builder.create<arith::ConstantOp>(loc, FloatAttr::get(floatType, 0.0))
            .getResult();
    return builder
        .create<arith::CmpFOp>(loc, arith::CmpFPredicate::UNE, *value, zero)
        .getResult();
  }
  default:
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
}

FailureOr<Value> CImporter::emitBinaryRValue(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  clang::BinaryOperatorKind opcode = op->getOpcode();

  // Value-position assignments perform the store and yield the stored
  // value by re-loading the assigned place (C's value of an assignment is
  // the post-assignment value; the load is promoted away by --mem2reg for
  // memref cells).
  if (const auto *compound = llvm::dyn_cast<clang::CompoundAssignOperator>(op)) {
    FailureOr<Value> place = emitCompoundAssignToPlace(compound);
    if (failed(place))
      return failure();
    // A union pun arm's re-loaded slot value reinterprets to the arm's
    // own type, the C type of the assignment expression.
    return reinterpretUnionArmRead(op->getLHS(), loadPlace(loc, *place), loc);
  }
  if (opcode == clang::BO_Assign) {
    FailureOr<Value> place = emitAssignToPlace(op);
    if (failed(place))
      return failure();
    return reinterpretUnionArmRead(op->getLHS(), loadPlace(loc, *place), loc);
  }
  // The comma operator evaluates the left operand for its side effects
  // only and yields the right operand's value.
  if (opcode == clang::BO_Comma) {
    if (failed(emitExprStmt(op->getLHS())))
      return failure();
    return emitRValue(op->getRHS());
  }
  // `p - q` on two pointers into the same object is the plain i64 cursor
  // difference (C's ptrdiff_t is `long`, i.e. i64, on the supported
  // targets); the operands never materialize as pointer values.
  if (opcode == clang::BO_Sub && isPointerType(op->getLHS()->getType()) &&
      isPointerType(op->getRHS()->getType()))
    return emitPointerDifference(op);
  // Any other pointer-valued binary result (`p + 1` in value position) is
  // consumed by `emitPointerRValue` from its dereference or assignment
  // context; a bare one has no scalar representation.
  if (isPointerType(op->getType()))
    return emitError(loc) << "unsupported pointer expression in this context";
  if (op->isComparisonOp()) {
    FailureOr<Value> flag = emitComparison(op);
    if (failed(flag))
      return failure();
    return extendBool(loc, *flag, op->getType());
  }
  if (opcode == clang::BO_LAnd || opcode == clang::BO_LOr) {
    FailureOr<Value> flag = emitCondition(op);
    if (failed(flag))
      return failure();
    return extendBool(loc, *flag, op->getType());
  }

  FailureOr<Value> lhs = emitRValue(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<Value> rhs = emitRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  Value rhsValue = *rhs;
  // A shift amount's C type is promoted independently of the shifted
  // operand's, so the operand types may legitimately differ (`long << int`);
  // normalize the amount to the shifted operand's width.
  auto lhsInt = llvm::dyn_cast<IntegerType>((*lhs).getType());
  auto rhsInt = llvm::dyn_cast<IntegerType>(rhsValue.getType());
  if ((opcode == clang::BO_Shl || opcode == clang::BO_Shr) && lhsInt && rhsInt)
    rhsValue = castToIntType(loc, rhsValue, lhsInt);
  if ((*lhs).getType() != rhsValue.getType())
    return emitError(loc) << "unsupported: binary operand type mismatch";
  return buildBinaryArith(loc, opcode, *lhs, rhsValue);
}

FailureOr<Value> CImporter::buildBinaryArith(Location loc,
                                             clang::BinaryOperatorKind opcode,
                                             Value lhs, Value rhs) {
  if (llvm::isa<FloatType>(lhs.getType())) {
    switch (opcode) {
    case clang::BO_Add:
      return builder.create<arith::AddFOp>(loc, lhs, rhs).getResult();
    case clang::BO_Sub:
      return builder.create<arith::SubFOp>(loc, lhs, rhs).getResult();
    case clang::BO_Mul:
      return builder.create<arith::MulFOp>(loc, lhs, rhs).getResult();
    case clang::BO_Div:
      return builder.create<arith::DivFOp>(loc, lhs, rhs).getResult();
    default:
      return emitError(loc)
             << "unsupported floating-point binary operator '"
             << clang::BinaryOperator::getOpcodeStr(opcode) << "'";
    }
  }
  if (isUnsignedInt(lhs.getType())) {
    // arith ops require signless operands; unsigned arithmetic uses the
    // emitrust binary ops, whose Rust infix operators are type-directed
    // and therefore perform unsigned division, remainder, and comparison.
    switch (opcode) {
    case clang::BO_Add:
      return builder.create<emitrust::AddOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Sub:
      return builder.create<emitrust::SubOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Mul:
      return builder.create<emitrust::MulOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Div:
      return builder.create<emitrust::DivOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Rem:
      return builder.create<emitrust::RemOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_And:
      return builder.create<emitrust::AndOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Or:
      return builder.create<emitrust::OrOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Xor:
      return builder.create<emitrust::XorOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Shl:
      return builder.create<emitrust::ShlOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Shr:
      // Rust's `>>` on uN is a logical shift, exactly C's unsigned `>>`.
      return builder.create<emitrust::ShrOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    default:
      return emitError(loc) << "unsupported binary operator '"
                            << clang::BinaryOperator::getOpcodeStr(opcode)
                            << "'";
    }
  }
  if (llvm::isa<IntegerType>(lhs.getType())) {
    switch (opcode) {
    case clang::BO_Add:
      return builder.create<arith::AddIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Sub:
      return builder.create<arith::SubIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Mul:
      return builder.create<arith::MulIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Div:
      return builder.create<arith::DivSIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Rem:
      return builder.create<arith::RemSIOp>(loc, lhs, rhs).getResult();
    case clang::BO_And:
      return builder.create<arith::AndIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Or:
      return builder.create<arith::OrIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Xor:
      return builder.create<arith::XOrIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Shl:
      return builder.create<arith::ShLIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Shr:
      // C's `>>` on a signed operand is implementation-defined; every
      // relevant C ABI (and Rust's `>>` on iN, which this lowers to)
      // performs an arithmetic shift, hence shrsi.
      return builder.create<arith::ShRSIOp>(loc, lhs, rhs).getResult();
    default:
      return emitError(loc) << "unsupported binary operator '"
                            << clang::BinaryOperator::getOpcodeStr(opcode)
                            << "'";
    }
  }
  return emitError(loc) << "unsupported binary operand type";
}

Value CImporter::castToIntType(Location loc, Value value,
                               IntegerType target) {
  auto source = llvm::cast<IntegerType>(value.getType());
  if (source == target)
    return value;
  if (!source.isSignless() || !target.isSignless())
    return builder.create<emitrust::CastOp>(loc, target, value).getResult();
  if (source.getWidth() < target.getWidth())
    return builder.create<arith::ExtSIOp>(loc, target, value).getResult();
  return builder.create<arith::TruncIOp>(loc, target, value).getResult();
}

FailureOr<Value> CImporter::emitUnaryRValue(const clang::UnaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  switch (op->getOpcode()) {
  case clang::UO_Plus:
    return emitRValue(op->getSubExpr());
  case clang::UO_Minus: {
    FailureOr<Value> value = emitRValue(op->getSubExpr());
    if (failed(value))
      return failure();
    if (llvm::isa<FloatType>((*value).getType()))
      return builder.create<arith::NegFOp>(loc, *value).getResult();
    // C negates an unsigned value modulo 2^N (C99 6.2.5p9); lower it as
    // `0 - x` through the unsigned emitrust.sub, which translates to Rust's
    // `wrapping_sub` and so matches C's modular semantics on every width
    // instead of panicking on overflow in debug builds.
    if (isUnsignedInt((*value).getType())) {
      Value zero = createScalarIntConstant(loc, (*value).getType(), 0);
      return builder
          .create<emitrust::SubOp>(loc, (*value).getType(), zero, *value)
          .getResult();
    }
    if (llvm::isa<IntegerType>((*value).getType())) {
      Value zero = createIntConstant(loc, (*value).getType(), 0);
      return builder.create<arith::SubIOp>(loc, zero, *value).getResult();
    }
    return emitError(loc) << "unsupported operand of unary '-'";
  }
  case clang::UO_LNot: {
    FailureOr<Value> flag = emitCondition(op->getSubExpr());
    if (failed(flag))
      return failure();
    Value truth = createBoolConstant(loc, true);
    Value inverted =
        builder.create<arith::XOrIOp>(loc, *flag, truth).getResult();
    return extendBool(loc, inverted, op->getType());
  }
  case clang::UO_AddrOf: {
    // `&f` on a function yields the same `Some(f)` constant as the
    // implicit function-to-pointer decay.
    if (isFunctionPointer(op->getType()))
      return emitFunctionPointerConstant(op->getSubExpr(), op->getType(),
                                         loc);
    // A pointer into a global would dangle from the staged local copy the
    // access model uses, so it is rejected until globals get a pointer
    // model.
    if (rootsAtGlobal(op->getSubExpr()))
      return emitError(loc)
             << "unsupported: taking the address of a global variable";
    FailureOr<Value> place = emitLValue(op->getSubExpr());
    if (failed(place))
      return failure();
    auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*place).getType());
    if (!lvalueType)
      return emitError(loc)
             << "unsupported: cannot take the address of this expression";
    Type refType = emitrust::MutRefType::get(lvalueType.getValueType());
    return builder
        .create<emitrust::AddrOfOp>(loc, refType, *place, /*is_mut=*/true)
        .getResult();
  }
  case clang::UO_PreInc:
  case clang::UO_PostInc:
  case clang::UO_PreDec:
  case clang::UO_PostDec:
    return emitIncDecValue(op);
  case clang::UO_Not: {
    FailureOr<Value> value = emitRValue(op->getSubExpr());
    if (failed(value))
      return failure();
    auto intType = llvm::dyn_cast<IntegerType>((*value).getType());
    if (!intType)
      return emitError(loc) << "unsupported operand of bitwise '~'";
    // `~x` is x XOR all-ones (the -1 bit pattern of the operand's width);
    // unsigned operands use emitrust.xor since arith requires signless.
    if (intType.isUnsigned()) {
      Value allOnes =
          builder
              .create<emitrust::ConstantOp>(
                  loc, intType,
                  IntegerAttr::get(intType,
                                   llvm::APInt::getAllOnes(
                                       intType.getWidth())))
              .getResult();
      return builder
          .create<emitrust::XorOp>(loc, intType, *value, allOnes)
          .getResult();
    }
    Value allOnes = createIntConstant(loc, intType, -1);
    return builder.create<arith::XOrIOp>(loc, *value, allOnes).getResult();
  }
  case clang::UO_Deref:
    // A statement-position dereference of an uncast `void *` (a GNU
    // extension in C) never names an element type at all (CTS-P9); it is
    // rejected rather than silently discarded.
    if (op->getType().getCanonicalType()->isVoidType() &&
        isPointerType(op->getSubExpr()->getType()))
      return emitError(loc) << "unsupported: dereference of a 'void *' "
                               "pointer";
    return emitError(loc) << "unsupported dereference in this context";
  default:
    return emitError(loc) << "unsupported unary operator";
  }
}

FailureOr<Value> CImporter::emitComparison(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());

  // Function pointers are ordinary Option<fn> values; C only defines
  // equality on distinct function pointers, which maps to `emitrust.cmp`
  // on the fn_ptr type (`Option<fn>` derives PartialEq; the null constant
  // arrives as a `None` constant through CK_NullToPointer).
  if (isFunctionPointer(op->getLHS()->getType()) ||
      isFunctionPointer(op->getRHS()->getType())) {
    if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
      return emitError(loc)
             << "unsupported: ordered comparison of function pointers";
    FailureOr<Value> lhs = emitRValue(op->getLHS());
    if (failed(lhs))
      return failure();
    FailureOr<Value> rhs = emitRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    if ((*lhs).getType() != (*rhs).getType())
      return emitError(loc)
             << "unsupported: comparison operand type mismatch";
    emitrust::CmpPredicate predicate = op->getOpcode() == clang::BO_EQ
                                           ? emitrust::CmpPredicate::eq
                                           : emitrust::CmpPredicate::ne;
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(), predicate, *lhs,
                                 *rhs)
        .getResult();
  }

  // Pointer comparisons decompose both sides into (base, cursor) pairs;
  // only pointers into the same object have a defined C ordering, and the
  // i64 cursors compare signed. A degenerate side (the address of a
  // scalar) is cursor 0 of its object. Comparisons against a null pointer
  // constant are the Option-of-cursor discrimination (CTS-P8): a string
  // literal or a statically non-null pointer folds (every held address
  // designates a live object, whose address is non-null), and a pointer
  // of a nullable region tests its i1 discriminant.
  if (isPointerType(op->getLHS()->getType()) ||
      isPointerType(op->getRHS()->getType())) {
    // An fgets result compared against a null pointer constant is the
    // end-of-file test of the pinned `while (fgets(...) != NULL)` shape
    // (C99-48): the helper returns -1 for C's NULL result, so the
    // comparison folds to an index test exactly like strchr's below.
    {
      const clang::CallExpr *gets = asHostedFgetsCall(op->getLHS());
      const clang::Expr *nullSide = op->getRHS();
      if (!gets) {
        gets = asHostedFgetsCall(op->getRHS());
        nullSide = op->getLHS();
      }
      if (gets && isNullPointerConstantExpr(nullSide)) {
        if (op->getOpcode() != clang::BO_EQ &&
            op->getOpcode() != clang::BO_NE)
          return emitError(loc) << "unsupported: ordered comparison of an "
                                   "fgets result against a null pointer";
        FailureOr<Value> index = emitFileGetsIndex(gets);
        if (failed(index))
          return failure();
        Value nullIndex =
            createIntConstant(loc, builder.getIntegerType(64), -1);
        arith::CmpIPredicate predicate = op->getOpcode() == clang::BO_EQ
                                             ? arith::CmpIPredicate::eq
                                             : arith::CmpIPredicate::ne;
        return builder
            .create<arith::CmpIOp>(loc, predicate, *index, nullIndex)
            .getResult();
      }
    }
    // A FILE* handle local compared against a null pointer constant is
    // the fopen NULL check (C99-48); `__emitrust_file_ok` is the negation
    // of C's `f == NULL`. Non-handle FILE* comparisons keep the
    // historical pointer machinery and its located rejections.
    {
      auto asHandleRef = [&](const clang::Expr *expr) -> const clang::Expr * {
        if (!isFilePtrType(expr->getType()))
          return nullptr;
        const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            expr->IgnoreParenImpCasts());
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        return var && fileLocals.count(var) ? expr : nullptr;
      };
      const clang::Expr *handleSide = asHandleRef(op->getLHS());
      const clang::Expr *nullSide = op->getRHS();
      if (!handleSide) {
        handleSide = asHandleRef(op->getRHS());
        nullSide = op->getLHS();
      }
      if (handleSide && isNullPointerConstantExpr(nullSide)) {
        if (op->getOpcode() != clang::BO_EQ &&
            op->getOpcode() != clang::BO_NE)
          return emitError(loc) << "unsupported: ordered comparison of a "
                                   "FILE* stream against a null pointer";
        FailureOr<Value> truth = emitFileTruth(handleSide);
        if (failed(truth))
          return failure();
        if (op->getOpcode() == clang::BO_NE)
          return truth;
        Value trueValue = createBoolConstant(loc, true);
        return builder.create<arith::XOrIOp>(loc, *truth, trueValue)
            .getResult();
      }
    }
    bool lhsNull = isNullPointerConstantExpr(op->getLHS());
    bool rhsNull = isNullPointerConstantExpr(op->getRHS());
    auto isLiteralPointer = [](const clang::Expr *expr) {
      return llvm::isa<clang::StringLiteral>(expr->IgnoreParenCasts());
    };
    if ((isLiteralPointer(op->getLHS()) && rhsNull) ||
        (lhsNull && isLiteralPointer(op->getRHS()))) {
      if (op->getOpcode() == clang::BO_EQ)
        return createBoolConstant(loc, false);
      if (op->getOpcode() == clang::BO_NE)
        return createBoolConstant(loc, true);
      return emitError(loc) << "unsupported: ordered comparison of a string "
                               "literal against a null pointer";
    }
    // A strchr/strrchr result compared against a null pointer constant
    // asks "was the byte found": the helpers return the found index or -1
    // for C's NULL result, so the comparison folds to an index test
    // (CTS-L1; 00179.c tests `strrchr(a, 'x') == NULL`). This runs before
    // the general null-comparison path below, which cannot decompose a
    // call expression.
    bool reverse = false;
    const clang::CallExpr *search =
        asHostedStrchrCall(op->getLHS(), reverse);
    const clang::Expr *nullSide = op->getRHS();
    if (!search) {
      search = asHostedStrchrCall(op->getRHS(), reverse);
      nullSide = op->getLHS();
    }
    if (search && isNullPointerConstantExpr(nullSide)) {
      if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
        return emitError(loc)
               << "unsupported: ordered comparison of a "
               << (reverse ? "strrchr" : "strchr")
               << " result against a null pointer";
      PtrExprValue region;
      FailureOr<Value> index = emitStrchrIndex(search, reverse, region);
      if (failed(index))
        return failure();
      Value notFound =
          createIntConstant(loc, builder.getIntegerType(64), -1);
      arith::CmpIPredicate predicate = op->getOpcode() == clang::BO_EQ
                                           ? arith::CmpIPredicate::eq
                                           : arith::CmpIPredicate::ne;
      return builder
          .create<arith::CmpIOp>(loc, predicate, *index, notFound)
          .getResult();
    }
    if (lhsNull || rhsNull) {
      if (op->getOpcode() != clang::BO_EQ &&
          op->getOpcode() != clang::BO_NE)
        return emitError(loc)
               << "unsupported: ordered comparison against a null pointer";
      bool isEq = op->getOpcode() == clang::BO_EQ;
      if (lhsNull && rhsNull) // `NULL == NULL` is defined and constant.
        return createBoolConstant(loc, isEq);
      // Clang converts the pointer operand to `void *` when the null
      // constant is `(void*)0`; the conversion does not change the
      // decomposition and is peeled here.
      const clang::Expr *pointerSide =
          stripTrivia(lhsNull ? op->getRHS() : op->getLHS());
      while (const auto *cast =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(pointerSide)) {
        if (cast->getCastKind() != clang::CK_BitCast ||
            !isPointerType(cast->getSubExpr()->getType()))
          break;
        pointerSide = stripTrivia(cast->getSubExpr());
      }
      FailureOr<PtrExprValue> pointer = emitPointerRValue(pointerSide);
      if (failed(pointer))
        return failure();
      // A statically-null pointer (base-less nullable region, CTS-P9)
      // decomposes to nothing at all; its null test folds to the constant
      // truth of `null == null`.
      if (!pointer->base && !pointer->literalBacking && !pointer->baseIndex &&
          !pointer->nonNull)
        return createBoolConstant(loc, isEq);
      if (!pointer->nonNull) // Statically non-null: the check folds.
        return createBoolConstant(loc, !isEq);
      if (isEq) {
        Value truth = createBoolConstant(loc, true);
        return builder.create<arith::XOrIOp>(loc, pointer->nonNull, truth)
            .getResult();
      }
      return pointer->nonNull;
    }
    FailureOr<PtrExprValue> lhs = emitPointerRValue(op->getLHS());
    if (failed(lhs))
      return failure();
    FailureOr<PtrExprValue> rhs = emitPointerRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    // `p == q` where either side may be null is defined in C (a null and a
    // non-null pointer compare unequal), but the cursor comparison cannot
    // express it; the mixed-state comparison stays rejected.
    if (lhs->nonNull || rhs->nonNull)
      return emitError(loc)
             << "unsupported: comparison of possibly-null pointers";
    if (lhs->baseIndex || rhs->baseIndex) {
      // A multi-base side compares by (discriminant, cursor) pair: C
      // defines equality across distinct objects (unequal) and within one
      // object (cursor equality), which is exactly the pairwise test.
      // Ordered comparison is only defined within one object, which the
      // static region cannot guarantee here, so it stays rejected.
      if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
        return emitError(loc) << "unsupported: ordered comparison of "
                                 "pointers bound to multiple objects";
      // Resolve each side to a discriminant value: its own loaded
      // discriminant, or — for a single-base side naming one of the other
      // side's bases (`p == x`) — that base's constant index.
      auto indexOf = [&](const PtrExprValue &side,
                         const PtrExprValue &other) -> Value {
        if (side.baseIndex)
          return side.baseIndex;
        const auto *found = llvm::find(other.multiBases,
                                       PointerBaseKey{side.base, side.member});
        if (!side.base || found == other.multiBases.end())
          return Value();
        return createIntConstant(loc, builder.getIntegerType(32),
                                 found - other.multiBases.begin());
      };
      Value leftIndex = indexOf(*lhs, *rhs);
      Value rightIndex = indexOf(*rhs, *lhs);
      if (!leftIndex || !rightIndex ||
          (lhs->baseIndex && rhs->baseIndex &&
           lhs->multiBases != rhs->multiBases))
        return emitError(loc)
               << "unsupported: comparison of pointers into different "
                  "objects";
      Type indexCursorType = builder.getIntegerType(64);
      Value sameBase = builder
                           .create<arith::CmpIOp>(loc,
                                                  arith::CmpIPredicate::eq,
                                                  leftIndex, rightIndex)
                           .getResult();
      Value leftCursor = lhs->cursor
                             ? lhs->cursor
                             : createIntConstant(loc, indexCursorType, 0);
      Value rightCursor = rhs->cursor
                              ? rhs->cursor
                              : createIntConstant(loc, indexCursorType, 0);
      Value sameCursor = builder
                             .create<arith::CmpIOp>(
                                 loc, arith::CmpIPredicate::eq, leftCursor,
                                 rightCursor)
                             .getResult();
      Value equal =
          builder.create<arith::AndIOp>(loc, sameBase, sameCursor)
              .getResult();
      if (op->getOpcode() == clang::BO_EQ)
        return equal;
      Value truth = createBoolConstant(loc, true);
      return builder.create<arith::XOrIOp>(loc, equal, truth).getResult();
    }
    if (lhs->base != rhs->base || lhs->member != rhs->member ||
        lhs->literalBacking != rhs->literalBacking)
      return emitError(loc)
             << "unsupported: comparison of pointers into different objects";
    Type cursorType = builder.getIntegerType(64);
    Value left =
        lhs->cursor ? lhs->cursor : createIntConstant(loc, cursorType, 0);
    Value right =
        rhs->cursor ? rhs->cursor : createIntConstant(loc, cursorType, 0);
    arith::CmpIPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = arith::CmpIPredicate::slt;
      break;
    case clang::BO_LE:
      predicate = arith::CmpIPredicate::sle;
      break;
    case clang::BO_GT:
      predicate = arith::CmpIPredicate::sgt;
      break;
    case clang::BO_GE:
      predicate = arith::CmpIPredicate::sge;
      break;
    case clang::BO_EQ:
      predicate = arith::CmpIPredicate::eq;
      break;
    case clang::BO_NE:
      predicate = arith::CmpIPredicate::ne;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder.create<arith::CmpIOp>(loc, predicate, left, right)
        .getResult();
  }

  // Enum comparisons: Sema promotes enum operands to the (possibly
  // unsigned) underlying type, so the enum values are recovered from behind
  // the promotion casts. Equality maps to `emitrust.cmp` on the enum type
  // (Rust derives PartialEq); relational comparison has no derived Rust
  // ordering and compares the i32 discriminants instead. A comparison
  // between an enum and a non-enum integer takes the generic integer path
  // below: `emitRValue` renders the enum side as its `#[repr(i32)]`
  // discriminant via `emitrust.cast` (Rust `as i32`), matching C's
  // conversion of the enum operand to the common integer type.
  std::optional<EnumOperand> lhsEnum = classifyEnumOperand(op->getLHS());
  std::optional<EnumOperand> rhsEnum = classifyEnumOperand(op->getRHS());
  if (lhsEnum && rhsEnum) {
    if (lhsEnum->decl != rhsEnum->decl)
      return emitError(loc)
             << "unsupported: comparison between distinct enum types";
    FailureOr<Value> lhsValue = emitEnumOperand(*lhsEnum, loc);
    if (failed(lhsValue))
      return failure();
    FailureOr<Value> rhsValue = emitEnumOperand(*rhsEnum, loc);
    if (failed(rhsValue))
      return failure();
    switch (op->getOpcode()) {
    case clang::BO_EQ:
    case clang::BO_NE: {
      emitrust::CmpPredicate predicate = op->getOpcode() == clang::BO_EQ
                                             ? emitrust::CmpPredicate::eq
                                             : emitrust::CmpPredicate::ne;
      return builder
          .create<emitrust::CmpOp>(loc, builder.getI1Type(), predicate,
                                   *lhsValue, *rhsValue)
          .getResult();
    }
    case clang::BO_LT:
    case clang::BO_LE:
    case clang::BO_GT:
    case clang::BO_GE: {
      arith::CmpIPredicate predicate;
      switch (op->getOpcode()) {
      case clang::BO_LT:
        predicate = arith::CmpIPredicate::slt;
        break;
      case clang::BO_LE:
        predicate = arith::CmpIPredicate::sle;
        break;
      case clang::BO_GT:
        predicate = arith::CmpIPredicate::sgt;
        break;
      default:
        predicate = arith::CmpIPredicate::sge;
        break;
      }
      Value lhsInt = castEnumToI32(loc, *lhsValue);
      Value rhsInt = castEnumToI32(loc, *rhsValue);
      return builder.create<arith::CmpIOp>(loc, predicate, lhsInt, rhsInt)
          .getResult();
    }
    default:
      return emitError(loc) << "unsupported comparison";
    }
  }

  // An equality comparison against a negated signed integer literal —
  // `!= EOF` with EOF's `(-1)` expansion in particular — folds the
  // literal to a single negative constant, the pinned C99-48 EOF shape
  // (`arith.constant -1 : i32` met by `arith.cmpi`). Only parens are
  // looked through (an implicit conversion would change the constant's
  // type), and relational comparisons keep the historical `0 - x`
  // negation lowering (pinned in enum-int.c).
  auto emitComparisonOperand =
      [&](const clang::Expr *expr) -> FailureOr<Value> {
    if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
      return emitRValue(expr);
    const auto *minus =
        llvm::dyn_cast<clang::UnaryOperator>(expr->IgnoreParens());
    if (!minus || minus->getOpcode() != clang::UO_Minus)
      return emitRValue(expr);
    const auto *literal = llvm::dyn_cast<clang::IntegerLiteral>(
        minus->getSubExpr()->IgnoreParens());
    if (!literal)
      return emitRValue(expr);
    Location litLoc = translateLoc(minus->getOperatorLoc());
    FailureOr<Type> mapped = mapType(minus->getType(), litLoc);
    if (failed(mapped))
      return failure();
    auto intType = llvm::dyn_cast<IntegerType>(*mapped);
    if (!intType || !intType.isSignless())
      return emitRValue(expr);
    // The negation wraps in the literal's own width, matching C's
    // int-typed negation of a representable literal.
    llvm::APInt negated = -literal->getValue().sextOrTrunc(intType.getWidth());
    return createIntConstant(litLoc, intType, negated.getSExtValue());
  };
  FailureOr<Value> lhs = emitComparisonOperand(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<Value> rhs = emitComparisonOperand(op->getRHS());
  if (failed(rhs))
    return failure();
  if ((*lhs).getType() != (*rhs).getType())
    return emitError(loc) << "unsupported: comparison operand type mismatch";

  if (llvm::isa<FloatType>((*lhs).getType())) {
    arith::CmpFPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = arith::CmpFPredicate::OLT;
      break;
    case clang::BO_LE:
      predicate = arith::CmpFPredicate::OLE;
      break;
    case clang::BO_GT:
      predicate = arith::CmpFPredicate::OGT;
      break;
    case clang::BO_GE:
      predicate = arith::CmpFPredicate::OGE;
      break;
    case clang::BO_EQ:
      predicate = arith::CmpFPredicate::OEQ;
      break;
    case clang::BO_NE:
      predicate = arith::CmpFPredicate::UNE;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder.create<arith::CmpFOp>(loc, predicate, *lhs, *rhs)
        .getResult();
  }
  if (isUnsignedInt((*lhs).getType())) {
    // arith.cmpi requires signless operands; `emitrust.cmp` renders the
    // Rust infix comparison, which is type-directed and therefore unsigned
    // on uN operands, exactly matching C's unsigned comparisons.
    emitrust::CmpPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = emitrust::CmpPredicate::lt;
      break;
    case clang::BO_LE:
      predicate = emitrust::CmpPredicate::le;
      break;
    case clang::BO_GT:
      predicate = emitrust::CmpPredicate::gt;
      break;
    case clang::BO_GE:
      predicate = emitrust::CmpPredicate::ge;
      break;
    case clang::BO_EQ:
      predicate = emitrust::CmpPredicate::eq;
      break;
    case clang::BO_NE:
      predicate = emitrust::CmpPredicate::ne;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(), predicate, *lhs,
                                 *rhs)
        .getResult();
  }
  if (llvm::isa<IntegerType>((*lhs).getType())) {
    arith::CmpIPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = arith::CmpIPredicate::slt;
      break;
    case clang::BO_LE:
      predicate = arith::CmpIPredicate::sle;
      break;
    case clang::BO_GT:
      predicate = arith::CmpIPredicate::sgt;
      break;
    case clang::BO_GE:
      predicate = arith::CmpIPredicate::sge;
      break;
    case clang::BO_EQ:
      predicate = arith::CmpIPredicate::eq;
      break;
    case clang::BO_NE:
      predicate = arith::CmpIPredicate::ne;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder.create<arith::CmpIOp>(loc, predicate, *lhs, *rhs)
        .getResult();
  }
  return emitError(loc) << "unsupported comparison operand type";
}

FailureOr<Value> CImporter::emitCondition(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (binary->isComparisonOp())
      return emitComparison(binary);
    if (binary->getOpcode() == clang::BO_LAnd ||
        binary->getOpcode() == clang::BO_LOr)
      return emitShortCircuit(binary);
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_LNot) {
      FailureOr<Value> inner = emitCondition(unary->getSubExpr());
      if (failed(inner))
        return failure();
      Value truth = createBoolConstant(loc, true);
      return builder.create<arith::XOrIOp>(loc, *inner, truth).getResult();
    }
  // A function pointer tested for truth (`if (fp)`, `!fp`) is a null
  // check: load the Option<fn> value and compare it against a `None`
  // constant. Clang leaves the condition fn-ptr-typed (no boolean cast).
  if (isFunctionPointer(e->getType())) {
    FailureOr<Value> value = emitRValue(e);
    if (failed(value))
      return failure();
    Value none = createFnPtrNone(loc, (*value).getType());
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                 emitrust::CmpPredicate::ne, *value, none)
        .getResult();
  }
  // A FILE* handle local tested for truth (`if (!f)` after fopen —
  // C99-48) is its NULL check, and a truth-tested fgets result is its
  // end-of-file test; both must run before the decomposed-pointer truth
  // paths below, which cannot represent a stream. Non-handle FILE*
  // expressions (`if (stdin)`) keep the historical paths and their
  // located rejections.
  auto isFileTruthExpr = [&](const clang::Expr *expr) {
    if (asHostedFgetsCall(expr))
      return true;
    const auto *ref =
        llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    return var && fileLocals.count(var);
  };
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_PointerToBoolean &&
        isFileTruthExpr(cast->getSubExpr()))
      return emitFileTruth(cast->getSubExpr());
  if (isFileTruthExpr(e) &&
      (isFilePtrType(e->getType()) || asHostedFgetsCall(e)))
    return emitFileTruth(e);
  // A data pointer tested for truth (`if (p)`, `!p`) is a null check: the
  // Option-of-cursor discrimination of the decomposed pointer (CTS-P8).
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_PointerToBoolean)
      return emitPointerTruth(cast->getSubExpr());
  if (isPointerType(e->getType()))
    return emitPointerTruth(e);
  // Strip explicit truthiness casts so `_Bool` conversions don't double up.
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_IntegralToBoolean ||
        cast->getCastKind() == clang::CK_FloatingToBoolean)
      return emitCondition(cast->getSubExpr());

  FailureOr<Value> value = emitRValue(e);
  if (failed(value))
    return failure();
  Type type = (*value).getType();
  if (type.isInteger(1))
    return *value;
  // C tests an enum for truth by its integer value; compare the i32
  // discriminant against zero.
  if (llvm::isa<emitrust::EnumType>(type)) {
    Value discriminant = castEnumToI32(loc, *value);
    Value zero = createIntConstant(loc, builder.getI32Type(), 0);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, discriminant,
                               zero)
        .getResult();
  }
  // An unsigned value is truthy when not equal to an unsigned zero; the
  // comparison must be an `emitrust.cmp` (arith requires signless).
  if (isUnsignedInt(type)) {
    Value zero = createScalarIntConstant(loc, type, 0);
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                 emitrust::CmpPredicate::ne, *value, zero)
        .getResult();
  }
  if (llvm::isa<IntegerType>(type)) {
    Value zero = createIntConstant(loc, type, 0);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, *value, zero)
        .getResult();
  }
  if (auto floatType = llvm::dyn_cast<FloatType>(type)) {
    Value zero =
        builder.create<arith::ConstantOp>(loc, FloatAttr::get(floatType, 0.0))
            .getResult();
    return builder
        .create<arith::CmpFOp>(loc, arith::CmpFPredicate::UNE, *value, zero)
        .getResult();
  }
  return emitError(loc) << "unsupported condition type";
}

FailureOr<Value> CImporter::emitShortCircuit(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  bool isAnd = op->getOpcode() == clang::BO_LAnd;

  // A compile-time-constant left operand decides the circuit BEFORE
  // lowering (mirroring `emitConditionalOperator`): when it
  // short-circuits, C guarantees the right operand never evaluates, so a
  // dead RHS may contain otherwise-unimportable constructs (the 00207
  // `0 && printf(...)` value shape); when it does not, the RHS alone
  // decides the truth value. A live label in the dead operand keeps the
  // full lowering below (see `emitConditionalOperator`).
  clang::Expr::EvalResult lhsValue;
  if (op->getLHS()->EvaluateAsInt(lhsValue, astContext())) {
    bool truth = lhsValue.Val.getInt() != 0;
    if (truth == isAnd) // `1 && rhs` / `0 || rhs`: the RHS decides.
      return emitCondition(op->getRHS());
    if (!containsLabelStmt(op->getRHS()) &&
        !findNestedSwitchLabel(op->getRHS()))
      return createIntConstant(loc, builder.getI1Type(), truth ? 1 : 0);
  }

  // The result lives in a promotable rank-0 i1 cell: the left value covers
  // the short-circuit path, the right value overwrites it otherwise.
  Value flag = createEntryAlloca(loc, builder.getI1Type());
  FailureOr<Value> lhs = emitCondition(op->getLHS());
  if (failed(lhs))
    return failure();
  builder.create<memref::StoreOp>(loc, *lhs, flag);

  Block *rhsBlock = createBlock();
  Block *endBlock = createBlock();
  if (isAnd)
    builder.create<cf::CondBranchOp>(loc, *lhs, rhsBlock, ValueRange(),
                                     endBlock, ValueRange());
  else
    builder.create<cf::CondBranchOp>(loc, *lhs, endBlock, ValueRange(),
                                     rhsBlock, ValueRange());

  builder.setInsertionPointToEnd(rhsBlock);
  FailureOr<Value> rhs = emitCondition(op->getRHS());
  if (failed(rhs))
    return failure();
  builder.create<memref::StoreOp>(loc, *rhs, flag);
  builder.create<cf::BranchOp>(loc, endBlock);

  builder.setInsertionPointToEnd(endBlock);
  return builder.create<memref::LoadOp>(loc, flag, ValueRange()).getResult();
}

FailureOr<Value>
CImporter::emitConditionalOperator(const clang::ConditionalOperator *op) {
  Location loc = translateLoc(op->getQuestionLoc());
  // A compile-time-constant, side-effect-free condition elides the dead
  // arm BEFORE lowering (mirroring `emitIfStmt`), so a dead arm may
  // contain otherwise-unimportable constructs. The elision is gated on
  // the same live-label check as the if form: a goto-targeted label in
  // the dead arm (necessarily inside a statement expression, and
  // necessarily targeted from within the arm itself — clang rejects
  // jumps into a statement expression from outside) keeps the arm's code
  // reachable, and a case/default label of an enclosing switch must not
  // be dropped either — both shapes keep the FULL lowering below, whose
  // constant branch leaves the arm dynamically dead while its labels
  // register with the ordinary dispatch (the 00213 kb_wait_1 shape).
  clang::Expr::EvalResult conditionValue;
  if (op->getCond()->EvaluateAsInt(conditionValue, astContext())) {
    bool truth = conditionValue.Val.getInt() != 0;
    const clang::Expr *live = truth ? op->getTrueExpr() : op->getFalseExpr();
    const clang::Expr *dead = truth ? op->getFalseExpr() : op->getTrueExpr();
    if (!containsLabelStmt(dead) && !findNestedSwitchLabel(dead))
      return emitRValue(live);
  }
  FailureOr<Type> mapped = mapType(op->getType(), loc);
  if (failed(mapped))
    return failure();
  if (!llvm::isa<IntegerType, FloatType>(*mapped))
    return emitError(loc)
           << "unsupported: conditional operator on a non-scalar operand";

  // The result flows through a cell so that each arm evaluates in its own
  // block and only the selected arm's side effects run: a promotable
  // rank-0 memref cell for memref-legal scalars, or an `emitrust.variable`
  // place for unsigned integers (see `emitLocalVar` for why unsigned
  // values cannot live in memref cells).
  Value cell = isUnsignedInt(*mapped)
                   ? builder
                         .create<emitrust::VariableOp>(
                             loc, emitrust::LValueType::get(*mapped))
                         .getResult()
                   : createEntryAlloca(loc, *mapped);

  FailureOr<Value> condition = emitCondition(op->getCond());
  if (failed(condition))
    return failure();
  Block *trueBlock = createBlock();
  Block *falseBlock = createBlock();
  Block *endBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, *condition, trueBlock, ValueRange(),
                                   falseBlock, ValueRange());

  // Clang has already wrapped both arms in the implicit conversions to the
  // operator's common type, so after mapping both arms must produce
  // exactly the cell's type; anything else is an importer gap and is
  // rejected rather than silently converted.
  auto emitArm = [&](Block *block, const clang::Expr *arm) -> LogicalResult {
    builder.setInsertionPointToEnd(block);
    FailureOr<Value> value = emitRValue(arm);
    if (failed(value))
      return failure();
    if ((*value).getType() != *mapped)
      return emitError(translateLoc(arm->getBeginLoc()))
             << "unsupported: conditional operator arm type mismatch";
    if (failed(storeToPlace(loc, cell, *value)))
      return failure();
    // The arm's expression may have opened further blocks (nested `?:` or
    // short-circuit operators); the branch closes the current one.
    builder.create<cf::BranchOp>(loc, endBlock);
    return success();
  };
  if (failed(emitArm(trueBlock, op->getTrueExpr())) ||
      failed(emitArm(falseBlock, op->getFalseExpr())))
    return failure();

  builder.setInsertionPointToEnd(endBlock);
  return loadPlace(loc, cell);
}

/// Finds a `goto` below `expr`'s body whose target label is NOT declared
/// inside the same statement expression, or null when every goto stays
/// internal. Labels of nested statement expressions count as internal:
/// clang itself rejects any jump INTO a statement expression, so a goto
/// this scan accepts always lands on a label the flattened body registers.
static const clang::GotoStmt *
findGotoOutOfStmtExpr(const clang::StmtExpr *expr) {
  llvm::SmallPtrSet<const clang::LabelDecl *, 8> internalLabels;
  SmallVector<const clang::Stmt *> worklist{expr->getSubStmt()};
  while (!worklist.empty()) {
    const clang::Stmt *current = worklist.pop_back_val();
    if (!current)
      continue;
    if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(current))
      internalLabels.insert(label->getDecl());
    for (const clang::Stmt *child : current->children())
      worklist.push_back(child);
  }
  worklist.push_back(expr->getSubStmt());
  while (!worklist.empty()) {
    const clang::Stmt *current = worklist.pop_back_val();
    if (!current)
      continue;
    if (const auto *gotoStmt = llvm::dyn_cast<clang::GotoStmt>(current))
      if (!internalLabels.contains(gotoStmt->getLabel()))
        return gotoStmt;
    for (const clang::Stmt *child : current->children())
      worklist.push_back(child);
  }
  return nullptr;
}

FailureOr<Value> CImporter::emitStmtExpr(const clang::StmtExpr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  // A goto that leaves a value-position statement expression abandons the
  // expression mid-evaluation: the synthesized value temp would never be
  // written and the consumer would read garbage.
  if (const clang::GotoStmt *escape = findGotoOutOfStmtExpr(expr))
    return emitError(translateLoc(escape->getGotoLoc()))
           << "unsupported: goto out of a statement expression in value "
              "position";
  FailureOr<Type> mapped = mapType(expr->getType(), loc);
  if (failed(mapped))
    return failure();
  if (!llvm::isa<IntegerType, FloatType>(*mapped))
    return emitError(loc)
           << "unsupported: statement expression of a non-scalar type";
  const clang::CompoundStmt *body = expr->getSubStmt();
  const clang::Expr *valueExpr =
      body->body_empty() ? nullptr
                         : llvm::dyn_cast<clang::Expr>(body->body_back());
  if (!valueExpr) // Defensive: clang typed this StmtExpr non-void.
    return emitError(loc) << "unsupported: statement expression without a "
                             "final expression statement";
  // The value transits a synthesized temp cell so that the body statements
  // — which lower FLATTENED into the enclosing function and may open
  // further blocks (labels, loops, nested conditionals) — never wall the
  // value off in a region: an `emitrust.variable` place for unsigned
  // integers, a promotable rank-0 memref cell otherwise (mirroring
  // `emitConditionalOperator`).
  Value cell = isUnsignedInt(*mapped)
                   ? builder
                         .create<emitrust::VariableOp>(
                             loc, emitrust::LValueType::get(*mapped))
                         .getResult()
                   : createEntryAlloca(loc, *mapped);
  for (const clang::Stmt *child : body->body()) {
    if (child == body->body_back())
      break;
    if (failed(emitStmt(child)))
      return failure();
  }
  // C applies the lvalue conversion to the final expression statement;
  // clang usually materializes it, but a bare lvalue is loaded here so
  // both AST shapes land the same value in the temp.
  FailureOr<Value> value = failure();
  if (valueExpr->isGLValue()) {
    FailureOr<Value> place = emitLValue(valueExpr);
    if (failed(place))
      return failure();
    value = loadPlace(loc, *place);
  } else {
    value = emitRValue(valueExpr);
  }
  if (failed(value))
    return failure();
  if ((*value).getType() != *mapped)
    return emitError(translateLoc(valueExpr->getBeginLoc()))
           << "unsupported: statement expression value type mismatch";
  if (failed(storeToPlace(loc, cell, *value)))
    return failure();
  return loadPlace(loc, cell);
}

FailureOr<Value>
CImporter::emitSizeofAlignof(const clang::UnaryExprOrTypeTraitExpr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  clang::UnaryExprOrTypeTrait kind = expr->getKind();
  if (kind != clang::UETT_SizeOf && kind != clang::UETT_AlignOf)
    return emitError(loc) << "unsupported sizeof/alignof kind";
  clang::QualType operand = expr->isArgumentType()
                                ? expr->getArgumentType()
                                : expr->getArgumentExpr()->getType();
  // `sizeof` of a variable-length array is the one operand C evaluates at
  // run time; there is no constant to fold. Incomplete and function types
  // (GNU extensions accept them) have no portable layout either.
  if (operand->isVariablyModifiedType())
    return emitError(loc)
           << "unsupported: sizeof/alignof of a variable-length array";
  if (operand->isIncompleteType() || operand->isFunctionType())
    return emitError(loc)
           << "unsupported: sizeof/alignof of an incomplete or function type";
  // C99-45: the backing-run model gives a bit-field struct a well-defined
  // Rust size, but that size need not match the C ABI layout this fold
  // would promise, so the importer refuses rather than asserting
  // ABI-exact layout.
  if (typeContainsBitField(operand))
    return emitError(loc)
           << (kind == clang::UETT_SizeOf
                   ? "unsupported: sizeof of a struct with bit-fields"
                   : "unsupported: alignof of a struct with bit-fields");
  // CTS 00204: long double imports as an 8-byte f64, but the C fold would
  // yield the 16-byte x86-64 ABI size — a layout the emitted Rust never
  // keeps (the same reasoning as the bit-field rejection above).
  if (typeContainsLongDouble(operand))
    return emitError(loc) << "unsupported: sizeof/alignof of long double";
  int64_t value =
      kind == clang::UETT_SizeOf
          ? astContext().getTypeSizeInChars(operand).getQuantity()
          : astContext().getTypeAlignInChars(operand).getQuantity();
  // The result's C type is size_t (unsigned long here); the fold uses
  // clang's target layout, so the value always matches a native build of
  // the same translation unit.
  FailureOr<Type> resultType = mapType(expr->getType(), loc);
  if (failed(resultType))
    return failure();
  return createScalarIntConstant(loc, *resultType, value);
}

FailureOr<Value> CImporter::emitCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee)
    return emitIndirectCall(call);
  // `__builtin_expect(e, c)` is a pure branch-prediction hint: it folds to
  // its first argument during import, in every position (condition or
  // value), so no call op or `__builtin_expect` symbol ever reaches the
  // IR. A constant argument (`!!(0)`) then composes with the
  // constant-condition dead-arm elision of `emitIfStmt` and
  // `emitConditionalOperator` through clang's constant evaluator, which
  // folds the intact call the same way.
  switch (callee->getBuiltinID()) {
  case clang::Builtin::BI__builtin_expect:
  case clang::Builtin::BI__builtin_expect_with_probability:
    return emitRValue(call->getArg(0));
  default:
    break;
  }
  if (!callee->getDeclName().isIdentifier())
    return emitError(loc) << "unsupported callee";
  // The hosted (definition-less) printf lowering is statement-position
  // only; a project-supplied printf definition is an ordinary imported
  // function whose result is an ordinary value in every position.
  if (callee->getName() == "printf" && !callee->getDefinition())
    return emitError(loc) << "unsupported: printf return value must be unused";
  // Statement-position puts/putchar are lowered by name (emitCallStmt);
  // their int result has no representation there, so a value use of a
  // definition-less puts/putchar is rejected.
  if ((callee->getName() == "puts" || callee->getName() == "putchar") &&
      !callee->getDefinition())
    return emitError(loc) << "unsupported: " << callee->getName()
                          << " return value must be unused";
  // A definition-less strlen is lowered by name like printf/puts: its
  // supported argument shape is a pointer into a string-literal region,
  // rendered through the `__emitrust_strlen` helper. A user-defined strlen
  // is an ordinary call.
  if (callee->getName() == "strlen" && !callee->getDefinition())
    return emitStrlenCall(call);
  // Hosted <string.h> comparisons (design.md C99-48, CTS-L1) are lowered
  // by name, like strlen; their int result is an ordinary value. The
  // copy/fill functions of the same surface are statement-position only
  // (their char* result has no decomposed representation), and a
  // strchr/strrchr result is consumed by the printf %s and null-comparison
  // interceptions before reaching this point.
  if (!callee->getDefinition()) {
    llvm::StringRef name = callee->getName();
    // A definition-less sprintf is lowered by name (design.md CTS-P9,
    // 00186): the literal format translates through the shared printf
    // grammar into a `format!` String and the `__emitrust_sprintf`
    // helper writes it into the destination char region, returning the
    // length. Statement-position calls arrive here through emitCallStmt's
    // emitCall fallthrough and simply discard the value.
    if (name == "sprintf")
      return emitSprintf(call);
    if (name == "strcmp")
      return emitStringCompareCall(call, "strcmp", /*hasCount=*/false);
    if (name == "strncmp")
      return emitStringCompareCall(call, "strncmp", /*hasCount=*/true);
    if (name == "memcmp")
      return emitStringCompareCall(call, "memcmp", /*hasCount=*/true);
    if (name == "strcpy" || name == "strncpy" || name == "strcat" ||
        name == "memset" || name == "memcpy" || name == "memmove")
      return emitError(loc) << "unsupported: " << name
                            << " return value must be unused";
    // Hosted <stdlib.h> value-position surface (design.md C99-48):
    // abs/labs onto wrapping_abs, atoi as an exact C parse over the
    // argument's char region. A user-defined function of any of these
    // names is an ordinary call (the getDefinition guard above).
    if (name == "abs")
      return emitAbsCall(call, /*isLong=*/false);
    if (name == "labs")
      return emitAbsCall(call, /*isLong=*/true);
    if (name == "atoi")
      return emitAtoiCall(call);
    // Curated <math.h> rejections (design.md C99-48): C imposes no
    // accuracy requirement on these, implementations disagree in the
    // last bits, and rustc may constant-fold them through a libm other
    // than the differential oracle's — no bit-exact safe-Rust mapping
    // can be argued, so they stay out of the curated subset by policy
    // (a sharper diagnostic than the generic system-header rejection).
    if (name == "pow" || name == "exp" || name == "log")
      return emitError(loc)
             << "unsupported: '" << name
             << "' has no bit-exact Rust mapping (C imposes no accuracy "
                "requirement and libm implementations disagree)";
    if (name == "strchr" || name == "strrchr")
      return emitError(loc)
             << "unsupported: a " << name
             << " result must feed a printf '%s' argument or a "
                "comparison against a null pointer";
    // Hosted <stdio.h> FILE* streams (design.md C99-48, CTS-T1.3):
    // sequential byte-wise I/O on an owned function-local handle. File
    // positioning contradicts the sequential-only model and stays a
    // located rejection.
    if (name == "fseek" || name == "ftell" || name == "rewind")
      return emitError(loc) << "unsupported: file positioning '" << name
                            << "' (FILE* streams are sequential-only)";
    // fprintf to a real stream variable would need a formatted writer
    // over the handle; only the devirtualized stdout-swallow form (see
    // emitAliasedPrintf) is accepted. The literal `stdout` argument
    // falls through to the historical variadic-call rejection.
    if (name == "fprintf" && call->getNumArgs() >= 1) {
      const auto *stream = llvm::dyn_cast<clang::DeclRefExpr>(
          call->getArg(0)->IgnoreParenImpCasts());
      const clang::NamedDecl *streamDecl =
          stream ? llvm::dyn_cast<clang::NamedDecl>(stream->getDecl())
                 : nullptr;
      if (!streamDecl || !streamDecl->getDeclName().isIdentifier() ||
          streamDecl->getName() != "stdout")
        return emitError(translateLoc(call->getArg(0)->getBeginLoc()))
               << "unsupported: fprintf to a FILE* stream (only the "
                  "devirtualized stdout form is supported)";
    }
    // A fopen result in any position but a FILE* handle local's
    // initializer or assignment (consumed by emitFileLocal /
    // emitFileOpenInto before this point) falls through to the
    // system-header rejection below, keeping the historical wording.
    if (name == "fgetc" || name == "getc")
      return emitFileGetc(call);
    if (name == "fread")
      return emitFileReadWrite(call, /*isWrite=*/false);
    if (name == "fwrite")
      return emitFileReadWrite(call, /*isWrite=*/true);
    // An fgets result is consumed by the null-comparison and truth-test
    // interceptions (emitComparison / emitCondition); its char* value
    // has no representation anywhere else.
    if (name == "fgets")
      return emitError(loc)
             << "unsupported: an fgets result must be compared against a "
                "null pointer or tested for truth";
    // Statement-position fclose is lowered by emitCallStmt; C's int
    // result has no representation here.
    if (name == "fclose")
      return emitError(loc)
             << "unsupported: fclose return value must be unused";
  }
  // A variadic callee is supported when its definition imports as its
  // fixed prototype (a va_list-free body, see `importFunction`; the call
  // drops its trailing extras below) or when it monomorphizes per call
  // site (a bounded va_list-using body, CTS 00204; the call rewrites to
  // this site's clone with the extras as ordinary fixed arguments).
  // Every other variadic call keeps the rejection.
  const VaClonePlan *vaClone = nullptr;
  if (callee->isVariadic()) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    auto planIt = definition
                      ? vaMonomorphPlans.find(definition->getCanonicalDecl())
                      : vaMonomorphPlans.end();
    if (planIt != vaMonomorphPlans.end()) {
      auto siteIt = vaCallSiteClones.find(call);
      if (siteIt == vaCallSiteClones.end()) // Defensive; the planner
                                            // enumerated every site.
        return emitError(loc) << "unsupported: call to a variadic function";
      vaClone = &planIt->second.clones[siteIt->second];
    } else if (!definition || !definition->hasBody() ||
               bodyUsesVaList(astContext(), definition->getBody())) {
      return emitError(loc) << "unsupported: call to a variadic function";
    }
  }

  // Hosted <math.h> surface (design.md C99-48): a definition-less call to
  // a recognized math function with its standard `double f(double)`
  // prototype lowers to the equivalent safe Rust f64 function. A
  // user-defined function of the same name stays an ordinary call
  // (mirroring the puts/putchar policy), and every other system-header
  // call keeps the rejection below.
  if (!callee->getDefinition() && callee->getNumParams() == 1 &&
      astContext().hasSameUnqualifiedType(callee->getReturnType(),
                                          astContext().DoubleTy) &&
      astContext().hasSameUnqualifiedType(callee->getParamDecl(0)->getType(),
                                          astContext().DoubleTy)) {
    if (std::optional<llvm::StringRef> rustCallee =
            hostedMathCallee(callee->getName()))
      return emitHostedMathCall(call, *rustCallee);
  }

  std::string name = vaClone ? vaClone->name : mlirFuncName(callee);
  func::FuncOp target = functions.lookup(name);
  if (!target) {
    if (isSystemHeaderDecl(callee))
      return rejectSystemHeaderUse(loc, "call to", callee->getName());
    return emitError(loc) << "unsupported: call to unimported function '"
                          << name << "'";
  }

  // A method-planned callee (Phase 4) takes the owner receiver plus i64
  // element cursors in place of its pointer arguments.
  if (const clang::VarDecl *ownerBase =
          methodPlans.lookup(callee->getCanonicalDecl()))
    return emitMethodCallSite(call, target, ownerBase, loc);

  // A callee with planned string-cursor parameters (CTS 00204) expands
  // each `&p` cursor argument into (shared region slice, in-out cursor)
  // and stores the advanced cursor back after the call.
  if (const clang::FunctionDecl *definition = callee->getDefinition();
      definition && llvm::any_of(definition->parameters(),
                                 [&](const clang::ParmVarDecl *param) {
                                   return cursorParams.contains(param);
                                 }))
    return emitCursorParamCall(call, target, loc);

  FunctionType targetType = target.getFunctionType();
  unsigned namedArgCount = call->getNumArgs();
  if (vaClone) {
    // A monomorphized call passes every argument — named parameters and
    // this site's extras — as ordinary fixed arguments to the clone.
    if (call->getNumArgs() != targetType.getNumInputs())
      return emitError(loc) << "unsupported: call argument count mismatch";
  } else if (callee->isVariadic()) {
    // A fixed-prototype variadic callee (va_list-free body, checked
    // above) takes only its named parameters; the trailing extras are
    // dropped from the call. A dropped extra is never imported — it must
    // not load a by-value struct or borrow an `&s` operand — so an extra
    // whose evaluation has side effects would silently lose them and is
    // rejected instead.
    if (call->getNumArgs() < targetType.getNumInputs())
      return emitError(loc) << "unsupported: call argument count mismatch";
    namedArgCount = targetType.getNumInputs();
    for (unsigned index = namedArgCount; index < call->getNumArgs(); ++index)
      if (call->getArg(index)->HasSideEffects(astContext()))
        return emitError(loc) << "unsupported: extra argument to a variadic "
                                 "call has side effects";
  } else if (call->getNumArgs() != targetType.getNumInputs()) {
    return emitError(loc) << "unsupported: call argument count mismatch";
  }

  // C leaves the argument evaluation order unspecified; materialize every
  // value argument before any borrow-producing argument so that no load is
  // emitted between a `&mut` borrow and the call consuming it (rustc
  // rejects an intervening use of the borrowed place).
  SmallVector<Value> arguments(namedArgCount, Value());
  struct PendingBorrow {
    unsigned index;
    const clang::Expr *expr;
  };
  SmallVector<PendingBorrow, 4> borrows;
  // Cell-slice arguments backed by globals (CTS-P10): the call nests
  // inside one `emitrust.global_cells` region per distinct global,
  // leftmost argument outermost. Only the named-parameter prefix is
  // examined: dropped variadic extras are never imported.
  struct PendingCellGlobal {
    unsigned index;
    const clang::VarDecl *global;
  };
  SmallVector<PendingCellGlobal, 3> cellGlobals;
  for (unsigned index = 0; index < namedArgCount; ++index) {
    const clang::Expr *argument = call->getArg(index);
    Type input = targetType.getInput(index);
    if (auto refType = llvm::dyn_cast<emitrust::RefType>(input);
        refType && llvm::isa<emitrust::CellSliceType>(refType.getPointee())) {
      // A forwarded cell-slice parameter passes as the same SSA value:
      // shared references are freely duplicable, so permuted recursive
      // forwarding needs no reborrow discipline.
      if (const clang::ParmVarDecl *param = asPointerParamRead(argument)) {
        auto it = symbols.find(param);
        if (it != symbols.end() && it->second.getType() == input) {
          arguments[index] = it->second;
          continue;
        }
      }
      // A directly decayed global array borrows its cell-slice for the
      // extent of the call (Pass A pinned exactly these two shapes).
      if (const clang::VarDecl *global = asDecayedGlobalArrayArg(argument)) {
        cellGlobals.push_back({index, global});
        continue;
      }
      return emitError(loc)
             << "unsupported: argument to a cell-slice parameter must be a "
                "whole global array or a forwarded cell-slice parameter";
    }
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      borrows.push_back({index, argument});
      continue;
    }
    // An integer-carrier parameter (CTS-P3) takes a plain i64; the
    // pointer-typed argument must itself be a carrier value (a carrier
    // local/parameter read, a null constant, an integer-to-pointer cast,
    // or a carrier-returning call).
    if (isDataPointer(argument->getType()) &&
        input == builder.getIntegerType(64)) {
      FailureOr<Value> carrier = emitCarrierValue(argument);
      if (failed(carrier))
        return failure();
      arguments[index] = *carrier;
      continue;
    }
    // Positioned against the callee's input: a refined
    // (callsite-inferred, FR-29 / CTS 00209) fn-ptr parameter binds a
    // directly-referenced function at the refined signature.
    FailureOr<Value> value = emitPositionedRValue(input, argument);
    if (failed(value))
      return failure();
    arguments[index] = *value;
  }

  // Borrow-producing arguments: each resolves to a fresh borrow of its
  // region base. Two borrows of the same base would alias mutably in Rust;
  // they are rejected rather than emitted.
  SmallVector<const clang::VarDecl *, 4> borrowRoots;
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    FailureOr<Value> reference = emitBorrowArgument(
        loc, borrow.expr, targetType.getInput(borrow.index), root);
    if (failed(reference))
      return failure();
    if (root && llvm::is_contained(borrowRoots, root))
      return emitError(loc)
             << "unsupported: aliasing mutable pointer arguments (two "
                "arguments borrow object '"
             << root->getName() << "')";
    if (root)
      borrowRoots.push_back(root);
    arguments[borrow.index] = *reference;
  }

  // Open one global_cells region per distinct global cell-slice argument
  // (argument order, leftmost outermost); the call is created inside the
  // innermost region and a scalar result flows out through a staging
  // variable declared before the outermost region.
  Value cellResultStaging;
  SmallVector<emitrust::GlobalCellsOp, 3> openedCellRegions;
  if (!cellGlobals.empty()) {
    if (targetType.getNumResults() == 1)
      cellResultStaging = createVariablePlace(loc, targetType.getResult(0));
    llvm::SmallDenseMap<const clang::VarDecl *, Value, 4> borrowedCells;
    for (const PendingCellGlobal &entry : cellGlobals) {
      if (Value existing = borrowedCells.lookup(entry.global)) {
        arguments[entry.index] = existing;
        continue;
      }
      const GlobalInfo *global = lookupGlobal(entry.global);
      if (!global)
        return emitError(loc) << "unsupported: global '"
                              << entry.global->getName()
                              << "' is not an importable cell-slice base";
      auto cellsOp = builder.create<emitrust::GlobalCellsOp>(
          loc, globalSymbol(global->symbol));
      Block *body = builder.createBlock(
          &cellsOp.getBody(), cellsOp.getBody().end(),
          TypeRange{targetType.getInput(entry.index)}, {loc});
      arguments[entry.index] = body->getArgument(0);
      borrowedCells[entry.global] = body->getArgument(0);
      openedCellRegions.push_back(cellsOp);
    }
  }

  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  // CTS-BR (00216): staged byte-region-global slice arguments store their
  // (possibly mutated) images back immediately after the call — the
  // load-modify-store shape of every staged global access.
  if (!pendingStagedGlobalStores.empty()) {
    SmallVector<std::pair<Value, std::string>, 2> stores =
        std::move(pendingStagedGlobalStores);
    pendingStagedGlobalStores.clear();
    for (auto &[place, symbol] : stores) {
      Value value = loadPlace(loc, place);
      builder.create<emitrust::GlobalStoreOp>(loc, value,
                                              globalSymbol(symbol));
    }
  }
  if (!openedCellRegions.empty()) {
    if (cellResultStaging && callOp->getNumResults() == 1)
      builder.create<emitrust::AssignOp>(loc, cellResultStaging,
                                         callOp->getResult(0));
    for (emitrust::GlobalCellsOp cellsOp : llvm::reverse(openedCellRegions)) {
      builder.create<emitrust::YieldOp>(loc);
      builder.setInsertionPointAfter(cellsOp);
    }
    if (callOp->getNumResults() == 0)
      return Value();
    return loadPlace(loc, cellResultStaging);
  }
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<Value> CImporter::emitMethodCallSite(const clang::CallExpr *call,
                                               func::FuncOp target,
                                               const clang::VarDecl *ownerBase,
                                               Location loc) {
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() + 1 != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";

  // The owner place in the current context: the dereferenced receiver in a
  // sibling method (rendering `(*self).m(...)` after conversion), or the
  // owner struct variable in the owning function.
  Value ownerPlace = currentMethodOwner == ownerBase
                         ? currentReceiverPlace
                         : ownerStructPlaces.lookup(ownerBase);
  if (!ownerPlace) // Defensive; the owner plan covers every call site.
    return emitError(loc) << "unsupported: call to owner method '"
                          << target.getSymName()
                          << "' outside its owner's scope";

  // Every argument is a plain value — pointer arguments lower to their i64
  // element cursors — so the receiver borrow below is the only reference
  // and is materialized last, immediately before the call (one-statement
  // borrow; no load intervenes).
  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    if (isPointerType(argument->getType()) &&
        !isFunctionPointer(argument->getType())) {
      FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
      if (failed(pointer))
        return failure();
      // Defensive: the interprocedural plan guarantees the argument points
      // into the owner's region — directly at the owner base in the owning
      // function, or through the current method's own decomposed pointer
      // parameters in a sibling method.
      bool rootedAtOwner =
          pointer->base == ownerBase ||
          (currentMethodOwner == ownerBase &&
           llvm::isa_and_nonnull<clang::ParmVarDecl>(pointer->base));
      if (!rootedAtOwner)
        return emitError(loc)
               << "unsupported: pointer argument does not point into owner "
                  "object '"
               << ownerBase->getName() << "'";
      // Defensive: owner planning excludes nullable regions, so no
      // discriminant should survive to a method-call cursor argument.
      if (pointer->nonNull)
        return emitError(loc) << "unsupported: possibly-null pointer passed "
                                 "as a function argument";
      if (!pointer->cursor) // Defensive; an array base always has a cursor.
        return emitError(loc) << "unsupported: the address of a scalar "
                                 "object cannot index an owner method";
      arguments[index + 1] = pointer->cursor;
      continue;
    }
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments[index + 1] = *value;
  }
  arguments[0] = builder
                     .create<emitrust::AddrOfOp>(loc, targetType.getInput(0),
                                                 ownerPlace, /*is_mut=*/true)
                     .getResult();

  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  callOp->setAttr(emitrust::kMethodCallAttrName, builder.getUnitAttr());
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

bool CImporter::involvesDecomposedPointer(const clang::Stmt *stmt) const {
  if (!stmt)
    return false;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (pointerLocals.contains(var))
        return true;
  for (const clang::Stmt *child : stmt->children())
    if (involvesDecomposedPointer(child))
      return true;
  return false;
}

FailureOr<Value> CImporter::emitCursorParamCall(const clang::CallExpr *call,
                                                func::FuncOp target,
                                                Location loc) {
  const clang::FunctionDecl *definition =
      call->getDirectCallee()->getDefinition();
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() != definition->getNumParams())
    return emitError(loc) << "unsupported: call argument count mismatch";
  // Formal-to-input slot mapping: a cursor parameter owns two inputs
  // (shared region slice, in-out i64 cursor).
  unsigned numParams = definition->getNumParams();
  SmallVector<unsigned, 4> slots(numParams, 0);
  unsigned slot = 0;
  for (unsigned index = 0; index < numParams; ++index) {
    slots[index] = slot;
    slot += cursorParams.contains(definition->getParamDecl(index)) ? 2 : 1;
  }
  if (slot != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";

  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  struct PendingCursor {
    unsigned index;
    const clang::VarDecl *pointer;
  };
  struct PendingBorrow {
    unsigned index;
    const clang::Expr *expr;
  };
  SmallVector<PendingCursor, 2> cursorArgs;
  SmallVector<PendingBorrow, 4> borrows;
  // Value arguments materialize first (C leaves evaluation order
  // unspecified); every borrow-producing argument follows, immediately
  // ahead of the call.
  for (unsigned index = 0; index < numParams; ++index) {
    const clang::Expr *argument = call->getArg(index);
    if (cursorParams.contains(definition->getParamDecl(index))) {
      // The argument must be `&p` over a decomposed pointer local — the
      // caller-side string cursor whose advancement the callee writes.
      const clang::Expr *stripped = stripTrivia(argument);
      while (const auto *cast =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(stripped))
        stripped = stripTrivia(cast->getSubExpr());
      const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(stripped);
      const clang::VarDecl *pointer =
          addrOf && addrOf->getOpcode() == clang::UO_AddrOf
              ? asLocalVarRef(addrOf->getSubExpr())
              : nullptr;
      if (!pointer || !pointerLocals.contains(pointer))
        return emitError(loc)
               << "unsupported: a string-cursor argument must be the "
                  "address of a decomposed pointer local";
      cursorArgs.push_back({index, pointer});
      continue;
    }
    Type input = targetType.getInput(slots[index]);
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      borrows.push_back({index, argument});
      continue;
    }
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments[slots[index]] = *value;
  }
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    FailureOr<Value> reference = emitBorrowArgument(
        loc, borrow.expr, targetType.getInput(slots[borrow.index]), root);
    if (failed(reference))
      return failure();
    arguments[slots[borrow.index]] = *reference;
  }
  struct StagedCursor {
    Value tmpPlace;
    Value cell;
  };
  SmallVector<StagedCursor, 2> stagedCursors;
  IntegerType i64Type = builder.getIntegerType(64);
  for (const PendingCursor &cursorArg : cursorArgs) {
    const PointerLocalInfo &info =
        pointerLocals.find(cursorArg.pointer)->second;
    if (!info.cursorCell || info.nonNullCell || info.baseIndexCell ||
        info.member)
      return emitError(loc)
             << "unsupported: a string-cursor argument must walk a single "
                "byte region";
    Value basePlace = info.literalBacking;
    if (!basePlace) {
      auto it = symbols.find(info.base);
      if (it == symbols.end())
        return emitError(loc)
               << "unsupported: pointer target '" << info.base->getName()
               << "' is not an importable place";
      basePlace = it->second;
    }
    auto lvalueType =
        llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
    Type elementType;
    if (lvalueType) {
      if (auto arrayType =
              llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
        elementType = arrayType.getElementType();
      else if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(
                   lvalueType.getValueType()))
        elementType = sliceType.getElementType();
    }
    if (elementType != builder.getIntegerType(8))
      return emitError(loc) << "unsupported: a string-cursor argument must "
                               "walk a byte region";
    unsigned slotIndex = slots[cursorArg.index];
    // Shared whole-region slice: the callee only reads bytes; the cursor
    // travels separately, so the slice starts at element zero and both
    // sides speak absolute positions.
    Value zero = createIntConstant(loc, i64Type, 0);
    arguments[slotIndex] =
        builder
            .create<emitrust::SliceOfOp>(loc, targetType.getInput(slotIndex),
                                         basePlace, zero, /*is_mut=*/false)
            .getResult();
    // The in-out cursor stages through a temp the callee writes; the
    // caller's cursor cell takes the advanced value back after the call.
    Value tmpPlace = createVariablePlace(loc, i64Type);
    Value current = loadPlace(loc, info.cursorCell);
    builder.create<emitrust::AssignOp>(loc, tmpPlace, current);
    arguments[slotIndex + 1] =
        builder
            .create<emitrust::AddrOfOp>(loc,
                                        targetType.getInput(slotIndex + 1),
                                        tmpPlace, /*is_mut=*/true)
            .getResult();
    stagedCursors.push_back({tmpPlace, info.cursorCell});
  }
  for (auto [index, value] : llvm::enumerate(arguments))
    if (!value || value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";
  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  for (const StagedCursor &staged : stagedCursors) {
    Value advanced = loadPlace(loc, staged.tmpPlace);
    builder.create<memref::StoreOp>(loc, advanced, staged.cell);
  }
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<Value> CImporter::emitBorrowArgument(Location loc,
                                               const clang::Expr *argument,
                                               Type paramType,
                                               const clang::VarDecl *&root) {
  root = nullptr;
  Type pointee;
  bool isMutParam = false;
  if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(paramType)) {
    pointee = mutRef.getPointee();
    isMutParam = true;
  } else if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(paramType)) {
    // Shared byte-slice parameters (`const unsigned char *`, CTS-BR
    // 00216) borrow their region base immutably.
    pointee = sharedRef.getPointee();
  } else {
    return emitError(loc) << "unsupported reference parameter type";
  }

  if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(pointee)) {
    // A string-literal argument (`f("abc")`) materializes a fresh mutable
    // backing byte array (bytes plus the terminating NUL) at the call
    // site and passes a whole-array slice of it. Per-call copies are
    // unobservable in defined C programs: identical literals may share
    // storage in C, but writing through a pointer to a string literal is
    // undefined behavior, so no defined program can distinguish a copy
    // from C's shared read-only storage.
    const clang::Expr *strippedArg = stripTrivia(argument);
    while (const auto *noop =
               llvm::dyn_cast<clang::ImplicitCastExpr>(strippedArg)) {
      if (noop->getCastKind() != clang::CK_NoOp)
        break;
      strippedArg = stripTrivia(noop->getSubExpr());
    }
    if (const auto *decay =
            llvm::dyn_cast<clang::ImplicitCastExpr>(strippedArg))
      if (decay->getCastKind() == clang::CK_ArrayToPointerDecay)
        if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
                stripTrivia(decay->getSubExpr()))) {
          if (sliceType.getElementType() != builder.getIntegerType(8))
            return emitError(loc)
                   << "unsupported: argument element type does not "
                      "match the slice parameter";
          FailureOr<Value> backing =
              createLiteralBacking(literal, loc, /*isConst=*/false);
          if (failed(backing))
            return failure();
          Value zero =
              createIntConstant(loc, builder.getIntegerType(64), 0);
          return builder
              .create<emitrust::SliceOfOp>(loc, paramType, *backing, zero,
                                           /*is_mut=*/isMutParam)
              .getResult();
        }
    // Slice parameter: reslice the argument's region base from its cursor.
    // A decayed array decomposes to cursor 0, `&arr[i]` to cursor i, a
    // walking pointer to its current cursor, and a slice parameter's own
    // read composes through the deref'd base place.
    FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
    if (failed(pointer))
      return failure();
    root = pointer->base;
    // A multi-base pointer has no single region base to reslice; the
    // callee would need the enum-of-bases discriminant, which a slice
    // parameter cannot carry (CTS-P7 scope).
    if (pointer->baseIndex)
      return emitError(loc) << "unsupported: passing a pointer bound to "
                               "multiple objects to a function";
    // Parameters have no null representation; passing a possibly-null
    // pointer would erase its discriminant (and the callee may be a
    // defined C program that null-checks it).
    if (pointer->nonNull)
      return emitError(loc) << "unsupported: possibly-null pointer passed "
                               "as a function argument";
    // A global region base would pass a borrow of the staged local copy,
    // not of the global itself: a callee that also touches the global
    // would see (or lose) the wrong values, so the shape is rejected
    // (with the cell-slice boundary wording when Pass A pinned one; the
    // all-global class itself lowers to a cell-slice and never gets
    // here). EXCEPT a byte-region aggregate global (CTS-BR, 00216),
    // which rides the ordinary staged-copy model: the borrow is of a
    // whole staged byte image, and a mutable parameter stores the image
    // back immediately after the call.
    if (pointer->base && !pointer->base->hasLocalStorage()) {
      if (!isByteRegionAggregate(pointer->base->getType()))
        return rejectGlobalPointerArgument(loc, pointer->base);
      FailureOr<std::pair<Value, std::string>> staged =
          stageGlobalCopy(loc, pointer->base);
      if (failed(staged))
        return failure();
      if (isMutParam)
        pendingStagedGlobalStores.push_back(*staged);
      Value cursor =
          pointer->cursor
              ? pointer->cursor
              : createIntConstant(loc, builder.getIntegerType(64), 0);
      return builder
          .create<emitrust::SliceOfOp>(loc, paramType, staged->first,
                                       cursor, isMutParam)
          .getResult();
    }
    if (!pointer->cursor)
      return emitError(loc) << "unsupported: the address of a scalar object "
                               "cannot be passed as a slice parameter";
    auto it = symbols.find(pointer->base);
    if (it == symbols.end())
      return emitError(loc) << "unsupported: pointer target '"
                            << pointer->base->getName()
                            << "' is not an importable place";
    Value basePlace = it->second;
    auto lvalueType =
        llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
    Type elementType;
    if (lvalueType) {
      if (auto arrayType =
              llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
        elementType = arrayType.getElementType();
      else if (auto baseSlice = llvm::dyn_cast<emitrust::SliceType>(
                   lvalueType.getValueType()))
        elementType = baseSlice.getElementType();
    }
    if (!elementType)
      return emitError(loc) << "unsupported pointer target place";
    if (elementType != sliceType.getElementType())
      return emitError(loc) << "unsupported: argument element type does not "
                               "match the slice parameter";
    return builder
        .create<emitrust::SliceOfOp>(loc, paramType, basePlace,
                                     pointer->cursor, /*is_mut=*/isMutParam)
        .getResult();
  }

  // C's enum/underlying-type pointer compatibility (C99 6.7.2.2p4): the
  // address of an enum object passed as a pointer to the enum's underlying
  // integer type arrives as an implicit BitCast around the address-of.
  // The borrow resolves to the enum place's raw-representation lvalue
  // (`emitrust.enum_raw`, rendered `.0` on the open-enum tuple struct),
  // whose value type is the enum's storage integer. Any other pointer
  // BitCast keeps its located rejection through emitPointerRValue below.
  if (const auto *bitcast =
          llvm::dyn_cast<clang::ImplicitCastExpr>(stripTrivia(argument));
      bitcast && bitcast->getCastKind() == clang::CK_BitCast) {
    const auto *innerAddrOf = llvm::dyn_cast<clang::UnaryOperator>(
        stripTrivia(bitcast->getSubExpr()));
    const auto *ref =
        innerAddrOf && innerAddrOf->getOpcode() == clang::UO_AddrOf
            ? llvm::dyn_cast<clang::DeclRefExpr>(
                  stripTrivia(innerAddrOf->getSubExpr()))
            : nullptr;
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    const clang::EnumDecl *enumDecl =
        var ? namedEnumDeclOf(var->getType()) : nullptr;
    if (enumDecl && bitcast->getType()->isPointerType() &&
        astContext().hasSameUnqualifiedType(
            bitcast->getType()->getPointeeType(),
            enumDecl->getIntegerType())) {
      auto it = symbols.find(var);
      if (it == symbols.end() ||
          !llvm::isa<emitrust::LValueType>(it->second.getType()))
        return emitError(loc) << "unsupported: enum object '"
                              << var->getName()
                              << "' is not an addressable place";
      FailureOr<Type> rawType = mapType(enumDecl->getIntegerType(), loc);
      if (failed(rawType))
        return failure();
      if (*rawType != pointee)
        return emitError(loc) << "unsupported: argument type does not match "
                                 "the pointer parameter";
      root = var;
      Value rawPlace = builder
                           .create<emitrust::EnumRawOp>(
                               loc, emitrust::LValueType::get(*rawType),
                               it->second)
                           .getResult();
      return builder
          .create<emitrust::AddrOfOp>(loc, paramType, rawPlace,
                                      /*is_mut=*/true)
          .getResult();
    }
  }

  // Scalar-reference parameter. Arguments involving a decomposed pointer
  // (or any non-address-of pointer expression, e.g. a decayed array) borrow
  // the designated element through the decomposition; plain address-of
  // arguments (`&x`, `&s.f`, `&arr[i]`) keep the historical
  // emitLValue+addr_of path.
  const clang::Expr *stripped = stripTrivia(argument);
  const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(stripped);
  bool isAddressOf = addrOf && addrOf->getOpcode() == clang::UO_AddrOf;
  if (!isAddressOf || involvesDecomposedPointer(argument)) {
    FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
    if (failed(pointer))
      return failure();
    root = pointer->base;
    // Parameters have no null representation; see the slice path above.
    if (pointer->nonNull)
      return emitError(loc) << "unsupported: possibly-null pointer passed "
                               "as a function argument";
    // A global region base would pass a borrow of the staged local copy
    // (writes through it would be lost); mirror the historical
    // address-of-a-global rejection (with the cell-slice boundary
    // wording when Pass A pinned one).
    if (pointer->base && !pointer->base->hasLocalStorage())
      return rejectGlobalPointerArgument(loc, pointer->base);
    FailureOr<Value> place = emitPointerPlace(loc, *pointer, pointee);
    if (failed(place))
      return failure();
    auto lvalueType = llvm::cast<emitrust::LValueType>((*place).getType());
    if (lvalueType.getValueType() != pointee)
      return emitError(loc) << "unsupported: argument type does not match "
                               "the pointer parameter";
    return builder
        .create<emitrust::AddrOfOp>(loc, paramType, *place, /*is_mut=*/true)
        .getResult();
  }
  root = addressArgumentRoot(argument);
  FailureOr<Value> value = emitRValue(argument);
  if (failed(value))
    return failure();
  return *value;
}


FailureOr<Value> CImporter::emitIndirectCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParens();
  // `(*fp)(...)`: the dereference of a function pointer designates the
  // function, which immediately decays back to the pointer value, so the
  // decay/deref pair cancels out and the pointer itself is evaluated.
  if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(calleeExpr))
    if (decay->getCastKind() == clang::CK_FunctionToPointerDecay) {
      const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(
          decay->getSubExpr()->IgnoreParens());
      if (deref && deref->getOpcode() == clang::UO_Deref &&
          isFunctionPointer(deref->getSubExpr()->getType()))
        calleeExpr = deref->getSubExpr()->IgnoreParens();
    }
  if (!isFunctionPointer(calleeExpr->getType()))
    return emitError(loc) << "unsupported: indirect function call";

  // A call with arguments through a prototype-less K&R pointer
  // (`int (*f)()`): a callee traceable to a callsite-inferred decl
  // (FR-29, CTS 00209) proceeds on the ordinary typed path below — the
  // decl's refined fn_ptr value type carries the inferred signature the
  // arguments are checked against. Any other callee (member, array
  // element, call result, uninferred decl) has no signature to check its
  // arguments against; the zero-argument form is the only one that can
  // be verified.
  const clang::Type *pointee = calleeExpr->getType()
                                   .getCanonicalType()
                                   ->getPointeeType()
                                   .getTypePtr();
  if (llvm::isa<clang::FunctionNoProtoType>(pointee) &&
      call->getNumArgs() > 0) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
        calleeExpr->IgnoreParenImpCasts());
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (!var || !inferredFnPtrSigs.contains(var))
      return emitError(loc)
             << "unsupported: call with arguments through a function "
                "pointer without a prototype";
  }

  // A call through a devirtualized alias (CTS-S, 00189) lowers as a
  // DIRECT call to the target: no fn_ptr value and no call_indirect.
  const clang::VarDecl *aliasVar = nullptr;
  if (const clang::FunctionDecl *target =
          devirtualizedCallee(call, &aliasVar))
    return emitDevirtualizedCall(call, aliasVar, target, loc);

  // A cast-call through an admitted local `void *` fn-ptr holder
  // (CTS-F, 00210) peels the cast entirely (clang already discarded any
  // attributes inside the spelled type): the holder's place carries the
  // target-signature fn_ptr value, so the callee is a plain load and no
  // cast op reaches the IR.
  Value holderValue;
  if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(calleeExpr)) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
        cast->getSubExpr()->IgnoreParenImpCasts());
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (var && voidFnPtrHolders.contains(var))
      holderValue = loadPlace(loc, symbols.lookup(var));
  }
  FailureOr<Value> fnValue =
      holderValue ? FailureOr<Value>(holderValue) : emitRValue(calleeExpr);
  if (failed(fnValue))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>((*fnValue).getType());
  if (!fnPtrType)
    return emitError(loc) << "unsupported: indirect function call";

  // Argument checking mirrors the direct-call path, each argument
  // positioned against the fn_ptr's input (see emitPositionedRValue).
  ArrayRef<Type> inputs = fnPtrType.getInputs();
  SmallVector<Value> arguments;
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    FailureOr<Value> value = emitPositionedRValue(
        index < inputs.size() ? inputs[index] : Type(), argument);
    if (failed(value))
      return failure();
    arguments.push_back(*value);
  }
  if (arguments.size() != inputs.size())
    return emitError(loc) << "unsupported: call argument count mismatch";
  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != inputs[index])
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<emitrust::CallIndirectOp>(
      loc, fnPtrType.getResults(), *fnValue, arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

const clang::FunctionDecl *
CImporter::devirtualizedCallee(const clang::CallExpr *call,
                               const clang::VarDecl **aliasVar) const {
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParens();
  // `(*fp)(...)`: the decay/deref pair cancels out (see emitIndirectCall).
  if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(calleeExpr))
    if (decay->getCastKind() == clang::CK_FunctionToPointerDecay) {
      const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(
          decay->getSubExpr()->IgnoreParens());
      if (deref && deref->getOpcode() == clang::UO_Deref &&
          isFunctionPointer(deref->getSubExpr()->getType()))
        calleeExpr = deref->getSubExpr()->IgnoreParens();
    }
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(calleeExpr->IgnoreParenImpCasts());
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var)
    return nullptr;
  const clang::FunctionDecl *target = fnPtrAliases.lookup(
      llvm::cast<clang::VarDecl>(var->getCanonicalDecl()));
  if (target && aliasVar)
    *aliasVar = var;
  return target;
}

FailureOr<Value>
CImporter::emitDevirtualizedCall(const clang::CallExpr *call,
                                 const clang::VarDecl *var,
                                 const clang::FunctionDecl *target,
                                 Location loc) {
  // A hosted-variadic alias is statement-position only (the printf
  // machinery has no result); reaching this value path is a rejection.
  if (target->isVariadic())
    return emitError(loc) << "unsupported: " << target->getName()
                          << " return value must be unused";
  FailureOr<Type> mapped = mapType(var->getType(), loc);
  if (failed(mapped))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*mapped);
  if (!fnPtrType)
    return emitError(loc) << "unsupported function pointer type";
  // The same import + signature check a `Some(target)` constant runs.
  FailureOr<std::string> name =
      resolveFunctionPointerDecl(target, fnPtrType, loc);
  if (failed(name))
    return failure();
  func::FuncOp targetOp = functions.lookup(*name);

  // Argument checking mirrors the indirect-call path.
  SmallVector<Value> arguments;
  for (const clang::Expr *argument : call->arguments()) {
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments.push_back(*value);
  }
  ArrayRef<Type> inputs = fnPtrType.getInputs();
  if (arguments.size() != inputs.size())
    return emitError(loc) << "unsupported: call argument count mismatch";
  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != inputs[index])
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, targetOp, arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<std::string>
CImporter::resolveFunctionPointerTarget(const clang::Expr *expr,
                                        emitrust::FnPtrType fnPtrType,
                                        Location loc) {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
  const auto *callee =
      ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
  if (!callee)
    return emitError(loc) << "unsupported: function pointer target is not a "
                             "direct function reference";
  return resolveFunctionPointerDecl(callee, fnPtrType, loc);
}

FailureOr<std::string>
CImporter::resolveFunctionPointerDecl(const clang::FunctionDecl *callee,
                                      emitrust::FnPtrType fnPtrType,
                                      Location loc) {
  // Variadic declarations (printf) are never imported, and a Rust `fn`
  // item cannot be variadic either.
  if (callee->isVariadic())
    return emitError(loc)
           << "unsupported: taking the address of a variadic function";
  std::string name = mlirFuncName(callee);
  func::FuncOp target = functions.lookup(name);
  if (!target) {
    if (isSystemHeaderDecl(callee))
      return rejectSystemHeaderUse(loc, "taking the address of",
                                   callee->getName());
    return emitError(loc)
           << "unsupported: taking the address of unimported function '"
           << name << "'";
  }
  // The imported function's MLIR signature must equal the fn_ptr's
  // component types exactly. This rejects prototype mismatches (including
  // a prototype-less `int (*)()` pointer bound to a function with
  // parameters) and functions whose data-pointer parameters import as
  // references, which no fn_ptr can carry.
  FunctionType targetType = target.getFunctionType();
  if (targetType.getInputs() != fnPtrType.getInputs() ||
      targetType.getResults() != fnPtrType.getResults())
    return emitError(loc)
           << "unsupported: function '" << name
           << "' does not match the function pointer signature";
  return name;
}

FailureOr<Value>
CImporter::emitFunctionPointerConstant(const clang::Expr *fnExpr,
                                       clang::QualType pointerType,
                                       Location loc) {
  FailureOr<Type> mapped = mapType(pointerType, loc);
  if (failed(mapped))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*mapped);
  if (!fnPtrType)
    return emitError(loc) << "unsupported function pointer type";
  FailureOr<std::string> name =
      resolveFunctionPointerTarget(fnExpr, fnPtrType, loc);
  if (failed(name))
    return failure();
  auto some = emitrust::OpaqueAttr::get(
      builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
  return builder.create<emitrust::ConstantOp>(loc, fnPtrType, some)
      .getResult();
}

FailureOr<Value>
CImporter::emitFunctionPointerConstant(const clang::FunctionDecl *target,
                                       emitrust::FnPtrType fnPtrType,
                                       Location loc) {
  FailureOr<std::string> name =
      resolveFunctionPointerDecl(target, fnPtrType, loc);
  if (failed(name))
    return failure();
  auto some = emitrust::OpaqueAttr::get(
      builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
  return builder.create<emitrust::ConstantOp>(loc, fnPtrType, some)
      .getResult();
}

FailureOr<Value> CImporter::emitPositionedRValue(Type expected,
                                                 const clang::Expr *expr) {
  auto fnPtrType = llvm::dyn_cast_if_present<emitrust::FnPtrType>(expected);
  if (!fnPtrType)
    return emitRValue(expr);
  clang::QualType type = expr->getType().getCanonicalType();
  if (!isFunctionPointer(type) ||
      !llvm::isa<clang::FunctionNoProtoType>(
          type->getPointeeType().getTypePtr()))
    return emitRValue(expr);
  // A prototype-less source binding into a known fn_ptr position: a
  // direct function reference resolves against the position's signature
  // (the refined type when the destination was callsite-inferred, FR-29 /
  // CTS 00209; the identical mapping otherwise), and a null constant is
  // the position's `None`. Anything else — a loaded fn-ptr VALUE keeps
  // its own (possibly unrefined) type, surfacing any mismatch at the
  // consuming check.
  Location loc = translateLoc(expr->getBeginLoc());
  if (const clang::Expr *fnExpr = returnedFunctionExpr(expr)) {
    const auto *target = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    return emitFunctionPointerConstant(target, fnPtrType, loc);
  }
  if (expr->isNullPointerConstant(astContext(),
                                  clang::Expr::NPC_ValueDependentIsNull))
    return createFnPtrNone(loc, fnPtrType);
  return emitRValue(expr);
}

Value CImporter::createFnPtrNone(Location loc, Type fnPtrType) {
  auto none = emitrust::OpaqueAttr::get(builder.getContext(), "None");
  return builder.create<emitrust::ConstantOp>(loc, fnPtrType, none)
      .getResult();
}

FailureOr<Value> CImporter::emitEnumConstant(
    const clang::EnumConstantDecl *enumerator, Location loc) {
  const auto *parent =
      llvm::cast<clang::EnumDecl>(enumerator->getDeclContext());
  const clang::EnumDecl *definition = parent->getDefinition();
  if (!definition)
    return emitError(loc) << "unsupported: enumerator of an incomplete enum";
  if (definition->getName().empty()) {
    // Anonymous enums have no Rust counterpart; the enumerator is a plain
    // `int` constant.
    const llvm::APSInt &initValue = enumerator->getInitVal();
    if (!initValue.isRepresentableByInt64() ||
        initValue.getExtValue() < INT32_MIN ||
        initValue.getExtValue() > INT32_MAX)
      return emitError(loc)
             << "unsupported: enumerator value does not fit in i32";
    return createIntConstant(loc, builder.getI32Type(),
                             initValue.getExtValue());
  }
  // Importing the definition validates the enumerator spellings and values
  // even when the enum type itself is never named.
  if (failed(importEnum(definition, loc)))
    return failure();
  std::string path =
      (llvm::Twine(definition->getName()) + "::" + enumerator->getName())
          .str();
  auto type =
      emitrust::EnumType::get(builder.getContext(), definition->getName());
  auto value = emitrust::OpaqueAttr::get(builder.getContext(), path);
  return builder.create<emitrust::ConstantOp>(loc, type, value).getResult();
}

FailureOr<Value> CImporter::emitEnumOperand(const EnumOperand &operand,
                                            Location loc) {
  if (operand.constant)
    return emitEnumConstant(operand.constant, loc);
  return emitRValue(operand.value);
}

Value CImporter::castEnumToI32(Location loc, Value value) {
  return builder.create<emitrust::CastOp>(loc, builder.getI32Type(), value)
      .getResult();
}

bool CImporter::isDecomposedPointerExpr(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_LValueToRValue)
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
              stripTrivia(cast->getSubExpr()))) {
        auto it = symbols.find(ref->getDecl());
        if (it != symbols.end() &&
            llvm::isa<emitrust::MutRefType, emitrust::RefType>(
                it->second.getType()))
          return false; // Pointer parameter: existing reference path.
      }
  // Every other pointer rvalue shape belongs to the decomposition, which
  // rejects the unsupported ones with located diagnostics.
  return true;
}

bool CImporter::isNullPointerConstantExpr(const clang::Expr *expr) const {
  if (expr->isNullPointerConstant(astContext(),
                                  clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return true;
  // `(const void *) 0`: only the UNQUALIFIED `void *` cast is a formal
  // null pointer constant (C11 6.3.2.3p3), but the qualified cast still
  // yields the null pointer value; clang models both as CK_NullToPointer.
  const auto *cast = llvm::dyn_cast<clang::CastExpr>(stripTrivia(expr));
  return cast && cast->getCastKind() == clang::CK_NullToPointer;
}

bool CImporter::isStaticallyNullPointerExpr(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const clang::Expr *sub = peelPointerCast(astContext(), e))
    e = stripTrivia(sub);
  const clang::VarDecl *var = asLoadedLocalVarRef(e);
  if (!var || !pointerRegions.tracks(var))
    return false;
  return isStaticallyNullRegion(pointerRegions.regionOf(var));
}

//===----------------------------------------------------------------------===//
// Cell-slice accesses (CTS-P10)
//===----------------------------------------------------------------------===//

std::optional<CellSliceAccess>
CImporter::matchCellSliceAccess(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  // Returns the parameter when `base` reads one bound to a
  // `!emitrust.ref<!emitrust.cell_slice<T>>` value.
  auto cellParamOf = [&](const clang::Expr *base) -> const clang::ParmVarDecl * {
    const clang::ParmVarDecl *param = asPointerParamRead(base);
    if (!param)
      return nullptr;
    auto it = symbols.find(param);
    if (it == symbols.end())
      return nullptr;
    auto refType = llvm::dyn_cast<emitrust::RefType>(it->second.getType());
    if (!refType || !llvm::isa<emitrust::CellSliceType>(refType.getPointee()))
      return nullptr;
    return param;
  };
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    if (const clang::ParmVarDecl *param = cellParamOf(subscript->getBase()))
      return CellSliceAccess{param, subscript->getIdx()};
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref)
      if (const clang::ParmVarDecl *param = cellParamOf(unary->getSubExpr()))
        return CellSliceAccess{param, nullptr};
  return std::nullopt;
}

FailureOr<Value> CImporter::emitCellSliceGet(const CellSliceAccess &access,
                                             Location loc) {
  Value ref = symbols.lookup(access.param);
  auto refType = llvm::cast<emitrust::RefType>(ref.getType());
  Type elementType =
      llvm::cast<emitrust::CellSliceType>(refType.getPointee())
          .getElementType();
  IntegerType cursorType = builder.getIntegerType(64);
  Value index;
  if (access.index) {
    FailureOr<Value> raw = emitRValue(access.index);
    if (failed(raw))
      return failure();
    auto indexType = llvm::dyn_cast<IntegerType>((*raw).getType());
    if (!indexType)
      return emitError(loc) << "unsupported subscript index type";
    index = castToIntType(loc, *raw, cursorType);
  } else {
    index = createIntConstant(loc, cursorType, 0);
  }
  return builder.create<emitrust::CellGetOp>(loc, elementType, ref, index)
      .getResult();
}

LogicalResult CImporter::emitCellSliceAssign(const CellSliceAccess &access,
                                             const clang::Expr *rhs,
                                             Location loc) {
  Value ref = symbols.lookup(access.param);
  auto refType = llvm::cast<emitrust::RefType>(ref.getType());
  Type elementType =
      llvm::cast<emitrust::CellSliceType>(refType.getPointee())
          .getElementType();
  IntegerType cursorType = builder.getIntegerType(64);
  Value index;
  if (access.index) {
    FailureOr<Value> raw = emitRValue(access.index);
    if (failed(raw))
      return failure();
    auto indexType = llvm::dyn_cast<IntegerType>((*raw).getType());
    if (!indexType)
      return emitError(loc) << "unsupported subscript index type";
    index = castToIntType(loc, *raw, cursorType);
  } else {
    index = createIntConstant(loc, cursorType, 0);
  }
  FailureOr<Value> value = emitRValue(rhs);
  if (failed(value))
    return failure();
  Value stored = *value;
  if (stored.getType() != elementType) {
    // C converts the assigned value to the element type (C99 6.5.16.1p2).
    FailureOr<Value> converted = convertScalarValue(loc, stored, elementType);
    if (failed(converted))
      return failure();
    stored = *converted;
  }
  builder.create<emitrust::CellSetOp>(loc, ref, index, stored);
  return success();
}

LogicalResult
CImporter::rejectGlobalPointerArgument(Location loc,
                                       const clang::VarDecl *base) {
  auto it = cellSliceRejects.find(base->getCanonicalDecl());
  if (it != cellSliceRejects.end()) {
    const CellSliceReject &reject = it->second;
    if (reject.kind == CellSliceReject::Kind::Mixed)
      return emitError(loc)
             << "unsupported: pointer parameter would join global '"
             << reject.globalName << "' and local object '"
             << reject.localName << "' into one region";
    return emitError(loc) << "unsupported: nullable pointer parameter "
                             "backed by a global variable";
  }
  return emitError(loc)
         << "unsupported: passing a pointer into a global variable to a "
            "function";
}

//===----------------------------------------------------------------------===//
// Byte puns over i8 regions (CTS-P11)
//===----------------------------------------------------------------------===//

std::optional<clang::QualType>
CImporter::pointerElementTypeFromAST(const clang::Expr *expr) const {
  const clang::Expr *e = stripObjectPointerCasts(astContext(), expr);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (binary->getOpcode() != clang::BO_Add &&
        binary->getOpcode() != clang::BO_Sub)
      return std::nullopt;
    const clang::Expr *pointerSide =
        isPointerType(binary->getLHS()->getType()) ? binary->getLHS()
                                                   : binary->getRHS();
    return pointerElementTypeFromAST(pointerSide);
  }
  const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
  if (!cast)
    return std::nullopt;
  if (cast->getCastKind() == clang::CK_ArrayToPointerDecay) {
    // A decayed (local or global) array: the innermost element is the
    // region's unit.
    clang::QualType arrayType = stripTrivia(cast->getSubExpr())->getType();
    if (!astContext().getAsConstantArrayType(arrayType))
      return std::nullopt;
    clang::QualType element = arrayType;
    while (const clang::ConstantArrayType *level =
               astContext().getAsConstantArrayType(element))
      element = level->getElementType();
    return element;
  }
  if (cast->getCastKind() != clang::CK_LValueToRValue)
    return std::nullopt;
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(cast->getSubExpr()));
  const auto *var =
      ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  if (!var)
    return std::nullopt;
  // A tracked pointer local (or slice-classified parameter) resolves
  // through its decomposition record; a data-pointer global through its
  // imported model.
  const clang::VarDecl *base = nullptr;
  auto localIt = pointerLocals.find(var);
  if (localIt != pointerLocals.end()) {
    if (localIt->second.literalBacking)
      return astContext().CharTy;
    base = localIt->second.base;
  } else {
    auto globalIt = pointerGlobals.find(var->getCanonicalDecl());
    if (globalIt != pointerGlobals.end())
      base = globalIt->second.base;
  }
  if (!base)
    return std::nullopt;
  if (isPointerType(base->getType()))
    return base->getType().getCanonicalType()->getPointeeType();
  clang::QualType element = base->getType();
  bool sawArray = false;
  while (const clang::ConstantArrayType *level =
             astContext().getAsConstantArrayType(element)) {
    element = level->getElementType();
    sawArray = true;
  }
  return sawArray ? std::optional<clang::QualType>(element) : std::nullopt;
}

CImporter::ByteViewDeref
CImporter::classifyByteViewDeref(const clang::Expr *expr) {
  ByteViewDeref view;
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(expr));
  if (!unary || unary->getOpcode() != clang::UO_Deref)
    return view;
  const clang::Expr *stripped =
      stripObjectPointerCasts(astContext(), unary->getSubExpr());
  if (!isDecomposedPointerExpr(stripped))
    return view;
  view.deref = unary;
  view.strippedPointer = stripped;
  // The single peel serves the changed-pointee test directly (the
  // historical `viewsChangedPointee` on the un-peeled sub-expression).
  view.reinterpreted = !astContext().hasSameUnqualifiedType(
      unary->getSubExpr()->getType().getCanonicalType()->getPointeeType(),
      stripped->getType().getCanonicalType()->getPointeeType());
  if (!view.reinterpreted)
    return view;
  clang::QualType viewed = unary->getType();
  if (!viewed->isIntegerType() || viewed->isBooleanType() ||
      viewed->isEnumeralType())
    return view;
  uint64_t viewedBits = astContext().getTypeSize(viewed);
  if (viewedBits != 16 && viewedBits != 32 && viewedBits != 64)
    return view;
  std::optional<clang::QualType> element =
      pointerElementTypeFromAST(stripped);
  if (!element || astContext().getTypeSize(*element) != 8 ||
      !(*element)->isIntegerType() || (*element)->isBooleanType() ||
      (*element)->isEnumeralType())
    return view;
  view.wideByte = true;
  return view;
}

/// Best-effort compile-time evaluation of an i64 cursor value: constants,
/// sums, differences, products, and sign/zero extensions fold; anything
/// else (a runtime offset) reports nothing, which skips the static bounds
/// check (an out-of-bounds runtime access panics in the generated Rust —
/// a legal refinement of C's undefined behavior).
static std::optional<int64_t> staticCursorValue(Value value) {
  llvm::APInt bits;
  if (matchPattern(value, m_ConstantInt(&bits)))
    return bits.getSExtValue();
  Operation *def = value.getDefiningOp();
  if (!def)
    return std::nullopt;
  if (auto add = llvm::dyn_cast<arith::AddIOp>(def)) {
    std::optional<int64_t> lhs = staticCursorValue(add.getLhs());
    std::optional<int64_t> rhs = staticCursorValue(add.getRhs());
    if (lhs && rhs)
      return *lhs + *rhs;
    return std::nullopt;
  }
  if (auto sub = llvm::dyn_cast<arith::SubIOp>(def)) {
    std::optional<int64_t> lhs = staticCursorValue(sub.getLhs());
    std::optional<int64_t> rhs = staticCursorValue(sub.getRhs());
    if (lhs && rhs)
      return *lhs - *rhs;
    return std::nullopt;
  }
  if (auto mul = llvm::dyn_cast<arith::MulIOp>(def)) {
    std::optional<int64_t> lhs = staticCursorValue(mul.getLhs());
    std::optional<int64_t> rhs = staticCursorValue(mul.getRhs());
    if (lhs && rhs)
      return *lhs * *rhs;
    return std::nullopt;
  }
  if (llvm::isa<arith::ExtSIOp, arith::ExtUIOp>(def))
    return staticCursorValue(def->getOperand(0));
  return std::nullopt;
}

FailureOr<CImporter::WideByteAccess>
CImporter::resolveWideByteAccess(const ByteViewDeref &view, Location loc,
                                 GlobalWriteback *writeback) {
  // The classification already peeled the reinterpreting casts once: the
  // decomposition itself only peels the qualification and void-wildcard
  // forms.
  FailureOr<PtrExprValue> pointer = emitPointerRValue(view.strippedPointer);
  if (failed(pointer))
    return failure();
  if (pointer->baseIndex || pointer->member)
    return emitError(loc)
           << "unsupported: wide byte access through this pointer";
  if (pointer->nonNull)
    return emitError(loc)
           << "unsupported: wide byte access through a possibly-null pointer";
  FailureOr<Type> viewedType = mapType(view.deref->getType(), loc);
  if (failed(viewedType))
    return failure();
  auto valueType = llvm::dyn_cast<IntegerType>(*viewedType);
  if (!valueType)
    return emitError(loc) << "unsupported: wide byte access value type";
  unsigned byteWidth = valueType.getWidth() / 8;

  Value basePlace;
  if (pointer->literalBacking) {
    if (writeback) // Defensive; the region analysis rejects literal writes.
      return emitError(loc)
             << "unsupported: write through a string-literal region";
    basePlace = pointer->literalBacking;
  } else if (pointer->base) {
    auto it = symbols.find(pointer->base);
    if (it != symbols.end()) {
      basePlace = it->second;
    } else {
      // A global byte region rides the ordinary staged-copy model: the
      // whole value is staged, punned, and (for writes) stored back.
      FailureOr<Value> staged =
          stageGlobalCopyAndRecord(loc, pointer->base, writeback);
      if (failed(staged))
        return failure();
      basePlace = *staged;
    }
  } else {
    return emitError(loc)
           << "unsupported: wide byte access through this pointer";
  }

  auto lvalueType = llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
  if (!lvalueType)
    return emitError(loc) << "unsupported: wide byte access base";
  Type byteType = builder.getIntegerType(8);
  uint64_t arrayBytes = 0;
  bool hasStaticSize = false;
  if (auto arrayType =
          llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType())) {
    if (arrayType.getElementType() != byteType)
      return emitError(loc) << "unsupported: wide byte access base";
    arrayBytes = arrayType.getSize();
    hasStaticSize = true;
  } else if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(
                 lvalueType.getValueType())) {
    if (sliceType.getElementType() != byteType)
      return emitError(loc) << "unsupported: wide byte access base";
  } else {
    return emitError(loc) << "unsupported: wide byte access base";
  }

  Value cursor =
      pointer->cursor ? pointer->cursor
                      : createIntConstant(loc, builder.getIntegerType(64), 0);
  // A compile-time-constant offset whose widened window overruns the
  // array is rejected here; runtime offsets are checked by the generated
  // Rust's slice bounds (a panic refines C's undefined behavior).
  if (hasStaticSize) {
    if (std::optional<int64_t> offset = staticCursorValue(cursor)) {
      if (*offset < 0 ||
          static_cast<uint64_t>(*offset) + byteWidth > arrayBytes)
        return emitError(loc)
               << "unsupported: " << byteWidth << "-byte access at offset "
               << *offset << " runs past the end of '"
               << pointer->base->getName() << "' (" << arrayBytes
               << " bytes)";
    }
  }
  return WideByteAccess{basePlace, cursor, valueType, byteWidth};
}

/// Returns the Rust integer type name (`u32`, `i64`, ...) the ne_bytes
/// helper is called on for `type`.
static std::string neBytesTypeName(IntegerType type) {
  return ((type.isUnsigned() ? llvm::Twine("u") : llvm::Twine("i")) +
          llvm::Twine(type.getWidth()))
      .str();
}

FailureOr<Value> CImporter::emitWideByteLoad(const WideByteAccess &access,
                                             Location loc) {
  auto u8Type = IntegerType::get(builder.getContext(), 8,
                                 IntegerType::Unsigned);
  IntegerType byteType = builder.getIntegerType(8);
  IntegerType cursorType = builder.getIntegerType(64);
  auto bytesType = emitrust::ArrayType::get(builder.getContext(),
                                            access.byteWidth, u8Type);
  // Gather the window's bytes (as u8) into a byte-array temporary...
  Value bytesVar =
      builder
          .create<emitrust::VariableOp>(loc,
                                        emitrust::LValueType::get(bytesType))
          .getResult();
  for (unsigned k = 0; k != access.byteWidth; ++k) {
    Value kValue = createIntConstant(loc, cursorType, k);
    Value sourceIndex =
        k == 0 ? access.cursor
               : builder.create<arith::AddIOp>(loc, access.cursor, kValue)
                     .getResult();
    Value element = builder
                        .create<emitrust::SubscriptOp>(
                            loc, emitrust::LValueType::get(byteType),
                            access.basePlace, sourceIndex)
                        .getResult();
    Value byte =
        builder.create<emitrust::LoadOp>(loc, byteType, element).getResult();
    Value unsignedByte =
        builder.create<emitrust::CastOp>(loc, u8Type, byte).getResult();
    Value slot = builder
                     .create<emitrust::SubscriptOp>(
                         loc, emitrust::LValueType::get(u8Type), bytesVar,
                         kValue)
                     .getResult();
    builder.create<emitrust::AssignOp>(loc, slot, unsignedByte);
  }
  // ... and combine them with T::from_ne_bytes.
  Value bytes =
      builder.create<emitrust::LoadOp>(loc, bytesType, bytesVar).getResult();
  std::string callee = neBytesTypeName(access.valueType) + "::from_ne_bytes";
  return builder
      .create<emitrust::CallOpaqueOp>(loc, TypeRange{access.valueType},
                                      builder.getStringAttr(callee),
                                      /*args=*/ArrayAttr(), ValueRange{bytes})
      .getResult(0);
}

LogicalResult CImporter::emitWideByteStore(const WideByteAccess &access,
                                           Value value, Location loc) {
  auto u8Type = IntegerType::get(builder.getContext(), 8,
                                 IntegerType::Unsigned);
  IntegerType byteType = builder.getIntegerType(8);
  IntegerType cursorType = builder.getIntegerType(64);
  auto bytesType = emitrust::ArrayType::get(builder.getContext(),
                                            access.byteWidth, u8Type);
  // Split the value with T::to_ne_bytes...
  std::string callee = neBytesTypeName(access.valueType) + "::to_ne_bytes";
  Value bytes = builder
                    .create<emitrust::CallOpaqueOp>(
                        loc, TypeRange{bytesType},
                        builder.getStringAttr(callee),
                        /*args=*/ArrayAttr(), ValueRange{value})
                    .getResult(0);
  Value bytesVar =
      builder
          .create<emitrust::VariableOp>(loc,
                                        emitrust::LValueType::get(bytesType))
          .getResult();
  builder.create<emitrust::AssignOp>(loc, bytesVar, bytes);
  // ... and scatter them (as i8) back over the window.
  for (unsigned k = 0; k != access.byteWidth; ++k) {
    Value kValue = createIntConstant(loc, cursorType, k);
    Value slot = builder
                     .create<emitrust::SubscriptOp>(
                         loc, emitrust::LValueType::get(u8Type), bytesVar,
                         kValue)
                     .getResult();
    Value byte =
        builder.create<emitrust::LoadOp>(loc, u8Type, slot).getResult();
    Value signedByte =
        builder.create<emitrust::CastOp>(loc, byteType, byte).getResult();
    Value targetIndex =
        k == 0 ? access.cursor
               : builder.create<arith::AddIOp>(loc, access.cursor, kValue)
                     .getResult();
    Value element = builder
                        .create<emitrust::SubscriptOp>(
                            loc, emitrust::LValueType::get(byteType),
                            access.basePlace, targetIndex)
                        .getResult();
    builder.create<emitrust::AssignOp>(loc, element, signedByte);
  }
  return success();
}

std::optional<clang::QualType>
CImporter::regionElementType(const PtrExprValue &pointer,
                             clang::QualType viewed) {
  const clang::VarDecl *var = pointer.base;
  const clang::FieldDecl *member = pointer.member;
  if (!var && !pointer.multiBases.empty()) {
    // The multi-base validation admits one uniform element type across
    // the closed set of bases, so the first base is representative.
    var = pointer.multiBases.front().var;
    member = pointer.multiBases.front().member;
  }
  if (member)
    return member->getType();
  if (var) {
    clang::QualType baseType = var->getType();
    if (isPointerType(baseType)) // Slice-classified parameter base.
      return baseType.getCanonicalType()->getPointeeType();
    // Prefer the array level matching the viewed type (a row pointer);
    // otherwise the innermost element is the region's element unit.
    clang::QualType element = baseType;
    while (const clang::ConstantArrayType *level =
               astContext().getAsConstantArrayType(element)) {
      element = level->getElementType();
      if (astContext().hasSameUnqualifiedType(viewed, element))
        return element;
    }
    return element;
  }
  if (pointer.literalBacking) // String-literal regions are byte runs.
    return astContext().CharTy;
  return std::nullopt; // Base-less: the deref rejects downstream.
}

Value CImporter::lookupCarrierCell(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        cast->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  if (!ref)
    return Value();
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var)
    return Value();
  if (Value cell = carrierLocals.lookup(var))
    return cell;
  if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(var))
    if (carrierParams.contains(param))
      return symbols.lookup(param);
  return Value();
}

FailureOr<Value> CImporter::emitCarrierValue(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  Location loc = translateLoc(e->getBeginLoc());
  IntegerType carrierType = builder.getIntegerType(64);
  // The null pointer constant is the carrier zero.
  if (isNullPointerConstantExpr(e))
    return createIntConstant(loc, carrierType, 0);
  if (Value cell = lookupCarrierCell(e))
    return loadPlace(loc, cell);
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    if (cast->getCastKind() == clang::CK_NoOp)
      return emitCarrierValue(cast->getSubExpr());
    if (cast->getCastKind() == clang::CK_IntegralToPointer) {
      // `(void *)v`: the integer rides along unchanged, converted to the
      // i64 carrier width (an `emitrust.cast` for unsigned sources). Only
      // pointer-width sources classify as carriers (a truncated address
      // could never round-trip), mirroring the region analysis gate.
      if (astContext().getTypeSize(cast->getSubExpr()->getType()) != 64)
        return emitError(loc)
               << "unsupported: pointer assigned a non-address value";
      FailureOr<Value> value = emitRValue(cast->getSubExpr());
      if (failed(value))
        return failure();
      if (!llvm::isa<IntegerType>((*value).getType()))
        return emitError(loc)
               << "unsupported: pointer assigned a non-address value";
      return castToIntType(loc, *value, carrierType);
    }
  }
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
    FailureOr<Value> result = emitCall(call);
    if (failed(result))
      return failure();
    if (*result && (*result).getType() == carrierType)
      return result;
    return emitError(loc)
           << "unsupported: pointer assigned a non-address value";
  }
  return emitError(loc) << "unsupported: pointer assigned a non-address value";
}

FailureOr<Value> CImporter::emitPointerTruth(const clang::Expr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  if (isNullPointerConstantExpr(expr)) // `if (NULL)` is constant false.
    return createBoolConstant(loc, false);
  // An integer-carrier pointer (CTS-P3) is a plain i64; its truth test is
  // an integer comparison against zero.
  if (Value cell = lookupCarrierCell(expr)) {
    Value value = loadPlace(loc, cell);
    Value zero = createIntConstant(loc, builder.getIntegerType(64), 0);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, value, zero)
        .getResult();
  }
  FailureOr<PtrExprValue> pointer = emitPointerRValue(expr);
  if (failed(pointer))
    return failure();
  // A pointer of a nullable region tests its Option-of-cursor
  // discriminant; a statically non-null pointer folds to true (every
  // address a decomposed region holds designates a live object), and a
  // statically-null pointer (base-less nullable region, CTS-P9 — the
  // empty decomposition) folds to false.
  if (pointer->nonNull)
    return pointer->nonNull;
  if (!pointer->base && !pointer->literalBacking && !pointer->baseIndex)
    return createBoolConstant(loc, false);
  return createBoolConstant(loc, true);
}

const clang::VarDecl *
CImporter::secondOrderDerefVar(const clang::Expr *expr) const {
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(expr));
  if (!unary || unary->getOpcode() != clang::UO_Deref)
    return nullptr;
  // `*(int **)pp` on a `void **` peels the reinterpret-back cast: the
  // pointee-wildcard rule applies at any pointer depth (CTS-P9), and the
  // selected first-order pointer's own region carries the element type
  // the final dereference checks against.
  const clang::Expr *sub = stripTrivia(unary->getSubExpr());
  while (const clang::Expr *peeled = peelPointerCast(astContext(), sub))
    sub = stripTrivia(peeled);
  const clang::VarDecl *var = asLoadedLocalVarRef(sub);
  return var && pointerRegions.tracksSecondOrder(var) ? var : nullptr;
}

FailureOr<PtrExprValue>
CImporter::emitPointerLocalRead(Location loc, const clang::VarDecl *var) {
  auto it = pointerLocals.find(var);
  if (it == pointerLocals.end()) {
    // An integer-carrier pointer (CTS-P3) has no (base, cursor)
    // decomposition at all: it is an integer in pointer clothing. Its
    // modeled consumers (truth tests, carrier assignments, returns, and
    // carrier call arguments) intercept it before this point, so reaching
    // here means the carrier is used AS a pointer — a dereference, which
    // would read from a fabricated address.
    if (var->hasLocalStorage() &&
        isCarrierRegion(pointerRegions.regionOf(var)))
      return emitError(loc)
             << "unsupported: dereference of an integer-carrier pointer";
    // A statically-null pointer (base-less nullable region, CTS-P9)
    // carries zero runtime state; its value is the empty decomposition,
    // which truth tests, null comparisons, and pointer-to-int casts fold
    // and which any dereference rejects as only-ever-null.
    if (var->hasLocalStorage() &&
        isStaticallyNullRegion(pointerRegions.regionOf(var)))
      return PtrExprValue{};
    return emitError(loc) << "unsupported: pointer variable '"
                          << var->getName()
                          << "' has no known target object";
  }
  const PointerLocalInfo &info = it->second;
  Value cursor;
  if (info.cursorCell)
    cursor = loadPlace(loc, info.cursorCell);
  Value nonNull;
  if (info.nonNullCell)
    nonNull = loadPlace(loc, info.nonNullCell);
  // A multi-base pointer additionally carries its enum-of-bases
  // discriminant (CTS-P7).
  Value baseIndex;
  if (info.baseIndexCell)
    baseIndex = loadPlace(loc, info.baseIndexCell);
  PtrExprValue value{info.base,   cursor,    info.literalBacking,
                     nonNull,     baseIndex, info.multiBases};
  value.member = info.member;
  return value;
}

FailureOr<PtrExprValue>
CImporter::emitPointerRValue(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  Location loc = translateLoc(e->getBeginLoc());
  IntegerType cursorType = builder.getIntegerType(64);

  // A null pointer constant has no (base, cursor) decomposition. Its
  // modeled consumers — pointer assignment (None side of the
  // Option-of-cursor model), equality comparison, and truth test —
  // intercept it before this point; any other context (call arguments,
  // pointer arithmetic, ...) stays a located rejection (CTS-P8 scope).
  if (isNullPointerConstantExpr(e))
    return emitError(loc)
           << "unsupported: null pointer constant in a pointer expression";

  // A decomposition-transparent pointer cast — a qualification adjustment
  // (`(int *)p` on an `int *` decomposition) or a `void *`-mediated cast
  // (the pointee-wildcard rule, CTS-P9) — changes nothing the
  // decomposition tracks; peel it. Whether a reinterpret-back site
  // `*(T *)p` type-checks against the region's base element type is the
  // deref emission's job. Genuinely reinterpreting casts fall through to
  // the located rejection below.
  if (const clang::Expr *peeled = peelPointerCast(astContext(), e))
    return emitPointerRValue(peeled);

  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    switch (cast->getCastKind()) {
    case clang::CK_NoOp:
      return emitPointerRValue(cast->getSubExpr());
    case clang::CK_LValueToRValue: {
      // A read of a data-pointer struct member resolves through its
      // static per-instance binding (CTS-P2): the member designates one
      // whole object, so the value is the degenerate (cursor-less) form
      // of that object — no runtime state is read at all. A
      // literal-bound member stays write-only.
      if (const clang::FieldDecl *field =
              dataPointerFieldOf(cast->getSubExpr())) {
        const auto *member =
            llvm::cast<clang::MemberExpr>(stripTrivia(cast->getSubExpr()));
        FailureOr<const MemberPointerFacts *> binding =
            resolveMemberPointerBinding(member, loc);
        if (failed(binding))
          return failure();
        if ((*binding)->literal)
          return emitError(loc)
                 << "unsupported: pointer struct member '"
                 << field->getName() << "' bound to a string literal";
        return PtrExprValue{(*binding)->base, Value()};
      }
      // A read of a pointer local: its base is static, its cursor is the
      // current value of the cursor cell (none for a degenerate base).
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub);
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var) {
        // `*s` on a string-cursor parameter (CTS 00204): the read is the
        // (region base, current cursor) decomposition the prologue bound
        // for the parameter.
        if (const clang::ParmVarDecl *cursorParam =
                asPointerPointerParamDeref(sub);
            cursorParam && pointerLocals.contains(cursorParam))
          return emitPointerLocalRead(loc, cursorParam);
        // `*pp`: reading the first-order pointer a second-order pointer
        // selects is exactly a read of that pointer — the selection is
        // static under the degenerate one-cell region (CTS-P5).
        if (const clang::VarDecl *pp = secondOrderDerefVar(sub)) {
          auto target = pointerPointerLocals.find(pp);
          if (target == pointerPointerLocals.end())
            return emitError(loc)
                   << "unsupported: pointer-to-pointer variable '"
                   << pp->getName() << "' has no bound pointer variable";
          return emitPointerLocalRead(loc, target->second);
        }
        break;
      }
      // A second-order pointer has no first-order decomposition; its only
      // modeled uses are its (static) rebinding and its dereferences.
      if (pointerPointerLocals.contains(var) ||
          pointerRegions.tracksSecondOrder(var))
        return emitError(loc)
               << "unsupported use of pointer-to-pointer variable '"
               << var->getName() << "'";
      // A statically-null pointer (CTS-P9) has no pointerLocals entry at
      // all; emitPointerLocalRead folds it to the empty decomposition.
      if (pointerLocals.contains(var) ||
          (var->hasLocalStorage() && pointerRegions.tracks(var)))
        return emitPointerLocalRead(loc, var);
      // A read of a pointer-typed global (CTS-P4): its base is static and
      // its cursor is the current value of the cursor global (none for a
      // degenerate base).
      auto globalIt = pointerGlobals.find(var->getCanonicalDecl());
      if (globalIt != pointerGlobals.end()) {
        const PointerGlobalInfo &info = globalIt->second;
        Value cursor;
        if (!info.cursorSymbol.empty())
          cursor = builder
                       .create<emitrust::GlobalLoadOp>(
                           loc, cursorType, globalSymbol(info.cursorSymbol))
                       .getResult();
        return PtrExprValue{info.base, cursor};
      }
      if (llvm::isa<clang::ParmVarDecl>(var))
        return emitError(loc) << "unsupported: pointer parameter used "
                                 "outside a direct dereference";
      return emitError(loc) << "unsupported: pointer variable '"
                            << var->getName()
                            << "' has no known target object";
    }
    case clang::CK_ArrayToPointerDecay: {
      // A decayed array is its own base at cursor 0; a decayed string
      // literal — or `__func__`, whose function-name literal is the same
      // shape (C99-29) — is its read-only backing array at cursor 0 (the
      // backing was created at the declaration of the pointer bound to
      // it); a decayed row of a multi-dimensional array (`arr[i]` in
      // `arr[i][j]` or `q = arr[i]`) decomposes the subscript into the
      // base's flat cursor.
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (const clang::StringLiteral *literal = underlyingStringLiteral(sub)) {
        // The backing is created on demand (C99-28): an anonymous decayed
        // literal (`*("abc" + i)`, `&"abc"[i]`) is the same read-only
        // region shape as a literal bound to a pointer variable, whose
        // declaration created the backing ahead of this walk.
        FailureOr<Value> backing = getOrCreateLiteralBacking(literal, loc);
        if (failed(backing))
          return failure();
        return PtrExprValue{nullptr, createIntConstant(loc, cursorType, 0),
                            *backing};
      }
      // A decayed block-scope compound literal (C99-13) materializes its
      // anonymous temp here — the evaluation point, which is where C
      // starts the object's lifetime — and decomposes to (backing,
      // cursor 0) like a decayed named array.
      if (const auto *compound =
              llvm::dyn_cast<clang::CompoundLiteralExpr>(sub)) {
        FailureOr<const clang::VarDecl *> backing =
            materializeCompoundLiteralBase(compound);
        if (failed(backing))
          return failure();
        return PtrExprValue{*backing, createIntConstant(loc, cursorType, 0)};
      }
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(sub))
        return emitSubscriptPointer(subscript);
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub);
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var)
        break;
      // A decayed global array is a global region base (CTS-P4/P6); its
      // element accesses stage the global like any direct element access.
      // The canonical declaration keys every consumer (pointer-local
      // bindings, global pointer assignments, and direct dereference
      // forms), matching the analysis's canonicalized base records.
      if (!var->hasLocalStorage())
        var = var->getCanonicalDecl();
      return PtrExprValue{var, createIntConstant(loc, cursorType, 0)};
    }
    default:
      break;
    }
    return emitError(loc) << "unsupported pointer cast ("
                          << cast->getCastKindName() << ")";
  }

  // CTS-BR (00216): `(u8 *)&x` / `(u8 *)p` over a byte-region aggregate
  // is the region base itself — a byte view of a padding-free all-u8
  // object is the object. A byte view of any OTHER aggregate would
  // expose object representation the typed model never materializes;
  // that cast is the located rejection authored by the byte-region
  // contract.
  if (const auto *cstyle = llvm::dyn_cast<clang::CStyleCastExpr>(e)) {
    clang::QualType destType = cstyle->getType().getCanonicalType();
    if (destType->isPointerType() &&
        isU8ScalarType(destType->getPointeeType())) {
      clang::QualType srcType =
          cstyle->getSubExpr()->getType().getCanonicalType();
      if (srcType->isPointerType()) {
        clang::QualType pointee = srcType->getPointeeType();
        if (isByteRegionAggregate(pointee))
          return emitPointerRValue(cstyle->getSubExpr());
        if (pointee.getCanonicalType()->getAsRecordDecl())
          return emitError(loc) << "unsupported: byte view of an aggregate "
                                   "with non-byte members";
      }
    }
  }

  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
    if (unary->getOpcode() == clang::UO_AddrOf) {
      const clang::Expr *sub = stripTrivia(unary->getSubExpr());
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
        const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
        if (!var)
          return emitError(loc) << "unsupported pointer target";
        if (isPointerType(var->getType()))
          return emitError(loc)
                 << "unsupported: taking the address of a pointer variable";
        // CTS-BR (00216): the whole-object address of a byte-region
        // aggregate is the region base at byte cursor 0, sliceable like
        // a decayed byte array.
        if (isByteRegionAggregate(var->getType())) {
          if (!var->hasLocalStorage())
            var = var->getCanonicalDecl();
          return PtrExprValue{var, createIntConstant(loc, cursorType, 0)};
        }
        // `&x`: the degenerate (cursor-less) form of the scalar or struct
        // object itself. A global object is a global region base
        // (CTS-P4), staged at each access.
        if (!var->hasLocalStorage())
          var = var->getCanonicalDecl();
        return PtrExprValue{var, Value()};
      }
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(sub)) {
        // `&arr[i]` (via the decay of `arr`), `&q[i]` (i.e. `q + i`), and
        // nested `&arr[i][j]` all decompose the subscript base and offset
        // it by the (row-scaled) index; a literal-backed base keeps its
        // read-only backing through the decomposition.
        return emitSubscriptPointer(subscript);
      }
      if (const auto *memberExpr = llvm::dyn_cast<clang::MemberExpr>(sub)) {
        // CTS-BR (00216): `&x.member` inside a byte-region aggregate is
        // the region base at the member's constant byte offset — union
        // arms included (every arm is a view of the one region).
        if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(
                memberExpr->getMemberDecl());
            field && isByteRegionRecord(field->getParent())) {
          int64_t offset = 0;
          const clang::Expr *baseExpr = memberExpr;
          bool supported = true;
          while (const auto *m = llvm::dyn_cast<clang::MemberExpr>(baseExpr)) {
            const auto *walkField =
                llvm::dyn_cast<clang::FieldDecl>(m->getMemberDecl());
            if (!walkField || m->isArrow()) {
              supported = false;
              break;
            }
            if (failed(checkSpecialArrayMemberAccess(walkField, loc)))
              return failure();
            offset += static_cast<int64_t>(
                astContext()
                    .getASTRecordLayout(walkField->getParent())
                    .getFieldOffset(walkField->getFieldIndex()) /
                8);
            baseExpr = m->getBase()->IgnoreParenImpCasts();
          }
          const auto *rootRef =
              supported ? llvm::dyn_cast<clang::DeclRefExpr>(baseExpr)
                        : nullptr;
          const auto *rootVar =
              rootRef ? llvm::dyn_cast<clang::VarDecl>(rootRef->getDecl())
                      : nullptr;
          if (rootVar && isByteRegionAggregate(rootVar->getType())) {
            if (!rootVar->hasLocalStorage())
              rootVar = rootVar->getCanonicalDecl();
            return PtrExprValue{rootVar,
                                createIntConstant(loc, cursorType, offset)};
          }
        }
        // `&s.b` / `&g.b`: the degenerate (cursor-less) member-rooted form
        // of the struct object (CTS-P9). Union storage has no unaliased
        // member place to root a region at.
        MemberAddressTarget target = classifyMemberAddress(memberExpr);
        if (target.touchesUnion)
          return emitError(loc)
                 << "unsupported: taking the address of a union member";
        if (target.root) {
          const clang::VarDecl *root = target.root;
          if (!root->hasLocalStorage())
            root = root->getCanonicalDecl();
          PtrExprValue value{root, Value()};
          value.member = target.field;
          return value;
        }
      }
      // `&(struct S){...}` (C99-13): the degenerate (cursor-less) form of
      // the literal's freshly materialized anonymous temp.
      if (const auto *compound =
              llvm::dyn_cast<clang::CompoundLiteralExpr>(sub)) {
        FailureOr<const clang::VarDecl *> backing =
            materializeCompoundLiteralBase(compound);
        if (failed(backing))
          return failure();
        return PtrExprValue{*backing, Value()};
      }
      return emitError(loc) << "unsupported pointer target expression";
    }
    if (unary->isIncrementDecrementOp()) {
      // `p++` / `--p` in pointer-value position: update the cursor cell and
      // yield the pre-value (postfix) or post-value (prefix) per C. Both
      // pointer locals and slice parameters carry cursor cells. Walking a
      // pointer to a whole row would need a row-scaled step (CTS-P scope).
      if (pointsToArray(unary->getSubExpr()->getType()))
        return emitError(loc)
               << "unsupported: arithmetic on a pointer to an array";
      const clang::VarDecl *var = asVarRef(unary->getSubExpr());
      if (!var)
        var = asGlobalDataPointerRef(unary->getSubExpr());
      auto it = var ? pointerLocals.find(var) : pointerLocals.end();
      if (it == pointerLocals.end()) {
        // `g++` on a pointer-typed global walks its stored i64 cursor
        // global (CTS-P4).
        auto globalIt = var ? pointerGlobals.find(var->getCanonicalDecl())
                            : pointerGlobals.end();
        if (globalIt == pointerGlobals.end())
          return emitError(loc)
                 << "unsupported: ++/-- on this pointer expression";
        const PointerGlobalInfo &info = globalIt->second;
        if (info.cursorSymbol.empty())
          return emitError(loc) << "unsupported: arithmetic on the address "
                                   "of a scalar object";
        Value current = builder
                            .create<emitrust::GlobalLoadOp>(
                                loc, cursorType,
                                globalSymbol(info.cursorSymbol))
                            .getResult();
        Value one = createIntConstant(loc, cursorType, 1);
        Value next =
            unary->isIncrementOp()
                ? builder.create<arith::AddIOp>(loc, current, one).getResult()
                : builder.create<arith::SubIOp>(loc, current, one)
                      .getResult();
        builder.create<emitrust::GlobalStoreOp>(
            loc, next, globalSymbol(info.cursorSymbol));
        return PtrExprValue{info.base, unary->isPostfix() ? current : next};
      }
      const PointerLocalInfo &info = it->second;
      if (!info.cursorCell) // Defensive; rejected at the declaration.
        return emitError(loc)
               << "unsupported: arithmetic on the address of a scalar object";
      Value current = loadPlace(loc, info.cursorCell);
      Value one = createIntConstant(loc, cursorType, 1);
      Value next =
          unary->isIncrementOp()
              ? builder.create<arith::AddIOp>(loc, current, one).getResult()
              : builder.create<arith::SubIOp>(loc, current, one).getResult();
      builder.create<memref::StoreOp>(loc, next, info.cursorCell);
      Value nonNull;
      if (info.nonNullCell)
        nonNull = loadPlace(loc, info.nonNullCell);
      Value baseIndex;
      if (info.baseIndexCell)
        baseIndex = loadPlace(loc, info.baseIndexCell);
      return PtrExprValue{info.base, unary->isPostfix() ? current : next,
                          info.literalBacking, nonNull, baseIndex,
                          info.multiBases};
    }
    return emitError(loc) << "unsupported pointer expression";
  }

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    clang::BinaryOperatorKind opcode = binary->getOpcode();
    if (opcode == clang::BO_Add || opcode == clang::BO_Sub) {
      // `p + n` / `p - n` / `n + p`: cursor arithmetic. The operands are
      // emitted in source order (C leaves the order unspecified). Walking
      // a pointer to a whole row would need a row-scaled step (CTS-P
      // scope).
      bool lhsIsPointer = isPointerType(binary->getLHS()->getType());
      const clang::Expr *pointerSide =
          lhsIsPointer ? binary->getLHS() : binary->getRHS();
      if (pointsToArray(pointerSide->getType()))
        return emitError(loc)
               << "unsupported: arithmetic on a pointer to an array";
      FailureOr<PtrExprValue> pointer;
      FailureOr<Value> amount;
      if (lhsIsPointer) {
        pointer = emitPointerRValue(binary->getLHS());
        if (failed(pointer))
          return failure();
        amount = emitRValue(binary->getRHS());
      } else {
        amount = emitRValue(binary->getLHS());
        if (failed(amount))
          return failure();
        pointer = emitPointerRValue(binary->getRHS());
        if (failed(pointer))
          return failure();
      }
      if (failed(amount))
        return failure();
      if (!llvm::isa<IntegerType>((*amount).getType()))
        return emitError(loc) << "unsupported pointer offset type";
      if (!pointer->cursor)
        return emitError(loc)
               << "unsupported: arithmetic on the address of a scalar object";
      Value offset = castToIntType(loc, *amount, cursorType);
      Value cursor =
          opcode == clang::BO_Add
              ? builder.create<arith::AddIOp>(loc, pointer->cursor, offset)
                    .getResult()
              : builder.create<arith::SubIOp>(loc, pointer->cursor, offset)
                    .getResult();
      return PtrExprValue{pointer->base, cursor, pointer->literalBacking,
                          pointer->nonNull, pointer->baseIndex,
                          pointer->multiBases};
    }
  }

  return emitError(loc) << "unsupported pointer expression: "
                        << e->getStmtClassName();
}

FailureOr<PtrExprValue>
CImporter::emitSubscriptPointer(const clang::ArraySubscriptExpr *subscript) {
  Location loc = translateLoc(subscript->getBeginLoc());
  IntegerType cursorType = builder.getIntegerType(64);
  FailureOr<PtrExprValue> pointer = emitPointerRValue(subscript->getBase());
  if (failed(pointer))
    return failure();
  if (!pointer->cursor) {
    // The address of a scalar admits only the constant-zero index.
    clang::Expr::EvalResult indexValue;
    if (subscript->getIdx()->EvaluateAsInt(indexValue, astContext()) &&
        indexValue.Val.getInt() == 0) {
      PtrExprValue degenerate{pointer->base, Value(),
                              pointer->literalBacking, pointer->nonNull,
                              pointer->baseIndex, pointer->multiBases};
      degenerate.member = pointer->member;
      return degenerate;
    }
    return emitError(loc) << "unsupported: arithmetic on the address "
                             "of a scalar object";
  }
  FailureOr<Value> index = emitRValue(subscript->getIdx());
  if (failed(index))
    return failure();
  if (!llvm::isa<IntegerType>((*index).getType()))
    return emitError(loc) << "unsupported subscript index type";
  Value offset = castToIntType(loc, *index, cursorType);
  // One index step over a row of a multi-dimensional array advances the
  // flat cursor by the row's whole element count (row-major layout); a
  // scalar or struct element keeps the historical unscaled offset.
  uint64_t span = flatElementCount(astContext(), subscript->getType());
  if (span != 1) {
    Value spanValue =
        createIntConstant(loc, cursorType, static_cast<int64_t>(span));
    offset =
        builder.create<arith::MulIOp>(loc, offset, spanValue).getResult();
  }
  Value cursor = builder.create<arith::AddIOp>(loc, pointer->cursor, offset)
                     .getResult();
  return PtrExprValue{pointer->base, cursor, pointer->literalBacking,
                      pointer->nonNull, pointer->baseIndex,
                      pointer->multiBases};
}

/// Returns the number of innermost (non-array) elements one value of the
/// mapped `type` spans: the product of all `!emitrust.array` extents, or 1
/// for a non-array type. The MLIR-side counterpart of the clang-side
/// `flatElementCount`, used to peel a flat row-major cursor one array
/// level at a time.
static uint64_t flatElementCount(Type type) {
  uint64_t count = 1;
  while (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type)) {
    count *= arrayType.getSize();
    type = arrayType.getElementType();
  }
  return count;
}

FailureOr<Value> CImporter::emitPointerPlace(Location loc,
                                             const PtrExprValue &pointer,
                                             Type pointeeType,
                                             GlobalWriteback *writeback) {
  // A pointer of a nullable region that was never bound to any object can
  // only ever hold the null constant; it has no place to designate.
  if (!pointer.base && !pointer.literalBacking && !pointer.baseIndex)
    return emitError(loc)
           << "unsupported: dereference of a pointer that is only ever null";
  // Dereferencing a possibly-null pointer guards on the Option-of-cursor
  // discriminant with a deterministic panic: C dereferencing null is
  // undefined behavior, so the panic is a legal refinement (the fn_ptr
  // `expect` precedent; transformation-theory section 6).
  if (pointer.nonNull)
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr("assert!"),
        builder.getArrayAttr(
            {builder.getIndexAttr(0),
             builder.getStringAttr("null pointer dereference")}),
        ValueRange{pointer.nonNull});
  if (pointer.literalBacking) {
    // A string-literal cursor subscripts the literal's read-only backing
    // byte array (a flat [N x i8], so no level peeling arises). Writes
    // never reach this place: the region analysis rejects any write
    // through a string-literal region at the pointer's declaration.
    if (!pointer.cursor) // Defensive; literal pointers always carry cursors.
      return emitError(loc) << "unsupported string-literal pointer shape";
    auto lvalueType =
        llvm::cast<emitrust::LValueType>(pointer.literalBacking.getType());
    auto arrayType =
        llvm::cast<emitrust::ArrayType>(lvalueType.getValueType());
    return builder
        .create<emitrust::SubscriptOp>(
            loc, emitrust::LValueType::get(arrayType.getElementType()),
            pointer.literalBacking, pointer.cursor)
        .getResult();
  }
  if (pointer.baseIndex) {
    // Multi-base pointer (CTS-P7): no single place exists, so the deref
    // dispatches on the enum-of-bases discriminant — a match over the
    // closed set of bases — and stages the active base's element in a
    // local cell (only the active base is ever touched, so an inactive
    // base's bounds are never consulted). A write context records a
    // writeback that dispatches the staged value back into the active
    // base after the mutation, mirroring the staged-global model.
    if (!llvm::isa<IntegerType, FloatType>(pointeeType))
      return emitError(loc)
             << "unsupported: dereference of a pointer bound to multiple "
                "objects with a non-scalar element type";
    Value staged = isUnsignedInt(pointeeType)
                       ? createVariablePlace(loc, pointeeType)
                       : createEntryAlloca(loc, pointeeType);
    if (failed(emitMultiBaseDispatch(
            loc, pointer.multiBases, pointer.baseIndex,
            [&](const PointerBaseKey &base) -> LogicalResult {
              // A local base resolves to its own (member-projected)
              // element place; a global-member base (CTS-P9) stages the
              // global's whole value afresh and projects the member — the
              // read-side half of the staged-copy model (the write flush
              // dispatches the store-back, see `flushGlobalWriteback`).
              FailureOr<Value> element;
              if (base.var->hasLocalStorage()) {
                element = materializeLocalElementPlace(
                    loc, base, pointer.cursor, pointeeType);
              } else {
                FailureOr<std::pair<Value, std::string>> stagedGlobal =
                    stageGlobalCopy(loc, base.var);
                if (failed(stagedGlobal))
                  return failure();
                Value place = stagedGlobal->first;
                if (base.member) {
                  FailureOr<Value> memberPlace =
                      projectMemberPlace(loc, place, base.member);
                  if (failed(memberPlace))
                    return failure();
                  place = *memberPlace;
                }
                element = refineElementPlace(loc, place, pointer.cursor,
                                             pointeeType);
              }
              if (failed(element))
                return failure();
              return storeToPlace(loc, staged, loadPlace(loc, *element));
            })))
      return failure();
    if (writeback) {
      writeback->place = staged;
      writeback->multiBases = pointer.multiBases;
      writeback->multiBaseIndex = pointer.baseIndex;
      writeback->multiCursor = pointer.cursor;
      writeback->multiPointeeType = pointeeType;
    }
    return staged;
  }
  auto it = symbols.find(pointer.base);
  Value basePlace;
  if (it != symbols.end()) {
    basePlace = it->second;
  } else {
    // A global region base (CTS-P4): a real global object, or a pointer
    // global's synthesized backing. Stage the global's whole value in a
    // local copy — exactly the staged-copy model of direct global element
    // accesses; a write context passes `writeback` and stores the copy
    // back afterwards.
    FailureOr<Value> staged =
        stageGlobalCopyAndRecord(loc, pointer.base, writeback);
    if (failed(staged))
      return failure();
    basePlace = *staged;
  }
  // A `&struct.member` base (CTS-P9) resolves to the member's projection
  // on the object's place (or on its staged copy, whose whole value the
  // writeback stores back).
  if (pointer.member) {
    FailureOr<Value> memberPlace =
        projectMemberPlace(loc, basePlace, pointer.member);
    if (failed(memberPlace))
      return failure();
    basePlace = *memberPlace;
  }
  return refineElementPlace(loc, basePlace, pointer.cursor, pointeeType);
}

FailureOr<std::pair<Value, std::string>>
CImporter::stageGlobalCopy(Location loc, const clang::VarDecl *base) {
  std::string symbol;
  Type stagedType;
  if (const GlobalInfo *global = base ? lookupGlobal(base) : nullptr) {
    symbol = global->symbol;
    stagedType = global->type;
  } else {
    auto globalIt = base ? pointerGlobals.find(base->getCanonicalDecl())
                         : pointerGlobals.end();
    if (globalIt == pointerGlobals.end() ||
        globalIt->second.backingSymbol.empty())
      return emitError(loc) << "unsupported: pointer target '"
                            << (base ? base->getName() : llvm::StringRef())
                            << "' is not an importable place";
    symbol = globalIt->second.backingSymbol;
    stagedType = globalIt->second.backingType;
  }
  Value staged = builder
                     .create<emitrust::VariableOp>(
                         loc, emitrust::LValueType::get(stagedType))
                     .getResult();
  Value current = builder
                      .create<emitrust::GlobalLoadOp>(loc, stagedType,
                                                      globalSymbol(symbol))
                      .getResult();
  builder.create<emitrust::AssignOp>(loc, staged, current);
  return std::make_pair(staged, symbol);
}

FailureOr<Value>
CImporter::stageGlobalCopyAndRecord(Location loc, const clang::VarDecl *base,
                                    GlobalWriteback *writeback) {
  FailureOr<std::pair<Value, std::string>> staged = stageGlobalCopy(loc, base);
  if (failed(staged))
    return failure();
  if (writeback)
    *writeback = GlobalWriteback{staged->first, staged->second};
  return staged->first;
}

//===----------------------------------------------------------------------===//
// CTS-BR (00216): the u8-only byte-region aggregate model.
//===----------------------------------------------------------------------===//

bool CImporter::isU8ScalarType(clang::QualType type) const {
  const auto *builtin = llvm::dyn_cast<clang::BuiltinType>(
      type.getCanonicalType().getTypePtr());
  return builtin && (builtin->getKind() == clang::BuiltinType::UChar ||
                     builtin->getKind() == clang::BuiltinType::Char_U);
}

bool CImporter::isZeroLengthArrayType(clang::QualType type) const {
  const clang::ConstantArrayType *array =
      astContext().getAsConstantArrayType(type);
  return array && array->getSize().isZero();
}

bool CImporter::isByteRegionRecord(const clang::RecordDecl *record) {
  if (!record)
    return false;
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition)
    return false;
  auto it = byteRegionRecords.find(definition);
  if (it != byteRegionRecords.end())
    return it->second;
  // Seed false first: C records cannot recurse by value, but the guard
  // keeps a malformed AST from looping.
  byteRegionRecords[definition] = false;
  clang::ASTContext &context = astContext();

  // Whether every scalar leaf of `type` is `unsigned char` (through
  // constant arrays and nested byte-region records).
  auto isU8Only = [&](auto &&self, clang::QualType type) -> bool {
    clang::QualType canonical = type.getCanonicalType();
    if (isU8ScalarType(canonical))
      return true;
    if (const clang::ConstantArrayType *array =
            context.getAsConstantArrayType(canonical))
      return self(self, array->getElementType());
    if (const auto *nested = canonical->getAsRecordDecl())
      return isByteRegionRecord(nested);
    return false;
  };

  bool result = [&]() -> bool {
    if (definition->isInvalidDecl())
      return false;
    uint64_t recordSize =
        context.getTypeSizeInChars(context.getRecordType(definition))
            .getQuantity();
    // A zero-size record (empty struct) has no dialect byte-array
    // counterpart; it stays typed. As a MEMBER of a byte-region record
    // it still contributes zero bytes (the parent's field walk below).
    if (recordSize == 0)
      return false;
    if (definition->isUnion()) {
      bool anyU8Arm = false;
      for (const clang::FieldDecl *arm : definition->fields()) {
        if (arm->isBitField())
          return false;
        clang::QualType armType = arm->getType();
        if (hasVolatileQualifier(context, armType))
          return false;
        if (armType->isIncompleteArrayType())
          return false;
        if (isU8Only(isU8Only, armType)) {
          anyU8Arm = true;
          continue;
        }
        // A non-u8 arm is tolerated TYPE-level only as a constant array
        // of arithmetic scalars whose total size equals the union's (the
        // in6_addr `unsigned short u6_addr16[8]` over `u8 u6_addr8[16]`
        // shape); accesses through it stay out of the contract. A
        // non-array arm (a scalar-pun union) keeps the typed one-slot
        // model.
        const clang::ConstantArrayType *array =
            context.getAsConstantArrayType(armType);
        if (array &&
            array->getElementType().getCanonicalType()->isArithmeticType() &&
            static_cast<uint64_t>(
                context.getTypeSizeInChars(armType).getQuantity()) ==
                recordSize)
          continue;
        return false;
      }
      return anyU8Arm;
    }
    for (const clang::FieldDecl *field : definition->fields()) {
      if (field->isBitField())
        return false;
      clang::QualType fieldType = field->getType();
      if (hasVolatileQualifier(context, fieldType))
        return false;
      // A trailing flexible array member contributes zero bytes to
      // sizeof; its element must itself be byte-region material so a
      // static tail image can fold.
      if (fieldType->isIncompleteArrayType()) {
        const clang::ArrayType *fam = context.getAsArrayType(fieldType);
        if (!fam || !isU8Only(isU8Only, fam->getElementType()))
          return false;
        continue;
      }
      // A GNU zero-length array contributes zero bytes regardless of its
      // element type (it has no leaves).
      if (isZeroLengthArrayType(fieldType))
        continue;
      // An empty struct member contributes zero bytes.
      if (const auto *nested =
              fieldType.getCanonicalType()->getAsRecordDecl()) {
        const clang::RecordDecl *nestedDefinition = nested->getDefinition();
        if (nestedDefinition && !nestedDefinition->isUnion() &&
            nestedDefinition->field_empty())
          continue;
      }
      if (!isU8Only(isU8Only, fieldType))
        return false;
    }
    return true;
  }();
  byteRegionRecords[definition] = result;
  return result;
}

bool CImporter::isByteRegionAggregate(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType();
  while (const clang::ConstantArrayType *array =
             astContext().getAsConstantArrayType(canonical))
    canonical = array->getElementType().getCanonicalType();
  const auto *record = canonical->getAsRecordDecl();
  return record && isByteRegionRecord(record);
}

bool CImporter::exprRootsInByteRegion(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    return field && isByteRegionRecord(field->getParent());
  }
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    return exprRootsInByteRegion(subscript->getBase());
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref) {
      clang::QualType pointerType = unary->getSubExpr()->getType();
      return isPointerType(pointerType) &&
             isByteRegionAggregate(
                 pointerType.getCanonicalType()->getPointeeType());
    }
  return false;
}

bool CImporter::isByteRegionDesignator(const clang::Expr *expr) const {
  const clang::Expr *e = expr->IgnoreParens();
  while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_NoOp &&
        cast->getCastKind() != clang::CK_LValueToRValue)
      break;
    e = cast->getSubExpr()->IgnoreParens();
  }
  if (llvm::isa<clang::DeclRefExpr, clang::MemberExpr,
                clang::ArraySubscriptExpr, clang::CompoundLiteralExpr>(e))
    return true;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    return unary->getOpcode() == clang::UO_Deref;
  return false;
}

LogicalResult
CImporter::checkSpecialArrayMemberAccess(const clang::FieldDecl *field,
                                         Location loc) {
  if (field->getType()->isIncompleteArrayType())
    return emitError(loc) << "unsupported: flexible array member access";
  if (isZeroLengthArrayType(field->getType()))
    return emitError(loc) << "unsupported: zero-length array member access";
  return success();
}

FailureOr<CImporter::ByteRegionRef>
CImporter::resolveByteRegionRef(const clang::Expr *expr,
                                GlobalWriteback *writeback) {
  const clang::Expr *e = expr->IgnoreParens();
  while (true) {
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e)) {
      e = cleanups->getSubExpr()->IgnoreParens();
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      // Loads and identity/qualification casts of the aggregate value
      // are transparent for region resolution.
      if (cast->getCastKind() == clang::CK_NoOp ||
          cast->getCastKind() == clang::CK_LValueToRValue) {
        e = cast->getSubExpr()->IgnoreParens();
        continue;
      }
    }
    break;
  }
  Location loc = translateLoc(e->getBeginLoc());
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (!var)
      return emitError(loc) << "unsupported byte-region base";
    if (auto it = symbols.find(var); it != symbols.end())
      return ByteRegionRef{it->second, 0, Value()};
    if (lookupGlobal(var)) {
      FailureOr<Value> staged = stageGlobalCopyAndRecord(loc, var, writeback);
      if (failed(staged))
        return failure();
      return ByteRegionRef{*staged, 0, Value()};
    }
    return emitError(loc) << "unsupported byte-region base '"
                          << var->getName() << "'";
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (!field)
      return emitError(loc) << "unsupported member access";
    if (failed(checkSpecialArrayMemberAccess(field, loc)))
      return failure();
    if (!isByteRegionRecord(field->getParent())) {
      // A byte-region record embedded in a TYPED aggregate: the member
      // itself is the region root (its mapped field type is already the
      // byte array).
      FailureOr<Value> place = emitLValue(member, writeback);
      if (failed(place))
        return failure();
      return ByteRegionRef{*place, 0, Value()};
    }
    FailureOr<ByteRegionRef> base =
        member->isArrow()
            ? resolveByteRegionPointer(member->getBase(), writeback)
            : resolveByteRegionRef(member->getBase(), writeback);
    if (failed(base))
      return failure();
    base->constOff += static_cast<int64_t>(
        astContext()
            .getASTRecordLayout(field->getParent())
            .getFieldOffset(field->getFieldIndex()) /
        8);
    return base;
  }
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
    const clang::Expr *baseExpr = subscript->getBase()->IgnoreParenImpCasts();
    FailureOr<ByteRegionRef> base =
        baseExpr->getType().getCanonicalType()->isArrayType()
            ? resolveByteRegionRef(baseExpr, writeback)
            : resolveByteRegionPointer(subscript->getBase(), writeback);
    if (failed(base))
      return failure();
    uint64_t elementSize =
        astContext().getTypeSizeInChars(subscript->getType()).getQuantity();
    clang::Expr::EvalResult eval;
    if (subscript->getIdx()->EvaluateAsInt(eval, astContext()) &&
        !eval.HasSideEffects) {
      base->constOff += eval.Val.getInt().getSExtValue() *
                        static_cast<int64_t>(elementSize);
      return base;
    }
    FailureOr<Value> index = emitRValue(subscript->getIdx());
    if (failed(index))
      return failure();
    Value index64 = castToIntType(loc, *index, builder.getIntegerType(64));
    if (elementSize != 1) {
      Value scale = createIntConstant(loc, builder.getIntegerType(64),
                                      static_cast<int64_t>(elementSize));
      index64 = builder.create<arith::MulIOp>(loc, index64, scale).getResult();
    }
    base->dynOff =
        base->dynOff
            ? builder.create<arith::AddIOp>(loc, base->dynOff, index64)
                  .getResult()
            : index64;
    return base;
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref)
      return resolveByteRegionPointer(unary->getSubExpr(), writeback);
  if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(e)) {
    clang::QualType type = compound->getType();
    FailureOr<Type> mlirType = mapType(type, loc);
    if (failed(mlirType))
      return failure();
    Value place = createVariablePlace(loc, *mlirType);
    if (failed(emitByteRegionInit(place, 0, type, compound->getInitializer())))
      return failure();
    return ByteRegionRef{place, 0, Value()};
  }
  return emitError(loc) << "unsupported byte-region expression: "
                        << e->getStmtClassName();
}

FailureOr<CImporter::ByteRegionRef>
CImporter::resolveByteRegionPointer(const clang::Expr *ptrExpr,
                                    GlobalWriteback *writeback) {
  Location loc = translateLoc(ptrExpr->getBeginLoc());
  FailureOr<PtrExprValue> pointer = emitPointerRValue(ptrExpr);
  if (failed(pointer))
    return failure();
  if (pointer->literalBacking || pointer->baseIndex || pointer->nonNull ||
      pointer->member || !pointer->base)
    return emitError(loc)
           << "unsupported: pointer into a byte-region aggregate";
  Value place;
  if (auto it = symbols.find(pointer->base); it != symbols.end()) {
    place = it->second;
  } else if (lookupGlobal(pointer->base)) {
    FailureOr<Value> staged =
        stageGlobalCopyAndRecord(loc, pointer->base, writeback);
    if (failed(staged))
      return failure();
    place = *staged;
  } else {
    return emitError(loc) << "unsupported byte-region base '"
                          << pointer->base->getName() << "'";
  }
  return ByteRegionRef{place, 0, pointer->cursor};
}

Value CImporter::byteRegionOffset(Location loc, const ByteRegionRef &ref) {
  if (ref.dynOff && ref.constOff == 0)
    return ref.dynOff;
  Value offset =
      createIntConstant(loc, builder.getIntegerType(64), ref.constOff);
  if (ref.dynOff)
    offset = builder.create<arith::AddIOp>(loc, offset, ref.dynOff).getResult();
  return offset;
}

Value CImporter::byteRegionBytePlace(Location loc, const ByteRegionRef &ref,
                                     int64_t extra) {
  ByteRegionRef adjusted = ref;
  adjusted.constOff += extra;
  Value offset = byteRegionOffset(loc, adjusted);
  Type ui8 = IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  return builder
      .create<emitrust::SubscriptOp>(loc, emitrust::LValueType::get(ui8),
                                     ref.place, offset)
      .getResult();
}

FailureOr<Value>
CImporter::emitByteRegionLeafLValue(const clang::Expr *expr, Location loc,
                                    GlobalWriteback *writeback) {
  // Only `unsigned char` leaves have a byte place; a non-u8 leaf (an
  // equal-size union arm's scalar) never joined the region contract.
  if (!isU8ScalarType(expr->getType()))
    return emitError(loc)
           << "unsupported: non-byte member access in a byte-region "
              "aggregate";
  FailureOr<ByteRegionRef> ref = resolveByteRegionRef(expr, writeback);
  if (failed(ref))
    return failure();
  return byteRegionBytePlace(loc, *ref);
}

LogicalResult CImporter::emitByteRegionCopy(Location loc,
                                            const ByteRegionRef &dst,
                                            const ByteRegionRef &src,
                                            uint64_t size) {
  for (uint64_t i = 0; i != size; ++i) {
    Value from = byteRegionBytePlace(loc, src, static_cast<int64_t>(i));
    Value byte = loadPlace(loc, from);
    Value to = byteRegionBytePlace(loc, dst, static_cast<int64_t>(i));
    builder.create<emitrust::AssignOp>(loc, to, byte);
  }
  return success();
}

LogicalResult CImporter::emitByteRegionInit(Value place, int64_t offset,
                                            clang::QualType type,
                                            const clang::Expr *init) {
  clang::ASTContext &context = astContext();
  const clang::Expr *e = init->IgnoreParens();
  while (true) {
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e)) {
      e = cleanups->getSubExpr()->IgnoreParens();
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      // Identity/qualification casts of aggregate values are transparent
      // here; scalar conversions are NOT stripped (the leaf emission
      // relies on the AST's own casts).
      if (cast->getCastKind() == clang::CK_NoOp) {
        e = cast->getSubExpr()->IgnoreParens();
        continue;
      }
    }
    break;
  }
  Location loc = translateLoc(e->getBeginLoc());
  // A hole keeps the region's default zero value (C99 zero fill).
  if (llvm::isa<clang::ImplicitValueInitExpr>(e) ||
      llvm::isa<clang::NoInitExpr>(e))
    return success();
  if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(e))
    return emitByteRegionInit(place, offset, type, compound->getInitializer());
  clang::QualType canonical = type.getCanonicalType();
  Type ui8 = IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  // The `unsigned char` scalar leaf: constants fold to ui8 stores,
  // runtime values flow through their AST conversion casts.
  if (isU8ScalarType(canonical)) {
    Value index = createIntConstant(loc, builder.getIntegerType(64), offset);
    Value leaf = builder
                     .create<emitrust::SubscriptOp>(
                         loc, emitrust::LValueType::get(ui8), place, index)
                     .getResult();
    clang::Expr::EvalResult eval;
    if (e->EvaluateAsInt(eval, context) && !eval.HasSideEffects) {
      uint64_t byte = eval.Val.getInt().getZExtValue() & 0xff;
      Value constant =
          builder
              .create<emitrust::ConstantOp>(loc, ui8, IntegerAttr::get(ui8, byte))
              .getResult();
      builder.create<emitrust::AssignOp>(loc, leaf, constant);
      return success();
    }
    FailureOr<Value> value = emitRValue(e);
    if (failed(value))
      return failure();
    Value byte = *value;
    if (byte.getType() != ui8)
      byte = builder.create<emitrust::CastOp>(loc, ui8, byte).getResult();
    builder.create<emitrust::AssignOp>(loc, leaf, byte);
    return success();
  }
  if (const clang::ConstantArrayType *array =
          context.getAsConstantArrayType(canonical)) {
    uint64_t elementSize =
        context.getTypeSizeInChars(array->getElementType()).getQuantity();
    uint64_t arraySize = array->getSize().getZExtValue();
    // A string-literal member initializer contributes its bytes; the
    // rest of the array member keeps the zero fill.
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(e)) {
      if (!literal->isOrdinary())
        return emitError(loc)
               << "unsupported: non-ordinary string literal initializer";
      uint64_t count = std::min<uint64_t>(literal->getLength(), arraySize);
      for (uint64_t i = 0; i != count; ++i) {
        uint32_t byte = literal->getCodeUnit(i);
        if (byte > 127)
          return emitError(loc)
                 << "unsupported: non-ASCII byte in string literal "
                    "initializer";
        if (byte == 0)
          continue; // Embedded NULs keep the zero fill.
        Value index = createIntConstant(loc, builder.getIntegerType(64),
                                        offset + static_cast<int64_t>(i));
        Value leaf = builder
                         .create<emitrust::SubscriptOp>(
                             loc, emitrust::LValueType::get(ui8), place, index)
                         .getResult();
        Value constant = builder
                             .create<emitrust::ConstantOp>(
                                 loc, ui8, IntegerAttr::get(ui8, byte))
                             .getResult();
        builder.create<emitrust::AssignOp>(loc, leaf, constant);
      }
      return success();
    }
    if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(e)) {
      if (const clang::InitListExpr *semantic = list->getSemanticForm())
        list = semantic;
      // `{"hello"}`: braces around a string literal initializer.
      if (list->getNumInits() == 1)
        if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
                list->getInit(0)->IgnoreParens()))
          return emitByteRegionInit(place, offset, type, literal);
      uint64_t count = std::min<uint64_t>(list->getNumInits(), arraySize);
      for (uint64_t i = 0; i != count; ++i)
        if (failed(emitByteRegionInit(
                place, offset + static_cast<int64_t>(i * elementSize),
                array->getElementType(), list->getInit(i))))
          return failure();
      if (list->hasArrayFiller())
        if (const clang::Expr *filler = list->getArrayFiller();
            filler && !llvm::isa<clang::ImplicitValueInitExpr>(filler))
          for (uint64_t i = count; i < arraySize; ++i)
            if (failed(emitByteRegionInit(
                    place, offset + static_cast<int64_t>(i * elementSize),
                    array->getElementType(), filler)))
              return failure();
      return success();
    }
    // Fall through: a whole-array value copies its source region.
  }
  if (const auto *record = canonical->getAsRecordDecl()) {
    if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(e)) {
      if (const clang::InitListExpr *semantic = list->getSemanticForm())
        list = semantic;
      const clang::RecordDecl *definition = record->getDefinition();
      if (!definition)
        return emitError(loc) << "unsupported: incomplete struct type";
      if (definition->isUnion()) {
        // Sema records the single arm the list initializes; every arm
        // is a view at offset 0 of the region, and the bytes past the
        // initialized arm keep the zero fill.
        const clang::FieldDecl *active = list->getInitializedFieldInUnion();
        if (!active || list->getNumInits() == 0)
          return success();
        return emitByteRegionInit(place, offset, active->getType(),
                                  list->getInit(0));
      }
      const clang::ASTRecordLayout &layout =
          context.getASTRecordLayout(definition);
      unsigned index = 0;
      for (const clang::FieldDecl *field : definition->fields()) {
        if (index >= list->getNumInits())
          break; // Remaining fields keep the zero fill.
        const clang::Expr *element = list->getInit(index++);
        if (!element || llvm::isa<clang::ImplicitValueInitExpr>(element) ||
            llvm::isa<clang::NoInitExpr>(element))
          continue;
        // A LOCAL flexible-array tail has no storage behind sizeof.
        if (field->getType()->isIncompleteArrayType())
          return emitError(translateLoc(element->getBeginLoc()))
                 << "unsupported: flexible array member access";
        if (isZeroLengthArrayType(field->getType()))
          continue;
        if (failed(emitByteRegionInit(
                place,
                offset + static_cast<int64_t>(
                             layout.getFieldOffset(field->getFieldIndex()) /
                             8),
                field->getType(), element)))
          return failure();
      }
      return success();
    }
    // Fall through: a whole-record value copies its source region.
  }
  if (canonical->isRecordType() || canonical->isArrayType()) {
    FailureOr<ByteRegionRef> src = resolveByteRegionRef(e, nullptr);
    if (failed(src))
      return failure();
    ByteRegionRef dst{place, offset, Value()};
    return emitByteRegionCopy(
        loc, dst, *src, context.getTypeSizeInChars(canonical).getQuantity());
  }
  return emitError(loc)
         << "unsupported: non-byte member in a byte-region initializer";
}

LogicalResult
CImporter::emitByteRegionAggregateAssign(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  GlobalWriteback writeback;
  FailureOr<ByteRegionRef> dst = resolveByteRegionRef(op->getLHS(), &writeback);
  if (failed(dst))
    return failure();
  FailureOr<ByteRegionRef> src = resolveByteRegionRef(op->getRHS(), nullptr);
  if (failed(src))
    return failure();
  uint64_t size = astContext()
                      .getTypeSizeInChars(op->getLHS()->getType())
                      .getQuantity();
  if (failed(emitByteRegionCopy(loc, *dst, *src, size)))
    return failure();
  if (writeback.place) {
    Value value = loadPlace(loc, writeback.place);
    builder.create<emitrust::GlobalStoreOp>(loc, value,
                                            globalSymbol(writeback.symbol));
  }
  return success();
}

LogicalResult CImporter::createByteRegionGlobal(const clang::VarDecl *key,
                                                const clang::VarDecl *decl,
                                                llvm::StringRef symbolName,
                                                Location loc) {
  clang::ASTContext &context = astContext();
  clang::QualType type = decl->getType();
  uint64_t imageSize = context.getTypeSizeInChars(type).getQuantity();
  Type ui8 = IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  Attribute initAttr;
  if (decl->getInit()) {
    Location initLoc = translateLoc(decl->getInit()->getBeginLoc());
    clang::APValue *value = decl->evaluateValue();
    SmallVector<uint8_t> image;
    if (value) {
      // A static flexible-array-member tail initializer folds into an
      // EXTENDED image past sizeof (00216's `struct W gw`); sizeof
      // itself stays FAM-free.
      if (const auto *record = type.getCanonicalType()->getAsRecordDecl()) {
        const clang::RecordDecl *definition = record->getDefinition();
        if (definition && definition->hasFlexibleArrayMember() &&
            value->isStruct()) {
          const clang::FieldDecl *fam = nullptr;
          for (const clang::FieldDecl *field : definition->fields())
            if (field->getType()->isIncompleteArrayType())
              fam = field;
          if (fam && fam->getFieldIndex() < value->getStructNumFields()) {
            const clang::APValue &tail =
                value->getStructField(fam->getFieldIndex());
            if (tail.isArray() && tail.getArraySize() > 0) {
              const clang::ArrayType *famType =
                  context.getAsArrayType(fam->getType());
              imageSize +=
                  tail.getArraySize() *
                  context.getTypeSizeInChars(famType->getElementType())
                      .getQuantity();
            }
          }
        }
      }
      image.assign(imageSize, 0);
      if (failed(serializeAPValueBytes(*value, type, 0, image, initLoc)))
        return failure();
    } else {
      // clang's constant evaluator refuses flexible-array-member tail
      // initializers; fold the SEMANTIC initializer form syntactically.
      // The FAM init's semantic type is the deduced constant array, so
      // its size extends the image.
      const clang::Expr *init = decl->getInit()->IgnoreParens();
      if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(init)) {
        const clang::InitListExpr *semantic =
            list->getSemanticForm() ? list->getSemanticForm() : list;
        if (const auto *record = type.getCanonicalType()->getAsRecordDecl();
            record && record->getDefinition() &&
            record->getDefinition()->hasFlexibleArrayMember()) {
          unsigned index = 0;
          for (const clang::FieldDecl *field :
               record->getDefinition()->fields()) {
            if (index >= semantic->getNumInits())
              break;
            const clang::Expr *element = semantic->getInit(index++);
            if (field->getType()->isIncompleteArrayType() && element &&
                !llvm::isa<clang::ImplicitValueInitExpr>(element) &&
                context.getAsConstantArrayType(element->getType()))
              imageSize +=
                  context.getTypeSizeInChars(element->getType()).getQuantity();
          }
        }
      }
      image.assign(imageSize, 0);
      if (failed(serializeInitExprBytes(decl->getInit(), type, 0, image,
                                        initLoc)))
        return failure();
    }
    SmallVector<Attribute> bytes;
    bytes.reserve(image.size());
    for (uint8_t byte : image)
      bytes.push_back(IntegerAttr::get(ui8, byte));
    initAttr = builder.getArrayAttr(bytes);
  }
  if (imageSize == 0)
    return emitError(loc) << "unsupported: zero-size byte-region global";
  Type regionType =
      emitrust::ArrayType::get(builder.getContext(), imageSize, ui8);
  bool isConst = type.isConstQualified();
  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::GlobalOp>(
      loc, moduleBuilder.getStringAttr(symbolName), TypeAttr::get(regionType),
      initAttr, isConst ? moduleBuilder.getUnitAttr() : UnitAttr());
  globals[key] = GlobalInfo{symbolName.str(), regionType};
  return success();
}

LogicalResult CImporter::serializeAPValueBytes(const clang::APValue &value,
                                               clang::QualType type,
                                               uint64_t offset,
                                               SmallVectorImpl<uint8_t> &image,
                                               Location loc) {
  clang::ASTContext &context = astContext();
  clang::QualType canonical = type.getCanonicalType();
  if (value.isAbsent() || value.isIndeterminate())
    return success(); // Zero fill.
  if (value.isInt()) {
    uint64_t width = context.getTypeSizeInChars(canonical).getQuantity();
    uint64_t raw = value.getInt().extOrTrunc(64).getZExtValue();
    for (uint64_t i = 0; i != width; ++i) {
      if (offset + i >= image.size())
        return emitError(loc)
               << "unsupported: global initializer exceeds the byte image";
      image[offset + i] = (raw >> (8 * i)) & 0xff;
    }
    return success();
  }
  if (value.isArray()) {
    const clang::ArrayType *array = context.getAsArrayType(canonical);
    if (!array)
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    clang::QualType element = array->getElementType();
    uint64_t elementSize = context.getTypeSizeInChars(element).getQuantity();
    unsigned initialized = value.getArrayInitializedElts();
    for (unsigned i = 0; i != initialized; ++i)
      if (failed(serializeAPValueBytes(value.getArrayInitializedElt(i),
                                       element, offset + i * elementSize,
                                       image, loc)))
        return failure();
    if (value.hasArrayFiller())
      for (unsigned i = initialized, n = value.getArraySize(); i != n; ++i)
        if (failed(serializeAPValueBytes(value.getArrayFiller(), element,
                                         offset + i * elementSize, image,
                                         loc)))
          return failure();
    return success();
  }
  if (value.isUnion()) {
    const clang::FieldDecl *active = value.getUnionField();
    if (!active)
      return success(); // Zero fill.
    return serializeAPValueBytes(value.getUnionValue(), active->getType(),
                                 offset, image, loc);
  }
  if (value.isStruct()) {
    const auto *record = canonical->getAsRecordDecl();
    const clang::RecordDecl *definition =
        record ? record->getDefinition() : nullptr;
    if (!definition)
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::ASTRecordLayout &layout =
        context.getASTRecordLayout(definition);
    unsigned index = 0;
    for (const clang::FieldDecl *field : definition->fields()) {
      if (index >= value.getStructNumFields())
        break;
      const clang::APValue &fieldValue = value.getStructField(index);
      uint64_t fieldOffset = layout.getFieldOffset(index) / 8;
      ++index;
      if (failed(serializeAPValueBytes(fieldValue, field->getType(),
                                       offset + fieldOffset, image, loc)))
        return failure();
    }
    return success();
  }
  return emitError(loc)
         << "unsupported: non-byte constant in a byte-region initializer";
}

LogicalResult
CImporter::serializeInitExprBytes(const clang::Expr *init,
                                  clang::QualType type, uint64_t offset,
                                  SmallVectorImpl<uint8_t> &image,
                                  Location loc) {
  clang::ASTContext &context = astContext();
  const clang::Expr *e = init->IgnoreParens();
  while (true) {
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e)) {
      e = cleanups->getSubExpr()->IgnoreParens();
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_NoOp ||
          cast->getCastKind() == clang::CK_LValueToRValue) {
        e = cast->getSubExpr()->IgnoreParens();
        continue;
      }
    }
    break;
  }
  if (llvm::isa<clang::ImplicitValueInitExpr>(e) ||
      llvm::isa<clang::NoInitExpr>(e))
    return success(); // Zero fill.
  if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(e))
    return serializeInitExprBytes(compound->getInitializer(), type, offset,
                                  image, loc);
  clang::QualType canonical = type.getCanonicalType();
  if (canonical->isArithmeticType() || canonical->isEnumeralType()) {
    clang::Expr::EvalResult eval;
    if (!e->EvaluateAsInt(eval, context) || eval.HasSideEffects)
      return emitError(loc) << "unsupported: non-constant global initializer";
    uint64_t width = context.getTypeSizeInChars(canonical).getQuantity();
    uint64_t raw = eval.Val.getInt().extOrTrunc(64).getZExtValue();
    for (uint64_t i = 0; i != width; ++i) {
      if (offset + i >= image.size())
        return emitError(loc)
               << "unsupported: global initializer exceeds the byte image";
      image[offset + i] = (raw >> (8 * i)) & 0xff;
    }
    return success();
  }
  if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(e)) {
    if (!literal->isOrdinary())
      return emitError(loc)
             << "unsupported: non-ordinary string literal initializer";
    for (unsigned i = 0, n = literal->getLength(); i != n; ++i) {
      uint32_t byte = literal->getCodeUnit(i);
      if (byte > 127)
        return emitError(loc)
               << "unsupported: non-ASCII byte in string literal initializer";
      if (offset + i >= image.size())
        break; // The unsized-array literal drops the excess NUL.
      image[offset + i] = byte & 0xff;
    }
    return success();
  }
  const auto *list = llvm::dyn_cast<clang::InitListExpr>(e);
  if (!list)
    return emitError(loc) << "unsupported: non-constant global initializer";
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  // The FAM tail's semantic list carries its own deduced constant array
  // type; prefer it over the declared (incomplete) member type.
  if (canonical->isIncompleteArrayType())
    if (const clang::ConstantArrayType *deduced =
            context.getAsConstantArrayType(list->getType()))
      canonical = clang::QualType(deduced, 0);
  if (const clang::ConstantArrayType *array =
          context.getAsConstantArrayType(canonical)) {
    clang::QualType element = array->getElementType();
    uint64_t elementSize = context.getTypeSizeInChars(element).getQuantity();
    uint64_t arraySize = array->getSize().getZExtValue();
    if (list->getNumInits() == 1)
      if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
              list->getInit(0)->IgnoreParens()))
        return serializeInitExprBytes(literal, canonical, offset, image, loc);
    uint64_t count = std::min<uint64_t>(list->getNumInits(), arraySize);
    for (uint64_t i = 0; i != count; ++i)
      if (failed(serializeInitExprBytes(list->getInit(i), element,
                                        offset + i * elementSize, image,
                                        loc)))
        return failure();
    if (list->hasArrayFiller())
      if (const clang::Expr *filler = list->getArrayFiller();
          filler && !llvm::isa<clang::ImplicitValueInitExpr>(filler))
        for (uint64_t i = count; i < arraySize; ++i)
          if (failed(serializeInitExprBytes(filler, element,
                                            offset + i * elementSize, image,
                                            loc)))
            return failure();
    return success();
  }
  const auto *record = canonical->getAsRecordDecl();
  const clang::RecordDecl *definition =
      record ? record->getDefinition() : nullptr;
  if (!definition)
    return emitError(loc) << "unsupported: non-constant global initializer";
  if (definition->isUnion()) {
    const clang::FieldDecl *active = list->getInitializedFieldInUnion();
    if (!active || list->getNumInits() == 0)
      return success();
    return serializeInitExprBytes(list->getInit(0), active->getType(), offset,
                                  image, loc);
  }
  const clang::ASTRecordLayout &layout = context.getASTRecordLayout(definition);
  unsigned index = 0;
  for (const clang::FieldDecl *field : definition->fields()) {
    if (index >= list->getNumInits())
      break;
    const clang::Expr *element = list->getInit(index++);
    if (!element)
      continue;
    if (failed(serializeInitExprBytes(
            element, field->getType(),
            offset + layout.getFieldOffset(field->getFieldIndex()) / 8, image,
            loc)))
      return failure();
  }
  return success();
}

//===----------------------------------------------------------------------===//
// CTS-BR (00216): `void *` fn-ptr struct members.
//===----------------------------------------------------------------------===//

void CImporter::collectDeclTypeRecords(const clang::TranslationUnitDecl *unit) {
  declTypeUsedRecords.clear();
  clang::ASTContext &context = astContext();
  auto noteType = [&](clang::QualType type) {
    clang::QualType t = type.getCanonicalType();
    while (true) {
      if (const clang::ArrayType *array = context.getAsArrayType(t)) {
        t = array->getElementType().getCanonicalType();
        continue;
      }
      if (t->isPointerType()) {
        t = t->getPointeeType().getCanonicalType();
        continue;
      }
      break;
    }
    if (const auto *record = t->getAsRecordDecl())
      if (const clang::RecordDecl *definition = record->getDefinition())
        declTypeUsedRecords.insert(definition);
  };
  auto scanStmt = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          noteType(var->getType());
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      noteType(func->getReturnType());
      for (const clang::ParmVarDecl *param : func->parameters())
        noteType(param->getType());
      if (func->hasBody() && func->getDefinition() == func)
        scanStmt(scanStmt, func->getBody());
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      noteType(var->getType());
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl))
      if (const clang::RecordDecl *definition = record->getDefinition())
        // A byte-region record never materializes its members as typed
        // fields, so its field types are not declaration-type uses.
        if (!isByteRegionRecord(definition))
          for (const clang::FieldDecl *field : definition->fields())
            noteType(field->getType());
  }
}

void CImporter::planFnPtrMembers(const clang::TranslationUnitDecl *unit) {
  clang::ASTContext &context = astContext();
  llvm::DenseMap<const clang::FieldDecl *, const clang::FunctionDecl *>
      targets;
  llvm::DenseSet<const clang::FieldDecl *> disqualified;
  llvm::SmallVector<std::pair<const clang::FieldDecl *, clang::QualType>, 4>
      readerCasts;

  auto isCandidateField = [&](const clang::FieldDecl *field) -> bool {
    if (!field)
      return false;
    clang::QualType type = field->getType().getCanonicalType();
    return type->isPointerType() && type->getPointeeType()->isVoidType();
  };

  // Strips the value trivia around a stored function address: implicit
  // and explicit casts (function-to-pointer decay, the void* conversion)
  // and the optional address-of.
  auto functionTarget =
      [&](const clang::Expr *expr) -> const clang::FunctionDecl * {
    const clang::Expr *e = expr->IgnoreParenCasts();
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
      if (unary->getOpcode() == clang::UO_AddrOf)
        e = unary->getSubExpr()->IgnoreParenCasts();
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
    return ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
  };

  // Walks an aggregate initializer, recording (or disqualifying) every
  // value stored into a candidate member.
  auto walkInit = [&](auto &&self, clang::QualType type,
                      const clang::Expr *init) -> void {
    if (!init)
      return;
    const clang::Expr *e = init->IgnoreParenImpCasts();
    if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(e)) {
      self(self, compound->getType(), compound->getInitializer());
      return;
    }
    const auto *list = llvm::dyn_cast<clang::InitListExpr>(e);
    if (!list)
      return;
    if (const clang::InitListExpr *semantic = list->getSemanticForm())
      list = semantic;
    clang::QualType canonical = type.getCanonicalType();
    if (const clang::ConstantArrayType *array =
            context.getAsConstantArrayType(canonical)) {
      for (unsigned i = 0, n = list->getNumInits(); i != n; ++i)
        self(self, array->getElementType(), list->getInit(i));
      if (list->hasArrayFiller())
        self(self, array->getElementType(), list->getArrayFiller());
      return;
    }
    const auto *record = canonical->getAsRecordDecl();
    const clang::RecordDecl *definition =
        record ? record->getDefinition() : nullptr;
    if (!definition)
      return;
    if (definition->isUnion()) {
      if (const clang::FieldDecl *active = list->getInitializedFieldInUnion();
          active && list->getNumInits())
        self(self, active->getType(), list->getInit(0));
      return;
    }
    unsigned index = 0;
    for (const clang::FieldDecl *field : definition->fields()) {
      if (index >= list->getNumInits())
        break;
      const clang::Expr *element = list->getInit(index++);
      if (!element)
        continue;
      if (!isCandidateField(field)) {
        self(self, field->getType(), element);
        continue;
      }
      if (llvm::isa<clang::ImplicitValueInitExpr>(element) ||
          element->isNullPointerConstant(
              context, clang::Expr::NPC_NeverValueDependent) !=
              clang::Expr::NPCK_NotNull)
        continue; // The null constant folds to None.
      const clang::FunctionDecl *target = functionTarget(element);
      if (!target || target->isVariadic() ||
          !target->getType()->getAs<clang::FunctionProtoType>()) {
        disqualified.insert(field);
        continue;
      }
      auto [it, inserted] = targets.try_emplace(field, target);
      if (!inserted &&
          !context.hasSameType(it->second->getType(), target->getType()))
        disqualified.insert(field);
    }
  };

  // Walks a function body: a candidate-member read under a cast to a
  // function-pointer type is the one admitted use; any other mention
  // disqualifies.
  auto scanStmt = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(stmt)) {
      if (isFunctionPointer(cast->getType())) {
        const clang::Expr *sub = cast->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(sub)) {
          if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(
                  member->getMemberDecl());
              field && isCandidateField(field)) {
            readerCasts.push_back({field, cast->getType()});
            self(self, member->getBase());
            return;
          }
        }
      }
    }
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          walkInit(walkInit, var->getType(), var->getInit());
      // Fall through: the generic child scan below revisits the
      // initializer expressions, which mention no candidate members in
      // admitted programs (a mention there disqualifies, as intended).
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt))
      if (const auto *field =
              llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
          field && isCandidateField(field))
        disqualified.insert(field);
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };

  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->hasBody() && func->getDefinition() == func)
        scanStmt(scanStmt, func->getBody());
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      walkInit(walkInit, var->getType(), var->getInit());
  }

  for (auto &[field, target] : targets) {
    if (disqualified.contains(field))
      continue;
    bool compatible = true;
    for (auto &[readField, castType] : readerCasts)
      if (readField == field &&
          !context.typesAreCompatible(
              castType.getCanonicalType()->getPointeeType(),
              target->getType()))
        compatible = false;
    if (!compatible)
      continue;
    fnPtrMemberTypes[field] = context.getPointerType(target->getType());
  }
}

FailureOr<Value>
CImporter::projectMemberPlace(Location loc, Value basePlace,
                              const clang::FieldDecl *field) {
  auto baseType = llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
  if (!baseType || !llvm::isa<emitrust::StructType>(baseType.getValueType()))
    return emitError(loc) << "unsupported member access base";
  FailureOr<Type> fieldType = mapType(field->getType(), loc);
  if (failed(fieldType))
    return failure();
  return builder
      .create<emitrust::MemberOp>(
          loc, emitrust::LValueType::get(*fieldType), basePlace,
          builder.getStringAttr(flattenedFieldName(field)))
      .getResult();
}

FailureOr<Value> CImporter::refineElementPlace(Location loc, Value basePlace,
                                               Value cursor,
                                               Type pointeeType) {
  if (!cursor)
    return basePlace; // Degenerate: the pointer designates the whole object.
  auto lvalueType = llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
  if (!lvalueType)
    return emitError(loc) << "unsupported pointer target place";
  // The base place wraps an array (local array base) or a slice (deref'd
  // slice parameter base); both subscript by the cursor. A nested array
  // peels one level per subscript until the pointee type is reached: the
  // flat row-major cursor divides by the level's element span for the
  // index and continues into the level with the remainder.
  Value place = basePlace;
  Type valueType = lvalueType.getValueType();
  while (valueType != pointeeType) {
    Type elementType;
    if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(valueType))
      elementType = arrayType.getElementType();
    else if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(valueType))
      elementType = sliceType.getElementType();
    else
      return emitError(loc) << "unsupported pointer target place";
    uint64_t span = flatElementCount(elementType);
    Value index = cursor;
    if (span != 1) {
      Value spanValue = createIntConstant(loc, builder.getIntegerType(64),
                                          static_cast<int64_t>(span));
      index =
          builder.create<arith::DivSIOp>(loc, cursor, spanValue).getResult();
      // The remainder feeds the next level's subscript; when the pointee
      // is this level's element (a row pointer) the peel stops here and
      // the remainder is not needed.
      if (elementType != pointeeType)
        cursor = builder.create<arith::RemSIOp>(loc, cursor, spanValue)
                     .getResult();
    }
    place = builder
                .create<emitrust::SubscriptOp>(
                    loc, emitrust::LValueType::get(elementType), place, index)
                .getResult();
    valueType = elementType;
  }
  if (place == basePlace)
    return emitError(loc) << "unsupported pointer target place";
  return place;
}

FailureOr<Value>
CImporter::materializeLocalElementPlace(Location loc,
                                        const PointerBaseKey &base,
                                        Value cursor, Type pointeeType) {
  auto it = symbols.find(base.var);
  if (it == symbols.end())
    return emitError(loc) << "unsupported: pointer target '"
                          << base.var->getName()
                          << "' is not an importable place";
  Value place = it->second;
  if (base.member) {
    FailureOr<Value> memberPlace = projectMemberPlace(loc, place, base.member);
    if (failed(memberPlace))
      return failure();
    place = *memberPlace;
  }
  return refineElementPlace(loc, place, cursor, pointeeType);
}

LogicalResult CImporter::emitMultiBaseDispatch(
    Location loc, ArrayRef<PointerBaseKey> bases, Value baseIndex,
    llvm::function_ref<LogicalResult(const PointerBaseKey &)> emitArm) {
  // Blocks are created in flow order — arm 0, the next test, ..., with
  // the continuation block last — so the printed block order matches the
  // dispatch order. The branches into the continuation are added after
  // every arm is emitted (the block does not exist earlier).
  SmallVector<Block *, 4> exits;
  for (auto [index, base] : llvm::enumerate(bases)) {
    Block *nextBlock = nullptr;
    if (index + 1 < bases.size()) {
      // Test this variant; a miss falls through to the next base's test.
      Block *armBlock = createBlock();
      nextBlock = createBlock();
      Value expected = createIntConstant(loc, builder.getIntegerType(32),
                                         static_cast<int64_t>(index));
      Value hit = builder
                      .create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                             baseIndex, expected)
                      .getResult();
      builder.create<cf::CondBranchOp>(loc, hit, armBlock, ValueRange(),
                                       nextBlock, ValueRange());
      builder.setInsertionPointToEnd(armBlock);
    }
    // The last base needs no test: the discriminant only ever holds a
    // bound index, so it is the final else of the match over the closed
    // set of bases.
    if (failed(emitArm(base)))
      return failure();
    exits.push_back(builder.getInsertionBlock());
    if (nextBlock)
      builder.setInsertionPointToEnd(nextBlock);
  }
  Block *mergeBlock = createBlock();
  for (Block *exit : exits) {
    builder.setInsertionPointToEnd(exit);
    builder.create<cf::BranchOp>(loc, mergeBlock);
  }
  builder.setInsertionPointToEnd(mergeBlock);
  return success();
}

FailureOr<Value>
CImporter::emitPointerDifference(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // The flat cursor difference counts innermost elements; a difference of
  // row pointers would need a row-scaled division (CTS-P scope).
  if (pointsToArray(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: arithmetic on a pointer to an array";
  FailureOr<PtrExprValue> lhs = emitPointerRValue(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<PtrExprValue> rhs = emitPointerRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  // A multi-base side may designate different objects at runtime, so no
  // static cursor difference exists (C defines the difference only within
  // one object).
  if (lhs->baseIndex || rhs->baseIndex)
    return emitError(loc)
           << "unsupported: difference of pointers bound to multiple objects";
  if (lhs->base != rhs->base || lhs->member != rhs->member ||
      lhs->literalBacking != rhs->literalBacking)
    return emitError(loc)
           << "unsupported: difference of pointers into different objects";
  // C defines pointer difference only for pointers into the same array; a
  // possibly-null operand has no defined difference, and erasing its
  // discriminant would translate a null operand silently.
  if (lhs->nonNull || rhs->nonNull)
    return emitError(loc)
           << "unsupported: difference of possibly-null pointers";
  Type cursorType = builder.getIntegerType(64);
  Value left =
      lhs->cursor ? lhs->cursor : createIntConstant(loc, cursorType, 0);
  Value right =
      rhs->cursor ? rhs->cursor : createIntConstant(loc, cursorType, 0);
  return builder.create<arith::SubIOp>(loc, left, right).getResult();
}

FailureOr<Value> CImporter::emitLValue(const clang::Expr *expr,
                                       GlobalWriteback *writeback) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  // A full expression containing a block-scope compound literal is wrapped
  // in ExprWithCleanups (C99-13); nothing to emit for the "cleanup".
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    return emitLValue(cleanups->getSubExpr(), writeback);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    return emitDeclRefLValue(ref, loc, writeback);
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
    return emitMemberLValue(member, loc, writeback);
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    return emitSubscriptLValue(subscript, loc, writeback);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref)
      return emitDerefLValue(unary, loc, writeback);
  // A compound literal is an lvalue in C99 (C99-13): its place is the
  // freshly materialized anonymous temp, so `(struct S){...}.a`,
  // `(int[]){...}[i]`, and whole-value loads all resolve on it like on a
  // named local.
  if (const auto *literal = llvm::dyn_cast<clang::CompoundLiteralExpr>(e))
    return emitCompoundLiteralPlace(literal);

  // A `__func__`-family identifier is modeled only in the string-literal
  // positions (C99-29); an element access or other place use of the name
  // array keeps a located rejection naming the identifier.
  if (const auto *predefined = llvm::dyn_cast<clang::PredefinedExpr>(e))
    return emitError(loc) << "unsupported use of '"
                          << predefined->getIdentKindName()
                          << "' outside a string literal position";
  return emitError(loc) << "unsupported assignable expression: "
                        << e->getStmtClassName();
}

FailureOr<Value> CImporter::emitDeclRefLValue(const clang::DeclRefExpr *ref,
                                              Location loc,
                                              GlobalWriteback *writeback) {
  auto it = symbols.find(ref->getDecl());
  if (it == symbols.end()) {
    if (lookupGlobal(ref->getDecl())) {
      // Stage the global's whole value in a local copy. Refined element
      // and field accesses read and write the copy; a write context
      // passes `writeback` and stores the copy back afterwards
      // (load-modify-store). Exact for the single-threaded C subset: a
      // function called from the same statement's index or right-hand
      // side that writes the same global is preserved by the pre-store
      // refresh in `commitGlobalWriteback`.
      return stageGlobalCopyAndRecord(
          loc, llvm::cast<clang::VarDecl>(ref->getDecl()), writeback);
    }
    // A devirtualized global function pointer (CTS-S, 00189) has no
    // global of its own; a value use reads as the `Some(target)`
    // constant (writes were excluded by the never-reassigned
    // criterion). A variadic (printf-routed) alias has no fn_ptr value
    // at all and keeps the type-level rejection at the use site.
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
      if (const clang::FunctionDecl *target =
              fnPtrAliases.lookup(var->getCanonicalDecl())) {
        if (target->isVariadic())
          return emitError(loc)
                 << "unsupported: variadic function pointer type";
        FailureOr<Type> mapped = mapType(var->getType(), loc);
        if (failed(mapped))
          return failure();
        auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*mapped);
        if (!fnPtrType)
          return emitError(loc) << "unsupported function pointer type";
        FailureOr<std::string> name =
            resolveFunctionPointerDecl(target, fnPtrType, loc);
        if (failed(name))
          return failure();
        Value place = builder
                          .create<emitrust::VariableOp>(
                              loc, emitrust::LValueType::get(fnPtrType))
                          .getResult();
        auto some = emitrust::OpaqueAttr::get(
            builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
        Value constant =
            builder.create<emitrust::ConstantOp>(loc, fnPtrType, some)
                .getResult();
        builder.create<emitrust::AssignOp>(loc, place, constant);
        return place;
      }
    }
    // Decomposed pointer locals have no place of their own; every
    // supported use is routed through the pointer paths before this one.
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (pointerRegions.tracks(var))
        return emitError(loc) << "unsupported use of pointer variable '"
                              << var->getName() << "'";
    // A named declaration skipped at import time because it lives in a
    // system header (stdin, errno-style globals, ...) gets the dedicated
    // use-site rejection; every other miss keeps the generic message.
    if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(ref->getDecl()))
      if (named->getDeclName().isIdentifier() && isSystemHeaderDecl(named))
        return rejectSystemHeaderUse(loc, "reference to", named->getName());
    return emitError(loc) << "unsupported: reference to an unknown variable";
  }
  Value place = it->second;
  if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(place.getType()))
    return emitError(loc)
           << "unsupported: pointer variable used as an assignable place";
  // A slice parameter's place designates its element run, not the C
  // pointer variable; every supported use is routed through the pointer
  // paths (deref, subscript, cursor updates) before this one. Function
  // pointers are ordinary by-value parameters and keep their place.
  if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
    if (isPointerType(param->getType()) && !isFunctionPointer(param->getType()))
      return emitError(loc) << "unsupported use of pointer parameter '"
                            << param->getName() << "'";
  return place;
}

FailureOr<Value> CImporter::emitMemberLValue(const clang::MemberExpr *member,
                                             Location loc,
                                             GlobalWriteback *writeback) {
  const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field)
    return emitError(loc) << "unsupported member access";
  // A flexible array member tail and a GNU zero-length array member have
  // no storage behind sizeof; runtime access is a located rejection
  // (CTS-BR, 00216).
  if (failed(checkSpecialArrayMemberAccess(field, loc)))
    return failure();
  // CTS-BR (00216): a member of a byte-region record is a subscript of
  // the region base at the member's constant byte offset.
  if (isByteRegionRecord(field->getParent()))
    return emitByteRegionLeafLValue(member, loc, writeback);
  // A byte-array union arm (CTS-F, 00210) exists only at the type level:
  // the union admitted on its integer slot, but no access through the
  // array arm can be modeled on that one slot.
  if (unionByteArrayArms.contains(field))
    return emitError(loc) << "unsupported: union byte-array arm access";
  // A data-pointer member has no place of its own (its stored i64
  // carries no information); reads resolve through the static binding
  // in `emitPointerRValue` and writes through `emitMemberPointerAssign` —
  // except an admitted `void *` fn-ptr member (CTS-BR, 00216), whose
  // place is the retyped fn_ptr field.
  if (isDataPointer(field->getType()) && !fnPtrMemberTypes.count(field))
    return emitError(loc) << "unsupported use of pointer struct member '"
                          << field->getName() << "'";
  // A bit-field member's storage is a window of a synthesized backing
  // field, not a place: supported reads and simple assignments were
  // intercepted before any lvalue was requested (`emitBitFieldRead`,
  // `emitBitFieldAssign`); anything else reaching here (compound
  // assignment, increment/decrement, ...) is out of the C99-45 scope.
  if (field->isBitField())
    return emitError(loc) << "unsupported: bit-field member '"
                          << field->getName() << "' in this context";
  FailureOr<Value> base = emitMemberBasePlace(member, loc, writeback);
  if (failed(base))
    return failure();
  Value basePlace = *base;
  // C11 6.7.2.1p13: an anonymous member's fields were flattened into
  // the parent struct_def (see `collectRecordFields`), so the implicit
  // intermediate access Sema synthesizes for `parent.leaf` designates
  // the parent place itself; the leaf below then selects its flattened
  // (possibly union-slot-aliased) name on that place.
  if (field->isAnonymousStructOrUnion())
    return basePlace;
  // A union arm designates its storage slot: the member selects the
  // slot's name at the slot's type. A pun arm's (differently-signed
  // integer, or float over an integer slot and vice versa) bit-exact
  // reinterpretation happens at the load or store site (see
  // `reinterpretUnionArmRead`/`reinterpretUnionArmWrite`). An admitted
  // `void *` fn-ptr member reads and writes at its retyped fn_ptr type.
  clang::QualType storageType = flattenedFieldStorage(field)->getType();
  if (clang::QualType retyped = fnPtrMemberTypes.lookup(field);
      !retyped.isNull())
    storageType = retyped;
  FailureOr<Type> fieldType = mapType(storageType, loc);
  if (failed(fieldType))
    return failure();
  return builder
      .create<emitrust::MemberOp>(
          loc, emitrust::LValueType::get(*fieldType), basePlace,
          builder.getStringAttr(flattenedFieldName(field)))
      .getResult();
}

FailureOr<Value>
CImporter::emitMemberBasePlace(const clang::MemberExpr *member, Location loc,
                               GlobalWriteback *writeback) {
  Value basePlace;
  const clang::CallExpr *erasedCall = nullptr;
  const clang::VarDecl *erasedBase =
      member->isArrow()
          ? erasedGlobalReturnCallBase(member->getBase(), &erasedCall)
          : nullptr;
  if (erasedBase) {
    // `f()->m` through an erased single-global-base pointer return
    // (CTS-S, 00089): the call is retained for its side effects (it
    // yields no value), and the access routes to the base global
    // through the ordinary staged-copy + writeback machinery — no
    // runtime pointer state exists in the caller.
    FailureOr<Value> effects = emitCall(erasedCall);
    if (failed(effects))
      return failure();
    if (!lookupGlobal(erasedBase))
      return emitError(loc)
             << "unsupported: global '" << erasedBase->getName()
             << "' backing an erased pointer return was not imported";
    FailureOr<Value> staged =
        stageGlobalCopyAndRecord(loc, erasedBase, writeback);
    if (failed(staged))
      return failure();
    basePlace = *staged;
  } else if (member->isArrow() && isDecomposedPointerExpr(member->getBase())) {
    // `p->f` through a decomposed pointer: resolve the pointer to its
    // place (the base object itself, or an element of the base array)
    // and refine it with the member access below.
    FailureOr<PtrExprValue> pointer = emitPointerRValue(member->getBase());
    if (failed(pointer))
      return failure();
    FailureOr<Type> pointeeType = mapType(
        member->getBase()->getType().getCanonicalType()->getPointeeType(), loc);
    if (failed(pointeeType))
      return failure();
    FailureOr<Value> place =
        emitPointerPlace(loc, *pointer, *pointeeType, writeback);
    if (failed(place))
      return failure();
    basePlace = *place;
  } else if (member->isArrow()) {
    FailureOr<Value> base = emitRValue(member->getBase());
    if (failed(base))
      return failure();
    Type pointee;
    if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>((*base).getType()))
      pointee = mutRef.getPointee();
    else if (auto sharedRef =
                 llvm::dyn_cast<emitrust::RefType>((*base).getType()))
      pointee = sharedRef.getPointee();
    else
      return emitError(loc)
             << "unsupported: '->' base is not a supported pointer";
    basePlace = builder
                    .create<emitrust::DerefOp>(
                        loc, emitrust::LValueType::get(pointee), *base)
                    .getResult();
  } else if (const clang::CallExpr *call = [&]() -> const clang::CallExpr * {
               // `f().m`: clang wraps the struct-returning call's result
               // in a MaterializeTemporaryExpr; peel it to the call.
               const clang::Expr *base = stripTrivia(member->getBase());
               if (const auto *materialize =
                       llvm::dyn_cast<clang::MaterializeTemporaryExpr>(base))
                 base = stripTrivia(materialize->getSubExpr());
               return llvm::dyn_cast<clang::CallExpr>(base);
             }()) {
    // A member access on a struct-returning call's result (CTS 00204).
    // The call value has no place of its own, so it materializes into a
    // temporary variable whose member is then read like any other place.
    FailureOr<Value> value = emitCall(call);
    if (failed(value))
      return failure();
    if (!*value || !llvm::isa<emitrust::StructType>((*value).getType()))
      return emitError(loc)
             << "unsupported: member access on a non-struct call result";
    Value temp = createVariablePlace(loc, (*value).getType());
    builder.create<emitrust::AssignOp>(loc, temp, *value);
    basePlace = temp;
  } else {
    FailureOr<Value> base = emitLValue(member->getBase(), writeback);
    if (failed(base))
      return failure();
    basePlace = *base;
  }
  auto baseType = llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
  if (!baseType || !llvm::isa<emitrust::StructType>(baseType.getValueType()))
    return emitError(loc) << "unsupported member access base";
  return basePlace;
}

Value CImporter::createBitFieldMask(Location loc, IntegerType backingType,
                                    unsigned width, unsigned offset,
                                    bool complement) {
  llvm::APInt mask =
      llvm::APInt::getLowBitsSet(backingType.getWidth(), width);
  if (complement)
    mask = ~mask.shl(offset);
  return builder
      .create<emitrust::ConstantOp>(loc, backingType,
                                    IntegerAttr::get(backingType, mask))
      .getResult();
}

FailureOr<Value> CImporter::emitBitFieldRead(const clang::MemberExpr *member,
                                             Location loc) {
  const auto *field = llvm::cast<clang::FieldDecl>(member->getMemberDecl());
  auto info = bitFieldAccessInfo.find(field);
  // Defensive: every imported struct recorded its bit-field runs, so a
  // missing entry means the parent record never imported successfully.
  if (info == bitFieldAccessInfo.end())
    return emitError(loc) << "unsupported: bit-field member '"
                          << field->getName() << "' has no accessor";
  const BitFieldAccess access = info->second;
  FailureOr<Value> base = emitMemberBasePlace(member, loc, /*writeback=*/
                                              nullptr);
  if (failed(base))
    return failure();
  Value backingPlace =
      builder
          .create<emitrust::MemberOp>(
              loc, emitrust::LValueType::get(access.backingType), *base,
              builder.getStringAttr(access.backingName))
          .getResult();
  Value word =
      builder.create<emitrust::LoadOp>(loc, access.backingType, backingPlace)
          .getResult();
  // The offset shift is ALWAYS emitted, offset 0 included: the accessor's
  // shape is uniform (pinned by bitfields.c), and the folder may tidy it.
  Value shiftAmount =
      createScalarIntConstant(loc, access.backingType, access.offset);
  Value shifted =
      builder
          .create<emitrust::ShrOp>(loc, access.backingType, word, shiftAmount)
          .getResult();
  Value widthMask = createBitFieldMask(loc, access.backingType, access.width,
                                       access.offset, /*complement=*/false);
  Value masked =
      builder
          .create<emitrust::AndOp>(loc, access.backingType, shifted, widthMask)
          .getResult();
  return convertBitFieldFieldValue(loc, masked, field);
}

FailureOr<Value> CImporter::convertBitFieldFieldValue(
    Location loc, Value masked, const clang::FieldDecl *field) {
  const BitFieldAccess &access = bitFieldAccessInfo.find(field)->second;
  FailureOr<Type> mapped = mapType(field->getType(), loc);
  if (failed(mapped))
    return failure();
  // An enum-typed field converts straight from the unsigned backing type,
  // so the conversion ZERO-extends regardless of the enum's own underlying
  // signedness (pinned: an `enum : 8` field holding 152 reads back 152,
  // never -104 — the pin is on the FIELD being enum-typed).
  if (llvm::isa<emitrust::EnumType>(*mapped))
    return builder.create<emitrust::CastOp>(loc, *mapped, masked).getResult();
  auto intType = llvm::dyn_cast<IntegerType>(*mapped);
  if (!intType)
    return emitError(loc) << "unsupported: bit-field member type";
  // A _Bool field: Rust has no `as bool`, so the (zero-extended) masked
  // value compares against zero — exact for a value already confined to
  // the field's width.
  if (intType.getWidth() == 1) {
    Value zero = createScalarIntConstant(loc, access.backingType, 0);
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                 emitrust::CmpPredicate::ne, masked, zero)
        .getResult();
  }
  // Unsigned fields zero-extend into their mapped type.
  if (intType.isUnsigned()) {
    if (intType == masked.getType())
      return masked;
    return builder.create<emitrust::CastOp>(loc, intType, masked).getResult();
  }
  // Plain-int signed fields sign-extend from their declared width in the
  // mapped signed type: shl then shrsi by (type width - field width), a
  // shift pair the Rust emitter renders on the signed type (`>>` on iN is
  // arithmetic). For `int s : 4` the amount is the pinned 28.
  Value value =
      builder.create<emitrust::CastOp>(loc, intType, masked).getResult();
  Value amount = createIntConstant(
      loc, intType, static_cast<int64_t>(intType.getWidth() - access.width));
  Value extended =
      builder.create<arith::ShLIOp>(loc, value, amount).getResult();
  return builder.create<arith::ShRSIOp>(loc, extended, amount).getResult();
}

FailureOr<Value> CImporter::emitBitFieldAssign(const clang::MemberExpr *member,
                                               const clang::Expr *rhs,
                                               Location loc,
                                               bool refreshStaged,
                                               bool wantValue) {
  const auto *field = llvm::cast<clang::FieldDecl>(member->getMemberDecl());
  auto info = bitFieldAccessInfo.find(field);
  if (info == bitFieldAccessInfo.end())
    return emitError(loc) << "unsupported: bit-field member '"
                          << field->getName() << "' has no accessor";
  const BitFieldAccess access = info->second;
  GlobalWriteback writeback;
  FailureOr<Value> base = emitMemberBasePlace(member, loc, &writeback);
  if (failed(base))
    return failure();
  Value backingPlace =
      builder
          .create<emitrust::MemberOp>(
              loc, emitrust::LValueType::get(access.backingType), *base,
              builder.getStringAttr(access.backingName))
          .getResult();
  FailureOr<Value> value = emitRValue(rhs);
  if (failed(value))
    return failure();
  Value fieldValue;
  auto mutate = [&]() -> LogicalResult {
    Value word =
        builder.create<emitrust::LoadOp>(loc, access.backingType, backingPlace)
            .getResult();
    // Clear the field's window with the complement mask.
    Value clearMask = createBitFieldMask(loc, access.backingType, access.width,
                                         access.offset, /*complement=*/true);
    Value cleared =
        builder
            .create<emitrust::AndOp>(loc, access.backingType, word, clearMask)
            .getResult();
    // Convert the assigned value to the backing type (from the enum type
    // for an enum-typed RHS; Rust `as` wraps, matching C's conversion to
    // the unsigned backing).
    Value raw = *value;
    if (raw.getType() != access.backingType)
      raw = builder.create<emitrust::CastOp>(loc, access.backingType, raw)
                .getResult();
    // Truncate to the declared width — ALWAYS a separate `and`, even for
    // statically in-range values (pinned by bitfields.c; the folder may
    // tidy it).
    Value widthMask = createBitFieldMask(loc, access.backingType, access.width,
                                         access.offset, /*complement=*/false);
    Value truncated =
        builder
            .create<emitrust::AndOp>(loc, access.backingType, raw, widthMask)
            .getResult();
    // Shift onto the window — ALWAYS emitted, offset 0 included.
    Value shiftAmount =
        createScalarIntConstant(loc, access.backingType, access.offset);
    Value shifted = builder
                        .create<emitrust::ShlOp>(loc, access.backingType,
                                                 truncated, shiftAmount)
                        .getResult();
    Value merged =
        builder
            .create<emitrust::OrOp>(loc, access.backingType, cleared, shifted)
            .getResult();
    builder.create<emitrust::AssignOp>(loc, backingPlace, merged);
    if (!wantValue)
      return success();
    // C's value of the assignment is the post-store FIELD value: the
    // truncated bits convert exactly like a read at offset 0.
    FailureOr<Value> converted =
        convertBitFieldFieldValue(loc, truncated, field);
    if (failed(converted))
      return failure();
    fieldValue = *converted;
    return success();
  };
  if (failed(commitGlobalWriteback(loc, writeback, refreshStaged, mutate)))
    return failure();
  if (!wantValue)
    return Value();
  Value staged = createVariablePlace(loc, fieldValue.getType());
  builder.create<emitrust::AssignOp>(loc, staged, fieldValue);
  return staged;
}

FailureOr<Value>
CImporter::emitSubscriptLValue(const clang::ArraySubscriptExpr *subscript,
                               Location loc, GlobalWriteback *writeback) {
  // CTS-BR (00216): a subscript rooted in a byte-region aggregate is a
  // subscript of the region base at the accumulated byte offset.
  if (exprRootsInByteRegion(subscript))
    return emitByteRegionLeafLValue(subscript, loc, writeback);
  const clang::Expr *base = subscript->getBase()->IgnoreParenImpCasts();
  if (!base->getType().getCanonicalType()->isArrayType()) {
    // Subscript through a pointer: decompose it into (base, cursor) and
    // subscript the base object at cursor+index. A subscripted pointer
    // parameter classifies as a slice and decomposes like a local; the
    // rejection below is a defensive guard for scalar-reference
    // parameters, which classification keeps out of subscript contexts.
    if (!isDecomposedPointerExpr(subscript->getBase()))
      return emitError(loc) << "unsupported: subscript on a pointer "
                               "parameter";
    // `emitSubscriptPointer` folds the (row-scaled) index into the flat
    // cursor; a degenerate base (the address of a scalar) admits only
    // the constant-zero subscript and resolves to the object itself.
    FailureOr<PtrExprValue> pointer = emitSubscriptPointer(subscript);
    if (failed(pointer))
      return failure();
    FailureOr<Type> pointeeType = mapType(subscript->getType(), loc);
    if (failed(pointeeType))
      return failure();
    return emitPointerPlace(loc, *pointer, *pointeeType, writeback);
  }
  // A subscript directly on a string literal (`"abc"[i]`, C99-28) reads
  // through the literal's read-only backing array — the same cached const
  // backing a `char *` binding to the literal uses (CTS-P1), so a bound
  // pointer and a direct subscript over one literal share storage. Writes
  // into the backing are rejected in `storeToPlace` (writing a C string
  // literal is UB). `__func__` element access stays rejected (C99-29).
  FailureOr<Value> basePlace = failure();
  if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(base)) {
    if (!literal->isOrdinary())
      return emitError(loc)
             << "unsupported: subscript of a non-ordinary string literal";
    basePlace = getOrCreateLiteralBacking(literal, loc);
  } else {
    basePlace = emitLValue(base, writeback);
  }
  if (failed(basePlace))
    return failure();
  auto baseType = llvm::dyn_cast<emitrust::LValueType>((*basePlace).getType());
  if (!baseType)
    return emitError(loc) << "unsupported subscript base";
  auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(baseType.getValueType());
  if (!arrayType)
    return emitError(loc) << "unsupported subscript base";
  FailureOr<Value> index = emitRValue(subscript->getIdx());
  if (failed(index))
    return failure();
  return builder
      .create<emitrust::SubscriptOp>(
          loc, emitrust::LValueType::get(arrayType.getElementType()),
          *basePlace, *index)
      .getResult();
}

FailureOr<Value> CImporter::emitDerefLValue(const clang::UnaryOperator *unary,
                                            Location loc,
                                            GlobalWriteback *writeback) {
  // Dereferencing a `void *` without a reinterpret-back cast (a GNU
  // extension) never names an element type at all (CTS-P9).
  if (unary->getType().getCanonicalType()->isVoidType() &&
      isPointerType(unary->getSubExpr()->getType()))
    return emitError(loc) << "unsupported: dereference of a 'void *' pointer";
  // A dereference of a decomposed pointer resolves to a place on its
  // base object; the pointer-parameter reference path is unchanged.
  // Direct pun casts (`*(T *)(char *)...`, CTS-P11) are stripped for
  // the decomposition and folded into the reinterpret type check.
  const clang::Expr *pointerExpr =
      stripObjectPointerCasts(astContext(), unary->getSubExpr());
  if (isDecomposedPointerExpr(pointerExpr)) {
    FailureOr<PtrExprValue> decomposed = emitPointerRValue(pointerExpr);
    if (failed(decomposed))
      return failure();
    // A reinterpret-back site `*(T *)p` (the pointer expression peeled
    // through a pointee-changing cast, CTS-P9/P11) type-checks T
    // against the region's base element type: an exact match lowers
    // like a direct pointer, a same-width integer view resolves the
    // place at the base element type (the load/store sites wrap the
    // value in an `emitrust.cast` bitcast), a wider view over a byte
    // region is intercepted upstream as a ne_bytes pun, and
    // everything else is rejected.
    clang::QualType accessType = unary->getType();
    if (viewsChangedPointee(astContext(), unary->getSubExpr())) {
      std::optional<clang::QualType> element =
          regionElementType(*decomposed, accessType);
      if (element &&
          !astContext().hasSameUnqualifiedType(accessType, *element)) {
        bool sameWidthIntView =
            accessType->isIntegerType() && (*element)->isIntegerType() &&
            !accessType->isBooleanType() && !(*element)->isBooleanType() &&
            astContext().getTypeSize(accessType) ==
                astContext().getTypeSize(*element);
        if (!sameWidthIntView)
          return emitError(loc)
                 << "unsupported: pointer cast reinterprets the pointee "
                    "('"
                 << accessType.getCanonicalType()
                        .getUnqualifiedType()
                        .getAsString()
                 << "' over '"
                 << element->getCanonicalType()
                        .getUnqualifiedType()
                        .getAsString()
                 << "' storage)";
        accessType = *element;
      }
    }
    FailureOr<Type> pointeeType = mapType(accessType, loc);
    if (failed(pointeeType))
      return failure();
    return emitPointerPlace(loc, *decomposed, *pointeeType, writeback);
  }
  FailureOr<Value> pointer = emitRValue(unary->getSubExpr());
  if (failed(pointer))
    return failure();
  Type pointee;
  if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>((*pointer).getType()))
    pointee = mutRef.getPointee();
  else if (auto sharedRef =
               llvm::dyn_cast<emitrust::RefType>((*pointer).getType()))
    pointee = sharedRef.getPointee();
  else
    return emitError(loc) << "unsupported dereference base";
  return builder
      .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(pointee),
                                 *pointer)
      .getResult();
}

Value CImporter::loadPlace(Location loc, Value place) {
  if (llvm::isa<MemRefType>(place.getType()))
    return builder.create<memref::LoadOp>(loc, place, ValueRange())
        .getResult();
  auto lvalueType = llvm::cast<emitrust::LValueType>(place.getType());
  return builder
      .create<emitrust::LoadOp>(loc, lvalueType.getValueType(), place)
      .getResult();
}

LogicalResult CImporter::storeToPlace(Location loc, Value place, Value value) {
  // A place rooted (through any subscript chain) at a `const`-marked
  // variable is a string-literal backing — the only const-marked
  // variables the importer creates (C99-28/CTS-P1). Writing a C string
  // literal is undefined behavior, and the backing renders as an
  // immutable Rust binding, so every write path (assignment, compound
  // assignment, ++/--) keeps a located rejection here.
  Value root = place;
  while (auto subscript = root.getDefiningOp<emitrust::SubscriptOp>())
    root = subscript.getOperand(0);
  if (auto variable = root.getDefiningOp<emitrust::VariableOp>())
    if (variable.getIsConst())
      return emitError(loc) << "unsupported: write into a string literal";
  if (auto memrefType = llvm::dyn_cast<MemRefType>(place.getType())) {
    if (value.getType() != memrefType.getElementType())
      return emitError(loc)
             << "unsupported: assigned value type does not match the variable";
    builder.create<memref::StoreOp>(loc, value, place);
    return success();
  }
  auto lvalueType = llvm::cast<emitrust::LValueType>(place.getType());
  if (value.getType() != lvalueType.getValueType())
    return emitError(loc)
           << "unsupported: assigned value type does not match the place";
  builder.create<emitrust::AssignOp>(loc, place, value);
  return success();
}

FailureOr<Value> CImporter::extendBool(Location loc, Value flag,
                                       clang::QualType type) {
  FailureOr<Type> target = mapType(type, loc);
  if (failed(target))
    return failure();
  if (*target == flag.getType())
    return flag;
  return builder.create<arith::ExtUIOp>(loc, *target, flag).getResult();
}

Location CImporter::firstSymbolUseLoc(llvm::StringRef symbol,
                                      Location fallback) {
  std::optional<SymbolTable::UseRange> uses = SymbolTable::getSymbolUses(
      StringAttr::get(module.getContext(), symbol), module.getOperation());
  if (uses)
    for (SymbolTable::SymbolUse use : *uses)
      return use.getUser()->getLoc();
  return fallback;
}

LogicalResult CImporter::finalizeProject() {
  // Every deferred `extern` global that was referenced must have a real
  // definition in some translation unit; the Rust program otherwise reads
  // an undefined symbol. Unreferenced extern declarations were skipped at
  // import (referenced-only policy), so a pending entry without a
  // definition is rejected at its first use site (falling back to the
  // declaration when no IR use survives).
  for (const auto &entry : pendingExternGlobals)
    if (!SymbolTable::lookupSymbolIn(module, entry.getKey()))
      return emitError(firstSymbolUseLoc(entry.getKey(), entry.getValue()))
             << "unsupported: extern global variable '" << entry.getKey()
             << "' is referenced but not defined in any translation unit";

  // No referenced non-variadic external function may remain body-less: the
  // Rust emitter cannot emit a body-less function. (Variadic prototypes such
  // as printf were never added to the module.) An external func whose symbol
  // ended up with no uses (e.g. a prototype referenced only in an
  // unevaluated context) demands no definition and is erased instead.
  for (func::FuncOp func :
       llvm::make_early_inc_range(module.getOps<func::FuncOp>()))
    if (func.isExternal()) {
      if (SymbolTable::symbolKnownUseEmpty(func.getOperation(),
                                           module.getOperation())) {
        func.erase();
        continue;
      }
      return emitError(firstSymbolUseLoc(func.getSymName(), func.getLoc()))
             << "unsupported: function '" << func.getSymName()
             << "' is referenced but not defined in any translation unit";
    }
  return success();
}

//===----------------------------------------------------------------------===//
// Entry points
//===----------------------------------------------------------------------===//

namespace {

/// Loads the dialects the importer produces into `context`.
void loadImportDialects(MLIRContext &context) {
  context.loadDialect<emitrust::EmitRustDialect, func::FuncDialect,
                      arith::ArithDialect, memref::MemRefDialect,
                      cf::ControlFlowDialect>();
}

/// Assembles the clang command line shared by every import path: `-std=c11`,
/// then clang's builtin `-resource-dir` (needed for system headers such as
/// `<stdint.h>`) taken from the `EMITRUST_RESOURCE_DIR` environment variable
/// or, failing that, the compile-time `EMITRUST_CLANG_RESOURCE_DIR` macro when
/// defined, and finally the caller's extra arguments in order.
std::vector<std::string>
buildCommandLine(llvm::ArrayRef<std::string> extraClangArgs) {
  // C89-era programs (the c-testsuite corpus, e.g. 00144's
  // `q = i ? 0 : 0`) assign integer expressions to pointers, which clang
  // >= 15 hard-errors by default; demote it back to the historical
  // warning — the importer itself classifies integer-to-pointer traffic
  // and rejects the unsupported shapes with located diagnostics.
  std::vector<std::string> commandLine{"-std=c11",
                                       "-Wno-error=int-conversion"};
  std::string resourceDir;
  if (const char *env = std::getenv("EMITRUST_RESOURCE_DIR"))
    resourceDir = env;
#ifdef EMITRUST_CLANG_RESOURCE_DIR
  if (resourceDir.empty())
    resourceDir = EMITRUST_CLANG_RESOURCE_DIR;
#endif
  if (!resourceDir.empty())
    commandLine.push_back("-resource-dir=" + resourceDir);
  commandLine.insert(commandLine.end(), extraClangArgs.begin(),
                     extraClangArgs.end());
  return commandLine;
}

} // namespace

OwningOpRef<ModuleOp>
mlir::emitrust::importC(llvm::StringRef path,
                        llvm::ArrayRef<std::string> extraClangArgs,
                        MLIRContext &context) {
  loadImportDialects(context);

  // Imperative shell: parse the file with clang. Parse diagnostics are
  // printed to stderr by clang's own diagnostic machinery.
  std::vector<std::string> commandLine = buildCommandLine(extraClangArgs);
  clang::tooling::FixedCompilationDatabase compilations(".", commandLine);
  std::vector<std::string> sources{path.str()};
  clang::tooling::ClangTool tool(compilations, sources);
  std::vector<std::unique_ptr<clang::ASTUnit>> asts;
  int status = tool.buildASTs(asts);
  if (asts.size() != 1 || !asts.front()) {
    emitError(UnknownLoc::get(&context))
        << "failed to parse C input '" << path << "'";
    return nullptr;
  }
  clang::ASTUnit &ast = *asts.front();
  if (status != 0 || ast.getDiagnostics().hasErrorOccurred())
    return nullptr;

  // Functional core: translate the AST into a fresh module.
  Location moduleLoc =
      FileLineColLoc::get(StringAttr::get(&context, path), /*line=*/1,
                          /*column=*/1);
  OwningOpRef<ModuleOp> module(ModuleOp::create(moduleLoc));
  CImporter importer(*module);
  if (failed(importer.importTranslationUnit(ast.getASTContext(),
                                            /*tuTag=*/"",
                                            /*deferExtern=*/false,
                                            /*soleTranslationUnit=*/true)))
    return nullptr;

  // A verifier failure indicates an importer bug; it is still an import
  // failure and must never yield unverified IR.
  if (failed(verify(*module)))
    return nullptr;
  return module;
}

OwningOpRef<ModuleOp> mlir::emitrust::importC(llvm::StringRef path,
                                              MLIRContext &context) {
  return importC(path, /*extraClangArgs=*/{}, context);
}

OwningOpRef<ModuleOp>
mlir::emitrust::importCProject(llvm::ArrayRef<std::string> paths,
                               llvm::ArrayRef<std::string> extraClangArgs,
                               MLIRContext &context) {
  loadImportDialects(context);
  if (paths.empty()) {
    emitError(UnknownLoc::get(&context)) << "no C input files given";
    return nullptr;
  }

  // Imperative shell: parse every source as an independent translation unit.
  std::vector<std::string> commandLine = buildCommandLine(extraClangArgs);
  clang::tooling::FixedCompilationDatabase compilations(".", commandLine);
  std::vector<std::string> sources(paths.begin(), paths.end());
  clang::tooling::ClangTool tool(compilations, sources);
  std::vector<std::unique_ptr<clang::ASTUnit>> asts;
  int status = tool.buildASTs(asts);
  if (asts.size() != paths.size()) {
    emitError(UnknownLoc::get(&context))
        << "failed to parse one or more C inputs";
    return nullptr;
  }
  for (const std::unique_ptr<clang::ASTUnit> &ast : asts)
    if (!ast || ast->getDiagnostics().hasErrorOccurred())
      return nullptr;
  if (status != 0)
    return nullptr;

  // Functional core: merge every AST into one module with shared cross-TU
  // dedup and extern-resolution state. All ASTs stay alive for the whole
  // import so their decl pointers remain valid.
  Location moduleLoc =
      FileLineColLoc::get(StringAttr::get(&context, paths.front()),
                          /*line=*/1, /*column=*/1);
  OwningOpRef<ModuleOp> module(ModuleOp::create(moduleLoc));
  CImporter importer(*module);
  for (auto [index, ast] : llvm::enumerate(asts)) {
    std::string tuTag = ("tu" + llvm::Twine(index) + "_").str();
    if (failed(importer.importTranslationUnit(
            ast->getASTContext(), tuTag,
            /*deferExtern=*/true,
            /*soleTranslationUnit=*/asts.size() == 1)))
      return nullptr;
  }
  if (failed(importer.finalizeProject()))
    return nullptr;

  if (failed(verify(*module)))
    return nullptr;
  return module;
}
