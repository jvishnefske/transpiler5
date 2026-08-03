//===- LinkMerge.h - FR-58 link-step shard merging -------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file declares the functional core of `emitrust-cc --link` (FR-58
/// slices 1 and 2): given the per-TU emitrust shards the FR-56 shim
/// recorded — one fully converted module per object file, embedded in the
/// object's `.emitrust` section, in the `<object>.emitrust.mlirbc` sidecar,
/// or archived as a static-archive member — merge them into the single
/// whole-program module the joint import would have produced, so the
/// EXISTING crate-emission path can materialize Rust with no `.c` re-parsed
/// at link time.
///
/// The merge algorithm is the byte-identity-proven one from design.md's
/// FR-58 spikes (2 and 3), implemented exactly:
///
///  0. The FR-57 shard metadata (`emitrust.item_graph`,
///     `emitrust.rejections` — see EmitRust/ShardMetadata.h) is stripped
///     from every shard first: those are per-TU facts the link driver
///     surfaces BEFORE merging (per-shard rejection summaries, the
///     `--emit=item-graph` shard dump), and the merged whole-program module
///     must stay byte-comparable to the joint import's, which never
///     carries them.
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
///     structural `mlir::OperationEquivalence` with locations ignored
///     (slice 2): these are module-level defs with no operands, so
///     equivalence is exactly attributes + types — two definitions with
///     identical layout but different FIELD NAMES conflict, because field
///     names are attributes of the definition.
///  4. Module-level `emitrust.use` / `emitrust.verbatim` header ops that a
///     later shard repeats textually are dropped, matching the joint
///     import's emit-once behavior.
///  5. The surviving ops are concatenated in shard order into one module,
///     which is then verified.
///
/// Everything here is pure with respect to the filesystem: payload
/// classification takes a loaded buffer (archive expansion included — a
/// static archive on the link line is expanded in place, each member's
/// payload extracted in archive order) and the merge takes parsed modules.
/// The imperative shell in emitrust-cc.cpp reads files, parses bytecode,
/// prints the payload-less-member warnings, and reports located
/// diagnostics (every failure path in the merge emits one through the
/// ops' recorded locations).
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_LINKMERGE_H
#define EMITRUST_TOOLS_EMITRUST_CC_LINKMERGE_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

#include <optional>
#include <string>

namespace emitrustcc {

/// One static-archive member's extracted shard payload.
struct ArchiveMemberPayload {
  /// The member's name as `ar` recorded it (usually the object's basename).
  std::string memberName;
  /// The payload bytes. Points into the archive's buffer, which must
  /// outlive this struct.
  llvm::StringRef payload;
};

/// The result of expanding one static archive named on the link line.
struct ArchivePayloads {
  /// The extracted member payloads, in ARCHIVE ORDER — the members merge as
  /// if they had been listed loose at the archive's position on the link
  /// line.
  llvm::SmallVector<ArchiveMemberPayload> payloads;
  /// Member names carrying no `.emitrust` payload, in archive order. A real
  /// build may archive objects the shim never produced (hand-written
  /// assembly, say); the caller warns and skips each — archive members have
  /// no sidecar to fall back to.
  llvm::SmallVector<std::string> skippedMembers;
};

/// Returns true when `buffer` holds a static archive (regular or thin `ar`
/// magic). A `.a` on the link line is detected by these bytes, not by its
/// extension, so an archive under any name is expanded and a mis-named
/// non-archive is not.
bool isStaticArchive(const llvm::MemoryBuffer &buffer);

/// Expands the static archive in `buffer`: every member that is an object
/// file with a `.emitrust` section contributes that section's contents, in
/// archive order; a member that is not an object file at all contributes
/// its whole bytes (a `.mlirbc` shard archived directly, mirroring how a
/// loose non-object link input is treated); an object member WITHOUT the
/// section is recorded in `skippedMembers` for the caller to warn about.
///
/// \param buffer the raw bytes of an archive link input; must outlive the
///        returned payloads.
/// \param errorMessage set to a description when the archive or a member
///        cannot be read.
/// \returns the member payloads and skipped members, or failure with
///          `errorMessage` set.
mlir::FailureOr<ArchivePayloads>
findArchivePayloads(const llvm::MemoryBuffer &buffer,
                    std::string &errorMessage);

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
/// `ordinalMaps`, when nonempty, gives each shard its per-TU tag ordinal
/// assignment: shard i's internal `tu<k>_`/`TU<k>_` tags rename to global
/// ordinal `ordinalMaps[i][k]`. This is what lets a link-time RE-IMPORTED
/// GROUP — one module produced by a joint `importCProject` over several
/// link-line positions, whose internal tags are group-relative `tu0_`,
/// `tu1_`, ... — stand in for its member shards while its statics still
/// land on the ordinals the whole-project joint import would have used
/// (the merge's byte-identity oracle). Each map must be strictly
/// increasing (global ordinals are link-line positions, so a TU's ordinal
/// is at least its group-internal index; the rename exploits this for
/// collision freedom). Empty — the default — assigns shard i the single
/// ordinal i, the historical solo-shard behavior.
///
/// \param shards the parsed shard modules, in link-line order; consumed.
/// \param ordinalMaps per-shard tag ordinal assignments, parallel to
///        `shards`, or empty for the positional default.
/// \returns the merged, verified module, or failure after a located
///          diagnostic.
mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>>
mergeLinkShards(llvm::MutableArrayRef<mlir::OwningOpRef<mlir::ModuleOp>> shards,
                llvm::ArrayRef<llvm::SmallVector<unsigned>> ordinalMaps = {});

/// FR-58 selective re-import, the detection half: true when `diagnostic`
/// is one of the MEASURED fact-starvation wordings — a rejection a solo
/// import produces precisely because a whole-program fact (the defining
/// TU's shape for an extern pointer global) was out of reach, and which a
/// joint re-import of the right TU group can therefore recover. Intrinsic
/// rejections (volatile, variadics, ...) never match: re-importing those
/// costs a parse and recovers nothing.
bool isFactStarvedDiagnostic(llvm::StringRef diagnostic);

/// The C-spelled object names a fact-starved rejection references: the
/// rejected symbol itself for a dropped pointer-global declaration, and
/// the single-quoted variable name inside the "no known target object"
/// wording for a stubbed accessor. Empty when `diagnostic` is not a
/// fact-starvation wording.
llvm::SmallVector<std::string>
factStarvedObjectNames(llvm::StringRef symbol, llvm::StringRef diagnostic);

/// True when the FR-57d item-graph text `graphText` records a DEFINED
/// global named `symbol` (a `node <symbol> kind=global def=1` line): the
/// signal that a shard can supply the missing shape even when its MODULE
/// does not carry the item (a defining TU with no local use of the cursor
/// never materializes it — measured, see design.md FR-58).
bool itemGraphDefinesGlobal(llvm::StringRef graphText, llvm::StringRef symbol);

/// One signature-starved external declaration: shard `declShard` carries a
/// body-less `emitrust.extern_decl` FUNCTION whose type disagrees with the
/// type shard `defShard` defines for the same symbol. This is the merge-
/// level face of the importer's cross-TU pointer-parameter refinement (a
/// solo shard shapes a call from the bare prototype; the defining body
/// refines the model to a slice/cell-slice/owner form the shard could not
/// know), so the pair must be re-imported jointly — dropping the
/// declaration for the definition, as a matching-signature obligation is,
/// would leave call sites shaped for a type the definition does not have.
struct SignatureStarvation {
  /// Index (into the scanned shard list) of the declaring shard.
  unsigned declShard;
  /// Index of the defining shard.
  unsigned defShard;
  /// The shared symbol name.
  std::string symbol;
};

/// Scans the shards, in order, for signature-starved external function
/// declarations (see `SignatureStarvation`). Pure: no module is modified.
/// Globals are deliberately out of scope — C99 6.2.7 permits looser
/// declaration shapes for objects (`extern int a[];` vs `int a[4];`), and
/// the pointer-global starvation is already ledger-detected.
llvm::SmallVector<SignatureStarvation>
findSignatureStarvedDecls(llvm::ArrayRef<mlir::ModuleOp> shards);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_LINKMERGE_H
