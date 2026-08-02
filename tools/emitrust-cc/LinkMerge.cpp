//===- LinkMerge.cpp - FR-58 link-step shard merging ----------------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implementation of the FR-58 slice-1 link merge declared in LinkMerge.h.
/// See that header for the algorithm; the comments here cover only the
/// mechanics that are not obvious from it.
//
//===----------------------------------------------------------------------===//

#include "LinkMerge.h"

#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Payload extraction
//===----------------------------------------------------------------------===//

FailureOr<std::optional<llvm::StringRef>>
emitrustcc::findShardPayload(const llvm::MemoryBuffer &buffer,
                             std::string &errorMessage) {
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> object =
      llvm::object::ObjectFile::createObjectFile(buffer.getMemBufferRef());
  if (!object) {
    // Not an object file: the input IS the shard (bytecode or textual
    // MLIR); the parser will diagnose anything else.
    llvm::consumeError(object.takeError());
    return std::optional<llvm::StringRef>(buffer.getBuffer());
  }
  for (const llvm::object::SectionRef &section : (*object)->sections()) {
    llvm::Expected<llvm::StringRef> name = section.getName();
    if (!name) {
      llvm::consumeError(name.takeError());
      continue;
    }
    if (*name != ".emitrust")
      continue;
    llvm::Expected<llvm::StringRef> contents = section.getContents();
    if (!contents) {
      errorMessage = llvm::toString(contents.takeError());
      return failure();
    }
    return std::optional<llvm::StringRef>(*contents);
  }
  // An object with no `.emitrust` section: sidecar fallback.
  return std::optional<llvm::StringRef>();
}

//===----------------------------------------------------------------------===//
// Merge
//===----------------------------------------------------------------------===//

namespace {

/// A parsed `tu<N>_`/`TU<N>_` per-TU tag at the front of a symbol name.
struct TuTag {
  /// The ordinal `N` the shard's import assigned (0 for a solo import).
  unsigned ordinal;
  /// Whether the tag is the SCREAMING `TU<N>_` global spelling.
  bool upper;
  /// The name after the tag, e.g. `scale` of `tu0_scale`.
  llvm::StringRef rest;
};

/// Parses the per-TU tag at the front of `name`, accepting exactly the two
/// spellings `CSymbolNaming.h` produces and `CSymbolLinkage.h` recognizes:
/// lowercase `tu<N>_` on a snake_case function and uppercase `TU<N>_` on a
/// SCREAMING_SNAKE_CASE global. Shares that predicate's stated imprecision:
/// a C identifier literally spelled `tu0_x` is indistinguishable from a
/// tagged one and is treated as tagged.
static std::optional<TuTag> parseTuTag(llvm::StringRef name) {
  bool upper;
  if (name.consume_front("tu"))
    upper = false;
  else if (name.consume_front("TU"))
    upper = true;
  else
    return std::nullopt;
  size_t digits = 0;
  while (digits < name.size() && llvm::isDigit(name[digits]))
    ++digits;
  if (digits == 0 || digits >= name.size() || name[digits] != '_')
    return std::nullopt;
  unsigned ordinal = 0;
  if (name.take_front(digits).getAsInteger(10, ordinal))
    return std::nullopt;
  return TuTag{ordinal, upper, name.drop_front(digits + 1)};
}

/// Prints `op` to a string with the default flags (no locations), the
/// slice-1 shape-equality oracle for the dedup in step 3 and the textual
/// identity for the use/verbatim dedup in step 4.
static std::string printOpToString(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}

/// Step 1: alpha-renames shard-local `tu0_`/`TU0_` tags on `shard`'s
/// module-level symbols to `ordinal`, rewriting symbol uses (globals are
/// referenced by `FlatSymbolRefAttr`) and `emitrust.call_opaque` callees
/// (which reference functions by plain string, not by symbol use, so
/// `SymbolTable::replaceAllSymbolUses` cannot see them).
///
/// Collisions cannot arise from the rename itself: every renamed name
/// carries this shard's unique ordinal, and a tag with any OTHER ordinal —
/// which a solo import can never have assigned — is rejected here rather
/// than renamed into a potential clash.
static LogicalResult renameShardTags(ModuleOp shard, unsigned ordinal) {
  llvm::StringMap<std::string> renames;
  llvm::SmallVector<Operation *> renamedOps;
  for (Operation &op : shard.getBody()->getOperations()) {
    auto symbol = dyn_cast<SymbolOpInterface>(&op);
    if (!symbol)
      continue;
    llvm::StringRef name = symbol.getName();
    std::optional<TuTag> tag = parseTuTag(name);
    if (!tag)
      continue;
    if (tag->ordinal != 0)
      return op.emitError()
             << "shard symbol '" << name << "' carries per-TU tag ordinal "
             << tag->ordinal
             << "; a solo-imported shard may only carry 'tu0_'";
    if (ordinal == 0)
      continue; // tu0_ -> tu0_ is a no-op.
    renames[name] = ((tag->upper ? "TU" : "tu") + llvm::Twine(ordinal) + "_" +
                     tag->rest)
                        .str();
    renamedOps.push_back(&op);
  }
  if (renames.empty())
    return success();
  for (Operation *op : renamedOps) {
    auto symbol = cast<SymbolOpInterface>(op);
    StringAttr newName =
        StringAttr::get(shard.getContext(), renames[symbol.getName()]);
    if (failed(SymbolTable::replaceAllSymbolUses(op, newName, shard)))
      return op->emitError()
             << "cannot rewrite uses of shard symbol '" << symbol.getName()
             << "' at link";
    SymbolTable::setSymbolName(op, newName);
  }
  shard.walk([&](emitrust::CallOpaqueOp call) {
    auto it = renames.find(call.getCallee());
    if (it != renames.end())
      call.setCallee(it->second);
  });
  return success();
}

} // namespace

FailureOr<OwningOpRef<ModuleOp>> emitrustcc::mergeLinkShards(
    llvm::MutableArrayRef<OwningOpRef<ModuleOp>> shards) {
  // Step 1: per-shard alpha-rename to global ordinals.
  for (auto [index, shard] : llvm::enumerate(shards))
    if (failed(renameShardTags(*shard, static_cast<unsigned>(index))))
      return failure();

  // One pass over every module-level op of every shard, in link-line order,
  // classifying: definitions (for steps 2 and 3), marked declarations
  // (step 2's obligations), and use/verbatim header ops (step 4).
  llvm::StringMap<Operation *> definitions;
  llvm::SmallVector<std::pair<llvm::StringRef, Operation *>> obligations;
  llvm::StringSet<> headerTexts;
  llvm::SmallVector<Operation *> toErase;
  for (OwningOpRef<ModuleOp> &shard : shards) {
    llvm::StringSet<> shardHeaderTexts;
    for (Operation &op : shard->getBody()->getOperations()) {
      if (isa<emitrust::UseOp, emitrust::VerbatimOp>(op)) {
        // Step 4: a header op an EARLIER shard already carries is dropped;
        // repeats within one shard are the import's own doing and are kept.
        std::string text = printOpToString(&op);
        if (headerTexts.contains(text))
          toErase.push_back(&op);
        else
          shardHeaderTexts.insert(text);
        continue;
      }
      auto symbol = dyn_cast<SymbolOpInterface>(&op);
      if (!symbol)
        continue;
      if (op.hasAttr(emitrust::kExternDeclAttrName)) {
        obligations.push_back({symbol.getName(), &op});
        continue;
      }
      auto [it, inserted] = definitions.try_emplace(symbol.getName(), &op);
      if (inserted)
        continue;
      Operation *first = it->second;
      // Step 3: struct/enum/global definitions repeat in every shard that
      // uses them (shape-dedup'd per TU by the import); first occurrence
      // wins when the shapes agree.
      bool dedupable =
          isa<emitrust::StructDefOp, emitrust::EnumDefOp, emitrust::GlobalOp>(
              op) &&
          first->getName() == op.getName();
      if (dedupable && printOpToString(first) == printOpToString(&op)) {
        toErase.push_back(&op);
        continue;
      }
      InFlightDiagnostic diag =
          dedupable ? op.emitError()
                          << "conflicting definitions of '" << symbol.getName()
                          << "' at link: the shards disagree on its shape"
                    : op.emitError() << "duplicate definition of '"
                                     << symbol.getName() << "' at link";
      diag.attachNote(first->getLoc()) << "first defined here";
      return failure();
    }
    for (const auto &entry : shardHeaderTexts)
      headerTexts.insert(entry.getKey());
  }

  // Step 2: declaration-for-definition replacement; an obligation nobody
  // defines is THE undefined-symbol link error.
  for (auto [name, op] : obligations) {
    if (definitions.contains(name)) {
      toErase.push_back(op);
      continue;
    }
    return op->emitError() << "unresolved external '" << name << "' at link";
  }

  for (Operation *op : toErase)
    op->erase();

  // Step 5: concatenate the survivors in shard order into the first shard's
  // module, then verify the whole.
  ModuleOp merged = *shards.front();
  for (OwningOpRef<ModuleOp> &shard : shards.drop_front())
    merged.getBody()->getOperations().splice(
        merged.getBody()->end(), shard->getBody()->getOperations());
  if (failed(verify(merged)))
    return failure();
  return std::move(shards.front());
}
