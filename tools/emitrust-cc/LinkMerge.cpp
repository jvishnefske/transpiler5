//===- LinkMerge.cpp - FR-58 link-step shard merging ----------------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implementation of the FR-58 slice-1 link merge declared in LinkMerge.h.
/// See that header for the algorithm; the comments here cover only the
/// mechanics that are not obvious from it.
//
//===----------------------------------------------------------------------===//

#include "LinkMerge.h"

#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/ShardMetadata.h"

#include "mlir/IR/AttrTypeSubElements.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/IR/Verifier.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Payload extraction
//===----------------------------------------------------------------------===//

/// True when `name` (as returned by `SectionRef::getName()`) is the
/// emitrust payload section for `object`'s format. Mach-O's `getName()`
/// reports only the bare section name (the segment half is a separate
/// field), so the query name diverges from the write side's
/// `<segment>,<section>` `--add-section` spelling (`emitRustSectionSpec` in
/// emitrust-clang.cpp) even though both name the same section.
static bool isEmitRustSectionName(const llvm::object::ObjectFile &object,
                                  llvm::StringRef name) {
  if (object.isMachO())
    return name == "__emitrust";
  return name == ".emitrust";
}

/// The buffer-ref core of `findShardPayload`, shared with the archive
/// expansion (an archive member is a MemoryBufferRef into the archive, not
/// a MemoryBuffer of its own). Same three-way contract as the public
/// wrapper.
static FailureOr<std::optional<llvm::StringRef>>
findShardPayloadRef(llvm::MemoryBufferRef buffer, std::string &errorMessage) {
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> object =
      llvm::object::ObjectFile::createObjectFile(buffer);
  if (!object) {
    // Not an object file: the input IS the shard (bytecode or textual
    // MLIR); the parser will diagnose anything else.
    llvm::consumeError(object.takeError());
    return std::optional<llvm::StringRef>(buffer.getBuffer());
  }
  for (const llvm::object::SectionRef &section : (*object)->sections()) {
    llvm::Expected<llvm::StringRef> name = section.getName();
    if (!name) {
      llvm::consumeError(name.takeError());
      continue;
    }
    if (!isEmitRustSectionName(**object, *name))
      continue;
    llvm::Expected<llvm::StringRef> contents = section.getContents();
    if (!contents) {
      errorMessage = llvm::toString(contents.takeError());
      return failure();
    }
    return std::optional<llvm::StringRef>(*contents);
  }
  // An object with no `.emitrust` section: sidecar fallback.
  return std::optional<llvm::StringRef>();
}

FailureOr<std::optional<llvm::StringRef>>
emitrustcc::findShardPayload(const llvm::MemoryBuffer &buffer,
                             std::string &errorMessage) {
  return findShardPayloadRef(buffer.getMemBufferRef(), errorMessage);
}

bool emitrustcc::isStaticArchive(const llvm::MemoryBuffer &buffer) {
  return llvm::identify_magic(buffer.getBuffer()) ==
         llvm::file_magic::archive;
}

FailureOr<emitrustcc::ArchivePayloads>
emitrustcc::findArchivePayloads(const llvm::MemoryBuffer &buffer,
                                std::string &errorMessage) {
  llvm::Expected<std::unique_ptr<llvm::object::Archive>> archive =
      llvm::object::Archive::create(buffer.getMemBufferRef());
  if (!archive) {
    errorMessage = llvm::toString(archive.takeError());
    return failure();
  }
  ArchivePayloads result;
  // `children` skips the symbol-table and string-table pseudo-members and
  // reports iteration corruption through `err`, which must be checked even
  // on the early-return paths.
  llvm::Error err = llvm::Error::success();
  for (const llvm::object::Archive::Child &child : (*archive)->children(err)) {
    llvm::Expected<llvm::StringRef> name = child.getName();
    if (!name) {
      errorMessage = llvm::toString(name.takeError());
      llvm::consumeError(std::move(err));
      return failure();
    }
    llvm::Expected<llvm::MemoryBufferRef> member = child.getMemoryBufferRef();
    if (!member) {
      errorMessage = ("member '" + *name +
                      "': " + llvm::toString(member.takeError()))
                         .str();
      llvm::consumeError(std::move(err));
      return failure();
    }
    FailureOr<std::optional<llvm::StringRef>> payload =
        findShardPayloadRef(*member, errorMessage);
    if (failed(payload)) {
      errorMessage = ("member '" + *name + "': " + errorMessage).str();
      llvm::consumeError(std::move(err));
      return failure();
    }
    if (*payload)
      result.payloads.push_back({name->str(), **payload});
    else
      result.skippedMembers.push_back(name->str());
  }
  if (err) {
    errorMessage = llvm::toString(std::move(err));
    return failure();
  }
  return result;
}

//===----------------------------------------------------------------------===//
// Merge
//===----------------------------------------------------------------------===//

namespace {

/// A parsed `tu<N>_`/`TU<N>_` per-TU tag at the front of a symbol name.
struct TuTag {
  /// The ordinal `N` the shard's import assigned (0 for a solo import).
  unsigned ordinal;
  /// Whether the tag is the SCREAMING `TU<N>_` global spelling.
  bool upper;
  /// The name after the tag, e.g. `scale` of `tu0_scale`.
  llvm::StringRef rest;
};

/// Parses the per-TU tag at the front of `name`, accepting exactly the two
/// spellings `CSymbolNaming.h` produces and `CSymbolLinkage.h` recognizes:
/// lowercase `tu<N>_` on a snake_case function and uppercase `TU<N>_` on a
/// SCREAMING_SNAKE_CASE global. Shares that predicate's stated imprecision:
/// a C identifier literally spelled `tu0_x` is indistinguishable from a
/// tagged one and is treated as tagged.
static std::optional<TuTag> parseTuTag(llvm::StringRef name) {
  bool upper;
  if (name.consume_front("tu"))
    upper = false;
  else if (name.consume_front("TU"))
    upper = true;
  else
    return std::nullopt;
  size_t digits = 0;
  while (digits < name.size() && llvm::isDigit(name[digits]))
    ++digits;
  if (digits == 0 || digits >= name.size() || name[digits] != '_')
    return std::nullopt;
  unsigned ordinal = 0;
  if (name.take_front(digits).getAsInteger(10, ordinal))
    return std::nullopt;
  return TuTag{ordinal, upper, name.drop_front(digits + 1)};
}

/// FR-159: the records of `shard` that must NOT be sunk into a per-TU Rust
/// module, each mapped to the op that makes it escape (which anchors the
/// note on the rejection). An empty map means every record in the shard is
/// translation-unit-local as far as this shard can tell.
///
/// The sink turns two same-named, differently-shaped records into two Rust
/// PATHS. That is faithful only while NOTHING outside the owning translation
/// unit can name the record: an internal-linkage name cannot cross a TU
/// boundary, so a record mentioned only from internal-linkage items (and
/// from function BODIES, which are private to their function whatever its
/// linkage) is safely repathed along with them. Everything else pins it:
///
///  * the SIGNATURE of an external-linkage function, or the TYPE of an
///    external-linkage global. This is the FR-58 `struct Box` case
///    (test/Driver/link-merge-errors.c) and the header-ODR case, and the
///    reachability is TRANSITIVE -- a record behind `mut_ref`, `slice`,
///    `array` or `fn_ptr` is just as reachable as one passed by value (the
///    real systemd users are `&mut [tu314::SwapEntries]`), which is why the
///    types are WALKED rather than pattern-matched at the top level;
///  * a FIELD of any other record. That record dedups across the shards
///    into ONE crate-wide Rust type, so its field cannot be repathed for
///    one shard alone -- doing so hands rustc an E0308 with no source
///    location, exactly the silent degradation this guard exists to stop;
///  * any PLAIN-TEXT mention: an `emitrust.opaque` payload (`Some(f)`,
///    `Enum::VARIANT`, `Name::default()`) or an `emitrust.call_opaque`
///    callee. Those are strings, not symbol uses and not types, so the
///    repath below cannot reach them at all;
///  * anything an `emitrust.impl`, `emitrust.trait_def`,
///    `emitrust.actor_runtime`, `emitrust.use` or `emitrust.verbatim`
///    mentions, for the same reason.
///
/// An unrecognized top-level op kind disables the sink for the whole shard
/// (`sinkDisabled`): a new item kind is a new way for a name to escape, and
/// the safe default is the historical link rejection.
static llvm::StringMap<Operation *>
collectNonSinkableRecords(ModuleOp shard, bool &sinkDisabled) {
  llvm::StringMap<Operation *> escapes;
  sinkDisabled = false;

  // Every record/enum name reachable from `type`, however deeply nested.
  auto noteTypes = [&](Type type, Operation *site, llvm::StringRef except) {
    if (!type)
      return;
    type.walk([&](Type sub) {
      llvm::StringRef name;
      if (auto structType = dyn_cast<emitrust::StructType>(sub))
        name = structType.getName();
      else if (auto enumType = dyn_cast<emitrust::EnumType>(sub))
        name = enumType.getName();
      else if (auto dataEnumType = dyn_cast<emitrust::DataEnumType>(sub))
        name = dataEnumType.getName();
      if (!name.empty() && name != except)
        escapes.try_emplace(name, site);
    });
  };
  // Every WHOLE identifier in a plain-text carrier. Whole-identifier, not
  // substring: `SwapEntries` must not be pinned by a mention of
  // `SwapEntriesX`, and conversely a record whose name really is a token of
  // the text is pinned however that text spells it.
  auto noteText = [&](llvm::StringRef text, Operation *site) {
    auto isIdentChar = [](char c) { return llvm::isAlnum(c) || c == '_'; };
    size_t index = 0;
    while (index < text.size()) {
      if (!isIdentChar(text[index])) {
        ++index;
        continue;
      }
      size_t start = index;
      while (index < text.size() && isIdentChar(text[index]))
        ++index;
      escapes.try_emplace(text.substr(start, index - start), site);
    }
  };
  auto noteAttrText = [&](Attribute attr, Operation *site) {
    if (!attr)
      return;
    attr.walk([&](Attribute sub) {
      if (auto opaque = dyn_cast<emitrust::OpaqueAttr>(sub))
        noteText(opaque.getValue(), site);
    });
  };

  // Plain-text carriers, anywhere in the shard including inside a tagged
  // function's body: these are exactly the carriers `renameShardTags`
  // documents, and none of them is rewritten by the repath.
  shard.walk([&](Operation *inner) {
    if (auto call = dyn_cast<emitrust::CallOpaqueOp>(inner))
      noteText(call.getCallee(), inner);
    else if (auto constant = dyn_cast<emitrust::ConstantOp>(inner))
      noteAttrText(constant.getValue(), inner);
    else if (auto global = dyn_cast<emitrust::GlobalOp>(inner))
      noteAttrText(global.getInitAttr(), inner);
  });

  for (Operation &op : shard.getBody()->getOperations()) {
    Operation *site = &op;
    StringAttr symbol = SymbolTable::getSymbolName(&op);
    bool internalLinkage = symbol && parseTuTag(symbol.getValue()).has_value();
    if (auto fn = dyn_cast<FunctionOpInterface>(&op)) {
      // A file-static function's signature is TU-local and is repathed with
      // the record; an external one's is the cross-TU interface. A BODY is
      // private either way, so it is deliberately not walked here.
      if (internalLinkage)
        continue;
      for (Type type : fn.getArgumentTypes())
        noteTypes(type, site, /*except=*/"");
      for (Type type : fn.getResultTypes())
        noteTypes(type, site, /*except=*/"");
      continue;
    }
    if (auto global = dyn_cast<emitrust::GlobalOp>(op)) {
      if (!internalLinkage)
        noteTypes(global.getType(), site, /*except=*/"");
      continue;
    }
    if (auto structDef = dyn_cast<emitrust::StructDefOp>(op)) {
      for (Attribute fieldType : structDef.getFieldTypes())
        noteTypes(cast<TypeAttr>(fieldType).getValue(), site,
                  /*except=*/structDef.getSymName());
      continue;
    }
    if (auto dataEnumDef = dyn_cast<emitrust::DataEnumDefOp>(op)) {
      for (Attribute variant : dataEnumDef.getVariantFieldTypes())
        for (Attribute fieldType : cast<ArrayAttr>(variant))
          noteTypes(cast<TypeAttr>(fieldType).getValue(), site,
                    /*except=*/dataEnumDef.getSymName());
      continue;
    }
    if (isa<emitrust::EnumDefOp>(op))
      continue; // a C enum's variants are integers; it names no record
    if (auto implOp = dyn_cast<emitrust::ImplOp>(op)) {
      // An impl block names its struct by plain STRING, and its methods'
      // signatures are reachable from wherever the impl's type is.
      noteText(implOp.getStructName(), site);
      implOp.walk([&](FunctionOpInterface method) {
        for (Type type : method.getArgumentTypes())
          noteTypes(type, site, /*except=*/"");
        for (Type type : method.getResultTypes())
          noteTypes(type, site, /*except=*/"");
      });
      continue;
    }
    if (auto traitDef = dyn_cast<emitrust::TraitDefOp>(op)) {
      for (Attribute fnType : traitDef.getFnTypes())
        noteTypes(cast<TypeAttr>(fnType).getValue(), site, /*except=*/"");
      continue;
    }
    if (auto useOp = dyn_cast<emitrust::UseOp>(op)) {
      noteText(useOp.getPath(), site);
      continue;
    }
    if (auto verbatim = dyn_cast<emitrust::VerbatimOp>(op)) {
      noteText(verbatim.getValue(), site);
      continue;
    }
    if (isa<emitrust::ActorRuntimeOp>(op))
      continue; // names an actor, never a record
    sinkDisabled = true;
    return escapes;
  }
  return escapes;
}

/// Prints `op` to a string with the default flags (no locations): the
/// textual identity for the use/verbatim dedup in step 4. (It was also
/// slice 1's shape-equality oracle for step 3, replaced by structural
/// `OperationEquivalence` in slice 2; when debugging a surprising step-3
/// verdict, comparing the two ops' `printOpToString` output is still the
/// quickest way to see WHERE they diverge.)
static std::string printOpToString(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}

/// Rewrites every whole-identifier occurrence of a `renames` key in `text`,
/// leaving everything else — punctuation, and identifiers the map does not
/// name — byte-for-byte alone. Whole-identifier, not substring: an
/// `emitrust.opaque` payload spells targets as `Some(tu0_p_a)`, and a
/// substring rewrite of `tu0_p_a` would also eat the prefix of a sibling
/// `tu0_p_ab`. That is precisely the silent-miscompile direction — both
/// names denote well-typed functions of the same signature, so a table
/// entry retargeted at the wrong one still compiles and just returns the
/// wrong number.
static std::string renameIdentifiersInText(
    llvm::StringRef text, const llvm::StringMap<std::string> &renames) {
  auto isIdentChar = [](char c) {
    return llvm::isAlnum(c) || c == '_';
  };
  std::string out;
  out.reserve(text.size());
  size_t index = 0;
  while (index < text.size()) {
    if (!isIdentChar(text[index])) {
      out += text[index++];
      continue;
    }
    size_t start = index;
    while (index < text.size() && isIdentChar(text[index]))
      ++index;
    llvm::StringRef word = text.substr(start, index - start);
    auto it = renames.find(word);
    out += it == renames.end() ? word : llvm::StringRef(it->second);
  }
  return out;
}

/// Step 1: alpha-renames the per-TU tags on `shard`'s module-level symbols
/// per `ordinalMap` (internal ordinal k renames to global ordinal
/// `ordinalMap[k]`), rewriting THREE carriers of a tagged name: symbol uses
/// (globals are referenced by `FlatSymbolRefAttr`), `emitrust.call_opaque`
/// callees, and `emitrust.opaque` attribute payloads. The last two name
/// functions by plain string, not by symbol use, so
/// `SymbolTable::replaceAllSymbolUses` cannot see them. A solo shard's
/// map is the single element [ordinal]; a re-imported group's map lists
/// its members' link-line positions.
///
/// FR-156: the opaque carrier was the one originally missed, and it made a
/// shard's emitted output depend on its POSITION on the link line. The
/// importer spells a fn-ptr target as `#emitrust.opaque<"Some(<symbol>)">`
/// both in an `emitrust.global`'s init (a file-static dispatch table, the
/// systemd src/basic/rlimit-util.c:222 shape) and in a body-level
/// `emitrust.constant`; at position 0 the shard's own `tu0_` tags were
/// already right and the link worked, while at any other position the
/// functions were retagged and the strings were not, so emission died at
/// the dangling-fn-ptr-target check. Payloads that are NOT symbols (`None`,
/// an `Enum::VARIANT` path, `Name::default()`) name nothing in the map and
/// pass through untouched; records and enums carry no per-TU tag at all.
///
/// Collision freedom: the map is required strictly increasing, so every
/// target ordinal is >= its source (a TU's link-line position counts at
/// least its group-internal predecessors), and the renames are applied in
/// DESCENDING source-ordinal order — by the time source k renames to
/// map[k], any source tag with a higher ordinal (which map[k] might equal)
/// has already been renamed away. The plain-string carriers are rewritten
/// from a map keyed by ORIGINAL names after all symbol renames, so they
/// cannot be captured by a name that became someone else's target in
/// between.
static LogicalResult renameShardTags(ModuleOp shard,
                                     llvm::ArrayRef<unsigned> ordinalMap) {
  for (size_t k = 1; k < ordinalMap.size(); ++k)
    if (ordinalMap[k] <= ordinalMap[k - 1])
      return shard.emitError()
             << "link-shard ordinal map is not strictly increasing";

  struct Rename {
    Operation *op;
    unsigned sourceOrdinal;
    std::string newName;
  };
  llvm::SmallVector<Rename> renames;
  llvm::StringMap<std::string> textRenames;
  for (Operation &op : shard.getBody()->getOperations()) {
    auto symbol = dyn_cast<SymbolOpInterface>(&op);
    if (!symbol)
      continue;
    llvm::StringRef name = symbol.getName();
    std::optional<TuTag> tag = parseTuTag(name);
    if (!tag)
      continue;
    if (tag->ordinal >= ordinalMap.size())
      return op.emitError()
             << "shard symbol '" << name << "' carries per-TU tag ordinal "
             << tag->ordinal << "; this shard covers only "
             << ordinalMap.size() << " translation unit(s)";
    unsigned target = ordinalMap[tag->ordinal];
    if (target == tag->ordinal)
      continue; // tu<k>_ -> tu<k>_ is a no-op.
    std::string newName = ((tag->upper ? "TU" : "tu") + llvm::Twine(target) +
                           "_" + tag->rest)
                              .str();
    textRenames[name] = newName;
    renames.push_back({&op, tag->ordinal, std::move(newName)});
  }
  if (renames.empty())
    return success();
  llvm::stable_sort(renames, [](const Rename &a, const Rename &b) {
    return a.sourceOrdinal > b.sourceOrdinal;
  });
  for (const Rename &rename : renames) {
    auto symbol = cast<SymbolOpInterface>(rename.op);
    StringAttr newName = StringAttr::get(shard.getContext(), rename.newName);
    if (failed(SymbolTable::replaceAllSymbolUses(rename.op, newName, shard)))
      return rename.op->emitError()
             << "cannot rewrite uses of shard symbol '" << symbol.getName()
             << "' at link";
    SymbolTable::setSymbolName(rename.op, newName);
  }
  shard.walk([&](emitrust::CallOpaqueOp call) {
    auto it = textRenames.find(call.getCallee());
    if (it != textRenames.end())
      call.setCallee(it->second);
  });
  AttrTypeReplacer opaqueReplacer;
  opaqueReplacer.addReplacement(
      [&](emitrust::OpaqueAttr opaque) -> std::optional<Attribute> {
        std::string rewritten =
            renameIdentifiersInText(opaque.getValue(), textRenames);
        if (rewritten == opaque.getValue())
          return std::nullopt;
        return emitrust::OpaqueAttr::get(opaque.getContext(), rewritten);
      });
  // Attributes only: an `!emitrust.opaque` TYPE spells a Rust type
  // (`Option<...>`), never a per-TU-tagged item symbol, and locations are
  // source facts.
  opaqueReplacer.recursivelyReplaceElementsIn(shard, /*replaceAttrs=*/true,
                                              /*replaceLocs=*/false,
                                              /*replaceTypes=*/false);
  return success();
}

//===----------------------------------------------------------------------===//
// FR-158 slice-model reconciliation
//===----------------------------------------------------------------------===//

/// The parameter slots at which `declType` carries the SCALAR pointer model
/// `!emitrust.mut_ref<T>` while `defType` carries the defining translation
/// unit's SLICE model `!emitrust.mut_ref<!emitrust.slice<T>>`, or
/// `std::nullopt` when the two function types differ in ANY other way (a
/// different arity, a different result, a parameter whose difference is not
/// that refinement).
///
/// This is the only divergence the merge knows how to reconcile, and it is
/// the divergence a body-less declaration MUST produce: `collectSliceParams`
/// promotes a pointer parameter to a slice from what the BODY does with it,
/// and a declaration has no body, so the declaring shard shapes its call
/// sites for `&mut T` while the definer's signature says `&mut [T]`.
/// Measured over the 501-object systemd whole-program link: 3796 of 3796
/// diverging declarations are exactly this shape, with zero arity and zero
/// result differences.
static std::optional<llvm::SmallVector<unsigned>>
sliceRefinedSlots(FunctionType declType, FunctionType defType) {
  if (declType.getNumInputs() != defType.getNumInputs() ||
      declType.getResults() != defType.getResults())
    return std::nullopt;
  llvm::SmallVector<unsigned> slots;
  for (unsigned i = 0, e = declType.getNumInputs(); i != e; ++i) {
    Type declIn = declType.getInput(i);
    Type defIn = defType.getInput(i);
    if (declIn == defIn)
      continue;
    auto declRef = dyn_cast<emitrust::MutRefType>(declIn);
    auto defRef = dyn_cast<emitrust::MutRefType>(defIn);
    if (!declRef || !defRef)
      return std::nullopt;
    auto defSlice = dyn_cast<emitrust::SliceType>(defRef.getPointee());
    if (!defSlice || defSlice.getElementType() != declRef.getPointee())
      return std::nullopt;
    slots.push_back(i);
  }
  return slots;
}

/// The operation in `shard` that uses `name` as a VALUE rather than as an
/// `emitrust.call_opaque` callee, or null when there is none.
///
/// Reconciling a slice-refined function reshapes its CALLS. Any other use
/// of the symbol carries a function-pointer TYPE built from the declaration
/// -- an `#emitrust.opaque<"Some(f)">` payload in a dispatch table's
/// initializer or in a body-level constant, or a symbol reference -- and
/// nothing here can retype it. Those uses must be refused, not left behind:
/// a table whose element type still says `fn(&mut T, i32)` while the
/// definition says `fn(&mut [T], i32)` is rustc E0308 with no source
/// location, which is the failure FR-158 exists to remove. systemd's link
/// has ZERO of these, so the guard is untested by construction and is
/// pinned by the synthetic FNPTR leg of Driver/link-slice-model-invalid.c.
///
/// The payload scan is WHOLE-IDENTIFIER, matching `renameShardTags`'s own
/// text carriers: `note` must not be found inside `note_all`.
///
/// Deliberately an ATTRIBUTE walk over every operation rather than
/// `SymbolTable::getSymbolUses`: that helper does not descend into nested
/// symbol tables (an FR-159 sunk `mod tu<N>` module is one) and reports
/// "unknown" for operations it cannot classify, and a MISSED use here is
/// exactly the silent wrong-type emission this guard exists to prevent.
static Operation *findSliceRefinedSymbolEscape(ModuleOp shard,
                                               llvm::StringRef name) {
  auto mentions = [&](llvm::StringRef text) {
    auto isIdentChar = [](char c) { return llvm::isAlnum(c) || c == '_'; };
    size_t index = 0;
    while (index < text.size()) {
      if (!isIdentChar(text[index])) {
        ++index;
        continue;
      }
      size_t start = index;
      while (index < text.size() && isIdentChar(text[index]))
        ++index;
      if (text.substr(start, index - start) == name)
        return true;
    }
    return false;
  };
  StringAttr nameAttr = StringAttr::get(shard.getContext(), name);
  Operation *escape = nullptr;
  shard.walk([&](Operation *inner) {
    if (escape || inner->hasAttr(emitrust::kExternDeclAttrName))
      return; // The declaration's own `sym_name` is not a use.
    for (NamedAttribute attr : inner->getAttrs()) {
      attr.getValue().walk([&](Attribute sub) {
        if (auto opaque = dyn_cast<emitrust::OpaqueAttr>(sub)) {
          if (mentions(opaque.getValue()))
            escape = inner;
        } else if (auto symbol = dyn_cast<FlatSymbolRefAttr>(sub)) {
          if (symbol.getAttr() == nameAttr)
            escape = inner;
        }
      });
    }
  });
  return escape;
}

/// The Rust path the FR-161 one-element view is spelled with.
///
/// `::std::`, NOT `::core::`: `core::slice::from_mut` resolves under cargo
/// but is E0433 under a bare `rustc`, which the lit EndToEnd tests invoke
/// directly. The LEADING `::` is load-bearing too -- FR-159 sinks items
/// into `mod tu<N>`, where a relative `std::` can be shadowed by a
/// TU-local item. Both spellings were measured, not chosen.
static constexpr llvm::StringLiteral kSliceFromMutPath =
    "::std::slice::from_mut";

/// FR-161: proves that a DEFINITION's own pointer parameter touches only
/// element ZERO of the region it is lent.
///
/// This is the admission fence for the one-element view. C's `f(&x)` lends
/// the callee a one-element region and `::std::slice::from_mut(&mut x)` is
/// exactly that region in Rust -- but only while the callee stays inside
/// it. Where C reading `p[1]` is undefined behaviour, Rust reading it is a
/// PANIC, and this project has already decided which side of that trade it
/// takes: FR-75 deliberately flipped the previously-ACCEPTED
/// `helper(&x, 1)` shape to the located address-of-scalar rejection, and
/// `test/Import/C/pointers-param-invalid.c` pins
/// `int first(int *a){return a[0]+a[1];}` called as `first(&x)` as a
/// rejection. Measured on that very program: unfenced, C prints `1 2` and
/// the emitted crate panics `index out of bounds: the len is 1 but the
/// index is 1`. So an unproven parameter keeps its located rejection.
///
/// The proof is a forward walk over the parameter's uses, and it must be
/// TRANSITIVE. `deref` + `subscript[0]` is the direct touch; `deref` +
/// `slice_of[0]` handed to another call is a FORWARD, admitted only when
/// the receiving parameter also passes. That leg is not a nicety: of the
/// 60 residual argument slots measured over the 501-object systemd link,
/// 3 (`parse_sec`, `pidfd_get_pid`, `read_attr_at`) forward instead of
/// subscripting -- `parse_sec` hands `&mut (*ret)[0..]` to `parse_time` --
/// and a shallow fence rejects them, after which the crate does not emit
/// at all. Anything else fails: a non-zero or dynamic index, an
/// `addr_of`, an `emitrust.call_indirect`, an `args`-remapped call (where
/// operand index is not argument position), or a callee this link has no
/// definition for.
///
/// Durability, deliberately NOT claimed to be stable: 28 of the 30
/// currently admitted systemd callees pass because they are today FR-52
/// `unimplemented!` stubs with no body uses. As later waves give them
/// bodies the fence will re-reject some of them. That is the correct
/// direction -- a located link rejection, never a runtime panic.
class ElementZeroFence {
public:
  explicit ElementZeroFence(const llvm::StringMap<Operation *> &definitions)
      : definitions(definitions) {}

  /// True when parameter `slot` of `definition` provably touches only
  /// element zero of the region it is lent.
  bool touchesOnlyElementZero(Operation *definition, unsigned slot) {
    Query query{definition, slot};
    auto cached = cache.find(query);
    if (cached != cache.end())
      return cached->second;
    // Fresh per top-level query: the in-flight set doubles as this query's
    // memo, and a `true` it hands back for a cycle is an ASSUMPTION, not a
    // result, so it must not outlive the query that made it.
    llvm::DenseSet<Query> inFlight;
    bool admitted = walk(definition, slot, inFlight);
    cache[query] = admitted;
    return admitted;
  }

private:
  using Query = std::pair<Operation *, unsigned>;

  static bool isElementZero(Value index) {
    auto constant = index.getDefiningOp<emitrust::ConstantOp>();
    auto value =
        constant ? dyn_cast<IntegerAttr>(constant.getValue()) : IntegerAttr();
    return value && value.getValue().isZero();
  }

  /// `borrow` is a `[0..]` tail borrow of a fenced parameter, so every call
  /// it reaches must itself stay inside element zero at the slot it lands
  /// in.
  bool forwardStaysInElementZero(Value borrow,
                                 llvm::DenseSet<Query> &inFlight) {
    for (Operation *user : borrow.getUsers()) {
      auto call = dyn_cast<emitrust::CallOpaqueOp>(user);
      if (!call || call.getArgs())
        return false;
      auto callee = definitions.find(call.getCallee());
      if (callee == definitions.end())
        return false;
      // One borrow may arrive at more than one parameter; all of them have
      // to pass, so this does not stop at the first match.
      for (auto [index, operand] : llvm::enumerate(call->getOperands()))
        if (operand == borrow &&
            !walk(callee->second, static_cast<unsigned>(index), inFlight))
          return false;
    }
    return true;
  }

  bool walk(Operation *definition, unsigned slot,
            llvm::DenseSet<Query> &inFlight) {
    // A repeat within one query is either a cycle -- which contributes no
    // new use -- or an already-proven parameter: a disproof would have
    // unwound the walk before reaching here.
    if (!inFlight.insert(Query{definition, slot}).second)
      return true;
    auto func = dyn_cast<emitrust::FuncOp>(definition);
    if (!func || func.getBody().empty())
      return false; // No body: nothing to prove anything from.
    Block &entry = func.getBody().front();
    if (slot >= entry.getNumArguments())
      return false;
    for (Operation *user : entry.getArgument(slot).getUsers()) {
      // The reference itself may only be dereferenced. Passing it on
      // whole, or taking its address, hands out the entire region.
      auto deref = dyn_cast<emitrust::DerefOp>(user);
      if (!deref)
        return false;
      for (Operation *place : deref.getResult().getUsers()) {
        if (auto subscript = dyn_cast<emitrust::SubscriptOp>(place)) {
          if (!isElementZero(subscript.getIndex()))
            return false;
          continue;
        }
        auto sliceOf = dyn_cast<emitrust::SliceOfOp>(place);
        if (!sliceOf || !isElementZero(sliceOf.getIndex()) ||
            !forwardStaysInElementZero(sliceOf.getResult(), inFlight))
          return false;
      }
    }
    return true;
  }

  const llvm::StringMap<Operation *> &definitions;
  llvm::DenseMap<Query, bool> cache;
};

/// Re-shapes `shard`'s calls of `name` so that the arguments at `slots`
/// carry the DEFINITION's slice model. The only admitted argument is the
/// `addr_of mut (subscript base[index])` cursor a C `&arr[k]` / `&p[k]`
/// argument imports to; it becomes the equivalent `slice_of mut base[index]`
/// (`&mut base[index..]`).
///
/// Semantics: C's `f(&arr[k])` lends the callee `arr[k..]`, and
/// `&mut arr[k..]` is exactly that range and no wider. The rewrite is also
/// strictly less panicky than the form it replaces -- `k == len` yields a
/// legal empty slice where `&mut arr[len]` panics.
///
/// FR-161: the argument may also be the address of a scalar OBJECT or of a
/// struct FIELD -- the 60-slot residue that has no region to tail-borrow,
/// and which is the C out-parameter idiom throughout. C's `f(&x)` lends a
/// ONE-element region, so the argument is wrapped in the standard library's
/// one-element view, `::std::slice::from_mut`. That wrap is admitted only
/// behind `ElementZeroFence`, which must prove the definition never looks
/// past element zero; see the fence for why an unproven parameter keeps
/// its rejection rather than trading C's undefined behaviour for a panic.
///
/// Everything else keeps a LOCATED rejection: an argument the fence cannot
/// clear, and a call whose arguments are remapped positionally by the
/// `args` attribute (where operand index is not argument position).
static LogicalResult adaptSliceRefinedCalls(ModuleOp shard,
                                            Operation *definition,
                                            llvm::StringRef name,
                                            llvm::ArrayRef<unsigned> slots,
                                            FunctionType declType,
                                            FunctionType defType,
                                            ElementZeroFence &fence) {
  if (Operation *escape = findSliceRefinedSymbolEscape(shard, name))
    return escape->emitError()
           << "unsupported: '" << name
           << "' is used as a value here but the defining translation unit "
              "classifies its parameter "
           << (slots.front() + 1)
           << " as a slice, so this use carries a function-pointer type the "
              "definition does not have";

  bool failed = false;
  shard.walk([&](emitrust::CallOpaqueOp call) {
    if (call.getCallee() != name)
      return;
    if (call.getArgs()) {
      call.emitError() << "unsupported: the call to '" << name
                       << "' remaps its arguments positionally, so it cannot "
                          "be adapted to the defining translation unit's "
                          "slice parameter model";
      failed = true;
      return;
    }
    for (unsigned slot : slots) {
      Value arg = slot < call->getNumOperands() ? call->getOperand(slot)
                                                : Value();
      if (arg && arg.getType() == defType.getInput(slot))
        continue; // Already the definition's model.
      auto addrOf = arg ? arg.getDefiningOp<emitrust::AddrOfOp>()
                        : emitrust::AddrOfOp();
      auto subscript =
          addrOf ? addrOf.getOperand().getDefiningOp<emitrust::SubscriptOp>()
                 : emitrust::SubscriptOp();
      // The element the cursor points at must be the pointee the
      // DECLARATION promised; the module verifier would catch a mismatch
      // downstream, but a link diagnostic beats a verifier failure.
      Type element =
          subscript
              ? cast<emitrust::LValueType>(subscript.getResult().getType())
                    .getValueType()
              : Type();
      auto declRef = dyn_cast<emitrust::MutRefType>(declType.getInput(slot));
      if (!addrOf || !addrOf.getIsMut() || !subscript || !declRef ||
          element != declRef.getPointee()) {
        // FR-161: no region to slice, but a one-element view of the
        // argument is exactly the region C lends. Admitted only when the
        // definition provably stays inside element zero.
        auto argRef = arg ? dyn_cast<emitrust::MutRefType>(arg.getType())
                          : emitrust::MutRefType();
        auto defRef = dyn_cast<emitrust::MutRefType>(defType.getInput(slot));
        auto defSlice = defRef
                            ? dyn_cast<emitrust::SliceType>(defRef.getPointee())
                            : emitrust::SliceType();
        if (argRef && defSlice &&
            defSlice.getElementType() == argRef.getPointee() &&
            fence.touchesOnlyElementZero(definition, slot)) {
          OpBuilder builder(call);
          auto view = builder.create<emitrust::CallOpaqueOp>(
              call.getLoc(), TypeRange{defType.getInput(slot)},
              builder.getStringAttr(kSliceFromMutPath),
              /*args=*/ArrayAttr(), ValueRange{arg});
          call->setOperand(slot, view.getResult(0));
          continue;
        }
        InFlightDiagnostic diag =
            call.emitError()
            << "unsupported: argument " << (slot + 1) << " of the call to '"
            << name
            << "' is a scalar reference but the defining translation unit "
               "classifies that parameter as a slice";
        diag.attachNote(definition->getLoc())
            << "'" << name << "' is defined here as " << defType;
        failed = true;
        continue;
      }
      OpBuilder builder(addrOf);
      auto sliced = builder.create<emitrust::SliceOfOp>(
          addrOf.getLoc(), defType.getInput(slot), subscript.getArray(),
          subscript.getIndex(), /*is_mut=*/true);
      call->setOperand(slot, sliced.getResult());
      // A shared cursor may still feed an UNREFINED callee, so the borrow
      // is dropped only once nothing reads it.
      if (addrOf.getResult().use_empty())
        addrOf.erase();
    }
  });
  return success(!failed);
}

} // namespace

FailureOr<OwningOpRef<ModuleOp>> emitrustcc::mergeLinkShards(
    llvm::MutableArrayRef<OwningOpRef<ModuleOp>> shards,
    llvm::ArrayRef<llvm::SmallVector<unsigned>> ordinalMaps) {
  if (!ordinalMaps.empty() && ordinalMaps.size() != shards.size()) {
    if (!shards.empty())
      (*shards.front())->emitError()
          << "link-shard ordinal maps do not match the shard count";
    return failure();
  }

  // Step 0: strip the FR-57 shard metadata (item-graph text, rejection
  // ledger, source facts) from every shard. It is a per-TU fact the caller
  // has already surfaced; the merged whole-program module must stay
  // byte-comparable to the joint import's, which never carries it.
  for (OwningOpRef<ModuleOp> &shard : shards)
    emitrust::stripShardMetadata(*shard);

  // Step 1: per-shard alpha-rename to global ordinals (the positional
  // default when the caller supplied no maps).
  for (auto [index, shard] : llvm::enumerate(shards)) {
    unsigned positional[1] = {static_cast<unsigned>(index)};
    llvm::ArrayRef<unsigned> map =
        ordinalMaps.empty() ? llvm::ArrayRef<unsigned>(positional)
                            : llvm::ArrayRef<unsigned>(ordinalMaps[index]);
    if (failed(renameShardTags(*shard, map)))
      return failure();
  }

  // One pass over every module-level op of every shard, in link-line order,
  // classifying: definitions (for steps 2 and 3), marked declarations
  // (step 2's obligations), and use/verbatim header ops (step 4).
  llvm::StringMap<Operation *> definitions;
  llvm::SmallVector<std::pair<llvm::StringRef, Operation *>> obligations;
  llvm::StringSet<> headerTexts;
  llvm::SmallVector<Operation *> toErase;
  for (auto [shardIndex, shard] : llvm::enumerate(shards)) {
    // FR-159: the ordinal this shard's per-TU module is named after. It is
    // the shard's own link-line position -- the same ordinal step 1 just
    // renamed its `tu<N>_` tags to -- so `mod tu314` holds exactly the items
    // `tu314_*` came from, and two shards can never claim one module.
    unsigned tuOrdinal = ordinalMaps.empty()
                             ? static_cast<unsigned>(shardIndex)
                             : ordinalMaps[shardIndex].front();
    // Computed lazily and once: a shape conflict is rare (exactly one across
    // the 501-object systemd link), and the walk is linear in the shard.
    llvm::StringMap<Operation *> escapes;
    bool escapesComputed = false;
    bool sinkDisabled = false;
    llvm::StringSet<> shardHeaderTexts;
    for (Operation &op : shard->getBody()->getOperations()) {
      if (isa<emitrust::UseOp, emitrust::VerbatimOp>(op)) {
        // Step 4: a header op an EARLIER shard already carries is dropped;
        // repeats within one shard are the import's own doing and are kept.
        std::string text = printOpToString(&op);
        if (headerTexts.contains(text))
          toErase.push_back(&op);
        else
          shardHeaderTexts.insert(text);
        continue;
      }
      auto symbol = dyn_cast<SymbolOpInterface>(&op);
      if (!symbol)
        continue;
      if (op.hasAttr(emitrust::kExternDeclAttrName)) {
        obligations.push_back({symbol.getName(), &op});
        continue;
      }
      auto [it, inserted] = definitions.try_emplace(symbol.getName(), &op);
      if (inserted)
        continue;
      Operation *first = it->second;
      // Step 3: struct/enum/global definitions repeat in every shard that
      // uses them (shape-dedup'd per TU by the import); first occurrence
      // wins when the shapes agree. Shape equality is structural
      // OperationEquivalence: these module-level defs carry no operands
      // (exactValueMatch is vacuous) and no regions, so equivalence is
      // exactly attributes + types with locations ignored — identical
      // layout under different field names is a CONFLICT, because field
      // names are attributes.
      bool dedupable =
          isa<emitrust::StructDefOp, emitrust::EnumDefOp, emitrust::GlobalOp>(
              op) &&
          first->getName() == op.getName();
      if (dedupable &&
          OperationEquivalence::isEquivalentTo(
              first, &op, OperationEquivalence::exactValueMatch,
              /*markEquivalent=*/nullptr,
              OperationEquivalence::Flags::IgnoreLocations)) {
        toErase.push_back(&op);
        continue;
      }
      // FR-159: a shape-CONFLICTING record that nothing outside this
      // translation unit can name is not an error at all -- it is two C
      // types sharing one tag, which Rust can spell as two PATHS. Sink it
      // into this shard's per-TU module and repath every reference to it
      // WITHIN this shard; the earlier shard's definition keeps the crate
      // root. Anything that makes the record visible across a TU boundary
      // keeps the historical rejection, with a note naming the site.
      Operation *escapeSite = nullptr;
      if (dedupable && isa<emitrust::StructDefOp>(op)) {
        if (!escapesComputed) {
          escapes = collectNonSinkableRecords(*shard, sinkDisabled);
          escapesComputed = true;
        }
        auto escapeIt = escapes.find(symbol.getName());
        if (!sinkDisabled && escapeIt == escapes.end()) {
          std::string sunk = ("crate::tu" + llvm::Twine(tuOrdinal) +
                              "::" + symbol.getName())
                                 .str();
          StringAttr sunkAttr = StringAttr::get(op.getContext(), sunk);
          llvm::StringRef recordName = symbol.getName();
          AttrTypeReplacer repath;
          repath.addReplacement(
              [&](emitrust::StructType structType) -> std::optional<Type> {
                if (structType.getName() != recordName)
                  return std::nullopt;
                return emitrust::StructType::get(structType.getContext(),
                                                 sunkAttr.getValue());
              });
          repath.recursivelyReplaceElementsIn(*shard, /*replaceAttrs=*/true,
                                              /*replaceLocs=*/false,
                                              /*replaceTypes=*/true);
          if (failed(SymbolTable::replaceAllSymbolUses(&op, sunkAttr, *shard)))
            return op.emitError()
                   << "cannot repath uses of '" << recordName << "' at link";
          SymbolTable::setSymbolName(&op, sunkAttr);
          definitions.try_emplace(sunk, &op);
          continue;
        }
        if (escapeIt != escapes.end())
          escapeSite = escapeIt->second;
      }
      InFlightDiagnostic diag =
          dedupable ? op.emitError()
                          << "conflicting definitions of '" << symbol.getName()
                          << "' at link: the shards disagree on its shape"
                    : op.emitError() << "duplicate definition of '"
                                     << symbol.getName() << "' at link";
      diag.attachNote(first->getLoc()) << "first defined here";
      if (escapeSite) {
        StringAttr escapeSymbol = SymbolTable::getSymbolName(escapeSite);
        diag.attachNote(escapeSite->getLoc())
            << "'" << symbol.getName()
            << "' appears in the cross-translation-unit interface of "
            << (escapeSymbol
                    ? (llvm::Twine("'") + escapeSymbol.getValue() + "'").str()
                    : (llvm::Twine("this ") +
                       escapeSite->getName().getStringRef())
                          .str())
            << ", so it cannot be made translation-unit-local";
      }
      return failure();
    }
    for (const auto &entry : shardHeaderTexts)
      headerTexts.insert(entry.getKey());
  }

  // Step 2: declaration-for-definition replacement; an obligation nobody
  // defines is THE undefined-symbol link error.
  //
  // FR-158: an obligation whose type DIFFERS from the definition's cannot
  // simply be dropped -- the declaring shard shaped its call sites for the
  // type it declared, and handing them a definition of another type is
  // rustc E0308 with no source location. When the difference is exactly the
  // scalar/slice pointer-model refinement a body-less declaration must
  // produce, the shard's call arguments are reconciled in place; anything
  // else is a located link rejection. Every diverging obligation is
  // reported before the merge gives up -- a whole-program link that stops
  // at the first of hundreds turns porting into a one-at-a-time loop.
  bool reconciliationFailed = false;
  // One fence per merge: its proofs are keyed on definition operations that
  // stay live for the whole reconciliation, and the same (callee, slot) is
  // asked about once per diverging call site.
  ElementZeroFence elementZeroFence(definitions);
  for (auto [name, op] : obligations) {
    auto it = definitions.find(name);
    if (it != definitions.end()) {
      auto declFn = dyn_cast<FunctionOpInterface>(op);
      auto defFn = dyn_cast<FunctionOpInterface>(it->second);
      if (declFn && defFn &&
          declFn.getFunctionType() != defFn.getFunctionType()) {
        auto declType = dyn_cast<FunctionType>(declFn.getFunctionType());
        auto defType = dyn_cast<FunctionType>(defFn.getFunctionType());
        std::optional<llvm::SmallVector<unsigned>> slots =
            declType && defType ? sliceRefinedSlots(declType, defType)
                                : std::nullopt;
        if (!slots || slots->empty()) {
          InFlightDiagnostic diag =
              op->emitError()
              << "unsupported: '" << name << "' is declared as "
              << declFn.getFunctionType()
              << " but the defining translation unit defines it as "
              << defFn.getFunctionType();
          diag.attachNote(it->second->getLoc()) << "defined here";
          reconciliationFailed = true;
          continue;
        }
        auto shard = op->getParentOfType<ModuleOp>();
        if (mlir::failed(adaptSliceRefinedCalls(shard, it->second, name,
                                                *slots, declType, defType,
                                                elementZeroFence))) {
          reconciliationFailed = true;
          continue;
        }
      }
      toErase.push_back(op);
      continue;
    }
    return op->emitError() << "unresolved external '" << name << "' at link";
  }
  // A shard left half-reconciled must never reach the splice or the
  // verifier: the located rejections above are the report.
  if (reconciliationFailed)
    return failure();

  for (Operation *op : toErase)
    op->erase();

  // Step 5: concatenate the survivors in shard order into the first shard's
  // module, then verify the whole.
  ModuleOp merged = *shards.front();
  for (OwningOpRef<ModuleOp> &shard : shards.drop_front())
    merged.getBody()->getOperations().splice(
        merged.getBody()->end(), shard->getBody()->getOperations());
  if (failed(verify(merged)))
    return failure();
  return std::move(shards.front());
}

//===----------------------------------------------------------------------===//
// FR-58 selective re-import: fact-starvation detection
//===----------------------------------------------------------------------===//

/// The two MEASURED fact-starvation wordings (design.md FR-58, SPIKE 2 and
/// the re-import spike): both are produced by the importer's pointer
/// classification exactly when the target object's shape was out of the
/// solo import's reach. They are matched as substrings of the ledgered
/// diagnostic, the same posture `classifyBlocker`'s tables take toward
/// importer wordings.
static constexpr llvm::StringLiteral kPointerGlobalDiag =
    "unsupported: pointer-typed global variable";
static constexpr llvm::StringLiteral kNoTargetObjectDiag =
    "has no known target object";

bool emitrustcc::isFactStarvedDiagnostic(llvm::StringRef diagnostic) {
  return diagnostic.contains(kPointerGlobalDiag) ||
         diagnostic.contains(kNoTargetObjectDiag);
}

llvm::SmallVector<std::string>
emitrustcc::factStarvedObjectNames(llvm::StringRef symbol,
                                   llvm::StringRef diagnostic) {
  llvm::SmallVector<std::string> names;
  if (diagnostic.contains(kPointerGlobalDiag))
    // The rejected item IS the pointer global; its own C spelling names
    // the object whose shape is missing.
    names.push_back(symbol.str());
  if (diagnostic.contains(kNoTargetObjectDiag)) {
    // "pointer variable 'X' has no known target object": X is quoted.
    size_t open = diagnostic.find('\'');
    if (open != llvm::StringRef::npos) {
      size_t close = diagnostic.find('\'', open + 1);
      if (close != llvm::StringRef::npos && close > open + 1)
        names.push_back(diagnostic.slice(open + 1, close).str());
    }
  }
  return names;
}

llvm::SmallVector<emitrustcc::SignatureStarvation>
emitrustcc::findSignatureStarvedDecls(llvm::ArrayRef<ModuleOp> shards) {
  // Definitions first: symbol -> (shard index, function type). Only
  // FunctionOpInterface symbols participate (see the header: globals are
  // deliberately out of scope).
  llvm::StringMap<std::pair<unsigned, Type>> definitions;
  for (auto [index, shardRef] : llvm::enumerate(shards)) {
    ModuleOp shard = shardRef;
    for (Operation &op : shard.getBody()->getOperations()) {
      if (op.hasAttr(emitrust::kExternDeclAttrName))
        continue;
      auto symbol = dyn_cast<SymbolOpInterface>(&op);
      auto func = dyn_cast<FunctionOpInterface>(&op);
      if (!symbol || !func)
        continue;
      definitions.try_emplace(symbol.getName(),
                              std::make_pair(static_cast<unsigned>(index),
                                             func.getFunctionType()));
    }
  }
  llvm::SmallVector<SignatureStarvation> starved;
  for (auto [index, shardRef] : llvm::enumerate(shards)) {
    ModuleOp shard = shardRef;
    for (Operation &op : shard.getBody()->getOperations()) {
      if (!op.hasAttr(emitrust::kExternDeclAttrName))
        continue;
      auto symbol = dyn_cast<SymbolOpInterface>(&op);
      auto func = dyn_cast<FunctionOpInterface>(&op);
      if (!symbol || !func)
        continue;
      auto it = definitions.find(symbol.getName());
      if (it == definitions.end())
        continue; // No definition anywhere: the undefined-symbol link
                  // error's territory, not starvation.
      auto [defShard, defType] = it->second;
      if (defType == func.getFunctionType())
        continue;
      starved.push_back({static_cast<unsigned>(index), defShard,
                         symbol.getName().str()});
    }
  }
  return starved;
}

llvm::SmallVector<llvm::StringRef>
emitrustcc::itemGraphGlobalDefs(llvm::StringRef graphText) {
  // One graph line per item, `node <symbol> kind=global def=1 ...`; every
  // field is a whole space-separated token (the format's stated grep
  // contract), so token-wise splitting is exact.
  llvm::SmallVector<llvm::StringRef> symbols;
  for (llvm::StringRef line : llvm::split(graphText, '\n')) {
    if (!line.consume_front("node "))
      continue;
    auto [symbol, rest] = line.split(' ');
    if (rest.starts_with("kind=global def=1 "))
      symbols.push_back(symbol);
  }
  return symbols;
}
