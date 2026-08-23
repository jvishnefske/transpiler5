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
  ordinaryTuNameOwners.clear();
  ordinaryTuQualifiedOwners.clear();
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
    // W2.15: mirror importTopLevelDecl's function-template arm. Each
    // INSTANTIATION becomes an emitted module symbol, so the pre-scan must
    // claim its composed (suffixed) spelling too — otherwise
    // `structSymbolName`'s ordinary-name collision check and the FR-73
    // underscore-fold guard are blind to every template symbol in the TU
    // and a struct tag could be assigned a name an instantiation owns.
    // The uninstantiated pattern is skipped here for exactly the reason it
    // is skipped there.
    if (const auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      for (const clang::FunctionDecl *spec : tmpl->specializations()) {
        if (!spec->isThisDeclarationADefinition())
          continue;
        std::string specName = mlirFuncName(spec);
        ordinaryTuNames.insert(specName);
        ordinaryRawTuNames.insert(spec->getName());
        ordinaryTuNameOwners.try_emplace(specName, spec->getName().str());
        ordinaryTuQualifiedOwners.try_emplace(specName,
                                              spec->getQualifiedNameAsString());
        if (spec->hasBody())
          collectStaticLocalNames(spec->getBody(), specName);
      }
      continue;
    }
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      std::string funcName = mlirFuncName(func);
      ordinaryTuNames.insert(funcName);
      ordinaryRawTuNames.insert(func->getName());
      // FR-73: remember which raw spelling claimed the composed name
      // first (try_emplace keeps the first claimant; redeclarations of
      // the same raw name agree), so the underscore-fold guard in
      // importFunction can reject a DIFFERENT spelling folding onto it.
      ordinaryTuNameOwners.try_emplace(funcName, func->getName().str());
      // FR-125: same first-claimant record, qualified spelling, for the
      // case-fold collision guard in importFunction.
      ordinaryTuQualifiedOwners.try_emplace(funcName,
                                            func->getQualifiedNameAsString());
      // Function-local statics surface at module level under their
      // `<function>_<name>` mangle (see emitLocalVar), claiming that
      // spelling in the ordinary namespace.
      if (func->hasBody())
        collectStaticLocalNames(func->getBody(), funcName);
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      std::string varName = globalVarSymbolName(var);
      ordinaryTuNames.insert(varName);
      ordinaryRawTuNames.insert(var->getName());
      // FR-73: same first-claimant record as the function branch, for the
      // underscore-fold guard in importGlobalVar.
      ordinaryTuNameOwners.try_emplace(varName, var->getName().str());
      ordinaryTuQualifiedOwners.try_emplace(varName,
                                            var->getQualifiedNameAsString());
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
  // W2.9 hardening: a DEPENDENT record — most concretely a class template
  // partial specialization, which IS-A RecordDecl and so reaches this
  // path straight from the TU walk (`template <size_t I> struct
  // std::tuple_element<I, Swapped> {...};`) — has no concrete layout to
  // import; letting one through used to crash (SIGILL via
  // llvm_unreachable) instead of rejecting. Located rejection, per the
  // rejection-is-a-feature rule.
  if (record->isDependentType())
    return emitError(loc) << "unsupported: dependent class template";
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
    std::string message =
        (llvm::Twine("unsupported: struct '") +
         (rejectedName.empty() ? llvm::StringRef("<anonymous>")
                               : llvm::StringRef(rejectedName)) +
         "' was rejected, so a type naming it cannot be imported")
            .str();
    // FR-126: remember which TYPE this restatement names, keyed by
    // the verbatim message, so the ledger record sites can thread it.
    std::string sourceSym = graphItemSymbol(definition);
    if (!sourceSym.empty())
      cascadeSourceByMessage[message] = sourceSym;
    return emitError(loc) << message;
  }
  // FR-115: capture the record's OWN rejection at the moment it is
  // memoized, so the cause reaches the ledger even when this import runs
  // on demand inside a function body (where the per-use "struct 'X' was
  // rejected" restatement is all that used to survive). Same capture shape
  // as the FR-112 method path below. The captured errors are re-emitted on
  // the way out, so outer handlers (a function's recovery capture, strict
  // mode's printer) observe exactly what they did before.
  struct Fr115Captured {
    Location loc;
    DiagnosticSeverity severity;
    std::string message;
  };
  SmallVector<Fr115Captured> captured;
  auto capture = [&captured](Diagnostic &diag) -> LogicalResult {
    if (diag.getSeverity() != DiagnosticSeverity::Error)
      return failure();
    captured.push_back({diag.getLocation(), diag.getSeverity(), diag.str()});
    for (Diagnostic &note : diag.getNotes())
      captured.push_back(
          {note.getLocation(), DiagnosticSeverity::Note, note.str()});
    return success();
  };
  LogicalResult imported = failure();
  {
    ScopedDiagnosticHandler handler(builder.getContext(), capture);
    imported = importRecordUncached(definition);
  }
  if (failed(imported)) {
    rejectedRecords.insert(definition);
    // Ledger ONLY records the graph models (file-scope, named): those are
    // the nodes that would otherwise read `missing` with no diagnostic.
    // A nested or block-scope record is not a node; its rejection already
    // reaches the report through whoever imported it (the enclosing record
    // or function captures this error), so a ledger entry here would only
    // inflate the blocker tally with an off-graph restatement.
    // `recoveryStubOnly` guard: a stub retry runs with ALL diagnostics
    // silenced (it is a best-effort restatement of an already-reported
    // item); a ledger entry from inside it would leak a rejection the
    // recovery contract says is discarded (measured: search-red.c grew a
    // spurious `learn ... rejected=Atom tag=other` line without this).
    std::string sym = graphItemSymbol(definition);
    if (rejectionLedger && !sym.empty() && !recoveryStubOnly) {
      Location diagLoc = captured.empty()
                             ? translateLoc(definition->getLocation())
                             : captured.front().loc;
      std::string reason = captured.empty()
                               ? std::string("unsupported declaration")
                               : captured.front().message;
      rejectionLedger->record(emitrust::RejectedItem{
          sym, diagLoc, reason, emitrust::classifyBlocker(reason, diagLoc),
          /*stubbed=*/false, /*ownerSymbol=*/"",
          cascadeSourceForReason(reason)});
      ledgerRecordedRecords.insert(definition);
    }
  }
  // Re-emit what was captured, preserving the error/attached-note structure
  // so outer observers (strict mode's printer, a function's recovery
  // capture) see byte-identical diagnostics.
  {
    std::optional<InFlightDiagnostic> active;
    for (const Fr115Captured &diag : captured) {
      if (diag.severity == DiagnosticSeverity::Error) {
        if (active)
          active->report();
        active.emplace(emitError(diag.loc) << diag.message);
      } else if (active) {
        active->attachNote(diag.loc) << diag.message;
      }
    }
    if (active)
      active->report();
  }
  return imported;
}

/// W2.16: the located rejection a class-template specialization outside
/// the admitted subset earns, or success when it is a plain instantiation
/// of a primary template at type arguments only.
///
/// Driven off the specialization's OWN properties — its specialization
/// kind, its specialized-from link, and its `TemplateArgument` kinds —
/// never off anything in the class body, which is measurably
/// insufficient: `template <int N> struct Fixed { int v; ... };` mentions
/// `N` nowhere, so a field-driven check sees an ordinary struct and admits
/// it under a name whose suffix codes the non-type argument as the `x`
/// placeholder, silently fusing `Fixed<3>` and `Fixed<40>` into one struct
/// with one set of methods. Both shapes are regression-pinned in
/// test/Import/Cpp/class-templates-invalid.cpp.
///
/// It is called from `importRecordUncached` rather than from the
/// `ClassTemplateDecl` walk because a specialization is reached by three
/// routes (the walk, the top-level `RecordDecl` visit, and `mapType` on
/// demand) and only the record import is common to all three — the
/// on-demand route is still live after FR-42 recovery drops the other two,
/// and it was measured emitting struct_defs for rejected shapes.
static LogicalResult checkClassTemplateSpecialization(
    const clang::ClassTemplateSpecializationDecl *spec, Location loc) {
  // A partial-specialization PATTERN (`template <typename T> struct
  // W<T*>`) IS-A `ClassTemplateSpecializationDecl`; the top-level walk
  // skips it structurally (see `importTopLevelDecl`), so this guard is
  // defensive: if any OTHER route ever hands the dependent pattern in,
  // it must stay a located rejection, never fall through to a body
  // import. Checked by NODE KIND, not specialization kind — a partial
  // reports `TSK_ExplicitSpecialization`, and explicit (full) specs are
  // admitted below.
  if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec))
    return emitError(loc)
           << "unsupported: partial class template specialization";
  // W2.28: an instantiation whose pattern is a PARTIAL specialization is
  // fully concrete — clang substituted the partial's body when it
  // instantiated — and its template args are the PRIMARY template's
  // concrete argument list, so the ordinary record path imports it under
  // a unique suffixed name with no extra dispatch (byte-diffed in
  // test/EndToEnd/cpp-template-partial-spec.cpp). Only the dependent
  // PATTERN above is out of subset.
  // W2.28: an EXPLICIT (full) specialization is no longer rejected here —
  // it is a concrete record whose fields and methods are the hand-written
  // ones, and it imports through the ordinary record path under the same
  // suffixed name its implicit instantiation would have used (clang never
  // creates the instantiation it displaces).
  for (const clang::TemplateArgument &arg : spec->getTemplateArgs().asArray()) {
    if (arg.getKind() == clang::TemplateArgument::Pack)
      return emitError(loc) << "unsupported: variadic class template "
                               "(template parameter pack)";
    // W2.28: an INTEGRAL non-type argument codes by value in the record
    // suffix (`templateArgIntegralCode`), so `Fixed<3>` and `Fixed<40>`
    // stay two structs. Every other non-type kind still codes `x` and
    // stays rejected.
    if (arg.getKind() != clang::TemplateArgument::Type &&
        arg.getKind() != clang::TemplateArgument::Integral)
      return emitError(loc) << "unsupported: non-type template argument in "
                               "class template instantiation";
  }
  return success();
}

LogicalResult
CImporter::importRecordUncached(const clang::RecordDecl *definition) {
  // Every diagnostic below is located on the DEFINITION: it is a verdict on
  // this record, not on whoever asked for it. The use-site location matters
  // only for the repeat rejection in `importRecord` above.
  Location defLoc = translateLoc(definition->getBeginLoc());
  // W2.16: the class-template frontier, checked before any name is
  // composed or any struct_def is emitted. A std-namespace specialization
  // is exempt: `mapStdLibraryType` is the authoritative route for those
  // (it pre-seeds their emitted names and models them by hand), and
  // `std::array<T, N>`-shaped non-type arguments are its business, not
  // this wave's.
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(definition))
    if (!spec->isInStdNamespace() &&
        failed(checkClassTemplateSpecialization(
            spec, translateLoc(spec->getLocation()))))
      return failure();
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

  // FR-102: the EMITTED name must be decided BEFORE the field walk. A
  // self-referential fn-ptr component (`struct node { int (*visit)(struct
  // node *, int); }`) resolves its own type through `emittedRecordName`
  // DURING the walk; if the block-scope mangling and the tag-versus-
  // ordinary collision rename below had not run yet, the component would
  // name the raw tag while the struct_def is emitted as `c_main_loc` /
  // `Struct_ops` — a dangling type reference with no located diagnostic
  // (rustc E0412/E0573). Deciding here keeps the field type and the
  // struct_def symbol one decision, which is the CSymbolNaming.h
  // byte-identity invariant.
  //
  // The commitment must be UNDONE when the field walk fails: a record
  // whose name is claimed but whose import is rejected changes cascade
  // attribution downstream (the C++ method/statement cascade tags), so
  // every failure path below erases both maps. The "struct definition
  // outside file or function scope" rejection deliberately stays where it
  // was, AFTER the walk: a missing entry here is exactly that case.
  std::string assignedStorage;
  const bool localScopeRecord =
      !structName.empty() &&
      !definition->getDeclContext()->getRedeclContext()->isFileContext();
  if (localScopeRecord) {
    const clang::FunctionDecl *enclosing = nullptr;
    for (const clang::DeclContext *ctx = definition->getDeclContext();
         ctx && !enclosing; ctx = ctx->getParent())
      enclosing = llvm::dyn_cast<clang::FunctionDecl>(ctx);
    if (enclosing) {
      // A block-scope record's `<fn>_<tag>` disambiguator is a type name,
      // so it takes the type spelling (UpperCamelCase under the idiomatic
      // rename). Each probe below tries a fresh suffix, so the loop takes
      // at most one step per already-emitted struct name — bounded and
      // deterministic.
      std::string mangledBase = typeRustName(
          (llvm::Twine(mlirFuncName(enclosing)) + "_" + structName).str());
      std::string mangled = mangledBase;
      for (unsigned suffix = 2; emittedStructNames.contains(mangled); ++suffix)
        mangled = idiomaticRenameEnabled()
                      ? (llvm::Twine(mangledBase) + llvm::Twine(suffix)).str()
                      : (llvm::Twine(mangledBase) + "_" + llvm::Twine(suffix))
                            .str();
      emittedStructNames.insert(mangled);
      localRecordNames.try_emplace(definition, std::move(mangled));
    }
  } else if (!structName.empty()) {
    FailureOr<std::string> assigned = structSymbolName(definition, defLoc);
    if (failed(assigned))
      return failure();
    assignedStorage = std::move(*assigned);
  }

  SmallVector<llvm::StringRef> fieldNames;
  SmallVector<Type> fieldTypes;
  // A union flattens to its single storage slot; every other record keeps
  // the anonymous-member-resolving field walk.
  if (definition->isUnion()) {
    // W2.17: the union import path does NOT run the C++ member gate
    // (`collectRecordFields`), so the destructor admission has to be
    // repeated here. A union's members are not independently alive, so
    // "destroy the members in reverse declaration order" has nothing to
    // reproduce -- this is the residual shape the GENERIC `user-declared
    // destructor` wording (and the `cxx-destructor` tag) is retained for.
    if (const auto *cxxUnion =
            llvm::dyn_cast<clang::CXXRecordDecl>(definition))
      for (const clang::CXXMethodDecl *method : cxxUnion->methods()) {
        if (method->isImplicit() || method->isDeleted())
          continue;
        if (llvm::isa<clang::CXXDestructorDecl>(method)) {
          assignedStructNames.erase(definition);
          localRecordNames.erase(definition);
          return emitError(translateLoc(method->getLocation()))
                 << "unsupported: user-declared destructor";
        }
        // FR-112 RETIRED the union-path OVERLOADED OPERATOR gate FR-117 had
        // added here. That gate existed only to keep this path in step with
        // the struct path's W2.2 member-shape gate (the union path never
        // runs `collectRecordFields`); both are gone together, and an
        // operator is now OMITTED by `importCXXMethods`' `isImportable`
        // (non-identifier `DeclarationName`) with every use a located
        // rejection at the call -- see cpp-contained-member.cpp. FR-41's
        // coloring screen was removed in the same change, so the importer
        // and the screen still agree, by ADMISSION now
        // (test/Project/coloring-cpp-class-gates.cpp). A union cannot have
        // a virtual member, so the struct path's kept virtual gate has
        // nothing to mirror here.
      }
    if (failed(collectUnionSlot(definition, fieldNames, fieldTypes))) {
      assignedStructNames.erase(definition);
      localRecordNames.erase(definition);
      return failure();
    }
  } else if (unsigned bitFieldRuns = 0; failed(collectRecordFields(
                 definition, fieldNames, fieldTypes, bitFieldRuns))) {
    assignedStructNames.erase(definition);
    localRecordNames.erase(definition);
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
  if (localScopeRecord) {
    // The name was decided above; a MISSING entry means no enclosing
    // function was found, which is the outside-file-or-function-scope
    // rejection (kept at its original position so the diagnostic order
    // relative to the field walk does not move).
    auto known = localRecordNames.find(definition);
    if (known == localRecordNames.end())
      return emitError(defLoc)
             << "unsupported: struct definition outside file or function "
                "scope";
    llvm::StringRef mangledRef = known->second;
    structDefRecords[mangledRef] = definition;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    auto structDef = moduleBuilder.create<emitrust::StructDefOp>(
        defLoc, moduleBuilder.getStringAttr(mangledRef),
        moduleBuilder.getStrArrayAttr(fieldNames),
        moduleBuilder.getTypeArrayAttr(fieldTypes));
    // FR-78: the opaque-union marker anchors the emitter's backstop (any
    // leaked arm access is refused at translate, never rustc E0609).
    if (opaqueUnions.contains(definition))
      structDef->setAttr(emitrust::kOpaqueUnionAttrName,
                         moduleBuilder.getUnitAttr());
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
    // FR-78: every opaque union shares one blob field shape
    // (`opaque:[u8;N]`), but the C arm types are the real identity — fold
    // them into the shape key so two structurally different anonymous
    // opaque unions never merge under the shape-keyed `Anon<n>` naming,
    // while the same union reached through a shared header in several TUs
    // still dedups to one struct_def.
    if (opaqueUnions.contains(definition))
      for (const clang::FieldDecl *arm : definition->fields())
        os << arm->getName() << '@'
           << arm->getType().getCanonicalType().getAsString() << ';';
    // FR-122: the field shape alone cannot see MEMBER semantics, so two
    // same-named records with identical fields but different member
    // surfaces merged silently -- and the merged struct_def carried
    // whichever TU's members (dtor presence, dtor body, method bodies)
    // were imported first. Both merge orders were byte-diffed as silent
    // miscompiles on the defined-behavior idiomatic-rename channel
    // (`struct c` POD vs droppy `struct C`): droppy-first gave the POD a
    // phantom destructor, POD-first deleted the legitimate dtor line.
    // Fold the ODR hash of the definition into the key so a divergent
    // member surface becomes the loud cross-TU conflict below -- a drop
    // BIT was measured and rejected as designed, because it cannot see
    // the method-body and dtor-body theft channels.
    //
    // Two gates keep the legitimate mergers on the shape-only key:
    // (a) template specializations are EXEMPT -- W2.16's samePattern
    //     aliasing merges `Box<char>`/`Box<signed char>`, whose two
    //     specializations hash DIFFERENTLY while emitting literally the
    //     same code (residue, recorded not fixed: divergent member
    //     surfaces of the SAME specialization keep merging silently);
    // (b) only records with a member surface -- a user-declared dtor
    //     (mirroring `userDeclaredDestructor`; W2.26's TRANSITIVE
    //     has_drop can additionally mark a merely-inheriting class whose
    //     key carries no dtor bit, but that opens no theft channel: the
    //     inherited droppiness is derived entirely from the base FIELD's
    //     type, which is itself part of the shape key, so two records
    //     merging on the key always agree on has_drop) or any
    //     non-implicit method -- carry the suffix, so pure field-only
    //     PODs (FR-108(c) benign twins, CTS-R1 Anon merges) and all of C
    //     keep today's shape-only merge untouched by construction.
    if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(definition))
      if (!llvm::isa<clang::ClassTemplateSpecializationDecl>(cxx) &&
          cxx->hasDefinition()) {
        bool hasMemberSurface = cxx->hasUserDeclaredDestructor();
        for (const clang::CXXMethodDecl *method : cxx->methods())
          if (!method->isImplicit()) {
            hasMemberSurface = true;
            break;
          }
        if (hasMemberSurface)
          os << "odr:" << cxx->getODRHash() << ';';
      }
  }

  // File-scope named records go through the tag-versus-ordinary-namespace
  // collision renaming (C99 6.2.3): `struct a` and a global or function
  // `a` may coexist in C, so the tag is renamed to `Struct_<tag>` exactly
  // when the ordinary namespace claims the spelling (see
  // `structSymbolName`, which caches the decision per defining decl for
  // `emittedRecordName`). The renamed spelling is what the shape dedup and
  // the emitted struct_def below use.
  if (!assignedStorage.empty())
    structName = assignedStorage;

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
    // W2.16: the dedup below merges by emitted NAME, and that merge is a
    // SILENT MISCOMPILE when the name is claimed by a DIFFERENT type in
    // the SAME translation unit — every method mangles as
    // `<StructName>_<method>`, so the second type's call sites resolve to
    // the first type's bodies (measured: `Box<int>` plus a hand-written
    // `struct Box_i32` print `1 101` natively and `1 1` from the emitted
    // crate). Template instantiations widen that channel, because a
    // composed spelling can now coincide with a hand-written tag or with
    // another namespace's same-named template, so a clash involving one is
    // a located rejection here.
    //
    // The discriminator is DECL IDENTITY WITHIN ONE TU, and it has to be,
    // because two legitimate mergers travel this same path: the SAME
    // header template instantiated in several TUs (different tuTags, one
    // struct_def — the FR-58 shard merge depends on it), and two
    // instantiations of the SAME pattern whose arguments alias onto one
    // type code (`Box<char>`/`Box<signed char>` both code `i8`), which
    // emit literally the same code and so may still merge.
    const clang::RecordDecl *owner = structDefRecords.lookup(structName);
    auto ownerTu = structNameOwnerTuTags.find(structName);
    if (owner && owner != definition &&
        ownerTu != structNameOwnerTuTags.end() &&
        ownerTu->second == currentTuTag && !owner->isInStdNamespace() &&
        !definition->isInStdNamespace()) {
      const auto *thisSpec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(definition);
      const auto *ownerSpec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(owner);
      const bool samePattern =
          thisSpec && ownerSpec &&
          thisSpec->getSpecializedTemplate()->getCanonicalDecl() ==
              ownerSpec->getSpecializedTemplate()->getCanonicalDecl();
      if ((thisSpec || ownerSpec) && !samePattern)
        return emitError(defLoc)
               << "unsupported: class template instantiation collides with "
                  "the existing struct '"
               << structName << "'";
      // FR-108: the NON-TEMPLATE half of the very same channel, and the
      // one that needed no C++ at all to fire. Two DIFFERENT file-scope
      // records in ONE TU that compute the same emitted name merged
      // silently here — the second returned success below, BEFORE
      // `importCXXMethods` ever ran, and since every method mangles
      // `<StructName>_<method>` the second type's call sites resolved to
      // the FIRST type's bodies (measured: `struct box_i32` beside
      // `struct BoxI32` prints `101 1` natively and `101 101` from the
      // emitted crate; `typedef struct { int v; } Box;` beside `struct
      // Box { int v; };` quietly becomes one Rust type in plain C).
      //
      // REJECT rather than rename: `ItemGraphBuilder::recordSymbolFor`
      // and FR-41's coloring probe recompute record symbols from the AST
      // ALONE, with no importer state, so an order-dependent
      // disambiguator would be unreproducible there and across the
      // cross-TU merge — it would break the CSymbolNaming.h byte-identity
      // invariant. The AST-pure injective spelling is the raw C one,
      // which `--preserve-c-names` already emits; the guard keys off the
      // EMITTED name, so that mode keeps transpiling these unharmed.
      //
      // Gated on a USER-WRITTEN name: the shape-keyed `Anon<n>` path
      // above merges two same-shape anonymous records ON PURPOSE
      // (CTS-R1) and must not be caught.
      if (!thisSpec && !ownerSpec && !recordRustName(definition).empty())
        return emitError(defLoc)
               << "unsupported: struct '" << structName
               << "' collides with the emitted name of a different struct "
                  "in this translation unit";
    }
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
  // W2.16: which TU claimed the name, so the same-TU collision check above
  // can tell a genuine clash from the cross-TU header merge it must not
  // break. Recorded for EVERY file-scope record, not just template ones:
  // either side of a clash may be the hand-written one.
  structNameOwnerTuTags[structName] = currentTuTag;

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  auto structDef = moduleBuilder.create<emitrust::StructDefOp>(
      defLoc, moduleBuilder.getStringAttr(structName),
      moduleBuilder.getStrArrayAttr(fieldNames),
      moduleBuilder.getTypeArrayAttr(fieldTypes));
  // FR-78: the opaque-union marker anchors the emitter's backstop (any
  // leaked arm access is refused at translate, never rustc E0609).
  if (opaqueUnions.contains(definition))
    structDef->setAttr(emitrust::kOpaqueUnionAttrName,
                       moduleBuilder.getUnitAttr());
  // W2.17: the class declared a destructor, so the emitted struct gets an
  // `impl Drop` -- which costs it `Copy` (rustc E0184) and makes every
  // binding of it unconditionally live (an uninitialized Rust binding is
  // never dropped). Both consequences are decided from the struct alone, so
  // they ride the struct_def rather than the impl. W2.26 made the predicate
  // TRANSITIVE: a merely-inheriting derived class gets ONLY this attr -- no
  // impl Drop, no dtor func -- and Rust's field-drop glue runs the base's
  // destructor through the `base` field exactly once; without the attr the
  // derive keeps `Copy` beside that field's Drop, which is rustc E0204
  // (measured), the loud backstop this synthesis exists to avoid.
  if (userOrInheritedDestructor(astContext(),
                                astContext().getRecordType(definition)))
    structDef->setAttr(emitrust::kHasDropAttrName, moduleBuilder.getUnitAttr());
  // W2.23: the class has an admitted user copy constructor, so the emitted
  // struct loses `Copy` (same mechanism as has_drop above, different
  // trigger; a copy+dtor class carries both). Load-bearing exactly for the
  // copy-ctor-WITHOUT-destructor class -- the only kind admitted by value
  // -- where a bitwise `Copy` at a by-value pass would silently substitute
  // for the user's constructor: 0 copies where C++ mandates 1 (measured,
  // the spike's compile-clean miscompile).
  if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(definition))
    if (admittedCopyConstructor(cxxRecord))
      structDef->setAttr(emitrust::kHasCopyCtorAttrName,
                         moduleBuilder.getUnitAttr());
  // W2.2: a genuine C++ class's non-static-data-member methods (mutating,
  // const, static, and non-delegating constructors) import onto the
  // `emitrust.impl`/`emitrust.method_of` surface right after the struct
  // itself, so every method call site reached later in the same
  // (single-pass, declaration-order) import sees an already-imported
  // target — mirroring this file's struct-import-order pin
  // (cpp-basics.cpp). Destructor/virtual/operator-overload rejections
  // already ran above, in `collectRecordFields`, before any field of this
  // struct was collected.
  // W2.8: a std-namespace record's methods are never imported (call sites
  // intercept them; std::pair's value construction is field-wise), so the
  // method walk — which would reject e.g. pair's defaulted copy ctor —
  // must not run on one.
  if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(definition))
    if (!cxxRecord->isInStdNamespace())
      if (failed(importCXXMethods(cxxRecord))) {
        // FR-118: THE record is the failing item, so it must leave no trace.
        //
        // W2.2's comments claimed "a rejected class never half-imports", but
        // the code did not hold it: every gate `importCXXMethods` raises
        // fires AFTER the struct_def above is in the module and after the
        // name registries were claimed, and neither was undone. Measured on
        // unpatched HEAD that is a SILENT MISCOMPILE, not dead weight: a
        // plain POD in ANOTHER translation unit whose emitted name and field
        // shape coincide with the rejected class merges onto the leftover
        // struct_def and inherits its `emitrust.has_drop`, so the crate runs
        // a destructor the C++ program never runs
        // (test/EndToEnd/cpp-rejected-class-no-trace.cpp). Single-TU it also
        // STARVED an importable sibling that wanted the same emitted name.
        // It is the FR-42 contract stated at the top of ImportCRecovery.cpp
        // ("a rejected item must leave NO trace in the module"), which
        // `rollbackTo` deliberately does not cover for aggregates -- that
        // exemption is right for a type pulled in ON DEMAND by another item,
        // and wrong for exactly this case.
        //
        // Hoisting the gates ahead of the struct_def, which design.md's
        // FR-118 entry proposed, is NOT sufficient and cannot be made
        // sufficient: pass 2 of `importCXXMethods` imports an ARBITRARY
        // method body and can fail on any unsupported construct in the
        // language, reproducing the byte-identical divergence with no
        // class-level predicate to hoist (pinned as `body-fail` in
        // test/Import/Cpp/cpp-rejected-class-no-trace.cpp).
        //
        // `importedRecords` deliberately stays inserted: the caller
        // (`importRecord`) adds this record to `rejectedRecords`, and that is
        // what turns every later use into the located
        // `struct 'X' was rejected` cascade rather than a silent re-import.
        // Method funcs are erased BY NAME rather than by position so that a
        // type or global this class's bodies pulled in on demand -- which is
        // `rollbackTo`'s legitimately-exempt category -- is left alone.
        // FR-112 factored the erase into `eraseImportedMethodFuncs`, which
        // is also the per-iteration clean slate of the containment fixpoint
        // inside `importCXXMethods` itself.
        eraseImportedMethodFuncs(cxxRecord);
        eraseTopLevelOp(structDef.getOperation());
        importedRecordShapes.erase(structName);
        emittedStructNames.erase(structName);
        structDefRecords.erase(structName);
        structNameOwnerTuTags.erase(structName);
        assignedStructNames.erase(definition);
        localRecordNames.erase(definition);
        anonRecordNames.erase(definition);
        if (auto anonShape = anonRecordShapeNames.find(shape);
            anonShape != anonRecordShapeNames.end() &&
            anonShape->second == structName)
          anonRecordShapeNames.erase(anonShape);
        return failure();
      }
  return success();
}

/// FR-118/FR-112: erases every already-imported method func of `record` BY
/// NAME (a type or global the class's bodies pulled in on demand is
/// `rollbackTo`'s legitimately-exempt category and is left alone), then
/// clears the per-function scratch state, which points into the bodies just
/// erased. Shared between the FR-118 whole-class undo in
/// `importRecordUncached` and the per-iteration clean slate of FR-112's
/// containment fixpoint in `importCXXMethods`.
void CImporter::eraseImportedMethodFuncs(const clang::CXXRecordDecl *record) {
  for (const clang::CXXMethodDecl *method : record->methods()) {
    if (method->isImplicit() || method->isDeleted())
      continue;
    func::FuncOp emitted = functions.lookup(cxxMethodMangledName(method));
    if (!emitted)
      continue;
    functions.erase(emitted.getName());
    eraseTopLevelOp(emitted.getOperation());
  }
  // Points into the bodies just erased.
  resetPerFunctionState();
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
    // FR-117 (widened by FR-112): a member whose `DeclarationName` is NOT
    // an ordinary identifier -- a conversion function (`operator int()`)
    // or, since FR-112 removed both class-level operator gates, ANY
    // overloaded operator -- has no spelling to mangle and used to import
    // as `<Struct>_`, an EMPTY method name nobody can call (two of them in
    // one class collided outright). It is OMITTED rather than class-level
    // fatal, which is sound because every USE of one is already a located
    // rejection: implicit and `static_cast` conversion uses are
    // `unsupported cast (UserDefinedConversion)`, the explicit
    // `c.operator int()` spelling and an out-of-line definition are
    // `unsupported: conversion function`, and a spelled or implicit
    // operator call is `call to overloaded operator ... omitted from class
    // ...` at the call dispatch's non-identifier-callee guard. Constructors
    // and destructors keep their FIXED base names (`new`, `dtor`) and are
    // unaffected.
    if (!method->getDeclName().isIdentifier() &&
        !llvm::isa<clang::CXXConstructorDecl>(method) &&
        !llvm::isa<clang::CXXDestructorDecl>(method))
      return false;
    return !method->isImplicit() && !method->isDeleted() &&
           !method->isDefaulted();
  };
  // Destructors outside the W2.17 subset were already rejected in
  // `collectRecordFields`, before this class's struct_def (and so before
  // this walk) ever ran (virtual methods import like any other method
  // since W2.19a); a move or delegating constructor is out of the method
  // wave's scope (no xvalue/last-use model, and move counts are
  // elision-dependent) and is rejected here, the first point a constructor
  // is inspected individually. Kept ahead of BOTH passes so a rejected
  // class never half-imports. It CANNOT become an FR-112 omission: a
  // copy/move constructor is invoked implicitly at by-value pass, return
  // and init -- there is no call node to reject -- and omitting one
  // substitutes Rust's bitwise Copy for the user's constructor (measured:
  // a copy ctor that sets `v=99` prints `1 99` natively and `1 1` when
  // omitted).
  //
  // W2.23 NARROWED the copy half of this gate: the user-provided
  // `T(const T&)` copy constructor is ADMITTED -- it imports as an
  // ordinary `&mut self` method below and `emitrust.has_copy_ctor` strips
  // `Copy` from the emitted derive, so no bitwise substitution channel is
  // left open. Everything else keeps a located rejection: a `= default`ed
  // copy ctor has no body to import (and no user semantics to lose --
  // pinned in cpp-defaulted-ctor-invalid.cpp), and a copy ctor taking
  // non-const `T&` is outside the FR-48 shared-borrow image (the receiver
  // already holds the &mut). The FR-41 coloring screen
  // (lib/Project/ItemColoring.cpp) mirrors this predicate EXACTLY.
  for (const clang::CXXMethodDecl *method : record->methods()) {
    // Broader than `isImportable`: a copy/move/delegating constructor is
    // rejected even when `= default`, whereas `isImportable` (used by the two
    // import passes below) additionally skips defaulted members so a null
    // body is never emitted. Compiler-synthesized members carry none of these
    // shapes and are skipped.
    if (method->isImplicit() || method->isDeleted())
      continue;
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(method)) {
      if (ctor->isMoveConstructor() || ctor->isDelegatingConstructor())
        return emitError(translateLoc(ctor->getLocation()))
               << "unsupported: copy/move/delegating constructor";
      if (ctor->isCopyConstructor()) {
        // A bodiless declaration (`T(const T&);` with no definition in
        // this TU) keeps the historical class-level wording, exactly the
        // W2.17 "destructor with no definition" posture: nothing could be
        // imported for its call sites, and admitting the class would leave
        // every copy point a dangling lookup.
        if (!ctor->isUserProvided() || !ctor->isDefined() ||
            ctor->getNumParams() != 1)
          return emitError(translateLoc(ctor->getLocation()))
                 << "unsupported: copy/move/delegating constructor";
        const auto *reference =
            ctor->getParamDecl(0)->getType()->getAs<clang::LValueReferenceType>();
        if (!reference || !reference->getPointeeType().isConstQualified() ||
            reference->getPointeeType().isVolatileQualified())
          return emitError(translateLoc(ctor->getLocation()))
                 << "unsupported: copy constructor taking a non-const "
                    "reference";
      }
    }
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
  //
  // Pass 2 imports each body. A method DEFINED OUT OF LINE (declared in the
  // class, defined in a `.cpp`) has no body here, so it stays the stub pass
  // 1 built and is filled in when the translation-unit walk reaches its
  // out-of-line definition — exactly the behavior this walk had before.
  //
  // FR-112 CONTAINMENT: a failure in either pass no longer rejects the
  // class when the failing member is an ORDINARY method (instance or
  // static). The member joins the omitted set — with its own verbatim
  // per-construct diagnostic re-reported as a warning, and a ledger entry
  // so the rejection stays rankable — and every USE of it is a located
  // rejection at the call site by pre-existing machinery ("call to
  // unimported method 'X'"). A CONSTRUCTOR or DESTRUCTOR failure stays
  // class-level: both are invoked implicitly (value init, scope exit), so
  // there is no call node a use-site rejection could attach to, exactly
  // the copy/move-constructor argument above.
  //
  // THE FIXPOINT (design.md FR-112, constraint C2): on any omission, every
  // already-imported method func of the class is erased
  // (`eraseImportedMethodFuncs`, the FR-118 clean-slate tool minus the
  // struct_def/cache erasure) and BOTH passes re-run skipping the omitted
  // set, until it stops growing. With the caller declared BEFORE the
  // failing callee, pass 1 registers the callee's stub, the caller imports
  // a call against it, and the callee's body then fails — a single-pass
  // omission would leave that caller holding a dangling reference:
  // measured as `'func.call' op ... does not reference a valid function`
  // for an instance callee (a TU-killing verifier error attributable to no
  // item), and as a verifier-SILENT dangling `emitrust.call_opaque` string
  // for a STATIC callee (constraint C8: nothing downstream catches it
  // before rustc), which is why convergence keys off importFunction
  // failure alone, never off verifier feedback. The re-run rejects such a
  // caller AT ITS CALL and it joins the omitted set in turn; the set only
  // grows and is bounded by the method count, so termination is
  // structural. Pinned in test/Import/Cpp/cpp-contained-fixpoint.cpp.
  llvm::SmallPtrSet<const clang::CXXMethodDecl *, 8> omittedMethods;
  auto isContainable = [](const clang::CXXMethodDecl *method) {
    return !llvm::isa<clang::CXXConstructorDecl>(method) &&
           !llvm::isa<clang::CXXDestructorDecl>(method);
  };
  std::string className = recordRustName(record);
  if (className.empty())
    className = "<anonymous>";
  // The FR-49 owner join key, same derivation as the recovery ledger's
  // `declOwnerSymbol`: file-scope classes are graph nodes, everything else
  // reports no owner.
  std::string ownerSymbol =
      record->getDeclContext()->getRedeclContext()->isFileContext()
          ? recordRustName(record)
          : std::string();
  bool omissionGrew = true;
  while (omissionGrew) {
    omissionGrew = false;
    for (bool signatureOnly : {true, false}) {
      for (const clang::CXXMethodDecl *method : record->methods()) {
        if (!isImportable(method) || omittedMethods.contains(method))
          continue;
        if (!isContainable(method)) {
          // Constructor/destructor: errors flow to the caller unscoped, and
          // a failure rejects the class (the FR-118 undo runs there).
          if (failed(importFunction(method, signatureOnly)))
            return failure();
          continue;
        }
        // Same capture shape as `importTopLevelDeclRecovering`, and for the
        // same reason: the rejection is reported through the diagnostic
        // engine at the point of detection, deep inside the import, and a
        // scoped handler is the only way the verbatim text and location
        // reach the omission warning and the ledger — and it is what keeps
        // a CONTAINED rejection from printing as an error in strict mode.
        struct CapturedDiagnostic {
          Location loc;
          DiagnosticSeverity severity;
          std::string message;
        };
        SmallVector<CapturedDiagnostic> captured;
        auto capture = [&captured](Diagnostic &diag) -> LogicalResult {
          if (diag.getSeverity() != DiagnosticSeverity::Error)
            return failure();
          captured.push_back(
              {diag.getLocation(), diag.getSeverity(), diag.str()});
          for (Diagnostic &note : diag.getNotes())
            captured.push_back(
                {note.getLocation(), DiagnosticSeverity::Note, note.str()});
          return success();
        };
        LogicalResult result = failure();
        {
          ScopedDiagnosticHandler handler(builder.getContext(), capture);
          result = importFunction(method, signatureOnly);
        }
        if (succeeded(result)) {
          // Never observed (an import that emitted an error and still
          // reported success); swallowing it would be a silent change.
          for (const CapturedDiagnostic &diag : captured)
            emitError(diag.loc) << diag.message;
          continue;
        }
        omittedMethods.insert(method);
        Location loc = captured.empty()
                           ? translateLoc(method->getLocation())
                           : captured.front().loc;
        std::string reason = captured.empty()
                                 ? std::string("unsupported declaration")
                                 : captured.front().message;
        // `getName()` is safe here: a containable, importable method always
        // has an ordinary identifier `DeclarationName` (non-identifier
        // members were omitted by `isImportable` without ever importing).
        InFlightDiagnostic warning = emitWarning(loc)
                                     << reason << " (omitted: method '"
                                     << method->getName() << "' of class '"
                                     << className << "')";
        for (const CapturedDiagnostic &diag : llvm::drop_begin(captured))
          warning.attachNote(diag.loc) << diag.message;
        // One ledger entry per omitted member, carrying the member's OWN
        // per-construct diagnostic: this is what turns an opaque
        // `method of an unimported class` cascade into rankable data
        // (design.md FR-112's honest payoff).
        if (rejectionLedger)
          rejectionLedger->record(emitrust::RejectedItem{
              method->getNameAsString(), loc, reason,
              emitrust::classifyBlocker(reason, loc), /*stubbed=*/false,
              ownerSymbol});
        eraseImportedMethodFuncs(record);
        omissionGrew = true;
        break;
      }
      if (omissionGrew)
        break;
    }
  }
  return success();
}

/// Declared in CImporterInternal.h: shared with the local/global/parameter
/// and expression admission checks in the other ImportC translation units.
const clang::CXXDestructorDecl *
userDeclaredDestructor(clang::ASTContext &context, clang::QualType type) {
  // An array's ELEMENT type is what decides: `Tracer a[3]` is three
  // destructor-carrying objects, and the array wrapper adds nothing.
  while (const clang::ArrayType *arrayType =
             context.getAsArrayType(type.getCanonicalType()))
    type = arrayType->getElementType();
  const auto *record =
      llvm::dyn_cast_or_null<clang::CXXRecordDecl>(type->getAsRecordDecl());
  if (!record || !record->hasDefinition())
    return nullptr;
  // A COMPILER-SYNTHESIZED destructor is not one: it has no body to import
  // and no observable effect, so every plain C struct (and every C++ class
  // without RAII) must keep answering "no" here.
  if (!record->hasUserDeclaredDestructor())
    return nullptr;
  return record->getDestructor();
}

/// Declared in CImporterInternal.h: W2.26's transitive drop predicate. The
/// recursion walks exactly the SINGLE public non-virtual base chain --
/// the one chain shape `admitsSingleBaseAsField` below turns into a `base`
/// field -- because that field is what Rust's field-drop glue runs the
/// inherited destructor through. Any other base shape is already a located
/// rejection at the class, so answering "not droppy" for it never lets an
/// object exist to miss a gate. The whole chain is walked, not one level:
/// a droppy ROOT under a dtor-less middle class still makes every class
/// above it droppy (measured byte-identical to three levels).
const clang::CXXDestructorDecl *
userOrInheritedDestructor(clang::ASTContext &context, clang::QualType type) {
  if (const clang::CXXDestructorDecl *own =
          userDeclaredDestructor(context, type))
    return own;
  while (const clang::ArrayType *arrayType =
             context.getAsArrayType(type.getCanonicalType()))
    type = arrayType->getElementType();
  const auto *record =
      llvm::dyn_cast_or_null<clang::CXXRecordDecl>(type->getAsRecordDecl());
  if (!record || !record->hasDefinition() || record->getNumBases() != 1)
    return nullptr;
  const clang::CXXBaseSpecifier &base = *record->bases_begin();
  if (base.isVirtual() || base.getAccessSpecifier() != clang::AS_public)
    return nullptr;
  const clang::CXXDestructorDecl *inherited =
      userOrInheritedDestructor(context, base.getType());
  // An inherited destructor counts only if it is user-PROVIDED (a real
  // body). An explicitly `= default`ed one is user-DECLARED -- so the
  // own-class predicate above still answers for it, where the no-body
  // class gate keeps the loud W2.17 behavior -- but it is trivial and has
  // no effect to lose, and treating it as droppy would reject every
  // std::pair: gcc-15's pair publicly inherits `__pair_base`, whose
  // defaulted destructor is exactly this shape (measured: the transitive
  // predicate without this filter regressed corpus entries 00502/00601).
  if (inherited && !inherited->isUserProvided())
    return nullptr;
  return inherited;
}

/// Declared in CImporterInternal.h: W2.23's admission predicate for the
/// user copy constructor. Answering non-null here must coincide exactly
/// with `importCXXMethods` NOT rejecting the class for a constructor
/// shape: a move/delegating/defaulted/non-const-ref sibling anywhere on
/// the class makes the WHOLE class a located rejection there, so this
/// returns null for it and every consumer (the by-value-return signature
/// gate, the has_copy_ctor attr, the FR-41 coloring clone) agrees with
/// the gate. The std-namespace exemption mirrors the method walk, which
/// never runs on a std record at all (and gcc-15's std::pair DECLARES a
/// defaulted copy ctor -- marking it would strip `Copy` from every
/// emitted pair, a golden byte shift).
const clang::CXXConstructorDecl *
admittedCopyConstructor(const clang::CXXRecordDecl *record) {
  if (!record || !record->hasDefinition() || record->isInStdNamespace())
    return nullptr;
  record = record->getDefinition();
  const clang::CXXConstructorDecl *admitted = nullptr;
  for (const clang::CXXConstructorDecl *ctor : record->ctors()) {
    if (ctor->isImplicit() || ctor->isDeleted())
      continue;
    if (ctor->isMoveConstructor() || ctor->isDelegatingConstructor())
      return nullptr;
    if (!ctor->isCopyConstructor())
      continue;
    if (!ctor->isUserProvided() || !ctor->isDefined() ||
        ctor->getNumParams() != 1)
      return nullptr;
    const auto *reference =
        ctor->getParamDecl(0)->getType()->getAs<clang::LValueReferenceType>();
    if (!reference || !reference->getPointeeType().isConstQualified() ||
        reference->getPointeeType().isVolatileQualified())
      return nullptr;
    admitted = ctor;
  }
  return admitted;
}

/// Declared in CImporterInternal.h: shared with FR-41's admissibility probe
/// (lib/Project/ItemColoring.cpp), which must screen exactly the shapes this
/// answers false for -- screening one it admits would color a portable class
/// Red and drag every caller down with it.
bool admitsSingleBaseAsField(const clang::CXXRecordDecl *record) {
  // Exactly ONE base. Multiple inheritance has no single-field image at
  // all: two bases would need two fields, and the `base` name (and every
  // inherited access through it) stops being unambiguous.
  if (record->getNumBases() != 1)
    return false;
  const clang::CXXBaseSpecifier &base = *record->bases_begin();
  // A VIRTUAL base is shared between several derived paths, which a
  // by-value field cannot represent (each path would get its own copy).
  if (base.isVirtual())
    return false;
  // PRIVATE/PROTECTED inheritance is "implemented in terms of": the base's
  // members are not part of the derived interface, but a plain `base` field
  // makes them reachable from anywhere in the crate. Out of subset until
  // the emitted field's visibility is modelled.
  if (base.getAccessSpecifier() != clang::AS_public)
    return false;
  const clang::CXXRecordDecl *baseRecord = base.getType()->getAsCXXRecordDecl();
  if (!baseRecord || !baseRecord->hasDefinition())
    return false;
  // W2.16 monomorphizes a class template at its point of use; a
  // specialization reached only as a BASE has never been exercised through
  // that ordering, so it stays out rather than being guessed at.
  if (llvm::isa<clang::ClassTemplateSpecializationDecl>(baseRecord))
    return false;
  return true;
}

/// Declared in CImporterInternal.h.
const clang::Expr *
peelDerivedToBaseCasts(const clang::Expr *expr,
                       llvm::SmallVectorImpl<clang::QualType> &hops) {
  const clang::Expr *e = expr;
  for (;;) {
    e = e->IgnoreParens();
    const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
    if (!cast)
      return e;
    clang::CastKind kind = cast->getCastKind();
    if (kind == clang::CK_DerivedToBase ||
        kind == clang::CK_UncheckedDerivedToBase) {
      // ONE cast node, one path entry per base traversed, derived-most
      // first: `C -> A` through `B` is a single cast whose path is
      // `(B -> A)`. Appending the path in order therefore appends the hops
      // in projection order (`self.base.base`).
      for (const clang::CXXBaseSpecifier *hop : cast->path())
        hops.push_back(hop->getType());
      e = cast->getSubExpr();
      continue;
    }
    // A qualified inherited call (`Base::get()`) inserts a `CK_NoOp` above
    // the derived-to-base cast; it changes nothing about the place.
    if (kind == clang::CK_NoOp) {
      e = cast->getSubExpr();
      continue;
    }
    return e;
  }
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
    for (const clang::CXXBaseSpecifier &base : cxxRecord->bases()) {
      Location baseLoc = translateLoc(base.getBeginLoc());
      // The one exemption: an EMPTY base of a STD-NAMESPACE record.
      // libstdc++ 15 makes `std::pair` derive from `__pair_base`, an
      // ABI-control tag with no data members whatsoever — skipping it
      // drops nothing (the data-loss rationale above is about inherited
      // FIELDS), and the synthesized Pair struct's shape is unchanged
      // byte for byte. A base carrying any data, and every base of a
      // USER record (whose layout the C-struct model must not
      // misrepresent even when empty), keeps the rejection.
      const clang::CXXRecordDecl *baseRecord =
          base.getType()->getAsCXXRecordDecl();
      if (cxxRecord->isInStdNamespace() && baseRecord &&
          baseRecord->hasDefinition() && baseRecord->isEmpty())
        continue;
      // W2.18 NARROWS the W2.0 rejection above rather than removing it.
      // Rust has no inheritance, so the ONE image that loses no inherited
      // data is the base as an ordinary FIRST field named `base`, with
      // every inherited access flattened through it (`self.base.x`,
      // `self.base.base_get()`, and `self.base.base` for a two-level
      // chain). It is PREPENDED, before the `fields()` walk below, for two
      // reasons: the C++ object's own layout puts the base subobject
      // first, and `appendField`'s duplicate-final-name guard only catches
      // a derived member literally spelled `base` if the synthesized field
      // is already in the list. Everything outside the measured
      // byte-diff-clean subset keeps the W2.0 wording.
      if (!admitsSingleBaseAsField(cxxRecord)) {
        return emitError(baseLoc)
               << "unsupported: base classes are not supported";
      }
      // W2.26 ADMITS the destructor-carrying base (W2.17's E0204 channel
      // is closed by the transitive predicate: every use-site drop gate
      // now fires for a merely-inheriting `Derived d;`, and the emitter
      // suppresses `Copy` from the transitive has_drop above). The base
      // lands as the ordinary first field, whose Drop is exactly where
      // Rust's field-drop glue runs `~Base` -- after the derived drop
      // body, which is C++'s derived-body-then-base order (measured
      // byte-identical to three levels, cpp-inheritance-drop.cpp). The
      // residual drop-ORDER divergence -- a droppy MEMBER beside a droppy
      // base -- stays rejected at the member below.
      //
      // An EMPTY NON-DROPPY base carries no inherited DATA, so the
      // data-loss rationale does not apply and a synthesized field would
      // be pure invention: the `[u8; 1]` placeholder a field-less,
      // method-less record maps to is not the C++ object (which applies
      // the empty-base optimization and adds no storage at all). Skipped
      // exactly like the std-namespace tag base above; an inherited
      // METHOD reached through such a base has no `base` field to project
      // through and is a located rejection in `projectBaseHops`. An empty
      // DROPPY base is NOT skipped: skipping it loses `~Base` outright
      // (the measured ~Shape-loss trap), so it is MATERIALIZED -- a
      // destructor is a user-declared method, which already keeps the
      // record off the byte-region path, so `mapType` below yields a true
      // zero-field struct_def whose Drop impl the field carries.
      if (baseRecord->isEmpty() &&
          !userOrInheritedDestructor(astContext(), base.getType()))
        continue;
      // `mapType` on the base's record type is what imports the base class
      // (and lands its `struct_def` ahead of this one); a rejection inside
      // it — a virtual method on the base, say — surfaces with the base's
      // own located diagnostic rather than this one.
      FailureOr<Type> baseType = mapType(base.getType(), baseLoc);
      if (failed(baseType))
        return failure();
      fieldNames.push_back("base");
      fieldTypes.push_back(*baseType);
    }
    // W2.2 (narrowed by FR-112): a user-declared destructor outside the
    // W2.17-admitted subset (no drop semantics modeled for it) or a
    // virtual method (no vtable/dynamic dispatch, and vptr STORAGE the
    // emitted struct cannot carry) is rejected at the member's own
    // declaration — checked here, before any field (or method) of the
    // class imports, so a rejected class never half-imports (no
    // struct_def, no methods). Compiler-synthesized
    // special members the class did not itself declare are skipped: they
    // carry none of these three shapes and never surface a diagnostic.
    // W2.8: a std-namespace record (std::pair, the one that imports
    // through this path) is exempt from the member-shape gate: its
    // methods (operator=, converting constructors) are NEVER imported —
    // every std member call is intercepted at the call site — so their
    // shapes cannot half-import anything. The base-class check above
    // stays in force for any base CARRYING data: a std record with such
    // a base would still silently drop inherited data (only the empty
    // std-namespace tag base is skipped, per the loop above).
    if (!cxxRecord->isInStdNamespace()) {
      for (const clang::CXXMethodDecl *method : cxxRecord->methods()) {
        if (method->isImplicit() || method->isDeleted())
          continue;
        Location methodLoc = translateLoc(method->getLocation());
        // W2.17: a user-declared destructor is ADMITTED (it becomes
        // `impl Drop for T`) for exactly the shape whose drop points were
        // measured byte-identical against `clang++ -std=c++17`. The three
        // class-level disqualifiers below stay LOUD; the use-site ones
        // (members, arrays, globals, by-value, unmodeled scopes) are raised
        // where the object is declared, in `emitLocalVar`/`importGlobalVar`/
        // `importFunction`, since the class itself is fine there.
        if (llvm::isa<clang::CXXDestructorDecl>(method)) {
          // W2.26 admits a VIRTUAL destructor here: destruction of a value
          // is static, and every site where the dynamism could be observed
          // (new, upcast, virtual member call through a pointer) is an
          // explicit AST node that is already a located rejection. W2.26's
          // practical reach was the class whose SOLE virtual member is the
          // destructor; since W2.19a removed the class-level `virtual
          // method` gate below, virtual methods beside the destructor are
          // admitted on the same value-only terms. The vptr the native
          // layout carries is refused where it could be observed --
          // `emitSizeofAlignof` screens polymorphic operands.
          // A union's members are not independently alive, so "destroy the
          // members in reverse order" has no meaning to reproduce; this is
          // the residual shape the generic wording is retained for.
          if (cxxRecord->isUnion())
            return emitError(methodLoc)
                   << "unsupported: user-declared destructor";
          // An uncalled, undefined method is silently DROPPED from emission
          // (measured), and nothing ever calls a destructor -- so a body-less
          // one would emit no `impl Drop` at all and lose every side effect
          // without a diagnostic. An out-of-line definition ELSEWHERE IN THIS
          // TU is fine: `hasBody` finds it across redeclarations, and the
          // FR-47 signature prepass leaves a stub the definition fills in.
          if (!method->hasBody())
            return emitError(methodLoc)
                   << "unsupported: destructor with no definition in this "
                      "translation unit";
          // The destructor's module symbol is `<Struct>_dtor`; a member
          // function literally spelled `dtor` would land on the same symbol
          // (the overload-suffix counter deliberately skips destructors).
          for (const clang::CXXMethodDecl *other : cxxRecord->methods())
            if (!other->isImplicit() && !other->isDeleted() &&
                !llvm::isa<clang::CXXDestructorDecl>(other) &&
                other->getDeclName().isIdentifier() &&
                other->getName() == "dtor")
              return emitError(methodLoc)
                     << "unsupported: destructor collides with the member "
                        "function 'dtor'";
          continue;
        }
        // W2.19a: a virtual method no longer rejects the CLASS. On a VALUE
        // the C++ static type IS the dynamic type, so `getMethodDecl()`'s
        // statically named override is the exact method C++ dispatches to
        // (byte-diffed against the devirtualized twin in the W2.19 spike);
        // the virtual method imports as an ordinary method and value calls
        // bind it directly. W2.19b widened the admitted set once more: a
        // pointer whose region binds EXACTLY ONE local object carries a
        // statically known dynamic type, so the polymorphic upcast BINDS
        // (`peelPointerCast` admits polymorphic chains; the planner's
        // JOIN/MULTIOBJ walls keep multi-object regions out) and a
        // virtual call through such a pointer DEVIRTUALIZES to the bound
        // object's final overrider. Every channel where the dynamic type
        // is genuinely unknown stays a located rejection at the site:
        // pointer parameters, implicit `this`, ctor bodies, nullable and
        // multi-object regions, global objects, qualified calls, and the
        // W2.21 Box payload all hit the fence in `emitCXXMemberCall`
        // (the spike measured the silent miscompile that fence
        // prevents), base references and
        // new/delete keep their pre-existing rejections, and FR-112's
        // sizeof concern (clang folds the vptr-carrying native layout for
        // a struct the emitter renders without one) is screened at the
        // fold by W2.26's `emitSizeofAlignof` polymorphic-operand check.
        // A PURE virtual method needs no gate of its own: it has no body,
        // so it rides the FR-47 stub channel (silently dropped when
        // uncalled, FR-52-loud when referenced), and a pure-virtual VALUE
        // cannot exist -- clang rejects abstract instantiation upstream.
        // An overloaded operator is NOT rejected here since FR-112: it is
        // OMITTED by `importCXXMethods` (non-identifier `DeclarationName`,
        // same channel as FR-117's conversion functions), and every use --
        // spelled, implicit-enclosing-`operator=`, or explicit member-call
        // syntax -- is a located rejection at the call
        // (cpp-contained-member-invalid.cpp).
      }
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
    // W2.17: a member whose class carries a destructor is a SILENT
    // MISCOMPILE channel, not merely an unmodeled one -- C++ destroys the
    // members of an object in REVERSE declaration order after the enclosing
    // body, Rust drops the fields in FORWARD order (measured: the middle
    // `dtor` line swaps). Array-typed members decide on the element type.
    // W2.26: TRANSITIVE, like every drop gate -- a member of a
    // merely-inheriting droppy-derived class diverges identically, and a
    // droppy member BESIDE a droppy base is the wave's re-measured order
    // divergence (native `~D ~M ~B` vs image `~D ~B ~M`, the base being
    // the FIRST field), so this gate is precisely what keeps that shape
    // out while the base itself is admitted.
    if (userOrInheritedDestructor(astContext(), field->getType()))
      return emitError(fieldLoc)
             << "unsupported: struct member of a class with a destructor";
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
    // treatment. FR-94/95 AMEND the amendment: an ADMITTED tail (gap-free
    // u8 or Vec-mappable FAM of a typed record, `famTailField`) grows a
    // trailing OWNED `Vec<T>` field — the allocation-backed tail
    // representation; the translator drops `Copy` from the derive for
    // exactly this field shape, and Clang's sizeof/offsetof folds are
    // unaffected (the Vec is a Rust-side owner, not C layout).
    if (field->getType()->isIncompleteArrayType()) {
      if (famTailField(record) == field) {
        if (failed(appendField(internName(mangleMemberName(field->getName())),
                               famTailVecType(field), fieldLoc,
                               field->getName())))
          return failure();
      }
      continue;
    }
    if (const clang::ConstantArrayType *zeroLength =
            astContext().getAsConstantArrayType(field->getType());
        zeroLength && zeroLength->getSize().isZero())
      continue;
    // FR-96: a pointer to an ADMITTED FAM record inside a container that
    // is ITSELF an admitted FAM record is an OWNED NULLABLE member —
    // `Option<pointee>` (`None` is C's null; the payload's Vec tail owns
    // the allocation). Gated on the program-wide poison set: any
    // unrecognized use keeps the historical i64 slot and the member-wall
    // rejections, and a NON-FAM container keeps its i64 slot and Copy
    // semantics (the whole-record-assignment rejection only covers FAM
    // containers).
    if (famOptionMemberPointee(field)) {
      FailureOr<emitrust::OpaqueType> optionType =
          famOptionMemberType(field, fieldLoc);
      if (failed(optionType))
        return failure();
      if (failed(appendField(internName(mangleMemberName(field->getName())),
                             *optionType, fieldLoc, field->getName())))
        return failure();
      continue;
    }
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
    // FR-78: exactly where the residual one-slot rejection would fire, an
    // ALL-aggregate-arm union (every arm a record or constant array —
    // lwIP's `union { ip6_addr_t; ip4_addr_t; }`) diverts to the OPAQUE
    // STORAGE model instead: the union imports as a single sizeof-sized
    // byte blob whose struct_def carries `emitrust.opaque_union`, so a
    // RECORD containing it imports and whole-value copies work, while
    // every access through any arm rejects at its own site
    // (`emitMemberLValue`/`projectMemberPlace`/`emitRecordInitField`/
    // `convertAPValueInit`) and the Rust emitter refuses any leaked arm
    // access (marker contract). Any scalar, pointer, bit-field, or
    // unnamed arm keeps the C99-44 one-slot rejections above verbatim,
    // and the C++ path is out of scope — both keep today's record-level
    // rejection.
    bool allArmsAggregate =
        !astContext().getLangOpts().CPlusPlus &&
        llvm::all_of(definition->fields(), [&](const clang::FieldDecl *f) {
          return f->getType()->isRecordType() ||
                 astContext().getAsConstantArrayType(f->getType());
        });
    if (allArmsAggregate) {
      // Arms admitted earlier in this loop (identical-type aliases) were
      // recorded as slot aliases; under the opaque model EVERY arm is
      // access-rejected instead, so those entries must not survive.
      fieldNames.clear();
      fieldTypes.clear();
      for (const clang::FieldDecl *member : definition->fields()) {
        unionSlotStorage.erase(member);
        opaqueUnionArms.insert(member);
      }
      opaqueUnions.insert(definition);
      // C sizeof of the union (largest arm rounded up to alignment, C99
      // 6.7.2.1), NOT the largest arm's bare size: keeps the existing
      // sizeof/alignof constant folds consistent with the blob.
      uint64_t bytes =
          astContext()
              .getTypeSizeInChars(astContext().getRecordType(definition))
              .getQuantity();
      fieldNames.push_back(memberNameArena.emplace_back("opaque"));
      fieldTypes.push_back(emitrust::ArrayType::get(
          builder.getContext(), bytes,
          IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned)));
      return success();
    }
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
  // FR-113: an enum this import already rejected has NO enum_def in the
  // module, so there is no Rust type for a caller to name and no variant
  // constant to reference. Saying so here — at the use site, which is where
  // the type or enumerator was wanted — is what keeps a recovering import
  // from shipping a field or a constant whose type does not exist (the
  // measured failure was a SILENTLY unbuildable crate: `pub m: Match` with
  // no `Match` anywhere, E0425 with zero attribution). Mirrors
  // `importRecord`'s rejectedRecords wrapper; see CImporterInternal.h for
  // why the ordinary `importedEnums` memo cannot answer this.
  if (rejectedEnums.contains(definition)) {
    std::string message =
        (llvm::Twine("unsupported: enum '") + definition->getName() +
         "' was rejected, so a type naming it cannot be imported")
            .str();
    // FR-126: same source-symbol memo as the record cascade above.
    std::string sourceSym = graphItemSymbol(definition);
    if (!sourceSym.empty())
      cascadeSourceByMessage[message] = sourceSym;
    return emitError(loc) << message;
  }
  if (!importedEnums.insert(definition).second)
    return success();
  LogicalResult imported = importEnumUncached(definition);
  if (failed(imported))
    rejectedEnums.insert(definition);
  return imported;
}

LogicalResult
CImporter::importEnumUncached(const clang::EnumDecl *definition) {
  Location defLoc = translateLoc(definition->getBeginLoc());
  // A SCOPED enumeration (`enum class`/`enum struct`) is deliberately NOT
  // rejected here (FR-113): scoping is compile-time namespacing that clang
  // already discharged at every resolved use site, so the definition
  // imports as the same underlying-typed open enum an unscoped enum gets.
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

