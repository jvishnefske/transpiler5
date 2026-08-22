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

#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustTypes.h"

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

/// Returns the zero/default attribute for `type`: `0` for integer and index
/// types (which renders as `false` for i1), `0.0` for floating-point types,
/// the opaque `None` expression for `!emitrust.fn_ptr` (the null
/// function pointer) and for the `!emitrust.opaque<"Option<...">` family
/// (W2.11 std::optional and the C99-43 Option-of-cursor cell — the Rust
/// emitter's own default for these is the same `None`, see
/// `RustEmitter`'s default-value rendering), and the `Name::default()`
/// expression for `!emitrust.enum` (FR-113: every emitted enum_def carries
/// an explicit `impl Default` returning its first variant, and the
/// placeholder is dead — the SCF lowerings overwrite it on every branch —
/// so any well-formed value serves; without this, a branch merge with an
/// enum result, e.g. a switch whose cases each return an enumerator, was a
/// NON-located `failed to legalize 'scf.index_switch'` that killed the
/// whole unit). Returns a null attribute for any other type.
inline Attribute getDefaultValueAttr(Type type) {
  if (isa<Float32Type, Float64Type>(type))
    return FloatAttr::get(type, 0.0);
  if (isa<IntegerType, IndexType>(type))
    return IntegerAttr::get(type, 0);
  if (isa<FnPtrType>(type))
    return OpaqueAttr::get(type.getContext(), "None");
  if (auto opaque = dyn_cast<OpaqueType>(type);
      opaque && opaque.getValue().starts_with("Option<"))
    return OpaqueAttr::get(type.getContext(), "None");
  if (auto enumType = dyn_cast<EnumType>(type))
    return OpaqueAttr::get(type.getContext(),
                           (enumType.getName() + "::default()").str());
  return nullptr;
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_EMITRUSTTYPECONVERTER_H
