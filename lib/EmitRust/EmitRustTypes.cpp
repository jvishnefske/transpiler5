//===- EmitRustTypes.cpp - EmitRust type implementations ------------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the EmitRust dialect types: the opaque type's verifier, the
/// lvalue nesting rule, the one-dimensional array type with its custom
/// `NxT` parser/printer and element-type verifier, the dynamically sized
/// slice type (sharing the array element rules), the nullable function
/// pointer type with its custom `(A, B) -> R` parser/printer and
/// component-type verifier, the named struct and enum reference types, and
/// the generated type parser/printer definitions.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/EmitRustTypes.h"

#include "EmitRust/EmitRustDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::emitrust;

#define GET_TYPEDEF_CLASSES
#include "EmitRust/EmitRustOpsTypes.cpp.inc"

/// Registers the EmitRust types with the dialect.
void EmitRustDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "EmitRust/EmitRustOpsTypes.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// OpaqueType
//===----------------------------------------------------------------------===//

/// Verifies that the opaque type carries a non-empty type string.
LogicalResult
emitrust::OpaqueType::verify(llvm::function_ref<InFlightDiagnostic()> emitError,
                             llvm::StringRef value) {
  if (value.empty())
    return emitError() << "expected non empty string in !emitrust.opaque type";
  return success();
}

//===----------------------------------------------------------------------===//
// LValueType
//===----------------------------------------------------------------------===//

/// Verifies that the wrapped value type is present and is not itself an
/// lvalue: lvalue types must not be nested.
LogicalResult emitrust::LValueType::verify(
    llvm::function_ref<InFlightDiagnostic()> emitError, Type valueType) {
  if (!valueType)
    return emitError() << "expected a value type in !emitrust.lvalue type";
  if (llvm::isa<emitrust::LValueType>(valueType))
    return emitError() << "lvalue types may not be nested";
  return success();
}

//===----------------------------------------------------------------------===//
// ArrayType
//===----------------------------------------------------------------------===//

/// Returns whether `type` may be used as an array element type: a scalar
/// (integer, index, f32, f64) or an EmitRust struct type. Nested arrays,
/// lvalues, references, and opaque types are rejected.
bool emitrust::ArrayType::isValidElementType(Type type) {
  return llvm::isa<IntegerType, IndexType, Float32Type, Float64Type,
                   emitrust::StructType>(type);
}

/// Parses the one-dimensional array syntax `!emitrust.array<NxT>`.
Type emitrust::ArrayType::parse(AsmParser &parser) {
  if (parser.parseLess())
    return Type();

  SmallVector<int64_t, 1> dimensions;
  if (parser.parseDimensionList(dimensions, /*allowDynamic=*/false,
                                /*withTrailingX=*/true))
    return Type();
  if (dimensions.size() != 1) {
    parser.emitError(parser.getNameLoc(),
                     "expected exactly one array dimension");
    return Type();
  }

  auto typeLoc = parser.getCurrentLocation();
  Type elementType;
  if (parser.parseType(elementType))
    return Type();
  if (!isValidElementType(elementType)) {
    parser.emitError(typeLoc, "invalid array element type '")
        << elementType << "'";
    return Type();
  }
  if (parser.parseGreater())
    return Type();
  return parser.getChecked<ArrayType>(parser.getContext(),
                                      static_cast<uint64_t>(dimensions.front()),
                                      elementType);
}

/// Prints the one-dimensional array syntax `!emitrust.array<NxT>`.
void emitrust::ArrayType::print(AsmPrinter &printer) const {
  printer << "<" << getSize() << 'x';
  printer.printType(getElementType());
  printer << ">";
}

/// Verifies that the array has at least one element and a valid element
/// type (scalar or struct; no nested arrays, lvalues, or references).
LogicalResult emitrust::ArrayType::verify(
    llvm::function_ref<InFlightDiagnostic()> emitError, uint64_t size,
    Type elementType) {
  if (size < 1)
    return emitError() << "array size must be at least 1";
  if (!elementType || !isValidElementType(elementType))
    return emitError() << "invalid array element type '" << elementType << "'";
  return success();
}

//===----------------------------------------------------------------------===//
// SliceType
//===----------------------------------------------------------------------===//

/// Verifies that the slice element type is valid: the element rules are
/// shared with `ArrayType` (scalar or struct; no nested arrays, slices,
/// lvalues, or references).
LogicalResult emitrust::SliceType::verify(
    llvm::function_ref<InFlightDiagnostic()> emitError, Type elementType) {
  if (!elementType || !ArrayType::isValidElementType(elementType))
    return emitError() << "invalid slice element type " << elementType;
  return success();
}

//===----------------------------------------------------------------------===//
// FnPtrType
//===----------------------------------------------------------------------===//

/// Returns whether `type` may be used as a fn_ptr parameter or result type:
/// an emitter-supported scalar (i1, i8/i16/i32/i64 signless, u8/u16/u32/u64,
/// index, f32, f64), an EmitRust struct or enum, or a nested fn_ptr.
bool emitrust::FnPtrType::isValidComponentType(Type type) {
  if (auto intType = llvm::dyn_cast<IntegerType>(type)) {
    if (intType.isSigned())
      return false;
    unsigned width = intType.getWidth();
    if (width == 1)
      return intType.isSignless();
    return width == 8 || width == 16 || width == 32 || width == 64;
  }
  return llvm::isa<IndexType, Float32Type, Float64Type, emitrust::StructType,
                   emitrust::EnumType, emitrust::FnPtrType>(type);
}

/// Parses the function pointer syntax `!emitrust.fn_ptr<(A, B) -> R>`; the
/// `-> R` clause is absent for a void result.
Type emitrust::FnPtrType::parse(AsmParser &parser) {
  if (parser.parseLess())
    return Type();

  SmallVector<Type> inputs;
  auto typesLoc = parser.getCurrentLocation();
  if (parser.parseCommaSeparatedList(AsmParser::Delimiter::Paren, [&]() {
        Type input;
        if (parser.parseType(input))
          return failure();
        inputs.push_back(input);
        return success();
      }))
    return Type();

  SmallVector<Type> results;
  if (succeeded(parser.parseOptionalArrow())) {
    Type result;
    if (parser.parseType(result))
      return Type();
    results.push_back(result);
  }
  if (parser.parseGreater())
    return Type();
  return parser.getChecked<FnPtrType>(typesLoc, parser.getContext(), inputs,
                                      results);
}

/// Prints the function pointer syntax `!emitrust.fn_ptr<(A, B) -> R>`,
/// omitting the `-> R` clause for a void result.
void emitrust::FnPtrType::print(AsmPrinter &printer) const {
  printer << "<(";
  llvm::interleaveComma(getInputs(), printer,
                        [&](Type input) { printer.printType(input); });
  printer << ")";
  if (!getResults().empty()) {
    printer << " -> ";
    printer.printType(getResults().front());
  }
  printer << ">";
}

/// Verifies that the function pointer has at most one result and that every
/// parameter and result type is a valid component type.
LogicalResult emitrust::FnPtrType::verify(
    llvm::function_ref<InFlightDiagnostic()> emitError, ArrayRef<Type> inputs,
    ArrayRef<Type> results) {
  if (results.size() > 1)
    return emitError() << "fn_ptr supports at most one result, but got "
                       << results.size();
  for (Type input : inputs)
    if (!input || !isValidComponentType(input))
      return emitError() << "invalid fn_ptr parameter type " << input;
  for (Type result : results)
    if (!result || !isValidComponentType(result))
      return emitError() << "invalid fn_ptr result type " << result;
  return success();
}

//===----------------------------------------------------------------------===//
// StructType
//===----------------------------------------------------------------------===//

/// Verifies that the struct reference carries a non-empty name.
LogicalResult emitrust::StructType::verify(
    llvm::function_ref<InFlightDiagnostic()> emitError, llvm::StringRef name) {
  if (name.empty())
    return emitError() << "expected non empty name in !emitrust.struct type";
  return success();
}

//===----------------------------------------------------------------------===//
// EnumType
//===----------------------------------------------------------------------===//

/// Verifies that the enum reference carries a non-empty name.
LogicalResult emitrust::EnumType::verify(
    llvm::function_ref<InFlightDiagnostic()> emitError, llvm::StringRef name) {
  if (name.empty())
    return emitError() << "expected non empty name in !emitrust.enum type";
  return success();
}
