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
///    the supported targets). Pointers whose address is taken, that rebind
///    across distinct objects, hold a null constant, or point into globals
///    or string literals are rejected with located diagnostics.
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
///    located rejections.
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
///    Anonymous enums contribute plain `i32` constants only.
///  - Variables with static storage duration (file-scope variables and
///    function-local statics, the latter mangled `<function>_<name>`)
///    become module-level `emitrust.global`s with constant-evaluated
///    initializers. Whole-value reads and writes use
///    `emitrust.global_load`/`emitrust.global_store`; element and field
///    accesses stage the whole value in a local copy and store it back
///    after a mutation, which is exact for the single-threaded subset.
///    Taking the address of a global is rejected until globals get a
///    pointer model.
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
#include "mlir/IR/Verifier.h"

#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
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

/// A pending store-back of a staged global copy. Element and field accesses
/// of a global stage its whole value in a local `emitrust.variable`; write
/// contexts store the modified copy back into the global afterwards
/// (load-modify-store, exact for the single-threaded C subset).
struct GlobalWriteback {
  /// The staging place holding the copy; null when no global was staged.
  Value place;
  /// The symbol of the global to store the copy back into.
  std::string symbol;
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

/// A pointer expression decomposed into its statically resolved base object
/// and an i64 element cursor value. `cursor` is null for a degenerate base
/// (the address of a scalar or struct object taken with `&x`), which
/// supports dereference but carries no element offset and hence no pointer
/// arithmetic.
struct PtrExprValue {
  /// The object the pointer points into (a local scalar, struct, or array).
  const clang::VarDecl *base;
  /// The i64 element offset from the start of `base`; null when degenerate.
  Value cursor;
};

/// Phase-1b classification of one pointer parameter, derived from the
/// function definition's body. `ScalarRef` parameters are only dereferenced
/// (`*p`) or arrowed (`p->f`), or are unused, and stay plain
/// `!emitrust.mut_ref<T>` references (the historical behavior). `Slice`
/// parameters are subscripted, walked, compared, differenced, reassigned,
/// copied into a pointer local, or passed onward, and become
/// `!emitrust.mut_ref<!emitrust.slice<T>>` region bases.
enum class ParamKind { ScalarRef, Slice };

/// The decomposition record of one accepted pointer local (or, in Phase 1b,
/// one slice-classified pointer parameter): the single base object of its
/// region and this pointer's rank-0 `memref<i64>` cursor cell.
/// The cell is null when the base is degenerate (a scalar object with no
/// element offset to track); such a pointer needs no runtime state at all.
/// A slice parameter is its own base, with its cursor initialized to zero.
struct PointerLocalInfo {
  /// The object every value of this pointer points into.
  const clang::VarDecl *base;
  /// Entry-block `memref<i64>` cell holding the element cursor, or null.
  Value cursorCell;
};

/// One base binding of a pointer region: the object some pointer in the
/// region was made to point into, and the source location of the assignment
/// (or initializer) that bound it. The multi-base diagnostic names the
/// first two bindings.
struct PointerBaseBinding {
  /// The bound object.
  const clang::VarDecl *base;
  /// Where the binding was established.
  clang::SourceLocation loc;
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
/// objects its pointers are bound to, whether any pointer arithmetic occurs
/// (which a degenerate scalar base cannot support), and the first construct
/// (if any) that puts the region outside the decomposition (escape, null
/// constant, non-address source, global target, ...).
struct PointerRegion {
  /// Distinct base objects, each with its first binding location.
  SmallVector<PointerBaseBinding, 2> bases;
  /// True when any pointer in the region is walked (`p+n`, `++`, `+=`).
  bool hasArithmetic = false;
  /// First pointer-arithmetic site; meaningful only with `hasArithmetic`.
  clang::SourceLocation arithmeticLoc;
  /// First invalidating construct; meaningful only with `invalidReason`.
  clang::SourceLocation invalidLoc;
  /// Diagnostic text of the invalidating construct; empty when the region
  /// is decomposable.
  std::string invalidReason;
};

/// Steensgaard-style union-find pre-pass that groups the pointer locals of
/// one function body into ownership regions (Phase 1a of pointer support).
/// One AST walk (in the style of `collectAddressTaken`) unions pointers on
/// assignment (`p = q`), binds base objects from `&x`, `&arr[i]`,
/// array-to-pointer decay, and slice-classified pointer parameters (Phase
/// 1b: `p = param` makes the parameter the region base), flags pointer
/// arithmetic, and records the first construct that makes a region
/// undecomposable (taking a pointer's address, null constants, non-address
/// sources, global or string-literal targets). Base objects participate in
/// the union-find alongside the pointers so that two pointers into the same
/// object always share a region. The importer validates each pointer local
/// against its region at the declaration; a region is consumable only when
/// it is single-base and never invalidated. The per-region output is the
/// deliberate seam for the later owner-struct codegen phases.
class PointerRegionAnalysis {
public:
  /// Analyzes `body`, replacing any previous analysis state. `context` is
  /// borrowed for the duration of the walk (null-constant classification).
  void analyze(clang::ASTContext &astContext, const clang::Stmt *body);

  /// Returns whether `var` is a pointer local tracked by this analysis.
  bool tracks(const clang::VarDecl *var) const {
    return pointerVars.contains(var);
  }

  /// Returns the region of the tracked pointer `var`, or null when the
  /// pointer was never bound, unioned, or invalidated (an unused pointer).
  const PointerRegion *regionOf(const clang::VarDecl *var);

  /// Returns every pointer-typed local variable the analysis tracks; the
  /// Phase-4 owner-planning pre-pass iterates these to project each
  /// per-function region into the interprocedural union-find.
  const llvm::SmallPtrSetImpl<const clang::VarDecl *> &trackedVars() const {
    return pointerVars;
  }

private:
  /// Recursive statement walk collecting pointer declarations, writes,
  /// arithmetic, escapes, and call-argument uses.
  void visit(const clang::Stmt *stmt);

  /// Classifies the right-hand side `rhs` of `ptr = rhs` (or of `ptr`'s
  /// initializer): unions pointer-to-pointer copies, binds bases from
  /// address expressions, flags arithmetic on `p +- n` forms, and marks the
  /// region invalid for everything else.
  void recordPointerWrite(const clang::VarDecl *ptr, const clang::Expr *rhs);

  /// Binds `base` into `ptr`'s region at `loc` (rejecting global-storage
  /// bases) and unions the two declarations.
  void addBase(const clang::VarDecl *ptr, const clang::VarDecl *base,
               clang::SourceLocation loc);

  /// Flags `ptr`'s region as performing pointer arithmetic at `loc`.
  void recordArithmetic(const clang::VarDecl *ptr, clang::SourceLocation loc);

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
  /// unit into the module: complete named struct definitions, function
  /// declarations or definitions, and file-scope variables (as module-level
  /// `emitrust.global`s). Other declarations are rejected, with one
  /// exception: declarations whose expansion location lies in a system
  /// header are skipped entirely (never imported, never rejected here);
  /// any main-file use of one is rejected at the use site instead.
  ///
  /// `tuTag` is prepended to internal-linkage (`static`) symbol names so that
  /// identically named file-statics in different translation units stay
  /// distinct; it is empty for a single-TU import (bare names, historical
  /// behavior). `deferExtern` controls whether an `extern`-only global with no
  /// definition in this TU is an immediate error (single-file) or deferred for
  /// cross-TU resolution (project). `soleTranslationUnit` states that this TU
  /// is the whole program, which lets the Phase-4 owner planning promote
  /// externally visible functions to methods (all their call sites are
  /// provably in this TU); in a multi-TU project only internal-linkage
  /// functions qualify. Repeated calls accumulate into one module.
  LogicalResult importTranslationUnit(clang::ASTContext &context,
                                      llvm::StringRef tuTag, bool deferExtern,
                                      bool soleTranslationUnit);

  /// After every translation unit has been imported, checks that no external
  /// symbol was left unresolved: every deferred `extern` global must have a
  /// definition, and no non-variadic external function may remain body-less
  /// (the Rust emitter cannot emit a body-less function). Emits located
  /// diagnostics otherwise.
  LogicalResult finalizeProject();

private:
  //===--------------------------------------------------------------------===//
  // Locations and types
  //===--------------------------------------------------------------------===//

  /// Converts a clang source location to an MLIR `FileLineColLoc` using the
  /// presumed (user-visible) location; unknown on invalid input.
  Location translateLoc(clang::SourceLocation sourceLoc);

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
  /// double->f64, `struct S`->`!emitrust.struct<"S">`,
  /// `T[N]`->`!emitrust.array<NxT>`, complete named
  /// `enum E`->`!emitrust.enum<"E">`, and function pointers
  /// `R (*)(A, B)`->`!emitrust.fn_ptr<(A, B) -> R>` (prototype-less K&R
  /// pointers map to the zero-parameter form). Typedefs resolve through the
  /// canonical type. Data pointers, unions, anonymous enums, variadic
  /// function pointers, fn_ptr component types outside the verifier set,
  /// and everything else produce a located diagnostic.
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

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Imports a complete named struct definition as a module-level
  /// `emitrust.struct_def`. Forward declarations are ignored; repeated
  /// imports of the same definition are deduplicated. An empty member list
  /// (`struct T {};`) imports as a field-less struct_def. Unions, anonymous
  /// structs, bit-fields, and unsupported field types are rejected.
  LogicalResult importRecord(const clang::RecordDecl *record, Location loc);

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
  /// as printf's) are skipped; variadic definitions are rejected. A body
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
  /// (single local base, no escapes, no arithmetic on a scalar base),
  /// registers its decomposition: an entry-block `memref<i64>` cursor cell
  /// for an array base, or no runtime state at all for a degenerate scalar
  /// or struct base. Undecomposable regions produce located diagnostics at
  /// the offending construct.
  LogicalResult emitPointerLocal(const clang::VarDecl *var, Location loc);

  /// Emits `ptr = rhs` for a decomposed pointer local by recomputing and
  /// storing its cursor; a degenerate binding (`p = &x`) needs no code at
  /// all because the target place is statically known.
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
  /// `getAnyInitializer`, the initializer; a variable that is only ever
  /// `extern`-declared in this TU is rejected. Thread-locals are rejected.
  LogicalResult importGlobalVar(const clang::VarDecl *var);

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
  /// one entry per array element or struct field for aggregates. Array
  /// holes left by partial or designated initialization take the array
  /// filler (C99 zero-fill); struct field types resolve through the
  /// module-level `emitrust.struct_def`. Anything else (enum-typed
  /// elements, pointers) is rejected with a located diagnostic.
  FailureOr<Attribute> convertAPValueInit(const clang::APValue &value,
                                          Type type, Location loc);

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

  /// Stores a staged global copy back into its global, if `writeback`
  /// captured one; no-op otherwise.
  void flushGlobalWriteback(Location loc, const GlobalWriteback &writeback);

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
  /// rejected.
  LogicalResult emitLocalVar(const clang::VarDecl *var);

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
  /// diagnostics.
  LogicalResult emitAggregateInitList(Value place, Type type,
                                      const clang::InitListExpr *list);

  /// Emits one element of an aggregate initializer list into `place` of
  /// value type `type`: recurses for a nested list, otherwise stores the
  /// element rvalue.
  LogicalResult emitInitListElement(Value place, Type type,
                                    const clang::Expr *element);

  /// Emits a block-scope `char s[N] = "..."` initializer as per-element
  /// byte assigns including the trailing NUL (when it fits, per C99
  /// 6.7.8p14); elements beyond the literal keep the place's default zero
  /// value. Only signless-i8 (plain/signed char) arrays are supported, and
  /// non-ASCII bytes are rejected with located diagnostics so the array's
  /// contents stay printable through the ASCII-only `%s`/`%c` helpers.
  LogicalResult emitStringArrayInit(Value place, Type type,
                                    const clang::StringLiteral *literal);

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

  /// Emits `switch` as a `cf.switch` over one block per top-level label
  /// position of the compound body plus an exit block. Consecutive labels
  /// share a block; a label section that does not end in a terminator falls
  /// through to the next section with a `cf.br`; `break` targets the exit
  /// block while `continue` still targets the enclosing loop. Labels nested
  /// inside sub-statements (Duff's device), GNU case ranges, non-compound
  /// bodies, and statements before the first label are rejected.
  LogicalResult emitSwitchStmt(const clang::SwitchStmt *stmt);

  /// Emits `return`, then continues in a fresh (dead) block so trailing
  /// statements still have an insertion point.
  LogicalResult emitReturnStmt(const clang::ReturnStmt *stmt);

  /// Emits an expression evaluated for its side effects only: assignments,
  /// compound assignments, ++/--, and calls (including printf).
  LogicalResult emitExprStmt(const clang::Expr *expr);

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

  /// Lowers a `%s` printf argument. Two shapes are supported: a string
  /// literal (after array-to-pointer decay), lowered to an
  /// `emitrust.literal` holding a `&'static str` (printable-ASCII bytes
  /// plus \n/\t/\r only; embedded NUL and non-ASCII bytes are rejected);
  /// and a char-array lvalue, lowered to an `emitrust.slice_of` of the
  /// whole array passed through the `__emitrust_cstr` helper, which stops
  /// at the first NUL like C. A `char *` variable bound to a literal stays
  /// rejected.
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
  /// type or the operator is rejected.
  FailureOr<Value>
  emitConditionalOperator(const clang::ConditionalOperator *op);

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
  /// printf reaching this path (i.e. with its result used) is rejected.
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
  /// base with an i64 cursor, `p +- n` is cursor arithmetic, and the
  /// `++`/`--` value forms update the cursor cell and yield the pre- or
  /// post-value per C semantics. Null pointer constants, string literals,
  /// globals, and every other pointer source are located rejections.
  FailureOr<PtrExprValue> emitPointerRValue(const clang::Expr *expr);

  /// Materializes the place a decomposed pointer designates: the base
  /// object's own place for a degenerate pointer, or
  /// `emitrust.subscript(base, cursor)` for a pointer into an array.
  FailureOr<Value> emitPointerPlace(Location loc,
                                    const PtrExprValue &pointer);

  /// Emits `p - q` on two decomposed pointers into the same object as the
  /// plain i64 cursor difference (C's ptrdiff_t is `long`, i.e. i64, on
  /// the supported targets); pointers into different objects are rejected.
  FailureOr<Value> emitPointerDifference(const clang::BinaryOperator *op);

  /// Returns whether the pointer-typed expression `expr` is handled by the
  /// Phase-1a decomposition (pointer locals, address-of, decay, pointer
  /// arithmetic) rather than by the untouched pointer-parameter reference
  /// path (a direct read of a `mut_ref`/`ref`-typed parameter).
  bool isDecomposedPointerExpr(const clang::Expr *expr) const;

  /// Emits an expression as an assignable place: either a rank-0 memref
  /// value (scalar locals) or an `!emitrust.lvalue` value (aggregates,
  /// dereferences, fields, elements). A reference to an imported global
  /// stages the global's whole value in a local copy; when `writeback` is
  /// non-null (write context) it captures the pending store-back of that
  /// copy, which the caller must flush with `flushGlobalWriteback` after
  /// the mutation.
  FailureOr<Value> emitLValue(const clang::Expr *expr,
                              GlobalWriteback *writeback = nullptr);

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
  /// Shape of every imported struct, keyed by symbol name, for cross-TU
  /// deduplication and mismatch detection.
  llvm::StringMap<std::string> importedRecordShapes;
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
  /// Per-function set of locals whose address is taken.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> addressTaken;
  /// Per-function pointer region analysis (Phase-1a decomposition).
  PointerRegionAnalysis pointerRegions;
  /// Per-function decomposition of each accepted pointer local and each
  /// slice-classified pointer parameter, keyed by its declaration.
  llvm::DenseMap<const clang::VarDecl *, PointerLocalInfo> pointerLocals;
  /// Cached Phase-1b parameter classifications, keyed by the function's
  /// canonical declaration (persists across the whole import; each TU's
  /// declarations are distinct clang decls, so entries never conflict).
  llvm::DenseMap<const clang::FunctionDecl *, SmallVector<ParamKind, 4>>
      paramKindsCache;
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
  /// Stack of break/continue targets for nested loops and switches.
  SmallVector<LoopTargets> loopStack;
  /// Blocks started by C labels in the function under construction, keyed
  /// by label declaration; created lazily on first mention so forward and
  /// backward `goto`s share one map.
  llvm::DenseMap<const clang::LabelDecl *, Block *> labelBlocks;
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
/// Used to reject Duff's-device-style switches whose labels are not at the
/// top level of the switch body.
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

/// Returns true if `type` is an MLIR unsigned integer type (the mapping of
/// the C unsigned integer types; signless types model the signed ones).
static bool isUnsignedInt(Type type) {
  auto intType = llvm::dyn_cast<IntegerType>(type);
  return intType && intType.isUnsigned();
}

/// Returns the Rust-facing name of a record: its tag name, or, for a tagless
/// record declared through `typedef struct { ... } T;`, the typedef name.
/// Returns an empty StringRef for a bare anonymous struct, which stays
/// rejected. The typedef name is the record's name for all mangling and
/// cross-TU shape-dedup purposes, exactly like a tagged struct.
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
  PointerRegion &target = regions[rootA];
  for (const PointerBaseBinding &binding : absorbed.bases) {
    bool known = llvm::any_of(target.bases,
                              [&](const PointerBaseBinding &existing) {
                                return existing.base == binding.base;
                              });
    if (!known)
      target.bases.push_back(binding);
  }
  if (absorbed.hasArithmetic && !target.hasArithmetic) {
    target.hasArithmetic = true;
    target.arithmeticLoc = absorbed.arithmeticLoc;
  }
  if (!absorbed.invalidReason.empty() && target.invalidReason.empty()) {
    target.invalidReason = std::move(absorbed.invalidReason);
    target.invalidLoc = absorbed.invalidLoc;
  }
}

PointerRegion &PointerRegionAnalysis::regionFor(const clang::VarDecl *decl) {
  return regions[findRoot(decl)];
}

void PointerRegionAnalysis::addBase(const clang::VarDecl *ptr,
                                    const clang::VarDecl *base,
                                    clang::SourceLocation loc) {
  if (!base->hasLocalStorage())
    return markInvalid(ptr, loc, "unsupported: pointer into a global variable");
  unite(ptr, base);
  PointerRegion &region = regionFor(ptr);
  bool known = llvm::any_of(region.bases,
                            [&](const PointerBaseBinding &existing) {
                              return existing.base == base;
                            });
  if (!known)
    region.bases.push_back(PointerBaseBinding{base, loc});
}

void PointerRegionAnalysis::recordArithmetic(const clang::VarDecl *ptr,
                                             clang::SourceLocation loc) {
  PointerRegion &region = regionFor(ptr);
  if (!region.hasArithmetic) {
    region.hasArithmetic = true;
    region.arithmeticLoc = loc;
  }
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

void PointerRegionAnalysis::recordPointerWrite(const clang::VarDecl *ptr,
                                               const clang::Expr *rhs) {
  const clang::Expr *e = stripTrivia(rhs);
  clang::SourceLocation loc = e->getBeginLoc();

  if (e->isNullPointerConstant(*context,
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return markInvalid(
        ptr, loc,
        "unsupported: null pointer constant assigned to a pointer variable");

  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    switch (cast->getCastKind()) {
    case clang::CK_NoOp:
      return recordPointerWrite(ptr, cast->getSubExpr());
    case clang::CK_LValueToRValue: {
      // `p = q`: copying a pointer joins the two into one region.
      if (const clang::VarDecl *source = asLocalVarRef(cast->getSubExpr()))
        if (tracks(source))
          return unite(ptr, source);
      // `p = param`: a pointer parameter (slice-classified by this very
      // use) becomes the region base; the local walks the parameter's
      // element run through its own cursor.
      if (const clang::ParmVarDecl *param =
              asPointerParamRef(cast->getSubExpr()))
        return addBase(ptr, param, loc);
      break;
    }
    case clang::CK_ArrayToPointerDecay: {
      // `p = arr`: the decayed array is the region base.
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (llvm::isa<clang::StringLiteral>(sub))
        return markInvalid(ptr, loc,
                           "unsupported: pointer to a string literal");
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub))
        if (const auto *array =
                llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          return addBase(ptr, array, loc);
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
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(sub)) {
        // `p = &arr[i]` binds the array; `p = &q[i]` is `p = q + i`; and
        // `p = &param[i]` binds a slice-classified pointer parameter.
        const clang::Expr *base =
            subscript->getBase()->IgnoreParenImpCasts();
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
    // the analysis never tracks them.
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
        if (var->hasLocalStorage() && !llvm::isa<clang::ParmVarDecl>(var) &&
            isPointerType(var->getType()) &&
            !isFunctionPointer(var->getType())) {
          pointerVars.insert(var);
          if (const clang::Expr *init = var->getInit())
            recordPointerWrite(var, init);
        }
  } else if (const auto *compound =
                 llvm::dyn_cast<clang::CompoundAssignOperator>(stmt)) {
    // `p += n` / `p -= n` walk the pointer without rebinding it.
    if (isPointerType(compound->getLHS()->getType()))
      if (const clang::VarDecl *var = asLocalVarRef(compound->getLHS()))
        if (tracks(var))
          recordArithmetic(var, compound->getOperatorLoc());
  } else if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
    if (binary->getOpcode() == clang::BO_Assign &&
        isPointerType(binary->getLHS()->getType()))
      if (const clang::VarDecl *var = asLocalVarRef(binary->getLHS()))
        if (tracks(var))
          recordPointerWrite(var, binary->getRHS());
  } else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
    if (unary->isIncrementDecrementOp() &&
        isPointerType(unary->getSubExpr()->getType())) {
      if (const clang::VarDecl *var = asLocalVarRef(unary->getSubExpr()))
        if (tracks(var))
          recordArithmetic(var, unary->getOperatorLoc());
    } else if (unary->getOpcode() == clang::UO_AddrOf) {
      // `&p` would let the pointer escape the decomposition.
      if (const clang::VarDecl *var = asLocalVarRef(unary->getSubExpr()))
        if (tracks(var))
          markInvalid(var, unary->getOperatorLoc(),
                      "unsupported: taking the address of a pointer variable");
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
    if (decl->isUnion())
      return emitError(loc) << "unsupported: union type";
    const clang::RecordDecl *definition = decl->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete struct type";
    llvm::StringRef structName = recordRustName(definition);
    if (structName.empty())
      return emitError(loc) << "unsupported: anonymous struct type";
    if (failed(importRecord(definition, loc)))
      return failure();
    return Type(emitrust::StructType::get(builder.getContext(), structName));
  }

  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(canonical)) {
    FailureOr<Type> element = mapType(array->getElementType(), loc);
    if (failed(element))
      return failure();
    if (llvm::isa<emitrust::ArrayType>(*element))
      return emitError(loc) << "unsupported: multi-dimensional array";
    // Arrays of function pointers are out of the v1 fn_ptr scope; reject
    // loudly instead of building an !emitrust.array the dialect does not
    // admit.
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
    if (definition->getName().empty())
      return emitError(loc) << "unsupported: anonymous enum type";
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
      FailureOr<Type> mapped = mapType(returnType, loc);
      if (failed(mapped))
        return failure();
      if (!emitrust::FnPtrType::isValidComponentType(*mapped))
        return emitError(loc) << "unsupported: function pointer result type";
      results.push_back(*mapped);
    }
    return Type(
        emitrust::FnPtrType::get(builder.getContext(), inputs, results));
  }

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
  clang::QualType canonical = type.getCanonicalType();
  // A function-pointer parameter is an ordinary Copy value, not a
  // reference; it maps to `!emitrust.fn_ptr` like every other position.
  if (canonical->isFunctionPointerType())
    return mapType(type, loc);
  if (canonical->isPointerType()) {
    clang::QualType pointee = canonical->getPointeeType();
    if (pointee.getCanonicalType()->isPointerType())
      return emitError(loc) << "unsupported: pointer-to-pointer parameter";
    FailureOr<Type> inner = mapType(pointee, loc);
    if (failed(inner))
      return failure();
    if (kind == ParamKind::Slice) {
      // A slice element must be sized and scalar/struct (the array element
      // rules); a pointer to an array (`int (*)[N]`) has no slice shape.
      if (!emitrust::ArrayType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: slice parameter element type " << *inner;
      return Type(
          emitrust::MutRefType::get(emitrust::SliceType::get(*inner)));
    }
    return Type(emitrust::MutRefType::get(*inner));
  }
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
    for (auto [index, param] : llvm::enumerate(definition->parameters()))
      if (sliceParams.contains(param))
        kinds[index] = ParamKind::Slice;
  }
  auto [entry, inserted] =
      paramKindsCache.try_emplace(canonical, std::move(kinds));
  (void)inserted;
  return entry->second;
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
      if (region && region->invalidReason.empty() &&
          region->bases.size() == 1)
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

void CImporter::planOwners(const clang::TranslationUnitDecl *unit,
                           bool soleTranslationUnit) {
  // Interprocedural union-find over storage bases (local arrays and
  // scalars) and the data-pointer parameters of function definitions.
  // Union-find transitively closes as edges are added, so one walk over
  // every body reaches the fixpoint.
  llvm::DenseMap<const clang::VarDecl *, const clang::VarDecl *> parent;
  auto find = [&](const clang::VarDecl *decl) -> const clang::VarDecl * {
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
  };
  auto unite = [&](const clang::VarDecl *a, const clang::VarDecl *b) {
    parent[find(b)] = find(a);
  };
  // Declarations whose class must not be promoted: unresolvable pointer
  // arguments, pointer-to-pointer parameters, invalidated (escaping)
  // regions. Membership is checked per node during aggregation, so a
  // poison mark survives later unions.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> poisoned;

  SmallVector<const clang::FunctionDecl *> definitions;
  for (const clang::Decl *decl : unit->decls())
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl))
      if (func->doesThisDeclarationHaveABody() && !func->isVariadic() &&
          !isSystemHeaderDecl(func))
        definitions.push_back(func);

  for (const clang::FunctionDecl *func : definitions) {
    PointerRegionAnalysis analysis;
    analysis.analyze(astContext(), func->getBody());

    // Data-pointer parameters are class nodes; a pointer-to-pointer
    // parameter poisons its class (it has no i64-index representation).
    for (const clang::ParmVarDecl *param : func->parameters()) {
      if (!isPointerType(param->getType()) ||
          isFunctionPointer(param->getType()))
        continue;
      (void)find(param);
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
          unite(first, binding.base);
        if (!region->invalidReason.empty())
          poisoned.insert(binding.base);
      }
    }

    // Call edges: a pointer argument's root object unifies with the callee
    // definition's parameter; an unresolvable root poisons the parameter's
    // class. Callees without a definition in this TU add no edge — passing
    // a region to them stays on the Phase-1b call lowering, which composes
    // with a promoted base through its rewritten data place.
    SmallVector<const clang::CallExpr *> calls;
    collectCallExprs(func->getBody(), calls);
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
        if (const clang::VarDecl *root = resolveArgRoot(analysis, argument))
          unite(root, param);
        else
          poisoned.insert(param);
      }
    }
  }

  // Aggregate the classes. `find` compresses paths in `parent`, so the
  // node set is snapshotted before aggregation.
  struct ClassInfo {
    SmallVector<const clang::VarDecl *, 2> storageBases;
    SmallVector<const clang::ParmVarDecl *, 4> params;
    bool poisoned = false;
  };
  SmallVector<const clang::VarDecl *> nodes;
  nodes.reserve(parent.size());
  for (const auto &entry : parent)
    nodes.push_back(entry.first);
  llvm::DenseMap<const clang::VarDecl *, ClassInfo> classes;
  for (const clang::VarDecl *node : nodes) {
    ClassInfo &info = classes[find(node)];
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
            find(param) != entry.first) {
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
// Declarations
//===----------------------------------------------------------------------===//

LogicalResult CImporter::importRecord(const clang::RecordDecl *record,
                                      Location loc) {
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition)
    return success(); // Forward declaration; imported once completed or used.
  Location defLoc = translateLoc(definition->getBeginLoc());
  if (definition->isUnion())
    return emitError(defLoc) << "unsupported: union type";
  if (!definition->isStruct())
    return emitError(defLoc) << "unsupported record declaration";
  if (!importedRecords.insert(definition).second)
    return success();
  llvm::StringRef structName = recordRustName(definition);
  if (structName.empty())
    return emitError(defLoc) << "unsupported: anonymous struct type";
  if (isRustKeyword(structName))
    return emitError(defLoc) << "unsupported: struct name '" << structName
                             << "' is a Rust keyword";

  SmallVector<llvm::StringRef> fieldNames;
  SmallVector<Type> fieldTypes;
  for (const clang::FieldDecl *field : definition->fields()) {
    Location fieldLoc = translateLoc(field->getLocation());
    if (field->isBitField())
      return emitError(fieldLoc) << "unsupported: bit-field struct member";
    if (field->getName().empty())
      return emitError(fieldLoc) << "unsupported: unnamed struct member";
    if (isRustKeyword(field->getName()))
      return emitError(fieldLoc) << "unsupported: struct member '"
                                 << field->getName() << "' is a Rust keyword";
    FailureOr<Type> fieldType = mapType(field->getType(), fieldLoc);
    if (failed(fieldType))
      return failure();
    fieldNames.push_back(field->getName());
    fieldTypes.push_back(*fieldType);
  }
  // An empty member list (`struct T {};`, a GNU/C2x shape clang accepts) is
  // permitted and becomes a unit-like Rust struct.

  // Cross-TU deduplication: the same struct reached through a shared header
  // has distinct decls in each TU. Dedup by symbol name; an identical shape is
  // skipped, a name reused with a different field shape is a diagnostic.
  std::string shape;
  {
    llvm::raw_string_ostream os(shape);
    for (auto [fieldName, fieldType] : llvm::zip(fieldNames, fieldTypes))
      os << fieldName << ':' << fieldType << ';';
  }
  auto existingShape = importedRecordShapes.find(structName);
  if (existingShape != importedRecordShapes.end()) {
    if (existingShape->second != shape)
      return emitError(defLoc)
             << "unsupported: conflicting definition of struct '" << structName
             << "' with a different shape in another translation unit";
    return success();
  }
  importedRecordShapes[structName] = shape;

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::StructDefOp>(
      defLoc, moduleBuilder.getStringAttr(structName),
      moduleBuilder.getStrArrayAttr(fieldNames),
      moduleBuilder.getTypeArrayAttr(fieldTypes));
  return success();
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

  // Cross-TU deduplication by symbol name (see importRecord): identical shape
  // is skipped, a name reused with a different variant shape is a diagnostic.
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
      moduleBuilder.getDenseI64ArrayAttr(variantValues));
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

LogicalResult CImporter::importGlobalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
  const clang::VarDecl *canonical = var->getCanonicalDecl();
  if (globals.contains(canonical))
    return success(); // Redeclaration of an already imported global.

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
  return createGlobal(canonical, typeDecl, symbolName, loc);
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
  if (init) {
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
            init->IgnoreParenImpCasts())) {
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
  return convertAPValueInit(*value, type, initLoc);
}

FailureOr<Attribute> CImporter::convertAPValueInit(const clang::APValue &value,
                                                   Type type, Location loc) {
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
    Type elementType = arrayType.getElementType();
    SmallVector<Attribute> elements;
    elements.reserve(arrayType.getSize());
    for (unsigned i = 0, n = value.getArrayInitializedElts(); i != n; ++i) {
      FailureOr<Attribute> element =
          convertAPValueInit(value.getArrayInitializedElt(i), elementType,
                             loc);
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
      FailureOr<Attribute> filler =
          convertAPValueInit(value.getArrayFiller(), elementType, loc);
      if (failed(filler))
        return failure();
      elements.append(arrayType.getSize() - elements.size(), *filler);
    }
    return Attribute(builder.getArrayAttr(elements));
  }
  if (auto structType = llvm::dyn_cast<emitrust::StructType>(type)) {
    if (!value.isStruct())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    auto structDef = llvm::dyn_cast_or_null<emitrust::StructDefOp>(
        SymbolTable::lookupSymbolIn(module, structType.getName()));
    if (!structDef)
      return emitError(loc)
             << "unsupported: global initializer for this type";
    ArrayAttr fieldTypes = structDef.getFieldTypes();
    if (value.getStructNumFields() != fieldTypes.size())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    SmallVector<Attribute> fields;
    fields.reserve(fieldTypes.size());
    for (auto [i, fieldType] : llvm::enumerate(fieldTypes)) {
      FailureOr<Attribute> field = convertAPValueInit(
          value.getStructField(i),
          llvm::cast<TypeAttr>(fieldType).getValue(), loc);
      if (failed(field))
        return failure();
      fields.push_back(*field);
    }
    return Attribute(builder.getArrayAttr(fields));
  }
  return emitError(loc) << "unsupported: global initializer for this type";
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

void CImporter::flushGlobalWriteback(Location loc,
                                     const GlobalWriteback &writeback) {
  if (!writeback.place)
    return;
  auto lvalueType =
      llvm::cast<emitrust::LValueType>(writeback.place.getType());
  Value full = builder
                   .create<emitrust::LoadOp>(loc, lvalueType.getValueType(),
                                             writeback.place)
                   .getResult();
  builder.create<emitrust::GlobalStoreOp>(loc, full,
                                          globalSymbol(writeback.symbol));
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
    if (func->hasBody())
      return emitError(loc) << "unsupported: variadic function definition";
    // Body-less variadic declarations (printf in particular) are skipped;
    // calls to them are handled specially or rejected at the call site.
    return success();
  }
  // Body-less puts/putchar declarations are skipped like printf's: their
  // statement-position calls are lowered by name (`emitPuts`/`emitPutchar`)
  // and never reference the symbol, and a body-less function would
  // otherwise be rejected by `finalizeProject`.
  if ((cName == "puts" || cName == "putchar") && !func->getDefinition())
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
  SmallVector<Type> resultTypes;
  clang::QualType returnType = func->getReturnType();
  if (!returnType->isVoidType()) {
    FailureOr<Type> mapped = mapType(returnType, loc);
    if (failed(mapped))
      return failure();
    resultTypes.push_back(*mapped);
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
  pointerLocals.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentReceiverPlace = Value();
  currentMethodOwner = nullptr;
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  currentFuncName = name;
  currentIsMain = name == "c_main";
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
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
    Location paramLoc = translateLoc(param->getLocation());
    Value blockArg =
        entryBlock->getArgument(methodOwner ? index + 1 : index);
    Type type = blockArg.getType();
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
    // Plain scalar: promotable rank-0 memref cell.
    Value cell = createEntryAlloca(paramLoc, type);
    builder.create<memref::StoreOp>(paramLoc, blockArg, cell);
    symbols[param] = cell;
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
  const clang::TranslationUnitDecl *unit = astContext().getTranslationUnitDecl();
  // Phase-4 Pass A: pure-AST owner planning over every function definition
  // before any IR is built; Pass B below consults the plans.
  planOwners(unit, soleTranslationUnit);
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

  // Terminate the fall-through block, if any.
  for (Block &block : region) {
    if (isTerminated(&block))
      continue;
    builder.setInsertionPointToEnd(&block);
    if (!currentReturnType) {
      builder.create<func::ReturnOp>(loc);
      continue;
    }
    if (currentIsMain) {
      // C11 5.1.2.2.3: falling off the end of main returns 0.
      Value zero = createIntConstant(loc, currentReturnType, 0);
      builder.create<func::ReturnOp>(loc, zero);
      continue;
    }
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
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt))
    return emitExprStmt(expr);
  return emitError(loc) << "unsupported statement: "
                        << stmt->getStmtClassName();
}

LogicalResult CImporter::emitLocalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
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
  clang::QualType type = var->getType().getCanonicalType();
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
        return emitAggregateInitList(place, *mlirType, list);
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
                                 const clang::InitListExpr *list) {
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
    unsigned index = 0;
    for (const clang::FieldDecl *field : record->fields()) {
      if (index >= list->getNumInits())
        break; // Remaining fields keep their default (zero) value.
      const clang::Expr *element = list->getInit(index++);
      if (llvm::isa<clang::ImplicitValueInitExpr>(element))
        continue;
      Location elementLoc = translateLoc(element->getBeginLoc());
      FailureOr<Type> fieldType = mapType(field->getType(), elementLoc);
      if (failed(fieldType))
        return failure();
      Value fieldPlace = builder
                             .create<emitrust::MemberOp>(
                                 elementLoc,
                                 emitrust::LValueType::get(*fieldType), place,
                                 builder.getStringAttr(field->getName()))
                             .getResult();
      if (failed(emitInitListElement(fieldPlace, *fieldType, element)))
        return failure();
    }
    return success();
  }
  return emitError(loc) << "unsupported: aggregate initializer";
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
  if (!arrayType || arrayType.getElementType() != builder.getIntegerType(8))
    return emitError(loc)
           << "unsupported: string literal initializer for this type";
  if (!literal->isOrdinary())
    return emitError(loc) << "unsupported: non-ordinary string literal "
                             "initializer";
  // C99 6.7.8p14: successive bytes of the literal (including the
  // terminating NUL if there is room) initialize the elements; Sema
  // guarantees the literal fits. Elements past the literal keep the
  // place's default zero value (matching C's zero fill), so only the
  // literal's bytes plus the NUL are assigned.
  uint64_t length = literal->getLength();
  uint64_t count = std::min<uint64_t>(length + 1, arrayType.getSize());
  for (uint64_t i = 0; i != count; ++i) {
    uint32_t byte = i < length ? literal->getCodeUnit(i) : 0;
    // Non-ASCII bytes are rejected so the array's contents stay exact
    // through the ASCII-only `%s`/`%c` printing helpers.
    if (byte > 127)
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

LogicalResult CImporter::emitPointerLocal(const clang::VarDecl *var,
                                          Location loc) {
  clang::QualType pointee =
      var->getType().getCanonicalType()->getPointeeType();
  if (pointee.getCanonicalType()->isPointerType())
    return emitError(loc) << "unsupported: pointer-to-pointer variable";

  const PointerRegion *region = pointerRegions.regionOf(var);
  if (!region)
    return success(); // Declared but never used as a pointer; no code.
  if (!region->invalidReason.empty())
    return emitError(translateLoc(region->invalidLoc))
           << region->invalidReason;
  if (region->bases.size() >= 2) {
    // A pointer that is rebound across distinct objects cannot decompose
    // into one (base, cursor) pair; name both objects and both bindings.
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
  }
  if (region->bases.empty())
    return success(); // Never bound; any dereference is rejected at its site.

  const PointerBaseBinding &binding = region->bases.front();
  const clang::VarDecl *base = binding.base;
  Location bindLoc = translateLoc(binding.loc);
  if (!base->hasLocalStorage()) // Defensive; the analysis flags this first.
    return emitError(bindLoc) << "unsupported: pointer into a global variable";

  Value cursorCell;
  if (isPointerType(base->getType())) {
    // The base is a slice-classified pointer parameter (the only pointer
    // that can be a region base): the local walks the parameter's element
    // run through its own cursor. The binding registered at the function
    // prologue guarantees the base place is an lvalue<slice>.
    auto baseInfo = pointerLocals.find(base);
    if (baseInfo == pointerLocals.end() || !baseInfo->second.cursorCell)
      return emitError(bindLoc)
             << "unsupported: pointer variable bound to a non-slice "
                "pointer parameter"; // Defensive; classification forbids it.
    if (!astContext().hasSameUnqualifiedType(
            pointee, base->getType().getCanonicalType()->getPointeeType()))
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target parameter";
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else if (const clang::ConstantArrayType *array =
                 astContext().getAsConstantArrayType(base->getType())) {
    if (!astContext().hasSameUnqualifiedType(pointee,
                                             array->getElementType()))
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
    if (!astContext().hasSameUnqualifiedType(pointee, base->getType()))
      return emitError(bindLoc)
             << "unsupported: pointer type does not match its target object";
  }
  pointerLocals[var] = PointerLocalInfo{base, cursorCell};
  if (const clang::Expr *init = var->getInit())
    return storePointerAssign(loc, var, init);
  return success();
}

LogicalResult CImporter::storePointerAssign(Location loc,
                                            const clang::VarDecl *ptr,
                                            const clang::Expr *rhs) {
  auto it = pointerLocals.find(ptr);
  if (it == pointerLocals.end())
    return emitError(loc) << "unsupported: assignment to pointer variable '"
                          << ptr->getName() << "' with no known target object";
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  const PointerLocalInfo &info = it->second;
  if (value->base != info.base) // Defensive; multi-base regions never get here.
    return emitError(loc)
           << "unsupported: pointer assignment would rebind to a different "
              "object";
  if (!info.cursorCell)
    return success(); // Degenerate: the target place is statically known.
  Value cursor = value->cursor
                     ? value->cursor
                     : createIntConstant(loc, builder.getIntegerType(64), 0);
  builder.create<memref::StoreOp>(loc, cursor, info.cursorCell);
  return success();
}

LogicalResult CImporter::emitPointerCompoundAssign(
    const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  if (opcode != clang::BO_Add && opcode != clang::BO_Sub)
    return emitError(loc) << "unsupported compound assignment on a pointer";
  const clang::VarDecl *var = asVarRef(op->getLHS());
  auto it = var ? pointerLocals.find(var) : pointerLocals.end();
  if (it == pointerLocals.end())
    return emitError(loc)
           << "unsupported: compound assignment to this pointer expression";
  const PointerLocalInfo &info = it->second;
  if (!info.cursorCell) // Defensive; the analysis rejects this at the decl.
    return emitError(loc)
           << "unsupported: arithmetic on the address of a scalar object";
  Value current = loadPlace(loc, info.cursorCell);
  FailureOr<Value> amount = emitRValue(op->getRHS());
  if (failed(amount))
    return failure();
  auto amountType = llvm::dyn_cast<IntegerType>((*amount).getType());
  if (!amountType)
    return emitError(loc) << "unsupported pointer offset type";
  Value offset = castToIntType(loc, *amount, builder.getIntegerType(64));
  Value next =
      opcode == clang::BO_Add
          ? builder.create<arith::AddIOp>(loc, current, offset).getResult()
          : builder.create<arith::SubIOp>(loc, current, offset).getResult();
  builder.create<memref::StoreOp>(loc, next, info.cursorCell);
  return success();
}

LogicalResult CImporter::emitIfStmt(const clang::IfStmt *stmt) {
  Location loc = translateLoc(stmt->getIfLoc());
  if (stmt->getConditionVariable() || stmt->getInit())
    return emitError(loc) << "unsupported: declaration in if condition";
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
  if (!body)
    return emitError(loc)
           << "unsupported: switch body must be a compound statement";

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
    } else if (sections.empty()) {
      return emitError(translateLoc(child->getBeginLoc()))
             << "unsupported: statement before the first case label of a "
                "switch";
    }
    if (const clang::Stmt *nested = findNestedSwitchLabel(statement))
      return emitError(translateLoc(nested->getBeginLoc()))
             << "unsupported: case label nested inside another statement";
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

LogicalResult CImporter::emitReturnStmt(const clang::ReturnStmt *stmt) {
  Location loc = translateLoc(stmt->getReturnLoc());
  if (const clang::Expr *retValue = stmt->getRetValue()) {
    if (!currentReturnType)
      return emitError(loc)
             << "unsupported: return with a value in a void function";
    FailureOr<Value> value = emitRValue(retValue);
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
  // Any other expression statement is evaluated and its value discarded.
  return success(succeeded(emitRValue(e)));
}

LogicalResult CImporter::emitAssign(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // Rebinding a decomposed pointer local (or a slice-classified pointer
  // parameter) recomputes its cursor; no pointer value is ever
  // materialized. Function pointers are ordinary values and take the
  // plain place-assignment (or global-store) path below.
  if (isPointerType(op->getLHS()->getType()) &&
      !isFunctionPointer(op->getLHS()->getType())) {
    if (const clang::VarDecl *var = asVarRef(op->getLHS()))
      if (pointerLocals.contains(var) || pointerRegions.tracks(var))
        return storePointerAssign(loc, var, op->getRHS());
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
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  FailureOr<Value> value = emitRValue(op->getRHS());
  if (failed(value))
    return failure();
  if (failed(storeToPlace(loc, *place, *value)))
    return failure();
  flushGlobalWriteback(loc, writeback);
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
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  FailureOr<Value> result = buildCompoundAssignValue(loc, op, current);
  if (failed(result))
    return failure();
  if (failed(storeToPlace(loc, *place, *result)))
    return failure();
  flushGlobalWriteback(loc, writeback);
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
  if (failed(storeToPlace(loc, *place, *next)))
    return failure();
  flushGlobalWriteback(loc, writeback);
  // C evaluates postfix forms to the original value and prefix forms to
  // the updated one.
  return op->isPostfix() ? current : *next;
}

LogicalResult CImporter::emitCallStmt(const clang::CallExpr *call) {
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (callee && callee->getDeclName().isIdentifier()) {
    llvm::StringRef name = callee->getName();
    if (name == "printf")
      return emitPrintf(call);
    // puts/putchar are intercepted by name only when the project supplies
    // no definition of its own (mirroring the printf by-name lowering); a
    // user-defined puts/putchar is an ordinary call.
    if (name == "puts" && !callee->getDefinition())
      return emitPuts(call);
    if (name == "putchar" && !callee->getDefinition())
      return emitPutchar(call);
  }
  // Calls without a direct callee (function pointers) are handled by the
  // indirect path inside emitCall.
  return success(succeeded(emitCall(call)));
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

  // Translate the C format string into a Rust format string. The literal's
  // bytes already have C escapes decoded (a "\n" is a real newline byte);
  // the StringAttr printer re-escapes them for the textual assembly.
  llvm::StringRef format = literal->getString();
  std::string rustFormat;
  rustFormat.reserve(format.size());
  SmallVector<Value> operands;
  unsigned argIndex = 1;
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

  SmallVector<Attribute> callArguments;
  callArguments.push_back(builder.getStringAttr(rustFormat));
  for (unsigned i = 0, e = operands.size(); i < e; ++i)
    callArguments.push_back(builder.getIndexAttr(i));
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("print!"),
      builder.getArrayAttr(callArguments), operands);
  return success();
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
  // stops at the first NUL. A `char *` variable bound to a literal is not
  // an array lvalue and stays rejected here.
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
    // The null constant of a function pointer is `None`; data pointers
    // have no null representation and keep their located rejections.
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
    FailureOr<Value> place = emitLValue(sub);
    if (failed(place))
      return failure();
    return loadPlace(loc, *place);
  }
  case clang::CK_IntegralCast: {
    // Integer-to-enum: Rust has no such cast, so the only conversion with
    // an enum destination the importer accepts is a reference to an
    // enumerator of that same enum (in C the enumerator itself has type
    // `int`, so even `enum Color c = Red;` arrives as this cast).
    if (const clang::EnumDecl *target = namedEnumDeclOf(cast->getType())) {
      if (const auto *ref =
              llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(sub)))
        if (const auto *enumerator =
                llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl()))
          if (llvm::cast<clang::EnumDecl>(enumerator->getDeclContext())
                  ->getDefinition() == target)
            return emitEnumConstant(enumerator, loc);
      return emitError(loc) << "unsupported: integer to enum conversion";
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
  // scalar) is cursor 0 of its object. Null pointer constants are rejected
  // inside `emitPointerRValue`.
  if (isPointerType(op->getLHS()->getType()) ||
      isPointerType(op->getRHS()->getType())) {
    FailureOr<PtrExprValue> lhs = emitPointerRValue(op->getLHS());
    if (failed(lhs))
      return failure();
    FailureOr<PtrExprValue> rhs = emitPointerRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    if (lhs->base != rhs->base)
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

  FailureOr<Value> lhs = emitRValue(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<Value> rhs = emitRValue(op->getRHS());
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
  // A data pointer tested for truth is a null check; decomposed pointers
  // have no null value (null pointer constants are rejected), so their
  // truth values are rejected rather than silently mistranslated.
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_PointerToBoolean)
      return emitError(loc) << "unsupported: pointer used as a truth value";
  if (isPointerType(e->getType()))
    return emitError(loc) << "unsupported: pointer used as a truth value";
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
  if (!callee->getDeclName().isIdentifier())
    return emitError(loc) << "unsupported callee";
  if (callee->getName() == "printf")
    return emitError(loc) << "unsupported: printf return value must be unused";
  // Statement-position puts/putchar are lowered by name (emitCallStmt);
  // their int result has no representation there, so a value use of a
  // definition-less puts/putchar is rejected.
  if ((callee->getName() == "puts" || callee->getName() == "putchar") &&
      !callee->getDefinition())
    return emitError(loc) << "unsupported: " << callee->getName()
                          << " return value must be unused";
  if (callee->isVariadic())
    return emitError(loc) << "unsupported: call to a variadic function";

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
  if (call->getNumArgs() != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";

  // C leaves the argument evaluation order unspecified; materialize every
  // value argument before any borrow-producing argument so that no load is
  // emitted between a `&mut` borrow and the call consuming it (rustc
  // rejects an intervening use of the borrowed place).
  SmallVector<Value> arguments(call->getNumArgs(), Value());
  struct PendingBorrow {
    unsigned index;
    const clang::Expr *expr;
  };
  SmallVector<PendingBorrow, 4> borrows;
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(
            targetType.getInput(index))) {
      borrows.push_back({static_cast<unsigned>(index), argument});
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

  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
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
      bool rootedAtOwner = pointer->base == ownerBase ||
                           (currentMethodOwner == ownerBase &&
                            isPointerType(pointer->base->getType()));
      if (!rootedAtOwner)
        return emitError(loc)
               << "unsupported: pointer argument does not point into owner "
                  "object '"
               << ownerBase->getName() << "'";
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
    // Slice parameter: reslice the argument's region base from its cursor.
    // A decayed array decomposes to cursor 0, `&arr[i]` to cursor i, a
    // walking pointer to its current cursor, and a slice parameter's own
    // read composes through the deref'd base place.
    FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
    if (failed(pointer))
      return failure();
    root = pointer->base;
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
    FailureOr<Value> place = emitPointerPlace(loc, *pointer);
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

  FailureOr<Value> fnValue = emitRValue(calleeExpr);
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

FailureOr<PtrExprValue>
CImporter::emitPointerRValue(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  Location loc = translateLoc(e->getBeginLoc());
  IntegerType cursorType = builder.getIntegerType(64);

  // Decomposed pointers have no null value; C code guarding on NULL cannot
  // be translated faithfully and is rejected instead.
  if (e->isNullPointerConstant(astContext(),
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return emitError(loc)
           << "unsupported: null pointer constant in a pointer expression";

  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    switch (cast->getCastKind()) {
    case clang::CK_NoOp:
      return emitPointerRValue(cast->getSubExpr());
    case clang::CK_LValueToRValue: {
      // A read of a pointer local: its base is static, its cursor is the
      // current value of the cursor cell (none for a degenerate base).
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
          stripTrivia(cast->getSubExpr()));
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var)
        break;
      auto it = pointerLocals.find(var);
      if (it != pointerLocals.end()) {
        const PointerLocalInfo &info = it->second;
        Value cursor;
        if (info.cursorCell)
          cursor = loadPlace(loc, info.cursorCell);
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
      // A decayed array is its own base at cursor 0.
      const clang::Expr *sub = stripTrivia(cast->getSubExpr());
      if (llvm::isa<clang::StringLiteral>(sub))
        return emitError(loc) << "unsupported: pointer to a string literal";
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub);
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var)
        break;
      if (!var->hasLocalStorage())
        return emitError(loc) << "unsupported: pointer into a global "
                                 "variable";
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
        if (!var->hasLocalStorage())
          return emitError(loc)
                 << "unsupported: pointer into a global variable";
        // `&x`: the degenerate (cursor-less) form of the scalar or struct
        // object itself.
        return PtrExprValue{var, Value()};
      }
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(sub)) {
        // `&arr[i]` (via the decay of `arr`) and `&q[i]` (i.e. `q + i`)
        // both decompose the subscript base and offset it by the index.
        FailureOr<PtrExprValue> pointer =
            emitPointerRValue(subscript->getBase());
        if (failed(pointer))
          return failure();
        if (!pointer->cursor) {
          // The address of a scalar admits only the constant-zero index.
          clang::Expr::EvalResult indexValue;
          if (subscript->getIdx()->EvaluateAsInt(indexValue, astContext()) &&
              indexValue.Val.getInt() == 0)
            return PtrExprValue{pointer->base, Value()};
          return emitError(loc) << "unsupported: arithmetic on the address "
                                   "of a scalar object";
        }
        FailureOr<Value> index = emitRValue(subscript->getIdx());
        if (failed(index))
          return failure();
        if (!llvm::isa<IntegerType>((*index).getType()))
          return emitError(loc) << "unsupported subscript index type";
        Value offset = castToIntType(loc, *index, cursorType);
        Value cursor =
            builder.create<arith::AddIOp>(loc, pointer->cursor, offset)
                .getResult();
        return PtrExprValue{pointer->base, cursor};
      }
      return emitError(loc) << "unsupported pointer target expression";
    }
    if (unary->isIncrementDecrementOp()) {
      // `p++` / `--p` in pointer-value position: update the cursor cell and
      // yield the pre-value (postfix) or post-value (prefix) per C. Both
      // pointer locals and slice parameters carry cursor cells.
      const clang::VarDecl *var = asVarRef(unary->getSubExpr());
      auto it = var ? pointerLocals.find(var) : pointerLocals.end();
      if (it == pointerLocals.end())
        return emitError(loc)
               << "unsupported: ++/-- on this pointer expression";
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
      return PtrExprValue{info.base, unary->isPostfix() ? current : next};
    }
    return emitError(loc) << "unsupported pointer expression";
  }

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    clang::BinaryOperatorKind opcode = binary->getOpcode();
    if (opcode == clang::BO_Add || opcode == clang::BO_Sub) {
      // `p + n` / `p - n` / `n + p`: cursor arithmetic. The operands are
      // emitted in source order (C leaves the order unspecified).
      bool lhsIsPointer = isPointerType(binary->getLHS()->getType());
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
      return PtrExprValue{pointer->base, cursor};
    }
  }

  return emitError(loc) << "unsupported pointer expression: "
                        << e->getStmtClassName();
}

FailureOr<Value> CImporter::emitPointerPlace(Location loc,
                                             const PtrExprValue &pointer) {
  auto it = symbols.find(pointer.base);
  if (it == symbols.end())
    return emitError(loc) << "unsupported: pointer target '"
                          << pointer.base->getName()
                          << "' is not an importable place";
  Value basePlace = it->second;
  if (!pointer.cursor)
    return basePlace; // Degenerate: the pointer designates the whole object.
  auto lvalueType = llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
  if (!lvalueType)
    return emitError(loc) << "unsupported pointer target place";
  // The base place wraps an array (local array base) or a slice (deref'd
  // slice parameter base); both subscript by the cursor.
  Type elementType;
  if (auto arrayType =
          llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
    elementType = arrayType.getElementType();
  else if (auto sliceType =
               llvm::dyn_cast<emitrust::SliceType>(lvalueType.getValueType()))
    elementType = sliceType.getElementType();
  else
    return emitError(loc) << "unsupported pointer target place";
  return builder
      .create<emitrust::SubscriptOp>(
          loc, emitrust::LValueType::get(elementType), basePlace,
          pointer.cursor)
      .getResult();
}

FailureOr<Value>
CImporter::emitPointerDifference(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  FailureOr<PtrExprValue> lhs = emitPointerRValue(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<PtrExprValue> rhs = emitPointerRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  if (lhs->base != rhs->base)
    return emitError(loc)
           << "unsupported: difference of pointers into different objects";
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

  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
    auto it = symbols.find(ref->getDecl());
    if (it == symbols.end()) {
      if (const GlobalInfo *global = lookupGlobal(ref->getDecl())) {
        // Stage the global's whole value in a local copy. Refined element
        // and field accesses read and write the copy; a write context
        // passes `writeback` and stores the copy back afterwards
        // (load-modify-store). Exact for the single-threaded C subset up
        // to one corner: a function called from the same statement's index
        // or right-hand side that writes the same global is overwritten by
        // the store-back (last writer wins).
        Value place = builder
                          .create<emitrust::VariableOp>(
                              loc, emitrust::LValueType::get(global->type))
                          .getResult();
        Value current = builder
                            .create<emitrust::GlobalLoadOp>(
                                loc, global->type, globalSymbol(global->symbol))
                            .getResult();
        builder.create<emitrust::AssignOp>(loc, place, current);
        if (writeback)
          *writeback = GlobalWriteback{place, global->symbol};
        return place;
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
      if (const auto *named =
              llvm::dyn_cast<clang::NamedDecl>(ref->getDecl()))
        if (named->getDeclName().isIdentifier() && isSystemHeaderDecl(named))
          return rejectSystemHeaderUse(loc, "reference to",
                                       named->getName());
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
    if (const auto *param =
            llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
      if (isPointerType(param->getType()) &&
          !isFunctionPointer(param->getType()))
        return emitError(loc) << "unsupported use of pointer parameter '"
                              << param->getName() << "'";
    return place;
  }

  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (!field)
      return emitError(loc) << "unsupported member access";
    Value basePlace;
    if (member->isArrow() && isDecomposedPointerExpr(member->getBase())) {
      // `p->f` through a decomposed pointer: resolve the pointer to its
      // place (the base object itself, or an element of the base array)
      // and refine it with the member access below.
      FailureOr<PtrExprValue> pointer = emitPointerRValue(member->getBase());
      if (failed(pointer))
        return failure();
      FailureOr<Value> place = emitPointerPlace(loc, *pointer);
      if (failed(place))
        return failure();
      basePlace = *place;
    } else if (member->isArrow()) {
      FailureOr<Value> base = emitRValue(member->getBase());
      if (failed(base))
        return failure();
      Type pointee;
      if (auto mutRef =
              llvm::dyn_cast<emitrust::MutRefType>((*base).getType()))
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
    FailureOr<Type> fieldType = mapType(field->getType(), loc);
    if (failed(fieldType))
      return failure();
    return builder
        .create<emitrust::MemberOp>(loc, emitrust::LValueType::get(*fieldType),
                                    basePlace,
                                    builder.getStringAttr(field->getName()))
        .getResult();
  }

  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
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
      FailureOr<PtrExprValue> pointer =
          emitPointerRValue(subscript->getBase());
      if (failed(pointer))
        return failure();
      if (!pointer->cursor) {
        // Degenerate base (the address of a scalar): only the constant-zero
        // subscript designates the object (`p[0]` on `p = &x`).
        clang::Expr::EvalResult indexValue;
        if (!subscript->getIdx()->EvaluateAsInt(indexValue, astContext()) ||
            indexValue.Val.getInt() != 0)
          return emitError(loc) << "unsupported: nonzero subscript on the "
                                   "address of a scalar object";
        return emitPointerPlace(loc, *pointer);
      }
      FailureOr<Value> index = emitRValue(subscript->getIdx());
      if (failed(index))
        return failure();
      if (!llvm::isa<IntegerType>((*index).getType()))
        return emitError(loc) << "unsupported subscript index type";
      Value offset =
          castToIntType(loc, *index, builder.getIntegerType(64));
      Value cursor =
          builder.create<arith::AddIOp>(loc, pointer->cursor, offset)
              .getResult();
      return emitPointerPlace(loc, PtrExprValue{pointer->base, cursor});
    }
    FailureOr<Value> basePlace = emitLValue(base, writeback);
    if (failed(basePlace))
      return failure();
    auto baseType =
        llvm::dyn_cast<emitrust::LValueType>((*basePlace).getType());
    if (!baseType)
      return emitError(loc) << "unsupported subscript base";
    auto arrayType =
        llvm::dyn_cast<emitrust::ArrayType>(baseType.getValueType());
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

  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_Deref) {
      // A dereference of a decomposed pointer resolves to a place on its
      // base object; the pointer-parameter reference path is unchanged.
      if (isDecomposedPointerExpr(unary->getSubExpr())) {
        FailureOr<PtrExprValue> decomposed =
            emitPointerRValue(unary->getSubExpr());
        if (failed(decomposed))
          return failure();
        return emitPointerPlace(loc, *decomposed);
      }
      FailureOr<Value> pointer = emitRValue(unary->getSubExpr());
      if (failed(pointer))
        return failure();
      Type pointee;
      if (auto mutRef =
              llvm::dyn_cast<emitrust::MutRefType>((*pointer).getType()))
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

  return emitError(loc) << "unsupported assignable expression: "
                        << e->getStmtClassName();
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

LogicalResult CImporter::finalizeProject() {
  // Every deferred `extern` global reference must have a real definition in
  // some translation unit; the Rust program otherwise reads an undefined
  // symbol.
  for (const auto &entry : pendingExternGlobals)
    if (!SymbolTable::lookupSymbolIn(module, entry.getKey()))
      return emitError(entry.getValue())
             << "unsupported: extern global variable '" << entry.getKey()
             << "' is referenced but not defined in any translation unit";

  // No non-variadic external function may remain body-less: the Rust emitter
  // cannot emit a body-less function. (Variadic prototypes such as printf were
  // never added to the module, so any external func here is a genuine
  // undefined reference.)
  for (func::FuncOp func : module.getOps<func::FuncOp>())
    if (func.isExternal())
      return emitError(func.getLoc())
             << "unsupported: function '" << func.getSymName()
             << "' is referenced but not defined in any translation unit";
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
  std::vector<std::string> commandLine{"-std=c11"};
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
