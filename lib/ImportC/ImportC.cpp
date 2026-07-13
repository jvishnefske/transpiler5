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
///    `cf.cond_br`, so that `--mem2reg --lift-cf-to-scf` downstream recovers
///    clean structured IR.
///  - Aggregates and pointers use EmitRust place operations, which are
///    opaque to upstream passes: struct and array locals are
///    `emitrust.variable`, field access is `emitrust.member`, indexing is
///    `emitrust.subscript`, pointer parameters are `!emitrust.mut_ref<T>`
///    arguments dereferenced with `emitrust.deref`, and reads/writes of any
///    such place use `emitrust.load`/`emitrust.assign`. A scalar local whose
///    address is taken is kept as an `emitrust.variable` so the reference
///    stays valid in the generated Rust.
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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/Twine.h"

#include <memory>
#include <string>
#include <vector>

using namespace mlir;

namespace {

/// Break/continue branch targets for the innermost enclosing loop.
struct LoopTargets {
  /// Block a `break` statement branches to (the loop exit block).
  Block *breakDest;
  /// Block a `continue` statement branches to (the condition or increment
  /// block).
  Block *continueDest;
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
  /// Creates an importer that appends to `module`.
  CImporter(clang::ASTContext &astContext, ModuleOp module)
      : astContext(astContext), module(module), builder(module.getContext()) {}

  /// Imports every supported top-level declaration of the translation unit:
  /// complete named struct definitions and function declarations or
  /// definitions. Global variables and other declarations are rejected.
  LogicalResult importTranslationUnit();

private:
  //===--------------------------------------------------------------------===//
  // Locations and types
  //===--------------------------------------------------------------------===//

  /// Converts a clang source location to an MLIR `FileLineColLoc` using the
  /// presumed (user-visible) location; unknown on invalid input.
  Location translateLoc(clang::SourceLocation sourceLoc);

  /// Maps a C value type to its MLIR type: `_Bool`->i1, char->i8,
  /// short->i16, int->i32, long/long long->i64, float->f32, double->f64,
  /// `struct S`->`!emitrust.struct<"S">`, `T[N]`->`!emitrust.array<NxT>`.
  /// Typedefs resolve through the canonical type. Unsigned integers,
  /// pointers, unions, enums, and everything else produce a located
  /// diagnostic.
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

  /// Imports a function declaration or definition as a `func.func`. C
  /// `main` is renamed to `c_main`. Body-less variadic declarations (such
  /// as printf's) are skipped; variadic definitions are rejected. A body
  /// replaces a previously imported body-less declaration of the same name.
  LogicalResult importFunction(const clang::FunctionDecl *func);

  /// Records every local variable whose address is taken with `&x` inside
  /// `stmt`; such scalars become `emitrust.variable` places instead of
  /// promotable memref cells.
  void collectAddressTaken(const clang::Stmt *stmt);

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
  Value createIntConstant(Location loc, Type type, int64_t value);

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

  /// Emits a local variable declaration. Scalars become entry-block memref
  /// cells (initializer stored at the declaration point); aggregates and
  /// address-taken scalars become `emitrust.variable` places.
  LogicalResult emitLocalVar(const clang::VarDecl *var);

  /// Emits `if`/`else` as a cf diamond: cond_br into then/else blocks that
  /// fall through to a continuation block.
  LogicalResult emitIfStmt(const clang::IfStmt *stmt);

  /// Emits `while` as condition/body/exit blocks with a back edge.
  LogicalResult emitWhileStmt(const clang::WhileStmt *stmt);

  /// Emits `for` as init in the current block plus condition, body,
  /// increment, and exit blocks; `continue` targets the increment block.
  LogicalResult emitForStmt(const clang::ForStmt *stmt);

  /// Emits `return`, then continues in a fresh (dead) block so trailing
  /// statements still have an insertion point.
  LogicalResult emitReturnStmt(const clang::ReturnStmt *stmt);

  /// Emits an expression evaluated for its side effects only: assignments,
  /// compound assignments, ++/--, and calls (including printf).
  LogicalResult emitExprStmt(const clang::Expr *expr);

  /// Emits a simple assignment `lhs = rhs` to a memref cell or EmitRust
  /// place.
  LogicalResult emitAssign(const clang::BinaryOperator *op);

  /// Emits a compound assignment (`+=` etc.) as load, arithmetic, store.
  LogicalResult emitCompoundAssign(const clang::CompoundAssignOperator *op);

  /// Emits statement-level `++x`/`x--` as load, add/sub 1, store; only
  /// integer operands are supported.
  LogicalResult emitIncDec(const clang::UnaryOperator *op);

  /// Emits a call statement, dispatching printf to `emitPrintf` and
  /// discarding the result of ordinary calls.
  LogicalResult emitCallStmt(const clang::CallExpr *call);

  /// Lowers a printf call with a literal format string to
  /// `emitrust.call_opaque "print!"` with a translated Rust format string
  /// in the `args` attribute. Supports %d (i32), %ld (i64), %f (f64,
  /// rendered `{:.6}`), and %%; anything else is rejected.
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

  /// Emits a non-assignment binary operator as an rvalue.
  FailureOr<Value> emitBinaryRValue(const clang::BinaryOperator *op);

  /// Emits a unary operator (+, -, !, &, statement-level ++/-- excluded)
  /// as an rvalue.
  FailureOr<Value> emitUnaryRValue(const clang::UnaryOperator *op);

  /// Emits a comparison as an i1 value: `arith.cmpi` (signed) on integers,
  /// `arith.cmpf` (ordered, `une` for !=) on floats.
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

  /// Emits an expression as an assignable place: either a rank-0 memref
  /// value (scalar locals) or an `!emitrust.lvalue` value (aggregates,
  /// dereferences, fields, elements).
  FailureOr<Value> emitLValue(const clang::Expr *expr);

  /// Reads the current value of a place produced by `emitLValue`.
  Value loadPlace(Location loc, Value place);

  /// Writes `value` to a place produced by `emitLValue`; fails on type
  /// mismatch.
  LogicalResult storeToPlace(Location loc, Value place, Value value);

  /// Widens an i1 truth value to the mapped MLIR type of the C expression
  /// type `type` (typically `int`); returns the value unchanged when the C
  /// type is `_Bool`.
  FailureOr<Value> extendBool(Location loc, Value flag, clang::QualType type);

  /// Builds the arith op for a C arithmetic binary operator on two values
  /// of the same integer or float type.
  FailureOr<Value> buildBinaryArith(Location loc,
                                    clang::BinaryOperatorKind opcode,
                                    Value lhs, Value rhs);

  //===--------------------------------------------------------------------===//
  // State
  //===--------------------------------------------------------------------===//

  /// The clang AST being translated (borrowed, read-only).
  clang::ASTContext &astContext;
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
  /// Imported functions by MLIR symbol name.
  llvm::StringMap<func::FuncOp> functions;
  /// Stack of break/continue targets for nested loops.
  SmallVector<LoopTargets> loopStack;
  /// Entry block of the function under construction (owns the allocas).
  Block *entryBlock = nullptr;
  /// Body region of the function under construction.
  Region *bodyRegion = nullptr;
  /// Mapped return type of the current function; null for void.
  Type currentReturnType;
  /// True while translating C `main` (enables the implicit `return 0`).
  bool currentIsMain = false;
};

} // namespace

//===----------------------------------------------------------------------===//
// Locations and types
//===----------------------------------------------------------------------===//

Location CImporter::translateLoc(clang::SourceLocation sourceLoc) {
  MLIRContext *context = builder.getContext();
  if (sourceLoc.isInvalid())
    return UnknownLoc::get(context);
  const clang::SourceManager &sourceManager = astContext.getSourceManager();
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
    case clang::BuiltinType::Char_U:
    case clang::BuiltinType::UChar:
    case clang::BuiltinType::UShort:
    case clang::BuiltinType::UInt:
    case clang::BuiltinType::ULong:
    case clang::BuiltinType::ULongLong:
      return emitError(loc) << "unsupported: unsigned integer type";
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
          astContext.getAsConstantArrayType(canonical)) {
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

  if (canonical->isPointerType())
    return emitError(loc)
           << "unsupported: pointer type outside a parameter position";
  if (canonical->isEnumeralType())
    return emitError(loc) << "unsupported: enum type";
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

  SmallVector<llvm::StringRef> fieldNames;
  SmallVector<Type> fieldTypes;
  for (const clang::FieldDecl *field : definition->fields()) {
    Location fieldLoc = translateLoc(field->getLocation());
    if (field->isBitField())
      return emitError(fieldLoc) << "unsupported: bit-field struct member";
    if (field->getName().empty())
      return emitError(fieldLoc) << "unsupported: unnamed struct member";
    FailureOr<Type> fieldType = mapType(field->getType(), fieldLoc);
    if (failed(fieldType))
      return failure();
    fieldNames.push_back(field->getName());
    fieldTypes.push_back(*fieldType);
  }
  if (fieldNames.empty())
    return emitError(defLoc) << "unsupported: struct with no members";

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::StructDefOp>(
      defLoc, moduleBuilder.getStringAttr(definition->getName()),
      moduleBuilder.getStrArrayAttr(fieldNames),
      moduleBuilder.getTypeArrayAttr(fieldTypes));
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
  llvm::StringRef name =
      cName == "main" ? llvm::StringRef("c_main") : cName;

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

  // Reconcile with an earlier import of the same symbol.
  if (func::FuncOp existing = functions.lookup(name)) {
    if (!isDefinition || !existing.isExternal())
      return success(); // Redundant declaration (or already defined).
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
    if (llvm::isa<emitrust::StructType>(type) || addressTaken.contains(param)) {
      // By-value struct or address-taken scalar: copy into a Rust variable.
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

LogicalResult CImporter::importTranslationUnit() {
  const clang::TranslationUnitDecl *unit = astContext.getTranslationUnitDecl();
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
    if (llvm::isa<clang::TypedefDecl>(decl) || llvm::isa<clang::EmptyDecl>(decl))
      continue;
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      return emitError(translateLoc(var->getLocation()))
             << "unsupported: global variable";
    return emitError(translateLoc(decl->getBeginLoc()))
           << "unsupported top-level declaration";
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
  if (llvm::isa<clang::BreakStmt>(stmt)) {
    if (loopStack.empty())
      return emitError(loc) << "unsupported: 'break' outside of a loop";
    builder.create<cf::BranchOp>(loc, loopStack.back().breakDest);
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (llvm::isa<clang::ContinueStmt>(stmt)) {
    if (loopStack.empty())
      return emitError(loc) << "unsupported: 'continue' outside of a loop";
    builder.create<cf::BranchOp>(loc, loopStack.back().continueDest);
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (llvm::isa<clang::GotoStmt>(stmt) || llvm::isa<clang::LabelStmt>(stmt) ||
      llvm::isa<clang::IndirectGotoStmt>(stmt))
    return emitError(loc) << "unsupported: goto statement";
  if (llvm::isa<clang::SwitchStmt>(stmt))
    return emitError(loc) << "unsupported: switch statement";
  if (llvm::isa<clang::DoStmt>(stmt))
    return emitError(loc) << "unsupported: do-while statement";
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt))
    return emitExprStmt(expr);
  return emitError(loc) << "unsupported statement: "
                        << stmt->getStmtClassName();
}

LogicalResult CImporter::emitLocalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
  if (!var->hasLocalStorage())
    return emitError(loc) << "unsupported: static or extern local variable";
  clang::QualType type = var->getType().getCanonicalType();
  if (type->isPointerType())
    return emitError(loc) << "unsupported: pointer-typed local variable";
  FailureOr<Type> mlirType = mapType(type, loc);
  if (failed(mlirType))
    return failure();

  bool isAggregate =
      llvm::isa<emitrust::StructType, emitrust::ArrayType>(*mlirType);
  if (isAggregate || addressTaken.contains(var)) {
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
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e))
    if (binary->getOpcode() == clang::BO_Assign)
      return emitAssign(binary);
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
  FailureOr<Value> place = emitLValue(op->getLHS());
  if (failed(place))
    return failure();
  FailureOr<Value> value = emitRValue(op->getRHS());
  if (failed(value))
    return failure();
  return storeToPlace(loc, *place, *value);
}

LogicalResult
CImporter::emitCompoundAssign(const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  if (!astContext.hasSameUnqualifiedType(op->getComputationLHSType(),
                                         op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: compound assignment with operand promotion";
  FailureOr<Value> place = emitLValue(op->getLHS());
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  FailureOr<Value> rhs = emitRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  if (current.getType() != (*rhs).getType())
    return emitError(loc)
           << "unsupported: compound assignment operand type mismatch";
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  FailureOr<Value> result = buildBinaryArith(loc, opcode, current, *rhs);
  if (failed(result))
    return failure();
  return storeToPlace(loc, *place, *result);
}

LogicalResult CImporter::emitIncDec(const clang::UnaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  FailureOr<Value> place = emitLValue(op->getSubExpr());
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  auto intType = llvm::dyn_cast<IntegerType>(current.getType());
  if (!intType)
    return emitError(loc) << "unsupported: ++/-- on a non-integer operand";
  Value one = createIntConstant(loc, intType, 1);
  Value next =
      op->isIncrementOp()
          ? builder.create<arith::AddIOp>(loc, current, one).getResult()
          : builder.create<arith::SubIOp>(loc, current, one).getResult();
  return storeToPlace(loc, *place, next);
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
      // C's %f prints six decimals; Rust's {:.6} matches it.
      expected = builder.getF64Type();
      placeholder = "{:.6}";
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

  if (const auto *literal = llvm::dyn_cast<clang::IntegerLiteral>(e)) {
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    return builder
        .create<arith::ConstantOp>(loc,
                                   IntegerAttr::get(*type, literal->getValue()))
        .getResult();
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
  if (llvm::isa<clang::ConditionalOperator>(e))
    return emitError(loc) << "unsupported: conditional operator '?:'";
  if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(e))
    return emitError(loc) << "unsupported: sizeof/alignof expression";
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
    // A pointer parameter read as a value yields its reference SSA value.
    if (const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(sub->IgnoreParens())) {
      auto it = symbols.find(ref->getDecl());
      if (it != symbols.end() &&
          llvm::isa<emitrust::MutRefType, emitrust::RefType>(
              it->second.getType()))
        return it->second;
    }
    FailureOr<Value> place = emitLValue(sub);
    if (failed(place))
      return failure();
    return loadPlace(loc, *place);
  }
  case clang::CK_IntegralCast: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    auto sourceType = llvm::dyn_cast<IntegerType>((*value).getType());
    auto targetType = llvm::dyn_cast<IntegerType>(*mapped);
    if (!sourceType || !targetType)
      return emitError(loc) << "unsupported integral cast";
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
    return builder.create<arith::SIToFPOp>(loc, *mapped, *value).getResult();
  }
  case clang::CK_FloatingToIntegral: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
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
    Value zero = createIntConstant(loc, (*value).getType(), 0);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, *value, zero)
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

  if (op->isAssignmentOp())
    return emitError(loc) << "unsupported: assignment used as a value";
  if (opcode == clang::BO_Comma)
    return emitError(loc) << "unsupported: comma operator";
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
  if ((*lhs).getType() != (*rhs).getType())
    return emitError(loc) << "unsupported: binary operand type mismatch";
  return buildBinaryArith(loc, opcode, *lhs, *rhs);
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
    default:
      return emitError(loc) << "unsupported binary operator '"
                            << clang::BinaryOperator::getOpcodeStr(opcode)
                            << "'";
    }
  }
  return emitError(loc) << "unsupported binary operand type";
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
    return emitError(loc) << "unsupported: ++/-- used as a value";
  case clang::UO_Not:
    return emitError(loc) << "unsupported: bitwise '~' operator";
  case clang::UO_Deref:
    return emitError(loc) << "unsupported dereference in this context";
  default:
    return emitError(loc) << "unsupported unary operator";
  }
}

FailureOr<Value> CImporter::emitComparison(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
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

  llvm::StringRef name = callee->getName() == "main"
                             ? llvm::StringRef("c_main")
                             : callee->getName();
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

FailureOr<Value> CImporter::emitLValue(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
    auto it = symbols.find(ref->getDecl());
    if (it == symbols.end())
      return emitError(loc)
             << "unsupported: reference to a global or unknown variable";
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
      FailureOr<Value> base = emitLValue(member->getBase());
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
    FailureOr<Value> basePlace = emitLValue(base);
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

//===----------------------------------------------------------------------===//
// Entry point
//===----------------------------------------------------------------------===//

OwningOpRef<ModuleOp> mlir::emitrust::importC(llvm::StringRef path,
                                              MLIRContext &context) {
  context.loadDialect<emitrust::EmitRustDialect, func::FuncDialect,
                      arith::ArithDialect, memref::MemRefDialect,
                      cf::ControlFlowDialect>();

  // Imperative shell: parse the file with clang. Parse diagnostics are
  // printed to stderr by clang's own diagnostic machinery.
  std::vector<std::string> commandLine{"-std=c11"};
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
  CImporter importer(ast.getASTContext(), *module);
  if (failed(importer.importTranslationUnit()))
    return nullptr;

  // A verifier failure indicates an importer bug; it is still an import
  // failure and must never yield unverified IR.
  if (failed(verify(*module)))
    return nullptr;
  return module;
}
