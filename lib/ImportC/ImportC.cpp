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
  /// `emitrust.global`s). Other declarations are rejected.
  ///
  /// `tuTag` is prepended to internal-linkage (`static`) symbol names so that
  /// identically named file-statics in different translation units stay
  /// distinct; it is empty for a single-TU import (bare names, historical
  /// behavior). `deferExtern` controls whether an `extern`-only global with no
  /// definition in this TU is an immediate error (single-file) or deferred for
  /// cross-TU resolution (project). Repeated calls accumulate into one module.
  LogicalResult importTranslationUnit(clang::ASTContext &context,
                                      llvm::StringRef tuTag, bool deferExtern);

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

  /// Maps a C value type to its MLIR type: `_Bool`->i1, char->i8,
  /// short->i16, int->i32, long/long long->i64, unsigned char->ui8,
  /// unsigned short->ui16, unsigned int->ui32, unsigned long/long
  /// long->ui64 (unsigned types map to MLIR *unsigned* integer types, not
  /// signless ones, so that they render as Rust `uN`), float->f32,
  /// double->f64, `struct S`->`!emitrust.struct<"S">`,
  /// `T[N]`->`!emitrust.array<NxT>`, complete named
  /// `enum E`->`!emitrust.enum<"E">`. Typedefs resolve through the
  /// canonical type. Pointers, unions, anonymous enums, and everything
  /// else produce a located diagnostic.
  FailureOr<Type> mapType(clang::QualType type, Location loc);

  /// Maps a C function-parameter type: pointer parameters `T*` become
  /// `!emitrust.mut_ref<T>`, everything else maps like `mapType`.
  FailureOr<Type> mapParamType(clang::QualType type, Location loc);

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Imports a complete named struct definition as a module-level
  /// `emitrust.struct_def`. Forward declarations are ignored; repeated
  /// imports of the same definition are deduplicated. Unions, anonymous
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
  LogicalResult importFunction(const clang::FunctionDecl *func);

  /// Records every local variable whose address is taken with `&x` inside
  /// `stmt`; such scalars become `emitrust.variable` places instead of
  /// promotable memref cells.
  void collectAddressTaken(const clang::Stmt *stmt);

  /// Imports a file-scope variable as a module-level `emitrust.global`.
  /// Redeclarations are reconciled the way C does: the definition (or a
  /// tentative definition) provides the type and, through
  /// `getAnyInitializer`, the initializer; a variable that is only ever
  /// `extern`-declared in this TU is rejected. Thread-locals are rejected.
  LogicalResult importGlobalVar(const clang::VarDecl *var);

  /// Creates the `emitrust.global` named `symbolName` for the declaration
  /// `decl` (which supplies the type and initializer) and registers it
  /// under the canonical declaration `key`. Rejects Rust-keyword names,
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
  /// evaluation) and converts it to a typed attribute of `type`. Supports
  /// integer (including `_Bool` and char) and floating-point constants;
  /// aggregate initializer lists and non-constant expressions are rejected
  /// with located diagnostics.
  FailureOr<Attribute> convertGlobalInit(const clang::VarDecl *decl,
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
  /// diagnostic.
  LogicalResult emitStmt(const clang::Stmt *stmt);

  /// Emits a local variable declaration. Signed scalars become entry-block
  /// memref cells (initializer stored at the declaration point);
  /// aggregates, enums, unsigned scalars, and address-taken scalars become
  /// `emitrust.variable` places. Function-local statics become module-level
  /// `emitrust.global`s mangled as `<function>_<name>`; extern locals are
  /// rejected.
  LogicalResult emitLocalVar(const clang::VarDecl *var);

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

  /// Emits a compound assignment (`+=` etc.) as load, arithmetic, store.
  LogicalResult emitCompoundAssign(const clang::CompoundAssignOperator *op);

  /// Emits a compound assignment and returns the assigned-to place, for
  /// value-position uses (see `emitAssignToPlace`).
  FailureOr<Value>
  emitCompoundAssignToPlace(const clang::CompoundAssignOperator *op);

  /// Emits statement-level `++x`/`x--` as load, add/sub 1, store; only
  /// integer operands are supported.
  LogicalResult emitIncDec(const clang::UnaryOperator *op);

  /// Emits `++`/`--` in value position: performs the store like
  /// `emitIncDec` and yields the expression's C value — the pre-value for
  /// the postfix forms, the post-value for the prefix forms.
  FailureOr<Value> emitIncDecValue(const clang::UnaryOperator *op);

  /// Emits a call statement, dispatching printf to `emitPrintf` and
  /// discarding the result of ordinary calls.
  LogicalResult emitCallStmt(const clang::CallExpr *call);

  /// Lowers a printf call with a literal format string to
  /// `emitrust.call_opaque "print!"` with a translated Rust format string
  /// in the `args` attribute. Supports %d (i32), %ld (i64), %f (f64,
  /// routed through the `__emitrust_fmt_f64` helper so that non-finite
  /// values print with C's spellings), and %%; anything else is rejected.
  LogicalResult emitPrintf(const clang::CallExpr *call);

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
  FailureOr<Value> emitCall(const clang::CallExpr *call);

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
};

} // namespace

//===----------------------------------------------------------------------===//
// AST helpers
//===----------------------------------------------------------------------===//

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
    if (definition->getName().empty())
      return emitError(loc) << "unsupported: anonymous struct type";
    if (failed(importRecord(definition, loc)))
      return failure();
    return Type(
        emitrust::StructType::get(builder.getContext(), definition->getName()));
  }

  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(canonical)) {
    FailureOr<Type> element = mapType(array->getElementType(), loc);
    if (failed(element))
      return failure();
    if (llvm::isa<emitrust::ArrayType>(*element))
      return emitError(loc) << "unsupported: multi-dimensional array";
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

  if (canonical->isPointerType())
    return emitError(loc)
           << "unsupported: pointer type outside a parameter position";
  if (canonical->isArrayType())
    return emitError(loc) << "unsupported: non-constant array size";
  return emitError(loc) << "unsupported type '"
                        << llvm::Twine(canonical.getAsString()) << "'";
}

FailureOr<Type> CImporter::mapParamType(clang::QualType type, Location loc) {
  clang::QualType canonical = type.getCanonicalType();
  if (canonical->isPointerType()) {
    clang::QualType pointee = canonical->getPointeeType();
    if (pointee.getCanonicalType()->isPointerType())
      return emitError(loc) << "unsupported: pointer-to-pointer parameter";
    FailureOr<Type> inner = mapType(pointee, loc);
    if (failed(inner))
      return failure();
    return Type(emitrust::MutRefType::get(*inner));
  }
  return mapType(type, loc);
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
  if (definition->getName().empty())
    return emitError(defLoc) << "unsupported: anonymous struct type";
  if (isRustKeyword(definition->getName()))
    return emitError(defLoc) << "unsupported: struct name '"
                             << definition->getName()
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
  if (fieldNames.empty())
    return emitError(defLoc) << "unsupported: struct with no members";

  // Cross-TU deduplication: the same struct reached through a shared header
  // has distinct decls in each TU. Dedup by symbol name; an identical shape is
  // skipped, a name reused with a different field shape is a diagnostic.
  std::string shape;
  {
    llvm::raw_string_ostream os(shape);
    for (auto [fieldName, fieldType] : llvm::zip(fieldNames, fieldTypes))
      os << fieldName << ':' << fieldType << ';';
  }
  auto existingShape = importedRecordShapes.find(definition->getName());
  if (existingShape != importedRecordShapes.end()) {
    if (existingShape->second != shape)
      return emitError(defLoc)
             << "unsupported: conflicting definition of struct '"
             << definition->getName()
             << "' with a different shape in another translation unit";
    return success();
  }
  importedRecordShapes[definition->getName()] = shape;

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::StructDefOp>(
      defLoc, moduleBuilder.getStringAttr(definition->getName()),
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
  if (qualType.getCanonicalType()->isPointerType())
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
  if (qualType.getCanonicalType()->isPointerType())
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
  // becomes an immutable Rust static. Struct-typed const globals keep the
  // mutable (Cell) representation because a struct's default value is not
  // const-evaluable in Rust.
  bool isConst = qualType.isConstQualified() &&
                 !llvm::isa<emitrust::StructType>(*mlirType);

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
  if (llvm::isa<emitrust::StructType, emitrust::ArrayType>(type))
    return emitError(initLoc)
           << "unsupported: aggregate initializer for a global variable";
  // Static storage duration requires a constant initializer (C11 6.7.9p4);
  // clang's constant evaluator produces the folded value.
  clang::APValue *value = decl->evaluateValue();
  if (!value)
    return emitError(initLoc) << "unsupported: non-constant global initializer";
  if (auto intType = llvm::dyn_cast<IntegerType>(type)) {
    if (!value->isInt())
      return emitError(initLoc)
             << "unsupported: global initializer does not match its type";
    if (intType.getWidth() == 1)
      return Attribute(builder.getBoolAttr(value->getInt().getBoolValue()));
    return Attribute(IntegerAttr::get(
        intType, value->getInt().extOrTrunc(intType.getWidth())));
  }
  if (auto floatType = llvm::dyn_cast<FloatType>(type)) {
    if (!value->isFloat())
      return emitError(initLoc)
             << "unsupported: global initializer does not match its type";
    return Attribute(FloatAttr::get(floatType, value->getFloat()));
  }
  return emitError(initLoc)
         << "unsupported: global initializer for this type";
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
  if (isRustKeyword(cName))
    return emitError(loc) << "unsupported: function name '" << cName
                          << "' is a Rust keyword";
  std::string name = mlirFuncName(func);

  // Build the signature.
  SmallVector<Type> inputTypes;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    FailureOr<Type> paramType =
        mapParamType(param->getType(), translateLoc(param->getLocation()));
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
    if (existing.getFunctionType() != functionType)
      return emitError(loc) << "unsupported: conflicting redeclaration of '"
                            << name << "'";
    existing.erase();
    functions.erase(name);
  }

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, name, functionType);
  functions[name] = funcOp;
  if (!isDefinition) {
    funcOp.setPrivate();
    return success();
  }

  // Function prologue: reset per-function state, then materialize each
  // parameter as a place appropriate to its kind.
  symbols.clear();
  addressTaken.clear();
  loopStack.clear();
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  currentFuncName = name;
  currentIsMain = name == "c_main";
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());

  for (auto [index, param] : llvm::enumerate(func->parameters())) {
    Location paramLoc = translateLoc(param->getLocation());
    Value blockArg = entryBlock->getArgument(index);
    Type type = blockArg.getType();
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(type)) {
      // Pointer parameter: used directly as a reference SSA value.
      symbols[param] = blockArg;
      continue;
    }
    if (llvm::isa<emitrust::StructType, emitrust::EnumType>(type) ||
        isUnsignedInt(type) || addressTaken.contains(param)) {
      // By-value struct, enum, or unsigned scalar, or an address-taken
      // scalar: copy into a Rust variable (enums must not become memref
      // cells — a memref of a dialect type is illegal — and unsigned cells
      // must not either, because mem2reg materializes its default value as
      // an `arith.constant`, which requires a signless type).
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
                                               bool deferExtern) {
  astContextPtr = &context;
  currentTuTag = tuTag.str();
  deferExternGlobals = deferExtern;
  const clang::TranslationUnitDecl *unit = astContext().getTranslationUnitDecl();
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit())
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
  if (llvm::isa<clang::GotoStmt>(stmt) || llvm::isa<clang::LabelStmt>(stmt) ||
      llvm::isa<clang::IndirectGotoStmt>(stmt))
    return emitError(loc) << "unsupported: goto statement";
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
  clang::QualType type = var->getType().getCanonicalType();
  if (type->isPointerType())
    return emitError(loc) << "unsupported: pointer-typed local variable";
  FailureOr<Type> mlirType = mapType(type, loc);
  if (failed(mlirType))
    return failure();

  bool isAggregate =
      llvm::isa<emitrust::StructType, emitrust::ArrayType>(*mlirType);
  // Enums and unsigned scalars live in `emitrust.variable` places rather
  // than memref cells: a memref of a dialect type is illegal, and mem2reg
  // materializes an unsigned cell's default value as an `arith.constant`,
  // which requires a signless type.
  bool isEnum = llvm::isa<emitrust::EnumType>(*mlirType);
  if (isAggregate || isEnum || isUnsignedInt(*mlirType) ||
      addressTaken.contains(var)) {
    Value place = builder
                      .create<emitrust::VariableOp>(
                          loc, emitrust::LValueType::get(*mlirType))
                      .getResult();
    symbols[var] = place;
    if (const clang::Expr *init = var->getInit()) {
      if (isAggregate)
        return emitError(loc) << "unsupported: aggregate initializer";
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
  // Compound assignment to a whole global in statement position:
  // load-modify-store through the global access ops, no staging copy
  // needed. Value-position uses go through emitCompoundAssignToPlace.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getLHS())) {
    if (!astContext().hasSameUnqualifiedType(op->getComputationLHSType(),
                                           op->getLHS()->getType()))
      return emitError(loc)
             << "unsupported: compound assignment with operand promotion";
    const GlobalInfo &global = globals.find(var)->second;
    Value current = builder
                        .create<emitrust::GlobalLoadOp>(
                            loc, global.type, globalSymbol(global.symbol))
                        .getResult();
    FailureOr<Value> rhs = emitRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    clang::BinaryOperatorKind opcode =
        clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
    Value rhsValue = *rhs;
    // `<<=`/`>>=` normalize the shift amount to the shifted operand's
    // width, mirroring emitCompoundAssignToPlace.
    auto currentInt = llvm::dyn_cast<IntegerType>(current.getType());
    auto rhsInt = llvm::dyn_cast<IntegerType>(rhsValue.getType());
    if ((opcode == clang::BO_Shl || opcode == clang::BO_Shr) && currentInt &&
        rhsInt)
      rhsValue = castToIntType(loc, rhsValue, currentInt);
    if (current.getType() != rhsValue.getType())
      return emitError(loc)
             << "unsupported: compound assignment operand type mismatch";
    FailureOr<Value> result = buildBinaryArith(loc, opcode, current, rhsValue);
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
  if (!astContext().hasSameUnqualifiedType(op->getComputationLHSType(),
                                         op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: compound assignment with operand promotion";
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  FailureOr<Value> rhs = emitRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  Value rhsValue = *rhs;
  // The shift amount's C type is independent of the shifted operand's, so
  // `<<=`/`>>=` normalize the right operand to the left operand's width;
  // every other compound assignment requires matching operand types.
  auto currentInt = llvm::dyn_cast<IntegerType>(current.getType());
  auto rhsInt = llvm::dyn_cast<IntegerType>(rhsValue.getType());
  if ((opcode == clang::BO_Shl || opcode == clang::BO_Shr) && currentInt &&
      rhsInt)
    rhsValue = castToIntType(loc, rhsValue, currentInt);
  if (current.getType() != rhsValue.getType())
    return emitError(loc)
           << "unsupported: compound assignment operand type mismatch";
  FailureOr<Value> result = buildBinaryArith(loc, opcode, current, rhsValue);
  if (failed(result))
    return failure();
  if (failed(storeToPlace(loc, *place, *result)))
    return failure();
  flushGlobalWriteback(loc, writeback);
  return place;
}

LogicalResult CImporter::emitIncDec(const clang::UnaryOperator *op) {
  return success(succeeded(emitIncDecValue(op)));
}

FailureOr<Value> CImporter::emitIncDecValue(const clang::UnaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
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
  Location loc = translateLoc(call->getBeginLoc());
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee)
    return emitError(loc) << "unsupported: indirect function call";
  if (callee->getDeclName().isIdentifier() && callee->getName() == "printf")
    return emitPrintf(call);
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
    char spec = format[i];
    if (spec == '%') {
      rustFormat += '%';
      continue;
    }
    Type expected;
    llvm::StringRef placeholder;
    if (spec == 'd') {
      expected = builder.getI32Type();
      placeholder = "{}";
    } else if (spec == 'l' && i + 1 < n && format[i + 1] == 'd') {
      ++i;
      expected = builder.getIntegerType(64);
      placeholder = "{}";
    } else if (spec == 'f') {
      // C's %f prints six decimals; Rust's {:.6} matches it for every
      // finite value and for infinities, but spells NaN as "NaN" where C
      // prints "nan"/"-nan". The argument is therefore routed through the
      // module-level `__emitrust_fmt_f64` helper (emitted once, on demand)
      // and printed with a plain `{}`.
      expected = builder.getF64Type();
      placeholder = "{}";
    } else {
      return emitError(loc) << "unsupported printf format specifier '%"
                            << llvm::Twine(std::string(1, spec)) << "'";
    }
    if (argIndex >= call->getNumArgs())
      return emitError(loc) << "unsupported: too few arguments to printf";
    FailureOr<Value> argument = emitRValue(call->getArg(argIndex));
    if (failed(argument))
      return failure();
    if ((*argument).getType() != expected)
      return emitError(loc) << "unsupported: printf argument " << argIndex
                            << " does not match its format specifier";
    ++argIndex;
    if (spec == 'f') {
      // Wrap the f64 in the C-compatible formatting helper; the resulting
      // String is what the print! placeholder consumes.
      needsFloatFormatHelper = true;
      auto stringType =
          emitrust::OpaqueType::get(builder.getContext(), "String");
      *argument = builder
                      .create<emitrust::CallOpaqueOp>(
                          loc, TypeRange{stringType},
                          builder.getStringAttr("__emitrust_fmt_f64"),
                          /*args=*/ArrayAttr(), ValueRange{*argument})
                      .getResult(0);
    }
    operands.push_back(*argument);
    rustFormat += placeholder;
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
    // explicit cast): an `emitrust.cast` to the destination width. The
    // destination is signless like every imported integer; enum values are
    // verified to fit in i32, so the width alone is sufficient.
    if (llvm::isa<emitrust::EnumType>((*value).getType())) {
      if (!cast->getType().getCanonicalType()->isIntegerType())
        return emitError(loc) << "unsupported integral cast";
      unsigned width = astContext().getIntWidth(cast->getType());
      return builder
          .create<emitrust::CastOp>(loc, builder.getIntegerType(width),
                                    *value)
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
    // C negates an unsigned value modulo 2^N, but the Rust `0 - x` this
    // would lower to panics on overflow in debug builds; rejected until a
    // wrapping negation lowering exists.
    if (isUnsignedInt((*value).getType()))
      return emitError(loc) << "unsupported: unary '-' on an unsigned operand";
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

  // Enum comparisons: Sema promotes enum operands to the (possibly
  // unsigned) underlying type, so the enum values are recovered from behind
  // the promotion casts. Equality maps to `emitrust.cmp` on the enum type
  // (Rust derives PartialEq); relational comparison has no derived Rust
  // ordering and compares the i32 discriminants instead.
  std::optional<EnumOperand> lhsEnum = classifyEnumOperand(op->getLHS());
  std::optional<EnumOperand> rhsEnum = classifyEnumOperand(op->getRHS());
  if (lhsEnum || rhsEnum) {
    if (!lhsEnum || !rhsEnum)
      return emitError(loc)
             << "unsupported: comparison between an enum and a non-enum value";
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
    return emitError(loc) << "unsupported: indirect function call";
  if (!callee->getDeclName().isIdentifier())
    return emitError(loc) << "unsupported callee";
  if (callee->getName() == "printf")
    return emitError(loc) << "unsupported: printf return value must be unused";
  if (callee->isVariadic())
    return emitError(loc) << "unsupported: call to a variadic function";

  std::string name = mlirFuncName(callee);
  func::FuncOp target = functions.lookup(name);
  if (!target)
    return emitError(loc) << "unsupported: call to unimported function '"
                          << name << "'";

  SmallVector<Value> arguments;
  for (const clang::Expr *argument : call->arguments()) {
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments.push_back(*value);
  }
  FunctionType targetType = target.getFunctionType();
  if (arguments.size() != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";
  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
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
      return emitError(loc) << "unsupported: reference to an unknown variable";
    }
    Value place = it->second;
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(place.getType()))
      return emitError(loc)
             << "unsupported: pointer variable used as an assignable place";
    return place;
  }

  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (!field)
      return emitError(loc) << "unsupported member access";
    Value basePlace;
    if (member->isArrow()) {
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
    if (!base->getType().getCanonicalType()->isArrayType())
      return emitError(loc)
             << "unsupported: subscript on a pointer (pointer arithmetic)";
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
                                            /*deferExtern=*/false)))
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
    if (failed(importer.importTranslationUnit(ast->getASTContext(), tuTag,
                                              /*deferExtern=*/true)))
      return nullptr;
  }
  if (failed(importer.finalizeProject()))
    return nullptr;

  if (failed(verify(*module)))
    return nullptr;
  return module;
}
