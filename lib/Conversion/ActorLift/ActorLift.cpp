//===- ActorLift.cpp - FR-62 same-thread actor lift -----------------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `emitrust-actor-lift` (FR-62 slice 4; default-on since
/// stage B): the module
/// pass that rewrites a pipeline-stage module into the same-thread actor
/// shape described by the driver-attached attributes. See
/// `EmitRust/Conversion/ActorLift.h` for the attribute contract and the
/// architecture-seam decision (attributes, not a plan-text pass option),
/// and design.md's FR-62 SLICE-4 SPIKE entry for the measured mechanics the
/// rewrites below implement:
///
///  - arm bodies are rewritten against a lazily created `emitrust.deref` of
///    the new receiver argument; one `emitrust.member` place per (function,
///    field) is cached at the receiver so repeated accesses share it,
///    matching the hand-lifted reference IR;
///  - a `global_load` of a CONST global inside the impl becomes
///    `emitrust.constant <#emitrust.opaque<"NAME">>` — the impl is a
///    SymbolTable, so the module-level symbol is unreachable there and the
///    opaque path renders the bare item name;
///  - the driver constructs each actor local with `emitrust.variable named`
///    (Default-initialized) and materializes non-default field initializers
///    as post-construction member assigns (scalars through
///    `emitrust.constant`, aggregates through an initialized staging
///    variable that is loaded once and assigned whole);
///  - cross clients receive the caller's own `mut_ref` block arguments when
///    they call each other (Rust's implicit reborrow), while the driver
///    materializes a fresh `emitrust.addr_of mut` per call site;
///  - the snapshot-elision cleanup collapses the importer's whole-global
///    round-trip (`variable` filled from a `global_load`, refined by
///    subscripts, written back by a `global_store`) into direct member
///    access on the receiver. It fires only on variables whose every whole
///    fill comes from, and every whole writeback goes to, ONE cached member
///    place created by this pass, with all remaining uses being place
///    refinements — anything else keeps the 1:1 load/assign mapping, which
///    is the sound fallback.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/ActorLift.h"

#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTACTORLIFT
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;

namespace {

/// One certified actor, resolved against the module.
struct ActorInfo {
  StringAttr name; ///< The struct type name (e.g. "CounterActor").
  StringAttr var;  ///< The driver-local / cross-parameter name.
  /// The owned mutable globals in field order, with their field names.
  SmallVector<emitrust::GlobalOp> globals;
  SmallVector<StringAttr> fieldNames;
  emitrust::StructType structType;
  emitrust::ImplOp impl;   ///< The synthesized impl (once created).
  Value driverLocal;       ///< The driver's actor variable (lvalue).
  bool vetoed = false;     ///< Safety-net demotion: leave untouched.
};

/// One driver-only global lowered to a named `c_main` local.
struct LocalInfo {
  emitrust::GlobalOp global;
  StringAttr name;
  Value driverLocal; ///< The named variable (lvalue), once created.
  bool vetoed = false;
};

/// Per-function rewrite context: which actor lvalue serves as the receiver
/// place, and the cached member places.
struct FnContext {
  emitrust::FuncOp fn;
  /// actor index -> receiver LVALUE (deref of a mut_ref argument for
  /// arms/cross clients, the actor variable itself for the driver). Null
  /// until first use for arms/cross (lazy deref creation).
  DenseMap<unsigned, Value> receiver;
  /// actor index -> the raw mut_ref block argument (cross clients only);
  /// passed through unchanged on cross-to-cross calls.
  DenseMap<unsigned, Value> argRef;
  /// (actor index, field index) -> cached member place.
  DenseMap<std::pair<unsigned, unsigned>, Value> members;
  /// Where lazily created derefs/members are inserted: immediately after
  /// this operation (or at the block start when null).
  Operation *placeAnchor = nullptr;
  bool isDriver = false;
};

struct ActorLift
    : public mlir::emitrust::impl::EmitRustActorLiftBase<ActorLift> {
  using EmitRustActorLiftBase::EmitRustActorLiftBase;

  ModuleOp module;
  SmallVector<ActorInfo> actors;
  SmallVector<LocalInfo> locals;
  /// global symbol -> (actor index, field index).
  llvm::StringMap<std::pair<unsigned, unsigned>> fieldOfGlobal;
  /// global symbol -> index into `locals`.
  llvm::StringMap<unsigned> localOfGlobal;
  /// arm symbol -> actor index (surviving actors only).
  llvm::StringMap<unsigned> armActor;
  /// cross symbol -> surviving actor indices, parameter order.
  llvm::StringMap<SmallVector<unsigned>> crossActors;
  /// The member places this pass created; the snapshot elision only
  /// collapses round-trips against these.
  DenseSet<Value> createdMemberPlaces;

  void runOnOperation() override {
    module = getOperation();
    auto actorsAttr =
        module->getAttrOfType<ArrayAttr>(emitrust::kActorLiftActorsAttrName);
    auto localsAttr =
        module->getAttrOfType<ArrayAttr>(emitrust::kActorLiftLocalsAttrName);
    if (!actorsAttr && !localsAttr)
      return; // The common case: nothing planned, nothing touched.

    if (failed(parseActors(actorsAttr)) || failed(parseLocals(localsAttr)))
      return signalPassFailure();
    vetoUnliftable();
    buildFunctionMaps();

    // Synthesis and rewriting. Any structural surprise from here on is a
    // hard pass failure: partial lifting must never emit silently.
    synthesizeStructs();
    if (failed(rewriteFunctions()))
      return signalPassFailure();
    elideSnapshots();
    if (failed(deleteLiftedGlobals()))
      return signalPassFailure();
    stripAttributes();
  }

  /// Parses the module-level actor spec. Malformed entries are producer
  /// bugs and fail loudly.
  LogicalResult parseActors(ArrayAttr actorsAttr) {
    if (!actorsAttr)
      return success();
    for (Attribute entry : actorsAttr) {
      auto dict = dyn_cast<DictionaryAttr>(entry);
      if (!dict)
        return module.emitError("malformed ")
               << emitrust::kActorLiftActorsAttrName
               << " entry: expected a dictionary";
      ActorInfo info;
      info.name = dict.getAs<StringAttr>("name");
      info.var = dict.getAs<StringAttr>("var");
      auto globalsAttr = dict.getAs<ArrayAttr>("globals");
      auto fieldsAttr = dict.getAs<ArrayAttr>("fields");
      if (!info.name || !info.var || !globalsAttr || !fieldsAttr ||
          globalsAttr.size() != fieldsAttr.size())
        return module.emitError("malformed ")
               << emitrust::kActorLiftActorsAttrName
               << " entry: expected {name, var, globals, fields} with "
                  "parallel globals/fields arrays";
      for (auto [globalAttr, fieldAttr] :
           llvm::zip_equal(globalsAttr, fieldsAttr)) {
        auto globalName = dyn_cast<StringAttr>(globalAttr);
        auto fieldName = dyn_cast<StringAttr>(fieldAttr);
        if (!globalName || !fieldName)
          return module.emitError("malformed ")
                 << emitrust::kActorLiftActorsAttrName
                 << " entry: globals/fields must be string arrays";
        auto global = dyn_cast_or_null<emitrust::GlobalOp>(
            SymbolTable::lookupSymbolIn(module, globalName));
        if (!global)
          continue; // Skip-if-absent: the plan may own folded symbols.
        if (global.getIsConst())
          return global.emitError("actor lift: const global '")
                 << globalName.getValue()
                 << "' cannot be an actor field (the driver keeps consts "
                    "as module items)";
        info.globals.push_back(global);
        info.fieldNames.push_back(fieldName);
      }
      if (info.globals.empty())
        continue; // Field-less actor (pure @stdout / const-only): no lift.
      info.structType =
          emitrust::StructType::get(&getContext(), info.name.getValue());
      unsigned actorIndex = actors.size();
      for (auto [fieldIndex, global] : llvm::enumerate(info.globals))
        fieldOfGlobal[global.getSymName()] = {actorIndex,
                                              (unsigned)fieldIndex};
      actors.push_back(std::move(info));
    }
    return success();
  }

  /// Parses the driver-only-global spec.
  LogicalResult parseLocals(ArrayAttr localsAttr) {
    if (!localsAttr)
      return success();
    for (Attribute entry : localsAttr) {
      auto dict = dyn_cast<DictionaryAttr>(entry);
      auto globalName = dict ? dict.getAs<StringAttr>("global") : StringAttr();
      auto name = dict ? dict.getAs<StringAttr>("name") : StringAttr();
      if (!globalName || !name)
        return module.emitError("malformed ")
               << emitrust::kActorLiftLocalsAttrName
               << " entry: expected {global, name}";
      auto global = dyn_cast_or_null<emitrust::GlobalOp>(
          SymbolTable::lookupSymbolIn(module, globalName));
      if (!global)
        continue; // Skip-if-absent.
      if (global.getIsConst())
        return global.emitError("actor lift: const global '")
               << globalName.getValue() << "' cannot become a driver local";
      localOfGlobal[global.getSymName()] = locals.size();
      locals.push_back({global, name, Value(), false});
    }
    return success();
  }

  /// Whether `fn` is attributed to actor `name`'s surface (arm of it,
  /// cross client listing it, or the driver).
  static bool onActorSurface(emitrust::FuncOp fn, StringAttr name) {
    if (fn->hasAttr(emitrust::kActorDriverAttrName))
      return true;
    if (auto arm =
            fn->getAttrOfType<StringAttr>(emitrust::kActorArmAttrName))
      if (arm == name)
        return true;
    if (auto cross =
            fn->getAttrOfType<ArrayAttr>(emitrust::kActorCrossAttrName))
      for (Attribute member : cross)
        if (member == name)
          return true;
    return false;
  }

  /// The safety-net veto (see the header): re-verify on the IR that every
  /// use of every owned global is a load/store inside an attributed
  /// function, and that no ARM touches a mutable global its actor does not
  /// own (an importer-synthesized backing global, invisible to the plan,
  /// would otherwise hit the impl-SymbolTable wall); otherwise demote the
  /// actor with a warning and leave its globals and functions untouched.
  void vetoUnliftable() {
    // A GlobalOp initializer the lift restates verbatim as a VariableOp
    // initializer (driver local, field-init staging variable). The ONE
    // GlobalOp-legal shape VariableOp rejects is a fn_ptr's opaque
    // `None`/`Some(f)` expression — there is no typed attribute for
    // function references — so it must demote rather than hit the
    // verifier (found by the stage-B default flip: c-testsuite 00088's
    // `int (*fptr)() = 0;`).
    auto initRestatable = [](Attribute init) {
      return !init || isa<ArrayAttr>(init) || isa<TypedAttr>(init);
    };
    auto isRewritableUse = [&](Operation *user, StringAttr actorName,
                               bool driverOnly) {
      if (!isa<emitrust::GlobalLoadOp, emitrust::GlobalStoreOp>(user))
        return false;
      auto fn = user->getParentOfType<emitrust::FuncOp>();
      if (!fn)
        return false;
      if (driverOnly)
        return fn->hasAttr(emitrust::kActorDriverAttrName);
      return onActorSurface(fn, actorName);
    };
    for (ActorInfo &actor : actors) {
      for (emitrust::GlobalOp global : actor.globals) {
        if (!initRestatable(global.getInitAttr())) {
          global.emitWarning("actor lift: demoted ")
              << actor.name.getValue() << ": global '" << global.getSymName()
              << "' carries an initializer with no local restatement "
                 "(fn_ptr opaque init)";
          actor.vetoed = true;
          break;
        }
        auto uses = SymbolTable::getSymbolUses(global, module);
        bool bad =
            uses && llvm::any_of(*uses, [&](const SymbolTable::SymbolUse &u) {
              return !isRewritableUse(u.getUser(), actor.name,
                                      /*driverOnly=*/false);
            });
        if (!bad)
          continue;
        global.emitWarning("actor lift: demoted ")
            << actor.name.getValue() << ": global '" << global.getSymName()
            << "' has a use outside the planned actor surface";
        actor.vetoed = true;
        break;
      }
      // An arm moves into the impl (a SymbolTable), where a mutable global
      // the actor does not own is unreachable — a shape the plan cannot
      // see when the global is importer-synthesized (e.g. a string-literal
      // `_BACKING` array). Demote rather than fail: today's thread-local
      // form is the sound fallback.
      if (!actor.vetoed) {
        llvm::StringSet<> owned;
        for (emitrust::GlobalOp global : actor.globals)
          owned.insert(global.getSymName());
        module.walk([&](emitrust::FuncOp fn) {
          if (actor.vetoed)
            return;
          auto arm =
              fn->getAttrOfType<StringAttr>(emitrust::kActorArmAttrName);
          if (arm != actor.name)
            return;
          fn.walk([&](Operation *op) {
            StringRef symbol;
            if (auto load = dyn_cast<emitrust::GlobalLoadOp>(op))
              symbol = load.getGlobal();
            else if (auto store = dyn_cast<emitrust::GlobalStoreOp>(op))
              symbol = store.getGlobal();
            else
              return;
            if (actor.vetoed || owned.contains(symbol))
              return;
            auto global = dyn_cast_or_null<emitrust::GlobalOp>(
                SymbolTable::lookupSymbolIn(module, symbol));
            if (global && global.getIsConst())
              return; // The opaque-constant path covers consts.
            op->emitWarning("actor lift: demoted ")
                << actor.name.getValue() << ": arm '" << fn.getSymName()
                << "' touches mutable global '" << symbol
                << "' the actor does not own";
            actor.vetoed = true;
          });
        });
      }
      if (actor.vetoed)
        for (emitrust::GlobalOp global : actor.globals)
          fieldOfGlobal.erase(global.getSymName());
    }
    for (LocalInfo &local : locals) {
      if (!initRestatable(local.global.getInitAttr())) {
        local.global.emitWarning("actor lift: demoted ")
            << local.name.getValue() << ": global '"
            << local.global.getSymName()
            << "' carries an initializer with no local restatement "
               "(fn_ptr opaque init)";
        local.vetoed = true;
        localOfGlobal.erase(local.global.getSymName());
        continue;
      }
      auto uses = SymbolTable::getSymbolUses(local.global, module);
      bool bad =
          uses && llvm::any_of(*uses, [&](const SymbolTable::SymbolUse &u) {
            return !isRewritableUse(u.getUser(), StringAttr(),
                                    /*driverOnly=*/true);
          });
      if (!bad)
        continue;
      local.global.emitWarning("actor lift: demoted ")
          << local.name.getValue() << ": global '"
          << local.global.getSymName()
          << "' has a use outside the driver";
      local.vetoed = true;
      localOfGlobal.erase(local.global.getSymName());
    }
  }

  /// Indexes the function attributes against the surviving actors.
  void buildFunctionMaps() {
    llvm::StringMap<unsigned> actorIndexByName;
    for (auto [index, actor] : llvm::enumerate(actors))
      if (!actor.vetoed)
        actorIndexByName[actor.name.getValue()] = index;
    module.walk([&](emitrust::FuncOp fn) {
      if (auto arm =
              fn->getAttrOfType<StringAttr>(emitrust::kActorArmAttrName)) {
        auto it = actorIndexByName.find(arm.getValue());
        if (it != actorIndexByName.end())
          armActor[fn.getSymName()] = it->second;
      }
      if (auto cross =
              fn->getAttrOfType<ArrayAttr>(emitrust::kActorCrossAttrName)) {
        SmallVector<unsigned> list;
        for (Attribute member : cross)
          if (auto name = dyn_cast<StringAttr>(member)) {
            auto it = actorIndexByName.find(name.getValue());
            if (it != actorIndexByName.end())
              list.push_back(it->second);
          }
        if (!list.empty())
          crossActors[fn.getSymName()] = std::move(list);
      }
    });
  }

  /// Creates each surviving actor's struct_def at its first owned global's
  /// position, with the impl immediately after it.
  void synthesizeStructs() {
    OpBuilder builder(&getContext());
    for (ActorInfo &actor : actors) {
      if (actor.vetoed)
        continue;
      emitrust::GlobalOp anchor = actor.globals.front();
      builder.setInsertionPoint(anchor);
      SmallVector<Attribute> names;
      SmallVector<Attribute> types;
      for (auto [fieldName, global] :
           llvm::zip_equal(actor.fieldNames, actor.globals)) {
        names.push_back(fieldName);
        types.push_back(TypeAttr::get(global.getType()));
      }
      builder.create<emitrust::StructDefOp>(
          anchor.getLoc(), actor.name, builder.getArrayAttr(names),
          builder.getArrayAttr(types));
      actor.impl = builder.create<emitrust::ImplOp>(anchor.getLoc(),
                                                    actor.name.getValue());
      actor.impl.getBody().emplaceBlock();
    }
  }

  /// The receiver lvalue for `actorIndex` inside `context`, creating the
  /// lazy `deref` (arms/cross) on first use. Returns null if the function
  /// has no access to that actor — a plan/IR inconsistency the caller
  /// reports.
  Value receiverPlace(FnContext &context, unsigned actorIndex) {
    auto it = context.receiver.find(actorIndex);
    if (it != context.receiver.end() && it->second)
      return it->second;
    auto argIt = context.argRef.find(actorIndex);
    if (argIt == context.argRef.end())
      return Value();
    OpBuilder builder(&getContext());
    if (context.placeAnchor)
      builder.setInsertionPointAfter(context.placeAnchor);
    else
      builder.setInsertionPointToStart(&context.fn.getBody().front());
    Value ref = argIt->second;
    auto deref = builder.create<emitrust::DerefOp>(
        ref.getLoc(), emitrust::LValueType::get(actors[actorIndex].structType),
        ref);
    context.placeAnchor = deref;
    context.receiver[actorIndex] = deref.getResult();
    return deref.getResult();
  }

  /// The cached member place for (actor, field) inside `context`.
  Value memberPlace(FnContext &context, unsigned actorIndex,
                    unsigned fieldIndex) {
    auto key = std::make_pair(actorIndex, fieldIndex);
    auto it = context.members.find(key);
    if (it != context.members.end())
      return it->second;
    Value receiver = receiverPlace(context, actorIndex);
    if (!receiver)
      return Value();
    OpBuilder builder(&getContext());
    if (context.placeAnchor)
      builder.setInsertionPointAfter(context.placeAnchor);
    else
      builder.setInsertionPointToStart(&context.fn.getBody().front());
    ActorInfo &actor = actors[actorIndex];
    auto member = builder.create<emitrust::MemberOp>(
        receiver.getLoc(),
        emitrust::LValueType::get(actor.globals[fieldIndex].getType()),
        receiver, actor.fieldNames[fieldIndex]);
    context.placeAnchor = member;
    context.members[key] = member.getResult();
    createdMemberPlaces.insert(member.getResult());
    return member.getResult();
  }

  /// Prepends `count` entries to a function's `emitrust.param_names` attr
  /// (creating it when new names are non-empty), keeping one slot per
  /// signature input.
  void prependParamNames(emitrust::FuncOp fn, ArrayRef<StringAttr> names,
                         unsigned oldInputCount) {
    auto existing = fn->getAttrOfType<ArrayAttr>(
        mlir::emitrust::kParamNamesAttrName);
    bool anyNamed = llvm::any_of(
        names, [](StringAttr name) { return !name.getValue().empty(); });
    if (!existing && !anyNamed)
      return; // All-empty attr stays omitted (the FR-61e convention).
    SmallVector<Attribute> slots(names.begin(), names.end());
    if (existing)
      slots.append(existing.begin(), existing.end());
    else
      slots.append(oldInputCount,
                   StringAttr::get(&getContext(), ""));
    fn->setAttr(mlir::emitrust::kParamNamesAttrName,
                ArrayAttr::get(&getContext(), slots));
  }

  /// Prepends mut_ref inputs for `actorIndices` to `fn`'s signature and
  /// entry block; records the new block arguments in `context.argRef`.
  void prependReceiverArgs(emitrust::FuncOp fn, ArrayRef<unsigned> actorIndices,
                           FnContext &context) {
    unsigned oldInputCount = fn.getFunctionType().getNumInputs();
    SmallVector<Type> inputs;
    for (unsigned actorIndex : actorIndices)
      inputs.push_back(
          emitrust::MutRefType::get(actors[actorIndex].structType));
    inputs.append(fn.getFunctionType().getInputs().begin(),
                  fn.getFunctionType().getInputs().end());
    fn.setFunctionTypeAttr(TypeAttr::get(FunctionType::get(
        &getContext(), inputs, fn.getFunctionType().getResults())));
    Block &entry = fn.getBody().front();
    for (auto [position, actorIndex] : llvm::enumerate(actorIndices)) {
      entry.insertArgument((unsigned)position,
                           inputs[position], fn.getLoc());
      context.argRef[actorIndex] = entry.getArgument(position);
    }
    // Keep an existing per-argument attr array in sync with the new arity.
    if (ArrayAttr argAttrs = fn.getArgAttrsAttr()) {
      SmallVector<Attribute> updated(
          actorIndices.size(), DictionaryAttr::get(&getContext(), {}));
      updated.append(argAttrs.begin(), argAttrs.end());
      fn.setArgAttrsAttr(ArrayAttr::get(&getContext(), updated));
    }
    (void)oldInputCount;
  }

  /// Rewrites every global access and call in `context.fn`. `insideImpl`
  /// selects the opaque-constant path for const globals.
  LogicalResult rewriteBody(FnContext &context, bool insideImpl) {
    emitrust::FuncOp fn = context.fn;
    SmallVector<Operation *> worklist;
    fn.walk([&](Operation *op) {
      if (isa<emitrust::GlobalLoadOp, emitrust::GlobalStoreOp,
              emitrust::CallOpaqueOp>(op))
        worklist.push_back(op);
    });
    for (Operation *op : worklist) {
      OpBuilder builder(op);
      if (auto load = dyn_cast<emitrust::GlobalLoadOp>(op)) {
        StringRef symbol = load.getGlobal();
        if (auto it = fieldOfGlobal.find(symbol); it != fieldOfGlobal.end()) {
          Value member =
              memberPlace(context, it->second.first, it->second.second);
          if (!member)
            return load.emitError(
                "actor lift: global '" + symbol +
                "' is accessed outside its actor's planned surface");
          auto value = builder.create<emitrust::LoadOp>(
              load.getLoc(), load.getType(), member);
          load.getResult().replaceAllUsesWith(value.getResult());
          load.erase();
          continue;
        }
        if (auto it = localOfGlobal.find(symbol);
            it != localOfGlobal.end() && context.isDriver) {
          auto value = builder.create<emitrust::LoadOp>(
              load.getLoc(), load.getType(),
              locals[it->second].driverLocal);
          load.getResult().replaceAllUsesWith(value.getResult());
          load.erase();
          continue;
        }
        if (insideImpl) {
          auto global = dyn_cast_or_null<emitrust::GlobalOp>(
              SymbolTable::lookupSymbolIn(module, load.getGlobalAttr()));
          if (!global || !global.getIsConst())
            return load.emitError("actor lift: mutable global '")
                   << symbol << "' is not owned by the enclosing actor";
          // The impl-SymbolTable wall: reference the const through the
          // opaque-constant path, which renders the bare item name.
          auto constant = builder.create<emitrust::ConstantOp>(
              load.getLoc(), load.getType(),
              emitrust::OpaqueAttr::get(&getContext(), symbol));
          load.getResult().replaceAllUsesWith(constant.getResult());
          load.erase();
        }
        continue;
      }
      if (auto store = dyn_cast<emitrust::GlobalStoreOp>(op)) {
        StringRef symbol = store.getGlobal();
        if (auto it = fieldOfGlobal.find(symbol); it != fieldOfGlobal.end()) {
          Value member =
              memberPlace(context, it->second.first, it->second.second);
          if (!member)
            return store.emitError(
                "actor lift: global '" + symbol +
                "' is accessed outside its actor's planned surface");
          builder.create<emitrust::AssignOp>(store.getLoc(), member,
                                             store.getValue());
          store.erase();
          continue;
        }
        if (auto it = localOfGlobal.find(symbol);
            it != localOfGlobal.end() && context.isDriver) {
          builder.create<emitrust::AssignOp>(
              store.getLoc(), locals[it->second].driverLocal,
              store.getValue());
          store.erase();
          continue;
        }
        if (insideImpl)
          return store.emitError("actor lift: mutable global '")
                 << symbol << "' is not owned by the enclosing actor";
        continue;
      }
      auto call = cast<emitrust::CallOpaqueOp>(op);
      StringRef callee = call.getCallee();
      if (auto it = armActor.find(callee); it != armActor.end()) {
        if (call.getArgs())
          return call.emitError("actor lift: cannot rewrite a call to arm '")
                 << callee << "' carrying an args attribute";
        Value receiver = receiverPlace(context, it->second);
        if (!receiver)
          return call.emitError("actor lift: caller of arm '")
                 << callee << "' has no access to actor '"
                 << actors[it->second].name.getValue() << "'";
        auto method = builder.create<emitrust::MethodCallOp>(
            call.getLoc(), call.getResultTypes(), receiver,
            builder.getStringAttr(callee), call.getOperands());
        call.replaceAllUsesWith(method.getResults());
        call.erase();
        continue;
      }
      if (auto it = crossActors.find(callee); it != crossActors.end()) {
        if (call.getArgs())
          return call.emitError(
                     "actor lift: cannot rewrite a call to cross client '")
                 << callee << "' carrying an args attribute";
        SmallVector<Value> operands;
        for (unsigned actorIndex : it->second) {
          Value ref;
          if (context.isDriver) {
            // A fresh &mut per call site (the E1/E5 measured shape).
            const ActorInfo &actor = actors[actorIndex];
            ref = builder.create<emitrust::AddrOfOp>(
                call.getLoc(), emitrust::MutRefType::get(actor.structType),
                actor.driverLocal, /*is_mut=*/true);
          } else {
            // Cross-to-cross: pass the own parameter through (implicit
            // reborrow).
            auto argIt = context.argRef.find(actorIndex);
            if (argIt == context.argRef.end())
              return call.emitError("actor lift: caller of cross client '")
                     << callee << "' has no access to actor '"
                     << actors[actorIndex].name.getValue() << "'";
            ref = argIt->second;
          }
          operands.push_back(ref);
        }
        operands.append(call.getOperands().begin(),
                        call.getOperands().end());
        auto rewritten = builder.create<emitrust::CallOpaqueOp>(
            call.getLoc(), call.getResultTypes(),
            builder.getStringAttr(callee), /*args=*/ArrayAttr(), operands);
        call.replaceAllUsesWith(rewritten.getResults());
        call.erase();
        continue;
      }
    }
    return success();
  }

  /// Materializes one field's initializer as a post-construction member
  /// assign in the driver.
  void materializeFieldInit(OpBuilder &builder, Location loc, Value actorVar,
                            StringAttr fieldName, emitrust::GlobalOp global) {
    Attribute init = global.getInitAttr();
    auto member = builder.create<emitrust::MemberOp>(
        loc, emitrust::LValueType::get(global.getType()), actorVar,
        fieldName);
    Value value;
    if (isa<IntegerType, FloatType, IndexType>(global.getType())) {
      value = builder
                  .create<emitrust::ConstantOp>(loc, global.getType(), init)
                  .getResult();
    } else {
      // Aggregate initializer: stage it in an initialized variable, load
      // once, assign whole. The staging variable renders as one `let`.
      auto staged = builder.create<emitrust::VariableOp>(
          loc, emitrust::LValueType::get(global.getType()), init,
          /*isConst=*/true);
      value = builder
                  .create<emitrust::LoadOp>(loc, global.getType(),
                                            staged.getResult())
                  .getResult();
    }
    builder.create<emitrust::AssignOp>(loc, member.getResult(), value);
  }

  /// Rewrites arms, cross clients, and the driver; moves arms into their
  /// impls.
  LogicalResult rewriteFunctions() {
    // Collect the top-level functions first: moving arms during iteration
    // would invalidate the module walk.
    SmallVector<emitrust::FuncOp> functions;
    for (auto fn : module.getOps<emitrust::FuncOp>())
      functions.push_back(fn);

    for (emitrust::FuncOp fn : functions) {
      if (auto it = armActor.find(fn.getSymName()); it != armActor.end()) {
        unsigned actorIndex = it->second;
        FnContext context;
        context.fn = fn;
        prependReceiverArgs(fn, {actorIndex}, context);
        prependParamNames(fn, {StringAttr::get(&getContext(), "")},
                          fn.getFunctionType().getNumInputs() - 1);
        if (failed(rewriteBody(context, /*insideImpl=*/true)))
          return failure();
        Block &implBlock = actors[actorIndex].impl.getBody().front();
        fn->moveBefore(&implBlock, implBlock.end());
        continue;
      }
      if (auto it = crossActors.find(fn.getSymName());
          it != crossActors.end()) {
        FnContext context;
        context.fn = fn;
        prependReceiverArgs(fn, it->second, context);
        SmallVector<StringAttr> names;
        for (unsigned actorIndex : it->second)
          names.push_back(actors[actorIndex].var);
        prependParamNames(fn, names,
                          fn.getFunctionType().getNumInputs() -
                              it->second.size());
        if (failed(rewriteBody(context, /*insideImpl=*/false)))
          return failure();
        continue;
      }
      if (fn->hasAttr(emitrust::kActorDriverAttrName)) {
        FnContext context;
        context.fn = fn;
        context.isDriver = true;
        OpBuilder builder(&getContext());
        Block &entry = fn.getBody().front();
        builder.setInsertionPointToStart(&entry);
        for (unsigned actorIndex = 0; actorIndex < actors.size();
             ++actorIndex) {
          ActorInfo &actor = actors[actorIndex];
          if (actor.vetoed)
            continue;
          auto variable = builder.create<emitrust::VariableOp>(
              fn.getLoc(), emitrust::LValueType::get(actor.structType),
              /*init=*/Attribute(), /*isConst=*/false,
              actor.var.getValue());
          actor.driverLocal = variable.getResult();
          context.receiver[actorIndex] = variable.getResult();
          for (unsigned fieldIndex = 0; fieldIndex < actor.globals.size();
               ++fieldIndex)
            if (actor.globals[fieldIndex].getInitAttr())
              materializeFieldInit(builder, fn.getLoc(),
                                   variable.getResult(),
                                   actor.fieldNames[fieldIndex],
                                   actor.globals[fieldIndex]);
        }
        for (LocalInfo &local : locals) {
          if (local.vetoed)
            continue;
          auto variable = builder.create<emitrust::VariableOp>(
              fn.getLoc(),
              emitrust::LValueType::get(local.global.getType()),
              local.global.getInitAttr(), /*isConst=*/false,
              local.name.getValue());
          local.driverLocal = variable.getResult();
        }
        // Lazily created member caches insert after the construction ops
        // (at the block start when nothing was constructed).
        context.placeAnchor =
            builder.getInsertionPoint() == entry.begin()
                ? nullptr
                : &*std::prev(builder.getInsertionPoint());
        if (failed(rewriteBody(context, /*insideImpl=*/false)))
          return failure();
        continue;
      }
    }
    return success();
  }

  /// The snapshot-elision cleanup (see the file header): collapses
  /// variable-staged whole-global round-trips onto the direct member place.
  void elideSnapshots() {
    SmallVector<emitrust::VariableOp> candidates;
    module.walk([&](emitrust::VariableOp variable) {
      if (!variable.getInitAttr() && !variable.getIsConst() &&
          !variable.getCName())
        candidates.push_back(variable);
    });
    for (emitrust::VariableOp variable : candidates) {
      Value place;
      SmallVector<emitrust::AssignOp> fills;
      SmallVector<emitrust::LoadOp> fillLoads;
      SmallVector<std::pair<emitrust::LoadOp, emitrust::AssignOp>> writebacks;
      bool eligible = true;
      for (Operation *user : variable.getResult().getUsers()) {
        if (auto assign = dyn_cast<emitrust::AssignOp>(user)) {
          if (assign.getVar() != variable.getResult()) {
            eligible = false;
            break;
          }
          auto load = assign.getValue().getDefiningOp<emitrust::LoadOp>();
          if (!load || !load.getResult().hasOneUse() ||
              !createdMemberPlaces.contains(load.getOperand()) ||
              (place && load.getOperand() != place)) {
            eligible = false;
            break;
          }
          place = load.getOperand();
          fills.push_back(assign);
          fillLoads.push_back(load);
          continue;
        }
        if (auto load = dyn_cast<emitrust::LoadOp>(user)) {
          if (!load.getResult().hasOneUse()) {
            eligible = false;
            break;
          }
          auto assign = dyn_cast<emitrust::AssignOp>(
              *load.getResult().getUsers().begin());
          if (!assign || assign.getValue() != load.getResult() ||
              !createdMemberPlaces.contains(assign.getVar()) ||
              (place && assign.getVar() != place)) {
            eligible = false;
            break;
          }
          place = assign.getVar();
          writebacks.push_back({load, assign});
          continue;
        }
        if (isa<emitrust::SubscriptOp, emitrust::MemberOp>(user))
          continue; // Place refinements follow the variable to the member.
        eligible = false;
        break;
      }
      if (!eligible || !place || fills.empty())
        continue;
      for (auto [assign, load] : llvm::zip_equal(fills, fillLoads)) {
        assign.erase();
        load.erase();
      }
      for (auto [load, assign] : writebacks) {
        assign.erase();
        load.erase();
      }
      variable.getResult().replaceAllUsesWith(place);
      variable.erase();
    }
  }

  /// Deletes the lifted globals, insisting that no use survived: a missed
  /// rewrite must fail the compile here, never surface as broken Rust.
  LogicalResult deleteLiftedGlobals() {
    SmallVector<emitrust::GlobalOp> doomed;
    for (const ActorInfo &actor : actors)
      if (!actor.vetoed)
        doomed.append(actor.globals.begin(), actor.globals.end());
    for (const LocalInfo &local : locals)
      if (!local.vetoed)
        doomed.push_back(local.global);
    for (emitrust::GlobalOp global : doomed) {
      if (!SymbolTable::symbolKnownUseEmpty(global, module))
        return global.emitError("actor lift: internal: lifted global '")
               << global.getSymName() << "' still has uses";
      global.erase();
    }
    return success();
  }

  /// Strips the consumed attributes from the module and every function
  /// (including arms already moved into impls, and functions of vetoed
  /// actors — the attributes are pass input, not program).
  void stripAttributes() {
    module->removeAttr(emitrust::kActorLiftActorsAttrName);
    module->removeAttr(emitrust::kActorLiftLocalsAttrName);
    module.walk([](emitrust::FuncOp fn) {
      fn->removeAttr(emitrust::kActorArmAttrName);
      fn->removeAttr(emitrust::kActorCrossAttrName);
      fn->removeAttr(emitrust::kActorDriverAttrName);
    });
  }
};

} // namespace
