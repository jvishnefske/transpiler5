//===- EmitRustTypeConverter.h - EmitRust type conversion -------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the near-identity type converter shared by all conversions into
/// the EmitRust dialect, together with the helper that produces the
/// zero/default value attribute of a scalar type. Every type the EmitRust
/// emitter can render passes through unchanged; anything else fails to
/// convert so the surrounding conversion reports it loudly.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_EMITRUSTTYPECONVERTER_H
#define EMITRUST_CONVERSION_EMITRUSTTYPECONVERTER_H

#include "EmitRust/EmitRustDialect.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Transforms/DialectConversion.h"

#include <optional>

namespace mlir {
namespace emitrust {

/// Returns true if `type` is representable in the EmitRust dialect and thus
/// passes through the conversions unchanged: any builtin integer type, the
/// index type, f32/f64, and every EmitRust dialect type.
inline bool isSupportedEmitRustType(Type type) {
  if (isa<IntegerType, IndexType, Float32Type, Float64Type>(type))
    return true;
  return isa<EmitRustDialect>(type.getDialect());
}

/// Populates `typeConverter` with the near-identity conversion used by all
/// X-to-EmitRust conversions: supported types map to themselves; all other
/// types fail to convert.
inline void populateEmitRustTypeConverter(TypeConverter &typeConverter) {
  typeConverter.addConversion([](Type type) -> std::optional<Type> {
    if (isSupportedEmitRustType(type))
      return type;
    return std::nullopt;
  });
}

/// Returns the zero/default typed attribute for a scalar `type`: `0` for
/// integer and index types (which renders as `false` for i1), `0.0` for
/// floating-point types. Returns a null attribute for any other type.
inline TypedAttr getDefaultValueAttr(Type type) {
  if (isa<Float32Type, Float64Type>(type))
    return FloatAttr::get(type, 0.0);
  if (isa<IntegerType, IndexType>(type))
    return IntegerAttr::get(type, 0);
  return nullptr;
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_EMITRUSTTYPECONVERTER_H
