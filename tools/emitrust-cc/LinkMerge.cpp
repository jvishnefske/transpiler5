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
#include "EmitRust/ShardMetadata.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Payload extraction
//===----------------------------------------------------------------------===//

/// The buffer-ref core of `findShardPayload`, shared with the archive
/// expansion (an archive member is a MemoryBufferRef into the archive, not
/// a MemoryBuffer of its own). Same three-way contract as the public
/// wrapper.
static FailureOr<std::optional<llvm::StringRef>>
findShardPayloadRef(llvm::MemoryBufferRef buffer, std::string &errorMessage) {
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> object =
      llvm::object::ObjectFile::createObjectFile(buffer);
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

FailureOr<std::optional<llvm::StringRef>>
emitrustcc::findShardPayload(const llvm::MemoryBuffer &buffer,
                             std::string &errorMessage) {
  return findShardPayloadRef(buffer.getMemBufferRef(), errorMessage);
}

bool emitrustcc::isStaticArchive(const llvm::MemoryBuffer &buffer) {
  return llvm::identify_magic(buffer.getBuffer()) ==
         llvm::file_magic::archive;
}

FailureOr<emitrustcc::ArchivePayloads>
emitrustcc::findArchivePayloads(const llvm::MemoryBuffer &buffer,
                                std::string &errorMessage) {
  llvm::Expected<std::unique_ptr<llvm::object::Archive>> archive =
      llvm::object::Archive::create(buffer.getMemBufferRef());
  if (!archive) {
    errorMessage = llvm::toString(archive.takeError());
    return failure();
  }
  ArchivePayloads result;
  // `children` skips the symbol-table and string-table pseudo-members and
  // reports iteration corruption through `err`, which must be checked even
  // on the early-return paths.
  llvm::Error err = llvm::Error::success();
  for (const llvm::object::Archive::Child &child : (*archive)->children(err)) {
    llvm::Expected<llvm::StringRef> name = child.getName();
    if (!name) {
      errorMessage = llvm::toString(name.takeError());
      llvm::consumeError(std::move(err));
      return failure();
    }
    llvm::Expected<llvm::MemoryBufferRef> member = child.getMemoryBufferRef();
    if (!member) {
      errorMessage = ("member '" + *name +
                      "': " + llvm::toString(member.takeError()))
                         .str();
      llvm::consumeError(std::move(err));
      return failure();
    }
    FailureOr<std::optional<llvm::StringRef>> payload =
        findShardPayloadRef(*member, errorMessage);
    if (failed(payload)) {
      errorMessage = ("member '" + *name + "': " + errorMessage).str();
      llvm::consumeError(std::move(err));
      return failure();
    }
    if (*payload)
      result.payloads.push_back({name->str(), **payload});
    else
      result.skippedMembers.push_back(name->str());
  }
  if (err) {
    errorMessage = llvm::toString(std::move(err));
    return failure();
  }
  return result;
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

/// Prints `op` to a string with the default flags (no locations): the
/// textual identity for the use/verbatim dedup in step 4. (It was also
/// slice 1's shape-equality oracle for step 3, replaced by structural
/// `OperationEquivalence` in slice 2; when debugging a surprising step-3
/// verdict, comparing the two ops' `printOpToString` output is still the
/// quickest way to see WHERE they diverge.)
static std::string printOpToString(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}

/// Step 1: alpha-renames the per-TU tags on `shard`'s module-level symbols
/// per `ordinalMap` (internal ordinal k renames to global ordinal
/// `ordinalMap[k]`), rewriting symbol uses (globals are referenced by
/// `FlatSymbolRefAttr`) and `emitrust.call_opaque` callees (which
/// reference functions by plain string, not by symbol use, so
/// `SymbolTable::replaceAllSymbolUses` cannot see them). A solo shard's
/// map is the single element [ordinal]; a re-imported group's map lists
/// its members' link-line positions.
///
/// Collision freedom: the map is required strictly increasing, so every
/// target ordinal is >= its source (a TU's link-line position counts at
/// least its group-internal predecessors), and the renames are applied in
/// DESCENDING source-ordinal order — by the time source k renames to
/// map[k], any source tag with a higher ordinal (which map[k] might equal)
/// has already been renamed away. Callee strings are rewritten from a map
/// keyed by ORIGINAL names after all symbol renames, so they cannot be
/// captured by a name that became someone else's target in between.
static LogicalResult renameShardTags(ModuleOp shard,
                                     llvm::ArrayRef<unsigned> ordinalMap) {
  for (size_t k = 1; k < ordinalMap.size(); ++k)
    if (ordinalMap[k] <= ordinalMap[k - 1])
      return shard.emitError()
             << "link-shard ordinal map is not strictly increasing";

  struct Rename {
    Operation *op;
    unsigned sourceOrdinal;
    std::string newName;
  };
  llvm::SmallVector<Rename> renames;
  llvm::StringMap<std::string> calleeRenames;
  for (Operation &op : shard.getBody()->getOperations()) {
    auto symbol = dyn_cast<SymbolOpInterface>(&op);
    if (!symbol)
      continue;
    llvm::StringRef name = symbol.getName();
    std::optional<TuTag> tag = parseTuTag(name);
    if (!tag)
      continue;
    if (tag->ordinal >= ordinalMap.size())
      return op.emitError()
             << "shard symbol '" << name << "' carries per-TU tag ordinal "
             << tag->ordinal << "; this shard covers only "
             << ordinalMap.size() << " translation unit(s)";
    unsigned target = ordinalMap[tag->ordinal];
    if (target == tag->ordinal)
      continue; // tu<k>_ -> tu<k>_ is a no-op.
    std::string newName = ((tag->upper ? "TU" : "tu") + llvm::Twine(target) +
                           "_" + tag->rest)
                              .str();
    calleeRenames[name] = newName;
    renames.push_back({&op, tag->ordinal, std::move(newName)});
  }
  if (renames.empty())
    return success();
  llvm::stable_sort(renames, [](const Rename &a, const Rename &b) {
    return a.sourceOrdinal > b.sourceOrdinal;
  });
  for (const Rename &rename : renames) {
    auto symbol = cast<SymbolOpInterface>(rename.op);
    StringAttr newName = StringAttr::get(shard.getContext(), rename.newName);
    if (failed(SymbolTable::replaceAllSymbolUses(rename.op, newName, shard)))
      return rename.op->emitError()
             << "cannot rewrite uses of shard symbol '" << symbol.getName()
             << "' at link";
    SymbolTable::setSymbolName(rename.op, newName);
  }
  shard.walk([&](emitrust::CallOpaqueOp call) {
    auto it = calleeRenames.find(call.getCallee());
    if (it != calleeRenames.end())
      call.setCallee(it->second);
  });
  return success();
}

} // namespace

FailureOr<OwningOpRef<ModuleOp>> emitrustcc::mergeLinkShards(
    llvm::MutableArrayRef<OwningOpRef<ModuleOp>> shards,
    llvm::ArrayRef<llvm::SmallVector<unsigned>> ordinalMaps) {
  if (!ordinalMaps.empty() && ordinalMaps.size() != shards.size()) {
    if (!shards.empty())
      (*shards.front())->emitError()
          << "link-shard ordinal maps do not match the shard count";
    return failure();
  }

  // Step 0: strip the FR-57 shard metadata (item-graph text, rejection
  // ledger, source facts) from every shard. It is a per-TU fact the caller
  // has already surfaced; the merged whole-program module must stay
  // byte-comparable to the joint import's, which never carries it.
  for (OwningOpRef<ModuleOp> &shard : shards)
    emitrust::stripShardMetadata(*shard);

  // Step 1: per-shard alpha-rename to global ordinals (the positional
  // default when the caller supplied no maps).
  for (auto [index, shard] : llvm::enumerate(shards)) {
    unsigned positional[1] = {static_cast<unsigned>(index)};
    llvm::ArrayRef<unsigned> map =
        ordinalMaps.empty() ? llvm::ArrayRef<unsigned>(positional)
                            : llvm::ArrayRef<unsigned>(ordinalMaps[index]);
    if (failed(renameShardTags(*shard, map)))
      return failure();
  }

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
      // wins when the shapes agree. Shape equality is structural
      // OperationEquivalence: these module-level defs carry no operands
      // (exactValueMatch is vacuous) and no regions, so equivalence is
      // exactly attributes + types with locations ignored — identical
      // layout under different field names is a CONFLICT, because field
      // names are attributes.
      bool dedupable =
          isa<emitrust::StructDefOp, emitrust::EnumDefOp, emitrust::GlobalOp>(
              op) &&
          first->getName() == op.getName();
      if (dedupable &&
          OperationEquivalence::isEquivalentTo(
              first, &op, OperationEquivalence::exactValueMatch,
              /*markEquivalent=*/nullptr,
              OperationEquivalence::Flags::IgnoreLocations)) {
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

//===----------------------------------------------------------------------===//
// FR-58 selective re-import: fact-starvation detection
//===----------------------------------------------------------------------===//

/// The two MEASURED fact-starvation wordings (design.md FR-58, SPIKE 2 and
/// the re-import spike): both are produced by the importer's pointer
/// classification exactly when the target object's shape was out of the
/// solo import's reach. They are matched as substrings of the ledgered
/// diagnostic, the same posture `classifyBlocker`'s tables take toward
/// importer wordings.
static constexpr llvm::StringLiteral kPointerGlobalDiag =
    "unsupported: pointer-typed global variable";
static constexpr llvm::StringLiteral kNoTargetObjectDiag =
    "has no known target object";

bool emitrustcc::isFactStarvedDiagnostic(llvm::StringRef diagnostic) {
  return diagnostic.contains(kPointerGlobalDiag) ||
         diagnostic.contains(kNoTargetObjectDiag);
}

llvm::SmallVector<std::string>
emitrustcc::factStarvedObjectNames(llvm::StringRef symbol,
                                   llvm::StringRef diagnostic) {
  llvm::SmallVector<std::string> names;
  if (diagnostic.contains(kPointerGlobalDiag))
    // The rejected item IS the pointer global; its own C spelling names
    // the object whose shape is missing.
    names.push_back(symbol.str());
  if (diagnostic.contains(kNoTargetObjectDiag)) {
    // "pointer variable 'X' has no known target object": X is quoted.
    size_t open = diagnostic.find('\'');
    if (open != llvm::StringRef::npos) {
      size_t close = diagnostic.find('\'', open + 1);
      if (close != llvm::StringRef::npos && close > open + 1)
        names.push_back(diagnostic.slice(open + 1, close).str());
    }
  }
  return names;
}

bool emitrustcc::itemGraphDefinesGlobal(llvm::StringRef graphText,
                                        llvm::StringRef symbol) {
  // One graph line per item, `node <symbol> kind=global def=1 ...`; every
  // field is a whole space-separated token (the format's stated grep
  // contract), so a prefix match on the three leading tokens is exact.
  std::string needle = ("node " + symbol + " kind=global def=1 ").str();
  for (llvm::StringRef line : llvm::split(graphText, '\n'))
    if (line.starts_with(needle))
      return true;
  return false;
}
