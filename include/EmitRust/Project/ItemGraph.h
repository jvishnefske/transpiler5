//===- ItemGraph.h - whole-project program item graph -----------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-40: the whole-project dependency graph over PROGRAM ITEMS — the
/// things the importer emits as top-level Rust items: functions, records
/// (`struct`/`union`/`class`), enums, and global variables — with directed,
/// kinded edges between them (`Calls`, `SigType`, `Field`, `Base`, ...).
///
/// What this is for. The importer is a translation pipeline: it answers
/// "what Rust does this C become?". A whole-project search, an impact
/// analysis, or a port-order planner needs a different answer — "what
/// depends on what?" — over the same items, and needs it BEFORE and
/// INDEPENDENTLY of any successful import, including for programs the
/// importer rejects. So this is a second, purely analytical pass over the
/// same clang ASTs, sharing the importer's clang-driving shell
/// (`EmitRust/ClangProjectParser.h`) and its symbol naming
/// (`EmitRust/CSymbolNaming.h`) but none of its IR-building state.
///
/// Two invariants the rest of the system may rely on:
///
///  1. NODE KEYS ARE EMITTED SYMBOLS. A node's `symbol` is the name the
///     importer will actually give the emitted Rust item — including the
///     `c_main` rename, the Rust-keyword member mangle, the W2.0 namespace
///     flattening (`ns_<name>_` per level, `ns_anon_` for an anonymous
///     namespace), and the per-TU `tu<i>_` tag on internal-linkage
///     (`static`) symbols. This is not mirrored logic: `CImporter` and this
///     file call the SAME functions (`cFunctionSymbolName`,
///     `cGlobalSymbolName`, `recordRustName`), so a graph node and an
///     emitted item are the same thing by construction.
///
///  2. TOTAL DETERMINISM. Every vector this file hands out is sorted, and
///     the sort is a total order on content only — never on pointer values,
///     never on hash-table iteration order. `nodes` is ordered by
///     (`symbol`, `tuIndex`); `edges` by (`from`, `kind`, `to`). No
///     `DenseMap`/`StringMap` iteration order reaches the boundary. Two
///     runs over the same sources produce byte-identical `print()` output.
///
/// Print format (`ItemGraph::print`, and `emitrust-cc --emit=item-graph`),
/// one item per line, every node line before every edge line:
///
/// \code
/// node <symbol> kind=<function|record|enum|global> def=<0|1> \
///      linkage=<extern|intern> tu=<i> loc=<file>:<line>:<col>
/// edge <symbol> -> <symbol> kind=<Calls|CallsIndirect|SigType|BodyType|\
///      Field|Base|ReadsGlobal|WritesGlobal|TakesAddressOf>
/// \endcode
///
/// (each real line is unwrapped — there are exactly two line shapes, and
/// every field is a single `key=value` token separated by one space, so
/// `grep '^node '`, `grep ' kind=Calls$'` and FileCheck patterns all work
/// on whole tokens.) The `-> <symbol>` of a `CallsIndirect` edge is the
/// fixed token `?`, the one target position that is not a node key.
///
/// Scope and deliberate omissions, so callers know what the graph does NOT
/// claim:
///  - Records/enums whose emitted name depends on accumulated import state
///    are named by their C spelling only: the tag-versus-ordinary-identifier
///    collision rename (`Struct_<tag>`), the shape-keyed `Anon<n>` naming of
///    bare anonymous records, and the `<function>_<tag>` block-scope mangle
///    all live in `CImporter::structSymbolName`/`importRecord` and are not
///    reproduced here. Consequently only NAMED, FILE-SCOPE records and enums
///    become nodes; an anonymous or block-scope one is skipped entirely
///    rather than given a name that might not match the emitted item.
///  - C++ member functions are not nodes. `importDeclsIn` never reaches them
///    (they are declared inside a `CXXRecordDecl`, not at item scope), and
///    their emitted name comes from `CImporter::cxxMethodMangledName`, which
///    depends on the class's assigned struct name. A `class` still becomes a
///    record node, and its `Base` edges are recorded.
///  - The graph is CLOSED: an edge is emitted only when BOTH endpoints are
///    nodes of this graph (`CallsIndirect`, which has no target, is the sole
///    exception). Calls into system headers, references to block-scope tags,
///    and uses of anything the two rules above skip therefore produce no
///    edge rather than a dangling one, so a consumer never has to handle a
///    target it cannot look up.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_PROJECT_ITEMGRAPH_H
#define EMITRUST_PROJECT_ITEMGRAPH_H

#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace clang {
class ASTUnit;
} // namespace clang

namespace mlir {
namespace emitrust {

/// The kind of program item a node stands for — one per top-level item the
/// importer emits, so that the four kinds partition the emitted Rust items.
enum class ItemKind {
  /// A function definition or prototype (`emitrust.func`/`func.func`).
  Function,
  /// A `struct`, `union`, or C++ `class` definition (`emitrust.struct_def`).
  Record,
  /// A named `enum` definition (`emitrust.enum_def`).
  Enum,
  /// A file-scope variable (`emitrust.global`).
  Global,
};

/// Whether an item's C linkage makes it a whole-program symbol or a
/// translation-unit-private one. This drives the node key: an internal
/// item's symbol carries the `tu<i>_` tag, so two file-`static`s spelled
/// the same in different TUs are distinct nodes, while one external symbol
/// declared in a shared header is a single node however many TUs see it.
enum class ItemLinkage {
  /// External linkage: unified program-wide under its bare C name.
  External,
  /// Internal linkage (`static` at file scope, or an anonymous namespace).
  Internal,
};

/// The kind of dependency an edge records. The edge always points from the
/// depending item to the item it depends on.
enum class EdgeKind {
  /// A `CallExpr` in the source item's body resolves to the target function.
  Calls,
  /// A `CallExpr` in the source item's body has no resolvable callee (a call
  /// through a function pointer). Recorded with NO target — the one edge
  /// shape whose target position is not a node key — rather than dropped,
  /// because "this function makes an indirect call" is precisely the fact a
  /// port-order or devirtualization consumer needs, and dropping it would
  /// make an item with only indirect calls look leaf-like.
  CallsIndirect,
  /// A record or enum named in the source item's signature: the return type
  /// or any parameter type of a function; the declared type of a global.
  SigType,
  /// A record or enum named inside a function body: by a local declaration,
  /// by a cast, or as the operand of `sizeof`/`_Alignof`.
  BodyType,
  /// A record's field type mentions the target record or enum.
  Field,
  /// A C++ record's base class (direct bases only; a grandparent is reached
  /// by following the base's own `Base` edge).
  Base,
  /// A global variable is read from the source item's body or initializer.
  /// Taking a global's address counts as a read: it is a reference to the
  /// object that does not itself store to it.
  ReadsGlobal,
  /// A global variable is assigned, compound-assigned, or incremented/
  /// decremented in the source item's body. A compound assignment or an
  /// increment records BOTH `WritesGlobal` and `ReadsGlobal`, because it is
  /// genuinely both.
  WritesGlobal,
  /// The source item mentions the target function outside callee position,
  /// i.e. takes its address (`&f` or the bare `f` that decays to a pointer).
  TakesAddressOf,
};

/// The stable spelling of `kind` used in `node ... kind=<...>` lines:
/// lowercase, one word.
llvm::StringRef itemKindName(ItemKind kind);

/// The stable spelling of `linkage` used in `node ... linkage=<...>` lines:
/// `extern` or `intern`.
llvm::StringRef itemLinkageName(ItemLinkage linkage);

/// The stable spelling of `kind` used in `edge ... kind=<...>` lines:
/// the CamelCase enumerator name.
llvm::StringRef edgeKindName(EdgeKind kind);

/// One program item.
struct ItemNode {
  /// The emitted Rust item name; the node's identity, unique in a graph.
  std::string symbol;
  /// Which of the four item kinds this is.
  ItemKind kind;
  /// Whether the project contains a DEFINITION of this item, as opposed to
  /// only a prototype (function) or an `extern` declaration (global).
  /// Records and enums are nodes only when defined, so this is always true
  /// for them.
  bool isDefinition;
  /// The item's C linkage.
  ItemLinkage linkage;
  /// The index of the translation unit this node is attributed to: the TU
  /// holding the definition when there is one, else the lowest-numbered TU
  /// declaring the item. Indices are positions in the path list handed to
  /// `buildItemGraph`, matching the `tu<i>_` tag of internal symbols.
  unsigned tuIndex;
  /// Presumed source file of `line`/`column` (macro expansions resolve to
  /// the expansion's presumed location, exactly like importer diagnostics).
  std::string file;
  /// 1-based source line of the declaration this node's location came from
  /// — the definition when the item has one.
  unsigned line;
  /// 1-based source column of that declaration.
  unsigned column;
};

/// One directed dependency between two items.
struct ItemEdge {
  /// The depending item's symbol; always a node of the same graph.
  std::string from;
  /// The depended-on item's symbol; always a node of the same graph, except
  /// for `EdgeKind::CallsIndirect`, where it is empty and prints as `?`.
  std::string to;
  /// What kind of dependency this is.
  EdgeKind kind;
};

/// The whole-project item graph: a deterministic, fully sorted value.
///
/// Duplicate edges are collapsed: an edge is identified by
/// (`from`, `to`, `kind`), so a function calling `g` five times contributes
/// one `Calls` edge. Multiplicity is deliberately not modelled — it is a
/// property of the call sites, not of the item dependency, and keeping it
/// would make the graph unstable under trivial source edits.
struct ItemGraph {
  /// Every item, ordered by (`symbol`, `tuIndex`). `symbol` alone is already
  /// unique; `tuIndex` is in the comparator so the order is total by
  /// construction and stays so if the key ever widens.
  std::vector<ItemNode> nodes;
  /// Every dependency, ordered by (`from`, `kind`, `to`).
  std::vector<ItemEdge> edges;

  /// Renders the graph in the line format documented at the top of this
  /// file: all `node` lines, then all `edge` lines, each newline-terminated.
  /// Pure; depends on nothing but `nodes` and `edges`.
  std::string print() const;
};

/// Builds the item graph of an already-parsed project.
///
/// `units` are the translation units, in project order: `units[i]`'s items
/// carry `tuIndex == i` and its internal-linkage symbols carry the tag
/// `tu<i>_`, matching `importCProject`. A single unit is tagged `tu0_`
/// nonetheless — unlike the importer, whose single-file entry point
/// (`importC`) suppresses the tag; the graph is a project-level artifact
/// and has no single-file mode, so its symbols always name the multi-file
/// import's items. (`emitrust-cc` always goes through `importCProject`, so
/// for that driver the two always agree.)
///
/// This is a pure function of the ASTs: it builds nothing, mutates nothing,
/// and emits no diagnostics for unsupported constructs — an item the
/// importer would REJECT still appears in the graph, which is the point.
/// Failure is reserved for a genuine internal error (a null unit).
///
/// \param units the parsed translation units, in project order.
/// \returns the graph, or failure if any unit is null.
FailureOr<ItemGraph> buildItemGraph(llvm::ArrayRef<clang::ASTUnit *> units);

/// Parses `paths` as an independent translation unit each (through the same
/// clang-driving shell `importCProject` uses, so per-input C/C++ language
/// selection and `extraClangArgs` behave identically) and builds their item
/// graph.
///
/// \param paths the source files of the project, in order; the index of a
///        path is the `tuIndex` of the items it declares.
/// \param extraClangArgs additional clang command-line arguments applied to
///        every translation unit (include paths etc.).
/// \returns the graph, or failure if any input failed to parse (clang has
///          already printed the parse diagnostics to stderr).
FailureOr<ItemGraph> buildItemGraph(llvm::ArrayRef<std::string> paths,
                                    llvm::ArrayRef<std::string> extraClangArgs);

/// `buildItemGraph` driven by a `compile_commands.json` (FR-45).
///
/// The graph must see EXACTLY the project the importer would see, so it
/// takes the database through the same shell (`buildProjectASTs`): each
/// input's language and flags come from its recorded entry, and an empty
/// `paths` with a database given means "every file the database lists".
/// With an empty `compilationDatabasePath` this is the overload above.
///
/// \param compilationDatabasePath a directory holding a
///        `compile_commands.json`, the JSON file itself, or empty for none.
/// \param error receives the reason when the database cannot be loaded.
/// \returns the graph, or failure (with `error` set only for a database
///          load failure; parse diagnostics go to stderr as usual).
FailureOr<ItemGraph> buildItemGraph(llvm::ArrayRef<std::string> paths,
                                    llvm::ArrayRef<std::string> extraClangArgs,
                                    llvm::StringRef compilationDatabasePath,
                                    std::string &error);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_PROJECT_ITEMGRAPH_H
