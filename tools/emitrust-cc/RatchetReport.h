//===- RatchetReport.h - FR-60 rejection report + ratchet manifest -*-C++-*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The two pure cores of FR-60's kernel-corpus ratchet, both computed from
/// the FR-57d shard artifacts alone (no merge, no re-import, no C parsed —
/// a pure artifact query that scales to a kernel link line):
///
///  1. THE PER-CONSTRUCT REJECTION REPORT (`--emit=rejection-report`): the
///     shard ledgers aggregated by the `classifyBlocker` TAG — the
///     normalization table each ledger entry already carries, shared
///     verbatim with the RealWorld survey, so the report and the survey
///     rank the same vocabulary (spike-verified: inline asm arrives as
///     `unsupported-stmt:GCCAsmStmt`, allocator use as `dynamic-memory`,
///     system-header calls as `libc:<name>`). Within a tag the distinct
///     diagnostic WORDINGS are tabulated with single-quoted spans
///     normalized to `'<name>'`, collapsing per-symbol variants. Ranked by
///     item count (then tag), each tag citing its first location: the
///     queryable answer to "what semantic work buys the most frontier".
///
///  2. THE RATCHET MANIFEST (`--emit=ratchet`): the c-testsuite ledger
///     generalized per project — admitted-item counts per shard and total,
///     the FR-59 partition facts (crate count, condensation-warning
///     count), and the per-tag rejection snapshot. Shards are keyed by
///     their recorded source path RELATIVE to the shards' common directory
///     prefix, so a committed manifest is checkout-portable. The
///     comparison keeps the c-testsuite ratchet's direction rules: an
///     admitted SHRINK (total, per shard, or a shard vanishing) or a
///     condensation GROWTH fails; growth passes, and the freshly rendered
///     manifest is the update.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_RATCHETREPORT_H
#define EMITRUST_TOOLS_EMITRUST_CC_RATCHETREPORT_H

#include "EmitRust/ImportC.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <string>
#include <vector>

namespace emitrustcc {

/// One shard's decoded facts, as the driver hands them to both renderers.
struct ShardFacts {
  /// The shard's recorded source path (used for the manifest key), or the
  /// link-line attribution name when the artifact records none.
  std::string sourcePath;
  /// The shard's decoded FR-57d rejection-ledger entries.
  llvm::SmallVector<mlir::emitrust::RejectedItem> rejections;
  /// Module-level definition count: symbol-carrying top-level ops minus
  /// `emitrust.extern_decl` declarations. Admitted = this minus the
  /// STUBBED subset of `rejections` (a stub is present but not ported).
  unsigned definitionCount = 0;
};

/// Normalizes one diagnostic wording for tabulation: every single-quoted
/// span becomes `'<name>'`, so "call to 'qsort' ..." and "call to 'free'
/// ..." are one wording. Pure.
std::string normalizeDiagnostic(llvm::StringRef diagnostic);

/// Renders the FR-60 per-construct rejection report (see the file
/// comment). Deterministic: tags ranked by item count descending then tag
/// name, wordings by count descending then text; the cited location is the
/// first rejection's, in shard order.
std::string renderRejectionReport(llvm::ArrayRef<ShardFacts> shards);

/// The parsed/computed ratchet manifest.
struct RatchetManifest {
  unsigned admittedTotal = 0;
  unsigned rejectedTotal = 0;
  unsigned crates = 1;
  unsigned condensationWarnings = 0;
  /// Shard key -> (admitted, rejected), in link-line order.
  std::vector<std::pair<std::string, std::pair<unsigned, unsigned>>> shards;
  /// Tag -> rejected-item count, ordered by tag.
  std::map<std::string, unsigned> tags;
};

/// Builds the manifest from the shard facts plus the FR-59 partition
/// facts. Shard keys are source paths relative to the shards' common
/// directory prefix (a single shard keeps its filename).
RatchetManifest buildRatchetManifest(llvm::ArrayRef<ShardFacts> shards,
                                     unsigned crates,
                                     unsigned condensationWarnings);

/// Renders the manifest in the pinned `emitrust ratchet manifest v1` line
/// format. Deterministic; `parseRatchetManifest` round-trips it.
std::string renderRatchetManifest(const RatchetManifest &manifest);

/// Parses a v1 manifest. Returns false with `error` set on a malformed
/// text (unknown header, unparsable count).
bool parseRatchetManifest(llvm::StringRef text, RatchetManifest &manifest,
                          std::string &error);

/// The ratchet comparison: regressions of `current` against `baseline`,
/// empty when the ratchet holds. Direction rules per the file comment;
/// wordings are pinned by test/Driver/link-ratchet.c.
llvm::SmallVector<std::string>
compareRatchetManifests(const RatchetManifest &baseline,
                        const RatchetManifest &current);

/// The improvements (admitted growth) worth telling the developer about;
/// purely informational.
llvm::SmallVector<std::string>
ratchetImprovements(const RatchetManifest &baseline,
                    const RatchetManifest &current);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_RATCHETREPORT_H
