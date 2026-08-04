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
///  0. (FR-62 F1b) a caller-supplied PRE-DEMOTION: the `preDemotions`
///     entries demote their actors before any table rule runs, with the
///     caller's reason. The partition path uses this to demote every
///     actor whose cluster is not bin-local (`spans workspace crates`)
///     while keeping the plan — and so the universe and every actor
///     index — intact; partial demotion of a cross client's targets then
///     composes through the same machinery rules 1b/2 already exercise;
///  1. an arm or cross function is the target of a `TakesAddressOf` edge —
///     a fn-pointer call site cannot thread the receiver; additionally
///     reported as a located remark at the address-taking function when the
///     graph carries its location;
///  1c. an owned global that is a deferred-external DECLARATION (FR-57a
///     `emitrust.extern_decl`): another translation unit owns the state,
///     so lifting would delete the declaration and drop the FR-58 link
///     obligation (found by the F2 export flip on a --defer-externals
///     library TU);
///  2. a poison-merged actor (`Actor::poisoned`); a pure writer-rule
///     "@stdout" merge is NOT poisoned and lifts fine — the pseudo-global
///     contributes no field, and an actor NAMED "@stdout" takes its
///     smallest real global's name for the synthesized type instead;
///  3. a plan arm/cross symbol with no top-level imported `emitrust.func`
///     (variadic monomorphization, recovery drops, FR-30 owner methods);
///  4. (FR-62 F2) a plan with no driver role, or a module with no `c_main`
///     (library unit: nobody constructs the actors), no longer demotes —
///     each certified actor EXPORTS as an owner handle instead: its dict
///     carries the `export` unit key, the pass synthesizes an associated
///     `fn new()` carrying the C initializers, and the struct is emitted
///     pub with PRIVATE fields. Two library-only demotions guard the
///     export: an arm that is generic over the FR-52 external-requirements
///     trait (an exported method call cannot carry the type parameter —
///     E0283 at every internal call site — so the actor keeps the proven
///     generic-compatible thread-local form), and an arm named `new`
///     (it would collide with the synthesized constructor);
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
///    "main-only globals become main locals" rule;
///  - under `--preserve-c-names` (FR-53's verbatim opt-out) every name
///    DERIVED FROM a C spelling — field, cell base, driver local — keeps
///    the symbol verbatim instead of snake_casing it; purely SYNTHESIZED
///    names (the actor type and `_actor` variable) keep the rules above,
///    since they never were C spellings.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_ACTORLIFTPLAN_H
#define EMITRUST_TOOLS_EMITRUST_CC_ACTORLIFTPLAN_H

#include "ActorPlan.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <string>
#include <utility>

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

/// One exported owner handle (FR-62 F2): a certified actor in a library
/// unit, exported as a pub struct with private fields and a synthesized
/// `new()` instead of being demoted; the driver prints one
/// `note: actor plan: exported <actor>: ...` per entry.
struct ActorLiftExport {
  std::string actor;    ///< The PLAN actor name (the plan-text identity).
  std::string typeName; ///< The exported owner struct name.
};

/// What `attachActorLiftAttributes` did.
struct ActorLiftAttachment {
  /// Whether any lift attribute was attached — i.e. whether running the
  /// emitrust-actor-lift pass can change the module.
  bool attachedAny = false;
  /// The demoted actors, in plan (name-sorted) order.
  llvm::SmallVector<ActorLiftDemotion> demotions;
  /// The exported owner handles (library units only), in entry order.
  llvm::SmallVector<ActorLiftExport> exports;
};

/// Certifies `plan` against `graph` and `module` and attaches the
/// `emitrust-actor-lift` attribute contract for every certified actor (see
/// the file comment). Mutates only attributes; never IR structure.
/// `preserveCNames` selects FR-53's verbatim spelling for every name
/// derived from a C symbol (fields, cell bases, driver locals).
/// `preDemotions` is rule 0: (plan actor index, reason) entries the caller
/// decided before certification — FR-62 F1b's partition demotions; each
/// demotes its actor with the given reason before any table rule runs.
ActorLiftAttachment attachActorLiftAttributes(
    mlir::ModuleOp module, const mlir::emitrust::ItemGraph &graph,
    const ActorPlan &plan, bool preserveCNames,
    llvm::ArrayRef<std::pair<unsigned, std::string>> preDemotions = {});

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_ACTORLIFTPLAN_H
