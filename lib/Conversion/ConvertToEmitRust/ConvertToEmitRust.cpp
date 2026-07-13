//===- ConvertToEmitRust.cpp - Composite conversion to EmitRust -----------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the composite convert-to-emitrust pass: it populates the ub,
/// arith, func, and scf pattern sets and applies them under one partial
/// conversion. The EmitRust dialect and `builtin.module` are legal; the
/// arith, cf, func, memref, scf, and ub dialects are illegal so that any
/// leftover operation from those dialects fails the pass loudly with the
/// standard "failed to legalize" diagnostic.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/ConvertToEmitRust.h"

#include "EmitRust/Conversion/ArithToEmitRust.h"
#include "EmitRust/Conversion/EmitRustTypeConverter.h"
#include "EmitRust/Conversion/FuncToEmitRust.h"
#include "EmitRust/Conversion/SCFToEmitRust.h"
#include "EmitRust/Conversion/UBToEmitRust.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_CONVERTTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

namespace {

/// The convert-to-emitrust pass: one partial conversion over all pattern
/// sets, with the source dialects wholesale illegal.
struct ConvertToEmitRust
    : public emitrust::impl::ConvertToEmitRustBase<ConvertToEmitRust> {
  /// Runs the combined partial conversion and fails on any leftover op
  /// from the source dialects.
  void runOnOperation() override {
    MLIRContext &context = getContext();

    ConversionTarget target(context);
    target.addLegalDialect<EmitRustDialect>();
    target.addLegalOp<ModuleOp>();
    target.addIllegalDialect<arith::ArithDialect, cf::ControlFlowDialect,
                             func::FuncDialect, memref::MemRefDialect,
                             scf::SCFDialect, ub::UBDialect>();

    TypeConverter typeConverter;
    populateEmitRustTypeConverter(typeConverter);

    RewritePatternSet patterns(&context);
    populateUBToEmitRustPatterns(typeConverter, patterns);
    populateArithToEmitRustPatterns(typeConverter, patterns);
    populateFuncToEmitRustPatterns(typeConverter, patterns);
    populateSCFToEmitRustPatterns(typeConverter, patterns);

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      signalPassFailure();
  }
};

} // namespace
