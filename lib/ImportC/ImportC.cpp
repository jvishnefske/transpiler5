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
///  - Aggregates and pointers use EmitRust place operations, which are
///    opaque to upstream passes: struct and array locals are
///    `emitrust.variable`, field access is `emitrust.member`, indexing is
///    `emitrust.subscript`, pointer parameters are `!emitrust.mut_ref<T>`
///    arguments dereferenced with `emitrust.deref`, and reads/writes of any
///    such place use `emitrust.load`/`emitrust.assign`. A scalar local whose
///    address is taken is kept as an `emitrust.variable` so the reference
///    stays valid in the generated Rust.
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
///    literal's bytes plus the terminating NUL; writes through such a
///    region are rejected (writing a C string literal is UB). A region
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
///    to pointers in C and classify the same way. At call sites every value
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
///
/// The importer is a functional core (the `CImporter` class below, which
/// owns the builder and per-function symbol table) driven by the imperative
/// shell in `importC`, which runs clang LibTooling and verifies the result.
/// Every unsupported construct produces a located diagnostic and fails the
/// import; no silently wrong IR is ever produced.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/ImportC.h"

#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Verifier.h"

#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace mlir;

namespace {

/// Break/continue branch targets for the innermost enclosing loop or switch.
struct LoopTargets {
  /// Block a `break` statement branches to (the loop or switch exit block).
  Block *breakDest;
  /// Block a `continue` statement branches to (the condition or increment
  /// block of the enclosing loop). Inside a `switch`, this is inherited from
  /// the enclosing loop and is null when the switch is not inside a loop.
  Block *continueDest;
};

/// A comparison or switch operand that denotes a value of a complete named
/// C enum, as recognized by `classifyEnumOperand`. Exactly one of `value`
/// (an enum-typed expression) and `constant` (an enumerator reference,
/// which has type `int` in C) is non-null.
struct EnumOperand {
  /// The enum's defining declaration.
  const clang::EnumDecl *decl;
  /// The enum-typed expression, or null for an enumerator reference.
  const clang::Expr *value;
  /// The referenced enumerator, or null for an enum-typed expression.
  const clang::EnumConstantDecl *constant;
};

/// An imported variable with static storage duration (a file-scope variable
/// or a function-local static), keyed by its canonical clang declaration.
struct GlobalInfo {
  /// The module-level `emitrust.global` symbol name (for statics, the
  /// mangled `<function>_<name>`).
  std::string symbol;
  /// The mapped MLIR value type of the global.
  Type type;
};

/// The emission-side identity of one pointer-region base: the bound object
/// and, for a `&struct.member` base (CTS-P9), the scalar member the region
/// roots at. Two bases are the same exactly when both components agree, so
/// `&s` and `&s.b` stay distinct bases of distinct regions.
struct PointerBaseKey {
  /// The bound object's declaration (canonical for globals).
  const clang::VarDecl *var = nullptr;
  /// The member the region roots at (`p = &s.b`); null for whole-object
  /// bases.
  const clang::FieldDecl *member = nullptr;

  bool operator==(const PointerBaseKey &other) const {
    return var == other.var && member == other.member;
  }
  bool operator!=(const PointerBaseKey &other) const {
    return !(*this == other);
  }
};

/// A pending store-back of a staged global copy. Element and field accesses
/// of a global stage its whole value in a local `emitrust.variable`; write
/// contexts store the modified copy back into the global afterwards
/// (load-modify-store, exact for the single-threaded C subset).
struct GlobalWriteback {
  /// The staging place holding the copy; null when no global was staged
  /// and no multi-base element was staged.
  Value place;
  /// The symbol of the global to store the copy back into; empty for a
  /// multi-base staging (which writes back through `multiBases`).
  std::string symbol;
  /// Multi-base dispatch store-back (CTS-P7): when non-empty, `place`
  /// stages one element of the base selected by `multiBaseIndex`, and the
  /// flush dispatches the staged value back into that base at
  /// `multiCursor` (a match over the closed set of bases). A global-member
  /// base's arm stages the global's whole value afresh, assigns the
  /// projected member, and stores the whole value back (CTS-P9).
  SmallVector<PointerBaseKey, 2> multiBases;
  /// The i32 enum-of-bases discriminant selecting the active base;
  /// meaningful only with a non-empty `multiBases`.
  Value multiBaseIndex;
  /// The i64 element cursor of the staged element; null when every base
  /// is a degenerate scalar object.
  Value multiCursor;
  /// The mapped pointee value type of the staged element; meaningful only
  /// with a non-empty `multiBases`.
  Type multiPointeeType;
};

/// Returns whether `name` is a Rust keyword (strict or reserved, editions
/// 2015-2021, plus the contextual `union`) and thus unusable as a Rust item
/// name. Global variables keep their C spelling verbatim, so colliding
/// names are rejected instead of being mangled.
static bool isRustKeyword(llvm::StringRef name) {
  static const llvm::StringSet<> keywords = {
      // Strict keywords (2015).
      "as", "break", "const", "continue", "crate", "else", "enum", "extern",
      "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
      "move", "mut", "pub", "ref", "return", "self", "Self", "static",
      "struct", "super", "trait", "true", "type", "unsafe", "use", "where",
      "while",
      // Strict keywords (2018).
      "async", "await", "dyn",
      // Reserved keywords.
      "abstract", "become", "box", "do", "final", "macro", "override", "priv",
      "try", "typeof", "unsized", "virtual", "yield",
      // Contextual keyword that still reads confusingly as an item name.
      "union"};
  return keywords.contains(name);
}

/// Returns the Rust spelling of a struct/union MEMBER name: a spelling that
/// is a Rust keyword mangles deterministically by appending a single
/// underscore (`type` -> `type_`, `match` -> `match_`); every other
/// spelling is kept verbatim. Member names are the one identifier class
/// that mangles instead of rejecting (C99-45: c-testsuite 00218 declares a
/// member named `type`): the mangled spelling never leaves the emitted
/// struct's own field namespace, so no cross-symbol collision policy is
/// disturbed. Struct/enum/function/global names keep their rejections. A
/// collision the mangle introduces (a struct declaring both `type` and
/// `type_`) is rejected where the fields are collected.
static std::string mangleMemberName(llvm::StringRef name) {
  if (isRustKeyword(name))
    return (name + "_").str();
  return name.str();
}

/// Returns whether `type` is, or contains (through record fields and array
/// elements, never through pointers), a record with a bit-field member.
/// Used to refuse `sizeof`/`_Alignof` folds over bit-field records: the
/// C99-45 backing-run layout is deliberately not ABI-compatible, so the C
/// layout numbers would promise a layout the emitted Rust does not keep.
/// Recursion depth is bounded by the source's type nesting (pointers are
/// not followed, so self-referencing records terminate).
static bool typeContainsBitField(clang::QualType type) {
  const clang::Type *canonical = type.getCanonicalType().getTypePtr();
  while (const auto *array = llvm::dyn_cast<clang::ArrayType>(canonical))
    canonical = array->getElementType().getCanonicalType().getTypePtr();
  const clang::RecordDecl *record = canonical->getAsRecordDecl();
  if (!record)
    return false;
  record = record->getDefinition();
  if (!record)
    return false;
  for (const clang::FieldDecl *field : record->fields()) {
    if (field->isBitField())
      return true;
    if (typeContainsBitField(field->getType()))
      return true;
  }
  return false;
}

/// A pointer expression decomposed into its statically resolved base object
/// and an i64 element cursor value. `cursor` is null for a degenerate base
/// (the address of a scalar or struct object taken with `&x`), which
/// supports dereference but carries no element offset and hence no pointer
/// arithmetic. Into a multi-dimensional array the cursor is flat and
/// row-major: it counts innermost (scalar or struct) elements from the
/// start of `base`, regardless of whether the pointer designates a scalar
/// or a whole row. A pointer into a string literal has no base
/// declaration; its region is the literal's read-only backing byte array
/// (`literalBacking`, an `!emitrust.lvalue<!emitrust.array<Nxi8>>`) and its
/// cursor is always present.
struct PtrExprValue {
  /// The object the pointer points into (a local scalar, struct, or
  /// array); null for a pointer into a string literal, for a pointer
  /// of a nullable region that was never bound to any object (a pointer
  /// that only ever holds the null constant), and for a pointer of a
  /// multi-base region (whose active base is the runtime `baseIndex`
  /// discriminant, CTS-P7).
  const clang::VarDecl *base = nullptr;
  /// The i64 element offset from the start of the region; null when
  /// degenerate.
  Value cursor;
  /// The read-only backing array place of a string-literal region; null
  /// for object-based pointers.
  Value literalBacking;
  /// The Option-of-cursor discriminant of a pointer in a nullable region:
  /// an i1 that is true when the pointer currently holds an address and
  /// false when it holds the null constant (CTS-P8). Null when the
  /// pointer is statically non-null (its region never sees NULL), which
  /// lets null-checks fold to constants.
  Value nonNull;
  /// The enum-of-bases discriminant of a pointer in a multi-base region
  /// (CTS-P7): an i32 index into `multiBases` naming the object the
  /// pointer currently points into. Null for single-base pointers.
  Value baseIndex;
  /// The ordered disjoint bases of a multi-base region, indexed by
  /// `baseIndex`; empty for single-base pointers. Copied out of the
  /// pointer's `PointerLocalInfo` so the value stays self-contained.
  SmallVector<PointerBaseKey, 2> multiBases;
  /// The struct member a `&struct.member` base roots at (CTS-P9): the
  /// pointer designates exactly that member of `base`, so every place it
  /// resolves to is the member's projection on the base's place (or on
  /// the base's staged global copy). Null for whole-object bases.
  const clang::FieldDecl *member = nullptr;
};

/// Phase-1b classification of one pointer parameter, derived from the
/// function definition's body. `ScalarRef` parameters are only dereferenced
/// (`*p`) or arrowed (`p->f`), or are unused, and stay plain
/// `!emitrust.mut_ref<T>` references (the historical behavior). `Slice`
/// parameters are subscripted, walked, compared, differenced, reassigned,
/// copied into a pointer local, or passed onward, and become
/// `!emitrust.mut_ref<!emitrust.slice<T>>` region bases. `CellSlice`
/// parameters (CTS-P10) belong to an interprocedural class whose bases are
/// ALL mutable global arrays of one element type; they become shared
/// `!emitrust.ref<!emitrust.cell_slice<T>>` references, which stay
/// coherent with direct global reads mid-call because both hit the same
/// thread-local `Cell` (a staged copy would be unsound here). `Carrier`
/// parameters (CTS-P3) are `void *` parameters of a defined function whose
/// body only ever truth-tests them: they carry an integer in pointer
/// clothing and become plain i64 values (call sites pass carrier values,
/// with null as the i64 zero).
enum class ParamKind { ScalarRef, Slice, CellSlice, Carrier };

/// Why a pointer-parameter class with global bases does NOT lower to a
/// cell-slice (CTS-P10 boundaries), keyed by the global base so the
/// call-site rejection can name the precise reason.
struct CellSliceReject {
  /// The boundary that was hit.
  enum class Kind {
    /// The class joins a global base with a local object: one parameter
    /// type would have to be both `&mut [T]` and `&[Cell<T>]`.
    Mixed,
    /// A parameter of the class is null-checked; Option wrapping and the
    /// global_cells borrow discipline do not compose in v1.
    NullableGlobal,
  };
  Kind kind;
  /// The global base's C name (for the Mixed wording).
  std::string globalName;
  /// The first local object joined into the class (Mixed only).
  std::string localName;
};

/// One cell-slice access expression in a callee body: a subscript `p[i]`
/// or dereference `*p` of a `!emitrust.ref<!emitrust.cell_slice<T>>`
/// parameter. `index` is null for the dereference form (element 0).
struct CellSliceAccess {
  const clang::ParmVarDecl *param;
  const clang::Expr *index;
};

/// The decomposition record of one accepted pointer local (or, in Phase 1b,
/// one slice-classified pointer parameter): the single base object of its
/// region and this pointer's rank-0 `memref<i64>` cursor cell.
/// The cell is null when the base is degenerate (a scalar object with no
/// element offset to track); such a pointer needs no runtime state at all.
/// A slice parameter is its own base, with its cursor initialized to zero.
struct PointerLocalInfo {
  /// The object every value of this pointer points into; null for a
  /// pointer whose region is a string literal and for a pointer of a
  /// multi-base region (CTS-P7, see `multiBases`).
  const clang::VarDecl *base = nullptr;
  /// Entry-block `memref<i64>` cell holding the element cursor, or null.
  Value cursorCell;
  /// The read-only backing array place of a string-literal region
  /// (`!emitrust.lvalue<!emitrust.array<Nxi8>>`, holding the literal's
  /// bytes plus the terminating NUL); null for object-based pointers.
  Value literalBacking;
  /// Entry-block `memref<i1>` cell holding the Option-of-cursor
  /// discriminant of a pointer in a nullable region (true = holds an
  /// address, false = holds the null constant); null for pointers whose
  /// region never sees a null constant (CTS-P8).
  Value nonNullCell;
  /// Entry-block `memref<i32>` cell holding the enum-of-bases
  /// discriminant of a pointer in a multi-base region (CTS-P7): the index
  /// into `multiBases` of the object the pointer currently points into.
  /// An address binding stores the bound object's index, `p = q` copies
  /// the source pointer's discriminant, and every dereference dispatches
  /// on it (a match over the closed set of bases). Null for single-base
  /// pointers, whose base is statically known.
  Value baseIndexCell;
  /// The ordered disjoint bases of a multi-base region, indexed by the
  /// discriminant; empty for single-base pointers. Every pointer of one
  /// region carries the same order (the region's binding order), so
  /// discriminants copy soundly across `p = q`.
  SmallVector<PointerBaseKey, 2> multiBases;
  /// The struct member a single `&struct.member` base roots at (CTS-P9);
  /// null for whole-object bases. A member base is a degenerate
  /// one-element run: no cursor, no arithmetic.
  const clang::FieldDecl *member = nullptr;
};

/// The imported model of one pointer-typed global variable (CTS-P4): the
/// pointer decomposes into a statically known *global* region base plus,
/// for array-shaped regions, a stored i64 element cursor that is itself a
/// module-level `emitrust.global` under the pointer's own C name. Cursors
/// are plain Copy integers, so a stored global cursor carries no borrow —
/// which is what makes a pointer-typed global representable at all under
/// the `thread_local!`+Cell global model (a borrow could never escape
/// `.with`). A degenerate pointer (bound to one global scalar or struct
/// object) needs no runtime state at all and creates no module ops.
struct PointerGlobalInfo {
  /// The global base object's declaration (canonical). For a synthesized
  /// backing — a promoted `calloc`/`malloc` allocation or a file-scope
  /// compound literal — this is the pointer's own declaration and
  /// `backingSymbol` names the backing global.
  const clang::VarDecl *base;
  /// Module symbol of the synthesized backing `emitrust.global`; empty
  /// when `base` is a real global object (resolved through the ordinary
  /// globals map at each access).
  std::string backingSymbol;
  /// Mapped value type of the synthesized backing; null with real bases.
  Type backingType;
  /// Module symbol of the pointer's i64 cursor `emitrust.global`; empty
  /// for a degenerate (whole-object) pointer, which has no runtime state.
  std::string cursorSymbol;
};

/// The statically resolved binding of one pointer-typed struct member of
/// one struct instance (CTS-P2 / C99-43). A data-pointer member is stored
/// as a plain i64 cursor field — a cursor is a borrow-free Copy integer
/// (docs/transformation-theory.md section 4), so storing one in a struct
/// never fights the borrow checker — and the analysis resolves the member
/// to at most one statically known target object (or string literal) per
/// struct instance. Every supported binding in the current model is
/// degenerate (the member designates one whole scalar/struct object), so
/// the stored i64 carries no runtime information and stays 0; member reads
/// resolve to the bound object's place with no runtime state at all.
/// Conflicting bindings, non-address sources, and array-run bindings make
/// the member unresolvable, recorded as a located rejection naming both
/// sites when two bindings clash.
struct MemberPointerFacts {
  /// The single object this instance's member points into; null for a
  /// literal binding or an invalid one.
  const clang::VarDecl *base = nullptr;
  /// The string literal bound to the member (write-only support: reads of
  /// a literal-bound member stay rejected); null for object bindings.
  const clang::StringLiteral *literal = nullptr;
  /// Where the binding was established (first site).
  clang::SourceLocation loc;
  /// The conflicting second binding site; meaningful only with a
  /// non-empty `invalidReason` produced by a binding clash.
  clang::SourceLocation secondLoc;
  /// Diagnostic text of the construct that made the member unresolvable;
  /// empty when the binding is consumable.
  std::string invalidReason;
  /// Location of the invalidating construct; meaningful only with
  /// `invalidReason`.
  clang::SourceLocation invalidLoc;
};

/// The program-wide key of one member-pointer binding: the struct instance
/// (a local or global variable — for a pointer global with a synthesized
/// backing, the pointer's own declaration stands for the backing) and the
/// pointer-typed field.
using MemberPointerKey =
    std::pair<const clang::VarDecl *, const clang::FieldDecl *>;

/// One base binding of a pointer region: the object some pointer in the
/// region was made to point into, and the source location of the assignment
/// (or initializer) that bound it. The multi-base diagnostic names the
/// first two bindings. A `&struct.member` binding (CTS-P9) additionally
/// carries the scalar member the region roots at; bindings to distinct
/// members of one object are distinct bases.
struct PointerBaseBinding {
  /// The bound object.
  const clang::VarDecl *base;
  /// Where the binding was established.
  clang::SourceLocation loc;
  /// The member the binding roots at (`p = &s.b`); null for whole-object
  /// bindings.
  const clang::FieldDecl *member = nullptr;
};

/// The Phase-4 owner-promotion plan of one local array base variable: the
/// variable becomes a module-level owner struct holding the array in its
/// single "data" field, and every function whose pointer parameters resolve
/// into the variable's region becomes a `&mut self` method of that struct
/// (recorded separately in the method-plan map).
struct OwnerPlan {
  /// The owner struct's module-level symbol name
  /// (`Owner_<function>_<base>`, TU-tag-mangled for internal-linkage
  /// owning functions so identically named statics in different TUs stay
  /// distinct).
  std::string structName;
  /// Whether the module-level `emitrust.struct_def` has been created; it
  /// is synthesized on first need, at the owning declaration.
  bool structDefCreated = false;
};

/// The Phase-1a facts about one pointer ownership region: the distinct
/// objects its pointers are bound to (or the string literal that is the
/// region's read-only base), whether any pointer arithmetic occurs (which a
/// degenerate scalar base cannot support), whether anything is written
/// through the region's pointers (which a read-only string-literal region
/// cannot support), whether any pointer of the region holds the null
/// pointer constant (which makes the region an Option of its cursor,
/// CTS-P8), and the first construct (if any) that puts the region outside
/// the decomposition (escape, non-address source, ...).
struct PointerRegion {
  /// Distinct base objects, each with its first binding location.
  SmallVector<PointerBaseBinding, 2> bases;
  /// The string literal the region's pointers are bound to, making the
  /// region a read-only `'static` byte run; null for object-based regions.
  /// Mutually exclusive with `bases` for a consumable region.
  const clang::StringLiteral *literalBase = nullptr;
  /// Where the literal binding was established; meaningful only with
  /// `literalBase`.
  clang::SourceLocation literalLoc;
  /// True when any pointer in the region is walked (`p+n`, `++`, `+=`).
  bool hasArithmetic = false;
  /// First pointer-arithmetic site; meaningful only with `hasArithmetic`.
  clang::SourceLocation arithmeticLoc;
  /// True when anything is written through a pointer of the region
  /// (`*p = v`, `p[i] = v`, `*p += v`, `(*p)++`); a string-literal region
  /// rejects at this location (writing a C string literal is UB).
  bool hasWriteThrough = false;
  /// First write-through site; meaningful only with `hasWriteThrough`.
  clang::SourceLocation writeThroughLoc;
  /// True when a null pointer constant is assigned to (or initializes) any
  /// pointer of the region. Such a region is nullable: each of its
  /// pointers models the Option-of-cursor discriminant in a runtime i1
  /// "non-null" flag cell, mirroring the fn_ptr `None` mapping (CTS-P8).
  bool nullable = false;
  /// First null-constant binding site; meaningful only with `nullable`.
  clang::SourceLocation nullableLoc;
  /// True when a pointer-typed conditional operator classified into the
  /// region (CTS-P9): its arms united here, so the region's null state
  /// merges through conditional control flow. A base-less nullable region
  /// with a conditional source is STATICALLY NULL — it carries zero
  /// runtime state and its null tests fold (see `isStaticallyNullRegion`);
  /// a base-less region built only from direct null bindings keeps the
  /// historical CTS-P8 flag cell.
  bool hasConditionalSource = false;
  /// True when an integer value rides into the region in pointer clothing
  /// (CTS-P3): an integer-to-pointer cast, or a call to a function whose
  /// pointer return classifies as an integer carrier. A region whose ONLY
  /// sources are such carriers and null constants never addresses a
  /// modeled object and lowers as a plain i64 value (see
  /// `isCarrierRegion`); a region that also binds a real address base is
  /// invalidated with the historical non-address rejection.
  bool hasCarrierSource = false;
  /// First carrier-source site; meaningful only with `hasCarrierSource`.
  clang::SourceLocation carrierLoc;
  /// The single recognized allocation call (`calloc`/`malloc` with
  /// compile-time-constant sizes) bound to a global pointer of the region;
  /// the allocation is promoted to a synthesized zero-initialized global
  /// backing array (see `importPointerGlobal`). Null when no allocation is
  /// bound. Mutually exclusive with `bases` for a consumable region.
  const clang::Expr *allocSite = nullptr;
  /// Where the allocation binding was established; meaningful only with
  /// `allocSite`.
  clang::SourceLocation allocLoc;
  /// Number of pointee elements the allocation covers; meaningful only
  /// with `allocSite`.
  uint64_t allocCount = 0;
  /// First invalidating construct; meaningful only with `invalidReason`.
  clang::SourceLocation invalidLoc;
  /// Diagnostic text of the invalidating construct; empty when the region
  /// is decomposable.
  std::string invalidReason;
};

/// The Phase-1a facts about one second-order pointer local (`T **pp`,
/// CTS-P5). Under the decomposition a first-order pointer is a plain Copy
/// i64 cursor into its region, so a pointer-to-pointer is a cursor into a
/// region of cursor cells — an index selecting WHICH first-order pointer to
/// operate on (transformation-theory sections 4 and 6). The implemented
/// shape is the degenerate one-cell region: `pp` is only ever bound to the
/// address of a single first-order pointer local, so the selection is
/// static, `pp` needs no runtime state, and no reference-to-reference ever
/// arises in the emitted Rust (the cursor-cell region and the pointee
/// region stay distinct separation-logic conjuncts). A second binding to a
/// distinct pointer variable records `secondTarget` for the located
/// multi-target rejection; every other construct records `invalidReason`.
struct SecondOrderRegion {
  /// The single first-order pointer local `pp` selects, from `pp = &p`.
  const clang::VarDecl *target = nullptr;
  /// Where the target binding was established.
  clang::SourceLocation targetLoc;
  /// A second, distinct bound pointer variable (would need a real region
  /// of cursor cells); null while the region stays single-target.
  const clang::VarDecl *secondTarget = nullptr;
  /// Where the second binding was established.
  clang::SourceLocation secondTargetLoc;
  /// First invalidating construct; meaningful only with `invalidReason`.
  clang::SourceLocation invalidLoc;
  /// Diagnostic text of the invalidating construct; empty when the
  /// degenerate second-order decomposition applies.
  std::string invalidReason;
};

/// Steensgaard-style union-find pre-pass that groups the pointer locals of
/// one function body into ownership regions (Phase 1a of pointer support).
/// One AST walk (in the style of `collectAddressTaken`) unions pointers on
/// assignment (`p = q`), binds base objects from `&x`, `&arr[i]`,
/// array-to-pointer decay, and slice-classified pointer parameters (Phase
/// 1b: `p = param` makes the parameter the region base), binds string
/// literals from their decay (`p = "..."` makes the literal the base of a
/// read-only region), flags pointer arithmetic, writes through the
/// region's pointers, and null-constant bindings (`p = NULL` marks the
/// region nullable instead of invalidating it, CTS-P8), and records the
/// first construct that makes a region undecomposable (taking a pointer's
/// address, non-address sources). Base objects participate
/// in the union-find
/// alongside the pointers so that two pointers into the same object always
/// share a region. The importer validates each pointer local against its
/// region at the declaration; a region is consumable only when it is
/// single-base — one object, or one string literal (whose read-only region
/// additionally rejects write-throughs) — and never invalidated. The
/// per-region output is the deliberate seam for the later owner-struct
/// codegen phases.
class PointerRegionAnalysis {
public:
  /// Analyzes `body`, replacing any previous analysis state. `context` is
  /// borrowed for the duration of the walk (null-constant classification).
  void analyze(clang::ASTContext &astContext, const clang::Stmt *body);

  /// Optional query telling the walk whether a direct call to `callee`
  /// returns an integer-carrier pointer (CTS-P3): such a call is a carrier
  /// source of the assigned pointer's region rather than a non-address
  /// invalidation. Left unset (the pure-AST planning passes), every call
  /// source keeps the historical non-address rejection, which is
  /// conservative — planning never consumes carrier regions.
  std::function<bool(const clang::FunctionDecl *)> carrierReturnQuery;

  /// Optional query telling the walk whether a local `void *` declaration
  /// is an admitted fn-ptr holder (CTS-F, 00210): such a local imports as
  /// an ordinary `!emitrust.fn_ptr` variable, so the decomposition never
  /// tracks it. Left unset (the pure-AST planning passes), the holder is
  /// tracked and conservatively invalid, which planning never consumes.
  std::function<bool(const clang::VarDecl *)> fnHolderQuery;

  /// Returns whether `var` is a pointer local tracked by this analysis.
  bool tracks(const clang::VarDecl *var) const {
    return pointerVars.contains(var);
  }

  /// Returns the region of the tracked pointer `var`, or null when the
  /// pointer was never bound, unioned, or invalidated (an unused pointer).
  const PointerRegion *regionOf(const clang::VarDecl *var);

  /// Returns whether `var` is a second-order pointer local (`T **pp`)
  /// tracked by this analysis (CTS-P5).
  bool tracksSecondOrder(const clang::VarDecl *var) const {
    return secondOrderVars.contains(var);
  }

  /// Returns the second-order record of the tracked pointer-to-pointer
  /// `var`, or null when it was never bound or invalidated (unused).
  const SecondOrderRegion *
  secondOrderRegionOf(const clang::VarDecl *var) const {
    auto it = secondOrderRegions.find(var);
    return it == secondOrderRegions.end() ? nullptr : &it->second;
  }

  /// Returns every pointer-typed local variable the analysis tracks; the
  /// Phase-4 owner-planning pre-pass iterates these to project each
  /// per-function region into the interprocedural union-find.
  const llvm::SmallPtrSetImpl<const clang::VarDecl *> &trackedVars() const {
    return pointerVars;
  }

  /// The member-pointer bindings this body established, keyed by (struct
  /// instance, field); `planOwners` merges them into the program-wide map.
  const llvm::DenseMap<MemberPointerKey, MemberPointerFacts> &
  memberBindings() const {
    return memberFacts;
  }

  /// Data-pointer fields this body used in a shape the per-instance model
  /// cannot resolve (a write through an alias or unresolved place, an
  /// escaping member address, a whole-struct overwrite), with the first
  /// such site. A poisoned field rejects every read program-wide.
  const llvm::DenseMap<const clang::FieldDecl *, clang::SourceLocation> &
  poisonedMemberFields() const {
    return poisonedFields;
  }

private:
  /// Recursive statement walk collecting pointer declarations, writes,
  /// arithmetic, escapes, and call-argument uses.
  void visit(const clang::Stmt *stmt);

  /// Classifies the right-hand side `rhs` of `ptr = rhs` (or of `ptr`'s
  /// initializer): unions pointer-to-pointer copies, binds bases from
  /// address expressions and string literals from their decay, flags
  /// arithmetic on `p +- n` forms, and marks the region invalid for
  /// everything else.
  void recordPointerWrite(const clang::VarDecl *ptr, const clang::Expr *rhs);

  /// Binds `base` into `ptr`'s region at `loc` (rejecting local bases of
  /// global pointers) and unions the two declarations. A non-null `member`
  /// roots the binding at `&base.member` (CTS-P9); member bindings do not
  /// join the base object into the union-find — every access resolves
  /// through the object's own place (or its staged global copy) directly,
  /// so no cursor state is ever shared with pointers into the whole
  /// object.
  void addBase(const clang::VarDecl *ptr, const clang::VarDecl *base,
               clang::SourceLocation loc,
               const clang::FieldDecl *member = nullptr);

  /// Binds the string literal `literal` as the read-only base of `ptr`'s
  /// region at `loc`; a region bound to two distinct literals is marked
  /// invalid (rebinding across literals is out of the decomposition).
  void addLiteralBase(const clang::VarDecl *ptr,
                      const clang::StringLiteral *literal,
                      clang::SourceLocation loc);

  /// Flags `ptr`'s region as performing pointer arithmetic at `loc`.
  void recordArithmetic(const clang::VarDecl *ptr, clang::SourceLocation loc);

  /// Flags `ptr`'s region as nullable at `loc` (a null pointer constant
  /// was assigned to one of its pointers); only the first site is kept.
  void recordNullable(const clang::VarDecl *ptr, clang::SourceLocation loc);

  /// Flags `ptr`'s region as fed by an integer carrier at `loc` (CTS-P3):
  /// an integer-to-pointer cast or a call returning a carrier. Only local
  /// pointers may carry integers (globals keep the historical non-address
  /// rejection), and a region that already binds an address base, string
  /// literal, or allocation is invalidated instead — the two models cannot
  /// mix.
  void recordCarrierSource(const clang::VarDecl *ptr,
                           clang::SourceLocation loc);

  /// Flags `ptr`'s region as written through (`*p = v`, `p[i] = v`, ...)
  /// at `loc`; a string-literal region rejects at this location.
  void recordWriteThrough(const clang::VarDecl *ptr,
                          clang::SourceLocation loc);

  /// Binds a `calloc(n, size)` / `malloc(bytes)` call with compile-time
  /// constant arguments as the allocation base of the global pointer
  /// `ptr`'s region (the allocation is later promoted to a synthesized
  /// zero-initialized global backing array of the pointer's element
  /// type). The byte total must be a positive constant multiple of the
  /// element size; violations and a second distinct allocation site mark
  /// the region invalid.
  void recordAllocBase(const clang::VarDecl *ptr, const clang::CallExpr *call,
                       clang::SourceLocation loc);

  /// Classifies the right-hand side `rhs` of `pp = rhs` for a second-order
  /// pointer (CTS-P5): `pp = &p` on a tracked first-order pointer local
  /// binds `p` as the (degenerate) selection target and marks the `&p`
  /// expression consumed so the escape check skips it; a second distinct
  /// target records the multi-target rejection; every other source
  /// (null constants, copies, non-address values) invalidates `pp`.
  void recordSecondOrderWrite(const clang::VarDecl *ptr,
                              const clang::Expr *rhs);

  /// Marks the second-order pointer `ptr` undecomposable with diagnostic
  /// `reason` at `loc`; only the first invalidation is kept.
  void markSecondOrderInvalid(const clang::VarDecl *ptr,
                              clang::SourceLocation loc,
                              llvm::StringRef reason);

  /// Returns the tracked second-order pointer `pp` of a stripped `*pp`
  /// dereference expression, or null when `expr` is not one.
  const clang::VarDecl *asSecondOrderDeref(const clang::Expr *expr) const;

  /// Returns the tracked pointer local at the root of a written place
  /// expression (`*p`, `p[i]`, `*p++`, ...), or null when the place is not
  /// a dereference or subscript through a tracked pointer. A place through
  /// a second-order dereference (`**pp`, `(*pp)[i]`) roots at the bound
  /// selection target, so the write lands on the target's region. A global
  /// data pointer at the root is tracked on first sight (like the
  /// arithmetic and rebinding forms), so a body whose only mention of the
  /// global is a write through it still contributes the write to the
  /// program-wide facts — a read-only literal-backed global region must
  /// reject it.
  const clang::VarDecl *trackedWritePlaceRoot(const clang::Expr *place);

  /// Classifies the right-hand side of a data-pointer member write
  /// (`s.f = rhs` on a `var.field` place rooted at `instance`): `&obj`
  /// binds the object degenerately, a decayed string literal binds the
  /// literal (write-only), and every other source records a located
  /// invalid-binding fact. A second binding to a different target records
  /// the clash with both sites.
  void recordMemberPointerWrite(const clang::VarDecl *instance,
                                const clang::FieldDecl *field,
                                const clang::Expr *rhs);

  /// Records one resolved member binding fact (object or literal) for
  /// `(instance, field)` at `loc`, merging with any existing fact.
  void bindMemberPointer(const clang::VarDecl *instance,
                         const clang::FieldDecl *field,
                         const clang::VarDecl *base,
                         const clang::StringLiteral *literal,
                         clang::SourceLocation loc);

  /// Marks the member binding of `(instance, field)` unresolvable with
  /// diagnostic `reason` at `loc`; only the first invalidation is kept.
  void markMemberInvalid(const clang::VarDecl *instance,
                         const clang::FieldDecl *field,
                         clang::SourceLocation loc, llvm::StringRef reason);

  /// Poisons `field` program-wide at `loc`: some use of the field in this
  /// body is outside the per-instance model (aliased write, escaping
  /// member address, whole-struct overwrite), so no read of the field can
  /// trust a static binding.
  void poisonMemberField(const clang::FieldDecl *field,
                         clang::SourceLocation loc);

  /// Walks the (semantic-form) initializer list of the struct local
  /// `instance`, recording bindings for its directly initialized
  /// data-pointer fields and poisoning data-pointer fields buried in
  /// nested aggregates (their instance path is outside the model).
  void collectStructInitBindings(const clang::VarDecl *instance,
                                 const clang::InitListExpr *list);

  /// Poisons every data-pointer field reachable from `record` (through
  /// nested struct and array-of-struct fields): a whole-value overwrite of
  /// an object of this type invalidates any static member binding.
  void poisonRecordPointerFields(const clang::RecordDecl *record,
                                 clang::SourceLocation loc);

  /// Marks `ptr`'s region undecomposable with diagnostic `reason` at `loc`;
  /// only the first invalidation of a region is kept.
  void markInvalid(const clang::VarDecl *ptr, clang::SourceLocation loc,
                   llvm::StringRef reason);

  /// Returns the union-find root of `decl`, inserting it on first use.
  const clang::VarDecl *findRoot(const clang::VarDecl *decl);

  /// Unions the regions of `a` and `b`, merging their recorded facts.
  void unite(const clang::VarDecl *a, const clang::VarDecl *b);

  /// Returns the (created on demand) region of `decl`'s current root.
  PointerRegion &regionFor(const clang::VarDecl *decl);

  /// The AST context of the function under analysis; borrowed.
  clang::ASTContext *context = nullptr;
  /// Union-find parent links over pointer and base declarations.
  llvm::DenseMap<const clang::VarDecl *, const clang::VarDecl *> parent;
  /// Region facts keyed by each set's current root.
  llvm::DenseMap<const clang::VarDecl *, PointerRegion> regions;
  /// Every pointer-typed local variable declared in the walked body.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> pointerVars;
  /// Every second-order pointer local (`T **pp`) declared in the walked
  /// body (CTS-P5); disjoint from `pointerVars`.
  llvm::SmallPtrSet<const clang::VarDecl *, 4> secondOrderVars;
  /// Second-order facts keyed by the pointer-to-pointer declaration (no
  /// union-find: second-order copies are not decomposable).
  llvm::DenseMap<const clang::VarDecl *, SecondOrderRegion> secondOrderRegions;
  /// `&p` expressions consumed as second-order bindings (`pp = &p`); the
  /// generic address-taken escape check skips exactly these.
  llvm::SmallPtrSet<const clang::Expr *, 4> consumedAddrOf;
  /// Member-pointer bindings established by this body (CTS-P2).
  llvm::DenseMap<MemberPointerKey, MemberPointerFacts> memberFacts;
  /// Data-pointer fields used outside the per-instance member model.
  llvm::DenseMap<const clang::FieldDecl *, clang::SourceLocation>
      poisonedFields;
};

/// Translates the clang AST of one C translation unit into an MLIR module.
///
/// The importer owns an `OpBuilder` positioned inside the function currently
/// being translated, a per-function symbol table from clang declarations to
/// their MLIR "place" values, and the loop stack for break/continue. All
/// state is confined to this object; ownership of the produced IR stays with
/// the module passed in by the caller.
class CImporter {
public:
  /// Creates an importer that appends to `module`. The translation-unit
  /// specific context is supplied per call to `importTranslationUnit`, so one
  /// importer can merge several ASTs (cross-TU dedup state persists).
  explicit CImporter(ModuleOp module)
      : module(module), builder(module.getContext()) {}

  /// Imports every supported top-level declaration of `context`'s translation
  /// unit into the module: complete struct definitions (bare anonymous
  /// structs under synthesized shape-keyed `Anon<n>` names), function
  /// declarations or definitions, and file-scope variables (as module-level
  /// `emitrust.global`s). Other declarations are rejected, with one
  /// exception: declarations whose expansion location lies in a system
  /// header are skipped entirely (never imported, never rejected here);
  /// any main-file use of one is rejected at the use site instead.
  ///
  /// `tuTag` is prepended to internal-linkage (`static`) symbol names so that
  /// identically named file-statics in different translation units stay
  /// distinct; it is empty for a single-TU import (bare names, historical
  /// behavior). `deferExtern` controls whether a referenced `extern`-only
  /// global with no definition in this TU is an immediate error (single-file)
  /// or deferred for cross-TU resolution (project); unreferenced ones are
  /// skipped either way. `soleTranslationUnit` states that this TU
  /// is the whole program, which lets the Phase-4 owner planning promote
  /// externally visible functions to methods (all their call sites are
  /// provably in this TU); in a multi-TU project only internal-linkage
  /// functions qualify. Repeated calls accumulate into one module.
  LogicalResult importTranslationUnit(clang::ASTContext &context,
                                      llvm::StringRef tuTag, bool deferExtern,
                                      bool soleTranslationUnit);

  /// After every translation unit has been imported, checks that no external
  /// symbol was left unresolved: every deferred `extern` global must have a
  /// definition, and no referenced non-variadic external function may remain
  /// body-less (the Rust emitter cannot emit a body-less function); external
  /// functions whose symbol has no uses are erased instead of rejected.
  /// Rejections are located at the symbol's first use site (falling back to
  /// its declaration).
  LogicalResult finalizeProject();

private:
  //===--------------------------------------------------------------------===//
  // Locations and types
  //===--------------------------------------------------------------------===//

  /// Converts a clang source location to an MLIR `FileLineColLoc` using the
  /// presumed (user-visible) location; unknown on invalid input.
  Location translateLoc(clang::SourceLocation sourceLoc);

  /// The location of the first IR use of `symbol` anywhere in the module,
  /// or `fallback` when the symbol has no uses. Locates the
  /// referenced-but-undefined rejections of `finalizeProject` at the use
  /// site rather than at the declaration.
  Location firstSymbolUseLoc(llvm::StringRef symbol, Location fallback);

  /// True when `decl`'s expansion location lies in a system header (an
  /// angle-bracket include or an `-isystem` search path). Such declarations
  /// are skipped by `importTranslationUnit` instead of being imported
  /// eagerly; a main-file use of one is rejected at the use site via
  /// `rejectSystemHeaderUse`. Project headers included via `-I` are not
  /// system headers and keep the eager fail-fast import.
  bool isSystemHeaderDecl(const clang::Decl *decl) const;

  /// Located rejection for a main-file use of a declaration that
  /// `importTranslationUnit` skipped because it lives in a system header.
  /// `what` describes the use ("call to", "reference to", ...); `name` is
  /// the used symbol's spelling.
  LogicalResult rejectSystemHeaderUse(Location loc, llvm::StringRef what,
                                      llvm::StringRef name);

  /// Maps a C value type to its MLIR type: `_Bool`->i1, char->i8,
  /// short->i16, int->i32, long/long long->i64, unsigned char->ui8,
  /// unsigned short->ui16, unsigned int->ui32, unsigned long/long
  /// long->ui64 (unsigned types map to MLIR *unsigned* integer types, not
  /// signless ones, so that they render as Rust `uN`), float->f32,
  /// double->f64, `struct S`->`!emitrust.struct<"S">` (a bare anonymous
  /// struct under its synthesized shape-keyed name, importing the
  /// definition on the way), `T[N]`->`!emitrust.array<NxT>`, complete named
  /// `enum E`->`!emitrust.enum<"E">`, anonymous enums->`i32`, and function
  /// pointers `R (*)(A, B)`->`!emitrust.fn_ptr<(A, B) -> R>` (prototype-less
  /// K&R pointers map to the zero-parameter form). Typedefs resolve through
  /// the canonical type. Data pointers, unions, variadic function pointers,
  /// fn_ptr component types outside the verifier set, and everything else
  /// produce a located diagnostic.
  FailureOr<Type> mapType(clang::QualType type, Location loc);

  /// Maps a C function-parameter type: data-pointer parameters `T*` become
  /// `!emitrust.mut_ref<T>` for `ParamKind::ScalarRef` and
  /// `!emitrust.mut_ref<!emitrust.slice<T>>` for `ParamKind::Slice`
  /// (array parameters have already decayed to pointers in clang);
  /// function-pointer parameters stay by-value `!emitrust.fn_ptr` values;
  /// everything else maps like `mapType` and ignores `kind`.
  FailureOr<Type> mapParamType(clang::QualType type, Location loc,
                               ParamKind kind);

  /// Returns the Phase-1b classification of every parameter of `func`
  /// (non-pointer parameters report `ScalarRef`, which is ignored). Kinds
  /// derive from the definition's body via `collectSliceParams`; a function
  /// with no definition in the merged ASTs classifies every pointer
  /// parameter as `ScalarRef` (the cross-TU assumption checked at
  /// definition-time signature refinement in `importFunction`). Results are
  /// cached per canonical declaration.
  ArrayRef<ParamKind> classifyPointerParams(const clang::FunctionDecl *func);

  /// Principal-kind classification of a data-pointer return type (CTS-P2):
  /// each `return` site contributes one located constraint, and the
  /// function's return kind is the one consistent with all of them. The
  /// single supported kind today is a returned function address (`return
  /// &f;` / `return f;` behind a `void *` return type), which returns the
  /// plain `!emitrust.fn_ptr` value — every return site must name a
  /// function of the same mapped signature. Returning any other pointer
  /// value — in particular a cursor into a callee-local region, which
  /// would dangle — is a located rejection at the offending return.
  /// Requires the definition's body (a declaration classifies through
  /// `getDefinition`); results are cached per canonical declaration.
  FailureOr<Type> classifyPointerReturn(const clang::FunctionDecl *func,
                                        Location loc);

  /// Classifies the data-pointer RESULT of a function-pointer type (CTS-S,
  /// 00089): the result is representable exactly when at least one function
  /// of the TU has its address taken with the same (canonical, unqualified)
  /// data-pointer return type and EVERY such candidate classifies to the
  /// erased single-global-base return kind with one common base — then any
  /// value of the fn-ptr type can only designate a function returning that
  /// global's address, so the pointer result erases from the fn_ptr
  /// signature and indirect calls route to the base like direct calls.
  /// Restricted to single-TU imports (the candidate set must be
  /// whole-program). Memoized per canonical pointee type; failures are
  /// located rejections.
  FailureOr<const clang::VarDecl *>
  classifyFnPtrPointerResult(const clang::FunctionType *fnType, Location loc);

  /// Returns the single-global-base routed to by an erased-pointer-return
  /// call (CTS-S, 00089), or null: `expr` (stripped of trivia) must be a
  /// call whose direct callee classified to the erased global-return kind,
  /// or an indirect call through a fn-ptr type whose data-pointer result
  /// classified the same way. On success `*callOut` receives the call.
  const clang::VarDecl *
  erasedGlobalReturnCallBase(const clang::Expr *expr,
                             const clang::CallExpr **callOut) const;

  /// Pass-A scan for the fn-ptr facts of one TU (CTS-S, 00189/00089):
  /// records which file-scope function-pointer variables are assigned or
  /// address-taken in any body (`fnPtrGlobalsWritten`), which functions
  /// have their address taken outside a direct-call callee position
  /// (`addressTakenFunctions`), and then plans the devirtualization
  /// aliases (`fnPtrAliases`): a file-scope function pointer initialized
  /// to a known function, never written, and — unless internally linked —
  /// imported as the sole TU, aliases its target. A variadic target
  /// aliases only when it is the hosted definition-less `printf`/`fprintf`
  /// (calls route through the printf machinery); any other shape keeps the
  /// ordinary import path and its located rejections.
  void planFnPtrAliases(const clang::TranslationUnitDecl *unit);

  /// Returns the devirtualization target when `call`'s callee (through
  /// parens, the decay/deref cancellation, and implicit casts) names an
  /// aliased global function pointer (CTS-S, 00189); null otherwise. On
  /// success `*aliasVar` (when supplied) receives the alias variable.
  const clang::FunctionDecl *
  devirtualizedCallee(const clang::CallExpr *call,
                      const clang::VarDecl **aliasVar = nullptr) const;

  /// Emits a statement-position call through a devirtualized alias of the
  /// hosted variadic `fprintf`/`printf` (CTS-S, 00189) via the printf
  /// machinery. For `fprintf` the leading stream argument must be the
  /// literal `stdout` and is swallowed with the fprintf->printf routing —
  /// the ONLY position where a FILE* value is accepted; the format string
  /// and conversions then translate exactly like a direct printf call.
  LogicalResult emitAliasedPrintf(const clang::CallExpr *call,
                                  const clang::FunctionDecl *target);

  /// Emits a call through a devirtualized non-variadic alias (CTS-S,
  /// 00189) as a DIRECT `func.call` to the target: the alias variable's
  /// fn-ptr type maps and signature-checks against the imported target
  /// exactly like a fn-ptr constant would, but no fn_ptr value and no
  /// call_indirect is created. A variadic (printf-routed) alias reaching
  /// this value-position path is rejected: its result must be unused.
  FailureOr<Value> emitDevirtualizedCall(const clang::CallExpr *call,
                                         const clang::VarDecl *var,
                                         const clang::FunctionDecl *target,
                                         Location loc);

  /// Returns whether `func`'s data-pointer return classifies as an
  /// integer carrier (CTS-P3): the definition exists and EVERY return site
  /// yields a carrier value — a null pointer constant, an
  /// integer-to-pointer cast, a read of a carrier-region local, or a call
  /// to another carrier-returning function. Such a function returns a
  /// plain i64 (null is the i64 zero). Memoized per canonical declaration;
  /// recursion cycles classify pessimistically as non-carrier.
  bool isCarrierReturnFunction(const clang::FunctionDecl *func);

  /// Returns whether one return-site expression yields an integer-carrier
  /// value, consulting `regions` (the definition body's own analysis) for
  /// reads of carrier-region locals.
  bool isCarrierReturnExpr(PointerRegionAnalysis &regions,
                           const clang::Expr *expr);

  //===--------------------------------------------------------------------===//
  // Owner planning (Phase-4 Pass A)
  //===--------------------------------------------------------------------===//

  /// Pure-AST interprocedural pre-pass over every function definition of
  /// the translation unit (no IR is built). System-header definitions are
  /// excluded — they are never imported, so they can never be methods. Runs the per-function
  /// `PointerRegionAnalysis` on each body, unifies each pointer call
  /// argument's root object with the callee definition's parameter in a
  /// program-wide union-find, and promotes every class that satisfies ALL
  /// of: single storage base; base is a local array of 1..32 elements whose
  /// element type equals every unified parameter's pointee; at least one
  /// unified parameter (the region crosses a function boundary); every
  /// unified function is defined in this TU, has a value (non-pointer)
  /// return type, is not the owner or C `main`, resolves ALL of its own
  /// data-pointer parameters into this one class, and — unless this TU is
  /// the whole program — has internal linkage (so no unseen TU can call
  /// it). Qualified classes populate `ownerPlans` and `methodPlans`; every
  /// disqualification is a silent fallback to the Phase-1b slice lowering.
  /// Regions cannot cross translation units (the base is a local), so the
  /// pass runs independently per TU in a project import.
  void planOwners(const clang::TranslationUnitDecl *unit,
                  bool soleTranslationUnit);

  /// Side-effect-free AST mirror of `emitPointerRValue`'s base resolution:
  /// returns the single object a pointer-typed call argument points into (a
  /// local array or scalar, or a pointer parameter of the calling
  /// function), or null when no single root is statically known. `regions`
  /// is the calling function's per-body analysis, consulted for pointer
  /// locals.
  const clang::VarDecl *resolveArgRoot(PointerRegionAnalysis &regions,
                                       const clang::Expr *expr) const;

  /// Collects the function definitions the Pass-A planners analyze: every
  /// function of `unit` whose body is defined here, is not variadic, and
  /// lives outside a system header — the shared traversal seed of
  /// `planOwners` and `planCellSlices`, which stay separately invoked
  /// passes in a fixed order (fusing their TU traversals would change the
  /// inter-analysis evaluation order and is deferred).
  SmallVector<const clang::FunctionDecl *>
  collectPassAFunctionDefinitions(const clang::TranslationUnitDecl *unit) const;

  //===--------------------------------------------------------------------===//
  // Cell-slice planning (CTS-P10 Pass A)
  //===--------------------------------------------------------------------===//

  /// Pure-AST interprocedural pre-pass over every function definition of
  /// the translation unit, run alongside `planOwners`. It builds a
  /// union-find over data-pointer parameters and mutable global array
  /// bases from exactly two argument shapes — the direct decay of a global
  /// array (`f(G)`) and the forwarding of another cell-slice-candidate
  /// parameter (`f(p)`, which is what makes Hanoi's permuted recursion
  /// classify) — and qualifies every class ALL of whose storage bases are
  /// mutable, internal-or-sole-TU global arrays of one supported scalar
  /// element type, whose parameters are never walked, reassigned,
  /// null-checked, escaped, or joined by a pointer local, and whose
  /// functions are defined here (and internal unless this TU is the whole
  /// program). Qualified parameters populate `cellSliceParams`
  /// (`classifyPointerParams` reports them as `ParamKind::CellSlice`); a
  /// class with global bases that hits the Mixed or NullableGlobal
  /// boundary records a `CellSliceReject` per global base, which the
  /// call-site rejection consults for its precise wording. Every other
  /// disqualification silently keeps the historical staged-copy rejection.
  void planCellSlices(const clang::TranslationUnitDecl *unit,
                      bool soleTranslationUnit);

  /// Returns the global array variable a call argument decays directly
  /// (`f(G)` with no offset), or null: the only global argument shape the
  /// cell-slice class admits.
  const clang::VarDecl *
  asDecayedGlobalArrayArg(const clang::Expr *expr) const;

  /// Returns the pointer parameter a call argument reads directly
  /// (`f(p)`), or null.
  const clang::ParmVarDecl *asPointerParamRead(const clang::Expr *expr) const;

  /// Returns the cell-slice access `expr` denotes — a subscript `p[i]` or
  /// dereference `*p` whose base reads a parameter bound to a
  /// `!emitrust.ref<!emitrust.cell_slice<T>>` value — or nothing.
  std::optional<CellSliceAccess>
  matchCellSliceAccess(const clang::Expr *expr) const;

  /// Emits `emitrust.cell_get` for the access: the parameter's reference
  /// SSA value indexed at the i64-converted index (0 for a dereference).
  FailureOr<Value> emitCellSliceGet(const CellSliceAccess &access,
                                    Location loc);

  /// Emits `rhs` and stores it with `emitrust.cell_set` (converting the
  /// value to the element type as C assignment does).
  LogicalResult emitCellSliceAssign(const CellSliceAccess &access,
                                    const clang::Expr *rhs, Location loc);

  /// Emits the located rejection for passing a pointer into the global
  /// `base` to a function: the precise cell-slice boundary wording when
  /// Pass A recorded one (mixed local+global class, nullable
  /// global-backed parameter), the historical staged-copy wording
  /// otherwise.
  LogicalResult rejectGlobalPointerArgument(Location loc,
                                            const clang::VarDecl *base);

  //===--------------------------------------------------------------------===//
  // Pointer struct members (CTS-P2)
  //===--------------------------------------------------------------------===//

  /// Merges one member-pointer binding fact into the program-wide map:
  /// first binding wins the slot, an identical rebinding is idempotent, a
  /// differing target records the clash with both sites, and an invalid
  /// fact propagates first-wins.
  void mergeMemberPointerFacts(const MemberPointerKey &key,
                               const MemberPointerFacts &incoming);

  /// Walks the constant-evaluated initializer `value` of the global struct
  /// object keyed `instance` (in parallel with its C type), recording a
  /// member binding for every data-pointer field: an address-of-object
  /// lvalue at offset 0 binds the object degenerately, a string-literal
  /// lvalue binds the literal (write-only), a null pointer leaves the
  /// member unbound, and anything else records an invalid binding. Arrays
  /// recurse per element (two elements binding different targets clash
  /// into an invalid fact, and reads through subscripted instances are
  /// unresolvable anyway — sound either way).
  void collectGlobalMemberBindings(const clang::VarDecl *instance,
                                   const clang::APValue &value,
                                   clang::QualType type,
                                   clang::SourceLocation loc);

  /// Resolves the member-pointer binding a read or write of `member` (a
  /// data-pointer field access) consults: the instance is the directly
  /// named base variable (`s.f`) or the degenerate single object behind a
  /// decomposed arrow base (`p->f`, `s->f` through a pointer global).
  /// Rejects — with the located first/second sites — poisoned fields,
  /// unknown instances, unbound members, invalid bindings, bindings to
  /// locals of other functions, and (in a multi-file project) members of
  /// externally visible global instances.
  FailureOr<const MemberPointerFacts *>
  resolveMemberPointerBinding(const clang::MemberExpr *member, Location loc);

  /// Emits `s.f = rhs` on a data-pointer member: the analysis pinned the
  /// member's static binding, so a right-hand side that decomposes to the
  /// bound target emits no runtime code at all (the stored i64 stays 0 and
  /// carries no information); any other right-hand side is a located
  /// rejection.
  LogicalResult emitMemberPointerAssign(const clang::MemberExpr *member,
                                        const clang::Expr *rhs, Location loc);

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Imports a complete struct definition as a module-level
  /// `emitrust.struct_def`. Forward declarations are ignored; repeated
  /// imports of the same definition are deduplicated. A file-scope record
  /// is emitted under its tag (or anonymous-typedef) name with cross-TU
  /// name/shape deduplication; a block-scope record is its own type per
  /// defining decl and is emitted under a mangled name (see
  /// `localRecordNames`). A bare anonymous struct (no tag, no typedef
  /// name) receives a synthesized `Anon<n>` name keyed by its field shape
  /// (see `anonRecordShapeNames`); repeated occurrences of the same
  /// anonymous shape share one struct_def. An empty member list
  /// (`struct T {};`) imports as a field-less struct_def. The field list
  /// is flattened through `collectRecordFields`, which resolves C11
  /// 6.7.2.1p13 anonymous struct/union members, packs bit-field runs
  /// into `__bits<n>` backing fields (C99-45), and mangles Rust-keyword
  /// member spellings. A union definition imports as a ONE-FIELD struct
  /// through `collectUnionSlot` (its storage field is the first arm's
  /// leaf); union shapes outside that model (including bit-field arms)
  /// and unsupported field types are rejected.
  LogicalResult importRecord(const clang::RecordDecl *record, Location loc);

  /// Appends the flattened field list of `record` to
  /// `fieldNames`/`fieldTypes`, resolving C11 6.7.2.1p13 anonymous
  /// members (an unnamed member whose type is an anonymous struct or
  /// union, `FieldDecl::isAnonymousStructOrUnion`): an anonymous struct
  /// member's fields join the parent's member namespace, so they are
  /// injected in place under their own spellings (Sema has already
  /// enforced their uniqueness there); an anonymous union member is
  /// representable without a union type exactly when every arm flattens
  /// to a single leaf field and all leaves map to one identical type —
  /// the arms then alias a single storage slot named after the first
  /// leaf (recorded in `unionSlotStorage`), which is exact because
  /// reading any union member with the type of the last store yields
  /// that stored value. Any other anonymous union rejects exactly as
  /// union types do elsewhere ("unsupported: union type", CTS-R3); other
  /// unnamed members are rejected with the field's location. Member
  /// names that are Rust keywords mangle with a trailing underscore
  /// (`mangleMemberName`); a final spelling that collides with another
  /// member's is a located rejection. Bit-field members pack per run
  /// (C99-45): each maximal run of consecutively declared bit-field
  /// members packs LSB-first in declaration order into one synthesized
  /// backing field `__bits<n>` (`n` counts runs from 0 across the whole
  /// flattened record, threaded through `bitFieldRuns`) of the smallest
  /// unsigned type (ui8/ui16/ui32/ui64) holding the run's total bits;
  /// runs split at any non-bit-field member. The bit-field members' own
  /// names never appear in the struct_def; their accessors are recorded
  /// in `bitFieldAccessInfo`. Zero-width and anonymous bit-fields, and
  /// runs wider than 64 bits, are located rejections.
  LogicalResult
  collectRecordFields(const clang::RecordDecl *record,
                      SmallVectorImpl<llvm::StringRef> &fieldNames,
                      SmallVectorImpl<Type> &fieldTypes,
                      unsigned &bitFieldRuns);

  /// Resolves one arm of an anonymous union member to its single
  /// flattened leaf field, descending through nested anonymous struct
  /// members. Fails — with the union-type rejection at `unionLoc` — when
  /// the arm flattens to zero or several fields, which the single-slot
  /// aliasing of `collectRecordFields` cannot model.
  FailureOr<const clang::FieldDecl *>
  anonymousUnionArmLeaf(const clang::FieldDecl *arm, Location unionLoc);

  /// Appends the single storage slot of a union definition to
  /// `fieldNames`/`fieldTypes`: a union imports as a ONE-FIELD struct
  /// whose storage field carries the slot arm's name and mapped type,
  /// generalizing the anonymous-union slot aliasing of
  /// `collectRecordFields` (CTS-R2) to named and untagged union types.
  /// The slot is the first arm, EXCEPT in the byte-array mix (CTS-F,
  /// 00210): a union pairing a non-array arm with constant integer-array
  /// arms takes the first NON-ARRAY arm as its slot regardless of
  /// declaration order. Every non-slot arm is recorded in
  /// `unionSlotStorage` as an alias of that slot. An arm is admitted
  /// when it maps to the identical type (exact: reading any union member
  /// with the type of the last store yields that stored value), when
  /// both arms are integers of the same width differing only in
  /// signedness — accesses through such an arm reinterpret the slot
  /// bit-exactly via `emitrust.cast` (two's complement, C99 6.5.2.3) —
  /// or when the arm is a constant integer ARRAY whose total width
  /// equals the integer slot's (CTS-F, 00210): such a byte-array arm is
  /// a TYPE-level concession recorded in `unionByteArrayArms`, and every
  /// access through it rejects at the access site. Bit-field arms,
  /// unnamed/anonymous arms, pointer arms, integer arms of differing
  /// sizes, mixed non-integer arms, and empty unions are rejected with
  /// located `unsupported: union ...` diagnostics at the union
  /// definition.
  LogicalResult collectUnionSlot(const clang::RecordDecl *definition,
                                 SmallVectorImpl<llvm::StringRef> &fieldNames,
                                 SmallVectorImpl<Type> &fieldTypes);

  /// Returns the field that provides `field`'s storage in its flattened
  /// parent struct_def: the aliased first-arm slot for a union arm
  /// recorded by `collectUnionSlot`/`collectRecordFields`, or `field`
  /// itself.
  const clang::FieldDecl *
  flattenedFieldStorage(const clang::FieldDecl *field) const;

  /// If `expr` (modulo parens and implicit trivia) reads a union arm
  /// whose mapped type differs from its storage slot's — a signedness
  /// pun admitted by `collectUnionSlot` — reinterprets `value` (the
  /// loaded slot value) as the arm's own mapped type with a bit-exact
  /// `emitrust.cast`; otherwise returns `value` unchanged.
  FailureOr<Value> reinterpretUnionArmRead(const clang::Expr *expr,
                                           Value value, Location loc);

  /// If the assignment target `expr` is a union arm whose mapped type
  /// differs from its storage slot's, casts `value` (the assigned value,
  /// of the arm's type) to the slot's type so the store lands bit-exactly
  /// on the slot; otherwise returns `value` unchanged.
  FailureOr<Value> reinterpretUnionArmWrite(const clang::Expr *expr,
                                            Value value, Location loc);

  /// Returns the spelling `field` carries in its flattened parent
  /// struct_def: its own name or, for a union arm aliased by
  /// `collectUnionSlot`/`collectRecordFields`, the storage slot's name —
  /// mangled through `mangleMemberName` when the C spelling is a Rust
  /// keyword, matching the spelling `collectRecordFields` emitted.
  std::string flattenedFieldName(const clang::FieldDecl *field) const;

  /// Returns the Rust type name `definition` was imported under: the
  /// mangled block-scope name recorded by `importRecord`, the
  /// collision-resolved name assigned by `structSymbolName` for a
  /// file-scope record, the synthesized `Anon<n>` name for a bare
  /// anonymous struct, or the tag (or anonymous-typedef) name as the
  /// fallback. Empty only for an anonymous struct that was never imported.
  std::string emittedRecordName(const clang::RecordDecl *definition) const;

  /// Returns the MLIR/Rust symbol name assigned to a struct definition,
  /// modeling C's separate tag and ordinary identifier namespaces (C99
  /// 6.2.3): `struct a` and a global or function `a` may coexist in C, but
  /// the module has a single symbol table, so the tag is deterministically
  /// renamed to `Struct_<tag>` when — and only when — the ordinary
  /// namespace also claims the name. The common, collision-free case keeps
  /// the readable tag spelling. The decision is cached per defining
  /// declaration so every mention of the type agrees. Returns an empty
  /// string for an anonymous struct (callers reject with their own
  /// located message) and failure when even the renamed spelling is
  /// claimed by an ordinary identifier.
  FailureOr<std::string> structSymbolName(const clang::RecordDecl *definition,
                                          Location loc);

  /// Whether `name` is claimed by C's ordinary identifier namespace: either
  /// the current TU's pre-scanned ordinary names (functions, file-scope
  /// variables, mangled function-local statics) or an already-imported
  /// module symbol other than a struct definition (a global or function
  /// from a previously imported TU).
  bool ordinaryNameTaken(llvm::StringRef name) const;

  /// Pre-scans a translation unit and records in `ordinaryTuNames` every
  /// module-symbol name its ordinary identifier namespace will claim:
  /// function names (after `main` -> `c_main` and internal-linkage TU-tag
  /// mangling), file-scope variable names (with the same internal-linkage
  /// mangling), and function-local statics under their `<function>_<name>`
  /// mangle. Runs before any struct type is imported so tag renaming
  /// (`structSymbolName`) is independent of declaration order.
  void collectOrdinaryNames(const clang::TranslationUnitDecl *unit);

  /// Walks a function body and records the `<function>_<name>` mangled
  /// spelling of every function-local static in `ordinaryTuNames`.
  void collectStaticLocalNames(const clang::Stmt *stmt,
                               llvm::StringRef funcName);

  /// Imports a complete named enum definition as a module-level
  /// `emitrust.enum_def`. Incomplete and anonymous enums are silently
  /// skipped (anonymous enumerators are imported as plain `i32` constants
  /// at their use sites); repeated imports of the same definition are
  /// deduplicated. Enumerator spellings become Rust variant names verbatim,
  /// so spellings that are Rust keywords are rejected, as are values
  /// outside the `i32` range and duplicate values (the generated Rust enum
  /// needs one variant per discriminant).
  LogicalResult importEnum(const clang::EnumDecl *enumDecl, Location loc);

  /// Imports a function declaration or definition as a `func.func`. C
  /// `main` is renamed to `c_main`. Body-less variadic declarations (such
  /// as printf's) are skipped; a variadic definition whose body never
  /// touches va_list imports as its fixed prototype — the named
  /// parameters only, with call sites dropping effect-free trailing
  /// extras in `emitCall` (CTS-P9) — while a definition that does use
  /// va_list is rejected. A
  /// body-less prototype with no definition in this TU is skipped when
  /// nothing in this TU references it (referenced-only policy). A body
  /// replaces a previously imported body-less declaration of the same name.
  /// Block-scope prototypes (which C gives external linkage) are imported
  /// through this same path by `emitStmt`; the module-scope insertion point
  /// is guarded, so a mid-body call leaves the caller's insertion point
  /// untouched.
  LogicalResult importFunction(const clang::FunctionDecl *func);

  /// Records every local variable whose address is taken with `&x` inside
  /// `stmt`; such scalars become `emitrust.variable` places instead of
  /// promotable memref cells. This also covers the degenerate bases of the
  /// pointer decomposition (`int *p = &x` marks `x`); array and struct
  /// bases are `emitrust.variable` places regardless.
  void collectAddressTaken(const clang::Stmt *stmt);

  /// Emits an owner-promoted local array (Phase 4): synthesizes the
  /// module-level `emitrust.struct_def @Owner_... ["data"]` on first need
  /// (a symbol collision is a located rejection, mirroring `createGlobal`),
  /// declares the owner as an `emitrust.variable` of the struct type
  /// (rendered `Owner::default()`), and registers the `member("data")`
  /// place as the variable's symbol so that every direct access — and every
  /// decomposed pointer whose region base it is — rewrites to the struct's
  /// array field. The owner struct place itself is recorded separately for
  /// method-call receivers and is never loaded whole.
  LogicalResult emitOwnerLocal(const clang::VarDecl *var, Location loc);

  /// Validates a pointer-typed local variable against its
  /// `PointerRegionAnalysis` region and, when the region is decomposable
  /// (local bases, no escapes, no arithmetic on a scalar base),
  /// registers its decomposition: an entry-block `memref<i64>` cursor cell
  /// for an array base, or no runtime state at all for a degenerate scalar
  /// or struct base. A multi-base region (CTS-P7) additionally registers
  /// an entry-block `memref<i32>` enum-of-bases discriminant cell per
  /// pointer when every base is local and of one uniform kind (all
  /// element runs of the pointee's element type, or all degenerate
  /// scalars of the pointee type); other multi-base shapes keep the
  /// located join rejection naming the first two objects and bindings. A string-literal region registers a cursor cell plus
  /// the literal's read-only backing array (see
  /// `getOrCreateLiteralBacking`) and rejects regions that are written
  /// through or that join a literal with an object. A nullable region
  /// (one that sees a null pointer constant, CTS-P8) additionally
  /// registers an entry-block `memref<i1>` "non-null" flag cell per
  /// pointer — the Option-of-cursor discriminant — including for a
  /// nullable region with no base at all (a pointer that only ever holds
  /// null, which supports null-checks but rejects dereference); a
  /// nullable string-literal region is rejected. Undecomposable regions
  /// produce located diagnostics at the offending construct.
  LogicalResult emitPointerLocal(const clang::VarDecl *var, Location loc);

  /// Emits the declaration of a second-order pointer local (`T **pp`,
  /// CTS-P5). The accepted shape is the degenerate one-cell region of
  /// cursor cells: `pp` statically selects a single first-order pointer
  /// local, recorded in `pointerPointerLocals`, and needs no runtime state
  /// of its own — `*pp` designates the target's (base, cursor)
  /// decomposition and `**pp` is an indirect use of it, so no
  /// reference-to-reference ever arises in the emitted Rust. Third-order
  /// pointers, pointers to function pointers, multi-target selections, and
  /// every invalidated shape are located rejections.
  LogicalResult emitPointerPointerLocal(const clang::VarDecl *var,
                                        Location loc);

  /// Returns the tracked second-order pointer `pp` of a stripped `*pp`
  /// dereference expression, or null when `expr` is not one.
  const clang::VarDecl *secondOrderDerefVar(const clang::Expr *expr) const;

  /// Emits the read of the decomposed pointer local (or slice-classified
  /// parameter) `var`: its static base plus the current value of its
  /// cursor cell and, in a nullable region, its non-null flag cell.
  /// Shared by the direct read (`p` in pointer-value position) and the
  /// second-order dereference (`*pp`, which reads the selected pointer).
  FailureOr<PtrExprValue> emitPointerLocalRead(Location loc,
                                               const clang::VarDecl *var);

  /// Emits `ptr = rhs` for a decomposed pointer local by recomputing and
  /// storing its cursor; a degenerate binding (`p = &x`) needs no cursor
  /// code at all because the target place is statically known. For a
  /// pointer of a nullable region the assignment also stores the
  /// Option-of-cursor discriminant: false for `ptr = NULL`, true for an
  /// address binding, and the source pointer's own flag for `ptr = q`.
  LogicalResult storePointerAssign(Location loc, const clang::VarDecl *ptr,
                                   const clang::Expr *rhs);

  /// Emits `p += n` / `p -= n` on a decomposed pointer local as cursor
  /// arithmetic: load the i64 cursor cell, add or subtract the widened
  /// amount, and store the cursor back.
  LogicalResult
  emitPointerCompoundAssign(const clang::CompoundAssignOperator *op);

  /// Imports a file-scope variable as a module-level `emitrust.global`.
  /// Redeclarations are reconciled the way C does: the definition (or a
  /// tentative definition) provides the type and, through
  /// `getAnyInitializer`, the initializer. A variable that is only ever
  /// `extern`-declared in this TU is skipped when nothing references it
  /// (referenced-only policy); when referenced it is deferred for cross-TU
  /// resolution (project import) or rejected (single-file import).
  /// Data-pointer-typed variables route to `importPointerGlobal`.
  /// Thread-locals are rejected.
  LogicalResult importGlobalVar(const clang::VarDecl *var);

  /// Imports a pointer-typed file-scope variable under the CTS-P4 global
  /// region model. The program-wide facts merged by `planOwners` combine
  /// with the file-scope initializer's constant-evaluated binding (clang
  /// APValue lvalue: base declaration plus byte offset); the pointer must
  /// resolve to exactly one region base, which is one of:
  ///  - a global scalar or struct object (`int *p = &x;`): degenerate, no
  ///    runtime state, every access resolves statically to the base;
  ///  - a global array: an i64 cursor `emitrust.global` named after the
  ///    pointer, initialized to the initializer's element offset (or 0);
  ///  - a file-scope compound literal (`&(struct S){1, 2}`): a synthesized
  ///    constant-initialized backing global named `<name>_backing`;
  ///  - a file-scope string-literal initializer (`char *s = "...";`,
  ///    CTS-L3): a synthesized immutable `<name>_backing` byte-array
  ///    global (the literal's ASCII bytes plus the terminating NUL, the
  ///    CTS-P1 read-only backing lifted to module scope) plus a cursor
  ///    global; writes through the region are rejected up front;
  ///  - a single constant-size `calloc`/`malloc` site: a synthesized
  ///    zero-initialized backing array global plus a cursor global.
  /// An unreferenced pointer global imports nothing (referenced-only
  /// policy, matching extern declarations). Located rejections: a binding
  /// to a local object (the borrow would outlive the object — the exact
  /// program rustc refuses), multiple bases, body bindings to string
  /// literals, copying a global pointer, address-of, null constants,
  /// external linkage in a multi-file project, and type/base mismatches.
  LogicalResult importPointerGlobal(const clang::VarDecl *key,
                                    const clang::VarDecl *decl,
                                    llvm::StringRef symbolName, Location loc);

  /// Emits `g = rhs` for an imported pointer-typed global: a recognized
  /// allocation call re-zeroes the synthesized backing (exact calloc
  /// semantics; malloc's contents are indeterminate, so zero-filling is a
  /// legal refinement) and resets the cursor; any other right-hand side
  /// decomposes and must resolve into the pointer's region, storing its
  /// cursor with `emitrust.global_store` (nothing for degenerate bases,
  /// whose target place is statically known).
  LogicalResult storeGlobalPointerAssign(Location loc,
                                         const clang::VarDecl *ptr,
                                         const PointerGlobalInfo &info,
                                         const clang::Expr *rhs);

  /// Creates the `emitrust.global` named `symbolName` for the declaration
  /// `decl` (which supplies the type and initializer) and registers it
  /// under the canonical declaration `key`. Rejects Rust-keyword names,
  /// the reserved `__emitrust_tl` accessor-binder name,
  /// module symbol collisions, pointer types, and non-constant or aggregate
  /// initializers. `const`-qualified variables become immutable globals
  /// unless struct-typed (a struct default is not const-evaluable in Rust).
  LogicalResult createGlobal(const clang::VarDecl *key,
                             const clang::VarDecl *decl,
                             llvm::StringRef symbolName, Location loc);

  /// Registers an `extern`-only global reference (project import) without
  /// creating an `emitrust.global`: records the mapping so uses in this TU
  /// resolve and remembers `symbolName` for the post-merge check that some TU
  /// really defines it. Rejects pointer-typed and unmappable globals.
  LogicalResult deferExternGlobal(const clang::VarDecl *key,
                                  llvm::StringRef symbolName,
                                  clang::QualType qualType, Location loc);

  /// Evaluates `decl`'s initializer as a constant (clang APValue
  /// evaluation) and converts it to an attribute of `type`. Supports
  /// integer (including `_Bool` and char) and floating-point constants,
  /// function-pointer initializers as opaque `None`/`Some(name)`
  /// attributes, and array/struct aggregates as ArrayAttr element lists
  /// (via `convertAPValueInit`); non-constant expressions are rejected
  /// with located diagnostics.
  FailureOr<Attribute> convertGlobalInit(const clang::VarDecl *decl,
                                         Type type, Location loc);

  /// Converts a constant-evaluated `clang::APValue` to the initializer
  /// attribute for a global of value type `type`: IntegerAttr/BoolAttr for
  /// integers, FloatAttr for floats, and a (possibly nested) ArrayAttr with
  /// one entry per array element or flattened struct field for
  /// aggregates. Array holes left by partial or designated
  /// initialization take the array filler (C99 zero-fill); struct field
  /// types resolve through the module-level `emitrust.struct_def`, and a
  /// struct with flattened anonymous members converts along the C field
  /// structure via `convertRecordAPValue`. `cType` is the C type walked
  /// in parallel: a data-pointer field (stored as an i64 cursor member,
  /// CTS-P2) converts to 0 — its lvalue APValue carries no stored
  /// representation; the member's binding is recorded separately by
  /// `collectGlobalMemberBindings`. Anything else (enum-typed elements,
  /// non-member pointers) is rejected with a located diagnostic.
  FailureOr<Attribute> convertAPValueInit(const clang::APValue &value,
                                          Type type, clang::QualType cType,
                                          Location loc);

  /// Maps the C type of one struct field: a data-pointer field is stored
  /// as a plain i64 cursor member (CTS-P2 — a cursor is a borrow-free Copy
  /// integer, so a struct can hold one; the member's target object is
  /// resolved statically per instance and the stored value carries no
  /// information in the degenerate model); every other type maps through
  /// `mapType`.
  FailureOr<Type> mapStructFieldType(clang::QualType type, Location loc);

  /// Converts the struct `APValue` of `record` to one attribute per
  /// flattened struct_def field, appended to `fields`. `fieldTypes` is
  /// the struct_def's flattened type list and `typeIndex` the cursor into
  /// it, advanced per emitted field: a plain field converts positionally,
  /// an anonymous struct member recurses into its struct value, and an
  /// anonymous union member converts its single aliased storage slot via
  /// `convertAnonymousSlotInit`.
  LogicalResult convertRecordAPValue(const clang::APValue &value,
                                     const clang::RecordDecl *record,
                                     ArrayAttr fieldTypes, unsigned &typeIndex,
                                     SmallVectorImpl<Attribute> &fields,
                                     Location loc);

  /// Converts the constant value of a union — a flattened anonymous
  /// union member, or a named/untagged union type imported as a
  /// one-field struct by `collectUnionSlot` — to the attribute of its
  /// single storage slot of type `slotType`: the union's active arm
  /// descends through nested anonymous members to the slot's scalar
  /// value; a union with no active arm takes the slot's zero value (C99
  /// zero-fill).
  FailureOr<Attribute>
  convertAnonymousSlotInit(const clang::APValue &value,
                           const clang::RecordDecl *record, Type slotType,
                           Location loc);

  /// Returns the imported global for `decl`, or null when `decl` is not an
  /// imported variable with static storage duration.
  const GlobalInfo *lookupGlobal(const clang::ValueDecl *decl) const;

  /// Returns the canonical declaration when `expr` (ignoring parens) is a
  /// direct reference to an imported global, or null otherwise.
  const clang::VarDecl *asDirectGlobalRef(const clang::Expr *expr) const;

  /// Returns whether the place expression `expr` (a declaration reference,
  /// or member/subscript chains over one) is rooted at an imported global;
  /// used to reject taking the address of a global.
  bool rootsAtGlobal(const clang::Expr *expr) const;

  /// Stores a staged copy back where it came from, if `writeback` captured
  /// one; no-op otherwise. A staged global copy stores back into its
  /// global; a staged multi-base element (CTS-P7) dispatches on the
  /// discriminant and stores into the selected base at the staged cursor.
  LogicalResult flushGlobalWriteback(Location loc,
                                     const GlobalWriteback &writeback);

  /// Commits a mutation of a place rooted at a staged global copy: applies
  /// `mutate` (the caller's store into the staged place) and flushes
  /// `writeback`. When `refreshStaged` is set — the statement evaluated a
  /// side-effecting subexpression (an RHS call, a subscript-index call)
  /// AFTER the staging load — the staged whole-value copy is first rebound
  /// to a fresh snapshot of the global, so the flush's whole-value
  /// store-back cannot revert a write that intervening code made to
  /// another subobject of the same global (C11 6.5.16p3: the RHS's side
  /// effects are sequenced before the assignment's store, and the store
  /// writes only the designated subobject). Multi-base writebacks need no
  /// refresh: their flush re-stages the active base afresh and merges only
  /// the staged element (see `flushGlobalWriteback`). Every staged-global
  /// write path (assignment, compound assignment, wide-byte stores,
  /// ++/--) must route its mutation through this seam.
  LogicalResult
  commitGlobalWriteback(Location loc, const GlobalWriteback &writeback,
                        bool refreshStaged,
                        llvm::function_ref<LogicalResult()> mutate);

  /// Returns whether either operand of the (compound) assignment `op`
  /// contains a side-effecting subexpression — the conservative trigger
  /// for `commitGlobalWriteback`'s staged-copy refresh: only such a
  /// subexpression (an RHS call, a subscript-index call in the LHS) can
  /// have written the staged global after the staging load. Pure
  /// statements skip the refresh and emit exactly the historical IR.
  bool assignStalenessRisk(const clang::BinaryOperator *op) const {
    return op->getLHS()->HasSideEffects(astContext()) ||
           op->getRHS()->HasSideEffects(astContext());
  }

  /// Emits one dispatch over the closed set of `bases` of a multi-base
  /// region (CTS-P7): a chain of `baseIndex == i` conditional branches
  /// with one arm block per base (the last base is the final else — the
  /// discriminant can only ever hold a bound index), each arm populated
  /// by `emitArm`, all joining in a fresh continuation block where the
  /// insertion point is left.
  LogicalResult emitMultiBaseDispatch(
      Location loc, ArrayRef<PointerBaseKey> bases, Value baseIndex,
      llvm::function_ref<LogicalResult(const PointerBaseKey &)> emitArm);

  /// Materializes the element place of the local base `base` at `cursor`
  /// (null for a degenerate scalar or member base, which resolves to the
  /// object's — or member's — own place): the base's registered place,
  /// projected through the member for a `&struct.member` base (CTS-P9)
  /// and refined through `refineElementPlace`. Used per dispatch arm of a
  /// multi-base pointer; global-member arms stage the global instead (see
  /// `stageGlobalCopy`).
  FailureOr<Value> materializeLocalElementPlace(Location loc,
                                                const PointerBaseKey &base,
                                                Value cursor,
                                                Type pointeeType);

  /// Stages the whole value of the global region base `base` (a real
  /// global object, or a pointer global's synthesized backing) in a fresh
  /// local `emitrust.variable` copy — the staged-copy model of direct
  /// global element accesses — returning the staging place and the global
  /// symbol a write context must store the copy back into.
  FailureOr<std::pair<Value, std::string>>
  stageGlobalCopy(Location loc, const clang::VarDecl *base);

  /// Stages `base`'s whole global value through `stageGlobalCopy` and,
  /// when the caller passes a write context, records the staging place
  /// and symbol in `*writeback` so the mutation flushes through
  /// `commitGlobalWriteback`. Returns the staging place the access
  /// refines — the one staged-copy read-side idiom shared by every
  /// global region access site.
  FailureOr<Value> stageGlobalCopyAndRecord(Location loc,
                                            const clang::VarDecl *base,
                                            GlobalWriteback *writeback);

  /// Projects the member place `field` designates on the struct place
  /// `basePlace` (an `emitrust.member` with the flattened field name),
  /// used to resolve a `&struct.member` region base (CTS-P9).
  FailureOr<Value> projectMemberPlace(Location loc, Value basePlace,
                                      const clang::FieldDecl *field);

  /// Refines the array-or-slice place `basePlace` down to the element the
  /// flat row-major `cursor` designates, peeling one `emitrust.subscript`
  /// per array level (dividing the cursor by the level's flat element
  /// span and continuing with the remainder) until the wrapped value type
  /// is `pointeeType`; returns `basePlace` unchanged when `cursor` is
  /// null (a degenerate whole-object pointer).
  FailureOr<Value> refineElementPlace(Location loc, Value basePlace,
                                      Value cursor, Type pointeeType);

  /// Builds the symbol reference attribute for `symbol`.
  FlatSymbolRefAttr globalSymbol(llvm::StringRef symbol) {
    return FlatSymbolRefAttr::get(builder.getContext(), symbol);
  }

  //===--------------------------------------------------------------------===//
  // Function-body plumbing
  //===--------------------------------------------------------------------===//

  /// Creates a rank-0 `memref.alloca` of `elementType` at the start of the
  /// entry block, leaving the current insertion point untouched.
  Value createEntryAlloca(Location loc, Type elementType);

  /// Appends a fresh empty block to the current function body without
  /// moving the insertion point.
  Block *createBlock();

  /// Returns the block that `label` starts, creating it on first mention
  /// (either the `goto` or the label itself may be seen first).
  Block *getLabelBlock(const clang::LabelDecl *label);

  /// Creates an `emitrust.variable` place of `type`. In a function that
  /// contains labels the op is hoisted to the start of the entry block so
  /// that a `goto` jumping over the declaration cannot leave a later use
  /// undominated by the definition; otherwise it is created at the current
  /// insertion point.
  Value createVariablePlace(Location loc, Type type);

  /// Returns true if `block` already ends with a terminator operation.
  static bool isTerminated(Block *block);

  /// Creates an `arith.constant` of integer `type` with the given value.
  /// The type must be signless (an arith invariant); use
  /// `createScalarIntConstant` when the type may be unsigned.
  Value createIntConstant(Location loc, Type type, int64_t value);

  /// Creates an integer constant of any supported integer `type`: an
  /// `arith.constant` for signless types and an `emitrust.constant` for
  /// unsigned types (arith constants must be signless).
  Value createScalarIntConstant(Location loc, Type type, int64_t value);

  /// Creates an `arith.constant` of type i1 with the given truth value.
  Value createBoolConstant(Location loc, bool value);

  /// Erases blocks unreachable from the entry block, then terminates any
  /// remaining unterminated block: void functions get a bare `func.return`,
  /// `c_main` gets an implicit `return 0`, and any other non-void function
  /// is rejected with a located diagnostic.
  LogicalResult finalizeFunction(func::FuncOp funcOp, Location loc);

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  /// Emits one statement at the current insertion point; dispatches over
  /// the supported statement kinds and rejects the rest with a located
  /// diagnostic. C labels start their mapped block (see `getLabelBlock`)
  /// and `goto` emits a `cf.br` to it followed by a fresh block for any
  /// trailing code; computed goto is rejected. Block-scope function
  /// prototypes have external linkage and are hoisted to module scope
  /// through `importFunction`; other unsupported block-scope declarations
  /// are rejected.
  LogicalResult emitStmt(const clang::Stmt *stmt);

  /// Emits a local variable declaration. Signed scalars become entry-block
  /// memref cells (initializer stored at the declaration point);
  /// aggregates, enums, function pointers, unsigned scalars, and
  /// address-taken scalars become `emitrust.variable` places. Data-pointer
  /// locals are decomposed through `emitPointerLocal`; function-pointer
  /// locals are ordinary values and bypass the decomposition entirely.
  /// Function-local statics become module-level
  /// `emitrust.global`s mangled as `<function>_<name>`; extern locals are
  /// rejected. An UNREFERENCED local VLA whose size expression is
  /// side-effect-free is elided entirely (CTS-F, 00207): no IR and no
  /// diagnostic — the object never materializes and dropping the size
  /// expression loses nothing. Referenced VLAs and dead VLAs with a
  /// side-effecting size expression keep the non-constant-array-size
  /// rejection.
  LogicalResult emitLocalVar(const clang::VarDecl *var);

  /// Populates `voidFnPtrHolders` with the admitted local `void *`
  /// fn-ptr holders of `body` (CTS-F, 00210): a local `void *` whose
  /// initializer is (an implicit cast of) `&f` or the decayed `f` for a
  /// known non-variadic function (a prototype-less K&R `f` maps to the
  /// zero-parameter form), never reassigned, and whose EVERY value use
  /// is an explicit cast to exactly `f`'s signature (C type
  /// compatibility, C11 6.2.7) in callee position
  /// (`((T (*)(...))fp)(...)`). Any other mention of the holder — a
  /// reassignment, an escaping argument, a mismatched cast —
  /// disqualifies it, keeping the existing pointer-region rejection.
  void collectVoidFnPtrHolders(const clang::Stmt *body);

  /// Emits the admitted holder local `var` (CTS-F, 00210) exactly like a
  /// directly-typed local fn-ptr: an `emitrust.variable` of the target's
  /// `!emitrust.fn_ptr` signature assigned the opaque `Some(target)`
  /// constant (the FR-29 model). The spelled `void *` type never reaches
  /// the IR.
  LogicalResult emitFnHolderLocal(const clang::VarDecl *var,
                                  const clang::FunctionDecl *target,
                                  Location loc);

  /// Emits a block-scope aggregate initializer list into the
  /// default-initialized place `place` of array or struct value type
  /// `type`: one `emitrust.assign` per explicitly initialized element
  /// (constant-index `emitrust.subscript` for array elements,
  /// `emitrust.member` for struct fields), recursing for nested lists.
  /// Elements left implicit (partial or designated initialization) keep
  /// the place's default value, which models C99 zero-fill. Works on the
  /// semantic form of the list, so designators are already resolved to
  /// positions. Aggregate-typed elements that are not initializer lists
  /// (string literals, struct copies) are rejected with located
  /// diagnostics. `instance` names the declared variable when the list
  /// initializes a struct local directly: a data-pointer field's
  /// initializer then validates against the member binding the analysis
  /// recorded and emits nothing (the stored i64 stays 0, CTS-P2); with a
  /// null `instance` a non-implicit data-pointer field initializer is
  /// rejected (its instance path is outside the member model).
  LogicalResult emitAggregateInitList(Value place, Type type,
                                      const clang::InitListExpr *list,
                                      const clang::VarDecl *instance =
                                          nullptr);

  /// Emits a (semantic-form) initializer list for the C record `record`
  /// into the parent struct place `place`, resolving flattened anonymous
  /// members: a plain field initializes through `emitrust.member` under
  /// its flattened name; an anonymous struct member's nested list
  /// recurses onto the same parent place; an anonymous union member's
  /// nested list initializes only its active arm (Sema records it on the
  /// semantic form), which lands on the arm's aliased storage slot. Called
  /// with `record->isUnion()` only for such flattened anonymous members.
  LogicalResult emitRecordInitFields(Value place,
                                     const clang::RecordDecl *record,
                                     const clang::InitListExpr *list,
                                     const clang::VarDecl *instance = nullptr);

  /// Emits the initializer `element` for `field` of a flattened record
  /// into the parent struct place `place`: an anonymous member requires a
  /// nested list and recurses via `emitRecordInitFields`; a plain field
  /// assigns through `emitrust.member` under its flattened name.
  LogicalResult emitRecordInitField(Value place, const clang::FieldDecl *field,
                                    const clang::Expr *element,
                                    const clang::VarDecl *instance = nullptr);

  /// Emits one element of an aggregate initializer list into `place` of
  /// value type `type`: recurses for a nested list, otherwise stores the
  /// element rvalue.
  LogicalResult emitInitListElement(Value place, Type type,
                                    const clang::Expr *element);

  /// Emits a block-scope `char s[N] = "..."` (or `wchar_t s[N] = L"..."`)
  /// initializer as per-element assigns including the trailing NUL (when
  /// it fits, per C99 6.7.8p14); elements beyond the literal keep the
  /// place's default zero value. An ordinary literal requires a
  /// signless-i8 (plain/signed char) array and rejects non-ASCII bytes
  /// with located diagnostics so the array's contents stay printable
  /// through the ASCII-only `%s`/`%c` helpers; a wide literal requires an
  /// i32 (`wchar_t`) array and carries its code units verbatim (a wide
  /// array never feeds those byte-string helpers, so no ASCII limit
  /// applies). u8/u/U literals stay rejected.
  LogicalResult emitStringArrayInit(Value place, Type type,
                                    const clang::StringLiteral *literal);

  /// Creates a backing byte array place for `literal`: an
  /// `emitrust.variable` of `!emitrust.array<(len+1)xi8>` initialized with
  /// the literal's bytes plus the terminating NUL (C string literals
  /// always carry one, which is what terminates strlen-style walks).
  /// Only ordinary literals whose bytes are ASCII are supported, the same
  /// policy as `emitStringArrayInit` (design.md C99-28). A `const`-marked
  /// backing (immutable `let`) is hoisted to the entry block when the
  /// function contains labels; a mutable backing (`isConst` false, used
  /// for per-call-site copies passed to slice parameters) is created at
  /// the current insertion point, immediately before its single use.
  FailureOr<Value> createLiteralBacking(const clang::StringLiteral *literal,
                                        Location loc, bool isConst);

  /// Returns the read-only backing byte array place of the string-literal
  /// pointer region bound to `literal`, creating it on first need via
  /// `createLiteralBacking` (immutable form). The created variable is
  /// cached per literal for the current function, so every pointer of the
  /// region shares one backing.
  FailureOr<Value>
  getOrCreateLiteralBacking(const clang::StringLiteral *literal,
                            Location loc);

  /// Lowers a value-position call to a definition-less `strlen`: the
  /// argument's char region (a string-literal backing, a char array, or a
  /// pointer into either — see `emitCharRegionArg`) is borrowed as a byte
  /// slice from its cursor and passed through the `__emitrust_strlen`
  /// helper (which counts bytes up to the first NUL, exactly C's strlen),
  /// cast to the call's declared result type. Arguments outside a char
  /// region are rejected with a located diagnostic.
  FailureOr<Value> emitStrlenCall(const clang::CallExpr *call);

  /// Resolves a hosted `<string.h>` argument to the (base, cursor, backing)
  /// decomposition of the char region it designates. Three shapes are
  /// accepted: a decayed string literal (its read-only backing is created
  /// on first need, cursor 0), any pointer expression the decomposition
  /// already handles (a decayed char array at cursor 0, `&arr[i]` at
  /// cursor i, a walking pointer at its current cursor), and either shape
  /// under the implicit pointer bitcasts that `void *` parameters
  /// (memset/memcpy/memcmp) introduce, which are stripped. The address of
  /// a scalar object (no cursor) is rejected with a located diagnostic.
  FailureOr<PtrExprValue> emitCharRegionArg(const clang::Expr *expr);

  /// Borrows the char region of a decomposed pointer as a byte slice from
  /// its cursor: `emitrust.slice_of` of the region's place — the literal
  /// backing, or the base object's own place — typed
  /// `!emitrust.ref<!emitrust.slice<i8>>` (or `mut_ref` when `isMut`).
  /// Rejects a mutable borrow of a read-only literal region and any base
  /// whose place is not a char array (both located diagnostics); the
  /// array's compile-time-known size is what makes every helper access
  /// bounds-checked safe Rust.
  FailureOr<Value> emitCharRegionSlice(Location loc,
                                       const PtrExprValue &pointer,
                                       bool isMut);

  /// Records that the hosted `<string.h>` helper `name` must be emitted at
  /// the end of the module (see `stringHelperSource`).
  void requestStringHelper(llvm::StringRef name);

  /// Lowers a statement-position `strcpy`/`strncpy`/`strcat` call (C name
  /// in `name`; `hasCount` for strncpy) to the matching one-per-module
  /// safe helper over `(&mut [i8], &[i8][, i64])`: destination and source
  /// resolve through `emitCharRegionArg`/`emitCharRegionSlice`, and a
  /// source region sharing the destination's base object would alias a
  /// mutable borrow and is rejected. The helper copies bytes exactly as C
  /// does (strcpy/strcat through the source NUL, strncpy NUL-padded to n).
  LogicalResult emitStringCopyCall(const clang::CallExpr *call,
                                   llvm::StringRef name, bool hasCount);

  /// Lowers a statement-position `memset(s, c, n)` call to the
  /// `__emitrust_memset` helper: the destination region as a mutable byte
  /// slice from its cursor, the fill byte as i32, the count as i64.
  LogicalResult emitMemsetCall(const clang::CallExpr *call);

  /// Lowers a statement-position `memcpy(dst, src, n)` call. Distinct base
  /// objects (or a literal source) borrow two slices for the
  /// `__emitrust_memcpy` helper; both arguments rooted in the same base
  /// object would alias a mutable borrow, so that shape takes one mutable
  /// borrow of the whole array plus both cursors through the
  /// `__emitrust_memcpy_within` helper (`copy_within`, whose memmove
  /// semantics refine C's undefined overlapping memcpy).
  LogicalResult emitMemcpyCall(const clang::CallExpr *call);

  /// Lowers a value-position `strcmp`/`strncmp`/`memcmp` call (C name in
  /// `name`; `hasCount` for the n-limited forms) to the matching helper
  /// over two shared byte slices, returning C's int result (the helpers
  /// compare as unsigned char and return a sign-correct difference).
  FailureOr<Value> emitStringCompareCall(const clang::CallExpr *call,
                                         llvm::StringRef name, bool hasCount);

  /// Returns the argument as a definition-less `strchr`/`strrchr` call
  /// (setting `reverse` for strrchr), or null for every other expression.
  const clang::CallExpr *asHostedStrchrCall(const clang::Expr *expr,
                                            bool &reverse) const;

  /// Lowers a `strchr`/`strrchr` call to its found byte index: the
  /// searched region (returned through `region`) is borrowed as a shared
  /// slice from its cursor and passed to the `__emitrust_strchr` /
  /// `__emitrust_strrchr` helper, whose i64 result is the index relative
  /// to that cursor, or -1 when the byte does not occur (C's NULL result).
  FailureOr<Value> emitStrchrIndex(const clang::CallExpr *call, bool reverse,
                                   PtrExprValue &region);

  /// Emits `if`/`else` as a cf diamond: cond_br into then/else blocks that
  /// fall through to a continuation block.
  LogicalResult emitIfStmt(const clang::IfStmt *stmt);

  /// Emits `while` as condition/body/exit blocks with a back edge.
  LogicalResult emitWhileStmt(const clang::WhileStmt *stmt);

  /// Emits `for` as init in the current block plus condition, body,
  /// increment, and exit blocks; `continue` targets the increment block.
  LogicalResult emitForStmt(const clang::ForStmt *stmt);

  /// Emits `do`/`while` as body/condition/exit blocks: the body is entered
  /// unconditionally, the condition block branches back to the body or to
  /// the exit; `break` targets the exit and `continue` the condition block.
  LogicalResult emitDoStmt(const clang::DoStmt *stmt);

  /// Emits `switch`. A plain compound body (every case/default label at the
  /// top level, nothing before the first label) takes the structured path:
  /// a `cf.switch` over one block per top-level label position plus an exit
  /// block, where consecutive labels share a block and a label section that
  /// does not end in a terminator falls through to the next section with a
  /// `cf.br`. Any other body shape (non-compound bodies, statements before
  /// the first label, labels nested inside inner statements — Duff's
  /// device) is delegated to `emitDispatchSwitch`. In both paths `break`
  /// targets the exit block while `continue` still targets the enclosing
  /// loop, and GNU case ranges are rejected.
  LogicalResult emitSwitchStmt(const clang::SwitchStmt *stmt);

  /// Fallback `switch` lowering for bodies the structured path cannot
  /// shape: every case/default label of this switch (found via clang's
  /// `SwitchStmt::getSwitchCaseList`, which covers labels buried inside
  /// inner statements but not those of nested switches) becomes an
  /// ordinary block, registered in `switchCaseBlocks`; the dispatch is one
  /// `cf.switch` from the current block to those targets; and the body is
  /// then emitted in source order starting in a fresh dead block, with the
  /// `SwitchCase` case of `emitStmt` redirecting emission into each
  /// label's block as the walk reaches it, so fall-through (including into
  /// and around loop bodies, as in Duff's device) is plain block
  /// fall-into. The possibly irreducible result is absorbed downstream by
  /// lift-cf-to-scf, exactly like goto. Variable places emitted below the
  /// dispatch are hoisted to the entry block (the dispatch may jump over
  /// their declarations, exactly like goto over a declaration).
  LogicalResult emitDispatchSwitch(const clang::SwitchStmt *stmt, Value flag,
                                   IntegerType flagType, Location loc);

  /// Emits `return`, then continues in a fresh (dead) block so trailing
  /// statements still have an insertion point.
  LogicalResult emitReturnStmt(const clang::ReturnStmt *stmt);

  /// Emits an expression evaluated for its side effects only: assignments,
  /// compound assignments, ++/--, calls (including printf), casts to void
  /// (the operand's side effects run, the value is discarded, and a
  /// side-effect-free operand emits nothing), and void-typed conditional
  /// operators (see `emitVoidConditionalStmt`).
  LogicalResult emitExprStmt(const clang::Expr *expr);

  /// Emits a void-typed conditional operator in statement position as an
  /// if/else: the condition selects which arm's side effects run, and no
  /// value is materialized (there is none to materialize — `void` is not a
  /// value type in the dialect).
  LogicalResult
  emitVoidConditionalStmt(const clang::ConditionalOperator *op);

  /// Emits a simple assignment `lhs = rhs` to a memref cell or EmitRust
  /// place.
  LogicalResult emitAssign(const clang::BinaryOperator *op);

  /// Emits a simple assignment and returns the assigned-to place, so that
  /// value-position assignments (`y = (x = 1)`) can re-load the stored
  /// value (C's value of an assignment is the post-assignment value).
  FailureOr<Value> emitAssignToPlace(const clang::BinaryOperator *op);

  /// Emits a compound assignment (`+=` etc.) as load, widen to the
  /// computation type, arithmetic, narrow back, store (the widen/narrow
  /// pair is a no-op when no operand promotion applies; see
  /// `buildCompoundAssignValue`).
  LogicalResult emitCompoundAssign(const clang::CompoundAssignOperator *op);

  /// Emits a compound assignment and returns the assigned-to place, for
  /// value-position uses (see `emitAssignToPlace`).
  FailureOr<Value>
  emitCompoundAssignToPlace(const clang::CompoundAssignOperator *op);

  /// Computes the value a compound assignment stores: widens the loaded
  /// LHS `current` to Sema's computation type
  /// (`CompoundAssignOperator::getComputationLHSType`), applies the
  /// operator against the RHS (which Sema already converted to the
  /// computation type, except shift amounts, which are normalized to the
  /// shifted operand's width), and narrows the result back to the LHS's
  /// storage type. Both conversions go through `convertScalarValue`, so
  /// `char c; c += wider;` widens and narrows by C's conversion rules
  /// (zero-extension from unsigned, sign-extension from signed, low-bits
  /// truncation on narrowing).
  FailureOr<Value>
  buildCompoundAssignValue(Location loc,
                           const clang::CompoundAssignOperator *op,
                           Value current);

  /// Converts a scalar `value` to `target` following C's conversion
  /// rules: integer-to-integer via `castToIntType`, `arith`
  /// extension/truncation between float widths, and integer/float
  /// conversions that route through `emitrust.cast` whenever the integer
  /// side is unsigned (mirroring the `CK_IntegralToFloating` and
  /// `CK_FloatingToIntegral` lowerings). `_Bool` (`i1`) endpoints are
  /// rejected: C converts to `_Bool` by comparison against zero, which
  /// truncation would lower incorrectly.
  FailureOr<Value> convertScalarValue(Location loc, Value value, Type target);

  /// Emits statement-level `++x`/`x--` as load, add/sub 1, store; only
  /// integer operands are supported.
  LogicalResult emitIncDec(const clang::UnaryOperator *op);

  /// Emits `++`/`--` in value position: performs the store like
  /// `emitIncDec` and yields the expression's C value — the pre-value for
  /// the postfix forms, the post-value for the prefix forms.
  FailureOr<Value> emitIncDecValue(const clang::UnaryOperator *op);

  /// Emits a call statement, dispatching printf to `emitPrintf` (and
  /// definition-less puts/putchar to `emitPuts`/`emitPutchar`) and
  /// discarding the result of ordinary calls.
  LogicalResult emitCallStmt(const clang::CallExpr *call);

  /// Lowers a printf call with a literal format string to
  /// `emitrust.call_opaque "print!"` with a translated Rust format string
  /// in the `args` attribute. The supported directive grammar is
  /// `%[flags][width][length]conv` with flags `-`/`0`, a decimal width,
  /// length `l`, and conversions d/i (i32, or i64 with `l`), u/x/X/o
  /// (unsigned; the argument is `as`-cast to u32/u64 so negative signed
  /// arguments print their two's-complement bit pattern exactly like C),
  /// c (byte, via `__emitrust_fmt_c`), s (string literal or char-array
  /// lvalue, see `emitPrintfStringArg`), f (f64, via `__emitrust_fmt_f64`
  /// so non-finite values print with C's spellings; no flags/width), and
  /// %%. Precision, other lengths (`ll`, `h`, ...), and every other
  /// conversion keep located rejections. Integer arguments of a different
  /// width or signedness than the conversion expects are `as`-cast, which
  /// truncates to the low bits exactly like the x86-64 varargs read that C
  /// performs.
  LogicalResult emitPrintf(const clang::CallExpr *call);

  /// Translates the C printf-family format string `literal` into a Rust
  /// format string, consuming the directive arguments of `call` starting
  /// at `firstArgIndex` and appending their lowered SSA values to
  /// `operands` (one per Rust `{}` placeholder, in order). This is the
  /// shared directive grammar of `emitPrintf` and `emitSprintf` (see
  /// `emitPrintf` for the supported set); precision, the unsupported
  /// length modifiers, and unknown conversions keep their located
  /// rejections here so every caller enforces the same subset. Fails if
  /// `call` supplies too few or too many arguments for the directives.
  FailureOr<std::string>
  translatePrintfFormat(Location loc, const clang::CallExpr *call,
                        const clang::StringLiteral *literal,
                        unsigned firstArgIndex,
                        SmallVectorImpl<Value> &operands);

  /// Lowers a definition-less `sprintf(dest, fmt, ...)` call (CTS-P9,
  /// 00186). The format must be an ordinary string literal and translates
  /// through `translatePrintfFormat` into an
  /// `emitrust.call_opaque "format!"` producing a String; the destination
  /// is a char region borrowed mutably from its cursor (exactly like the
  /// <string.h> copy helpers, so a string-literal-backed destination is
  /// rejected), and both feed the one-per-module `__emitrust_sprintf`
  /// helper, whose i32 result — the written length, excluding the NUL —
  /// is C's sprintf return value. A destination too small for the bytes
  /// plus the NUL terminator panics in the helper (C leaves the overflow
  /// undefined; the deterministic panic is a legal refinement).
  FailureOr<Value> emitSprintf(const clang::CallExpr *call);

  /// Lowers a `%s` printf argument. Four shapes are supported: a string
  /// literal (after array-to-pointer decay), lowered to an
  /// `emitrust.literal` holding a `&'static str` (printable-ASCII bytes
  /// plus \n/\t/\r only; embedded NUL and non-ASCII bytes are rejected);
  /// a char-array lvalue, lowered to an `emitrust.slice_of` of the
  /// whole array passed through the `__emitrust_cstr` helper, which stops
  /// at the first NUL like C; a `char *` pointer into a string-literal
  /// region, lowered to an `emitrust.slice_of` of the region's read-only
  /// backing from the pointer's cursor through the same helper; and a
  /// slice-classified `char *` parameter (FR-28, CTS-L2), lowered to an
  /// `emitrust.slice_of` of the parameter's deref'd slice base place from
  /// its cursor through the same helper.
  FailureOr<Value> emitPrintfStringArg(const clang::Expr *expr);

  /// Wraps an integer value for a `%c` directive: casts it to i32 and
  /// routes it through the `__emitrust_fmt_c` helper (C converts the
  /// argument to unsigned char and prints that byte; the helper matches C
  /// byte-for-byte for ASCII values, see design.md C99-48).
  Value wrapCharFormat(Location loc, Value value);

  /// Lowers a statement-position `puts(s)` call to
  /// `emitrust.call_opaque "println!"` using the `%s` machinery
  /// (`emitPrintfStringArg`). Only called when `puts` has no user
  /// definition.
  LogicalResult emitPuts(const clang::CallExpr *call);

  /// Lowers a statement-position `putchar(c)` call to
  /// `emitrust.call_opaque "print!"` of the argument routed through
  /// `__emitrust_fmt_c`. Only called when `putchar` has no user
  /// definition.
  LogicalResult emitPutchar(const clang::CallExpr *call);

  /// Maps a hosted `<math.h>` function name to the safe Rust callable it
  /// lowers to (design.md C99-48; currently exactly `sin` -> `f64::sin`).
  /// Returns std::nullopt for every other name, which keeps the
  /// system-header rejection in `emitCall`.
  static std::optional<llvm::StringRef>
  hostedMathCallee(llvm::StringRef name);

  /// Lowers a call to a definition-less hosted `<math.h>` function with
  /// C's standard `double f(double)` prototype to
  /// `emitrust.call_opaque "<rustCallee>"` of the f64 argument (e.g.
  /// `sin(x)` -> `f64::sin(vX)`). Only called when `hostedMathCallee`
  /// recognized the name and the prototype matched; clang has already
  /// inserted the usual argument conversion to double, so the operand is
  /// f64 by construction (checked defensively).
  FailureOr<Value> emitHostedMathCall(const clang::CallExpr *call,
                                      llvm::StringRef rustCallee);

  //===--------------------------------------------------------------------===//
  // Hosted <stdio.h> FILE* streams (design.md C99-48, CTS-T1.3, 00187)
  //===--------------------------------------------------------------------===//
  //
  // A `FILE *` local is an OWNED handle over std::fs, held in an
  // `emitrust.variable` of the opaque `__EmitrustFile` type (an enum over
  // Null / Read(File) / Write(File), emitted once per module). fopen with
  // a literal path and mode "r"/"w" produces the handle, every stream
  // operation borrows it `&mut` through a `__emitrust_f*` helper call, and
  // fclose resets it to Null so the same variable can be reopened (the
  // serial-reuse shape of 00187). The supported surface is sequential
  // byte-wise I/O only: fgetc/getc (i32 byte or -1), byte-wise
  // fread/fwrite (element size 1), fgets, and the NULL truth test of a
  // handle. File positioning, other fopen modes, fprintf to a real
  // stream, wide fread/fwrite elements, and any FILE* escaping its
  // function (parameter, return, struct member, global, array) keep
  // located rejections. fopen failure takes C's NULL path; read/write
  // errors beyond EOF panic in the helpers (C UB, refined
  // deterministically).

  /// Returns the opaque `__EmitrustFile` handle type.
  emitrust::OpaqueType fileHandleType();

  /// Requests the one-per-module emission of a `kFileHelpers` entry (and
  /// the `__EmitrustFile` enum definition every helper needs; fread and
  /// fgets additionally pull in the fgetc primitive they call).
  void requestFileHelper(llvm::StringRef name);

  /// Emits a `FILE *` local as an owned-handle `emitrust.variable` place
  /// (registered in `fileLocals`), lowering an `= fopen(...)` initializer
  /// through `emitFileOpenInto`.
  LogicalResult emitFileLocal(const clang::VarDecl *var, Location loc);

  /// Lowers `place = fopen(path, mode)`: the path must be an ordinary
  /// ASCII string literal (literal-only, v1) and the mode literal must be
  /// exactly "r" or "w"; the matching `__emitrust_fopen_r`/`_w` helper
  /// call produces the handle assigned into `place` (Null on failure —
  /// C's NULL path). Any other right-hand side is rejected.
  LogicalResult emitFileOpenInto(Value place, const clang::Expr *init);

  /// Borrows the FILE* handle argument `expr` (a function-local handle
  /// variable) as `&mut __EmitrustFile` (or `&` when `isMut` is false)
  /// for a helper call; anything but a tracked handle local is rejected.
  FailureOr<Value> emitFileHandleArg(const clang::Expr *expr, bool isMut);

  /// Lowers `fgetc(f)` / `getc(f)` to `__emitrust_fgetc(&mut f)`: the
  /// byte as i32, or -1 — C's EOF, which the surrounding `!= EOF`
  /// comparison meets as an ordinary `arith.constant -1 : i32`.
  FailureOr<Value> emitFileGetc(const clang::CallExpr *call);

  /// Lowers byte-wise `fread(ptr, 1, n, f)` / `fwrite(ptr, 1, n, f)` to
  /// the matching helper over a char-region slice of `ptr` and an i64
  /// count; the helper's i64 result (the byte count) is converted to the
  /// call's C size_t result type like `emitStrlenCall`. An element size
  /// other than the constant 1 is a located rejection (byte-wise only).
  FailureOr<Value> emitFileReadWrite(const clang::CallExpr *call,
                                     bool isWrite);

  /// Lowers `fgets(buf, size, f)` to `__emitrust_fgets`, whose i64 result
  /// is -1 for C's NULL return (end of file with nothing read) and the
  /// stored byte count otherwise. `emitComparison`/`emitCondition`
  /// consume the result as `!= -1` (the strchr convention), so the
  /// pinned `while (fgets(...) != NULL)` shape and the bare truth test
  /// both work; any other use of the char* result is rejected.
  FailureOr<Value> emitFileGetsIndex(const clang::CallExpr *call);

  /// Returns `expr` as a definition-less hosted `fgets` call, or null.
  const clang::CallExpr *asHostedFgetsCall(const clang::Expr *expr) const;

  /// Lowers a statement-position `fclose(f)` to `__emitrust_fclose(&mut
  /// f)`, which drops the handle (closing the file) and leaves the
  /// variable Null for serial reopening.
  LogicalResult emitFileClose(const clang::CallExpr *call);

  /// Emits the truth value of a FILE*-typed expression: a handle
  /// variable's NULL test routes through `__emitrust_file_ok(&f)` (the
  /// `if (!f)` shape), and an fgets call tests its index against -1.
  FailureOr<Value> emitFileTruth(const clang::Expr *expr);

  //===--------------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------------===//

  /// Emits an expression as an SSA value (an "rvalue"). Comparison and
  /// logical results are widened from i1 to their C `int` type here;
  /// `emitCondition` is the narrow-i1 entry point used by control flow.
  FailureOr<Value> emitRValue(const clang::Expr *expr);

  /// Emits an implicit or explicit cast; supports lvalue-to-rvalue loads
  /// and the numeric conversion kinds of the subset.
  FailureOr<Value> emitCast(const clang::CastExpr *cast);

  /// Emits a binary operator as an rvalue, including value-position
  /// assignments, compound assignments, and the comma operator.
  FailureOr<Value> emitBinaryRValue(const clang::BinaryOperator *op);

  /// Emits the conditional operator `cond ? a : b` with short-circuit
  /// evaluation: only the selected arm's side effects run. The result
  /// flows through a rank-0 memref cell (promoted later by `--mem2reg`)
  /// for memref-legal scalar types and through an `emitrust.variable`
  /// place for unsigned integers; both arms must map to the same scalar
  /// type or the operator is rejected. A compile-time-constant,
  /// side-effect-free condition elides the dead arm BEFORE lowering (so a
  /// dead arm may contain otherwise-unimportable constructs); a
  /// goto-targeted label or a case/default label in the dead arm keeps
  /// the full lowering instead — the constant branch leaves the arm
  /// dynamically dead while its labels stay registered.
  FailureOr<Value>
  emitConditionalOperator(const clang::ConditionalOperator *op);

  /// Emits a GNU statement expression `({ ... })` in value position: the
  /// body statements lower FLATTENED into the enclosing function (never as
  /// a region op, so labels inside register with the ordinary
  /// labelBlocks/goto dispatch), and the final expression statement's
  /// value transits a synthesized temp cell that the surrounding
  /// expression reads. A `goto` targeting a label outside the statement
  /// expression would abandon the value mid-evaluation and is a located
  /// rejection (design.md CTS-S).
  FailureOr<Value> emitStmtExpr(const clang::StmtExpr *expr);

  /// Emits an rvalue of an integer-carrier pointer expression (CTS-P3) as
  /// its plain i64 value: a null constant is the i64 zero, an
  /// integer-to-pointer cast converts its integer operand to i64, a read
  /// of a carrier local/parameter loads its cell, and a call to a
  /// carrier-returning function yields its i64 result directly. Any other
  /// shape is a located rejection.
  FailureOr<Value> emitCarrierValue(const clang::Expr *expr);

  /// Returns the i64 cell of the integer-carrier pointer local or
  /// parameter a (possibly lvalue-to-rvalue-wrapped) reference `expr`
  /// names, or a null Value when `expr` is not such a reference.
  Value lookupCarrierCell(const clang::Expr *expr);

  /// Constant-folds `sizeof`/`_Alignof` (on a type or an expression) into
  /// an integer constant of the mapped `size_t` type using clang's target
  /// layout. The operand of `sizeof` is unevaluated in C (side effects do
  /// not run), which the fold preserves; variable-length array operands,
  /// whose size is not a constant, are rejected.
  FailureOr<Value>
  emitSizeofAlignof(const clang::UnaryExprOrTypeTraitExpr *expr);

  /// Emits a unary operator (+, -, !, &, statement-level ++/-- excluded)
  /// as an rvalue.
  FailureOr<Value> emitUnaryRValue(const clang::UnaryOperator *op);

  /// Emits a comparison as an i1 value: `arith.cmpi` (signed) on signless
  /// integers, `emitrust.cmp` (type-directed in Rust, hence unsigned) on
  /// unsigned integers, `arith.cmpf` (ordered, `une` for !=) on floats.
  FailureOr<Value> emitComparison(const clang::BinaryOperator *op);

  /// Emits an expression as an i1 truth value following C semantics:
  /// comparisons directly, `&&`/`||` with short-circuit blocks, `!` by
  /// inversion, and any integer/float value compared against zero.
  FailureOr<Value> emitCondition(const clang::Expr *expr);

  /// Emits `&&`/`||` with short-circuit evaluation through a rank-0 i1
  /// memref cell and a conditional branch; `--mem2reg` later promotes the
  /// cell.
  FailureOr<Value> emitShortCircuit(const clang::BinaryOperator *op);

  /// Emits a call expression; returns a null `Value` for void results.
  /// printf reaching this path (i.e. with its result used) is rejected;
  /// a definition-less sprintf routes to `emitSprintf`. A call to a
  /// fixed-prototype variadic definition (va_list-free body, CTS-P9)
  /// passes only the named arguments: the trailing extras are dropped
  /// without being imported (no loads, no borrows), and an extra whose
  /// evaluation has side effects is rejected rather than silently lost.
  /// Every value argument is materialized before any borrow-producing
  /// argument (C's evaluation order is unspecified; this keeps loads out of
  /// the borrow/call window), and two borrow arguments resolving to the
  /// same region base are rejected as aliasing mutable borrows.
  /// Calls without a direct callee are routed to `emitIndirectCall`.
  FailureOr<Value> emitCall(const clang::CallExpr *call);

  /// Emits a call to a method-planned function (Phase 4). Every argument
  /// is a plain value — a pointer argument lowers to its i64 element cursor
  /// (a constant for `&arr[i]` and decayed arrays, the loaded cursor for
  /// walking pointers) — so the receiver borrow (`addr_of mut` of the owner
  /// place: the owner struct variable in the owning function, or the
  /// dereferenced receiver in a sibling method) is the only reference and
  /// is materialized last, immediately before the call. The `func.call` is
  /// tagged `emitrust.method_call` for the conversion-layer rewrite into an
  /// `emitrust.method_call` place expression.
  FailureOr<Value> emitMethodCallSite(const clang::CallExpr *call,
                                      func::FuncOp target,
                                      const clang::VarDecl *ownerBase,
                                      Location loc);

  /// Lowers one borrow-producing call argument against the reference-typed
  /// target parameter `paramType`. A slice parameter receives an
  /// `emitrust.slice_of` of the argument's region base at the argument's
  /// cursor (a decayed array passes cursor 0; reslicing through another
  /// slice parameter composes); a scalar-reference parameter receives an
  /// `emitrust.addr_of` of the designated element, or of the named place
  /// for plain address-of arguments that involve no decomposed pointer.
  /// `root` receives the argument's region base declaration when one is
  /// statically known (feeding the aliasing rejection in `emitCall`).
  FailureOr<Value> emitBorrowArgument(Location loc,
                                      const clang::Expr *argument,
                                      Type paramType,
                                      const clang::VarDecl *&root);

  /// Returns whether any declaration reference below `stmt` names a
  /// decomposed pointer (a pointer local or slice parameter registered in
  /// `pointerLocals`), in which case a borrow-producing call argument must
  /// be lowered through the pointer decomposition.
  bool involvesDecomposedPointer(const clang::Stmt *stmt) const;

  /// Emits a call through a function pointer (`fp(...)`, `(*fp)(...)`,
  /// `v.op(...)`) as an `emitrust.call_indirect`: the callee expression is
  /// evaluated to its `!emitrust.fn_ptr` value (a deref of a function
  /// pointer cancels against the implicit decay) and the argument and
  /// result types are checked against the pointer's signature exactly like
  /// a direct call. Calls with arguments through a prototype-less K&R
  /// pointer are rejected: there is no signature to check them against.
  FailureOr<Value> emitIndirectCall(const clang::CallExpr *call);

  /// Resolves a function reference used as a function pointer value: the
  /// expression must be a direct reference to an imported, non-variadic
  /// function whose signature equals `fnPtrType` (functions with
  /// data-pointer parameters can never match). Returns the function's MLIR
  /// symbol name (`c_main` and per-TU static mangling included); every
  /// violation is a located rejection.
  FailureOr<std::string>
  resolveFunctionPointerTarget(const clang::Expr *expr,
                               emitrust::FnPtrType fnPtrType, Location loc);

  /// The declaration-level half of `resolveFunctionPointerTarget`: checks
  /// that `callee` is an imported, non-variadic function whose MLIR
  /// signature equals `fnPtrType` and returns its MLIR symbol name. Used
  /// directly by the constant-initializer path (`convertAPValueInit`),
  /// where clang's evaluator yields the target declaration rather than an
  /// expression.
  FailureOr<std::string>
  resolveFunctionPointerDecl(const clang::FunctionDecl *callee,
                             emitrust::FnPtrType fnPtrType, Location loc);

  /// Emits `f` (function-to-pointer decay) or `&f` as an
  /// `emitrust.constant` with an opaque `Some(<symbol>)` payload of the
  /// `!emitrust.fn_ptr` type mapped from the C pointer type `pointerType`.
  FailureOr<Value> emitFunctionPointerConstant(const clang::Expr *fnExpr,
                                               clang::QualType pointerType,
                                               Location loc);

  /// Creates an `emitrust.constant` with an opaque `None` payload of the
  /// given `!emitrust.fn_ptr` type (the C null function pointer).
  Value createFnPtrNone(Location loc, Type fnPtrType);

  /// Emits a reference to an enumerator: for a complete named enum, an
  /// `emitrust.constant` with an opaque `Name::Variant` payload typed
  /// `!emitrust.enum<"Name">` (importing the enum definition on the way);
  /// for an anonymous enum, a plain `i32` constant.
  FailureOr<Value> emitEnumConstant(const clang::EnumConstantDecl *enumerator,
                                    Location loc);

  /// Emits an operand classified by `classifyEnumOperand` as an SSA value
  /// of the `!emitrust.enum` type.
  FailureOr<Value> emitEnumOperand(const EnumOperand &operand, Location loc);

  /// Builds an `emitrust.cast` from an `!emitrust.enum` value to i32 (the
  /// representation type of every imported enum).
  Value castEnumToI32(Location loc, Value value);

  /// Emits a pointer-typed expression in the decomposed representation:
  /// reads of pointer locals load their cursor cell, `&x` yields the
  /// degenerate (cursor-less) form, `&arr[i]` and array decay yield the
  /// base with an i64 cursor, a decayed string literal yields its
  /// read-only backing array at cursor 0, `p +- n` is cursor arithmetic,
  /// and the `++`/`--` value forms update the cursor cell and yield the
  /// pre- or post-value per C semantics. A cursor into a multi-dimensional
  /// array counts innermost (scalar or struct) elements in row-major
  /// order, so `&arr[i][j]` on `T arr[M][N]` yields the flat cursor
  /// `i*N + j`. A pointer of a nullable region carries its
  /// Option-of-cursor discriminant (the loaded i1 flag) in the result's
  /// `nonNull` field. Null pointer constants (whose modeled consumers —
  /// pointer assignment, null comparison, truth test — intercept them
  /// before this point), globals, arithmetic on pointers to arrays
  /// (rows), and every other pointer source are located rejections.
  FailureOr<PtrExprValue> emitPointerRValue(const clang::Expr *expr);

  /// Returns whether `expr` is a C null pointer constant (`NULL`, `0`,
  /// `(void*)0` in a pointer context).
  bool isNullPointerConstantExpr(const clang::Expr *expr) const;

  /// Emits the truth value of a data-pointer expression (`if (p)`, `!p`):
  /// the Option-of-cursor discriminant of a pointer in a nullable region,
  /// or constant true for a statically non-null pointer (every address a
  /// decomposed region holds designates a live object).
  FailureOr<Value> emitPointerTruth(const clang::Expr *expr);

  /// Emits the (base, flat cursor) decomposition of one array subscript
  /// level `base[idx]` whose base is itself a decomposed pointer
  /// expression (a pointer read, an array decay, or a decayed inner
  /// subscript). The index is scaled by the flat element count of the
  /// subscript's result type, so subscripting a row of a
  /// multi-dimensional array advances the cursor by whole rows.
  FailureOr<PtrExprValue>
  emitSubscriptPointer(const clang::ArraySubscriptExpr *subscript);

  /// Materializes the place a decomposed pointer designates: the base
  /// object's own place for a degenerate pointer, or a chain of
  /// `emitrust.subscript(base, cursor)` refinements for a pointer into an
  /// array. `pointeeType` is the mapped value type the resulting place
  /// must wrap; a flat cursor into a multi-dimensional array peels one
  /// array level per subscript, dividing the cursor by the level's flat
  /// element count and continuing with the remainder (row-major order).
  /// A possibly-null pointer (`pointer.nonNull` set) first emits a
  /// deterministic panic guard, `assert!(flag, "null pointer
  /// dereference")`: C dereferencing null is undefined behavior, so the
  /// panic is a legal refinement (the fn_ptr `expect` precedent). A
  /// pointer with no base at all (only ever null) is a located rejection.
  /// A global region base (a global object, or a pointer global's
  /// synthesized backing) stages the global's whole value in a local
  /// copy exactly like a direct global element access; a write context
  /// passes `writeback` to capture the pending store-back, which the
  /// caller must commit through `commitGlobalWriteback` (refresh, mutate,
  /// flush) after evaluating the rest of the statement.
  FailureOr<Value> emitPointerPlace(Location loc,
                                    const PtrExprValue &pointer,
                                    Type pointeeType,
                                    GlobalWriteback *writeback = nullptr);

  /// Emits `p - q` on two decomposed pointers into the same object as the
  /// plain i64 cursor difference (C's ptrdiff_t is `long`, i.e. i64, on
  /// the supported targets); pointers into different objects and
  /// possibly-null pointers are rejected.
  FailureOr<Value> emitPointerDifference(const clang::BinaryOperator *op);

  /// Returns whether the pointer-typed expression `expr` is handled by the
  /// Phase-1a decomposition (pointer locals, address-of, decay, pointer
  /// arithmetic) rather than by the untouched pointer-parameter reference
  /// path (a direct read of a `mut_ref`/`ref`-typed parameter).
  bool isDecomposedPointerExpr(const clang::Expr *expr) const;

  /// Returns whether `expr` (through decomposition-transparent cast peels)
  /// reads a pointer local whose region is statically null (CTS-P9): a
  /// base-less nullable region — one that only ever unites null constants
  /// and other null-only pointers. Such a pointer carries zero runtime
  /// state; its truth tests and null comparisons fold to constants and a
  /// pointer-to-int cast of it folds to 0.
  bool isStaticallyNullPointerExpr(const clang::Expr *expr);

  /// One classification of `expr` as a dereference view over a decomposed
  /// pointer, computed with a single reinterpreting-cast peel
  /// (`stripObjectPointerCasts`) shared by every consumer. `deref` is null
  /// when `expr` is not a dereference of a decomposed pointer at all.
  /// `reinterpreted` reports a pointee-changing peel (`*(T *)p` on a
  /// `void *` cursor, CTS-P9, or a direct pun cast, CTS-P11): the deref
  /// emission resolves such a place at the region's base element type,
  /// and when the viewed type is a same-width integer view over that
  /// element the load and store sites wrap the accessed value in an
  /// `emitrust.cast` bitcast (see `emitCast`'s `CK_LValueToRValue` case
  /// and `emitAssignToPlace`). `wideByte` additionally reports the
  /// byte-pun shape — a WIDER integer view (sizeof(T) in {2, 4, 8}) over
  /// a byte region — which widens to a `T::from_ne_bytes`/`to_ne_bytes`
  /// access instead of the same-width reinterpret path (wider views over
  /// non-byte bases keep the reinterpret rejection family).
  struct ByteViewDeref {
    /// The dereference `*p`; null when `expr` matches no decomposed
    /// pointer deref (every flag is then false).
    const clang::UnaryOperator *deref = nullptr;
    /// The pointer expression with the reinterpreting casts peeled once.
    const clang::Expr *strippedPointer = nullptr;
    /// The peel changed the pointee: a reinterpreted view (CTS-P9/P11).
    bool reinterpreted = false;
    /// The wider-integer-view-over-a-byte-region pun shape (CTS-P11).
    bool wideByte = false;
  };

  /// Classifies `expr` (see `ByteViewDeref`): the one entry point for
  /// reinterpreted-view and wide-byte-view dispatch, peeling the
  /// reinterpreting casts exactly once per query.
  ByteViewDeref classifyByteViewDeref(const clang::Expr *expr);

  /// Returns the C element type at the bottom of `pointer`'s region: the
  /// member's declared type for a `&struct.member` base, the pointee for a
  /// slice-parameter base, the array level matching `viewed` (falling back
  /// to the innermost element) for an array base, `char` for a
  /// string-literal region, and nothing for a base-less (statically null)
  /// region. Used to type-check a reinterpret-back site `*(T *)p` against
  /// the region (CTS-P9).
  std::optional<clang::QualType>
  regionElementType(const PtrExprValue &pointer, clang::QualType viewed);

  //===--------------------------------------------------------------------===//
  // Byte puns over i8 regions (CTS-P11)
  //===--------------------------------------------------------------------===//

  /// Statically resolves the region element C type of a pointer
  /// expression from the AST alone (decayed arrays, tracked pointer
  /// locals, global data pointers, and +/- arithmetic peel through), or
  /// nothing when no single element type is known. Side-effect-free
  /// companion of `regionElementType` for use before any emission.
  std::optional<clang::QualType>
  pointerElementTypeFromAST(const clang::Expr *expr) const;

  /// The resolved target of one wide byte access: the byte-array place
  /// (a local array, a string-literal backing, or a staged global copy),
  /// the i64 byte cursor, and the access width.
  struct WideByteAccess {
    /// The `!emitrust.lvalue<!emitrust.array<Nxi8>>` byte-run place.
    Value basePlace;
    /// The i64 cursor of the access's first byte.
    Value cursor;
    /// The mapped integer type of the viewed access (e.g. ui32).
    IntegerType valueType;
    /// sizeof(T) of the viewed type, in bytes.
    unsigned byteWidth;
  };

  /// Resolves the wide byte access a `wideByte` classification `view`
  /// designates: decomposes the (already peeled) pointer, resolves its
  /// byte-array base place (staging a global base's whole value like
  /// every other global element access; a write context passes
  /// `writeback` for the store-back), and rejects — with the located
  /// `runs past the end` diagnostic — a compile-time-constant offset
  /// whose widened window overruns the array.
  FailureOr<WideByteAccess> resolveWideByteAccess(const ByteViewDeref &view,
                                                  Location loc,
                                                  GlobalWriteback *writeback);

  /// Emits the widened load: the `byteWidth` bytes at the cursor are
  /// gathered (as u8) into a byte-array temporary and combined with
  /// `T::from_ne_bytes`.
  FailureOr<Value> emitWideByteLoad(const WideByteAccess &access,
                                    Location loc);

  /// Emits the widened store: `value` is split with `T::to_ne_bytes` and
  /// the bytes are stored back (as i8) at the cursor.
  LogicalResult emitWideByteStore(const WideByteAccess &access, Value value,
                                  Location loc);

  /// Emits an expression as an assignable place: either a rank-0 memref
  /// value (scalar locals) or an `!emitrust.lvalue` value (aggregates,
  /// dereferences, fields, elements). A reference to an imported global
  /// stages the global's whole value in a local copy; when `writeback` is
  /// non-null (write context) it captures the pending store-back of that
  /// copy, which the caller must commit through `commitGlobalWriteback`
  /// (refresh, mutate, flush) after evaluating the rest of the statement.
  FailureOr<Value> emitLValue(const clang::Expr *expr,
                              GlobalWriteback *writeback = nullptr);

  /// `emitLValue`'s declaration-reference branch: an imported global
  /// stages its whole value (recording the store-back in `writeback`), a
  /// devirtualized function-pointer alias materializes its `Some(target)`
  /// constant, a registered declaration yields its place, and pointer
  /// variables/parameters used as places keep their located rejections.
  FailureOr<Value> emitDeclRefLValue(const clang::DeclRefExpr *ref,
                                     Location loc, GlobalWriteback *writeback);

  /// `emitLValue`'s member-access branch: resolves the base place (an
  /// erased global-return call, a decomposed `p->f`, a reference-typed
  /// `->`, or a recursive lvalue) and selects the flattened field on it
  /// (anonymous members designate the parent place itself). Bit-field
  /// members have no place of their own (their storage is a window of a
  /// synthesized backing field) and are rejected here; supported
  /// bit-field traffic routes through `emitBitFieldRead` and
  /// `emitBitFieldAssign` before any lvalue is formed.
  FailureOr<Value> emitMemberLValue(const clang::MemberExpr *member,
                                    Location loc, GlobalWriteback *writeback);

  /// Resolves the place of `member`'s base aggregate — the shared front
  /// half of `emitMemberLValue`, also used by the bit-field accessors:
  /// an erased global-return call base, a decomposed `p->f`, a
  /// reference-typed `->` (dereferenced once), or a recursive lvalue.
  /// The result is an `!emitrust.lvalue` of a struct type.
  FailureOr<Value> emitMemberBasePlace(const clang::MemberExpr *member,
                                       Location loc,
                                       GlobalWriteback *writeback);

  /// Emits the C99-45 bit-field READ accessor for `member` (whose field
  /// must have an entry in `bitFieldAccessInfo`): load the backing
  /// field, `emitrust.shr` by the field's bit offset (always emitted,
  /// offset 0 included), `emitrust.and` with the width mask, then
  /// convert the masked backing-typed value to the member's mapped type
  /// via `convertBitFieldFieldValue`.
  FailureOr<Value> emitBitFieldRead(const clang::MemberExpr *member,
                                    Location loc);

  /// Emits the C99-45 bit-field WRITE accessor `member = rhs` as a
  /// read-modify-write on the backing field: load, `emitrust.and` with
  /// the complement mask (clearing the field's window), cast the RHS to
  /// the backing type (from an enum type when the RHS is enum-typed),
  /// `emitrust.and` with the width mask (the truncation is always its
  /// own step), `emitrust.shl` by the bit offset (always emitted),
  /// `emitrust.or` into the cleared word, `emitrust.assign` the backing
  /// field. A global base commits through the ordinary staged-copy
  /// writeback (`refreshStaged` mirrors `assignStalenessRisk`). With
  /// `wantValue` the truncated field value converts to the member's
  /// mapped type (C's value of an assignment is the post-store field
  /// value) and is staged in a fresh place, which is returned; otherwise
  /// the returned value is null.
  FailureOr<Value> emitBitFieldAssign(const clang::MemberExpr *member,
                                      const clang::Expr *rhs, Location loc,
                                      bool refreshStaged, bool wantValue);

  /// Converts `masked` — a bit-field's value bits, right-aligned in its
  /// unsigned backing type — to the member's mapped type: enum-typed
  /// fields cast (zero-extending from the unsigned source) to the enum
  /// type, `_Bool` fields compare against zero, unsigned fields
  /// zero-extend with `emitrust.cast`, and plain-int signed fields cast
  /// to the mapped signed type and sign-extend from their declared width
  /// via `arith.shli`/`arith.shrsi` by (type width - field width).
  FailureOr<Value> convertBitFieldFieldValue(Location loc, Value masked,
                                             const clang::FieldDecl *field);

  /// Builds the unsigned mask constant of a bit-field window, typed as
  /// the backing type: the width mask (`width` low bits set, used after
  /// the read shift and for the write truncation) or, with `complement`,
  /// its inverse shifted onto the window (`~(widthMask << offset)`, used
  /// to clear the window in the write's read-modify-write).
  Value createBitFieldMask(Location loc, IntegerType backingType,
                           unsigned width, unsigned offset, bool complement);

  /// `emitLValue`'s subscript branch: an array base subscripts its
  /// element place; a pointer base decomposes into (base, cursor) and
  /// resolves through `emitPointerPlace`.
  FailureOr<Value>
  emitSubscriptLValue(const clang::ArraySubscriptExpr *subscript, Location loc,
                      GlobalWriteback *writeback);

  /// `emitLValue`'s dereference branch: a decomposed pointer resolves to
  /// a place on its base object (type-checking a reinterpret-back view
  /// against the region's element type), a reference-typed pointer
  /// dereferences directly.
  FailureOr<Value> emitDerefLValue(const clang::UnaryOperator *unary,
                                   Location loc, GlobalWriteback *writeback);

  /// Reads the current value of a place produced by `emitLValue`.
  Value loadPlace(Location loc, Value place);

  /// Writes `value` to a place produced by `emitLValue`; fails on type
  /// mismatch.
  LogicalResult storeToPlace(Location loc, Value place, Value value);

  /// Widens an i1 truth value to the mapped MLIR type of the C expression
  /// type `type` (typically `int`); returns the value unchanged when the C
  /// type is `_Bool`.
  FailureOr<Value> extendBool(Location loc, Value flag, clang::QualType type);

  /// Builds the op for a C arithmetic, bitwise, or shift binary operator
  /// on two values of the same integer or float type: arith ops for floats
  /// and signless integers, `emitrust.add`/`sub`/`mul`/`div`/`rem` and
  /// `emitrust.and`/`or`/`xor`/`shl`/`shr` for unsigned integers (arith
  /// requires signless operands; the Rust infix operators are
  /// type-directed and hence unsigned).
  FailureOr<Value> buildBinaryArith(Location loc,
                                    clang::BinaryOperatorKind opcode,
                                    Value lhs, Value rhs);

  /// Converts an integer `value` to integer type `target`: a no-op on
  /// matching types, `arith` extension/truncation between signless types,
  /// and an `emitrust.cast` (Rust `as`, which matches C's integer
  /// conversion semantics) when either side is unsigned. Used to normalize
  /// a shift amount to the width of the shifted operand.
  Value castToIntType(Location loc, Value value, IntegerType target);


  //===--------------------------------------------------------------------===//
  // State
  //===--------------------------------------------------------------------===//

  /// Computes the MLIR symbol name of a function: `c_main` for C `main`, the
  /// per-TU-mangled `<tag><name>` for internal-linkage (`static`) functions,
  /// and the bare C name for external-linkage functions.
  std::string mlirFuncName(const clang::FunctionDecl *func) const;

  /// The clang AST currently being translated (borrowed, read-only). Rebound
  /// by each `importTranslationUnit` call so one importer can span TUs.
  clang::ASTContext *astContextPtr = nullptr;
  /// Accessor giving the reference-style spelling used throughout.
  clang::ASTContext &astContext() const { return *astContextPtr; }
  /// Prefix prepended to internal-linkage symbol names in the current TU
  /// (empty for single-TU imports).
  std::string currentTuTag;
  /// When true (project import), an `extern`-only global with no definition in
  /// this TU is deferred to `finalizeProject` instead of being an error.
  bool deferExternGlobals = false;
  /// Deferred `extern` global references awaiting a cross-TU definition,
  /// keyed by MLIR symbol name; the location is the first reference for the
  /// diagnostic if no TU defines it.
  llvm::StringMap<Location> pendingExternGlobals;
  /// Shape of every imported file-scope struct, keyed by symbol name, for
  /// cross-TU deduplication and mismatch detection. Block-scope records are
  /// never entered here: their identity is the defining decl (see
  /// `localRecordNames`), not the tag name.
  llvm::StringMap<std::string> importedRecordShapes;
  /// Emitted Rust type name of every block-scope struct definition, keyed by
  /// the defining decl. C tag identity is per declaration (C99 6.2.1: an
  /// inner-scope `struct T` shadowing an outer `T` is a new type even when
  /// the shapes match), so each block-scope definition gets its own
  /// struct_def under a `<function>_<tag>` name following the
  /// function-local-static mangling convention, `_<n>`-suffixed when that
  /// name is already taken.
  llvm::DenseMap<const clang::RecordDecl *, std::string> localRecordNames;
  /// Every struct_def symbol name emitted so far (file-scope tags and
  /// mangled block-scope names alike), consulted so block-scope mangling
  /// never reuses an existing type name.
  llvm::StringSet<> emittedStructNames;
  /// Synthesized Rust names for bare anonymous structs (no tag, no typedef
  /// name), keyed by the same field-shape serialization used for cross-TU
  /// dedup. Living in its own map (never keyed by a user-written name) is
  /// the anonymity marker: an anonymous struct whose shape matches a named
  /// struct's still gets its own Rust type, because C type identity is by
  /// declaration, not by shape. The name is a deterministic function of the
  /// shape, so the same anonymous shape in two translation units maps to
  /// one Rust type and two different shapes never collide.
  llvm::StringMap<std::string> anonRecordShapeNames;
  /// Synthesized name of every imported bare anonymous struct, keyed by its
  /// defining declaration; populated by `importRecord` and consulted by
  /// `emittedRecordName`.
  llvm::DenseMap<const clang::RecordDecl *, std::string> anonRecordNames;
  /// Next `Anon<n>` suffix to try when a new anonymous shape needs a name;
  /// names are assigned in first-encounter order per import.
  unsigned anonStructCounter = 0;
  /// Union arm -> the first arm's leaf field, whose spelling names the
  /// single flattened storage slot every arm aliases; populated by
  /// `collectRecordFields` (anonymous union members) and
  /// `collectUnionSlot` (named/untagged union types; the storage leaf
  /// itself has no entry) and consulted by `flattenedFieldName` and
  /// `flattenedFieldStorage`.
  llvm::DenseMap<const clang::FieldDecl *, const clang::FieldDecl *>
      unionSlotStorage;
  /// Byte-array union arms admitted at the TYPE level by
  /// `collectUnionSlot` (CTS-F, 00210): the arm's total width equals the
  /// integer slot's, so the union type imports on the one-slot model, but
  /// no access through the arm is representable on that slot;
  /// `emitMemberLValue` rejects each such access at its own site.
  llvm::SmallPtrSet<const clang::FieldDecl *, 4> unionByteArrayArms;
  /// The C99-45 accessor geometry of one bit-field member: the window
  /// `[offset, offset + width)` of the synthesized unsigned backing field
  /// `backingName` (of type `backingType`) in its flattened parent
  /// struct_def. Recorded by `collectRecordFields` when the member's run
  /// packs; consulted by `emitBitFieldRead`/`emitBitFieldAssign`.
  struct BitFieldAccess {
    /// The backing field's spelling (`__bits<n>`); owned by
    /// `memberNameArena`.
    llvm::StringRef backingName;
    /// The smallest unsigned integer type (ui8/ui16/ui32/ui64) holding
    /// the run's total bits.
    IntegerType backingType;
    /// The field's bit offset from bit 0 (LSB) of the backing field.
    unsigned offset = 0;
    /// The field's declared width in bits.
    unsigned width = 0;
  };
  /// Bit-field member -> its accessor geometry (see `BitFieldAccess`).
  llvm::DenseMap<const clang::FieldDecl *, BitFieldAccess> bitFieldAccessInfo;
  /// Stable backing storage for member spellings synthesized at import
  /// (`__bits<n>` backing names, keyword-mangled member names): the
  /// StringRefs handed to struct_def field lists point in here, and a
  /// deque never relocates its elements.
  std::deque<std::string> memberNameArena;
  /// The defining C record behind each emitted struct_def symbol, recorded
  /// by `importRecord` so record-aware consumers (global initializer
  /// conversion) can walk the C field structure of a flattened struct.
  /// Synthesized struct_defs (Phase-4 owner structs) have no entry and
  /// keep the positional field conversion.
  llvm::StringMap<const clang::RecordDecl *> structDefRecords;
  /// Module-symbol names the current TU's ordinary identifier namespace
  /// claims (functions, file-scope variables, mangled function-local
  /// statics), pre-scanned by `collectOrdinaryNames`; struct tags colliding
  /// with these are renamed (see `structSymbolName`).
  llvm::StringSet<> ordinaryTuNames;
  /// Symbol name assigned to each struct definition by `structSymbolName`,
  /// keyed on the defining declaration (per-TU decls are distinct; cross-TU
  /// unification still happens by final name through
  /// `importedRecordShapes`, so the rename decision must be reproducible
  /// from each TU's own ordinary names).
  llvm::DenseMap<const clang::RecordDecl *, std::string> assignedStructNames;
  /// Shape of every imported enum, keyed by symbol name, for cross-TU
  /// deduplication and mismatch detection.
  llvm::StringMap<std::string> importedEnumShapes;
  /// The module receiving struct definitions and functions.
  ModuleOp module;
  /// Builder positioned inside the function body under construction.
  OpBuilder builder;
  /// Per-function map from clang declarations to their MLIR place or, for
  /// pointer parameters, their reference SSA value.
  llvm::DenseMap<const clang::ValueDecl *, Value> symbols;
  /// Per-function admitted local `void *` fn-ptr holders (CTS-F, 00210),
  /// each mapped to the one known non-variadic function whose address it
  /// holds; populated by `collectVoidFnPtrHolders` before the pointer
  /// region analysis (which skips them via `fnHolderQuery`). An admitted
  /// holder imports as an ordinary local `!emitrust.fn_ptr` variable and
  /// its cast-calls peel to plain `emitrust.call_indirect`.
  llvm::DenseMap<const clang::VarDecl *, const clang::FunctionDecl *>
      voidFnPtrHolders;
  /// The clang body of the function under import; consulted by dead-VLA
  /// elision (CTS-F, 00207) to decide whether a local is referenced
  /// anywhere in the body.
  const clang::Stmt *currentFunctionBody = nullptr;
  /// Per-function set of locals whose address is taken.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> addressTaken;
  /// Per-function pointer region analysis (Phase-1a decomposition).
  PointerRegionAnalysis pointerRegions;
  /// Per-function decomposition of each accepted pointer local and each
  /// slice-classified pointer parameter, keyed by its declaration.
  llvm::DenseMap<const clang::VarDecl *, PointerLocalInfo> pointerLocals;
  /// Per-function second-order pointer locals (CTS-P5), each mapped to the
  /// single first-order pointer local it statically selects (the
  /// degenerate one-cell region of cursor cells); a second-order pointer
  /// carries no runtime state of its own.
  llvm::DenseMap<const clang::VarDecl *, const clang::VarDecl *>
      pointerPointerLocals;
  /// Per-function read-only backing byte arrays of string-literal pointer
  /// regions, keyed by the bound literal; created once per literal at the
  /// declaration of the first pointer bound to it.
  llvm::DenseMap<const clang::StringLiteral *, Value> literalBackings;
  /// Per-function prologue cells of by-value scalar parameters.
  /// `finalizeFunction` sweeps any such cell whose every remaining use is
  /// a store (the parameter is never read on any surviving path — e.g.
  /// its uses folded away with a statically-null pointer, CTS-P9), so a
  /// fully folded function carries no runtime state at all.
  SmallVector<Value, 8> paramCells;
  /// Cached Phase-1b parameter classifications, keyed by the function's
  /// canonical declaration (persists across the whole import; each TU's
  /// declarations are distinct clang decls, so entries never conflict).
  llvm::DenseMap<const clang::FunctionDecl *, SmallVector<ParamKind, 4>>
      paramKindsCache;
  /// Cached pointer-return kinds (CTS-P2), keyed by the function's
  /// canonical declaration: the mapped `!emitrust.fn_ptr` result type of a
  /// function whose data-pointer return classifies as a returned function
  /// address.
  llvm::DenseMap<const clang::FunctionDecl *, Type> pointerReturnKinds;
  /// Erased single-global-base pointer returns (CTS-S, 00089), keyed by the
  /// function's canonical declaration: a function whose every return site
  /// yields the address of this ONE mutable whole global classifies to an
  /// ERASED pointer result (its `pointerReturnKinds` entry is the null
  /// `Type`), and callers route accesses through the returned pointer to
  /// the recorded global directly.
  llvm::DenseMap<const clang::FunctionDecl *, const clang::VarDecl *>
      globalReturnBases;
  /// Erased single-global-base results of function POINTER types (CTS-S,
  /// 00089), keyed by the canonical clang function type of the pointee: a
  /// fn-ptr signature returning a data pointer is representable exactly
  /// when every address-taken function of that return type erases to the
  /// same global base (see `classifyFnPtrPointerResult`); indirect calls
  /// through such a pointer route to the recorded global like direct calls.
  llvm::DenseMap<const clang::Type *, const clang::VarDecl *>
      fnPtrReturnBases;
  /// Function-pointer pointee types whose data-pointer-result
  /// classification is currently being computed; a re-entry (a recursion
  /// cycle through a candidate's own return classification) rejects.
  llvm::SmallPtrSet<const clang::Type *, 4> fnPtrReturnInProgress;
  /// Devirtualized global function pointers (CTS-S, 00189), keyed by the
  /// variable's canonical declaration: a file-scope function pointer
  /// initialized to a known function and never reassigned (nor
  /// address-taken) anywhere in the TU is an import-time alias of its
  /// target. No `emitrust.global` is materialized; calls through the alias
  /// lower as direct calls (a hosted variadic target routes through the
  /// printf machinery), and value uses lower to the `Some(target)`
  /// constant. Populated per TU by `planFnPtrAliases`.
  llvm::DenseMap<const clang::VarDecl *, const clang::FunctionDecl *>
      fnPtrAliases;
  /// File-scope function-pointer variables the current TU assigns to (or
  /// takes the address of) somewhere in a function body; such a variable
  /// is never an alias. Rebuilt per TU by `planFnPtrAliases`.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> fnPtrGlobalsWritten;
  /// Functions of the current TU whose address is taken anywhere outside a
  /// direct-call callee position (function bodies and file-scope
  /// initializers alike); the candidate set of every fn-ptr value flow,
  /// consulted by `classifyFnPtrPointerResult`. Rebuilt per TU by
  /// `planFnPtrAliases`.
  llvm::SmallVector<const clang::FunctionDecl *, 8> addressTakenFunctions;
  /// The single-global-base of the function currently being imported when
  /// its pointer return was erased (CTS-S, 00089); null otherwise. Return
  /// sites emit a bare `return` (the address carries no runtime state).
  const clang::VarDecl *currentErasedReturnBase = nullptr;
  /// Cached integer-carrier return classifications (CTS-P3), keyed by the
  /// function's canonical declaration (see `isCarrierReturnFunction`).
  llvm::DenseMap<const clang::FunctionDecl *, bool> carrierReturnCache;
  /// Functions whose carrier-return classification is currently being
  /// computed; a re-entry (a recursion cycle) classifies pessimistically.
  llvm::SmallPtrSet<const clang::FunctionDecl *, 4> carrierReturnInProgress;
  /// Per-function i64 cells of integer-carrier pointer locals (CTS-P3),
  /// keyed by declaration: the pointer's entire runtime state is one plain
  /// i64 (null is 0); no base, cursor, or flag cell exists.
  llvm::DenseMap<const clang::VarDecl *, Value> carrierLocals;
  /// Per-function integer-carrier `void *` parameters (CTS-P3): their
  /// prologue cells live in `symbols` like any scalar parameter, and this
  /// set routes their truth tests and carrier reads.
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> carrierParams;
  /// Parameters whose interprocedural class qualified for the cell-slice
  /// lowering (CTS-P10), keyed by the definition's parameter declaration;
  /// populated by `planCellSlices` and consulted by
  /// `classifyPointerParams` (accumulates across TUs).
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 16> cellSliceParams;
  /// Located boundary verdicts for classes with global bases that did NOT
  /// qualify (mixed local+global, nullable global-backed), keyed by the
  /// canonical global base declaration; the call-site rejection consults
  /// this map for its precise wording.
  llvm::DenseMap<const clang::VarDecl *, CellSliceReject> cellSliceRejects;
  /// Phase-4 owner plans keyed by the promoted base variable declaration
  /// (accumulates across TUs; each TU's declarations are distinct).
  llvm::DenseMap<const clang::VarDecl *, OwnerPlan> ownerPlans;
  /// Phase-4 method plans: the canonical declaration of every function that
  /// becomes an owner method, mapped to its owner's base variable.
  llvm::DenseMap<const clang::FunctionDecl *, const clang::VarDecl *>
      methodPlans;
  /// Per-function owner struct places (populated in the owning function
  /// only), keyed by the promoted base variable; feeds method-call
  /// receivers. The struct place is only ever borrowed, never loaded.
  llvm::DenseMap<const clang::VarDecl *, Value> ownerStructPlaces;
  /// The `deref(arg0)` receiver place while importing a method body; null
  /// otherwise. Sibling method calls borrow it (rendering `(*self).m(...)`).
  Value currentReceiverPlace;
  /// The owner base variable of the method currently being imported; null
  /// when the current function is not a method.
  const clang::VarDecl *currentMethodOwner = nullptr;
  /// Struct definitions already imported (keyed on the defining decl).
  llvm::SmallPtrSet<const clang::RecordDecl *, 8> importedRecords;
  /// Enum definitions already imported (keyed on the defining decl).
  llvm::SmallPtrSet<const clang::EnumDecl *, 8> importedEnums;
  /// Imported functions by MLIR symbol name.
  llvm::StringMap<func::FuncOp> functions;
  /// Imported globals (file-scope variables and function-local statics),
  /// keyed by canonical clang declaration.
  llvm::DenseMap<const clang::VarDecl *, GlobalInfo> globals;
  /// Imported pointer-typed globals (CTS-P4), keyed by canonical
  /// declaration; disjoint from `globals` (a pointer global has no
  /// whole-value representation of its own, only a region base and an
  /// optional cursor global).
  llvm::DenseMap<const clang::VarDecl *, PointerGlobalInfo> pointerGlobals;
  /// Program-wide pointer-region facts of every global pointer variable,
  /// keyed by canonical declaration: `planOwners` (Pass A) merges each
  /// function body's region view, and `importPointerGlobal` (Pass B)
  /// validates the union against the file-scope initializer.
  llvm::DenseMap<const clang::VarDecl *, PointerRegion> globalPtrFacts;
  /// Program-wide member-pointer bindings (CTS-P2), keyed by (struct
  /// instance, data-pointer field): `planOwners` merges every function
  /// body's bindings, and the constant-initializer walk
  /// (`collectGlobalMemberBindings`) merges the bindings of global struct
  /// objects. Reads and writes of data-pointer members resolve against
  /// this map at their use sites.
  llvm::DenseMap<MemberPointerKey, MemberPointerFacts> memberPtrBindings;
  /// Data-pointer fields used somewhere in the program in a shape the
  /// per-instance member model cannot resolve, with the first such site;
  /// every read of a poisoned field is a located rejection.
  llvm::DenseMap<const clang::FieldDecl *, clang::SourceLocation>
      poisonedPtrFields;
  /// Whether the TU currently being imported is the whole program (see
  /// `importTranslationUnit`); pointer-typed globals with external linkage
  /// are rejected in multi-file projects because a later TU's bindings
  /// could invalidate facts this TU has already consumed.
  bool currentSoleTU = false;
  /// Stack of break/continue targets for nested loops and switches.
  SmallVector<LoopTargets> loopStack;
  /// Blocks started by C labels in the function under construction, keyed
  /// by label declaration; created lazily on first mention so forward and
  /// backward `goto`s share one map.
  llvm::DenseMap<const clang::LabelDecl *, Block *> labelBlocks;
  /// Dispatch target blocks for the case/default labels of every
  /// dispatch-lowered switch in the function under construction (see
  /// `emitDispatchSwitch`), keyed by the label statement. Structured
  /// switches never register their labels here.
  llvm::DenseMap<const clang::SwitchCase *, Block *> switchCaseBlocks;
  /// True when the function under construction contains any C label;
  /// `emitrust.variable` places are then hoisted to the entry block (see
  /// `createVariablePlace`).
  bool currentHasLabels = false;
  /// Entry block of the function under construction (owns the allocas).
  Block *entryBlock = nullptr;
  /// Body region of the function under construction.
  Region *bodyRegion = nullptr;
  /// Mapped return type of the current function; null for void.
  Type currentReturnType;
  /// MLIR symbol name of the function under construction (used to mangle
  /// function-local statics).
  std::string currentFuncName;
  /// True while translating C `main` (enables the implicit `return 0`).
  bool currentIsMain = false;
  /// True once a `%f` printf directive has been imported; triggers the
  /// one-per-module emission of the `__emitrust_fmt_f64` helper that
  /// matches C's non-finite `%f` spellings (`nan`/`-nan`).
  bool needsFloatFormatHelper = false;
  /// True once the `__emitrust_fmt_f64` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool floatFormatHelperEmitted = false;
  /// True once a `%c` printf directive (or a putchar call) has been
  /// imported; triggers the one-per-module emission of the
  /// `__emitrust_fmt_c` helper that renders the argument as C does
  /// (converted to unsigned char; ASCII-only, see design.md C99-48).
  bool needsCharFormatHelper = false;
  /// True once the `__emitrust_fmt_c` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool charFormatHelperEmitted = false;
  /// True once a `%s` char-array argument has been imported; triggers the
  /// one-per-module emission of the `__emitrust_cstr` helper that renders
  /// a char array up to its first NUL, matching C's `%s`.
  bool needsCStrHelper = false;
  /// True once the `__emitrust_cstr` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool cStrHelperEmitted = false;
  /// True once a definition-less `sprintf` call has been imported;
  /// triggers the one-per-module emission of the `__emitrust_sprintf`
  /// helper that copies the formatted bytes plus a NUL terminator into
  /// the destination slice and returns the written length.
  bool needsSprintfHelper = false;
  /// True once the `__emitrust_sprintf` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool sprintfHelperEmitted = false;
  /// True once a definition-less `strlen` call has been imported; triggers
  /// the one-per-module emission of the `__emitrust_strlen` helper that
  /// counts bytes up to the first NUL, matching C's strlen.
  bool needsStrlenHelper = false;
  /// True once the `__emitrust_strlen` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool strlenHelperEmitted = false;
  /// Hosted `<string.h>` helpers requested by lowered calls
  /// (`requestStringHelper`); each is emitted once per module, in the
  /// fixed order of the `kStringHelpers` table.
  llvm::StringSet<> neededStringHelpers;
  /// Helpers already emitted, so a multi-TU import never emits one twice.
  llvm::StringSet<> emittedStringHelpers;
  /// FILE* handle locals of the current function (C99-48): each maps to
  /// its owned `emitrust.variable` place of the opaque `__EmitrustFile`
  /// type. Reset per function like `symbols`.
  llvm::DenseMap<const clang::VarDecl *, Value> fileLocals;
  /// Hosted FILE* helpers requested by lowered stdio calls
  /// (`requestFileHelper`); each is emitted once per module, in the fixed
  /// order of the `kFileHelpers` table (the `__EmitrustFile` enum first).
  llvm::StringSet<> neededFileHelpers;
  /// FILE* helpers already emitted, so a multi-TU import never emits one
  /// twice.
  llvm::StringSet<> emittedFileHelpers;
};

} // namespace

//===----------------------------------------------------------------------===//
// AST helpers
//===----------------------------------------------------------------------===//

/// Returns true if the statement tree rooted at `stmt` contains any C label
/// (`LabelStmt`). Iterative worklist traversal over the AST.
static bool containsLabelStmt(const clang::Stmt *stmt) {
  SmallVector<const clang::Stmt *> worklist{stmt};
  while (!worklist.empty()) {
    const clang::Stmt *current = worklist.pop_back_val();
    if (!current)
      continue;
    if (llvm::isa<clang::LabelStmt>(current))
      return true;
    for (const clang::Stmt *child : current->children())
      worklist.push_back(child);
  }
  return false;
}

/// Returns true when the statement tree rooted at `body` touches C's
/// va_list machinery: a `va_arg` read (`VAArgExpr`), a call to any of the
/// va_start/va_end/va_copy builtins, or a declaration of a variable of the
/// target's `va_list` (`__builtin_va_list`) type. A variadic definition
/// whose body is va_list-free by this scan never observes its trailing
/// arguments, so it imports as its fixed prototype (CTS-P9); a body this
/// scan flags keeps the variadic-definition rejection. Iterative worklist
/// traversal over the AST.
static bool bodyUsesVaList(const clang::ASTContext &context,
                           const clang::Stmt *body) {
  clang::QualType vaListType =
      context.getBuiltinVaListType().getCanonicalType();
  SmallVector<const clang::Stmt *> worklist{body};
  while (!worklist.empty()) {
    const clang::Stmt *current = worklist.pop_back_val();
    if (!current)
      continue;
    if (llvm::isa<clang::VAArgExpr>(current))
      return true;
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(current)) {
      switch (call->getBuiltinCallee()) {
      case clang::Builtin::BI__builtin_va_start:
      case clang::Builtin::BI__builtin_c23_va_start:
      case clang::Builtin::BI__builtin_va_end:
      case clang::Builtin::BI__builtin_va_copy:
      case clang::Builtin::BI__builtin_ms_va_start:
      case clang::Builtin::BI__builtin_ms_va_end:
      case clang::Builtin::BI__builtin_ms_va_copy:
      case clang::Builtin::BI__va_start:
      case clang::Builtin::BIva_start:
      case clang::Builtin::BIva_end:
      case clang::Builtin::BIva_copy:
        return true;
      default:
        break;
      }
    }
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(current))
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          if (context.hasSameType(var->getType().getCanonicalType(),
                                  vaListType))
            return true;
    for (const clang::Stmt *child : current->children())
      worklist.push_back(child);
  }
  return false;
}

/// Strips parentheses and `ConstantExpr` wrappers (clang wraps constant
/// contexts such as case values in `ConstantExpr`) without touching casts.
static const clang::Expr *stripTrivia(const clang::Expr *expr) {
  while (true) {
    expr = expr->IgnoreParens();
    if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(expr)) {
      expr = constant->getSubExpr();
      continue;
    }
    return expr;
  }
}

/// Returns the defining declaration of `type`'s complete named enum, or
/// null when `type` is not an enum, incomplete, or anonymous.
static const clang::EnumDecl *namedEnumDeclOf(clang::QualType type) {
  const auto *enumType = llvm::dyn_cast<clang::EnumType>(
      type.getCanonicalType().getTypePtr());
  if (!enumType)
    return nullptr;
  const clang::EnumDecl *definition = enumType->getDecl()->getDefinition();
  if (!definition || definition->getName().empty())
    return nullptr;
  return definition;
}

/// Recognizes an operand that denotes a value of a complete named enum. In
/// C, Sema promotes enum operands of comparisons and switch conditions to
/// the enum's underlying integer type with an implicit `IntegralCast`, and
/// enumerator references themselves have type `int`; this helper peels one
/// such promotion cast and classifies what is underneath: an enum-typed
/// expression or an enumerator reference. Anonymous enums are plain `int`
/// values and yield `nullopt`.
static std::optional<EnumOperand>
classifyEnumOperand(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_IntegralCast)
      e = stripTrivia(cast->getSubExpr());
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (const auto *enumerator =
            llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl())) {
      const auto *parent =
          llvm::cast<clang::EnumDecl>(enumerator->getDeclContext());
      const clang::EnumDecl *definition = parent->getDefinition();
      if (!definition || definition->getName().empty())
        return std::nullopt;
      return EnumOperand{definition, nullptr, enumerator};
    }
  if (const clang::EnumDecl *decl = namedEnumDeclOf(e->getType()))
    return EnumOperand{decl, e, nullptr};
  return std::nullopt;
}

/// Finds a case or default label nested anywhere below `stmt` without
/// descending into nested switch statements (whose labels are their own and
/// are handled when the recursive statement importer reaches them). The root
/// itself may be a nested switch: a case arm whose sub-statement is another
/// switch is legal, so the check applies to the root as well as to children.
/// Used to route Duff's-device-style switches whose labels are not at the
/// top level of the switch body to the dispatch lowering
/// (`emitDispatchSwitch`) instead of the structured one.
static const clang::Stmt *findNestedSwitchLabel(const clang::Stmt *stmt) {
  if (!stmt || llvm::isa<clang::SwitchStmt>(stmt))
    return nullptr;
  for (const clang::Stmt *child : stmt->children()) {
    if (!child)
      continue;
    if (llvm::isa<clang::SwitchCase>(child))
      return child;
    if (const clang::Stmt *found = findNestedSwitchLabel(child))
      return found;
  }
  return nullptr;
}

/// Returns true if `body` has the plain shape the structured switch
/// lowering handles: the first top-level statement starts a case/default
/// label chain (so nothing precedes the first label) and no case/default
/// label of this switch is nested inside an inner statement. Any other
/// shape is lowered by `emitDispatchSwitch`.
static bool isPlainSwitchBody(const clang::CompoundStmt *body) {
  bool seenLabel = false;
  for (const clang::Stmt *child : body->body()) {
    const clang::Stmt *statement = child;
    if (llvm::isa<clang::SwitchCase>(child)) {
      seenLabel = true;
      while (const auto *label = llvm::dyn_cast<clang::SwitchCase>(statement))
        statement = label->getSubStmt();
    } else if (!seenLabel) {
      return false;
    }
    if (findNestedSwitchLabel(statement))
      return false;
  }
  return true;
}

/// Returns true if `type` is an MLIR unsigned integer type (the mapping of
/// the C unsigned integer types; signless types model the signed ones).
static bool isUnsignedInt(Type type) {
  auto intType = llvm::dyn_cast<IntegerType>(type);
  return intType && intType.isUnsigned();
}

/// Returns the C-declared Rust-facing name of a record: its tag name, or,
/// for a tagless record declared through `typedef struct { ... } T;`, the
/// typedef name. Returns an empty StringRef for a bare anonymous struct,
/// for which `importRecord` synthesizes a shape-keyed `Anon<n>` name
/// (retrieved through `CImporter::emittedRecordName`). The typedef name is
/// the record's name for all mangling and cross-TU shape-dedup purposes,
/// exactly like a tagged struct. This is the base spelling only: for
/// file-scope records `CImporter::structSymbolName` layers the tag-versus-
/// ordinary-namespace collision renaming on top, and block-scope records
/// take the `<function>_<tag>` mangle in `importRecord`.
static llvm::StringRef recordRustName(const clang::RecordDecl *record) {
  llvm::StringRef name = record->getName();
  if (!name.empty())
    return name;
  if (const clang::TypedefNameDecl *typedefName =
          record->getTypedefNameForAnonDecl())
    return typedefName->getName();
  return {};
}

/// Returns whether the canonical type of `type` is a C pointer type.
static bool isPointerType(clang::QualType type) {
  return type.getCanonicalType()->isPointerType();
}

/// Returns whether the canonical type of `type` is a C function pointer.
/// Function pointers are ordinary `!emitrust.fn_ptr` values and take none
/// of the data-pointer (decomposition or reference-parameter) paths.
static bool isFunctionPointer(clang::QualType type) {
  return type.getCanonicalType()->isFunctionPointerType();
}

/// Returns whether `type` is a data pointer: a C pointer that is not a
/// function pointer. Data pointers decompose into (base, cursor) pairs;
/// function pointers are ordinary Copy values.
static bool isDataPointer(clang::QualType type) {
  return isPointerType(type) && !isFunctionPointer(type);
}

/// C99-7 volatile policy: returns whether any level of `type` — the type
/// itself, an array element, or a pointee at any pointer depth — is
/// volatile-qualified. A volatile access has no counterpart in the
/// emitted single-threaded Rust model (no MMIO, no signal handlers, no
/// setjmp), so every declaration position rejects it with a located
/// diagnostic instead of silently dropping the qualifier; an
/// expression-level cast that introduces volatile refuses to peel
/// instead (see `peelPointerCast`). One exception: a qualifier on a
/// parameter OBJECT itself is body-local and never part of the function
/// type, so `mapParamType` strips the top level before scanning
/// (`int x[volatile 5]`, which adjusts to `int * volatile x`, imports
/// like the unqualified spelling). const and restrict are unaffected:
/// const maps positionally (immutable statics, const-marked variables,
/// SSA lets), and restrict is a pure optimization hint the region
/// analysis is already stricter than, so it is accepted and ignored.
static bool hasVolatileQualifier(clang::ASTContext &context,
                                 clang::QualType type) {
  clang::QualType current = type.getCanonicalType();
  while (true) {
    if (current.isVolatileQualified())
      return true;
    if (const clang::ArrayType *array = context.getAsArrayType(current)) {
      current = array->getElementType().getCanonicalType();
      continue;
    }
    if (current->isPointerType() && !current->isFunctionPointerType()) {
      current = current->getPointeeType().getCanonicalType();
      continue;
    }
    return false;
  }
}

/// Returns whether the canonical type of `type` is C's `FILE *` stream
/// handle (a pointer to the stdio stream record: glibc and musl spell it
/// `struct _IO_FILE`, BSD/macOS `struct __sFILE`, MSVC `struct _iobuf`,
/// and a freestanding header may leave the tag `FILE` itself). FILE*
/// values take the C99-48 owned-handle lowering — never the (base,
/// cursor) pointer decomposition — and are only supported as
/// function-local variables opened by fopen.
static bool isFilePtrType(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType();
  if (!canonical->isPointerType())
    return false;
  const auto *record =
      canonical->getPointeeType().getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  llvm::StringRef name = record->getDecl()->getName();
  return name == "FILE" || name == "_IO_FILE" || name == "__sFILE" ||
         name == "_iobuf";
}

/// Returns whether a FILE*-typed local variable takes the owned-handle
/// lowering (C99-48): declared with no initializer, or initialized
/// directly by a definition-less fopen call. Any other initializer
/// (stdout, another FILE* value, ...) falls back to the historical
/// pointer machinery and its located rejections, which older tests pin
/// (`FILE *g = stdout;` stays "copying a global pointer variable").
static bool isFileHandleLocal(const clang::VarDecl *var) {
  if (!var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var) ||
      !isFilePtrType(var->getType()))
    return false;
  const clang::Expr *init = var->getInit();
  if (!init)
    return true;
  const auto *call =
      llvm::dyn_cast<clang::CallExpr>(init->IgnoreParenImpCasts());
  const clang::FunctionDecl *callee = call ? call->getDirectCallee() : nullptr;
  return callee && callee->getDeclName().isIdentifier() &&
         callee->getName() == "fopen" && !callee->getDefinition();
}

/// Returns the data-pointer field a member expression designates, or null
/// when `expr` is not a member access or its field is not a data pointer.
static const clang::FieldDecl *dataPointerFieldOf(const clang::Expr *expr) {
  const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripTrivia(expr));
  if (!member)
    return nullptr;
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field || !isDataPointer(field->getType()))
    return nullptr;
  return field;
}

/// Returns the operand of a pointer cast (explicit C-style, or the
/// implicit bitcast Sema inserts for `void *` conversions) that is
/// transparent to the (base, cursor) decomposition, or null for every
/// other cast. Transparent casts are qualification adjustments (`(int *)p`
/// on an `int *`, `(const char *)s`) and — the pointee-wildcard rule,
/// CTS-P9 — casts to or from a `void` pointee at any matching pointer
/// depth: `(void *)&x`, `(int *)voidp`, and the second-order
/// `(int **)voidpp` all peel, because a `void *` carries no element unit
/// of its own. A reinterpret-back site `*(T *)p` therefore reaches the
/// deref emission, which type-checks `T` against the region's base
/// element type. Casts between distinct non-void pointees
/// (`(char *)&x`) and integer-to-pointer casts are never peeled: the
/// decomposition's element unit would change, so those shapes keep their
/// located rejections.
static const clang::Expr *peelPointerCast(clang::ASTContext &context,
                                          const clang::Expr *expr) {
  const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr);
  if (!cast || (!llvm::isa<clang::CStyleCastExpr>(cast) &&
                !llvm::isa<clang::ImplicitCastExpr>(cast)))
    return nullptr;
  if (cast->getCastKind() != clang::CK_NoOp &&
      cast->getCastKind() != clang::CK_BitCast)
    return nullptr;
  clang::QualType from = cast->getSubExpr()->getType().getCanonicalType();
  clang::QualType to = cast->getType().getCanonicalType();
  if (!isDataPointer(from) || !isDataPointer(to))
    return nullptr;
  while (true) {
    clang::QualType fromPointee =
        from->getPointeeType().getCanonicalType();
    clang::QualType toPointee = to->getPointeeType().getCanonicalType();
    // C99-7: a cast that introduces (or carries) a volatile-qualified
    // pointee is never transparent — volatile is rejected by policy, so
    // the site keeps a located rejection instead of silently dropping
    // the qualifier. const/restrict adjustments keep peeling.
    if (fromPointee.isVolatileQualified() || toPointee.isVolatileQualified())
      return nullptr;
    if (context.hasSameUnqualifiedType(fromPointee, toPointee))
      return cast->getSubExpr();
    if (fromPointee->isVoidType() || toPointee->isVoidType())
      return cast->getSubExpr();
    if (fromPointee->isPointerType() && toPointee->isPointerType()) {
      from = fromPointee;
      to = toPointee;
      continue;
    }
    return nullptr;
  }
}

/// Strips the leading pointer casts a *dereference* site sees through:
/// first the decomposition-transparent peels (`peelPointerCast` —
/// qualification adjustments and the `void *` wildcard), then direct
/// explicit bitcasts between distinct non-void single-level object
/// pointees (`(unsigned *)(char *)...`, the classic type-pun spelling,
/// CTS-P11). The latter are NOT transparent to the general decomposition
/// (binding a pointer through one still rejects); only the deref
/// emission strips them, and it then type-checks the viewed type against
/// the region's element type — exact matches lower directly, same-width
/// integer views bitcast, wider views over byte regions widen to
/// ne_bytes accesses, and everything else keeps the located
/// `reinterprets the pointee` rejection.
static const clang::Expr *stripObjectPointerCasts(clang::ASTContext &context,
                                                  const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (true) {
    if (const clang::Expr *sub = peelPointerCast(context, e)) {
      e = stripTrivia(sub);
      continue;
    }
    const auto *cast = llvm::dyn_cast<clang::CastExpr>(e);
    if (!cast || (!llvm::isa<clang::CStyleCastExpr>(cast) &&
                  !llvm::isa<clang::ImplicitCastExpr>(cast)) ||
        cast->getCastKind() != clang::CK_BitCast)
      return e;
    clang::QualType from = cast->getSubExpr()->getType();
    clang::QualType to = cast->getType();
    if (!isDataPointer(from) || !isDataPointer(to))
      return e;
    // Pointer-to-pointer reinterprets stay out (CTS-P5 scope).
    if (from.getCanonicalType()->getPointeeType()->isPointerType() ||
        to.getCanonicalType()->getPointeeType()->isPointerType())
      return e;
    e = stripTrivia(cast->getSubExpr());
  }
}

/// Returns whether the dereference of `expr` views the region through a
/// changed pointee: the fully cast-stripped pointer's pointee differs
/// from `expr`'s own pointee (covers both the `void *`-mediated
/// reinterpret-back sites of CTS-P9 and the direct pun casts of
/// CTS-P11). Qualification-only peels report false.
static bool viewsChangedPointee(clang::ASTContext &context,
                                const clang::Expr *expr) {
  const clang::Expr *stripped = stripObjectPointerCasts(context, expr);
  return !context.hasSameUnqualifiedType(
      expr->getType().getCanonicalType()->getPointeeType(),
      stripped->getType().getCanonicalType()->getPointeeType());
}

/// Returns whether `region` is statically null (CTS-P9): a consumable
/// base-less nullable region with a conditional (ternary) source — one
/// that only ever united null constants and other null-only pointers
/// through a pointer-typed conditional. Such a region carries zero
/// runtime state: no flag cell is materialized, null tests fold to
/// constants, a pointer-to-int cast folds to 0, and dereferences are
/// located rejections. A base-less nullable region built only from
/// direct null bindings keeps the historical CTS-P8 flag cell (the
/// pointers-null.c contract).
static bool isStaticallyNullRegion(const PointerRegion *region) {
  return region && region->invalidReason.empty() && region->bases.empty() &&
         !region->literalBase && !region->allocSite && region->nullable &&
         region->hasConditionalSource;
}

/// Returns whether `region` is an integer-carrier region (CTS-P3): a
/// consumable region whose only sources are integer-to-pointer casts,
/// calls returning carriers, and null pointer constants. Such a region
/// never addresses a modeled object — it is an integer riding in pointer
/// clothing — so each of its pointers lowers as a plain i64 value (null is
/// the i64 zero) with no base, cursor, or flag cell. Dereference and
/// pointer arithmetic have nothing to resolve against and stay located
/// rejections at emission.
static bool isCarrierRegion(const PointerRegion *region) {
  return region && region->invalidReason.empty() &&
         region->hasCarrierSource && region->bases.empty() &&
         !region->literalBase && !region->allocSite;
}

/// The statically resolved target of a `&root.member` address expression
/// (CTS-P9): the root object and the (possibly anonymous-chain-flattened)
/// leaf field. `touchesUnion` reports union storage anywhere on the path,
/// which has no unaliased member place to root a region at.
struct MemberAddressTarget {
  /// The root variable the member chain is rooted at; null when the shape
  /// is outside the single-object member-path model (arrow bases, nested
  /// named members, bitfields).
  const clang::VarDecl *root = nullptr;
  /// The leaf field whose address is taken.
  const clang::FieldDecl *field = nullptr;
  /// True when the leaf or any implicit anonymous hop lives in union
  /// storage.
  bool touchesUnion = false;
};

/// Classifies the operand of `&member-expr`: accepts a non-arrow member
/// of a directly named (local or global) struct variable, walking implicit
/// anonymous-aggregate hops (whose fields are flattened into the parent
/// struct_def, so a single `emitrust.member` projection reaches the leaf).
/// Reports union storage on the path via `touchesUnion`; every other shape
/// leaves `root` null.
static MemberAddressTarget
classifyMemberAddress(const clang::MemberExpr *memberExpr) {
  MemberAddressTarget target;
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl());
  if (!field)
    return target;
  target.field = field;
  target.touchesUnion = field->getParent()->isUnion();
  if (memberExpr->isArrow() || field->isBitField())
    return target;
  const clang::Expr *base = stripTrivia(memberExpr->getBase());
  while (const auto *inner = llvm::dyn_cast<clang::MemberExpr>(base)) {
    const auto *innerField =
        llvm::dyn_cast<clang::FieldDecl>(inner->getMemberDecl());
    if (!innerField || inner->isArrow())
      return target;
    if (innerField->getParent()->isUnion())
      target.touchesUnion = true;
    // Only implicit anonymous-aggregate hops keep the leaf reachable with
    // one flattened-name projection; a named intermediate member is a
    // nested path outside the model.
    if (!innerField->isAnonymousStructOrUnion())
      return target;
    base = stripTrivia(inner->getBase());
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (!isPointerType(var->getType()))
        target.root = var;
  return target;
}

/// Peels the casts a returned function address wears (the implicit or
/// C-style bitcast to a `void *`/data-pointer return type, no-op casts,
/// parens) and returns the underlying function reference expression (a
/// `DeclRefExpr` naming a `FunctionDecl`, reached through `&f` or the
/// function-to-pointer decay), or null when the expression is anything
/// else. Only qualification-preserving peeling: no cast that changes what
/// the value decomposes into is looked through.
static const clang::Expr *returnedFunctionExpr(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    clang::CastKind kind = cast->getCastKind();
    if (kind != clang::CK_BitCast && kind != clang::CK_NoOp &&
        kind != clang::CK_FunctionToPointerDecay)
      return nullptr;
    if (kind == clang::CK_FunctionToPointerDecay) {
      e = cast->getSubExpr()->IgnoreParens();
      break;
    }
    e = cast->getSubExpr()->IgnoreParens();
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_AddrOf)
      e = unary->getSubExpr()->IgnoreParens();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (llvm::isa<clang::FunctionDecl>(ref->getDecl()))
      return ref;
  return nullptr;
}

/// The base object of one returned global address site (CTS-S, 00089): a
/// return-site expression of the form `&g` (whole object, cursor 0) sets
/// `base` to the global `g` with `wholeObject` true; `&g.member...` (a
/// member address rooted at a global through non-arrow, non-bitfield hops)
/// sets `base` with `wholeObject` false. Anything else leaves `base` null.
struct ReturnedGlobalAddress {
  const clang::VarDecl *base = nullptr;
  bool wholeObject = false;
};

/// Classifies a return-site expression as a returned global address (see
/// `ReturnedGlobalAddress`). Only qualification-preserving casts (no-op or
/// bit casts, parens) are peeled — mirroring `returnedFunctionExpr` — so
/// no cast that changes what the value decomposes into is looked through.
static ReturnedGlobalAddress returnedGlobalAddress(const clang::Expr *expr) {
  ReturnedGlobalAddress result;
  const clang::Expr *e = expr->IgnoreParens();
  while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    clang::CastKind kind = cast->getCastKind();
    if (kind != clang::CK_BitCast && kind != clang::CK_NoOp)
      return result;
    e = cast->getSubExpr()->IgnoreParens();
  }
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
  if (!unary || unary->getOpcode() != clang::UO_AddrOf)
    return result;
  e = stripTrivia(unary->getSubExpr());
  bool whole = true;
  while (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
    if (member->isArrow())
      return result;
    whole = false;
    e = stripTrivia(member->getBase());
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *var = ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl())
                        : nullptr;
  if (!var || !var->hasGlobalStorage() || isPointerType(var->getType()))
    return result;
  result.base = llvm::cast<clang::VarDecl>(var->getCanonicalDecl());
  result.wholeObject = whole;
  return result;
}

/// Collects every `return` statement of `stmt`'s subtree into `returns`.
static void collectReturnStmts(
    const clang::Stmt *stmt,
    SmallVectorImpl<const clang::ReturnStmt *> &returns) {
  if (!stmt)
    return;
  if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt))
    returns.push_back(ret);
  for (const clang::Stmt *child : stmt->children())
    collectReturnStmts(child, returns);
}

/// Returns the record declaration of `type` when it (canonically) is a
/// complete struct type; null otherwise.
static const clang::RecordDecl *recordOfType(clang::QualType type) {
  const auto *recordType =
      type.getCanonicalType()->getAs<clang::RecordType>();
  if (!recordType)
    return nullptr;
  const clang::RecordDecl *record = recordType->getDecl();
  return record->getDefinition();
}

/// Returns whether `record` (transitively, through nested struct and
/// array-of-struct fields) contains a data-pointer field. Cycles cannot
/// arise: recursion only follows by-value struct fields, and a struct
/// cannot contain itself by value.
static bool recordHasDataPointerField(const clang::RecordDecl *record) {
  if (!record)
    return false;
  for (const clang::FieldDecl *field : record->fields()) {
    clang::QualType fieldType = field->getType();
    while (const auto *array = llvm::dyn_cast<clang::ArrayType>(
               fieldType.getCanonicalType().getTypePtr()))
      fieldType = array->getElementType();
    if (isDataPointer(fieldType))
      return true;
    if (recordHasDataPointerField(recordOfType(fieldType)))
      return true;
  }
  return false;
}

/// Returns the number of innermost (non-array) elements one value of
/// `type` spans: the product of all constant array extents, or 1 for a
/// non-array type. This is the scale factor of the flat row-major cursor
/// scheme: a decomposed pointer's i64 cursor counts innermost elements of
/// its base object, so an index over a row of a multi-dimensional array
/// advances the cursor by the row's flat element count.
static uint64_t flatElementCount(clang::ASTContext &context,
                                 clang::QualType type) {
  uint64_t count = 1;
  const clang::ConstantArrayType *array = context.getAsConstantArrayType(type);
  while (array) {
    count *= array->getSize().getZExtValue();
    array = context.getAsConstantArrayType(array->getElementType());
  }
  return count;
}

/// Returns whether the pointer-typed expression type `type` points to a
/// whole array (a row of a multi-dimensional array, e.g. `char (*)[4]`).
/// Arithmetic on such pointers moves the cursor by whole rows, which the
/// flat cursor scheme only implements for the subscript and address-of
/// forms; the walking forms (`++`, `+ n`, `+=`, difference) are rejected.
static bool pointsToArray(clang::QualType type) {
  return type.getCanonicalType()->getPointeeType()->isArrayType();
}

/// Returns the local, non-parameter variable a stripped declaration
/// reference `expr` names, or null when `expr` is not such a reference.
static const clang::VarDecl *asLocalVarRef(const clang::Expr *expr) {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var || !var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var))
    return nullptr;
  return var;
}

/// Returns the local, non-parameter variable behind a (possibly
/// lvalue-to-rvalue-wrapped) declaration reference `expr`, or null when
/// `expr` is not such a reference. The dereference forms of a second-order
/// pointer (`*pp`, `**pp`) read the pointer through a load, so their
/// resolvers strip the load wrapper first (CTS-P5).
static const clang::VarDecl *asLoadedLocalVarRef(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_LValueToRValue ||
        cast->getCastKind() == clang::CK_NoOp)
      e = cast->getSubExpr();
  return asLocalVarRef(e);
}

/// Returns whether `type` is a second-order data pointer: a pointer whose
/// pointee is itself a pointer (`T **`, including a pointer to a function
/// pointer, which the importer rejects at the declaration).
static bool isSecondOrderPointerType(clang::QualType type) {
  return isPointerType(type) &&
         isPointerType(type.getCanonicalType()->getPointeeType());
}

/// Returns the local variable or parameter a stripped declaration
/// reference `expr` names, or null when `expr` is not such a reference.
/// Used by the pointer choke points that accept both decomposed pointer
/// locals and slice-classified pointer parameters.
static const clang::VarDecl *asVarRef(const clang::Expr *expr) {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var || !var->hasLocalStorage())
    return nullptr;
  return var;
}

/// Returns the static-storage (file-scope or static-local) data-pointer
/// variable a stripped declaration reference `expr` names, canonicalized,
/// or null when `expr` is not such a reference. Global pointers
/// participate in the region analysis so that `planOwners` can merge
/// their per-function facts program-wide and `importPointerGlobal` can
/// validate the union (CTS-P4).
static const clang::VarDecl *asGlobalDataPointerRef(const clang::Expr *expr) {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var || var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var))
    return nullptr;
  if (!isPointerType(var->getType()) || isFunctionPointer(var->getType()))
    return nullptr;
  return var->getCanonicalDecl();
}

/// Returns the `calloc`/`malloc` call at the root of `expr` (looking
/// through casts, e.g. the implicit `void *` conversion), or null. Only
/// definition-less declarations qualify: a user-defined function of the
/// same name is an ordinary call, never a promotable allocation.
static const clang::CallExpr *asAllocCall(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    e = stripTrivia(cast->getSubExpr());
  const auto *call = llvm::dyn_cast<clang::CallExpr>(e);
  if (!call)
    return nullptr;
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee || callee->hasBody() || !callee->getIdentifier())
    return nullptr;
  llvm::StringRef name = callee->getName();
  if (name != "calloc" && name != "malloc")
    return nullptr;
  return call;
}

/// Returns the pointer-typed parameter a stripped (possibly
/// lvalue-to-rvalue-wrapped) declaration reference `expr` names, or null
/// when `expr` is not such a reference.
static const clang::ParmVarDecl *asPointerParamRef(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_LValueToRValue ||
        cast->getCastKind() == clang::CK_NoOp)
      e = stripTrivia(cast->getSubExpr());
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  if (!ref)
    return nullptr;
  const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl());
  if (!param || !isPointerType(param->getType()))
    return nullptr;
  return param;
}

/// Recursive walk of `classifyPointerParams`: collects every pointer
/// parameter whose use demands the whole element run. A direct dereference
/// (`*p`) or arrow (`p->f`) is benign and keeps the parameter a scalar
/// reference; any other appearance of a pointer parameter (subscript,
/// arithmetic, comparison, difference, reassignment, copy into a pointer
/// local, address-of, call argument) inserts it into `sliceParams`.
static void collectSliceParams(
    const clang::Stmt *stmt,
    llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &sliceParams) {
  if (!stmt)
    return;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (unary->getOpcode() == clang::UO_Deref &&
        asPointerParamRef(unary->getSubExpr()))
      return; // Benign direct dereference; do not descend into the read.
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt))
    if (member->isArrow() && asPointerParamRef(member->getBase()))
      return; // Benign arrow access; the base has no other children.
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (const auto *param =
            llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
      if (isPointerType(param->getType()))
        sliceParams.insert(param);
  for (const clang::Stmt *child : stmt->children())
    collectSliceParams(child, sliceParams);
}

static bool voidParamOnlyTruthTested(const clang::Stmt *stmt,
                                     const clang::ParmVarDecl *param);

/// Scans a condition expression for the integer-carrier classification of
/// `void *` parameters (CTS-P3): bare loads of `param` are consumed as
/// truth tests, looking through parens, `PointerToBoolean` conversions,
/// `!`, and the `&&`/`||` connectives; any other subexpression is walked
/// generally (so a use of `param` inside it disqualifies).
static bool voidParamCondOk(const clang::Expr *cond,
                            const clang::ParmVarDecl *param) {
  const clang::Expr *e = stripTrivia(cond);
  if (asPointerParamRef(e) == param)
    return true;
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_PointerToBoolean)
      return voidParamCondOk(cast->getSubExpr(), param);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_LNot)
      return voidParamCondOk(unary->getSubExpr(), param);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e))
    if (binary->getOpcode() == clang::BO_LAnd ||
        binary->getOpcode() == clang::BO_LOr)
      return voidParamCondOk(binary->getLHS(), param) &&
             voidParamCondOk(binary->getRHS(), param);
  return voidParamOnlyTruthTested(e, param);
}

/// Returns whether every use of the `void *` parameter `param` below
/// `stmt` is a truth test: the condition position of if/while/do/for and
/// of the conditional operator, or a `!` in any expression position. Such
/// a parameter never acts as a pointer at all, so it classifies as an
/// integer carrier (`ParamKind::Carrier`, CTS-P3); any other appearance
/// keeps the historical void-pointer-parameter rejection.
static bool voidParamOnlyTruthTested(const clang::Stmt *stmt,
                                     const clang::ParmVarDecl *param) {
  if (!stmt)
    return true;
  if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt))
    return voidParamCondOk(ifStmt->getCond(), param) &&
           voidParamOnlyTruthTested(ifStmt->getInit(), param) &&
           voidParamOnlyTruthTested(ifStmt->getThen(), param) &&
           voidParamOnlyTruthTested(ifStmt->getElse(), param);
  if (const auto *whileStmt = llvm::dyn_cast<clang::WhileStmt>(stmt))
    return voidParamCondOk(whileStmt->getCond(), param) &&
           voidParamOnlyTruthTested(whileStmt->getBody(), param);
  if (const auto *doStmt = llvm::dyn_cast<clang::DoStmt>(stmt))
    return voidParamCondOk(doStmt->getCond(), param) &&
           voidParamOnlyTruthTested(doStmt->getBody(), param);
  if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
    return (!forStmt->getCond() ||
            voidParamCondOk(forStmt->getCond(), param)) &&
           voidParamOnlyTruthTested(forStmt->getInit(), param) &&
           voidParamOnlyTruthTested(forStmt->getInc(), param) &&
           voidParamOnlyTruthTested(forStmt->getBody(), param);
  if (const auto *conditional =
          llvm::dyn_cast<clang::ConditionalOperator>(stmt))
    return voidParamCondOk(conditional->getCond(), param) &&
           voidParamOnlyTruthTested(conditional->getTrueExpr(), param) &&
           voidParamOnlyTruthTested(conditional->getFalseExpr(), param);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (unary->getOpcode() == clang::UO_LNot)
      return voidParamCondOk(unary, param);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (ref->getDecl() == param)
      return false; // A use that no truth-test context consumed.
  for (const clang::Stmt *child : stmt->children())
    if (!voidParamOnlyTruthTested(child, param))
      return false;
  return true;
}

/// Returns the local variable at the root of an address-of call argument
/// (`&x`, `&s.f`, `&arr[i]`), or null when no single local root is known.
/// Feeds the same-base aliasing rejection of `emitCall`.
static const clang::VarDecl *addressArgumentRoot(const clang::Expr *expr) {
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(expr));
  if (!unary || unary->getOpcode() != clang::UO_AddrOf)
    return nullptr;
  const clang::Expr *place = stripTrivia(unary->getSubExpr());
  while (true) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(place)) {
      if (member->isArrow())
        return nullptr;
      place = stripTrivia(member->getBase());
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(place)) {
      place = subscript->getBase()->IgnoreParenImpCasts();
      continue;
    }
    break;
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(place))
    return llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  return nullptr;
}

//===----------------------------------------------------------------------===//
// PointerRegionAnalysis
//===----------------------------------------------------------------------===//

namespace {

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
      break;
    }
    case clang::CK_ArrayToPointerDecay: {
      // `p = arr`: the decayed array is the region base. `p = "..."`
      // binds the literal as the region's read-only base. A decayed row
      // of a multi-dimensional array (`q = arr[i]`) peels the subscripts
      // to the same root.
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(sub))
        return addLiteralBase(ptr, literal, loc);
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
        // their global import instead).
        if (var->hasLocalStorage() && !llvm::isa<clang::ParmVarDecl>(var))
          if (const auto *list = llvm::dyn_cast_if_present<
                  clang::InitListExpr>(var->getInit()))
            if (recordOfType(var->getType()))
              collectStructInitBindings(var, list);
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
  }
  // Pointer call arguments no longer invalidate the region (Phase 1b):
  // `emitCall` reborrows the region base per target-parameter kind
  // (emitrust.slice_of / emitrust.addr_of), so no raw pointer ever escapes.

  for (const clang::Stmt *child : stmt->children())
    visit(child);
}

} // namespace

//===----------------------------------------------------------------------===//
// Locations and types
//===----------------------------------------------------------------------===//

Location CImporter::translateLoc(clang::SourceLocation sourceLoc) {
  MLIRContext *context = builder.getContext();
  if (sourceLoc.isInvalid())
    return UnknownLoc::get(context);
  const clang::SourceManager &sourceManager = astContext().getSourceManager();
  clang::PresumedLoc presumed = sourceManager.getPresumedLoc(sourceLoc);
  if (presumed.isInvalid())
    return UnknownLoc::get(context);
  return FileLineColLoc::get(StringAttr::get(context, presumed.getFilename()),
                             presumed.getLine(), presumed.getColumn());
}

bool CImporter::isSystemHeaderDecl(const clang::Decl *decl) const {
  const clang::SourceManager &sourceManager = astContext().getSourceManager();
  clang::SourceLocation loc =
      sourceManager.getExpansionLoc(decl->getLocation());
  return loc.isValid() && sourceManager.isInSystemHeader(loc);
}

LogicalResult CImporter::rejectSystemHeaderUse(Location loc,
                                               llvm::StringRef what,
                                               llvm::StringRef name) {
  return emitError(loc) << "unsupported: " << what << " '" << name
                        << "' declared in a system header; not part of the "
                           "supported C subset";
}

FailureOr<Type> CImporter::mapType(clang::QualType type, Location loc) {
  clang::QualType canonical = type.getCanonicalType();

  // C99-7 qualifier policy. volatile: located rejection (no Rust
  // counterpart in the emitted model). _Atomic: located rejection (no
  // atomics in the single-threaded model; checked here for a precise
  // message instead of the generic tail rejection). const: no effect on
  // the value type — the const mapping is positional (immutable statics
  // for never-written globals, const-marked variables for literal
  // backings, plain SSA lets after mem2reg). restrict: accepted and
  // ignored (an aliasing hint; the pointer region analysis is stricter).
  if (canonical.isVolatileQualified())
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (canonical->isAtomicType())
    return emitError(loc) << "unsupported: _Atomic-qualified type";

  if (const auto *builtin =
          llvm::dyn_cast<clang::BuiltinType>(canonical.getTypePtr())) {
    switch (builtin->getKind()) {
    case clang::BuiltinType::Bool:
      return Type(builder.getI1Type());
    case clang::BuiltinType::Char_S:
    case clang::BuiltinType::SChar:
      return Type(builder.getIntegerType(8));
    case clang::BuiltinType::Short:
      return Type(builder.getIntegerType(16));
    case clang::BuiltinType::Int:
      return Type(builder.getIntegerType(32));
    case clang::BuiltinType::Long:
    case clang::BuiltinType::LongLong:
      return Type(builder.getIntegerType(64));
    case clang::BuiltinType::Float:
      return Type(builder.getF32Type());
    case clang::BuiltinType::Double:
      return Type(builder.getF64Type());
    // Unsigned types map to MLIR unsigned (not signless) integers so the
    // Rust emitter renders them as `uN`; arith ops require signless
    // operands, so all arithmetic on these goes through emitrust ops.
    case clang::BuiltinType::Char_U:
    case clang::BuiltinType::UChar:
      return Type(IntegerType::get(builder.getContext(), 8,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::UShort:
      return Type(IntegerType::get(builder.getContext(), 16,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::UInt:
      return Type(IntegerType::get(builder.getContext(), 32,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::ULong:
    case clang::BuiltinType::ULongLong:
      return Type(IntegerType::get(builder.getContext(), 64,
                                   IntegerType::Unsigned));
    default:
      break;
    }
    return emitError(loc) << "unsupported builtin type '"
                          << llvm::Twine(canonical.getAsString()) << "'";
  }

  if (const auto *record =
          llvm::dyn_cast<clang::RecordType>(canonical.getTypePtr())) {
    const clang::RecordDecl *decl = record->getDecl();
    // A union imports as a one-field struct (see `collectUnionSlot`), so
    // it maps to the same `!emitrust.struct` any record does; shapes the
    // one-slot model cannot represent are rejected by the import below.
    const clang::RecordDecl *definition = decl->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete struct type";
    if (failed(importRecord(definition, loc)))
      return failure();
    // The type name is resolved after the import: a block-scope record
    // maps to its mangled per-declaration name (see localRecordNames), a
    // file-scope record to the collision-resolved name assigned by
    // `structSymbolName` during the import, and a bare anonymous struct
    // to its synthesized shape-keyed name.
    std::string structName = emittedRecordName(definition);
    if (structName.empty())
      return emitError(loc) << "unsupported: anonymous struct type";
    return Type(emitrust::StructType::get(builder.getContext(), structName));
  }

  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(canonical)) {
    FailureOr<Type> element = mapType(array->getElementType(), loc);
    if (failed(element))
      return failure();
    // A multi-dimensional array recurses naturally: the element of the
    // outer dimension is itself an `!emitrust.array` (rendered as the
    // nested Rust array `[[T; N]; M]`). Arrays of function pointers are
    // out of the v1 fn_ptr scope; reject loudly instead of building an
    // !emitrust.array the dialect does not admit.
    if (llvm::isa<emitrust::FnPtrType>(*element))
      return emitError(loc) << "unsupported: array of function pointers";
    uint64_t size = array->getSize().getZExtValue();
    if (size == 0)
      return emitError(loc) << "unsupported: zero-length array";
    return Type(emitrust::ArrayType::get(builder.getContext(), size, *element));
  }

  if (const auto *enumType =
          llvm::dyn_cast<clang::EnumType>(canonical.getTypePtr())) {
    const clang::EnumDecl *definition = enumType->getDecl()->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete enum type";
    // Anonymous enums are plain `int` everywhere else in the importer
    // (enumerator references become `i32` constants at their use sites), so
    // a value of anonymous enum type is a plain `i32`.
    if (definition->getName().empty())
      return Type(builder.getIntegerType(32));
    if (failed(importEnum(definition, loc)))
      return failure();
    return Type(
        emitrust::EnumType::get(builder.getContext(), definition->getName()));
  }

  // Function pointers are ordinary `!emitrust.fn_ptr` values (rendered
  // `Option<fn(...)>`), legal as locals, globals, struct fields,
  // parameters, and results alike; they must be recognized before the
  // general pointer rejection below.
  if (canonical->isFunctionPointerType()) {
    const auto *fnType =
        canonical->getPointeeType()->castAs<clang::FunctionType>();
    SmallVector<Type> inputs;
    if (const auto *proto = llvm::dyn_cast<clang::FunctionProtoType>(fnType)) {
      if (proto->isVariadic())
        return emitError(loc) << "unsupported: variadic function pointer type";
      for (clang::QualType param : proto->getParamTypes()) {
        FailureOr<Type> mapped = mapType(param, loc);
        if (failed(mapped))
          return failure();
        if (!emitrust::FnPtrType::isValidComponentType(*mapped))
          return emitError(loc)
                 << "unsupported: function pointer parameter type";
        inputs.push_back(*mapped);
      }
    }
    // A prototype-less K&R `int (*f)()` maps to the zero-parameter form;
    // calls with arguments through it are rejected at the call site.
    SmallVector<Type> results;
    clang::QualType returnType = fnType->getReturnType();
    if (!returnType->isVoidType()) {
      if (isDataPointer(returnType)) {
        // A data-pointer fn-ptr result (CTS-S, 00089) is representable
        // only when every possible target erases it to one global base;
        // the result then erases from the fn_ptr signature too, and
        // indirect calls route to the base (`erasedGlobalReturnCallBase`).
        if (failed(classifyFnPtrPointerResult(fnType, loc)))
          return failure();
      } else {
        FailureOr<Type> mapped = mapType(returnType, loc);
        if (failed(mapped))
          return failure();
        if (!emitrust::FnPtrType::isValidComponentType(*mapped))
          return emitError(loc)
                 << "unsupported: function pointer result type";
        results.push_back(*mapped);
      }
    }
    return Type(
        emitrust::FnPtrType::get(builder.getContext(), inputs, results));
  }

  // A FILE* is an owned function-local stream handle (C99-48); any other
  // position (array element, return type, ...) that maps its type keeps
  // this located rejection.
  if (isFilePtrType(canonical))
    return emitError(loc)
           << "unsupported: FILE* is only supported as a function-local "
              "variable";
  if (canonical->isPointerType())
    return emitError(loc)
           << "unsupported: pointer type outside a parameter position";
  if (canonical->isArrayType())
    return emitError(loc) << "unsupported: non-constant array size";
  return emitError(loc) << "unsupported type '"
                        << llvm::Twine(canonical.getAsString()) << "'";
}

FailureOr<Type> CImporter::mapParamType(clang::QualType type, Location loc,
                                        ParamKind kind) {
  // C99-7: qualifiers on the parameter OBJECT itself are body-local and
  // never part of the function type (C11 6.7.6.3p15 composite rules;
  // `int x[volatile 5]` adjusts to `int * volatile x`, c-testsuite
  // 00162), so a top-level volatile is accepted and ignored exactly like
  // const and restrict. volatile anywhere deeper — the pointee chain,
  // which names caller-owned storage — keeps the located rejection; the
  // deep scan also covers the Carrier shape that bypasses `mapType`.
  clang::QualType canonical = type.getCanonicalType().getUnqualifiedType();
  if (canonical->isPointerType() && !canonical->isFunctionPointerType() &&
      hasVolatileQualifier(astContext(), canonical->getPointeeType()))
    return emitError(loc) << "unsupported: volatile-qualified type";
  // A function-pointer parameter is an ordinary Copy value, not a
  // reference; it maps to `!emitrust.fn_ptr` like every other position
  // (through the stripped canonical, so a qualifier on the parameter
  // object itself stays ignored).
  if (canonical->isFunctionPointerType())
    return mapType(canonical, loc);
  if (canonical->isPointerType()) {
    clang::QualType pointee = canonical->getPointeeType();
    // An integer-carrier `void *` parameter (CTS-P3) is a plain i64: the
    // callee only ever truth-tests it, so the value never needs a region.
    if (kind == ParamKind::Carrier)
      return Type(builder.getIntegerType(64));
    // A `void *` parameter has no element type to classify against and no
    // region to join at the call boundary (CTS-P9).
    if (pointee.getCanonicalType()->isVoidType())
      return emitError(loc) << "unsupported: void pointer parameter";
    // A pointee that is itself a *data* pointer has no representation
    // (CTS-P5); a function-pointer pointee is an ordinary Copy value
    // (`!emitrust.fn_ptr`) and slices/references over it are fine — the
    // shape a decayed array-of-function-pointers parameter produces.
    if (pointee.getCanonicalType()->isPointerType() &&
        !pointee.getCanonicalType()->isFunctionPointerType())
      return emitError(loc) << "unsupported: pointer-to-pointer parameter";
    FailureOr<Type> inner = mapType(pointee, loc);
    if (failed(inner))
      return failure();
    if (kind == ParamKind::CellSlice) {
      // A cell-slice class parameter (CTS-P10): a shared reference to a
      // run of Cells over one of the class's mutable global array bases.
      if (!emitrust::CellSliceType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: cell-slice parameter element type " << *inner;
      return Type(
          emitrust::RefType::get(emitrust::CellSliceType::get(*inner)));
    }
    if (kind == ParamKind::Slice) {
      // A slice element must be sized and scalar/struct; a pointer to an
      // array (`int (*)[N]`) has no slice shape.
      if (!emitrust::SliceType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: slice parameter element type " << *inner;
      return Type(
          emitrust::MutRefType::get(emitrust::SliceType::get(*inner)));
    }
    return Type(emitrust::MutRefType::get(*inner));
  }
  // The stripped canonical keeps a top-level-volatile value parameter
  // (`volatile int x`, a body-local copy) out of `mapType`'s rejection.
  return mapType(canonical, loc);
}

FailureOr<Type> CImporter::mapStructFieldType(clang::QualType type,
                                              Location loc) {
  // C99-7: a data-pointer field stores as a plain i64 without mapping its
  // pointee, so the volatile scan must run before that shortcut.
  if (hasVolatileQualifier(astContext(), type))
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (isDataPointer(type))
    return Type(builder.getIntegerType(64));
  return mapType(type, loc);
}

ArrayRef<ParamKind>
CImporter::classifyPointerParams(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = paramKindsCache.find(canonical);
  if (it != paramKindsCache.end())
    return it->second;
  SmallVector<ParamKind, 4> kinds(func->getNumParams(), ParamKind::ScalarRef);
  // The classification is a property of the definition's body; without a
  // definition in the merged ASTs every pointer parameter stays a scalar
  // reference (checked at definition-time signature refinement).
  const clang::FunctionDecl *definition = func->getDefinition();
  if (definition && definition->hasBody() &&
      definition->getNumParams() == kinds.size()) {
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> sliceParams;
    collectSliceParams(definition->getBody(), sliceParams);
    for (auto [index, param] : llvm::enumerate(definition->parameters())) {
      if (sliceParams.contains(param))
        kinds[index] = ParamKind::Slice;
      // The interprocedural cell-slice class (CTS-P10, planned in Pass A)
      // overrides the per-body slice classification.
      if (cellSliceParams.contains(param))
        kinds[index] = ParamKind::CellSlice;
      // A `void *` parameter that the body only ever truth-tests is an
      // integer carrier (CTS-P3): it lowers as a plain i64 and call sites
      // pass carrier values. Any other `void *` parameter keeps the
      // historical rejection in `mapParamType`.
      if (isDataPointer(param->getType()) &&
          param->getType()
              .getCanonicalType()
              ->getPointeeType()
              .getCanonicalType()
              ->isVoidType() &&
          voidParamOnlyTruthTested(definition->getBody(), param))
        kinds[index] = ParamKind::Carrier;
    }
  }
  auto [entry, inserted] =
      paramKindsCache.try_emplace(canonical, std::move(kinds));
  (void)inserted;
  return entry->second;
}

FailureOr<Type> CImporter::classifyPointerReturn(
    const clang::FunctionDecl *func, Location loc) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = pointerReturnKinds.find(canonical);
  if (it != pointerReturnKinds.end())
    return it->second;
  const clang::FunctionDecl *definition = func->getDefinition();
  if (!definition || !definition->hasBody())
    return emitError(loc) << "unsupported: pointer return type";
  SmallVector<const clang::ReturnStmt *> returns;
  collectReturnStmts(definition->getBody(), returns);
  if (returns.empty())
    return emitError(loc) << "unsupported: pointer return type";
  // The integer-carrier kind (CTS-P3): every return site yields an
  // integer riding in pointer clothing (a null constant, an
  // integer-to-pointer cast, a carrier-region local, or a call to another
  // carrier-returning function), so the function returns a plain i64.
  if (llvm::any_of(returns,
                   [](const clang::ReturnStmt *ret) {
                     return !ret->getRetValue() ||
                            !returnedFunctionExpr(ret->getRetValue());
                   }) &&
      isCarrierReturnFunction(func)) {
    Type kind = builder.getIntegerType(64);
    pointerReturnKinds.try_emplace(canonical, kind);
    return kind;
  }
  // The single-global-base RETURN region kind (CTS-S, 00089): one return
  // site yielding the address of a global claims the kind for the whole
  // function — every site must then return the SAME whole mutable global
  // (cursor 0) and no site may return NULL. The pointer result ERASES from
  // the signature (the classification is the null `Type`): no runtime
  // pointer state travels, the call is retained for its side effects, and
  // callers route accesses through the returned pointer to the global
  // directly (see `erasedGlobalReturnCallBase`).
  if (llvm::any_of(returns, [](const clang::ReturnStmt *ret) {
        return ret->getRetValue() &&
               returnedGlobalAddress(ret->getRetValue()).base;
      })) {
    const clang::VarDecl *commonBase = nullptr;
    Location memberSiteLoc = loc;
    bool sawMemberSite = false;
    for (const clang::ReturnStmt *ret : returns) {
      Location retLoc = translateLoc(ret->getReturnLoc());
      const clang::Expr *value = ret->getRetValue();
      if (!value)
        return emitError(retLoc) << "unsupported: returned pointer value "
                                    "(a bare return cannot carry the "
                                    "global address)";
      if (isNullPointerConstantExpr(value))
        return emitError(retLoc)
               << "unsupported: return sites mix a global address and NULL";
      ReturnedGlobalAddress site = returnedGlobalAddress(value);
      if (!site.base)
        return emitError(retLoc)
               << "unsupported: returned pointer value (only a returned "
                  "whole-global or function address has a representation)";
      if (commonBase && site.base != commonBase)
        return emitError(retLoc) << "unsupported: return sites disagree on "
                                    "the returned global base";
      commonBase = site.base;
      if (!site.wholeObject) {
        sawMemberSite = true;
        memberSiteLoc = retLoc;
      }
    }
    if (sawMemberSite)
      return emitError(memberSiteLoc)
             << "unsupported: returned pointer value (a member address is "
                "not a whole-global base)";
    if (commonBase->getType().isConstQualified())
      return emitError(loc)
             << "unsupported: returned address of a const global";
    // The erased routing stages and stores the base back at its own type,
    // so the returned pointee must be exactly the whole global's type.
    clang::QualType pointee = definition->getReturnType()
                                  .getCanonicalType()
                                  ->getPointeeType();
    if (!astContext().hasSameUnqualifiedType(pointee, commonBase->getType()))
      return emitError(loc) << "unsupported: returned pointer value (the "
                               "returned global does not match the "
                               "pointee type)";
    globalReturnBases[canonical] = commonBase;
    pointerReturnKinds.try_emplace(canonical, Type());
    return Type();
  }
  Type kind;
  for (const clang::ReturnStmt *ret : returns) {
    Location retLoc = translateLoc(ret->getReturnLoc());
    const clang::Expr *value = ret->getRetValue();
    const clang::Expr *fnExpr = value ? returnedFunctionExpr(value) : nullptr;
    if (!fnExpr)
      return emitError(retLoc)
             << "unsupported: returned pointer value (only a returned "
                "function address has a representation; a cursor into a "
                "callee-local region would dangle)";
    const auto *fn = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    FailureOr<Type> mapped =
        mapType(astContext().getPointerType(fn->getType()), retLoc);
    if (failed(mapped))
      return failure();
    if (kind && kind != *mapped)
      return emitError(retLoc)
             << "unsupported: return sites disagree on the returned "
                "function pointer signature";
    kind = *mapped;
  }
  if (!kind)
    return emitError(loc) << "unsupported: pointer return type";
  pointerReturnKinds.try_emplace(canonical, kind);
  return kind;
}

FailureOr<const clang::VarDecl *>
CImporter::classifyFnPtrPointerResult(const clang::FunctionType *fnType,
                                      Location loc) {
  const clang::Type *key = astContext()
                               .getCanonicalType(clang::QualType(fnType, 0))
                               .getTypePtr();
  if (const clang::VarDecl *cached = fnPtrReturnBases.lookup(key))
    return cached;
  // The candidate set must be whole-program: in a multi-TU project another
  // TU could take a diverging function's address after this TU classified.
  if (!currentSoleTU)
    return emitError(loc) << "unsupported: function pointer result type";
  if (!fnPtrReturnInProgress.insert(key).second)
    return emitError(loc) << "unsupported: function pointer result type";
  auto eraseInProgress = [&]() { fnPtrReturnInProgress.erase(key); };
  clang::QualType returnType = fnType->getReturnType().getCanonicalType();
  SmallVector<const clang::FunctionDecl *, 4> candidates;
  for (const clang::FunctionDecl *fn : addressTakenFunctions)
    if (astContext().hasSameUnqualifiedType(
            fn->getReturnType().getCanonicalType(), returnType))
      candidates.push_back(fn);
  if (candidates.empty()) {
    eraseInProgress();
    return emitError(loc) << "unsupported: function pointer result type";
  }
  const clang::VarDecl *common = nullptr;
  for (const clang::FunctionDecl *fn : candidates) {
    FailureOr<Type> kind = classifyPointerReturn(fn, loc);
    if (failed(kind)) {
      eraseInProgress();
      return failure();
    }
    const clang::VarDecl *base =
        globalReturnBases.lookup(fn->getCanonicalDecl());
    if (*kind || !base) {
      eraseInProgress();
      return emitError(loc) << "unsupported: function pointer result type";
    }
    if (common && base != common) {
      eraseInProgress();
      return emitError(loc) << "unsupported: return sites disagree on the "
                               "returned global base";
    }
    common = base;
  }
  eraseInProgress();
  fnPtrReturnBases[key] = common;
  return common;
}

const clang::VarDecl *
CImporter::erasedGlobalReturnCallBase(const clang::Expr *expr,
                                      const clang::CallExpr **callOut) const {
  const auto *call = llvm::dyn_cast<clang::CallExpr>(stripTrivia(expr));
  if (!call)
    return nullptr;
  if (callOut)
    *callOut = call;
  if (const clang::FunctionDecl *callee = call->getDirectCallee())
    return globalReturnBases.lookup(callee->getCanonicalDecl());
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParenImpCasts();
  clang::QualType calleeType = calleeExpr->getType().getCanonicalType();
  if (!calleeType->isFunctionPointerType())
    return nullptr;
  return fnPtrReturnBases.lookup(
      calleeType->getPointeeType().getCanonicalType().getTypePtr());
}

bool CImporter::isCarrierReturnFunction(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = carrierReturnCache.find(canonical);
  if (it != carrierReturnCache.end())
    return it->second;
  if (!isDataPointer(func->getReturnType())) {
    carrierReturnCache.try_emplace(canonical, false);
    return false;
  }
  const clang::FunctionDecl *definition = func->getDefinition();
  if (!definition || !definition->hasBody()) {
    carrierReturnCache.try_emplace(canonical, false);
    return false;
  }
  // Break recursion cycles pessimistically (a self-recursive carrier
  // return would need a fixpoint; none of the supported programs do).
  if (!carrierReturnInProgress.insert(canonical).second)
    return false;
  PointerRegionAnalysis regions;
  regions.carrierReturnQuery = [this](const clang::FunctionDecl *callee) {
    return isCarrierReturnFunction(callee);
  };
  regions.analyze(astContext(), definition->getBody());
  SmallVector<const clang::ReturnStmt *> returns;
  collectReturnStmts(definition->getBody(), returns);
  bool carrier = !returns.empty();
  for (const clang::ReturnStmt *ret : returns)
    if (!ret->getRetValue() ||
        !isCarrierReturnExpr(regions, ret->getRetValue())) {
      carrier = false;
      break;
    }
  carrierReturnInProgress.erase(canonical);
  carrierReturnCache.try_emplace(canonical, carrier);
  return carrier;
}

bool CImporter::isCarrierReturnExpr(PointerRegionAnalysis &regions,
                                    const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  if (e->isNullPointerConstant(astContext(),
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return true;
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    if (cast->getCastKind() == clang::CK_NullToPointer)
      return true;
    // Only a pointer-width integer rides as a carrier (mirroring
    // `recordPointerWrite`'s classification gate).
    if (cast->getCastKind() == clang::CK_IntegralToPointer)
      return astContext().getTypeSize(cast->getSubExpr()->getType()) == 64;
    if (cast->getCastKind() == clang::CK_NoOp)
      return isCarrierReturnExpr(regions, cast->getSubExpr());
  }
  if (const clang::VarDecl *var = asLoadedLocalVarRef(e))
    if (regions.tracks(var) && isCarrierRegion(regions.regionOf(var)))
      return true;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e))
    if (const clang::FunctionDecl *callee = call->getDirectCallee())
      return isCarrierReturnFunction(callee);
  return false;
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

bool CImporter::ordinaryNameTaken(llvm::StringRef name) const {
  if (ordinaryTuNames.contains(name))
    return true;
  Operation *existing = SymbolTable::lookupSymbolIn(module, name);
  return existing && !llvm::isa<emitrust::StructDefOp>(existing);
}

void CImporter::collectStaticLocalNames(const clang::Stmt *stmt,
                                        llvm::StringRef funcName) {
  if (!stmt)
    return;
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
        if (var->isStaticLocal())
          ordinaryTuNames.insert(
              (llvm::Twine(funcName) + "_" + var->getName()).str());
  for (const clang::Stmt *child : stmt->children())
    collectStaticLocalNames(child, funcName);
}

void CImporter::collectOrdinaryNames(const clang::TranslationUnitDecl *unit) {
  ordinaryTuNames.clear();
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      std::string funcName = mlirFuncName(func);
      ordinaryTuNames.insert(funcName);
      // Function-local statics surface at module level under their
      // `<function>_<name>` mangle (see emitLocalVar), claiming that
      // spelling in the ordinary namespace.
      if (func->hasBody())
        collectStaticLocalNames(func->getBody(), funcName);
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      bool internal = !var->isExternallyVisible();
      ordinaryTuNames.insert(internal ? currentTuTag + var->getName().str()
                                      : var->getName().str());
    }
  }
}

FailureOr<std::string>
CImporter::structSymbolName(const clang::RecordDecl *definition,
                            Location loc) {
  auto cached = assignedStructNames.find(definition);
  if (cached != assignedStructNames.end())
    return cached->second;
  llvm::StringRef base = recordRustName(definition);
  if (base.empty())
    return std::string(); // Anonymous struct; callers reject it.
  std::string assigned = base.str();
  if (ordinaryNameTaken(assigned)) {
    // C's tag namespace is separate from the ordinary one (C99 6.2.3);
    // the module symbol table is not, so the tag yields deterministically.
    assigned = ("Struct_" + base).str();
    if (ordinaryNameTaken(assigned))
      return emitError(loc)
             << "unsupported: struct '" << base
             << "' collides with an ordinary identifier, and so does its "
                "renamed spelling '"
             << assigned << "'";
  }
  assignedStructNames[definition] = assigned;
  return assigned;
}

LogicalResult CImporter::importRecord(const clang::RecordDecl *record,
                                      Location loc) {
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition)
    return success(); // Forward declaration; imported once completed or used.
  Location defLoc = translateLoc(definition->getBeginLoc());
  if (!definition->isStruct() && !definition->isUnion())
    return emitError(defLoc) << "unsupported record declaration";
  if (!importedRecords.insert(definition).second)
    return success();
  llvm::StringRef structName = recordRustName(definition);
  if (!structName.empty() && isRustKeyword(structName))
    return emitError(defLoc) << "unsupported: struct name '" << structName
                             << "' is a Rust keyword";

  SmallVector<llvm::StringRef> fieldNames;
  SmallVector<Type> fieldTypes;
  // A union flattens to its single storage slot; every other record keeps
  // the anonymous-member-resolving field walk.
  if (definition->isUnion()) {
    if (failed(collectUnionSlot(definition, fieldNames, fieldTypes)))
      return failure();
  } else if (unsigned bitFieldRuns = 0; failed(collectRecordFields(
                 definition, fieldNames, fieldTypes, bitFieldRuns))) {
    return failure();
  }
  // An empty member list (`struct T {};`, a GNU/C2x shape clang accepts) is
  // permitted and becomes a unit-like Rust struct.

  // C tag identity is (tag name, scope), and every block-scope declaration
  // of a tag introduces a new type (C99 6.2.1) — a `struct T` inside a
  // block may shadow a file-scope `struct T` with a different shape, and
  // even a same-shaped redeclaration is a distinct type. clang has already
  // resolved the scoping, so the defining decl is the identity; only the
  // emitted Rust name needs disambiguation. Block-scope records take the
  // function-local-static mangling convention `<function>_<tag>`
  // (`_<n>`-suffixed if that name is taken) and skip the name-keyed
  // cross-TU dedup below, which exists solely to merge the same file-scope
  // definition reached through a shared header in several TUs. A bare
  // anonymous record has no tag to mangle and is excluded: whatever its
  // scope, it takes the shape-keyed `Anon<n>` path below, where the
  // defining decl is already the identity and the shape is the name key.
  if (!structName.empty() &&
      !definition->getDeclContext()->getRedeclContext()->isFileContext()) {
    const clang::FunctionDecl *enclosing = nullptr;
    for (const clang::DeclContext *ctx = definition->getDeclContext();
         ctx && !enclosing; ctx = ctx->getParent())
      enclosing = llvm::dyn_cast<clang::FunctionDecl>(ctx);
    if (!enclosing)
      return emitError(defLoc)
             << "unsupported: struct definition outside file or function "
                "scope";
    std::string mangledBase =
        (llvm::Twine(mlirFuncName(enclosing)) + "_" + structName).str();
    std::string mangled = mangledBase;
    // Each probe below tries a fresh suffix, so the loop takes at most one
    // step per already-emitted struct name — bounded and deterministic.
    for (unsigned suffix = 2; emittedStructNames.contains(mangled); ++suffix)
      mangled = (llvm::Twine(mangledBase) + "_" + llvm::Twine(suffix)).str();
    emittedStructNames.insert(mangled);
    llvm::StringRef mangledRef =
        localRecordNames.try_emplace(definition, std::move(mangled))
            .first->second;
    structDefRecords[mangledRef] = definition;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::StructDefOp>(
        defLoc, moduleBuilder.getStringAttr(mangledRef),
        moduleBuilder.getStrArrayAttr(fieldNames),
        moduleBuilder.getTypeArrayAttr(fieldTypes));
    return success();
  }

  // Cross-TU deduplication (file-scope records only): the same struct
  // reached through a shared header has distinct decls in each TU. Dedup by
  // symbol name; an identical shape is skipped, a name reused with a
  // different field shape is a diagnostic.
  std::string shape;
  {
    llvm::raw_string_ostream os(shape);
    for (auto [fieldName, fieldType] : llvm::zip(fieldNames, fieldTypes))
      os << fieldName << ':' << fieldType << ';';
  }

  // File-scope named records go through the tag-versus-ordinary-namespace
  // collision renaming (C99 6.2.3): `struct a` and a global or function
  // `a` may coexist in C, so the tag is renamed to `Struct_<tag>` exactly
  // when the ordinary namespace claims the spelling (see
  // `structSymbolName`, which caches the decision per defining decl for
  // `emittedRecordName`). The renamed spelling is what the shape dedup and
  // the emitted struct_def below use.
  std::string assignedStorage;
  if (!structName.empty()) {
    FailureOr<std::string> assigned = structSymbolName(definition, defLoc);
    if (failed(assigned))
      return failure();
    assignedStorage = std::move(*assigned);
    structName = assignedStorage;
  }

  // A bare anonymous struct (no tag, no typedef name) gets a synthesized
  // `Anon<n>` name that is a deterministic function of its field shape:
  // the shape is the key, so the same anonymous shape anywhere in the
  // project reuses one name (and, through the shape dedup below, one
  // struct_def), while distinct shapes always get distinct names. The
  // counter only orders first encounters; it never influences which name a
  // given shape maps to within an import. `anonRecordShapeNames` is
  // consulted only for anonymous structs, so a named struct with the same
  // shape keeps its own Rust type.
  if (structName.empty()) {
    auto known = anonRecordShapeNames.find(shape);
    if (known != anonRecordShapeNames.end()) {
      structName = known->second;
    } else {
      std::string synthesized;
      do {
        synthesized = ("Anon" + llvm::Twine(anonStructCounter++)).str();
      } while (importedRecordShapes.contains(synthesized) ||
               importedEnumShapes.contains(synthesized) ||
               ordinaryNameTaken(synthesized));
      structName =
          anonRecordShapeNames.try_emplace(shape, synthesized).first->second;
    }
    anonRecordNames.try_emplace(definition, structName.str());
  }

  auto existingShape = importedRecordShapes.find(structName);
  if (existingShape != importedRecordShapes.end()) {
    if (existingShape->second != shape)
      return emitError(defLoc)
             << "unsupported: conflicting definition of struct '" << structName
             << "' with a different shape in another translation unit";
    return success();
  }
  // A file-scope tag that lands on a name already claimed by a mangled
  // block-scope record cannot be merged (they are different C types) and
  // cannot share the symbol; reject with a located diagnostic, mirroring
  // createGlobal's collision policy for mangled local statics.
  if (emittedStructNames.contains(structName))
    return emitError(defLoc)
           << "unsupported: struct name '" << structName
           << "' collides with the mangled name of a block-scope struct";
  importedRecordShapes[structName] = shape;
  emittedStructNames.insert(structName);
  structDefRecords[structName] = definition;

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::StructDefOp>(
      defLoc, moduleBuilder.getStringAttr(structName),
      moduleBuilder.getStrArrayAttr(fieldNames),
      moduleBuilder.getTypeArrayAttr(fieldTypes));
  return success();
}

LogicalResult CImporter::collectRecordFields(
    const clang::RecordDecl *record,
    SmallVectorImpl<llvm::StringRef> &fieldNames,
    SmallVectorImpl<Type> &fieldTypes, unsigned &bitFieldRuns) {
  // Interns a synthesized or mangled member spelling in the arena so the
  // StringRef stored in the field list (and in `bitFieldAccessInfo`)
  // stays valid for the import's lifetime.
  auto internName = [&](std::string name) -> llvm::StringRef {
    return memberNameArena.emplace_back(std::move(name));
  };
  // Appends one field under the collision guard: C member names are
  // unique within a record, so a duplicate FINAL spelling can only come
  // from keyword mangling (`type` next to `type_`) or from a member
  // spelled like a synthesized `__bits<n>` backing field.
  auto appendField = [&](llvm::StringRef finalName, Type type, Location loc,
                         llvm::StringRef original) -> LogicalResult {
    if (llvm::is_contained(fieldNames, finalName))
      return emitError(loc)
             << "unsupported: struct member '" << original
             << "' maps to the Rust field name '" << finalName
             << "', which collides with another member";
    fieldNames.push_back(finalName);
    fieldTypes.push_back(type);
    return success();
  };
  SmallVector<const clang::FieldDecl *> fields;
  for (const clang::FieldDecl *field : record->fields())
    fields.push_back(field);
  for (unsigned index = 0, count = fields.size(); index != count; ++index) {
    const clang::FieldDecl *field = fields[index];
    Location fieldLoc = translateLoc(field->getLocation());
    if (field->isBitField()) {
      // C99-45: pack the maximal run of consecutively declared bit-field
      // members, LSB-first in declaration order, into one backing field.
      uint64_t totalBits = 0;
      unsigned runEnd = index;
      for (; runEnd != count && fields[runEnd]->isBitField(); ++runEnd) {
        const clang::FieldDecl *bitField = fields[runEnd];
        Location bitFieldLoc = translateLoc(bitField->getLocation());
        // A zero-width bit-field is a pure ABI padding directive; the
        // backing-run layout is deliberately NOT ABI-compatible, so
        // honoring it would promise a layout this model does not keep.
        if (bitField->isZeroLengthBitField())
          return emitError(bitFieldLoc)
                 << "unsupported: zero-width bit-field member";
        // An anonymous bit-field is likewise padding-only.
        if (bitField->isUnnamedBitField() || bitField->getName().empty())
          return emitError(bitFieldLoc)
                 << "unsupported: anonymous bit-field member";
        totalBits += bitField->getBitWidthValue();
      }
      if (totalBits > 64)
        return emitError(fieldLoc)
               << "unsupported: bit-field run wider than 64 bits";
      unsigned backingWidth =
          totalBits <= 8 ? 8 : totalBits <= 16 ? 16 : totalBits <= 32 ? 32
                                                                      : 64;
      auto backingType = IntegerType::get(builder.getContext(), backingWidth,
                                          IntegerType::Unsigned);
      llvm::StringRef backingName =
          internName(("__bits" + llvm::Twine(bitFieldRuns++)).str());
      unsigned offset = 0;
      for (unsigned i = index; i != runEnd; ++i) {
        const clang::FieldDecl *bitField = fields[i];
        bitFieldAccessInfo[bitField] =
            BitFieldAccess{backingName, backingType, offset,
                           bitField->getBitWidthValue()};
        offset += bitField->getBitWidthValue();
      }
      if (failed(appendField(backingName, backingType, fieldLoc, backingName)))
        return failure();
      index = runEnd - 1; // The loop increment lands on the run's end.
      continue;
    }
    if (field->isAnonymousStructOrUnion()) {
      // C11 6.7.2.1p13: the members of an anonymous struct/union member
      // are considered members of the containing structure. Recursion
      // depth is the member nesting depth of the source, so it is bounded
      // by the program text.
      const clang::RecordDecl *member =
          field->getType()->getAsRecordDecl()->getDefinition();
      if (member->isStruct()) {
        // Sema has already enforced that the injected spellings are
        // unique in the parent's member namespace (a collision is a
        // clang "member of anonymous struct redeclares" error), so the
        // fields keep their own names. The run counter threads through so
        // backing names stay unique across the whole flattened record.
        if (failed(collectRecordFields(member, fieldNames, fieldTypes,
                                       bitFieldRuns)))
          return failure();
        continue;
      }
      // Anonymous union member: modeled without a union type exactly
      // when every arm flattens to a single leaf field and all leaves
      // map to one identical type. The arms then alias one storage slot
      // named after the first leaf, which is exact — reading any union
      // member with the type of the last store yields that stored value
      // (same object representation, same type). Any other shape stays
      // in CTS-R3 territory and rejects exactly as union types do
      // elsewhere.
      Location unionLoc = translateLoc(member->getBeginLoc());
      const clang::FieldDecl *storage = nullptr;
      Type slotType;
      for (const clang::FieldDecl *arm : member->fields()) {
        FailureOr<const clang::FieldDecl *> leaf =
            anonymousUnionArmLeaf(arm, unionLoc);
        if (failed(leaf))
          return failure();
        Location leafLoc = translateLoc((*leaf)->getLocation());
        FailureOr<Type> leafType = mapType((*leaf)->getType(), leafLoc);
        if (failed(leafType))
          return failure();
        if (!storage) {
          storage = *leaf;
          slotType = *leafType;
          if (failed(appendField(
                  internName(mangleMemberName(storage->getName())), slotType,
                  leafLoc, storage->getName())))
            return failure();
          continue;
        }
        if (*leafType != slotType)
          return emitError(unionLoc) << "unsupported: union type";
        unionSlotStorage[*leaf] = storage;
      }
      if (!storage) // An empty anonymous union has no representable slot.
        return emitError(unionLoc) << "unsupported: union type";
      continue;
    }
    if (field->getName().empty())
      return emitError(fieldLoc) << "unsupported: unnamed struct member";
    // A FILE* member of a MAIN-FILE record would store an owned handle
    // inside an aggregate, which the function-local handle model does not
    // cover (C99-48); the check must precede the data-pointer cursor
    // mapping in mapStructFieldType. System-header records (the stdio
    // stream record's own FILE*-typed links) keep the historical cursor
    // field so their import stays inert.
    if (isFilePtrType(field->getType()) && !isSystemHeaderDecl(field))
      return emitError(fieldLoc)
             << "unsupported: FILE* is only supported as a function-local "
                "variable";
    FailureOr<Type> fieldType = mapStructFieldType(field->getType(), fieldLoc);
    if (failed(fieldType))
      return failure();
    if (failed(appendField(internName(mangleMemberName(field->getName())),
                           *fieldType, fieldLoc, field->getName())))
      return failure();
  }
  return success();
}

FailureOr<const clang::FieldDecl *>
CImporter::anonymousUnionArmLeaf(const clang::FieldDecl *arm,
                                 Location unionLoc) {
  Location armLoc = translateLoc(arm->getLocation());
  // A bit-field arm is not addressable storage the one-slot aliasing can
  // model; same policy (and wording) as `collectUnionSlot`'s.
  if (arm->isBitField())
    return emitError(armLoc) << "unsupported: union with a bit-field arm";
  if (!arm->isAnonymousStructOrUnion()) {
    if (arm->getName().empty())
      return emitError(armLoc) << "unsupported: unnamed struct member";
    return arm;
  }
  // A nested anonymous struct arm contributes exactly its own flattened
  // fields; single-slot aliasing admits it only when that is one leaf.
  // (A nested anonymous union arm with several arms of its own is
  // conservatively rejected the same way.)
  const clang::RecordDecl *record =
      arm->getType()->getAsRecordDecl()->getDefinition();
  const clang::FieldDecl *leaf = nullptr;
  for (const clang::FieldDecl *inner : record->fields()) {
    if (leaf) // A second field: the arm is wider than one slot.
      return emitError(unionLoc) << "unsupported: union type";
    FailureOr<const clang::FieldDecl *> innerLeaf =
        anonymousUnionArmLeaf(inner, unionLoc);
    if (failed(innerLeaf))
      return failure();
    leaf = *innerLeaf;
  }
  if (!leaf) // An empty arm has no slot to alias.
    return emitError(unionLoc) << "unsupported: union type";
  return leaf;
}

LogicalResult CImporter::collectUnionSlot(
    const clang::RecordDecl *definition,
    SmallVectorImpl<llvm::StringRef> &fieldNames,
    SmallVectorImpl<Type> &fieldTypes) {
  Location unionLoc = translateLoc(definition->getBeginLoc());
  // Structural arm checks, and slot selection: the slot is the first arm,
  // EXCEPT in the byte-array mix (CTS-F, 00210) — a union pairing a
  // non-array arm with constant integer-array arms takes the first
  // NON-ARRAY arm as its slot regardless of declaration order (the array
  // arms are width-checked type-level aliases whose accesses reject at
  // the access site).
  auto integerArrayArm =
      [&](const clang::FieldDecl *arm) -> const clang::ConstantArrayType * {
    const clang::ConstantArrayType *array =
        astContext().getAsConstantArrayType(arm->getType());
    return array && array->getElementType()->isIntegerType() ? array
                                                             : nullptr;
  };
  const clang::FieldDecl *storage = nullptr;
  bool hasIntegerArrayArm = false;
  const clang::FieldDecl *firstNonArrayArm = nullptr;
  for (const clang::FieldDecl *arm : definition->fields()) {
    // A bit-field arm is not addressable storage the slot can alias.
    if (arm->isBitField())
      return emitError(unionLoc) << "unsupported: union with a bit-field arm";
    if (arm->isAnonymousStructOrUnion() || arm->getName().empty())
      return emitError(unionLoc) << "unsupported: union with an unnamed arm";
    // A pointer arm never aliases another slot under the decomposed
    // pointer model, whatever its width; checked before `mapType` so the
    // rejection is the union's, not the pointer's.
    if (isPointerType(arm->getType()))
      return emitError(unionLoc) << "unsupported: union with a pointer arm";
    if (!storage)
      storage = arm; // Declaration order: the first arm.
    if (integerArrayArm(arm))
      hasIntegerArrayArm = true;
    else if (!firstNonArrayArm)
      firstNonArrayArm = arm;
  }
  // An empty union (a GNU extension clang accepts in C) has no first arm
  // and therefore no representable slot.
  if (!storage)
    return emitError(unionLoc) << "unsupported: union with no members";
  if (hasIntegerArrayArm && firstNonArrayArm)
    storage = firstNonArrayArm;

  Location slotLoc = translateLoc(storage->getLocation());
  FailureOr<Type> slotMapped = mapType(storage->getType(), slotLoc);
  if (failed(slotMapped))
    return failure();
  Type slotType = *slotMapped;
  // A keyword-spelled slot name mangles exactly like a struct member's
  // (`mangleMemberName`); the arena keeps the spelling alive for the
  // struct_def attribute below.
  fieldNames.push_back(
      memberNameArena.emplace_back(mangleMemberName(storage->getName())));
  fieldTypes.push_back(slotType);

  auto slotInt = llvm::dyn_cast<IntegerType>(slotType);
  for (const clang::FieldDecl *arm : definition->fields()) {
    if (arm == storage)
      continue;
    // A constant integer-array arm whose total width equals the integer
    // slot's admits as a TYPE-level alias (CTS-F, 00210): its spelling
    // never reaches the IR, and any access through it rejects at the
    // access site (`unsupported: union byte-array arm access`). An
    // unequal-total-width array arm falls through to the family
    // rejections below.
    if (integerArrayArm(arm) && slotInt &&
        astContext().getTypeSize(arm->getType()) == slotInt.getWidth()) {
      unionByteArrayArms.insert(arm);
      continue;
    }
    Location armLoc = translateLoc(arm->getLocation());
    FailureOr<Type> armType = mapType(arm->getType(), armLoc);
    if (failed(armType))
      return failure();
    // An identical mapped type aliases the slot exactly: reading any
    // union member with the type of the last store yields that value.
    if (*armType == slotType) {
      unionSlotStorage[arm] = storage;
      continue;
    }
    auto armInt = llvm::dyn_cast<IntegerType>(*armType);
    // Same-width integers differing only in signedness alias bit-exactly
    // on two's complement (C99 6.5.2.3 union punning); accesses through
    // this arm wrap an `emitrust.cast` reinterpretation.
    if (slotInt && armInt && slotInt.getWidth() == armInt.getWidth()) {
      unionSlotStorage[arm] = storage;
      continue;
    }
    if (slotInt && armInt)
      return emitError(unionLoc)
             << "unsupported: union arms of differing sizes";
    return emitError(unionLoc)
           << "unsupported: union type mixing non-integer arms";
  }
  return success();
}

const clang::FieldDecl *
CImporter::flattenedFieldStorage(const clang::FieldDecl *field) const {
  auto storage = unionSlotStorage.find(field);
  return storage == unionSlotStorage.end() ? field : storage->second;
}

std::string
CImporter::flattenedFieldName(const clang::FieldDecl *field) const {
  return mangleMemberName(flattenedFieldStorage(field)->getName());
}

FailureOr<Value> CImporter::reinterpretUnionArmRead(const clang::Expr *expr,
                                                    Value value,
                                                    Location loc) {
  const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripTrivia(expr));
  if (!member)
    return value;
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field || flattenedFieldStorage(field) == field)
    return value;
  FailureOr<Type> armType = mapType(field->getType(), loc);
  if (failed(armType))
    return failure();
  if (*armType == value.getType())
    return value;
  return builder.create<emitrust::CastOp>(loc, *armType, value).getResult();
}

FailureOr<Value> CImporter::reinterpretUnionArmWrite(const clang::Expr *expr,
                                                     Value value,
                                                     Location loc) {
  const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripTrivia(expr));
  if (!member)
    return value;
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field)
    return value;
  const clang::FieldDecl *storage = flattenedFieldStorage(field);
  if (storage == field)
    return value;
  FailureOr<Type> slotType = mapType(storage->getType(), loc);
  if (failed(slotType))
    return failure();
  if (*slotType == value.getType())
    return value;
  return builder.create<emitrust::CastOp>(loc, *slotType, value).getResult();
}

std::string
CImporter::emittedRecordName(const clang::RecordDecl *definition) const {
  auto local = localRecordNames.find(definition);
  if (local != localRecordNames.end())
    return local->second;
  auto assigned = assignedStructNames.find(definition);
  if (assigned != assignedStructNames.end())
    return assigned->second;
  llvm::StringRef name = recordRustName(definition);
  if (!name.empty())
    return name.str();
  auto anon = anonRecordNames.find(definition);
  if (anon == anonRecordNames.end())
    return {};
  return anon->second;
}

LogicalResult CImporter::importEnum(const clang::EnumDecl *enumDecl,
                                    Location loc) {
  const clang::EnumDecl *definition = enumDecl->getDefinition();
  if (!definition)
    return success(); // Incomplete; imported once completed or used.
  if (definition->getName().empty())
    return success(); // Anonymous; enumerators import as i32 at use sites.
  if (!importedEnums.insert(definition).second)
    return success();
  Location defLoc = translateLoc(definition->getBeginLoc());
  if (isRustKeyword(definition->getName()))
    return emitError(defLoc) << "unsupported: enum name '"
                             << definition->getName()
                             << "' is a Rust keyword";

  SmallVector<llvm::StringRef> variantNames;
  SmallVector<int64_t> variantValues;
  llvm::SmallDenseSet<int64_t> seenValues;
  for (const clang::EnumConstantDecl *enumerator : definition->enumerators()) {
    Location enumeratorLoc = translateLoc(enumerator->getLocation());
    llvm::StringRef name = enumerator->getName();
    if (isRustKeyword(name))
      return emitError(enumeratorLoc)
             << "unsupported: enumerator '" << name << "' is a Rust keyword";
    const llvm::APSInt &initValue = enumerator->getInitVal();
    if (!initValue.isRepresentableByInt64())
      return emitError(enumeratorLoc)
             << "unsupported: enumerator value does not fit in i32";
    int64_t value = initValue.getExtValue();
    if (value < INT32_MIN || value > INT32_MAX)
      return emitError(enumeratorLoc)
             << "unsupported: enumerator value does not fit in i32";
    if (!seenValues.insert(value).second)
      return emitError(enumeratorLoc)
             << "unsupported: duplicate enumerator value";
    variantNames.push_back(name);
    variantValues.push_back(value);
  }
  if (variantNames.empty())
    return emitError(defLoc) << "unsupported: enum with no enumerators";

  // The storage of the emitted open enum follows clang's underlying type
  // choice (unsigned when every enumerator is non-negative), so that the
  // raw-representation place (`emitrust.enum_raw`) and enum/integer
  // conversions meet C's unsigned semantics at the right type.
  bool unsignedUnderlying =
      definition->getIntegerType()->isUnsignedIntegerType();

  // Cross-TU deduplication by symbol name (see importRecord): identical shape
  // is skipped, a name reused with a different variant shape is a diagnostic.
  // The underlying signedness is derived from the values, so the value list
  // determines it and the shape key needs no extra component.
  std::string shape;
  {
    llvm::raw_string_ostream os(shape);
    for (auto [variantName, variantValue] :
         llvm::zip(variantNames, variantValues))
      os << variantName << '=' << variantValue << ';';
  }
  auto existingShape = importedEnumShapes.find(definition->getName());
  if (existingShape != importedEnumShapes.end()) {
    if (existingShape->second != shape)
      return emitError(defLoc)
             << "unsupported: conflicting definition of enum '"
             << definition->getName()
             << "' with a different shape in another translation unit";
    return success();
  }
  importedEnumShapes[definition->getName()] = shape;

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::EnumDefOp>(
      defLoc, moduleBuilder.getStringAttr(definition->getName()),
      moduleBuilder.getStrArrayAttr(variantNames),
      moduleBuilder.getDenseI64ArrayAttr(variantValues), unsignedUnderlying);
  return success();
}

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
  if (!cType.isNull() && isDataPointer(cType)) {
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
    return Attribute(FloatAttr::get(floatType, value.getFloat()));
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
    // anonymous union member's slot, bit-exact across a signedness pun.
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
    return convertAPValueInit(value.getUnionValue(), slotType,
                              active->getType(), loc);
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
  // Internal-linkage (`static`) functions are mangled with the per-TU tag so
  // identically named file-statics in different TUs never collide. The tag is
  // empty for a single-TU import, preserving the historical bare name.
  if (func->getStorageClass() == clang::SC_Static)
    return currentTuTag + cName.str();
  return cName.str();
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
      // effect-free trailing extras in `emitCall`. A body that does use
      // va_list keeps the rejection: the trailing arguments have no
      // decomposed representation.
      if (bodyUsesVaList(astContext(), definition->getBody()))
        return emitError(loc) << "unsupported: variadic function definition";
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
  if (isRustKeyword(cName))
    return emitError(loc) << "unsupported: function name '" << cName
                          << "' is a Rust keyword";
  std::string name = mlirFuncName(func);

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

  for (auto [index, param] : llvm::enumerate(func->parameters())) {
    // main's `argv` was dropped from the imported signature (it has no
    // entry-block argument); its uses were rejected at signature time, so
    // no binding is needed.
    if (currentIsMain && isPointerType(param->getType()))
      continue;
    Location paramLoc = translateLoc(param->getLocation());
    Value blockArg =
        entryBlock->getArgument(methodOwner ? index + 1 : index);
    Type type = blockArg.getType();
    // An integer-carrier `void *` parameter (CTS-P3) is a plain i64
    // scalar; remember it so truth tests and carrier reads route to its
    // prologue cell (bound through the ordinary scalar path below).
    if (!methodOwner && isDataPointer(param->getType()) &&
        type == builder.getIntegerType(64))
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
    if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(type)) {
      if (auto sliceType =
              llvm::dyn_cast<emitrust::SliceType>(mutRef.getPointee())) {
        // Slice parameter (Phase 1b): one entry-block dereference
        // establishes the region base place, and the parameter itself
        // decomposes into (base, i64 cursor = 0) exactly like a decayed
        // local array; every element access renders `(*param)[i as usize]`
        // so no borrow is ever held across statements.
        Value basePlace =
            builder
                .create<emitrust::DerefOp>(
                    paramLoc, emitrust::LValueType::get(sliceType), blockArg)
                .getResult();
        Value cursorCell =
            createEntryAlloca(paramLoc, builder.getIntegerType(64));
        Value zero =
            createIntConstant(paramLoc, builder.getIntegerType(64), 0);
        builder.create<memref::StoreOp>(paramLoc, zero, cursorCell);
        symbols[param] = basePlace;
        pointerLocals[param] = PointerLocalInfo{param, cursorCell};
        continue;
      }
    }
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(type)) {
      // Scalar-reference pointer parameter: used directly as a reference
      // SSA value.
      symbols[param] = blockArg;
      continue;
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
      continue;
    }
    if (llvm::isa<emitrust::ArrayType>(type))
      return emitError(paramLoc) << "unsupported: array parameter";
    // Plain scalar: promotable rank-0 memref cell (swept by
    // `finalizeFunction` when the parameter is never read).
    Value cell = createEntryAlloca(paramLoc, type);
    builder.create<memref::StoreOp>(paramLoc, blockArg, cell);
    symbols[param] = cell;
    paramCells.push_back(cell);
  }

  if (failed(emitStmt(func->getBody())))
    return failure();
  return finalizeFunction(funcOp, loc);
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
      builder.create<func::ReturnOp>(loc, zero);
      continue;
    }
    if (auto floatType = llvm::dyn_cast<FloatType>(currentReturnType)) {
      Value zero = builder
                       .create<arith::ConstantOp>(
                           loc, FloatAttr::get(floatType, 0.0))
                       .getResult();
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
        // `= {...}` lists and `char s[] = "..."` string initializers are
        // supported; a whole-aggregate copy initializer stays rejected.
        if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
                init->IgnoreParenImpCasts()))
          return emitStringArrayInit(place, *mlirType, literal);
        const auto *list = llvm::dyn_cast<clang::InitListExpr>(init);
        if (!list)
          return emitError(loc) << "unsupported: aggregate initializer";
        return emitAggregateInitList(place, *mlirType, list, var);
      }
      FailureOr<Value> value = emitRValue(init);
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
    if (llvm::isa<clang::ImplicitValueInitExpr>(element))
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
  FailureOr<Type> fieldType = mapType(field->getType(), elementLoc);
  if (failed(fieldType))
    return failure();
  Value fieldPlace = builder
                         .create<emitrust::MemberOp>(
                             elementLoc, emitrust::LValueType::get(*fieldType),
                             place,
                             builder.getStringAttr(flattenedFieldName(field)))
                         .getResult();
  return emitInitListElement(fieldPlace, *fieldType, element);
}

LogicalResult CImporter::emitInitListElement(Value place, Type type,
                                             const clang::Expr *element) {
  if (const auto *nested = llvm::dyn_cast<clang::InitListExpr>(element))
    return emitAggregateInitList(place, type, nested);
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
  // An ordinary literal fills a byte (i8) array; a wide literal fills a
  // `wchar_t` (i32 on the supported targets) array, one code unit per
  // element. u8/u/U literals have no mapped element representation.
  if (!literal->isOrdinary() && !literal->isWide())
    return emitError(loc) << "unsupported: non-ordinary string literal "
                             "initializer";
  Type expectedElement =
      builder.getIntegerType(literal->isOrdinary() ? 8 : 32);
  if (!arrayType || arrayType.getElementType() != expectedElement)
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
    Value value = createIntConstant(loc, arrayType.getElementType(),
                                    static_cast<int64_t>(byte));
    if (failed(storeToPlace(loc, elementPlace, value)))
      return failure();
  }
  return success();
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
    if (!wildcard &&
        !astContext().hasSameUnqualifiedType(
            pointee, base->getType().getCanonicalType()->getPointeeType()))
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
    builder.create<func::ReturnOp>(loc, *value);
  } else {
    if (currentReturnType)
      return emitError(loc)
             << "unsupported: return without a value in a non-void function";
    builder.create<func::ReturnOp>(loc);
  }
  // Continue in a fresh block; if it stays unreachable it is erased later.
  builder.setInsertionPointToEnd(createBlock());
  return success();
}

LogicalResult CImporter::emitExprStmt(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
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
    // A data-pointer struct member holds a statically resolved degenerate
    // binding; the write validates against it and emits nothing (CTS-P2).
    if (dataPointerFieldOf(op->getLHS()))
      return emitMemberPointerAssign(
          llvm::cast<clang::MemberExpr>(stripTrivia(op->getLHS())),
          op->getRHS(), loc);
    return emitError(loc)
           << "unsupported: assignment to this pointer expression";
  }
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
  FailureOr<Value> value = emitRValue(op->getRHS());
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
  FailureOr<Value> result = buildCompoundAssignValue(loc, op, current);
  if (failed(result))
    return failure();
  if (failed(commitGlobalWriteback(
          loc, writeback, assignStalenessRisk(op),
          [&]() { return storeToPlace(loc, *place, *result); })))
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
  auto intType = llvm::dyn_cast<IntegerType>(current.getType());
  if (!intType)
    return emitError(loc) << "unsupported: ++/-- on a non-integer operand";
  Value one = createScalarIntConstant(loc, intType, 1);
  clang::BinaryOperatorKind opcode =
      op->isIncrementOp() ? clang::BO_Add : clang::BO_Sub;
  FailureOr<Value> next = buildBinaryArith(loc, opcode, current, one);
  if (failed(next))
    return failure();
  // The subexpression's own side effects (a subscript-index call,
  // `g[f()]++`) run after the staging load and force the pre-store
  // refresh of the staged copy.
  if (failed(commitGlobalWriteback(
          loc, writeback, op->getSubExpr()->HasSideEffects(astContext()),
          [&]() { return storeToPlace(loc, *place, *next); })))
    return failure();
  // C evaluates postfix forms to the original value and prefix forms to
  // the updated one.
  return op->isPostfix() ? current : *next;
}

LogicalResult CImporter::emitCallStmt(const clang::CallExpr *call) {
  const clang::FunctionDecl *callee = call->getDirectCallee();
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
        return emitMemcpyCall(call);
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
    // Parse `%[flags][width][.precision][length]conv` (C99 7.19.6.1).
    // Supported flags are '-' (left align) and '0' (zero pad); width is a
    // decimal number; precision stays rejected; the only supported length
    // is a single 'l'.
    bool leftAlign = false;
    bool zeroPad = false;
    while (i < n && (format[i] == '-' || format[i] == '0')) {
      if (format[i] == '-')
        leftAlign = true;
      else
        zeroPad = true;
      ++i;
    }
    std::string width;
    while (i < n && format[i] >= '0' && format[i] <= '9')
      width += format[i++];
    if (i < n && format[i] == '.')
      return emitError(loc)
             << "unsupported: precision in printf format specifier";
    bool isLong = false;
    if (i < n && format[i] == 'l') {
      isLong = true;
      ++i;
      if (i < n && format[i] == 'l')
        return emitError(loc) << "unsupported printf length modifier 'll'";
    } else if (i < n && (format[i] == 'h' || format[i] == 'L' ||
                         format[i] == 'j' || format[i] == 'z' ||
                         format[i] == 't')) {
      return emitError(loc) << "unsupported printf length modifier '"
                            << llvm::Twine(std::string(1, format[i])) << "'";
    }
    if (i >= n)
      return emitError(loc) << "unsupported: trailing '%' in printf format";
    char spec = format[i];
    // Validate the conversion before consuming an argument so an unknown
    // conversion is always the diagnostic, even when arguments are short.
    if (spec != 'd' && spec != 'i' && spec != 'u' && spec != 'x' &&
        spec != 'X' && spec != 'o' && spec != 'c' && spec != 's' &&
        spec != 'f')
      return emitError(loc) << "unsupported printf format specifier '%"
                            << llvm::Twine(std::string(1, spec)) << "'";
    bool hasAdjustment = leftAlign || zeroPad || !width.empty();
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
    if (argIndex >= call->getNumArgs())
      return emitError(loc) << "unsupported: too few arguments to printf";
    const clang::Expr *argExpr = call->getArg(argIndex);
    unsigned argNumber = argIndex++;

    if (spec == 's') {
      if (hasAdjustment || isLong)
        return emitError(loc)
               << "unsupported: flags, width, or length on printf '%s'";
      FailureOr<Value> text = emitPrintfStringArg(argExpr);
      if (failed(text))
        return failure();
      operands.push_back(*text);
      rustFormat += "{}";
      continue;
    }

    FailureOr<Value> argument = emitRValue(argExpr);
    if (failed(argument))
      return failure();
    Type argType = (*argument).getType();

    if (spec == 'f') {
      // C's %f prints six decimals; Rust's {:.6} matches it for every
      // finite value and for infinities, but spells NaN as "NaN" where C
      // prints "nan"/"-nan". The argument is therefore routed through the
      // module-level `__emitrust_fmt_f64` helper (emitted once, on demand)
      // and printed with a plain `{}`. (`%lf` is identical to `%f` in
      // C99; flags and width on the String-typed helper result would not
      // match C's numeric padding and stay rejected.)
      if (hasAdjustment)
        return emitError(loc) << "unsupported: flags or width on printf '%f'";
      if (!llvm::isa<Float64Type>(argType))
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      needsFloatFormatHelper = true;
      auto stringType =
          emitrust::OpaqueType::get(builder.getContext(), "String");
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

    auto argIntType = llvm::dyn_cast<IntegerType>(argType);
    bool isIntArgument = argIntType && argIntType.getWidth() > 1;

    if (spec == 'c') {
      // C converts the argument to unsigned char and prints that byte;
      // the i32 argument (chars arrive int-promoted) goes through the
      // `__emitrust_fmt_c` helper (ASCII-only, see design.md C99-48).
      if (hasAdjustment || isLong)
        return emitError(loc)
               << "unsupported: flags, width, or length on printf '%c'";
      if (!isIntArgument)
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      operands.push_back(wrapCharFormat(loc, *argument));
      rustFormat += "{}";
      continue;
    }

    // Integer conversions. d/i print signed; u/x/X/o print the value as
    // unsigned, so the argument is `as`-cast to the unsigned type of the
    // directive's width — a negative signed argument then prints its
    // two's-complement bit pattern ("%x" of -1 is ffffffff), exactly like
    // C. An argument of a different width is `as`-cast as well, which
    // truncates to the low bits just like C's varargs read on x86-64
    // (printf("%d", sizeof(x)) prints the low 32 bits of the size_t).
    llvm::StringRef radix;
    bool isSigned;
    switch (spec) {
    case 'd':
    case 'i':
      isSigned = true;
      break;
    case 'u':
      isSigned = false;
      break;
    case 'x':
      isSigned = false;
      radix = "x";
      break;
    case 'X':
      isSigned = false;
      radix = "X";
      break;
    case 'o':
      isSigned = false;
      radix = "o";
      break;
    default: // Defensive; the conversion was validated above.
      return emitError(loc) << "unsupported printf format specifier '%"
                            << llvm::Twine(std::string(1, spec)) << "'";
    }
    if (!isIntArgument)
      return emitError(loc) << "unsupported: printf argument " << argNumber
                            << " does not match its format specifier";
    IntegerType target =
        isSigned ? builder.getIntegerType(isLong ? 64 : 32)
                 : IntegerType::get(builder.getContext(), isLong ? 64 : 32,
                                    IntegerType::Unsigned);
    operands.push_back(castToIntType(loc, *argument, target));
    rustFormat += placeholderFor(radix);
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

FailureOr<Value> CImporter::emitPrintfStringArg(const clang::Expr *expr) {
  // The array-to-pointer decay wrapping both supported shapes is implicit;
  // strip it (and parentheses) to see the underlying literal or lvalue.
  const clang::Expr *arg = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(arg->getBeginLoc());
  if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(arg)) {
    if (!literal->isOrdinary())
      return emitError(loc)
             << "unsupported: non-ordinary string literal in printf '%s'";
    // The literal's decoded bytes become a Rust string literal emitted
    // verbatim into the generated source: an embedded NUL would diverge
    // from C (which stops printing there) and a non-ASCII byte would fail
    // rustc's UTF-8 check, so both are rejected; quote, backslash, and the
    // whitespace escapes are re-escaped for the Rust spelling.
    std::string text = "\"";
    for (char c : literal->getString()) {
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
    needsCStrHelper = true;
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    return builder
        .create<emitrust::CallOpaqueOp>(
            loc, TypeRange{stringType},
            builder.getStringAttr("__emitrust_cstr"),
            /*args=*/ArrayAttr(), ValueRange{slice})
        .getResult(0);
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
    needsCStrHelper = true;
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    return builder
        .create<emitrust::CallOpaqueOp>(
            loc, TypeRange{stringType},
            builder.getStringAttr("__emitrust_cstr"),
            /*args=*/ArrayAttr(), ValueRange{*slice})
        .getResult(0);
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
      needsCStrHelper = true;
      auto stringType =
          emitrust::OpaqueType::get(builder.getContext(), "String");
      return builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{stringType},
              builder.getStringAttr("__emitrust_cstr"),
              /*args=*/ArrayAttr(), ValueRange{*slice})
          .getResult(0);
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
    needsCStrHelper = true;
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    return builder
        .create<emitrust::CallOpaqueOp>(
            loc, TypeRange{stringType},
            builder.getStringAttr("__emitrust_cstr"),
            /*args=*/ArrayAttr(), ValueRange{slice})
        .getResult(0);
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
  // A decayed string literal argument creates (or reuses) the literal's
  // read-only backing; unlike a literal bound to a pointer variable, this
  // shape may appear with no pointer region referring to the literal.
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
      if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
              stripTrivia(cast->getSubExpr()))) {
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

LogicalResult CImporter::emitMemcpyCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc)
           << "unsupported: memcpy requires exactly 3 arguments";
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
    return emitError(loc) << "unsupported: memcpy count type";
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
  return llvm::StringSwitch<std::optional<llvm::StringRef>>(name)
      .Case("sin", "f64::sin")
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

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

FailureOr<Value> CImporter::emitRValue(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  // Constant contexts (case values, enumerator initializers) are wrapped in
  // ConstantExpr; translate the wrapped expression.
  if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(e))
    return emitRValue(constant->getSubExpr());
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
    return builder
        .create<arith::ConstantOp>(loc,
                                   FloatAttr::get(*type, literal->getValue()))
        .getResult();
  }
  if (const auto *literal = llvm::dyn_cast<clang::CharacterLiteral>(e)) {
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    return createIntConstant(loc, *type,
                             static_cast<int64_t>(literal->getValue()));
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
  if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(e))
    return emitSizeofAlignof(trait);
  if (llvm::isa<clang::StringLiteral>(e))
    return emitError(loc)
           << "unsupported: string literal outside a printf format";
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
    return loadPlace(loc, *place);
  }
  if (opcode == clang::BO_Assign) {
    FailureOr<Value> place = emitAssignToPlace(op);
    if (failed(place))
      return failure();
    return loadPlace(loc, *place);
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
        name == "memset" || name == "memcpy")
      return emitError(loc) << "unsupported: " << name
                            << " return value must be unused";
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
  // A variadic callee is only supported when its definition imports as
  // its fixed prototype (a va_list-free body, see `importFunction`); the
  // call then drops its trailing extras below. Every other variadic call
  // keeps the rejection.
  if (callee->isVariadic()) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || !definition->hasBody() ||
        bodyUsesVaList(astContext(), definition->getBody()))
      return emitError(loc) << "unsupported: call to a variadic function";
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

  std::string name = mlirFuncName(callee);
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

  FunctionType targetType = target.getFunctionType();
  unsigned namedArgCount = call->getNumArgs();
  if (callee->isVariadic()) {
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
    FailureOr<Value> value = emitRValue(argument);
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

FailureOr<Value> CImporter::emitBorrowArgument(Location loc,
                                               const clang::Expr *argument,
                                               Type paramType,
                                               const clang::VarDecl *&root) {
  root = nullptr;
  auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(paramType);
  if (!mutRef) // Parameters are always mut_ref today; ref is defensive.
    return emitError(loc) << "unsupported reference parameter type";
  Type pointee = mutRef.getPointee();

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
                                           /*is_mut=*/true)
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
    // here).
    if (pointer->base && !pointer->base->hasLocalStorage())
      return rejectGlobalPointerArgument(loc, pointer->base);
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
                                     pointer->cursor, /*is_mut=*/true)
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

  // A call through a prototype-less K&R pointer (`int (*f)()`) has no
  // signature to check its arguments against; the zero-argument form is
  // the only one that can be verified.
  const clang::Type *pointee = calleeExpr->getType()
                                   .getCanonicalType()
                                   ->getPointeeType()
                                   .getTypePtr();
  if (llvm::isa<clang::FunctionNoProtoType>(pointee) && call->getNumArgs() > 0)
    return emitError(loc)
           << "unsupported: call with arguments through a function pointer "
              "without a prototype";

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

  // Argument checking mirrors the direct-call path.
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
      // literal is its read-only backing array at cursor 0 (the backing
      // was created at the declaration of the pointer bound to it); a
      // decayed row of a multi-dimensional array (`arr[i]` in `arr[i][j]`
      // or `q = arr[i]`) decomposes the subscript into the base's flat
      // cursor.
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(sub)) {
        Value backing = literalBackings.lookup(literal);
        if (!backing)
          return emitError(loc) << "unsupported: pointer to a string literal";
        return PtrExprValue{nullptr, createIntConstant(loc, cursorType, 0),
                            backing};
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

  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    return emitDeclRefLValue(ref, loc, writeback);
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
    return emitMemberLValue(member, loc, writeback);
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    return emitSubscriptLValue(subscript, loc, writeback);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref)
      return emitDerefLValue(unary, loc, writeback);

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
  // A byte-array union arm (CTS-F, 00210) exists only at the type level:
  // the union admitted on its integer slot, but no access through the
  // array arm can be modeled on that one slot.
  if (unionByteArrayArms.contains(field))
    return emitError(loc) << "unsupported: union byte-array arm access";
  // A data-pointer member has no place of its own (its stored i64
  // carries no information); reads resolve through the static binding
  // in `emitPointerRValue` and writes through `emitMemberPointerAssign`.
  if (isDataPointer(field->getType()))
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
  // slot's name at the slot's type. A differently-signed arm's
  // bit-exact reinterpretation happens at the load or store site (see
  // `reinterpretUnionArmRead`/`reinterpretUnionArmWrite`).
  FailureOr<Type> fieldType =
      mapType(flattenedFieldStorage(field)->getType(), loc);
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
  FailureOr<Value> basePlace = emitLValue(base, writeback);
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
