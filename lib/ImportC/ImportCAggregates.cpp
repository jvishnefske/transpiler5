//===- ImportCAggregates.cpp - struct/union/enum import --------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's aggregate-type import: importRecord/collectRecordFields/
/// collectUnionSlot (struct and union layout, including bit-field and
/// anonymous-union-arm flattening, a C++ `class`'s data members treated
/// like a `struct`'s, and a located rejection for base classes, W2.0) and
/// importEnum, plus the small ordinary-name-collision and
/// struct-symbol-naming helpers they share (collectOrdinaryNames'
/// recursive `namespace`/`extern "C"` walk is also W2.0). Split out of
/// ImportC.cpp by pure code motion (W1.7); see CImporterInternal.h for the
/// CImporter class declaration this file implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

bool CImporter::ordinaryNameTaken(llvm::StringRef name) const {
  if (ordinaryTuNames.contains(name))
    return true;
  Operation *existing = SymbolTable::lookupSymbolIn(module, name);
  return existing && !llvm::isa<emitrust::StructDefOp>(existing);
}

void CImporter::collectStaticLocalNames(const clang::Stmt *stmt,
                                        llvm::StringRef funcName) {
  if (!stmt)
    return;
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
        if (var->isStaticLocal())
          ordinaryTuNames.insert(globalRustName(
              (llvm::Twine(funcName) + "_" + var->getName()).str()));
  for (const clang::Stmt *child : stmt->children())
    collectStaticLocalNames(child, funcName);
}

void CImporter::collectOrdinaryNames(const clang::TranslationUnitDecl *unit) {
  ordinaryTuNames.clear();
  ordinaryRawTuNames.clear();
  collectOrdinaryNamesFrom(unit);
}

void CImporter::collectOrdinaryNamesFrom(const clang::DeclContext *context) {
  for (const clang::Decl *decl : context->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    // W2.0: mirror importDeclsIn's recursion into `namespace`/`extern "C"`
    // bodies so the pre-scanned name set matches what will actually be
    // emitted (namespace-flattened function/variable names included).
    if (const auto *linkageSpec = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      collectOrdinaryNamesFrom(linkageSpec);
      continue;
    }
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      collectOrdinaryNamesFrom(ns);
      continue;
    }
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      std::string funcName = mlirFuncName(func);
      ordinaryTuNames.insert(funcName);
      ordinaryRawTuNames.insert(func->getName());
      // Function-local statics surface at module level under their
      // `<function>_<name>` mangle (see emitLocalVar), claiming that
      // spelling in the ordinary namespace.
      if (func->hasBody())
        collectStaticLocalNames(func->getBody(), funcName);
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      ordinaryTuNames.insert(globalVarSymbolName(var));
      ordinaryRawTuNames.insert(var->getName());
    }
  }
}

FailureOr<std::string>
CImporter::structSymbolName(const clang::RecordDecl *definition,
                            Location loc) {
  auto cached = assignedStructNames.find(definition);
  if (cached != assignedStructNames.end())
    return cached->second;
  std::string base = recordRustName(definition);
  if (base.empty())
    return std::string(); // Anonymous struct; callers reject it.
  std::string assigned = base;
  if (ordinaryNameTaken(assigned)) {
    // C's tag namespace is separate from the ordinary one (C99 6.2.3);
    // the module symbol table is not, so the tag yields deterministically.
    // Under the idiomatic rename the disambiguated name stays UpperCamelCase.
    assigned = typeRustName("Struct_" + base);
    if (ordinaryNameTaken(assigned))
      return emitError(loc)
             << "unsupported: struct '" << base
             << "' collides with an ordinary identifier, and so does its "
                "renamed spelling '"
             << assigned << "'";
  }
  assignedStructNames[definition] = assigned;
  return assigned;
}

LogicalResult CImporter::importRecord(const clang::RecordDecl *record,
                                      Location loc) {
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition)
    return success(); // Forward declaration; imported once completed or used.
  // A record this import already rejected has NO struct_def in the module, so
  // there is no Rust type for a caller to name. Saying so here — at the use
  // site, which is where the type was wanted — is what keeps a recovering
  // import from emitting a field, a local or a parameter whose type does not
  // exist. See the declaration in CImporterInternal.h for why the ordinary
  // `importedRecords` memo cannot answer this.
  if (rejectedRecords.contains(definition)) {
    std::string rejectedName = recordRustName(definition);
    return emitError(loc)
           << "unsupported: struct '"
           << (rejectedName.empty() ? llvm::StringRef("<anonymous>")
                                    : rejectedName)
           << "' was rejected, so a type naming it cannot be imported";
  }
  LogicalResult imported = importRecordUncached(definition);
  if (failed(imported))
    rejectedRecords.insert(definition);
  return imported;
}

LogicalResult
CImporter::importRecordUncached(const clang::RecordDecl *definition) {
  // Every diagnostic below is located on the DEFINITION: it is a verdict on
  // this record, not on whoever asked for it. The use-site location matters
  // only for the repeat rejection in `importRecord` above.
  Location defLoc = translateLoc(definition->getBeginLoc());
  // W2.0: a C++ `class` (TTK_Class) imports exactly like a `struct` — the
  // keyword only changes the DEFAULT member access, which the importer
  // ignores anyway (it walks `fields()`, skipping AccessSpecDecl entries
  // and any CXXMethodDecl, since neither is a FieldDecl). Base classes are
  // rejected separately, in `collectRecordFields`, before any field is
  // collected.
  if (!definition->isStruct() && !definition->isUnion() &&
      !definition->isClass())
    return emitError(defLoc) << "unsupported record declaration";
  // CTS-BR (00216): a byte-region record never emits a struct_def — its
  // objects are plain byte arrays (`mapType` maps the record type
  // directly), so the eager file-scope record import is a no-op.
  if (isByteRegionRecord(definition))
    return success();
  if (!importedRecords.insert(definition).second)
    return success();
  std::string structName = recordRustName(definition);
  if (!structName.empty() && isRustKeyword(structName))
    return emitError(defLoc) << "unsupported: struct name '" << structName
                             << "' is a Rust keyword";

  SmallVector<llvm::StringRef> fieldNames;
  SmallVector<Type> fieldTypes;
  // A union flattens to its single storage slot; every other record keeps
  // the anonymous-member-resolving field walk.
  if (definition->isUnion()) {
    if (failed(collectUnionSlot(definition, fieldNames, fieldTypes)))
      return failure();
  } else if (unsigned bitFieldRuns = 0; failed(collectRecordFields(
                 definition, fieldNames, fieldTypes, bitFieldRuns))) {
    return failure();
  }
  // An empty member list (`struct T {};`, a GNU/C2x shape clang accepts) is
  // permitted and becomes a unit-like Rust struct.

  // C tag identity is (tag name, scope), and every block-scope declaration
  // of a tag introduces a new type (C99 6.2.1) — a `struct T` inside a
  // block may shadow a file-scope `struct T` with a different shape, and
  // even a same-shaped redeclaration is a distinct type. clang has already
  // resolved the scoping, so the defining decl is the identity; only the
  // emitted Rust name needs disambiguation. Block-scope records take the
  // function-local-static mangling convention `<function>_<tag>`
  // (`_<n>`-suffixed if that name is taken) and skip the name-keyed
  // cross-TU dedup below, which exists solely to merge the same file-scope
  // definition reached through a shared header in several TUs. A bare
  // anonymous record has no tag to mangle and is excluded: whatever its
  // scope, it takes the shape-keyed `Anon<n>` path below, where the
  // defining decl is already the identity and the shape is the name key.
  if (!structName.empty() &&
      !definition->getDeclContext()->getRedeclContext()->isFileContext()) {
    const clang::FunctionDecl *enclosing = nullptr;
    for (const clang::DeclContext *ctx = definition->getDeclContext();
         ctx && !enclosing; ctx = ctx->getParent())
      enclosing = llvm::dyn_cast<clang::FunctionDecl>(ctx);
    if (!enclosing)
      return emitError(defLoc)
             << "unsupported: struct definition outside file or function "
                "scope";
    // A block-scope record's `<fn>_<tag>` disambiguator is a type name, so it
    // takes the type spelling (UpperCamelCase under the idiomatic rename).
    std::string mangledBase = typeRustName(
        (llvm::Twine(mlirFuncName(enclosing)) + "_" + structName).str());
    std::string mangled = mangledBase;
    // Each probe below tries a fresh suffix, so the loop takes at most one
    // step per already-emitted struct name — bounded and deterministic.
    for (unsigned suffix = 2; emittedStructNames.contains(mangled); ++suffix)
      mangled = idiomaticRenameEnabled()
                    ? (llvm::Twine(mangledBase) + llvm::Twine(suffix)).str()
                    : (llvm::Twine(mangledBase) + "_" + llvm::Twine(suffix))
                          .str();
    emittedStructNames.insert(mangled);
    llvm::StringRef mangledRef =
        localRecordNames.try_emplace(definition, std::move(mangled))
            .first->second;
    structDefRecords[mangledRef] = definition;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::StructDefOp>(
        defLoc, moduleBuilder.getStringAttr(mangledRef),
        moduleBuilder.getStrArrayAttr(fieldNames),
        moduleBuilder.getTypeArrayAttr(fieldTypes));
    return success();
  }

  // Cross-TU deduplication (file-scope records only): the same struct
  // reached through a shared header has distinct decls in each TU. Dedup by
  // symbol name; an identical shape is skipped, a name reused with a
  // different field shape is a diagnostic.
  std::string shape;
  {
    llvm::raw_string_ostream os(shape);
    for (auto [fieldName, fieldType] : llvm::zip(fieldNames, fieldTypes))
      os << fieldName << ':' << fieldType << ';';
  }

  // File-scope named records go through the tag-versus-ordinary-namespace
  // collision renaming (C99 6.2.3): `struct a` and a global or function
  // `a` may coexist in C, so the tag is renamed to `Struct_<tag>` exactly
  // when the ordinary namespace claims the spelling (see
  // `structSymbolName`, which caches the decision per defining decl for
  // `emittedRecordName`). The renamed spelling is what the shape dedup and
  // the emitted struct_def below use.
  std::string assignedStorage;
  if (!structName.empty()) {
    FailureOr<std::string> assigned = structSymbolName(definition, defLoc);
    if (failed(assigned))
      return failure();
    assignedStorage = std::move(*assigned);
    structName = assignedStorage;
  }

  // A bare anonymous struct (no tag, no typedef name) gets a synthesized
  // `Anon<n>` name that is a deterministic function of its field shape:
  // the shape is the key, so the same anonymous shape anywhere in the
  // project reuses one name (and, through the shape dedup below, one
  // struct_def), while distinct shapes always get distinct names. The
  // counter only orders first encounters; it never influences which name a
  // given shape maps to within an import. `anonRecordShapeNames` is
  // consulted only for anonymous structs, so a named struct with the same
  // shape keeps its own Rust type.
  if (structName.empty()) {
    auto known = anonRecordShapeNames.find(shape);
    if (known != anonRecordShapeNames.end()) {
      structName = known->second;
    } else {
      std::string synthesized;
      do {
        synthesized = ("Anon" + llvm::Twine(anonStructCounter++)).str();
      } while (importedRecordShapes.contains(synthesized) ||
               importedEnumShapes.contains(synthesized) ||
               ordinaryNameTaken(synthesized));
      structName =
          anonRecordShapeNames.try_emplace(shape, synthesized).first->second;
    }
    anonRecordNames.try_emplace(definition, structName);
  }

  auto existingShape = importedRecordShapes.find(structName);
  if (existingShape != importedRecordShapes.end()) {
    if (existingShape->second != shape)
      return emitError(defLoc)
             << "unsupported: conflicting definition of struct '" << structName
             << "' with a different shape in another translation unit";
    return success();
  }
  // A file-scope tag that lands on a name already claimed by a mangled
  // block-scope record cannot be merged (they are different C types) and
  // cannot share the symbol; reject with a located diagnostic, mirroring
  // createGlobal's collision policy for mangled local statics.
  if (emittedStructNames.contains(structName))
    return emitError(defLoc)
           << "unsupported: struct name '" << structName
           << "' collides with the mangled name of a block-scope struct";
  importedRecordShapes[structName] = shape;
  emittedStructNames.insert(structName);
  structDefRecords[structName] = definition;

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::StructDefOp>(
      defLoc, moduleBuilder.getStringAttr(structName),
      moduleBuilder.getStrArrayAttr(fieldNames),
      moduleBuilder.getTypeArrayAttr(fieldTypes));
  // W2.2: a genuine C++ class's non-static-data-member methods (mutating,
  // const, static, and non-delegating constructors) import onto the
  // `emitrust.impl`/`emitrust.method_of` surface right after the struct
  // itself, so every method call site reached later in the same
  // (single-pass, declaration-order) import sees an already-imported
  // target — mirroring this file's struct-import-order pin
  // (cpp-basics.cpp). Destructor/virtual/operator-overload rejections
  // already ran above, in `collectRecordFields`, before any field of this
  // struct was collected.
  if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(definition))
    if (failed(importCXXMethods(cxxRecord)))
      return failure();
  return success();
}

LogicalResult
CImporter::importCXXMethods(const clang::CXXRecordDecl *record) {
  // Whether `method` is one this walk imports at all; the two passes below
  // must agree exactly, or the definition pass would emit a body for a
  // symbol the signature pass never registered (or leave a stub behind).
  auto isImportable = [](const clang::CXXMethodDecl *method) {
    // Compiler-synthesized special members are out of scope, and so is an
    // EXPLICITLY defaulted one (`= default`): it has no user-written body to
    // import, so `importFunction` would ask for a null `getBody()` and the
    // statement walk would deref it and crash. Like the implicit member it
    // stands in for, a defaulted special member is realized structurally (a
    // defaulted default constructor becomes `derive(Default)`), never as an
    // imported function body. The copy/move/delegating rejection below runs
    // on the broader "user-declared" predicate so a *defaulted* copy/move
    // ctor is still rejected rather than silently skipped here.
    return !method->isImplicit() && !method->isDeleted() &&
           !method->isDefaulted();
  };
  // Destructors, virtual methods, and overloaded operators were already
  // rejected in `collectRecordFields`, before this class's struct_def (and
  // so before this walk) ever ran; a copy/move/delegating constructor is
  // out of the method wave's scope too (no value/aliasing semantics modeled
  // for it) and is rejected here, the first point a constructor is
  // inspected individually. Kept ahead of BOTH passes so a rejected class
  // never half-imports.
  for (const clang::CXXMethodDecl *method : record->methods()) {
    // Broader than `isImportable`: a copy/move/delegating constructor is
    // rejected even when `= default`, whereas `isImportable` (used by the two
    // import passes below) additionally skips defaulted members so a null
    // body is never emitted. Compiler-synthesized members carry none of these
    // shapes and are skipped.
    if (method->isImplicit() || method->isDeleted())
      continue;
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(method))
      if (ctor->isCopyOrMoveConstructor() || ctor->isDelegatingConstructor())
        return emitError(translateLoc(ctor->getLocation()))
               << "unsupported: copy/move/delegating constructor";
  }
  // FR-47, pass 1: register every method's SIGNATURE as an external stub
  // before any body imports. A C++ member function body is a
  // complete-class context — every member is visible from every other one,
  // regardless of declaration order — whereas the single-pass,
  // declaration-order import this file was built around only ever resolved
  // a callee that had already been imported. Without this pass, a class
  // whose methods call each other rejects with "call to unimported method"
  // for every call that runs "up" the declaration order, most notably a
  // constructor calling a method declared after it (`A() { init(); }`),
  // which is the ordinary way such classes are written.
  //
  // Two alternatives were rejected. (a) Importing the missing callee ON
  // DEMAND at the call site: a pair of mutually recursive methods would
  // recurse forever, and it would make the emitted symbol order depend on
  // call order. (b) Sorting methods into a call-graph topological order:
  // no such order exists for mutual recursion, and it would still be a
  // second, divergable notion of "which methods this class has". The
  // prepass costs one extra signature computation per method and reuses
  // the redeclaration reconciliation `importFunction` already performs for
  // a C prototype later satisfied by its definition.
  for (const clang::CXXMethodDecl *method : record->methods())
    if (isImportable(method))
      if (failed(importFunction(method, /*signatureOnly=*/true)))
        return failure();
  // Pass 2: import each body. A method DEFINED OUT OF LINE (declared in the
  // class, defined in a `.cpp`) has no body here, so it stays the stub pass
  // 1 built and is filled in when the translation-unit walk reaches its
  // out-of-line definition — exactly the behavior this walk had before.
  for (const clang::CXXMethodDecl *method : record->methods())
    if (isImportable(method))
      if (failed(importFunction(method)))
        return failure();
  return success();
}

LogicalResult CImporter::collectRecordFields(
    const clang::RecordDecl *record,
    SmallVectorImpl<llvm::StringRef> &fieldNames,
    SmallVectorImpl<Type> &fieldTypes, unsigned &bitFieldRuns) {
  // W2.0: a class/struct with base classes is rejected instead of
  // silently dropping them — importing only the derived class's own
  // fields would produce a struct with the wrong layout and no
  // inherited data at all, a data-loss hazard rather than a merely
  // unsupported construct. Checked before any field is collected, and
  // ahead of the (unrelated) anonymous-struct-member recursion below, so
  // every entry point into this function (top-level and recursive) sees
  // it. A record with no base classes reaches this function whether it
  // is a C `struct`, a bare C++ `class`, or a `struct`/`class` that
  // simply lists none, and imports exactly like a C struct either way.
  if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
    if (cxxRecord->getNumBases() > 0)
      return emitError(translateLoc(cxxRecord->bases_begin()->getBeginLoc()))
             << "unsupported: base classes are not supported";
    // W2.2: a user-declared destructor (no drop semantics modeled), a
    // virtual method (no vtable/dynamic dispatch), or an overloaded
    // operator (no operator-overload lowering) is rejected at the
    // member's own declaration — checked here, before any field (or
    // method) of the class imports, so a rejected class never
    // half-imports (no struct_def, no methods). Compiler-synthesized
    // special members the class did not itself declare are skipped: they
    // carry none of these three shapes and never surface a diagnostic.
    for (const clang::CXXMethodDecl *method : cxxRecord->methods()) {
      if (method->isImplicit() || method->isDeleted())
        continue;
      Location methodLoc = translateLoc(method->getLocation());
      if (llvm::isa<clang::CXXDestructorDecl>(method))
        return emitError(methodLoc)
               << "unsupported: user-declared destructor";
      if (method->isVirtual())
        return emitError(methodLoc) << "unsupported: virtual method";
      if (method->isOverloadedOperator())
        return emitError(methodLoc) << "unsupported: overloaded operator";
    }
  }
  // Interns a synthesized or mangled member spelling in the arena so the
  // StringRef stored in the field list (and in `bitFieldAccessInfo`)
  // stays valid for the import's lifetime.
  auto internName = [&](std::string name) -> llvm::StringRef {
    return memberNameArena.emplace_back(std::move(name));
  };
  // Appends one field under the collision guard: C member names are
  // unique within a record, so a duplicate FINAL spelling can only come
  // from keyword mangling (`type` next to `type_`) or from a member
  // spelled like a synthesized `__bits<n>` backing field.
  auto appendField = [&](llvm::StringRef finalName, Type type, Location loc,
                         llvm::StringRef original) -> LogicalResult {
    if (llvm::is_contained(fieldNames, finalName))
      return emitError(loc)
             << "unsupported: struct member '" << original
             << "' maps to the Rust field name '" << finalName
             << "', which collides with another member";
    fieldNames.push_back(finalName);
    fieldTypes.push_back(type);
    return success();
  };
  SmallVector<const clang::FieldDecl *> fields;
  for (const clang::FieldDecl *field : record->fields())
    fields.push_back(field);
  for (unsigned index = 0, count = fields.size(); index != count; ++index) {
    const clang::FieldDecl *field = fields[index];
    Location fieldLoc = translateLoc(field->getLocation());
    if (field->isBitField()) {
      // C99-45: pack the maximal run of consecutively declared bit-field
      // members, LSB-first in declaration order, into one backing field.
      uint64_t totalBits = 0;
      unsigned runEnd = index;
      for (; runEnd != count && fields[runEnd]->isBitField(); ++runEnd) {
        const clang::FieldDecl *bitField = fields[runEnd];
        Location bitFieldLoc = translateLoc(bitField->getLocation());
        // A zero-width bit-field is a pure ABI padding directive; the
        // backing-run layout is deliberately NOT ABI-compatible, so
        // honoring it would promise a layout this model does not keep.
        if (bitField->isZeroLengthBitField())
          return emitError(bitFieldLoc)
                 << "unsupported: zero-width bit-field member";
        // An anonymous bit-field is likewise padding-only.
        if (bitField->isUnnamedBitField() || bitField->getName().empty())
          return emitError(bitFieldLoc)
                 << "unsupported: anonymous bit-field member";
        totalBits += bitField->getBitWidthValue();
      }
      if (totalBits > 64)
        return emitError(fieldLoc)
               << "unsupported: bit-field run wider than 64 bits";
      unsigned backingWidth =
          totalBits <= 8 ? 8 : totalBits <= 16 ? 16 : totalBits <= 32 ? 32
                                                                      : 64;
      auto backingType = IntegerType::get(builder.getContext(), backingWidth,
                                          IntegerType::Unsigned);
      llvm::StringRef backingName =
          internName(("__bits" + llvm::Twine(bitFieldRuns++)).str());
      unsigned offset = 0;
      for (unsigned i = index; i != runEnd; ++i) {
        const clang::FieldDecl *bitField = fields[i];
        bitFieldAccessInfo[bitField] =
            BitFieldAccess{backingName, backingType, offset,
                           bitField->getBitWidthValue()};
        offset += bitField->getBitWidthValue();
      }
      if (failed(appendField(backingName, backingType, fieldLoc, backingName)))
        return failure();
      index = runEnd - 1; // The loop increment lands on the run's end.
      continue;
    }
    if (field->isAnonymousStructOrUnion()) {
      // C11 6.7.2.1p13: the members of an anonymous struct/union member
      // are considered members of the containing structure. Recursion
      // depth is the member nesting depth of the source, so it is bounded
      // by the program text.
      const clang::RecordDecl *member =
          field->getType()->getAsRecordDecl()->getDefinition();
      if (member->isStruct()) {
        // Sema has already enforced that the injected spellings are
        // unique in the parent's member namespace (a collision is a
        // clang "member of anonymous struct redeclares" error), so the
        // fields keep their own names. The run counter threads through so
        // backing names stay unique across the whole flattened record.
        if (failed(collectRecordFields(member, fieldNames, fieldTypes,
                                       bitFieldRuns)))
          return failure();
        continue;
      }
      // Anonymous union member: modeled without a union type exactly
      // when every arm flattens to a single leaf field and all leaves
      // map to one identical type. The arms then alias one storage slot
      // named after the first leaf, which is exact — reading any union
      // member with the type of the last store yields that stored value
      // (same object representation, same type). Any other shape stays
      // in CTS-R3 territory and rejects exactly as union types do
      // elsewhere.
      Location unionLoc = translateLoc(member->getBeginLoc());
      const clang::FieldDecl *storage = nullptr;
      Type slotType;
      for (const clang::FieldDecl *arm : member->fields()) {
        FailureOr<const clang::FieldDecl *> leaf =
            anonymousUnionArmLeaf(arm, unionLoc);
        if (failed(leaf))
          return failure();
        Location leafLoc = translateLoc((*leaf)->getLocation());
        FailureOr<Type> leafType = mapType((*leaf)->getType(), leafLoc);
        if (failed(leafType))
          return failure();
        if (!storage) {
          storage = *leaf;
          slotType = *leafType;
          if (failed(appendField(
                  internName(mangleMemberName(storage->getName())), slotType,
                  leafLoc, storage->getName())))
            return failure();
          continue;
        }
        if (*leafType != slotType)
          return emitError(unionLoc) << "unsupported: union type";
        unionSlotStorage[*leaf] = storage;
      }
      if (!storage) // An empty anonymous union has no representable slot.
        return emitError(unionLoc) << "unsupported: union type";
      continue;
    }
    if (field->getName().empty())
      return emitError(fieldLoc) << "unsupported: unnamed struct member";
    // C99-17, AMENDED by CTS-BR (00216): a flexible array member
    // (C99 6.7.2.1p16, `T tail[];`) is TOLERATED at the declaration — it
    // contributes no field and no size, matching C's sizeof — and only
    // RUNTIME accesses to the tail reject, with a dedicated located
    // wording (see `checkSpecialArrayMemberAccess`). GNU zero-length
    // array members (`T r[0];`) get the same zero-size, field-less
    // treatment.
    if (field->getType()->isIncompleteArrayType())
      continue;
    if (const clang::ConstantArrayType *zeroLength =
            astContext().getAsConstantArrayType(field->getType());
        zeroLength && zeroLength->getSize().isZero())
      continue;
    // A FILE* member of a MAIN-FILE record would store an owned handle
    // inside an aggregate, which the function-local handle model does not
    // cover (C99-48); the check must precede the data-pointer cursor
    // mapping in mapStructFieldType. System-header records (the stdio
    // stream record's own FILE*-typed links) keep the historical cursor
    // field so their import stays inert.
    if (isFilePtrType(field->getType()) && !isSystemHeaderDecl(field))
      return emitError(fieldLoc)
             << "unsupported: FILE* is only supported as a function-local "
                "variable";
    // An admitted `void *` fn-ptr member (CTS-BR, 00216) retypes to its
    // one target signature's fn_ptr.
    clang::QualType declaredType = field->getType();
    if (clang::QualType retyped = fnPtrMemberTypes.lookup(field);
        !retyped.isNull())
      declaredType = retyped;
    FailureOr<Type> fieldType =
        mapStructFieldType(declaredType, fieldLoc, field);
    if (failed(fieldType))
      return failure();
    if (failed(appendField(internName(mangleMemberName(field->getName())),
                           *fieldType, fieldLoc, field->getName())))
      return failure();
  }
  return success();
}

FailureOr<const clang::FieldDecl *>
CImporter::anonymousUnionArmLeaf(const clang::FieldDecl *arm,
                                 Location unionLoc) {
  Location armLoc = translateLoc(arm->getLocation());
  // A bit-field arm is not addressable storage the one-slot aliasing can
  // model; same policy (and wording) as `collectUnionSlot`'s.
  if (arm->isBitField())
    return emitError(armLoc) << "unsupported: union with a bit-field arm";
  if (!arm->isAnonymousStructOrUnion()) {
    if (arm->getName().empty())
      return emitError(armLoc) << "unsupported: unnamed struct member";
    return arm;
  }
  // A nested anonymous struct arm contributes exactly its own flattened
  // fields; single-slot aliasing admits it only when that is one leaf.
  // (A nested anonymous union arm with several arms of its own is
  // conservatively rejected the same way.)
  const clang::RecordDecl *record =
      arm->getType()->getAsRecordDecl()->getDefinition();
  const clang::FieldDecl *leaf = nullptr;
  for (const clang::FieldDecl *inner : record->fields()) {
    if (leaf) // A second field: the arm is wider than one slot.
      return emitError(unionLoc) << "unsupported: union type";
    FailureOr<const clang::FieldDecl *> innerLeaf =
        anonymousUnionArmLeaf(inner, unionLoc);
    if (failed(innerLeaf))
      return failure();
    leaf = *innerLeaf;
  }
  if (!leaf) // An empty arm has no slot to alias.
    return emitError(unionLoc) << "unsupported: union type";
  return leaf;
}

LogicalResult CImporter::collectUnionSlot(
    const clang::RecordDecl *definition,
    SmallVectorImpl<llvm::StringRef> &fieldNames,
    SmallVectorImpl<Type> &fieldTypes) {
  Location unionLoc = translateLoc(definition->getBeginLoc());
  // Structural arm checks, and slot selection: the slot is the first arm,
  // EXCEPT in the byte-array mix (CTS-F, 00210) — a union pairing a
  // non-array arm with constant integer-array arms takes the first
  // NON-ARRAY arm as its slot regardless of declaration order (the array
  // arms are width-checked type-level aliases whose accesses reject at
  // the access site).
  auto integerArrayArm =
      [&](const clang::FieldDecl *arm) -> const clang::ConstantArrayType * {
    const clang::ConstantArrayType *array =
        astContext().getAsConstantArrayType(arm->getType());
    return array && array->getElementType()->isIntegerType() ? array
                                                             : nullptr;
  };
  const clang::FieldDecl *storage = nullptr;
  bool hasIntegerArrayArm = false;
  const clang::FieldDecl *firstNonArrayArm = nullptr;
  for (const clang::FieldDecl *arm : definition->fields()) {
    // A bit-field arm is not addressable storage the slot can alias.
    if (arm->isBitField())
      return emitError(unionLoc) << "unsupported: union with a bit-field arm";
    if (arm->isAnonymousStructOrUnion() || arm->getName().empty())
      return emitError(unionLoc) << "unsupported: union with an unnamed arm";
    // A pointer arm never aliases another slot under the decomposed
    // pointer model, whatever its width; checked before `mapType` so the
    // rejection is the union's, not the pointer's.
    if (isPointerType(arm->getType()))
      return emitError(unionLoc) << "unsupported: union with a pointer arm";
    if (!storage)
      storage = arm; // Declaration order: the first arm.
    if (integerArrayArm(arm))
      hasIntegerArrayArm = true;
    else if (!firstNonArrayArm)
      firstNonArrayArm = arm;
  }
  // An empty union (a GNU extension clang accepts in C) has no first arm
  // and therefore no representable slot.
  if (!storage)
    return emitError(unionLoc) << "unsupported: union with no members";
  if (hasIntegerArrayArm && firstNonArrayArm)
    storage = firstNonArrayArm;

  Location slotLoc = translateLoc(storage->getLocation());
  FailureOr<Type> slotMapped = mapType(storage->getType(), slotLoc);
  if (failed(slotMapped))
    return failure();
  Type slotType = *slotMapped;
  // A keyword-spelled slot name mangles exactly like a struct member's
  // (`mangleMemberName`); the arena keeps the spelling alive for the
  // struct_def attribute below.
  fieldNames.push_back(
      memberNameArena.emplace_back(mangleMemberName(storage->getName())));
  fieldTypes.push_back(slotType);

  auto slotInt = llvm::dyn_cast<IntegerType>(slotType);
  // The bit width of a scalar (integer or float) mapped type; 0 for
  // aggregates, enums, and every other non-scalar type.
  auto scalarWidth = [](Type type) -> unsigned {
    if (auto intType = llvm::dyn_cast<IntegerType>(type))
      return intType.getWidth();
    if (auto floatType = llvm::dyn_cast<FloatType>(type))
      return floatType.getWidth();
    return 0;
  };
  for (const clang::FieldDecl *arm : definition->fields()) {
    if (arm == storage)
      continue;
    // A constant integer-array arm whose total width equals the integer
    // slot's admits as a TYPE-level alias (CTS-F, 00210): its spelling
    // never reaches the IR, and any access through it rejects at the
    // access site (`unsupported: union byte-array arm access`). An
    // unequal-total-width array arm — or one over a float slot — falls
    // through to the family rejections below.
    if (integerArrayArm(arm) && slotInt &&
        astContext().getTypeSize(arm->getType()) == slotInt.getWidth()) {
      unionByteArrayArms.insert(arm);
      continue;
    }
    Location armLoc = translateLoc(arm->getLocation());
    FailureOr<Type> armType = mapType(arm->getType(), armLoc);
    if (failed(armType))
      return failure();
    // An identical mapped type aliases the slot exactly: reading any
    // union member with the type of the last store yields that value.
    if (*armType == slotType) {
      unionSlotStorage[arm] = storage;
      continue;
    }
    unsigned slotWidth = scalarWidth(slotType);
    unsigned armWidth = scalarWidth(*armType);
    // Same-width scalars alias the slot bit-exactly (C99 6.5.2.3 union
    // punning over one object representation): integers differing only
    // in signedness reinterpret through a same-width `emitrust.cast`
    // (two's complement), and a float paired with a same-width integer
    // through an `emitrust.bitcast` (`to_bits`/`from_bits`) — both at
    // the access sites (`reinterpretUnionArmRead`/`Write`).
    if (slotWidth != 0 && slotWidth == armWidth) {
      unionSlotStorage[arm] = storage;
      continue;
    }
    if (slotWidth != 0 && armWidth != 0)
      return emitError(unionLoc)
             << "unsupported: union arms of differing sizes";
    return emitError(unionLoc)
           << "unsupported: union arm cannot alias the storage slot";
  }
  return success();
}

const clang::FieldDecl *
CImporter::flattenedFieldStorage(const clang::FieldDecl *field) const {
  auto storage = unionSlotStorage.find(field);
  return storage == unionSlotStorage.end() ? field : storage->second;
}

std::string
CImporter::flattenedFieldName(const clang::FieldDecl *field) const {
  return mangleMemberName(flattenedFieldStorage(field)->getName());
}

Value CImporter::reinterpretScalarBits(Location loc, Value value,
                                       Type target) {
  if (value.getType() == target)
    return value;
  if (llvm::isa<FloatType>(value.getType()) || llvm::isa<FloatType>(target))
    return builder.create<emitrust::BitcastOp>(loc, target, value)
        .getResult();
  return builder.create<emitrust::CastOp>(loc, target, value).getResult();
}

FailureOr<Value> CImporter::reinterpretUnionArmRead(const clang::Expr *expr,
                                                    Value value,
                                                    Location loc) {
  const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripTrivia(expr));
  if (!member)
    return value;
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field || flattenedFieldStorage(field) == field)
    return value;
  FailureOr<Type> armType = mapType(field->getType(), loc);
  if (failed(armType))
    return failure();
  return reinterpretScalarBits(loc, value, *armType);
}

FailureOr<Value> CImporter::reinterpretUnionArmWrite(const clang::Expr *expr,
                                                     Value value,
                                                     Location loc) {
  const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripTrivia(expr));
  if (!member)
    return value;
  const auto *field =
      llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field)
    return value;
  const clang::FieldDecl *storage = flattenedFieldStorage(field);
  if (storage == field)
    return value;
  FailureOr<Type> slotType = mapType(storage->getType(), loc);
  if (failed(slotType))
    return failure();
  return reinterpretScalarBits(loc, value, *slotType);
}

std::string
CImporter::emittedRecordName(const clang::RecordDecl *definition) const {
  auto local = localRecordNames.find(definition);
  if (local != localRecordNames.end())
    return local->second;
  auto assigned = assignedStructNames.find(definition);
  if (assigned != assignedStructNames.end())
    return assigned->second;
  std::string name = recordRustName(definition);
  if (!name.empty())
    return name;
  auto anon = anonRecordNames.find(definition);
  if (anon == anonRecordNames.end())
    return {};
  return anon->second;
}

LogicalResult CImporter::importEnum(const clang::EnumDecl *enumDecl,
                                    Location loc) {
  const clang::EnumDecl *definition = enumDecl->getDefinition();
  if (!definition)
    return success(); // Incomplete; imported once completed or used.
  if (definition->getName().empty())
    return success(); // Anonymous; enumerators import as i32 at use sites.
  if (!importedEnums.insert(definition).second)
    return success();
  Location defLoc = translateLoc(definition->getBeginLoc());
  // A scoped enumeration (`enum class`/`enum struct`) has a distinct value
  // type with no implicit integer conversions; the open-enum model (a tuple
  // struct freely convertible to and from its integer) does not represent it.
  // Rejected here, located at the definition, rather than surfacing later as
  // the misleading "assigned value type does not match the place" mismatch at
  // the first use site.
  if (definition->isScoped())
    return emitError(defLoc)
           << "unsupported: scoped enumeration (enum class/struct)";
  if (isRustKeyword(definition->getName()))
    return emitError(defLoc) << "unsupported: enum name '"
                             << definition->getName()
                             << "' is a Rust keyword";

  SmallVector<std::string> variantNames;
  SmallVector<int64_t> variantValues;
  // Enumerator NAMES must stay unique (they become the tuple-struct's
  // associated const names), which C already guarantees within one enum; the
  // dialect verifier enforces it. VALUES need not be distinct: two C
  // enumerators sharing a value (`enum { A = 1, B = 1 }`) lower to two
  // associated consts of equal value (`const A: E = E(1); const B: E =
  // E(1);`), which is valid Rust and preserves `A == B`.
  for (const clang::EnumConstantDecl *enumerator : definition->enumerators()) {
    Location enumeratorLoc = translateLoc(enumerator->getLocation());
    llvm::StringRef name = enumerator->getName();
    if (isRustKeyword(name))
      return emitError(enumeratorLoc)
             << "unsupported: enumerator '" << name << "' is a Rust keyword";
    const llvm::APSInt &initValue = enumerator->getInitVal();
    if (!initValue.isRepresentableByInt64())
      return emitError(enumeratorLoc)
             << "unsupported: enumerator value does not fit in i32";
    int64_t value = initValue.getExtValue();
    if (value < INT32_MIN || value > INT32_MAX)
      return emitError(enumeratorLoc)
             << "unsupported: enumerator value does not fit in i32";
    variantNames.push_back(enumVariantRustName(name));
    variantValues.push_back(value);
  }
  if (variantNames.empty())
    return emitError(defLoc) << "unsupported: enum with no enumerators";

  // The storage of the emitted open enum follows clang's underlying type
  // choice (unsigned when every enumerator is non-negative), so that the
  // raw-representation place (`emitrust.enum_raw`) and enum/integer
  // conversions meet C's unsigned semantics at the right type.
  bool unsignedUnderlying =
      definition->getIntegerType()->isUnsignedIntegerType();

  // Cross-TU deduplication by symbol name (see importRecord): identical shape
  // is skipped, a name reused with a different variant shape is a diagnostic.
  // The underlying signedness is derived from the values, so the value list
  // determines it and the shape key needs no extra component.
  std::string shape;
  {
    llvm::raw_string_ostream os(shape);
    for (auto [variantName, variantValue] :
         llvm::zip(variantNames, variantValues))
      os << variantName << '=' << variantValue << ';';
  }
  auto existingShape = importedEnumShapes.find(definition->getName());
  if (existingShape != importedEnumShapes.end()) {
    if (existingShape->second != shape)
      return emitError(defLoc)
             << "unsupported: conflicting definition of enum '"
             << definition->getName()
             << "' with a different shape in another translation unit";
    return success();
  }
  importedEnumShapes[definition->getName()] = shape;

  SmallVector<llvm::StringRef> variantNameRefs(variantNames.begin(),
                                               variantNames.end());
  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::EnumDefOp>(
      defLoc,
      moduleBuilder.getStringAttr(enumTypeRustName(definition->getName())),
      moduleBuilder.getStrArrayAttr(variantNameRefs),
      moduleBuilder.getDenseI64ArrayAttr(variantValues), unsignedUnderlying);
  return success();
}

