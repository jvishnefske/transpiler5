//===- LowerExternalRequirements.cpp - FR-52 externals trait --------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `emitrust-lower-external-requirements`: the pass that turns the
/// external-requirement declarations the C importer marked into one
/// `emitrust.trait_def` and makes the code that needs them generic over it.
/// FR-52 covered marked FUNCTIONS; FR-70 adds marked GLOBALS, expressed as a
/// getter/setter pair of associated functions (`fn g() -> T` /
/// `fn set_g(v: T)`) whose call sites replace the whole-value
/// `emitrust.global_load`/`global_store` ops — read/write external STORAGE
/// modelled without an address, monomorphised to direct calls, no dyn, no
/// unsafe.
///
/// # Why this is a pass and not part of the importer
///
/// The importer knows the FACT (this symbol is referenced and nobody defines
/// it) and records it. It does not know the SHAPE of the answer, because the
/// answer is expressed in the emitted item names and in the call syntax, and
/// neither exists until `convert-to-emitrust` has run: before it, a call is a
/// `func.call` whose callee must resolve in the symbol table, so a
/// requirement could not be erased without breaking the verifier. After it, a
/// call is an `emitrust.call_opaque` with a plain STRING callee, which is
/// exactly the freedom `E::<name>` and `<name>::<E>` need.
///
/// # The propagation rule
///
/// A function is generic over the trait iff it can reach a requirement
/// through direct calls. That is the smallest correct set: a Rust caller of
/// `f<E: Externals>` must itself name some `E`, and it has one only if it is
/// generic too, so genericity propagates up the call graph and stops exactly
/// where the calls do. Functions outside the closure are not touched at all,
/// which is what keeps a project with no requirements byte-identical.
///
/// Recursion is handled by the worklist reaching a fixpoint rather than by
/// any acyclicity assumption; a cycle simply converges.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/LowerExternalRequirements.h"

#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/RustCasing.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTLOWEREXTERNALREQUIREMENTS
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

namespace {

/// The callee name of `op` when it is one of the two call forms this pass
/// requalifies, or an empty `StringRef` otherwise.
///
/// The two forms share one namespace: `convert-func-to-emitrust` writes the
/// callee's SYMBOL into `emitrust.method_call`'s `method` attribute, and the
/// Rust emitter renders an `emitrust.impl` member under that same symbol. So
/// "the string naming the callee" is a single concept here.
StringRef calleeOf(Operation *op) {
  if (auto call = dyn_cast<emitrust::CallOpaqueOp>(op))
    return call.getCallee();
  if (auto call = dyn_cast<emitrust::MethodCallOp>(op))
    return call.getMethod();
  return StringRef();
}

/// The function symbol `attr` names as a function-POINTER value, i.e. the `X`
/// of the opaque text `Some(X)` the importer emits for a decayed function
/// reference, or an empty `StringRef`.
///
/// Reading the text back is reading a contract, not guessing: every function
/// address in the module is produced by `resolveFunctionPointerDecl` in
/// exactly this spelling. It matters here because such a reference is NOT a
/// symbol use — nothing in the IR links it to the callee — so a generic
/// function whose address is taken would otherwise be renamed out from under
/// its own `Some(...)`.
StringRef fnPointerTargetOf(Attribute attr) {
  auto opaque = dyn_cast_if_present<emitrust::OpaqueAttr>(attr);
  if (!opaque)
    return StringRef();
  StringRef text = opaque.getValue();
  if (!text.consume_front("Some(") || !text.consume_back(")"))
    return StringRef();
  return text;
}

/// `fnPointerTargetOf` for the value of an `emitrust.constant`.
StringRef fnPointerTargetOf(Operation *op) {
  auto constOp = dyn_cast<emitrust::ConstantOp>(op);
  return constOp ? fnPointerTargetOf(constOp.getValue()) : StringRef();
}

/// Replaces `op`'s callee string with `callee`.
void setCallee(Operation *op, StringRef callee) {
  StringAttr attr = StringAttr::get(op->getContext(), callee);
  if (auto call = dyn_cast<emitrust::CallOpaqueOp>(op)) {
    call.setCalleeAttr(attr);
    return;
  }
  cast<emitrust::MethodCallOp>(op).setMethodAttr(attr);
}

/// Every `emitrust.func` of `module`, free functions and `emitrust.impl`
/// members alike, in module order.
SmallVector<emitrust::FuncOp> collectFuncs(ModuleOp module) {
  SmallVector<emitrust::FuncOp> funcs;
  for (Operation &op : *module.getBody()) {
    if (auto funcOp = dyn_cast<emitrust::FuncOp>(&op)) {
      funcs.push_back(funcOp);
      continue;
    }
    if (auto implOp = dyn_cast<emitrust::ImplOp>(&op))
      for (Operation &member : implOp.getBody().front())
        if (auto funcOp = dyn_cast<emitrust::FuncOp>(&member))
          funcs.push_back(funcOp);
  }
  return funcs;
}

/// The `emitrust-lower-external-requirements` pass.
struct LowerExternalRequirements
    : public emitrust::impl::EmitRustLowerExternalRequirementsBase<
          LowerExternalRequirements> {
  void runOnOperation() override {
    ModuleOp module = getOperation();

    // The requirements, in module order, so the emitted trait lists them in
    // the order the declarations appear rather than in hash order.
    SmallVector<emitrust::FuncOp> requirements;
    for (auto funcOp : module.getOps<emitrust::FuncOp>())
      if (funcOp->hasAttr(emitrust::kExternalRequirementAttrName))
        requirements.push_back(funcOp);
    // FR-70: the marked GLOBALS — external storage the project reads and
    // writes but never defines — join the same trait as a getter/setter pair
    // of associated functions. Same collection rule, same order.
    SmallVector<emitrust::GlobalOp> globalRequirements;
    for (auto globalOp : module.getOps<emitrust::GlobalOp>())
      if (globalOp->hasAttr(emitrust::kExternalRequirementAttrName))
        globalRequirements.push_back(globalOp);
    if (requirements.empty() && globalRequirements.empty())
      return; // The overwhelmingly common case: nothing to do, nothing said.

    // A type parameter shadows a same-named type inside the generic item, and
    // a trait shares the item namespace with structs and enums, so either
    // clash would silently change what an emitted signature means. Both are
    // reported rather than worked around: renaming would make the emitted
    // API depend on an unrelated C identifier.
    for (StringRef reserved :
         {emitrust::kExternalsTraitName, emitrust::kExternalsTypeParam}) {
      if (Operation *clash = SymbolTable::lookupSymbolIn(module, reserved)) {
        clash->emitError()
            << "unsupported: the project defines an item named '" << reserved
            << "', which clashes with the external-requirement trait";
        return signalPassFailure();
      }
    }

    llvm::StringSet<> requirementNames;
    for (emitrust::FuncOp funcOp : requirements)
      requirementNames.insert(funcOp.getSymName());

    // FR-70: the trait spelling of each global requirement, derived at this
    // single point. The getter is the snake_case of the emitted SYMBOL — the
    // one derivation both naming modes agree on (`G_CONFIG` under the FR-53
    // idiomatic rename and verbatim `g_config` both yield `g_config`) — and
    // the setter prefixes `set_`. Which accessors actually become trait
    // items is decided by the uses recorded below: a write-less global emits
    // only the getter, so the item list and the rewrites stay self-consistent
    // by construction.
    struct GlobalAccessors {
      std::string getter;
      std::string setter;
      bool hasLoad = false;
      bool hasStore = false;
      // FR-80: some use is an `emitrust.global_addr` — the getter then
      // returns the REFERENCE (`() -> !emitrust.ref<T>`, rendered
      // `fn g() -> &'static T`) instead of the value, and loads read
      // through it, so ONE item serves value reads and addresses alike.
      bool hasAddr = false;
    };
    llvm::StringMap<GlobalAccessors> globalAccessors;
    for (auto globalOp : globalRequirements) {
      GlobalAccessors info;
      info.getter = emitrust::toSnakeCase(globalOp.getSymName());
      info.setter = "set_" + info.getter;
      globalAccessors[globalOp.getSymName()] = std::move(info);
    }

    SmallVector<emitrust::FuncOp> funcs = collectFuncs(module);

    // Transitive closure of callers, by fixpoint over the direct-call edges.
    llvm::StringSet<> generic;
    bool changed = true;
    while (changed) {
      changed = false;
      for (emitrust::FuncOp funcOp : funcs) {
        if (funcOp->hasAttr(emitrust::kExternalRequirementAttrName) ||
            generic.contains(funcOp.getSymName()))
          continue;
        bool needs = false;
        funcOp.walk([&](Operation *op) {
          // Taking the address of a generic function is an edge just like
          // calling one: `Some(f)` must become `Some(f::<E>)`, and only a
          // function that itself has an `E` can spell that.
          for (StringRef name : {calleeOf(op), fnPointerTargetOf(op)})
            if (!name.empty() &&
                (requirementNames.contains(name) || generic.contains(name)))
              needs = true;
          // FR-70: touching a requirement GLOBAL is an edge for the same
          // reason a call is — the rewritten `E::<getter>()` needs an `E`
          // in scope, and only a generic function has one.
          if (auto load = dyn_cast<emitrust::GlobalLoadOp>(op)) {
            if (globalAccessors.contains(load.getGlobal()))
              needs = true;
          } else if (auto store = dyn_cast<emitrust::GlobalStoreOp>(op)) {
            if (globalAccessors.contains(store.getGlobal()))
              needs = true;
          } else if (auto addr = dyn_cast<emitrust::GlobalAddrOp>(op)) {
            // FR-80: taking a requirement's address is an edge exactly
            // like loading it — the rewritten `E::<getter>()` needs an E.
            if (globalAccessors.contains(addr.getGlobal()))
              needs = true;
          }
        });
        if (needs) {
          generic.insert(funcOp.getSymName());
          changed = true;
        }
      }
    }

    // Refusals, all of them before a single edit, so a rejected module is
    // never half-rewritten.
    //
    // A function POINTER to a REQUIREMENT has no spelling at all: the trait
    // method is reached through the type parameter and `Some(E::f)` is not a
    // function item. The importer already keeps such a symbol rejected
    // (`isExternalRequirementShape`), so this only fires on hand-written IR —
    // but it fires rather than emitting text that names nothing.
    for (emitrust::FuncOp funcOp : funcs) {
      LogicalResult refused = success();
      funcOp.walk([&](Operation *op) {
        StringRef target = fnPointerTargetOf(op);
        if (!target.empty() && requirementNames.contains(target)) {
          op->emitError() << "unsupported: the address of external requirement '"
                          << target << "' is taken; a trait method is not a "
                                       "function item";
          refused = failure();
        }
      });
      if (failed(refused))
        return signalPassFailure();
    }
    // A function pointer stored in a module-level GLOBAL has no enclosing
    // generic item, so there is nowhere to write the `::<E>` its target now
    // needs. The project keeps its pre-FR-52 outcome for this shape.
    for (auto globalOp : module.getOps<emitrust::GlobalOp>()) {
      StringRef target = fnPointerTargetOf(globalOp.getInitAttr());
      if (!target.empty() && generic.contains(target)) {
        globalOp.emitError()
            << "unsupported: global '" << globalOp.getSymName()
            << "' holds the address of '" << target
            << "', which the external-requirement trait makes generic";
        return signalPassFailure();
      }
    }
    // FR-70: every use of a marked global must be a whole-value load or
    // store — the only two shapes `E::g()` / `E::set_g(v)` can express. The
    // importer's gate guarantees this for imported IR, so like the
    // fn-pointer refusal above this fires only on hand-written IR — but it
    // fires rather than erasing a global out from under a use the rewrite
    // cannot express. The same walk records which accessors each global
    // actually needs.
    for (auto globalOp : globalRequirements) {
      GlobalAccessors &info = globalAccessors[globalOp.getSymName()];
      std::optional<SymbolTable::UseRange> uses = SymbolTable::getSymbolUses(
          globalOp.getSymNameAttr(), module.getOperation());
      if (!uses)
        continue;
      for (SymbolTable::SymbolUse use : *uses) {
        Operation *user = use.getUser();
        if (isa<emitrust::GlobalLoadOp>(user)) {
          info.hasLoad = true;
          continue;
        }
        if (isa<emitrust::GlobalStoreOp>(user)) {
          info.hasStore = true;
          continue;
        }
        // FR-80: an address use makes the getter address-carrying. The
        // dialect verifier already restricts `global_addr` to const
        // globals, so hasAddr and hasStore are mutually exclusive by
        // construction (a const global refuses stores structurally).
        if (isa<emitrust::GlobalAddrOp>(user)) {
          info.hasLoad = true;
          info.hasAddr = true;
          continue;
        }
        user->emitError() << "unsupported: external-requirement global '"
                          << globalOp.getSymName()
                          << "' has a use that is not a whole-value load or "
                             "store";
        return signalPassFailure();
      }
    }
    // FR-70: the derived accessor names share the trait's single item
    // namespace with the function requirements, so a collision would emit a
    // trait Rust rejects. Renaming is not an option for the same reason the
    // Externals/E clash above is not worked around: the emitted API would
    // silently depend on an unrelated C identifier. Only the accessors that
    // will actually be emitted participate, so a write-less `x` never
    // reserves `set_x`.
    {
      llvm::StringMap<StringRef> itemSource;
      for (emitrust::FuncOp funcOp : requirements)
        itemSource[funcOp.getSymName()] = funcOp.getSymName();
      for (auto globalOp : globalRequirements) {
        const GlobalAccessors &info = globalAccessors[globalOp.getSymName()];
        SmallVector<StringRef, 2> items;
        if (info.hasLoad)
          items.push_back(info.getter);
        if (info.hasStore)
          items.push_back(info.setter);
        for (StringRef item : items) {
          auto [it, inserted] =
              itemSource.insert({item, globalOp.getSymName()});
          if (!inserted) {
            globalOp.emitError()
                << "unsupported: external requirement '"
                << globalOp.getSymName() << "' derives trait item '" << item
                << "', which collides with the item derived from '"
                << it->second << "'";
            return signalPassFailure();
          }
        }
      }
    }

    // Requalify every call. A call to a requirement resolves through the type
    // parameter; a call to a generic function passes it on. Nothing outside
    // the closure can be a caller of a closure member (that is what the
    // closure means), so no other call form needs touching.
    for (emitrust::FuncOp funcOp : funcs)
      funcOp.walk([&](Operation *op) {
        if (StringRef callee = calleeOf(op); !callee.empty()) {
          if (requirementNames.contains(callee))
            setCallee(op, (emitrust::kExternalsTypeParam + Twine("::") + callee)
                              .str());
          else if (generic.contains(callee))
            setCallee(op, (callee + Twine("::<") +
                           emitrust::kExternalsTypeParam + ">")
                              .str());
          return;
        }
        StringRef target = fnPointerTargetOf(op);
        if (!target.empty() && generic.contains(target))
          cast<emitrust::ConstantOp>(op).setValueAttr(emitrust::OpaqueAttr::get(
              &getContext(), ("Some(" + target + Twine("::<") +
                              emitrust::kExternalsTypeParam + ">)")
                                 .str()));
      });

    // FR-70: rewrite every access of a marked global to its accessor. A load
    // becomes a result-bearing opaque call — an EXPRESSION that sits exactly
    // where the load's value flowed — and a store becomes a call STATEMENT
    // consuming the stored value. Done BEFORE the marked globals are erased:
    // both ops verify their symbol reference, so the other order would leave
    // dangling uses.
    SmallVector<emitrust::GlobalLoadOp> loads;
    SmallVector<emitrust::GlobalStoreOp> stores;
    SmallVector<emitrust::GlobalAddrOp> addrs;
    module.walk([&](Operation *op) {
      if (auto load = dyn_cast<emitrust::GlobalLoadOp>(op)) {
        if (globalAccessors.contains(load.getGlobal()))
          loads.push_back(load);
      } else if (auto store = dyn_cast<emitrust::GlobalStoreOp>(op)) {
        if (globalAccessors.contains(store.getGlobal()))
          stores.push_back(store);
      } else if (auto addr = dyn_cast<emitrust::GlobalAddrOp>(op)) {
        if (globalAccessors.contains(addr.getGlobal()))
          addrs.push_back(addr);
      }
    });
    for (emitrust::GlobalLoadOp load : loads) {
      OpBuilder builder(load);
      const GlobalAccessors &info = globalAccessors[load.getGlobal()];
      // FR-80: when the getter is address-carrying, a whole-value load
      // reads THROUGH the returned reference — call, then deref-and-load —
      // so mixed value/address usage resolves to the one &'static item.
      if (info.hasAddr) {
        Type valueType = load.getResult().getType();
        auto call = builder.create<emitrust::CallOpaqueOp>(
            load.getLoc(), TypeRange{emitrust::RefType::get(valueType)},
            builder.getStringAttr(
                (emitrust::kExternalsTypeParam + Twine("::") + info.getter)
                    .str()),
            /*args=*/ArrayAttr(), ValueRange());
        auto place = builder.create<emitrust::DerefOp>(
            load.getLoc(), emitrust::LValueType::get(valueType),
            call.getResult(0));
        auto read = builder.create<emitrust::LoadOp>(load.getLoc(),
                                                     valueType, place);
        load.getResult().replaceAllUsesWith(read.getResult());
        load.erase();
        continue;
      }
      auto call = builder.create<emitrust::CallOpaqueOp>(
          load.getLoc(), TypeRange{load.getResult().getType()},
          builder.getStringAttr((emitrust::kExternalsTypeParam +
                                 Twine("::") +
                                 globalAccessors[load.getGlobal()].getter)
                                    .str()),
          /*args=*/ArrayAttr(), ValueRange());
      load.getResult().replaceAllUsesWith(call.getResult(0));
      load.erase();
    }
    // FR-80: every address becomes the same result-bearing getter call —
    // the reference VALUE `E::<getter>()` returns, which is the consumer's
    // one static item, so identity is preserved across every rewrite site.
    for (emitrust::GlobalAddrOp addr : addrs) {
      OpBuilder builder(addr);
      auto call = builder.create<emitrust::CallOpaqueOp>(
          addr.getLoc(), TypeRange{addr.getResult().getType()},
          builder.getStringAttr((emitrust::kExternalsTypeParam +
                                 Twine("::") +
                                 globalAccessors[addr.getGlobal()].getter)
                                    .str()),
          /*args=*/ArrayAttr(), ValueRange());
      addr.getResult().replaceAllUsesWith(call.getResult(0));
      addr.erase();
    }
    for (emitrust::GlobalStoreOp store : stores) {
      OpBuilder builder(store);
      builder.create<emitrust::CallOpaqueOp>(
          store.getLoc(), TypeRange(),
          builder.getStringAttr((emitrust::kExternalsTypeParam +
                                 Twine("::") +
                                 globalAccessors[store.getGlobal()].setter)
                                    .str()),
          /*args=*/ArrayAttr(), ValueRange{store.getValue()});
      store.erase();
    }

    // Mark the closure. Done after the requalification so the walk above sees
    // the module in one consistent state.
    StringAttr traitName =
        StringAttr::get(&getContext(), emitrust::kExternalsTraitName);
    for (emitrust::FuncOp funcOp : funcs)
      if (generic.contains(funcOp.getSymName()))
        funcOp->setAttr(emitrust::kExternalsGenericAttrName, traitName);

    // Materialize the trait at the FRONT of the module: a Rust reader meets
    // the crate's requirements before the code that consumes them, and the
    // position is free — the op only exists when there are requirements.
    SmallVector<Attribute> names;
    SmallVector<Attribute> types;
    for (emitrust::FuncOp funcOp : requirements) {
      names.push_back(StringAttr::get(&getContext(), funcOp.getSymName()));
      types.push_back(TypeAttr::get(funcOp.getFunctionType()));
    }
    // FR-70: the globals contribute their USED accessors after the function
    // requirements, getter before setter, in module order. The item types
    // are ordinary associated-function types — `() -> T` and `(T) -> ()` —
    // so the emitter needs nothing global-specific to render them.
    for (auto globalOp : globalRequirements) {
      const GlobalAccessors &info = globalAccessors[globalOp.getSymName()];
      Type valueType = globalOp.getType();
      if (info.hasLoad) {
        names.push_back(StringAttr::get(&getContext(), info.getter));
        // FR-80: the address-carrying getter's item type returns the
        // reference; the emitter renders it `fn <getter>() -> &'static T`.
        Type resultType = info.hasAddr
                              ? Type(emitrust::RefType::get(valueType))
                              : valueType;
        types.push_back(TypeAttr::get(
            FunctionType::get(&getContext(), {}, {resultType})));
      }
      if (info.hasStore) {
        names.push_back(StringAttr::get(&getContext(), info.setter));
        types.push_back(
            TypeAttr::get(FunctionType::get(&getContext(), {valueType}, {})));
      }
    }
    // `names` can only be empty on hand-written IR (a marked global no code
    // touches — the importer's referenced-only policy never produces one);
    // an itemless trait would say nothing, so none is emitted.
    if (!names.empty()) {
      OpBuilder builder(&getContext());
      builder.setInsertionPointToStart(module.getBody());
      builder.create<emitrust::TraitDefOp>(
          requirements.empty() ? globalRequirements.front().getLoc()
                               : requirements.front().getLoc(),
          traitName, builder.getArrayAttr(names), builder.getArrayAttr(types));
    }

    for (emitrust::FuncOp funcOp : requirements)
      funcOp.erase();
    // After the load/store rewrite above, nothing references the marked
    // globals any more; the requirement now lives in the trait.
    for (auto globalOp : globalRequirements)
      globalOp.erase();
  }
};

} // namespace
