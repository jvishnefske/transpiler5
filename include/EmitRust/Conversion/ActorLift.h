//===- ActorLift.h - FR-62 same-thread actor lift ---------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares `emitrust-actor-lift`, the FR-62 slice-4 module pass that
/// restructures a pipeline-stage module into the same-thread actor shape:
/// each planned actor's mutable globals become fields of a synthesized
/// struct, the actor's arm functions move into an `emitrust.impl` taking the
/// actor as a `&mut self` receiver, cross-actor client functions gain one
/// `!emitrust.mut_ref` parameter per actor in their closure footprint, and
/// the driver (`c_main`) constructs each actor as a local and calls arms as
/// methods. The lifted `emitrust.global` ops are deleted, so the emitted
/// crate carries no `thread_local!` cell for any owned global.
///
/// # Architecture seam (the slice-4 stage-A decision, recorded here)
///
/// The pass consumes ONLY discardable attributes; it never sees the actor
/// PLAN, the item graph, or any clang AST. The `emitrust-cc` driver — the
/// one component that has the FR-40/FR-57d graph facts and the pure
/// `planActors` core — evaluates the plan, applies every demotion rule
/// (address-taken arm/cross functions, poison-merged actors, plan arms with
/// no imported function, driverless library TUs, `--link`), and attaches
/// the attributes below for CERTIFIED actors only. The alternative seam
/// (passing the pinned actor-plan text as a pass option and re-parsing it
/// in the pass) was rejected because the plan alone is not enough — the
/// demotion rules need graph edges (`TakesAddressOf`) the plan does not
/// carry, and the naming rules need `CSymbolNaming.h`, which must not leak
/// into a conversion library. With attributes, hand-written pipeline-stage
/// MLIR exercises the pass in isolation under `emitrust-opt`, and the
/// driver's plan computation stays where the plan's inputs live.
///
/// # The attribute contract
///
/// Module attributes:
///  - `emitrust.actor_lift`: ArrayAttr of DictionaryAttr, one per certified
///    actor, in construction order:
///      {name    = StringAttr   // struct type name, e.g. "CounterActor"
///       var     = StringAttr   // driver local / cross param name,
///                              // e.g. "counter_actor"
///       globals = ArrayAttr    // owned global symbol names (StringAttr),
///                              // field order
///       fields  = ArrayAttr}   // parallel Rust field names (StringAttr)
///    A listed global with no matching `emitrust.global` is skipped (the
///    plan may own symbols the importer folded away); a listed CONST global
///    is a producer bug and fails the pass loudly.
///    An entry may additionally carry `export` (UnitAttr, FR-62 F2): the
///    actor lives in a LIBRARY unit with no constructing driver and is
///    exported as an owner handle instead — the pass marks the synthesized
///    struct_def with `emitrust.private_fields` (pub struct, private
///    fields in export-mode emission) and synthesizes an associated
///    `fn new()` (`emitrust.static_method`) as the impl's first function,
///    carrying each field's C initializer as the same Default-plus-member-
///    assign materialization the driver construction uses. No driver
///    construction happens for an exported actor (there is no driver).
///  - `emitrust.actor_locals`: ArrayAttr of DictionaryAttr
///      {global = StringAttr, name = StringAttr}
///    for globals of driver-only actors (no arms, no cross clients): each
///    becomes a named local variable of `c_main` carrying the global's
///    initializer, and the global is deleted.
///
/// Function attributes:
///  - `emitrust.actor_arm = "<name>"`: the function is an arm of that
///    actor; it is rewritten to take the receiver and moves into the impl.
///  - `emitrust.actor_cross = ["<name>", ...]`: the function stays at
///    module level and gains one `!emitrust.mut_ref` parameter per listed
///    actor, in list order (the driver sorts by actor name).
///  - `emitrust.actor_driver` (unit): the function is the driver; it
///    constructs the actor locals and calls arms as methods.
///
/// All five attributes are consumed: the pass strips them from the output.
///
/// # Safety net
///
/// Before touching an actor the pass re-verifies, on the IR itself, that
/// every symbol use of every owned global is a `global_load`/`global_store`
/// inside a function attributed to that actor's surface (arm / listed
/// cross / driver). Any other use — an unattributed function (e.g. a
/// variadic monomorph the plan could not see), a `global_cells` region, a
/// use kind added later — vetoes the actor with a printed warning and
/// leaves its globals and functions completely untouched, which is today's
/// correct thread-local form. The failure direction of anything the veto
/// misses is a loud rustc error (the deleted global's accessor no longer
/// exists), never silent misbehavior.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_ACTORLIFT_H
#define EMITRUST_CONVERSION_ACTORLIFT_H

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTACTORLIFT
#include "EmitRust/Conversion/Passes.h.inc"

/// Module-level ArrayAttr of per-actor DictionaryAttrs (see the file
/// comment for the dictionary keys).
inline constexpr llvm::StringLiteral kActorLiftActorsAttrName =
    "emitrust.actor_lift";

/// Module-level ArrayAttr of `{global, name}` DictionaryAttrs naming the
/// driver-only globals lowered to `c_main` locals.
inline constexpr llvm::StringLiteral kActorLiftLocalsAttrName =
    "emitrust.actor_locals";

/// Function StringAttr: the actor (struct name) this arm belongs to.
inline constexpr llvm::StringLiteral kActorArmAttrName = "emitrust.actor_arm";

/// Function ArrayAttr of StringAttr: the actors threaded through this
/// cross-actor client, in parameter order.
inline constexpr llvm::StringLiteral kActorCrossAttrName =
    "emitrust.actor_cross";

/// Function UnitAttr marking the driver (`c_main`).
inline constexpr llvm::StringLiteral kActorDriverAttrName =
    "emitrust.actor_driver";

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_ACTORLIFT_H
