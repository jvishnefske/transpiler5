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
/// The one deliberate crack in that wall is the hosted-sink node set
/// (FR-62, `isHostedSinkName`): a system-header callee the importer lowers
/// by name into a Rust OUTPUT EFFECT is synthesized as a `def=0` node at
/// its call, during pass 2 — see the CallExpr arm — so the closure
/// invariant holds with the node present rather than by dropping the edge.
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
/// record/enum symbols from `recordRustName`. The tag-versus-ordinary-
/// identifier rename of `CImporter::structSymbolName` is REPRODUCED
/// (FR-62, pass 0 below): its trigger is not accumulated import state but
/// the per-TU ordinary-name pre-scan (`collectOrdinaryNames`), which this
/// file mirrors decl-for-decl over the same symbol-naming calls, with the
/// earlier TUs' scans standing in for `ordinaryNameTaken`'s already-
/// imported-symbol component (equivalent for every importable program: a
/// TU's pre-scan is exactly the set of ordinary module symbols it goes on
/// to claim). What still cannot be followed is naming that DOES depend on
/// import state (the `Anon<n>` shape keying, the block-scope
/// `<function>_<tag>` mangle); records and enums whose emitted name would
/// depend on it are NOT nodes at all (see `recordSymbolFor`). A missing
/// node is a visible, honest gap; a wrong node key would silently corrupt
/// every consumer — which is precisely what the pre-fix `struct G` /
/// global `g` key collision did, and why the rename is reproduced rather
/// than left as a known gap.
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
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <optional>
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
  case EdgeKind::AddressOfGlobal:
    return "AddressOfGlobal";
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

//===----------------------------------------------------------------------===//
// Parsing (FR-62 F1a) — the printer's exact inverse
//===----------------------------------------------------------------------===//

namespace {

/// The inverse of `itemKindName`; no match is std::nullopt.
std::optional<ItemKind> parseItemKind(llvm::StringRef spelling) {
  if (spelling == "function")
    return ItemKind::Function;
  if (spelling == "record")
    return ItemKind::Record;
  if (spelling == "enum")
    return ItemKind::Enum;
  if (spelling == "global")
    return ItemKind::Global;
  return std::nullopt;
}

/// The inverse of `edgeKindName`; no match is std::nullopt.
std::optional<EdgeKind> parseEdgeKind(llvm::StringRef spelling) {
  static constexpr EdgeKind kinds[] = {
      EdgeKind::Calls,        EdgeKind::CallsIndirect,
      EdgeKind::SigType,      EdgeKind::BodyType,
      EdgeKind::Field,        EdgeKind::Base,
      EdgeKind::ReadsGlobal,  EdgeKind::WritesGlobal,
      EdgeKind::TakesAddressOf, EdgeKind::FieldIndirect,
      EdgeKind::AddressOfGlobal};
  for (EdgeKind kind : kinds)
    if (spelling == edgeKindName(kind))
      return kind;
  return std::nullopt;
}

} // namespace

FailureOr<ItemGraph>
mlir::emitrust::parseItemGraphText(llvm::StringRef text, std::string &error) {
  auto fail = [&](llvm::StringRef line, llvm::StringRef reason) {
    error = ("malformed item-graph line '" + line + "': " + reason).str();
    return failure();
  };
  ItemGraph graph;
  for (llvm::StringRef line : llvm::split(text, '\n')) {
    if (line.empty())
      continue;
    llvm::StringRef rest = line;
    if (rest.consume_front("node ")) {
      // node <symbol> kind=<k> def=<0|1> linkage=<l> tu=<i>
      //      loc=<file>:<line>:<col> — every field one whole token.
      ItemNode node;
      llvm::SmallVector<llvm::StringRef, 6> tokens;
      rest.split(tokens, ' ');
      if (tokens.size() != 6)
        return fail(line, "expected 6 node fields");
      node.symbol = tokens[0].str();
      if (node.symbol.empty())
        return fail(line, "empty node symbol");
      llvm::StringRef kindToken = tokens[1], defToken = tokens[2],
                      linkageToken = tokens[3], tuToken = tokens[4],
                      locToken = tokens[5];
      if (!kindToken.consume_front("kind="))
        return fail(line, "expected kind=");
      std::optional<ItemKind> kind = parseItemKind(kindToken);
      if (!kind)
        return fail(line, "unknown item kind");
      node.kind = *kind;
      if (!defToken.consume_front("def=") ||
          (defToken != "0" && defToken != "1"))
        return fail(line, "expected def=0 or def=1");
      node.isDefinition = defToken == "1";
      if (!linkageToken.consume_front("linkage="))
        return fail(line, "expected linkage=");
      if (linkageToken == "intern")
        node.linkage = ItemLinkage::Internal;
      else if (linkageToken == "extern")
        node.linkage = ItemLinkage::External;
      else
        return fail(line, "unknown linkage");
      if (!tuToken.consume_front("tu=") ||
          tuToken.getAsInteger(10, node.tuIndex))
        return fail(line, "expected tu=<index>");
      if (!locToken.consume_front("loc="))
        return fail(line, "expected loc=");
      // The file may itself contain ':' (a Windows drive, an odd path), so
      // the line and column split off the RIGHT end.
      auto [fileLine, columnToken] = locToken.rsplit(':');
      auto [file, lineToken] = fileLine.rsplit(':');
      if (file.empty() || lineToken.getAsInteger(10, node.line) ||
          columnToken.getAsInteger(10, node.column))
        return fail(line, "expected loc=<file>:<line>:<col>");
      node.file = file.str();
      graph.nodes.push_back(std::move(node));
      continue;
    }
    if (rest.consume_front("edge ")) {
      // edge <from> -> <to|?> kind=<K>
      llvm::SmallVector<llvm::StringRef, 4> tokens;
      rest.split(tokens, ' ');
      if (tokens.size() != 4 || tokens[1] != "->")
        return fail(line, "expected 'edge <from> -> <to> kind=<kind>'");
      ItemEdge edge;
      edge.from = tokens[0].str();
      llvm::StringRef kindToken = tokens[3];
      if (edge.from.empty() || tokens[2].empty())
        return fail(line, "empty edge endpoint");
      if (!kindToken.consume_front("kind="))
        return fail(line, "expected kind=");
      std::optional<EdgeKind> kind = parseEdgeKind(kindToken);
      if (!kind)
        return fail(line, "unknown edge kind");
      edge.kind = *kind;
      // `?` is CallsIndirect's fixed no-target token, and only its.
      if (tokens[2] == "?") {
        if (edge.kind != EdgeKind::CallsIndirect)
          return fail(line, "target '?' is only valid for CallsIndirect");
      } else {
        if (edge.kind == EdgeKind::CallsIndirect)
          return fail(line, "CallsIndirect takes the fixed target '?'");
        edge.to = tokens[2].str();
      }
      graph.edges.push_back(std::move(edge));
      continue;
    }
    return fail(line, "unknown line prefix (expected 'node ' or 'edge ')");
  }
  return graph;
}

std::string mlir::emitrust::retagInternalSymbol(llvm::StringRef symbol,
                                                unsigned ordinal) {
  llvm::StringRef rest = symbol;
  llvm::StringRef tag;
  if (rest.consume_front("tu"))
    tag = "tu";
  else if (rest.consume_front("TU"))
    tag = "TU";
  else
    return symbol.str();
  size_t digits = 0;
  while (digits < rest.size() && llvm::isDigit(rest[digits]))
    ++digits;
  if (digits == 0 || digits >= rest.size() || rest[digits] != '_')
    return symbol.str();
  return (tag + llvm::Twine(ordinal) + "_" + rest.drop_front(digits + 1))
      .str();
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

/// The importer's hosted OUTPUT SINKS: definition-less system-header
/// functions whose calls the importer lowers BY NAME into Rust output
/// effects, mirrored here so the graph can attribute output effects totally
/// (FR-62 hosted-sink visibility; E2 measured `@stdout` attribution
/// reaching only 7/131 kernel TUs under the fully closed graph). The set is
/// exactly the importer's output-effect emission surface, one cite each:
/// printf (`emitPrintf`, ImportCStatements.cpp), puts (`emitPuts`, ibid.),
/// putchar (`emitPutchar`, ImportCHosted.cpp), fprintf (the devirtualized
/// stdout-swallow routing of `emitAliasedPrintf`, ImportCStatements.cpp),
/// fwrite (`emitFileReadWrite(isWrite=true)`, ImportCHosted.cpp), sprintf
/// and snprintf (`emitSprintf`, ImportCExpressions.cpp). Hosted NON-sink
/// functions (strlen, strcmp, memcpy, abs, atoi, fread, ...) are
/// deliberately absent: they produce no output effect, so their
/// system-header calls keep the closed-graph silence. Membership is by
/// name only — argument-shape conditions (fprintf's literal-`stdout` rule,
/// printf's literal format) stay the importer's business, because a call
/// the importer will REJECT still attributes an intended output effect.
bool isHostedSinkName(llvm::StringRef name) {
  return name == "printf" || name == "puts" || name == "putchar" ||
         name == "fprintf" || name == "fwrite" || name == "sprintf" ||
         name == "snprintf";
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
  // Pass 0: per-TU ordinary-identifier names (FR-62 rename reproduction)
  //===--------------------------------------------------------------------===//

  /// Records into `tuOrdinaryNames[tuIndex]` every module symbol `context`'s
  /// ordinary identifier namespace will claim, mirroring
  /// `CImporter::collectOrdinaryNamesFrom` decl-for-decl over the same
  /// naming calls: function symbols (`cFunctionSymbolName`), file-scope
  /// variable symbols (`cGlobalSymbolName`), and function-local statics
  /// under their `<function>_<name>` mangle (`collectStaticLocalNames`).
  /// One deliberate guard the importer's walk does not need: a
  /// `FunctionDecl` whose declaration name is not a plain identifier (an
  /// out-of-line constructor/destructor/operator) is skipped, since
  /// `getName` is only defined for identifiers — such a name can never
  /// collide with a record's UpperCamel spelling anyway.
  void collectOrdinaryNames(const clang::DeclContext *context);

  /// The static-local half of the pre-scan: every `static` local of a
  /// function body claims `globalRustName(<function>_<name>)` at module
  /// level (the importer's `collectStaticLocalNames`, reproduced).
  void collectStaticLocalNames(const clang::Stmt *stmt,
                               llvm::StringRef funcSymbol);

  /// Whether `name` is claimed by the ordinary identifier namespace as the
  /// importer would see it while importing the CURRENT TU:
  /// `CImporter::ordinaryNameTaken` consults the current TU's pre-scan plus
  /// every already-imported non-struct module symbol, and TUs import in
  /// project order, so the union of scans `0..tuIndex` reproduces both
  /// components for every importable program.
  bool ordinaryNameTaken(llvm::StringRef name) const;

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

  /// Synthesizes the node for a hosted-sink callee (`isHostedSinkName`) and
  /// records the `Calls` edge from `from`. Runs in pass 2 — the one place a
  /// node is created outside `collectItems` — because the sink is only an
  /// item of this graph BECAUSE it is called; the closure invariant is
  /// satisfied by inserting the node before its edge, and the node can
  /// change no other edge's fate: the only edges that ever target it are
  /// the ones synthesized here alongside it (`TakesAddressOf` of a
  /// system-header function is filtered before its symbol is formed).
  /// The node is `def=0`, extern, located at the callee's real
  /// system-header declaration; multiple TUs' calls merge in `addNode`
  /// exactly like a shared prototype's.
  void addHostedSinkCall(const clang::FunctionDecl *callee,
                         llvm::StringRef from);

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
  ///
  /// `addressOf` rides alongside `reads`/`writes` down the same designator
  /// chain: it is set by the two contexts whose result is a POINTER INTO
  /// the designated object (`&` and array-to-pointer decay used as a
  /// value), so the rooted global additionally records `AddressOfGlobal`
  /// (FR-62) — always on top of the `ReadsGlobal` those contexts have
  /// recorded since before the kind existed.
  void collectLValueDependencies(const clang::Expr *expr, llvm::StringRef from,
                                 bool reads, bool writes,
                                 bool addressOf = false);

  /// C99-43 C1: when `assign` is `*pp = <rhs>` on a data
  /// pointer-to-pointer PARAMETER with every RHS alternative a whole
  /// statically-known file-scope global or a null pointer constant (the
  /// admitted single-global-or-NULL grammar, including the
  /// `cond ? g : NULL` ternary), records each named global as
  /// `ReadsGlobal` (the emitted Some-offset write holds no address into
  /// its storage), walks the ternary condition normally, and returns
  /// true. Returns false — recording nothing — for every other
  /// assignment, which then takes the ordinary walk (a surviving decay
  /// stays `AddressOfGlobal`).
  bool walkGlobalCursorWrite(const clang::BinaryOperator *assign,
                             llvm::StringRef from);

  //===--------------------------------------------------------------------===//
  // Symbols and edges
  //===--------------------------------------------------------------------===//

  /// The emitted symbol of `record`: `recordRustName`, renamed to the
  /// `Struct_<tag>` spelling through the same `typeRustName` composition
  /// `CImporter::structSymbolName` uses when the ordinary identifier
  /// namespace claims the tag spelling (FR-62 — the fix for the
  /// `struct G` / global `g` node-key collision). Empty when the graph
  /// deliberately declines to name it: an anonymous record (the importer
  /// would assign a shape-keyed `Anon<n>`), a non-file-scope one (the
  /// importer would apply the `<function>_<tag>` block-scope mangle) —
  /// both namings depend on accumulated import state the graph does not
  /// carry — or a record whose renamed spelling is ALSO claimed, which
  /// `structSymbolName` rejects outright, so no emitted name exists to
  /// match. See this file's header comment for why an omitted node beats a
  /// guessed one.
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

  /// Pass 0 result: `tuOrdinaryNames[i]` is TU `i`'s pre-scanned ordinary
  /// module symbols. Queried through `ordinaryNameTaken` only; never
  /// iterated, so the container's order never reaches the boundary.
  std::vector<std::set<std::string>> tuOrdinaryNames;

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
// Pass 0: per-TU ordinary-identifier names
//===----------------------------------------------------------------------===//

void ItemGraphBuilder::collectStaticLocalNames(const clang::Stmt *stmt,
                                               llvm::StringRef funcSymbol) {
  if (!stmt)
    return;
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
        if (var->isStaticLocal())
          tuOrdinaryNames[tuIndex].insert(globalRustName(
              (llvm::Twine(funcSymbol) + "_" + var->getName()).str()));
  for (const clang::Stmt *child : stmt->children())
    collectStaticLocalNames(child, funcSymbol);
}

void ItemGraphBuilder::collectOrdinaryNames(const clang::DeclContext *context) {
  for (const clang::Decl *decl : context->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(*sourceManager, decl))
      continue;
    if (const auto *linkageSpec =
            llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      collectOrdinaryNames(linkageSpec);
      continue;
    }
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      collectOrdinaryNames(ns);
      continue;
    }
    // W2.15: a function template's INSTANTIATIONS are the emitted items
    // (the pattern is not), so they claim the ordinary names — mirroring
    // `CImporter::collectOrdinaryNamesFrom`.
    if (const auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      for (const clang::FunctionDecl *spec : tmpl->specializations()) {
        if (!spec->isThisDeclarationADefinition())
          continue;
        std::string symbol = cFunctionSymbolName(spec, tuTag);
        if (spec->hasBody())
          collectStaticLocalNames(spec->getBody(), symbol);
        tuOrdinaryNames[tuIndex].insert(std::move(symbol));
      }
      continue;
    }
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      // W2.25: an ADMITTED free operator now claims its synthesized
      // spelling (`op_eq`) like any ordinary function; every other
      // non-identifier shape (literal operators, non-admitted kinds)
      // composes the empty symbol and claims nothing — mirroring the
      // importer's `collectOrdinaryNamesFrom`. An out-of-line MEMBER
      // operator's real symbol is `<Struct>_op_*`, so it must not claim
      // the free spelling (identifier out-of-line methods keep their
      // historical claim unchanged).
      if (llvm::isa<clang::CXXMethodDecl>(func) &&
          !func->getDeclName().isIdentifier())
        continue;
      std::string symbol = cFunctionSymbolName(func, tuTag);
      if (symbol.empty())
        continue;
      if (func->hasBody())
        collectStaticLocalNames(func->getBody(), symbol);
      tuOrdinaryNames[tuIndex].insert(std::move(symbol));
      continue;
    }
    // FR-123: a friend function defined inline in a class claims an
    // ordinary module symbol like the free function it is, but is absent
    // from `decls()`; mirroring `CImporter::collectOrdinaryNamesFrom`'s
    // own FR-123 arm keeps this pre-scan the same set the importer's is.
    if (llvm::isa<clang::CXXRecordDecl>(decl)) {
      llvm::SmallVector<const clang::FunctionDecl *, 4> friends;
      collectFriendDefinitions(decl, friends);
      for (const clang::FunctionDecl *func : friends) {
        std::string symbol = cFunctionSymbolName(func, tuTag);
        if (symbol.empty())
          continue;
        if (func->hasBody())
          collectStaticLocalNames(func->getBody(), symbol);
        tuOrdinaryNames[tuIndex].insert(std::move(symbol));
      }
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      tuOrdinaryNames[tuIndex].insert(cGlobalSymbolName(var, tuTag));
  }
}

bool ItemGraphBuilder::ordinaryNameTaken(llvm::StringRef name) const {
  for (unsigned i = 0, e = tuIndex + 1;
       i < e && i < tuOrdinaryNames.size(); ++i)
    if (tuOrdinaryNames[i].count(name.str()))
      return true;
  return false;
}

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

    // W2.15: a function template contributes one item per INSTANTIATION,
    // mirroring `CImporter::importTopLevelDecl`'s arm — the uninstantiated
    // pattern is not an item (nothing is emitted for it). Without this the
    // whole-program index would be silently incomplete: the specializations
    // would have no node while a caller's body still adds a `Calls` edge
    // under their (suffixed) names, and that denominator feeds
    // `--incremental`'s PORTING.md / emitrust-progress.json.
    if (const auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      for (const clang::FunctionDecl *spec : tmpl->specializations()) {
        if (!spec->isThisDeclarationADefinition())
          continue;
        clang::PresumedLoc specLoc =
            sourceManager->getPresumedLoc(spec->getLocation());
        addNode({cFunctionSymbolName(spec, tuTag), ItemKind::Function,
                 /*isDefinition=*/true, linkageOf(spec), tuIndex,
                 specLoc.isValid() ? specLoc.getFilename() : "",
                 specLoc.isValid() ? specLoc.getLine() : 0,
                 specLoc.isValid() ? specLoc.getColumn() : 0});
      }
      continue;
    }

    // W2.16: a class template contributes one RECORD item per
    // INSTANTIATION, mirroring `CImporter::importTopLevelDecl`'s arm — the
    // uninstantiated pattern is not an item (nothing is emitted for it).
    // Without this the whole-program index would be silently incomplete:
    // an instantiation would have no node while a body naming it still
    // adds a `BodyType` edge under its (suffixed) name, and that
    // denominator feeds `--incremental`'s PORTING.md /
    // emitrust-progress.json.
    if (const auto *classTmpl =
            llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
      for (const clang::ClassTemplateSpecializationDecl *spec :
           classTmpl->specializations()) {
        if (!spec->isThisDeclarationADefinition())
          continue;
        std::string symbol = recordSymbolFor(spec);
        if (symbol.empty())
          continue;
        clang::PresumedLoc specLoc =
            sourceManager->getPresumedLoc(spec->getLocation());
        addNode({std::move(symbol), ItemKind::Record, /*isDefinition=*/true,
                 ItemLinkage::External, tuIndex,
                 specLoc.isValid() ? specLoc.getFilename() : "",
                 specLoc.isValid() ? specLoc.getLine() : 0,
                 specLoc.isValid() ? specLoc.getColumn() : 0});
      }
      continue;
    }

    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      // C++ member functions are not items: they are declared inside a
      // record, so `importDeclsIn` never reaches them, and their emitted
      // name comes from `CImporter::cxxMethodMangledName`, which needs the
      // class's assigned struct name. An OUT-OF-LINE method definition does
      // appear here at item scope, and is skipped for the same reason.
      if (llvm::isa<clang::CXXMethodDecl>(func))
        continue;
      // FR-119, narrowed by W2.25: a free operator of an ADMITTED kind now
      // has a synthesized identifier spelling (`op_eq`, with FR-114's
      // per-parameter suffixes) and mints a node exactly like an ordinary
      // function — in lockstep with the importer's admission, so the graph
      // and the emitted module agree on the item set. Every other
      // non-identifier shape (literal operators, operator templates,
      // non-admitted kinds) still composes the EMPTY symbol — the importer
      // rejects it located (`unsupported: overloaded operator`) and the
      // graph skips it, never minting an empty-named node (getName() is no
      // longer reached: `cFunctionSymbolName` screens the DeclarationName
      // itself).
      std::string symbol = cFunctionSymbolName(func, tuTag);
      if (symbol.empty())
        continue;
      addNode({std::move(symbol), ItemKind::Function,
               func->isThisDeclarationADefinition(), linkageOf(func), tuIndex,
               std::move(file), line, column});
      continue;
    }
    // FR-123: the friend functions DEFINED INLINE in this class are items
    // of THIS scope — their semantic declaration context is this one, so
    // they name exactly like free functions — reachable only through the
    // record's lexical member list. Without this arm the graph would be
    // missing an item the crate DOES emit, which is the denominator half
    // of the defect: `--incremental` scored a program that had silently
    // lost a function at `graph_items: 2, ported: 2, permille 1000`. The
    // arm runs BEFORE the record arm below and does not consume `decl`,
    // so the record still mints its own node; the selection rule is shared
    // with `CImporter::importDeclsIn` so the two agree by construction.
    if (llvm::isa<clang::CXXRecordDecl>(decl)) {
      llvm::SmallVector<const clang::FunctionDecl *, 4> friends;
      collectFriendDefinitions(decl, friends);
      for (const clang::FunctionDecl *func : friends) {
        std::string symbol = cFunctionSymbolName(func, tuTag);
        if (symbol.empty())
          continue;
        clang::PresumedLoc friendLoc =
            sourceManager->getPresumedLoc(func->getLocation());
        addNode({std::move(symbol), ItemKind::Function,
                 /*isDefinition=*/true, linkageOf(func), tuIndex,
                 friendLoc.isValid() ? friendLoc.getFilename() : "",
                 friendLoc.isValid() ? friendLoc.getLine() : 0,
                 friendLoc.isValid() ? friendLoc.getColumn() : 0});
      }
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
  std::string base = recordRustName(definition);
  if (base.empty() || !ordinaryNameTaken(base))
    return base;
  // The tag yields to the ordinary namespace, deterministically, exactly as
  // in `CImporter::structSymbolName` (C's tag namespace is separate, C99
  // 6.2.3; the module symbol table is not). The composition is the
  // importer's own: under the idiomatic rename `typeRustName` re-camels the
  // joined spelling, so `struct G` emits — and keys — as `StructG`.
  std::string renamed = typeRustName("Struct_" + base);
  if (ordinaryNameTaken(renamed))
    return {}; // The importer rejects this program; no emitted name exists.
  return renamed;
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
                                                 bool reads, bool writes,
                                                 bool addressOf) {
  if (!expr)
    return;
  const clang::Expr *stripped = expr->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripped)) {
    // An EXPLICIT `&f` routes the function reference through this walk
    // (the bare-name decay reaches the rvalue DeclRefExpr arm instead);
    // both spellings are the same TakesAddressOf fact, and FR-62's rule 1
    // demotion keys on it (found by the stage-B default flip: c-testsuite
    // 00089 returns `&anon` and the edge was silently dropped).
    if (const auto *func =
            llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
      if (!isSystemHeaderDecl(*sourceManager, func) &&
          !llvm::isa<clang::CXXMethodDecl>(func))
        addEdge(from, cFunctionSymbolName(func, tuTag),
                EdgeKind::TakesAddressOf);
      return;
    }
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
      if (addressOf)
        addEdge(from, symbol, EdgeKind::AddressOfGlobal);
    }
    return;
  }
  // A designator chain propagates the access context to its base object and
  // is otherwise an ordinary expression: `g.a[i] = 1` writes `g`, while `i`
  // is read by the subscript, not written by the assignment. `addressOf`
  // propagates too: `&g.a` and `&g[i]` hand out a pointer into `g`'s
  // storage just as `&g` does.
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stripped)) {
    collectLValueDependencies(member->getBase(), from, reads, writes,
                              addressOf);
    return;
  }
  if (const auto *subscript =
          llvm::dyn_cast<clang::ArraySubscriptExpr>(stripped)) {
    collectLValueDependencies(subscript->getBase(), from, reads, writes,
                              addressOf);
    collectStmtDependencies(subscript->getIdx(), from);
    return;
  }
  // Not a designator chain (a dereference, a call result, a conditional):
  // whatever it writes cannot be attributed to a named global, so it is
  // walked as an ordinary reading expression.
  collectStmtDependencies(stripped, from);
}

/// The whole-address-of-one-global matcher of the C1 carve-out
/// (`walkGlobalCursorWrite`): an array-to-pointer decay of a file-scope
/// global array or `&g` over a file-scope global, and nothing derived
/// (`g + 1`, `&g[i]` keep the ordinary walk). The syntactic twin of the
/// importer's `asWholeGlobalAddress` (ImportCPlanning.cpp); a
/// function-local static is excluded exactly as the importer excludes it.
static const clang::VarDecl *asWholeGlobalAddressExpr(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
      unary && unary->getOpcode() == clang::UO_AddrOf)
    e = unary->getSubExpr()->IgnoreParenImpCasts();
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var || !var->hasGlobalStorage() || var->isStaticLocal())
    return nullptr;
  return var;
}

/// The `*pp` LHS matcher of the C1 carve-out: a dereference of a data
/// pointer-to-pointer PARAMETER (the only position the importer's
/// cursor-parameter planning refines).
static const clang::ParmVarDecl *asPtrPtrParamDeref(const clang::Expr *expr) {
  const auto *unary =
      llvm::dyn_cast<clang::UnaryOperator>(expr->IgnoreParenImpCasts());
  if (!unary || unary->getOpcode() != clang::UO_Deref)
    return nullptr;
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
      unary->getSubExpr()->IgnoreParenImpCasts());
  if (!ref)
    return nullptr;
  const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl());
  if (!param)
    return nullptr;
  clang::QualType type = param->getType().getCanonicalType();
  if (!type->isPointerType())
    return nullptr;
  clang::QualType pointee = type->getPointeeType().getCanonicalType();
  if (!pointee->isPointerType() || pointee->isFunctionPointerType())
    return nullptr;
  return param;
}

bool ItemGraphBuilder::walkGlobalCursorWrite(const clang::BinaryOperator *assign,
                                             llvm::StringRef from) {
  const clang::ParmVarDecl *param = asPtrPtrParamDeref(assign->getLHS());
  if (!param)
    return false;
  clang::ASTContext &context = param->getASTContext();
  auto isNull = [&](const clang::Expr *e) {
    return e->isNullPointerConstant(context,
                                    clang::Expr::NPC_NeverValueDependent) !=
           clang::Expr::NPCK_NotNull;
  };
  auto readGlobal = [&](const clang::VarDecl *global) {
    addEdge(from, cGlobalSymbolName(global, tuTag), EdgeKind::ReadsGlobal);
  };
  const clang::Expr *rhs = assign->getRHS()->IgnoreParens();
  if (const clang::VarDecl *global = asWholeGlobalAddressExpr(rhs)) {
    readGlobal(global);
    return true;
  }
  if (const auto *conditional =
          llvm::dyn_cast<clang::ConditionalOperator>(rhs);
      conditional && !isNull(rhs)) {
    const clang::Expr *trueArm = conditional->getTrueExpr();
    const clang::Expr *falseArm = conditional->getFalseExpr();
    const clang::VarDecl *trueGlobal = asWholeGlobalAddressExpr(trueArm);
    const clang::VarDecl *falseGlobal = asWholeGlobalAddressExpr(falseArm);
    bool covered = (trueGlobal || isNull(trueArm)) &&
                   (falseGlobal || isNull(falseArm)) &&
                   (trueGlobal || falseGlobal);
    if (!covered)
      return false;
    collectStmtDependencies(conditional->getCond(), from);
    if (trueGlobal)
      readGlobal(trueGlobal);
    if (falseGlobal)
      readGlobal(falseGlobal);
    return true;
  }
  return false;
}

void ItemGraphBuilder::addHostedSinkCall(const clang::FunctionDecl *callee,
                                         llvm::StringRef from) {
  // The symbol comes from the same naming function as every other function
  // node; a hosted sink is never `static`, so the per-TU tag does not
  // apply and one program-wide node results however many TUs call it.
  std::string symbol = cFunctionSymbolName(callee, tuTag);
  clang::PresumedLoc loc = sourceManager->getPresumedLoc(callee->getLocation());
  addNode({symbol, ItemKind::Function, /*isDefinition=*/false,
           linkageOf(callee), tuIndex, loc.isValid() ? loc.getFilename() : "",
           loc.isValid() ? loc.getLine() : 0,
           loc.isValid() ? loc.getColumn() : 0});
  addEdge(from, symbol, EdgeKind::Calls);
}

void ItemGraphBuilder::collectStmtDependencies(const clang::Stmt *stmt,
                                               llvm::StringRef from) {
  if (!stmt)
    return;

  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    if (const clang::FunctionDecl *callee = call->getDirectCallee()) {
      if (!isSystemHeaderDecl(*sourceManager, callee))
        addEdge(from, cFunctionSymbolName(callee, tuTag), EdgeKind::Calls);
      else if (callee->getDeclName().isIdentifier() &&
               isHostedSinkName(callee->getName()) && !callee->getDefinition())
        // The hosted-sink exception to the closed graph (FR-62): mirror the
        // importer's interception conditions — by name, and only when the
        // project supplies no definition of its own (a project-defined
        // printf is an ordinary call handled above, since its definition is
        // not in a system header).
        addHostedSinkCall(callee, from);
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
      // C99-43 C1 carve-out: `*pp = <single-global-or-NULL>` on a T**
      // parameter is the admitted out-param cursor write, whose emitted
      // form ERASES the address into a `Some(0)` offset — no pointer
      // into the global's storage survives, so the RHS records plain
      // `ReadsGlobal` instead of the `AddressOfGlobal` the decay arm
      // below would stamp (which FR-62's rule 1b would turn into a
      // spurious thread-local demotion; the spike pinned the actor form
      // as the target shape). Sound even when the importer later
      // REJECTS the function (a residual global shape, an escape
      // elsewhere): the item then has no imported function op and the
      // actor plan's missing-function rule still demotes.
      if (!binary->isCompoundAssignmentOp() &&
          walkGlobalCursorWrite(binary, from)) {
        collectLValueDependencies(binary->getLHS(), from,
                                  /*reads=*/false, /*writes=*/true);
        return;
      }
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
      // so it counts as a read — and the pointer it hands out is the
      // global's address escaping, the `AddressOfGlobal` fact (FR-62); `&f`
      // is the address-taken case handled by the DeclRefExpr arm below.
      collectLValueDependencies(unary->getSubExpr(), from, /*reads=*/true,
                                /*writes=*/false, /*addressOf=*/true);
      return;
    }
  }

  // A subscript READ in ordinary (rvalue) position: route it through the
  // lvalue walk so the base's array-to-pointer decay is consumed by the
  // subscript rather than reaching the decay arm below — `g[i]` reads `g`,
  // it does not take its address, because no pointer value survives the
  // subscript. The lvalue walk propagates the read to the base designator
  // chain and walks the index as an ordinary expression, exactly as it
  // already does for a subscript inside an assignment's LHS.
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(stmt)) {
    collectLValueDependencies(subscript, from, /*reads=*/true,
                              /*writes=*/false);
    return;
  }

  // An array-to-pointer decay that SURVIVES as a value (returned, passed as
  // an argument, stored, fed to pointer arithmetic — every decay except a
  // subscript base, which the arm above consumes) hands out a pointer to
  // the array's storage: for a global array that is the address escaping,
  // recorded as `AddressOfGlobal` on top of the historical `ReadsGlobal`
  // (FR-62). A function-to-pointer decay is a different cast kind and keeps
  // its `TakesAddressOf` path through the DeclRefExpr arm below.
  if (const auto *implicitCast = llvm::dyn_cast<clang::ImplicitCastExpr>(stmt)) {
    if (implicitCast->getCastKind() == clang::CK_ArrayToPointerDecay) {
      collectLValueDependencies(implicitCast->getSubExpr(), from,
                                /*reads=*/true, /*writes=*/false,
                                /*addressOf=*/true);
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
    // W2.15: edges out of an instantiation's own body. Without this arm a
    // `scale<double>` node would exist with no `Calls` edge to the
    // `add<double>` its body invokes, and the index would understate the
    // porting frontier.
    if (const auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      for (const clang::FunctionDecl *spec : tmpl->specializations()) {
        if (!spec->isThisDeclarationADefinition())
          continue;
        collectFunctionDependencies(spec, cFunctionSymbolName(spec, tuTag));
      }
      continue;
    }
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (llvm::isa<clang::CXXMethodDecl>(func))
        continue;
      collectFunctionDependencies(func, cFunctionSymbolName(func, tuTag));
      continue;
    }
    // W2.16: field edges out of a class-template INSTANTIATION, the record
    // twin of the arm above. Without it `Wrap<Point>` would have a node
    // but no `Field` edge to `Point`, so the coloring pass could not
    // propagate an inadmissible field type through an instantiation.
    if (const auto *classTmpl =
            llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
      for (const clang::ClassTemplateSpecializationDecl *spec :
           classTmpl->specializations()) {
        if (!spec->isThisDeclarationADefinition())
          continue;
        std::string symbol = recordSymbolFor(spec);
        if (!symbol.empty())
          collectRecordDependencies(spec, symbol);
      }
      continue;
    }
    // FR-123: edges out of a hidden friend's own body and signature, the
    // pass-2 twin of the node arm above. Without it the friend would have
    // a node with no `Calls`/`SigType` edges and the coloring could not
    // propagate an inadmissible type through it.
    if (llvm::isa<clang::CXXRecordDecl>(decl)) {
      llvm::SmallVector<const clang::FunctionDecl *, 4> friends;
      collectFriendDefinitions(decl, friends);
      for (const clang::FunctionDecl *func : friends) {
        std::string symbol = cFunctionSymbolName(func, tuTag);
        if (!symbol.empty())
          collectFunctionDependencies(func, symbol);
      }
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
  // Pass 0 over every unit before pass 1 over any of them: a record's node
  // key needs its TU's (and every earlier TU's) ordinary names to decide
  // the `Struct_<tag>` rename, and pass 1 already forms record keys.
  tuOrdinaryNames.assign(units.size(), {});
  for (auto [index, unit] : llvm::enumerate(units)) {
    tuIndex = static_cast<unsigned>(index);
    tuTag = ("tu" + llvm::Twine(index) + "_").str();
    sourceManager = &unit->getASTContext().getSourceManager();
    collectOrdinaryNames(unit->getASTContext().getTranslationUnitDecl());
  }
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
  return buildItemGraph(llvm::ArrayRef<clang::ASTUnit *>(units));
}
