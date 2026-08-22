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
/// W2.0 opens a narrow C++ INPUT subset on top of the C11 pipeline above:
/// a source whose extension marks it as C++ (.cpp/.cc/.cxx/.C/.c++/.hpp,
/// see `isCxxSourcePath`) parses under `-x c++ -std=c++17` instead of
/// `-std=c11` (per-input selection, `PerFileCompilationDatabase`), and the
/// translation-unit decl walk (`importDeclsIn`) recurses into `namespace`
/// and `extern "C"` bodies as if their members were declared at the
/// enclosing level, with namespace membership flattened into the emitted
/// symbol name (`namespacePrefix`, `ns_<name>_` per level; `extern "C"`
/// keeps the bare C name). A `class` with no base classes imports its
/// data members exactly like a `struct` (member functions are never
/// visited by this walk, so they are silently absent rather than
/// rejected — full member-function support is a later wave); a class or
/// struct WITH base classes is rejected instead of silently dropping
/// inherited data (`collectRecordFields`). C++ references have no
/// representation and reject with a dedicated message (`mapType`).
/// `true`/`false` map to the same i1 constant a C `_Bool` literal would.
/// Every other C++-only construct (templates, virtual dispatch, multiple
/// inheritance, overloading, exceptions, ...) is untouched and falls
/// through to whatever existing generic rejection applies (usually
/// "unsupported top-level declaration" or "unsupported statement: ...").
///
/// FR-45 adds a second way to obtain those command lines: instead of the
/// synthesized `PerFileCompilationDatabase`, both entry points accept the
/// path of a real `compile_commands.json` (or of the directory holding
/// one) and take each translation unit's flags — include paths, macro
/// definitions, and above all its LANGUAGE — from the recorded entry.
/// Everything downstream of `clang::tooling::CompilationDatabase` is
/// shared: the database is the single seam (see
/// `makeCompilationDatabase`), so no parallel import path exists and a
/// file the database does not mention still falls back to the extension
/// guess.
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

#include "EmitRust/ClangProjectParser.h"

#include "clang/Tooling/JSONCompilationDatabase.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// PointerRegionAnalysis
//===----------------------------------------------------------------------===//

void PointerRegionAnalysis::analyze(clang::ASTContext &astContext,
                                    const clang::Stmt *body) {
  context = &astContext;
  analyzedBody = body;
  parent.clear();
  regions.clear();
  pointerVars.clear();
  secondOrderVars.clear();
  secondOrderRegions.clear();
  consumedAddrOf.clear();
  memberFacts.clear();
  poisonedFields.clear();
  visit(body);
  analyzedBody = nullptr;
  context = nullptr;
}

// A local is foldable only if the analyzed body never mutates it (`=`,
// compound assign, `++`/`--`) nor takes its address. The scan is a bounded
// walk of the function body (W4.2e Part A).
static bool bodyMutatesOrEscapes(const clang::Stmt *stmt,
                                 const clang::VarDecl *var) {
  if (!stmt)
    return false;
  if (const auto *bin = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
    if (bin->isAssignmentOp())
      if (const clang::VarDecl *lhs = asLocalVarRef(bin->getLHS()))
        if (lhs == var)
          return true;
  } else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
    if (unary->isIncrementDecrementOp() || unary->getOpcode() == clang::UO_AddrOf)
      if (const clang::VarDecl *sub = asLocalVarRef(unary->getSubExpr()))
        if (sub == var)
          return true;
  }
  for (const clang::Stmt *child : stmt->children())
    if (bodyMutatesOrEscapes(child, var))
      return true;
  return false;
}

bool PointerRegionAnalysis::isFoldableLocal(const clang::VarDecl *var) const {
  if (!var || !var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var) ||
      !var->getInit())
    return false;
  clang::Expr::EvalResult result;
  if (!var->getInit()->EvaluateAsInt(result, *context) ||
      result.Val.getInt().isNegative())
    return false;
  return !bodyMutatesOrEscapes(analyzedBody, var);
}

bool PointerRegionAnalysis::evalFoldableInt(const clang::Expr *expr,
                                            uint64_t &out) const {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    e = stripTrivia(cast->getSubExpr());
  // A genuine integer-constant expression (literals, `sizeof`, const vars).
  clang::Expr::EvalResult result;
  if (e->EvaluateAsInt(result, *context)) {
    if (result.Val.getInt().isNegative())
      return false;
    out = result.Val.getInt().getZExtValue();
    return true;
  }
  // A reference to a foldable automatic local folds to its initializer.
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (isFoldableLocal(var))
        return evalFoldableInt(var->getInit(), out);
  // Arithmetic over foldable operands (the `cap * sizeof(int)` idiom).
  if (const auto *bin = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    uint64_t lhs = 0, rhs = 0;
    if (!evalFoldableInt(bin->getLHS(), lhs) ||
        !evalFoldableInt(bin->getRHS(), rhs))
      return false;
    switch (bin->getOpcode()) {
    case clang::BO_Add:
      out = lhs + rhs;
      return true;
    case clang::BO_Sub:
      if (rhs > lhs)
        return false;
      out = lhs - rhs;
      return true;
    case clang::BO_Mul:
      out = lhs * rhs;
      return true;
    case clang::BO_Div:
      if (rhs == 0)
        return false;
      out = lhs / rhs;
      return true;
    case clang::BO_Rem:
      if (rhs == 0)
        return false;
      out = lhs % rhs;
      return true;
    default:
      return false;
    }
  }
  return false;
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
void mergeRegionFacts(PointerRegion &target,
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
  // FR-94: an allocation binding a flexible-array-member record that the
  // owned-tail recognition did NOT claim (the re-binding form, a calloc, an
  // unmatched size shape, a gap layout, a non-u8 tail) must never fall
  // through to the element-divisibility path below: a divisible size would
  // silently keep the historical OVER-COUNTED backing (`sizeof(S)+32` on a
  // 2-byte record synthesized a 17-record array — tail bytes became extra
  // whole records). The branch sits BEFORE `elementBytes` on purpose.
  if (const clang::RecordDecl *record = pointee->getAsRecordDecl();
      record && record->getDefinition() &&
      record->getDefinition()->hasFlexibleArrayMember())
    return markInvalid(ptr, loc,
                       "unsupported: unrecognized allocation of a "
                       "flexible-array-member record");
  uint64_t elementBytes =
      context->getTypeSizeInChars(pointee).getQuantity();
  auto evalConstant = [&](const clang::Expr *arg, uint64_t &out) {
    // Fold references to foldable automatic locals (W4.2e Part A) so the
    // `malloc(cap * sizeof(int))` idiom resolves; `EvaluateAsInt` alone
    // rejects it because `cap` is not a C constant expression.
    return evalFoldableInt(arg, out);
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
  // An allocation cannot join an object, string-literal, or carrier base
  // into one region (W4.2e Part A): the region owns a synthesized backing
  // exclusively (the reverse order is rejected in `recordPointerWrite`).
  if (!region.bases.empty() || region.literalBase || region.hasCarrierSource)
    return markInvalid(ptr, loc,
                       "unsupported: allocation joined with an object, "
                       "string-literal, or carrier base into one pointer "
                       "region");
  if (region.allocSite && region.allocSite != call)
    return markInvalid(ptr, loc,
                       "unsupported: pointer bound to multiple allocations");
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
  // FR-64: a recognized constant-fill string local is lifted whole to
  // `String::repeat`, so its `malloc`/`calloc` binding is not modeled as a
  // flat heap backing — leave the region untracked entirely (the emitter
  // routes the local to its `String` binding and never consults this region).
  if (stringValueLocalQuery && stringValueLocalQuery(ptr))
    return;
  // FR-65: a recognized runtime-sized heap buffer is lifted whole to
  // `Vec<T>` (`vec![<zero>; n]`), so its `malloc`/`calloc` binding never
  // reaches `recordAllocBase`'s compile-time-constant size gate — leave the
  // region untracked (the emitter routes the local to its `Vec` binding).
  if (vecValueLocalQuery && vecValueLocalQuery(ptr))
    return;
  // FR-94: a recognized FAM-record owned-tail local is lifted whole to an
  // owned struct with a `Vec<u8>` tail, so its binding never reaches
  // `recordAllocBase`'s FAM rejection — leave the region untracked (the
  // emitter routes the local to its owned struct binding).
  if (famValueLocalQuery && famValueLocalQuery(ptr))
    return;
  // FR-96: a recognized member-read local over a lifted member-held FAM
  // field (`struct hs_index *hsi = hse->search_index`) re-projects the
  // member's Option payload at every use — the region model never claims
  // it (the emitter binds no code for the declaration at all).
  if (famMemberLocalQuery && famMemberLocalQuery(ptr))
    return;
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

  // `g = calloc(n, sizeof(T))` / `g = malloc(bytes)`: a constant-size
  // allocation binding. A global pointer promotes to a synthesized global
  // backing array (CTS-P4); a local pointer synthesizes an entry-block
  // mutable backing array plus a cursor cell (W4.2e Part A).
  // `recordAllocBase` enforces the flat-buffer predicate and rejects a
  // straddle with an object/literal/carrier base.
  if (const clang::CallExpr *alloc = asAllocCall(e))
    return recordAllocBase(ptr, alloc, loc);
  // `realloc` has no representation in the fixed-backing model: the
  // synthesized backing array cannot resize (W4.2e Part A). Reject it with
  // a dedicated located diagnostic rather than the generic straddle below.
  {
    const clang::Expr *callExpr = e;
    while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(callExpr))
      callExpr = stripTrivia(cast->getSubExpr());
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(callExpr))
      if (const clang::FunctionDecl *callee = call->getDirectCallee())
        if (!callee->hasBody() && callee->getIdentifier() &&
            callee->getName() == "realloc")
          return markInvalid(ptr, loc,
                             "unsupported: realloc is not part of the "
                             "supported allocation model (the fixed backing "
                             "cannot resize)");
  }
  // A non-allocation address source after an allocation binding straddles
  // the two models: the region already owns a synthesized backing, so it
  // cannot also decompose against an object, literal, or carrier base
  // (W4.2e Part A; the reverse order is rejected in `recordAllocBase`).
  if (regionFor(ptr).allocSite)
    return markInvalid(ptr, loc,
                       "unsupported: allocation joined with an object, "
                       "string-literal, or carrier base into one pointer "
                       "region");

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
    if (const clang::FunctionDecl *callee = call->getDirectCallee()) {
      if (carrierReturnQuery && carrierReturnQuery(callee))
        return recordCarrierSource(ptr, loc);
      // `p = f(...)` on a promoted owner method proven to return an
      // i64 element index into the same class as its own pointer
      // parameter(s) (Stage 1, design.md FR-30 follow-on): the call is a
      // region source exactly like copying from one of the callee's own
      // pointer parameters. Every data-pointer parameter of such a method
      // shares one class (planOwners' all-or-nothing per-function rule),
      // so the FIRST one re-classifies `p`'s region identically to
      // whichever the callee's own return-site resolution used.
      if (ownerIndexReturnQuery && ownerIndexReturnQuery(callee)) {
        for (unsigned i = 0,
                      n = std::min<unsigned>(callee->getNumParams(),
                                             call->getNumArgs());
             i != n; ++i) {
          const clang::ParmVarDecl *param = callee->getParamDecl(i);
          if (isPointerType(param->getType()) &&
              !isFunctionPointer(param->getType()))
            return recordPointerWrite(ptr, call->getArg(i));
        }
        return markInvalid(ptr, loc,
                           "unsupported: owner-index-returning call has no "
                           "pointer argument to root the result at");
      }
    }

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
      // `p = x->field` on an array-member self-referential pointer field
      // (Stage 4 of the owner-struct self-reference extension, design.md
      // FR-30 follow-on, B3 — e.g. `parent = node->parent;`): the field
      // always decodes to a cursor value inside the SAME owner class `x`
      // itself roots into (Pass A's own per-field proof, consulted here
      // through `arrayMemberFieldQuery`), so `p` joins that class exactly
      // like copying from `x` directly — a parameter arrow base becomes
      // the region base (mirroring the `p = param` case above), a tracked
      // local arrow base unions the two regions (mirroring the `p = q`
      // case above). The actual field decode is deferred entirely to
      // emission (`emitArrayMemberPointerRead`); this only has to get the
      // destination's OWN region right. Any other arrow-base shape (a
      // nested member access, an untracked local, ...) is Stage 4+ scope
      // and falls through to the non-address rejection below.
      if (arrayMemberFieldQuery) {
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
                stripTrivia(cast->getSubExpr()));
            member && member->isArrow()) {
          if (const auto *field =
                  llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
              field && arrayMemberFieldQuery(field)) {
            if (const clang::ParmVarDecl *param =
                    asPointerParamRef(member->getBase()))
              return addBase(ptr, param, loc);
            if (const clang::VarDecl *source =
                    asLoadedLocalVarRef(member->getBase()))
              if (tracks(source))
                return unite(ptr, source);
          }
        }
      }
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
      // `p = s->arr` / `p = s.arr` (FR-93): a MEMBER-array decay binds a
      // member-place backing — a TYPED single-link chain roots the region
      // at (root, field) with a member-relative cursor, and a BYTE-REGION
      // chain (FR-91) roots it at the region ROOT itself (field null),
      // whose flat byte image the absolute byte cursor walks. Placed
      // BEFORE the subscript peel: a decayed member ROW (`p = s->mat[i]`)
      // must keep the historical rejection, not silently drop its row
      // offset. Unclassified member shapes (nested typed chains, global
      // roots, union arms) fall through to the rejection below unchanged.
      if (memberArrayDecayQuery)
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(sub))
          if (auto target = memberArrayDecayQuery(member))
            return addBase(ptr, target->first, loc, target->second);
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
        // FR-94: `p = &d->tail[k]` over an ADMITTED FAM tail (heatshrink's
        // poll-site `uint8_t *buf = &hsd->buffers[ibs]`) binds the member
        // Vec place as a cursored backing at cursor k — the FR-93 member
        // convention with a nonzero initial cursor. Gated on the FAM leaf
        // so constant-extent member arrays keep their pinned frontier.
        if (const auto *memberExpr = llvm::dyn_cast<clang::MemberExpr>(base))
          if (famTailMemberQuery && famTailMemberQuery(memberExpr) &&
              memberArrayDecayQuery)
            if (auto target = memberArrayDecayQuery(memberExpr);
                target && target->second)
              return addBase(ptr, target->first, loc, target->second);
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
          // FR-96: a RECOGNIZED write to a lift-candidate member-held FAM
          // field (the planned alloc form or the null constant) is owned
          // by the Option-member machinery — neither a poison nor a
          // per-instance member fact. Any unrecognized write falls through
          // and poisons, vetoing the lift program-wide.
          if (famMemberWriteQuery &&
              famMemberWriteQuery(field, binary->getRHS())) {
            // Owned by the FR-96 Option-member lowering.
          } else {
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
    // A `&e` argument feeding a Shape-P paired out-cursor parameter
    // (C99-43 slice 1b) is likewise consumed, and `e` JOINS the paired
    // co-argument expression's region: the callee hands back a cursor
    // into the region the co-argument roots, exactly as `e = <co-arg>`
    // followed by arithmetic would. This join is what lets a
    // possibly-uninitialized `e` classify (and `e - base` emit) after
    // the call.
    if (callee && pairedArgQuery) {
      for (unsigned index = 0, count = call->getNumArgs(); index < count;
           ++index) {
        int coIndex = pairedArgQuery(callee, index);
        if (coIndex < 0 ||
            static_cast<unsigned>(coIndex) >= call->getNumArgs())
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
        recordPointerWrite(pointer, call->getArg(coIndex));
        recordArithmetic(pointer, addrOf->getOperatorLoc());
      }
    }
    // A `&p` argument feeding a Shape-G single-global-or-NULL out-param
    // cursor (C99-43 C1) is likewise consumed. The callee hands back an
    // Option<i64> offset into its plan's statically-known global, so
    // that global binds as `p`'s region base — the caller's later reads
    // (`p[i]`, `*p`) resolve into its backing through the CTS-P6
    // staged-copy machinery — and a null-writing callee marks the
    // region nullable, giving `p` the CTS-P8 flag cell its `if (p)`
    // tests read. A pure-NULL callee (no global) contributes only the
    // nullable fact.
    if (callee && globalCursorArgQuery) {
      for (unsigned index = 0, count = call->getNumArgs(); index < count;
           ++index) {
        std::optional<GlobalCursorPlan> plan =
            globalCursorArgQuery(callee, index);
        if (!plan)
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
        if (plan->global)
          addBase(pointer, plan->global, addrOf->getOperatorLoc());
        if (plan->writesNull)
          recordNullable(pointer, addrOf->getOperatorLoc());
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

bool CImporter::isArrayMemberOwnerRoot(const clang::VarDecl *root,
                                       const clang::VarDecl *ownerArray) const {
  if (!root)
    return false;
  if (root == ownerArray)
    return true;
  const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(root);
  if (!param)
    return false;
  const auto *owningFn =
      llvm::dyn_cast_if_present<clang::FunctionDecl>(param->getDeclContext());
  if (!owningFn)
    return false;
  auto methodIt = methodPlans.find(owningFn->getCanonicalDecl());
  return methodIt != methodPlans.end() && methodIt->second == ownerArray;
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
  // Stage 2 of the owner-struct self-reference extension: a field Pass A
  // (`planArrayMemberPointers`) proved usable is consulted BEFORE the
  // historical per-instance static-binding model, so every existing test
  // of a field it could not prove (absent here) sees byte-identical
  // output.
  if (const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl())) {
    auto arrayIt = arrayMemberPtrBindings.find(field);
    if (arrayIt != arrayMemberPtrBindings.end() &&
        arrayIt->second.invalidReason.empty())
      return emitArrayMemberPointerAssign(member, field, arrayIt->second, rhs,
                                          loc);
    // W4.2e Part B (FR-39): a node-pool self-ref field write builds the
    // field's `Option<usize>` from the right-hand handle's (non-null, index)
    // pair.
    if (member->isArrow() && poolNextFields.contains(field->getCanonicalDecl()))
      return emitPoolNextFieldAssign(member, rhs, loc);
    // FR-96: a LIFTED member-held FAM field write — `None` for the null
    // constant, `Some(temp record)` for the planned alloc form. A poisoned
    // field never reaches this arm and keeps the member-wall rejection
    // below.
    if (famOptionMemberPointee(field))
      return emitFamOptionMemberAssign(member, field, rhs, loc);
  }
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

FailureOr<Type> CImporter::getOrCreateArrayMemberEnumType(
    const clang::FieldDecl *field, ArrayMemberPointerFacts &facts,
    Location loc) {
  if (facts.enumSymbol.empty()) {
    std::string symbol = emitrust::typeRustName(
        (llvm::Twine(field->getParent()->getName()) + "_" + field->getName() +
         "_Bases")
            .str());
    if (SymbolTable::lookupSymbolIn(module, symbol))
      return emitError(loc)
             << "unsupported: array-member-pointer enum name '" << symbol
             << "' collides with an existing symbol";
    // One variant per array index: the field's storage IS the index by
    // construction, so a read never needs to branch (`castEnumToI32`) and
    // a write's `emitrust.switch` case regions assign these constants
    // 1:1 with the case values.
    llvm::SmallVector<std::string, 8> variantNameStorage;
    llvm::SmallVector<int64_t, 8> variantValues;
    variantNameStorage.reserve(facts.elementCount);
    variantValues.reserve(facts.elementCount);
    for (unsigned index = 0; index < facts.elementCount; ++index) {
      variantNameStorage.push_back(("E" + llvm::Twine(index)).str());
      variantValues.push_back(index);
    }
    llvm::SmallVector<llvm::StringRef, 8> variantNames;
    for (const std::string &name : variantNameStorage)
      variantNames.push_back(name);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::EnumDefOp>(
        loc, moduleBuilder.getStringAttr(symbol),
        moduleBuilder.getStrArrayAttr(variantNames),
        moduleBuilder.getDenseI64ArrayAttr(variantValues),
        /*unsigned_underlying=*/true);
    facts.enumSymbol = symbol;
  }
  return Type(emitrust::EnumType::get(builder.getContext(), facts.enumSymbol));
}

FailureOr<PtrExprValue> CImporter::emitArrayMemberPointerRead(
    const clang::MemberExpr *member, const clang::FieldDecl *field,
    ArrayMemberPointerFacts &facts, Location loc) {
  if (!member->isArrow()) // Defensive; Pass A only proves arrow-form sites.
    return emitError(loc) << "unsupported: pointer struct member '"
                          << field->getName() << "' read in dot form";
  // Resolved directly (mirroring `emitMemberBasePlace`'s decomposed-`->`
  // branch) rather than through `emitMemberBasePlace` itself, because the
  // arrow base's OWN resolved base — a method's own pointer parameter, not
  // `facts.ownerArray` — is exactly the base identity a plain read of that
  // same base expression already uses elsewhere in this function; reusing
  // it (instead of the fixed `facts.ownerArray`) is what lets `p->self`
  // and `p` compare equal (`emitComparison` requires `lhs->base ==
  // rhs->base` by literal identity, and each owner method's parameter is
  // registered as its own base — see `isArrayMemberOwnerRoot`'s doc).
  FailureOr<PtrExprValue> arrowBase = emitPointerRValue(member->getBase());
  if (failed(arrowBase))
    return failure();
  FailureOr<Type> pointeeType = mapType(
      member->getBase()->getType().getCanonicalType()->getPointeeType(), loc);
  if (failed(pointeeType))
    return failure();
  FailureOr<Value> basePlace =
      emitPointerPlace(loc, *arrowBase, *pointeeType, /*writeback=*/nullptr);
  if (failed(basePlace))
    return failure();
  FailureOr<Type> enumType = getOrCreateArrayMemberEnumType(field, facts, loc);
  if (failed(enumType))
    return failure();
  Value fieldPlace = builder
                          .create<emitrust::MemberOp>(
                              loc, emitrust::LValueType::get(*enumType),
                              *basePlace, builder.getStringAttr(
                                              flattenedFieldName(field)))
                          .getResult();
  Value enumValue = loadPlace(loc, fieldPlace);
  Value asI32 = castEnumToI32(loc, enumValue);
  Value index = castToIntType(loc, asI32, builder.getIntegerType(64));
  return PtrExprValue{arrowBase->base, index};
}

LogicalResult CImporter::emitArrayMemberPointerAssign(
    const clang::MemberExpr *member, const clang::FieldDecl *field,
    ArrayMemberPointerFacts &facts, const clang::Expr *rhs, Location loc) {
  if (!member->isArrow()) // Defensive; Pass A only proves arrow-form sites.
    return emitError(loc) << "unsupported: pointer struct member '"
                          << field->getName() << "' assigned in dot form";
  FailureOr<Value> basePlace =
      emitMemberBasePlace(member, loc, /*writeback=*/nullptr);
  if (failed(basePlace))
    return failure();
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  // `value->base` is the DECLARATION a promoted pointer's decomposition
  // roots at, which for a method's own pointer parameter is the parameter
  // itself (see `importFunction`'s owner-region-pointer-parameter
  // prologue, which registers `pointerLocals[param]` with the parameter
  // as its own base), not the owner array — `isArrayMemberOwnerRoot` is
  // the same class-membership test Pass A already ran on this exact
  // right-hand side, so this is a defensive re-check, not new analysis.
  if (!isArrayMemberOwnerRoot(value->base, facts.ownerArray) ||
      !value->cursor || value->member || value->literalBacking)
    return emitError(loc) // Defensive; Pass A pins the right-hand shape.
           << "unsupported: pointer struct member assigned this value";
  FailureOr<Type> enumType = getOrCreateArrayMemberEnumType(field, facts, loc);
  if (failed(enumType))
    return failure();
  auto enumTypeValue = llvm::cast<emitrust::EnumType>(*enumType);
  Value fieldPlace = builder
                          .create<emitrust::MemberOp>(
                              loc, emitrust::LValueType::get(*enumType),
                              *basePlace, builder.getStringAttr(
                                              flattenedFieldName(field)))
                          .getResult();
  // Encodes the i64 index as a genuine Rust `match` (one arm per array
  // element, exhaustive) rather than a bare transmute, so the enum's
  // closed variant set stays visibly exhaustive at every write site.
  SmallVector<int64_t> caseValues;
  caseValues.reserve(facts.elementCount);
  for (unsigned index = 0; index < facts.elementCount; ++index)
    caseValues.push_back(index);
  auto switchOp = builder.create<emitrust::SwitchOp>(
      loc, value->cursor, builder.getDenseI64ArrayAttr(caseValues),
      facts.elementCount);
  auto assignVariant = [&](unsigned index) {
    std::string path =
        (llvm::Twine(facts.enumSymbol) + "::E" + llvm::Twine(index)).str();
    Value variant =
        builder
            .create<emitrust::ConstantOp>(
                loc, enumTypeValue,
                emitrust::OpaqueAttr::get(builder.getContext(), path))
            .getResult();
    builder.create<emitrust::AssignOp>(loc, fieldPlace, variant);
    builder.create<emitrust::YieldOp>(loc);
  };
  for (unsigned index = 0; index < facts.elementCount; ++index) {
    builder.createBlock(&switchOp.getCaseRegions()[index]);
    assignVariant(index);
  }
  // The default region is provably unreachable — Pass A proved every
  // write's index is one of `facts.elementCount` array elements — and
  // assigns E0 to keep every region's block well-formed, mirroring
  // `IndexSwitchLowering`'s default-region convention for a discriminator
  // whose value set is closed by construction.
  builder.createBlock(&switchOp.getDefaultRegion());
  assignVariant(0);
  builder.setInsertionPointAfter(switchOp);
  return success();
}

void CImporter::collectCrossTuVaListVariadics(clang::ASTContext &context) {
  // Pre-scan (W3.0): set the AST context so `isSystemHeaderDecl` and
  // `mlirFuncName` resolve against THIS TU; `importTranslationUnit` sets it
  // again to the same value before its own real pass over this same AST.
  astContextPtr = &context;
  const clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!func || !func->isThisDeclarationADefinition() || !func->hasBody() ||
        !func->isVariadic() || !func->isExternallyVisible())
      continue;
    if (bodyUsesVaList(context, func->getBody()))
      crossTuVaListVariadicNames.insert(mlirFuncName(func));
  }
}

namespace {
/// A minimal, memoization-free scalar type mapper for the whole-program
/// array-completion pre-scan (`collectWholeProgramInfo`): mirrors
/// `mapType`'s plain `clang::BuiltinType` switch exactly (same widths,
/// same signedness-to-MLIR-unsigned mapping), returning a null `Type` for
/// anything else. Deliberately narrower than `mapType`: it must never
/// touch `isByteRegionRecord`/`isByteRegionAggregate` (whose memoized
/// classification is only sound once this TU's own Pass-A has run) or
/// emit a diagnostic (this is a speculative fact-gathering probe, not a
/// real import step).
Type mapSpeculativeScalarType(OpBuilder &builder, clang::ASTContext &context,
                              clang::QualType type) {
  const auto *builtin = llvm::dyn_cast<clang::BuiltinType>(type.getTypePtr());
  if (!builtin)
    return Type();
  switch (builtin->getKind()) {
  case clang::BuiltinType::Bool:
    return builder.getI1Type();
  case clang::BuiltinType::Char_S:
  case clang::BuiltinType::SChar:
    return builder.getIntegerType(8);
  case clang::BuiltinType::Short:
    return builder.getIntegerType(16);
  case clang::BuiltinType::Int:
    return builder.getIntegerType(32);
  // FR-56 target-width `long`, mirroring `mapType` (see its comment).
  case clang::BuiltinType::Long:
    return builder.getIntegerType(context.getTypeSize(type));
  case clang::BuiltinType::LongLong:
    return builder.getIntegerType(64);
  case clang::BuiltinType::Float:
    return builder.getF32Type();
  case clang::BuiltinType::Double:
  case clang::BuiltinType::LongDouble:
    return builder.getF64Type();
  case clang::BuiltinType::Char_U:
  case clang::BuiltinType::UChar:
    return IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  case clang::BuiltinType::UShort:
    return IntegerType::get(builder.getContext(), 16, IntegerType::Unsigned);
  case clang::BuiltinType::UInt:
    return IntegerType::get(builder.getContext(), 32, IntegerType::Unsigned);
  case clang::BuiltinType::ULong:
    return IntegerType::get(builder.getContext(), context.getTypeSize(type),
                            IntegerType::Unsigned);
  case clang::BuiltinType::ULongLong:
    return IntegerType::get(builder.getContext(), 64, IntegerType::Unsigned);
  default:
    return Type();
  }
}

/// Recursively maps a (possibly multi-dimensional) constant-size array of
/// `mapSpeculativeScalarType`-eligible scalars to `!emitrust.array`,
/// bottoming out at the innermost scalar element; returns null for
/// anything else (an array of records, pointers, enums, byte-region
/// aggregates, ...), which is exactly the "not speculatively safe to map"
/// signal `collectWholeProgramInfo` needs.
Type mapSpeculativeArrayType(OpBuilder &builder, clang::ASTContext &context,
                             clang::QualType type) {
  if (const clang::ConstantArrayType *array =
          context.getAsConstantArrayType(type)) {
    Type element =
        mapSpeculativeArrayType(builder, context, array->getElementType());
    if (!element)
      return Type();
    return emitrust::ArrayType::get(
        builder.getContext(), array->getSize().getZExtValue(), element);
  }
  return mapSpeculativeScalarType(builder, context, type);
}
} // namespace

void CImporter::collectWholeProgramInfo(clang::ASTContext &context,
                                        unsigned tuIndex) {
  // Pre-scan (W3.2): set the AST context so `mlirFuncName`,
  // `globalVarSymbolName`, and `isSystemHeaderDecl` resolve against THIS TU
  // (`importTranslationUnit` sets it again to the same value for its own real
  // pass over this AST). Only externally visible symbols are recorded, whose
  // names are tag-free, so no per-TU tag is needed here.
  astContextPtr = &context;
  const clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();

  // The externally visible global object an address expression binds to, as a
  // module symbol name, or empty: peels a leading address-of and any
  // element/member selections down to the object root, then requires an
  // externally visible global. Handles `&g`, `&g[i]`, `&g.m`, and a bare
  // global lvalue (an array-decay operand). Internal-linkage globals and
  // locals return empty (they cannot cross a TU boundary).
  auto addressBoundGlobal = [&](const clang::Expr *expr) -> std::string {
    const clang::Expr *cur = expr ? expr->IgnoreParenImpCasts() : nullptr;
    if (const auto *unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(cur))
      if (unary->getOpcode() == clang::UO_AddrOf)
        cur = unary->getSubExpr()->IgnoreParenImpCasts();
    for (;;) {
      if (const auto *sub =
              llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(cur)) {
        cur = sub->getBase()->IgnoreParenImpCasts();
        continue;
      }
      if (const auto *mem = llvm::dyn_cast_or_null<clang::MemberExpr>(cur)) {
        cur = mem->getBase()->IgnoreParenImpCasts();
        continue;
      }
      break;
    }
    const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(cur);
    if (!ref)
      return {};
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (!var || !var->hasGlobalStorage() || !var->isExternallyVisible())
      return {};
    return globalVarSymbolName(var);
  };

  // Records a write (assignment, increment) or escape (address-of) of an
  // externally visible function-pointer global into `fnPtrGlobalsWritten`.
  auto markFnPtrWrite = [&](const clang::Expr *expr) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
    if (!ref)
      return;
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (var && var->hasGlobalStorage() && var->isExternallyVisible() &&
        var->getType().getCanonicalType()->isFunctionPointerType())
      wholeProgram.fnPtrGlobalsWritten.insert(globalVarSymbolName(var));
  };

  // Records a data-pointer global's binding base (`g = &base…`) into
  // `pointerGlobalBases`, given an assignment's LHS and RHS. Every call site
  // of this lambda is a BODY-level reassignment (the file-scope initializer
  // is handled separately below), so it also flags
  // `pointerGlobalHasBodyRebind`: a global pointer reassigned anywhere in
  // the project has a runtime, not compile-time, cursor, which rules it out
  // of the static cross-TU reconstruction below regardless of how many
  // distinct bases it ends up bound to.
  auto recordPointerGlobalBinding = [&](const clang::Expr *lhs,
                                        const clang::Expr *rhs) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(lhs));
    if (!ref)
      return;
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (!var || !var->hasGlobalStorage() || !var->isExternallyVisible())
      return;
    clang::QualType type = var->getType().getCanonicalType();
    if (!type->isPointerType() || type->isFunctionPointerType())
      return;
    std::string symbol = globalVarSymbolName(var);
    wholeProgram.pointerGlobalHasBodyRebind.insert(symbol);
    std::string base = addressBoundGlobal(rhs);
    if (!base.empty())
      wholeProgram.pointerGlobalBases[symbol].insert(base);
  };

  // Records the flat i64 cursor start of `var`'s file-scope initializer
  // into `pointerGlobalSoleFileScopeBase`/`pointerGlobalCursorStart`, when
  // it is the single supported "real global object base" shape (CTS-P4
  // shape 3: `T *g = &base[i];` or `T *g = &base;`, no member projection,
  // an element-boundary offset) -- the narrow cross-TU reconstruction fact
  // `deferExternGlobal` (W3.2 COMMIT B) consumes for the shared pointer
  // global fix. Anything else (a literal/allocation-backed shape, a
  // member-rooted binding, a non-element-boundary offset, or a mismatched
  // pointee type) records nothing, so `deferExternGlobal` keeps the
  // historical unconditional rejection for those shapes.
  auto recordPointerGlobalFileScopeDetail = [&](const clang::VarDecl *var) {
    const clang::APValue *value = var->evaluateValue();
    if (!value || !value->isLValue() || value->isNullPointer())
      return;
    const auto *baseDecl =
        value->getLValueBase().dyn_cast<const clang::ValueDecl *>();
    const auto *baseVar =
        baseDecl ? llvm::dyn_cast<clang::VarDecl>(baseDecl) : nullptr;
    // Only an externally visible base is eagerly re-importable by its bare
    // symbol name in `deferExternPointerGlobal` (W3.2 COMMIT B): an
    // internal-linkage base would need the DEFINING TU's own per-TU
    // mangling tag, which this whole-program, tag-free fact set does not
    // track.
    if (!baseVar || baseVar->hasLocalStorage() ||
        !baseVar->isExternallyVisible())
      return;
    clang::QualType pointee =
        var->getType().getCanonicalType()->getPointeeType();
    clang::QualType baseType = baseVar->getType().getCanonicalType();
    int64_t byteOffset = value->getLValueOffset().getQuantity();
    int64_t cursorStart = 0;
    if (const clang::ConstantArrayType *array =
            context.getAsConstantArrayType(baseType)) {
      bool matchesLevel = false;
      for (const clang::ConstantArrayType *level = array; level;
           level = context.getAsConstantArrayType(level->getElementType())) {
        if (context.hasSameUnqualifiedType(pointee, level->getElementType())) {
          matchesLevel = true;
          break;
        }
      }
      if (!matchesLevel)
        return;
      clang::QualType innermost = context.getBaseElementType(baseType);
      int64_t innerBytes = context.getTypeSizeInChars(innermost).getQuantity();
      if (innerBytes <= 0 || byteOffset < 0 || byteOffset % innerBytes != 0)
        return;
      cursorStart = byteOffset / innerBytes;
    } else {
      if (byteOffset != 0 ||
          !context.hasSameUnqualifiedType(pointee, baseType))
        return;
    }
    std::string symbol = globalVarSymbolName(var);
    wholeProgram.pointerGlobalSoleFileScopeBase[symbol] =
        baseVar->getCanonicalDecl();
    wholeProgram.pointerGlobalCursorStart[symbol] = cursorStart;
  };

  // Records `tuIndex` into a per-symbol TU list, deduplicated.
  auto recordTu = [&](llvm::SmallVectorImpl<unsigned> &tus) {
    if (!llvm::is_contained(tus, tuIndex))
      tus.push_back(tuIndex);
  };

  // FR-83 (the FR-82 fold prerequisite): a `&` IMMEDIATELY cancelled by
  // `->` or unary `*` — `(&g)->f`, `(*(&g)).f`, lwIP's macro-expanded
  // `((T*)&ip_data)->...` after the cast peels — folds to a plain member
  // access at emission and never materializes an address, so it must not
  // mark the global address-taken (the mark is what disqualifies the
  // FR-81 requirement model). The scan visits parents before children, so
  // the cancelling node records its `&` here and the `UO_AddrOf` case
  // below skips exactly that set; a real address-take still marks.
  llvm::SmallPtrSet<const clang::Stmt *, 8> derefCancelledAddrOf;

  // One statement/expression subtree: enumerates direct external calls,
  // externally visible function address-takings, explicit address-of of
  // externally visible globals, fn-ptr global writes/escapes, and
  // data-pointer-global rebindings. The callee of a direct call is NOT a
  // value use of the function, so it is not recursed into (mirroring
  // `planFnPtrAliases`); every other `DeclRefExpr` naming a function is an
  // address-taking use.
  auto scan = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    // FR-83: note a `&` this node cancels (see `derefCancelledAddrOf`).
    auto noteCancelledAddrOf = [&](const clang::Expr *base) {
      if (const auto *inner = llvm::dyn_cast_or_null<clang::UnaryOperator>(
              base ? base->IgnoreParenImpCasts() : nullptr))
        if (inner->getOpcode() == clang::UO_AddrOf)
          derefCancelledAddrOf.insert(inner);
    };
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt))
      if (member->isArrow())
        noteCancelledAddrOf(member->getBase());
    if (const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(stmt))
      if (deref->getOpcode() == clang::UO_Deref)
        noteCancelledAddrOf(deref->getSubExpr());
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      if (const auto *callee = call->getDirectCallee()) {
        if (callee->isExternallyVisible() && !isSystemHeaderDecl(callee))
          recordTu(wholeProgram.calleeToCallerTus[mlirFuncName(callee)]);
      } else {
        self(self, call->getCallee());
      }
      for (const clang::Expr *argument : call->arguments())
        self(self, argument);
      return;
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
      if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
        if (fn->isExternallyVisible() && !isSystemHeaderDecl(fn)) {
          recordTu(wholeProgram.fnAddressTakenTus[mlirFuncName(fn)]);
          // G1 candidate-completeness fact: when the address-taken function
          // returns a data pointer, record its canonical return-type spelling
          // → this TU, so `classifyFnPtrPointerResult` can tell whether its
          // per-TU candidate set is the whole-program set. Spelling key only
          // (no `mapType`) keeps this side-effect-free and cross-TU-stable.
          clang::QualType returnType =
              fn->getReturnType().getCanonicalType();
          if (returnType->isPointerType() &&
              !returnType->isFunctionPointerType())
            recordTu(wholeProgram.dataPtrReturnFnAddressTakenTus
                         [returnType.getAsString()]);
        }
      return;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
      if (unary->getOpcode() == clang::UO_AddrOf) {
        // FR-83: a deref-cancelled `&` (recorded by its `->`/`*` parent
        // above) folds away at emission — skip the address-taken mark for
        // it, and it alone.
        if (!derefCancelledAddrOf.contains(unary)) {
          std::string taken = addressBoundGlobal(unary);
          if (!taken.empty())
            wholeProgram.addressTakenGlobals.insert(taken);
        }
        markFnPtrWrite(unary->getSubExpr());
      }
      if (unary->isIncrementDecrementOp())
        markFnPtrWrite(unary->getSubExpr());
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt))
      if (binary->isAssignmentOp()) {
        markFnPtrWrite(binary->getLHS());
        recordPointerGlobalBinding(binary->getLHS(), binary->getRHS());
      }
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };

  // FR-77: the pre-scan ran, so `wholeProgram.definedFunctions` below is the
  // authoritative project-wide definition set and the use-site
  // undefined-target gate in `resolveFunctionPointerDecl` may consult it.
  wholeProgram.prescanRan = true;

  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->hasBody() && func->getDefinition() == func) {
        // FR-77: record the definition fact for the whole-program
        // undefined-fn-ptr-target gate. Externally visible only — the one
        // linkage that can satisfy another TU's reference — so the name is
        // tag-free, matching `resolveFunctionPointerDecl`'s query.
        if (func->isExternallyVisible())
          wholeProgram.definedFunctions.insert(mlirFuncName(func));
        scan(scan, func->getBody());
      }
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      // A data-pointer global's file-scope initializer is a binding too
      // (`int *g = &arr[0];`); the initializer scan below also records the
      // `&arr` address-taken fact.
      if (var->hasGlobalStorage() && var->isExternallyVisible()) {
        clang::QualType type = var->getType().getCanonicalType();
        if (type->isPointerType() && !type->isFunctionPointerType())
          if (const clang::Expr *init = var->getAnyInitializer()) {
            std::string base = addressBoundGlobal(init);
            if (!base.empty())
              wholeProgram.pointerGlobalBases[globalVarSymbolName(var)].insert(
                  base);
            recordPointerGlobalFileScopeDetail(var);
          }
        // Extern-array composite merge (W3.2 COMMIT B): record this TU's
        // COMPLETE mapped type for a bounded array definition, so
        // `deferExternGlobal` can resolve another TU's `extern int a[];`
        // (whose own type is incomplete and unmappable) against it. Only a
        // COMPLETE array records here (never overwriting with an
        // incomplete sighting). This deliberately does NOT call the real
        // `mapType`: `mapType`'s array case consults
        // `isByteRegionAggregate`/`isByteRegionRecord`, whose memoized
        // classification is only sound once THIS TU's own Pass-A
        // (`collectDeclTypeRecords`/`planFnPtrMembers`) has run -- calling
        // it speculatively here, before any TU's Pass-A runs, poisons that
        // cache for the real import that follows (observed: a spurious
        // 00216.c ledger regression, a `void*` struct member classified
        // inconsistently). `mapSpeculativeArrayType` instead mirrors only
        // `mapType`'s plain-scalar builtin cases, touching no cache; any
        // other element type (records, enums, pointers, byte-region
        // aggregates, ...) leaves this symbol unrecorded, and
        // `deferExternGlobal` keeps the historical rejection for it.
        if (context.getAsConstantArrayType(type))
          if (Type mapped = mapSpeculativeArrayType(builder, context, type))
            wholeProgram.completeArrayGlobalTypes[globalVarSymbolName(var)] =
                mapped;
      }
      if (const clang::Expr *init = var->getInit())
        scan(scan, init);
    }
  }
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
  // The historical CTS-P11 base is a C char array (signless i8, gathered
  // through a u8 cast per byte); an FR-83 blob base is already ui8 and
  // needs no cast.
  IntegerType byteType =
      access.elementType ? access.elementType : builder.getIntegerType(8);
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
        byteType == u8Type
            ? byte
            : builder.create<emitrust::CastOp>(loc, u8Type, byte).getResult();
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
  IntegerType byteType =
      access.elementType ? access.elementType : builder.getIntegerType(8);
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
        byteType == u8Type
            ? byte
            : builder.create<emitrust::CastOp>(loc, byteType, byte)
                  .getResult();
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

bool CImporter::isOpaqueArmScalarLeaf(const clang::Expr *expr) const {
  const clang::Expr *e = expr ? expr->IgnoreParenImpCasts() : nullptr;
  if (!e)
    return false;
  // Only a plain INTEGER scalar leaf has the ne_bytes/byte-subscript
  // image: `_Bool` converts by zero-comparison (not truncation), an enum
  // maps to its own item type, and a `_BitInt`'s storage size need not
  // match its mapped width. Floats stay out (the helpers are emitted for
  // integer widths only; measured lwIP demand is u32/u8).
  clang::QualType leafType = e->getType().getCanonicalType();
  if (!leafType->isIntegerType() || leafType->isBooleanType() ||
      leafType->isEnumeralType() || leafType->isBitIntType())
    return false;
  // Walk leaf-inward: dot members and array-typed subscripts only, ending
  // at an opaque-union ARM selection (the arm itself may be dot or arrow —
  // its base is the union, resolved by the ordinary member-base
  // machinery). Any other link (an arrow through an interior pointer, a
  // bit-field, a call) is not a pure layout projection of the blob.
  for (const clang::Expr *cur = e;;) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(cur)) {
      const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (!field || field->isBitField())
        return false;
      if (opaqueUnionArms.contains(field))
        return true;
      if (member->isArrow())
        return false;
      cur = member->getBase()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(cur)) {
      const clang::Expr *base = subscript->getBase()->IgnoreParenImpCasts();
      if (!base->getType().getCanonicalType()->isArrayType())
        return false;
      cur = base;
      continue;
    }
    return false;
  }
}

FailureOr<CImporter::WideByteAccess>
CImporter::resolveOpaqueArmByteView(const clang::Expr *expr, Location loc,
                                    GlobalWriteback *writeback) {
  // Collect the projection chain leaf-first down to (and including) the
  // ARM member; the classifier proved the shapes, so the casts hold.
  llvm::SmallVector<const clang::Expr *, 4> chain;
  const clang::MemberExpr *armMember = nullptr;
  for (const clang::Expr *cur = expr->IgnoreParenImpCasts(); !armMember;) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(cur)) {
      chain.push_back(member);
      const auto *field =
          llvm::cast<clang::FieldDecl>(member->getMemberDecl());
      if (opaqueUnionArms.contains(field)) {
        armMember = member;
        break;
      }
      cur = member->getBase()->IgnoreParenImpCasts();
      continue;
    }
    const auto *subscript = llvm::cast<clang::ArraySubscriptExpr>(cur);
    chain.push_back(subscript);
    cur = subscript->getBase()->IgnoreParenImpCasts();
  }
  const auto *armField =
      llvm::cast<clang::FieldDecl>(armMember->getMemberDecl());
  // The union's place resolves through the ordinary member-base machinery
  // (dot on an lvalue, arrow through a pointer, staged global copies with
  // their writeback), then projects to the blob field — the byte array
  // sized by C sizeof of the union, exactly as `collectUnionSlot` built
  // the struct_def.
  FailureOr<Value> unionPlace = emitMemberBasePlace(armMember, loc, writeback);
  if (failed(unionPlace))
    return failure();
  const clang::RecordDecl *unionDecl = armField->getParent();
  uint64_t blobBytes =
      astContext()
          .getTypeSizeInChars(astContext().getRecordType(unionDecl))
          .getQuantity();
  auto u8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  auto blobType =
      emitrust::ArrayType::get(builder.getContext(), blobBytes, u8Type);
  Value blobPlace = builder
                        .create<emitrust::MemberOp>(
                            loc, emitrust::LValueType::get(blobType),
                            *unionPlace, builder.getStringAttr("opaque"))
                        .getResult();
  // Accumulate the leaf's byte offset arm-outward: clang's ASTRecordLayout
  // gives each member's authoritative offset, and a subscript adds its
  // element-size-scaled index (constant-folded when it evaluates, a
  // runtime i64 term otherwise) — the byte-region walk's precedent
  // (`resolveByteRegionRef`).
  int64_t constOff = 0;
  Value dynOff;
  IntegerType cursorType = builder.getIntegerType(64);
  for (const clang::Expr *link : llvm::reverse(chain)) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(link)) {
      const auto *field =
          llvm::cast<clang::FieldDecl>(member->getMemberDecl());
      constOff += static_cast<int64_t>(
          astContext()
              .getASTRecordLayout(field->getParent())
              .getFieldOffset(field->getFieldIndex()) /
          8);
      continue;
    }
    const auto *subscript = llvm::cast<clang::ArraySubscriptExpr>(link);
    uint64_t elementSize =
        astContext().getTypeSizeInChars(subscript->getType()).getQuantity();
    clang::Expr::EvalResult eval;
    if (subscript->getIdx()->EvaluateAsInt(eval, astContext()) &&
        !eval.HasSideEffects) {
      constOff += eval.Val.getInt().getSExtValue() *
                  static_cast<int64_t>(elementSize);
      continue;
    }
    FailureOr<Value> index = emitRValue(subscript->getIdx());
    if (failed(index))
      return failure();
    Value index64 = castToIntType(loc, *index, cursorType);
    if (elementSize != 1) {
      Value scale = createIntConstant(loc, cursorType,
                                      static_cast<int64_t>(elementSize));
      index64 = builder.create<arith::MulIOp>(loc, index64, scale).getResult();
    }
    dynOff = dynOff
                 ? builder.create<arith::AddIOp>(loc, dynOff, index64)
                       .getResult()
                 : index64;
  }
  FailureOr<Type> leafType = mapType(expr->getType(), loc);
  if (failed(leafType))
    return failure();
  auto valueType = llvm::dyn_cast<IntegerType>(*leafType);
  uint64_t byteWidth =
      astContext().getTypeSizeInChars(expr->getType()).getQuantity();
  // Defensive: the classifier admits only plain integer leaves, whose
  // mapped type is integral and whose window fits the blob; anything else
  // keeps the arm-access rejection (same wording as the frontier's).
  if (!valueType || byteWidth == 0 ||
      (!dynOff && (constOff < 0 || static_cast<uint64_t>(constOff) +
                                           byteWidth >
                                       blobBytes)))
    return emitError(loc) << "unsupported: opaque union arm access";
  Value cursor;
  if (dynOff && constOff == 0) {
    cursor = dynOff;
  } else {
    cursor = createIntConstant(loc, cursorType, constOff);
    if (dynOff)
      cursor = builder.create<arith::AddIOp>(loc, cursor, dynOff).getResult();
  }
  return WideByteAccess{blobPlace, cursor, valueType,
                        static_cast<unsigned>(byteWidth), u8Type};
}

FailureOr<Value> CImporter::emitOpaqueArmLoad(const WideByteAccess &access,
                                              Location loc) {
  if (access.byteWidth != 1)
    return emitWideByteLoad(access, loc);
  // A one-byte leaf is a single blob subscript; a differently-signed leaf
  // (plain/signed char) reinterprets the u8 bit-exactly.
  auto u8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  Value element = builder
                      .create<emitrust::SubscriptOp>(
                          loc, emitrust::LValueType::get(u8Type),
                          access.basePlace, access.cursor)
                      .getResult();
  Value byte =
      builder.create<emitrust::LoadOp>(loc, u8Type, element).getResult();
  if (byte.getType() != access.valueType)
    byte = builder.create<emitrust::CastOp>(loc, access.valueType, byte)
               .getResult();
  return byte;
}

LogicalResult CImporter::emitOpaqueArmStore(const WideByteAccess &access,
                                            Value value, Location loc) {
  if (access.byteWidth != 1)
    return emitWideByteStore(access, value, loc);
  auto u8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  Value stored = value;
  if (stored.getType() != u8Type)
    stored =
        builder.create<emitrust::CastOp>(loc, u8Type, stored).getResult();
  Value element = builder
                      .create<emitrust::SubscriptOp>(
                          loc, emitrust::LValueType::get(u8Type),
                          access.basePlace, access.cursor)
                      .getResult();
  builder.create<emitrust::AssignOp>(loc, element, stored);
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
  // FR-88: a NULLABLE byte-slice parameter's truth test (`if (p)`, `!p`)
  // is its Option discriminant — never the statically-non-null constant
  // fold below, because `None` call sites are legal for this class. The
  // let-bound i1 keeps the emitted guard clippy-clean.
  if (const clang::ParmVarDecl *param = asPointerParamRef(expr);
      param && nullableByteParams.contains(param)) {
    Value place = symbols.lookup(param);
    return builder
        .create<emitrust::MethodCallOp>(loc, TypeRange{builder.getI1Type()},
                                        place,
                                        builder.getStringAttr("is_some"),
                                        ValueRange{})
        .getResult(0);
  }
  // FR-99: a local bound to a NULLABLE owned-FAM allocator holds its result
  // in an Option temp until the recognized guard resolves it, so its truth
  // test IS the Option discriminant — never the statically-non-null constant
  // fold below, which would silently delete the guard. A test after the temp
  // was unwrapped has no discriminant left to read (the payload is already
  // owned), so it rejects rather than folding.
  if (const clang::VarDecl *bound = famNullableBoundLocal(expr)) {
    Value temp = famOptionTemps.lookup(bound);
    if (!temp)
      return emitError(loc)
             << "unsupported: null test of '"
             << canonicalStreamName(bound->getName())
             << "' outside its binding guard (a nullable "
                "flexible-array-record allocator result is unwrapped at the "
                "guard immediately following the binding)";
    return builder
        .create<emitrust::MethodCallOp>(loc, TypeRange{builder.getI1Type()},
                                        temp,
                                        builder.getStringAttr("is_some"),
                                        ValueRange{})
        .getResult(0);
  }
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
                          << canonicalStreamName(var->getName())
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
  value.backing = info.backing;
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
        // Stage 2 of the owner-struct self-reference extension: a field
        // Pass A (`planArrayMemberPointers`) proved usable is consulted
        // BEFORE the historical per-instance static-binding model below,
        // so every existing test of a field it could not prove (absent
        // here) sees byte-identical output.
        auto arrayIt = arrayMemberPtrBindings.find(field);
        if (arrayIt != arrayMemberPtrBindings.end() &&
            arrayIt->second.invalidReason.empty())
          return emitArrayMemberPointerRead(member, field, arrayIt->second,
                                            loc);
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
      // FR-94: a FAM-record owned-tail local decomposes as the DEGENERATE
      // whole-object pointer to its own struct place — `d->f` resolves as a
      // member of the owned local, borrows pass `&mut d`, and null tests
      // fold statically non-null (the owned binding is infallible).
      if (famAllocLocals.contains(var))
        return PtrExprValue{var, Value()};
      // FR-94: the free-only wrapper's OWNED-BY-VALUE parameter decomposes
      // the same degenerate way, so its member READS (the byte-size
      // computation before the drop) resolve on the by-value shadow place.
      if (const auto *parm = llvm::dyn_cast<clang::ParmVarDecl>(var);
          parm && famOwnedParams.contains(parm))
        return PtrExprValue{var, Value()};
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
                            << canonicalStreamName(var->getName())
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
      // `(*s)++` on a planned cursor parameter (C99-43 slice 1) advances
      // the parameter's cursor cell exactly like `*s = *s + 1`: the
      // parameter's own pointer-local binding carries the cell.
      if (!var)
        if (const clang::ParmVarDecl *cursorParam =
                asPointerPointerParamDeref(unary->getSubExpr());
            cursorParam && cursorParams.contains(cursorParam))
          var = cursorParam;
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
      PtrExprValue result{info.base, unary->isPostfix() ? current : next,
                          info.literalBacking, nonNull, baseIndex,
                          info.multiBases};
      // FR-93: a member-array-backed pointer walks inside its member.
      result.member = info.member;
      result.backing = info.backing;
      return result;
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
      PtrExprValue result{pointer->base, cursor, pointer->literalBacking,
                          pointer->nonNull, pointer->baseIndex,
                          pointer->multiBases};
      // FR-93: a member-array-backed pointer walks inside its member.
      result.member = pointer->member;
      result.backing = pointer->backing;
      return result;
    }
  }

  // `f(...)` on a promoted owner method proven to return an i64 element
  // index into the same class as its own pointer parameter(s) (Stage 1,
  // design.md FR-30 follow-on): the call IS the pointer's (base, cursor)
  // decomposition — its i64 result is the cursor, and its base is whichever
  // region the SAME argument `recordPointerWrite` chose (mirrored here via
  // `emitMethodCallSite`'s single argument-materialization pass, so the
  // argument is evaluated exactly once).
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
    if (const clang::FunctionDecl *callee = call->getDirectCallee()) {
      const clang::FunctionDecl *canonical = callee->getCanonicalDecl();
      if (ownerIndexReturns.contains(canonical)) {
        const clang::VarDecl *ownerBase = methodPlans.lookup(canonical);
        func::FuncOp target = functions.lookup(mlirFuncName(callee));
        if (!ownerBase || !target) // Defensive; every plan pairs the two.
          return emitError(loc)
                 << "unsupported: call to unimported owner-index method";
        const clang::VarDecl *argBase = nullptr;
        FailureOr<Value> result =
            emitMethodCallSite(call, target, ownerBase, loc, &argBase);
        if (failed(result))
          return failure();
        if (!argBase) // Defensive; planOwners requires >=1 pointer param.
          return emitError(loc)
                 << "unsupported: owner-index-returning call has no pointer "
                    "argument to root the result at";
        return PtrExprValue{argBase, *result};
      }
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
      degenerate.backing = pointer->backing;
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
  PtrExprValue result{pointer->base, cursor, pointer->literalBacking,
                      pointer->nonNull, pointer->baseIndex,
                      pointer->multiBases};
  // FR-93: a member-array-backed pointer subscripts inside its member
  // place; the member survives the cursor offset.
  result.member = pointer->member;
  result.backing = pointer->backing;
  return result;
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
  if (!pointer.base && !pointer.literalBacking && !pointer.baseIndex &&
      !pointer.backing)
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
  if (pointer.backing) {
    if (auto collection =
            pointer.backing.getDefiningOp<emitrust::CollectionOp>()) {
      // W4.2e Part B (FR-39): a node-pool handle projects the shared pool at
      // the handle's index through the high-level `collection_at`, which
      // emitrust-lower-containers turns back into the pool subscript.
      if (!pointer.cursor) // Defensive; pool handles always carry cursors.
        return emitError(loc) << "unsupported heap-allocation pointer shape";
      return builder
          .create<emitrust::CollectionAtOp>(
              loc, emitrust::LValueType::get(pointeeType), pointer.backing,
              pointer.cursor)
          .getResult();
    }
    // A local heap-allocation cursor subscripts the pointer's synthesized
    // MUTABLE backing array (a flat [CAP x T], W4.2e Part A) at the loaded
    // cursor. Unlike a string literal, writes ARE allowed: the place feeds
    // both reads (`sum += stack[--top]`) and writes (`stack[top++] = v`).
    if (!pointer.cursor) // Defensive; backing pointers always carry cursors.
      return emitError(loc) << "unsupported heap-allocation pointer shape";
    auto lvalueType =
        llvm::cast<emitrust::LValueType>(pointer.backing.getType());
    auto arrayType =
        llvm::cast<emitrust::ArrayType>(lvalueType.getValueType());
    return builder
        .create<emitrust::SubscriptOp>(
            loc, emitrust::LValueType::get(arrayType.getElementType()),
            pointer.backing, pointer.cursor)
        .getResult();
  }
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
  // FR-98 Arm A: a FAM-tail pointer local rooted at a recognized
  // MEMBER-READ local (`index = hsi->index` after `hsi = hse->search_index`,
  // FR-96): the root binds no place of its own (`symbols` has no entry), so
  // every use re-projects the member's Option payload FRESH — the FR-96
  // per-use discipline extended through one more member link — then
  // projects the tail's owned Vec member and subscripts it at this
  // pointer's own cursor.
  if (pointer.member)
    if (const clang::MemberExpr *rootInit =
            famMemberLocals.lookup(pointer.base)) {
      FailureOr<Value> projected = emitFamOptionMemberProjection(
          rootInit, llvm::cast<clang::FieldDecl>(rootInit->getMemberDecl()),
          loc, writeback);
      if (failed(projected))
        return failure();
      FailureOr<Value> memberPlace =
          projectMemberPlace(loc, *projected, pointer.member);
      if (failed(memberPlace))
        return failure();
      return refineElementPlace(loc, *memberPlace, pointer.cursor,
                                pointeeType);
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
  // writeback stores back). FR-93: a member-ARRAY base additionally
  // carries a cursor, so `refineElementPlace` subscripts the projected
  // member place; a reference-held struct-pointer parameter root derefs
  // freshly per use (projectPointerMemberBase).
  if (pointer.member) {
    // FR-98 Arm B: a struct-pointer PARAMETER root whose place is an
    // ELEMENT RUN of its struct — a slice-classified parameter (`data =
    // hse->buffer` when `hse` also escapes to a call; the deref'd struct
    // slice) or a promoted owner-region parameter (FR-30; the receiver's
    // data array). The member projection first subscripts the run at the
    // ROOT'S OWN cursor — the identical load the direct `hse->member` path
    // emits. MANDATORY gate: a MOVED root (`hse++`, `hse = q`, `&hse`)
    // would silently retarget every later use of the local through the
    // moved cursor — a miscompile, not a rejection — and the region model
    // records no arithmetic fact for parameters, so the gate re-checks the
    // root's mutation at the AST level (`mutatesVar`, the planFamLift
    // stability gate).
    if (auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>(basePlace.getType())) {
      Type runElement;
      if (auto sliceType =
              llvm::dyn_cast<emitrust::SliceType>(lvalueType.getValueType()))
        runElement = sliceType.getElementType();
      else if (auto arrayType =
                   llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
        runElement = arrayType.getElementType();
      if (runElement && llvm::isa<emitrust::StructType>(runElement)) {
        if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(
                pointer.base->getDeclContext());
            !fn || mutatesVar(fn->getBody(), pointer.base))
          return emitError(loc)
                 << "unsupported: member-array binding rooted at a "
                    "struct-pointer parameter that is itself modified";
        auto rootInfo = pointerLocals.find(pointer.base);
        if (rootInfo == pointerLocals.end() || !rootInfo->second.cursorCell)
          return emitError(loc) << "unsupported member access base";
        Value rootCursor = loadPlace(loc, rootInfo->second.cursorCell);
        FailureOr<Value> element =
            refineElementPlace(loc, basePlace, rootCursor, runElement);
        if (failed(element))
          return failure();
        basePlace = *element;
      }
    }
    FailureOr<Value> memberPlace =
        projectPointerMemberBase(loc, basePlace, pointer.member);
    if (failed(memberPlace))
      return failure();
    basePlace = *memberPlace;
  }
  return refineElementPlace(loc, basePlace, pointer.cursor, pointeeType);
}

FailureOr<Value>
CImporter::projectPointerMemberBase(Location loc, Value basePlace,
                                    const clang::FieldDecl *member) {
  // FR-93: a ScalarRef struct-pointer parameter root holds its raw
  // `&mut S` / `&S` block argument in `symbols`; the member projection
  // needs the pointee PLACE, so the reference derefs freshly at each
  // use — the reason no borrow is ever held across statements.
  if (!llvm::isa<emitrust::LValueType>(basePlace.getType())) {
    Type held = basePlace.getType();
    Type refPointee;
    if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(held))
      refPointee = mutRef.getPointee();
    else if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(held))
      refPointee = sharedRef.getPointee();
    if (!refPointee || !llvm::isa<emitrust::StructType>(refPointee))
      return emitError(loc) << "unsupported member access base";
    basePlace = builder
                    .create<emitrust::DerefOp>(
                        loc, emitrust::LValueType::get(refPointee), basePlace)
                    .getResult();
  }
  return projectMemberPlace(loc, basePlace, member);
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
  // W2.9 hardening: a DEPENDENT record (a class template partial
  // specialization pattern reaching a TU-scope scan) has no concrete
  // layout; asking clang for its size (getTypeSizeInChars below) blows
  // the stack instead of failing. It can never be a byte region.
  // importRecord separately rejects it, located, if an import is ever
  // attempted.
  if (definition->isDependentType())
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
    // W2.2: a C++ class with base classes, virtual methods, or ANY
    // user-declared method (mutating, const, static, a constructor, ...)
    // carries semantics — a vtable pointer, base subobjects, or genuine
    // methods living on the `emitrust.impl` surface — this walk's plain
    // `.fields()` scan never accounts for. A zero-OWN-field such class
    // (e.g. `class Risky { int attempt(int x) {...} };`, or a polymorphic
    // `class Base2 { virtual int f(); }`) would otherwise vacuously pass
    // the all-fields-are-u8 walk below and get misclassified as a byte
    // region, letting a value declaration or pointer parameter of its
    // type (this function's own value-typed caller in `mapType`, or
    // `mapParamType`'s pointer byte-region short-circuit) skip
    // `importRecord` entirely — and so skip both the class's method
    // import and its destructor/virtual/operator rejections. Never
    // byte-region classify such a class; it always takes the normal
    // `importRecord` path, where those rejections and imports are pinned
    // to fire. A plain data-only class/struct (only compiler-synthesized
    // special members, `isImplicit()`) is unaffected and keeps the
    // existing byte-region classification.
    if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(definition)) {
      if (cxxRecord->getNumBases() > 0 || cxxRecord->isPolymorphic())
        return false;
      for (const clang::CXXMethodDecl *method : cxxRecord->methods())
        if (!method->isImplicit())
          return false;
    }
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

const clang::FieldDecl *
CImporter::famTailField(const clang::RecordDecl *record) {
  // FR-94/95: the ADMITTED flexible-array-member tail — the field that grows
  // an owned `Vec<T>` struct member. C path only: the C++ importer keeps
  // every historical FAM rejection.
  if (!record || astContext().getLangOpts().CPlusPlus)
    return nullptr;
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition || definition->isInvalidDecl() ||
      definition->isDependentType())
    return nullptr;
  auto it = famTailFieldCache.find(definition);
  if (it != famTailFieldCache.end())
    return it->second;
  const clang::FieldDecl *result = [&]() -> const clang::FieldDecl * {
    // Structs only; a union's arms alias, and a BYTE-REGION record keeps its
    // flat-image model and every FR-91 pin (its FAM folds into the image at
    // the type level and its accesses keep their historical rejections).
    if (!definition->isStruct() || !definition->hasFlexibleArrayMember() ||
        isByteRegionRecord(definition))
      return nullptr;
    const clang::FieldDecl *last = nullptr;
    for (const clang::FieldDecl *field : definition->fields())
      last = field;
    // The tail must be THIS record's own trailing incomplete array (a nested
    // FAM record propagates `hasFlexibleArrayMember` without one) whose leaf
    // is u8 (heatshrink's `uint8_t buffers[]`, FR-94) or a Vec-mappable
    // scalar (FR-95: hs_index's `int16_t index[]` — the FR-65 element
    // domain); char/bool/__int128/long double/aggregate leaves keep the
    // historical rejections.
    if (!last || !last->getType()->isIncompleteArrayType())
      return nullptr;
    const clang::ArrayType *tail =
        astContext().getAsArrayType(last->getType());
    if (!tail || (!isU8ScalarType(tail->getElementType()) &&
                  !famTailVecElementType(tail->getElementType())))
      return nullptr;
    // Gap-free layout only: offsetof(tail) == sizeof(S). A padding-gap
    // layout (constructible: {u8; u16; u8; u8 t[];} has offsetof 5 < sizeof
    // 6) lets C legally index the tail below sizeof, which the tail-only Vec
    // cannot represent; it keeps the historical FAM rejections.
    const clang::ASTRecordLayout &layout =
        astContext().getASTRecordLayout(definition);
    uint64_t offsetBits = layout.getFieldOffset(last->getFieldIndex());
    uint64_t sizeBits =
        static_cast<uint64_t>(layout.getSize().getQuantity()) * 8;
    if (offsetBits != sizeBits)
      return nullptr;
    return last;
  }();
  famTailFieldCache[definition] = result;
  return result;
}

Type CImporter::famTailVecElementType(clang::QualType element) {
  // FR-95: mirrors planVecLift's element domain (`vecElementType`) so a
  // typed tail lifts to exactly the `Vec<T>` shapes FR-65 already renders.
  // Width gates out the char family and bool (width 8 and 1); floats map
  // to f32/f64 only.
  clang::QualType canonical = element.getCanonicalType();
  if (const auto *builtin = canonical->getAs<clang::BuiltinType>()) {
    if (builtin->getKind() == clang::BuiltinType::Float)
      return Float32Type::get(builder.getContext());
    if (builtin->getKind() == clang::BuiltinType::Double)
      return Float64Type::get(builder.getContext());
  }
  if (!canonical->isIntegerType())
    return {};
  unsigned width = astContext().getIntWidth(canonical);
  if (width != 16 && width != 32 && width != 64)
    return {};
  return canonical->isUnsignedIntegerType()
             ? IntegerType::get(builder.getContext(), width,
                                IntegerType::Unsigned)
             : IntegerType::get(builder.getContext(), width);
}

emitrust::OpaqueType CImporter::famTailVecType(const clang::FieldDecl *tail) {
  clang::QualType element =
      astContext().getAsArrayType(tail->getType())->getElementType();
  if (isU8ScalarType(element))
    return emitrust::OpaqueType::get(builder.getContext(), "Vec<u8>");
  std::optional<std::string> spelling =
      rustSpellingForElementType(famTailVecElementType(element));
  assert(spelling && "famTailField admitted an unspellable tail element");
  return emitrust::OpaqueType::get(builder.getContext(),
                                   "Vec<" + *spelling + ">");
}

const clang::RecordDecl *
CImporter::famMemberFieldShape(const clang::FieldDecl *field) {
  // FR-96: the lift-candidate shape — a data pointer to an ADMITTED FAM
  // record held inside a container that is ITSELF an admitted FAM record.
  // The container gate is this wave's scope: a FAM container is already
  // non-Copy (its Vec tail drops the derive), so the owned Option payload
  // cannot be duplicated by a whole-record copy — that shape rejects at
  // `emitAssign`. C path only (famTailField declines on the C++ path).
  if (!field || !isDataPointer(field->getType()))
    return nullptr;
  const clang::RecordDecl *pointee = field->getType()
                                         .getCanonicalType()
                                         ->getPointeeType()
                                         ->getAsRecordDecl();
  if (!pointee || !famTailField(pointee))
    return nullptr;
  const clang::RecordDecl *container = field->getParent();
  if (!container || !famTailField(container))
    return nullptr;
  // A self-referential member (`struct node *next` inside node) would be
  // an infinite-size `Option<S>` field inside S; it stays on the wall.
  if (pointee->getDefinition() == container->getDefinition())
    return nullptr;
  return pointee;
}

const clang::RecordDecl *
CImporter::famOptionMemberPointee(const clang::FieldDecl *field) {
  const clang::RecordDecl *pointee = famMemberFieldShape(field);
  // The poison set is complete after planOwners: any use outside the
  // recognized family (address-of, ++/--, unrecognized alloc forms,
  // whole-struct init paths) vetoed the lift, and every use keeps the
  // historical member-wall rejection.
  if (!pointee || poisonedPtrFields.count(field))
    return nullptr;
  return pointee;
}

FailureOr<emitrust::OpaqueType>
CImporter::famOptionOfStruct(Type structType, Location loc) {
  auto record = llvm::dyn_cast<emitrust::StructType>(structType);
  if (!record) // Defensive; the shape admitted a FAM record.
    return emitError(loc) << "unsupported: flexible-array record type";
  return emitrust::OpaqueType::get(
      builder.getContext(), ("Option<" + record.getName() + ">").str());
}

FailureOr<emitrust::OpaqueType>
CImporter::famOptionMemberType(const clang::FieldDecl *field, Location loc) {
  clang::QualType pointee =
      field->getType().getCanonicalType()->getPointeeType();
  FailureOr<Type> structType = mapType(pointee, loc);
  if (failed(structType))
    return failure();
  return famOptionOfStruct(*structType, loc);
}

const clang::MemberExpr *
CImporter::famOptionMemberOf(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (true) {
    if (const clang::Expr *sub = peelPointerCast(astContext(), e)) {
      e = stripTrivia(sub);
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
        cast && (cast->getCastKind() == clang::CK_LValueToRValue ||
                 cast->getCastKind() == clang::CK_NoOp ||
                 cast->getCastKind() == clang::CK_BitCast)) {
      e = stripTrivia(cast->getSubExpr());
      continue;
    }
    break;
  }
  const clang::MemberExpr *member = llvm::dyn_cast<clang::MemberExpr>(e);
  if (!member) {
    // A recognized member-read local stands for its initializing member
    // expression; every use re-projects it.
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
        member = famMemberLocals.lookup(var);
    if (!member)
      return nullptr;
  }
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  return field && famOptionMemberPointee(field) ? member : nullptr;
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

FailureOr<Value>
CImporter::projectMemberPlace(Location loc, Value basePlace,
                              const clang::FieldDecl *field) {
  auto baseType = llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
  if (!baseType || !llvm::isa<emitrust::StructType>(baseType.getValueType()))
    return emitError(loc) << "unsupported member access base";
  // FR-78 belt: an opaque-union arm has no field behind it to project
  // (member-rooted region bases funnel here; `emitMemberLValue` carries
  // the primary check).
  if (opaqueUnionArms.contains(field))
    return emitError(loc) << "unsupported: opaque union arm access";
  // FR-94/95: an admitted FAM tail projects at its owned `Vec<T>` field type
  // (the incomplete array type itself has no mapping).
  if (famTailField(field->getParent()) == field)
    return builder
        .create<emitrust::MemberOp>(
            loc, emitrust::LValueType::get(famTailVecType(field)), basePlace,
            builder.getStringAttr(flattenedFieldName(field)))
        .getResult();
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
    else if (auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(valueType);
             opaque && opaque.getValue().starts_with("Vec<") &&
             opaque.getValue().ends_with(">")) {
      // FR-94: a Vec-typed member place (the owned FAM tail) subscripts by
      // the cursor exactly like a slice; the element type comes from the
      // spelling (the W2.3 `emitrust.subscript`-over-opaque trust model).
      elementType = parseStlElementType(
          opaque.getValue().substr(4, opaque.getValue().size() - 5));
      if (!elementType)
        return emitError(loc) << "unsupported pointer target place";
    } else
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
  // FR-98: a member-read-local root (FR-96) has no `symbols` place; the
  // element resolves through a fresh Option-payload projection exactly
  // like `emitPointerPlace`'s Arm A. (No multi-base region can currently
  // carry this base — the join gate element-checks the member type, and a
  // FAM tail's incomplete array type never matches — but the resolver
  // stays total so a future admission cannot silently miss.)
  if (base.member)
    if (const clang::MemberExpr *rootInit = famMemberLocals.lookup(base.var)) {
      FailureOr<Value> projected = emitFamOptionMemberProjection(
          rootInit, llvm::cast<clang::FieldDecl>(rootInit->getMemberDecl()),
          loc, /*writeback=*/nullptr);
      if (failed(projected))
        return failure();
      FailureOr<Value> memberPlace =
          projectMemberPlace(loc, *projected, base.member);
      if (failed(memberPlace))
        return failure();
      return refineElementPlace(loc, *memberPlace, cursor, pointeeType);
    }
  auto it = symbols.find(base.var);
  if (it == symbols.end())
    return emitError(loc) << "unsupported: pointer target '"
                          << base.var->getName()
                          << "' is not an importable place";
  Value place = it->second;
  if (base.member) {
    FailureOr<Value> memberPlace =
        projectPointerMemberBase(loc, place, base.member);
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
  // W2.21: the place of a std::unique_ptr TEMPORARY (`*std::make_unique<T>
  // (a)`). Same reason as the rvalue screen in `emitRValue`: no binding
  // exists to own the Box, so there is no drop point for it.
  if (const auto *bindTemp = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
    if (isStdUniquePtrRecordType(bindTemp->getType()))
      return emitError(loc)
             << "unsupported: std::make_unique is only recognized as the "
                "initializer of a local std::unique_ptr variable";
  // W2.21: `*std::move(p)`. `std::move` yields an XVALUE CallExpr, which
  // has no place, so it reaches here and would report the AST node class
  // ("unsupported assignable expression: CallExpr") instead of the
  // ownership-transfer boundary. Mirrors the `emitCall` interception.
  if (const auto *stdCall = llvm::dyn_cast<clang::CallExpr>(e))
    if (const clang::FunctionDecl *callee = stdCall->getDirectCallee();
        callee && callee->isInStdNamespace() &&
        callee->getDeclName().isIdentifier() &&
        (callee->getName() == "move" || callee->getName() == "forward"))
      for (const clang::Expr *arg : stdCall->arguments())
        if (isStdUniquePtrRecordType(arg->getType()))
          return emitError(loc)
                 << "unsupported: a moved-from std::unique_ptr is null and "
                    "testable, but Rust cannot read a moved-from binding";
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    return emitDeclRefLValue(ref, loc, writeback);
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
    return emitMemberLValue(member, loc, writeback);
  // C99-43 C3: an `argv[i][j]` byte place resolves through the argv table
  // (emitrust.argv_arg + deref + subscript), never the generic pointer
  // decomposition — the table has no cursor cell for the subscript to walk.
  // This makes `emitRValue(argv[i][j])`, comparisons (`argv[1][n] != 0`),
  // and `%d`/`%i` holes all read through the same slice place.
  if (mainArgvTableValue)
    if (const clang::ArraySubscriptExpr *argvByte = matchArgvByteRead(e))
      return emitArgvByteLValue(argvByte, loc);
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    return emitSubscriptLValue(subscript, loc, writeback);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref)
      return emitDerefLValue(unary, loc, writeback);
  // FR-47: an intra-class method call through the IMPLICIT receiver (`m();`
  // inside a sibling method, and the equivalent explicit `this->m();`)
  // reaches here, because clang spells the implicit object argument of both
  // as a bare `CXXThisExpr` and `emitCXXMemberCall` needs the receiver's
  // PLACE to borrow. W2.2 landed `this` only as an rvalue
  // (`emitRValue`'s own `CXXThisExpr` case) and as a `->` member-access
  // base, so this case was missing and every such call rejected as
  // "unsupported assignable expression: CXXThisExpr" — the rank-1 blocker
  // on the FR-46 C++ demand corpus, since it stops any class whose methods
  // call each other. Sits immediately after the `UO_Deref` branch above
  // deliberately: `(*this).m()` already worked through that branch, and
  // this one builds the very same `emitrust.deref` place, so the two
  // spellings stay indistinguishable downstream.
  if (llvm::isa<clang::CXXThisExpr>(e))
    return emitCxxThisPlace(loc);
  // W2.21: `*p` over a recognized `std::unique_ptr` is a genuine C++ place
  // (`operator*` returns `T&`), so a scalar value read (`int a = *n;`)
  // wraps it in a `CK_LValueToRValue` cast whose `emitCast` case calls
  // `emitLValue` here, and `*n += 1` reaches here as the compound
  // assignment's LHS. The payload place is the `Deref` borrow refined by an
  // `emitrust.deref`; the three WRITE positions set `stlBoxWriteContext` so
  // the borrow is `DerefMut` instead (two live `&mut` borrows of one Box in
  // one expression is rustc E0499, so reads must stay shared).
  if (const auto *opCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(e);
      opCall && opCall->getOperator() == clang::OO_Star)
    if (const clang::Expr *boxBase = matchStlBoxDerefBase(e)) {
      FailureOr<Value> receiver = emitLValue(boxBase);
      if (failed(receiver))
        return failure();
      auto lvalueType =
          llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
      auto boxType = lvalueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                      lvalueType.getValueType())
                                : emitrust::OpaqueType();
      if (!boxType || !isStlBoxOpaque(boxType))
        return emitError(loc)
               << "unsupported: operator* receiver is not a recognized STL "
                  "type";
      return emitStlBoxDerefPlace(*receiver, boxType, stlBoxWriteContext,
                                  loc);
    }
  // W2.3: `v[i]` / `v.at(i)` over a recognized `std::vector<T>` receiver
  // both return `T&` in real C++ — a genuine place — so a plain scalar VALUE
  // read (`int x = v[i];`) wraps the call in an implicit `CK_LValueToRValue`
  // cast whose `emitCast` case calls `emitLValue` on the call expression
  // itself, reaching here (rather than `emitCall`, which only sees the
  // no-load statement-discard shape). Both spellings pin the identical
  // bracket-indexing place.
  if (const auto *opCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(e);
      opCall && opCall->getOperator() == clang::OO_Subscript) {
    const auto *opMethod =
        llvm::dyn_cast_or_null<clang::CXXMethodDecl>(opCall->getDirectCallee());
    if (opMethod && opMethod->getParent()->isInStdNamespace()) {
      if (opCall->getNumArgs() != 2)
        return emitError(loc) << "unsupported: operator[] requires exactly "
                                 "one index argument";
      // W2.12: a `std::string_view` receiver is a decomposed local (shared
      // literal backing + cursor/len cells) with NO place of its own, so
      // it is intercepted before the receiver place emission below; its
      // byte place is the backing subscripted at cursor + i.
      if (const auto *svRef = llvm::dyn_cast<clang::DeclRefExpr>(
              opCall->getArg(0)->IgnoreParenImpCasts()))
        if (const auto *svVar =
                llvm::dyn_cast<clang::VarDecl>(svRef->getDecl()))
          if (stringViewLocals.contains(svVar))
            return emitStringViewIndexPlace(svVar, opCall->getArg(1), loc);
      FailureOr<Value> receiver =
          emitLValue(opCall->getArg(0)->IgnoreParenImpCasts());
      if (failed(receiver))
        return failure();
      auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
      // W2.7: a `std::array<T, N>` receiver is an `!emitrust.array<NxT>`
      // place — build the identical `emitrust.subscript` place a C
      // `a[i]` builds (read AND write positions, like any C array).
      if (auto arrayType =
              lvalueType ? llvm::dyn_cast<emitrust::ArrayType>(
                               lvalueType.getValueType())
                         : emitrust::ArrayType()) {
        FailureOr<Value> index = emitRValue(opCall->getArg(1));
        if (failed(index))
          return failure();
        if (!llvm::isa<IntegerType>((*index).getType()))
          return emitError(loc)
                 << "unsupported: operator[] index must be an integer";
        return builder
            .create<emitrust::SubscriptOp>(
                loc, emitrust::LValueType::get(arrayType.getElementType()),
                *receiver, *index)
            .getResult();
      }
      auto opaqueType = lvalueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                         lvalueType.getValueType())
                                   : emitrust::OpaqueType();
      // W2.20: a `std::map` receiver takes the DEFAULT-INSERTING entry
      // place (`*m.entry(k).or_default()`), never `emitrust.subscript`.
      // This is the SAME place the read, the write, the compound and the
      // missing-key read all use, because in C++ all four MUTATE.
      if (opaqueType && isStlMapOpaque(opaqueType))
        return emitStlMapEntryPlace(*receiver, opaqueType, opCall->getArg(1),
                                    loc);
      if (!opaqueType || !isStlOpaqueType(opaqueType) ||
          !opaqueType.getValue().starts_with("Vec<")) {
        // W2.20: this fall-through used to hardcode "std::string" for
        // every non-Vec receiver, which becomes actively misleading now
        // that a map or set can reach it. A genuine std::string keeps the
        // historical wording, which names the real reason.
        if (opaqueType && opaqueType.getValue() == "String")
          return emitError(loc)
                 << "unsupported: std::string::operator[] is not a "
                    "recognized STL method (bytes indexing is not supported "
                    "this wave)";
        if (opaqueType && isStlOpaqueType(opaqueType))
          return emitError(loc)
                 << "unsupported: " << stlOpaqueDisplayName(opaqueType)
                 << "::operator[] is not a recognized STL method";
        return emitError(loc) << "unsupported: operator[] receiver is not a "
                                 "recognized STL type";
      }
      return emitStlVectorIndexPlace(*receiver, opaqueType,
                                     opCall->getArg(1), loc, "operator[]");
    }
  }
  if (const auto *memberCall = llvm::dyn_cast<clang::CXXMemberCallExpr>(e)) {
    const clang::CXXMethodDecl *method = memberCall->getMethodDecl();
    // W2.6 widened the W2.3 `at()`-only case: `front()`/`back()` also
    // return `T&` in real C++, so a scalar value read of either reaches
    // here through the same `CK_LValueToRValue` route as `at()`.
    llvm::StringRef stlPlaceMethod =
        method && method->getParent()->isInStdNamespace() &&
                method->getDeclName().isIdentifier()
            ? method->getName()
            : llvm::StringRef();
    if (stlPlaceMethod == "at" || stlPlaceMethod == "front" ||
        stlPlaceMethod == "back") {
      bool isAt = stlPlaceMethod == "at";
      if (memberCall->getNumArgs() != (isAt ? 1u : 0u))
        return emitError(loc)
               << "unsupported: " << stlPlaceMethod
               << (isAt ? " requires exactly one argument"
                        : " takes no arguments");
      FailureOr<Value> receiver = emitLValue(
          memberCall->getImplicitObjectArgument()->IgnoreParenImpCasts());
      if (failed(receiver))
        return failure();
      auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
      auto opaqueType = lvalueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                         lvalueType.getValueType())
                                   : emitrust::OpaqueType();
      // W2.20: `m.at(k)` over a recognized std::map is the READ-ONLY
      // `*std::ops::Index::index(&m, &k)` place. Rust's panic on a
      // missing key refines C++'s std::out_of_range throw exactly the way
      // W2.6 argued for vector front()/back() on an empty vector.
      if (opaqueType && isAt && isStlMapOpaque(opaqueType))
        return emitStlMapIndexPlace(*receiver, opaqueType,
                                    memberCall->getArg(0), loc);
      if (!opaqueType || !isStlOpaqueType(opaqueType) ||
          !opaqueType.getValue().starts_with("Vec<")) {
        // W2.20: the historical wording named std::vector for EVERY
        // receiver; a map/set/string/optional receiver now names itself.
        if (opaqueType && isStlOpaqueType(opaqueType))
          return emitError(loc)
                 << "unsupported: " << stlOpaqueDisplayName(opaqueType)
                 << "::" << stlPlaceMethod
                 << " is not a recognized STL method";
        return emitError(loc)
               << "unsupported: member call receiver is not a recognized "
                  "std::vector";
      }
      if (isAt)
        return emitStlVectorIndexPlace(*receiver, opaqueType,
                                       memberCall->getArg(0), loc, "at");
      return emitStlVectorEndPlace(*receiver, opaqueType,
                                   /*isFront=*/stlPlaceMethod == "front", loc);
    }
  }
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
  // W2.22: a `std::cout`/`std::cerr` `<<` chain has no place — the whole
  // construct lowers to print macros in statement position. Reaching here
  // means the chain's ostream result feeds something that needs an lvalue
  // (`(std::cout << 1).good()`, an inner link of a chain whose base is NOT
  // cout/cerr); name the real cause instead of the generic node class.
  if (const auto *opCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(e)) {
    llvm::StringRef stream;
    llvm::SmallVector<const clang::CXXOperatorCallExpr *> links;
    if (matchOstreamChain(opCall, stream, links))
      return emitError(loc) << "unsupported: the result of a std::ostream << "
                               "chain must be unused";
  }
  return emitError(loc) << "unsupported assignable expression: "
                        << e->getStmtClassName();
}

FailureOr<Value> CImporter::emitCxxThisPlace(Location loc) {
  if (!currentCxxThisRef)
    return emitError(loc)
           << "unsupported: 'this' outside a non-static member function";
  // Reuses the ref/mut_ref pointee extraction that `emitMemberBasePlace`'s
  // `->` branch performs on any reference-typed base, rather than mapping
  // the clang pointee type afresh: the receiver argument's MLIR type is the
  // authority here (`importFunction` chose it when it built the signature),
  // and rederiving it from the AST would introduce a second, silently
  // divergable source of truth for the receiver struct type.
  Type pointee = borrowPointee(currentCxxThisRef.getType());
  if (!pointee)
    // Defensive: `importFunction` only ever binds `currentCxxThisRef` to the
    // entry block's leading ref/mut_ref receiver argument.
    return emitError(loc)
           << "unsupported: 'this' receiver is not a supported reference";
  // A FRESH deref per use, matching how every other `this` consumer already
  // behaves (a method body reading two fields emits two `emitrust.deref`s of
  // the same receiver argument). Hoisting one shared place into the method
  // prologue was rejected on two counts: it would not dominate uses the
  // importer materializes inside a nested region, and it would perturb the
  // op stream of existing W2.2 method bodies for no gain.
  return builder
      .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(pointee),
                                 currentCxxThisRef)
      .getResult();
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
  // FR-48: a C++ reference parameter's symbol IS the borrow, so the place
  // its name denotes is the REFERENT, reached by the same fresh-per-use
  // `emitrust.deref` `emitCxxThisPlace` builds for `this` (a `this`
  // receiver is itself just an unnamed reference parameter, which is why
  // the two share this shape rather than each rolling their own). Fresh
  // per use for the same reason: a hoisted place would not dominate uses
  // materialized inside a nested region.
  //
  // This branch sits ahead of the pointer-variable rejection below because
  // that rejection is what a reference would otherwise hit: at the AST
  // level a reference use has NO dereference node of its own (clang gives
  // the `DeclRefExpr` the referent's type directly), so `x` on a
  // reference parameter arrives here looking exactly like `p` on a pointer
  // parameter — the one place the two shapes genuinely diverge.
  if (isCxxReferenceDecl(ref->getDecl()))
    if (Type pointee = borrowPointee(place.getType()))
      return builder
          .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(pointee),
                                     place)
          .getResult();
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
  // FR-94/95: an ADMITTED FAM tail (gap-free u8 or Vec-mappable trailing
  // member of a typed record) designates its owned `Vec<T>` field; the base
  // resolves through the ordinary member-base machinery (`&mut S`
  // parameters, owned FAM locals, dot chains), and unresolvable bases
  // (extern pointers, globals' pointer members) keep their located
  // rejections there.
  if (famTailField(field->getParent()) == field) {
    FailureOr<Value> base = emitMemberBasePlace(member, loc, writeback);
    if (failed(base))
      return failure();
    return builder
        .create<emitrust::MemberOp>(
            loc, emitrust::LValueType::get(famTailVecType(field)), *base,
            builder.getStringAttr(flattenedFieldName(field)))
        .getResult();
  }
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
  // FR-78: an opaque-union arm exists only at the type level — the union
  // admitted as a sizeof-sized byte blob, and the blob carries no
  // arm-typed view. FR-83 lowers the ONE enumerated family — integer-
  // scalar leaves, intercepted BEFORE any lvalue is requested (the
  // rvalue-read, assignment, compound-assignment, and ++/-- paths route
  // through `resolveOpaqueArmByteView`) — so every arm access that still
  // reaches this generic lvalue path (whole-arm copies, array decay,
  // float leaves, region-rooted bases) rejects here, at its own site.
  // This check is load-bearing: a leaked arm member op verifies and
  // translates, dying only as rustc E0609 (the emitter's marker backstop
  // is the last line, not this one's substitute).
  if (opaqueUnionArms.contains(field))
    return emitError(loc) << "unsupported: opaque union arm access";
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
CImporter::projectBaseHops(Value place, llvm::ArrayRef<clang::QualType> hops,
                           Location loc) {
  for (clang::QualType hop : hops) {
    auto placeType = llvm::dyn_cast<emitrust::LValueType>(place.getType());
    if (!placeType ||
        !llvm::isa<emitrust::StructType>(placeType.getValueType()))
      return emitError(loc)
             << "unsupported: inherited access through a non-struct place";
    // An empty base contributes NO field (see `collectRecordFields`), so
    // there is nothing to project through. Rejected here rather than
    // emitting `self.base` against a struct that has no such field, which
    // would be a rustc E0609 in the generated crate instead of a located
    // diagnostic.
    const clang::CXXRecordDecl *hopRecord = hop->getAsCXXRecordDecl();
    if (hopRecord && hopRecord->hasDefinition() && hopRecord->isEmpty())
      return emitError(loc)
             << "unsupported: inherited member of an empty base class";
    FailureOr<Type> hopType = mapType(hop, loc);
    if (failed(hopType))
      return failure();
    place = builder
                .create<emitrust::MemberOp>(
                    loc, emitrust::LValueType::get(*hopType), place,
                    builder.getStringAttr("base"))
                .getResult();
  }
  return place;
}

FailureOr<Value>
CImporter::emitMemberBasePlace(const clang::MemberExpr *member, Location loc,
                               GlobalWriteback *writeback) {
  // W2.18: an INHERITED field access (`x` / `this->x` inside a derived
  // method, `d.x` on a derived object) reaches here with clang's implicit
  // derived-to-base conversion wrapped around the member's base
  // expression. The base is an ordinary first field, so the access is the
  // derived place refined by one `member ["base"]` per hop -- and the peel
  // must happen HERE, ahead of every other dispatch below, because
  // `member->isArrow()` is true for the `this`-rooted spelling (the cast's
  // type is `Base *`) and the arrow path would otherwise hand the cast to
  // `emitRValue`, which rejects it as `unsupported cast
  // (UncheckedDerivedToBase)`.
  {
    llvm::SmallVector<clang::QualType, 2> hops;
    const clang::Expr *inner = peelDerivedToBaseCasts(member->getBase(), hops);
    if (!hops.empty()) {
      // Two receiver spellings are in subset: the implicit/explicit `this`
      // of a derived method (arrow, because the cast produced a pointer),
      // and a derived OBJECT place (`d.x`, non-arrow). An upcast reached
      // through a real pointer variable (`p->x` for `Derived *p`) is NOT
      // peeled: it keeps the pre-existing pointer-path rejection rather
      // than silently borrowing the pointer's own place as a struct.
      FailureOr<Value> place = failure();
      if (llvm::isa<clang::CXXThisExpr>(inner))
        place = emitCxxThisPlace(loc);
      else if (!member->isArrow())
        place = emitLValue(inner, writeback);
      else
        return emitError(loc)
               << "unsupported: inherited member through a pointer to a "
                  "derived class";
      if (failed(place))
        return failure();
      return projectBaseHops(*place, hops, loc);
    }
  }
  // FR-96: `->` through a LIFTED member-held FAM field — directly
  // (`hse->search_index->size`) or through a recognized member-read local
  // (`hsi->index[i]` after `hsi = hse->search_index`) — resolves to a
  // fresh PER-USE projection of the member's Option payload.
  if (member->isArrow())
    if (const clang::MemberExpr *inner = famOptionMemberOf(member->getBase()))
      return emitFamOptionMemberProjection(
          inner, llvm::cast<clang::FieldDecl>(inner->getMemberDecl()), loc,
          writeback);
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
  } else if (member->isArrow() &&
             isDecomposedPointerExpr(member->getBase()) &&
             // W2.21: `p->field` over a recognized std::unique_ptr. The `->`
             // operator call's own TYPE is `T *`, so
             // `isDecomposedPointerExpr` claims it for the C pointer
             // decomposition (measured: "unsupported pointer expression:
             // CXXOperatorCallExpr"). It is not a C pointer at all — the
             // generic arrow branch below emits the Deref/DerefMut borrow
             // and refines it into the payload struct place.
             !matchStlBoxDerefBase(member->getBase())) {
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

FailureOr<Value>
CImporter::emitFamOptionMemberPlace(const clang::MemberExpr *member,
                                    const clang::FieldDecl *field,
                                    Location loc,
                                    GlobalWriteback *writeback) {
  // FR-96: the member's Option place. The container base resolves through
  // the ordinary member-base machinery (`&mut S` parameters, owned FAM
  // locals, FR-94 owned-by-value wrapper parameters, dot chains) and
  // unresolvable bases keep their located rejections there.
  FailureOr<Value> base = emitMemberBasePlace(member, loc, writeback);
  if (failed(base))
    return failure();
  FailureOr<emitrust::OpaqueType> optionType =
      famOptionMemberType(field, loc);
  if (failed(optionType))
    return failure();
  return builder
      .create<emitrust::MemberOp>(
          loc, emitrust::LValueType::get(*optionType), *base,
          builder.getStringAttr(flattenedFieldName(field)))
      .getResult();
}

FailureOr<Value>
CImporter::emitFamOptionMemberProjection(const clang::MemberExpr *member,
                                         const clang::FieldDecl *field,
                                         Location loc,
                                         GlobalWriteback *writeback) {
  // FR-96: the per-use payload projection — `as_mut().unwrap()` borrowed
  // fresh at every use and consumed immediately (NLL-safe; the let-bound
  // borrow shape is rustc E0499 against the real do_indexing structure).
  // `unwrap()`'s panic on None is the deterministic fail-loud refinement
  // of C's null-dereference undefined behavior (the FR-88 precedent).
  FailureOr<Value> optionPlace =
      emitFamOptionMemberPlace(member, field, loc, writeback);
  if (failed(optionPlace))
    return failure();
  clang::QualType pointee =
      field->getType().getCanonicalType()->getPointeeType();
  FailureOr<Type> structType = mapType(pointee, loc);
  if (failed(structType))
    return failure();
  Value borrow =
      builder
          .create<emitrust::MethodCallOp>(
              loc, TypeRange{Type(emitrust::MutRefType::get(*structType))},
              *optionPlace, builder.getStringAttr("as_mut().unwrap"),
              ValueRange{})
          .getResult(0);
  return builder
      .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(*structType),
                                 borrow)
      .getResult();
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
  // FR-65: a subscript whose base roots in a lifted `Vec<T>` local routes to
  // the shared STL vector index place — `emitrust.subscript(vecPlace, idx) :
  // lvalue<T>`. One interception covers BOTH read (`x = a[i]` loads the place)
  // and write (`a[i] = x` assigns it); the place machinery is shared and `mut`
  // is inferred automatically. `planVecLift` proved the buffer is used only
  // through such subscripts and `free`, so the pointer-decompose branch below
  // never sees it.
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base))
    if (const auto *root = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (vecValueLocals.contains(root)) {
        Value vecPlace = symbols.lookup(root);
        if (vecPlace)
          if (auto lvalueType =
                  llvm::dyn_cast<emitrust::LValueType>(vecPlace.getType()))
            if (auto vecType = llvm::dyn_cast<emitrust::OpaqueType>(
                    lvalueType.getValueType()))
              return emitStlVectorIndexPlace(vecPlace, vecType,
                                             subscript->getIdx(), loc,
                                             "vector index");
      }
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
  // FR-94: `d->tail[i]` over an admitted FAM tail — the member place is the
  // owned `Vec<u8>` field (an incomplete array is an array type in C, so the
  // subscript arrives on this array-typed-base path); it indexes through the
  // shared STL vector index place exactly like a lifted FR-65 `Vec` local.
  if (auto opaque =
          llvm::dyn_cast<emitrust::OpaqueType>(baseType.getValueType());
      opaque && opaque.getValue().starts_with("Vec<"))
    return emitStlVectorIndexPlace(*basePlace, opaque, subscript->getIdx(),
                                   loc, "vector index");
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
  // FR-52/FR-70: whether an unresolved external may become a requirement on
  // the ENVIRONMENT is one whole-project policy decision, consumed by both
  // the global loop here and the function loop below, so it is computed
  // once, first.
  bool trait = externalRequirementsAllowed();
  // Every deferred `extern` global that was referenced must have a real
  // definition in some translation unit; the Rust program otherwise reads
  // an undefined symbol. Unreferenced extern declarations were skipped at
  // import (referenced-only policy), so a pending entry without a
  // definition is rejected at its first use site (falling back to the
  // declaration when no IR use survives).
  for (const auto &entry : pendingExternGlobals)
    if (!SymbolTable::lookupSymbolIn(module, entry.getKey())) {
      // FR-57a defer mode: the missing definition is a link-time obligation,
      // not an import-time error. Materialize a declaration-only global (no
      // initializer — there is no storage to model here) with the recorded
      // value type, marked for the FR-58 link step; the Rust emitter refuses
      // a module still carrying the marker.
      if (deferExternals) {
        OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
        auto declOp = moduleBuilder.create<emitrust::GlobalOp>(
            entry.getValue().loc, moduleBuilder.getStringAttr(entry.getKey()),
            TypeAttr::get(entry.getValue().type), /*init=*/Attribute(),
            /*is_const=*/UnitAttr());
        declOp->setAttr(emitrust::kExternDeclAttrName,
                        moduleBuilder.getUnitAttr());
        continue;
      }
      // FR-70: under a trait policy, a scalar global whose every access is a
      // direct whole-value load or store is not an error but a REQUIREMENT —
      // storage the environment owns, read and written through a getter/
      // setter pair on the Externals trait. FR-79 extends the admission to
      // CONST STRUCT globals, getter-only; FR-81 to NON-const structs,
      // whose whole-value pair is exact for the single-threaded programs
      // the importer accepts (address-taken structs stay out — a mutable
      // requirement address would need `&'static mut`). Recorded exactly
      // like the FR-57a
      // branch above but with the FR-52 requirement marker;
      // `emitrust-lower-external-requirements` does the rewriting, and the
      // Rust emitter refuses a module still carrying the marker (a
      // declaration-only global would otherwise silently render DEFAULTED
      // storage the C program never had). The C const fact is carried onto
      // the op: a const-marked global structurally refuses stores (the
      // dialect verifier), which is what makes getter-only an enforced
      // contract rather than an accident of the observed uses.
      if (trait && isExternalRequirementGlobalShape(entry.getKey(),
                                                    entry.getValue().type,
                                                    entry.getValue().isConst)) {
        OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
        auto reqOp = moduleBuilder.create<emitrust::GlobalOp>(
            entry.getValue().loc, moduleBuilder.getStringAttr(entry.getKey()),
            TypeAttr::get(entry.getValue().type), /*init=*/Attribute(),
            /*is_const=*/entry.getValue().isConst ? moduleBuilder.getUnitAttr()
                                                  : UnitAttr());
        reqOp->setAttr(emitrust::kExternalRequirementAttrName,
                       moduleBuilder.getUnitAttr());
        continue;
      }
      return emitError(firstSymbolUseLoc(entry.getKey(), entry.getValue().loc))
             << "unsupported: extern global variable '" << entry.getKey()
             << "' is referenced but not defined in any translation unit";
    }

  // FR-80 backstop: an `emitrust.global_addr` was emitted optimistically —
  // the importing TU saw an extern const struct with no definition SO FAR —
  // but only a global the loop above just MARKED as a requirement gives it
  // a meaning (the lowering rewrites it to the &'static getter). A
  // definition in a later TU (or any other survivor) has no address model,
  // so it keeps the historical located rejection, at the address-taking
  // site the op's location preserves.
  {
    emitrust::GlobalAddrOp stray;
    module.walk([&](emitrust::GlobalAddrOp addrOp) {
      Operation *target =
          SymbolTable::lookupSymbolIn(module, addrOp.getGlobalAttr());
      if (target && target->hasAttr(emitrust::kExternalRequirementAttrName))
        return WalkResult::advance();
      stray = addrOp;
      return WalkResult::interrupt();
    });
    if (stray)
      return emitError(stray.getLoc())
             << "unsupported: taking the address of a global variable";
  }

  // No referenced non-variadic external function may remain body-less: the
  // Rust emitter cannot emit a body-less function. (Variadic prototypes such
  // as printf were never added to the module.) An external func whose symbol
  // ended up with no uses (e.g. a prototype referenced only in an
  // unevaluated context) demands no definition and is erased instead.
  //
  // FR-52: under a trait policy the survivors are not an error but the
  // project's REQUIREMENTS on its environment, and are marked as such for
  // `emitrust-lower-external-requirements` instead. Only the shapes the trait
  // can faithfully express qualify — see `isExternalRequirementShape`.
  for (func::FuncOp func :
       llvm::make_early_inc_range(module.getOps<func::FuncOp>()))
    if (func.isExternal()) {
      // FR-77: an address-taken function is spelled out as the OPAQUE text
      // `Some(<name>)`, which is not a SymbolUse, so it must be checked
      // BEFORE the erase-unused branch below — to that branch an
      // initializer-only reference looks unreferenced, and erasing the
      // prototype would ship a crate whose `Some(<name>)` dangles (rustc
      // E0425, whole crate lost). In defer mode the name is a link
      // obligation like any other referenced external (the FR-58 link step
      // resolves or reports it); otherwise it is refused, located at the
      // recorded address-taking site. The use-site gate in
      // `resolveFunctionPointerDecl` normally fires first — this is the
      // finalize-time backstop completing the same contract.
      auto fnPtrTarget = fnPointerTargetSymbols.find(func.getSymName());
      if (fnPtrTarget != fnPointerTargetSymbols.end()) {
        if (deferExternals) {
          func->setAttr(emitrust::kExternDeclAttrName, builder.getUnitAttr());
          continue;
        }
        return emitError(fnPtrTarget->second)
               << "unsupported: taking the address of undefined function '"
               << func.getSymName() << "'";
      }
      if (SymbolTable::symbolKnownUseEmpty(func.getOperation(),
                                           module.getOperation())) {
        func.erase();
        continue;
      }
      // FR-57a defer mode takes precedence over both the FR-52 trait path
      // and the rejection: the declaration is a per-TU shard's requirement
      // on its sibling TUs, marked for the FR-58 link step. (The
      // erase-unused branch above still applies: an unreferenced
      // declaration demands nothing from anyone.)
      if (deferExternals) {
        func->setAttr(emitrust::kExternDeclAttrName, builder.getUnitAttr());
        continue;
      }
      if (trait && isExternalRequirementShape(func)) {
        func->setAttr(emitrust::kExternalRequirementAttrName,
                      builder.getUnitAttr());
        continue;
      }
      return emitError(firstSymbolUseLoc(func.getSymName(), func.getLoc()))
             << "unsupported: function '" << func.getSymName()
             << "' is referenced but not defined in any translation unit";
    }
  return success();
}

bool CImporter::externalRequirementsAllowed() {
  switch (externalRequirements) {
  case emitrust::ExternalRequirements::Reject:
    return false;
  case emitrust::ExternalRequirements::Trait:
    return true;
  case emitrust::ExternalRequirements::TraitWhenLibrary:
    break;
  }
  // The library predicate, spelled exactly as `emitrustcc::hasCMain` spells
  // it: a module that DEFINES the imported C entry point becomes a binary
  // crate under `--crate-type=auto`, and a binary crate has no caller to
  // supply the trait impl. A body-less `c_main` (an entry point that some
  // TU only declared) is not a definition and does not count.
  func::FuncOp entry = functions.lookup("c_main");
  return !entry || entry.isExternal();
}

bool CImporter::classifyTimeTraitEligible() {
  // FR-57a defer mode takes precedence over the trait path in
  // `finalizeProject`, so it must equally suppress the eager slice
  // classification: a deferred declaration keeps its historical
  // scalar-reference shape for the FR-58 link step.
  if (deferExternals)
    return false;
  switch (externalRequirements) {
  case emitrust::ExternalRequirements::Reject:
    return false;
  case emitrust::ExternalRequirements::Trait:
    return true;
  case emitrust::ExternalRequirements::TraitWhenLibrary:
    break;
  }
  // The per-TU spelling of the library predicate: `finalizeProject` asks
  // whether the merged MODULE defines `c_main`, but classification runs
  // mid-import, so the question is asked of the current AST — does THIS
  // TU define `main`? (A body-less `main` declaration does not count,
  // matching `externalRequirementsAllowed`.) For a multi-TU project whose
  // `main` lives in another TU the approximation diverges; every
  // divergent outcome is a located rejection downstream, never a silent
  // shape change. Scanned once per AST.
  clang::ASTContext &ctx = astContext();
  auto it = classifyTimeTraitCache.find(&ctx);
  if (it != classifyTimeTraitCache.end())
    return it->second;
  bool definesMain = false;
  for (const clang::Decl *decl : ctx.getTranslationUnitDecl()->decls())
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
      if (fn->getDeclName().isIdentifier() && fn->getName() == "main" &&
          fn->getDefinition()) {
        definesMain = true;
        break;
      }
  classifyTimeTraitCache.try_emplace(&ctx, !definesMain);
  return !definesMain;
}

bool CImporter::isExternalRequirementGlobalShape(llvm::StringRef symbol,
                                                 Type type, bool isConst) {
  // Only a type the trait can faithfully pass BY VALUE qualifies: the
  // getter/setter pair copies a whole scalar in and out. Pointer-typed
  // externs never reach the scalar pending map (`deferExternPointerGlobal`).
  // FR-79: a CONST struct qualifies too, GETTER-ONLY — imported structs are
  // Copy, so the by-value return is a faithful read. FR-81 completes the
  // matrix with the NON-const struct, getter/setter PAIR: the tearing FR-70
  // feared assumed concurrent observers, but the mutable-global model is
  // already exact only for the single-threaded programs the importer
  // accepts, and sequentially the staged whole-value get/modify/set the
  // importer lowers field stores to (copy, member-assign the temporary, one
  // store back — with fresh loads per RHS read and a post-call refresh) IS
  // the C-sequenced semantics. The struct_def must be visible in the final
  // module (a recovery mode can drop one); when it is not, the safe failure
  // is the historical rejection. FR-85 admits one array shape: the CONST
  // BYTE REGION (`!emitrust.array<Nxui8>`, the CTS-BR image of a u8-only
  // record, a record array, or a plain byte array — the type key erases
  // the distinction, and all three share the image below), getter-only.
  // The licensing fact is the importer's byte-region lowering: EVERY use —
  // `&g` at an argument position included — is a staged whole-value copy
  // (`global_load` + temporary + slice_of/subscript), never a carried
  // address, so a by-value `fn g() -> [u8; N]` reproduces the emitted IR
  // exactly and no pointer identity exists to lose. Arrays of any OTHER
  // element type keep the rejection: element access into environment-owned
  // storage needs PLACES, which no associated trait item yields.
  bool constStruct = false;
  bool constByteRegion = false;
  if (!llvm::isa<IntegerType, FloatType>(type)) {
    if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type)) {
      if (!isConst || !arrayType.getElementType().isUnsignedInteger(8))
        return false;
      constByteRegion = true;
    } else {
      auto structType = llvm::dyn_cast<emitrust::StructType>(type);
      if (!structType)
        return false;
      if (!llvm::isa_and_nonnull<emitrust::StructDefOp>(
              SymbolTable::lookupSymbolIn(module, structType.getName())))
        return false;
      constStruct = isConst;
    }
  }
  // Address-takenness is an AST fact, not an IR one: `&g` on an undefined
  // extern can leave NO surviving symbol use at all (the pointer plan
  // swallows the body it flowed into), so an IR-use scan alone would wrongly
  // qualify it. The whole-program pre-scan records the fact under exactly
  // this symbol key (`addressBoundGlobal` → `globalVarSymbolName`, the same
  // naming `pendingExternGlobals` is keyed by).
  //
  // FR-80 makes this gate SHAPE-AWARE rather than absolute: a CONST STRUCT
  // is exactly the type whose address the `fn g() -> &'static T` getter
  // can now carry, so for it the decision falls to the surviving-use scan
  // below (loads and global_addrs both rewrite against the one getter; a
  // swallowed non-qualifying use has already rejected at its own site or
  // been dropped by a recovery mode that reports it). FR-85 exempts the
  // const byte region for the complementary reason: the AST-level `&g` is
  // real, but the byte-region lowering never carries the address — the
  // pointer plan stages a copy and slices the TEMPORARY — so the decision
  // again falls to the surviving-use scan, which for this arm accepts
  // LOADS ONLY (a copy-back `global_store` from a cast-away-const write
  // must keep the rejection, never silently write to a copy). Every other
  // type keeps the absolute disqualifier: a by-value getter erases
  // identity.
  if (!constStruct && !constByteRegion &&
      wholeProgram.addressTakenGlobals.contains(symbol))
    return false;
  // And every IR use that DID survive must be a direct whole-value load or
  // store — the only shapes `E::g()` / `E::set_g(v)` can express — or, for
  // the const struct, the FR-80 address anchor the lowering rewrites to
  // the same getter. The const byte region is loads-only.
  std::optional<SymbolTable::UseRange> uses = SymbolTable::getSymbolUses(
      StringAttr::get(module.getContext(), symbol), module.getOperation());
  if (!uses)
    return false;
  for (SymbolTable::SymbolUse use : *uses) {
    if (llvm::isa<emitrust::GlobalLoadOp>(use.getUser()))
      continue;
    if (!constByteRegion && llvm::isa<emitrust::GlobalStoreOp>(use.getUser()))
      continue;
    if (constStruct && llvm::isa<emitrust::GlobalAddrOp>(use.getUser()))
      continue;
    return false;
  }
  return true;
}

bool CImporter::isExternalRequirementShape(func::FuncOp func) {
  // A C++ member function's call sites are receiver-bearing
  // `emitrust.method_call`s; an associated trait function has no receiver to
  // bind them to, and a missing method body is a hole in code the project
  // OWNS rather than a requirement on its environment.
  if (func->hasAttr(emitrust::kMethodOfAttrName))
    return false;
  // An address-taken function is spelled out by name inside an opaque
  // `Some(<name>)` constant that the trait's type parameter cannot reach.
  if (fnPointerTargetSymbols.contains(func.getSymName()))
    return false;
  // Every surviving reference must be a DIRECT CALL. Anything else — the
  // callee slot of an indirect construct, an unmodelled symbol use a future
  // op introduces — has no `E::<name>` spelling, so it keeps the rejection.
  std::optional<SymbolTable::UseRange> uses = SymbolTable::getSymbolUses(
      func.getSymNameAttr(), module.getOperation());
  if (!uses)
    return false;
  for (SymbolTable::SymbolUse use : *uses) {
    auto call = llvm::dyn_cast<func::CallOp>(use.getUser());
    if (!call || call.getCalleeAttr() != use.getSymbolRef())
      return false;
    // A method-tagged call is a `emitrust.method_call` after conversion, for
    // the same reason a method declaration is excluded above.
    if (call->hasAttr(emitrust::kMethodCallAttrName))
      return false;
  }
  return true;
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

} // namespace

OwningOpRef<ModuleOp>
mlir::emitrust::importC(llvm::StringRef path,
                        llvm::ArrayRef<std::string> extraClangArgs,
                        llvm::StringRef compilationDatabasePath,
                        MLIRContext &context) {
  ImportOptions options;
  options.compilationDatabasePath = compilationDatabasePath.str();
  return importC(path, extraClangArgs, options, context);
}

OwningOpRef<ModuleOp>
mlir::emitrust::importC(llvm::StringRef path,
                        llvm::ArrayRef<std::string> extraClangArgs,
                        const ImportOptions &options, MLIRContext &context) {
  loadImportDialects(context);

  // Imperative shell: parse the file with clang. Parse diagnostics are
  // printed to stderr by clang's own diagnostic machinery. The language
  // (C or C++) comes from the file's `compile_commands.json` entry when a
  // database was given (FR-45), else from its extension (W2.0). The
  // command-line assembly, the database selection, and the ClangTool run
  // all live in ClangProjectParser.cpp, shared verbatim with the FR-40 item
  // graph so both see the same project.
  std::vector<std::string> requested{path.str()};
  std::vector<std::string> sources;
  std::string databaseError;
  ProjectParseError firstClangError;
  std::vector<std::unique_ptr<clang::ASTUnit>> asts;
  int status = buildProjectASTs(requested, extraClangArgs,
                                options.compilationDatabasePath, asts, sources,
                                databaseError, firstClangError);
  if (!databaseError.empty()) {
    // Locate the diagnostic ON the database path so the driver's
    // `file:line:col:` prefix names it; the message then carries only
    // clang's own explanation.
    emitError(FileLineColLoc::get(
                  StringAttr::get(&context, options.compilationDatabasePath),
                  /*line=*/1, /*column=*/1))
        << "cannot load compilation database: " << databaseError;
    return nullptr;
  }
  if (asts.size() != 1 || !asts.front()) {
    emitError(UnknownLoc::get(&context))
        << "failed to parse C input '" << path << "'";
    return nullptr;
  }
  clang::ASTUnit &ast = *asts.front();
  // A parse error was already printed, located, by clang's own machinery;
  // returning null silently keeps that historical stderr byte-for-byte.
  if (ast.getDiagnostics().hasErrorOccurred())
    return nullptr;
  if (status != 0) {
    // FR-68: an error-severity diagnostic that BYPASSED the ASTUnit's
    // engine — in practice a driver-level rejection such as
    // `unknown argument: '-fbogus-flag-xyz'`, which clang prints unlocated.
    // Importing anyway would silently honor a command line the driver
    // rejected (possibly an ABI-relevant flag), so fail with a diagnostic
    // located on the offending translation unit carrying clang's own text.
    InFlightDiagnostic diag =
        emitError(FileLineColLoc::get(
            StringAttr::get(&context, firstClangError.file.empty()
                                          ? path
                                          : llvm::StringRef(
                                                firstClangError.file)),
            /*line=*/1, /*column=*/1))
        << "clang error while building this translation unit";
    if (!firstClangError.message.empty())
      diag << ": " << firstClangError.message;
    return nullptr;
  }

  // Functional core: translate the AST into a fresh module.
  Location moduleLoc =
      FileLineColLoc::get(StringAttr::get(&context, path), /*line=*/1,
                          /*column=*/1);
  OwningOpRef<ModuleOp> module(ModuleOp::create(moduleLoc));
  CImporter importer(*module);
  // FR-57a: deferred-externals mode exists FOR this single-TU entry point
  // (the FR-56 shim imports each TU solo); inert at its default.
  importer.setDeferExternals(options.deferExternals);
  // FR-42: recovery is opted into per import and touches nothing when off.
  // A caller that wants recovery without a ledger gets this scratch one, so
  // the importer never has to test for a null ledger mid-import.
  RejectionLedger scratchLedger;
  if (options.recover) {
    importer.enableRecovery(options.ledger ? *options.ledger : scratchLedger);
    // FR-43, single-file: this entry point emits file-statics under their
    // BARE names (no `tu<i>_` tag) while the item graph always tags them, so
    // only externally visible items are addressable here. Installing the set
    // anyway keeps the two entry points' semantics one rule rather than two.
    if (!options.excludedItems.empty())
      importer.setExcludedItems(options.excludedItems);
  }
  // FR-57a: with deferred externals this TU is one shard of a larger
  // program, so an extern-only global takes the same deferral path a
  // project import gives it (recorded in `pendingExternGlobals` for
  // `finalizeProject`) instead of the immediate single-file rejection.
  // FR-58: for the same reason, a shard-mode import tags its file-statics
  // with the TU-LOCAL placeholder `tu0_` -- the linkage fact the merge step
  // needs (nothing else in the IR records linkage; see CSymbolLinkage.h),
  // and the tag the link step alpha-renames to the shard's link-line
  // ordinal. A historical non-defer single-file import keeps the bare
  // names, byte for byte.
  //
  // FR-58 owner-planning divergence (measured, see design.md): a shard is
  // BY DEFINITION not the whole program, so defer mode must not claim
  // `soleTranslationUnit` -- under that claim the Phase-4 owner/cell-slice
  // planners promote EXTERNALLY VISIBLE functions on the strength of "all
  // call sites are in this TU", which no shard can know; a caller in
  // another TU (a local-array argument, say) makes the joint import refuse
  // the very promotion the shard performed, and the merged crate would
  // silently carry the promoted form. The historical non-defer single-file
  // import keeps the claim: there, the TU genuinely is the program.
  if (failed(importer.importTranslationUnit(
          ast.getASTContext(),
          /*tuTag=*/options.deferExternals ? "tu0_" : "",
          /*deferExtern=*/options.deferExternals,
          /*soleTranslationUnit=*/!options.deferExternals)))
    return nullptr;
  // FR-57a: finalization is what turns the deferred references into marked
  // declarations (and erases unused body-less prototypes). Historical
  // single-file imports never ran it, so it stays defer-mode-only here.
  if (options.deferExternals && failed(importer.finalizeProject()))
    return nullptr;

  // A verifier failure indicates an importer bug; it is still an import
  // failure and must never yield unverified IR.
  if (failed(verify(*module)))
    return nullptr;
  return module;
}

OwningOpRef<ModuleOp>
mlir::emitrust::importC(llvm::StringRef path,
                        llvm::ArrayRef<std::string> extraClangArgs,
                        MLIRContext &context) {
  return importC(path, extraClangArgs, /*compilationDatabasePath=*/"",
                 context);
}

OwningOpRef<ModuleOp> mlir::emitrust::importC(llvm::StringRef path,
                                              MLIRContext &context) {
  return importC(path, /*extraClangArgs=*/{}, context);
}

OwningOpRef<ModuleOp>
mlir::emitrust::importCProject(llvm::ArrayRef<std::string> paths,
                               llvm::ArrayRef<std::string> extraClangArgs,
                               llvm::StringRef compilationDatabasePath,
                               MLIRContext &context) {
  ImportOptions options;
  options.compilationDatabasePath = compilationDatabasePath.str();
  return importCProject(paths, extraClangArgs, options, context);
}

OwningOpRef<ModuleOp>
mlir::emitrust::importCProject(llvm::ArrayRef<std::string> paths,
                               llvm::ArrayRef<std::string> extraClangArgs,
                               const ImportOptions &options,
                               MLIRContext &context) {
  loadImportDialects(context);

  // Imperative shell: parse every source as an independent translation
  // unit. Each input selects its own language from its
  // `compile_commands.json` entry when a database was given (FR-45), else
  // by extension (W2.0): a mixed C+C++ project compiles each file
  // correctly in isolation, though mixing them into one program stays out
  // of scope.
  // The database selection, the "with a database and no named sources the
  // project IS the database" resolution, and the ClangTool run all live in
  // ClangProjectParser.cpp, shared verbatim with the FR-40 item graph so
  // both see the same project.
  std::vector<std::string> sources;
  std::string databaseError;
  ProjectParseError firstClangError;
  std::vector<std::unique_ptr<clang::ASTUnit>> asts;
  int status = buildProjectASTs(paths, extraClangArgs,
                                options.compilationDatabasePath, asts, sources,
                                databaseError, firstClangError);
  if (!databaseError.empty()) {
    // Locate the diagnostic ON the database path so the driver's
    // `file:line:col:` prefix names it; the message then carries only
    // clang's own explanation.
    emitError(FileLineColLoc::get(
                  StringAttr::get(&context, options.compilationDatabasePath),
                  /*line=*/1, /*column=*/1))
        << "cannot load compilation database: " << databaseError;
    return nullptr;
  }
  // Without a database the caller's list is the only source of inputs, so
  // an empty list stays the historical error; with one, it means the
  // database itself listed nothing.
  if (sources.empty()) {
    emitError(UnknownLoc::get(&context)) << "no C input files given";
    return nullptr;
  }
  if (asts.size() != sources.size()) {
    emitError(UnknownLoc::get(&context))
        << "failed to parse one or more C inputs";
    return nullptr;
  }
  // A parse error was already printed, located, by clang's own machinery;
  // returning null silently keeps that historical stderr byte-for-byte.
  for (const std::unique_ptr<clang::ASTUnit> &ast : asts)
    if (!ast || ast->getDiagnostics().hasErrorOccurred())
      return nullptr;
  if (status != 0) {
    // FR-68: an error-severity diagnostic that BYPASSED every ASTUnit's
    // engine — in practice a driver-level rejection such as
    // `unknown argument: '-fbogus-flag-xyz'` from one TU's recorded command
    // line, which clang prints unlocated. Importing anyway would silently
    // honor a command line the driver rejected (possibly an ABI-relevant
    // flag), so fail with a diagnostic located on the offending translation
    // unit carrying clang's own text.
    InFlightDiagnostic diag =
        emitError(FileLineColLoc::get(
            StringAttr::get(&context, firstClangError.file.empty()
                                          ? llvm::StringRef(sources.front())
                                          : llvm::StringRef(
                                                firstClangError.file)),
            /*line=*/1, /*column=*/1))
        << "clang error while building this translation unit";
    if (!firstClangError.message.empty())
      diag << ": " << firstClangError.message;
    return nullptr;
  }

  // Functional core: merge every AST into one module with shared cross-TU
  // dedup and extern-resolution state. All ASTs stay alive for the whole
  // import so their decl pointers remain valid.
  Location moduleLoc =
      FileLineColLoc::get(StringAttr::get(&context, sources.front()),
                          /*line=*/1, /*column=*/1);
  OwningOpRef<ModuleOp> module(ModuleOp::create(moduleLoc));
  CImporter importer(*module);
  // FR-52: the unresolved-external policy is a whole-project decision and
  // `finalizeProject` — the only place it is consulted — runs only here, so
  // it is installed once, unconditionally, and is inert at its default.
  importer.setExternalRequirements(options.externalRequirements);
  // FR-57a: like the FR-52 policy above, a whole-project decision consulted
  // only in `finalizeProject`; inert at its default.
  importer.setDeferExternals(options.deferExternals);
  // FR-42: one shared ledger across every TU — a project's recovery report
  // is a project-level artifact, and the per-TU walks accumulate into it in
  // path order.
  RejectionLedger scratchLedger;
  if (options.recover) {
    importer.enableRecovery(options.ledger ? *options.ledger : scratchLedger);
    // FR-43: the admitted set is a recovery-mode restriction, so it is only
    // installed here. `options` outlives the import, so the reference the
    // importer keeps stays valid.
    if (!options.excludedItems.empty())
      importer.setExcludedItems(options.excludedItems);
  }
  // W3.0: scan every AST for externally visible va_list-using variadic
  // definitions BEFORE importing any of them, so the registry is complete
  // regardless of whether a caller's TU or its callee's defining TU is
  // processed first (`importTranslationUnit` below runs the TUs in path
  // order, but the cross-TU call-site check needs the full-project answer
  // from the start).
  // W3.2 rides the same pre-import pass: whole-program facts must also be
  // complete before the first TU imports, and for the same order-independence
  // reason (a defining TU may be processed before or after a caller's TU).
  for (auto [index, ast] : llvm::enumerate(asts)) {
    importer.collectCrossTuVaListVariadics(ast->getASTContext());
    importer.collectWholeProgramInfo(ast->getASTContext(),
                                     static_cast<unsigned>(index));
    importer.collectCellSliceCallFacts(ast->getASTContext());
  }
  // Reduce the accumulated whole-program cell-slice call facts to their final
  // eligibility sets before any TU's planCellSlices consults them (W3.3).
  importer.finalizeCellSliceWholeProgram();
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

OwningOpRef<ModuleOp>
mlir::emitrust::importCProject(llvm::ArrayRef<std::string> paths,
                               llvm::ArrayRef<std::string> extraClangArgs,
                               MLIRContext &context) {
  return importCProject(paths, extraClangArgs,
                        /*compilationDatabasePath=*/"", context);
}
