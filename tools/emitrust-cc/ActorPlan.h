//===- ActorPlan.h - FR-62 actor decomposition planning ---------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The functional core of FR-62 actor planning: given the link line's
/// actor UNITS -- one per post-re-import shard, each carrying its FR-57d
/// item-graph texts -- plan the actor decomposition: which global lands in
/// which actor, which defined functions are that actor's arms, and which
/// are the driver / free helpers / cross-actor clients.
///
/// Decisions this planner encodes (each measured in design.md's FR-62 E5
/// entry; the interface is the E5 FROZEN signature):
///
///  - DEFAULT SEEDING is the co-access clustering (E2's algorithm):
///    union-find over each non-main defined function's DIRECT
///    ReadsGlobal|WritesGlobal|AddressOfGlobal targets, plus the "@stdout"
///    pseudo-global. `c_main` is exempt from seeding and from the writer
///    rule: it is the DRIVER and owns all actors.
///  - "@stdout" IS TOTAL (slice 1's hosted-sink visibility): a `Calls`
///    edge whose target is a hosted OUTPUT-STREAM sink the project does
///    not define itself -- printf, puts, putchar, fprintf, fwrite -- is a
///    WRITE of the distinguished pseudo-global "@stdout", which exists
///    only in plans, never as a graph node. sprintf and snprintf are
///    graph-visible sinks too but are DELIBERATELY NOT "@stdout" touches:
///    the importer's emitSprintf writes the caller's BUFFER and has no
///    output effect, so attributing a stream write would fabricate a
///    coupling the emitted Rust does not have.
///  - ADDRESS-TAKING IS A WRITE (slice 1's AddressOfGlobal edge kind): a
///    function whose body takes a global's address may store through the
///    escaped pointer, so an `AddressOfGlobal` edge counts as a WRITE of
///    that global for both co-access seeding and the writer rule --
///    conservative and sound where the pre-slice-1 planner was
///    direction-blind. A GLOBAL-to-global `AddressOfGlobal` edge (a
///    pointer global's initializer, `int *q = arr + 2`) puts pointer and
///    pointee in ONE actor -- E1's measured rule that a pointer global
///    and its pointee region are one owner -- by seeding when both are
///    unpinned and by noted condensation when overrides pinned them
///    apart.
///  - CONDENSATION, not failure, resolves every conflict the actor
///    decomposition cannot express:
///      (1) a non-trivial call-graph SCC is one emission unit -- if its
///          members' combined Calls-closure footprint (reads|writes)
///          spans >1 actor, those actors merge;
///      (2) a NON-main defined function whose Calls-closure WRITES
///          globals of >1 actor merges them -- a cross-actor writer
///          cannot be an arm of either actor; a function with
///          CallsIndirect anywhere in its closure write-spans the whole
///          universe (poison) and merges everything it can see.
///    Every condensation is reported as a note the driver prints as a
///    warning. The merged actor keeps the smallest participating name
///    (deterministic, the analogue of FR-59's earliest-member rule).
///  - `--actor-map` OVERRIDES: (symbol prefix -> actor name), the LONGEST
///    matching prefix wins; an overridden global is PINNED to the named
///    actor BEFORE condensation and excluded from co-access seeding.
///    Condensation runs after and may merge override-named actors, so
///    overrides can never create unsoundness -- the exact isomorphism to
///    planPartition's `--partition-map` rule. There is NO separate seeds
///    parameter: a seed cluster is expressible as exact-symbol override
///    entries sharing one actor name (measured equivalent in E5 on all
///    eight dry-run programs).
///
/// Render format (`renderActorPlan`, and `emitrust-cc --emit=actor-plan`),
/// one fact per line, every `actor` line before every `fn` line before
/// every `note` line:
///
/// \code
/// actor <name> globals=<comma-joined-sorted-owned-globals>
/// fn <symbol> role=<arm|driver|free|cross> actor=<name>
/// note <free text>
/// \endcode
///
/// Every field of an `actor`/`fn` line is a single whole space-separated
/// token, so `grep ' role=arm '` and FileCheck patterns work on whole
/// tokens; `actor` lines are sorted by name, `fn` lines by symbol (one per
/// defined function), notes keep the planner's deterministic rule order. A
/// function that belongs to no single actor (driver/free/cross) prints
/// `actor=-`. `note` lines are free text, pinned only as deterministic.
///
/// Everything here is pure: strings in, plan out, no filesystem, no MLIR.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_ACTORPLAN_H
#define EMITRUST_TOOLS_EMITRUST_CC_ACTORPLAN_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <string>
#include <utility>

namespace emitrustcc {

/// One actor-planning unit: a post-re-import link-line shard (a re-import
/// group is ONE unit, matching PartitionUnit). Internal-linkage globals
/// are keyed (unit index, symbol): two units' file-statics sharing a name
/// are distinct objects and may land in distinct actors.
struct ActorUnit {
  /// The unit's C source paths (one per member TU, in link-line order);
  /// used only in notes and diagnostics.
  llvm::SmallVector<std::string> sourcePaths;
  /// The unit's FR-57d item-graph texts, parallel to `sourcePaths`.
  llvm::SmallVector<std::string> graphTexts;
};

/// A defined function's relation to the plan.
enum class ActorRole : uint8_t {
  Arm,    ///< footprint within exactly one actor; `actor` is valid.
  Driver, ///< `c_main`: owns/drives all actors; exempt from the writer
          ///< rule; never forces a merge.
  Free,   ///< empty closure footprint (pure helper); callable from any
          ///< actor's arms; owned by no actor.
  Cross,  ///< READS globals of >1 actor (writes never span >1 after
          ///< condensation): a message-passing client, not an arm.
};

/// One defined function's assignment.
struct ActorFunction {
  std::string symbol;
  ActorRole role = ActorRole::Free;
  unsigned actor = 0; ///< index into ActorPlan::actors; valid iff Arm.
};

/// One planned actor.
struct Actor {
  /// The sanitized actor name: an override's name, or the smallest owned
  /// global's symbol; the smallest name survives a condensation merge.
  std::string name;
  /// The globals this actor owns, sorted; "@stdout" pseudo-global
  /// included when hosted sinks are visible.
  llvm::SmallVector<std::string> globals;
};

/// The planned decomposition.
struct ActorPlan {
  /// The actors, sorted by name; every universe element (all `global`
  /// nodes plus Reads/Writes edge targets plus "@stdout") appears in
  /// exactly one actor's `globals`.
  llvm::SmallVector<Actor> actors;
  /// (global symbol -> actor index), sorted by symbol; total over the
  /// universe.
  llvm::SmallVector<std::pair<std::string, unsigned>> actorOfGlobal;
  /// One entry per DEFINED function, sorted by symbol.
  llvm::SmallVector<ActorFunction> actorOfFunction;
  /// Human-readable condensation notes, in a deterministic order; the
  /// driver prints each as `warning: actor plan: <note>`.
  llvm::SmallVector<std::string> notes;
};

/// Plans the actor decomposition for `units` (see the file comment for
/// the rules).
///
/// \param units the planning units, in link-line order.
/// \param overrides `--actor-map` entries: (symbol prefix, actor name);
///        the LONGEST prefix matching a global's symbol wins, pinning it
///        to the named actor; unmatched globals fall back to co-access
///        cluster seeding.
/// \returns the plan; infallible (condensation resolves every conflict).
///
/// Soundness invariant (checked by the E5 harness on every emitted plan):
/// after condensation, no non-main defined function's closure
/// write-footprint spans more than one actor, and every universe global
/// is owned by exactly one actor.
ActorPlan
planActors(llvm::ArrayRef<ActorUnit> units,
           llvm::ArrayRef<std::pair<std::string, std::string>> overrides);

/// Renders `plan` in the pinned line format documented at the top of this
/// file. Pure; depends on nothing but the plan value.
std::string renderActorPlan(const ActorPlan &plan);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_ACTORPLAN_H
