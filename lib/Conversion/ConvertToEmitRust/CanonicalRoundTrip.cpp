//===- CanonicalRoundTrip.cpp - Canonical-form round-trip check -----------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the emitrust-canonical-roundtrip pass. See the TableGen
/// description in Passes.td for the contract.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/CanonicalRoundTrip.h"

#include "EmitRust/EmitRustDialect.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTCANONICALROUNDTRIP
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;

namespace {

/// Prints `op` in MLIR's GENERIC form. The generic form is the canonical
/// serialization: it involves no custom assembly, so a difference between two
/// generic prints is a difference in the IR itself rather than in a printer.
std::string printGeneric(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  OpPrintingFlags flags;
  flags.printGenericOpForm();
  // Locations are deliberately EXCLUDED. They are metadata, not program
  // semantics, and a fresh parse assigns fresh unknown locations to anything
  // the printer elided -- comparing them would report a formatting artifact as
  // a round-trip failure.
  op->print(os, flags);
  return text;
}

/// Reports the first differing line of two generic prints. A whole-module
/// diff is unreadable at corpus scale; the first divergence is what a person
/// actually needs.
void reportFirstDifference(Operation *op, StringRef before, StringRef after) {
  size_t line = 1;
  StringRef b = before, a = after;
  while (!b.empty() || !a.empty()) {
    auto [bLine, bRest] = b.split('\n');
    auto [aLine, aRest] = a.split('\n');
    if (bLine != aLine) {
      op->emitError()
          << "canonical round-trip diverged at generic-form line " << line
          << "\n  before: " << bLine << "\n   after: " << aLine;
      return;
    }
    if (b.empty() && a.empty())
      break;
    b = bRest;
    a = aRest;
    ++line;
  }
  op->emitError() << "canonical round-trip diverged in length only (before "
                  << before.size() << " bytes, after " << after.size()
                  << " bytes)";
}

/// The dialect universe an imported project module can contain. It is
/// enumerated EXPLICITLY rather than copied from the live context because the
/// drivers `loadDialect<>()` straight into their context without going through
/// a registry, so `getDialectRegistry()` is empty there and a fresh context
/// seeded from it parses nothing.
///
/// Enumerating has a second, better property: if a future stage introduces a
/// dialect that is not on this list, the round-trip fails LOUDLY with MLIR's
/// "unregistered dialect" error naming it, instead of silently degrading. The
/// alternative -- `allowUnregisteredDialects(true)` -- would make an unknown
/// op round-trip as opaque generic text and PASS, which is exactly the silent
/// weakening this project's oracles exist to prevent.
static void registerProjectDialects(DialectRegistry &registry) {
  registry.insert<emitrust::EmitRustDialect, arith::ArithDialect,
                  cf::ControlFlowDialect, func::FuncDialect,
                  memref::MemRefDialect, scf::SCFDialect, ub::UBDialect>();
}

/// The emitrust-canonical-roundtrip pass. See Passes.td.
struct EmitRustCanonicalRoundTrip
    : public emitrust::impl::EmitRustCanonicalRoundTripBase<
          EmitRustCanonicalRoundTrip> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registerProjectDialects(registry);
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    std::string before = printGeneric(module);

    // A FRESH context, not the current one. Reusing the live context would
    // let interned types, attributes and strings mask exactly the round-trip
    // failures this pass exists to find.
    //
    // Unregistered dialects stay FORBIDDEN in it: allowing them would let an
    // unknown op survive as opaque generic text and compare equal, turning a
    // real gap into a silent pass.
    DialectRegistry registry;
    registerProjectDialects(registry);
    MLIRContext fresh(registry);
    fresh.loadAllAvailableDialects();

    OwningOpRef<ModuleOp> reparsed =
        parseSourceString<ModuleOp>(before, &fresh);
    if (!reparsed) {
      module.emitError("canonical round-trip failed: the module's own "
                       "generic form does not parse back");
      return signalPassFailure();
    }

    std::string after = printGeneric(*reparsed);
    if (before != after) {
      reportFirstDifference(module, before, after);
      return signalPassFailure();
    }

    // Verification only: nothing was mutated, so nothing is invalidated.
    markAllAnalysesPreserved();
  }
};

} // namespace
