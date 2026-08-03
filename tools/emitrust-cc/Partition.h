//===- Partition.h - FR-59 workspace partition planning ---------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The functional core of FR-59 workspace partitioning: given the link
/// line's partition UNITS — one per post-re-import shard, each carrying its
/// source paths, its FR-57d item-graph texts, and the type names of its
/// `emitrust.impl` blocks — plan the Cargo workspace: which unit lands in
/// which crate, what each crate is named, and which crates depend on which.
///
/// Decisions this planner encodes (each measured or argued in design.md's
/// FR-59 entry):
///
///  - PARTITION UNITS ARE WHOLE TUs (a re-import group counts as one unit).
///    This is what makes the statics invariant hold BY CONSTRUCTION: a C
///    file-static cannot be referenced from another TU, so an internal-
///    linkage symbol and its users can never face a crate boundary, and no
///    `pub` ever needs widening for one.
///  - DEPENDENCY EDGES COME FROM THE SHARDS' ITEM-GRAPH TEXTS — the same
///    per-shard, already-carried, indexable source the FR-58 selective
///    re-import uses — not from walking the merged module (whose
///    `call_opaque` callees are bare strings and whose ops no longer record
///    their TU). A unit that references a symbol another unit DEFINES
///    (`def=1` node, first definer wins, matching the merge's
///    first-occurrence dedup) depends on that unit's crate.
///  - CONDENSATION, not failure, resolves every boundary the packaging
///    cannot express: a GLOBAL referenced across the boundary (FR-51 never
///    exports globals), an `emitrust.impl` block away from its type's crate
///    (Rust's orphan rule), a crate referencing INTO the binary crate
///    (cargo cannot depend on a bin), and dependency CYCLES (SCCs collapse
///    into their earliest member crate). Every condensation is reported as
///    a note the driver prints as a warning.
///  - DEFAULT ASSIGNMENT is the source file's parent directory; the
///    `--partition-map` override maps the longest matching path prefix to
///    an explicit crate name.
///
/// Everything here is pure: strings in, plan out, no filesystem, no MLIR.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_PARTITION_H
#define EMITRUST_TOOLS_EMITRUST_CC_PARTITION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <utility>

namespace emitrustcc {

/// One partition unit: a post-re-import link-line shard (a re-import group
/// is ONE unit — its members were merged into one module and are
/// inseparable at materialization time).
struct PartitionUnit {
  /// The unit's C source paths (one per member TU, in link-line order).
  llvm::SmallVector<std::string> sourcePaths;
  /// The unit's FR-57d item-graph texts, parallel to `sourcePaths`.
  llvm::SmallVector<std::string> graphTexts;
  /// The struct names of the unit's module-level `emitrust.impl` blocks:
  /// an impl must land in its type's crate (Rust's orphan rule), so each
  /// name is a condensation edge to the type's defining unit.
  llvm::SmallVector<std::string> implTypes;
};

/// One planned workspace member.
struct PartitionCrate {
  /// The sanitized cargo package name; also the member directory name.
  std::string name;
  /// True for the binary member (the crate whose unit defines `c_main`).
  bool isBin = false;
  /// Indices (into the plan's crate list) of the crates this one depends
  /// on, sorted, self excluded.
  llvm::SmallVector<unsigned> deps;
  /// The units assigned to this crate, in link-line order.
  llvm::SmallVector<unsigned> units;
};

/// The planned workspace.
struct PartitionPlan {
  /// Crate index per unit, parallel to the input units.
  llvm::SmallVector<unsigned> unitCrate;
  /// The workspace members, ordered by their earliest unit's link-line
  /// position (deterministic; the root manifest lists them in this order).
  llvm::SmallVector<PartitionCrate> crates;
  /// Human-readable condensation notes, in a deterministic order; the
  /// driver prints each as `warning: workspace partition: <note>`.
  llvm::SmallVector<std::string> notes;
};

/// Plans the workspace for `units` (see the file comment for the rules).
///
/// \param units the partition units, in link-line order.
/// \param binCrateName the package name for the binary member (used only
///        when some unit's graph defines `c_main`).
/// \param overrides `--partition-map` entries: (path prefix, crate name);
///        the LONGEST prefix matching a unit's first source path wins,
///        and unmatched units fall back to the parent-directory default.
/// \returns the plan; infallible (condensation resolves every conflict).
PartitionPlan
planPartition(llvm::ArrayRef<PartitionUnit> units,
              llvm::StringRef binCrateName,
              llvm::ArrayRef<std::pair<std::string, std::string>> overrides);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_PARTITION_H
