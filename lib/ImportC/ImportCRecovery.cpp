//===- ImportCRecovery.cpp - Recoverable import (FR-42) --------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the off-by-default recovery mode of the C importer: the
/// checkpoint/rollback machinery, the diagnostic capture that turns a
/// located rejection into a warning, the `unimplemented!()` signature stub,
/// and the ledger recording.
///
/// Motivation (FR-42). Without recovery, `importDeclsIn` stops at the first
/// unsupported top-level declaration and `importTranslationUnit` /
/// `importCProject` propagate that failure, so ONE construct outside the
/// subset anywhere in a project yields no output at all. That is the single
/// reason a real C++ project transpiles to nothing today. With recovery on,
/// the rejected item is recorded and the walk continues, so what IS in the
/// subset still reaches the emitted crate.
///
/// The correctness crux is that a rejected item must leave NO trace in the
/// module. The strategy chosen here is CHECKPOINT-AND-UNDO on the module
/// body rather than build-into-a-scratch-region-and-splice. Both were
/// considered:
///
///  - Scratch region. Attractive in the abstract ("nothing enters the module
///    until it is complete"), but the importer does not build an item into
///    one region: `importFunction` creates its `func::FuncOp` directly in
///    the module body and then, while importing the body, reaches back into
///    the module through `mapType` -> `importRecord` and through
///    `importGlobalVar` to append struct/enum/global definitions AS SIBLINGS.
///    Redirecting only the function into a scratch module would leave those
///    siblings in the real module anyway (which is what we want) while
///    breaking the `SymbolTable`-based checks that consult
///    `module.getOperation()` mid-import. The splice would have to be undone
///    for exactly the same set of operations the undo below erases, with an
///    extra moving part.
///
///  - Checkpoint and undo (chosen). Record the last module-body operation
///    before the item; on rejection erase the `func::FuncOp`s appended after
///    it. That is sufficient because a function body is the ONLY region the
///    importer fills incrementally — every other module-level operation is
///    created in one `create<...>` call from already-validated data — so a
///    half-built item is always exactly a half-built function.
///
/// What is deliberately NOT undone, and why, is documented on `rollbackTo`
/// in CImporterInternal.h: the type and storage definitions the item pulled
/// in on demand stay, because their name registries have no rollback and an
/// erased definition with a live registry entry would be a dangling symbol
/// instead of a dead one.
///
/// FR-43 rides on exactly this machinery. A frontier-search STATE is a set
/// of admitted items, and probing one means importing the project with the
/// complement EXCLUDED (`ImportOptions::excludedItems`). An excluded item is
/// not a new outcome: it is fed into the rejection path above with a
/// synthetic reason, so it stubs if its signature maps and drops otherwise,
/// exactly like a genuinely unsupported item. That is what keeps the search
/// from needing its own emission model — every state it can propose is a
/// module the recovering importer already knows how to build.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

#include "mlir/IR/Diagnostics.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <utility>

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// Checkpoint / rollback
//===----------------------------------------------------------------------===//

CImporter::RecoveryCheckpoint CImporter::checkpointModule() {
  RecoveryCheckpoint checkpoint;
  Block *body = module.getBody();
  checkpoint.anchor = body->empty() ? nullptr : &body->back();
  return checkpoint;
}

void CImporter::eraseTopLevelOp(Operation *op) {
  // Moving the anchor back one operation is exact: `getPrevNode` is null
  // when `op` was the first operation in the body, which is the same "the
  // body was empty before the item" state a fresh checkpoint records.
  if (activeCheckpoint && activeCheckpoint->anchor == op)
    activeCheckpoint->anchor = op->getPrevNode();
  op->erase();
}

void CImporter::resetPerFunctionState() {
  // Mirrors `importFunction`'s prologue (ImportCFunctions.cpp). Every field
  // it clears is cleared here, and every field it SETS from the incoming
  // function is reset to its default-constructed value, because after a
  // rollback there is no function for it to describe. Keep the two in step:
  // a field left holding a `Value` or `Block *` into an erased body is a
  // use-after-free waiting for the next item that reads it.
  symbols.clear();
  addressTaken.clear();
  fileLocals.clear();
  pointerLocals.clear();
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  inferredFnPtrSigs.clear();
  cursorWritebacks.clear();
  voidFnPtrHolders.clear();
  currentVaCloneActive = false;
  currentVaExtras.clear();
  currentVaCursorCell = Value();
  currentHasLabels = false;
  currentFunctionBody = nullptr;
  currentReceiverPlace = Value();
  currentPoolPlace = Value();
  currentPoolCursorCell = Value();
  currentMethodOwner = nullptr;
  currentOwnerIndexReturn = false;
  currentCxxThisRef = Value();
  currentReturnType = Type();
  currentErasedReturnBase = nullptr;
  currentFuncName.clear();
  currentIsMain = false;
  bodyRegion = nullptr;
  entryBlock = nullptr;
}

void CImporter::rollbackTo(const RecoveryCheckpoint &checkpoint) {
  Block *body = module.getBody();
  // Collect first, erase after: erasing while iterating the block would
  // invalidate the iterator, and the erase order among the collected
  // operations is irrelevant because none of them can reference another
  // (nothing has been emitted yet that calls into the failed item, and the
  // item's own recursive self-call lives inside the body being erased).
  SmallVector<func::FuncOp> appended;
  Operation *first =
      checkpoint.anchor ? checkpoint.anchor->getNextNode()
                        : (body->empty() ? nullptr : &body->front());
  for (Operation *op = first; op; op = op->getNextNode())
    if (auto funcOp = llvm::dyn_cast<func::FuncOp>(op))
      appended.push_back(funcOp);
  for (func::FuncOp funcOp : appended) {
    // Drop the symbol map entry with the operation, never one without the
    // other: that pairing is what keeps `functions` and the module mutually
    // consistent, so a later item that calls the dropped function takes the
    // ordinary "call to unimported function" rejection (and is recovered in
    // turn) instead of emitting a call to a symbol that is not there.
    if (functions.lookup(funcOp.getName()) == funcOp)
      functions.erase(funcOp.getName());
    funcOp.erase();
  }
  // Per-function scratch state points into the bodies just erased.
  resetPerFunctionState();
  // Re-insert the prototypes the item's redeclaration reconciliation
  // erased. Position within the module body is irrelevant (it is a symbol
  // table, and the Rust emitter orders items by their own rules), so they
  // are appended; the clone was taken before the erase, so the signature
  // and visibility are exactly what they were.
  for (Operation *clone : checkpoint.erasedExternalClones) {
    body->push_back(clone);
    if (auto funcOp = llvm::dyn_cast<func::FuncOp>(clone))
      functions[funcOp.getName()] = funcOp;
  }
}

//===----------------------------------------------------------------------===//
// The recovery stub
//===----------------------------------------------------------------------===//

LogicalResult CImporter::emitRecoveryStub(func::FuncOp funcOp, Location loc) {
  // `importFunction` has already positioned the builder at the end of the
  // module body under its own insertion guard, so the guard here is only
  // about not leaking the entry-block position out of this helper.
  OpBuilder::InsertionGuard guard(builder);
  Block *entry = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entry);
  // `unimplemented!("<reason>")` has type `!`, so ONE call op can carry the
  // function's result types whatever they are; the emitter renders it as
  // `let vN: T = unimplemented!("...");` and the `return vN;` below coerces
  // from `!`. A void function gets the bare `unimplemented!("...");` and an
  // operand-less return. The reason is passed through the `args` attribute
  // rather than as an operand because Rust macros need a literal there.
  TypeRange resultTypes = funcOp.getFunctionType().getResults();
  auto call = builder.create<emitrust::CallOpaqueOp>(
      loc, resultTypes, builder.getStringAttr("unimplemented!"),
      builder.getArrayAttr({builder.getStringAttr(recoveryStubReason)}),
      ValueRange());
  builder.create<func::ReturnOp>(loc, call.getResults());
  return success();
}

//===----------------------------------------------------------------------===//
// The recovering dispatch
//===----------------------------------------------------------------------===//

namespace {

/// One diagnostic intercepted while a recovered item was importing. Only the
/// location and the rendered text are kept: the `Diagnostic` itself owns
/// arguments that do not outlive the handler callback.
struct CapturedDiagnostic {
  Location loc;
  DiagnosticSeverity severity;
  std::string message;
};

/// The best available name for a rejected declaration. Records and functions
/// are `NamedDecl`s; an unnamed record (or an anonymous namespace's unnamed
/// member) has no spelling at all, which the ledger reports literally rather
/// than inventing a name the user never wrote.
std::string declLedgerName(const clang::Decl *decl) {
  if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(decl)) {
    std::string name = named->getNameAsString();
    if (!name.empty())
      return name;
  }
  return "<anonymous>";
}

/// Re-emits a captured diagnostic at its original severity. Used only on the
/// (never-observed) path where an import emitted an error and still reported
/// success: swallowing it there would be a silent behavior change.
void replayDiagnostic(const CapturedDiagnostic &diag) {
  switch (diag.severity) {
  case DiagnosticSeverity::Error:
    emitError(diag.loc) << diag.message;
    return;
  case DiagnosticSeverity::Warning:
    emitWarning(diag.loc) << diag.message;
    return;
  case DiagnosticSeverity::Note:
  case DiagnosticSeverity::Remark:
    emitRemark(diag.loc) << diag.message;
    return;
  }
}

/// Destroys clones a checkpoint captured but never re-inserted (the item
/// succeeded, so the prototypes they mirror are legitimately gone and the
/// detached clones are the only thing left owning that IR).
void discardClones(SmallVectorImpl<Operation *> &clones) {
  for (Operation *clone : clones)
    clone->destroy();
  clones.clear();
}

} // namespace

//===----------------------------------------------------------------------===//
// The FR-43 admitted-set filter
//===----------------------------------------------------------------------===//

std::string CImporter::frontierExcludedSymbol(const clang::Decl *decl) const {
  if (!excludedItems || excludedItems->empty())
    return {};
  // The key derivation MIRRORS `ItemGraphBuilder::collectItems` decision for
  // decision, including its skips: a declaration the graph does not turn into
  // a node has no key, so it cannot be named by a search state and is left
  // alone here. Keeping the two in the same shape (same order of kinds, same
  // guards) is what makes "the graph's vocabulary" a real invariant rather
  // than a coincidence.
  std::string symbol;
  if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    // C++ member functions are not graph items (their emitted name depends on
    // the owning class's assigned struct name), so they are never excludable;
    // dropping the CLASS is how a search state removes them.
    if (llvm::isa<clang::CXXMethodDecl>(func))
      return {};
    symbol = cFunctionSymbolName(func, currentTuTag);
  } else if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
    const clang::RecordDecl *definition = record->getDefinition();
    if (!definition ||
        !definition->getDeclContext()->getRedeclContext()->isFileContext())
      return {};
    symbol = recordRustName(definition).str();
  } else if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
    const clang::EnumDecl *definition = enumDecl->getDefinition();
    if (!definition)
      return {};
    symbol = definition->getName().str();
  } else if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
    symbol = cGlobalSymbolName(var, currentTuTag);
  } else {
    return {};
  }
  if (symbol.empty() || !excludedItems->count(symbol))
    return {};
  return symbol;
}

LogicalResult
CImporter::importTopLevelDeclRecovering(const clang::Decl *decl) {
  // The insertion point is importer-wide state that a failed body import may
  // have left inside the region about to be erased. `importFunction` guards
  // it too, but guarding here as well makes the property hold for every
  // dispatch kind without depending on each one's internals.
  OpBuilder::InsertionGuard guard(builder);

  RecoveryCheckpoint checkpoint = checkpointModule();
  RecoveryCheckpoint *previousCheckpoint = activeCheckpoint;
  activeCheckpoint = &checkpoint;

  // Diagnostic capture. The importer reports rejections through the context's
  // diagnostic engine at the point they are detected, deep inside the import;
  // there is no return channel carrying the text back out. A scoped handler
  // that consumes errors is therefore how the verbatim message and its
  // location reach the ledger, and it doubles as the mechanism that keeps a
  // RECOVERED rejection from printing as an error. Warnings/notes/remarks are
  // passed through (`failure()` here means "not handled, try the next
  // handler") so nothing else about diagnostics changes.
  SmallVector<CapturedDiagnostic> captured;
  auto capture = [&captured](Diagnostic &diag) -> LogicalResult {
    if (diag.getSeverity() != DiagnosticSeverity::Error)
      return failure();
    captured.push_back({diag.getLocation(), diag.getSeverity(), diag.str()});
    for (Diagnostic &note : diag.getNotes())
      captured.push_back(
          {note.getLocation(), DiagnosticSeverity::Note, note.str()});
    return success();
  };

  LogicalResult result = success();
  // FR-43: an item the search state does not admit is not imported at all.
  // It is turned into a rejection with a synthetic reason and then falls
  // through the ordinary path below, so it stubs when its signature maps and
  // drops otherwise — the same two outcomes an unsupported item has, and the
  // same ledger entry shape, which is what lets FR-44's report describe a
  // searched build without knowing a search happened.
  std::string excluded = frontierExcludedSymbol(decl);
  if (!excluded.empty()) {
    result = failure();
    captured.push_back({translateLoc(decl->getLocation()),
                        DiagnosticSeverity::Error,
                        "excluded by the search state: item '" + excluded +
                            "' is not admitted"});
  } else {
    ScopedDiagnosticHandler handler(builder.getContext(), capture);
    result = importTopLevelDecl(decl);
  }
  activeCheckpoint = previousCheckpoint;

  if (succeeded(result)) {
    for (const CapturedDiagnostic &diag : captured)
      replayDiagnostic(diag);
    discardClones(checkpoint.erasedExternalClones);
    return success();
  }

  // `rollbackTo` re-inserts every captured clone into the module body and
  // takes ownership of it, so nothing is leaked on this path.
  rollbackTo(checkpoint);

  // The first captured error is the rejection: later ones are consequences
  // reported on the way out. A rejection with no located diagnostic at all
  // cannot happen today (every `emitError` in the importer carries a
  // location), but the fallback keeps the ledger total.
  Location loc = captured.empty() ? translateLoc(decl->getBeginLoc())
                                  : captured.front().loc;
  std::string reason = captured.empty()
                           ? std::string("unsupported declaration")
                           : captured.front().message;

  // Stub attempt: only a function can be stubbed, and only if its SIGNATURE
  // maps. The retry re-enters `importFunction` with `recoveryStubOnly`, which
  // builds the signature exactly as the real import does and then emits the
  // `unimplemented!()` body instead of importing the real one. A signature
  // that does not map fails the retry, and the item is dropped — deliberately
  // without a fallback approximation, because a stub whose signature differed
  // from the real one would mis-compile every caller instead of rejecting it.
  bool stubbed = false;
  std::string symbol = declLedgerName(decl);
  if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    RecoveryCheckpoint stubCheckpoint = checkpointModule();
    activeCheckpoint = &stubCheckpoint;
    recoveryStubOnly = true;
    recoveryStubReason = reason;
    recoveryStubSymbol.clear();
    LogicalResult stub = failure();
    {
      // The retry's own diagnostics are discarded: it is a best-effort
      // second attempt at an item that has ALREADY been reported, and its
      // rejections are a strictly less informative restatement of the one
      // the ledger records.
      ScopedDiagnosticHandler silence(
          builder.getContext(), [](Diagnostic &) { return success(); });
      stub = importFunction(func);
    }
    recoveryStubOnly = false;
    recoveryStubReason.clear();
    if (succeeded(stub) && !recoveryStubSymbol.empty()) {
      stubbed = true;
      symbol = recoveryStubSymbol;
      discardClones(stubCheckpoint.erasedExternalClones);
    } else {
      rollbackTo(stubCheckpoint);
    }
    activeCheckpoint = previousCheckpoint;
  }

  if (rejectionLedger)
    rejectionLedger->record(RejectedItem{symbol, loc, reason,
                                         classifyBlocker(reason, loc),
                                         stubbed});
  // The rejection is re-reported as a WARNING: the item is gone from the
  // module, but the compile as a whole succeeded, and a driver that exits 0
  // with a partial module must not have printed an error on the way.
  InFlightDiagnostic warning =
      emitWarning(loc) << reason
                       << (stubbed ? " (recovered: emitted an unimplemented!() "
                                     "stub with the mapped signature)"
                                   : " (recovered: item dropped)");
  // Consequential diagnostics stay attached as notes so the cause chain the
  // non-recovering import would have shown is not lost.
  for (const CapturedDiagnostic &diag : llvm::drop_begin(captured))
    warning.attachNote(diag.loc) << diag.message;
  return success();
}
