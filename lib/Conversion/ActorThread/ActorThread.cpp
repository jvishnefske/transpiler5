//===- ActorThread.cpp - FR-62 threaded-mode actor rewrite ----------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `emitrust-actor-thread` (FR-62 slice 5b): the module pass
/// that rewrites lifted actors selected by the driver-computed
/// `emitrust.actor_thread` attribute into the threaded-runtime driver
/// shape. See `EmitRust/Conversion/ActorThread.h` for the attribute
/// contract and the eligibility vetoes, and design.md's FR-62 SLICE-5b
/// SPIKE entry for the measured mechanics (the hand-written post-threadify
/// prototype this pass reproduces):
///
///  - accessor synthesis: one `get_<field>`/`set_<field>` impl func per
///    driver-accessed field, scalar (`get() -> T` / `set(v: T)`) or
///    array-element (`get(i) -> E` / `set(i, v)`), with the receiver
///    deref/member/subscript body and `emitrust.param_names` slots exactly
///    as the spike's reference IR;
///  - the driver rewrite is ORDER-PRESERVING: each member/subscript load
///    becomes a `get_` method_call at the load's position, each assign a
///    `set_` method_call at the assign's position (a slice-4
///    post-construction member-assign initializer is the same shape, so it
///    becomes a `set_` call through the same rewrite);
///  - the actor local is retyped `!emitrust.lvalue<!emitrust.opaque<
///    "<Actor>Handle">>` and its construction becomes
///    `%st = call_opaque "<Actor>::default"()` before the variable plus
///    `%sp = call_opaque "<Actor>Handle::spawn"(%st)` + assign after it
///    (the deferred-init `let mut h: Handle; h = spawn(..)` render);
///  - `.shutdown()` is a plain method_call inserted before every driver
///    return; the `emitrust.actor_runtime` anchor lands after the impl.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/ActorThread.h"

#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTACTORTHREAD
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;

namespace {

/// Whether `type` may be the value behind a scalar accessor (or an array
/// element behind an element accessor): a scalar or an open C enum. Structs
/// and arrays are deliberately OUT — a whole-aggregate accessor would be a
/// snapshot with copy semantics the driver's access pattern never proved,
/// so those shapes veto instead.
static bool isAccessorScalarType(Type type) {
  return isa<IntegerType, IndexType, FloatType, emitrust::EnumType>(type);
}

/// One driver-accessed field's accessor plan.
struct FieldAccess {
  StringAttr fieldName;
  Type fieldType;         ///< The struct field's type.
  unsigned defIndex = 0;  ///< Field position in the struct_def.
  bool element = false;   ///< Array-element accessors (vs whole scalar).
  Type indexType;         ///< Subscript index type (element only).
  bool needsGet = false;
  bool needsSet = false;
};

/// The per-actor rewrite work list, gathered before any mutation.
struct DriverRewrite {
  /// Scalar loads: load of a member place -> get_<field>().
  SmallVector<std::pair<emitrust::LoadOp, unsigned>> scalarGets;
  /// Scalar assigns: assign member = v -> set_<field>(v).
  SmallVector<std::pair<emitrust::AssignOp, unsigned>> scalarSets;
  /// Element loads: load of subscript(member, i) -> get_<field>(i).
  SmallVector<std::pair<emitrust::LoadOp, unsigned>> elementGets;
  /// Element assigns: assign subscript(member, i) = v -> set_<field>(i, v).
  SmallVector<std::pair<emitrust::AssignOp, unsigned>> elementSets;
  SmallVector<emitrust::SubscriptOp> subscripts;
  SmallVector<emitrust::MemberOp> members;
  SmallVector<FieldAccess> fields; ///< Indexed by the pairs above.
};

struct ActorThread
    : public mlir::emitrust::impl::EmitRustActorThreadBase<ActorThread> {
  using EmitRustActorThreadBase::EmitRustActorThreadBase;

  ModuleOp module;

  void runOnOperation() override {
    module = getOperation();
    auto listAttr =
        module->getAttrOfType<ArrayAttr>(emitrust::kActorThreadAttrName);
    if (!listAttr)
      return; // The common case: nothing selected, nothing touched.
    for (Attribute entry : listAttr) {
      auto dict = dyn_cast<DictionaryAttr>(entry);
      auto name = dict ? dict.getAs<StringAttr>("name") : StringAttr();
      auto modeStr = dict ? dict.getAs<StringAttr>("mode") : StringAttr();
      std::optional<emitrust::ActorMode> mode =
          modeStr ? emitrust::symbolizeActorMode(modeStr.getValue())
                  : std::nullopt;
      if (!name || !mode) {
        module.emitError("malformed ")
            << emitrust::kActorThreadAttrName
            << " entry: expected {name, mode = \"threaded\"|\"async\"}";
        return signalPassFailure();
      }
      threadOne(name, *mode);
    }
    module->removeAttr(emitrust::kActorThreadAttrName);
  }

  /// Threads a single selected actor; every failure mode either skips
  /// silently (lift-demoted: nothing to thread) or vetoes with a located
  /// warning (lifted but thread-ineligible: stays same-thread).
  void threadOne(StringAttr name, emitrust::ActorMode mode) {
    auto structDef = dyn_cast_or_null<emitrust::StructDefOp>(
        SymbolTable::lookupSymbolIn(module, name));
    if (!structDef)
      return; // Lift-demoted (or field-less): never lifted, skip silently.
    emitrust::ImplOp impl;
    for (emitrust::ImplOp candidate : module.getOps<emitrust::ImplOp>())
      // W2.17: a trait impl (`impl Drop for T`) is not a method table; only
      // the INHERENT impl carries the arms a mailbox could serve.
      if (candidate.getStructName() == name.getValue() &&
          !candidate.getTraitName()) {
        impl = candidate;
        break;
      }
    if (!impl)
      return; // No arms materialized: nothing a mailbox could serve.
    auto structType =
        emitrust::StructType::get(&getContext(), name.getValue());

    // The constructing driver local: the one emitrust.variable of the
    // actor's struct type in a module-level function (the lift's driver
    // rule). Absent -> skip silently (library TU / demoted driver).
    emitrust::FuncOp driver;
    emitrust::VariableOp actorVar;
    unsigned count = 0;
    for (emitrust::FuncOp fn : module.getOps<emitrust::FuncOp>())
      fn.walk([&](emitrust::VariableOp variable) {
        auto lvalue = cast<emitrust::LValueType>(variable.getResult().getType());
        if (lvalue.getValueType() != structType)
          return;
        driver = fn;
        actorVar = variable;
        ++count;
      });
    if (count == 0)
      return;
    if (count > 1) {
      actorVar.emitWarning("actor plan: ")
          << name.getValue()
          << " stays same-thread: multiple locals of the actor type exist";
      return;
    }

    // Veto 1: a cross-actor client holds a &mut of the actor's struct.
    // (An arm receiver inside the actor's own impl is the one legal
    // mut_ref position.)
    for (emitrust::FuncOp fn : collectAllFuncs()) {
      bool isOwnMethod = fn->getParentOp() == impl.getOperation();
      FunctionType type = fn.getFunctionType();
      for (unsigned i = isOwnMethod ? 1 : 0; i < type.getNumInputs(); ++i) {
        auto mutRef = dyn_cast<emitrust::MutRefType>(type.getInput(i));
        if (mutRef && mutRef.getPointee() == structType) {
          fn.emitWarning("actor plan: ")
              << name.getValue() << " stays same-thread: function '"
              << fn.getSymName()
              << "' holds a &mut reference across the thread boundary";
          return;
        }
      }
    }

    // Veto 2: an unsendable method signature (types outside the sendable
    // set, unnamed parameters, statics, body-less declarations) — the
    // message surface could not be derived.
    for (emitrust::FuncOp fn : impl.getBody().front().getOps<emitrust::FuncOp>()) {
      if (fn->hasAttr(emitrust::kStaticMethodAttrName) || fn.isExternal()) {
        fn.emitWarning("actor plan: ")
            << name.getValue() << " stays same-thread: method '"
            << fn.getSymName() << "' has no receiver delegation form";
        return;
      }
      FunctionType type = fn.getFunctionType();
      auto paramNames =
          fn->getAttrOfType<ArrayAttr>(mlir::emitrust::kParamNamesAttrName);
      for (unsigned i = 1; i < type.getNumInputs(); ++i) {
        StringRef paramName;
        if (paramNames && i < paramNames.size())
          if (auto slot = dyn_cast<StringAttr>(paramNames[i]))
            paramName = slot.getValue();
        if (!emitrust::isSendableActorType(type.getInput(i)) ||
            paramName.empty()) {
          fn.emitWarning("actor plan: ")
              << name.getValue() << " stays same-thread: method '"
              << fn.getSymName() << "' has an unsendable signature";
          return;
        }
      }
      if (type.getNumResults() == 1 &&
          !emitrust::isSendableActorType(type.getResult(0))) {
        fn.emitWarning("actor plan: ")
            << name.getValue() << " stays same-thread: method '"
            << fn.getSymName() << "' has an unsendable signature";
        return;
      }
    }

    // Veto 3 (+ gathering): every driver use of the actor local must be an
    // arm method_call or a member/subscript access with an accessor form.
    DriverRewrite rewrite;
    if (failed(analyzeDriverAccesses(name, structDef, actorVar, rewrite)))
      return; // analyzeDriverAccesses printed the located warning.

    // Veto 4: a synthesized accessor name collides with an existing method.
    for (const FieldAccess &access : rewrite.fields) {
      for (int set = 0; set < 2; ++set) {
        if ((set ? !access.needsSet : !access.needsGet))
          continue;
        std::string accessor =
            ((set ? "set_" : "get_") + access.fieldName.getValue()).str();
        for (emitrust::FuncOp fn :
             impl.getBody().front().getOps<emitrust::FuncOp>())
          if (fn.getSymName() == accessor) {
            fn.emitWarning("actor plan: ")
                << name.getValue() << " stays same-thread: accessor '"
                << accessor << "' collides with an existing method";
            return;
          }
      }
    }

    // Eligible: rewrite. Any structural surprise from here on is a hard
    // pass failure — partial threading must never emit silently.
    synthesizeAccessors(impl, structType, rewrite);
    rewriteDriverAccesses(actorVar, rewrite);
    retypeAndSpawn(name, structType, actorVar);
    insertShutdowns(driver, actorVar);
    OpBuilder builder(&getContext());
    builder.setInsertionPointAfter(impl);
    builder.create<emitrust::ActorRuntimeOp>(
        impl.getLoc(), FlatSymbolRefAttr::get(name),
        emitrust::ActorModeAttr::get(&getContext(), mode));
  }

  /// Every emitrust.func in the module, incl. impl methods.
  SmallVector<emitrust::FuncOp> collectAllFuncs() {
    SmallVector<emitrust::FuncOp> funcs;
    module.walk([&](emitrust::FuncOp fn) { funcs.push_back(fn); });
    return funcs;
  }

  /// Classifies every use of the actor local, filling `rewrite`; a use with
  /// no accessor form prints the located stays-same-thread warning and
  /// fails (the caller leaves the actor untouched).
  LogicalResult analyzeDriverAccesses(StringAttr name,
                                      emitrust::StructDefOp structDef,
                                      emitrust::VariableOp actorVar,
                                      DriverRewrite &rewrite) {
    auto veto = [&](Operation *at, const llvm::Twine &reason) {
      at->emitWarning("actor plan: ")
          << name.getValue() << " stays same-thread: " << reason;
      return failure();
    };
    auto fieldIndexOf = [&](StringRef fieldName) -> std::optional<unsigned> {
      for (auto [index, entry] :
           llvm::enumerate(structDef.getFieldNames()))
        if (cast<StringAttr>(entry).getValue() == fieldName)
          return index;
      return std::nullopt;
    };
    // field name -> index into rewrite.fields.
    llvm::SmallDenseMap<unsigned, unsigned> slotOfField;
    auto slotFor = [&](unsigned defIndex) {
      auto it = slotOfField.find(defIndex);
      if (it != slotOfField.end())
        return it->second;
      FieldAccess access;
      access.fieldName =
          cast<StringAttr>(structDef.getFieldNames()[defIndex]);
      access.fieldType =
          cast<TypeAttr>(structDef.getFieldTypes()[defIndex]).getValue();
      access.defIndex = defIndex;
      unsigned slot = rewrite.fields.size();
      rewrite.fields.push_back(access);
      slotOfField[defIndex] = slot;
      return slot;
    };

    for (Operation *user : actorVar.getResult().getUsers()) {
      if (auto call = dyn_cast<emitrust::MethodCallOp>(user)) {
        if (call.getReceiver() == actorVar.getResult())
          continue; // An arm call: retypes with the variable, no rewrite.
        return veto(user, "the actor local is a method-call argument");
      }
      auto member = dyn_cast<emitrust::MemberOp>(user);
      if (!member)
        return veto(user,
                    "driver use of the actor local has no message form");
      std::optional<unsigned> defIndex = fieldIndexOf(member.getMember());
      if (!defIndex)
        return veto(member, "driver access to unknown field '" +
                                member.getMember() + "'");
      unsigned slot = slotFor(*defIndex);
      FieldAccess &access = rewrite.fields[slot];
      rewrite.members.push_back(member);
      for (Operation *memberUser : member.getResult().getUsers()) {
        if (auto load = dyn_cast<emitrust::LoadOp>(memberUser)) {
          if (!isAccessorScalarType(access.fieldType))
            return veto(load, "driver access to field '" +
                                  access.fieldName.getValue() +
                                  "' has no accessor form");
          access.needsGet = true;
          rewrite.scalarGets.push_back({load, slot});
          continue;
        }
        if (auto assign = dyn_cast<emitrust::AssignOp>(memberUser)) {
          if (assign.getVar() != member.getResult() ||
              !isAccessorScalarType(access.fieldType))
            return veto(assign, "driver access to field '" +
                                    access.fieldName.getValue() +
                                    "' has no accessor form");
          access.needsSet = true;
          rewrite.scalarSets.push_back({assign, slot});
          continue;
        }
        auto subscript = dyn_cast<emitrust::SubscriptOp>(memberUser);
        if (!subscript || subscript.getArray() != member.getResult())
          return veto(memberUser, "driver access to field '" +
                                      access.fieldName.getValue() +
                                      "' has no accessor form");
        auto arrayType = dyn_cast<emitrust::ArrayType>(access.fieldType);
        if (!arrayType || !isAccessorScalarType(arrayType.getElementType()))
          return veto(subscript, "driver access to field '" +
                                     access.fieldName.getValue() +
                                     "' has no accessor form");
        Type indexType = subscript.getIndex().getType();
        if (access.element && access.indexType != indexType)
          return veto(subscript, "driver accesses to field '" +
                                     access.fieldName.getValue() +
                                     "' mix subscript index types");
        access.element = true;
        access.indexType = indexType;
        rewrite.subscripts.push_back(subscript);
        for (Operation *subscriptUser : subscript.getResult().getUsers()) {
          if (auto load = dyn_cast<emitrust::LoadOp>(subscriptUser)) {
            access.needsGet = true;
            rewrite.elementGets.push_back({load, slot});
            continue;
          }
          auto assign = dyn_cast<emitrust::AssignOp>(subscriptUser);
          if (!assign || assign.getVar() != subscript.getResult())
            return veto(subscriptUser, "driver access to field '" +
                                           access.fieldName.getValue() +
                                           "' has no accessor form");
          access.needsSet = true;
          rewrite.elementSets.push_back({assign, slot});
        }
      }
    }
    return success();
  }

  /// Appends the needed accessor methods to the impl, in struct-field
  /// declaration order, get before set — the spike's reference order.
  void synthesizeAccessors(emitrust::ImplOp impl,
                           emitrust::StructType structType,
                           DriverRewrite &rewrite) {
    OpBuilder builder(&getContext());
    Block &implBlock = impl.getBody().front();
    auto receiverType = emitrust::MutRefType::get(structType);
    // Accessor order = struct_def field declaration order (get before
    // set), the spike's reference order — discovery order follows MLIR
    // use-list order, which is not a stable contract.
    SmallVector<unsigned> order;
    for (unsigned i = 0; i < rewrite.fields.size(); ++i)
      order.push_back(i);
    llvm::sort(order, [&](unsigned a, unsigned b) {
      return rewrite.fields[a].defIndex < rewrite.fields[b].defIndex;
    });
    for (unsigned slot : order) {
      FieldAccess &access = rewrite.fields[slot];
      Location loc = impl.getLoc();
      auto makeBody = [&](emitrust::FuncOp fn) {
        Block *entry = &fn.getBody().emplaceBlock();
        for (Type input : fn.getFunctionType().getInputs())
          entry->addArgument(input, loc);
        OpBuilder body = OpBuilder::atBlockBegin(entry);
        auto deref = body.create<emitrust::DerefOp>(
            loc, emitrust::LValueType::get(structType),
            entry->getArgument(0));
        Value place = body.create<emitrust::MemberOp>(
            loc, emitrust::LValueType::get(access.fieldType),
            deref.getResult(), access.fieldName);
        Type valueType = access.fieldType;
        if (access.element) {
          auto arrayType = cast<emitrust::ArrayType>(access.fieldType);
          valueType = arrayType.getElementType();
          place = body.create<emitrust::SubscriptOp>(
              loc, emitrust::LValueType::get(valueType), place,
              entry->getArgument(1));
        }
        return std::make_pair(place, valueType);
      };
      auto paramNamesAttr = [&](ArrayRef<StringRef> names) {
        SmallVector<Attribute> slots;
        for (StringRef paramName : names)
          slots.push_back(builder.getStringAttr(paramName));
        return builder.getArrayAttr(slots);
      };
      if (access.needsGet) {
        SmallVector<Type> inputs{receiverType};
        if (access.element)
          inputs.push_back(access.indexType);
        Type valueType =
            access.element
                ? cast<emitrust::ArrayType>(access.fieldType).getElementType()
                : access.fieldType;
        builder.setInsertionPointToEnd(&implBlock);
        auto fn = builder.create<emitrust::FuncOp>(
            loc, ("get_" + access.fieldName.getValue()).str(),
            builder.getFunctionType(inputs, {valueType}));
        if (access.element)
          fn->setAttr(mlir::emitrust::kParamNamesAttrName,
                      paramNamesAttr({"", "i"}));
        auto [place, loadedType] = makeBody(fn);
        OpBuilder body(&getContext());
        body.setInsertionPointToEnd(&fn.getBody().front());
        auto value =
            body.create<emitrust::LoadOp>(loc, loadedType, place);
        body.create<emitrust::ReturnOp>(loc, value.getResult());
      }
      if (access.needsSet) {
        SmallVector<Type> inputs{receiverType};
        SmallVector<StringRef> names{""};
        if (access.element) {
          inputs.push_back(access.indexType);
          names.push_back("i");
        }
        Type valueType =
            access.element
                ? cast<emitrust::ArrayType>(access.fieldType).getElementType()
                : access.fieldType;
        inputs.push_back(valueType);
        names.push_back("v");
        builder.setInsertionPointToEnd(&implBlock);
        auto fn = builder.create<emitrust::FuncOp>(
            loc, ("set_" + access.fieldName.getValue()).str(),
            builder.getFunctionType(inputs, {}));
        fn->setAttr(mlir::emitrust::kParamNamesAttrName,
                    paramNamesAttr(names));
        auto [place, storedType] = makeBody(fn);
        (void)storedType;
        OpBuilder body(&getContext());
        body.setInsertionPointToEnd(&fn.getBody().front());
        body.create<emitrust::AssignOp>(
            loc, place, fn.getBody().front().getArguments().back());
        body.create<emitrust::ReturnOp>(loc, Value());
      }
      // Remember the accessor names for the rewrite below.
      (void)slot;
    }
  }

  /// Rewrites the driver's accesses to accessor method calls at the
  /// original sites (order-preserving), then erases the dead places.
  void rewriteDriverAccesses(emitrust::VariableOp actorVar,
                             DriverRewrite &rewrite) {
    Value receiver = actorVar.getResult();
    auto getName = [&](unsigned slot) {
      return ("get_" + rewrite.fields[slot].fieldName.getValue()).str();
    };
    auto setName = [&](unsigned slot) {
      return ("set_" + rewrite.fields[slot].fieldName.getValue()).str();
    };
    for (auto [load, slot] : rewrite.scalarGets) {
      OpBuilder builder(load);
      auto call = builder.create<emitrust::MethodCallOp>(
          load.getLoc(), TypeRange{load.getType()}, receiver,
          builder.getStringAttr(getName(slot)), ValueRange{});
      load.getResult().replaceAllUsesWith(call.getResult(0));
      load.erase();
    }
    for (auto [assign, slot] : rewrite.scalarSets) {
      OpBuilder builder(assign);
      builder.create<emitrust::MethodCallOp>(
          assign.getLoc(), TypeRange{}, receiver,
          builder.getStringAttr(setName(slot)),
          ValueRange{assign.getValue()});
      assign.erase();
    }
    for (auto [load, slot] : rewrite.elementGets) {
      auto subscript = cast<emitrust::SubscriptOp>(
          load.getOperand().getDefiningOp());
      OpBuilder builder(load);
      auto call = builder.create<emitrust::MethodCallOp>(
          load.getLoc(), TypeRange{load.getType()}, receiver,
          builder.getStringAttr(getName(slot)),
          ValueRange{subscript.getIndex()});
      load.getResult().replaceAllUsesWith(call.getResult(0));
      load.erase();
    }
    for (auto [assign, slot] : rewrite.elementSets) {
      auto subscript =
          cast<emitrust::SubscriptOp>(assign.getVar().getDefiningOp());
      OpBuilder builder(assign);
      builder.create<emitrust::MethodCallOp>(
          assign.getLoc(), TypeRange{}, receiver,
          builder.getStringAttr(setName(slot)),
          ValueRange{subscript.getIndex(), assign.getValue()});
      assign.erase();
    }
    for (emitrust::SubscriptOp subscript : rewrite.subscripts)
      subscript.erase();
    for (emitrust::MemberOp member : rewrite.members)
      member.erase();
  }

  /// Retypes the actor local to the opaque handle and inserts the
  /// default-state + spawn construction around it.
  void retypeAndSpawn(StringAttr name, emitrust::StructType structType,
                      emitrust::VariableOp actorVar) {
    auto handleType = emitrust::OpaqueType::get(
        &getContext(), (name.getValue() + "Handle").str());
    Location loc = actorVar.getLoc();
    OpBuilder builder(actorVar);
    auto defaultState = builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange{structType},
        builder.getStringAttr((name.getValue() + "::default").str()),
        /*args=*/ArrayAttr(), ValueRange{});
    actorVar.getResult().setType(emitrust::LValueType::get(handleType));
    builder.setInsertionPointAfter(actorVar);
    auto spawned = builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange{handleType},
        builder.getStringAttr((name.getValue() + "Handle::spawn").str()),
        /*args=*/ArrayAttr(), ValueRange{defaultState.getResult(0)});
    builder.create<emitrust::AssignOp>(loc, actorVar.getResult(),
                                       spawned.getResult(0));
  }

  /// Inserts `.shutdown()` before every return of the driver.
  void insertShutdowns(emitrust::FuncOp driver,
                       emitrust::VariableOp actorVar) {
    SmallVector<emitrust::ReturnOp> returns;
    driver.walk(
        [&](emitrust::ReturnOp returnOp) { returns.push_back(returnOp); });
    for (emitrust::ReturnOp returnOp : returns) {
      OpBuilder builder(returnOp);
      builder.create<emitrust::MethodCallOp>(
          returnOp.getLoc(), TypeRange{}, actorVar.getResult(),
          builder.getStringAttr("shutdown"), ValueRange{});
    }
  }
};

} // namespace
