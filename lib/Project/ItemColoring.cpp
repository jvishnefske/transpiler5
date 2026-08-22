//===- ItemColoring.cpp - three-color lattice over the item graph ---------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements FR-41's `probeAdmissibility`, `computeColoring`, and
/// `colorItems` (see EmitRust/Project/ItemColoring.h for the contract, the
/// three colors, the stub/type asymmetry, and the print format).
///
/// Shape of the implementation, and why.
///
/// ONE PROPAGATION RULE, NOT TWO. The header states the asymmetry as a single
/// rule — an edge to a Red target poisons its source RED unless the target is
/// stub-replaceable, in which case only YELLOW — and that is literally how
/// `runFixpoint` is written. The alternative, a hand-written case analysis per
/// edge kind, was rejected: it would have needed a separate, separately
/// justified answer for `BodyType`, for `ReadsGlobal`, and for a function that
/// is Red only because ITS callee is Red, and the three answers would drift.
/// Under the single rule they are consequences, and each is spelled out in the
/// header.
///
/// TWO SETS IN THE FIXPOINT, NOT ONE. `Red` alone cannot express the
/// asymmetry, because whether a Red function poisons its callers Red or Yellow
/// depends on whether a stub can be written FOR it, which is itself a
/// propagated fact: a function whose signature names a Red record has no
/// writable stub, so its callers are Red. So the fixpoint carries a second
/// monotone set, `signatureBroken`, seeded by the probe's signature-level
/// rejections and grown along `SigType` edges into Red types. Both sets only
/// grow, so the joint iteration is a least fixpoint and terminates.
///
/// RANKS, NOT VISIT ORDER, DECIDE BLAME. The color of an item is
/// order-independent for free (least fixpoint of a monotone rule set), but the
/// blame CHAIN is not: "the neighbor that poisoned me" is meaningless in a
/// graph where several neighbors did. Recording whichever neighbor the
/// iteration happened to reach first would make the output depend on edge
/// order, which the determinism contract forbids. Instead each Red item gets a
/// rank — the least number of poison steps from an inadmissible seed, itself a
/// least fixpoint and so order-independent — and blames the
/// smallest-by-(edge kind, symbol) neighbor exactly one rank closer to the
/// seed. Ranks strictly decrease along a chain, so a chain terminates even
/// though the item graph has cycles (mutual recursion, self-referential
/// records) and even though a Red cycle can exist.
///
/// THE PROBE UNDER-APPROXIMATES ON PURPOSE. See `probeAdmissibility` below,
/// and the enumerated list of constructs knowingly left Green in the
/// "Deliberate under-approximation" comment before it. The rule the whole
/// analysis rests on is: a false Green costs the search above this one wasted
/// import attempt, while a false Red permanently amputates a subset that no
/// later stage can recover. So every uncertain construct is Green.
///
/// REUSE VERSUS DIVERGENCE. Three things are called, not copied: the symbol
/// naming (`EmitRust/CSymbolNaming.h`, the same functions `CImporter` calls,
/// so a verdict key and an item graph node key are the same string by
/// construction), the clang-driving shell (`EmitRust/ClangProjectParser.h`,
/// so the probe sees exactly the project the importer would), and the item
/// graph itself. What is NOT reused is `CImporter`'s rejection logic: every
/// real rejection is a method on `CImporter` and needs its `OpBuilder`, its
/// live `ModuleOp`, and its accumulated per-TU planning state (`planOwners`,
/// `planCursorParams`, ...) — calling into it would mean building a module,
/// which is exactly what a pre-import analysis must not require. The screens
/// below are therefore a deliberate, documented DIVERGENCE RISK: if
/// `collectRecordFields` ever learns to import base classes, this file will
/// keep calling them Red until it is updated. The risk is bounded in the safe
/// direction only by the under-approximation discipline for constructs the
/// probe stays silent about; for the eleven it does screen, the mitigation is
/// that each one is a by-design rejection with a single unconditional check
/// in the importer (`base classes are not supported`, `virtual method`,
/// `user-declared destructor`, `overloaded operator`, `_Atomic-qualified
/// type`) or a construct the importer has no code for at all (templates,
/// exceptions, inline asm, lambdas, `new`/`delete`). The C++ REFERENCE screen
/// was one of these until FR-48 made references importable in the PARAMETER
/// position; `typeConstructTag` now takes an `inParam` flag and exempts
/// exactly the shapes `mapParamType` accepts, because screening a supported
/// construct is this probe's unsafe direction. A screen that goes stale this
/// way is the standing maintenance cost of the list above.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Project/ItemColoring.h"

#include "EmitRust/ClangProjectParser.h"
#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/Project/ItemGraph.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/ASTUnit.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// Enumerator spellings
//===----------------------------------------------------------------------===//

llvm::StringRef mlir::emitrust::itemColorName(ItemColor color) {
  switch (color) {
  case ItemColor::Green:
    return "green";
  case ItemColor::Yellow:
    return "yellow";
  case ItemColor::Red:
    return "red";
  }
  return "unknown";
}

llvm::StringRef mlir::emitrust::colorReasonName(ColorReason reason) {
  switch (reason) {
  case ColorReason::Admissible:
    return "admissible";
  case ColorReason::Inadmissible:
    return "inadmissible";
  case ColorReason::RedType:
    return "red-type";
  case ColorReason::RedGlobal:
    return "red-global";
  case ColorReason::RedCallee:
    return "red-callee";
  case ColorReason::StubCallee:
    return "stub-callee";
  }
  return "unknown";
}

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

unsigned ItemColoring::countOf(ItemColor color) const {
  unsigned count = 0;
  for (const ColoredItem &item : items)
    if (item.color == color)
      ++count;
  return count;
}

std::string ItemColoring::print() const {
  std::string text;
  llvm::raw_string_ostream os(text);
  for (const ColoredItem &item : items) {
    os << "item " << item.symbol << " kind=" << itemKindName(item.kind)
       << " color=" << itemColorName(item.color)
       << " reason=" << colorReasonName(item.reason);
    // The blame group is present exactly when some OTHER item is at fault;
    // an item that is itself the blocker has no `via`, and prints only its
    // `construct`.
    if (!item.via.empty()) {
      os << " via=" << item.via << " edge=" << edgeKindName(item.viaEdge)
         << " chain=";
      llvm::interleave(item.chain, os, "->");
    }
    if (!item.construct.empty())
      os << " construct=" << item.construct;
    os << "\n";
  }
  // One trailing tally, so a survey over a corpus needs no post-processing and
  // a FileCheck can assert the whole-project answer in one line.
  os << "tally green=" << countOf(ItemColor::Green)
     << " yellow=" << countOf(ItemColor::Yellow)
     << " red=" << countOf(ItemColor::Red) << "\n";
  return text;
}

//===----------------------------------------------------------------------===//
// The verdict table
//===----------------------------------------------------------------------===//

void ItemAdmissibility::reject(llvm::StringRef symbol,
                               llvm::StringRef construct,
                               bool signatureLevel) {
  if (symbol.empty() || construct.empty())
    return;
  AdmissibilityVerdict &verdict = verdicts[symbol.str()];
  verdict.admissible = false;
  // AND over every declaration of the symbol: the importer sees them all, so
  // one signature-level rejection anywhere is enough to make the signature
  // unwritable.
  if (signatureLevel)
    verdict.signatureAdmissible = false;
  // The lexicographically smallest tag wins. The choice of WHICH tag is
  // arbitrary (several can apply at once); the point is that it is a function
  // of the set of tags alone, so it cannot depend on the order declarations
  // were walked in.
  if (verdict.construct.empty() || construct.str() < verdict.construct)
    verdict.construct = construct.str();
}

const AdmissibilityVerdict *
ItemAdmissibility::lookup(llvm::StringRef symbol) const {
  auto it = verdicts.find(symbol.str());
  return it == verdicts.end() ? nullptr : &it->second;
}

namespace {

//===----------------------------------------------------------------------===//
// Construct tags
//===----------------------------------------------------------------------===//

/// The probe's fixed tag vocabulary. Every tag names a construct the importer
/// rejects PERMANENTLY and by design, either through a single unconditional
/// check (the first six) or by having no code for it at all (the rest).
///
/// Tags are lowercase and hyphenated so a whole tag is one FileCheck token,
/// and they are compared as strings when several apply to one item, so the
/// vocabulary must stay stable.
namespace tag {
/// A class template, a function template, or an instantiation of one.
constexpr llvm::StringLiteral Template = "template";
/// A C++ record with at least one direct base class.
constexpr llvm::StringLiteral BaseClass = "base-class";
/// A user-declared `virtual` member function.
constexpr llvm::StringLiteral VirtualMethod = "virtual-method";
/// A user-declared destructor.
constexpr llvm::StringLiteral Destructor = "destructor";
/// A user-declared copy, move, or delegating constructor.
constexpr llvm::StringLiteral CopyMoveConstructor = "copy-move-constructor";
/// An lvalue or rvalue reference anywhere in a type.
constexpr llvm::StringLiteral ReferenceType = "reference-type";
/// An `_Atomic`-qualified type anywhere in a type.
constexpr llvm::StringLiteral AtomicType = "atomic-type";
/// `throw` or `try`/`catch` in a body.
constexpr llvm::StringLiteral Exceptions = "exceptions";
/// A `__asm__` statement in a body.
constexpr llvm::StringLiteral InlineAsm = "inline-asm";
/// A lambda expression in a body.
constexpr llvm::StringLiteral Lambda = "lambda";
/// A `new` or `delete` expression in a body.
constexpr llvm::StringLiteral NewDelete = "new-delete";
} // namespace tag

//===----------------------------------------------------------------------===//
// Syntactic screens
//===----------------------------------------------------------------------===//

/// Calls `visit` on the canonical form of `type` and of every type
/// structurally reachable from it through pointers, references, arrays, and
/// function prototypes.
///
/// This is the same reachability `ItemGraph`'s `collectTypeEdges` walks and
/// the same one `CImporter::mapType` recurses along, and for the same reason:
/// a `struct S **` parameter and a `void (*)(int &)` one both genuinely
/// MENTION their inner types, and the importer's verdict on the inner type is
/// the verdict on the whole. It deliberately does NOT walk a record's fields —
/// that indirection is what the item graph's `Field` edges and this file's
/// type poisoning are for, and following it here would make the probe's
/// per-item verdict depend on other items.
/// FR-48: the referent of a C++ LVALUE reference, or a null QualType. A local
/// twin of `cxxReferentType` in the importer's internal header, which this
/// library deliberately does not include.
clang::QualType cxxReferentTypeForProbe(clang::QualType type) {
  if (const auto *reference = llvm::dyn_cast<clang::LValueReferenceType>(
          type.getCanonicalType().getTypePtr()))
    return reference->getPointeeType();
  return clang::QualType();
}

void forEachStructuralType(clang::QualType type,
                           llvm::function_ref<void(clang::QualType)> visit) {
  llvm::SmallVector<clang::QualType, 8> worklist{type};
  llvm::SmallPtrSet<const clang::Type *, 8> seen;
  while (!worklist.empty()) {
    clang::QualType current = worklist.pop_back_val();
    if (current.isNull())
      continue;
    clang::QualType canonical = current.getCanonicalType();
    if (!seen.insert(canonical.getTypePtr()).second)
      continue;
    visit(canonical);
    const clang::Type *typePtr = canonical.getTypePtr();
    if (const auto *pointer = llvm::dyn_cast<clang::PointerType>(typePtr)) {
      worklist.push_back(pointer->getPointeeType());
      continue;
    }
    if (const auto *reference = llvm::dyn_cast<clang::ReferenceType>(typePtr)) {
      worklist.push_back(reference->getPointeeType());
      continue;
    }
    if (const auto *array = llvm::dyn_cast<clang::ArrayType>(typePtr)) {
      worklist.push_back(array->getElementType());
      continue;
    }
    if (const auto *atomic = llvm::dyn_cast<clang::AtomicType>(typePtr)) {
      worklist.push_back(atomic->getValueType());
      continue;
    }
    if (const auto *proto = llvm::dyn_cast<clang::FunctionProtoType>(typePtr)) {
      worklist.push_back(proto->getReturnType());
      for (clang::QualType param : proto->getParamTypes())
        worklist.push_back(param);
      continue;
    }
    if (const auto *function = llvm::dyn_cast<clang::FunctionType>(typePtr))
      worklist.push_back(function->getReturnType());
  }
}

/// The construct tag `type` earns, or empty when the probe has nothing to say
/// about it.
///
/// Only two type screens exist, and both are unconditional rejections in
/// `CImporter::mapType` with no diverting path around them: a C++ reference
/// and `unsupported: _Atomic-qualified type`. Every other type-shaped
/// rejection the importer has is CONTEXTUAL (a data pointer is fine as a
/// parameter and rejected as a return type; a `void *` is fine as an integer
/// carrier; a `const char **` is fine as a string cursor) and is therefore
/// left Green — see the under-approximation list.
///
/// FR-48 made the REFERENCE screen contextual too, so it takes `inParam`:
/// an lvalue reference in a PARAMETER position now imports (as `&T`/`&mut T`)
/// and must NOT be screened, while a reference return, a reference member and
/// a reference global stay unconditional rejections. Screening a supported
/// construct is the probe's UNSAFE direction — it would color a genuinely
/// portable item Red and drag its callers down with it — so the parameter
/// exemption tracks `mapParamType` exactly, including the two shapes that
/// stay rejected there (`T *&` and `T (&)[N]`).
llvm::StringRef typeConstructTag(clang::QualType type, bool inParam = false) {
  // The parameter exemption applies only to the OUTERMOST type: `void (*)(
  // int &)` mentions a reference that is a parameter of the pointed-to
  // function type, not of the item being probed, and the structural walk
  // cannot tell the two apart once it has descended.
  if (inParam) {
    clang::QualType referent = cxxReferentTypeForProbe(type);
    if (!referent.isNull() && !referent.getCanonicalType()->isArrayType() &&
        !(referent.getCanonicalType()->isPointerType() &&
          !referent.getCanonicalType()->isFunctionPointerType()))
      return typeConstructTag(referent);
  }
  llvm::StringRef found;
  forEachStructuralType(type, [&](clang::QualType current) {
    llvm::StringRef here;
    if (current->isReferenceType())
      here = tag::ReferenceType;
    else if (current->isAtomicType())
      here = tag::AtomicType;
    if (here.empty())
      return;
    // Smallest tag wins, so the answer does not depend on the worklist order.
    if (found.empty() || here < found)
      found = here;
  });
  return found;
}

/// Whether `decl` is, or belongs to, a template — a class or function
/// template, an explicit or implicit instantiation of one, or a member of an
/// instantiated template.
///
/// The screen is deliberately CONSERVATIVE about templates rather than
/// authoritative. W2.15 and W2.16 gave the importer real template
/// machinery — a function or class template's INSTANTIATIONS import, one
/// item each, under the `templateArgSuffix` naming scheme — so an
/// instantiation reaching here is no longer "obviously unimportable". It
/// is still screened out because a template's ADMISSIBILITY is a property
/// of each instantiation, not of the pattern this predicate is asked
/// about, and the probe has no per-instantiation key to record a verdict
/// under; the item graph does (`collectItems` walks `specializations()`),
/// so the instantiations still appear in the index. Screening the pattern
/// out only ever under-approximates: it can leave an item uncolored, never
/// color an inadmissible one green. W2.3's STL recognition, which diverts
/// `std`-namespace records to a hand-written model before any generic
/// import, remains a separate matter — those live in system headers and
/// are never item graph nodes at all.
bool isTemplated(const clang::Decl *decl) {
  if (const auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
    if (record->getDescribedClassTemplate())
      return true;
    if (llvm::isa<clang::ClassTemplateSpecializationDecl>(record))
      return true;
  }
  if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (func->getDescribedFunctionTemplate())
      return true;
    if (func->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate)
      return true;
  }
  return decl->isTemplated();
}

/// Records the body-level construct tags reachable from `stmt` through
/// `note`.
///
/// Body-level, not signature-level: a function whose BODY contains one of
/// these can still be replaced by a stub with its original signature, so the
/// caller stays Yellow. That distinction is the whole reason the probe reports
/// the two levels separately.
void noteBodyConstructs(const clang::Stmt *stmt,
                        llvm::function_ref<void(llvm::StringRef)> note) {
  if (!stmt)
    return;
  if (llvm::isa<clang::CXXThrowExpr>(stmt) ||
      llvm::isa<clang::CXXTryStmt>(stmt))
    note(tag::Exceptions);
  else if (llvm::isa<clang::AsmStmt>(stmt))
    note(tag::InlineAsm);
  else if (llvm::isa<clang::LambdaExpr>(stmt))
    note(tag::Lambda);
  else if (llvm::isa<clang::CXXNewExpr>(stmt) ||
           llvm::isa<clang::CXXDeleteExpr>(stmt))
    note(tag::NewDelete);
  // Bounded by the source's statement nesting; the AST is a tree, so no
  // visited set is needed.
  for (const clang::Stmt *child : stmt->children())
    noteBodyConstructs(child, note);
}

//===----------------------------------------------------------------------===//
// The probe walk
//===----------------------------------------------------------------------===//

/// Whether `decl` comes from a system header. Identical to the item graph's
/// test (and the importer's), and it has to be: the probe's keys must be the
/// graph's node keys, so the two walks must accept exactly the same
/// declarations.
bool isSystemHeaderDecl(const clang::SourceManager &sourceManager,
                        const clang::Decl *decl) {
  clang::SourceLocation loc = sourceManager.getExpansionLoc(decl->getLocation());
  return loc.isValid() && sourceManager.isInSystemHeader(loc);
}

/// Runs the admissibility probe over every translation unit, accumulating
/// into one verdict table. One instance per `probeAdmissibility` call.
///
/// The walk is `ItemGraphBuilder::collectItems`'s, declaration for
/// declaration: same implicit/system-header filtering, same transparent
/// recursion through `extern "C"` and `namespace`, same
/// named-file-scope-records-only rule, same `CSymbolNaming.h` calls. That is
/// not a coincidence to be maintained by discipline — it is the correctness
/// condition for the whole file, since a key that is not a graph node key is a
/// verdict nothing will ever read.
class AdmissibilityProbe {
public:
  /// Probes every unit and returns the verdicts.
  ItemAdmissibility run(llvm::ArrayRef<clang::ASTUnit *> units);

private:
  /// Probes every item declared directly in `context`, recursing through
  /// `extern "C"` and `namespace` bodies.
  void probeDeclsIn(const clang::DeclContext *context);

  /// Probes one function item: its signature types (signature-level) and,
  /// when this declaration is the definition, its body (body-level).
  void probeFunction(const clang::FunctionDecl *func);

  /// Probes one record item: its C++ member shape (bases, destructors,
  /// virtuals, overloaded operators) and its field types.
  void probeRecord(const clang::RecordDecl *record, llvm::StringRef symbol);

  /// Probes one global item: its declared type.
  void probeGlobal(const clang::VarDecl *var);

  /// The item graph's record naming rule, reproduced exactly: only a NAMED,
  /// FILE-SCOPE record is a node, because every other record's emitted name
  /// depends on accumulated import state the graph declines to model.
  std::string recordSymbolFor(const clang::RecordDecl *record) const;

  /// The per-TU mangling tag of the unit being walked, `tu<i>_`.
  std::string tuTag;
  /// The source manager of the unit being walked.
  const clang::SourceManager *sourceManager = nullptr;
  /// The accumulating verdicts.
  ItemAdmissibility verdicts;
};

std::string
AdmissibilityProbe::recordSymbolFor(const clang::RecordDecl *record) const {
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition)
    return {};
  if (!definition->getDeclContext()->getRedeclContext()->isFileContext())
    return {};
  return recordRustName(definition);
}

void AdmissibilityProbe::probeFunction(const clang::FunctionDecl *func) {
  std::string symbol = cFunctionSymbolName(func, tuTag);
  if (symbol.empty())
    return;
  if (isTemplated(func)) {
    // Signature-level: a template's signature is not a signature at all until
    // it is instantiated, so no stub can be written either.
    verdicts.reject(symbol, tag::Template, /*signatureLevel=*/true);
    return;
  }
  // The SIGNATURE is what a stub would have to reproduce, so a rejection in
  // the return type or a parameter type is signature-level and makes the item
  // unstubbable; its callers go Red rather than Yellow.
  llvm::StringRef returnTag = typeConstructTag(func->getReturnType());
  if (!returnTag.empty())
    verdicts.reject(symbol, returnTag, /*signatureLevel=*/true);
  for (const clang::ParmVarDecl *param : func->parameters()) {
    llvm::StringRef paramTag =
        typeConstructTag(param->getType(), /*inParam=*/true);
    if (!paramTag.empty())
      verdicts.reject(symbol, paramTag, /*signatureLevel=*/true);
  }
  // The BODY is not: a function whose body throws still has a writable
  // signature, so FR-42 would stub it and its callers only go Yellow.
  if (func->isThisDeclarationADefinition())
    noteBodyConstructs(func->getBody(), [&](llvm::StringRef bodyTag) {
      verdicts.reject(symbol, bodyTag, /*signatureLevel=*/false);
    });
}

/// W2.18: whether the SINGLE base of `record` is one the importer admits as
/// an ordinary first field named `base`. Mirrors
/// `admitsSingleBaseAsField` in lib/ImportC/ImportCAggregates.cpp, which is
/// the source of truth; that function lives behind the importer's private
/// header, so the rule is restated here rather than shared. The two must
/// move together: a shape the importer admits but this screens is a FALSE
/// RED, the one direction FR-41 may not get wrong.
static bool admitsSingleBaseAsField(const clang::CXXRecordDecl *record) {
  if (record->getNumBases() != 1)
    return false;
  const clang::CXXBaseSpecifier &base = *record->bases_begin();
  if (base.isVirtual() || base.getAccessSpecifier() != clang::AS_public)
    return false;
  const clang::CXXRecordDecl *baseRecord = base.getType()->getAsCXXRecordDecl();
  if (!baseRecord || !baseRecord->hasDefinition())
    return false;
  if (llvm::isa<clang::ClassTemplateSpecializationDecl>(baseRecord))
    return false;
  // A base carrying a destructor is a separate importer rejection
  // (`unsupported: base class with a destructor`), so it is screened here
  // too -- under the base-class tag, because a base class is what makes the
  // shape unrepresentable.
  if (baseRecord->hasUserDeclaredDestructor())
    return false;
  return true;
}

/// W2.17: whether a user-declared destructor is one the importer turns into
/// `impl Drop`. Mirrors the CLASS-LEVEL disqualifiers in
/// `CImporter::collectRecordFields`; the wave's USE-SITE gates (a
/// destructor-carrying member, array, global, by-value parameter or return,
/// or an unmodelled scope) are raised where the OBJECT is declared and have
/// no record-level screen at all, so they are not restated here.
static bool admitsDestructorAsDrop(const clang::CXXRecordDecl *record,
                                   const clang::CXXMethodDecl *destructor) {
  if (destructor->isVirtual() || record->isUnion())
    return false;
  // An uncalled, undefined method is silently dropped from emission, so a
  // body-less destructor would emit no `impl Drop` at all.
  if (!destructor->hasBody())
    return false;
  // The destructor's module symbol is `<Struct>_dtor`; a member function
  // literally spelled `dtor` collides with it.
  for (const clang::CXXMethodDecl *other : record->methods())
    if (!other->isImplicit() && !other->isDeleted() &&
        !llvm::isa<clang::CXXDestructorDecl>(other) &&
        other->getDeclName().isIdentifier() && other->getName() == "dtor")
      return false;
  return true;
}

void AdmissibilityProbe::probeRecord(const clang::RecordDecl *record,
                                     llvm::StringRef symbol) {
  // A record is never stub-replaceable — a rejected record is DROPPED, not
  // stubbed — so the signature-level bit is meaningless for it and every
  // rejection is recorded body-level. See `stubReplaceable` in the fixpoint.
  if (isTemplated(record)) {
    verdicts.reject(symbol, tag::Template, /*signatureLevel=*/false);
    return;
  }
  if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
    // The C++ record rejections `CImporter::collectRecordFields` raises
    // before any field of the class is collected. Every member is checked
    // (rather than stopping at the first) so that the tag kept is the
    // smallest of the set, not the first in declaration order.
    //
    // Both record-level screens below are NARROWER than they were, and
    // deliberately so: screening a construct the importer SUPPORTS is this
    // probe's unsafe direction -- it colors a portable item Red and drags
    // every caller down with it, with no diagnostic and no later stage that
    // could recover it (see test/Project/search-false-red.cpp).
    if (cxxRecord->getNumBases() > 0 && !admitsSingleBaseAsField(cxxRecord))
      verdicts.reject(symbol, tag::BaseClass, /*signatureLevel=*/false);
    for (const clang::CXXMethodDecl *method : cxxRecord->methods()) {
      // Compiler-synthesized special members carry none of these shapes and
      // never surface a diagnostic in the importer either.
      if (method->isImplicit() || method->isDeleted())
        continue;
      if (llvm::isa<clang::CXXDestructorDecl>(method) &&
          !admitsDestructorAsDrop(cxxRecord, method))
        verdicts.reject(symbol, tag::Destructor, /*signatureLevel=*/false);
      // FR-112 kept this screen while REMOVING the overloaded-operator one
      // that used to sit under it: the importer still rejects a virtual
      // method at the class (the vptr is real storage the emitted struct
      // lacks; no use-site rejection can repair a layout), so the screen
      // mirrors `collectRecordFields` exactly. An overloaded operator, by
      // contrast, is now OMITTED member-by-member -- on the struct AND
      // union paths -- with the class importable and every use a located
      // rejection, so screening it was measured as three FALSE REDS on
      // FR-112's motivating repro: the direction that breaks FR-41's
      // "false reds remain zero" contract and starves FR-43's `--search`
      // (test/Project/coloring-cpp-class-gates.cpp pins both halves).
      if (method->isVirtual())
        verdicts.reject(symbol, tag::VirtualMethod, /*signatureLevel=*/false);
      // FR-118: the screen this probe was MISSING, measured as a live FALSE
      // GREEN -- the importer rejects a copy/move/delegating constructor
      // (`CImporter::importCXXMethods`, which is why FR-118 had to undo the
      // struct_def it had already emitted) while this probe called the class
      // `admissible`. Mirrors the importer's predicate EXACTLY, including
      // that a `= default`ed copy constructor is rejected too (the importer
      // gate runs on the broader user-DECLARED shape; see
      // test/Import/Cpp/cpp-defaulted-ctor-invalid.cpp), and that a
      // std-namespace record is exempt because the importer never walks its
      // methods at all.
      //
      // A CONVERSION FUNCTION is deliberately NOT screened: FR-117 OMITS one
      // and keeps the class importable, so screening it would mint a fresh
      // false red -- this probe's unsafe direction.
      if (!cxxRecord->isInStdNamespace())
        if (const auto *ctor =
                llvm::dyn_cast<clang::CXXConstructorDecl>(method))
          if (ctor->isCopyOrMoveConstructor() || ctor->isDelegatingConstructor())
            verdicts.reject(symbol, tag::CopyMoveConstructor,
                            /*signatureLevel=*/false);
    }
  }
  for (const clang::FieldDecl *field : record->fields()) {
    llvm::StringRef fieldTag = typeConstructTag(field->getType());
    if (!fieldTag.empty())
      verdicts.reject(symbol, fieldTag, /*signatureLevel=*/false);
  }
}

void AdmissibilityProbe::probeGlobal(const clang::VarDecl *var) {
  std::string symbol = cGlobalSymbolName(var, tuTag);
  if (symbol.empty())
    return;
  // A global is dropped rather than stubbed too, so body-level.
  llvm::StringRef typeTag = typeConstructTag(var->getType());
  if (!typeTag.empty())
    verdicts.reject(symbol, typeTag, /*signatureLevel=*/false);
}

void AdmissibilityProbe::probeDeclsIn(const clang::DeclContext *context) {
  for (const clang::Decl *decl : context->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(*sourceManager, decl))
      continue;
    if (const auto *linkageSpec =
            llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      probeDeclsIn(linkageSpec);
      continue;
    }
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      probeDeclsIn(ns);
      continue;
    }
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      // C++ member functions are not item graph nodes (they are declared
      // inside a record, and their emitted name depends on the class's
      // assigned struct name), so a verdict on one would key nothing. Their
      // rejectable shapes — virtual, destructor, overloaded operator — are
      // probed as part of the enclosing RECORD instead, which is a node.
      if (llvm::isa<clang::CXXMethodDecl>(func))
        continue;
      // FR-119: a free operator's DeclarationName is not an identifier, so
      // ItemGraph mints no node for it -- a verdict here would key nothing.
      // Screened AST-pure BEFORE probeFunction so `cFunctionSymbolName` is
      // never called on it: the empty-symbol early-return inside
      // probeFunction only saved NDEBUG builds by accident (getName()
      // asserts in debug), which was the latent assert path FR-119 closes.
      if (!func->getDeclName().isIdentifier())
        continue;
      probeFunction(func);
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
      const clang::RecordDecl *definition = record->getDefinition();
      if (!definition)
        continue;
      std::string symbol = recordSymbolFor(definition);
      if (symbol.empty())
        continue;
      probeRecord(definition, symbol);
      continue;
    }
    if (llvm::isa<clang::EnumDecl>(decl)) {
      // Enums are screened as unconditionally admissible. Since FR-113
      // admitted scoped enums, that is NEARLY true: `CImporter::importEnum`
      // still rejects a keyword-named enum or enumerator, a value outside
      // i32, an empty enum, and a cross-TU shape conflict. Those are left
      // GREEN here deliberately — a false GREEN costs one wasted import
      // attempt (the allowed, optimistic direction) — see the
      // under-approximation list.
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      probeGlobal(var);
      continue;
    }
    // Anything else has no key here to record a verdict under. A
    // `ClassTemplateDecl` in particular arrives here (it is not a
    // `RecordDecl`) and is skipped — note this is NO LONGER symmetric with
    // the item graph, which since W2.16 emits one record node per
    // instantiation: the probe under-approximates, leaving those items
    // uncolored rather than coloring them wrongly.
  }
}

ItemAdmissibility
AdmissibilityProbe::run(llvm::ArrayRef<clang::ASTUnit *> units) {
  for (auto [index, unit] : llvm::enumerate(units)) {
    tuTag = ("tu" + llvm::Twine(index) + "_").str();
    sourceManager = &unit->getASTContext().getSourceManager();
    probeDeclsIn(unit->getASTContext().getTranslationUnitDecl());
  }
  return std::move(verdicts);
}

//===----------------------------------------------------------------------===//
// The fixpoint
//===----------------------------------------------------------------------===//

/// One outgoing edge, resolved to a node index.
struct Successor {
  /// The edge kind, which is also the poison channel.
  EdgeKind kind;
  /// The index of the target node in `ItemGraph::nodes`.
  unsigned to;
};

/// Order on successors: edge kind first (its enumerator value, matching the
/// item graph's own edge order, which groups call edges before type edges
/// before data edges), then target symbol. This is the order the blame choice
/// takes its minimum in, and sorting explicitly — rather than relying on
/// `ItemGraph::edges` already being sorted — is what makes the blame chain
/// independent of the edge vector's order, which the determinism test shuffles.
struct SuccessorOrder {
  llvm::ArrayRef<ItemNode> nodes;
  bool operator()(const Successor &lhs, const Successor &rhs) const {
    return std::tie(lhs.kind, nodes[lhs.to].symbol) <
           std::tie(rhs.kind, nodes[rhs.to].symbol);
  }
};

/// The color reason a poisoning edge of kind `kind` produces when the poisoned
/// item ends up Red. `CallsIndirect` never reaches here (it has no target and
/// is dropped when successors are built).
ColorReason redReasonFor(EdgeKind kind) {
  switch (kind) {
  case EdgeKind::Calls:
  case EdgeKind::TakesAddressOf:
  case EdgeKind::CallsIndirect:
    return ColorReason::RedCallee;
  case EdgeKind::SigType:
  case EdgeKind::BodyType:
  case EdgeKind::Field:
  case EdgeKind::Base:
  case EdgeKind::FieldIndirect:
    return ColorReason::RedType;
  case EdgeKind::ReadsGlobal:
  case EdgeKind::WritesGlobal:
  case EdgeKind::AddressOfGlobal:
    // Never the blame minimum in practice: every AddressOfGlobal edge is
    // accompanied by a ReadsGlobal edge for the same pair (FR-62), and
    // ReadsGlobal's smaller enumerator value wins the successor-order
    // minimum. Handled here so the reason is right by construction, not by
    // that pairing.
    return ColorReason::RedGlobal;
  }
  return ColorReason::RedType;
}

/// Whether an edge of `kind` carries poison AT ALL.
///
/// Every kind does except `FieldIndirect`, which is the one dependency the
/// emitted Rust does not SPELL: the importer's pointer-struct-member models
/// erase a `struct S *p` member to an integer or an index, so a record whose
/// only route to a Red `S` is through such a member emits completely and
/// compiles. Calling it Red would be a false Red, and a false Red is the one
/// error this analysis is not allowed to make — see ItemColoring.h's
/// asymmetry argument. It is excluded from the YELLOW rule for the same
/// reason and one more: Yellow means "emits, but calls a stub", and a record
/// calls nothing.
bool poisonBearing(EdgeKind kind) { return kind != EdgeKind::FieldIndirect; }

/// The rank sentinel: "not reached from any inadmissible seed". Every Red item
/// ends up with a finite rank, because Red membership is only ever derived
/// from a seed through a finite chain of poisoning edges.
constexpr unsigned kNoRank = std::numeric_limits<unsigned>::max();

/// The whole fixpoint, as a value: the graph, the seeds, and the derived sets.
class ColoringSolver {
public:
  ColoringSolver(const ItemGraph &graph, const ItemAdmissibility &probe)
      : graph(graph) {
    build(probe);
  }

  /// Runs the color fixpoint, the rank fixpoint, and the blame walk, and
  /// returns the finished coloring.
  ItemColoring solve();

private:
  /// Indexes the nodes, resolves the edges to successor lists, and seeds
  /// `red`/`signatureBroken` from the probe.
  void build(const ItemAdmissibility &probe);

  /// Grows `red` and `signatureBroken` to their least fixpoint.
  void runColorFixpoint();

  /// Computes `rank` for every Red node: the least number of poison steps
  /// from an inadmissible seed.
  void runRankFixpoint();

  /// Whether a Red `index` can be replaced by a signature-preserving stub, so
  /// that its dependents are only demoted to Yellow.
  ///
  /// Exactly FR-42's rule, and only functions can satisfy it: a rejected
  /// record, enum, or global is DROPPED from the module, leaving nothing for a
  /// dependent to compile against, while a rejected function whose signature
  /// still maps becomes an `unimplemented!()` stub that every call site still
  /// type-checks against. A function whose signature is itself broken — the
  /// probe found a reference parameter, or a `SigType` edge reaches a Red
  /// record — has no writable stub and is therefore not stub-replaceable
  /// either.
  bool stubReplaceable(unsigned index) const {
    return graph.nodes[index].kind == ItemKind::Function &&
           !signatureBroken[index];
  }

  /// Whether an edge to `index` poisons its source RED (as opposed to only
  /// Yellow). True exactly when the target is Red and cannot be stubbed.
  bool poisonsRed(unsigned index) const {
    return red[index] && !stubReplaceable(index);
  }

  /// The same question for a whole EDGE, which is the form every rule below
  /// asks it in: a `FieldIndirect` edge carries no poison whatever its target
  /// is, because the emitted Rust never names that target.
  bool poisonsRed(const Successor &successor) const {
    return poisonBearing(successor.kind) && poisonsRed(successor.to);
  }

  /// Fills in `item`'s blame fields by walking one step to the
  /// smallest-by-(kind, symbol) neighbor that is one rank closer to a seed,
  /// then splicing that neighbor's already-computed chain. `hardOnly` selects
  /// the Red walk (poisoning neighbors only) over the Yellow one (any Red
  /// neighbor, all of which are stub-replaceable).
  void blame(unsigned index, bool hardOnly, ItemColoring &coloring);

  const ItemGraph &graph;
  /// Node index by symbol. Only ever read through, so its own order is
  /// irrelevant; `std::map` keeps it obviously deterministic anyway.
  std::map<std::string, unsigned> indexOf;
  /// Outgoing edges per node, sorted by `SuccessorOrder`.
  std::vector<std::vector<Successor>> successors;
  /// The probe's construct tag per node, empty when it found nothing.
  std::vector<std::string> construct;
  /// Whether the probe rejected the node itself.
  std::vector<bool> inadmissible;
  /// The Red set, grown to a least fixpoint.
  std::vector<bool> red;
  /// The unwritable-signature set, grown to a least fixpoint alongside `red`.
  std::vector<bool> signatureBroken;
  /// Poison distance from the nearest inadmissible seed, `kNoRank` when not
  /// Red.
  std::vector<unsigned> rank;
};

void ColoringSolver::build(const ItemAdmissibility &probe) {
  unsigned count = static_cast<unsigned>(graph.nodes.size());
  for (unsigned index = 0; index != count; ++index)
    indexOf[graph.nodes[index].symbol] = index;

  successors.resize(count);
  for (const ItemEdge &edge : graph.edges) {
    // A `CallsIndirect` edge has no target by design. It is dropped here
    // rather than treated as a poison source: "this item makes an indirect
    // call" says nothing about whether the item is emittable, and the item
    // graph is closed, so every other edge resolves.
    if (edge.to.empty())
      continue;
    auto from = indexOf.find(edge.from);
    auto to = indexOf.find(edge.to);
    if (from == indexOf.end() || to == indexOf.end())
      continue;
    successors[from->second].push_back({edge.kind, to->second});
  }
  for (std::vector<Successor> &list : successors)
    llvm::sort(list, SuccessorOrder{graph.nodes});

  construct.assign(count, std::string());
  inadmissible.assign(count, false);
  red.assign(count, false);
  signatureBroken.assign(count, false);
  rank.assign(count, kNoRank);
  for (unsigned index = 0; index != count; ++index) {
    const AdmissibilityVerdict *verdict =
        probe.lookup(graph.nodes[index].symbol);
    // A node the probe has no verdict for is admissible: absence means Green,
    // which is the under-approximating default in the safe direction.
    if (!verdict || verdict->admissible)
      continue;
    inadmissible[index] = true;
    red[index] = true;
    rank[index] = 0;
    construct[index] = verdict->construct;
    if (!verdict->signatureAdmissible)
      signatureBroken[index] = true;
  }
}

void ColoringSolver::runColorFixpoint() {
  // A least fixpoint of two monotone rules, iterated to saturation. Both sets
  // only ever grow and are bounded by the node count, so this terminates; and
  // because it runs to saturation the RESULT is the same whatever order the
  // nodes and edges are visited in, which is the determinism guarantee the
  // search above this depends on.
  bool changed = true;
  while (changed) {
    changed = false;
    for (unsigned index = 0, count = graph.nodes.size(); index != count;
         ++index) {
      for (const Successor &successor : successors[index]) {
        // Rule 1: an edge into a Red type/global/unstubbable function makes
        // the source Red. This is the single rule the header's asymmetry is
        // stated as; `BodyType`, `SigType`, `Field`, `Base`, `ReadsGlobal`,
        // `WritesGlobal`, and a call to an unstubbable function are all just
        // instances of it.
        if (!red[index] && poisonsRed(successor)) {
          red[index] = true;
          changed = true;
        }
        // Rule 2: a signature that names a Red type cannot be written, so not
        // even a stub survives. This is what makes a `SigType` dependency
        // poison the item's CALLERS Red while a `BodyType` one leaves them
        // Yellow.
        if (successor.kind == EdgeKind::SigType && red[successor.to] &&
            !signatureBroken[index]) {
          signatureBroken[index] = true;
          changed = true;
        }
      }
    }
  }
}

void ColoringSolver::runRankFixpoint() {
  // Least fixpoint again, so the ranks — and therefore the blame chains — are
  // a function of the graph's content alone.
  bool changed = true;
  while (changed) {
    changed = false;
    for (unsigned index = 0, count = graph.nodes.size(); index != count;
         ++index) {
      if (!red[index])
        continue;
      unsigned best = rank[index];
      for (const Successor &successor : successors[index]) {
        if (!poisonsRed(successor) || rank[successor.to] == kNoRank)
          continue;
        best = std::min(best, rank[successor.to] + 1);
      }
      if (best < rank[index]) {
        rank[index] = best;
        changed = true;
      }
    }
  }
}

void ColoringSolver::blame(unsigned index, bool hardOnly,
                           ItemColoring &coloring) {
  ColoredItem &item = coloring.items[index];
  const Successor *chosen = nullptr;
  for (const Successor &successor : successors[index]) {
    if (hardOnly) {
      // The Red walk: only a neighbor that is strictly closer to a seed, so
      // the chain cannot loop back through a Red cycle.
      if (!poisonsRed(successor) || rank[successor.to] != rank[index] - 1)
        continue;
    } else {
      // The Yellow walk: any Red neighbor reached along a poison-bearing
      // edge. Every such neighbor is necessarily stub-replaceable — an
      // unstubbable one would have made this item Red — and therefore a
      // function reached along `Calls`/`TakesAddressOf`.
      if (!poisonBearing(successor.kind) || !red[successor.to])
        continue;
    }
    // `successors` is sorted by (kind, symbol), so the first match IS the
    // minimum.
    chosen = &successor;
    break;
  }
  if (!chosen)
    return; // Defensive: a Red non-seed always has a poisoning neighbor.
  const ColoredItem &target = coloring.items[chosen->to];
  item.via = target.symbol;
  item.viaEdge = chosen->kind;
  item.reason = hardOnly ? redReasonFor(chosen->kind) : ColorReason::StubCallee;
  // The neighbor's chain is already final: for the Red walk it has a strictly
  // smaller rank and was visited first; for the Yellow walk it is Red and
  // every Red chain is computed before any Yellow one.
  item.chain.push_back(item.symbol);
  item.chain.insert(item.chain.end(), target.chain.begin(), target.chain.end());
  item.construct = target.construct;
}

ItemColoring ColoringSolver::solve() {
  runColorFixpoint();
  runRankFixpoint();

  unsigned count = static_cast<unsigned>(graph.nodes.size());
  ItemColoring coloring;
  coloring.items.reserve(count);
  for (const ItemNode &node : graph.nodes)
    coloring.items.push_back(
        {node.symbol, node.kind, ItemColor::Green, ColorReason::Admissible});

  // Red first, in rank order, so a chain is always spliced onto an
  // already-final one; then Yellow, whose first step lands on a Red item.
  llvm::SmallVector<unsigned> redOrder;
  for (unsigned index = 0; index != count; ++index)
    if (red[index])
      redOrder.push_back(index);
  // Ties inside a rank are broken by symbol, and never actually matter (two
  // items of the same rank cannot blame each other, since blame strictly
  // decreases rank); the sort is here so the traversal itself is total.
  llvm::sort(redOrder, [&](unsigned lhs, unsigned rhs) {
    return std::tie(rank[lhs], graph.nodes[lhs].symbol) <
           std::tie(rank[rhs], graph.nodes[rhs].symbol);
  });
  for (unsigned index : redOrder) {
    ColoredItem &item = coloring.items[index];
    item.color = ItemColor::Red;
    if (inadmissible[index]) {
      // The item is itself the blocker: it has no `via`, and its chain is the
      // one-element path every longer chain terminates in.
      item.reason = ColorReason::Inadmissible;
      item.construct = construct[index];
      item.chain.push_back(item.symbol);
      continue;
    }
    blame(index, /*hardOnly=*/true, coloring);
  }

  for (unsigned index = 0; index != count; ++index) {
    if (red[index])
      continue;
    // Yellow is exactly "not Red, but depends on something Red". Given the
    // fixpoint has saturated, every Red neighbor of a non-Red item is
    // stub-replaceable, so this is precisely the stub-call case.
    bool touchesRed = llvm::any_of(successors[index], [&](const Successor &s) {
      return poisonBearing(s.kind) && red[s.to];
    });
    if (!touchesRed)
      continue;
    coloring.items[index].color = ItemColor::Yellow;
    blame(index, /*hardOnly=*/false, coloring);
  }

  // The published order is by symbol, established HERE rather than inherited
  // from `graph.nodes`. Inheriting it would be a no-op today — the graph
  // publishes its nodes sorted — but it would make this function's output
  // depend on its input's ORDER as well as its content, and the whole point of
  // the functional core is that it does not: shuffle `graph.nodes` and
  // `graph.edges` and the printed coloring is byte-identical. (Successor lists
  // are sorted for the same reason, in `build`.)
  llvm::stable_sort(coloring.items,
                    [](const ColoredItem &lhs, const ColoredItem &rhs) {
                      return lhs.symbol < rhs.symbol;
                    });
  return coloring;
}

} // namespace

//===----------------------------------------------------------------------===//
// Entry points
//===----------------------------------------------------------------------===//

/// Deliberate under-approximation: constructs KNOWINGLY left Green.
///
/// Each of these is rejected by the importer in at least some circumstances,
/// and is nonetheless left admissible here, because the rejection is
/// CONTEXTUAL — the same syntax imports fine elsewhere — and a syntactic
/// screen cannot tell the cases apart without reproducing the importer's
/// flow-sensitive analyses. A false Green costs FR-43's search one wasted
/// import attempt; a false Red would amputate a translatable subset forever.
/// This list is therefore FR-43's work list, not a set of oversights:
///
///  - POINTER-TO-POINTER parameters. Rejected as `pointer-to-pointer
///    parameter` in general, but `const char **` string cursors (CTS 00204)
///    and `main`'s `char **argv` both import today, so the syntax alone
///    decides nothing.
///  - `void *` parameters. Rejected as `void pointer parameter` unless the
///    planner classifies the parameter as an integer CARRIER (CTS-P3), which
///    is a body-usage fact.
///  - POINTER RETURN TYPES. Rejected as `pointer return type` unless the
///    owner-index-return model (FR-36) applies, which depends on every return
///    site in the body.
///  - POINTER STRUCT MEMBERS and pointer GLOBALS. A large family of
///    flow-sensitive rejections (`pointer struct member '...'`,
///    `global pointer bound to ...`) sits behind these, and an equally large
///    family of supported shapes (FR-35/37/38/39).
///  - VARIADIC functions. Rejected in general, but monomorphized when the call
///    sites permit (`planVaMonomorph`).
///  - `volatile`-qualified types. Rejected by `mapType`, but a top-level
///    `volatile` on a parameter object is stripped and accepted, and an array
///    parameter's bracket qualifiers adjust onto the decayed pointer.
///  - ENUM DEFINITION GATES. Scoped enums are ADMITTED since FR-113 (they
///    import as their underlying-typed C-like image), and `importEnum`'s
///    residual gates — keyword-named enums/enumerators, values outside i32,
///    an empty enum, a cross-TU shape conflict — are left GREEN: rare in
///    practice, cheap to discover by an import attempt, and the shape
///    conflict is a cross-TU property a per-item probe cannot see anyway.
///  - `dynamic_cast`, `typeid`, RTTI generally. No importer code handles them,
///    but they are also unreachable without the base classes and virtual
///    methods that are already screened, so a tag would never fire alone.
///  - EVERY BODY-LEVEL C REJECTION: unsupported statements and expressions,
///    the allocation model's rejections, `realloc`, `FILE*` outside a local,
///    `setjmp`, and so on. These are the bulk of real rejections and are
///    exactly what an import attempt is for.
///  - CROSS-ITEM WHOLE-PROGRAM rejections: a symbol name colliding after the
///    Rust-keyword mangle, two definitions of one external symbol, a record
///    name reused with a different shape. These are properties of a SET of
///    items, not of one, so they have no place in a per-item probe.
FailureOr<ItemAdmissibility>
mlir::emitrust::probeAdmissibility(llvm::ArrayRef<clang::ASTUnit *> units) {
  for (const clang::ASTUnit *unit : units)
    if (!unit)
      return failure();
  AdmissibilityProbe probe;
  return probe.run(units);
}

ItemColoring mlir::emitrust::computeColoring(const ItemGraph &graph,
                                             const ItemAdmissibility &probe) {
  ColoringSolver solver(graph, probe);
  return solver.solve();
}

FailureOr<ItemColoring>
mlir::emitrust::colorItems(llvm::ArrayRef<clang::ASTUnit *> units) {
  FailureOr<ItemGraph> graph = buildItemGraph(units);
  if (failed(graph))
    return failure();
  FailureOr<ItemAdmissibility> probe = probeAdmissibility(units);
  if (failed(probe))
    return failure();
  return computeColoring(*graph, *probe);
}

FailureOr<ItemColoring>
mlir::emitrust::colorItems(llvm::ArrayRef<std::string> paths,
                           llvm::ArrayRef<std::string> extraClangArgs) {
  std::string error;
  return colorItems(paths, extraClangArgs, /*compilationDatabasePath=*/"",
                    error);
}

FailureOr<ItemColoring>
mlir::emitrust::colorItems(llvm::ArrayRef<std::string> paths,
                           llvm::ArrayRef<std::string> extraClangArgs,
                           llvm::StringRef compilationDatabasePath,
                           std::string &error) {
  // The ASTs are parsed ONCE and both the graph and the probe are computed
  // over them: parsing twice would double the cost of the analysis and, worse,
  // would leave open the possibility of the two halves seeing different ASTs.
  std::vector<std::unique_ptr<clang::ASTUnit>> owned;
  std::vector<std::string> sources;
  // FR-68: the attribution is unused here — a driver-level error still fails
  // the build through the hardened status below, and clang has already
  // printed the diagnostic itself; only the importer entry points relocate
  // it onto the offending TU.
  ProjectParseError firstClangError;
  int status =
      buildProjectASTs(paths, extraClangArgs, compilationDatabasePath, owned,
                       sources, error, firstClangError);
  if (!error.empty())
    return failure();
  if (owned.size() != sources.size() || status != 0)
    return failure();
  llvm::SmallVector<clang::ASTUnit *, 4> units;
  for (const std::unique_ptr<clang::ASTUnit> &unit : owned) {
    if (!unit || unit->getDiagnostics().hasErrorOccurred())
      return failure();
    units.push_back(unit.get());
  }
  return colorItems(llvm::ArrayRef<clang::ASTUnit *>(units));
}
