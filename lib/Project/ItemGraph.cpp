//===- ItemGraph.cpp - whole-project program item graph -------------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements FR-40's `buildItemGraph` (see EmitRust/Project/ItemGraph.h for
/// the contract, the node/edge model, and the print format).
///
/// Shape of the implementation, and why.
///
/// TWO PASSES, NOT ONE. Pass 1 (`ItemGraphBuilder::collectItems`) walks
/// every TU's declarations and records the NODES; pass 2
/// (`collectDependencies`) walks the same declarations again and records the
/// EDGES. They are separate because the graph is closed — an edge is
/// emitted only when its target is a node — and a single pass would have to
/// either buffer every candidate edge or accept forward references it
/// cannot yet validate. Two cheap AST walks are simpler than either, and the
/// walk is the cheap part next to parsing.
///
/// BOTH PASSES REUSE `importDeclsIn`'s WALK SHAPE. Skipping implicit decls,
/// skipping system-header decls (`isSystemHeaderDecl`, reproduced here as
/// the free `isSystemHeaderDecl` since it needs nothing but a
/// `SourceManager`), and recursing transparently through `LinkageSpecDecl`
/// (`extern "C" { ... }`) and `NamespaceDecl` is not an approximation of
/// what the importer does — it is the same rule, and it has to be, or the
/// graph would contain items the importer never emits (all of libc, via the
/// system headers) or miss ones it does (everything inside a namespace).
///
/// DETERMINISM BY CONSTRUCTION, NOT BY DISCIPLINE. Nodes accumulate into a
/// `std::map<std::string, ItemNode>` and edges into a `std::set<ItemEdge>`
/// with an explicit content-only comparator, so the containers are ALREADY
/// in the published order and the boundary conversion is a straight copy.
/// The alternative — `DenseMap` plus a sort at the end — was rejected: it
/// leaves a live window in which unordered data exists and any future
/// early-out or streaming change could leak it. Nothing here is hot enough
/// for the `std::map` node cost to matter (it is one entry per program
/// item, built once).
///
/// NAMING IS CALLED, NOT COPIED. Function and global symbols come from
/// `cFunctionSymbolName`/`cGlobalSymbolName` — literally the functions
/// `CImporter::mlirFuncName`/`globalVarSymbolName` are wrappers over — and
/// record/enum symbols from `recordRustName`. The one place the graph
/// cannot follow the importer is the part of record naming that depends on
/// accumulated import state (`CImporter::structSymbolName`'s tag-versus-
/// ordinary-identifier rename, the `Anon<n>` shape keying, the block-scope
/// `<function>_<tag>` mangle); rather than mirror that logic and let it
/// drift, records and enums whose emitted name would depend on it are NOT
/// nodes at all (see `recordSymbolFor`). A missing node is a visible,
/// honest gap; a wrong node key would silently corrupt every consumer.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Project/ItemGraph.h"

#include "EmitRust/ClangProjectParser.h"
#include "EmitRust/CSymbolNaming.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/ASTUnit.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// Enumerator spellings
//===----------------------------------------------------------------------===//

llvm::StringRef mlir::emitrust::itemKindName(ItemKind kind) {
  switch (kind) {
  case ItemKind::Function:
    return "function";
  case ItemKind::Record:
    return "record";
  case ItemKind::Enum:
    return "enum";
  case ItemKind::Global:
    return "global";
  }
  return "unknown";
}

llvm::StringRef mlir::emitrust::itemLinkageName(ItemLinkage linkage) {
  return linkage == ItemLinkage::Internal ? "intern" : "extern";
}

llvm::StringRef mlir::emitrust::edgeKindName(EdgeKind kind) {
  switch (kind) {
  case EdgeKind::Calls:
    return "Calls";
  case EdgeKind::CallsIndirect:
    return "CallsIndirect";
  case EdgeKind::SigType:
    return "SigType";
  case EdgeKind::BodyType:
    return "BodyType";
  case EdgeKind::Field:
    return "Field";
  case EdgeKind::Base:
    return "Base";
  case EdgeKind::ReadsGlobal:
    return "ReadsGlobal";
  case EdgeKind::WritesGlobal:
    return "WritesGlobal";
  case EdgeKind::TakesAddressOf:
    return "TakesAddressOf";
  case EdgeKind::FieldIndirect:
    return "FieldIndirect";
  }
  return "Unknown";
}

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

std::string ItemGraph::print() const {
  std::string text;
  llvm::raw_string_ostream os(text);
  // Nodes first, then edges: a consumer streaming the output can populate
  // its node table before it has to resolve any edge endpoint, and a human
  // reading a diff sees item churn separately from dependency churn.
  for (const ItemNode &node : nodes)
    os << "node " << node.symbol << " kind=" << itemKindName(node.kind)
       << " def=" << (node.isDefinition ? "1" : "0")
       << " linkage=" << itemLinkageName(node.linkage) << " tu=" << node.tuIndex
       << " loc=" << node.file << ":" << node.line << ":" << node.column
       << "\n";
  for (const ItemEdge &edge : edges)
    os << "edge " << edge.from << " -> " << (edge.to.empty() ? "?" : edge.to)
       << " kind=" << edgeKindName(edge.kind) << "\n";
  return text;
}

namespace {

//===----------------------------------------------------------------------===//
// Ordering
//===----------------------------------------------------------------------===//

/// Total order on nodes: symbol, then translation unit. `symbol` alone is
/// already a key (nodes are merged by symbol), so the second component
/// never decides; it is in the comparator because the published contract
/// names it, and because the order must stay total if the key ever widens.
struct NodeOrder {
  bool operator()(const ItemNode &lhs, const ItemNode &rhs) const {
    return std::tie(lhs.symbol, lhs.tuIndex) <
           std::tie(rhs.symbol, rhs.tuIndex);
  }
};

/// Total order on edges: source symbol, then edge kind, then target symbol.
/// Kind before target so that all of one item's `Calls` edges are
/// contiguous — the shape a call-graph consumer and a FileCheck block both
/// want. The kind is ordered by its enumerator VALUE rather than its
/// spelling: the value is the declaration order in `EdgeKind`, which groups
/// the call edges, then the type edges, then the data edges, whereas an
/// alphabetical order would interleave them meaninglessly.
struct EdgeOrder {
  bool operator()(const ItemEdge &lhs, const ItemEdge &rhs) const {
    return std::tie(lhs.from, lhs.kind, lhs.to) <
           std::tie(rhs.from, rhs.kind, rhs.to);
  }
};

//===----------------------------------------------------------------------===//
// Decl filtering, shared with the importer's walk
//===----------------------------------------------------------------------===//

/// Whether `decl` comes from a system header (angle-bracket include,
/// `-isystem`). The importer skips these wholesale
/// (`CImporter::isSystemHeaderDecl`, and the identical test in
/// `importDeclsIn`) because real libc headers are full of constructs
/// outside the supported subset; the graph must skip them for the same
/// reason plus a stronger one — a graph containing every declaration of
/// `<stdio.h>` would drown the project's own items.
bool isSystemHeaderDecl(const clang::SourceManager &sourceManager,
                        const clang::Decl *decl) {
  clang::SourceLocation loc = sourceManager.getExpansionLoc(decl->getLocation());
  return loc.isValid() && sourceManager.isInSystemHeader(loc);
}

/// The C linkage of a function or variable, in the graph's two-valued
/// model. `isExternallyVisible` is clang's own answer and already accounts
/// for `static`, anonymous namespaces, and const-at-namespace-scope in C++.
ItemLinkage linkageOf(const clang::NamedDecl *decl) {
  return decl->isExternallyVisible() ? ItemLinkage::External
                                     : ItemLinkage::Internal;
}

//===----------------------------------------------------------------------===//
// The builder
//===----------------------------------------------------------------------===//

/// Accumulates the graph over every translation unit. One instance per
/// `buildItemGraph` call; holds no state that outlives it.
class ItemGraphBuilder {
public:
  /// Runs both passes over `units` and returns the finished, sorted graph.
  /// `units[i]` is translation unit `i`.
  ItemGraph build(llvm::ArrayRef<clang::ASTUnit *> units);

private:
  //===--------------------------------------------------------------------===//
  // Pass 1: nodes
  //===--------------------------------------------------------------------===//

  /// Records every item declared directly in `context`, recursing through
  /// `extern "C"` and `namespace` bodies exactly as `importDeclsIn` does.
  void collectItems(const clang::DeclContext *context);

  /// Merges `node` into the node table under its symbol.
  ///
  /// The merge rule encodes the importer's linkage model: an external
  /// symbol is ONE program-wide item however many TUs declare it, so a
  /// prototype in a shared header and the definition in one TU collapse
  /// into a single node attributed to the DEFINING TU and located at the
  /// definition. Internal symbols never collide in the first place, their
  /// key already carrying the `tu<i>_` tag. Two definitions of one external
  /// symbol is a malformed program the importer rejects; here the lower TU
  /// index wins, deterministically, rather than the walk order.
  void addNode(ItemNode node);

  //===--------------------------------------------------------------------===//
  // Pass 2: edges
  //===--------------------------------------------------------------------===//

  /// Records every dependency of every item declared directly in `context`,
  /// with the same transparent recursion as `collectItems`.
  void collectDependencies(const clang::DeclContext *context);

  /// Records the dependencies of one function item: its signature types,
  /// and, when this declaration is the definition, everything its body
  /// mentions.
  void collectFunctionDependencies(const clang::FunctionDecl *func,
                                   llvm::StringRef from);

  /// Records the dependencies of one record item: its field types and its
  /// C++ direct base classes.
  void collectRecordDependencies(const clang::RecordDecl *record,
                                 llvm::StringRef from);

  /// Walks `stmt` (a function body, or a global's initializer) recording
  /// the call, type, global-access, and address-taken edges it induces.
  /// `writtenGlobals` and `readGlobals` are threaded rather than derived
  /// locally because an assignment's read/write split is decided at the
  /// ASSIGNMENT node while the `DeclRefExpr` that names the global is
  /// several nodes below it.
  void collectStmtDependencies(const clang::Stmt *stmt, llvm::StringRef from);

  /// Records the global accesses `expr` performs when evaluated in a
  /// context that `writes` and/or `reads` the object it designates.
  ///
  /// This is the lvalue side of the walk, kept separate from
  /// `collectStmtDependencies` because C's read/write distinction is
  /// positional: `g = 1` writes `g`, `g += 1` both writes and reads it,
  /// `f(g)` reads it, and `g.field` / `g[i]` propagate whichever context
  /// they sit in down to `g` while the SUBSCRIPT expression itself is an
  /// ordinary read. Anything that is not a designator chain rooted at a
  /// global is handed back to the ordinary walk.
  void collectLValueDependencies(const clang::Expr *expr, llvm::StringRef from,
                                 bool reads, bool writes);

  //===--------------------------------------------------------------------===//
  // Symbols and edges
  //===--------------------------------------------------------------------===//

  /// The emitted symbol of `record`, or empty when the graph deliberately
  /// declines to name it: an anonymous record (the importer would assign a
  /// shape-keyed `Anon<n>`) or a non-file-scope one (the importer would
  /// apply the `<function>_<tag>` block-scope mangle). Both namings depend
  /// on accumulated import state the graph does not carry — see this file's
  /// header comment for why an omitted node beats a guessed one.
  std::string recordSymbolFor(const clang::RecordDecl *record) const;

  /// The emitted symbol of `enumDecl`, or empty for an anonymous enum
  /// (whose enumerators the importer materializes as plain constants, with
  /// no enum item at all).
  std::string enumSymbolFor(const clang::EnumDecl *enumDecl) const;

  /// Adds an edge, dropping it when the target is not a node of this graph
  /// (the closure invariant; `CallsIndirect`, whose target is empty by
  /// design, is exempt). Duplicates collapse in the underlying set.
  void addEdge(llvm::StringRef from, llvm::StringRef to, EdgeKind kind);

  /// Records `SigType`/`BodyType`/`Field` edges from `from` to every named
  /// record and enum `type` mentions.
  ///
  /// "Mentions" walks THROUGH pointers, references, arrays, and function
  /// prototypes (a `struct S **` parameter depends on `struct S`, and so
  /// does a `void (*)(struct S)` one), and through typedefs by way of the
  /// canonical type, but never through a record's own fields — that
  /// indirection is what `Field` edges are for, and following it here would
  /// turn every signature edge into the transitive closure.
  ///
  /// `EdgeKind::Field` is the one kind that SPLITS on how the target was
  /// reached: an occurrence that only ever sits behind a data pointer is
  /// recorded as `FieldIndirect` instead, because the importer's
  /// pointer-struct-member models erase such a member and the emitted Rust
  /// never spells the target's name (see the `FieldIndirect` comment in
  /// ItemGraph.h for the measurement). Every other kind is recorded as given:
  /// a `struct S *` PARAMETER really does emit as `&mut S`, so a signature
  /// edge names its target however many pointers are in the way.
  void collectTypeEdges(clang::QualType type, llvm::StringRef from,
                        EdgeKind kind);

  /// The translation unit currently being walked.
  unsigned tuIndex = 0;
  /// The per-TU mangling tag of the unit currently being walked, `tu<i>_`.
  std::string tuTag;
  /// The source manager of the unit currently being walked.
  const clang::SourceManager *sourceManager = nullptr;

  /// Items by symbol; ordered by construction, hence deterministic.
  std::map<std::string, ItemNode> nodes;
  /// Dependencies, deduplicated and ordered by construction.
  std::set<ItemEdge, EdgeOrder> edges;
};

//===----------------------------------------------------------------------===//
// Pass 1: nodes
//===----------------------------------------------------------------------===//

void ItemGraphBuilder::addNode(ItemNode node) {
  auto [it, inserted] = nodes.emplace(node.symbol, node);
  if (inserted)
    return;
  ItemNode &existing = it->second;
  // A definition always outranks a mere declaration, and carries the
  // location worth reporting.
  if (node.isDefinition && !existing.isDefinition) {
    existing = std::move(node);
    return;
  }
  if (!node.isDefinition && existing.isDefinition)
    return;
  // Same rank: the lowest TU index wins, so the answer does not depend on
  // the order the units happen to be walked in.
  if (node.tuIndex < existing.tuIndex)
    existing = std::move(node);
}

void ItemGraphBuilder::collectItems(const clang::DeclContext *context) {
  for (const clang::Decl *decl : context->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(*sourceManager, decl))
      continue;
    if (const auto *linkageSpec =
            llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      collectItems(linkageSpec);
      continue;
    }
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      collectItems(ns);
      continue;
    }

    clang::PresumedLoc presumed =
        sourceManager->getPresumedLoc(decl->getLocation());
    std::string file = presumed.isValid() ? presumed.getFilename() : "";
    unsigned line = presumed.isValid() ? presumed.getLine() : 0;
    unsigned column = presumed.isValid() ? presumed.getColumn() : 0;

    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      // C++ member functions are not items: they are declared inside a
      // record, so `importDeclsIn` never reaches them, and their emitted
      // name comes from `CImporter::cxxMethodMangledName`, which needs the
      // class's assigned struct name. An OUT-OF-LINE method definition does
      // appear here at item scope, and is skipped for the same reason.
      if (llvm::isa<clang::CXXMethodDecl>(func))
        continue;
      addNode({cFunctionSymbolName(func, tuTag), ItemKind::Function,
               func->isThisDeclarationADefinition(), linkageOf(func), tuIndex,
               std::move(file), line, column});
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
      // Only a complete definition is an item; a forward declaration emits
      // nothing on its own.
      const clang::RecordDecl *definition = record->getDefinition();
      if (!definition)
        continue;
      std::string symbol = recordSymbolFor(definition);
      if (symbol.empty())
        continue;
      clang::PresumedLoc defLoc =
          sourceManager->getPresumedLoc(definition->getLocation());
      // A record has no linkage of its own: the importer emits one struct
      // per NAME program-wide (aggregate definitions shared through a header
      // are deduplicated by symbol name), which is exactly external-linkage
      // behavior, so that is what the node reports.
      addNode({std::move(symbol), ItemKind::Record, /*isDefinition=*/true,
               ItemLinkage::External, tuIndex,
               defLoc.isValid() ? defLoc.getFilename() : "",
               defLoc.isValid() ? defLoc.getLine() : 0,
               defLoc.isValid() ? defLoc.getColumn() : 0});
      continue;
    }
    if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
      const clang::EnumDecl *definition = enumDecl->getDefinition();
      if (!definition)
        continue;
      std::string symbol = enumSymbolFor(definition);
      if (symbol.empty())
        continue;
      clang::PresumedLoc defLoc =
          sourceManager->getPresumedLoc(definition->getLocation());
      addNode({std::move(symbol), ItemKind::Enum, /*isDefinition=*/true,
               ItemLinkage::External, tuIndex,
               defLoc.isValid() ? defLoc.getFilename() : "",
               defLoc.isValid() ? defLoc.getLine() : 0,
               defLoc.isValid() ? defLoc.getColumn() : 0});
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      addNode({cGlobalSymbolName(var, tuTag), ItemKind::Global,
               var->hasDefinition() != clang::VarDecl::DeclarationOnly,
               linkageOf(var), tuIndex, std::move(file), line, column});
      continue;
    }
    // Anything else (typedefs, using declarations, static asserts,
    // templates) is not a program item in this model and is skipped
    // silently: unlike the importer, the graph never rejects — it describes
    // what it can see and stays quiet about the rest.
  }
}

//===----------------------------------------------------------------------===//
// Symbols
//===----------------------------------------------------------------------===//

std::string
ItemGraphBuilder::recordSymbolFor(const clang::RecordDecl *record) const {
  const clang::RecordDecl *definition = record->getDefinition();
  if (!definition)
    return {};
  if (!definition->getDeclContext()->getRedeclContext()->isFileContext())
    return {};
  return recordRustName(definition);
}

std::string
ItemGraphBuilder::enumSymbolFor(const clang::EnumDecl *enumDecl) const {
  const clang::EnumDecl *definition = enumDecl->getDefinition();
  if (!definition)
    return {};
  return mlir::emitrust::enumTypeRustName(definition->getName());
}

//===----------------------------------------------------------------------===//
// Pass 2: edges
//===----------------------------------------------------------------------===//

void ItemGraphBuilder::addEdge(llvm::StringRef from, llvm::StringRef to,
                               EdgeKind kind) {
  if (from.empty())
    return;
  // Self-edges are kept, not filtered: a `Calls` self-edge is direct
  // recursion and a `Field` self-edge is a self-referential record
  // (`struct Node { struct Node *next; }`) — both are exactly the facts a
  // consumer of this graph is looking for.
  if (kind != EdgeKind::CallsIndirect && (to.empty() || !nodes.count(to.str())))
    return; // Closure invariant: no edge to a non-item.
  if (!nodes.count(from.str()))
    return;
  edges.insert({from.str(), to.str(), kind});
}

void ItemGraphBuilder::collectTypeEdges(clang::QualType type,
                                        llvm::StringRef from, EdgeKind kind) {
  // Bounded by the source's type nesting; pointers are followed structurally
  // (`struct S **` -> `struct S`) but a record's fields are not, so a
  // self-referential struct cannot cycle here.
  //
  // The flag rides along with each worklist entry: it is set the moment the
  // walk steps through a DATA pointer and never cleared, so it answers "was
  // every route from the declared type to this target through a pointer?".
  // A pointer whose pointee is a FUNCTION does not set it — `int (*)(struct
  // S)` erases nothing, the emitted function-pointer type spells `S` in its
  // own parameter list.
  llvm::SmallVector<std::pair<clang::QualType, bool>, 8> worklist{
      {type, /*behindPointer=*/false}};
  // Keyed on (type, flag), not on the type alone: `struct Holder { struct S s;
  // struct S *p; }` must reach `S` twice, once each way, or whichever route
  // the worklist happened to pop first would decide the edge kind.
  llvm::SmallPtrSet<const clang::Type *, 8> seen[2];
  while (!worklist.empty()) {
    auto [current, behindPointer] = worklist.pop_back_val();
    if (current.isNull())
      continue;
    // The canonical type strips typedefs, `decltype`, and sugar, so a
    // `typedef struct S Alias;` parameter yields the same edge a bare
    // `struct S` one does.
    const clang::Type *canonical = current.getCanonicalType().getTypePtr();
    if (!seen[behindPointer ? 1 : 0].insert(canonical).second)
      continue;
    if (const auto *pointer = llvm::dyn_cast<clang::PointerType>(canonical)) {
      clang::QualType pointee = pointer->getPointeeType();
      bool toFunction = pointee.getCanonicalType()->isFunctionType();
      worklist.push_back({pointee, behindPointer || !toFunction});
      continue;
    }
    if (const auto *reference =
            llvm::dyn_cast<clang::ReferenceType>(canonical)) {
      // A C++ reference is NOT erased: it emits as a Rust reference that
      // spells its pointee. (It is also moot for `Field`, since a reference
      // member makes its record inadmissible outright — `reference-type` —
      // so the record is a Red SEED and no edge decides its color.)
      worklist.push_back({reference->getPointeeType(), behindPointer});
      continue;
    }
    if (const auto *array = llvm::dyn_cast<clang::ArrayType>(canonical)) {
      worklist.push_back({array->getElementType(), behindPointer});
      continue;
    }
    if (const auto *proto =
            llvm::dyn_cast<clang::FunctionProtoType>(canonical)) {
      worklist.push_back({proto->getReturnType(), behindPointer});
      for (clang::QualType param : proto->getParamTypes())
        worklist.push_back({param, behindPointer});
      continue;
    }
    if (const auto *function =
            llvm::dyn_cast<clang::FunctionType>(canonical)) {
      worklist.push_back({function->getReturnType(), behindPointer});
      continue;
    }
    EdgeKind reached = kind == EdgeKind::Field && behindPointer
                           ? EdgeKind::FieldIndirect
                           : kind;
    if (const auto *record = llvm::dyn_cast<clang::RecordType>(canonical)) {
      addEdge(from, recordSymbolFor(record->getDecl()), reached);
      continue;
    }
    if (const auto *enumType = llvm::dyn_cast<clang::EnumType>(canonical))
      addEdge(from, enumSymbolFor(enumType->getDecl()), reached);
  }
}

void ItemGraphBuilder::collectLValueDependencies(const clang::Expr *expr,
                                                 llvm::StringRef from,
                                                 bool reads, bool writes) {
  if (!expr)
    return;
  const clang::Expr *stripped = expr->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripped)) {
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    // Only objects with STATIC storage at file scope are globals in this
    // model; a function-local static surfaces under the importer's
    // `<function>_<name>` mangle and is not an item node here, so it falls
    // through the closure check in `addEdge` anyway.
    if (var && var->hasGlobalStorage()) {
      std::string symbol = cGlobalSymbolName(var, tuTag);
      if (writes)
        addEdge(from, symbol, EdgeKind::WritesGlobal);
      if (reads)
        addEdge(from, symbol, EdgeKind::ReadsGlobal);
    }
    return;
  }
  // A designator chain propagates the access context to its base object and
  // is otherwise an ordinary expression: `g.a[i] = 1` writes `g`, while `i`
  // is read by the subscript, not written by the assignment.
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripped)) {
    collectLValueDependencies(member->getBase(), from, reads, writes);
    return;
  }
  if (const auto *subscript =
          llvm::dyn_cast<clang::ArraySubscriptExpr>(stripped)) {
    collectLValueDependencies(subscript->getBase(), from, reads, writes);
    collectStmtDependencies(subscript->getIdx(), from);
    return;
  }
  // Not a designator chain (a dereference, a call result, a conditional):
  // whatever it writes cannot be attributed to a named global, so it is
  // walked as an ordinary reading expression.
  collectStmtDependencies(stripped, from);
}

void ItemGraphBuilder::collectStmtDependencies(const clang::Stmt *stmt,
                                               llvm::StringRef from) {
  if (!stmt)
    return;

  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    if (const clang::FunctionDecl *callee = call->getDirectCallee()) {
      if (!isSystemHeaderDecl(*sourceManager, callee))
        addEdge(from, cFunctionSymbolName(callee, tuTag), EdgeKind::Calls);
    } else {
      // A call with no resolvable callee is a call through a function
      // pointer. Recorded target-less rather than dropped: "this item makes
      // an indirect call" is the fact a port-order or devirtualization
      // consumer needs, and dropping it would make an item whose only calls
      // are indirect look like a leaf.
      addEdge(from, /*to=*/"", EdgeKind::CallsIndirect);
    }
    // The callee expression is deliberately NOT walked as an ordinary
    // subexpression for a direct call: the implicit function-to-pointer
    // decay clang inserts there would otherwise read as taking the
    // callee's address. Every argument is walked normally.
    for (const clang::Expr *arg : call->arguments())
      collectStmtDependencies(arg, from);
    if (!call->getDirectCallee())
      collectStmtDependencies(call->getCallee(), from);
    return;
  }

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
    if (binary->isAssignmentOp()) {
      // A compound assignment (`g += 1`) genuinely both reads and writes.
      collectLValueDependencies(binary->getLHS(), from,
                                /*reads=*/binary->isCompoundAssignmentOp(),
                                /*writes=*/true);
      collectStmtDependencies(binary->getRHS(), from);
      return;
    }
  }

  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
    if (unary->isIncrementDecrementOp()) {
      collectLValueDependencies(unary->getSubExpr(), from, /*reads=*/true,
                                /*writes=*/true);
      return;
    }
    if (unary->getOpcode() == clang::UO_AddrOf) {
      // `&g` is a reference to the object that does not itself store to it,
      // so it counts as a read; `&f` is the address-taken case handled by
      // the DeclRefExpr arm below.
      collectLValueDependencies(unary->getSubExpr(), from, /*reads=*/true,
                                /*writes=*/false);
      return;
    }
  }

  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
    if (const auto *func =
            llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
      // Reached only outside callee position (the direct-call arm above
      // never walks its callee expression), i.e. the function's address is
      // being taken — with or without an explicit `&`, since a bare
      // function name decays to a pointer.
      if (!isSystemHeaderDecl(*sourceManager, func) &&
          !llvm::isa<clang::CXXMethodDecl>(func))
        addEdge(from, cFunctionSymbolName(func, tuTag),
                EdgeKind::TakesAddressOf);
      return;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
      if (var->hasGlobalStorage())
        addEdge(from, cGlobalSymbolName(var, tuTag), EdgeKind::ReadsGlobal);
      return;
    }
    return;
  }

  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *decl : declStmt->decls()) {
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        collectTypeEdges(var->getType(), from, EdgeKind::BodyType);
        collectStmtDependencies(var->getInit(), from);
      }
    }
    return;
  }

  if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(stmt))
    collectTypeEdges(cast->getTypeAsWritten(), from, EdgeKind::BodyType);

  if (const auto *traitExpr =
          llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(stmt)) {
    // `sizeof(struct S)` names its operand type directly; `sizeof expr`
    // carries the type of the expression, which is just as much a mention.
    if (traitExpr->isArgumentType())
      collectTypeEdges(traitExpr->getArgumentTypeInfo()->getType(), from,
                       EdgeKind::BodyType);
    else
      collectTypeEdges(traitExpr->getArgumentExpr()->getType(), from,
                       EdgeKind::BodyType);
    return;
  }

  for (const clang::Stmt *child : stmt->children())
    collectStmtDependencies(child, from);
}

void ItemGraphBuilder::collectFunctionDependencies(
    const clang::FunctionDecl *func, llvm::StringRef from) {
  collectTypeEdges(func->getReturnType(), from, EdgeKind::SigType);
  for (const clang::ParmVarDecl *param : func->parameters())
    collectTypeEdges(param->getType(), from, EdgeKind::SigType);
  if (func->isThisDeclarationADefinition())
    collectStmtDependencies(func->getBody(), from);
}

void ItemGraphBuilder::collectRecordDependencies(const clang::RecordDecl *record,
                                                 llvm::StringRef from) {
  for (const clang::FieldDecl *field : record->fields())
    collectTypeEdges(field->getType(), from, EdgeKind::Field);
  if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
    // Direct bases only; a grandparent is reached by following the base's
    // own `Base` edge, which keeps the graph a graph rather than a
    // pre-computed transitive closure.
    for (const clang::CXXBaseSpecifier &base : cxxRecord->bases())
      collectTypeEdges(base.getType(), from, EdgeKind::Base);
  }
}

void ItemGraphBuilder::collectDependencies(const clang::DeclContext *context) {
  for (const clang::Decl *decl : context->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(*sourceManager, decl))
      continue;
    if (const auto *linkageSpec =
            llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      collectDependencies(linkageSpec);
      continue;
    }
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      collectDependencies(ns);
      continue;
    }
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (llvm::isa<clang::CXXMethodDecl>(func))
        continue;
      collectFunctionDependencies(func, cFunctionSymbolName(func, tuTag));
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
      const clang::RecordDecl *definition = record->getDefinition();
      if (!definition)
        continue;
      std::string symbol = recordSymbolFor(definition);
      if (!symbol.empty())
        collectRecordDependencies(definition, symbol);
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      // A global's own declared type is its signature, and its initializer
      // is the only "body" it has: `static struct Node *head = &root;`
      // depends on `Node` (SigType) and reads `root` (ReadsGlobal).
      std::string symbol = cGlobalSymbolName(var, tuTag);
      collectTypeEdges(var->getType(), symbol, EdgeKind::SigType);
      collectStmtDependencies(var->getInit(), symbol);
      continue;
    }
  }
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

ItemGraph ItemGraphBuilder::build(llvm::ArrayRef<clang::ASTUnit *> units) {
  // Pass 1 over every unit before pass 2 over any of them: the closure
  // invariant needs the complete node set, and cross-TU edges (a call from
  // TU 1 into a function defined in TU 0) would otherwise depend on the
  // order the units are visited in.
  for (auto [index, unit] : llvm::enumerate(units)) {
    tuIndex = static_cast<unsigned>(index);
    tuTag = ("tu" + llvm::Twine(index) + "_").str();
    sourceManager = &unit->getASTContext().getSourceManager();
    collectItems(unit->getASTContext().getTranslationUnitDecl());
  }
  for (auto [index, unit] : llvm::enumerate(units)) {
    tuIndex = static_cast<unsigned>(index);
    tuTag = ("tu" + llvm::Twine(index) + "_").str();
    sourceManager = &unit->getASTContext().getSourceManager();
    collectDependencies(unit->getASTContext().getTranslationUnitDecl());
  }

  ItemGraph graph;
  graph.nodes.reserve(nodes.size());
  for (const auto &entry : nodes)
    graph.nodes.push_back(entry.second);
  // The map is keyed on symbol alone while the published order is
  // (symbol, tuIndex); the sort is a no-op today and exists so the contract
  // holds by construction rather than by coincidence.
  llvm::stable_sort(graph.nodes, NodeOrder{});
  // `Field` SUBSUMES `FieldIndirect` for the same pair. A record that embeds
  // another both ways (`struct Holder { struct S s; struct S *p; }`) depends
  // on it by value, and the weaker edge alongside the stronger one would say
  // nothing a consumer could act on while doubling the line count of every
  // linked structure. One dependency between two items keeps one edge.
  std::set<std::pair<std::string, std::string>> byValue;
  for (const ItemEdge &edge : edges)
    if (edge.kind == EdgeKind::Field)
      byValue.insert({edge.from, edge.to});
  for (const ItemEdge &edge : edges)
    if (edge.kind != EdgeKind::FieldIndirect ||
        !byValue.count({edge.from, edge.to}))
      graph.edges.push_back(edge);
  return graph;
}

} // namespace

//===----------------------------------------------------------------------===//
// Entry points
//===----------------------------------------------------------------------===//

FailureOr<ItemGraph>
mlir::emitrust::buildItemGraph(llvm::ArrayRef<clang::ASTUnit *> units) {
  for (const clang::ASTUnit *unit : units)
    if (!unit)
      return failure();
  ItemGraphBuilder builder;
  return builder.build(units);
}

FailureOr<ItemGraph>
mlir::emitrust::buildItemGraph(llvm::ArrayRef<std::string> paths,
                               llvm::ArrayRef<std::string> extraClangArgs) {
  std::string error;
  return buildItemGraph(paths, extraClangArgs,
                        /*compilationDatabasePath=*/"", error);
}

FailureOr<ItemGraph>
mlir::emitrust::buildItemGraph(llvm::ArrayRef<std::string> paths,
                               llvm::ArrayRef<std::string> extraClangArgs,
                               llvm::StringRef compilationDatabasePath,
                               std::string &error) {
  std::vector<std::unique_ptr<clang::ASTUnit>> owned;
  // `sources` — not `paths` — is the translation-unit list the graph is
  // built over: with a database and no named paths the project IS the
  // database, so only the shell knows how many units there are, and the
  // `tuIndex` of every item keys off this resolved order.
  std::vector<std::string> sources;
  int status = buildProjectASTs(paths, extraClangArgs,
                                compilationDatabasePath, owned, sources, error);
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
  return buildItemGraph(llvm::ArrayRef<clang::ASTUnit *>(units));
}
