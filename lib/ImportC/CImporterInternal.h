//===- CImporterInternal.h - CImporter class + private helpers -*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Private implementation header for the C importer: the `CImporter` class
/// (the functional core of `importC`/`importCProject`, declared in the public
/// `EmitRust/ImportC.h`) plus the helper structs and small analyses its
/// methods share. Included only by the `ImportC*.cpp` translation units that
/// implement `CImporter`'s methods; never installed or exposed publicly.
///
/// `CImporter` and its helper types are declared here at ordinary (non
/// anonymous-namespace) scope so that identical copies of this header,
/// included by several `.cpp` files, name the *same* class: out-of-line
/// member definitions such as `CImporter::importFunction` in one translation
/// unit must refer to the identical type used by callers compiled in another
/// translation unit. Wrapping them in an anonymous namespace, as the
/// single-file version of this code did, would give each translation unit
/// its own distinct type and break linkage across the split files.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_IMPORTC_CIMPORTERINTERNAL_H
#define EMITRUST_IMPORTC_CIMPORTERINTERNAL_H

// The pure decl-to-emitted-symbol naming primitives (isRustKeyword,
// mangleMemberName, recordRustName, namespacePrefix, and the
// cFunctionSymbolName/cGlobalSymbolName composites `mlirFuncName` and
// `globalVarSymbolName` are thin wrappers over) live in this shared header
// so that the FR-40 project item graph names items EXACTLY as the importer
// emits them, by calling the same code rather than by mirroring it.
#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"
#include "EmitRust/ImportC.h"

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
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/RecordLayout.h"
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

// The shared naming primitives are pulled in by name rather than with a
// blanket `using namespace mlir::emitrust`, which would also drag in every
// dialect operation class and make the many deliberate `emitrust::`-
// qualified spellings in this file ambiguous to read.
using mlir::emitrust::cFunctionSymbolName;
using mlir::emitrust::cGlobalSymbolName;
using mlir::emitrust::enumTypeRustName;
using mlir::emitrust::enumVariantRustName;
using mlir::emitrust::fnRustName;
using mlir::emitrust::globalRustName;
using mlir::emitrust::idiomaticRenameEnabled;
using mlir::emitrust::isRustKeyword;
using mlir::emitrust::mangleMemberName;
using mlir::emitrust::namespacePrefix;
using mlir::emitrust::recordRustName;
using mlir::emitrust::typeRustName;

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

/// Whole-program facts gathered by a pre-import pass over EVERY translation
/// unit of a multi-file project (`collectWholeProgramInfo`), BEFORE any TU is
/// imported. The `planOwners`/`planCellSlices`/`planFnPtrAliases` planners do
/// ZERO cross-TU merging today — each runs per TU over fresh, `Decl*`-keyed
/// state — so a project's whole-program view cannot be assembled from their
/// results: a raw `clang::Decl*` from one TU's `ASTContext` is meaningless in
/// another (a project's per-file `ClangTool` parses each input as an
/// independent `ASTUnit`). This structure is therefore keyed by MLIR SYMBOL
/// NAME, the one identity that bridges the independent contexts — the same
/// bridge `crossTuVaListVariadicNames` already uses (W3.0).
///
/// W3.2 only BUILDS this substrate; nothing consumes it yet, so populating it
/// must leave every import byte-identical. The multi-TU gate relaxations
/// (W3.3/W3.4) and cross-TU va_list monomorphization (W3.5) are its intended
/// consumers, each documented on the field it reads. Every fact concerns only
/// EXTERNALLY VISIBLE symbols — the sole entities that can cross a TU
/// boundary — so the symbol names are tag-free (`mlirFuncName` /
/// `globalVarSymbolName` add the per-TU tag to internal-linkage names only).
struct WholeProgramInfo {
  /// Symbol names of externally visible globals whose address is taken (`&g`,
  /// `&g[i]`, `&g.m`, or an array-to-pointer decay) in ANY TU's function body
  /// or file-scope initializer. The whole-program analogue of the per-TU
  /// `addressTaken` set; the intended consumer is G1 (whole-program
  /// address-taken → pointer-result erasure), which may erase a global's
  /// pointer result only once it is proven the address is taken nowhere the
  /// project cannot see.
  llvm::StringSet<> addressTakenGlobals;
  /// Symbol names of externally visible function-pointer globals written
  /// (assigned, incremented/decremented) or escaped (address-of) in ANY TU.
  /// The whole-program analogue of the per-TU `fnPtrGlobalsWritten`; the
  /// intended consumer is G7 (a fn-ptr global provably never written across
  /// the WHOLE program may back a cross-TU dispatch table).
  llvm::StringSet<> fnPtrGlobalsWritten;
  /// Cross-TU call enumeration: each externally visible callee's symbol name
  /// mapped to the indices of the TUs that call it directly. A function
  /// called from a TU other than its definer needs its variadic clones
  /// materialized in the DEFINING TU (W3.5), which no single TU's AST can
  /// enumerate; this is that whole-program call-site set.
  llvm::StringMap<llvm::SmallVector<unsigned, 2>> calleeToCallerTus;
  /// Externally visible function symbol name → the indices of the TUs that
  /// take its address (any `DeclRefExpr` to it outside a direct-call callee
  /// position — passed as an argument, assigned to a function pointer, etc.).
  /// Combined with `calleeToCallerTus`, this is the whole-program "which TUs
  /// reference this function" view the G3 owner-promotion relaxation reads: an
  /// externally visible function is safe to promote in a multi-TU project
  /// exactly when at most ONE TU references it (necessarily its own definer),
  /// so this TU's `planOwners` then has the sole-TU-equivalent call-site
  /// visibility the all-or-nothing promotion rule requires. Address-taking
  /// matters because an indirectly called function could be reached from
  /// another TU with an argument this TU's analysis never sees.
  llvm::StringMap<llvm::SmallVector<unsigned, 2>> fnAddressTakenTus;
  /// Data-pointer return-type spelling (canonical `QualType::getAsString`) →
  /// the deduped indices of the TUs that take the address of at least one
  /// function returning a pointer of that type. The G1 candidate-completeness
  /// oracle: `classifyFnPtrPointerResult` builds its erased-return candidate
  /// set from THIS TU's per-TU `addressTakenFunctions` (refilled each TU by
  /// `planFnPtrAliases`), so in a multi-TU project a diverging function whose
  /// address is taken only in ANOTHER TU could be missed — an unsound
  /// erasure. When this map reports at most ONE TU for the return-type
  /// spelling, this TU's per-TU candidate set is provably the complete
  /// whole-program set and the classifier may run; two or more TUs keep the
  /// blanket rejection (the precise cross-TU disagreement wording needs the
  /// full erased-base substrate, deferred — design.md FR-34). The key is a
  /// deliberately narrow type SPELLING, not a `mapType` result: it is
  /// side-effect-free (no memoization poisoning, per W3.2 COMMIT B's lesson)
  /// and cross-TU-stable for named types (anonymous types cannot be shared
  /// across a TU boundary anyway).
  llvm::StringMap<llvm::SmallVector<unsigned, 2>> dataPtrReturnFnAddressTakenTus;
  /// Externally visible pointer-global symbol name → the set of base-object
  /// symbol names it is bound to across the whole program (from its
  /// file-scope initializer and every `g = &base…` assignment in any body).
  /// Exactly one base project-wide is the sound single-region shape the G8
  /// relaxation admits; two or more distinct bases is a divergent cross-TU
  /// rebinding the single-base cursor model cannot represent and must keep
  /// rejecting (the whole-program analogue of the in-TU multi-object-regions
  /// rejection).
  llvm::StringMap<llvm::StringSet<>> pointerGlobalBases;
  /// Externally visible pointer-global symbol name → the SOLE project-wide
  /// binding's base object, canonical `VarDecl*` in its OWN defining TU's
  /// `ASTContext` (valid for the whole `importCProject` call: every parsed
  /// AST stays alive until every TU has imported). Populated ONLY for a
  /// symbol whose `pointerGlobalBases` entry has EXACTLY one base and whose
  /// binding came from a plain file-scope initializer with no other
  /// project-wide rebinding evidence (`pointerGlobalHasBodyRebind` is
  /// false) — the narrow "shared header pointer global" shape W3.2 COMMIT B
  /// reconstructs cross-TU (deferExternGlobal): the consuming TU eagerly
  /// re-imports this Decl (idempotent, `importGlobalVar` already tolerates
  /// re-entry) so `globals`/`pointerGlobals` resolve regardless of which TU
  /// is processed first, then synthesizes its own cursor global by name.
  /// Anything else (a synthesized backing, a body-level rebind anywhere, a
  /// member-rooted or arithmetic-derived base) is left unpopulated here and
  /// `deferExternGlobal` keeps the historical unconditional rejection.
  llvm::StringMap<const clang::VarDecl *> pointerGlobalSoleFileScopeBase;
  /// Externally visible pointer-global symbol name → whether ANY function
  /// body anywhere in the project reassigns it (`g = &x;`), as opposed to
  /// only its file-scope initializer. Guards
  /// `pointerGlobalSoleFileScopeBase`: a dynamically reassigned global
  /// pointer's cursor is a runtime value with no compile-time offset, so
  /// such a symbol is never eligible for the static cross-TU
  /// reconstruction even when every rebinding happens to target the same
  /// base object.
  llvm::StringSet<> pointerGlobalHasBodyRebind;
  /// Externally visible pointer-global symbol name → the flat i64 cursor
  /// start of its SOLE file-scope binding (meaningful only alongside
  /// `pointerGlobalSoleFileScopeBase`), computed in the DEFINING TU's own
  /// `ASTContext` while it was the active pre-scan AST. Zero (and unused)
  /// for a degenerate whole-object binding, where the pointer needs no
  /// runtime cursor at all.
  llvm::StringMap<int64_t> pointerGlobalCursorStart;
  /// Externally visible global symbol name → its COMPLETE MLIR type,
  /// resolved from whichever TU defines it with a bounded array type
  /// (`int a[4] = {...};`), mapped while that TU's `ASTContext` was active.
  /// Populated ONLY when the type is a bounded array (never overwritten by
  /// a later TU's incomplete declaration of the same symbol, since the
  /// pre-pass only records here on a COMPLETE sighting) — the substrate for
  /// the extern-array composite-merge fix (W3.2 COMMIT B,
  /// `deferExternGlobal`): C99 6.2.7 lets `extern int a[];` in one TU be
  /// completed by another TU's sized definition, but each TU parses as an
  /// independent `clang::ASTUnit` with no cross-TU type composition step of
  /// its own, so the incomplete declaration's OWN (incomplete) type can
  /// never be mapped — this whole-program fact, gathered before any TU
  /// imports, supplies the composite type instead.
  llvm::StringMap<Type> completeArrayGlobalTypes;

  //=== W3.3 G4/G5/G6: whole-program cell-slice (CTS-P10) merge ============
  // `planCellSlices` runs per TU and blanket-excludes every externally
  // visible function parameter (:612 poison) and every externally visible
  // global/owning function (:724/:751) from the cell-slice path, because an
  // unseen TU could call the function with a Cell-less local argument. The
  // emission model is already generic (`&[Cell<T>]` parameters, the concrete
  // global bound per call site via `emitrust.global_cells`), so the only
  // barrier is these per-TU gates. These fields carry the whole-program
  // call-argument facts that lift them soundly. Built by
  // `collectCellSliceCallFacts` (per TU) and `finalizeCellSliceWholeProgram`.

  /// Raw fact: cell-slice parameter key `"<fnSymbol>#<index>"` (for an
  /// externally visible callee's data-pointer parameter) → the set of
  /// externally visible global-array symbols passed to it as a directly
  /// decayed argument anywhere in the project. Internal-linkage globals are
  /// NOT recorded (they stay per-TU, so the same external function may back
  /// a different internal global in each TU — g6 — which the generic
  /// `&[Cell<T>]` parameter represents without merging).
  llvm::StringMap<llvm::StringSet<>> cellSliceParamExtGlobals;
  /// Raw fact: parameter keys poisoned by a Cell-less cross-TU argument
  /// anywhere — a local-array decay, a pointer-to-pointer parameter, or a
  /// forwarded parameter (conservatively poisoned: the untested cross-TU
  /// forward-to-external shape keeps the historical rejection).
  llvm::StringSet<> cellSliceParamPoisoned;
  /// FINAL (`finalizeCellSliceWholeProgram`): the `"<fnSymbol>#<index>"`
  /// keys whose whole-program cell-slice class is eligible — not poisoned
  /// and backed by AT MOST ONE externally visible global (the multi-base
  /// guard: two distinct external globals through one parameter is the
  /// single-TU multi-base disqualification, made whole-program — g5
  /// negative). `planCellSlices` consults this to lift its :612 and :751
  /// external-visibility exclusions.
  llvm::StringSet<> cellSliceEligibleParamKeys;
  /// FINAL: externally visible global-array symbols whose every cell-slice
  /// binding is to a clean (non-poisoned, single-external-base) parameter —
  /// the globals `planCellSlices` may keep on the cell-slice path despite
  /// their external linkage (:724 lift).
  llvm::StringSet<> cellSliceEligibleGlobals;
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

/// Returns whether `type` is, or contains (through record fields and array
/// elements, never through pointers), a record with a bit-field member.
/// Used to refuse `sizeof`/`_Alignof` folds over bit-field records: the
/// C99-45 backing-run layout is deliberately not ABI-compatible, so the C
/// layout numbers would promise a layout the emitted Rust does not keep.
/// Recursion depth is bounded by the source's type nesting (pointers are
/// not followed, so self-referencing records terminate).
static inline bool typeContainsBitField(clang::QualType type) {
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

/// Returns whether `type` is, or contains (through record fields and array
/// elements, never through pointers), the `long double` builtin. Used to
/// refuse `sizeof`/`_Alignof` folds over long double: the type imports as
/// f64 (CTS 00204), so the C ABI size (16 on x86-64) would promise a
/// layout the emitted Rust never keeps.
static inline bool typeContainsLongDouble(clang::QualType type) {
  const clang::Type *canonical = type.getCanonicalType().getTypePtr();
  while (const auto *array = llvm::dyn_cast<clang::ArrayType>(canonical))
    canonical = array->getElementType().getCanonicalType().getTypePtr();
  if (const auto *builtin = llvm::dyn_cast<clang::BuiltinType>(canonical))
    return builtin->getKind() == clang::BuiltinType::LongDouble;
  const clang::RecordDecl *record = canonical->getAsRecordDecl();
  if (!record)
    return false;
  record = record->getDefinition();
  if (!record)
    return false;
  for (const clang::FieldDecl *field : record->fields())
    if (typeContainsLongDouble(field->getType()))
      return true;
  return false;
}

/// Builds a FloatAttr of `type` from `value`, converting the APFloat to
/// the type's semantics when they differ. The only differing source is a
/// `long double` constant (x87 80-bit extended on the x86-64 target),
/// whose value narrows to the f64 the type policy substitutes for it
/// (CTS 00204; correctly rounded, exact for every f64-exact source).
static inline FloatAttr floatAttrFor(FloatType type, llvm::APFloat value) {
  if (&value.getSemantics() != &type.getFloatSemantics()) {
    bool losesInfo = false;
    (void)value.convert(type.getFloatSemantics(),
                        llvm::APFloat::rmNearestTiesToEven, &losesInfo);
  }
  return FloatAttr::get(type, value);
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
  /// The MUTABLE local backing array place of a heap-allocation region
  /// (W4.2e Part A): a synthesized entry-block
  /// `!emitrust.lvalue<!emitrust.array<CAP x T>>` a local `T *p =
  /// malloc(...)` decomposes against, subscripted at `cursor`. Distinct
  /// from `literalBacking`, which is the READ-ONLY backing of a string
  /// literal; writes through this backing are allowed. Null for
  /// object-based and string-literal pointers.
  Value backing;
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
  /// The synthesized MUTABLE entry-block backing array place of a local
  /// heap-allocation region (W4.2e Part A): an
  /// `!emitrust.lvalue<!emitrust.array<CAP x T>>` a local `T *p =
  /// malloc(...)` decomposes against (`p[i]` subscripts it at `cursorCell`).
  /// Null for every non-allocation local. Distinct from `literalBacking`
  /// (read-only string-literal backing).
  Value backing;
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

/// The proven facts of one self-referential array-member-pointer field
/// (Stage 2 of the owner-struct self-reference extension, design.md FR-30
/// follow-on): `planArrayMemberPointers` (Pass A, run after `planOwners`)
/// proves that EVERY read or write of this field, anywhere in the
/// program, is an arrow access (`x->field`) whose pointer root is
/// provably an element of the SAME promoted owner array — the closed set
/// of `elementCount` possible values a write can ever store. Unlike
/// `MemberPointerFacts` (one binding per struct INSTANCE, tracking a
/// single degenerate whole-object target), this fact is one binding per
/// FIELD DECLARATION covering every instance at once, because the proof
/// itself is program-wide: the field's synthesized `!emitrust.enum`
/// storage type and its `emitrust.switch`-encoded writes are shared by
/// every element of the array. A field absent from
/// `arrayMemberPtrBindings` (or present with a non-empty
/// `invalidReason`) falls through unchanged to the historical
/// `memberPtrBindings`/`poisonedPtrFields` static-binding model — this
/// analysis is strictly additive and never changes behavior for a field
/// it cannot prove safe.
struct ArrayMemberPointerFacts {
  /// The promoted owner array (an `ownerPlans` key) every proven site of
  /// this field roots into; null until populated.
  const clang::VarDecl *ownerArray = nullptr;
  /// The number of elements of `ownerArray` (1..32, the owner-struct MVP
  /// limit `kMaxOwnerArrayElements` already enforced by `planOwners`) —
  /// the field's enum variant count.
  unsigned elementCount = 0;
  /// The module symbol of the synthesized `emitrust.enum_def` backing
  /// this field's storage type, assigned on first emission use
  /// (`getOrCreateArrayMemberEnumType`); empty until then.
  std::string enumSymbol;
  /// Diagnostic text explaining why this field could not be proven safe;
  /// empty when the field is populated and usable. A field this pass
  /// cannot fully prove is simply left absent from
  /// `arrayMemberPtrBindings` rather than recorded with a reason here, so
  /// this member currently stays unused, kept for parity with
  /// `MemberPointerFacts` and as a landing spot for a future pass that
  /// wants a located "why not" diagnostic instead of a silent fallback.
  std::string invalidReason;
  /// Location of the invalidating construct; meaningful only with
  /// `invalidReason`.
  clang::SourceLocation invalidLoc;
};

/// The program-wide key of one array-member-pointer fact: the field
/// declaration alone (see `ArrayMemberPointerFacts`'s doc for why this
/// fact is per-field rather than per-instance).
using ArrayMemberPointerKey = const clang::FieldDecl *;

/// The RFC index-handle node-pool plan of one function (W4.2e Part B,
/// C99-46 Stage 1; design.md FR-39). A function that builds a linked
/// structure from `malloc(sizeof(struct T))` calls inside a
/// foldable-trip-count loop -- where `struct T` has exactly one
/// self-referential data-pointer field, every `struct T *` local roots in
/// this one pool (bound to a malloc result, another such local, a
/// self-ref field read, or NULL), nothing escapes, and `free` is applied
/// only to such locals -- promotes to a fixed `[T; cap]` pool array plus a
/// free cursor. Each node pointer becomes a nullable index HANDLE (an i64
/// index cell + an i1 non-null cell, the CTS-P8 shape) rooted in the
/// synthesized pool; the self-ref field renders as `Option<usize>`.
/// Modeled on the owner-array self-ref member (`ArrayMemberPointerFacts`)
/// but with NO declared array base and a NULL variant -- the pool owner is
/// this function, not a `clang::VarDecl`.
struct MallocPoolFacts {
  /// The pooled node record (`struct T`); the `[T; cap]` element type.
  const clang::RecordDecl *structDecl = nullptr;
  /// The single self-referential data-pointer field (`next`) rendered as
  /// the nullable pool index `Option<usize>`.
  const clang::FieldDecl *nextField = nullptr;
  /// The fixed pool capacity, folded from the malloc loop's trip count
  /// times the mallocs per iteration (1..kMaxOwnerArrayElements-style cap).
  unsigned cap = 0;
  /// The single `malloc(sizeof(struct T))` call site that appends a slot.
  const clang::CallExpr *allocSite = nullptr;
};

/// FR-64: the recognized facts of a constant-fill C string buffer that lifts
/// to an idiomatic Rust `String`. The whole `char *a = malloc(N+1); for (i=0;
/// i<N; ++i) a[i]=C; a[N]='\0';` idiom is fused into a single
/// `let a: String = "C".repeat(N as usize);` binding, and every recognized
/// consumer (`puts`/`printf("%s")`) prints the `String` by `Display`. The
/// recognizer is conservative: any buffer with an arbitrary byte read, a
/// non-constant or non-ASCII fill, an escape, a return, or a size that cannot
/// be proven `>= count + 1` is NOT recorded and keeps its historical located
/// rejection.
struct StringFillFacts {
  /// The lifted buffer local (`a`); the `let a: String` binding site.
  const clang::VarDecl *bufferDecl = nullptr;
  /// The constant fill byte `C`, guaranteed ASCII 0x01..0x7F (single-byte
  /// UTF-8, non-NUL) so the `String`'s bytes equal the C buffer's bytes.
  char fillChar = 0;
  /// The half-open fill count expression `N` (the loop's upper bound), imported
  /// as an i64 rvalue and widened to `usize` for `.repeat`. Loop-invariant and
  /// side-effect free; every variable it reads is unwritten in the function, so
  /// evaluating it once at the decl site yields the loop's per-iteration value.
  const clang::Expr *countExpr = nullptr;
};

/// FR-65: the recognized facts of a runtime-sized heap buffer of a non-char
/// scalar element type that lifts to an owned Rust `Vec<T>` (the Vec arm of the
/// {array, Vec, span, Option} representation match). A `T *a = malloc(n *
/// sizeof(T))` / `calloc(n, sizeof(T))` whose only uses are `a[i]` indexing
/// (read AND write — `Vec<T>` has `IndexMut`, so there is no constant-fill
/// restriction) and `free(a)` binds `let a: Vec<T> = vec![<zero>; n as usize]`.
/// The recognizer is conservative: pointer arithmetic, `&a`, aliasing, an
/// escape/return, a nullable buffer, a char element, or a non-extractable size
/// leaves the buffer UNLIFTED with its historical located rejection.
struct VecFacts {
  /// The lifted buffer local (`a`); the `let a: Vec<T>` binding site.
  const clang::VarDecl *bufferDecl = nullptr;
  /// The mapped Rust element type `T` (a non-char arithmetic scalar), used to
  /// spell the `Vec<T>` opaque type and the suffixed zero fill literal.
  mlir::Type elementType;
  /// The element-count expression `n` (extracted from the allocation size),
  /// imported as an i64 rvalue at the decl site and widened to `usize` for
  /// `vec![_; n]`. Non-negative and side-effect free.
  const clang::Expr *countExpr = nullptr;
};

/// Registry of the synthesized backing declarations for block-scope
/// compound literals used as pointer-region bases (C99-13). A compound
/// literal in expression position is a fresh anonymous object with the
/// storage duration of the enclosing block; modeling it as an implicit
/// local variable lets the existing region machinery (bases, cursors,
/// slices, degenerate scalar-object bindings) consume it unchanged. The
/// registry is owned by the importer and shared with every
/// `PointerRegionAnalysis` instance (planning and emission passes alike),
/// so all passes agree on the identity of each literal's backing
/// declaration. The synthesized declaration is parented to the
/// translation unit but carries `SC_Auto` storage, so `hasLocalStorage()`
/// classifies it as a local object; it is never added to any lookup scope.
class CompoundLiteralTemps {
public:
  /// Returns the backing declaration of `literal`, synthesizing it on
  /// first request. All temps share the diagnostic-only name
  /// "compound literal" (their emitted places are anonymous).
  const clang::VarDecl *getOrCreate(clang::ASTContext &context,
                                    const clang::CompoundLiteralExpr *literal) {
    const clang::VarDecl *&slot = decls[literal];
    if (!slot) {
      clang::VarDecl *decl = clang::VarDecl::Create(
          context, context.getTranslationUnitDecl(), literal->getBeginLoc(),
          literal->getBeginLoc(), &context.Idents.get("compound literal"),
          literal->getType(),
          context.getTrivialTypeSourceInfo(literal->getType()),
          clang::SC_Auto);
      decl->setImplicit();
      slot = decl;
      temps.insert(decl);
    }
    return slot;
  }

  /// Returns whether `decl` is a synthesized compound-literal backing.
  bool isTemp(const clang::VarDecl *decl) const {
    return temps.contains(decl);
  }

private:
  /// One backing declaration per compound literal expression.
  llvm::DenseMap<const clang::CompoundLiteralExpr *, const clang::VarDecl *>
      decls;
  /// The synthesized declarations, for the reverse membership test.
  llvm::SmallPtrSet<const clang::VarDecl *, 4> temps;
};

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

/// The C99-43 C1 (Shape G) plan of one single-global-or-NULL out-param
/// cursor `T **p`: the parameter lowers to ONE in-out `&mut Option<i64>`
/// cell (Q1: arity preserved; Q4: Option-of-cursor NULL — None = C NULL,
/// Some(offset) = element offset into `global`'s backing). `global` is
/// the ONE statically-known target every admitted write names, or null
/// for the degenerate pure-NULL writer (`*p = 0` only). `writesNull` is
/// true when the callee can write NULL through the cell (a null-constant
/// or ternary RHS), which is what marks the caller-side pointer region
/// nullable.
struct GlobalCursorPlan {
  /// The single whole-global target; null for a pure-NULL writer.
  const clang::VarDecl *global = nullptr;
  /// Whether any admitted write can store NULL through the cell.
  bool writesNull = false;
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
/// Merges the facts of `absorbed` into `target`: bases are deduplicated by
/// declaration, literal and allocation bases conflict when they differ (a
/// pointer cannot range over two of them), flag/location pairs keep the
/// first recorded site, and only the first invalidation is kept. Used by
/// the per-function union-find (`PointerRegionAnalysis::unite`), by the
/// program-wide aggregation of a global pointer's per-function regions
/// (`CImporter::planOwners`, in ImportC.cpp), and by
/// `CImporter::importPointerGlobal` (in ImportCGlobals.cpp) — so unlike the
/// AST-helpers section above, this one needs a single definition (kept in
/// ImportC.cpp, next to `PointerRegionAnalysis`'s other out-of-line methods)
/// shared via an ordinary declaration rather than a `static inline` copy per
/// translation unit.
void mergeRegionFacts(PointerRegion &target, const PointerRegion &absorbed);

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

  /// Optional query telling the walk whether a direct call to `callee` is a
  /// promoted owner method proven to return an i64 element index into the
  /// same owner region as its own pointer parameter(s) (Stage 1 of the
  /// owner-index-return extension, design.md FR-30 follow-on). Such a call
  /// is a region source exactly like a copy from one of the callee's own
  /// pointer parameters: `recordPointerWrite` re-classifies the call's
  /// first pointer argument instead of falling through to the non-address
  /// rejection. Left unset (the pure-AST planning passes and any TU where
  /// no function yet qualifies), every call source keeps the historical
  /// rejection.
  std::function<bool(const clang::FunctionDecl *)> ownerIndexReturnQuery;

  /// Optional query telling the walk whether `field` is a (candidate or
  /// proven) array-member self-referential pointer field (Stage 2/4 of the
  /// owner-struct self-reference extension, design.md FR-30 follow-on):
  /// `p = x->field` on such a field is a region source exactly like a copy
  /// from `x` itself — the field always decodes to a cursor value inside
  /// the SAME owner class `x` roots into (Pass A's own per-field proof),
  /// so `recordPointerWrite` re-classifies the destination local through
  /// the arrow base (`x`) instead of falling through to the non-address
  /// rejection (Stage 4, B3). `planArrayMemberPointers` (Pass A) sets this
  /// to structural candidacy (a field is still being proven, so no
  /// `arrayMemberPtrBindings` entry exists yet) while emission sets it to
  /// the fully proven `arrayMemberPtrBindings` membership. Left unset (the
  /// other pure-AST planning passes), every such read keeps the historical
  /// non-address rejection.
  std::function<bool(const clang::FieldDecl *)> arrayMemberFieldQuery;

  /// Optional query telling the walk whether a local `void *` declaration
  /// is an admitted fn-ptr holder (CTS-F, 00210): such a local imports as
  /// an ordinary `!emitrust.fn_ptr` variable, so the decomposition never
  /// tracks it. Left unset (the pure-AST planning passes), the holder is
  /// tracked and conservatively invalid, which planning never consumes.
  std::function<bool(const clang::VarDecl *)> fnHolderQuery;

  /// Optional query telling the walk whether a local `char *` declaration is
  /// a recognized constant-fill string local (FR-64): its `malloc`/`calloc`
  /// binding is lifted whole to `String::repeat`, so the pointer-region model
  /// must NOT claim it as a flat heap backing (`recordAllocBase` is skipped).
  /// Left unset (the pure-AST planning passes), the local keeps its historical
  /// region tracking and its located non-constant-size rejection.
  std::function<bool(const clang::VarDecl *)> stringValueLocalQuery;

  /// Optional query telling the walk whether a local `T *` declaration is a
  /// recognized runtime-sized heap buffer lifted to `Vec<T>` (FR-65): its
  /// `malloc`/`calloc` binding becomes `vec![<zero>; n]`, so the
  /// pointer-region model must NOT claim it as a flat heap backing
  /// (`recordAllocBase`'s const-size gate is skipped). Left unset, the local
  /// keeps its historical region tracking and its located rejection.
  std::function<bool(const clang::VarDecl *)> vecValueLocalQuery;

  /// The importer-owned registry of synthesized compound-literal backing
  /// declarations (C99-13): a decayed or address-taken block-scope
  /// compound literal binds its backing declaration as an ordinary local
  /// region base. Shared across every analysis instance so planning and
  /// emission agree on each literal's backing identity. Left unset, such
  /// bindings keep the historical non-address rejection.
  CompoundLiteralTemps *literalTemps = nullptr;

  /// Optional query telling the walk whether a pointer-to-pointer
  /// parameter of the CURRENT function is a planned string-cursor
  /// parameter (CTS 00204). When set, `p = *s` binds the parameter as
  /// the region base (like a slice parameter) and `*s = expr` joins the
  /// parameter itself into the region as a rebindable pointer. Left
  /// unset, both shapes keep their historical rejections.
  std::function<bool(const clang::ParmVarDecl *)> cursorParamQuery;

  /// Optional query telling the walk whether argument `index` of a direct
  /// call to `callee` feeds a planned string-cursor parameter (CTS
  /// 00204). When set, a `&p` argument in such a position is consumed by
  /// the call lowering (region + in-out cursor) instead of invalidating
  /// `p`'s region; the callee's advancement is ordinary arithmetic.
  std::function<bool(const clang::FunctionDecl *, unsigned)> cursorArgQuery;

  /// Optional query telling the walk whether argument `index` of a direct
  /// call to `callee` feeds a planned Shape-P paired out-cursor parameter
  /// (C99-43 slice 1b), returning the index of the paired co-argument in
  /// the same call, or -1. When set, a `&e` argument in such a position
  /// is consumed by the call lowering AND `e` joins the co-argument
  /// expression's region — the callee returns `e` as a cursor into that
  /// region, exactly as if `e = <co-arg>; e += n;` had executed. Left
  /// unset, the `&e` argument invalidates `e` (address escape).
  std::function<int(const clang::FunctionDecl *, unsigned)> pairedArgQuery;

  /// Optional query telling the walk whether argument `index` of a direct
  /// call to `callee` feeds a planned Shape-G single-global-or-NULL
  /// out-param cursor (C99-43 C1), returning the parameter's plan. When
  /// set, a `&p` argument in such a position is consumed by the call
  /// lowering (a staged `Option<i64>` temp the callee overwrites), the
  /// plan's global binds as `p`'s region base — the caller's later reads
  /// route into that backing at the returned offset — and a null-writing
  /// callee marks the region nullable (the CTS-P8 flag cell carries the
  /// Option discriminant). Left unset, the `&p` argument invalidates
  /// `p` (address escape).
  std::function<std::optional<GlobalCursorPlan>(const clang::FunctionDecl *,
                                                unsigned)>
      globalCursorArgQuery;

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

  /// Evaluates `expr` to a non-negative constant `out`, folding references
  /// to *foldable automatic locals* (W4.2e Part A) in addition to the
  /// genuine integer-constant expressions `EvaluateAsInt` accepts. A
  /// foldable local has an integer-constant initializer and is never
  /// reassigned or address-taken in the analyzed body (`isFoldableLocal`),
  /// so its value is its initializer everywhere — this lets
  /// `malloc(cap * sizeof(int))` fold when `cap` is such a local (the
  /// natural allocation-size idiom, which `EvaluateAsInt` alone rejects
  /// because `cap` is not a C constant expression). Returns false on
  /// overflow-free failure; the arithmetic is unsigned and rejects a
  /// negative intermediate. Bounded: recursion follows a foldable local to
  /// its initializer, itself constant, so the depth is the expression's.
  /// Public so Pass-A planners (`foldLoopTripCount`) can fold with it.
public:
  /// Re-establishes the borrowed context and body after `analyze` has
  /// returned (which nulls them), so a planner can call `evalFoldableInt`
  /// on an already-analyzed instance (W4.2e Part B).
  void primeForFolding(clang::ASTContext &ctx, const clang::Stmt *body) {
    context = &ctx;
    analyzedBody = body;
  }

  bool evalFoldableInt(const clang::Expr *expr, uint64_t &out) const;

  /// True when `var` is an automatic local with an integer-constant
  /// initializer that is never reassigned (plain `=`, compound, or
  /// `++`/`--`) nor address-taken anywhere in the analyzed body, so every
  /// read of it yields the initializer (W4.2e Part A, `evalFoldableInt`).
  bool isFoldableLocal(const clang::VarDecl *var) const;

private:

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
  /// The body of the function under analysis (W4.2e Part A); borrowed, set
  /// for the duration of `analyze`. `isFoldableLocal` scans it to prove a
  /// local is never reassigned or address-taken.
  const clang::Stmt *analyzedBody = nullptr;
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
/// One monomorphized clone of a bounded va_list-using variadic definition
/// (CTS 00204): the synthesized symbol plus the MLIR types of the extra
/// arguments the clone's call sites pass (in declared order, by value).
struct VaClonePlan {
  std::string name;
  SmallVector<Type, 4> extraTypes;
};

/// The per-definition monomorphization plan: one clone per distinct extras
/// signature over the definition's direct call sites. A plan with zero
/// clones drops the definition entirely (no symbol).
struct VaMonomorphPlan {
  SmallVector<VaClonePlan, 4> clones;
};

/// FR-61f: a canonical C counting `for` recognized on the clang AST as a
/// half-open ascending range loop `for (int i = LO; i < HI; i += K)`. The
/// induction `i` is loop-scoped (declared in the init), `HI` is
/// loop-invariant, and `K` is a positive integer constant. Produced by
/// `matchRangeFor` and consumed by `emitRangeFor` to emit an `emitrust.for`
/// (`for i in LO..HI { .. }`) instead of the CFG `while` lowering.
struct RangeFor {
  const clang::VarDecl *iv; ///< induction variable, declared in the init
  const clang::Expr *lo;    ///< LO: the induction's initial value
  const clang::Expr *hi;    ///< HI: upper bound (loop-invariant)
  int64_t step;             ///< K: positive constant step
  bool inclusive;           ///< `i <= HI` (renders `..=`) vs `i < HI`
};

class CImporter {
public:
  /// Creates an importer that appends to `module`. The translation-unit
  /// specific context is supplied per call to `importTranslationUnit`, so one
  /// importer can merge several ASTs (cross-TU dedup state persists).
  explicit CImporter(ModuleOp module)
      : module(module), builder(module.getContext()) {}

  /// Turns on recoverable import (FR-42) for every subsequent
  /// `importTranslationUnit` call, recording each recovered rejection into
  /// `ledger`. Off by default, and there is deliberately no way to turn it
  /// back off: recovery is a whole-import mode chosen by the driver, and a
  /// half-recovering import would produce a module whose completeness
  /// depends on declaration order.
  void enableRecovery(emitrust::RejectionLedger &ledger) {
    recoverFromRejections = true;
    rejectionLedger = &ledger;
  }

  /// Restricts the import to the items a search state admits (FR-43):
  /// `excluded` holds FR-40 item-graph node keys that must NOT be imported.
  ///
  /// Only meaningful together with `enableRecovery` — the substitution an
  /// excluded item takes IS the recovery path — and `importCProject` calls
  /// it only when both are asked for. `excluded` must outlive the import;
  /// the importer holds a pointer so the (possibly large) set is not copied
  /// per translation unit.
  void setExcludedItems(const std::set<std::string> &excluded) {
    excludedItems = &excluded;
  }

  /// FR-52: chooses what `finalizeProject` does with a referenced-but-
  /// undefined external function. `ExternalRequirements::Reject` — the
  /// default — is the historical whole-program error; see
  /// `ImportC.h`'s `ExternalRequirements` for the two trait settings and for
  /// the shapes they deliberately do not cover.
  void setExternalRequirements(emitrust::ExternalRequirements policy) {
    externalRequirements = policy;
  }

  /// FR-57a: chooses what `finalizeProject` does with a referenced external
  /// symbol no imported TU defines. Off — the default — keeps the
  /// historical rejection (and the FR-52 trait path for functions). On, the
  /// symbol becomes a declaration marked `emitrust.extern_decl` for the
  /// FR-58 link step to resolve: an undefined extern global materializes as
  /// a declaration-only `emitrust.global`, and a referenced body-less
  /// function keeps its declaration. Defer takes precedence over the FR-52
  /// trait policy.
  void setDeferExternals(bool defer) { deferExternals = defer; }

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

  /// W3.0: records the `mlirFuncName` of every externally visible,
  /// va_list-using variadic DEFINITION in `context`'s translation unit into
  /// `crossTuVaListVariadicNames`. `importCProject` calls this for every
  /// parsed AST BEFORE importing any of them (order-independent), so a call
  /// site in a TU processed before its callee's defining TU still
  /// recognizes the callee as "monomorphized elsewhere" and raises the
  /// located cross-TU rejection (`emitCall`) instead of the generic
  /// "call to a variadic function" message. Internal-linkage (`static`)
  /// definitions are excluded: a `static` function cannot be called from
  /// another translation unit in valid C, and `mlirFuncName`'s per-TU tag
  /// would make its recorded name ambiguous across TUs anyway.
  void collectCrossTuVaListVariadics(clang::ASTContext &context);

  /// W3.2: gathers whole-program facts from `context`'s translation unit
  /// (identified by `tuIndex`, its position in `importCProject`'s path list)
  /// into `wholeProgram`. Like `collectCrossTuVaListVariadics`, this runs in
  /// `importCProject`'s pre-import pass over every AST, so `wholeProgram` is
  /// complete before the first TU imports and reflects the whole project
  /// regardless of processing order. Records only externally visible symbols
  /// (the only cross-TU entities); their `mlirFuncName`/`globalVarSymbolName`
  /// spellings are tag-free, so the pre-scan needs no per-TU tag. Purely
  /// additive: nothing reads `wholeProgram` yet, so it perturbs no import.
  void collectWholeProgramInfo(clang::ASTContext &context, unsigned tuIndex);

  /// W3.3 G4/G5/G6 pre-pass: scans one TU's function bodies for every direct
  /// call to an externally visible function, recording each data-pointer
  /// argument's shape into `WholeProgramInfo::cellSliceParamExtGlobals` /
  /// `cellSliceParamPoisoned` (keyed by callee symbol + parameter index).
  /// Runs in `importCProject`'s pre-import pass over every AST (like
  /// `collectWholeProgramInfo`), so the facts are complete and
  /// order-independent before any TU imports.
  void collectCellSliceCallFacts(clang::ASTContext &context);

  /// W3.3 G4/G5/G6: reduces the raw call-argument facts to the two final
  /// eligibility sets (`cellSliceEligibleParamKeys` /
  /// `cellSliceEligibleGlobals`) once every TU has been scanned. Called once,
  /// after the pre-pass loop and before any TU imports.
  void finalizeCellSliceWholeProgram();

  /// After every translation unit has been imported, checks that no external
  /// symbol was left unresolved: every deferred `extern` global must have a
  /// definition, and no referenced non-variadic external function may remain
  /// body-less (the Rust emitter cannot emit a body-less function); external
  /// functions whose symbol has no uses are erased instead of rejected.
  /// Rejections are located at the symbol's first use site (falling back to
  /// its declaration).
  LogicalResult finalizeProject();

private:
  /// FR-52: whether `finalizeProject` may record an unresolved external
  /// function as a requirement rather than reject it. True for
  /// `ExternalRequirements::Trait`, and for `TraitWhenLibrary` only when the
  /// module defines no `c_main` (i.e. only when the emitted crate will be a
  /// library).
  bool externalRequirementsAllowed();

  /// FR-75: the classify-time approximation of `externalRequirementsAllowed`
  /// — whether a body-less function imported from the CURRENT AST may become
  /// a trait requirement, decided before `finalizeProject` can run. True for
  /// `ExternalRequirements::Trait`; for `TraitWhenLibrary` true iff this
  /// TU's AST defines no `main` (the same predicate `finalizeProject`
  /// applies to the merged module, evaluated per TU — for a multi-TU project
  /// whose `main` lives in ANOTHER TU the two can diverge, and every
  /// divergent outcome is a located rejection, never silent). Always false
  /// under `deferExternals` (FR-57a is untouched by FR-75). Gates the eager
  /// slice classification in `classifyPointerParams`; cached per AST.
  bool classifyTimeTraitEligible();

  /// FR-52: whether `func` — a referenced body-less external — is a shape the
  /// external-requirement trait can express: a free function (not a C++
  /// member), whose address is never taken, and every one of whose symbol
  /// uses is a plain direct `func.call` callee.
  bool isExternalRequirementShape(func::FuncOp func);

  /// FR-70: whether the undefined extern GLOBAL named `symbol` (with the
  /// recorded MLIR value type `type`) is a shape the trait can express as a
  /// getter/setter pair of associated functions: a scalar (integer or
  /// float), whose address is taken in no TU (an AST fact — see
  /// `WholeProgramInfo::addressTakenGlobals`), and every one of whose
  /// surviving IR uses is a direct whole-value `emitrust.global_load` /
  /// `emitrust.global_store`. Everything else keeps the historical
  /// rejection: the refusal shrinks, it never silently mis-emits.
  bool isExternalRequirementGlobalShape(llvm::StringRef symbol, Type type);

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

  /// Imports every supported declaration directly in `context` — a
  /// translation unit, a `namespace { ... }` body, or an `extern "C" {
  /// ... }` body — recursing into a nested `NamespaceDecl` or
  /// `LinkageSpecDecl` member as if it were declared at this level (W2.0
  /// C++ AST tolerance). This is the single per-decl dispatch shared by
  /// the top-level walk and the recursion: a plain C program never
  /// contains either nested-decl-context kind, so its behavior is
  /// unchanged. Member functions/fields of a `CXXRecordDecl` are never
  /// reached this way (they live in the record's own `DeclContext`, never
  /// a sibling of the record in `context->decls()`), which is exactly how
  /// W2.0 leaves method import untouched.
  LogicalResult importDeclsIn(const clang::DeclContext *context);

  /// Imports ONE top-level declaration: the per-kind dispatch that
  /// `importDeclsIn` used to inline. Split out so recoverable import
  /// (FR-42) has a single call it can wrap, checkpoint, and roll back.
  /// `namespace`/`extern "C"` containers are NOT handled here — they are
  /// recursion, not items, and stay in `importDeclsIn` so that each of
  /// their members is recovered individually rather than the whole
  /// container being dropped as one unit.
  LogicalResult importTopLevelDecl(const clang::Decl *decl);

  //===--------------------------------------------------------------------===//
  // Recoverable import (FR-42)
  //===--------------------------------------------------------------------===//

  /// Everything needed to undo a rejected top-level item's effect on the
  /// module. See `rollbackTo` for what is (and is not) restored, and why.
  struct RecoveryCheckpoint {
    /// The last module-body operation that existed BEFORE the item began,
    /// or null when the body was empty. Every operation after it belongs to
    /// the item. Kept as an operation pointer rather than an index so that
    /// an insertion elsewhere in the body cannot shift it; the one way it
    /// could dangle — the item erasing that very operation — is closed by
    /// routing all such erases through `eraseTopLevelOp`.
    Operation *anchor = nullptr;
    /// Clones of the external (body-less) declarations the item erased
    /// while reconciling a redeclaration. Re-inserted on rollback so a call
    /// imported against the prototype before the definition was reached
    /// does not end up referencing a symbol that no longer exists.
    SmallVector<Operation *> erasedExternalClones;
  };

  /// Captures the module state a rejected item must be rolled back to.
  RecoveryCheckpoint checkpointModule();

  /// Undoes a rejected item's module-level effect.
  ///
  /// Erases every `func::FuncOp` the item appended (which is where ALL
  /// half-built IR lives — a function body is the only region the importer
  /// fills incrementally), drops those symbols from `functions`, clears the
  /// per-function scratch state whose `Value`s point into the erased bodies,
  /// and re-inserts the prototypes the item's redeclaration reconciliation
  /// erased.
  ///
  /// Deliberately NOT erased: the `struct_def`/`enum_def`/`global` operations
  /// the item pulled in on demand while mapping its own types. Each of those
  /// is emitted atomically by its own import routine and is tracked by a
  /// name registry (`assignedStructNames`, `importedRecordShapes`,
  /// `globals`, ...) that has no rollback of its own; erasing the operation
  /// while leaving the registry entry would leave a later item referring to
  /// a struct that is no longer defined, which is precisely the silent
  /// corruption recovery exists to avoid. Leaving them costs an unused
  /// definition in the emitted crate, which `#![allow(dead_code)]` already
  /// covers. (The rejected alternative — rolling every registry back too —
  /// was measured against this and rejected: it would have to unwind a
  /// dozen maps plus the anonymous-record counter, and any one of them
  /// missed is a dangling symbol rather than a dead one.)
  void rollbackTo(const RecoveryCheckpoint &checkpoint);

  /// Erases a MODULE-BODY operation, keeping any live recovery checkpoint's
  /// anchor valid. Every erase of a module-level operation performed while
  /// importing an item must go through here; erases of operations nested
  /// inside a function body must not (they can never be the anchor).
  void eraseTopLevelOp(Operation *op);

  /// Resets the per-function scratch state to the same values
  /// `importFunction`'s prologue establishes for a fresh function.
  ///
  /// Recovery needs this because that scratch state holds `Value`s and
  /// `Block *`s pointing INTO the function body being erased: a following
  /// item that read one would dereference freed IR. Normal (non-recovering)
  /// import never needs it — the prologue overwrites every field before the
  /// next body is built — so this is called only from `rollbackTo`, and it
  /// must be kept in step with that prologue.
  void resetPerFunctionState();

  /// Imports one top-level declaration in recovery mode: captures the
  /// importer's own error diagnostics instead of letting them print,
  /// rolls the module back if the item rejected, tries a signature-only
  /// `unimplemented!()` stub when the item was a function, records the
  /// rejection in the ledger, and re-reports it as a WARNING.
  ///
  /// Returns failure only for a rejection that could not be attributed to
  /// this item at all (today: none — every dispatch failure is recoverable),
  /// so `importDeclsIn`'s loop keeps its ordinary `failed(...) -> return
  /// failure()` shape.
  LogicalResult importTopLevelDeclRecovering(const clang::Decl *decl);

  /// FR-53: runs one Pass-A planner step under diagnostic capture and, if it
  /// rejects, attributes the rejection to a single declaration instead of
  /// failing the whole translation unit.
  ///
  /// Called ONLY when `recoverFromRejections` is set — a non-recovering import
  /// never reaches this and keeps the historical
  /// `failed(planner) -> return failure()` shape, diagnostics and exit code
  /// included.
  ///
  /// `body` is the planner fragment whose rejections belong to ONE
  /// declaration. Its `emitError` calls are intercepted (so a recovered
  /// rejection does not print as an error) and the first of them becomes the
  /// ledger's verbatim reason, exactly as `importTopLevelDeclRecovering`
  /// derives it from a failed item import. The attributed declaration is
  /// `attribution`, or — when `body` left `pendingPlannerAttribution` set — the
  /// declaration `body` itself named; a failure with neither is NOT
  /// attributable and is returned as a failure for the caller to propagate.
  ///
  /// \param attribution the declaration to credit when `body` names none.
  /// \param body the planner fragment to run.
  /// \returns which of the three outcomes below occurred.
  enum class PlannerRecovery {
    /// `body` succeeded; the plan it built stands.
    Planned,
    /// `body` rejected and the rejection was recorded against a declaration,
    /// which `importDeclsIn` will drop or stub. A planner that builds
    /// incremental state must replan from scratch after this.
    Recorded,
    /// `body` rejected and no declaration could be credited. The caller must
    /// propagate the failure; the diagnostic has already been re-emitted.
    Unattributable,
  };
  PlannerRecovery
  recoverPlannerRejection(const clang::Decl *attribution,
                          llvm::function_ref<LogicalResult()> body);

  /// FR-43: the item-graph key of `decl` when the current search state
  /// EXCLUDES it, and the empty string otherwise (including when no search
  /// state was set at all).
  ///
  /// The key is computed with exactly the primitives the item graph uses —
  /// `cFunctionSymbolName`, `cGlobalSymbolName`, `recordRustName`, and the
  /// enum's own name, all from `EmitRust/CSymbolNaming.h`, under this TU's
  /// `currentTuTag` — so "the graph node named X" and "the declaration this
  /// returns X for" are the same item by construction, the same way FR-40
  /// makes a node key and an emitted symbol the same thing. Declaration
  /// kinds the graph does not model (C++ member functions, block-scope and
  /// anonymous records, typedefs) have no key and are therefore never
  /// excludable: a search state can only speak about items the graph named,
  /// which is exactly the vocabulary FR-41's coloring and FR-44's report use
  /// as well.
  ///
  /// \param decl the top-level declaration about to be imported.
  /// \returns the excluded item's key, or an empty string.
  std::string frontierExcludedSymbol(const clang::Decl *decl) const;

  /// Emits a stub for a rejected function: a `func::FuncOp` carrying the
  /// real mapped signature whose whole body is
  /// `unimplemented!("<reason>")`. Called by
  /// `importTopLevelDeclRecovering` through `importFunction` (with
  /// `recoveryStubOnly` set), which is what makes the stub's signature the
  /// SAME signature the real import would have built rather than a
  /// re-derived approximation — a stub whose parameter classification
  /// differed from the real one would break every caller it exists to keep
  /// compiling.
  ///
  /// The body is a single `emitrust.call_opaque "unimplemented!"` carrying
  /// the function's result types, so the Rust emitter renders
  /// `let vN: T = unimplemented!("...");` followed by `return vN;` — valid
  /// for every result type because `unimplemented!()` has type `!`. (The
  /// rejected alternative was a module-level `emitrust.verbatim` holding the
  /// whole Rust function text: it renders fine, but it defines no MLIR
  /// symbol, so every `func.call` to it would fail symbol resolution and the
  /// module would not verify — exactly the callers the stub is for.)
  LogicalResult emitRecoveryStub(func::FuncOp funcOp, Location loc);

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
  /// va_list (the target's `__builtin_va_list` and its `__va_list_tag`
  /// record, rejected in any position per C99-37 — Rust has no stable
  /// varargs), fn_ptr component types outside the verifier set, and
  /// everything else produce a located diagnostic.
  FailureOr<Type> mapType(clang::QualType type, Location loc);

  /// W2.3 STL recognition: maps a RecordType decl living in namespace
  /// `std` (per `Decl::isInStdNamespace()`, which transparently unwraps
  /// libstdc++'s `std::__cxx11` inline namespace) BEFORE `mapType`'s
  /// generic RecordType path would recurse into `importRecord` and hit
  /// libstdc++ internals. Recognizes exactly two shapes, without ever
  /// importing a single libstdc++ field:
  ///   * `std::vector<T>` (a `ClassTemplateSpecializationDecl` named
  ///     "vector") -> `!emitrust.opaque<"Vec<<T>>">`, where `<T>` is `T`
  ///     mapped recursively through `mapType` and re-spelled by
  ///     `rustSpellingForElementType`. `T` must land in the supported
  ///     scalar set, a supported struct, or another recognized STL opaque
  ///     (nested containers compose for free); any other `T` is a located
  ///     rejection (propagated from the recursive `mapType` failure, or a
  ///     "not in the supported STL element set" diagnostic when `T` maps
  ///     but has no Rust spelling this function knows).
  ///   * `std::basic_string<char, ...>` (i.e. `std::string`) ->
  ///     `!emitrust.opaque<"String">`. A `basic_string` over any other
  ///     character type is a located rejection.
  /// Every other `std::` entity (map, set, cout, ...) is a located
  /// rejection naming the entity: "unsupported: std::<name> is not a
  /// recognized STL type".
  FailureOr<Type> mapStdLibraryType(const clang::RecordDecl *decl,
                                    Location loc);

  /// Maps a C function-parameter type: data-pointer parameters `T*` become
  /// `!emitrust.mut_ref<T>` for `ParamKind::ScalarRef` and
  /// `!emitrust.mut_ref<!emitrust.slice<T>>` for `ParamKind::Slice`
  /// (array parameters have already decayed to pointers in clang);
  /// function-pointer parameters stay by-value `!emitrust.fn_ptr` values;
  /// everything else maps like `mapType` and ignores `kind`. FR-71:
  /// `voidByteElem`, when non-null, is the byte element the admission scan
  /// proved a `void *` Slice parameter is walked as (see
  /// `voidByteSliceElem`); a `void *` has no pointee of its own to derive
  /// the element from, so it rides in beside `kind`.
  FailureOr<Type> mapParamType(clang::QualType type, Location loc,
                               ParamKind kind,
                               clang::QualType voidByteElem = clang::QualType());

  /// FR-71: the byte element (`char` or `unsigned char`) the admission
  /// scan recorded for the `index`th parameter of `func` — non-null
  /// exactly when that parameter is a `void *` admitted as a byte-slice
  /// cursor. Valid only after `classifyPointerParams(func)` (which fills
  /// the cache); both existing signature builders call it first.
  clang::QualType voidByteSliceElem(const clang::FunctionDecl *func,
                                    unsigned index) const {
    auto it = voidByteElemsCache.find(func->getCanonicalDecl());
    if (it == voidByteElemsCache.end() || index >= it->second.size())
      return clang::QualType();
    return it->second[index];
  }

  /// Returns the Phase-1b classification of every parameter of `func`
  /// (non-pointer parameters report `ScalarRef`, which is ignored). Kinds
  /// derive from the definition's body via `collectSliceParams`; a function
  /// with no definition in the merged ASTs classifies every pointer
  /// parameter as `ScalarRef` (the cross-TU assumption checked at
  /// definition-time signature refinement in `importFunction`) — EXCEPT
  /// under FR-75's trait gate (C mode, `classifyTimeTraitEligible`), where
  /// a body-less function's arithmetic-pointee data-pointer parameters
  /// classify as SLICES eagerly (a requirement signature must carry the C
  /// region contract, not one element) and a `void *` parameter joins by
  /// TU-wide call-site consensus (`voidParamCallSitesAllByteViews`).
  /// Results are cached per canonical declaration.
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

  /// Stage 2 of the owner-struct self-reference extension (design.md
  /// FR-30 follow-on): pure-AST pass, run immediately after `planOwners`
  /// and consuming its `ownerPlans`/`methodPlans` output. For each
  /// promoted owner class with element type `E`, considers every
  /// self-referential data-pointer field of `E` (a field whose pointee is
  /// `E` itself) as a candidate. A field claimed by more than one
  /// promoted owner class is ambiguous and stays unpopulated. For every
  /// remaining candidate, walks every function definition of the TU
  /// (mirroring `planOwners`'s own traversal) collecting every
  /// `MemberExpr` naming the field: a dot-form access, or an arrow access
  /// whose base's root (via `resolveArgRoot`, exactly like `planOwners`'s
  /// own call-edge unification and Stage 1's return-value analysis)
  /// cannot be proven to belong to the class, disqualifies the field; an
  /// assignment additionally requires the same proof of its right-hand
  /// side. A field with every site so proven is populated into
  /// `arrayMemberPtrBindings`; anything else is left absent, falling
  /// through unchanged to the historical `memberPtrBindings`/
  /// `poisonedPtrFields` model. This pass never emits a diagnostic and
  /// never fails: it is strictly additive over `planOwners`'s output.
  void planArrayMemberPointers(const clang::TranslationUnitDecl *unit);

  /// W4.2e Part B (design.md FR-39): proves, per function, the RFC
  /// index-handle node-pool pattern and records a `MallocPoolFacts` +
  /// `poolHandleVars` entries. A function qualifies when it contains a
  /// `malloc(sizeof(struct T))` inside a foldable-trip-count loop (→ cap),
  /// `struct T` has exactly one self-referential data-pointer field, every
  /// `struct T *` local roots in this one pool (bound to the malloc result,
  /// another such local, a self-ref field read, or NULL), nothing escapes
  /// (no node-pointer return, global store, or address-of), and `free` is
  /// applied only to such locals. Runs after `planArrayMemberPointers` and
  /// before record collection so the `next` field's `Option<usize>` type
  /// reaches `mapStructFieldType`. Emits no diagnostic and never fails:
  /// like the owner passes it is additive; an unprovable function is simply
  /// left unpromoted (its node pointers keep the region-driven rejection).
  void planMallocPool(const clang::TranslationUnitDecl *unit);

  /// FR-64: pure-AST recognition of constant-fill C string buffers that lift
  /// to `String::repeat`. For every function definition it scans each local
  /// `char *a = malloc(N+1)`/`calloc(N+1, 1)` bound to a canonical `[0,N)`
  /// constant-ASCII fill loop, a NUL terminator, and only string-consumer uses
  /// (`puts`/`printf("%s")`/`free`), recording a `StringFillFacts` into
  /// `stringFillLocals` and marking the fill loop and NUL store for elision in
  /// `stringFillElidedStmts`. Runs alongside the other Pass-A planners, before
  /// any IR is built. Emits no diagnostic and never fails: a buffer that fails
  /// any clause is simply not recorded and keeps its historical rejection.
  void planStringFill(const clang::TranslationUnitDecl *unit);

  /// FR-64: emits the fused `let a: String = "<fill>".repeat(<count> as usize)`
  /// binding for a recognized constant-fill string local, in place of the
  /// pointer decomposition. Called from `emitLocalVar` when `stringFillLocals`
  /// holds `var`.
  LogicalResult emitStringFillLocal(const clang::VarDecl *var, Location loc);

  /// FR-65: pure-AST recognition of runtime-sized heap buffers of a non-char
  /// scalar element type that lift to an owned `Vec<T>` (the Vec arm of the
  /// {array, Vec, span, Option} representation match). For every function
  /// definition it scans each local `T *a = malloc(n * sizeof(T))`/`calloc(n,
  /// sizeof(T))` whose only uses are `a[i]` indexing and `free(a)`, recording a
  /// `VecFacts` into `vecValueLocals`. Runs alongside the other Pass-A
  /// planners, after `planStringFill` (char buffers are the String domain and
  /// are claimed first). Emits no diagnostic and never fails: a buffer that
  /// fails any clause is simply not recorded and keeps its historical
  /// non-constant-size rejection. Unlike FR-64 there is NO statement elision:
  /// the `a[i] = x` writes are kept as real `Vec` index writes.
  void planVecLift(const clang::TranslationUnitDecl *unit);

  /// FR-65: emits the `let a: Vec<T> = vec![<zero>; <count> as usize]` binding
  /// for a recognized runtime-sized heap buffer, in place of the pointer
  /// decomposition. Called from `emitLocalVar` when `vecValueLocals` holds
  /// `var`.
  LogicalResult emitVecLocal(const clang::VarDecl *var, Location loc);

  /// W2.12: matches the ONE recognized `std::string_view` local
  /// initializer chain — `ImplicitCastExpr<ConstructorConversion>` over
  /// `CXXConstructExpr 'void (const char *)'` over an
  /// `ArrayToPointerDecay`ed ordinary `StringLiteral` (trailing
  /// `CXXDefaultArgExpr`s tolerated) — returning the literal, or null when
  /// the local's initializer is any other shape (absent, from a
  /// std::string, from another view, the (pointer, count) ctor, a wide
  /// literal, ...), in which case `emitLocalVar` falls through to
  /// `mapType`'s located rejection.
  const clang::StringLiteral *
  matchStringViewLiteralInit(const clang::VarDecl *var);

  /// W2.12: decomposes the literal-initialized `std::string_view` local
  /// `var` into (shared read-only literal backing via
  /// `getOrCreateLiteralBacking`, entry i64 cursor cell initialized to 0,
  /// entry i64 len cell initialized to the literal's length WITHOUT the
  /// terminating NUL) and records the triple in `stringViewLocals`. No
  /// string_view value or place is ever materialized.
  LogicalResult emitStringViewLocal(const clang::VarDecl *var,
                                    const clang::StringLiteral *literal,
                                    Location loc);

  /// Folds the trip count of `stmt` when it is a `for`/`while` loop with a
  /// foldable bound (`for (i = A; i < B; i++)`-style, or an equivalent
  /// `while`) into `out`, using `regions.evalFoldableInt` (W4.2e Part B).
  /// Returns false for any loop whose count is not a compile-time constant.
  bool foldLoopTripCount(PointerRegionAnalysis &regions,
                         const clang::Stmt *stmt, uint64_t &out) const;

  /// Requests the one-per-module emission of the `Option<usize>` pool-handle
  /// helper functions (`__emitrust_pool_opt`/`__emitrust_pool_unpack`,
  /// W4.2e Part B); emitted via `emitFileHelpers`' verbatim mechanism.
  void requestPoolHelpers();

  /// Lowers a node-pool handle assignment `ptr = rhs` (W4.2e Part B): a
  /// `malloc` append (index = free cursor, non-null = true, cursor++), a
  /// NULL binding (non-null = false), a copy of another handle, or a
  /// destructured self-referential field read (`c = c->next`).
  LogicalResult storePoolHandleAssign(Location loc, const clang::VarDecl *ptr,
                                      const PointerLocalInfo &info,
                                      const clang::Expr *rhs);

  /// Returns the `h->next` arrow member read `expr` names when `h` is a pool
  /// handle and the field is its pool's self-ref field, else null (W4.2e
  /// Part B).
  const clang::MemberExpr *asPoolNextFieldRead(const clang::Expr *expr) const;

  /// Loads the `Option<usize>` value of a pool handle's self-ref field
  /// (`member` is `h->next`): subscripts the pool at the handle's index and
  /// projects the field (W4.2e Part B).
  FailureOr<Value> emitPoolNextFieldRead(const clang::MemberExpr *member,
                                         Location loc);

  /// Lowers `h->next = rhs` on a pool handle base (W4.2e Part B): builds the
  /// field's `Option<usize>` from the right-hand handle's (non-null, index)
  /// pair (via `__emitrust_pool_opt`) and assigns it. Returns success only
  /// when it handled the assignment; the caller falls through otherwise.
  LogicalResult emitPoolNextFieldAssign(const clang::MemberExpr *member,
                                        const clang::Expr *rhs, Location loc);

  /// Side-effect-free AST mirror of `emitPointerRValue`'s base resolution:
  /// returns the single object a pointer-typed call argument points into (a
  /// local array or scalar, or a pointer parameter of the calling
  /// function), or null when no single root is statically known. `regions`
  /// is the calling function's per-body analysis, consulted for pointer
  /// locals.
  const clang::VarDecl *resolveArgRoot(PointerRegionAnalysis &regions,
                                       const clang::Expr *expr) const;

  /// Returns whether `root` — a `resolveArgRoot` result at Pass-A planning
  /// time, or a `PtrExprValue::base` at emission time (the same VarDecl
  /// identity space: either is "the declaration a pointer decomposition's
  /// region roots at") — is provably an element of `ownerArray`'s class:
  /// either the array itself (reachable only from the owning function via
  /// `&arr[i]`), or a pointer parameter of a function `planOwners` already
  /// qualified as one of `ownerArray`'s methods — every data-pointer
  /// parameter of such a function is, by `planOwners`'s own all-or-nothing
  /// invariant, unified into that exact class. Shared by
  /// `planArrayMemberPointers` (which proves every site ahead of time) and
  /// `emitArrayMemberPointerAssign` (whose defensive re-check mirrors the
  /// same class membership test the proof already ran, since a pointer
  /// parameter's `PtrExprValue::base` is the PARAMETER declaration itself,
  /// not the owner array's declaration — a bare pointer-equality check
  /// against `ownerArray` would reject every method-parameter-rooted
  /// value).
  bool isArrayMemberOwnerRoot(const clang::VarDecl *root,
                              const clang::VarDecl *ownerArray) const;

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

  /// True when the whole-program facts prove function `fn`'s data-pointer
  /// parameter `paramIndex` may join a cell-slice class despite `fn`'s
  /// external linkage (consulted by `planCellSlices` to lift its :612/:751
  /// gates). Meaningful only in a multi-file import; `wholeProgram` is empty
  /// otherwise.
  bool cellSliceParamEligibleWholeProgram(const clang::FunctionDecl *fn,
                                          unsigned paramIndex) const;

  /// True when the externally visible global `symbol` may stay on the
  /// cell-slice path despite its external linkage (consulted by
  /// `planCellSlices` to lift its :724 gate).
  bool cellSliceGlobalEligibleWholeProgram(llvm::StringRef symbol) const;

  /// Number of distinct TUs across the whole program that take the address of
  /// a function whose canonical return type is `returnType` (a data pointer).
  /// The G1 gate uses this to decide whether this TU's per-TU
  /// `addressTakenFunctions` candidate set is the complete whole-program set:
  /// 0 or 1 means complete (run the classifier), ≥2 means a diverging
  /// candidate may live in an unseen TU (keep the blanket rejection).
  /// Reads `WholeProgramInfo::dataPtrReturnFnAddressTakenTus`; returns 0 for a
  /// single-file import (`wholeProgram` is empty).
  unsigned dataPtrReturnFnAddressTakenTuCount(clang::QualType returnType) const;

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
  // Array-member pointers (Stage 2 of the owner-struct self-reference
  // extension)
  //===--------------------------------------------------------------------===//

  /// Returns the `!emitrust.enum` type backing `field`'s storage,
  /// synthesizing its module-level `emitrust.enum_def` (one variant `E0`
  /// .. `E{elementCount-1}` per array index) on first need and memoizing
  /// the symbol into `facts.enumSymbol`. The synthesized name
  /// (`<Record>_<Field>_Bases`) is collision-checked against the module
  /// symbol table, mirroring `emitOwnerLocal`'s synthesize-on-first-need
  /// pattern.
  FailureOr<Type>
  getOrCreateArrayMemberEnumType(const clang::FieldDecl *field,
                                 ArrayMemberPointerFacts &facts, Location loc);

  /// Emits the READ decode of an array-member-pointer field already
  /// proven usable (`arrayMemberPtrBindings` has a populated entry for
  /// `field`): resolves `member`'s (arrow-only) base to the array
  /// element's place, loads the field's enum value, and decodes it to an
  /// i64 element index via `castEnumToI32` widened to i64 — the field's
  /// enum storage IS the index by construction, so no branching is
  /// needed. The result's `PtrExprValue::base` is the arrow base
  /// expression's OWN resolved base (a method's own pointer parameter, not
  /// `facts.ownerArray`), so it compares equal to a plain read of that
  /// same base expression (see the implementation's doc comment).
  FailureOr<PtrExprValue>
  emitArrayMemberPointerRead(const clang::MemberExpr *member,
                             const clang::FieldDecl *field,
                             ArrayMemberPointerFacts &facts, Location loc);

  /// Emits `x->field = rhs` on an array-member-pointer field already
  /// proven usable: resolves `rhs` to its (base, cursor) decomposition
  /// (proven by Pass A to root in the same owner array), then encodes the
  /// i64 cursor into the field's enum storage through an `emitrust.switch`
  /// over the cursor value (one case per array index assigning that
  /// index's enum constant, plus a default region assigning `E0` that
  /// Pass A's proof makes unreachable — mirroring `IndexSwitchLowering`'s
  /// default-region convention) — a genuine Rust `match`, not a bare
  /// transmute, so the enum's closed variant set stays visibly exhaustive
  /// at every write site.
  LogicalResult
  emitArrayMemberPointerAssign(const clang::MemberExpr *member,
                               const clang::FieldDecl *field,
                               ArrayMemberPointerFacts &facts,
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
  ///
  /// A record whose import FAILED is remembered (`rejectedRecords`) and every
  /// later attempt to materialize it fails again, at the new use site. This
  /// is not an optimization: without it a rejected record still hands back an
  /// emitted name, because `importedRecords` is marked before the field walk
  /// runs, and `mapType` then builds an `!emitrust.struct<"S">` for a struct
  /// nobody defines. Under `recover` — where a rejected record is DROPPED
  /// rather than aborting the run — that produced a crate referring to a
  /// missing Rust type, i.e. one that does not compile (FR-50). Failing again
  /// instead propagates the rejection to whoever named the type, which is
  /// exactly the `Field`/`SigType`/`BodyType` poisoning FR-41 predicts.
  LogicalResult importRecord(const clang::RecordDecl *record, Location loc);

  /// `importRecord`'s body, without the rejection memo around it. Every exit
  /// is a verdict on THIS record, which is what makes the wrapper's "remember
  /// the failure" correct.
  LogicalResult importRecordUncached(const clang::RecordDecl *definition);

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
  /// both arms are same-width scalars — integers differing only in
  /// signedness reinterpret the slot bit-exactly via `emitrust.cast`
  /// (two's complement, C99 6.5.2.3), and a float paired with a
  /// same-width integer (float/32-bit int, double/64-bit int)
  /// reinterprets via `emitrust.bitcast` (`to_bits`/`from_bits`) — or
  /// when the arm is a constant integer ARRAY whose total width equals
  /// the integer slot's (CTS-F, 00210): such a byte-array arm is a
  /// TYPE-level concession recorded in `unionByteArrayArms`, and every
  /// access through it rejects at the access site. Bit-field arms,
  /// unnamed/anonymous arms, pointer arms, scalar arms of differing
  /// sizes, aggregate/enum arms that do not match the slot's type
  /// exactly, and empty unions are rejected with located
  /// `unsupported: union ...` diagnostics at the union definition.
  LogicalResult collectUnionSlot(const clang::RecordDecl *definition,
                                 SmallVectorImpl<llvm::StringRef> &fieldNames,
                                 SmallVectorImpl<Type> &fieldTypes);

  /// Returns the field that provides `field`'s storage in its flattened
  /// parent struct_def: the aliased first-arm slot for a union arm
  /// recorded by `collectUnionSlot`/`collectRecordFields`, or `field`
  /// itself.
  const clang::FieldDecl *
  flattenedFieldStorage(const clang::FieldDecl *field) const;

  /// Reinterprets `value` bit-exactly as `target`: a no-op when the
  /// types already match, an `emitrust.bitcast` (`to_bits`/`from_bits`)
  /// when either side is a float type, and a same-width `emitrust.cast`
  /// (bit-exact on two's complement) otherwise. The union pun helpers
  /// below and the pun-arm initializer path share this as the single
  /// slot<->arm reinterpretation primitive.
  Value reinterpretScalarBits(Location loc, Value value, Type target);

  /// If `expr` (modulo parens and implicit trivia) reads a union arm
  /// whose mapped type differs from its storage slot's — a signedness or
  /// float pun admitted by `collectUnionSlot` — reinterprets `value`
  /// (the loaded slot value) bit-exactly as the arm's own mapped type
  /// (`reinterpretScalarBits`); otherwise returns `value` unchanged.
  FailureOr<Value> reinterpretUnionArmRead(const clang::Expr *expr,
                                           Value value, Location loc);

  /// If the assignment target `expr` is a union arm whose mapped type
  /// differs from its storage slot's, reinterprets `value` (the assigned
  /// value, of the arm's type) bit-exactly to the slot's type so the
  /// store lands on the slot; otherwise returns `value` unchanged.
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

  /// Recursive walk `collectOrdinaryNames` delegates to: scans every
  /// declaration directly in `context`, recursing into a nested
  /// `NamespaceDecl` or `LinkageSpecDecl` (`extern "C" { ... }`) exactly as
  /// `importDeclsIn` does for the real import, so the pre-scanned name set
  /// matches what will actually be emitted (W2.0).
  void collectOrdinaryNamesFrom(const clang::DeclContext *context);

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
  /// extras in `emitCall` (CTS-P9) — while a va_list-using definition
  /// emits its planned monomorphization clones (CTS 00204,
  /// `emitVaClones`) or, without a plan, is rejected. A
  /// body-less prototype with no definition in this TU is skipped when
  /// nothing in this TU references it (referenced-only policy). A body
  /// replaces a previously imported body-less declaration of the same name.
  /// Block-scope prototypes (which C gives external linkage) are imported
  /// through this same path by `emitStmt`; the module-scope insertion point
  /// is guarded, so a mid-body call leaves the caller's insertion point
  /// untouched.
  /// FR-47: `signatureOnly` imports the SIGNATURE ONLY, as if `func` were a
  /// body-less prototype, even when this declaration does have a body — the
  /// declare-then-define prepass `importCXXMethods` needs so that a C++
  /// class's methods can refer to one another regardless of declaration
  /// order (a member function body is a complete-class context in C++,
  /// unlike C's strictly-preceding-declaration rule this importer was built
  /// around). The definition pass then calls `importFunction` again with
  /// the flag clear, and the ordinary redeclaration reconciliation below
  /// erases the external stub and rebuilds it with the body.
  LogicalResult importFunction(const clang::FunctionDecl *func,
                               bool signatureOnly = false);

  /// W2.2: imports every user-declared, non-virtual, non-deleted method of
  /// `record` — plain methods, const methods, static methods, and
  /// non-delegating, non-copy/move constructors — onto the
  /// `emitrust.impl`/`emitrust.method_of` surface via `importFunction`.
  /// Implicitly-defined special members (default ctor/dtor/copy/move the
  /// class did not declare) are skipped. A destructor, virtual method, or
  /// overloaded operator is rejected earlier, in `collectRecordFields`
  /// (before any field — or method — of the class imports), so none of
  /// those three shapes ever reaches this walk.
  ///
  /// FR-47: runs in TWO passes over the same method set — every signature
  /// first (`importFunction(method, /*signatureOnly=*/true)`), then every
  /// body — so a method may call any sibling regardless of declaration
  /// order, matching C++'s complete-class context. See the definition for
  /// the alternatives rejected.
  LogicalResult importCXXMethods(const clang::CXXRecordDecl *record);

  /// W2.2: the per-(class, overload-signature) mangled `func.func`/
  /// `emitrust.impl` symbol name for `method`:
  /// `<StructName>_<methodBaseName>[_<overloadSuffix>]`. `<StructName>` is
  /// the class's already-assigned emitrust struct name (`assignedStructNames`,
  /// set by `structSymbolName` before any of its methods import).
  /// `<methodBaseName>` is `"new"` for a constructor (whose
  /// `DeclarationName` has no ordinary identifier spelling) or the method's
  /// C++ name mangled through `mangleMemberName`, exactly like a struct
  /// field. `<overloadSuffix>` is present only when the class declares more
  /// than one method (or constructor) sharing the same base name: it is the
  /// declaration-order concatenation of each parameter's overload type code
  /// (`cxxOverloadParamCode`) — empty for a zero-parameter member of an
  /// overload set, which then keeps the bare `<StructName>_<methodBaseName>`
  /// spelling. This is the SAME name used for the imported `func.func`
  /// symbol, every `method_call`/`call_opaque` call-site reference to it,
  /// and constructor lookup, so all three always agree by construction
  /// (deliberately decoupled from `mlirFuncName`'s per-TU static-storage-
  /// class tag, which C++ methods must never pick up: a method and a C
  /// file-static function share `clang::SC_Static` for unrelated reasons).
  std::string cxxMethodMangledName(const clang::CXXMethodDecl *method) const;

  /// W2.2: imports a `CXXMemberCallExpr` (`obj.method(args)` /
  /// `obj->method(args)`) as an `emitrust.method_call` on the (possibly
  /// const) receiver place, mirroring the Phase-4 method-call lowering
  /// (`emitMethodCallSite`) but for a genuine object expression rather than
  /// a promoted owner place.
  FailureOr<Value> emitCXXMemberCall(const clang::CXXMemberCallExpr *call);

  /// W2.3 STL recognition: whether `type` is a recognized STL opaque type
  /// (an `!emitrust.opaque` whose value is exactly "String" or begins with
  /// "Vec<" or, since W2.11, "Option<") — the set `mapStdLibraryType` ever
  /// produces.
  static bool isStlOpaqueType(Type type);

  /// W2.3: the Rust spelling of a MAPPED element type `T` for composing
  /// `"Vec<" + spelling + ">"` — the inverse of `parseStlElementType`.
  /// Handles exactly the supported vector-element set: signed/unsigned
  /// integers of width 8/16/32/64, `i1`->"bool", f32, f64, an
  /// `!emitrust.struct` by its bare name, and a nested recognized STL
  /// opaque by its own spelling verbatim (so `Vec<Vec<i32>>`/`Vec<String>`
  /// compose for free through the same recursive `mapType` call). Returns
  /// `std::nullopt` for anything else (an enum, a pointer/fn_ptr/array —
  /// none of which reach here anyway since `mapType` would have already
  /// rejected them as `T` before this function runs).
  static std::optional<std::string> rustSpellingForElementType(Type type);

  /// W2.3: the inverse of `rustSpellingForElementType` — reconstructs the
  /// MLIR element type from a `Vec<...>` opaque's inner spelling (needed at
  /// `operator[]`/`at()` sites, which must produce a genuinely typed
  /// element place — an `i32` lvalue, say — rather than another opaque
  /// string). A closed, controlled round-trip: the only spellings ever
  /// seen are ones `rustSpellingForElementType` itself produced. Returns a
  /// null `Type` if `spelling` matches none of the known forms.
  Type parseStlElementType(llvm::StringRef spelling);

  /// W2.3: imports a `CXXMemberCallExpr` whose method is declared in
  /// namespace `std` (i.e. `std::vector<T>`/`std::string`'s own inherent
  /// methods) — intercepted at the top of `emitCXXMemberCall` before the
  /// generic imported-method lookup, which would never find one (no
  /// libstdc++ method is ever imported). Dispatches the PINNED method
  /// table by receiver opaque spelling and method name (design.md's STL
  /// section is the single source of truth for the table); anything else
  /// is a located rejection naming the receiver type and method.
  FailureOr<Value> emitStlMemberCall(const clang::CXXMemberCallExpr *call);

  /// W2.3: imports a `CXXOperatorCallExpr` whose resolved operator method
  /// is declared in namespace `std` — `v[i]` (`OO_Subscript`, read
  /// position only) and `s1 += s2` / `s += 'c'` (`OO_PlusEqual`).
  /// Intercepted in `emitCall` before the ordinary direct-callee dispatch,
  /// which a `CXXOperatorCallExpr` (itself a `CallExpr`) would otherwise
  /// reach and reject as a call to an unimported function.
  FailureOr<Value> emitStlOperatorCall(const clang::CXXOperatorCallExpr *call);

  /// W2.3: shared `operator[]`/`at()` PLACE implementation over a `Vec<T>`
  /// receiver: reconstructs `T` from the opaque's spelling (via
  /// `parseStlElementType`) and builds an `emitrust.subscript` place over
  /// `receiver` at `idxExpr`'s value. Two call sites load this place into a
  /// value (the `emitCall`-reached statement-discard shapes in
  /// `emitStlMemberCall`/`emitStlOperatorCall`); a third, `emitLValue`'s own
  /// `CXXOperatorCallExpr`/`CXXMemberCallExpr` cases (the path a scalar
  /// VALUE read — `int x = v[i];` — actually takes, since `operator[]`/
  /// `at()` return `T&`, always wrapped in an `CK_LValueToRValue` cast whose
  /// `emitLValue(sub)` call reaches here directly), consumes the place
  /// as-is. Read position only — there is no assignment-target
  /// (`v[i] = x`) support this wave.
  FailureOr<Value> emitStlVectorIndexPlace(Value receiver,
                                           emitrust::OpaqueType vectorType,
                                           const clang::Expr *idxExpr,
                                           Location loc,
                                           llvm::StringRef opName);

  /// W2.6: `front()`/`back()` PLACE implementation over a `Vec<T>`
  /// receiver: the `emitrust.subscript` place at index-typed 0 (`v[0]`)
  /// or `len - 1` (`v[v.len() - 1]`; index-typed operands render bare, no
  /// `as usize`). C++ front/back on an empty vector is UB; Rust's index
  /// panic (or the usize-underflow panic feeding `len - 1`) is a safe
  /// refinement. Same two consumer shapes as `emitStlVectorIndexPlace`:
  /// loaded by `emitStlMemberCall`'s statement-discard path, consumed
  /// as-is by `emitLValue`'s `CXXMemberCallExpr` case (the path a scalar
  /// value read takes, since both return `T&` behind `CK_LValueToRValue`).
  FailureOr<Value> emitStlVectorEndPlace(Value receiver,
                                         emitrust::OpaqueType vectorType,
                                         bool isFront, Location loc);

  /// W2.7: whether `type` is a `std::array<T, N>` specialization (the one
  /// std-namespace record that maps to a non-opaque type,
  /// `!emitrust.array<NxT>`; see mapStdLibraryType). Used where the
  /// CLANG-side shape needs distinguishing — the aggregate initializer's
  /// struct-wrapper peel — since the mapped type alone is
  /// indistinguishable from a C array's.
  bool isStdArrayRecordType(clang::QualType type);

  /// W2.8: whether `type` is a `std::pair<T1, T2>` specialization (which
  /// imports as a synthesized real struct; see mapStdLibraryType's pair
  /// case).
  bool isStdPairRecordType(clang::QualType type);

  /// W2.11: whether `type` is a `std::optional<T>` specialization (which
  /// maps to the `!emitrust.opaque<"Option<S>">` family; see
  /// mapStdLibraryType's optional case). Used where the CLANG-side shape
  /// needs distinguishing: `emitRValue`'s value-position `CXXConstructExpr`
  /// routing, which must intercept optional construction (`return v;` /
  /// `return std::nullopt;`) before the generic trivial-copy unwrap.
  bool isStdOptionalRecordType(clang::QualType type);

  /// W2.12: whether `type` is a `std::basic_string_view` specialization
  /// (`std::string_view` is its char typedef). string_view has NO type
  /// mapping — a literal-initialized LOCAL decomposes at `emitLocalVar`
  /// into (shared literal backing, cursor cell, len cell; see
  /// `emitStringViewLocal`), and every other string_view position keeps
  /// the mapType-tail rejection.
  bool isStdStringViewRecordType(clang::QualType type);

  /// W2.14: whether `type` is a `std::variant<...>` specialization (which
  /// maps to a synthesized closed data enum; see mapStdLibraryType's
  /// variant case). Used where the CLANG-side shape needs distinguishing:
  /// `emitRValue`'s value-position `CXXConstructExpr` routing (before the
  /// generic trivial-copy unwrap) and the std free-function interceptions
  /// (`std::get`/`std::holds_alternative`), which must not disturb the
  /// same-named functions over pairs/tuples/arrays.
  bool isStdVariantRecordType(clang::QualType type);

  /// W2.12: imports a `CXXMemberCallExpr` whose receiver is a decomposed
  /// string_view local `var` (a `stringViewLocals` entry) — intercepted at
  /// the top of `emitStlMemberCall` BEFORE the receiver place emission,
  /// since a decomposed local has no place of its own. Recognized:
  /// size() (len-cell load, cast to the call's declared C type per the
  /// emitLenCall convention) and remove_prefix(n) (cursor += n; len -= n;
  /// statement position). Anything else is a located rejection naming the
  /// entity.
  FailureOr<Value> emitStringViewMemberCall(const clang::CXXMemberCallExpr *call,
                                            const clang::VarDecl *var,
                                            Location loc);

  /// W2.12: the byte PLACE of `sv[i]` over a decomposed string_view local
  /// `var`: the shared literal backing subscripted at cursor + i, typed
  /// `!emitrust.lvalue<i8>` (C `char` semantics). Two consumer shapes,
  /// mirroring `emitStlVectorIndexPlace`: loaded by `emitStlOperatorCall`'s
  /// statement-discard path, consumed as-is by `emitLValue`'s
  /// `CXXOperatorCallExpr` case (the path a scalar value read takes, since
  /// string_view's operator[] returns `const char&` behind
  /// `CK_LValueToRValue`). Read position only — the backing is const.
  FailureOr<Value> emitStringViewIndexPlace(const clang::VarDecl *var,
                                            const clang::Expr *idxExpr,
                                            Location loc);

  /// W2.8: field-wise import of std::pair's two-argument value
  /// constructor onto `place` (`.first = arg0; .second = arg1;`),
  /// mirroring emitDefaultConstructInit's member-place shape. No libc++
  /// constructor body is ever imported.
  LogicalResult emitPairConstructInit(Value place,
                                      const clang::CXXConstructExpr *construct,
                                      Location loc);

  /// W2.9: by-value structured binding (`auto [a, b] = src;`): the
  /// holding copy materializes into an anonymous place and each
  /// BindingDecl becomes an ordinary scalar local initialized from the
  /// zipped field (struct sources) or element (array sources), registered
  /// in `symbols` so every later reference resolves like a plain local.
  /// Reference forms and tuple-like-protocol user types stay rejected.
  LogicalResult
  emitDecompositionDecl(const clang::DecompositionDecl *decomp);

  /// W2.13: by-value-capture lambda local via LAMBDA LIFTING
  /// (`auto f = [a, b](int x) {...};`, intercepted in the DeclStmt walk
  /// BEFORE the VarDecl's type conversion so the closure record never
  /// reaches aggregate import). Recognizer gate: explicit by-value
  /// captures of SCALAR locals only, non-mutable, non-generic, and every
  /// use of the lambda local in the enclosing body is a direct operator()
  /// call; anything else is a located rejection. On acceptance, freezes
  /// each capture (one load at the declaration point) and hands off to
  /// `importLiftedLambda`.
  LogicalResult emitLambdaLocal(const clang::VarDecl *var,
                                const clang::LambdaExpr *lambda);

  /// W2.13: creates the lifted module-level FuncOp for a recognized
  /// lambda local — the captures PREPENDED as parameters ahead of the
  /// operator()'s own — under the block-scope `<function>_<name>` mangle,
  /// registers it in `lambdaLocals` (with the frozen capture values) for
  /// the call rewrite, and queues the body import on
  /// `pendingLiftedLambdas`.
  LogicalResult importLiftedLambda(const clang::VarDecl *var,
                                   const clang::LambdaExpr *lambda,
                                   SmallVector<Value, 4> frozenCaptures,
                                   SmallVector<const clang::VarDecl *, 4>
                                       captures,
                                   Location loc);

  /// W2.13: drains `pendingLiftedLambdas` at the end of `importFunction`
  /// (a lambda inside a lifted body re-queues, so this loops until
  /// empty).
  LogicalResult importPendingLiftedLambdas();

  /// W2.13: imports one lifted lambda's body: resets the per-function
  /// emission state (mirroring `emitVaClone`'s secondary prologue), binds
  /// the capture VarDecls AND the operator()'s ParmVarDecls to the entry
  /// block arguments — the operator() body's DeclRefExprs point at the
  /// ENCLOSING VarDecls, not closure fields, so binding the captured
  /// decls themselves is the whole rewrite — then reuses the shared body
  /// emitter. `importFunction` cannot be reused verbatim: the prepended
  /// capture parameters exist in no FunctionDecl.
  struct PendingLiftedLambda; // defined with the queue below
  LogicalResult importLiftedLambdaBody(const PendingLiftedLambda &pending);

  /// W2.13: binds a lifted lambda's capture (a non-Parm VarDecl, so
  /// `bindOrdinaryParam` cannot take it) to its prepended entry-block
  /// argument, mirroring bindOrdinaryParam's scalar/place branches.
  LogicalResult bindLiftedCaptureValue(const clang::VarDecl *var,
                                       Value blockArg, Location loc);

  /// W2.13: rewrites a direct `f(args)` operator() call on a registered
  /// lifted-lambda local to `lifted(frozen..., args...)`.
  FailureOr<Value>
  emitLambdaLocalCall(const clang::CXXOperatorCallExpr *call,
                      const clang::VarDecl *var);

  /// W2.10: ranged-for over a recognized container local (Vec<T> opaque
  /// or std::array-mapped !emitrust.array), lowered to a len()-bounded
  /// counted CFG loop. Restrictions (the R3 end-evaluation mitigation):
  /// the range must be a bare local DeclRefExpr and the body may not name
  /// the range variable, so the length is loop-invariant by construction.
  /// A by-value loop variable is a fresh per-iteration copy; a reference
  /// loop variable binds directly to the per-iteration element place.
  LogicalResult emitCXXForRangeStmt(const clang::CXXForRangeStmt *stmt);

  /// W2.3: imports the (possibly implicit) `CXXConstructExpr` initializing
  /// a local of a recognized STL opaque type `stlType`. Supports exactly:
  /// a zero-argument default construction (`Vec::new()` / `String::new()`)
  /// and, for `String` only, a single ordinary-string-literal argument (the
  /// `const char*` conversion constructor, `String::from("literal")`).
  /// Copy/move construction, the sized/fill vector constructor
  /// (`std::vector<T>(n)`), and initializer-list construction are located
  /// rejections this wave (design.md's STL OUT list).
  FailureOr<Value> emitStlConstruct(Type stlType,
                                    const clang::CXXConstructExpr *construct,
                                    Location loc);

  /// W2.14: imports the `CXXConstructExpr` producing a std::variant VALUE
  /// (`variantType` is the mapped `!emitrust.data_enum`). Supports exactly:
  /// the default construction — C++17 [variant.ctor]p2 value-initializes
  /// the FIRST alternative, emitted as the EXPLICIT `V0 { 0 }` image (a
  /// data enum deliberately derives no Default, so the emitter's
  /// default-value path must never see it) — and the converting ctor from
  /// an alternative value, selected by EXACT mapped-type equality (clang
  /// already materialized any implicit conversion in the AST). Copy/move
  /// construction and every other shape (in_place tags, ...) are located
  /// rejections this wave.
  FailureOr<Value>
  emitVariantConstruct(Type variantType,
                       const clang::CXXConstructExpr *construct, Location loc);

  /// W2.14: the alternative index (0 or 1) of `altType` in the synthesized
  /// variant enum `enumType`, or std::nullopt when `altType` is not an
  /// alternative (selection is by exact mapped-type equality).
  std::optional<unsigned> variantAltIndex(emitrust::DataEnumType enumType,
                                          Type altType);

  /// W2.14: constructs the `emitrust.enum_variant` value tagging `payload`
  /// as alternative `index` (0 -> "V0", 1 -> "V1") of `enumType`.
  Value createVariantValue(Location loc, emitrust::DataEnumType enumType,
                           unsigned index, Value payload);

  /// W2.14: creates a two-arm RESULT-mode (or, with a null `resultType`,
  /// statement-mode) `emitrust.match` over `scrutinee` — exhaustive by
  /// construction, one case per declared variant in order. `buildArm` is
  /// invoked once per arm with the builder positioned inside that arm's
  /// fresh block (its payload bound as the block argument) and must
  /// terminate it with an `emitrust.yield`.
  emitrust::MatchOp createVariantMatch(
      Location loc, Value scrutinee, emitrust::DataEnumType enumType,
      Type resultType,
      llvm::function_ref<void(unsigned index, Value payload)> buildArm);

  /// W2.14: matches a `std::get<T>(v)` free-function call over a
  /// recognized std::variant argument (any other callee, arity, or
  /// argument record shape returns null). Shared by `emitCall`'s std
  /// free-function interception and the `CK_LValueToRValue` read path —
  /// std::get returns `T&`, so the value read of that reference IS the
  /// match expansion and no place for the result ever exists.
  const clang::CallExpr *matchVariantGetCall(const clang::Expr *e);

  /// W2.14: expands `std::get<T>(v)` over a recognized std::variant local
  /// into a RESULT-mode match yielding the held payload in T's arm and
  /// diverging through the `panic!` image in the other (the corpus only
  /// gets the held alternative; catch is unsupported, so no program can
  /// observe the C++ bad_variant_access instead — a behavior-compatible
  /// refinement for the supported subset). The index form `std::get<0>`
  /// stays a located rejection.
  FailureOr<Value> emitVariantGet(const clang::CallExpr *call);

  /// W2.2: lowers `place`'s initialization from a non-trivial
  /// `CXXConstructExpr` by invoking the matching constructor method
  /// (imported as an ordinary `&mut self` method by `importCXXMethods`,
  /// named via `cxxMethodMangledName`) on `&mut place`, discarding its
  /// (void) result. `place` is already default-constructed (an
  /// `emitrust.variable` of the struct type) by the caller.
  LogicalResult emitCXXConstructInit(Value place,
                                     const clang::CXXConstructExpr *construct,
                                     Location loc);

  /// Initializes `place` from a default construction whose constructor is not
  /// user-provided (an implicit or `= default` default ctor made non-trivial
  /// only by in-class member initializers). Applies each of the ctor's member
  /// initializers to the matching field of `place` directly — an NSDMI's
  /// `CXXDefaultInitExpr` resolves through `emitRValue` to the in-class
  /// initializer — instead of calling a constructor function that was never
  /// imported. A member with no initializer keeps `place`'s default.
  LogicalResult emitDefaultConstructInit(Value place,
                                         const clang::CXXConstructorDecl *ctor,
                                         Location loc);

  /// CTS 00204 Pass A: plans the per-call-site monomorphization of every
  /// variadic definition whose body uses va_list. Scope checks reject
  /// va_copy, a va_list object escaping its definition (passed to any
  /// callee — checked BEFORE the callee imports its va_list parameter
  /// rejection), and taking the address of such a definition; the plan
  /// then enumerates every direct call site and assigns one clone per
  /// distinct extras signature. Runs before any declaration imports.
  LogicalResult planVaMonomorph(const clang::TranslationUnitDecl *unit);

  /// One whole-TU va-monomorphization planning attempt, skipping every
  /// declaration already in `plannerRejections`. This IS the historical
  /// `planVaMonomorph` body; under recovery `planVaMonomorph` re-runs it until
  /// it succeeds, adding one attributed rejection per round.
  ///
  /// A rejection sets `pendingPlannerAttribution` to the top-level declaration
  /// it belongs to: the variadic definition itself for the scope checks
  /// (va_copy, an escaping va_list), and the declaration whose body or
  /// initializer CONTAINS the offending construct for the address-of scan and
  /// the per-call-site clone assignment. Dropping the containing declaration
  /// removes the construct; dropping the variadic definition additionally
  /// removes it from the plan entirely, which is what makes every surviving
  /// caller reject in turn (`emitCall`'s "call to a variadic function") rather
  /// than call a clone that was never emitted.
  LogicalResult planVaMonomorphOnce(const clang::TranslationUnitDecl *unit);

  /// CTS 00204 Pass A: plans the `const char **` string-cursor parameters
  /// of this TU. A pointer-to-pointer parameter of a definition qualifies
  /// when the body only ever reads through `*s` and advances it with
  /// `*s = <pointer expr>`; every other use (the parameter escaping into
  /// a global, another call, deeper writes) is a located rejection.
  ///
  /// FR-53: under recovery each definition is planned in isolation and its
  /// rejection is credited to it, so one unsupported `char **` writer costs
  /// that definition and nothing else. This is the planner that dominated the
  /// third-party measurement — it alone accounted for most of the translation
  /// units that used to emit no crate at all.
  LogicalResult planCursorParams(const clang::TranslationUnitDecl *unit);

  /// The per-definition half of `planCursorParams`: proves `func`'s
  /// pointer-to-pointer parameters fit the cursor-parameter shape and, only
  /// if ALL of them do, admits them into `cursorParams`.
  ///
  /// The all-or-nothing ordering is what makes this function the unit of
  /// FR-53 recovery: a rejection leaves `cursorParams` exactly as it found it,
  /// so a dropped definition contributes no half-plan for a survivor to read.
  /// A caller of a dropped definition sees NO cursor plan for its parameters,
  /// which is the pre-CTS-00204 default and rejects the `char **` argument at
  /// the call site — recovered in turn — rather than mis-lowering it.
  LogicalResult planCursorParamsFor(const clang::FunctionDecl *func);

  /// C99-43 C3: classifies every use of C `main`'s `char **argv`
  /// parameter against the admitted read grammar — whole-value `argv[i]`
  /// as a direct `printf` `%s`/`%.Ns` argument (no field width) and
  /// `argv[i][j]` byte reads consumed as VALUES — and, when EVERY use is
  /// admitted, records the parameter in `mainArgvAdmittedParam` so the
  /// signature import adds the `!emitrust.argv_table` input. NEVER
  /// rejects: any use outside the grammar (stores, escapes to other
  /// calls, address-of, pointer arithmetic like `argv++`/`*argv`,
  /// writes, pointer-value tests like `argv[0] != 0`) simply leaves the
  /// parameter unadmitted, keeping the historical located rejection at
  /// the signature (ImportCFunctions' c_main special case) with its
  /// wording — and ledger tag — byte-identical. Runs at
  /// `planCursorParamsFor`'s `main` early-return, so the plan exists
  /// before any signature is built.
  LogicalResult planArgvUsesFor(const clang::FunctionDecl *func);

  /// Emits every planned clone of the monomorphized variadic definition
  /// `func` (CTS 00204); a plan with zero clones emits nothing.
  LogicalResult emitVaClones(const clang::FunctionDecl *func,
                             const VaMonomorphPlan &plan);

  /// Emits one monomorphized clone: the named parameters keep their
  /// classified shapes, the site's extras append as by-value parameters,
  /// and the body imports with the va_start/va_arg/va_end lowerings
  /// active (an internal i64 consumption cursor; no synthetic parameter).
  LogicalResult emitVaClone(const clang::FunctionDecl *func,
                            const VaClonePlan &clone);

  /// Lowers `va_arg(ap, T)` inside a clone: a dispatch over the
  /// consumption cursor selecting among the clone's extras of static
  /// type T (the cursor increments per read); a cursor position with no
  /// matching extra is a deterministic panic (the C call would be UB).
  FailureOr<Value> emitVaArg(const clang::VAArgExpr *expr);

  /// Binds one ordinary (non-method, non-main, non-cursor) parameter to
  /// its place: slice parameters deref into a region base plus cursor
  /// cell, references bind directly, by-value dialect-typed and unsigned
  /// values copy into `emitrust.variable` places, and plain scalars get
  /// promotable prologue cells. Shared by `importFunction` and
  /// `emitVaClone`.
  LogicalResult bindOrdinaryParam(const clang::ParmVarDecl *param,
                                  Value blockArg, Location paramLoc);

  /// Maps a planned `T **` cursor parameter's element run to its slice
  /// type `!emitrust.slice<T'>` (C99-43 slice 1: T' = mapType(T), i8 for
  /// the historical char** string cursor). An element type the slice
  /// cannot view is a located rejection.
  FailureOr<Type> mapCursorParamSliceType(const clang::ParmVarDecl *param);

  /// Binds a planned cursor parameter (CTS 00204, element-generalized):
  /// the shared element-slice argument derefs into the region base place,
  /// the in-out cursor copies into a local i64 cell at entry, and every
  /// return site copies it back (`cursorWritebacks`).
  LogicalResult bindCursorParam(const clang::ParmVarDecl *param,
                                Value baseArg, Value cursorArg,
                                Location paramLoc);

  /// Binds C `main`'s admitted argv-table parameter (C99-43 C3): records
  /// the `!emitrust.argv_table` entry-block argument in
  /// `mainArgvTableValue` for the translation-time argv intercepts. The
  /// parameter deliberately does NOT enter `symbols`: every admitted use
  /// is intercepted syntactically (printf `%s` holes, `argv[i][j]`
  /// subscripts), so an argv reference reaching the generic
  /// decl-reference path is a bug that must fail loudly there.
  void bindArgvParam(const clang::ParmVarDecl *param, Value tableArg);

  /// Returns the index expression when `expr` (parens/implicit casts
  /// stripped) is a whole-value subscript `argv[i]` of the admitted argv
  /// table, null otherwise.
  const clang::Expr *matchArgvWholeSubscript(const clang::Expr *expr) const;

  /// Returns the outer subscript when `expr` (parens/implicit casts
  /// stripped) is a byte read `argv[i][j]` over the admitted argv table,
  /// null otherwise.
  const clang::ArraySubscriptExpr *
  matchArgvByteRead(const clang::Expr *expr) const;

  /// Emits `emitrust.argv_arg` borrowing argument `indexExpr`'s
  /// NUL-terminated byte run out of the bound argv table, as an
  /// `!emitrust.ref<!emitrust.slice<i8>>`.
  FailureOr<Value> emitArgvArgSlice(Location loc,
                                    const clang::Expr *indexExpr);

  /// `emitLValue`'s argv branch (C99-43 C3): the byte place of
  /// `argv[i][j]` — `emitrust.argv_arg` + deref + subscript, reusing the
  /// existing slice element read. An out-of-range i or j panics where the
  /// C read is UB (the accepted refinement direction).
  FailureOr<Value>
  emitArgvByteLValue(const clang::ArraySubscriptExpr *subscript,
                     Location loc);

  /// Emits the pending string-cursor writebacks (local cell -> deref'd
  /// in-out parameter place) ahead of a return.
  void emitCursorWritebacks(Location loc);

  /// Emits the unique admitted `*param = rhs` write of a Shape-P paired
  /// out-cursor parameter (C99-43 slice 1b): the RHS's cursor value —
  /// in the mapped co-parameter's slice coordinates — assigns straight
  /// through the deref'd `&mut i64` argument. No cell, no return-site
  /// writeback: planning proved the write unique and unconditional.
  LogicalResult emitPairedCursorWrite(const clang::ParmVarDecl *param,
                                      const clang::Expr *rhs, Location loc);

  /// Classifies the RHS of a Shape-G candidate write `*param = rhs`
  /// against the C99-43 C1 single-global-or-NULL grammar: a whole
  /// statically-known global `g` (array decay or `&g`), a null pointer
  /// constant, or `cond ? g : NULL` (either arm order). Returns the
  /// admitted plan, `std::nullopt` when the RHS names no global and no
  /// null constant (the Shape-P sibling-root path applies instead), or
  /// a located rejection for the residual global cases — more than one
  /// distinct global, a global mixed with a non-global root, a derived
  /// address, an element-type mismatch — every wording carrying the
  /// "global address" needle the ledger tables route to
  /// `ptr-to-ptr-global-target`. Shared by planning and the write
  /// emission so both sides agree on the grammar.
  FailureOr<std::optional<GlobalCursorPlan>>
  classifyGlobalCursorWrite(const clang::ParmVarDecl *param,
                            const clang::Expr *rhs, Location writeLoc);

  /// Emits the unique admitted `*param = rhs` write of a Shape-G
  /// single-global-or-NULL out-param cursor (C99-43 C1): `Some(0)` /
  /// `None` values assign through the deref'd `&mut Option<i64>` cell —
  /// the ternary form branches and assigns per arm. No cell copy, no
  /// return-site writeback: planning proved the write unique and
  /// unconditional.
  LogicalResult emitGlobalCursorWrite(const clang::ParmVarDecl *param,
                                      const clang::Expr *rhs, Location loc);

  /// The C1 Option-of-cursor cell type: `Option<i64>` as an emitrust
  /// opaque type (None = C NULL, Some(offset) = element offset into the
  /// planned global's backing). One helper so the signature seat, the
  /// callee write, and the call-site staging can never drift apart.
  emitrust::OpaqueType optionCursorType() {
    return emitrust::OpaqueType::get(builder.getContext(), "Option<i64>");
  }

  /// Emits a call to a callee with planned string-cursor parameters: a
  /// `&p` argument in a cursor position expands to (shared region slice,
  /// `&mut` staged cursor temp), and the temp stores back into `p`'s
  /// cursor cell after the call — the caller-visible advancement.
  FailureOr<Value> emitCursorParamCall(const clang::CallExpr *call,
                                       func::FuncOp target, Location loc);

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
  /// really defines it. Rejects unmappable non-pointer globals (an
  /// incomplete-array extern resolves its bound from
  /// `WholeProgramInfo::completeArrayGlobalTypes` when some TU completes it,
  /// W3.2 COMMIT B) and delegates every pointer-typed extern to
  /// `deferExternPointerGlobal`.
  LogicalResult deferExternGlobal(const clang::VarDecl *key,
                                  llvm::StringRef symbolName,
                                  clang::QualType qualType, Location loc);

  /// Handles a pointer-typed `extern`-only global reference (project
  /// import): a global historically rejected unconditionally, now resolved
  /// against `WholeProgramInfo`'s narrow "shared header pointer global"
  /// reconstruction (W3.2 COMMIT B) when the whole project shows exactly
  /// one, never-reassigned, file-scope binding; every other shape keeps the
  /// historical rejection.
  LogicalResult deferExternPointerGlobal(const clang::VarDecl *key,
                                         llvm::StringRef symbolName,
                                         clang::QualType qualType,
                                         Location loc);

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
  /// `mapType`. `field`, when given and proven usable in
  /// `arrayMemberPtrBindings` (Stage 2 of the owner-struct self-reference
  /// extension), overrides the plain-i64 pointer mapping with the field's
  /// synthesized `!emitrust.enum` storage type instead. Null (the
  /// historical behavior) when the caller has no field to associate — the
  /// data-pointer field's only caller (struct-def field emission)
  /// supplies it.
  FailureOr<Type> mapStructFieldType(clang::QualType type, Location loc,
                                     const clang::FieldDecl *field = nullptr);

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

  //===--------------------------------------------------------------------===//
  // CTS-BR (00216): the u8-only byte-region aggregate model.
  //
  // An aggregate whose scalar leaves are ALL `unsigned char` is
  // padding-free by construction and imports as a plain byte region: the
  // object is an `!emitrust.array<Nxui8>` (N == sizeof), no struct_def is
  // emitted for the record, constant initializers fold to complete byte
  // images (globals) or per-byte stores (locals), member access is an
  // `emitrust.subscript` at the member's constant byte offset, and
  // `(u8 *)&x` is the region base. Pointers to byte-region records are
  // `!emitrust.slice<ui8>` parameters riding the existing slice-parameter
  // decomposition, with byte-granular cursors.
  //===--------------------------------------------------------------------===//

  /// Returns whether `type` is exactly C's `unsigned char` (the one leaf
  /// scalar the byte-region model admits).
  bool isU8ScalarType(clang::QualType type) const;

  /// Returns whether `record` classifies as a byte-region record: every
  /// struct leaf is `unsigned char` (directly, through nested byte-region
  /// records, through constant arrays of either, with empty structs,
  /// GNU zero-length arrays, and a trailing flexible array member
  /// contributing zero bytes), and every union arm is either u8-only or a
  /// constant array of non-u8 scalars whose total size equals the
  /// union's (the in6_addr `unsigned short u6_addr16[8]` type-level
  /// alias). A zero-size record (empty struct) stays on the typed path —
  /// the dialect has no zero-length array — and unions must keep at
  /// least one u8-only arm so scalar-pun unions never reclassify.
  bool isByteRegionRecord(const clang::RecordDecl *record);

  /// Returns whether `type` is a byte-region aggregate: a byte-region
  /// record, or a constant array (of arrays) of byte-region records —
  /// which flattens to ONE region of n*sizeof bytes. Plain `unsigned
  /// char` arrays are NOT byte-region aggregates; they keep the native
  /// array path.
  bool isByteRegionAggregate(clang::QualType type);

  /// Whether the (possibly nested) member/subscript/deref expression
  /// `expr` designates storage inside a byte-region aggregate, i.e. must
  /// route through the byte-region place resolution instead of the typed
  /// member/subscript emission.
  bool exprRootsInByteRegion(const clang::Expr *expr);

  /// A resolved byte-region designator: a base region place (an
  /// `!emitrust.lvalue` of `!emitrust.array<Nxui8>` or
  /// `!emitrust.slice<ui8>`) plus the byte offset of the designated
  /// storage, split into a folded constant part and an optional runtime
  /// i64 part (subscripts with runtime indices, walking pointer cursors).
  struct ByteRegionRef {
    Value place;
    int64_t constOff = 0;
    Value dynOff;
  };

  /// Resolves the lvalue-shaped expression `expr` (declaration
  /// references, dot and arrow member chains, subscripts, dereferences,
  /// compound literals, and identity/qualification casts over any of
  /// them) to its byte-region designator. Global bases stage a whole
  /// region copy (recording `writeback` in write contexts); flexible
  /// array member and zero-length array member accesses are located
  /// rejections.
  FailureOr<ByteRegionRef> resolveByteRegionRef(const clang::Expr *expr,
                                                GlobalWriteback *writeback);

  /// Resolves the data-pointer expression `ptrExpr` (whose pointee is a
  /// byte-region aggregate) to the designated region: the decomposed
  /// pointer's base place at its current byte cursor.
  FailureOr<ByteRegionRef>
  resolveByteRegionPointer(const clang::Expr *ptrExpr,
                           GlobalWriteback *writeback);

  /// Materializes the i64 byte-offset value `constOff + dynOff` of a
  /// resolved designator.
  Value byteRegionOffset(Location loc, const ByteRegionRef &ref);

  /// Emits the `!emitrust.lvalue<ui8>` place of the single byte `ref`
  /// designates offset by `extra` bytes.
  Value byteRegionBytePlace(Location loc, const ByteRegionRef &ref,
                            int64_t extra = 0);

  /// Emits the scalar-leaf place of the byte-region member or subscript
  /// expression `expr`: an `emitrust.subscript` of the region base at the
  /// accumulated byte offset. Only `unsigned char` leaves have a place; a
  /// non-u8 leaf (an equal-size union arm's scalar) is a located
  /// rejection.
  FailureOr<Value> emitByteRegionLeafLValue(const clang::Expr *expr,
                                            Location loc,
                                            GlobalWriteback *writeback);

  /// Copies `size` bytes from the resolved source region `src` into the
  /// destination region `dst` (unrolled per-byte subscript loads and
  /// stores; region sizes are small compile-time constants).
  LogicalResult emitByteRegionCopy(Location loc, const ByteRegionRef &dst,
                                   const ByteRegionRef &src, uint64_t size);

  /// Initializes the byte-region storage at byte `offset` of `place`
  /// (an `!emitrust.lvalue<!emitrust.array<Nxui8>>`, default-zeroed by
  /// the bare `emitrust.variable`) from the initializer `init` of C type
  /// `type`: brace lists and compound literals recurse per field/element
  /// at their layout offsets, string literals store their bytes, constant
  /// scalars fold to `emitrust.constant` ui8 stores, runtime scalars flow
  /// through their AST conversion casts, and whole-aggregate values
  /// (named objects, dereferences, members, identity casts) copy their
  /// source region per byte. Holes keep the C99 zero fill.
  LogicalResult emitByteRegionInit(Value place, int64_t offset,
                                   clang::QualType type,
                                   const clang::Expr *init);

  /// Whole-aggregate assignment over byte-region records (`a = b`, `a.s =
  /// b`, statement position): per-byte region copy with the LHS's global
  /// writeback flushed afterwards.
  LogicalResult emitByteRegionAggregateAssign(const clang::BinaryOperator *op);

  /// Creates the byte-region global for `decl` (called from
  /// `createGlobal` once the type classified): the global's type is
  /// `!emitrust.array<Nxui8>` where N is sizeof — EXTENDED past sizeof by
  /// a static flexible-array-member tail initializer — and the
  /// initializer folds to a complete zero-filled byte image computed from
  /// the APValue against the target record layout.
  LogicalResult createByteRegionGlobal(const clang::VarDecl *key,
                                       const clang::VarDecl *decl,
                                       llvm::StringRef symbolName,
                                       Location loc);

  /// Serializes the constant `value` of C type `type` into `image`
  /// starting at byte `offset`, following the target record layout
  /// (little-endian for the multi-byte scalars an equal-size union arm
  /// may alias over the region).
  LogicalResult serializeAPValueBytes(const clang::APValue &value,
                                      clang::QualType type, uint64_t offset,
                                      SmallVectorImpl<uint8_t> &image,
                                      Location loc);

  /// Syntactic counterpart of `serializeAPValueBytes` for the one shape
  /// clang's constant evaluator refuses: a record initializer with a
  /// flexible-array-member tail. Walks the SEMANTIC initializer form,
  /// folding constant scalar leaves, string literals, and nested lists
  /// into the image at their layout offsets.
  LogicalResult serializeInitExprBytes(const clang::Expr *init,
                                       clang::QualType type, uint64_t offset,
                                       SmallVectorImpl<uint8_t> &image,
                                       Location loc);

  /// If `field` is a flexible array member or a GNU zero-length array
  /// member, emits the dedicated located access rejection; such members
  /// are tolerated at the declaration (zero size, no storage) but have no
  /// runtime-accessible elements.
  LogicalResult checkSpecialArrayMemberAccess(const clang::FieldDecl *field,
                                              Location loc);

  /// Returns whether `type` is a GNU zero-length array (`T r[0]`).
  bool isZeroLengthArrayType(clang::QualType type) const;

  /// Whether `expr` is a designator shape `resolveByteRegionRef` handles
  /// (a declaration reference, member chain, subscript, dereference, or
  /// compound literal, under identity casts) — used to gate the per-byte
  /// aggregate-assignment path.
  bool isByteRegionDesignator(const clang::Expr *expr) const;

  /// The u8-only classification cache (`isByteRegionRecord` is consulted
  /// for every record type mapping and member access).
  llvm::DenseMap<const clang::RecordDecl *, bool> byteRegionRecords;

  //===--------------------------------------------------------------------===//
  // CTS-BR (00216): `void *` fn-ptr struct members (the T1.1 holder bound
  // extended to members). A struct member declared `void *` whose every
  // stored value across the TU is the address of a function of ONE
  // signature — stores happen only in aggregate initializers, never by
  // assignment — imports as an `!emitrust.fn_ptr` member; reads under a
  // cast to that one signature load the member place directly.
  //===--------------------------------------------------------------------===//

  /// TU pre-pass populating `fnPtrMemberTypes`: candidates are `void *`
  /// fields whose every aggregate-initializer value is the address of a
  /// function of one common signature and whose only other mentions are
  /// reads under a cast to that signature.
  void planFnPtrMembers(const clang::TranslationUnitDecl *unit);

  /// Record definitions mentioned by any DECLARATION type in the TU
  /// (globals, parameters, returns, fields, block-scope locals) —
  /// stripped of typedefs, arrays, and pointers. An EMPTY struct is
  /// eagerly imported only when this set names it; one that only ever
  /// appears inside byte-region initializer expressions (CTS-BR, 00216)
  /// never emits a struct_def.
  llvm::DenseSet<const clang::RecordDecl *> declTypeUsedRecords;

  /// Populates `declTypeUsedRecords` for this TU.
  void collectDeclTypeRecords(const clang::TranslationUnitDecl *unit);

  /// The admitted `void *` fn-ptr members, mapped to the function-pointer
  /// C type they retype to (`T (*)(...)` of the common target signature).
  llvm::DenseMap<const clang::FieldDecl *, clang::QualType> fnPtrMemberTypes;

  /// Byte-region-global slice arguments staged by `emitBorrowArgument`
  /// for the call being emitted: each (staged place, global symbol) pair
  /// stores the image back right after the call op (drained per call
  /// site, so nested calls consume their own entries first).
  SmallVector<std::pair<Value, std::string>, 2> pendingStagedGlobalStores;

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

  /// FR-61e freshness guard for scalar-local name preservation: whether a
  /// signed scalar's imported initializer is a fresh, in-function computation
  /// whose location may carry the local's source name onto the promoted SSA
  /// value. Rejects values whose location is shared with another binding
  /// (constants, bare loads, parameters/block arguments).
  static bool carriesLocalName(Value value);

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
  /// insertion point. A non-empty `rustName` (FR-61e: the final Rust base
  /// spelling, already through `mangleMemberName`) is carried on the op so
  /// the emitter binds the declared local under its C name; synthesized
  /// temporaries pass nothing.
  Value createVariablePlace(Location loc, Type type,
                            llvm::StringRef rustName = {},
                            mlir::Attribute init = {});

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

  /// K&R callsite-prototype inference (FR-29, CTS 00209): walks the
  /// DEFINITION's body for calls with arguments whose callee, after the
  /// `(*fp)` deref-peel, is a `DeclRefExpr` to a local-storage
  /// `ParmVarDecl`/`VarDecl` of pointer-to-`FunctionNoProtoType`, and
  /// records decl -> `!emitrust.fn_ptr<promoted... -> ret>` in `inferred`
  /// — the argument types verbatim (clang already applied the default
  /// argument promotions at the call) plus the declared return type.
  /// Multiple call sites for one decl must agree on the signature; a
  /// disagreeing site is a located rejection at that (second) site.
  /// Zero-argument calls infer nothing (a never-argument-called pointer
  /// keeps the unrefined zero-parameter mapping), and non-decl callees
  /// (members, array elements, call results) are skipped — their
  /// argument-carrying calls keep the existing no-prototype rejection in
  /// `emitIndirectCall`.
  LogicalResult inferNoProtoCallSignatures(
      const clang::FunctionDecl *definition,
      llvm::DenseMap<const clang::VarDecl *, emitrust::FnPtrType> &inferred);

  /// Emits `expr` destined for a position of type `expected`. When
  /// `expected` is a `!emitrust.fn_ptr` signature and `expr` is (a cast
  /// chain over) a direct function reference of prototype-less
  /// pointer-to-function type, the `Some(target)` constant resolves
  /// against `expected` — the shape a refined (callsite-inferred, FR-29 /
  /// CTS 00209) destination requires, and identical to the ordinary
  /// mapping when the destination is unrefined. A prototype-less null
  /// pointer constant likewise takes `expected`'s `None`. Every other
  /// shape (including all prototyped sources) is a plain `emitRValue`.
  FailureOr<Value> emitPositionedRValue(Type expected,
                                        const clang::Expr *expr);

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

  /// Materializes a block-scope compound literal in expression position
  /// (C99-13) as a fresh anonymous place: a default-initialized
  /// `emitrust.variable` of the literal's aggregate type followed by the
  /// per-element assigns of its initializer list (the C99-11 machinery;
  /// holes keep the C99 zero fill), or the string-array fill for
  /// `(char[N]){"..."}`. With `hoistForRegion` false (direct lvalue and
  /// value uses, whose consumers sit in the same statement) the place is
  /// created at the current insertion point. With `hoistForRegion` true
  /// (the literal becomes a pointer-region base, so dereferences anywhere
  /// in the function resolve against the place) the place is hoisted to
  /// the entry block for SSA dominance, and each evaluation first
  /// re-assigns the type's pristine default value — captured by a load
  /// right after the hoisted creation — so re-executions (a literal
  /// bound inside a loop) restore the C99 zero fill of the holes exactly.
  /// Scalar compound literals and file-scope literals reaching an
  /// expression context are located rejections.
  FailureOr<Value>
  emitCompoundLiteralPlace(const clang::CompoundLiteralExpr *literal,
                           bool hoistForRegion = false);

  /// Materializes `literal` (see `emitCompoundLiteralPlace`) and registers
  /// the place under the literal's synthesized backing declaration — the
  /// region base the analysis recorded for its decay or address-of — so
  /// every downstream consumer (dereference, subscript, slice argument)
  /// resolves the base like any named local object. Returns the backing
  /// declaration.
  FailureOr<const clang::VarDecl *>
  materializeCompoundLiteralBase(const clang::CompoundLiteralExpr *literal);

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
  /// backing, the base object's own place, or (FR-72) a byte-slice
  /// PARAMETER's deref'd backing (`!emitrust.lvalue<!emitrust.slice<i8|
  /// ui8>>`, resliced at the cursor exactly like the general slice-param
  /// call machinery) — typed `!emitrust.ref<!emitrust.slice<i8|ui8>>`
  /// (or `mut_ref` when `isMut`). Rejects a mutable borrow of a
  /// read-only literal region, a mutable borrow of a SHARED slice
  /// parameter (a `const` pointee; rustc E0596 would be the only
  /// downstream catch), a ui8 parameter region unless the caller's
  /// helper family has u8 images (`allowUnsignedByte` — the str*-family
  /// helpers are i8-typed), and any base whose place is neither a char
  /// array nor a byte-slice parameter (all located diagnostics); the
  /// array's compile-time-known size — or the slice's own length — is
  /// what makes every helper access bounds-checked safe Rust.
  FailureOr<Value> emitCharRegionSlice(Location loc,
                                       const PtrExprValue &pointer,
                                       bool isMut,
                                       bool allowUnsignedByte = false);

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

  /// Lowers a statement-position `memcpy(dst, src, n)` or
  /// `memmove(dst, src, n)` call (C name in `name`, diagnostics only —
  /// the lowering is shared and exact for both). Distinct base objects
  /// (or a literal source) borrow two slices for the `__emitrust_memcpy`
  /// helper — distinct char regions never overlap, so memmove's
  /// overlap-safety is vacuous there; both arguments rooted in the same
  /// base object would alias a mutable borrow, so that shape takes one
  /// mutable borrow of the whole array plus both cursors through the
  /// `__emitrust_memcpy_within` helper (`copy_within`, exactly memmove's
  /// overlap-correct semantics, which also refine C's undefined
  /// overlapping memcpy).
  LogicalResult emitMemcpyCall(const clang::CallExpr *call,
                               llvm::StringRef name);

  /// Lowers a value-position call to a definition-less `atoi`: the
  /// argument's char region is borrowed as a shared byte slice from its
  /// cursor and parsed by the `__emitrust_atoi` helper with C's exact
  /// semantics (skip isspace, one optional sign, decimal digits to the
  /// first non-digit; no digits yields 0). Out-of-range values are C UB
  /// (7.20.1p1), refined to deterministic i32 wrapping.
  FailureOr<Value> emitAtoiCall(const clang::CallExpr *call);

  /// Lowers a value-position call to a definition-less `abs` (i32) or
  /// `labs` (i64, `isLong`) to `iN::wrapping_abs`. C leaves
  /// abs(INT_MIN)/labs(LONG_MIN) undefined (7.20.6.1p2); wrapping_abs
  /// refines that to the deterministic two's-complement result the
  /// differential oracle's platform also produces.
  FailureOr<Value> emitAbsCall(const clang::CallExpr *call, bool isLong);

  /// Lowers a statement-position `exit(status)` call to
  /// `std::process::exit(status as i32)`, matching C's process
  /// termination and exit-status semantics (both truncate to the OS's
  /// low byte on this target). Statement position only: exit returns
  /// void in C, so no value use exists to represent.
  LogicalResult emitExitCall(const clang::CallExpr *call);

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

  /// FR-61f: sound AST matcher for the canonical counting `for`. Returns a
  /// `RangeFor` iff every clause holds (init `int i = LO;`, cond `i < HI`,
  /// inc `i++`/`i += K` with K a positive constant, `i` body-immutable and
  /// non-escaping, `HI` loop-invariant, no `break`/`continue`/`goto`/
  /// `return`/label in the body). Any doubt returns nullopt so the caller
  /// falls through to the already-correct CFG `while` lowering.
  std::optional<RangeFor> matchRangeFor(const clang::ForStmt *stmt);

  /// FR-61f: emits a matched `RangeFor` as an `emitrust.for` with the
  /// induction seeded from the region's block argument into a place.
  LogicalResult emitRangeFor(const RangeFor &range,
                             const clang::ForStmt *stmt);

  /// FR-61f: function-body pre-pass. Walks every range-eligible `for` and
  /// unions the body-touched signed-scalar locals (accumulators declared
  /// before the loop and temps declared inside, minus the induction) into
  /// `placeBackedScalars`, so `emitLocalVar` routes them to
  /// `emitrust.variable` places (which `convert-to-emitrust` accepts inside
  /// a region op) rather than un-promotable `memref.alloca` cells.
  void collectRangeForPlaceScalars(const clang::Stmt *stmt);

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
  /// `%[flags][width][.precision][length]conv` with all five C99 flags
  /// (`-`, `0`, `+`, ` `, `#`), decimal width and precision, lengths
  /// `l`/`ll` (i64/u64) and `h`/`hh` (the promoted argument reduced to
  /// short/char range by an `as`-cast), and conversions d/i, u, x/X, o,
  /// c (byte, via `__emitrust_fmt_c`), s (see `emitPrintfStringArg`),
  /// f/F/e/E/g/G (f64), and %%. Directives that map 1:1 onto Rust format
  /// specs use them (width/`-`/`0` on integers, width/`-` on c/s); every
  /// other supported form routes through the on-demand `__emitrust_fmt_*`
  /// helpers, which implement the C99 rendering rules exactly (integer
  /// precision and sign/prefix placement, the f/e/g floating algorithms
  /// including glibc's `%#g` rounding-carry quirk, space-padded
  /// non-finite values) — all validated byte-exactly against glibc.
  /// Undefined-by-C99 flag combinations (`%#d`, `%+u`, `%0c`, ...),
  /// `*` width/precision, lengths `L`/`j`/`z`/`t`, and the conversions
  /// a/A (hex float) and n keep located rejections. %p stays rejected by
  /// design: pointer provenance is compiled away by the decomposition, so
  /// no address exists to print. Integer arguments of a different width
  /// or signedness than the conversion expects are `as`-cast, which
  /// truncates to the low bits exactly like the x86-64 varargs read that C
  /// performs.
  LogicalResult emitPrintf(const clang::CallExpr *call);

  /// Emits a single printf-family output as an `emitrust.call_opaque` to the
  /// idiomatic Rust print macro, choosing the macro from `rustFormat`'s
  /// trailing newline: a format ending in `'\n'` drops that one byte and
  /// prints through `println!` (stdout-identical to `print!` of the
  /// newline-terminated string), otherwise it keeps `print!`. `operands`
  /// supplies one SSA value per `{}` placeholder, in order. The degenerate
  /// bare `println!()` case (format is exactly `"\n"` with no operands) is
  /// emitted with an empty args array so it renders `println!()` rather than
  /// `println!("")` (which would trip `clippy::println_empty_string`). All
  /// printf-family emission sites route through here so the macro choice is
  /// centralized.
  void emitPrintMacro(Location loc, std::string rustFormat,
                      ValueRange operands);

  /// Translates the C printf-family format string `literal` into a Rust
  /// format string, consuming the directive arguments of `call` starting
  /// at `firstArgIndex` and appending their lowered SSA values to
  /// `operands` (one per Rust `{}` placeholder, in order). This is the
  /// shared directive grammar of `emitPrintf` and `emitSprintf` (see
  /// `emitPrintf` for the supported set); `*` width/precision, the
  /// unsupported length modifiers, undefined flag combinations, and
  /// unknown conversions keep their located rejections here so every
  /// caller enforces the same subset. Fails if `call` supplies too few or
  /// too many arguments for the directives.
  /// C99-43 C3: with `allowArgvBypass` set (the stdout `print!` context
  /// only — `emitPrintf`), an argv-fed `%s`/`%c` hole BYPASSES the
  /// `__emitrust_cstr`/`__emitrust_fmt_c` Display funnels, whose Latin-1
  /// byte-to-char widening would double-encode any non-ASCII argument
  /// byte: the pending format segment is flushed as its own `print!`
  /// call, the hole renders through the raw on-demand helpers
  /// (`__emitrust_cstr_out`/`__emitrust_cstr_n_out`/`__emitrust_byte_out`
  /// — NUL-scan + `write_all` on the SAME globally buffered stdout handle
  /// `print!` locks, so ordering holds even on block-buffered pipes), and
  /// translation continues into a fresh segment whose remainder the
  /// caller prints. `*argvBypassed` reports whether any hole took the
  /// bypass (so the caller can skip an empty trailing `print!`). Without
  /// the flag an argv-fed hole is a located rejection in the historical
  /// argv wording — planning admits argv only into the direct-printf
  /// path, so reaching one here is the loud-failure direction.
  FailureOr<std::string>
  translatePrintfFormat(Location loc, const clang::CallExpr *call,
                        const clang::StringLiteral *literal,
                        unsigned firstArgIndex,
                        SmallVectorImpl<Value> &operands,
                        bool allowArgvBypass = false,
                        bool *argvBypassed = nullptr);

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
  /// When `isSnprintf` is true the call is `snprintf(dest, size, fmt, ...)`:
  /// the size bound shifts the format literal and variadic arguments by one
  /// and routes to the truncating `__emitrust_snprintf` helper, which writes
  /// at most `size - 1` bytes plus a NUL (C's defined truncation, not the
  /// overflow panic sprintf uses) and still returns the full formatted
  /// length. `false` is the plain `sprintf(dest, fmt, ...)`.
  FailureOr<Value> emitSprintf(const clang::CallExpr *call,
                               bool isSnprintf = false);

  /// Escapes `data` (raw decoded string-literal bytes: printable ASCII plus
  /// \n/\t/\r only; embedded NUL and any other non-ASCII byte are located
  /// rejections, worded "... in <context>") into a quoted Rust string
  /// literal and returns it as an `emitrust.literal` of `&'static str`
  /// type. Shared by `emitPrintfStringArg`'s string-literal `%s` shape
  /// (`context` = "printf '%s' string literal", the historical wording)
  /// and W2.3's `std::string s = "literal";` construction / `s +=
  /// "literal"` (`String::from(...)` / `.push_str(...)`).
  FailureOr<Value> emitRustStrLiteral(Location loc, llvm::StringRef data,
                                      llvm::StringRef context);

  /// Lowers a `%s` printf argument. Five shapes are supported: a string
  /// literal (after array-to-pointer decay), lowered to an
  /// `emitrust.literal` holding a `&'static str` (printable-ASCII bytes
  /// plus \n/\t/\r only; embedded NUL and non-ASCII bytes are rejected);
  /// a char-array lvalue, lowered to an `emitrust.slice_of` of the
  /// whole array passed through the `__emitrust_cstr` helper, which stops
  /// at the first NUL like C; a `char *` pointer into a string-literal
  /// region, lowered to an `emitrust.slice_of` of the region's read-only
  /// backing from the pointer's cursor through the same helper; a
  /// slice-classified `char *` parameter (FR-28, CTS-L2), lowered to an
  /// `emitrust.slice_of` of the parameter's deref'd slice base place from
  /// its cursor through the same helper; and (W2.3) a `std::string`
  /// object's `.c_str()` call, lowered to a shared borrow of the String
  /// place (deref coercion to `&str` applies at the format-argument
  /// position, exactly like `__emitrust_sprintf`'s staged String borrow) —
  /// the ONLY position `.c_str()` is recognized in. A `%.Ns` precision caps
  /// the printed bytes at N like C: a literal is truncated at import time
  /// (only the retained prefix is validated), the slice shapes route
  /// through `__emitrust_cstr_n`, which stops at N bytes or the first
  /// NUL, whichever comes first (a `.c_str()` argument does not support a
  /// precision — a located rejection — since it has no fixed byte count
  /// to bound at import time).
  FailureOr<Value>
  emitPrintfStringArg(const clang::Expr *expr,
                      std::optional<unsigned> precision = std::nullopt);

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
  /// lowers to (design.md C99-48): the IEEE-exact fabs/sqrt/floor/ceil
  /// onto the matching f64 methods, plus the differentially pinned
  /// `sin` -> `f64::sin`. Returns std::nullopt for every other name,
  /// which keeps the located rejections in `emitCall` (a curated
  /// non-bit-exact diagnostic for exp/log/pow, the system-header
  /// rejection otherwise).
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
  /// `outFirstPointerArgBase`, when non-null, receives the region base
  /// (`PtrExprValue::base`) the call's FIRST data-pointer argument resolved
  /// to, or null if the call has none. Stage 1's owner-index-return call
  /// consumption (`emitPointerRValue`'s `CallExpr` case) reuses this single
  /// argument-materialization pass to learn which region an
  /// owner-index-returning callee's i64 result roots at, without
  /// re-evaluating the argument a second time.
  FailureOr<Value>
  emitMethodCallSite(const clang::CallExpr *call, func::FuncOp target,
                     const clang::VarDecl *ownerBase, Location loc,
                     const clang::VarDecl **outFirstPointerArgBase = nullptr);

  /// Lowers one borrow-producing call argument against the reference-typed
  /// target parameter `paramType`. A slice parameter receives an
  /// `emitrust.slice_of` of the argument's region base at the argument's
  /// cursor (a decayed array passes cursor 0; reslicing through another
  /// slice parameter composes); a scalar-reference parameter receives an
  /// `emitrust.addr_of` of the designated element, or of the named place
  /// for plain address-of arguments that involve no decomposed pointer.
  /// `root` receives the argument's region base declaration when one is
  /// statically known (feeding the aliasing rejection in `emitCall`).
  /// FR-74: `rootPath`, when provided, receives the member projection
  /// chain (outermost field first) for a member-array slice argument —
  /// the borrow then covers only that FIELD of `root`, so `emitCall`'s
  /// aliasing guard can admit disjoint sibling fields of one struct
  /// while still rejecting prefix-overlapping borrows. Every other
  /// argument shape leaves the path empty (a whole-object borrow, which
  /// collides with everything under the same root — the historical
  /// behavior).
  FailureOr<Value> emitBorrowArgument(
      Location loc, const clang::Expr *argument, Type paramType,
      const clang::VarDecl *&root,
      SmallVectorImpl<const clang::FieldDecl *> *rootPath = nullptr);

  /// FR-74: matches a member-array slice ARGUMENT — a dot/arrow
  /// projection chain ending at a fixed-extent array-typed field —
  /// against the admitted base forms: a LOCAL struct place (dot chains,
  /// nested) or an arrow at the chain root through a ref/mut_ref-struct
  /// pointer parameter (the one pointer convention whose member place
  /// is the pointee itself rather than a staged copy or a decomposed
  /// cursor). On a match, `chainRoot` receives the root variable and
  /// `path` the field chain (outermost first). A non-match returns
  /// false WITHOUT diagnosing so the caller falls through to the
  /// historical verbatim rejection: global roots (their member place is
  /// a staged copy — a mutable slice of it would silently lose the
  /// callee's writes), unions (arms overlap), byte-region records
  /// (subscript-of-region model), and every unprovable base.
  bool matchMemberArraySliceArg(const clang::MemberExpr *member,
                                bool isMutParam,
                                const clang::VarDecl *&chainRoot,
                                SmallVectorImpl<const clang::FieldDecl *> &path);

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

  /// Decl-level overload for an already-known MLIR signature: emits the
  /// opaque `Some(<symbol>)` constant at exactly `fnPtrType` after the
  /// `resolveFunctionPointerDecl` signature check. Used by
  /// `emitPositionedRValue`, where the destination's (possibly
  /// callsite-inferred, FR-29 / CTS 00209) fn_ptr type — not the source
  /// expression's spelled C type — is the binding contract.
  FailureOr<Value>
  emitFunctionPointerConstant(const clang::FunctionDecl *target,
                              emitrust::FnPtrType fnPtrType, Location loc);

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

  /// FR-47: `emitLValue`'s `this` branch — the place denoted by the current
  /// method's receiver, i.e. the object `*this`, obtained by dereferencing
  /// `currentCxxThisRef` exactly the way the method prologue and
  /// `emitMemberBasePlace`'s `->` branch already dereference it.
  ///
  /// NOTE the deliberate asymmetry with `emitRValue`'s `CXXThisExpr` case,
  /// which yields the UNDEREFERENCED ref/mut_ref (`this` is a POINTER
  /// prvalue of type `C *`). `this` is never itself an lvalue in C++, so
  /// the only way this branch is reached is a receiver position that clang
  /// spells with a bare `CXXThisExpr` — the implicit object argument of
  /// `m()` and of `this->m()` — where the place actually wanted is the
  /// pointee. Every other place context is unreachable by construction:
  /// `this = p` and `&this` are ill-formed, an lvalue-to-rvalue cast never
  /// has a prvalue operand, and `(*this).x` / `(*this).m()` spell an
  /// explicit `UnaryOperator` that reaches `emitDerefLValue` instead (which
  /// builds the identical op — that spelling already worked before FR-47,
  /// which is what pinned the shape this function has to reproduce).
  ///
  /// Receiver mutability is deliberately NOT decided here: the place is
  /// borrow-agnostic, so the single pre-existing rule keeps applying
  /// unchanged — `emitCXXMemberCall`'s `is_mut = !method->isConst()`, paired
  /// with the receiver type `importFunction` already fixed at signature
  /// time (`mut_ref` for a mutating method or a constructor, `ref` for a
  /// `const` one). Inventing a second rule here (e.g. deciding from the
  /// CALLER's constness) was rejected: it would double-source the decision,
  /// and it is unnecessary because C++ itself rejects a `const` caller
  /// reaching a non-`const` callee long before the importer runs, so no
  /// `addr_of mut` of a shared-ref-derived place can ever be built.
  FailureOr<Value> emitCxxThisPlace(Location loc);

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
  /// and the bare C name for external-linkage functions. The name is
  /// additionally namespace-flattened (W2.0, see `namespacePrefix`) when
  /// `func` is declared inside a C++ `namespace`; `extern "C"` never
  /// contributes a prefix.
  std::string mlirFuncName(const clang::FunctionDecl *func) const;

  /// Computes the MLIR symbol name of a file-scope variable, mirroring
  /// `mlirFuncName`: internal linkage gets the per-TU tag, and any
  /// enclosing C++ namespace chain contributes its flattening prefix
  /// (W2.0, see `namespacePrefix`). `importGlobalVar` and
  /// `collectOrdinaryNames`'s pre-scan both call this so the two always
  /// agree.
  std::string globalVarSymbolName(const clang::VarDecl *var) const;

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
  /// One deferred `extern` global reference awaiting a cross-TU definition:
  /// the first reference's location (for the diagnostic if no TU defines
  /// it) and the global's MLIR value type (so FR-57a defer mode can
  /// materialize a declaration-only `emitrust.global` without re-deriving
  /// the type from a clang AST that is no longer current).
  struct PendingExternGlobal {
    Location loc;
    Type type;
  };
  /// Deferred `extern` global references awaiting a cross-TU definition,
  /// keyed by MLIR symbol name.
  llvm::StringMap<PendingExternGlobal> pendingExternGlobals;
  /// FR-57a: whether `finalizeProject` turns referenced-but-undefined
  /// external symbols into `emitrust.extern_decl`-marked declarations
  /// instead of rejecting them; see `setDeferExternals`.
  bool deferExternals = false;
  /// FR-52: what `finalizeProject` does with a referenced-but-undefined
  /// external function; see `setExternalRequirements`.
  emitrust::ExternalRequirements externalRequirements =
      emitrust::ExternalRequirements::Reject;
  /// FR-52: the MLIR symbol name of every function whose ADDRESS was taken
  /// anywhere in the project (`resolveFunctionPointerDecl`'s successful
  /// answers).
  ///
  /// A function pointer is emitted as an `emitrust.constant` holding the
  /// opaque text `Some(<name>)`, which is NOT a symbol use — the symbol-table
  /// machinery cannot see it. So this set is the only record that the name is
  /// still spelled out somewhere in the module, and an undefined external in
  /// it keeps the historical rejection rather than becoming a trait
  /// requirement whose `Some(<name>)` would dangle.
  llvm::StringSet<> fnPointerTargetSymbols;
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

  /// The RAW C spellings the current TU's ordinary identifier namespace
  /// declares (functions and file-scope variables, no mangling applied).
  /// Backs the keyword-function collision check (CTS 00204): `match`
  /// mangles to `match_`, which must not silently merge with a source
  /// declaration already spelled `match_`.
  llvm::StringSet<> ordinaryRawTuNames;
  /// Symbol name assigned to each struct definition by `structSymbolName`,
  /// keyed on the defining declaration (per-TU decls are distinct; cross-TU
  /// unification still happens by final name through
  /// `importedRecordShapes`, so the rename decision must be reproducible
  /// from each TU's own ordinary names).
  llvm::DenseMap<const clang::RecordDecl *, std::string> assignedStructNames;
  /// Shape of every imported enum, keyed by symbol name, for cross-TU
  /// deduplication and mismatch detection.
  llvm::StringMap<std::string> importedEnumShapes;
  /// W2.14: the two alternative types of every SYNTHESIZED std::variant
  /// data enum, keyed by its shape-keyed symbol name (`VariantI32F64`).
  /// First insertion emits the module-level `emitrust.data_enum_def`;
  /// every later mention of the same shape — spelled `variant` in clang
  /// regardless of instantiation — reuses the one definition. Consulted
  /// by `variantAltIndex` (construction/assignment/get selection) and
  /// `createVariantMatch` (per-arm payload types).
  llvm::StringMap<llvm::SmallVector<Type, 2>> variantEnumAlternatives;
  //===--------------------------------------------------------------------===//
  // Recoverable import (FR-42) state
  //===--------------------------------------------------------------------===//

  /// Recoverable import mode; see `enableRecovery`. False by default, and
  /// every code path that reads it is guarded so that a non-recovering
  /// import executes exactly the instructions it always did.
  bool recoverFromRejections = false;
  /// The FR-43 search state's complement — item-graph keys that must not be
  /// imported; null (the default) means "admit everything the importer can
  /// take", i.e. exactly FR-42's behavior. Read only from
  /// `frontierExcludedSymbol`, which is called only under recovery.
  const std::set<std::string> *excludedItems = nullptr;
  /// Where recovered rejections are recorded; null unless recovery is on.
  emitrust::RejectionLedger *rejectionLedger = nullptr;
  /// The checkpoint of the item currently being imported under recovery, or
  /// null. Held as importer state (rather than only on the stack) so
  /// `eraseTopLevelOp` can keep its anchor valid from anywhere in the
  /// import, however deeply nested.
  RecoveryCheckpoint *activeCheckpoint = nullptr;
  /// Signature-only import: `importFunction` builds the signature exactly as
  /// it normally would and then, instead of importing the body, emits the
  /// `unimplemented!()` stub body and returns. Set ONLY by
  /// `importTopLevelDeclRecovering`, for the duration of one retry of an
  /// already-rejected function.
  bool recoveryStubOnly = false;
  /// The reason text embedded in the stub emitted under `recoveryStubOnly`
  /// — the verbatim diagnostic that rejected the real import.
  std::string recoveryStubReason;
  /// The MLIR symbol name the last stub claimed, read back by
  /// `importTopLevelDeclRecovering` for the ledger (the mangling
  /// `importFunction` applies is not reproducible from the AST alone).
  std::string recoveryStubSymbol;

  //===--------------------------------------------------------------------===//
  // Recoverable Pass-A planning (FR-53)
  //===--------------------------------------------------------------------===//

  /// A Pass-A planner rejection that was attributed to one declaration.
  struct PlannerRejection {
    /// The rejection's own location — the offending construct, never the
    /// declaration's `getBeginLoc()`, so the ledger points where the
    /// non-recovering diagnostic pointed.
    Location loc;
    /// The verbatim diagnostic the planner would have printed.
    std::string reason;
  };

  /// Pass-A planner rejections attributed to a top-level declaration of the TU
  /// under import, keyed by the very `clang::Decl *` `importDeclsIn` walks.
  /// Populated only under `recoverFromRejections`, cleared at the start of
  /// every translation unit, and consulted in exactly one place —
  /// `importTopLevelDeclRecovering`, which turns an entry into the same
  /// rollback/stub/ledger/warning outcome an `importFunction` rejection has.
  ///
  /// The planners themselves consult it too, to SKIP an already-rejected
  /// declaration on a replan: a plan the surviving declarations consult must
  /// be the plan a non-recovering run over exactly those declarations would
  /// have built, and the only way to guarantee that is to rebuild it with the
  /// rejected declaration absent rather than to patch the half-built one.
  llvm::DenseMap<const clang::Decl *, PlannerRejection> plannerRejections;

  /// Set by a planner fragment that knows which declaration its rejection
  /// belongs to, overriding `recoverPlannerRejection`'s default attribution;
  /// read and cleared by that function. Null at every other moment.
  const clang::Decl *pendingPlannerAttribution = nullptr;

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
  /// Per-function callsite-inferred prototypes (FR-29, CTS 00209) for
  /// prototype-less K&R fn-ptr parameters and locals, keyed by the
  /// definition's decls; populated by `inferNoProtoCallSignatures` at
  /// signature-building time and installed here for the body emission.
  /// An inferred decl declares (parameter and local place alike) at its
  /// refined `!emitrust.fn_ptr` signature, its argument-carrying calls
  /// bypass the no-prototype rejection onto the ordinary typed
  /// `emitrust.call_indirect` path, and bindings of real functions to it
  /// resolve against the refined signature.
  llvm::DenseMap<const clang::VarDecl *, emitrust::FnPtrType>
      inferredFnPtrSigs;
  /// The clang body of the function under import; consulted by dead-VLA
  /// elision (CTS-F, 00207) to decide whether a local is referenced
  /// anywhere in the body.
  const clang::Stmt *currentFunctionBody = nullptr;
  /// Per-function set of locals whose address is taken.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> addressTaken;
  /// FR-61f: per-function set of signed-scalar locals a range-eligible `for`
  /// body touches; `emitLocalVar` routes these to `emitrust.variable`
  /// places instead of `memref.alloca` cells (filled by
  /// `collectRangeForPlaceScalars`).
  llvm::SmallPtrSet<const clang::VarDecl *, 8> placeBackedScalars;
  /// FR-61f: a lifted range-`for`'s induction variable mapped to its
  /// `emitrust.for` block-argument value. Clause 4 guarantees the induction
  /// is body-immutable and non-address-taken, so every read is that SSA value
  /// directly -- no place, no seed store, no `let i` binding. An outer
  /// induction stays registered while a nested loop body emits.
  llvm::DenseMap<const clang::VarDecl *, mlir::Value> inductionValues;
  /// Per-function pointer region analysis (Phase-1a decomposition).
  PointerRegionAnalysis pointerRegions;
  /// Program-wide registry of synthesized compound-literal backing
  /// declarations (C99-13), shared with every analysis instance (the
  /// planning passes and the per-function emission analysis) so all
  /// passes agree on each literal's backing identity.
  CompoundLiteralTemps literalTemps;
  /// Per-function decomposition of each accepted pointer local and each
  /// slice-classified pointer parameter, keyed by its declaration.
  llvm::DenseMap<const clang::VarDecl *, PointerLocalInfo> pointerLocals;
  /// W2.12: per-function decomposition of each literal-initialized
  /// `std::string_view` local (see `emitStringViewLocal`): the literal's
  /// shared read-only backing byte array place plus two entry
  /// `memref<i64>` cells — the byte cursor into the backing and the
  /// remaining length (the literal's length without the NUL at init;
  /// `remove_prefix` advances the cursor and shrinks the length).
  struct StringViewLocalInfo {
    Value backing;
    Value cursorCell;
    Value lenCell;
  };
  llvm::DenseMap<const clang::VarDecl *, StringViewLocalInfo>
      stringViewLocals;
  /// W2.13: per-function registry of recognized lifted-lambda locals
  /// (`auto f = [a, b](int x) {...};`, see `emitLambdaLocal`): the
  /// module-level fn the lambda lifted to, the capture values FROZEN at
  /// the declaration point (one load per capture, in capture-list order —
  /// C++'s capture-by-value semantics: a later mutation of the source
  /// variable is invisible to every call), and the closure's operator()
  /// (whose body becomes the lifted fn's body). Every use of a registered
  /// local is a direct operator() call — the recognizer verified this —
  /// rewritten by `emitLambdaLocalCall` to `lifted(frozen..., args...)`.
  struct LambdaLocalInfo {
    func::FuncOp funcOp;
    SmallVector<Value, 4> frozenCaptures;
    const clang::CXXMethodDecl *callOperator;
  };
  llvm::DenseMap<const clang::VarDecl *, LambdaLocalInfo> lambdaLocals;
  /// W2.13: lifted lambdas whose module-level FuncOp signature exists but
  /// whose body has not yet imported. Bodies import AFTER the enclosing
  /// function's own emission completes (`importPendingLiftedLambdas` at
  /// the end of `importFunction`) because the shared body emitter's
  /// per-function state (`symbols`, `entryBlock`, ...) is
  /// single-occupancy. Cleared defensively in `importFunction`'s
  /// definition prologue so a rolled-back rejection (FR-42) can never
  /// leave a stale entry pointing at an erased FuncOp.
  struct PendingLiftedLambda {
    func::FuncOp funcOp;
    const clang::CXXMethodDecl *callOperator;
    SmallVector<const clang::VarDecl *, 4> captures;
  };
  SmallVector<PendingLiftedLambda, 2> pendingLiftedLambdas;
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
  /// FR-71: byte elements of `void *` parameters admitted as byte-slice
  /// cursors, keyed like `paramKindsCache` (canonical declaration) and
  /// index-aligned with its entry; a null slot (or an absent entry) means
  /// the parameter was not admitted. Filled by `classifyPointerParams`,
  /// read through `voidByteSliceElem`.
  llvm::DenseMap<const clang::FunctionDecl *, SmallVector<clang::QualType, 4>>
      voidByteElemsCache;
  /// FR-75: per-AST cache of `classifyTimeTraitEligible`'s TraitWhenLibrary
  /// leg (whether the TU's AST defines no `main`); each TU is scanned once.
  llvm::DenseMap<const clang::ASTContext *, bool> classifyTimeTraitCache;
  /// CTS 00204 va_list monomorphization plans, keyed by the variadic
  /// definition's canonical declaration (per-AST decls; entries from
  /// different TUs never conflict).
  llvm::DenseMap<const clang::FunctionDecl *, VaMonomorphPlan>
      vaMonomorphPlans;
  /// The clone index (into its plan's `clones`) of every direct call to a
  /// monomorphized variadic definition.
  llvm::DenseMap<const clang::CallExpr *, unsigned> vaCallSiteClones;
  /// W3.0: `mlirFuncName`s of externally visible va_list-using variadic
  /// definitions, gathered across every TU of a multi-TU project before any
  /// of them import (`collectCrossTuVaListVariadics`). Empty for a
  /// single-file import, where no cross-TU call site can exist. Consulted
  /// only when a call's callee has no definition visible in the CURRENT
  /// TU — the same-TU case is already resolved through `vaMonomorphPlans`.
  llvm::StringSet<> crossTuVaListVariadicNames;
  /// W3.2: whole-program facts, keyed by MLIR symbol name, gathered by
  /// `collectWholeProgramInfo` over every TU before any import. Empty for a
  /// single-file import. Nothing consumes it yet (built as staged substrate
  /// for the multi-TU gate relaxations); see `WholeProgramInfo`.
  WholeProgramInfo wholeProgram;
  /// Planned Shape-S cursor parameters (CTS 00204, element-generalized
  /// by C99-43 slice 1): the `T **` parameters of definitions whose
  /// bodies stay inside the bounded read-and-advance shape. Keyed by the
  /// DEFINITION's parameter decls.
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> cursorParams;
  /// Planned Shape-P paired out-cursor parameters (C99-43 slice 1b, the
  /// strtol/endp family): a `T **` parameter with NO reads of `*p` and
  /// exactly one unconditional top-level write `*p = <expr>` whose RHS
  /// roots in the mapped same-element slice-classified co-parameter.
  /// Lowers to ONE `&mut i64` input; the write assigns straight through
  /// the reference (no cell, no return-site writeback). Keyed by the
  /// DEFINITION's parameter decls; disjoint from `cursorParams`.
  llvm::DenseMap<const clang::ParmVarDecl *, const clang::ParmVarDecl *>
      pairedCursorParams;
  /// Per-function emission state for Shape-P parameters: the deref'd
  /// `!emitrust.lvalue<i64>` place of each paired out-cursor argument,
  /// assigned exactly once by the admitted write. Cleared with the other
  /// per-function maps.
  llvm::DenseMap<const clang::ParmVarDecl *, Value> pairedCursorPlaces;
  /// Planned Shape-G single-global-or-NULL out-param cursors (C99-43
  /// C1): a `T **` parameter with NO reads of `*p` and exactly one
  /// unconditional top-level write whose RHS is one whole statically
  /// known global, NULL, or `cond ? g : NULL`. Lowers to ONE
  /// `&mut Option<i64>` input written directly with `Some(0)`/`None`
  /// values (no cell, no return-site writeback). Keyed by the
  /// DEFINITION's parameter decls; disjoint from `cursorParams` and
  /// `pairedCursorParams`.
  llvm::DenseMap<const clang::ParmVarDecl *, GlobalCursorPlan>
      globalCursorParams;
  /// Per-function emission state for Shape-G parameters: the deref'd
  /// `!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>` place of each
  /// out-cell argument, assigned exactly once by the admitted write.
  /// Cleared with the other per-function maps.
  llvm::DenseMap<const clang::ParmVarDecl *, Value> globalCursorPlaces;
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
  /// Owner methods (Stage 1 of the owner-index-return extension) whose
  /// pointer return type was proven, at `planOwners` time, to always root
  /// in the SAME owner class as the method's own pointer parameter(s):
  /// every `return` operand's `resolveArgRoot` resolves to the method's
  /// class. Such a method's Rust result type is a plain i64 element index
  /// instead of hitting `classifyPointerReturn`'s rejection (which has no
  /// representation for a pointer into a callee-local/parameter region).
  /// Keyed by canonical declaration; disjoint concern from
  /// `pointerReturnKinds`/`globalReturnBases` (the CTS-P2/CTS-S pointer-
  /// return classifications), which never apply to a method (methods never
  /// reach `classifyPointerReturn`).
  llvm::DenseSet<const clang::FunctionDecl *> ownerIndexReturns;
  /// Per-function owner struct places (populated in the owning function
  /// only), keyed by the promoted base variable; feeds method-call
  /// receivers. The struct place is only ever borrowed, never loaded.
  llvm::DenseMap<const clang::VarDecl *, Value> ownerStructPlaces;
  /// W4.2e Part B (design.md FR-39): the index-handle node-pool plan of
  /// each promoting function, keyed by canonical declaration.
  llvm::DenseMap<const clang::FunctionDecl *, MallocPoolFacts> mallocPools;
  /// Every `struct T *` local that is a handle into its function's node
  /// pool (W4.2e Part B), mapped to the promoting function's canonical
  /// declaration. Consulted at `emitPointerLocal` to build a pool handle
  /// instead of the region-driven decomposition.
  llvm::DenseMap<const clang::VarDecl *, const clang::FunctionDecl *>
      poolHandleVars;
  /// FR-64: every `char *` local lifted to a `String` constant-fill binding,
  /// keyed by its declaration. Consulted at `emitLocalVar` (to build the
  /// `String::repeat` binding), in `emitPrintfStringArg`/`emitPuts` (to print
  /// the `String` by `Display`), in the `free` handler (a no-op — `String`
  /// drops at scope end), and by `stringValueLocalQuery` (to skip the
  /// pointer-region flat-backing model).
  llvm::DenseMap<const clang::VarDecl *, StringFillFacts> stringFillLocals;
  /// FR-64: the fill-loop and NUL-terminator statements of every recognized
  /// string-fill local, elided at `emitStmt` (they are fused into the
  /// `String::repeat` binding). The `free` call is intercepted separately in
  /// the free handler, not elided here.
  llvm::DenseSet<const clang::Stmt *> stringFillElidedStmts;
  /// FR-65: every `T *` local lifted to a `Vec<T>` runtime-sized heap buffer,
  /// keyed by its declaration. Consulted at `emitLocalVar` (to build the
  /// `vec![<zero>; n]` binding), at `emitSubscriptLValue` (to route `a[i]`
  /// read/write to the Vec index place), in the `free` handler (a no-op — the
  /// `Vec` drops at scope end), and by `vecValueLocalQuery` (to skip the
  /// pointer-region flat-backing model). Unlike `stringFillLocals` there is no
  /// companion elided-statement set: the `a[i] = x` writes are kept as real
  /// `Vec` index writes.
  llvm::DenseMap<const clang::VarDecl *, VecFacts> vecValueLocals;
  /// Self-referential node-pool fields (`next`) that render as the nullable
  /// pool index `Option<usize>` (W4.2e Part B); the emission diverts their
  /// struct-field type and read/write lowering.
  llvm::SmallPtrSet<const clang::FieldDecl *, 4> poolNextFields;
  /// The high-level `emitrust.collection` pool place of the function
  /// currently being imported (W4.2e Part B); null outside a pooling
  /// function. Set at function entry, consumed by malloc-append
  /// (`collection_push`) and every handle's member projection
  /// (`collection_at`); lowered to the concrete `[T; cap]` array + i64 cursor
  /// by the emitrust-lower-containers pass.
  Value currentPoolPlace;
  /// The `deref(arg0)` receiver place while importing a method body; null
  /// otherwise. Sibling method calls borrow it (rendering `(*self).m(...)`).
  Value currentReceiverPlace;
  /// The owner base variable of the method currently being imported; null
  /// when the current function is not a method.
  const clang::VarDecl *currentMethodOwner = nullptr;
  /// Whether the method currently being imported is an owner-index-return
  /// method (Stage 1): `emitReturnStmt` routes its return sites through the
  /// `emitPointerRValue`/cursor path instead of the integer-carrier (CTS-P3)
  /// path, even though both classify to a plain i64 result type. Always
  /// false outside a method (`currentMethodOwner` null).
  bool currentOwnerIndexReturn = false;
  /// W2.2: the raw entry-block receiver argument (an
  /// `!emitrust.mut_ref<!emitrust.struct<...>>` or
  /// `!emitrust.ref<!emitrust.struct<...>>`) while importing a genuine C++
  /// non-static member function body; null otherwise. `CXXThisExpr`
  /// resolves directly to this value (undereferenced), which lets the
  /// existing `->`-base rvalue path in `emitMemberBasePlace` deref it like
  /// any other pointer-typed base.
  Value currentCxxThisRef;
  /// Struct definitions already imported (keyed on the defining decl).
  llvm::SmallPtrSet<const clang::RecordDecl *, 8> importedRecords;
  /// Struct definitions whose import was REJECTED (keyed on the defining
  /// decl). Disjoint from the set above in effect, though not in membership:
  /// `importedRecords` is marked before the field walk, so a rejected record
  /// is in both and only this set says whether a struct_def exists. Consulted
  /// by `importRecord` so that naming a rejected type fails at the use site
  /// rather than emitting a reference to a struct that was never defined.
  llvm::SmallPtrSet<const clang::RecordDecl *, 4> rejectedRecords;
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
  /// Program-wide array-member-pointer bindings (Stage 2 of the
  /// owner-struct self-reference extension), keyed by field declaration:
  /// `planArrayMemberPointers` (Pass A, run after `planOwners`) populates
  /// an entry only for a field it proves every site of. Consulted FIRST —
  /// before `memberPtrBindings`/`poisonedPtrFields` — by
  /// `resolveMemberPointerBinding`'s callers (the member-pointer
  /// assignment emission and `emitPointerRValue`'s member-read branch);
  /// a field absent here falls through unchanged to that historical
  /// model.
  llvm::DenseMap<ArrayMemberPointerKey, ArrayMemberPointerFacts>
      arrayMemberPtrBindings;
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
  /// C99-43 C3: C `main`'s `char **argv` parameter when planning admitted
  /// EVERY use of it (whole-value `argv[i]` printf `%s` arguments and
  /// `argv[i][j]` byte reads as values) — the signature then carries the
  /// `!emitrust.argv_table` input. Null when argv is absent, unused, or
  /// used outside the admitted grammar (the historical signature-time
  /// rejection stands). Keyed by the DEFINITION's parameter decl;
  /// planning-scope, NOT reset per function.
  const clang::ParmVarDecl *mainArgvAdmittedParam = nullptr;
  /// Per-function emission state: the `!emitrust.argv_table` entry-block
  /// argument while translating an admitted `main`, null elsewhere. The
  /// translation-time argv intercepts (printf holes, `argv[i][j]`
  /// places) consume it; deliberately never entered into `symbols`.
  Value mainArgvTableValue;
  /// True while emitting the body of a va_list monomorphization clone
  /// (CTS 00204): enables the va_start/va_end/va_arg lowerings and the
  /// elision of `va_list` locals.
  bool currentVaCloneActive = false;
  /// The clone's extra-argument block values, in declared order; the
  /// va_arg dispatch selects among them by static type.
  SmallVector<Value, 8> currentVaExtras;
  /// Entry-block `memref<i64>` cell holding the clone's va_arg
  /// consumption cursor; va_start resets it to zero.
  Value currentVaCursorCell;
  /// String-cursor parameter writebacks of the function under
  /// construction (CTS 00204): (local i64 cursor cell, deref'd
  /// `!emitrust.lvalue<i64>` place of the in-out cursor parameter) pairs,
  /// copied out at every return site.
  SmallVector<std::pair<Value, Value>, 2> cursorWritebacks;
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
  /// True once a `%.Ns` (precision-bounded) `%s` slice argument has been
  /// imported; triggers the one-per-module emission of the
  /// `__emitrust_cstr_n` helper (stops at N bytes or the first NUL,
  /// whichever comes first, matching C's %s precision).
  bool needsCStrNHelper = false;
  /// True once the `__emitrust_cstr_n` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool cStrNHelperEmitted = false;
  /// True once an argv-fed `%s` hole has been imported (C99-43 C3);
  /// triggers the one-per-module emission of the raw `__emitrust_cstr_out`
  /// helper — NUL-scan + `write_all` of the raw bytes on the shared
  /// stdout handle, bypassing the Latin-1 `__emitrust_cstr` funnel that
  /// would double-encode non-ASCII argument bytes.
  bool needsCStrOutHelper = false;
  /// True once the `__emitrust_cstr_out` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool cStrOutHelperEmitted = false;
  /// True once an argv-fed `%.Ns` hole has been imported (C99-43 C3);
  /// triggers emission of the raw `__emitrust_cstr_n_out` helper (stops
  /// at N bytes or the first NUL, whichever comes first).
  bool needsCStrNOutHelper = false;
  /// True once the `__emitrust_cstr_n_out` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool cStrNOutHelperEmitted = false;
  /// True once an argv-fed `%c` hole has been imported (C99-43 C3);
  /// triggers emission of the raw `__emitrust_byte_out` helper (one raw
  /// byte via `write_all`, bypassing the ASCII-only `__emitrust_fmt_c`
  /// char widening).
  bool needsByteOutHelper = false;
  /// True once the `__emitrust_byte_out` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool byteOutHelperEmitted = false;
  /// True once a signed integer printf directive outside the 1:1 Rust
  /// format-spec subset (precision or '+'/' ' flags) has been imported;
  /// triggers emission of the `__emitrust_fmt_i64` wrapper (plus the
  /// shared `__emitrust_fmt_int` core).
  bool needsIntFormatSignedHelper = false;
  /// True once the `__emitrust_fmt_i64` wrapper has been emitted.
  bool intFormatSignedHelperEmitted = false;
  /// True once an unsigned integer printf directive outside the 1:1 Rust
  /// format-spec subset (precision or the '#' flag) has been imported;
  /// triggers emission of the `__emitrust_fmt_u64` wrapper (plus the
  /// shared `__emitrust_fmt_int` core).
  bool needsIntFormatUnsignedHelper = false;
  /// True once the `__emitrust_fmt_u64` wrapper has been emitted.
  bool intFormatUnsignedHelperEmitted = false;
  /// True once the shared `__emitrust_fmt_int` core (C99 7.19.6.1 integer
  /// directive rendering: precision, sign/prefix, width padding) has been
  /// emitted, so a multi-TU import never emits it twice.
  bool intFormatCoreHelperEmitted = false;
  /// True once a floating printf directive outside the bare-%f subset
  /// (%e/%E/%g/%G/%F, or %f with flags/width/precision) has been
  /// imported; triggers emission of the `__emitrust_fmt_float` helper
  /// family (exact C99 f/e/g rendering incl. the glibc %#g carry quirk).
  bool needsFloatFormatExtHelper = false;
  /// True once the `__emitrust_fmt_float` helper family has been emitted,
  /// so a multi-TU import never emits it twice.
  bool floatFormatExtHelperEmitted = false;
  /// True once a definition-less `sprintf` call has been imported;
  /// triggers the one-per-module emission of the `__emitrust_sprintf`
  /// helper that copies the formatted bytes plus a NUL terminator into
  /// the destination slice and returns the written length.
  bool needsSprintfHelper = false;
  /// True once the `__emitrust_sprintf` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool sprintfHelperEmitted = false;
  /// True once a definition-less `snprintf` call has been imported; triggers
  /// the one-per-module emission of the `__emitrust_snprintf` helper that
  /// writes at most `size - 1` formatted bytes plus a NUL into the
  /// destination slice (C's defined truncation) and returns the full length.
  bool needsSnprintfHelper = false;
  /// True once the `__emitrust_snprintf` helper has been emitted, so a
  /// multi-TU import never emits it twice.
  bool snprintfHelperEmitted = false;
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
  /// W4.2e Part B (FR-39): true once any node-pool field is promoted, so
  /// the `__emitrust_pool_*` Option<usize> helpers are emitted once.
  bool neededPoolHelpers = false;
  bool poolHelpersEmitted = false;
  /// Hosted FILE* helpers requested by lowered stdio calls
  /// (`requestFileHelper`); each is emitted once per module, in the fixed
  /// order of the `kFileHelpers` table (the `__EmitrustFile` enum first).
  llvm::StringSet<> neededFileHelpers;
  /// FILE* helpers already emitted, so a multi-TU import never emits one
  /// twice.
  llvm::StringSet<> emittedFileHelpers;
};


//===----------------------------------------------------------------------===//
// AST helpers
//===----------------------------------------------------------------------===//

/// Returns true if the statement tree rooted at `stmt` contains any C label
/// (`LabelStmt`). Iterative worklist traversal over the AST.
static inline bool containsLabelStmt(const clang::Stmt *stmt) {
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
static inline bool bodyUsesVaList(const clang::ASTContext &context,
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

static inline const clang::Expr *stripTrivia(const clang::Expr *expr);
static inline bool isPointerType(clang::QualType type);
static inline bool isFunctionPointer(clang::QualType type);

/// Returns the expression under `expr`'s implicit casts and trivia — the
/// DeclRefExpr node itself for the `ap` operand of va_start/va_end/va_arg
/// (CTS 00204 scope checks key consumed references by node identity).
static inline const clang::Expr *strippedImplicitRef(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    e = stripTrivia(cast->getSubExpr());
  return e;
}

/// Returns whether `place` writes through MORE than the single cursor
/// dereference of the string-cursor parameter `param` (CTS 00204):
/// `**s = c` or `(*s)[k] = c` write region content, which the shared
/// slice lowering cannot accept; `*s = p` (depth one) is the legal
/// advancement.
static inline bool writesThroughCursorParam(const clang::Expr *place,
                                     const clang::ParmVarDecl *param) {
  unsigned depth = 0;
  const clang::Expr *e = stripTrivia(place);
  while (true) {
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
        unary && unary->getOpcode() == clang::UO_Deref) {
      ++depth;
      e = stripTrivia(unary->getSubExpr());
      continue;
    }
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
      ++depth;
      e = stripTrivia(subscript->getBase());
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      e = stripTrivia(cast->getSubExpr());
      continue;
    }
    break;
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  return ref && ref->getDecl() == param && depth >= 2;
}

/// Recursively scans `stmt` for a use of the candidate string-cursor
/// parameter `param` outside the bounded shape (CTS 00204). Legal uses
/// read through `*param` or advance it with `*param = <expr>`; the bare
/// parameter as a value (stored into a global, passed to another call),
/// its address, and writes deeper than the cursor dereference all escape.
/// Returns the offending expression, or null when the shape holds.
static inline const clang::Expr *
findCursorParamEscape(const clang::Stmt *stmt,
                      const clang::ParmVarDecl *param) {
  if (!stmt)
    return nullptr;
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt);
      binary && binary->isAssignmentOp() &&
      writesThroughCursorParam(binary->getLHS(), param))
    return binary->getLHS();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
    if (unary->isIncrementDecrementOp() &&
        writesThroughCursorParam(unary->getSubExpr(), param))
      return unary->getSubExpr();
    if (unary->getOpcode() == clang::UO_Deref)
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
              strippedImplicitRef(unary->getSubExpr()));
          ref && ref->getDecl() == param)
        return nullptr; // `*param`: the bounded read/advance shape.
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt);
      ref && ref->getDecl() == param)
    return ref; // The bare parameter escapes.
  for (const clang::Stmt *child : stmt->children())
    if (const clang::Expr *hit = findCursorParamEscape(child, param))
      return hit;
  return nullptr;
}

/// Collects every pointer-typed local variable declared inside `stmt`
/// (data pointers only; function pointers are ordinary values). Used by
/// the string-cursor planning pass to interrogate their regions.
static inline void collectLocalPointerDecls(
    const clang::Stmt *stmt,
    SmallVectorImpl<const clang::VarDecl *> &locals) {
  if (!stmt)
    return;
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
        if (var->hasLocalStorage() && isPointerType(var->getType()) &&
            !isFunctionPointer(var->getType()))
          locals.push_back(var);
  for (const clang::Stmt *child : stmt->children())
    collectLocalPointerDecls(child, locals);
}

/// Strips parentheses, `ConstantExpr` wrappers (clang wraps constant
/// contexts such as case values in `ConstantExpr`), and `ExprWithCleanups`
/// wrappers (clang marks full expressions containing block-scope compound
/// literals, whose "cleanup" is the end of the object's lifetime — nothing
/// to emit, the temp's place is ordinary SSA) without touching casts.
static inline const clang::Expr *stripTrivia(const clang::Expr *expr) {
  while (true) {
    expr = expr->IgnoreParens();
    if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(expr)) {
      expr = constant->getSubExpr();
      continue;
    }
    if (const auto *cleanups =
            llvm::dyn_cast<clang::ExprWithCleanups>(expr)) {
      expr = cleanups->getSubExpr();
      continue;
    }
    return expr;
  }
}

/// Returns the string literal an expression carries in a literal position:
/// the literal itself, or the function-name literal of a `__func__`-family
/// predefined identifier (C99 6.4.2.2 defines `__func__` as if a
/// `static const char` array holding the function name existed; clang
/// materializes exactly that array's contents as a `StringLiteral` inside
/// the `PredefinedExpr`, so every literal consumer — printf `%s`, the
/// read-only literal-region machinery — treats the two identically).
/// Returns null for any other expression.
static inline const clang::StringLiteral *
underlyingStringLiteral(const clang::Expr *expr) {
  if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(expr))
    return literal;
  if (const auto *predefined = llvm::dyn_cast<clang::PredefinedExpr>(expr))
    return predefined->getFunctionName();
  return nullptr;
}

/// Returns the defining declaration of `type`'s complete named enum, or
/// null when `type` is not an enum, incomplete, or anonymous.
static inline const clang::EnumDecl *namedEnumDeclOf(clang::QualType type) {
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
static inline std::optional<EnumOperand>
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
static inline const clang::Stmt *findNestedSwitchLabel(const clang::Stmt *stmt) {
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
static inline bool isPlainSwitchBody(const clang::CompoundStmt *body) {
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
static inline bool isUnsignedInt(Type type) {
  auto intType = llvm::dyn_cast<IntegerType>(type);
  return intType && intType.isUnsigned();
}

/// W2.0 C++ input tolerance: whether `init` is exactly the implicit,
/// no-op default-construction C++ wraps a class-typed declaration with
/// NO explicit initializer in. `struct Point p;` has no initializer
/// expression at all in C, but in C++ the same declaration's `VarDecl`
/// carries a `CXXConstructExpr` calling `Point`'s default constructor
/// (`callinit`) even though nothing changes at runtime — plain data
/// members are left uninitialized exactly like C. True only when the
/// constructed class's default constructor is TRIVIAL (C++11
/// [class.default.ctor]: has no effect) and the call carries no
/// arguments; a class with a NON-trivial default constructor (one this
/// wave never imports, since methods are not visited) keeps producing a
/// real `CXXConstructExpr` here, which is intentionally left for the
/// generic aggregate-initializer rejection below to catch — silently
/// skipping real constructor side effects would be a miscompile, not a
/// merely unsupported construct.
static inline bool isVacuousDefaultConstruct(const clang::Expr *init) {
  const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(init);
  if (!construct || construct->getNumArgs() != 0)
    return false;
  const clang::CXXConstructorDecl *ctor = construct->getConstructor();
  return ctor && ctor->getParent()->hasTrivialDefaultConstructor();
}

/// The initializer expression import should see for `var`: its clang
/// initializer, or null if `var` has none — INCLUDING the case where
/// C++ synthesized a vacuous default-construction wrapper a C
/// declaration would never carry (W2.0, see `isVacuousDefaultConstruct`).
/// Plain C input is unaffected: `getInit()` is already null there
/// whenever this returns null.
static inline const clang::Expr *
significantInit(const clang::VarDecl *var) {
  const clang::Expr *init = var->getInit();
  if (init && isVacuousDefaultConstruct(init))
    return nullptr;
  return init;
}

/// Returns whether the canonical type of `type` is a C pointer type.
static inline bool isPointerType(clang::QualType type) {
  return type.getCanonicalType()->isPointerType();
}

/// Returns whether the canonical type of `type` is a C function pointer.
/// Function pointers are ordinary `!emitrust.fn_ptr` values and take none
/// of the data-pointer (decomposition or reference-parameter) paths.
static inline bool isFunctionPointer(clang::QualType type) {
  return type.getCanonicalType()->isFunctionPointerType();
}

/// Returns whether `type` is a data pointer: a C pointer that is not a
/// function pointer. Data pointers decompose into (base, cursor) pairs;
/// function pointers are ordinary Copy values.
static inline bool isDataPointer(clang::QualType type) {
  return isPointerType(type) && !isFunctionPointer(type);
}

/// FR-48: the referent type of a C++ LVALUE reference `type`, or a null
/// QualType when `type` is not one.
///
/// An lvalue reference is exactly a data pointer that is non-null, never
/// reseated, and never subject to arithmetic — a STRICTLY simpler case of
/// the pointer model, which is why `mapParamType` maps it onto the very
/// borrow types a `ParamKind::ScalarRef` pointer parameter already uses
/// instead of growing a parallel path. RVALUE references (`T&&`) are
/// deliberately NOT reported here: binding one implies a move, and the
/// model has no ownership transfer to express it with.
static inline clang::QualType cxxReferentType(clang::QualType type) {
  if (const auto *ref =
          type.getCanonicalType()->getAs<clang::LValueReferenceType>())
    return ref->getPointeeType();
  return clang::QualType();
}

/// FR-48: returns whether `decl` is declared with a C++ lvalue reference
/// type. Such a declaration's `symbols` entry is the BORROW of the
/// referent, not a place of its own — the same binding a scalar-reference
/// pointer parameter gets — so its uses dereference once per access
/// (`emitDeclRefLValue`) exactly the way a `this` receiver does.
static inline bool isCxxReferenceDecl(const clang::ValueDecl *decl) {
  return decl && !cxxReferentType(decl->getType()).isNull();
}

/// The pointee of an `!emitrust.ref<T>` or `!emitrust.mut_ref<T>` value
/// type, or a null Type for anything else. The single place the two borrow
/// types are destructured; every consumer of a borrow SSA value (`this`
/// receivers, scalar-reference pointer parameters, C++ reference
/// parameters) reaches its referent through this.
static inline Type borrowPointee(Type type) {
  if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(type))
    return mutRef.getPointee();
  if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(type))
    return sharedRef.getPointee();
  return Type();
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
static inline bool hasVolatileQualifier(clang::ASTContext &context,
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
static inline bool isFilePtrType(clang::QualType type) {
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
static inline bool isFileHandleLocal(const clang::VarDecl *var) {
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

/// Returns whether a declared type names `va_list` / `__builtin_va_list`
/// through its typedef sugar. The C99-37 rejection cannot rely on the
/// CANONICAL type everywhere: on AArch64 Darwin `__builtin_va_list` is
/// plain `char *`, so a canonical comparison either misses the va_list
/// (the type-level checks) or swallows every `char *` (the clone-local
/// skip). The sugar names the user's intent unambiguously on every
/// target; x86_64's `__va_list_tag [1]` record form keeps its canonical
/// checks for expression positions that have lost the sugar.
static inline bool isVaListSugarType(clang::QualType type,
                                     clang::ASTContext &context) {
  const clang::TypedefNameDecl *builtinDecl =
      context.getBuiltinVaListDecl();
  for (clang::QualType current = type; !current.isNull();) {
    const auto *typedefType = current->getAs<clang::TypedefType>();
    if (!typedefType)
      return false;
    const clang::TypedefNameDecl *decl = typedefType->getDecl();
    if (builtinDecl &&
        decl->getCanonicalDecl() == builtinDecl->getCanonicalDecl())
      return true;
    current = decl->getUnderlyingType();
  }
  return false;
}

/// Returns the standard C name of a hosted stdio stream variable,
/// canonicalizing Darwin's spellings: on macOS `<stdio.h>` defines
/// `stdin`/`stdout`/`stderr` as macros for the libc globals
/// `__stdinp`/`__stdoutp`/`__stderrp`, so the AST decl the importer sees
/// carries the dunder name even though the user wrote the ISO spelling.
/// Any other name passes through unchanged. Used both for recognition
/// (the devirtualized-fprintf stdout check) and for diagnostics, which
/// should print the name the user actually wrote.
static inline llvm::StringRef canonicalStreamName(llvm::StringRef name) {
  return llvm::StringSwitch<llvm::StringRef>(name)
      .Case("__stdinp", "stdin")
      .Case("__stdoutp", "stdout")
      .Case("__stderrp", "stderr")
      .Default(name);
}

/// Returns the data-pointer field a member expression designates, or null
/// when `expr` is not a member access or its field is not a data pointer.
static inline const clang::FieldDecl *dataPointerFieldOf(const clang::Expr *expr) {
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
static inline const clang::Expr *peelPointerCast(clang::ASTContext &context,
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
static inline const clang::Expr *stripObjectPointerCasts(clang::ASTContext &context,
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
static inline bool viewsChangedPointee(clang::ASTContext &context,
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
static inline bool isStaticallyNullRegion(const PointerRegion *region) {
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
static inline bool isCarrierRegion(const PointerRegion *region) {
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
static inline MemberAddressTarget
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
static inline const clang::Expr *returnedFunctionExpr(const clang::Expr *expr) {
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
static inline ReturnedGlobalAddress returnedGlobalAddress(const clang::Expr *expr) {
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
static inline void collectReturnStmts(
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
static inline const clang::RecordDecl *recordOfType(clang::QualType type) {
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
static inline bool recordHasDataPointerField(const clang::RecordDecl *record) {
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
static inline uint64_t flatElementCount(clang::ASTContext &context,
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
static inline bool pointsToArray(clang::QualType type) {
  return type.getCanonicalType()->getPointeeType()->isArrayType();
}

/// Returns the local, non-parameter variable a stripped declaration
/// reference `expr` names, or null when `expr` is not such a reference.
static inline const clang::VarDecl *asLocalVarRef(const clang::Expr *expr) {
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
static inline const clang::VarDecl *asLoadedLocalVarRef(const clang::Expr *expr) {
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
static inline bool isSecondOrderPointerType(clang::QualType type) {
  return isPointerType(type) &&
         isPointerType(type.getCanonicalType()->getPointeeType());
}

/// Returns whether `type` is the two-level cursor-parameter shape
/// `T **` (C99-43 slice 1, generalizing the CTS-00204 `const char **`
/// string cursor): exactly two pointer levels whose element T is a
/// non-void, non-function, non-pointer type a slice can view. Deeper
/// nesting (T***) and void** keep the historical pointer-to-pointer
/// parameter rejection.
static inline bool isDataPointerPointerType(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType();
  const auto *outer = canonical->getAs<clang::PointerType>();
  if (!outer)
    return false;
  const auto *inner =
      outer->getPointeeType().getCanonicalType()->getAs<clang::PointerType>();
  if (!inner)
    return false;
  clang::QualType element = inner->getPointeeType().getCanonicalType();
  return !element->isVoidType() && !element->isFunctionType() &&
         !element->isPointerType();
}

/// The element type a `T **` cursor parameter walks: the pointee of the
/// parameter's POINTEE (`int **p` walks a run of `int`). Only meaningful
/// for types `isDataPointerPointerType` accepts.
static inline clang::QualType
pointerPointerElementType(clang::QualType type) {
  return type.getCanonicalType()
      ->getPointeeType()
      .getCanonicalType()
      ->getPointeeType();
}

/// Matches `*s` where `s` is a pointer-to-pointer PARAMETER read through
/// the usual lvalue-to-rvalue load: the shape every use of a string-cursor
/// parameter reduces to (CTS 00204). Returns the parameter or null.
static inline const clang::ParmVarDecl *
asPointerPointerParamDeref(const clang::Expr *expr) {
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(expr));
  if (!unary || unary->getOpcode() != clang::UO_Deref)
    return nullptr;
  const clang::Expr *sub = stripTrivia(unary->getSubExpr());
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(sub))
    sub = stripTrivia(cast->getSubExpr());
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(sub);
  const auto *param =
      ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
  if (!param || !isSecondOrderPointerType(param->getType()))
    return nullptr;
  return param;
}

/// Returns the local variable or parameter a stripped declaration
/// reference `expr` names, or null when `expr` is not such a reference.
/// Used by the pointer choke points that accept both decomposed pointer
/// locals and slice-classified pointer parameters.
static inline const clang::VarDecl *asVarRef(const clang::Expr *expr) {
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
static inline const clang::VarDecl *asGlobalDataPointerRef(const clang::Expr *expr) {
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
static inline const clang::CallExpr *asAllocCall(const clang::Expr *expr) {
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

/// Returns the one-argument `free(p)` call `expr` names (after cast/paren
/// stripping), or null when it is not a call to the definition-less libc
/// `free` (W4.2e Part A). A project-supplied `free` definition keeps its
/// ordinary-call lowering (the `hasBody` guard, mirroring `asAllocCall`).
static inline const clang::CallExpr *asFreeCall(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    e = stripTrivia(cast->getSubExpr());
  const auto *call = llvm::dyn_cast<clang::CallExpr>(e);
  if (!call)
    return nullptr;
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee || callee->hasBody() || !callee->getIdentifier())
    return nullptr;
  if (callee->getName() != "free" || call->getNumArgs() != 1)
    return nullptr;
  return call;
}

/// Returns the pointer-typed parameter a stripped (possibly
/// lvalue-to-rvalue-wrapped) declaration reference `expr` names, or null
/// when `expr` is not such a reference.
static inline const clang::ParmVarDecl *asPointerParamRef(const clang::Expr *expr) {
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
static inline void collectSliceParams(
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

static inline bool voidParamOnlyTruthTested(const clang::Stmt *stmt,
                                     const clang::ParmVarDecl *param);

/// Scans a condition expression for the integer-carrier classification of
/// `void *` parameters (CTS-P3): bare loads of `param` are consumed as
/// truth tests, looking through parens, `PointerToBoolean` conversions,
/// `!`, and the `&&`/`||` connectives; any other subexpression is walked
/// generally (so a use of `param` inside it disqualifies).
static inline bool voidParamCondOk(const clang::Expr *cond,
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
static inline bool voidParamOnlyTruthTested(const clang::Stmt *stmt,
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

/// FR-71: the byte-cursor admission scan for a `void *` parameter.
/// Returns whether EVERY use of `param` below `stmt` is either (a) a
/// conversion (implicit or explicit `CK_BitCast`) to ONE consistent
/// byte-pointee pointer type — `char *` or `unsigned char *` (which
/// `uint8_t *` canonicalizes to) — or (b, FR-72) a bare reference passed
/// DIRECTLY as a REGION argument of a modeled definition-less
/// byte-family libc call (memset's destination, memcpy/memmove/memcmp's
/// two regions), which consumes the parameter as an unsigned-char region
/// — the sibling `uint8_t *` convention; memset's own `void *` parameter
/// wraps the argument in NO cast, so clause (a) can never see the
/// tinycrypt `_set` shape whose body is ONLY `memset(to, val, len)`.
/// `elem` accumulates the pointee (canonical, unqualified) across the
/// walk. Such a parameter acts exactly like a byte pointer the body
/// renamed, so it admits as a byte-slice cursor (`ParamKind::Slice` with
/// `elem` as the slice element). The scan is deliberately conservative,
/// mirroring `voidParamOnlyTruthTested`: any reference of `param` that
/// no qualifying conversion or byte-family region position consumed — a
/// non-byte or mixed-pointee conversion, arithmetic, comparison, a store
/// of the pointer itself, a return, a truth test, any other call
/// argument (including a byte-family COUNT position) — returns false,
/// keeping the historical void-pointer-parameter rejection verbatim. A
/// body with NO use at all leaves `elem` null and the caller declines
/// the admission (an unused `void *` is already the integer-carrier
/// class, CTS-P3).
static inline bool voidParamByteUsesOk(const clang::Stmt *stmt,
                                       const clang::ParmVarDecl *param,
                                       clang::QualType &elem) {
  if (!stmt)
    return true;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee && callee->getDeclName().isIdentifier() &&
        !callee->getDefinition() && call->getNumArgs() == 3) {
      llvm::StringRef name = callee->getName();
      unsigned regionArgs = name == "memset" ? 1
                            : name == "memcpy" || name == "memmove" ||
                                    name == "memcmp"
                                ? 2
                                : 0;
      if (regionArgs) {
        clang::ASTContext &ctx = param->getASTContext();
        for (auto [index, arg] : llvm::enumerate(call->arguments())) {
          // The region positions accept the bare parameter reference
          // under the implicit value/qualification/void* adjustments a
          // call argument carries; anything else in ANY position walks
          // generally (so clause (a) still consumes a cast shape, and an
          // unconsumed reference still disqualifies).
          const clang::Expr *e = stripTrivia(arg);
          while (const auto *argCast =
                     llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
            if (argCast->getCastKind() != clang::CK_LValueToRValue &&
                argCast->getCastKind() != clang::CK_NoOp &&
                argCast->getCastKind() != clang::CK_BitCast)
              break;
            e = stripTrivia(argCast->getSubExpr());
          }
          const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
          if (index < regionArgs && ref && ref->getDecl() == param) {
            clang::QualType pointee = ctx.UnsignedCharTy;
            if (elem.isNull())
              elem = pointee;
            else if (!ctx.hasSameType(elem, pointee))
              return false; // Mixed with an i8 cast elsewhere in the body.
            continue; // The region position consumed the reference.
          }
          if (!voidParamByteUsesOk(arg, param, elem))
            return false;
        }
        return voidParamByteUsesOk(call->getCallee(), param, elem);
      }
    }
  }
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(stmt))
    if ((llvm::isa<clang::ImplicitCastExpr>(cast) ||
         llvm::isa<clang::CStyleCastExpr>(cast)) &&
        cast->getCastKind() == clang::CK_BitCast &&
        asPointerParamRef(cast->getSubExpr()) == param) {
      clang::QualType to = cast->getType().getCanonicalType();
      if (!to->isPointerType())
        return false;
      clang::ASTContext &ctx = param->getASTContext();
      clang::QualType pointee =
          to->getPointeeType().getCanonicalType().getUnqualifiedType();
      if (!ctx.hasSameType(pointee, ctx.CharTy) &&
          !ctx.hasSameType(pointee, ctx.UnsignedCharTy))
        return false; // Non-byte pointee: outside the subset.
      if (elem.isNull())
        elem = pointee;
      else if (!ctx.hasSameType(elem, pointee))
        return false; // Mixed byte pointees: the element is ambiguous.
      return true; // The conversion consumed the reference below it.
    }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (ref->getDecl() == param)
      return false; // A use no byte conversion consumed.
  for (const clang::Stmt *child : stmt->children())
    if (!voidParamByteUsesOk(child, param, elem))
      return false;
  return true;
}

/// FR-75: one call-site argument of the `void *` consensus scan. Returns
/// whether `arg` is an admissible byte VIEW: after the implicit argument
/// adjustments (decay, the cast to `void *`), the underlying expression's
/// type is a byte array or a byte pointer (`char` / `unsigned char`
/// element — which `uint8_t` canonicalizes to). `elem` accumulates the
/// element (canonical, unqualified) across the walk exactly like FR-71's
/// body scan: mixed byte pointees make the element ambiguous and decline.
/// Deliberately a TYPE-shape test only — a byte-typed argument that still
/// has no region behind it (`&x` on a scalar) is admitted here and then
/// rejected LOCATED at the call-site slice lowering, so over-admission
/// can never silently borrow one element.
static inline bool voidParamCallSiteByteViewOk(const clang::Expr *arg,
                                               clang::ASTContext &ctx,
                                               clang::QualType &elem) {
  const clang::Expr *e = arg->IgnoreParenImpCasts();
  clang::QualType type = e->getType();
  clang::QualType pointee;
  if (const clang::ArrayType *array = ctx.getAsArrayType(type))
    pointee = array->getElementType();
  else if (type->isPointerType())
    pointee = type->getPointeeType();
  else
    return false;
  pointee = pointee.getCanonicalType().getUnqualifiedType();
  if (!ctx.hasSameType(pointee, ctx.CharTy) &&
      !ctx.hasSameType(pointee, ctx.UnsignedCharTy))
    return false;
  if (elem.isNull())
    elem = pointee;
  else if (!ctx.hasSameType(elem, pointee))
    return false; // Mixed byte pointees: the element is ambiguous.
  return true;
}

/// FR-75: the CALL-SITE CONSENSUS scan for a declaration-only `void *`
/// parameter under a trait policy. FR-71's admission is a body-usage fact
/// and cannot run without a body, so a body-less function's `void *`
/// parameter is admitted from the OUTSIDE instead: every direct call to
/// `callee` below `stmt` must pass an admissible byte view (one
/// consistent element, `voidParamCallSiteByteViewOk`) in position
/// `index`. `sawCall` records that at least one call site exists — a
/// parameter no call constrains has no consensus to join and keeps the
/// verbatim rejection. Any non-byte or non-view call site returns false.
static inline bool voidParamCallSitesAllByteViews(
    const clang::Stmt *stmt, const clang::FunctionDecl *callee,
    unsigned index, clang::QualType &elem, bool &sawCall) {
  if (!stmt)
    return true;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    const clang::FunctionDecl *direct = call->getDirectCallee();
    if (direct &&
        direct->getCanonicalDecl() == callee->getCanonicalDecl()) {
      if (index >= call->getNumArgs())
        return false;
      sawCall = true;
      if (!voidParamCallSiteByteViewOk(call->getArg(index),
                                       direct->getASTContext(), elem))
        return false;
    }
  }
  for (const clang::Stmt *child : stmt->children())
    if (!voidParamCallSitesAllByteViews(child, callee, index, elem,
                                        sawCall))
      return false;
  return true;
}

/// FR-48: the place expression under a C++ reference argument's implicit
/// adjustments. Binding an `int` lvalue to a `const int &` parameter adds a
/// `CK_NoOp` cast to the const-qualified type; that cast changes nothing
/// about WHICH object is named, so both the root walk and the borrow look
/// straight through it. Only `CK_NoOp` is peeled — an `LValueToRValue`
/// would mean a genuine value read rather than a place, and peeling it
/// would silently borrow the wrong thing.
static inline const clang::Expr *stripLValueNoOp(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  return e;
}

/// Returns the variable at the root of a PLACE expression (`x`, `s.f`,
/// `arr[i]`, and chains of those), or null when no single named root is
/// known. Factored out of `addressArgumentRoot` (FR-48) so a C++ reference
/// argument, which names the place directly with no address-of node,
/// identifies its root by exactly the same walk the `&`-spelled C argument
/// does — one definition of "which object does this borrow name", so both
/// spellings land in `emitCall`'s same-base aliasing rejection alike.
static inline const clang::VarDecl *placeExprRoot(const clang::Expr *expr) {
  const clang::Expr *place = stripLValueNoOp(expr);
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

/// FR-48: returns whether `expr` is a place rooted at the current method's
/// receiver — `*this`, `this->f`, `(*this).f[i]`, and chains of those.
/// A `VarDecl` root cannot express "the receiver", so the same-object
/// aliasing check in `emitCXXMemberCall` needs this second rooting test
/// alongside `placeExprRoot` to cover `m(*this)`.
static inline bool rootsAtCxxThis(const clang::Expr *expr) {
  const clang::Expr *place = stripLValueNoOp(expr);
  while (true) {
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(place);
        unary && unary->getOpcode() == clang::UO_Deref) {
      place = stripLValueNoOp(unary->getSubExpr());
      continue;
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(place)) {
      place = stripLValueNoOp(member->getBase());
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(place)) {
      place = stripLValueNoOp(subscript->getBase());
      continue;
    }
    break;
  }
  return llvm::isa<clang::CXXThisExpr>(place);
}

/// Returns the local variable at the root of an address-of call argument
/// (`&x`, `&s.f`, `&arr[i]`), or null when no single local root is known.
/// Feeds the same-base aliasing rejection of `emitCall`.
static inline const clang::VarDecl *
addressArgumentRoot(const clang::Expr *expr) {
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(expr));
  if (!unary || unary->getOpcode() != clang::UO_AddrOf)
    return nullptr;
  return placeExprRoot(unary->getSubExpr());
}

/// FR-48: the parameter list a call's arguments bind to, or null when the
/// callee is not a resolved `FunctionDecl`. A `CXXMemberCallExpr`'s
/// arguments line up with the method's parameters (the receiver is NOT an
/// argument), so both call shapes share this accessor.
static inline const clang::FunctionDecl *
calleeParamSource(const clang::CallExpr *call) {
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee)
    return nullptr;
  return callee->getDefinition() ? callee->getDefinition() : callee;
}

#endif // EMITRUST_IMPORTC_CIMPORTERINTERNAL_H
