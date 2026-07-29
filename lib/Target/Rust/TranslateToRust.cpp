//===- TranslateToRust.cpp - Translating EmitRust to Rust ----------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the EmitRust-to-Rust emitter: a pure, syntax-directed
/// translation from the EmitRust dialect to Rust source text. The emitter is
/// structured after upstream EmitC's CppEmitter: an internal emitter class
/// holds the indented output stream and a per-function value-name map, and a
/// llvm::TypeSwitch dispatches over the supported operations. Place
/// operations (variable/member/subscript/deref) emit nothing at their
/// program point; the place expressions they denote are rendered on demand
/// by a recursive helper when a load, assign, or borrow consumes them. Every
/// construct that cannot be represented in Rust fails the translation with a
/// located diagnostic; no silently wrong output is ever produced.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Target/TranslateToRust.h"

#include "EmitRust/CSymbolLinkage.h"
#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Support/IndentedOstream.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/MathExtras.h"

#include <charconv>
#include <string>
#include <system_error>

using namespace mlir;

namespace {

/// Emitter that translates EmitRust operations into Rust source text.
///
/// The emitter is a functional core over an output stream: it owns no state
/// other than the indented stream wrapper and the per-function map from SSA
/// values to their Rust binding names. Names are assigned in emission order:
/// within each function, first the entry block arguments, then every op
/// result (and for-loop induction variable) as it is encountered top-down.
class RustEmitter {
public:
  /// Creates an emitter writing to `os` under `options`.
  RustEmitter(raw_ostream &os, const emitrust::RustEmitOptions &options)
      : os(os), options(options) {}

  /// Emits `op` as Rust source text; dispatches over all supported ops.
  /// Unsupported operations fail with a located "unable to translate op"
  /// diagnostic.
  LogicalResult emitOperation(Operation &op);

private:
  //===--------------------------------------------------------------------===//
  // Structural helpers
  //===--------------------------------------------------------------------===//

  /// Increases the output indentation by one nesting level (4 spaces).
  void increaseIndent() {
    // raw_indented_ostream uses a fixed 2-space step; apply it twice to get
    // the 4-space Rust convention.
    os.indent();
    os.indent();
  }

  /// Decreases the output indentation by one nesting level (4 spaces).
  void decreaseIndent() {
    os.unindent();
    os.unindent();
  }

  /// Returns the Rust name bound to `value`, assigning the next sequential
  /// name (v0, v1, ...) if the value has not been named yet.
  std::string assignName(Value value);

  /// Returns the Rust name previously bound to `value`, or a located error
  /// if the value has not been defined yet.
  FailureOr<std::string> lookupName(Location loc, Value value);

  /// Emits the Rust name of `value`, or fails with a located diagnostic.
  LogicalResult emitOperand(Location loc, Value value);

  /// Emits the Rust rendering of `type`, or fails with a located
  /// "cannot translate type" diagnostic.
  LogicalResult emitType(Location loc, Type type);

  /// Emits the Rust expression for a constant `attr`, or fails with a
  /// located diagnostic for unsupported attributes.
  LogicalResult emitAttribute(Location loc, Attribute attr);

  /// Emits a floating-point literal with round-trip precision, guaranteeing
  /// that either a decimal point or an exponent appears (e.g. 4.2, 1.0).
  LogicalResult emitFloatValue(Location loc, double value, bool isF32);

  /// Emits the Rust place expression denoted by the lvalue-typed `value` by
  /// recursing through its chain of variable/member/subscript/deref/enum_raw
  /// producers. Any other producer (or a block argument) is an error.
  LogicalResult emitPlaceExpr(Location loc, Value value);

  /// Emits the default value of `type`: `0` for integers and index, `0.0`
  /// for floats, `false` for `i1`, `Name::default()` for structs and
  /// enums, `None` for function pointers, and `[<element-default>; N]` for
  /// arrays.
  LogicalResult emitDefaultValue(Location loc, Type type);

  /// Emits the Rust expression for a global initializer of value type
  /// `type`: an ArrayAttr renders as an array literal `[e0, e1, ...]` for
  /// an array type or a struct literal `Name { f0: e0, ... }` for a struct
  /// type (fields resolved from the `emitrust.struct_def` visible from
  /// `op`, in declaration order); any other attribute renders through
  /// emitAttribute. Nested aggregate elements recurse.
  LogicalResult emitAggregateInit(Operation *op, Location loc, Attribute init,
                                  Type type);

  /// Emits `value` as a quoted Rust string literal, escaping backslashes,
  /// quotes, newlines, tabs, and carriage returns.
  void emitEscapedStringLiteral(StringRef value);

  /// Emits "let vN: T = " (with `mut` when `isMut`), naming `result`.
  LogicalResult emitLetPrologue(Value result, bool isMut);

  /// Emits the statements of a single-block `region` one indentation level
  /// deeper. Fails on multi-block regions; an empty region emits nothing.
  LogicalResult emitRegionBody(Operation *parent, Region &region);

  //===--------------------------------------------------------------------===//
  // Per-operation emitters
  //===--------------------------------------------------------------------===//

  /// Emits the children of a module in order; only use, verbatim, func,
  /// impl, struct/enum definition, and global operations are allowed at
  /// module level.
  LogicalResult emitModule(ModuleOp moduleOp);
  /// Emits `impl <name> { ... }` with the contained functions rendered as
  /// `&mut self` methods one indentation level deeper.
  LogicalResult emitImpl(emitrust::ImplOp implOp);
  /// Emits `<place>.<method>(args);`, bound with a `let` when the call
  /// produces a result. Rust's auto-ref scopes the `&mut` borrow of the
  /// receiver place to the call expression.
  LogicalResult emitMethodCall(emitrust::MethodCallOp callOp);
  /// Emits `use <path>;`.
  LogicalResult emitUse(emitrust::UseOp useOp);
  /// Emits the verbatim string on its own line(s).
  LogicalResult emitVerbatim(emitrust::VerbatimOp verbatimOp);
  /// Emits a `fn` item with typed parameters, return type, and body.
  LogicalResult emitFunc(emitrust::FuncOp funcOp);
  /// Emits `return;` or `return vX;`.
  LogicalResult emitReturn(emitrust::ReturnOp returnOp);
  /// Emits an opaque call as a statement, a let, or a tuple-destructuring
  /// let depending on the number of results. When the `args` attribute is
  /// present, the rendered argument list is taken from it: index-typed
  /// integer attributes reference operands, string attributes render as
  /// escaped Rust string literals, and other attributes render as constants.
  LogicalResult emitCallOpaque(emitrust::CallOpaqueOp callOp);
  /// Emits an indirect call through a function pointer as
  /// `vF.expect("null function pointer")(vA, vB)`, bound with a `let` when
  /// the call produces a result and as a bare statement otherwise.
  LogicalResult emitCallIndirect(emitrust::CallIndirectOp callOp);
  /// Emits `let vN: T = <constant>;`.
  LogicalResult emitConstant(emitrust::ConstantOp constantOp);
  /// Emits `let vN: T = <verbatim expression>;`.
  LogicalResult emitLiteral(emitrust::LiteralOp literalOp);
  /// Emits `let vN: T = vInit;` or `let mut vN: T = vInit;`.
  LogicalResult emitLet(emitrust::LetOp letOp);
  /// Emits `vDst = vSrc;`, rendering an lvalue-typed destination as its
  /// place expression.
  LogicalResult emitAssign(emitrust::AssignOp assignOp);
  /// Emits `let vN: T = vA <symbol> vB;` for binary and comparison ops.
  LogicalResult emitBinary(Operation *op, StringRef symbol);
  /// Emits `let vN: T = vA.<method>(vB);` for the method-call binary form.
  LogicalResult emitBinaryMethod(Operation *op, StringRef method);
  /// Emits an add/sub/mul: the `wrapping_*` method-call form when the
  /// result type is an unsigned integer (C defines unsigned overflow as
  /// wrap-around; Rust's infix operators panic on overflow in debug
  /// builds), the infix `symbol` form otherwise.
  LogicalResult emitWrappingBinary(Operation *op, StringRef symbol,
                                   StringRef method);
  /// Emits `let vN: bool = vA <pred> vB;`.
  LogicalResult emitCmp(emitrust::CmpOp cmpOp);
  /// Emits `let vN: T = vA as T;`.
  LogicalResult emitCast(emitrust::CastOp castOp);
  /// Emits the bit-exact float/integer reinterpretation:
  /// `let vN: fW = fW::from_bits(vA);` for an integer-to-float bitcast,
  /// `let vN: TInt = vA.to_bits();` for a float-to-integer one. A
  /// signless integer side wraps the same-width (bit-preserving) `as`
  /// conversion `from_bits`/`to_bits`'s `uW` requires.
  LogicalResult emitBitcast(emitrust::BitcastOp bitcastOp);
  /// Emits `let vN: T = if vCond { vA } else { vB };`.
  LogicalResult emitSelect(emitrust::SelectOp selectOp);
  /// Emits an `if` statement, with `} else {` when the else region is
  /// present and non-empty.
  LogicalResult emitIf(emitrust::IfOp ifOp);
  /// Emits `for vI in (vLB..vUB).step_by(vS as usize) { ... }`.
  LogicalResult emitFor(emitrust::ForOp forOp);
  /// Emits `loop { ... }`.
  LogicalResult emitLoop(emitrust::LoopOp loopOp);
  /// Emits a `match` statement with one literal integer arm per case region
  /// (each case value rendered in the discriminator's type) and a trailing
  /// `_ =>` arm for the default region.
  LogicalResult emitSwitch(emitrust::SwitchOp switchOp);
  /// Emits a `#[derive(Clone, Copy, Default)]` struct item with its fields.
  LogicalResult emitStructDef(emitrust::StructDefOp structDefOp);
  /// Emits a C enum as a value-preserving open enum: a
  /// `#[repr(transparent)]` tuple struct over the storage integer (`i32`,
  /// or `u32` with the `unsigned_underlying` marker), one associated
  /// constant per variant, and a `Default` impl returning the first
  /// variant.
  LogicalResult emitEnumDef(emitrust::EnumDefOp enumDefOp);
  /// Emits a global: `static NAME: T = <init-or-default>;` for `const`
  /// globals, and a `thread_local!` `std::cell::Cell` item otherwise.
  LogicalResult emitGlobal(emitrust::GlobalOp globalOp);
  /// Emits `let vN: T = NAME;` (const global) or
  /// `let vN: T = NAME.with(|__emitrust_tl| __emitrust_tl.get());`
  /// (mutable global). The binder name is reserved by the importer:
  /// identifier patterns cannot shadow statics, so a translated C global
  /// spelled like the binder would break every accessor.
  LogicalResult emitGlobalLoad(emitrust::GlobalLoadOp loadOp);
  /// Emits `NAME.with(|__emitrust_tl| __emitrust_tl.set(vX));`.
  LogicalResult emitGlobalStore(emitrust::GlobalStoreOp storeOp);
  /// Emits `let vN: T = vS[vI as usize].get();`.
  LogicalResult emitCellGet(emitrust::CellGetOp getOp);
  /// Emits `vS[vI as usize].set(vV);`.
  LogicalResult emitCellSet(emitrust::CellSetOp setOp);
  /// Emits the global's `with` accessor wrapping the region body:
  /// `NAME.with(|__emitrust_tl| {`, an unsizing coercion of the binder to
  /// `&std::cell::Cell<[T]>`, the region argument's
  /// `let vN: &[std::cell::Cell<T>] = __emitrust_tl.as_slice_of_cells();`
  /// binding, the region's statements, and the closing `});`. Nested
  /// `emitrust.global_cells` render as nested `with` closures, so no
  /// borrow ever escapes its accessor.
  LogicalResult emitGlobalCells(emitrust::GlobalCellsOp cellsOp);
  /// Emits `let mut vN: T = <init-or-default>;` for a local variable.
  LogicalResult emitVariable(emitrust::VariableOp variableOp);
  /// Emits `let vN: T = <place-expr>;`.
  LogicalResult emitLoad(emitrust::LoadOp loadOp);
  /// Emits `let vN: &T = &<place>;` or `let vN: &mut T = &mut <place>;`.
  LogicalResult emitAddrOf(emitrust::AddrOfOp addrOfOp);
  /// Emits `let vN: &[T] = &<place>[idx as usize..];` or the `&mut` form
  /// with the `mut` marker; the `as usize` cast is omitted for an
  /// index-typed index.
  LogicalResult emitSliceOf(emitrust::SliceOfOp sliceOfOp);

  /// FR-51: the `pub ` an exported item is prefixed with, or the empty
  /// string. Returns nothing at all unless `RustEmitOptions::exportItems` is
  /// set, which is what keeps binary-crate output byte-identical.
  ///
  /// \param symbol the emitted item name, consulted only for its
  ///        internal-linkage marker.
  StringRef itemVisibility(StringRef symbol) const {
    if (!options.exportItems || emitrust::isInternalLinkageSymbolName(symbol))
      return "";
    return "pub ";
  }

  /// The `pub ` prefix for a part of an exported TYPE — a struct field, a
  /// tuple element, an enum variant constant. Unlike `itemVisibility` this
  /// takes no symbol: types are exported unconditionally in library mode
  /// (see `RustEmitOptions::exportItems` for why), so their parts must be
  /// reachable too or the type is exported but unusable.
  StringRef typePartVisibility() const {
    return options.exportItems ? "pub " : "";
  }

  /// Output stream tracking the current indentation.
  raw_indented_ostream os;

  /// The emission knobs this translation runs under.
  emitrust::RustEmitOptions options;

  /// Per-function map from SSA values to their Rust binding names.
  DenseMap<Value, std::string> valueNames;

  /// Counter feeding the sequential v0, v1, ... naming scheme.
  unsigned valueCount = 0;
};

} // namespace

//===----------------------------------------------------------------------===//
// Structural helpers
//===----------------------------------------------------------------------===//

std::string RustEmitter::assignName(Value value) {
  std::string &name = valueNames[value];
  if (name.empty())
    name = ("v" + Twine(valueCount++)).str();
  return name;
}

FailureOr<std::string> RustEmitter::lookupName(Location loc, Value value) {
  auto it = valueNames.find(value);
  if (it == valueNames.end()) {
    emitError(loc) << "operand value is used before it is defined";
    return failure();
  }
  return it->second;
}

LogicalResult RustEmitter::emitOperand(Location loc, Value value) {
  FailureOr<std::string> name = lookupName(loc, value);
  if (failed(name))
    return failure();
  os << *name;
  return success();
}

LogicalResult RustEmitter::emitType(Location loc, Type type) {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    unsigned width = intType.getWidth();
    if (width == 1 && intType.isSignless()) {
      os << "bool";
      return success();
    }
    if (width == 8 || width == 16 || width == 32 || width == 64) {
      os << (intType.isUnsigned() ? 'u' : 'i') << width;
      return success();
    }
    return emitError(loc) << "cannot translate type " << type;
  }
  if (isa<IndexType>(type)) {
    os << "usize";
    return success();
  }
  if (type.isF32()) {
    os << "f32";
    return success();
  }
  if (type.isF64()) {
    os << "f64";
    return success();
  }
  if (auto opaqueType = dyn_cast<emitrust::OpaqueType>(type)) {
    os << opaqueType.getValue();
    return success();
  }
  if (auto refType = dyn_cast<emitrust::RefType>(type)) {
    os << "&";
    return emitType(loc, refType.getPointee());
  }
  if (auto mutRefType = dyn_cast<emitrust::MutRefType>(type)) {
    os << "&mut ";
    return emitType(loc, mutRefType.getPointee());
  }
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    os << "[";
    if (failed(emitType(loc, arrayType.getElementType())))
      return failure();
    os << "; " << arrayType.getSize() << "]";
    return success();
  }
  if (auto sliceType = dyn_cast<emitrust::SliceType>(type)) {
    // Unsized; reaches the output only behind a reference (`&mut [T]`).
    os << "[";
    if (failed(emitType(loc, sliceType.getElementType())))
      return failure();
    os << "]";
    return success();
  }
  if (auto cellSliceType = dyn_cast<emitrust::CellSliceType>(type)) {
    // Unsized; reaches the output only behind the shared reference
    // (`&[std::cell::Cell<T>]`, always fully qualified).
    os << "[std::cell::Cell<";
    if (failed(emitType(loc, cellSliceType.getElementType())))
      return failure();
    os << ">]";
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    os << structType.getName();
    return success();
  }
  if (auto enumType = dyn_cast<emitrust::EnumType>(type)) {
    os << enumType.getName();
    return success();
  }
  if (auto fnPtrType = dyn_cast<emitrust::FnPtrType>(type)) {
    // Nullable function pointer: the C null pointer is None, so no unsafe
    // sentinel is ever needed. The `-> R` clause is omitted for a void
    // result, matching Rust's `fn(...)` spelling.
    os << "Option<fn(";
    bool first = true;
    for (Type input : fnPtrType.getInputs()) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitType(loc, input)))
        return failure();
    }
    os << ")";
    if (!fnPtrType.getResults().empty()) {
      os << " -> ";
      if (failed(emitType(loc, fnPtrType.getResults().front())))
        return failure();
    }
    os << ">";
    return success();
  }
  // Lvalue types are never rendered; they fall through to the error below.
  return emitError(loc) << "cannot translate type " << type;
}

LogicalResult RustEmitter::emitAttribute(Location loc, Attribute attr) {
  if (auto intAttr = dyn_cast<IntegerAttr>(attr)) {
    Type type = intAttr.getType();
    if (type.isInteger(1)) {
      os << (intAttr.getValue().getBoolValue() ? "true" : "false");
      return success();
    }
    auto intType = dyn_cast<IntegerType>(type);
    bool isUnsigned = intType && intType.isUnsigned();
    SmallString<32> digits;
    intAttr.getValue().toString(digits, /*Radix=*/10, /*Signed=*/!isUnsigned);
    os << digits;
    return success();
  }
  if (auto floatAttr = dyn_cast<FloatAttr>(attr)) {
    // A fold of `1.0 / 0.0` produces an infinity constant; Rust spells it
    // deterministically. NaN constants stay rejected: Rust does not
    // guarantee the sign/payload of its NAN constant, so a byte pattern
    // could silently diverge from C's.
    if (floatAttr.getValue().isInfinity()) {
      os << (floatAttr.getType().isF32() ? "f32" : "f64")
         << (floatAttr.getValue().isNegative() ? "::NEG_INFINITY"
                                               : "::INFINITY");
      return success();
    }
    if (!floatAttr.getValue().isFinite())
      return emitError(loc)
             << "cannot translate non-finite floating-point constant";
    return emitFloatValue(loc, floatAttr.getValueAsDouble(),
                          floatAttr.getType().isF32());
  }
  if (auto opaqueAttr = dyn_cast<emitrust::OpaqueAttr>(attr)) {
    os << opaqueAttr.getValue();
    return success();
  }
  return emitError(loc) << "cannot translate constant attribute " << attr;
}

LogicalResult RustEmitter::emitFloatValue(Location loc, double value,
                                          bool isF32) {
  // Shortest representation that round-trips through the source type.
  char buffer[64];
  std::to_chars_result result{};
  if (isF32)
    result =
        std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(value));
  else
    result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc())
    return emitError(loc) << "failed to format floating-point constant";
  StringRef text(buffer, static_cast<size_t>(result.ptr - buffer));
  os << text;
  // Rust float literals need a decimal point or an exponent.
  if (!text.contains('.') && !text.contains('e') && !text.contains('E'))
    os << ".0";
  return success();
}

LogicalResult RustEmitter::emitPlaceExpr(Location loc, Value value) {
  Operation *def = value.getDefiningOp();
  if (!def)
    return emitError(loc)
           << "cannot emit a place expression for a block argument";
  return llvm::TypeSwitch<Operation *, LogicalResult>(def)
      .Case<emitrust::VariableOp>([&](emitrust::VariableOp variableOp) {
        return emitOperand(loc, variableOp.getResult());
      })
      .Case<emitrust::MemberOp>([&](emitrust::MemberOp memberOp) {
        if (failed(emitPlaceExpr(loc, memberOp.getOperand())))
          return failure();
        os << "." << memberOp.getMember();
        return success();
      })
      .Case<emitrust::SubscriptOp>([&](emitrust::SubscriptOp subscriptOp) {
        if (failed(emitPlaceExpr(loc, subscriptOp.getArray())))
          return failure();
        os << "[";
        if (failed(emitOperand(loc, subscriptOp.getIndex())))
          return failure();
        if (!isa<IndexType>(subscriptOp.getIndex().getType()))
          os << " as usize";
        os << "]";
        return success();
      })
      .Case<emitrust::DerefOp>([&](emitrust::DerefOp derefOp) {
        os << "(*";
        if (failed(emitOperand(loc, derefOp.getOperand())))
          return failure();
        os << ")";
        return success();
      })
      .Case<emitrust::EnumRawOp>([&](emitrust::EnumRawOp enumRawOp) {
        if (failed(emitPlaceExpr(loc, enumRawOp.getOperand())))
          return failure();
        os << ".0";
        return success();
      })
      .Default([&](Operation *) -> LogicalResult {
        return emitError(loc) << "expected a place-producing operation "
                                 "(variable, member, subscript, deref, or "
                                 "enum_raw)";
      });
}

LogicalResult RustEmitter::emitDefaultValue(Location loc, Type type) {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    if (intType.getWidth() == 1 && intType.isSignless())
      os << "false";
    else
      os << "0";
    return success();
  }
  if (isa<IndexType>(type)) {
    os << "0";
    return success();
  }
  if (type.isF32() || type.isF64()) {
    os << "0.0";
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    os << structType.getName() << "::default()";
    return success();
  }
  if (auto enumType = dyn_cast<emitrust::EnumType>(type)) {
    os << enumType.getName() << "::default()";
    return success();
  }
  if (isa<emitrust::FnPtrType>(type)) {
    // A default-initialized function pointer is the C null pointer.
    os << "None";
    return success();
  }
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    os << "[";
    if (failed(emitDefaultValue(loc, arrayType.getElementType())))
      return failure();
    os << "; " << arrayType.getSize() << "]";
    return success();
  }
  if (auto opaqueType = dyn_cast<emitrust::OpaqueType>(type)) {
    // The opaque types a variable is declared with are the String staging
    // binding of the sprintf lowering (default: the empty string), the
    // owned FILE* handle of the stdio lowering (default: C's NULL), and
    // (W2.3) a recognized `std::vector<T>`/`std::string` local (default:
    // `Vec::new()`/`String::new()`, mirroring the real C++ default
    // constructor's empty-container result) — every STL local's importer-
    // side construction (`emitStlConstruct`) always follows up with an
    // explicit `emitrust.assign` immediately after, so this default value
    // is dead code for every STL-recognized program the importer produces,
    // but `emitVariable` renders it unconditionally for every
    // no-initializer `emitrust.variable`.
    if (opaqueType.getValue() == "String") {
      os << "String::new()";
      return success();
    }
    if (opaqueType.getValue() == "__EmitrustFile") {
      os << "__EmitrustFile::Null";
      return success();
    }
    if (opaqueType.getValue().starts_with("Vec<")) {
      os << "Vec::new()";
      return success();
    }
  }
  return emitError(loc) << "no default value for type " << type;
}

void RustEmitter::emitEscapedStringLiteral(StringRef value) {
  os << '"';
  for (char c : value) {
    switch (c) {
    case '\\':
      os << "\\\\";
      break;
    case '"':
      os << "\\\"";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\t':
      os << "\\t";
      break;
    case '\r':
      os << "\\r";
      break;
    default:
      os << c;
      break;
    }
  }
  os << '"';
}

LogicalResult RustEmitter::emitLetPrologue(Value result, bool isMut) {
  std::string name = assignName(result);
  os << "let ";
  if (isMut)
    os << "mut ";
  os << name << ": ";
  if (failed(emitType(result.getLoc(), result.getType())))
    return failure();
  os << " = ";
  return success();
}

LogicalResult RustEmitter::emitRegionBody(Operation *parent, Region &region) {
  if (region.empty())
    return success();
  if (!region.hasOneBlock())
    return parent->emitOpError("multi-block regions are not supported");
  increaseIndent();
  for (Operation &op : region.front()) {
    if (failed(emitOperation(op)))
      return failure();
  }
  decreaseIndent();
  return success();
}

//===----------------------------------------------------------------------===//
// Per-operation emitters
//===----------------------------------------------------------------------===//

LogicalResult RustEmitter::emitModule(ModuleOp moduleOp) {
  for (Operation &op : *moduleOp.getBody()) {
    if (!isa<emitrust::UseOp, emitrust::VerbatimOp, emitrust::FuncOp,
             emitrust::ImplOp, emitrust::StructDefOp, emitrust::EnumDefOp,
             emitrust::GlobalOp>(&op))
      return op.emitOpError("unable to translate op");
    if (failed(emitOperation(op)))
      return failure();
  }
  return success();
}

LogicalResult RustEmitter::emitImpl(emitrust::ImplOp implOp) {
  os << "impl " << implOp.getStructName() << " {\n";
  increaseIndent();
  for (Operation &op : implOp.getBody().front()) {
    if (failed(emitOperation(op)))
      return failure();
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitUse(emitrust::UseOp useOp) {
  os << "use " << useOp.getPath() << ";\n";
  return success();
}

LogicalResult RustEmitter::emitVerbatim(emitrust::VerbatimOp verbatimOp) {
  os << verbatimOp.getValue() << "\n";
  return success();
}

LogicalResult RustEmitter::emitFunc(emitrust::FuncOp funcOp) {
  Operation *op = funcOp.getOperation();
  auto fn = cast<FunctionOpInterface>(op);

  // Each function opens a fresh value-naming scope: v0, v1, ...
  valueNames.clear();
  valueCount = 0;

  Region &body = fn.getFunctionBody();
  if (body.empty())
    return op->emitOpError("cannot translate a function without a body");
  if (!body.hasOneBlock())
    return op->emitOpError("multi-block regions are not supported");
  if (fn.getNumResults() > 1)
    return op->emitOpError(
        "cannot translate a function with more than one result");

  Block &entryBlock = body.front();
  // A function directly inside an `emitrust.impl` is a method, UNLESS it
  // carries the `static_method` marker (W2.2), in which case it is a
  // receiverless associated function (`Struct::name(...)`) and every
  // argument renders like an ordinary parameter. A genuine (receiver-
  // having) method's first argument is the receiver, named `self` and
  // rendered as `&mut self` or `&self` depending on whether the impl
  // verifier-checked receiver type is `!emitrust.mut_ref` (mutating,
  // historical behavior) or `!emitrust.ref` (const, W2.2). The existing
  // deref place rendering then yields `(*self).field...` naturally either
  // way.
  bool isMethod = isa<emitrust::ImplOp>(op->getParentOp()) &&
                  !op->hasAttr(emitrust::kStaticMethodAttrName);
  StringRef symbol = SymbolTable::getSymbolName(op).getValue();
  os << itemVisibility(symbol) << "fn " << symbol << "(";
  bool first = true;
  for (BlockArgument argument : entryBlock.getArguments()) {
    if (!first)
      os << ", ";
    first = false;
    if (isMethod && argument.getArgNumber() == 0) {
      valueNames[argument] = "self";
      os << (isa<emitrust::RefType>(argument.getType()) ? "&self"
                                                         : "&mut self");
      continue;
    }
    os << assignName(argument) << ": ";
    if (failed(emitType(argument.getLoc(), argument.getType())))
      return failure();
  }
  os << ")";
  if (fn.getNumResults() == 1) {
    os << " -> ";
    if (failed(emitType(op->getLoc(), fn.getResultTypes().front())))
      return failure();
  }
  os << " {\n";
  increaseIndent();
  for (Operation &child : entryBlock) {
    if (failed(emitOperation(child)))
      return failure();
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitReturn(emitrust::ReturnOp returnOp) {
  Operation *op = returnOp.getOperation();
  if (op->getNumOperands() == 0) {
    os << "return;\n";
    return success();
  }
  os << "return ";
  if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitCallOpaque(emitrust::CallOpaqueOp callOp) {
  Operation *op = callOp.getOperation();
  Location loc = op->getLoc();
  unsigned numResults = op->getNumResults();

  if (numResults == 1) {
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
  } else if (numResults > 1) {
    os << "let (";
    bool first = true;
    for (Value result : op->getResults()) {
      if (!first)
        os << ", ";
      first = false;
      os << assignName(result);
    }
    os << "): (";
    first = true;
    for (Value result : op->getResults()) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitType(loc, result.getType())))
        return failure();
    }
    os << ") = ";
  }

  os << callOp.getCallee() << "(";
  bool first = true;
  if (std::optional<ArrayAttr> args = callOp.getArgs()) {
    for (Attribute arg : *args) {
      if (!first)
        os << ", ";
      first = false;
      auto intAttr = dyn_cast<IntegerAttr>(arg);
      if (intAttr && isa<IndexType>(intAttr.getType())) {
        if (failed(emitOperand(loc, op->getOperand(intAttr.getInt()))))
          return failure();
      } else if (auto strAttr = dyn_cast<StringAttr>(arg)) {
        emitEscapedStringLiteral(strAttr.getValue());
      } else if (failed(emitAttribute(loc, arg))) {
        return failure();
      }
    }
  } else {
    for (Value operand : op->getOperands()) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitOperand(loc, operand)))
        return failure();
    }
  }
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitCallIndirect(emitrust::CallIndirectOp callOp) {
  Operation *op = callOp.getOperation();
  Location loc = op->getLoc();
  if (op->getNumResults() == 1 &&
      failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  // Calling a null C function pointer is undefined behavior; the expect
  // refines it into a deterministic panic.
  if (failed(emitOperand(loc, callOp.getCallee())))
    return failure();
  os << ".expect(\"null function pointer\")(";
  bool first = true;
  for (Value argument : callOp.getArgs()) {
    if (!first)
      os << ", ";
    first = false;
    if (failed(emitOperand(loc, argument)))
      return failure();
  }
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitMethodCall(emitrust::MethodCallOp callOp) {
  Operation *op = callOp.getOperation();
  Location loc = op->getLoc();
  if (op->getNumResults() == 1 &&
      failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitPlaceExpr(loc, callOp.getReceiver())))
    return failure();
  os << "." << callOp.getMethod() << "(";
  bool first = true;
  for (Value argument : callOp.getArgs()) {
    if (!first)
      os << ", ";
    first = false;
    if (failed(emitOperand(loc, argument)))
      return failure();
  }
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitConstant(emitrust::ConstantOp constantOp) {
  Operation *op = constantOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitAttribute(op->getLoc(), constantOp.getValue())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitLiteral(emitrust::LiteralOp literalOp) {
  Operation *op = literalOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << literalOp.getValue() << ";\n";
  return success();
}

LogicalResult RustEmitter::emitLet(emitrust::LetOp letOp) {
  Operation *op = letOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), letOp.getIsMut())))
    return failure();
  if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitAssign(emitrust::AssignOp assignOp) {
  Operation *op = assignOp.getOperation();
  Location loc = op->getLoc();
  Value var = assignOp.getVar();
  if (isa<emitrust::LValueType>(var.getType())) {
    if (failed(emitPlaceExpr(loc, var)))
      return failure();
  } else if (failed(emitOperand(loc, var))) {
    return failure();
  }
  os << " = ";
  if (failed(emitOperand(loc, assignOp.getValue())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitBinary(Operation *op, StringRef symbol) {
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
    return failure();
  os << " " << symbol << " ";
  if (failed(emitOperand(op->getLoc(), op->getOperand(1))))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitBinaryMethod(Operation *op, StringRef method) {
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
    return failure();
  os << "." << method << "(";
  if (failed(emitOperand(op->getLoc(), op->getOperand(1))))
    return failure();
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitWrappingBinary(Operation *op, StringRef symbol,
                                              StringRef method) {
  auto intType = dyn_cast<IntegerType>(op->getResult(0).getType());
  if (intType && intType.isUnsigned())
    return emitBinaryMethod(op, method);
  return emitBinary(op, symbol);
}

/// Returns the Rust operator spelling for `predicate`, or an empty string if
/// the predicate is unknown.
static StringRef cmpPredicateSymbol(emitrust::CmpPredicate predicate) {
  switch (predicate) {
  case emitrust::CmpPredicate::eq:
    return "==";
  case emitrust::CmpPredicate::ne:
    return "!=";
  case emitrust::CmpPredicate::lt:
    return "<";
  case emitrust::CmpPredicate::le:
    return "<=";
  case emitrust::CmpPredicate::gt:
    return ">";
  case emitrust::CmpPredicate::ge:
    return ">=";
  }
  return "";
}

LogicalResult RustEmitter::emitCmp(emitrust::CmpOp cmpOp) {
  StringRef symbol = cmpPredicateSymbol(cmpOp.getPredicate());
  if (symbol.empty())
    return cmpOp.getOperation()->emitOpError("unknown comparison predicate");
  return emitBinary(cmpOp.getOperation(), symbol);
}

LogicalResult RustEmitter::emitCast(emitrust::CastOp castOp) {
  Operation *op = castOp.getOperation();
  // Rust has no `as bool`; a boolean-producing conversion must be modeled
  // as a comparison instead of a cast.
  if (op->getResult(0).getType().isInteger(1))
    return op->emitOpError("cannot translate a cast to bool");
  // Integer-to-enum: the open-enum tuple struct is constructed around the
  // source converted to the enum's storage type, preserving the value
  // exactly as C's conversion to the underlying type does.
  if (auto enumType = dyn_cast<emitrust::EnumType>(op->getResult(0).getType())) {
    auto enumDef = SymbolTable::lookupNearestSymbolFrom<emitrust::EnumDefOp>(
        op, StringAttr::get(op->getContext(), enumType.getName()));
    if (!enumDef)
      return op->emitOpError("cast to enum type ")
             << enumType << " requires a visible emitrust.enum_def";
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    os << enumType.getName() << "(";
    if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
      return failure();
    os << " as " << (enumDef.getUnsignedUnderlying() ? "u32" : "i32")
       << ");\n";
    return success();
  }
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
    return failure();
  // Enum-to-integer: the raw value is read out of the tuple struct's only
  // field before the `as` conversion.
  if (isa<emitrust::EnumType>(op->getOperand(0).getType()))
    os << ".0";
  os << " as ";
  if (failed(emitType(op->getLoc(), op->getResult(0).getType())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitBitcast(emitrust::BitcastOp bitcastOp) {
  Operation *op = bitcastOp.getOperation();
  Location loc = op->getLoc();
  Type resultType = op->getResult(0).getType();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (auto floatType = dyn_cast<FloatType>(resultType)) {
    // Integer-to-float: `fW::from_bits` takes `uW`, so a signless source
    // first converts (same-width `as`, bit-preserving) to unsigned.
    auto intType = cast<IntegerType>(op->getOperand(0).getType());
    os << (floatType.isF32() ? "f32" : "f64") << "::from_bits(";
    if (failed(emitOperand(loc, op->getOperand(0))))
      return failure();
    if (!intType.isUnsigned())
      os << " as u" << intType.getWidth();
    os << ");\n";
    return success();
  }
  // Float-to-integer: `to_bits` yields `uW`; a signless result converts
  // from it (same-width `as`, bit-preserving).
  auto intType = cast<IntegerType>(resultType);
  if (failed(emitOperand(loc, op->getOperand(0))))
    return failure();
  os << ".to_bits()";
  if (!intType.isUnsigned())
    os << " as i" << intType.getWidth();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitSelect(emitrust::SelectOp selectOp) {
  Operation *op = selectOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << "if ";
  if (failed(emitOperand(loc, selectOp.getCondition())))
    return failure();
  os << " { ";
  if (failed(emitOperand(loc, selectOp.getTrueValue())))
    return failure();
  os << " } else { ";
  if (failed(emitOperand(loc, selectOp.getFalseValue())))
    return failure();
  os << " };\n";
  return success();
}

LogicalResult RustEmitter::emitIf(emitrust::IfOp ifOp) {
  Operation *op = ifOp.getOperation();
  os << "if ";
  if (failed(emitOperand(op->getLoc(), op->getOperand(0))))
    return failure();
  os << " {\n";
  if (failed(emitRegionBody(op, ifOp.getThenRegion())))
    return failure();
  Region &elseRegion = ifOp.getElseRegion();
  if (!elseRegion.empty()) {
    os << "} else {\n";
    if (failed(emitRegionBody(op, elseRegion)))
      return failure();
  }
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitFor(emitrust::ForOp forOp) {
  Operation *op = forOp.getOperation();
  Location loc = op->getLoc();

  Region &region = op->getRegion(0);
  if (region.empty())
    return op->emitOpError("expected a non-empty loop body region");
  if (!region.hasOneBlock())
    return op->emitOpError("multi-block regions are not supported");
  Block &body = region.front();
  if (body.getNumArguments() != 1)
    return op->emitOpError(
        "expected a single induction-variable block argument");

  FailureOr<std::string> lower = lookupName(loc, op->getOperand(0));
  FailureOr<std::string> upper = lookupName(loc, op->getOperand(1));
  FailureOr<std::string> step = lookupName(loc, op->getOperand(2));
  if (failed(lower) || failed(upper) || failed(step))
    return failure();

  // The induction variable is named when the loop is emitted, i.e. after all
  // values defined above the loop and before the loop body's results.
  std::string induction = assignName(body.getArgument(0));
  os << "for " << induction << " in (" << *lower << ".." << *upper
     << ").step_by(" << *step << " as usize) {\n";
  increaseIndent();
  for (Operation &child : body) {
    if (failed(emitOperation(child)))
      return failure();
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitLoop(emitrust::LoopOp loopOp) {
  os << "loop {\n";
  if (failed(emitRegionBody(loopOp.getOperation(), loopOp.getRegion())))
    return failure();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitSwitch(emitrust::SwitchOp switchOp) {
  Operation *op = switchOp.getOperation();
  os << "match ";
  if (failed(emitOperand(op->getLoc(), switchOp.getDiscriminator())))
    return failure();
  os << " {\n";
  increaseIndent();
  Type discriminatorType = switchOp.getDiscriminator().getType();
  for (auto [value, region] :
       llvm::zip(switchOp.getCases(), switchOp.getCaseRegions())) {
    // The cases attribute stores the raw 64-bit case pattern; render it
    // interpreted in the discriminator's Rust type so the literal arm
    // compares equal at runtime. A usize (index) or unsigned discriminator
    // prints the low bits as unsigned decimal (a C `case -1:` on `long`
    // reaches a usize scrutinee as the bit pattern 2^64 - 1); a signed iN
    // discriminator prints the sign-interpreted value of its width.
    if (isa<IndexType>(discriminatorType)) {
      os << static_cast<uint64_t>(value);
    } else {
      auto intType = cast<IntegerType>(discriminatorType);
      unsigned width = intType.getWidth();
      if (intType.isUnsigned())
        os << (static_cast<uint64_t>(value) &
               llvm::maskTrailingOnes<uint64_t>(width));
      else
        os << llvm::SignExtend64(value, width);
    }
    os << " => {\n";
    if (failed(emitRegionBody(op, region)))
      return failure();
    os << "}\n";
  }
  os << "_ => {\n";
  if (failed(emitRegionBody(op, switchOp.getDefaultRegion())))
    return failure();
  os << "}\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitStructDef(emitrust::StructDefOp structDefOp) {
  Location loc = structDefOp.getLoc();
  os << "#[derive(Clone, Copy, Default)]\n";
  // A field-less struct_def (C's `struct T {};`) prints unit-like with an
  // empty brace body; the derives keep declaration, copy, and default
  // construction working exactly as for the non-empty shape.
  if (structDefOp.getFieldNames().empty()) {
    os << typePartVisibility() << "struct " << structDefOp.getSymName()
       << " {}\n";
    return success();
  }
  os << typePartVisibility() << "struct " << structDefOp.getSymName() << " {\n";
  increaseIndent();
  for (auto [nameAttr, typeAttr] :
       llvm::zip_equal(structDefOp.getFieldNames(),
                       structDefOp.getFieldTypes())) {
    os << typePartVisibility() << cast<StringAttr>(nameAttr).getValue() << ": ";
    if (failed(emitType(loc, cast<TypeAttr>(typeAttr).getValue())))
      return failure();
    os << ",\n";
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitEnumDef(emitrust::EnumDefOp enumDefOp) {
  StringRef name = enumDefOp.getSymName();
  StringRef storage = enumDefOp.getUnsignedUnderlying() ? "u32" : "i32";
  os << "#[repr(transparent)]\n";
  os << "#[derive(Clone, Copy, PartialEq)]\n";
  StringRef pub = typePartVisibility();
  os << pub << "struct " << name << "(" << pub << storage << ");\n";
  os << "impl " << name << " {\n";
  increaseIndent();
  for (auto [nameAttr, value] : llvm::zip_equal(enumDefOp.getVariantNames(),
                                                enumDefOp.getVariantValues()))
    os << pub << "const " << cast<StringAttr>(nameAttr).getValue() << ": "
       << name << " = " << name << "(" << value << ");\n";
  decreaseIndent();
  os << "}\n";
  StringRef firstVariant =
      cast<StringAttr>(enumDefOp.getVariantNames()[0]).getValue();
  os << "impl Default for " << name << " {\n";
  increaseIndent();
  os << "fn default() -> " << name << " { " << name << "::" << firstVariant
     << " }\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitAggregateInit(Operation *op, Location loc,
                                             Attribute init, Type type) {
  auto elements = dyn_cast<ArrayAttr>(init);
  if (!elements)
    return emitAttribute(loc, init);
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    os << "[";
    bool first = true;
    for (Attribute element : elements) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitAggregateInit(op, loc, element,
                                   arrayType.getElementType())))
        return failure();
    }
    os << "]";
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    auto structDef = SymbolTable::lookupNearestSymbolFrom<
        emitrust::StructDefOp>(op, StringAttr::get(op->getContext(),
                                                   structType.getName()));
    if (!structDef)
      return op->emitOpError("aggregate init for struct type ")
             << type << " requires a visible emitrust.struct_def";
    os << structType.getName() << " { ";
    for (auto [element, name, fieldType] :
         llvm::zip_equal(elements, structDef.getFieldNames(),
                         structDef.getFieldTypes())) {
      os << cast<StringAttr>(name).getValue() << ": ";
      if (failed(emitAggregateInit(op, loc, element,
                                   cast<TypeAttr>(fieldType).getValue())))
        return failure();
      os << ", ";
    }
    os << "}";
    return success();
  }
  // The GlobalOp verifier rejects list initializers on non-aggregate types.
  return emitError(loc) << "cannot translate list initializer for type "
                        << type;
}

LogicalResult RustEmitter::emitGlobal(emitrust::GlobalOp globalOp) {
  Location loc = globalOp.getLoc();
  Type type = globalOp.getType();

  // Emits the initializer expression: the (possibly aggregate) init
  // attribute when present, the type's default value otherwise (C
  // zero-initialization of static storage).
  auto emitInit = [&]() -> LogicalResult {
    if (Attribute init = globalOp.getInitAttr())
      return emitAggregateInit(globalOp.getOperation(), loc, init, type);
    return emitDefaultValue(loc, type);
  };

  if (globalOp.getIsConst()) {
    // Never-written global: a plain static item read directly. The
    // verifier guarantees the type is a scalar or array, whose default and
    // initializer expressions are const-evaluable.
    os << "static " << globalOp.getSymName() << ": ";
    if (failed(emitType(loc, type)))
      return failure();
    os << " = ";
    if (failed(emitInit()))
      return failure();
    os << ";\n";
    return success();
  }

  // Mutable global: interior mutability through a thread-local Cell keeps
  // the generated crate free of `unsafe` and `static mut`. Exact for the
  // single-threaded programs the importer accepts.
  os << "thread_local! {\n";
  increaseIndent();
  os << "static " << globalOp.getSymName() << ": std::cell::Cell<";
  if (failed(emitType(loc, type)))
    return failure();
  os << "> = std::cell::Cell::new(";
  if (failed(emitInit()))
    return failure();
  os << ");\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

/// Resolves the `emitrust.global` referenced by the load or store `op`, or
/// fails with a located diagnostic when the symbol does not name one.
static FailureOr<emitrust::GlobalOp> lookupGlobal(Operation *op,
                                                  FlatSymbolRefAttr symbol) {
  auto global = SymbolTable::lookupNearestSymbolFrom<emitrust::GlobalOp>(
      op, symbol.getAttr());
  if (!global) {
    op->emitOpError("'") << symbol.getValue()
                         << "' does not reference a valid emitrust.global";
    return failure();
  }
  return global;
}

LogicalResult RustEmitter::emitGlobalLoad(emitrust::GlobalLoadOp loadOp) {
  Operation *op = loadOp.getOperation();
  FailureOr<emitrust::GlobalOp> global =
      lookupGlobal(op, loadOp.getGlobalAttr());
  if (failed(global))
    return failure();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << global->getSymName();
  if (!global->getIsConst())
    os << ".with(|__emitrust_tl| __emitrust_tl.get())";
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitGlobalStore(emitrust::GlobalStoreOp storeOp) {
  Operation *op = storeOp.getOperation();
  FailureOr<emitrust::GlobalOp> global =
      lookupGlobal(op, storeOp.getGlobalAttr());
  if (failed(global))
    return failure();
  if (global->getIsConst())
    return op->emitOpError("cannot store to the immutable global @")
           << storeOp.getGlobal();
  os << global->getSymName() << ".with(|__emitrust_tl| __emitrust_tl.set(";
  if (failed(emitOperand(op->getLoc(), storeOp.getValue())))
    return failure();
  os << "));\n";
  return success();
}

LogicalResult RustEmitter::emitCellGet(emitrust::CellGetOp getOp) {
  Operation *op = getOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitOperand(loc, getOp.getSlice())))
    return failure();
  os << "[";
  if (failed(emitOperand(loc, getOp.getIndex())))
    return failure();
  os << " as usize].get();\n";
  return success();
}

LogicalResult RustEmitter::emitCellSet(emitrust::CellSetOp setOp) {
  Operation *op = setOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitOperand(loc, setOp.getSlice())))
    return failure();
  os << "[";
  if (failed(emitOperand(loc, setOp.getIndex())))
    return failure();
  os << " as usize].set(";
  if (failed(emitOperand(loc, setOp.getValue())))
    return failure();
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitGlobalCells(emitrust::GlobalCellsOp cellsOp) {
  Operation *op = cellsOp.getOperation();
  Location loc = op->getLoc();
  FailureOr<emitrust::GlobalOp> global =
      lookupGlobal(op, cellsOp.getGlobalAttr());
  if (failed(global))
    return failure();
  Block &body = cellsOp.getBody().front();
  BlockArgument cells = body.getArgument(0);
  auto refType = cast<emitrust::RefType>(cells.getType());
  Type elementType =
      cast<emitrust::CellSliceType>(refType.getPointee()).getElementType();

  os << global->getSymName() << ".with(|__emitrust_tl| {\n";
  increaseIndent();
  // Rust resolves `as_slice_of_cells` on `Cell<[T]>` only, and method
  // lookup performs no unsizing on the wrapped array, so the binder is
  // coerced to the unsized `&Cell<[T]>` first (the documented
  // `as_slice_of_cells` idiom).
  os << "let __emitrust_tl: &std::cell::Cell<[";
  if (failed(emitType(loc, elementType)))
    return failure();
  os << "]> = __emitrust_tl;\n";
  os << "let " << assignName(cells) << ": ";
  if (failed(emitType(loc, cells.getType())))
    return failure();
  os << " = __emitrust_tl.as_slice_of_cells();\n";
  for (Operation &child : body) {
    if (failed(emitOperation(child)))
      return failure();
  }
  decreaseIndent();
  os << "});\n";
  return success();
}

LogicalResult RustEmitter::emitVariable(emitrust::VariableOp variableOp) {
  Value result = variableOp.getResult();
  Location loc = variableOp.getLoc();
  Type valueType =
      cast<emitrust::LValueType>(result.getType()).getValueType();
  // A `const`-marked variable is never written after its initializer and
  // becomes an immutable `let` binding.
  os << (variableOp.getIsConst() ? "let " : "let mut ") << assignName(result)
     << ": ";
  if (failed(emitType(loc, valueType)))
    return failure();
  os << " = ";
  if (Attribute init = variableOp.getInitAttr()) {
    if (failed(emitAggregateInit(variableOp.getOperation(), loc, init,
                                 valueType)))
      return failure();
  } else if (failed(emitDefaultValue(loc, valueType))) {
    return failure();
  }
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitLoad(emitrust::LoadOp loadOp) {
  Operation *op = loadOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitPlaceExpr(op->getLoc(), loadOp.getOperand())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitAddrOf(emitrust::AddrOfOp addrOfOp) {
  Operation *op = addrOfOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << (addrOfOp.getIsMut() ? "&mut " : "&");
  if (failed(emitPlaceExpr(op->getLoc(), addrOfOp.getOperand())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitSliceOf(emitrust::SliceOfOp sliceOfOp) {
  Operation *op = sliceOfOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << (sliceOfOp.getIsMut() ? "&mut " : "&");
  if (failed(emitPlaceExpr(op->getLoc(), sliceOfOp.getBase())))
    return failure();
  os << "[";
  if (failed(emitOperand(op->getLoc(), sliceOfOp.getIndex())))
    return failure();
  if (!isa<IndexType>(sliceOfOp.getIndex().getType()))
    os << " as usize";
  os << "..];\n";
  return success();
}

//===----------------------------------------------------------------------===//
// Dispatch
//===----------------------------------------------------------------------===//

LogicalResult RustEmitter::emitOperation(Operation &op) {
  return llvm::TypeSwitch<Operation *, LogicalResult>(&op)
      .Case<ModuleOp>([&](ModuleOp moduleOp) { return emitModule(moduleOp); })
      .Case<emitrust::UseOp>(
          [&](emitrust::UseOp useOp) { return emitUse(useOp); })
      .Case<emitrust::VerbatimOp>([&](emitrust::VerbatimOp verbatimOp) {
        return emitVerbatim(verbatimOp);
      })
      .Case<emitrust::FuncOp>(
          [&](emitrust::FuncOp funcOp) { return emitFunc(funcOp); })
      .Case<emitrust::ImplOp>(
          [&](emitrust::ImplOp implOp) { return emitImpl(implOp); })
      .Case<emitrust::MethodCallOp>([&](emitrust::MethodCallOp callOp) {
        return emitMethodCall(callOp);
      })
      .Case<emitrust::ReturnOp>(
          [&](emitrust::ReturnOp returnOp) { return emitReturn(returnOp); })
      .Case<emitrust::CallOpaqueOp>([&](emitrust::CallOpaqueOp callOp) {
        return emitCallOpaque(callOp);
      })
      .Case<emitrust::CallIndirectOp>([&](emitrust::CallIndirectOp callOp) {
        return emitCallIndirect(callOp);
      })
      .Case<emitrust::ConstantOp>([&](emitrust::ConstantOp constantOp) {
        return emitConstant(constantOp);
      })
      .Case<emitrust::LiteralOp>([&](emitrust::LiteralOp literalOp) {
        return emitLiteral(literalOp);
      })
      .Case<emitrust::LetOp>(
          [&](emitrust::LetOp letOp) { return emitLet(letOp); })
      .Case<emitrust::AssignOp>(
          [&](emitrust::AssignOp assignOp) { return emitAssign(assignOp); })
      .Case<emitrust::AddOp>([&](emitrust::AddOp addOp) {
        return emitWrappingBinary(addOp.getOperation(), "+", "wrapping_add");
      })
      .Case<emitrust::SubOp>([&](emitrust::SubOp subOp) {
        return emitWrappingBinary(subOp.getOperation(), "-", "wrapping_sub");
      })
      .Case<emitrust::MulOp>([&](emitrust::MulOp mulOp) {
        return emitWrappingBinary(mulOp.getOperation(), "*", "wrapping_mul");
      })
      // Division, remainder, bitwise, and shift operators keep the infix
      // form on every integer type: Rust's `/`, `%`, `&`, `|`, `^`, `<<`,
      // and `>>` already match C's semantics on the matching signedness
      // (division by zero panics where C is undefined).
      .Case<emitrust::DivOp>([&](emitrust::DivOp divOp) {
        return emitBinary(divOp.getOperation(), "/");
      })
      .Case<emitrust::RemOp>([&](emitrust::RemOp remOp) {
        return emitBinary(remOp.getOperation(), "%");
      })
      .Case<emitrust::AndOp>([&](emitrust::AndOp andOp) {
        return emitBinary(andOp.getOperation(), "&");
      })
      .Case<emitrust::OrOp>([&](emitrust::OrOp orOp) {
        return emitBinary(orOp.getOperation(), "|");
      })
      .Case<emitrust::XorOp>([&](emitrust::XorOp xorOp) {
        return emitBinary(xorOp.getOperation(), "^");
      })
      .Case<emitrust::ShlOp>([&](emitrust::ShlOp shlOp) {
        return emitBinary(shlOp.getOperation(), "<<");
      })
      .Case<emitrust::ShrOp>([&](emitrust::ShrOp shrOp) {
        return emitBinary(shrOp.getOperation(), ">>");
      })
      .Case<emitrust::CmpOp>(
          [&](emitrust::CmpOp cmpOp) { return emitCmp(cmpOp); })
      .Case<emitrust::CastOp>(
          [&](emitrust::CastOp castOp) { return emitCast(castOp); })
      .Case<emitrust::BitcastOp>([&](emitrust::BitcastOp bitcastOp) {
        return emitBitcast(bitcastOp);
      })
      .Case<emitrust::SelectOp>([&](emitrust::SelectOp selectOp) {
        return emitSelect(selectOp);
      })
      .Case<emitrust::IfOp>([&](emitrust::IfOp ifOp) { return emitIf(ifOp); })
      .Case<emitrust::ForOp>(
          [&](emitrust::ForOp forOp) { return emitFor(forOp); })
      .Case<emitrust::LoopOp>(
          [&](emitrust::LoopOp loopOp) { return emitLoop(loopOp); })
      .Case<emitrust::SwitchOp>([&](emitrust::SwitchOp switchOp) {
        return emitSwitch(switchOp);
      })
      .Case<emitrust::BreakOp>([&](emitrust::BreakOp) {
        os << "break;\n";
        return success();
      })
      .Case<emitrust::ContinueOp>([&](emitrust::ContinueOp) {
        os << "continue;\n";
        return success();
      })
      .Case<emitrust::StructDefOp>([&](emitrust::StructDefOp structDefOp) {
        return emitStructDef(structDefOp);
      })
      .Case<emitrust::EnumDefOp>([&](emitrust::EnumDefOp enumDefOp) {
        return emitEnumDef(enumDefOp);
      })
      .Case<emitrust::GlobalOp>([&](emitrust::GlobalOp globalOp) {
        return emitGlobal(globalOp);
      })
      .Case<emitrust::GlobalLoadOp>([&](emitrust::GlobalLoadOp loadOp) {
        return emitGlobalLoad(loadOp);
      })
      .Case<emitrust::GlobalStoreOp>([&](emitrust::GlobalStoreOp storeOp) {
        return emitGlobalStore(storeOp);
      })
      .Case<emitrust::CellGetOp>(
          [&](emitrust::CellGetOp getOp) { return emitCellGet(getOp); })
      .Case<emitrust::CellSetOp>(
          [&](emitrust::CellSetOp setOp) { return emitCellSet(setOp); })
      .Case<emitrust::GlobalCellsOp>([&](emitrust::GlobalCellsOp cellsOp) {
        return emitGlobalCells(cellsOp);
      })
      .Case<emitrust::VariableOp>([&](emitrust::VariableOp variableOp) {
        return emitVariable(variableOp);
      })
      // Place-refining operations emit nothing at their program point; the
      // place expressions they denote are rendered by their consumers.
      .Case<emitrust::MemberOp, emitrust::SubscriptOp, emitrust::DerefOp,
            emitrust::EnumRawOp>([&](auto) { return success(); })
      .Case<emitrust::LoadOp>(
          [&](emitrust::LoadOp loadOp) { return emitLoad(loadOp); })
      .Case<emitrust::AddrOfOp>([&](emitrust::AddrOfOp addrOfOp) {
        return emitAddrOf(addrOfOp);
      })
      .Case<emitrust::SliceOfOp>([&](emitrust::SliceOfOp sliceOfOp) {
        return emitSliceOf(sliceOfOp);
      })
      .Case<emitrust::YieldOp>(
          [&](emitrust::YieldOp) { return success(); })
      .Default([&](Operation *unsupported) -> LogicalResult {
        return unsupported->emitOpError("unable to translate op");
      });
}

//===----------------------------------------------------------------------===//
// Public entry point
//===----------------------------------------------------------------------===//

LogicalResult mlir::emitrust::translateToRust(Operation *op, raw_ostream &os) {
  return translateToRust(op, os, RustEmitOptions());
}

LogicalResult mlir::emitrust::translateToRust(Operation *op, raw_ostream &os,
                                              const RustEmitOptions &options) {
  if (!op)
    return failure();
  RustEmitter emitter(os, options);
  return emitter.emitOperation(*op);
}
