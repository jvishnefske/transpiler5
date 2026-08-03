//===- ActorThread.h - FR-62 threaded-mode actor rewrite -------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares `emitrust-actor-thread`, the FR-62 slice-5b module pass that
/// rewrites selected LIFTED actors (the `emitrust-actor-lift` output shape)
/// into the threaded-runtime driver shape: the driver's actor local is
/// retyped to the opaque `<Actor>Handle`, constructed by
/// `<Actor>Handle::spawn(<Actor>::default())`, the driver's direct
/// member/subscript accesses become synthesized get_/set_ accessor METHODS
/// (ordinary impl funcs) called through the handle, `.shutdown()` is
/// inserted before every driver return, and one `emitrust.actor_runtime`
/// anchor per actor carries the mode for the emitter's runtime synthesis.
/// Runs strictly AFTER emitrust-actor-lift at the pipeline tail (ActorLift
/// strips its own attributes, so this pass has its own contract).
///
/// # The attribute contract (mirroring ActorLift.h's seam)
///
/// Module attribute `emitrust.actor_thread`: ArrayAttr of DictionaryAttr,
/// one per actor the driver selected for a non-same-thread runtime:
///     {name = StringAttr    // the lifted struct type name, "CounterActor"
///      mode = StringAttr}   // "threaded" | "async"
/// The DRIVER computes the list (beside `attachActorLiftAttributes`, from
/// the same certified-actor set) and the pass consumes it: the attribute is
/// STRIPPED after use. An entry whose struct_def, impl, or constructing
/// driver local does not exist in the module is skipped SILENTLY — that is
/// the composition rule for lift-demoted actors (they were never lifted, so
/// they keep today's thread_local form; demote-from-lifting already warned).
///
/// # Eligibility vetoes (actor stays LIFTED same-thread, warning printed)
///
/// A veto leaves the actor's IR completely untouched and prints
/// `warning: actor plan: <actor> stays same-thread: <reason>` (located on
/// the offending op). Demote-from-threading is NOT demote-from-lifting:
///  - a cross-actor client (any function outside the actor's impl) holds an
///    `!emitrust.mut_ref` of the actor's struct — a borrow cannot cross the
///    thread boundary (the E3 landmine rule);
///  - an impl method has an unsendable signature: a param/result type
///    outside the struct-field validity set (`isSendableActorType`), an
///    unnamed parameter (no message field to derive), a static/associated
///    function, or a body-less declaration;
///  - a driver access has no accessor form: anything but a scalar member
///    load/assign or a scalar array-element subscript load/assign (whole
///    aggregate snapshots, nested member paths, address-taking, mixed
///    subscript index types);
///  - a synthesized accessor name (`get_<field>` / `set_<field>`) collides
///    with an existing impl method.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_ACTORTHREAD_H
#define EMITRUST_CONVERSION_ACTORTHREAD_H

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTACTORTHREAD
#include "EmitRust/Conversion/Passes.h.inc"

/// Module-level ArrayAttr of `{name, mode}` DictionaryAttrs naming the
/// actors selected for a non-same-thread runtime (see the file comment).
inline constexpr llvm::StringLiteral kActorThreadAttrName =
    "emitrust.actor_thread";

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_ACTORTHREAD_H
