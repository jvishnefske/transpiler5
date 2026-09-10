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

#include "llvm/Support/SaveAndRestore.h"

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
    // FR-56: `long` maps by the TARGET's width, not a hardcoded 64 — a
    // forwarded `-m32` triple makes it 4 bytes, and on 32-bit Darwin
    // triples `size_t` is spelled `unsigned long` (where 32-bit Linux
    // spells it `unsigned int`), so the hardcoded width would leak ui64
    // into every sizeof fold there. 64-bit targets are unchanged.
    case clang::BuiltinType::Long:
      return Type(
          builder.getIntegerType(astContext().getTypeSize(canonical)));
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
      return Type(IntegerType::get(builder.getContext(),
                                   astContext().getTypeSize(canonical),
                                   IntegerType::Unsigned));
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
    return Type(emitrust::EnumType::get(builder.getContext(),
                                        enumRustName(definition)));
  }

  // Function pointers are ordinary `!emitrust.fn_ptr` values (rendered
  // `Option<fn(...)>`), legal as locals, globals, struct fields,
  // parameters, and results alike; they must be recognized before the
  // general pointer rejection below.
  if (canonical->isFunctionPointerType()) {
    const auto *fnType =
        canonical->getPointeeType()->castAs<clang::FunctionType>();
    // FR-76: an arithmetic-pointee scalar-pointer PARAMETER component
    // classifies as a region-typed slice — the same `mapParamType` Slice
    // branch a body-classified parameter uses (`&mut [T]`, shared `&[u8]`
    // for the const-u8 flavor, FR-55/CTS-BR) — so a callback signature
    // like `void (*)(uint8_t *, unsigned)` imports instead of funneling
    // into the pointer residual and rejecting the whole record. One level
    // deep only (`mappingFnPtrComponent` gates the recursion: a NESTED
    // fn-ptr component keeps plain mapping, so nested-with-pointer stays a
    // located frontier) and C-only (the C++ divert is untouched). Unlike
    // FR-75's eager block this is gate-free — the classification is a
    // property of the TYPE, not of a trait policy — and the address-taken
    // forcing in `classifyPointerParams` keeps a bound function's own
    // signature exactly equal, so `resolveFunctionPointerDecl`'s equality
    // check stays the loud backstop for every unforceable mismatch
    // (CellSlice, Carrier).
    //
    // FR-102 widens the subset by one shape: a component whose pointee is
    // a COMPLETE record maps through the SAME `mapParamType` ScalarRef
    // branch an ordinary struct-pointer parameter takes (`&mut Record`,
    // or `&mut [u8]` when the record is a byte region), so a callback
    // signature like `int (*)(struct payload *, int)` — including the
    // SELF-REFERENTIAL `int (*)(struct node *, int)` inside `struct node`
    // itself, which terminates on `importedRecords` and resolves the type
    // by name — imports instead of rejecting the whole record. Rust needs
    // no lifetime there: `fn(&mut Node, i32) -> i32` is higher-ranked and
    // elides. Completeness is checked BEFORE the dispatch: an incomplete
    // pointee must keep the pointer residual verbatim rather than move to
    // `mapParamType`'s incomplete-struct wording. Components still outside
    // the subset — `void *` (no consensus source solo-TU),
    // pointer-to-pointer, incomplete pointees, mutually recursive pairs
    // (incomplete by construction) — fall through to `mapType` and keep
    // their located rejections.
    const bool classifyComponents =
        !astContext().getLangOpts().CPlusPlus && !mappingFnPtrComponent;
    llvm::SaveAndRestore<bool> componentGuard(mappingFnPtrComponent, true);
    SmallVector<Type> inputs;
    if (const auto *proto = llvm::dyn_cast<clang::FunctionProtoType>(fnType)) {
      if (proto->isVariadic())
        return emitError(loc) << "unsupported: variadic function pointer type";
      for (clang::QualType param : proto->getParamTypes()) {
        bool sliceComponent = false;
        bool structRefComponent = false;
        if (classifyComponents && isDataPointer(param)) {
          clang::QualType pointee =
              param.getCanonicalType()->getPointeeType();
          sliceComponent = pointee.getCanonicalType()->isArithmeticType();
          // FR-102: a COMPLETE record pointee takes the ordinary
          // struct-pointer parameter mapping. The completeness test is the
          // gate, not an afterthought — routing an incomplete pointee into
          // `mapParamType` would replace the frontier's pointer residual
          // with the incomplete-struct wording.
          if (!sliceComponent)
            if (const auto *recordType =
                    pointee.getCanonicalType()->getAs<clang::RecordType>())
              structRefComponent =
                  recordType->getDecl()->getDefinition() != nullptr;
        }
        FailureOr<Type> mapped =
            sliceComponent  ? mapParamType(param, loc, ParamKind::Slice)
            : structRefComponent
                ? mapParamType(param, loc, ParamKind::ScalarRef)
                : mapType(param, loc);
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
  if (auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type)) {
    // W2.21: a `Box<T>` (std::unique_ptr) NEVER composes into another
    // recognized container. This is the wave's mandatory CONTAINER SCREEN
    // and it is deliberately central: every other family
    // (vector/pair/optional/map value) reaches its element spelling
    // through this one function, so refusing `Box<` here keeps
    // `std::vector<std::unique_ptr<T>>`, `std::optional<std::unique_ptr<T>>`
    // and `std::pair<int, std::unique_ptr<T>>` located rejections instead
    // of silently admitting `Vec<Box<T>>`-shaped surface the wave never
    // spiked (the move/clone question at every element read is real and
    // unanswered). `std::array` maps its element directly and needs its
    // own copy of this screen.
    if (opaque.getValue().starts_with("Box<"))
      return std::nullopt;
    return opaque.getValue().str();
  }
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
  // W2.21: EVERY opaque family `rustSpellingForElementType` can produce
  // must round-trip back to an `OpaqueType` here. Before this wave only
  // `String`/`Vec<` did; `Option<`, `BTreeMap<` and `BTreeSet<` fell into
  // the "trusted to be a struct name" tail below and would have come back
  // as a bogus `!emitrust.struct<"Option<i32>">`. Not reachable as a bug
  // before (no container ever nested one), but `Box<` would have inherited
  // it, so the whole set is listed rather than a fourth special case added.
  if (spelling == "String" || spelling.starts_with("Vec<") ||
      spelling.starts_with("Option<") || spelling.starts_with("BTreeMap<") ||
      spelling.starts_with("BTreeSet<") || spelling.starts_with("Box<"))
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

/// W2.21: whether `type` is a `std::unique_ptr<T, D>` specialization. The
/// libstdc++-15 spelling a `auto p = std::make_unique<T>(...)` VarDecl
/// carries is the alias sugar `__detail::__unique_ptr_t<T>`, so the probe
/// always goes through the CANONICAL record (whose name is still
/// "unique_ptr").
bool CImporter::isStdUniquePtrRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "unique_ptr";
}

bool CImporter::isStdPairRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "pair";
}

bool CImporter::isStdOptionalRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "optional";
}

bool CImporter::isStdStringViewRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "basic_string_view";
}

bool CImporter::isStdVariantRecordType(clang::QualType type) {
  const auto *record = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const clang::RecordDecl *decl = record->getDecl();
  return decl->isInStdNamespace() && decl->getIdentifier() &&
         decl->getName() == "variant";
}

bool CImporter::isStlOpaqueType(Type type) {
  auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type);
  return opaque && (opaque.getValue() == "String" ||
                    opaque.getValue().starts_with("Vec<") ||
                    opaque.getValue().starts_with("Option<") ||
                    opaque.getValue().starts_with("BTreeMap<") ||
                    opaque.getValue().starts_with("BTreeSet<") ||
                    opaque.getValue().starts_with("Box<"));
}

/// W2.21: whether `type` is the `Box<T>` opaque `std::unique_ptr<T>` maps
/// to. Split out for the same reason `isStlMapOpaque` was: the family gates
/// answer differently for a Box (its "methods" are the payload's, reached
/// through Deref, not a container vocabulary).
bool CImporter::isStlBoxOpaque(Type type) {
  auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type);
  return opaque && opaque.getValue().starts_with("Box<");
}

/// W2.21: the mapped payload type of a `Box<T>` opaque, or a null Type if
/// the spelling fails to round-trip.
Type CImporter::stlBoxPayloadType(emitrust::OpaqueType boxType) {
  llvm::StringRef spelling = boxType.getValue();
  if (!spelling.starts_with("Box<") || !spelling.ends_with(">"))
    return Type();
  return parseStlElementType(spelling.substr(4, spelling.size() - 5));
}

bool CImporter::isStlMapOpaque(Type type) {
  auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type);
  return opaque && opaque.getValue().starts_with("BTreeMap<");
}

bool CImporter::isStlSetOpaque(Type type) {
  auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(type);
  return opaque && opaque.getValue().starts_with("BTreeSet<");
}

llvm::StringRef CImporter::stlOpaqueDisplayName(emitrust::OpaqueType opaque) {
  llvm::StringRef spelling = opaque.getValue();
  if (spelling == "String")
    return "std::string";
  if (spelling.starts_with("Vec<"))
    return "std::vector";
  if (spelling.starts_with("Option<"))
    return "std::optional";
  if (spelling.starts_with("BTreeMap<"))
    return "std::map";
  if (spelling.starts_with("BTreeSet<"))
    return "std::set";
  if (spelling.starts_with("Box<"))
    return "std::unique_ptr";
  return "std::";
}

/// W2.20: splits a `BTreeMap<K, V>` opaque's inner argument list at the
/// TOP-LEVEL comma. A positional split would break the moment either
/// argument is itself generic, so the scan tracks angle-bracket depth.
static bool splitStlMapArgs(llvm::StringRef inner, llvm::StringRef &key,
                            llvm::StringRef &value) {
  unsigned depth = 0;
  for (size_t i = 0; i < inner.size(); ++i) {
    char c = inner[i];
    if (c == '<')
      ++depth;
    else if (c == '>')
      --depth;
    else if (c == ',' && depth == 0) {
      key = inner.substr(0, i);
      value = inner.substr(i + 1).ltrim();
      return !key.empty() && !value.empty();
    }
  }
  return false;
}

bool CImporter::stlMapKeyValueTypes(emitrust::OpaqueType mapType, Type &key,
                                    Type &value) {
  llvm::StringRef spelling = mapType.getValue();
  if (!spelling.starts_with("BTreeMap<") || !spelling.ends_with(">"))
    return false;
  llvm::StringRef inner =
      spelling.substr(9, spelling.size() - 10);
  llvm::StringRef keySpelling;
  llvm::StringRef valueSpelling;
  if (!splitStlMapArgs(inner, keySpelling, valueSpelling))
    return false;
  key = parseStlElementType(keySpelling);
  value = parseStlElementType(valueSpelling);
  return key && value;
}

Type CImporter::stlSetElementType(emitrust::OpaqueType setType) {
  llvm::StringRef spelling = setType.getValue();
  if (!spelling.starts_with("BTreeSet<") || !spelling.ends_with(">"))
    return Type();
  return parseStlElementType(spelling.substr(9, spelling.size() - 10));
}

/// W2.20: the ORD screen. Rust's `BTreeMap`/`BTreeSet` require `Ord` on
/// the key; `f32`/`f64` implement only `PartialOrd`, and a synthesized
/// struct or data enum derives neither. Without this screen those keys
/// sail through `rustSpellingForElementType` and surface as a DEFERRED
/// rustc `error[E0277]: the trait bound `f64: Ord` is not satisfied`
/// instead of a located rejection (measured 2026-08-21). Integers (both
/// signednesses, every C width) and `bool` order identically on both
/// sides. `String` keys ORDER identically too (measured byte-for-byte on
/// an 11-key adversarial set including \x7f, \xC8 and \xFF — libstdc++
/// `char_traits` compares unsigned), but `BTreeMap::entry(&mut m, k)`
/// MOVES the key, so admitting them needs an inserted clone; out this
/// wave.
static bool isOrdKeyType(Type type) {
  return llvm::isa<IntegerType>(type);
}

/// W2.20: the DEFAULT screen. The `operator[]` lowering is
/// `*m.entry(k).or_default()`, which needs the value type to implement
/// Rust's `Default` AND to agree with C++'s value-initialization. Both
/// hold for the integer widths (0), `bool` (false) and the floats (0.0).
/// A `std::variant` alternative (a synthesized data enum) derives no
/// `Default`, and a nested container value raises a move/clone question
/// at the load site that is out of subset this wave.
static bool isDefaultableValueType(Type type) {
  return llvm::isa<IntegerType>(type) || llvm::isa<FloatType>(type);
}

/// W2.20: whether `comp` is exactly `std::less<key>` — the DEFAULT
/// comparator. A `std::map<int,int,Rev>` keeps the RecordDecl name "map"
/// (AST-confirmed), so the comparator argument is the only thing that
/// distinguishes a reversed or custom ordering from the one BTreeMap
/// reproduces.
/// W2.20: emits a module-level `emitrust.use` for `path` exactly once
/// per module, keeping first-mention order (each new one lands after the
/// last existing `use`, ahead of every item). The BTreeMap/BTreeSet
/// spellings are the only unqualified std paths the importer emits, so
/// the crate needs the import to compile at all.
void CImporter::requireModuleUse(llvm::StringRef path) {
  if (!emittedModuleUses.insert(path).second)
    return;
  Block *body = module.getBody();
  Operation *lastUse = nullptr;
  for (Operation &op : *body)
    if (llvm::isa<emitrust::UseOp>(&op))
      lastUse = &op;
  OpBuilder moduleBuilder(module.getContext());
  if (lastUse)
    moduleBuilder.setInsertionPointAfter(lastUse);
  else
    moduleBuilder.setInsertionPointToStart(body);
  moduleBuilder.create<emitrust::UseOp>(module.getLoc(),
                                        moduleBuilder.getStringAttr(path));
}

bool CImporter::isStdLessComparator(clang::QualType comp,
                                    clang::QualType key) {
  const auto *record = comp.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const auto *spec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record->getDecl());
  if (!spec || !spec->isInStdNamespace() || !spec->getIdentifier() ||
      spec->getName() != "less")
    return false;
  const clang::TemplateArgumentList &args = spec->getTemplateArgs();
  return args.size() == 1 &&
         args[0].getKind() == clang::TemplateArgument::Type &&
         astContext().hasSameType(args[0].getAsType().getCanonicalType(),
                                  key.getCanonicalType());
}

/// W2.21: whether `deleter` is exactly `std::default_delete<payload>` —
/// the DEFAULT deleter, and the only one whose behavior `Box<T>`'s own drop
/// reproduces. A custom deleter (`std::unique_ptr<int, D>`) keeps the
/// RecordDecl name "unique_ptr" (AST-confirmed), so the deleter template
/// argument is the only screen that catches it. Modeled verbatim on
/// `isStdLessComparator` above.
bool CImporter::isStdDefaultDeleter(clang::QualType deleter,
                                    clang::QualType payload) {
  const auto *record = deleter.getCanonicalType()->getAs<clang::RecordType>();
  if (!record)
    return false;
  const auto *spec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record->getDecl());
  if (!spec || !spec->isInStdNamespace() || !spec->getIdentifier() ||
      spec->getName() != "default_delete")
    return false;
  const clang::TemplateArgumentList &args = spec->getTemplateArgs();
  return args.size() == 1 &&
         args[0].getKind() == clang::TemplateArgument::Type &&
         astContext().hasSameType(args[0].getAsType().getCanonicalType(),
                                  payload.getCanonicalType());
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
    // W2.21: std::array maps its element DIRECTLY (no
    // `rustSpellingForElementType` round-trip), so it needs its own copy of
    // the container screen — and it additionally dodges the W2.17
    // destructor gate, because `userDeclaredDestructor` strips CLANG array
    // types, not `std::array`. Without this, `std::array<std::unique_ptr<T>,
    // N>` would be the one position that silently admits an unspiked
    // `[Box<T>; N]`.
    if (isStlBoxOpaque(*element))
      return emitError(loc)
             << "unsupported: std::array<std::unique_ptr<...>, N> element "
                "type is not in the supported STL element set";
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
  // W2.11: `std::optional<T>` maps to `!emitrust.opaque<"Option<S>">` where
  // S is the mapped element's Rust spelling — Rust's `Option<T>` IS the
  // semantic model (engaged/empty), so no synthesized struct is needed and
  // the existing opaque machinery (variable places, method_call receivers)
  // applies unchanged. A nested optional element is OUT this wave:
  // `Option<Option<T>>` would need `Some(None)` construction shapes the
  // recognized constructor table cannot spell yet.
  if (name == "optional" && spec) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    if (args.size() < 1 || args[0].getKind() != clang::TemplateArgument::Type)
      return emitError(loc)
             << "unsupported: std::optional element type could not be "
                "determined";
    clang::QualType elementType = args[0].getAsType();
    FailureOr<Type> mappedElement = mapType(elementType, loc);
    if (failed(mappedElement))
      return failure();
    std::optional<std::string> spelling =
        rustSpellingForElementType(*mappedElement);
    if (!spelling || llvm::StringRef(*spelling).starts_with("Option<"))
      return emitError(loc)
             << "unsupported: std::optional<" << elementType.getAsString()
             << "> element type is not in the supported STL element set";
    return Type(emitrust::OpaqueType::get(builder.getContext(),
                                          "Option<" + *spelling + ">"));
  }
  // W2.14: `std::variant<A, B>` over exactly two DISTINCT supported
  // scalar alternatives imports as a SYNTHESIZED closed two-variant Rust
  // data enum — route (a) of the W2.14 spike: no Rust std variant image
  // exists (so no opaque mapping is possible), and the data-enum dialect
  // machinery (enum_variant construction, RESULT-mode match expansion
  // for index()/std::get<T>) renders today. The enum name is shape-keyed
  // from the mapped alternative spellings (`VariantI32F64` — UpperCamel;
  // rustc rejects the double-underscore sketch under the crate's
  // non_camel_case_types deny) so distinct instantiations — all spelled
  // `variant` in clang — cannot collide, and repeated mentions reuse one
  // module-level definition. Everything the recognizer cannot prove is a
  // located rejection: any arity but two, duplicate alternatives
  // (indistinguishable BY TYPE at every use site, since construction,
  // operator=, and std::get<T> all select by exact mapped-type
  // equality), and non-scalar alternatives (the Copy-deriving
  // data_enum_def payload set) this wave.
  if (name == "variant" && spec) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    // The alternatives arrive as one Pack template argument
    // (`variant<_Types...>`); flatten defensively.
    llvm::SmallVector<clang::QualType, 4> alternatives;
    for (const clang::TemplateArgument &arg : args.asArray()) {
      if (arg.getKind() == clang::TemplateArgument::Type) {
        alternatives.push_back(arg.getAsType());
        continue;
      }
      if (arg.getKind() != clang::TemplateArgument::Pack)
        return emitError(loc)
               << "unsupported: std::variant shape could not be determined";
      for (const clang::TemplateArgument &element : arg.pack_elements()) {
        if (element.getKind() != clang::TemplateArgument::Type)
          return emitError(loc)
                 << "unsupported: std::variant shape could not be determined";
        alternatives.push_back(element.getAsType());
      }
    }
    if (alternatives.size() != 2)
      return emitError(loc)
             << "unsupported: only a two-alternative std::variant is "
                "recognized";
    llvm::SmallVector<Type, 2> mapped;
    llvm::SmallVector<std::string, 2> spellings;
    for (clang::QualType alternative : alternatives) {
      FailureOr<Type> element = mapType(alternative, loc);
      if (failed(element))
        return failure();
      // Scalars only this wave: floats and SIGNLESS (signed-C) integers —
      // the types whose arith constants back the V0{0} default image and
      // the panic-arm dummy yields. Unsigned integers (emitrust-typed,
      // not arith-constructible) and every aggregate/opaque stay out.
      auto intType = llvm::dyn_cast<IntegerType>(*element);
      bool scalar =
          llvm::isa<FloatType>(*element) || (intType && intType.isSignless());
      std::optional<std::string> spelling =
          rustSpellingForElementType(*element);
      if (!scalar || !spelling)
        return emitError(loc) << "unsupported: std::variant alternative "
                                 "type is not in the supported scalar set";
      mapped.push_back(*element);
      spellings.push_back(*spelling);
    }
    if (mapped[0] == mapped[1])
      return emitError(loc)
             << "unsupported: std::variant with duplicate alternatives";
    std::string enumName = typeRustName(
        ("Variant_" + spellings[0] + "_" + spellings[1]));
    auto [it, inserted] = variantEnumAlternatives.try_emplace(
        enumName, llvm::SmallVector<Type, 2>(mapped));
    if (inserted) {
      OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
      moduleBuilder.create<emitrust::DataEnumDefOp>(
          loc, moduleBuilder.getStringAttr(enumName),
          moduleBuilder.getStrArrayAttr({"V0", "V1"}),
          moduleBuilder.getArrayAttr({moduleBuilder.getStrArrayAttr({"v"}),
                                      moduleBuilder.getStrArrayAttr({"v"})}),
          moduleBuilder.getArrayAttr(
              {moduleBuilder.getTypeArrayAttr({mapped[0]}),
               moduleBuilder.getTypeArrayAttr({mapped[1]})}));
    }
    return Type(emitrust::DataEnumType::get(builder.getContext(), enumName));
  }
  // W2.20: `std::map<K, V>` -> `!emitrust.opaque<"BTreeMap<K, V>">` and
  // `std::set<K>` -> `!emitrust.opaque<"BTreeSet<K>">`.
  //
  // BTreeMap/BTreeSet, not HashMap/HashSet: std::map and std::set are
  // ORDERED and their iteration order is OBSERVABLE in stdout, so the
  // byte-diff oracle demands the ordered Rust container. Measured
  // 2026-08-21: libstdc++ `std::map<std::string,int>` and Rust
  // `BTreeMap<String,i32>` agree byte-for-byte on an 11-key adversarial
  // set including \x7f, \xC8 and \xFF. `std::unordered_map` has no such
  // image at all and stays a PERMANENT located rejection below.
  //
  // Three screens, all new code — nothing existing rejects any of them:
  //  * the comparator must be the default `std::less<K>` (a custom or
  //    reversed ordering keeps the RecordDecl name "map"),
  //  * the key must be Ord in Rust (floats are only PartialOrd; a
  //    synthesized struct/data enum derives neither), and
  //  * the value must be Default (the `entry(k).or_default()` place that
  //    reproduces C++'s default-inserting `operator[]` needs it).
  if ((name == "map" || name == "set") && spec) {
    bool isMap = name == "map";
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    unsigned arity = isMap ? 4 : 3;
    if (args.size() != arity ||
        args[0].getKind() != clang::TemplateArgument::Type ||
        (isMap && args[1].getKind() != clang::TemplateArgument::Type))
      return emitError(loc) << "unsupported: std::" << name.str()
                            << " shape could not be determined";
    clang::QualType keyType = args[0].getAsType();
    unsigned comparatorIndex = isMap ? 2 : 1;
    if (args[comparatorIndex].getKind() != clang::TemplateArgument::Type ||
        !isStdLessComparator(args[comparatorIndex].getAsType(), keyType))
      return emitError(loc) << "unsupported: std::" << name.str()
                            << " with a comparator other than std::less";
    FailureOr<Type> mappedKey = mapType(keyType, loc);
    if (failed(mappedKey))
      return failure();
    std::optional<std::string> keySpelling =
        rustSpellingForElementType(*mappedKey);
    if (!keySpelling || !isOrdKeyType(*mappedKey))
      return emitError(loc)
             << "unsupported: std::" << name.str() << " key type '"
             << keyType.getAsString()
             << "' is not in the supported ordered key set";
    if (!isMap) {
      requireModuleUse("std::collections::BTreeSet");
      return Type(emitrust::OpaqueType::get(builder.getContext(),
                                            "BTreeSet<" + *keySpelling + ">"));
    }
    clang::QualType valueType = args[1].getAsType();
    FailureOr<Type> mappedValue = mapType(valueType, loc);
    if (failed(mappedValue))
      return failure();
    std::optional<std::string> valueSpelling =
        rustSpellingForElementType(*mappedValue);
    if (!valueSpelling || !isDefaultableValueType(*mappedValue))
      return emitError(loc)
             << "unsupported: std::map value type '"
             << valueType.getAsString()
             << "' is not in the supported value set";
    requireModuleUse("std::collections::BTreeMap");
    return Type(emitrust::OpaqueType::get(
        builder.getContext(),
        "BTreeMap<" + *keySpelling + ", " + *valueSpelling + ">"));
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
  // W2.21: `std::unique_ptr<T>` -> `!emitrust.opaque<"Box<T>">`.
  //
  // Box<T>, NOT Option<Box<T>>: both images were measured byte-identical
  // against clang++ -std=c++17 during the spike, so the choice is cost, not
  // correctness — and `Option<Box<T>>` forces `(**o.as_ref().unwrap())` at
  // every dereference a never-null program never needed. This follows
  // FR-99's precedent verbatim (design.md: "the local stays the bare
  // struct"): the ALWAYS-INITIALIZED subset is admitted as the bare owned
  // value and every nullable shape — default construction, `= nullptr`,
  // `if (p)`, `p == nullptr`, `reset()` — takes a located rejection naming
  // the real reason (a Rust `Box<T>` cannot be null).
  //
  // Four screens, all new code:
  //  * the ARRAY form `std::unique_ptr<T[]>` (template argument 0 is a
  //    clang ArrayType; the record name is unchanged) destroys with
  //    `delete[]` and would need `Box<[T]>`, a different representation,
  //  * a CUSTOM DELETER (template argument 1 is not `std::default_delete<T>`;
  //    the record name is unchanged) runs user code at drop that `Box`'s
  //    own drop does not,
  //  * a CONST payload has no writable place image, and
  //  * the PAYLOAD must be a scalar or an imported struct — a nested
  //    container payload is exactly the unspiked `Box<Vec<...>>` surface
  //    the container screen in `rustSpellingForElementType` refuses from
  //    the other direction.
  if (name == "unique_ptr" && spec) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    if (args.size() < 1 || args[0].getKind() != clang::TemplateArgument::Type)
      return emitError(loc)
             << "unsupported: std::unique_ptr shape could not be determined";
    clang::QualType payload = args[0].getAsType();
    if (payload->isArrayType())
      return emitError(loc)
             << "unsupported: the std::unique_ptr<T[]> array form has no Box "
                "image (Box<[T]> is a different, unspiked representation)";
    if (args.size() < 2 || args[1].getKind() != clang::TemplateArgument::Type ||
        !isStdDefaultDeleter(args[1].getAsType(), payload))
      return emitError(loc) << "unsupported: std::unique_ptr with a deleter "
                               "other than std::default_delete";
    if (payload.isConstQualified())
      return emitError(loc)
             << "unsupported: std::unique_ptr payload type '"
             << payload.getAsString()
             << "' is not in the supported payload set";
    FailureOr<Type> mappedPayload = mapType(payload, loc);
    if (failed(mappedPayload))
      return failure();
    std::optional<std::string> spelling =
        rustSpellingForElementType(*mappedPayload);
    bool admissible = llvm::isa<IntegerType, FloatType, emitrust::StructType>(
        *mappedPayload);
    if (!spelling || !admissible)
      return emitError(loc)
             << "unsupported: std::unique_ptr payload type '"
             << payload.getAsString()
             << "' is not in the supported payload set";
    return Type(emitrust::OpaqueType::get(builder.getContext(),
                                          "Box<" + *spelling + ">"));
  }
  // W2.21: the SHARED-ownership smart pointers get their own wording rather
  // than staying on the generic tail below, because the reason they are out
  // is a MODEL gap and not a backlog item: `Rc`/`Arc` have different
  // aliasing rules from `shared_ptr` (interior mutability is required for
  // any write through them, and `Rc` is not `Send`), and this subset has no
  // representation for shared ownership at all. `std::weak_ptr` names the
  // same missing model from the non-owning side.
  if (name == "shared_ptr" || name == "weak_ptr")
    return emitError(loc)
           << "unsupported: std::" << name.str()
           << " has no Rust image; Rc/Arc have different aliasing and this "
              "subset has no model for shared ownership";
  // W2.20: the unordered containers are a PERMANENT rejection, not a
  // backlog item, and they get their own wording so the ledger can tell
  // "deliberately, permanently out" from "not done yet": their iteration
  // order is UNSPECIFIED, so no Rust container reproduces them byte for
  // byte and admitting them would be a silent nondeterminism channel.
  if (name == "unordered_map" || name == "unordered_set" ||
      name == "unordered_multimap" || name == "unordered_multiset")
    return emitError(loc) << "unsupported: std::" << name.str()
                          << " iteration order is unspecified; no Rust "
                             "container reproduces it byte for byte";
  // W2.20: the multi- containers hold DUPLICATE keys; neither BTreeMap
  // nor BTreeSet does, and the Rust standard library has no equivalent.
  if (name == "multimap" || name == "multiset")
    return emitError(loc) << "unsupported: std::" << name.str()
                          << " stores duplicate keys; no Rust standard "
                             "container reproduces it";
  // W2.20: a map/set ITERATOR type. Only the whole `find(k) != end()`
  // idiom is recognized, and it never materializes an iterator; anything
  // that names one (`std::map<K,V>::iterator it;`) would otherwise leak a
  // standard-library-INTERNAL spelling into the diagnostic
  // (`std::_Rb_tree_iterator` on libstdc++, `std::__map_iterator` on
  // libc++), which no portable test could pin.
  if (name == "_Rb_tree_iterator" || name == "_Rb_tree_const_iterator" ||
      name == "__map_iterator" || name == "__map_const_iterator" ||
      name == "__tree_iterator" || name == "__tree_const_iterator")
    return emitError(loc) << "unsupported: std::map/std::set iterators are "
                             "only recognized in the find(k) != end() idiom";
  return emitError(loc) << "unsupported: std::" << name.str()
                        << " is not a recognized STL type";
}

FailureOr<Type> CImporter::mapParamType(clang::QualType type, Location loc,
                                        ParamKind kind,
                                        clang::QualType voidByteElem,
                                        bool sharedConstRecord) {
  // C99-7: qualifiers on the parameter OBJECT itself are body-local and
  // never part of the function type (C11 6.7.6.3p15 composite rules;
  // `int x[volatile 5]` adjusts to `int * volatile x`, c-testsuite
  // 00162), so a top-level volatile is accepted and ignored exactly like
  // const and restrict. volatile anywhere deeper — the pointee chain,
  // which names caller-owned storage — keeps the located rejection; the
  // deep scan also covers the Carrier shape that bypasses `mapType`.
  clang::QualType canonical = type.getCanonicalType().getUnqualifiedType();
  // C99-37 in the parameter position, by SUGAR: on AArch64 Darwin
  // `__builtin_va_list` is canonically `char *`, so without this a
  // va_list parameter would import as an ordinary scalar-ref pointer
  // instead of keeping its permanent rejection (see mapType's canonical
  // checks, which cover the x86_64 record form).
  if (isVaListSugarType(type, astContext()))
    return emitError(loc) << "unsupported: va_list type";
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
    // FR-88: a NULLABLE byte-slice parameter is an Option-wrapped shared
    // byte slice. The type is the OpaqueType spelling the C99-43
    // Option-of-cursor cells already render through (`Option<` opaque
    // values are established translate surface); classification
    // guarantees the const-u8 pointee, so the wrapped reference is
    // always the shared `&[u8]` — Copy, so per-use-site unwraps never
    // move the parameter value.
    if (kind == ParamKind::Nullable)
      return Type(emitrust::OpaqueType::get(builder.getContext(),
                                            "Option<&[u8]>"));
    // A `void *` parameter has no element type to classify against and no
    // region to join at the call boundary (CTS-P9) — EXCEPT the FR-71
    // byte-cursor admission: when the body scan proved every use converts
    // to one consistent byte pointee, the parameter maps as that byte
    // slice (the element rides in through `voidByteElem`; the `void`
    // pointee itself carries the constness, so `const void *` borrows
    // shared exactly like a walked `const uint8_t *`).
    if (pointee.getCanonicalType()->isVoidType()) {
      if (kind == ParamKind::Slice && !voidByteElem.isNull()) {
        FailureOr<Type> inner = mapType(voidByteElem, loc);
        if (failed(inner))
          return failure();
        auto slice = emitrust::SliceType::get(*inner);
        if (pointee.isConstQualified())
          return Type(emitrust::RefType::get(slice));
        return Type(emitrust::MutRefType::get(slice));
      }
      return emitError(loc) << "unsupported: void pointer parameter";
    }
    // A pointee that is itself a *data* pointer has no representation
    // (CTS-P5); a function-pointer pointee is an ordinary Copy value
    // (`!emitrust.fn_ptr`) and slices/references over it are fine — the
    // shape a decayed array-of-function-pointers parameter produces.
    if (pointee.getCanonicalType()->isPointerType() &&
        !pointee.getCanonicalType()->isFunctionPointerType())
      return emitError(loc) << "unsupported: pointer-to-pointer parameter";
    // FR-94: the free-only wrapper's FAM-record parameter takes the record
    // OWNED BY VALUE (dropping it is the deallocation); the caller's
    // argument MOVES in, so a use-after-free is rustc E0382, never silent.
    if (kind == ParamKind::OwnedRecord)
      return mapType(pointee, loc);
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
    // FR-80: a body-less (prospective requirement) function's const-STRUCT
    // pointee borrows shared too — the borrow shape the consumer-supplied
    // `&'static T` requirement address can flow into. Same soundness
    // rationale as the CTS-BR/FR-55 u8 rule above: `const T *` means the
    // callee never writes through the pointer, and a write anyway is a
    // constraint violation clang rejects. Gated by the SIGNATURE BUILDER
    // (`requirementSharedConstStructParam`) so defined functions, bin
    // crates, the ledgers, and defer mode keep their historical `&mut T`
    // byte-for-byte.
    if (kind == ParamKind::ScalarRef && sharedConstRecord &&
        pointee.isConstQualified() &&
        pointee.getCanonicalType()->isStructureType())
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
  // A function-pointer field of a SYSTEM-HEADER record stores as the same
  // inert i64 slot a data-pointer field does, WITHOUT mapping its
  // signature. Darwin's `struct __sFILE` carries cookie-I/O callback
  // members (`_close`/`_read`/`_seek`/`_write`) whose `void *` parameters
  // reject in fn-ptr signature mapping, which would make every
  // FILE*-parameter signature un-importable on macOS — glibc's
  // `_IO_FILE` has only data-pointer members, so this branch never fires
  // there. The record import exists only to keep hosted FILE handles
  // representable; main-file uses of such fields stay rejected at the
  // use site, so the slot is never read.
  if (field && type.getCanonicalType()->isFunctionPointerType() &&
      isSystemHeaderDecl(field))
    return Type(builder.getIntegerType(64));
  // FR-107: a pointer-ARRAY member (`T *m[N]`) stores as `[i64; N]` — one
  // inert cursor slot per element, the scalar data-pointer member rule
  // taken one dimension out. Only the TYPE is admitted: no element is
  // ever bound to a target object, so every element read, write,
  // address-of, comparison, cast, argument and return keeps a LOCATED
  // rejection at the use (see pointers-member-array-invalid.c). What this
  // buys is cascade dissolution — a record whose pointer-array members
  // are never touched in a TU stops gating every type that names it.
  if (const clang::ConstantArrayType *array =
          pointerArrayMemberType(astContext(), type)) {
    uint64_t size = array->getSize().getZExtValue();
    if (size == 0) // Defensive: a zero-length member is dropped earlier.
      return emitError(loc) << "unsupported: zero-length array";
    return Type(emitrust::ArrayType::get(builder.getContext(), size,
                                         builder.getIntegerType(64)));
  }
  return mapType(type, loc);
}

bool CImporter::requirementSharedConstStructParam(
    const clang::FunctionDecl *func, const clang::ParmVarDecl *param) {
  // Body-less under a trait policy only: a definition's own body decides
  // its shapes, and every non-trait mode keeps the historical `&mut T`.
  const clang::FunctionDecl *definition = func->getDefinition();
  if (definition && definition->hasBody())
    return false;
  if (func->isVariadic() || astContext().getLangOpts().CPlusPlus)
    return false;
  if (!classifyTimeTraitEligible())
    return false;
  clang::QualType type = param->getType();
  if (!isDataPointer(type))
    return false;
  clang::QualType pointee = type.getCanonicalType()->getPointeeType();
  return pointee.isConstQualified() &&
         pointee.getCanonicalType()->isStructureType();
}

ArrayRef<ParamKind>
CImporter::classifyPointerParams(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = paramKindsCache.find(canonical);
  if (it != paramKindsCache.end())
    return it->second;
  SmallVector<ParamKind, 4> kinds(func->getNumParams(), ParamKind::ScalarRef);
  SmallVector<clang::QualType, 4> byteElems(func->getNumParams(),
                                            clang::QualType());
  // The classification is a property of the definition's body; without a
  // definition in the merged ASTs every pointer parameter stays a scalar
  // reference (checked at definition-time signature refinement) — EXCEPT
  // the FR-75 requirement gate below.
  const clang::FunctionDecl *definition = func->getDefinition();
  // FR-75: a body-less function under a trait policy is (prospectively) a
  // REQUIREMENT on the environment, and a requirement signature must carry
  // the C region contract: `unsigned char *buf` means a region an
  // implementor may touch anywhere in, so a scalar reference (`&mut u8`,
  // one element) misrepresents it. Every arithmetic-pointee data-pointer
  // parameter therefore classifies as a SLICE eagerly — `mapParamType`'s
  // existing Slice branch produces `&mut [T]` (`&[u8]` for the const-u8
  // flavor, FR-55/CTS-BR) and the type-driven call-site lowering produces
  // region views at each argument's cursor; a call site with no region
  // behind it keeps its located rejection (never a silent one-element
  // borrow). A `void *` parameter has no pointee to classify from and no
  // body for FR-71's scan, so it joins by CALL-SITE CONSENSUS: a TU-wide
  // AST scan admits it as a byte-slice cursor iff every direct call site
  // passes an admissible byte view with one consistent element; anything
  // else keeps the verbatim void-pointer rejection. C-only, gated by
  // `classifyTimeTraitEligible` (never under FR-57a defer; for
  // `TraitWhenLibrary` only when this TU defines no `main`), so bin
  // crates, the c-testsuite ledger, and defer mode keep their historical
  // scalar shapes byte-for-byte. Non-arithmetic pointees (structs,
  // pointer-to-array) deliberately keep today's classification: the FR-75
  // mandate is the scalar-pointer region contract.
  if ((!definition || !definition->hasBody()) && !func->isVariadic() &&
      !astContext().getLangOpts().CPlusPlus && classifyTimeTraitEligible()) {
    for (auto [index, param] : llvm::enumerate(func->parameters())) {
      clang::QualType type = param->getType();
      if (!isDataPointer(type))
        continue;
      clang::QualType pointee =
          type.getCanonicalType()->getPointeeType().getCanonicalType();
      if (pointee->isVoidType()) {
        clang::QualType elem;
        bool sawCall = false;
        bool allByteViews = true;
        for (const clang::Decl *decl :
             astContext().getTranslationUnitDecl()->decls()) {
          const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
          if (!fn || !fn->doesThisDeclarationHaveABody())
            continue;
          if (!voidParamCallSitesAllByteViews(fn->getBody(), func, index,
                                              elem, sawCall)) {
            allByteViews = false;
            break;
          }
        }
        if (allByteViews && sawCall && !elem.isNull()) {
          kinds[index] = ParamKind::Slice;
          byteElems[index] = elem;
        }
        continue;
      }
      if (pointee->isArithmeticType())
        kinds[index] = ParamKind::Slice;
    }
  }
  if (definition && definition->hasBody() &&
      definition->getNumParams() == kinds.size()) {
    // FR-100 (C only): the TU-wide forwarding fixpoint answers the slice
    // question for C definitions, so a parameter that is only FORWARDED
    // into a callee that keeps a scalar reference keeps one too. The C++
    // path keeps the per-body walk verbatim (the FR's scope is the C
    // out-parameter idiom, and the C++ emission stays byte-identical).
    bool useFixpoint = !astContext().getLangOpts().CPlusPlus;
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> sliceParams;
    if (useFixpoint)
      computeForwardSliceParams();
    else
      collectSliceParams(definition->getBody(), sliceParams);
    for (auto [index, param] : llvm::enumerate(definition->parameters())) {
      if (useFixpoint ? forwardSliceParams.contains(param)
                      : sliceParams.contains(param))
        kinds[index] = ParamKind::Slice;
      // The interprocedural cell-slice class (CTS-P10, planned in Pass A)
      // overrides the per-body slice classification.
      if (cellSliceParams.contains(param))
        kinds[index] = ParamKind::CellSlice;
      bool isVoidPointerParam =
          isDataPointer(param->getType()) &&
          param->getType()
              .getCanonicalType()
              ->getPointeeType()
              .getCanonicalType()
              ->isVoidType();
      // A `void *` parameter that the body only ever truth-tests is an
      // integer carrier (CTS-P3): it lowers as a plain i64 and call sites
      // pass carrier values.
      if (isVoidPointerParam &&
          voidParamOnlyTruthTested(definition->getBody(), param)) {
        kinds[index] = ParamKind::Carrier;
        continue;
      }
      // FR-71: a `void *` parameter whose EVERY use converts to ONE
      // consistent byte pointee (`uint8_t *`/`unsigned char *`/`char *`
      // — the tinycrypt `_set`/`_compare` shape) is a byte-slice cursor:
      // it classifies `Slice` with the scanned element recorded for
      // `mapParamType` (a `void *` has no pointee to derive it from).
      // C-only by design — the C++ `static_cast` shape diverts before
      // this and keeps its verbatim rejection. Any `void *` parameter
      // neither scan admits keeps the historical rejection in
      // `mapParamType`.
      if (isVoidPointerParam && !astContext().getLangOpts().CPlusPlus) {
        clang::QualType elem;
        if (voidParamByteUsesOk(definition->getBody(), param, elem) &&
            !elem.isNull()) {
          kinds[index] = ParamKind::Slice;
          byteElems[index] = elem;
        }
      }
      // FR-88: a `const uint8_t *` parameter whose body null-tests it and
      // uses its region ONLY under a proven null guard (the tinycrypt
      // `if (personalization) memcpy(..., personalization, ...)` /
      // `if (p == NULL) return;` shapes) is NULLABLE: it maps as
      // `Option<&[u8]>`, so a C null-constant argument becomes a legal
      // `None` instead of the call-site rejection. CONST-POINTEE ONLY
      // (`Option<&mut [u8]>` is not Copy, so multiple use-site unwraps
      // would move it); C-only. A candidate the scan declines keeps its
      // historical classification — sound, because the declined shape
      // keeps BOTH the statically-non-null fold and the null-constant
      // call-site rejection verbatim. Requires a test AND a guarded use:
      // a never-used or never-tested parameter gains nothing from the
      // signature change.
      if (!astContext().getLangOpts().CPlusPlus &&
          kinds[index] == ParamKind::Slice && isDataPointer(param->getType())) {
        clang::QualType pointee =
            param->getType().getCanonicalType()->getPointeeType();
        if (pointee.isConstQualified() && isU8ScalarType(pointee)) {
          bool sawTest = false;
          bool sawUse = false;
          if (nullableParamUsesOk(definition->getBody(), param,
                                  /*nonNull=*/false, sawTest, sawUse) &&
              sawTest && sawUse)
            kinds[index] = ParamKind::Nullable;
        }
      }
    }
  }
  // FR-76 unification (option (b) of the spike): a function whose ADDRESS
  // is taken is (prospectively) bound into a fn-ptr position, and the
  // fn-ptr TYPE mapping classifies every arithmetic-pointee scalar-pointer
  // component as a region-typed slice. The function's own classification
  // must present the same region contract or the natural callback-table
  // shape would mismatch at the binding — measured in the spike: a
  // deref-only body stays ScalarRef (`&mut u8`) under `collectSliceParams`
  // alone, and a direct call with a local array would promote the callee
  // to an Owner_ method (`planOwners` now disqualifies address-taken
  // functions for the same reason). Only ScalarRef is forced: CellSlice (a
  // Cell-backed global class has no `&mut [T]` to lend) and Carrier keep
  // their kinds and fall to `resolveFunctionPointerDecl`'s located
  // signature-mismatch rejection, never a silent unification. C-only;
  // variadic targets can never bind to a fn_ptr, and a system-header
  // address is rejected at resolution, so both are excluded from forcing.
  if (!astContext().getLangOpts().CPlusPlus && !func->isVariadic() &&
      !isSystemHeaderDecl(func) &&
      llvm::is_contained(addressTakenFunctions, canonical) &&
      func->getNumParams() == kinds.size()) {
    for (auto [index, param] : llvm::enumerate(func->parameters())) {
      if (kinds[index] != ParamKind::ScalarRef ||
          !isDataPointer(param->getType()))
        continue;
      clang::QualType pointee =
          param->getType().getCanonicalType()->getPointeeType();
      if (pointee.getCanonicalType()->isArithmeticType())
        kinds[index] = ParamKind::Slice;
    }
  }
  // FR-94: the free-only wrapper's FAM-record parameters (planFamLift's
  // `famOwnedParams`) override to OWNED BY VALUE — checked last so no other
  // classification can claim them first.
  if (const clang::FunctionDecl *definition = func->getDefinition();
      definition && definition->getNumParams() == kinds.size())
    for (auto [index, param] : llvm::enumerate(definition->parameters()))
      if (famOwnedParams.contains(param))
        kinds[index] = ParamKind::OwnedRecord;
  voidByteElemsCache.try_emplace(canonical, std::move(byteElems));
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
  // FR-94: a recognized FAM-record allocator (`planFamLift` pinned every
  // return site to a claimed owned-tail local, or an elided-guard NULL)
  // returns the record OWNED BY VALUE — the tail rides along, and callers
  // bind the result to their own owned locals.
  if (famOwnedReturnFns.contains(canonical)) {
    FailureOr<Type> mapped = mapType(
        func->getReturnType().getCanonicalType()->getPointeeType(), loc);
    if (failed(mapped))
      return failure();
    // FR-99: an allocator with a REACHABLE `return NULL` (a parameter
    // validation, or a member-allocation failure arm) returns the record
    // NULLABLY: `Option<S>`, spelled exactly as `famOptionMemberType` spells
    // the FR-96 member-position one. Callers bind it through the Option temp
    // and unwrap at the binding or at the recognized guard.
    if (famNullableReturnFns.contains(canonical)) {
      FailureOr<emitrust::OpaqueType> option = famOptionOfStruct(*mapped, loc);
      if (failed(option))
        return failure();
      pointerReturnKinds.try_emplace(canonical, Type(*option));
      return Type(*option);
    }
    pointerReturnKinds.try_emplace(canonical, *mapped);
    return *mapped;
  }
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

// FR-100: resolve the TU-wide forwarding fixpoint for the current AST.
// Seeds are the per-body LOCAL slice demands (every non-benign,
// non-forwarding appearance); each forwarding edge (callerParam ->
// calleeParam) then propagates a slice demand BACKWARD, iterated to
// stability. The relation is monotone over a finite set, so it
// terminates, and — unlike an on-demand recursive query — the answer
// does not depend on which function the importer classifies first, which
// is what makes mutual-recursion cycles (a <-> b forwarding one another's
// parameter) well-defined instead of order-dependent.
//
// Computed once per AST: one `CImporter` spans every TU of a project and
// `astContext()` is swapped per TU, so a single "already computed" flag
// would reuse the first TU's answer for the second and silently
// scalarize its parameters.
void CImporter::computeForwardSliceParams() {
  if (!forwardSliceComputedFor.insert(&astContext()).second)
    return;
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 16> seeds;
  SmallVector<std::pair<const clang::ParmVarDecl *, const clang::ParmVarDecl *>>
      edges;
  // FR-209: the non-arithmetic-pointee forwards, whose demand the walker
  // SUSPENDS so that "is the delegation the sole reason?" is answerable, and
  // which is restored below before anything reads `seeds`.
  llvm::DenseMap<const clang::ParmVarDecl *, SliceParamDelegation> delegations;
  // FR-209: the FR-75 conservative re-walk's demands, held aside for the
  // same reason. They are not a SECOND cause -- that walk sees the very same
  // forwarding appearance -- so folding them into `seeds` before the
  // sole-reason question is asked would answer it with its own echo.
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 8> traitSeeds;
  // FR-75 interaction: under a trait policy, ANOTHER TU that sees only a
  // DECLARATION of an externally visible function classifies its
  // arithmetic-pointee data-pointer parameters as Slice eagerly (the
  // requirement-shape gate above), and this TU's definition may not
  // disagree with that assumption — it would be a conflicting
  // redeclaration. So for such a function the pre-FR-100 (per-body) class
  // is SEEDED here. Seeding, rather than special-casing the lookup, is
  // what keeps the result consistent: the demand also propagates BACKWARD
  // through forwarding edges, demoting any caller that forwards into the
  // function instead of leaving it to mismatch at the call site. The cost
  // is recorded in the FR entry: under a trait policy FR-100's exception
  // applies to internal-linkage functions only.
  bool traitEligible = classifyTimeTraitEligible();
  for (const clang::Decl *decl :
       astContext().getTranslationUnitDecl()->decls()) {
    const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!fn || !fn->doesThisDeclarationHaveABody())
      continue;
    collectSliceParamsWithEdges(fn->getBody(), seeds, edges, delegations);
    if (traitEligible && fn->isExternallyVisible()) {
      llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> local;
      collectSliceParams(fn->getBody(), local);
      traitSeeds.insert(local.begin(), local.end());
    }
  }
  // FR-209: ask the question, then RESTORE the suspended demand. After this
  // loop `seeds` is bit-for-bit what it was before FR-209 -- every recorded
  // parameter is back in it -- so no signature anywhere moves; the only
  // thing gained is a truthful `soleReason` on each record.
  for (auto &entry : delegations) {
    entry.second.soleReason = !seeds.contains(entry.first);
    seeds.insert(entry.first);
  }
  seeds.insert(traitSeeds.begin(), traitSeeds.end());
  forwardSliceDelegations.insert(delegations.begin(), delegations.end());
  forwardSliceParams.insert(seeds.begin(), seeds.end());
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, callee] : edges)
      if (forwardSliceParams.contains(callee) &&
          forwardSliceParams.insert(caller).second)
        changed = true;
  }
}
