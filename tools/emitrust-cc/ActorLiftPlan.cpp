//===- ActorLiftPlan.cpp - FR-62 slice 4 driver-side certification --------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `ActorLiftPlan.h`: the demotion table, the naming rules, the
/// static-cell attribution, and the attribute attachment. Everything
/// deterministic: plan order is name-sorted, cell owners iterate in
/// owner-symbol order, and every emitted name goes through
/// `CSymbolNaming.h`.
//
//===----------------------------------------------------------------------===//

#include "ActorLiftPlan.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/Conversion/ActorLift.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/Project/ItemGraph.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <map>
#include <optional>
#include <set>

using namespace emitrustcc;
using mlir::emitrust::ItemGraph;

namespace {

/// One actor the pass will lift: a certified plan actor or a synthesized
/// static-cell actor.
struct LiftEntry {
  std::string typeName;
  std::string varName;
  /// (global symbol, field name), in field order.
  llvm::SmallVector<std::pair<std::string, std::string>> fields;
  /// The arm function symbols (top-level `emitrust.func` ops, verified).
  llvm::SmallVector<std::string> arms;
  /// The plan actor this entry certifies, or -1 for a cell actor.
  int planActor = -1;
};

/// The unique top-level function containing every symbol use of `global`,
/// or null when uses span functions, sit outside any top-level function,
/// or do not exist at all.
mlir::emitrust::FuncOp uniqueUsingFunction(mlir::emitrust::GlobalOp global,
                                           mlir::ModuleOp module) {
  auto uses = mlir::SymbolTable::getSymbolUses(global, module);
  mlir::emitrust::FuncOp owner;
  if (!uses)
    return {};
  for (const mlir::SymbolTable::SymbolUse &use : *uses) {
    auto fn = use.getUser()->getParentOfType<mlir::emitrust::FuncOp>();
    if (!fn || (owner && fn != owner))
      return {};
    // Only direct children of the module qualify (an owner METHOD inside
    // an impl is out of scope for cell lifting).
    if (fn->getParentOp() != module.getOperation())
      return {};
    owner = fn;
  }
  return owner;
}

/// Whether every symbol use of `global` sits inside `fn`.
bool allUsesInside(mlir::emitrust::GlobalOp global, mlir::ModuleOp module,
                   mlir::emitrust::FuncOp fn) {
  auto uses = mlir::SymbolTable::getSymbolUses(global, module);
  if (!uses)
    return true;
  return llvm::all_of(*uses, [&](const mlir::SymbolTable::SymbolUse &use) {
    return use.getUser()->getParentOfType<mlir::emitrust::FuncOp>() == fn;
  });
}

/// Drops the owning function's static-local mangle prefix from a cell
/// symbol: `ACCUMULATE_TOTAL` owned by `accumulate` -> `TOTAL` (idiomatic
/// rename), `accumulate_total` -> `total` (verbatim names). Falls back to
/// the whole symbol when neither prefix matches.
std::string cellBaseName(llvm::StringRef cellSymbol,
                         llvm::StringRef ownerSymbol) {
  std::string screaming =
      mlir::emitrust::toScreamingSnakeCase(ownerSymbol) + "_";
  if (cellSymbol.starts_with(screaming))
    return cellSymbol.drop_front(screaming.size()).str();
  std::string verbatim = ownerSymbol.str() + "_";
  if (cellSymbol.starts_with(verbatim))
    return cellSymbol.drop_front(verbatim.size()).str();
  return cellSymbol.str();
}

} // namespace

ActorLiftAttachment
emitrustcc::attachActorLiftAttributes(mlir::ModuleOp module,
                                      const ItemGraph &graph,
                                      const ActorPlan &plan,
                                      bool preserveCNames) {
  ActorLiftAttachment result;
  mlir::MLIRContext *context = module.getContext();
  // FR-53's verbatim opt-out applies to every name DERIVED from a C
  // spelling; synthesized type/variable names are new names and keep the
  // idiomatic rules.
  auto derivedName = [preserveCNames](llvm::StringRef symbol) {
    return preserveCNames ? symbol.str()
                          : mlir::emitrust::toSnakeCase(symbol);
  };

  // -- Module indices.
  llvm::StringMap<mlir::emitrust::FuncOp> funcOps;
  llvm::StringMap<mlir::emitrust::GlobalOp> globalOps;
  for (mlir::Operation &op : module.getBody()->getOperations()) {
    if (auto fn = llvm::dyn_cast<mlir::emitrust::FuncOp>(op))
      funcOps[fn.getSymName()] = fn;
    else if (auto global = llvm::dyn_cast<mlir::emitrust::GlobalOp>(op))
      globalOps[global.getSymName()] = global;
  }

  // -- Graph indices for rule 1 and the free-function cell check.
  llvm::StringMap<std::string> addressTakenBy; // target -> first taker
  llvm::StringMap<std::string> globalAddressBy; // global -> first taker
  std::map<std::string, std::set<std::string>> callersOf;
  for (const mlir::emitrust::ItemEdge &edge : graph.edges) {
    if (mlir::emitrust::edgeKindName(edge.kind) == "TakesAddressOf")
      addressTakenBy.try_emplace(edge.to, edge.from);
    else if (mlir::emitrust::edgeKindName(edge.kind) == "AddressOfGlobal")
      globalAddressBy.try_emplace(edge.to, edge.from);
    else if (mlir::emitrust::edgeKindName(edge.kind) == "Calls")
      callersOf[edge.to].insert(edge.from);
  }
  auto nodeOf =
      [&](llvm::StringRef symbol) -> const mlir::emitrust::ItemNode * {
    for (const mlir::emitrust::ItemNode &node : graph.nodes)
      if (node.symbol == symbol)
        return &node;
    return nullptr;
  };

  // -- The naming rule, applied uniformly: an actor named "@stdout" takes
  // its smallest real owned global's symbol as the naming base (the
  // pseudo-global contributes no field and cannot name a type).
  auto namingBase = [&](const Actor &actor) -> llvm::StringRef {
    if (actor.name != "@stdout")
      return actor.name;
    for (const std::string &global : actor.globals)
      if (global != "@stdout")
        return global;
    return {};
  };
  llvm::SmallVector<std::string> typeNameOfActor(plan.actors.size());
  for (auto [i, actor] : llvm::enumerate(plan.actors)) {
    llvm::StringRef base = namingBase(actor);
    if (!base.empty())
      typeNameOfActor[i] =
          mlir::emitrust::toUpperCamelCase(base) + "Actor";
  }

  // -- The demotion table. First matching rule per actor wins.
  llvm::SmallVector<std::optional<ActorLiftDemotion>> demoted(
      plan.actors.size());
  auto demote = [&](unsigned actorIndex, llvm::StringRef reason,
                    llvm::StringRef takerSymbol = {}) {
    if (demoted[actorIndex])
      return;
    ActorLiftDemotion demotion;
    demotion.actor = plan.actors[actorIndex].name;
    demotion.reason = reason.str();
    if (!takerSymbol.empty())
      if (const mlir::emitrust::ItemNode *node = nodeOf(takerSymbol)) {
        demotion.remarkText =
            ("actor plan: the address-taking use in '" + takerSymbol +
             "' keeps actor '" + plan.actors[actorIndex].name +
             "' in the thread-local form")
                .str();
        demotion.remarkFile = node->file;
        demotion.remarkLine = node->line;
        demotion.remarkColumn = node->column;
      }
    demoted[actorIndex] = std::move(demotion);
  };

  // Rules 1 and 3, per arm/cross function.
  for (const ActorFunction &fn : plan.actorOfFunction) {
    if (fn.role != ActorRole::Arm && fn.role != ActorRole::Cross)
      continue;
    llvm::SmallVector<unsigned> targets;
    if (fn.role == ActorRole::Arm)
      targets.push_back(fn.actor);
    else
      targets.append(fn.crossActors.begin(), fn.crossActors.end());
    if (auto it = addressTakenBy.find(fn.symbol);
        it != addressTakenBy.end()) {
      for (unsigned target : targets)
        demote(target,
               "function '" + fn.symbol +
                   "' has its address taken (a fn-pointer call site "
                   "cannot thread the actor)",
               it->second);
      continue;
    }
    if (!funcOps.count(fn.symbol))
      for (unsigned target : targets)
        demote(target, "plan function '" + fn.symbol +
                           "' has no imported function (variadic "
                           "monomorphization or a recovered item)");
  }

  // Rule 1b (found by the stage-B default flip: c-testsuite 00089): an
  // owned global whose ADDRESS is taken escapes the planned actor surface.
  // The pass's IR-level veto cannot see the escape when the importer has
  // folded it away (00089's returned-pointer rewrite leaves only an
  // opaque `Some(anon)` constant that names the arm textually), so the
  // graph fact demotes at certification, with the remark at the taker.
  for (auto [i, actor] : llvm::enumerate(plan.actors))
    for (const std::string &global : actor.globals)
      if (auto it = globalAddressBy.find(global);
          it != globalAddressBy.end())
        demote((unsigned)i,
               "global '" + global +
                   "' has its address taken (the address escapes the "
                   "actor surface)",
               it->second);

  // Rule 2: poison-merged actors.
  for (auto [i, actor] : llvm::enumerate(plan.actors))
    if (actor.poisoned)
      demote(i, "condensed under indirect-call poison (an indirect call "
                "may reach any state)");

  // Rule 4: no driver, nothing constructs the actors.
  bool haveDriver =
      llvm::any_of(plan.actorOfFunction,
                   [](const ActorFunction &fn) {
                     return fn.role == ActorRole::Driver;
                   }) &&
      funcOps.count("c_main");
  if (!haveDriver)
    for (unsigned i = 0; i < plan.actors.size(); ++i)
      demote(i, "library unit has no driver (no main constructs the "
                "actors; owner-handle export is a later slice)");

  // -- Certified plan actors -> lift entries (field-less ones are simply
  // not lifted: a pure "@stdout" or const-only actor synthesizes nothing
  // and its arms stay module-level; that is not a demotion).
  llvm::SmallVector<LiftEntry> entries;
  for (auto [actorIndex, actor] : llvm::enumerate(plan.actors)) {
    if (demoted[actorIndex] || typeNameOfActor[actorIndex].empty())
      continue;
    LiftEntry entry;
    entry.planActor = (int)actorIndex;
    entry.typeName = typeNameOfActor[actorIndex];
    entry.varName =
        mlir::emitrust::toSnakeCase(namingBase(actor)) + "_actor";
    llvm::StringSet<> fieldNames;
    bool fieldCollision = false;
    for (const std::string &global : actor.globals) {
      if (global == "@stdout")
        continue;
      auto it = globalOps.find(global);
      if (it == globalOps.end())
        continue; // Skip-if-absent (importer-folded symbol).
      if (it->second.getIsConst())
        continue; // Consts stay module items (opaque path in arms).
      std::string field = derivedName(global);
      if (!fieldNames.insert(field).second)
        fieldCollision = true;
      entry.fields.push_back({global, std::move(field)});
    }
    if (fieldCollision) {
      demote(actorIndex, "two owned globals map to the same field name");
      continue;
    }
    if (entry.fields.empty())
      continue;
    for (const ActorFunction &fn : plan.actorOfFunction)
      if (fn.role == ActorRole::Arm && fn.actor == actorIndex)
        entry.arms.push_back(fn.symbol);
    entries.push_back(std::move(entry));
  }

  // -- Static cells: a non-const global outside the plan universe whose
  // every use sits in ONE top-level function.
  std::set<std::string> planUniverse;
  for (const auto &[symbol, actorIndex] : plan.actorOfGlobal)
    planUniverse.insert(symbol);
  llvm::StringMap<const ActorFunction *> planFn;
  for (const ActorFunction &fn : plan.actorOfFunction)
    planFn[fn.symbol] = &fn;
  auto entryOfPlanActor = [&](unsigned actorIndex) -> LiftEntry * {
    for (LiftEntry &entry : entries)
      if (entry.planActor == (int)actorIndex)
        return &entry;
    return nullptr;
  };
  std::map<std::string, llvm::SmallVector<mlir::emitrust::GlobalOp>>
      freeFnCells; // owner symbol -> cells
  for (mlir::Operation &op : module.getBody()->getOperations()) {
    auto global = llvm::dyn_cast<mlir::emitrust::GlobalOp>(op);
    if (!global || global.getIsConst() ||
        planUniverse.count(global.getSymName().str()))
      continue;
    mlir::emitrust::FuncOp owner = uniqueUsingFunction(global, module);
    if (!owner)
      continue; // Multi-function, method-owned, or unused: keep today's
                // form.
    auto fnIt = planFn.find(owner.getSymName());
    if (fnIt == planFn.end())
      continue;
    const ActorFunction &ownerFn = *fnIt->second;
    if (ownerFn.role == ActorRole::Arm) {
      // The cell joins its arm's actor as an extra field.
      LiftEntry *entry = entryOfPlanActor(ownerFn.actor);
      if (!entry)
        continue;
      std::string field =
          derivedName(cellBaseName(global.getSymName(), owner.getSymName()));
      if (llvm::any_of(entry->fields, [&](const auto &existing) {
            return existing.second == field;
          }))
        continue; // Name collision: the cell keeps its thread-local form.
      entry->fields.push_back(
          {global.getSymName().str(), std::move(field)});
      continue;
    }
    if (ownerFn.role != ActorRole::Free)
      continue; // Driver-/cross-owned cells keep today's form.
    if (!haveDriver || addressTakenBy.count(owner.getSymName()))
      continue;
    // Every caller must be the driver: an arm caller would need the fresh
    // actor threaded through a certified impl, which stage A does not do.
    if (auto it = callersOf.find(owner.getSymName().str());
        it != callersOf.end())
      if (!llvm::all_of(it->second, [](const std::string &caller) {
            return caller == "c_main";
          }))
        continue;
    freeFnCells[owner.getSymName().str()].push_back(global);
  }
  for (auto &[ownerSymbol, cells] : freeFnCells) {
    LiftEntry entry;
    entry.typeName =
        mlir::emitrust::toUpperCamelCase(ownerSymbol) + "Actor";
    entry.varName = mlir::emitrust::toSnakeCase(ownerSymbol) + "_actor";
    llvm::sort(cells,
               [](mlir::emitrust::GlobalOp a, mlir::emitrust::GlobalOp b) {
                 return a.getSymName() < b.getSymName();
               });
    llvm::StringSet<> fieldNames;
    for (mlir::emitrust::GlobalOp cell : cells) {
      std::string field =
          derivedName(cellBaseName(cell.getSymName(), ownerSymbol));
      if (!fieldNames.insert(field).second)
        continue;
      entry.fields.push_back({cell.getSymName().str(), std::move(field)});
    }
    if (entry.fields.empty())
      continue;
    entry.arms.push_back(ownerSymbol);
    entries.push_back(std::move(entry));
  }

  // -- Mechanical name guard: a synthesized type name colliding with an
  // existing module symbol, or duplicated among the entries, drops the
  // entry (plan actors get a demotion warning; a dropped cell actor
  // simply keeps its thread-local form).
  {
    llvm::StringSet<> seenTypeNames;
    llvm::SmallVector<LiftEntry> guarded;
    for (LiftEntry &entry : entries) {
      bool collides =
          mlir::SymbolTable::lookupSymbolIn(module, entry.typeName) !=
              nullptr ||
          !seenTypeNames.insert(entry.typeName).second;
      if (collides) {
        if (entry.planActor >= 0)
          demote(entry.planActor, "synthesized name '" + entry.typeName +
                                      "' collides with an existing symbol");
        continue;
      }
      guarded.push_back(std::move(entry));
    }
    entries = std::move(guarded);
  }

  // -- Driver-only plan actors lower to named driver locals instead of a
  // struct: no arms, no certified cross client threads them, and the IR
  // confirms every use sits in c_main.
  std::set<unsigned> crossThreadedActors;
  for (const ActorFunction &fn : plan.actorOfFunction)
    if (fn.role == ActorRole::Cross && funcOps.count(fn.symbol))
      crossThreadedActors.insert(fn.crossActors.begin(),
                                 fn.crossActors.end());
  mlir::emitrust::FuncOp driverFn =
      haveDriver ? funcOps["c_main"] : mlir::emitrust::FuncOp();
  llvm::SmallVector<std::pair<std::string, std::string>> localGlobals;
  llvm::SmallVector<LiftEntry> structEntries;
  for (LiftEntry &entry : entries) {
    bool driverOnly =
        entry.arms.empty() && driverFn && entry.planActor >= 0 &&
        !crossThreadedActors.count((unsigned)entry.planActor) &&
        llvm::all_of(entry.fields, [&](const auto &field) {
          return allUsesInside(globalOps[field.first], module, driverFn);
        });
    if (!driverOnly) {
      structEntries.push_back(std::move(entry));
      continue;
    }
    for (const auto &[globalSymbol, field] : entry.fields)
      localGlobals.push_back({globalSymbol, derivedName(globalSymbol)});
  }

  // -- Attachment.
  mlir::OpBuilder builder(context);
  if (!structEntries.empty()) {
    llvm::SmallVector<mlir::Attribute> actorDicts;
    for (const LiftEntry &entry : structEntries) {
      llvm::SmallVector<mlir::Attribute> globals;
      llvm::SmallVector<mlir::Attribute> fields;
      for (const auto &[globalSymbol, field] : entry.fields) {
        globals.push_back(builder.getStringAttr(globalSymbol));
        fields.push_back(builder.getStringAttr(field));
      }
      actorDicts.push_back(builder.getDictionaryAttr(
          {builder.getNamedAttr("name",
                                builder.getStringAttr(entry.typeName)),
           builder.getNamedAttr("var",
                                builder.getStringAttr(entry.varName)),
           builder.getNamedAttr("globals", builder.getArrayAttr(globals)),
           builder.getNamedAttr("fields",
                                builder.getArrayAttr(fields))}));
    }
    module->setAttr(mlir::emitrust::kActorLiftActorsAttrName,
                    builder.getArrayAttr(actorDicts));
    result.attachedAny = true;
  }
  if (!localGlobals.empty()) {
    llvm::SmallVector<mlir::Attribute> localDicts;
    for (const auto &[globalSymbol, name] : localGlobals)
      localDicts.push_back(builder.getDictionaryAttr(
          {builder.getNamedAttr("global",
                                builder.getStringAttr(globalSymbol)),
           builder.getNamedAttr("name", builder.getStringAttr(name))}));
    module->setAttr(mlir::emitrust::kActorLiftLocalsAttrName,
                    builder.getArrayAttr(localDicts));
    result.attachedAny = true;
  }
  if (result.attachedAny) {
    llvm::StringSet<> liftedTypeNames;
    for (const LiftEntry &entry : structEntries)
      liftedTypeNames.insert(entry.typeName);
    for (const LiftEntry &entry : structEntries)
      for (const std::string &arm : entry.arms)
        funcOps[arm]->setAttr(mlir::emitrust::kActorArmAttrName,
                              builder.getStringAttr(entry.typeName));
    for (const ActorFunction &fn : plan.actorOfFunction) {
      if (fn.role != ActorRole::Cross || !funcOps.count(fn.symbol))
        continue;
      llvm::SmallVector<mlir::Attribute> names;
      for (unsigned actorIndex : fn.crossActors)
        if (!typeNameOfActor[actorIndex].empty() &&
            liftedTypeNames.contains(typeNameOfActor[actorIndex]))
          names.push_back(
              builder.getStringAttr(typeNameOfActor[actorIndex]));
      if (!names.empty())
        funcOps[fn.symbol]->setAttr(mlir::emitrust::kActorCrossAttrName,
                                    builder.getArrayAttr(names));
    }
    if (driverFn)
      driverFn->setAttr(mlir::emitrust::kActorDriverAttrName,
                        builder.getUnitAttr());
  }

  for (std::optional<ActorLiftDemotion> &demotion : demoted)
    if (demotion)
      result.demotions.push_back(std::move(*demotion));
  return result;
}
