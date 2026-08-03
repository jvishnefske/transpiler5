//===- ActorLiftPlan.h - FR-62 slice 4 driver-side certification -*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The driver half of the FR-62 slice-4 architecture seam (see
/// `EmitRust/Conversion/ActorLift.h`): given the pure `planActors` plan and
/// the FR-40 item graph, CERTIFY each actor against the demotion rules,
/// compute every emitted name through the shared `CSymbolNaming.h`
/// conventions, and attach the discardable attributes the
/// `emitrust-actor-lift` pass consumes. Nothing here rewrites IR beyond
/// attaching attributes; a demoted actor's globals and functions get no
/// attribute at all, which IS the demotion (the pass only touches what is
/// attributed).
///
/// Demotion rules (the SLICE-4 SPIKE table; demotion-is-not-an-error, one
/// warning per demoted actor, first matching rule wins):
///  1. an arm or cross function is the target of a `TakesAddressOf` edge —
///     a fn-pointer call site cannot thread the receiver; additionally
///     reported as a located remark at the address-taking function when the
///     graph carries its location;
///  2. a poison-merged actor (`Actor::poisoned`); a pure writer-rule
///     "@stdout" merge is NOT poisoned and lifts fine — the pseudo-global
///     contributes no field, and an actor NAMED "@stdout" takes its
///     smallest real global's name for the synthesized type instead;
///  3. a plan arm/cross symbol with no top-level imported `emitrust.func`
///     (variadic monomorphization, recovery drops, FR-30 owner methods);
///  4. a plan with no driver role, or a module with no `c_main` (library
///     unit: nobody constructs the actors; owner-handle export is a
///     recorded later slice);
///  5. `--link` (demote-all) is handled by the caller before any plan
///     reaches this function.
/// Additionally, a synthesized type/variable name colliding with an
/// existing module symbol demotes the actor (mechanical guard), and the
/// pass applies its own IR-level use veto as the last safety net.
///
/// Naming rules (recorded here as the stage-A contract):
///  - actor type = UpperCamelCase(plan actor name) + "Actor"
///    (`COUNTER` -> `CounterActor`); for an actor named "@stdout" the
///    smallest real owned global's symbol substitutes for the plan name;
///  - driver local and cross parameter = snake_case(same base) + "_actor"
///    (`counter_actor`);
///  - field = snake_case(owned global symbol) (`COUNTER` -> `counter`);
///  - a lifted function-local static cell drops its owning function's
///    mangle prefix (`ACCUMULATE_TOTAL` owned by `accumulate` -> field
///    `total`) and, when its owner is a FREE function called only by the
///    driver, seeds a synthesized actor named after the owner
///    (`AccumulateActor` / `accumulate_actor`);
///  - a driver-only actor (no arms, no cross clients, all uses in
///    `c_main`) lowers each global to a named driver local,
///    snake_case(symbol), carrying the global's initializer — the E1
///    "main-only globals become main locals" rule.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_ACTORLIFTPLAN_H
#define EMITRUST_TOOLS_EMITRUST_CC_ACTORLIFTPLAN_H

#include "ActorPlan.h"

#include "llvm/ADT/SmallVector.h"

#include <string>

namespace mlir {
class ModuleOp;
namespace emitrust {
struct ItemGraph;
} // namespace emitrust
} // namespace mlir

namespace emitrustcc {

/// One demoted actor, with the warning the driver prints as
/// `warning: actor plan: demoted <actor>: <reason>` and an optional
/// located remark (rule 1's address-taking site).
struct ActorLiftDemotion {
  std::string actor;  ///< The PLAN actor name (the plan-text identity).
  std::string reason; ///< Free text after "demoted <actor>: ".
  std::string remarkText; ///< Empty when no located remark applies.
  std::string remarkFile;
  unsigned remarkLine = 0;
  unsigned remarkColumn = 0;
};

/// What `attachActorLiftAttributes` did.
struct ActorLiftAttachment {
  /// Whether any lift attribute was attached — i.e. whether running the
  /// emitrust-actor-lift pass can change the module.
  bool attachedAny = false;
  /// The demoted actors, in plan (name-sorted) order.
  llvm::SmallVector<ActorLiftDemotion> demotions;
};

/// Certifies `plan` against `graph` and `module` and attaches the
/// `emitrust-actor-lift` attribute contract for every certified actor (see
/// the file comment). Mutates only attributes; never IR structure.
ActorLiftAttachment
attachActorLiftAttributes(mlir::ModuleOp module,
                          const mlir::emitrust::ItemGraph &graph,
                          const ActorPlan &plan);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_ACTORLIFTPLAN_H
