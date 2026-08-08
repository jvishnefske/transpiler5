//===- ImportCTypes.cpp - C type mapping for the importer -----*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's C-to-MLIR type mapping: mapType/mapParamType/
/// mapStructFieldType, the classifyPointerReturn/classifyPointerParams/
/// classifyFnPtrPointerResult pointer-shape classification family, and the
/// small location/system-header utilities. Split out of ImportC.cpp by pure
/// code motion (W1.6); see CImporterInternal.h for the CImporter class
/// declaration this file implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Locations and types
//===----------------------------------------------------------------------===//

Location CImporter::translateLoc(clang::SourceLocation sourceLoc) {
  MLIRContext *context = builder.getContext();
  if (sourceLoc.isInvalid())
    return UnknownLoc::get(context);
  const clang::SourceManager &sourceManager = astContext().getSourceManager();
  clang::PresumedLoc presumed = sourceManager.getPresumedLoc(sourceLoc);
  if (presumed.isInvalid())
    return UnknownLoc::get(context);
  return FileLineColLoc::get(StringAttr::get(context, presumed.getFilename()),
                             presumed.getLine(), presumed.getColumn());
}

bool CImporter::isSystemHeaderDecl(const clang::Decl *decl) const {
  const clang::SourceManager &sourceManager = astContext().getSourceManager();
  clang::SourceLocation loc =
      sourceManager.getExpansionLoc(decl->getLocation());
  return loc.isValid() && sourceManager.isInSystemHeader(loc);
}

LogicalResult CImporter::rejectSystemHeaderUse(Location loc,
                                               llvm::StringRef what,
                                               llvm::StringRef name) {
  return emitError(loc) << "unsupported: " << what << " '" << name
                        << "' declared in a system header; not part of the "
                           "supported C subset";
}

FailureOr<Type> CImporter::mapType(clang::QualType type, Location loc) {
  clang::QualType canonical = type.getCanonicalType();

  // C99-7 qualifier policy. volatile: located rejection (no Rust
  // counterpart in the emitted model). _Atomic: located rejection (no
  // atomics in the single-threaded model; checked here for a precise
  // message instead of the generic tail rejection). const: no effect on
  // the value type — the const mapping is positional (immutable statics
  // for never-written globals, const-marked variables for literal
  // backings, plain SSA lets after mem2reg). restrict: accepted and
  // ignored (an aliasing hint; the pointer region analysis is stricter).
  if (canonical.isVolatileQualified())
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (canonical->isAtomicType())
    return emitError(loc) << "unsupported: _Atomic-qualified type";
  // FR-48: a C++ reference. An lvalue reference in a PARAMETER position is
  // supported and never arrives here: `mapParamType` intercepts it ahead
  // of its `mapType` delegation and maps it onto the same borrow types a
  // `ParamKind::ScalarRef` pointer parameter already uses. Every OTHER
  // position stays rejected, each with its own located wording so a later
  // wave has a documented starting point instead of discovering the
  // message fresh (the sharpening W2.0 did for references as a whole,
  // continued one level down now that one of the positions works).
  //
  // An rvalue reference is separated out because it is a different
  // problem, not a harder version of the same one: binding `T&&` implies
  // a MOVE, and this model has no ownership transfer to express it with,
  // so no amount of borrow machinery reaches it.
  if (canonical->isRValueReferenceType())
    return emitError(loc)
           << "unsupported: rvalue reference types are not yet supported";
  // The residual lvalue-reference positions — a reference LOCAL, a
  // reference global, a reference behind a pointer, a reference to a
  // pointer. A reference local is the interesting one and is deliberately
  // NOT supported: its binding is a borrow that lives from the
  // declaration to the end of scope, so `int &r = x; r = 1; x = 2; r = 3;`
  // would emit a `&mut x` held across an independent use of `x` and fail
  // Rust's borrow checker. A parameter has no such hazard — its borrow is
  // created and consumed inside one call expression, and each use inside
  // the callee derefs afresh — which is exactly why parameters land this
  // wave and locals do not.
  if (canonical->isReferenceType())
    return emitError(loc) << "unsupported: reference types are not yet supported";

  // C99-37: va_list is a PERMANENT rejection — Rust has no stable
  // variadic-argument access, so the target's `__builtin_va_list` (and
  // its underlying `__va_list_tag` record) has no meaningful
  // representation in any position: local, parameter, field, or global.
  // A va_list touched inside a variadic DEFINITION is caught earlier by
  // the variadic-definition rejection (`bodyUsesVaList`); this check
  // catches the type escaping into non-variadic contexts, which would
  // otherwise import as a garbage register-save-area struct.
  {
    clang::ASTContext &context = astContext();
    if (context.hasSameType(canonical,
                            context.getBuiltinVaListType().getCanonicalType()))
      return emitError(loc) << "unsupported: va_list type";
    if (const auto *record = canonical->getAs<clang::RecordType>())
      if (const clang::Decl *tag = context.getVaListTagDecl())
        if (record->getDecl()->getCanonicalDecl() == tag->getCanonicalDecl())
          return emitError(loc) << "unsupported: va_list type";
  }

  if (const auto *builtin =
          llvm::dyn_cast<clang::BuiltinType>(canonical.getTypePtr())) {
    switch (builtin->getKind()) {
    case clang::BuiltinType::Bool:
      return Type(builder.getI1Type());
    case clang::BuiltinType::Char_S:
    case clang::BuiltinType::SChar:
      return Type(builder.getIntegerType(8));
    case clang::BuiltinType::Short:
      return Type(builder.getIntegerType(16));
    case clang::BuiltinType::Int:
      return Type(builder.getIntegerType(32));
    case clang::BuiltinType::Long:
    case clang::BuiltinType::LongLong:
      return Type(builder.getIntegerType(64));
    case clang::BuiltinType::Float:
      return Type(builder.getF32Type());
    case clang::BuiltinType::Double:
      return Type(builder.getF64Type());
    // CTS 00204 / C99-8 policy: `long double` maps to f64, the same type
    // as `double` (a UB refinement: C requires long double to be at least
    // as wide as double, every f64-exact value round-trips, and the
    // supported shapes perform no long-double-only arithmetic). The
    // conversions double <-> long double become identities, L-suffixed
    // literals convert their x87 APFloat to IEEE double at import, and
    // constructs that would observe the substitution stay rejected:
    // sizeof/alignof of long double (emitSizeofAlignof) and the printf
    // %La/%LA hex-float forms (translatePrintfFormat).
    case clang::BuiltinType::LongDouble:
      return Type(builder.getF64Type());
    // Unsigned types map to MLIR unsigned (not signless) integers so the
    // Rust emitter renders them as `uN`; arith ops require signless
    // operands, so all arithmetic on these goes through emitrust ops.
    case clang::BuiltinType::Char_U:
    case clang::BuiltinType::UChar:
      return Type(IntegerType::get(builder.getContext(), 8,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::UShort:
      return Type(IntegerType::get(builder.getContext(), 16,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::UInt:
      return Type(IntegerType::get(builder.getContext(), 32,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::ULong:
    case clang::BuiltinType::ULongLong:
      return Type(IntegerType::get(builder.getContext(), 64,
                                   IntegerType::Unsigned));
    default:
      break;
    }
    return emitError(loc) << "unsupported builtin type '"
                          << llvm::Twine(canonical.getAsString()) << "'";
  }

  if (const auto *record =
          llvm::dyn_cast<clang::RecordType>(canonical.getTypePtr())) {
    const clang::RecordDecl *decl = record->getDecl();
    // W2.3 STL recognition: a record living in namespace `std` is diverted
    // BEFORE the generic path below ever calls `importRecord` — the
    // recursion into libstdc++ internals (private pointers, allocators,
    // ...) the STL recognition wave exists specifically to avoid. A plain
    // C program never has a `NamespaceDecl` in any `DeclContext` chain, so
    // `isInStdNamespace()` is unconditionally false there: this check is a
    // no-op on the C path.
    if (decl->isInStdNamespace())
      return mapStdLibraryType(decl, loc);
    // A union imports as a one-field struct (see `collectUnionSlot`), so
    // it maps to the same `!emitrust.struct` any record does; shapes the
    // one-slot model cannot represent are rejected by the import below.
    const clang::RecordDecl *definition = decl->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete struct type";
    // CTS-BR (00216): a u8-only aggregate is padding-free by construction
    // and imports as a BYTE REGION — a plain `!emitrust.array<Nxui8>`
    // where N == sizeof (a flexible array member contributes zero) — and
    // NO struct_def is ever emitted for the record.
    if (isByteRegionRecord(definition)) {
      uint64_t bytes =
          astContext().getTypeSizeInChars(canonical).getQuantity();
      return Type(emitrust::ArrayType::get(
          builder.getContext(), bytes,
          IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned)));
    }
    if (failed(importRecord(definition, loc)))
      return failure();
    // The type name is resolved after the import: a block-scope record
    // maps to its mangled per-declaration name (see localRecordNames), a
    // file-scope record to the collision-resolved name assigned by
    // `structSymbolName` during the import, and a bare anonymous struct
    // to its synthesized shape-keyed name.
    std::string structName = emittedRecordName(definition);
    if (structName.empty())
      return emitError(loc) << "unsupported: anonymous struct type";
    return Type(emitrust::StructType::get(builder.getContext(), structName));
  }

  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(canonical)) {
    // CTS-BR (00216): an array of byte-region records flattens to ONE
    // region of n*sizeof bytes — elements are consecutive padding-free
    // byte runs, so the flat region preserves every offset.
    if (isByteRegionAggregate(canonical)) {
      uint64_t bytes =
          astContext().getTypeSizeInChars(canonical).getQuantity();
      if (bytes > 0)
        return Type(emitrust::ArrayType::get(
            builder.getContext(), bytes,
            IntegerType::get(builder.getContext(), 8,
                             IntegerType::Unsigned)));
    }
    FailureOr<Type> element = mapType(array->getElementType(), loc);
    if (failed(element))
      return failure();
    uint64_t size = array->getSize().getZExtValue();
    if (size == 0)
      return emitError(loc) << "unsupported: zero-length array";
    // A multi-dimensional array recurses naturally: the element of the
    // outer dimension is itself an `!emitrust.array` (rendered as the
    // nested Rust array `[[T; N]; M]`). An array of function pointers is
    // a fn-ptr TABLE (CTS-BR, 00216): its never-reassigned global form
    // folds to a Some(target) element list, and runtime stores into a
    // slot are rejected at the assignment.
    return Type(emitrust::ArrayType::get(builder.getContext(), size, *element));
  }

  if (const auto *enumType =
          llvm::dyn_cast<clang::EnumType>(canonical.getTypePtr())) {
    const clang::EnumDecl *definition = enumType->getDecl()->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete enum type";
    // Anonymous enums are plain `int` everywhere else in the importer
    // (enumerator references become `i32` constants at their use sites), so
    // a value of anonymous enum type is a plain `i32`.
    if (definition->getName().empty())
      return Type(builder.getIntegerType(32));
    if (failed(importEnum(definition, loc)))
      return failure();
    return Type(emitrust::EnumType::get(
        builder.getContext(), enumTypeRustName(definition->getName())));
  }

  // Function pointers are ordinary `!emitrust.fn_ptr` values (rendered
  // `Option<fn(...)>`), legal as locals, globals, struct fields,
  // parameters, and results alike; they must be recognized before the
  // general pointer rejection below.
  if (canonical->isFunctionPointerType()) {
    const auto *fnType =
        canonical->getPointeeType()->castAs<clang::FunctionType>();
    SmallVector<Type> inputs;
    if (const auto *proto = llvm::dyn_cast<clang::FunctionProtoType>(fnType)) {
      if (proto->isVariadic())
        return emitError(loc) << "unsupported: variadic function pointer type";
      for (clang::QualType param : proto->getParamTypes()) {
        FailureOr<Type> mapped = mapType(param, loc);
        if (failed(mapped))
          return failure();
        if (!emitrust::FnPtrType::isValidComponentType(*mapped))
          return emitError(loc)
                 << "unsupported: function pointer parameter type";
        inputs.push_back(*mapped);
      }
    }
    // A prototype-less K&R `int (*f)()` maps to the zero-parameter form.
    // A local-storage decl whose call sites carry arguments is refined to
    // its callsite-inferred signature at declaration/parameter mapping
    // (FR-29, CTS 00209) and never consults this default; any remaining
    // argument-carrying call through an UNREFINED no-proto value is
    // rejected at the call site.
    SmallVector<Type> results;
    clang::QualType returnType = fnType->getReturnType();
    if (!returnType->isVoidType()) {
      if (isDataPointer(returnType)) {
        // A data-pointer fn-ptr result (CTS-S, 00089) is representable
        // only when every possible target erases it to one global base;
        // the result then erases from the fn_ptr signature too, and
        // indirect calls route to the base (`erasedGlobalReturnCallBase`).
        if (failed(classifyFnPtrPointerResult(fnType, loc)))
          return failure();
      } else {
        FailureOr<Type> mapped = mapType(returnType, loc);
        if (failed(mapped))
          return failure();
        if (!emitrust::FnPtrType::isValidComponentType(*mapped))
          return emitError(loc)
                 << "unsupported: function pointer result type";
        results.push_back(*mapped);
      }
    }
    return Type(
        emitrust::FnPtrType::get(builder.getContext(), inputs, results));
  }

  // A FILE* is an owned function-local stream handle (C99-48); any other
  // position (array element, return type, ...) that maps its type keeps
  // this located rejection.
  if (isFilePtrType(canonical))
    return emitError(loc)
           << "unsupported: FILE* is only supported as a function-local "
              "variable";
  if (canonical->isPointerType())
    return emitError(loc)
           << "unsupported: pointer type outside a parameter position";
  if (canonical->isArrayType())
    return emitError(loc) << "unsupported: non-constant array size";
  return emitError(loc) << "unsupported type '"
                        << llvm::Twine(canonical.getAsString()) << "'";
}

//===----------------------------------------------------------------------===//
// W2.3: STL recognition (std::vector<T>, std::string)
//===----------------------------------------------------------------------===//

std::optional<std::string>
CImporter::rustSpellingForElementType(Type type) {
  if (auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type))
    return opaque.getValue().str();
  if (auto structType = llvm::dyn_cast<emitrust::StructType>(type))
    return structType.getName().str();
  if (llvm::isa<Float32Type>(type))
    return std::string("f32");
  if (llvm::isa<Float64Type>(type))
    return std::string("f64");
  if (auto intType = llvm::dyn_cast<IntegerType>(type)) {
    if (intType.getWidth() == 1)
      return std::string("bool");
    bool isUnsigned = intType.isUnsigned();
    switch (intType.getWidth()) {
    case 8:
      return isUnsigned ? std::string("u8") : std::string("i8");
    case 16:
      return isUnsigned ? std::string("u16") : std::string("i16");
    case 32:
      return isUnsigned ? std::string("u32") : std::string("i32");
    case 64:
      return isUnsigned ? std::string("u64") : std::string("i64");
    default:
      break;
    }
  }
  return std::nullopt;
}

Type CImporter::parseStlElementType(llvm::StringRef spelling) {
  MLIRContext *context = builder.getContext();
  if (spelling == "String" || spelling.starts_with("Vec<"))
    return emitrust::OpaqueType::get(context, spelling);
  if (spelling == "bool")
    return builder.getI1Type();
  if (spelling == "f32")
    return builder.getF32Type();
  if (spelling == "f64")
    return builder.getF64Type();
  bool isUnsigned = spelling.starts_with("u");
  unsigned width = 0;
  if ((spelling.starts_with("i") || spelling.starts_with("u")) &&
      !spelling.substr(1).getAsInteger(10, width)) {
    switch (width) {
    case 8:
    case 16:
    case 32:
    case 64:
      return isUnsigned
                 ? IntegerType::get(context, width, IntegerType::Unsigned)
                 : builder.getIntegerType(width);
    default:
      break;
    }
  }
  // Anything else is trusted to be a struct name (StructType performs no
  // cross-checking against its referenced symbol either, per its own
  // documentation).
  if (spelling.empty())
    return Type();
  return emitrust::StructType::get(context, spelling);
}

bool CImporter::isStdArrayRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "array";
}

bool CImporter::isStdPairRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "pair";
}

bool CImporter::isStlOpaqueType(Type type) {
  auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type);
  return opaque && (opaque.getValue() == "String" ||
                    opaque.getValue().starts_with("Vec<"));
}

FailureOr<Type> CImporter::mapStdLibraryType(const clang::RecordDecl *decl,
                                             Location loc) {
  llvm::StringRef name = decl->getName();
  const auto *spec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl);
  if (name == "vector" && spec) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    if (args.size() < 1 || args[0].getKind() != clang::TemplateArgument::Type)
      return emitError(loc)
             << "unsupported: std::vector element type could not be "
                "determined";
    clang::QualType elementType = args[0].getAsType();
    FailureOr<Type> mappedElement = mapType(elementType, loc);
    if (failed(mappedElement))
      return failure();
    std::optional<std::string> spelling =
        rustSpellingForElementType(*mappedElement);
    if (!spelling)
      return emitError(loc)
             << "unsupported: std::vector<" << elementType.getAsString()
             << "> element type is not in the supported STL element set";
    return Type(emitrust::OpaqueType::get(builder.getContext(),
                                          "Vec<" + *spelling + ">"));
  }
  // W2.7: `std::array<T, N>` maps to the SAME `!emitrust.array<NxT>` a C
  // `T[N]` maps to — no new opaque family, and every existing array path
  // (aggregate init, subscript places, struct fields, multi-dim nesting)
  // applies unchanged. The importer-side special cases are the aggregate
  // initializer's one-level struct-wrapper peel (emitLocalVar) and the
  // `size()` member call (a compile-time constant N).
  if (name == "array" && spec) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    if (args.size() < 2 ||
        args[0].getKind() != clang::TemplateArgument::Type ||
        args[1].getKind() != clang::TemplateArgument::Integral)
      return emitError(loc)
             << "unsupported: std::array shape could not be determined";
    FailureOr<Type> element = mapType(args[0].getAsType(), loc);
    if (failed(element))
      return failure();
    uint64_t size = args[1].getAsIntegral().getZExtValue();
    if (size == 0)
      return emitError(loc) << "unsupported: zero-length std::array";
    return Type(emitrust::ArrayType::get(builder.getContext(), size, *element));
  }
  // W2.8: `std::pair<T1, T2>` imports as an importer-synthesized REAL
  // struct (route (b) of the task-005 spike): the specialization's record
  // genuinely holds just the two public fields `first`/`second`, so
  // importing it through the ordinary importRecord machinery reuses every
  // existing struct path (member places read/write, by-value returns,
  // parameter passing) with zero emitter changes. The struct name is
  // shape-keyed from the mapped element spellings (`PairI32I32`, ...) and
  // PRE-SEEDED into assignedStructNames so distinct instantiations — all
  // spelled `pair` in clang — cannot collide; construction is field-wise
  // (emitPairConstructInit), since no libc++ method is ever imported.
  if (name == "pair" && spec) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    if (args.size() < 2 ||
        args[0].getKind() != clang::TemplateArgument::Type ||
        args[1].getKind() != clang::TemplateArgument::Type)
      return emitError(loc)
             << "unsupported: std::pair shape could not be determined";
    FailureOr<Type> firstType = mapType(args[0].getAsType(), loc);
    if (failed(firstType))
      return failure();
    FailureOr<Type> secondType = mapType(args[1].getAsType(), loc);
    if (failed(secondType))
      return failure();
    std::optional<std::string> firstSpelling =
        rustSpellingForElementType(*firstType);
    std::optional<std::string> secondSpelling =
        rustSpellingForElementType(*secondType);
    auto nameable = [](const std::optional<std::string> &spelling) {
      return spelling && llvm::all_of(*spelling, [](char c) {
               return llvm::isAlnum(c) || c == '_';
             });
    };
    if (!nameable(firstSpelling) || !nameable(secondSpelling))
      return emitError(loc) << "unsupported: std::pair element type is not "
                               "in the supported set";
    const clang::RecordDecl *definition = decl->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: std::pair without a definition";
    if (!assignedStructNames.contains(definition))
      assignedStructNames[definition] =
          typeRustName("Pair_" + *firstSpelling + "_" + *secondSpelling);
    if (failed(importRecord(definition, loc)))
      return failure();
    return Type(emitrust::StructType::get(
        builder.getContext(), assignedStructNames.lookup(definition)));
  }
  if (name == "basic_string") {
    if (spec) {
      const clang::TemplateArgumentList &args = spec->getTemplateArgs();
      if (args.size() >= 1 &&
          args[0].getKind() == clang::TemplateArgument::Type &&
          astContext().hasSameType(args[0].getAsType().getCanonicalType(),
                                   astContext().CharTy))
        return Type(emitrust::OpaqueType::get(builder.getContext(), "String"));
    }
    return emitError(loc)
           << "unsupported: std::basic_string with a non-char character "
              "type";
  }
  return emitError(loc) << "unsupported: std::" << name.str()
                        << " is not a recognized STL type";
}

FailureOr<Type> CImporter::mapParamType(clang::QualType type, Location loc,
                                        ParamKind kind) {
  // C99-7: qualifiers on the parameter OBJECT itself are body-local and
  // never part of the function type (C11 6.7.6.3p15 composite rules;
  // `int x[volatile 5]` adjusts to `int * volatile x`, c-testsuite
  // 00162), so a top-level volatile is accepted and ignored exactly like
  // const and restrict. volatile anywhere deeper — the pointee chain,
  // which names caller-owned storage — keeps the located rejection; the
  // deep scan also covers the Carrier shape that bypasses `mapType`.
  clang::QualType canonical = type.getCanonicalType().getUnqualifiedType();
  // FR-48: a C++ lvalue reference parameter. A reference is a pointer that
  // is non-null, never reseated, and never subject to arithmetic, so it is
  // the STRICTLY SIMPLER case of the `ParamKind::ScalarRef` pointer
  // parameter directly below, and it reuses that class's machinery whole
  // rather than growing a parallel path: the same two borrow types, the
  // same direct SSA binding in `bindOrdinaryParam`, the same
  // `emitrust.deref`-per-use place resolution, and the same
  // `emitrust.addr_of` argument at the call site.
  //
  // The one thing it does NOT reuse is `classifyPointerParams`, and it
  // does not need to: that classification exists to decide whether a
  // pointer parameter is walked (a slice) or only dereferenced (a scalar
  // reference), and C++ forbids the walking case outright — a reference
  // cannot be incremented or subscripted as a reference. Every reference
  // parameter is a scalar borrow by construction, so it is answered here
  // and never consults `kind` at all. For the same reason
  // `PointerRegionAnalysis` (FR-28) needs no extension: it already
  // excludes `ParmVarDecl`s from region tracking, and a reference
  // parameter has no cursor to track.
  //
  // Mutability comes from the referent's constness — `const T&` borrows
  // shared (`!emitrust.ref<T>`), `T&` borrows mutably
  // (`!emitrust.mut_ref<T>`). Note this is FINER than the pointer
  // parameter's own scalar fall-through, which hands back `mut_ref` even
  // for a `const T *`: C++ code says `const T&` where C code says `T *`,
  // so honoring const here is what makes the emitted Rust idiomatic
  // rather than uniformly `&mut`.
  if (clang::QualType referent = cxxReferentType(canonical);
      !referent.isNull()) {
    // Deep volatile keeps the C99-7 rejection: the referent names
    // caller-owned storage, exactly like a pointee.
    if (hasVolatileQualifier(astContext(), referent))
      return emitError(loc) << "unsupported: volatile-qualified type";
    // A reference TO a data pointer (`T *&`) has no representation for the
    // same reason `T **` does not: the pointee is a pointer, which the
    // model decomposes into (base, cursor) rather than storing. A
    // function pointer is an ordinary Copy value and is fine.
    if (isDataPointer(referent))
      return emitError(loc) << "unsupported: reference-to-pointer parameter";
    // A reference to an array (`int (&)[N]`) would borrow a whole array
    // object; the slice machinery is reached through pointer decay, which
    // a reference parameter never performs, so there is nothing to bind.
    if (referent.getCanonicalType()->isArrayType())
      return emitError(loc) << "unsupported: reference-to-array parameter";
    FailureOr<Type> inner = mapType(referent, loc);
    if (failed(inner))
      return failure();
    if (referent.isConstQualified())
      return Type(emitrust::RefType::get(*inner));
    return Type(emitrust::MutRefType::get(*inner));
  }
  if (canonical->isPointerType() && !canonical->isFunctionPointerType() &&
      hasVolatileQualifier(astContext(), canonical->getPointeeType()))
    return emitError(loc) << "unsupported: volatile-qualified type";
  // A function-pointer parameter is an ordinary Copy value, not a
  // reference; it maps to `!emitrust.fn_ptr` like every other position
  // (through the stripped canonical, so a qualifier on the parameter
  // object itself stays ignored).
  if (canonical->isFunctionPointerType())
    return mapType(canonical, loc);
  if (canonical->isPointerType()) {
    clang::QualType pointee = canonical->getPointeeType();
    // An integer-carrier `void *` parameter (CTS-P3) is a plain i64: the
    // callee only ever truth-tests it, so the value never needs a region.
    if (kind == ParamKind::Carrier)
      return Type(builder.getIntegerType(64));
    // A `void *` parameter has no element type to classify against and no
    // region to join at the call boundary (CTS-P9).
    if (pointee.getCanonicalType()->isVoidType())
      return emitError(loc) << "unsupported: void pointer parameter";
    // A pointee that is itself a *data* pointer has no representation
    // (CTS-P5); a function-pointer pointee is an ordinary Copy value
    // (`!emitrust.fn_ptr`) and slices/references over it are fine — the
    // shape a decayed array-of-function-pointers parameter produces.
    if (pointee.getCanonicalType()->isPointerType() &&
        !pointee.getCanonicalType()->isFunctionPointerType())
      return emitError(loc) << "unsupported: pointer-to-pointer parameter";
    // CTS-BR (00216): a pointer to a byte-region aggregate is a byte
    // slice parameter regardless of the body-usage classification —
    // member reads through it are region reads at constant byte offsets
    // over the slice base. A const pointee borrows shared.
    if (kind != ParamKind::CellSlice && isByteRegionAggregate(pointee)) {
      auto slice = emitrust::SliceType::get(IntegerType::get(
          builder.getContext(), 8, IntegerType::Unsigned));
      if (pointee.isConstQualified())
        return Type(emitrust::RefType::get(slice));
      return Type(emitrust::MutRefType::get(slice));
    }
    FailureOr<Type> inner = mapType(pointee, loc);
    if (failed(inner))
      return failure();
    if (kind == ParamKind::CellSlice) {
      // A cell-slice class parameter (CTS-P10): a shared reference to a
      // run of Cells over one of the class's mutable global array bases.
      if (!emitrust::CellSliceType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: cell-slice parameter element type " << *inner;
      return Type(
          emitrust::RefType::get(emitrust::CellSliceType::get(*inner)));
    }
    if (kind == ParamKind::Slice) {
      // A slice element must be sized and scalar/struct; a pointer to an
      // array (`int (*)[N]`) has no slice shape.
      if (!emitrust::SliceType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: slice parameter element type " << *inner;
      // CTS-BR (00216): a walked `const unsigned char *` is a SHARED
      // byte slice (`&[u8]`), the borrow shape the byte-region walkers
      // pass region views through.
      if (pointee.isConstQualified() && isU8ScalarType(pointee))
        return Type(
            emitrust::RefType::get(emitrust::SliceType::get(*inner)));
      return Type(
          emitrust::MutRefType::get(emitrust::SliceType::get(*inner)));
    }
    // FR-55: the SAME CTS-BR const rule, in the scalar-reference position.
    // A `const unsigned char *` that is only dereferenced borrows shared
    // (`&u8`) exactly as the walked form above borrows `&[u8]` and the
    // byte-region aggregate form borrows `&[u8]`. Without this the three
    // spellings of one rule disagree, and the disagreement is observable:
    // a caller holding a `&[u8]` region that hands ONE element to such a
    // parameter (`f(&k[i])`, the shape every `set_key`/`encrypt_block`
    // wrapper in real crypto code has) would have to reborrow a shared
    // slice mutably, which is `error[E0596]` — an emitted crate that does
    // not compile. C's own guarantee is what makes the shared borrow
    // right, not a convenience: `const T *` says the callee never writes
    // through the pointer, and writing through it anyway is a constraint
    // violation clang rejects outright, so no valid program can observe a
    // lost write. Non-u8 pointees are deliberately left mutable: their
    // slice form is mutable too, so the two agree already, and the
    // `&mut T` is what the rest of the model (staged globals, owner
    // receivers) is written against.
    if (kind == ParamKind::ScalarRef && pointee.isConstQualified() &&
        isU8ScalarType(pointee))
      return Type(emitrust::RefType::get(*inner));
    return Type(emitrust::MutRefType::get(*inner));
  }
  // The stripped canonical keeps a top-level-volatile value parameter
  // (`volatile int x`, a body-local copy) out of `mapType`'s rejection.
  return mapType(canonical, loc);
}

FailureOr<Type> CImporter::mapStructFieldType(clang::QualType type,
                                              Location loc,
                                              const clang::FieldDecl *field) {
  // C99-7: a data-pointer field stores as a plain i64 without mapping its
  // pointee, so the volatile scan must run before that shortcut.
  if (hasVolatileQualifier(astContext(), type))
    return emitError(loc) << "unsupported: volatile-qualified type";
  // FR-48: a reference-typed MEMBER is one of the two hard positions
  // references are not supported in, and it is rejected here rather than
  // through `mapType`'s residual so the message names the position. The
  // difficulty is ownership, not syntax: the borrow outlives the
  // expression that created it and is stored inside an object whose own
  // lifetime is unrelated to the referent's, so the emitted struct would
  // need a lifetime parameter that nothing in the model can infer or
  // name. A reference member also silently changes the struct's copy
  // semantics (it has no assignment operator), which the field-by-field
  // aggregate model would get wrong.
  if (type->isReferenceType())
    return emitError(loc)
           << "unsupported: reference struct members are not yet supported";
  if (isDataPointer(type)) {
    // Stage 2 of the owner-struct self-reference extension: a field Pass A
    // proved always points into the same promoted owner array stores as
    // that array's synthesized enum-of-indices type instead of a plain
    // i64, so its writes can lower to a genuine `emitrust.switch` match.
    if (field) {
      auto arrayIt = arrayMemberPtrBindings.find(field);
      if (arrayIt != arrayMemberPtrBindings.end() &&
          arrayIt->second.invalidReason.empty())
        return getOrCreateArrayMemberEnumType(field, arrayIt->second, loc);
      // W4.2e Part B (FR-39): a self-referential node-pool field renders as
      // the nullable pool index `Option<usize>`; its reads/writes lower
      // through the __emitrust_pool_* helpers.
      if (poolNextFields.contains(field->getCanonicalDecl())) {
        requestPoolHelpers();
        return Type(
            emitrust::OpaqueType::get(builder.getContext(), "Option<usize>"));
      }
    }
    return Type(builder.getIntegerType(64));
  }
  return mapType(type, loc);
}

ArrayRef<ParamKind>
CImporter::classifyPointerParams(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = paramKindsCache.find(canonical);
  if (it != paramKindsCache.end())
    return it->second;
  SmallVector<ParamKind, 4> kinds(func->getNumParams(), ParamKind::ScalarRef);
  // The classification is a property of the definition's body; without a
  // definition in the merged ASTs every pointer parameter stays a scalar
  // reference (checked at definition-time signature refinement).
  const clang::FunctionDecl *definition = func->getDefinition();
  if (definition && definition->hasBody() &&
      definition->getNumParams() == kinds.size()) {
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> sliceParams;
    collectSliceParams(definition->getBody(), sliceParams);
    for (auto [index, param] : llvm::enumerate(definition->parameters())) {
      if (sliceParams.contains(param))
        kinds[index] = ParamKind::Slice;
      // The interprocedural cell-slice class (CTS-P10, planned in Pass A)
      // overrides the per-body slice classification.
      if (cellSliceParams.contains(param))
        kinds[index] = ParamKind::CellSlice;
      // A `void *` parameter that the body only ever truth-tests is an
      // integer carrier (CTS-P3): it lowers as a plain i64 and call sites
      // pass carrier values. Any other `void *` parameter keeps the
      // historical rejection in `mapParamType`.
      if (isDataPointer(param->getType()) &&
          param->getType()
              .getCanonicalType()
              ->getPointeeType()
              .getCanonicalType()
              ->isVoidType() &&
          voidParamOnlyTruthTested(definition->getBody(), param))
        kinds[index] = ParamKind::Carrier;
    }
  }
  auto [entry, inserted] =
      paramKindsCache.try_emplace(canonical, std::move(kinds));
  (void)inserted;
  return entry->second;
}

FailureOr<Type> CImporter::classifyPointerReturn(
    const clang::FunctionDecl *func, Location loc) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = pointerReturnKinds.find(canonical);
  if (it != pointerReturnKinds.end())
    return it->second;
  const clang::FunctionDecl *definition = func->getDefinition();
  if (!definition || !definition->hasBody())
    return emitError(loc) << "unsupported: pointer return type";
  SmallVector<const clang::ReturnStmt *> returns;
  collectReturnStmts(definition->getBody(), returns);
  if (returns.empty())
    return emitError(loc) << "unsupported: pointer return type";
  // The integer-carrier kind (CTS-P3): every return site yields an
  // integer riding in pointer clothing (a null constant, an
  // integer-to-pointer cast, a carrier-region local, or a call to another
  // carrier-returning function), so the function returns a plain i64.
  if (llvm::any_of(returns,
                   [](const clang::ReturnStmt *ret) {
                     return !ret->getRetValue() ||
                            !returnedFunctionExpr(ret->getRetValue());
                   }) &&
      isCarrierReturnFunction(func)) {
    Type kind = builder.getIntegerType(64);
    pointerReturnKinds.try_emplace(canonical, kind);
    return kind;
  }
  // The single-global-base RETURN region kind (CTS-S, 00089): one return
  // site yielding the address of a global claims the kind for the whole
  // function — every site must then return the SAME whole mutable global
  // (cursor 0) and no site may return NULL. The pointer result ERASES from
  // the signature (the classification is the null `Type`): no runtime
  // pointer state travels, the call is retained for its side effects, and
  // callers route accesses through the returned pointer to the global
  // directly (see `erasedGlobalReturnCallBase`).
  if (llvm::any_of(returns, [](const clang::ReturnStmt *ret) {
        return ret->getRetValue() &&
               returnedGlobalAddress(ret->getRetValue()).base;
      })) {
    const clang::VarDecl *commonBase = nullptr;
    Location memberSiteLoc = loc;
    bool sawMemberSite = false;
    for (const clang::ReturnStmt *ret : returns) {
      Location retLoc = translateLoc(ret->getReturnLoc());
      const clang::Expr *value = ret->getRetValue();
      if (!value)
        return emitError(retLoc) << "unsupported: returned pointer value "
                                    "(a bare return cannot carry the "
                                    "global address)";
      if (isNullPointerConstantExpr(value))
        return emitError(retLoc)
               << "unsupported: return sites mix a global address and NULL";
      ReturnedGlobalAddress site = returnedGlobalAddress(value);
      if (!site.base)
        return emitError(retLoc)
               << "unsupported: returned pointer value (only a returned "
                  "whole-global or function address has a representation)";
      if (commonBase && site.base != commonBase)
        return emitError(retLoc) << "unsupported: return sites disagree on "
                                    "the returned global base";
      commonBase = site.base;
      if (!site.wholeObject) {
        sawMemberSite = true;
        memberSiteLoc = retLoc;
      }
    }
    if (sawMemberSite)
      return emitError(memberSiteLoc)
             << "unsupported: returned pointer value (a member address is "
                "not a whole-global base)";
    if (commonBase->getType().isConstQualified())
      return emitError(loc)
             << "unsupported: returned address of a const global";
    // The erased routing stages and stores the base back at its own type,
    // so the returned pointee must be exactly the whole global's type.
    clang::QualType pointee = definition->getReturnType()
                                  .getCanonicalType()
                                  ->getPointeeType();
    if (!astContext().hasSameUnqualifiedType(pointee, commonBase->getType()))
      return emitError(loc) << "unsupported: returned pointer value (the "
                               "returned global does not match the "
                               "pointee type)";
    globalReturnBases[canonical] = commonBase;
    pointerReturnKinds.try_emplace(canonical, Type());
    return Type();
  }
  Type kind;
  for (const clang::ReturnStmt *ret : returns) {
    Location retLoc = translateLoc(ret->getReturnLoc());
    const clang::Expr *value = ret->getRetValue();
    const clang::Expr *fnExpr = value ? returnedFunctionExpr(value) : nullptr;
    if (!fnExpr)
      return emitError(retLoc)
             << "unsupported: returned pointer value (only a returned "
                "function address has a representation; a cursor into a "
                "callee-local region would dangle)";
    const auto *fn = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    FailureOr<Type> mapped =
        mapType(astContext().getPointerType(fn->getType()), retLoc);
    if (failed(mapped))
      return failure();
    if (kind && kind != *mapped)
      return emitError(retLoc)
             << "unsupported: return sites disagree on the returned "
                "function pointer signature";
    kind = *mapped;
  }
  if (!kind)
    return emitError(loc) << "unsupported: pointer return type";
  pointerReturnKinds.try_emplace(canonical, kind);
  return kind;
}

unsigned CImporter::dataPtrReturnFnAddressTakenTuCount(
    clang::QualType returnType) const {
  auto it = wholeProgram.dataPtrReturnFnAddressTakenTus.find(
      returnType.getCanonicalType().getAsString());
  if (it == wholeProgram.dataPtrReturnFnAddressTakenTus.end())
    return 0;
  return it->second.size();
}

FailureOr<const clang::VarDecl *>
CImporter::classifyFnPtrPointerResult(const clang::FunctionType *fnType,
                                      Location loc) {
  const clang::Type *key = astContext()
                               .getCanonicalType(clang::QualType(fnType, 0))
                               .getTypePtr();
  if (const clang::VarDecl *cached = fnPtrReturnBases.lookup(key))
    return cached;
  // The candidate set must be whole-program: in a multi-TU project another
  // TU could take a diverging function's address after this TU classified.
  // W3.4 G1 relaxes this using the whole-program candidate-completeness fact:
  // when at most ONE TU takes the address of any function returning this data
  // pointer type, this TU's per-TU `addressTakenFunctions` is the complete
  // whole-program candidate set and the classifier below runs soundly. Two or
  // more TUs keep the blanket rejection — a precise cross-TU disagreement
  // diagnostic would need the full erased-base substrate (design.md FR-34).
  if (!currentSoleTU &&
      dataPtrReturnFnAddressTakenTuCount(fnType->getReturnType()) > 1)
    return emitError(loc) << "unsupported: function pointer result type";
  if (!fnPtrReturnInProgress.insert(key).second)
    return emitError(loc) << "unsupported: function pointer result type";
  auto eraseInProgress = [&]() { fnPtrReturnInProgress.erase(key); };
  clang::QualType returnType = fnType->getReturnType().getCanonicalType();
  SmallVector<const clang::FunctionDecl *, 4> candidates;
  for (const clang::FunctionDecl *fn : addressTakenFunctions)
    if (astContext().hasSameUnqualifiedType(
            fn->getReturnType().getCanonicalType(), returnType))
      candidates.push_back(fn);
  if (candidates.empty()) {
    eraseInProgress();
    return emitError(loc) << "unsupported: function pointer result type";
  }
  const clang::VarDecl *common = nullptr;
  for (const clang::FunctionDecl *fn : candidates) {
    FailureOr<Type> kind = classifyPointerReturn(fn, loc);
    if (failed(kind)) {
      eraseInProgress();
      return failure();
    }
    const clang::VarDecl *base =
        globalReturnBases.lookup(fn->getCanonicalDecl());
    if (*kind || !base) {
      eraseInProgress();
      return emitError(loc) << "unsupported: function pointer result type";
    }
    if (common && base != common) {
      eraseInProgress();
      return emitError(loc) << "unsupported: return sites disagree on the "
                               "returned global base";
    }
    common = base;
  }
  eraseInProgress();
  fnPtrReturnBases[key] = common;
  return common;
}

const clang::VarDecl *
CImporter::erasedGlobalReturnCallBase(const clang::Expr *expr,
                                      const clang::CallExpr **callOut) const {
  const auto *call = llvm::dyn_cast<clang::CallExpr>(stripTrivia(expr));
  if (!call)
    return nullptr;
  if (callOut)
    *callOut = call;
  if (const clang::FunctionDecl *callee = call->getDirectCallee())
    return globalReturnBases.lookup(callee->getCanonicalDecl());
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParenImpCasts();
  clang::QualType calleeType = calleeExpr->getType().getCanonicalType();
  if (!calleeType->isFunctionPointerType())
    return nullptr;
  return fnPtrReturnBases.lookup(
      calleeType->getPointeeType().getCanonicalType().getTypePtr());
}

bool CImporter::isCarrierReturnFunction(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = carrierReturnCache.find(canonical);
  if (it != carrierReturnCache.end())
    return it->second;
  if (!isDataPointer(func->getReturnType())) {
    carrierReturnCache.try_emplace(canonical, false);
    return false;
  }
  const clang::FunctionDecl *definition = func->getDefinition();
  if (!definition || !definition->hasBody()) {
    carrierReturnCache.try_emplace(canonical, false);
    return false;
  }
  // Break recursion cycles pessimistically (a self-recursive carrier
  // return would need a fixpoint; none of the supported programs do).
  if (!carrierReturnInProgress.insert(canonical).second)
    return false;
  PointerRegionAnalysis regions;
  regions.carrierReturnQuery = [this](const clang::FunctionDecl *callee) {
    return isCarrierReturnFunction(callee);
  };
  regions.literalTemps = &literalTemps;
  regions.analyze(astContext(), definition->getBody());
  SmallVector<const clang::ReturnStmt *> returns;
  collectReturnStmts(definition->getBody(), returns);
  bool carrier = !returns.empty();
  for (const clang::ReturnStmt *ret : returns)
    if (!ret->getRetValue() ||
        !isCarrierReturnExpr(regions, ret->getRetValue())) {
      carrier = false;
      break;
    }
  carrierReturnInProgress.erase(canonical);
  carrierReturnCache.try_emplace(canonical, carrier);
  return carrier;
}

bool CImporter::isCarrierReturnExpr(PointerRegionAnalysis &regions,
                                    const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  if (e->isNullPointerConstant(astContext(),
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return true;
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    if (cast->getCastKind() == clang::CK_NullToPointer)
      return true;
    // Only a pointer-width integer rides as a carrier (mirroring
    // `recordPointerWrite`'s classification gate).
    if (cast->getCastKind() == clang::CK_IntegralToPointer)
      return astContext().getTypeSize(cast->getSubExpr()->getType()) == 64;
    if (cast->getCastKind() == clang::CK_NoOp)
      return isCarrierReturnExpr(regions, cast->getSubExpr());
  }
  if (const clang::VarDecl *var = asLoadedLocalVarRef(e))
    if (regions.tracks(var) && isCarrierRegion(regions.regionOf(var)))
      return true;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e))
    if (const clang::FunctionDecl *callee = call->getDirectCallee())
      return isCarrierReturnFunction(callee);
  return false;
}

