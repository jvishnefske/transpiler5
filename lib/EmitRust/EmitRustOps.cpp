//===- EmitRustOps.cpp - EmitRust operation implementations ---------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the EmitRust dialect operations: the custom parsers and
/// printers of `emitrust.func`, `emitrust.for`, `emitrust.assign`, and
/// `emitrust.switch`, and the verifiers that enforce the dialect's
/// invariants (mutability discipline of assignments, lvalue placement
/// rules, at most one function result, matching return types, non-empty
/// callee and literal strings, loop-jump nesting, struct-, enum-, and
/// global-definition well-formedness, symbol-checked global loads and
/// stores, switch case/region agreement, the enum comparison and cast
/// restrictions, and induction-variable typing).
//
//===----------------------------------------------------------------------===//

#include "EmitRust/EmitRustOps.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Interfaces/FunctionImplementation.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// FuncOp
//===----------------------------------------------------------------------===//

/// Builds a function with the given name and signature. The body region is
/// created empty; callers populate it (or leave it empty for an external
/// function).
void FuncOp::build(OpBuilder &builder, OperationState &state, StringRef name,
                   FunctionType type, ArrayRef<NamedAttribute> attrs,
                   ArrayRef<DictionaryAttr> argAttrs) {
  state.addAttribute(SymbolTable::getSymbolAttrName(),
                     builder.getStringAttr(name));
  state.addAttribute(getFunctionTypeAttrName(state.name), TypeAttr::get(type));
  state.attributes.append(attrs.begin(), attrs.end());
  state.addRegion();

  if (argAttrs.empty())
    return;
  assert(type.getNumInputs() == argAttrs.size());
  call_interface_impl::addArgAndResultAttrs(
      builder, state, argAttrs, /*resultAttrs=*/{},
      getArgAttrsAttrName(state.name), getResAttrsAttrName(state.name));
}

/// Parses a function in the standard function-op syntax, e.g.
/// `emitrust.func @add(%a: i32, %b: i32) -> i32 { ... }`.
ParseResult FuncOp::parse(OpAsmParser &parser, OperationState &result) {
  auto buildFuncType =
      [](Builder &builder, ArrayRef<Type> argTypes, ArrayRef<Type> results,
         function_interface_impl::VariadicFlag,
         std::string &) { return builder.getFunctionType(argTypes, results); };

  return function_interface_impl::parseFunctionOp(
      parser, result, /*allowVariadic=*/false,
      getFunctionTypeAttrName(result.name), buildFuncType,
      getArgAttrsAttrName(result.name), getResAttrsAttrName(result.name));
}

/// Prints the function in the standard function-op syntax.
void FuncOp::print(OpAsmPrinter &p) {
  function_interface_impl::printFunctionOp(
      p, *this, /*isVariadic=*/false, getFunctionTypeAttrName(),
      getArgAttrsAttrName(), getResAttrsAttrName());
}

/// Verifies that the function has at most one result, that no parameter or
/// result is of lvalue type, and that the entry block's argument types match
/// the function signature.
LogicalResult FuncOp::verify() {
  if (getFunctionType().getNumResults() > 1)
    return emitOpError("requires zero or exactly one result, but has ")
           << getFunctionType().getNumResults();

  FunctionType functionType = getFunctionType();
  for (auto [index, argType] : llvm::enumerate(functionType.getInputs())) {
    if (isa<LValueType>(argType))
      return emitOpError("argument #")
             << index << " must not be of lvalue type";
  }
  for (Type resultType : functionType.getResults()) {
    if (isa<LValueType>(resultType))
      return emitOpError("result must not be of lvalue type");
  }

  if (isExternal())
    return success();

  Block &entryBlock = getBody().front();
  ArrayRef<Type> argTypes = getFunctionType().getInputs();
  if (entryBlock.getNumArguments() != argTypes.size())
    return emitOpError("entry block must have ")
           << argTypes.size() << " arguments to match function signature, but "
           << "has " << entryBlock.getNumArguments();

  for (auto [index, argType] : llvm::enumerate(argTypes)) {
    if (entryBlock.getArgument(index).getType() != argType)
      return emitOpError("type of entry block argument #")
             << index << " (" << entryBlock.getArgument(index).getType()
             << ") must match the type of the corresponding argument in "
             << "function signature (" << argType << ")";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ReturnOp
//===----------------------------------------------------------------------===//

/// Verifies that the operand count and type match the result of the
/// enclosing function.
LogicalResult ReturnOp::verify() {
  auto function = cast<FuncOp>((*this)->getParentOp());

  // The operand number and types must match the function signature.
  unsigned numResults = function.getFunctionType().getNumResults();
  if (getNumOperands() != numResults)
    return emitOpError() << "has " << getNumOperands()
                         << " operands, but enclosing function (@"
                         << function.getName() << ") returns " << numResults;

  if (numResults == 1 &&
      getOperand().getType() != function.getResultTypes()[0])
    return emitError() << "type of the return operand ("
                       << getOperand().getType()
                       << ") doesn't match function result type ("
                       << function.getResultTypes()[0] << ")"
                       << " in function @" << function.getName();

  return success();
}

//===----------------------------------------------------------------------===//
// CallOpaqueOp
//===----------------------------------------------------------------------===//

/// Verifies that the callee string is non-empty and that every index-typed
/// entry of the optional `args` attribute references an operand of this
/// operation.
LogicalResult CallOpaqueOp::verify() {
  if (getCallee().empty())
    return emitOpError("callee must not be empty");

  if (std::optional<ArrayAttr> argsAttr = getArgs()) {
    for (Attribute arg : *argsAttr) {
      auto intAttr = dyn_cast<IntegerAttr>(arg);
      if (!intAttr || !isa<IndexType>(intAttr.getType()))
        continue;
      int64_t index = intAttr.getInt();
      if (index < 0 || index >= static_cast<int64_t>(getNumOperands()))
        return emitOpError("args index ") << index << " is out of range";
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// LiteralOp
//===----------------------------------------------------------------------===//

/// Verifies that the literal string is non-empty.
LogicalResult LiteralOp::verify() {
  if (getValue().empty())
    return emitOpError() << "value must not be empty";
  return success();
}

//===----------------------------------------------------------------------===//
// AssignOp
//===----------------------------------------------------------------------===//

/// Parses an assignment in the form `emitrust.assign %var = %value : T`
/// where `T` is the type of the destination. When `T` is an
/// `!emitrust.lvalue`, the value operand resolves to the lvalue's wrapped
/// value type; otherwise it resolves to `T` itself.
ParseResult AssignOp::parse(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::UnresolvedOperand var, value;
  Type varType;
  if (parser.parseOperand(var) || parser.parseEqual() ||
      parser.parseOperand(value) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(varType))
    return failure();

  Type valueType = varType;
  if (auto lvalueType = dyn_cast<LValueType>(varType))
    valueType = lvalueType.getValueType();

  if (parser.resolveOperand(var, varType, result.operands) ||
      parser.resolveOperand(value, valueType, result.operands))
    return failure();
  return success();
}

/// Prints the assignment; only the destination type is printed.
void AssignOp::print(OpAsmPrinter &p) {
  p << " " << getVar() << " = " << getValue();
  p.printOptionalAttrDict((*this)->getAttrs());
  p << " : " << getVar().getType();
}

/// Verifies the assignment discipline. An lvalue-typed destination accepts
/// any lvalue producer, and the value must be of the lvalue's wrapped value
/// type. Any other destination must be the result of an `emitrust.let`
/// operation carrying the `mut` marker, and the value must be of the
/// destination's type.
LogicalResult AssignOp::verify() {
  if (auto lvalueType = dyn_cast<LValueType>(getVar().getType())) {
    if (getValue().getType() != lvalueType.getValueType())
      return emitOpError("value type ")
             << getValue().getType()
             << " does not match the destination value type "
             << lvalueType.getValueType();
    return success();
  }

  auto letOp = getVar().getDefiningOp<LetOp>();
  if (!letOp)
    return emitOpError("destination is not produced by an emitrust.let");
  if (!letOp.getIsMut())
    return emitOpError("destination let binding is not mutable");
  if (getValue().getType() != getVar().getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match the destination type "
           << getVar().getType();
  return success();
}

//===----------------------------------------------------------------------===//
// StructDefOp
//===----------------------------------------------------------------------===//

/// Returns whether `type` may be used as a struct field type: a scalar
/// (integer, index, or float), an EmitRust array, an EmitRust struct, or an
/// EmitRust enum.
static bool isValidStructFieldType(Type type) {
  return isa<IntegerType, IndexType, FloatType, ArrayType, StructType,
             EnumType>(type);
}

/// Verifies that the field name and type arrays have the same non-zero
/// length, that field names are non-empty and unique, and that every field
/// type is a scalar, array, struct, or enum type.
LogicalResult StructDefOp::verify() {
  ArrayAttr names = getFieldNames();
  ArrayAttr types = getFieldTypes();
  if (names.size() != types.size())
    return emitOpError("has ")
           << names.size() << " field names but " << types.size()
           << " field types";
  if (names.empty())
    return emitOpError("must have at least one field");

  llvm::StringSet<> seen;
  for (auto [nameAttr, typeAttr] : llvm::zip_equal(names, types)) {
    StringRef name = cast<StringAttr>(nameAttr).getValue();
    if (name.empty())
      return emitOpError("field names must not be empty");
    if (!seen.insert(name).second)
      return emitOpError("duplicate field name \"") << name << "\"";
    Type fieldType = cast<TypeAttr>(typeAttr).getValue();
    if (!isValidStructFieldType(fieldType))
      return emitOpError("invalid field type ") << fieldType;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// EnumDefOp
//===----------------------------------------------------------------------===//

/// Verifies that the variant name and value arrays have the same non-zero
/// length, that variant names are non-empty and unique, and that variant
/// values are unique and within the i32 range.
LogicalResult EnumDefOp::verify() {
  ArrayAttr names = getVariantNames();
  ArrayRef<int64_t> values = getVariantValues();
  if (names.size() != values.size())
    return emitOpError("has ")
           << names.size() << " variant names but " << values.size()
           << " variant values";
  if (names.empty())
    return emitOpError("must have at least one variant");

  llvm::StringSet<> seenNames;
  llvm::DenseSet<int64_t> seenValues;
  for (auto [nameAttr, value] : llvm::zip_equal(names, values)) {
    StringRef name = cast<StringAttr>(nameAttr).getValue();
    if (name.empty())
      return emitOpError("variant names must not be empty");
    if (!seenNames.insert(name).second)
      return emitOpError("duplicate variant name \"") << name << "\"";
    if (!seenValues.insert(value).second)
      return emitOpError("duplicate variant value ") << value;
    if (!llvm::isInt<32>(value))
      return emitOpError("variant value ")
             << value << " is out of the i32 range";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// GlobalOp
//===----------------------------------------------------------------------===//

/// Returns whether `type` is a scalar value type (integer, index, or float)
/// for the purpose of variable and global initializers.
static bool isScalarValueType(Type type) {
  return isa<IntegerType, IndexType, FloatType>(type);
}

/// Verifies that the global's value type is a scalar, array, or struct,
/// that the `const` marker is only used with const-initializable (scalar or
/// array) value types, and that a present initializer is a typed attribute
/// of the value type on a scalar global.
LogicalResult GlobalOp::verify() {
  Type type = getType();
  if (!isScalarValueType(type) && !isa<ArrayType, StructType>(type))
    return emitOpError("invalid global value type ") << type;
  if (getIsConst() && isa<StructType>(type))
    return emitOpError(
               "const marker requires a scalar or array value type, but got ")
           << type;

  Attribute init = getInitAttr();
  if (!init)
    return success();
  auto typedInit = dyn_cast<TypedAttr>(init);
  if (!typedInit)
    return emitOpError("init must be a typed attribute");
  if (!isScalarValueType(type))
    return emitOpError("init is only supported for scalar value types");
  if (typedInit.getType() != type)
    return emitOpError("init type ")
           << typedInit.getType() << " does not match the global value type "
           << type;
  return success();
}

//===----------------------------------------------------------------------===//
// GlobalLoadOp / GlobalStoreOp
//===----------------------------------------------------------------------===//

/// Resolves the referenced global of a load or store, or emits an error on
/// `op` when the symbol does not name an `emitrust.global`.
static FailureOr<GlobalOp> resolveGlobal(Operation *op,
                                         SymbolTableCollection &symbolTable,
                                         FlatSymbolRefAttr symbol) {
  auto global =
      symbolTable.lookupNearestSymbolFrom<GlobalOp>(op, symbol.getAttr());
  if (!global)
    return op->emitOpError("'")
           << symbol.getValue()
           << "' does not reference a valid emitrust.global";
  return global;
}

/// Verifies that the load references an `emitrust.global` whose value type
/// equals the result type.
LogicalResult
GlobalLoadOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<GlobalOp> global =
      resolveGlobal(getOperation(), symbolTable, getGlobalAttr());
  if (failed(global))
    return failure();
  if (getResult().getType() != global->getType())
    return emitOpError("result type ")
           << getResult().getType() << " does not match the value type "
           << global->getType() << " of the global @" << getGlobal();
  return success();
}

/// Verifies that the store references a non-`const` `emitrust.global` whose
/// value type equals the stored value's type.
LogicalResult
GlobalStoreOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<GlobalOp> global =
      resolveGlobal(getOperation(), symbolTable, getGlobalAttr());
  if (failed(global))
    return failure();
  if (global->getIsConst())
    return emitOpError("cannot store to the immutable global @") << getGlobal();
  if (getValue().getType() != global->getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match the value type "
           << global->getType() << " of the global @" << getGlobal();
  return success();
}

//===----------------------------------------------------------------------===//
// VariableOp
//===----------------------------------------------------------------------===//

/// Verifies that a present initializer is a typed attribute whose type
/// equals the lvalue's wrapped value type, and that initializers are only
/// used with scalar value types.
LogicalResult VariableOp::verify() {
  Attribute init = getInitAttr();
  if (!init)
    return success();

  Type valueType = cast<LValueType>(getResult().getType()).getValueType();
  auto typedInit = dyn_cast<TypedAttr>(init);
  if (!typedInit)
    return emitOpError("init must be a typed attribute");
  if (!isScalarValueType(valueType))
    return emitOpError("init is only supported for scalar value types");
  if (typedInit.getType() != valueType)
    return emitOpError("init type ")
           << typedInit.getType() << " does not match the variable value type "
           << valueType;
  return success();
}

//===----------------------------------------------------------------------===//
// MemberOp
//===----------------------------------------------------------------------===//

/// Verifies that the operand is an lvalue wrapping a struct type.
LogicalResult MemberOp::verify() {
  Type valueType = cast<LValueType>(getOperand().getType()).getValueType();
  if (!isa<StructType>(valueType))
    return emitOpError(
               "operand must be an lvalue of !emitrust.struct type, but got ")
           << getOperand().getType();
  return success();
}

//===----------------------------------------------------------------------===//
// SubscriptOp
//===----------------------------------------------------------------------===//

/// Verifies that the array operand is an lvalue wrapping an array type
/// whose element type equals the result lvalue's wrapped value type.
LogicalResult SubscriptOp::verify() {
  Type valueType = cast<LValueType>(getArray().getType()).getValueType();
  auto arrayType = dyn_cast<ArrayType>(valueType);
  if (!arrayType)
    return emitOpError(
               "operand must be an lvalue of !emitrust.array type, but got ")
           << getArray().getType();
  Type resultValueType = cast<LValueType>(getResult().getType()).getValueType();
  if (arrayType.getElementType() != resultValueType)
    return emitOpError("result value type ")
           << resultValueType << " does not match the array element type "
           << arrayType.getElementType();
  return success();
}

//===----------------------------------------------------------------------===//
// DerefOp
//===----------------------------------------------------------------------===//

/// Verifies that the reference's pointee type equals the result lvalue's
/// wrapped value type.
LogicalResult DerefOp::verify() {
  Type pointee;
  if (auto refType = dyn_cast<RefType>(getOperand().getType()))
    pointee = refType.getPointee();
  else
    pointee = cast<MutRefType>(getOperand().getType()).getPointee();
  Type resultValueType = cast<LValueType>(getResult().getType()).getValueType();
  if (pointee != resultValueType)
    return emitOpError("result value type ")
           << resultValueType << " does not match the pointee type " << pointee;
  return success();
}

//===----------------------------------------------------------------------===//
// LoadOp
//===----------------------------------------------------------------------===//

/// Verifies that the result type equals the operand's wrapped value type.
LogicalResult LoadOp::verify() {
  Type valueType = cast<LValueType>(getOperand().getType()).getValueType();
  if (getResult().getType() != valueType)
    return emitOpError("result type ")
           << getResult().getType()
           << " does not match the operand value type " << valueType;
  return success();
}

//===----------------------------------------------------------------------===//
// AddrOfOp
//===----------------------------------------------------------------------===//

/// Verifies that the `mut` marker is present exactly when the result is a
/// mutable reference and that the reference's pointee type equals the
/// operand's wrapped value type.
LogicalResult AddrOfOp::verify() {
  Type pointee;
  if (auto mutRefType = dyn_cast<MutRefType>(getResult().getType())) {
    if (!getIsMut())
      return emitOpError(
          "result is a !emitrust.mut_ref but the mut marker is absent");
    pointee = mutRefType.getPointee();
  } else {
    if (getIsMut())
      return emitOpError(
          "mut marker requires a !emitrust.mut_ref result type");
    pointee = cast<RefType>(getResult().getType()).getPointee();
  }
  Type valueType = cast<LValueType>(getOperand().getType()).getValueType();
  if (pointee != valueType)
    return emitOpError("result pointee type ")
           << pointee << " does not match the operand value type " << valueType;
  return success();
}

//===----------------------------------------------------------------------===//
// CmpOp
//===----------------------------------------------------------------------===//

/// Verifies that enum operands are only compared with the eq and ne
/// predicates: enums derive PartialEq but not PartialOrd.
LogicalResult CmpOp::verify() {
  if (!isa<EnumType>(getLhs().getType()))
    return success();
  CmpPredicate predicate = getPredicate();
  if (predicate != CmpPredicate::eq && predicate != CmpPredicate::ne)
    return emitOpError(
        "enum operands only support the eq and ne predicates");
  return success();
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

/// Verifies that the result is not an enum type: Rust has no
/// integer-to-enum `as` cast.
LogicalResult CastOp::verify() {
  if (isa<EnumType>(getResult().getType()))
    return emitOpError("cannot cast to an enum type");
  return success();
}

//===----------------------------------------------------------------------===//
// BreakOp / ContinueOp
//===----------------------------------------------------------------------===//

/// Verifies that `op` has an enclosing `emitrust.loop` or `emitrust.for`,
/// stopping the walk at function boundaries.
static LogicalResult verifyLoopJump(Operation *op) {
  Operation *parent = op->getParentOp();
  while (parent) {
    if (isa<LoopOp, ForOp>(parent))
      return success();
    if (isa<FuncOp>(parent))
      break;
    parent = parent->getParentOp();
  }
  return op->emitOpError(
      "must appear inside an emitrust.loop or emitrust.for");
}

/// Verifies that the break has an enclosing loop within the same function.
LogicalResult BreakOp::verify() { return verifyLoopJump(getOperation()); }

/// Verifies that the continue has an enclosing loop within the same
/// function.
LogicalResult ContinueOp::verify() { return verifyLoopJump(getOperation()); }

//===----------------------------------------------------------------------===//
// ForOp
//===----------------------------------------------------------------------===//

/// Parses a for loop in the form
/// `emitrust.for %i = %lb to %ub step %s (`:` type)? { ... }`,
/// with the bound/induction type defaulting to `index` when absent.
ParseResult ForOp::parse(OpAsmParser &parser, OperationState &result) {
  Builder &builder = parser.getBuilder();
  Type type;

  OpAsmParser::Argument inductionVariable;
  OpAsmParser::UnresolvedOperand lb, ub, step;

  // Parse the induction variable followed by '='.
  if (parser.parseOperand(inductionVariable.ssaName) || parser.parseEqual() ||
      // Parse loop bounds.
      parser.parseOperand(lb) || parser.parseKeyword("to") ||
      parser.parseOperand(ub) || parser.parseKeyword("step") ||
      parser.parseOperand(step))
    return failure();

  // Parse the optional bound type, else assume index.
  if (parser.parseOptionalColon())
    type = builder.getIndexType();
  else if (parser.parseType(type))
    return failure();

  // Resolve input operands.
  inductionVariable.type = type;
  if (parser.resolveOperand(lb, type, result.operands) ||
      parser.resolveOperand(ub, type, result.operands) ||
      parser.resolveOperand(step, type, result.operands))
    return failure();

  // Parse the body region.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, inductionVariable))
    return failure();

  ForOp::ensureTerminator(*body, builder, result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

/// Prints the for loop; the trailing colon-type is elided for `index`.
void ForOp::print(OpAsmPrinter &p) {
  p << " " << getInductionVar() << " = " << getLowerBound() << " to "
    << getUpperBound() << " step " << getStep();
  if (Type type = getInductionVar().getType(); !type.isIndex())
    p << " : " << type;
  p << ' ';
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
  p.printOptionalAttrDict((*this)->getAttrs());
}

/// Verifies that the body block carries exactly one induction-variable
/// argument whose type matches the loop bounds.
LogicalResult ForOp::verify() {
  Block *body = getBody();
  if (body->getNumArguments() != 1)
    return emitOpError(
               "expected body region to have a single argument, but got ")
           << body->getNumArguments();
  if (body->getArgument(0).getType() != getLowerBound().getType())
    return emitOpError("expected induction variable to be of type ")
           << getLowerBound().getType() << ", but got "
           << body->getArgument(0).getType();
  return success();
}

//===----------------------------------------------------------------------===//
// SwitchOp
//===----------------------------------------------------------------------===//

/// Parses a switch in the form
/// `emitrust.switch %d : type` followed by zero or more `case N { ... }`
/// groups, a mandatory `default { ... }` region, and an optional attribute
/// dictionary (mirroring upstream scf.index_switch). Missing
/// `emitrust.yield` terminators are inserted implicitly.
ParseResult SwitchOp::parse(OpAsmParser &parser, OperationState &result) {
  Builder &builder = parser.getBuilder();

  OpAsmParser::UnresolvedOperand discriminator;
  Type type;
  if (parser.parseOperand(discriminator) || parser.parseColonType(type) ||
      parser.resolveOperand(discriminator, type, result.operands))
    return failure();

  // The default region is stored first to satisfy the ODS requirement that
  // the variadic case-region list comes last.
  Region *defaultRegion = result.addRegion();

  SmallVector<int64_t> caseValues;
  while (succeeded(parser.parseOptionalKeyword("case"))) {
    int64_t value = 0;
    if (parser.parseInteger(value))
      return failure();
    caseValues.push_back(value);
    Region *caseRegion = result.addRegion();
    if (parser.parseRegion(*caseRegion))
      return failure();
    SwitchOp::ensureTerminator(*caseRegion, builder, result.location);
  }
  result.addAttribute(getCasesAttrName(result.name),
                      builder.getDenseI64ArrayAttr(caseValues));

  if (parser.parseKeyword("default") || parser.parseRegion(*defaultRegion))
    return failure();
  SwitchOp::ensureTerminator(*defaultRegion, builder, result.location);

  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

/// Prints the switch: the discriminator with its type, one `case N` group
/// per case region, the `default` region, and the remaining attributes.
void SwitchOp::print(OpAsmPrinter &p) {
  p << ' ' << getDiscriminator() << " : " << getDiscriminator().getType();
  for (auto [value, region] : llvm::zip(getCases(), getCaseRegions())) {
    p.printNewline();
    p << "case " << value << ' ';
    p.printRegion(region,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/false);
  }
  p.printNewline();
  p << "default ";
  p.printRegion(getDefaultRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/{getCasesAttrName()});
}

/// Verifies that the switch has exactly one case region per case value and
/// that the case values are unique.
LogicalResult SwitchOp::verify() {
  ArrayRef<int64_t> caseValues = getCases();
  if (getCaseRegions().size() != caseValues.size())
    return emitOpError("has ")
           << getCaseRegions().size() << " case regions but "
           << caseValues.size() << " case values";

  llvm::DenseSet<int64_t> seen;
  for (int64_t value : caseValues) {
    if (!seen.insert(value).second)
      return emitOpError("has duplicate case value ") << value;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "EmitRust/EmitRustOps.cpp.inc"
