//===- LinkMerge.h - FR-58 link-step shard merging -------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file declares the functional core of `emitrust-cc --link` (FR-58
/// slice 1): given the per-TU emitrust shards the FR-56 shim recorded — one
/// fully converted module per object file, embedded in the object's
/// `.emitrust` section or in the `<object>.emitrust.mlirbc` sidecar — merge
/// them into the single whole-program module the joint import would have
/// produced, so the EXISTING crate-emission path can materialize Rust with
/// no `.c` re-parsed at link time.
///
/// The merge algorithm is the byte-identity-proven one from design.md's
/// FR-58 spikes (2 and 3), implemented exactly:
///
///  1. Shards are taken in LINK-LINE ORDER; shard i's per-TU internal-linkage
///     tags (`tu0_`/`TU0_`, the TU-local placeholder a solo shard-mode
///     import assigns) are alpha-renamed to the shard's global ordinal
///     (`tu<i>_`/`TU<i>_`), with symbol uses following. A shard carrying a
///     tag with ordinal other than 0 is an error: a solo import can only
///     ever have claimed `tu0_`.
///  2. Every declaration marked `emitrust.extern_decl` is dropped when some
///     shard DEFINES its symbol (declaration-for-definition replacement);
///     when none does, that is THE undefined-symbol link error.
///  3. Module-level `emitrust.struct_def` / `emitrust.enum_def` /
///     `emitrust.global` definitions are deduped by symbol name, first
///     occurrence wins; a later same-symbol definition with a DIFFERENT
///     shape is the shape-conflict link error. Shape comparison is
///     print-to-string equality — acceptable for slice 1 (the printed form
///     is a total function of the op and carries no locations);
///     `OperationEquivalence` is the eventual upgrade.
///  4. Module-level `emitrust.use` / `emitrust.verbatim` header ops that a
///     later shard repeats textually are dropped, matching the joint
///     import's emit-once behavior.
///  5. The surviving ops are concatenated in shard order into one module,
///     which is then verified.
///
/// Everything here is pure with respect to the filesystem: payload
/// classification takes a loaded buffer and the merge takes parsed modules.
/// The imperative shell in emitrust-cc.cpp reads files, parses bytecode,
/// and reports located diagnostics (every failure path in the merge emits
/// one through the ops' recorded locations).
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_LINKMERGE_H
#define EMITRUST_TOOLS_EMITRUST_CC_LINKMERGE_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

#include <optional>
#include <string>

namespace emitrustcc {

/// Locates the emitrust shard payload inside one link input `buffer`.
///
/// Three cases, mirroring what a link line can carry:
///  - the buffer is an OBJECT FILE with a `.emitrust` section: the section
///    contents (a StringRef into `buffer`, which must outlive the result);
///  - the buffer is an object file WITHOUT that section: `std::nullopt`,
///    telling the caller to fall back to the `<object>.emitrust.mlirbc`
///    sidecar next to the object (the non-ELF path the shim also keeps);
///  - the buffer is not an object file at all: the whole buffer, so a
///    `.mlirbc` shard (or textual MLIR) can be named on the link line
///    directly.
///
/// \param buffer the raw bytes of one link-line input.
/// \param errorMessage set to a description when reading an object's
///        section contents fails.
/// \returns the payload bytes, `std::nullopt` for the sidecar fallback, or
///          failure with `errorMessage` set.
mlir::FailureOr<std::optional<llvm::StringRef>>
findShardPayload(const llvm::MemoryBuffer &buffer, std::string &errorMessage);

/// Merges the parsed per-TU shards, in link-line order, into one
/// whole-program module per the FR-58 algorithm in the file comment.
///
/// The merged module is spliced into `shards.front()`'s module (its
/// ownership is returned; every other shard is left empty), so the result
/// lives in the same MLIRContext the shards were parsed into. Every failure
/// emits a located diagnostic through that context before returning:
/// `unresolved external '<sym>' at link` for an obligation no shard
/// defines, `conflicting definitions of '<sym>' at link` for a shape
/// conflict, and `duplicate definition of '<sym>' at link` for two
/// non-dedupable definitions of one symbol. The result verifies before it
/// is returned.
///
/// \param shards the parsed shard modules, in link-line order; consumed.
/// \returns the merged, verified module, or failure after a located
///          diagnostic.
mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>>
mergeLinkShards(llvm::MutableArrayRef<mlir::OwningOpRef<mlir::ModuleOp>> shards);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_LINKMERGE_H
