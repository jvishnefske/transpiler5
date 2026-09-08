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

ActorLiftAttachment emitrustcc::attachActorLiftAttributes(
    mlir::ModuleOp module, const ItemGraph &graph, const ActorPlan &plan,
    bool preserveCNames,
    llvm::ArrayRef<std::pair<unsigned, std::string>> preDemotions) {
  ActorLiftAttachment result;
  mlir::MLIRContext *context = module.getContext();
  // FR-53's verbatim opt-out applies to every name DERIVED from a C
  // spelling; synthesized type/variable names are new names and keep the
  // idiomatic rules.
  // A name derived here is MEMBER-class: it becomes an actor struct field
  // or a `c_main` binding, and never leaves that namespace. So a spelling
  // that lands on a Rust keyword takes `mangleMemberName`'s single
  // trailing underscore rather than the file-scope REJECTION global names
  // get. The rejection cannot cover this case anyway: the importer refuses
  // a global whose emitted symbol is a keyword, but under the FR-53
  // idiomatic rename `pub` emits as `PUB`, which is not one -- and
  // `toSnakeCase` turns it straight back into `pub` here. Both lowerings
  // then shipped a bare keyword and the crate failed to PARSE (measured:
  // "expected identifier, found keyword `pub`" for both the demoted local
  // `let mut pub: i32 = 5;` and the lifted field `struct PubActor { pub:
  // i32 }`), the silently-unbuildable outcome the repo forbids.
  auto derivedName = [preserveCNames](llvm::StringRef symbol) {
    std::string name =
        preserveCNames ? symbol.str() : mlir::emitrust::toSnakeCase(symbol);
    if (mlir::emitrust::isRustKeyword(name))
      name += "_";
    return name;
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

  // Rule 0 (FR-62 F1b): caller-supplied pre-demotions run before any
  // table rule, so the caller's reason (the partition path's
  // `spans workspace crates`) wins over whatever rule would have fired
  // next. The plan itself stays whole — universe and actor indices are
  // untouched — so partial demotion of a cross client's targets composes
  // through the same machinery rules 1b/2 already exercise.
  for (const auto &[actorIndex, reason] : preDemotions)
    if (actorIndex < plan.actors.size())
      demote(actorIndex, reason);

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

  // Rule 1c (found by the F2 export flip: Import/C/defer-externals.c): a
  // deferred-external global (FR-57a `emitrust.extern_decl`) is a
  // DECLARATION, not storage — another translation unit owns the state,
  // so the actor cannot own it as a field: lifting would delete the
  // declaration and silently drop the FR-58 link obligation, and a
  // synthesized new() would fabricate an initializer this TU never had.
  for (auto [i, actor] : llvm::enumerate(plan.actors))
    for (const std::string &global : actor.globals)
      if (auto it = globalOps.find(global);
          it != globalOps.end() &&
          it->second->hasAttr(mlir::emitrust::kExternDeclAttrName))
        demote((unsigned)i,
               "global '" + global +
                   "' is a deferred external declaration (another "
                   "translation unit defines it)");

  // Rule 1c, FR-70 arm: an external-REQUIREMENT global (FR-70
  // `emitrust.external_requirement`) is a declaration for the same reason —
  // the ENVIRONMENT owns the storage, supplied later through the Externals
  // trait impl — so lifting it into an actor field would delete the
  // requirement and fabricate an initializer this project never had.
  for (auto [i, actor] : llvm::enumerate(plan.actors))
    for (const std::string &global : actor.globals)
      if (auto it = globalOps.find(global);
          it != globalOps.end() &&
          it->second->hasAttr(mlir::emitrust::kExternalRequirementAttrName))
        demote((unsigned)i,
               "global '" + global +
                   "' is an external requirement (the consumer supplies "
                   "its storage)");

  // Rule 2: poison-merged actors.
  for (auto [i, actor] : llvm::enumerate(plan.actors))
    if (actor.poisoned)
      demote(i, "condensed under indirect-call poison (an indirect call "
                "may reach any state)");

  // Rule 4 (FR-62 F2): no driver, nothing constructs the actors — a
  // LIBRARY unit. The certified actors EXPORT as owner handles (pub struct
  // with private fields, synthesized `new()` carrying the C initializers)
  // instead of demoting, with two library-only guards:
  //  - an arm generic over the FR-52 external-requirements trait must not
  //    export: the emitter renders method calls without a turbofish, so
  //    every internal call site of the exported method is an E0283.
  //    Fail toward current behavior — the demoted thread-local form is the
  //    proven generic-compatible shape (lib-crate-externals-trait.c);
  //  - an arm named `new` would collide with the synthesized constructor
  //    inside the impl.
  bool haveDriver =
      llvm::any_of(plan.actorOfFunction,
                   [](const ActorFunction &fn) {
                     return fn.role == ActorRole::Driver;
                   }) &&
      funcOps.count("c_main");
  bool exportOwners = !haveDriver;
  if (exportOwners)
    for (const ActorFunction &fn : plan.actorOfFunction) {
      if (fn.role != ActorRole::Arm)
        continue;
      if (fn.symbol == "new") {
        demote(fn.actor, "arm 'new' collides with the synthesized "
                         "constructor of an exported owner");
        continue;
      }
      auto it = funcOps.find(fn.symbol);
      if (it != funcOps.end() &&
          it->second->hasAttr(mlir::emitrust::kExternalsGenericAttrName))
        demote(fn.actor,
               "arm '" + fn.symbol +
                   "' is generic over the external-requirements trait (an "
                   "exported method call cannot carry the type parameter)");
    }

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

  // -- The keyword mangle above can make two distinct globals derive ONE
  // binding name (`pub` and `pub_` both become `pub_`). Two globals must
  // never share a `c_main` binding, so the later one keeps its module-level
  // form instead of being silently merged into the earlier one -- the same
  // "collision: keep today's shape" fallback the actor-field paths take
  // (`fieldCollision` above, and the static-cell skip). The struct-field
  // side needs no guard here: its collision checks already run before the
  // driver-only split.
  {
    llvm::StringSet<> takenLocalNames;
    llvm::SmallVector<std::pair<std::string, std::string>> guardedLocals;
    for (auto &local : localGlobals)
      if (takenLocalNames.insert(local.second).second)
        guardedLocals.push_back(std::move(local));
    localGlobals = std::move(guardedLocals);
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
      llvm::SmallVector<mlir::NamedAttribute> dict{
          builder.getNamedAttr("name",
                               builder.getStringAttr(entry.typeName)),
          builder.getNamedAttr("var",
                               builder.getStringAttr(entry.varName)),
          builder.getNamedAttr("globals", builder.getArrayAttr(globals)),
          builder.getNamedAttr("fields", builder.getArrayAttr(fields))};
      // FR-62 F2: a library unit's certified actor exports as an owner
      // handle — the pass synthesizes new() and keeps the fields private.
      if (exportOwners) {
        dict.push_back(builder.getNamedAttr("export", builder.getUnitAttr()));
        if (entry.planActor >= 0)
          result.exports.push_back(
              {plan.actors[entry.planActor].name, entry.typeName});
      }
      actorDicts.push_back(builder.getDictionaryAttr(dict));
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
