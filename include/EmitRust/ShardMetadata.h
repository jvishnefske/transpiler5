//===- ShardMetadata.h - FR-57 per-TU artifact metadata ---------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-57: the per-TU artifact carries more than the converted module — the
/// FR-58 link step also needs the TU's ITEM-GRAPH SHARD (the FR-40 graph of
/// this one translation unit, whose node keys are the same emitted symbols
/// the module uses) and the TU's REJECTION-LEDGER ENTRIES (which items the
/// recovering import stubbed or dropped, and why — the facts "selective
/// re-import of fact-starved items" is selected by). Both are encoded as
/// attributes ON the same bytecode module rather than as a second artifact,
/// so one payload rides the `.emitrust` section, one `ar`/`ld` collection
/// carries everything, and MLIR's bytecode round-trip guarantees apply to
/// the metadata for free (the round-trip is pinned byte-identical in
/// test/Driver/emitrust-clang-shim.c):
///
///  - `emitrust.item_graph`: a StringAttr holding `ItemGraph::print()`'s
///    stable line format verbatim. The TEXT form is deliberate: the format
///    is already a pinned public surface (`--emit=item-graph` goldens), it
///    is deterministic byte-for-byte, and the link step's consumers (graph
///    merge, admission) parse lines, not attribute trees.
///  - `emitrust.rejections`: an ArrayAttr of DictionaryAttr, one per
///    recovered rejection in declaration-walk order, fields mirroring
///    `RejectedItem` exactly: `symbol`, `diagnostic`, `tag`, `owner`
///    (StringAttr), `stubbed` (BoolAttr), and `loc` (the LocationAttr the
///    importer's diagnostic carried, kept as a first-class location so a
///    link-time report can cite the C construct). Omitted entirely when the
///    import rejected nothing, so a clean TU's artifact is unchanged.
///
/// The attributes are PER-SHARD facts, not program facts: `mergeLinkShards`
/// strips them from every shard before splicing, so the merged whole-program
/// module carries none of them and stays byte-comparable to the joint
/// import's.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_SHARDMETADATA_H
#define EMITRUST_SHARDMETADATA_H

#include "EmitRust/ImportC.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <optional>

namespace mlir {
namespace emitrust {

/// The module attribute holding the TU's item-graph shard text.
inline constexpr llvm::StringLiteral kItemGraphAttrName = "emitrust.item_graph";

/// The module attribute holding the TU's rejection-ledger entries.
inline constexpr llvm::StringLiteral kRejectionsAttrName =
    "emitrust.rejections";

/// Attaches the FR-57 shard metadata to `module`: the item-graph text
/// always, the rejection array only when nonempty (a clean TU's artifact
/// module is byte-for-byte what it was without metadata support).
inline void attachShardMetadata(ModuleOp module, llvm::StringRef itemGraphText,
                                llvm::ArrayRef<RejectedItem> rejections) {
  MLIRContext *context = module.getContext();
  module->setAttr(kItemGraphAttrName,
                  StringAttr::get(context, itemGraphText));
  if (rejections.empty())
    return;
  llvm::SmallVector<Attribute> entries;
  entries.reserve(rejections.size());
  for (const RejectedItem &item : rejections) {
    LocationAttr locAttr = item.loc;
    NamedAttribute fields[] = {
        {StringAttr::get(context, "symbol"),
         StringAttr::get(context, item.symbol)},
        {StringAttr::get(context, "diagnostic"),
         StringAttr::get(context, item.diagnostic)},
        {StringAttr::get(context, "tag"),
         StringAttr::get(context, item.blockerTag)},
        {StringAttr::get(context, "owner"),
         StringAttr::get(context, item.ownerSymbol)},
        {StringAttr::get(context, "stubbed"),
         BoolAttr::get(context, item.stubbed)},
        {StringAttr::get(context, "loc"), locAttr},
    };
    entries.push_back(DictionaryAttr::get(context, fields));
  }
  module->setAttr(kRejectionsAttrName, ArrayAttr::get(context, entries));
}

/// The shard's item-graph text, or `std::nullopt` when the module carries
/// no such attribute (an artifact from before metadata support). A PRESENT
/// but EMPTY text is meaningful and distinct from absence: it is the
/// artifact of a TU that deliberately contributes no items (FR-56's
/// whole-TU target/ABI rejection). The returned StringRef points into the
/// MLIRContext and outlives the module.
inline std::optional<llvm::StringRef> getShardItemGraph(ModuleOp module) {
  if (auto attr = module->getAttrOfType<StringAttr>(kItemGraphAttrName))
    return attr.getValue();
  return std::nullopt;
}

/// Decodes the shard's rejection-ledger entries back into `RejectedItem`s,
/// in the recorded (declaration-walk) order; empty when the module carries
/// none. A field a dictionary lacks decodes to its empty/default value and
/// a missing location to `UnknownLoc`, so an artifact written by a newer or
/// older shim degrades to less detail rather than failing the link.
inline llvm::SmallVector<RejectedItem> getShardRejections(ModuleOp module) {
  llvm::SmallVector<RejectedItem> items;
  auto array = module->getAttrOfType<ArrayAttr>(kRejectionsAttrName);
  if (!array)
    return items;
  MLIRContext *context = module.getContext();
  for (Attribute entry : array) {
    auto dict = dyn_cast<DictionaryAttr>(entry);
    if (!dict)
      continue;
    auto stringField = [&](llvm::StringRef name) -> std::string {
      if (auto attr = dict.getAs<StringAttr>(name))
        return attr.getValue().str();
      return std::string();
    };
    Location loc = UnknownLoc::get(context);
    if (auto locAttr = dict.getAs<LocationAttr>("loc"))
      loc = locAttr;
    bool stubbed = false;
    if (auto stubbedAttr = dict.getAs<BoolAttr>("stubbed"))
      stubbed = stubbedAttr.getValue();
    items.push_back(RejectedItem{stringField("symbol"), loc,
                                 stringField("diagnostic"),
                                 stringField("tag"), stubbed,
                                 stringField("owner")});
  }
  return items;
}

/// Removes the shard metadata from `module`. The link merge calls this on
/// every shard before splicing: the metadata is a per-TU fact, and the
/// merged whole-program module must stay byte-comparable to the joint
/// import's, which never carries it.
inline void stripShardMetadata(ModuleOp module) {
  module->removeAttr(kItemGraphAttrName);
  module->removeAttr(kRejectionsAttrName);
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_SHARDMETADATA_H
