//===- ProgressReport.cpp - Pure per-item porting report rendering --------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the FR-44 functional core declared in ProgressReport.h: the
/// join of item graph, rejection ledger, emitted symbol table and (FR-49)
/// item coloring, and the two pure renderers (`PORTING.md`,
/// `emitrust-progress.json`).
//
//===----------------------------------------------------------------------===//

#include "ProgressReport.h"

#include "mlir/IR/Location.h"
#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

using namespace mlir;
using namespace mlir::emitrust;

namespace {

/// Splits a rejection's location into (file, line, column). A location that
/// is not a `FileLineColLoc` — which the ledger documents as not happening in
/// practice — yields an empty file and zero line/column rather than an
/// invented one.
std::tuple<std::string, unsigned, unsigned> splitLoc(Location loc) {
  if (auto fileLoc = llvm::dyn_cast<FileLineColLoc>(loc))
    return {fileLoc.getFilename().getValue().str(), fileLoc.getLine(),
            fileLoc.getColumn()};
  return {std::string(), 0u, 0u};
}

/// One rejection after the multi-TU collapse: the ledger records the same
/// header declaration once per translation unit that parses it, and those
/// repeats are identical in every field that matters.
struct CollapsedRejection {
  std::string symbol;
  std::string file;
  unsigned line;
  unsigned column;
  std::string diagnostic;
  std::string blockerTag;
  bool stubbed;
  /// FR-49's join key to the coloring for an off-graph item: the item-graph
  /// node key of the enclosing C++ class, empty for everything else.
  std::string ownerSymbol;
};

/// Collapses `rejected` on (symbol, file, line, column), keeping the first
/// occurrence's diagnostic and tag and OR-ing `stubbed`: if any translation
/// unit managed to emit a stub, the merged module contains that stub.
/// Declaration-walk order is preserved, which is what makes the report's
/// "first rejection wins" tie-breaks deterministic.
std::vector<CollapsedRejection>
collapseRejections(llvm::ArrayRef<RejectedItem> rejected) {
  std::vector<CollapsedRejection> collapsed;
  std::map<std::tuple<std::string, std::string, unsigned, unsigned>, size_t>
      seen;
  for (const RejectedItem &item : rejected) {
    auto [file, line, column] = splitLoc(item.loc);
    auto key = std::make_tuple(item.symbol, file, line, column);
    auto [pos, inserted] = seen.try_emplace(key, collapsed.size());
    if (!inserted) {
      collapsed[pos->second].stubbed |= item.stubbed;
      continue;
    }
    collapsed.push_back({item.symbol, file, line, column, item.diagnostic,
                         item.blockerTag, item.stubbed, item.ownerSymbol});
  }
  return collapsed;
}

/// FR-49's lookup side of the coloring join: `symbol` to its `ColoredItem`,
/// or null. `ItemColoring::items` is already sorted by symbol, but a
/// `std::map` is built rather than binary-searched because the same map is
/// consulted once per item and once per off-graph item, and because the map
/// makes the absence case (a symbol the coloring never saw) explicit.
using ColorIndex = std::map<std::string, const ColoredItem *>;

ColorIndex indexColoring(const ItemColoring *coloring) {
  ColorIndex index;
  if (!coloring)
    return index;
  for (const ColoredItem &item : coloring->items)
    index.emplace(item.symbol, &item);
  return index;
}

const ColoredItem *lookupColor(const ColorIndex &index,
                               llvm::StringRef symbol) {
  if (symbol.empty())
    return nullptr;
  auto found = index.find(symbol.str());
  return found == index.end() ? nullptr : found->second;
}

/// Fills `item`'s FR-49 root attribution.
///
/// `own` is the item's own coloring entry (a graph item) and `ownerColor` the
/// entry of the class it belongs to (an off-graph member function); at most
/// one is ever non-null. The rule is the same in both cases — the root is the
/// `construct` at the end of the chain — and the fallback is the same too: an
/// item with no usable chain is its own root, so `rootBlockerTag` mirrors the
/// direct tag rather than reading `unknown`.
///
/// `item.blockerTag` need NOT be set (FR-115): an item with no ledger entry
/// of its own — a `missing` template instantiation whose one located
/// rejection joined a different symbol — can still sit on a computed poison
/// chain, and discarding that chain is how 624 corpus items used to read
/// blank. When the tag is empty the item still takes the coloring's root and
/// chain if one exists; only an item with neither a tag nor a color is left
/// with everything empty.
void attributeRoot(emitrustcc::ProgressItem &item, const ColoredItem *own,
                   const ColoredItem *ownerColor,
                   llvm::StringRef ownerSymbol) {
  if (item.blockerTag.empty()) {
    // FR-115: a silent item (never in the ledger) still gets the coloring's
    // root when the coloring has one, instead of staying blank.
    if (own && !own->construct.empty()) {
      item.rootBlockerTag = own->construct;
      item.blameChain = own->chain;
    }
    return;
  }
  if (own && !own->construct.empty()) {
    item.rootBlockerTag = own->construct;
    item.blameChain = own->chain;
    return;
  }
  if (ownerColor && !ownerColor->construct.empty()) {
    item.rootBlockerTag = ownerColor->construct;
    // The method is not on its class's chain (it is not a node at all), so
    // it is PREPENDED: the rendered chain reads
    // `area_x100 -> Rect`, i.e. "this method, through this class, to this
    // root", and the step from the method to its class is the one edge the
    // graph does not model.
    item.blameChain.push_back(item.symbol);
    llvm::append_range(item.blameChain, ownerColor->chain);
    item.attributedVia = ownerSymbol.str();
    return;
  }
  // No chain: the item is its own root.
  item.rootBlockerTag = item.blockerTag;
  item.blameChain.push_back(item.symbol);
}

/// Escapes `text` for a JSON string literal: the two mandatory escapes, the
/// five short forms, and `\u00XX` for the remaining control characters.
/// Bytes >= 0x80 are passed through unchanged — the inputs are clang
/// diagnostics and source paths, already UTF-8.
std::string jsonEscape(llvm::StringRef text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xf];
        out += kHex[static_cast<unsigned char>(c) & 0xf];
      } else {
        out += c;
      }
    }
  }
  return out;
}

/// Escapes `text` for a GitHub-flavored markdown TABLE CELL: `|` would end
/// the cell and a backslash would eat the escape, so both are escaped, and
/// embedded newlines (which a multi-line diagnostic could carry) become
/// spaces so the row stays one line.
std::string markdownCell(llvm::StringRef text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '\\' || c == '|')
      out += '\\';
    if (c == '\n' || c == '\r')
      out += ' ';
    else
      out += c;
  }
  return out;
}

/// `permille` as a percentage with one decimal, e.g. 250 -> "25.0". Integer
/// arithmetic only: the artifacts must not contain locale- or
/// rounding-dependent text.
std::string permilleAsPercent(unsigned permille) {
  return std::to_string(permille / 10) + "." + std::to_string(permille % 10);
}

/// The location of `item` as `file:line:col`, or `-` when it has none.
std::string locationText(const emitrustcc::ProgressItem &item) {
  if (item.file.empty())
    return "-";
  return item.file + ":" + std::to_string(item.line) + ":" +
         std::to_string(item.column);
}

/// The blame chain as `a -> b -> c`, or `-` when there is none. The arrow is
/// spaced so a long chain still wraps inside its markdown cell.
std::string chainText(llvm::ArrayRef<std::string> chain) {
  if (chain.empty())
    return "-";
  std::string text;
  for (size_t index = 0, end = chain.size(); index != end; ++index) {
    if (index != 0)
      text += " -> ";
    text += chain[index];
  }
  return text;
}

/// Renders one `| ... |` markdown row for `item`.
void renderMarkdownRow(llvm::raw_ostream &os,
                       const emitrustcc::ProgressItem &item) {
  os << "| " << emitrustcc::itemStatusName(item.status) << " | "
     << emitrustcc::itemStatusColor(item.status) << " | `"
     << markdownCell(item.symbol) << "` | "
     << (item.kind.empty() ? "-" : markdownCell(item.kind)) << " | "
     << (item.rootBlockerTag.empty() ? "-" : markdownCell(item.rootBlockerTag))
     << " | " << markdownCell(chainText(item.blameChain)) << " | "
     << (item.blockerTag.empty() ? "-" : markdownCell(item.blockerTag)) << " | "
     << (item.diagnostic.empty() ? "-" : markdownCell(item.diagnostic)) << " | "
     << markdownCell(locationText(item)) << " |\n";
}

/// The header and separator of the per-item markdown tables, shared by the
/// graph and off-graph tables so their columns cannot drift apart.
constexpr llvm::StringLiteral kItemTableHeader =
    "| status | color | symbol | kind | root blocker | blame chain | blocker "
    "| diagnostic | location |\n"
    "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";

/// Renders one ranked blocker table, or nothing when the ranking is empty.
void renderRankingTable(llvm::raw_ostream &os, llvm::StringRef columnName,
                        llvm::ArrayRef<std::pair<std::string, unsigned>>
                            ranking) {
  if (ranking.empty())
    return;
  os << "| " << columnName << " | items |\n| --- | ---: |\n";
  for (const auto &[tag, count] : ranking)
    os << "| " << markdownCell(tag) << " | " << count << " |\n";
  os << "\n";
}

/// Renders one item object of the JSON `items`/`off_graph_items` arrays,
/// indented by four spaces and WITHOUT a trailing comma (the caller adds it).
void renderJsonItem(llvm::raw_ostream &os,
                    const emitrustcc::ProgressItem &item) {
  os << "    {\n"
     << "      \"symbol\": \"" << jsonEscape(item.symbol) << "\",\n"
     << "      \"kind\": \"" << jsonEscape(item.kind) << "\",\n"
     << "      \"status\": \"" << emitrustcc::itemStatusName(item.status)
     << "\",\n"
     << "      \"color\": \"" << emitrustcc::itemStatusColor(item.status)
     << "\",\n"
     << "      \"linkage\": \"" << jsonEscape(item.linkage) << "\",\n"
     << "      \"tu\": " << item.tuIndex << ",\n"
     << "      \"file\": \"" << jsonEscape(item.file) << "\",\n"
     << "      \"line\": " << item.line << ",\n"
     << "      \"column\": " << item.column << ",\n"
     << "      \"blocker\": \"" << jsonEscape(item.blockerTag) << "\",\n"
     << "      \"diagnostic\": \"" << jsonEscape(item.diagnostic) << "\",\n"
     << "      \"root_blocker\": \"" << jsonEscape(item.rootBlockerTag)
     << "\",\n"
     << "      \"attributed_via\": \"" << jsonEscape(item.attributedVia)
     << "\",\n"
     << "      \"blame_chain\": [";
  for (size_t index = 0, end = item.blameChain.size(); index != end; ++index)
    os << (index == 0 ? "" : ", ") << "\"" << jsonEscape(item.blameChain[index])
       << "\"";
  os << "]\n"
     << "    }";
}

/// Renders one `{ "tag": ..., "count": ... }` array, already ranked, as the
/// value of `name`. Always followed by a comma: both call sites are followed
/// by further keys.
void renderRankingJson(llvm::raw_ostream &os, llvm::StringRef name,
                       llvm::ArrayRef<std::pair<std::string, unsigned>>
                           ranking) {
  os << "  \"" << name << "\": [";
  if (ranking.empty()) {
    os << "],\n";
    return;
  }
  os << "\n";
  for (size_t index = 0, end = ranking.size(); index != end; ++index)
    os << "    { \"tag\": \"" << jsonEscape(ranking[index].first)
       << "\", \"count\": " << ranking[index].second << " }"
       << (index + 1 == end ? "\n" : ",\n");
  os << "  ],\n";
}

} // namespace

namespace emitrustcc {

llvm::StringRef itemStatusName(ItemStatus status) {
  switch (status) {
  case ItemStatus::Ported:
    return "ported";
  case ItemStatus::Stubbed:
    return "stubbed";
  case ItemStatus::Dropped:
    return "dropped";
  case ItemStatus::Missing:
    return "missing";
  case ItemStatus::Declared:
    return "declared";
  }
  return "declared";
}

llvm::StringRef itemStatusColor(ItemStatus status) {
  switch (status) {
  case ItemStatus::Ported:
    return "green";
  case ItemStatus::Stubbed:
    return "yellow";
  case ItemStatus::Dropped:
    return "red";
  case ItemStatus::Missing:
    return "orange";
  case ItemStatus::Declared:
    return "grey";
  }
  return "grey";
}

unsigned itemStatusRank(ItemStatus status) {
  switch (status) {
  case ItemStatus::Dropped:
    return 0;
  case ItemStatus::Stubbed:
    return 1;
  case ItemStatus::Missing:
    return 2;
  case ItemStatus::Ported:
    return 3;
  case ItemStatus::Declared:
    return 4;
  }
  return 4;
}

unsigned ProgressReport::count(ItemStatus status) const {
  unsigned total = 0;
  for (const ProgressItem &item : items)
    if (item.status == status)
      ++total;
  return total;
}

unsigned ProgressReport::portableItems() const {
  return static_cast<unsigned>(items.size()) - count(ItemStatus::Declared);
}

unsigned ProgressReport::portedPermille() const {
  unsigned portable = portableItems();
  if (portable == 0)
    return 0;
  return 1000u * count(ItemStatus::Ported) / portable;
}

/// Tallies one string field over both buckets and ranks it by count
/// descending then tag ascending. The counting map is a `std::map` so the
/// vector handed to the sort is already in tag order and the stable sort's
/// tie-break is therefore total on content — no hash-map order reaches the
/// artifacts.
static std::vector<std::pair<std::string, unsigned>>
rankTags(llvm::ArrayRef<ProgressItem> items,
         llvm::ArrayRef<ProgressItem> offGraphItems,
         std::string ProgressItem::*field) {
  std::map<std::string, unsigned> counts;
  for (llvm::ArrayRef<ProgressItem> bucket : {items, offGraphItems})
    for (const ProgressItem &item : bucket)
      if (!(item.*field).empty())
        ++counts[item.*field];
  std::vector<std::pair<std::string, unsigned>> ranking(counts.begin(),
                                                        counts.end());
  llvm::stable_sort(ranking, [](const auto &a, const auto &b) {
    if (a.second != b.second)
      return a.second > b.second;
    return a.first < b.first;
  });
  return ranking;
}

std::vector<std::pair<std::string, unsigned>>
ProgressReport::blockerRanking() const {
  return rankTags(items, offGraphItems, &ProgressItem::blockerTag);
}

std::vector<std::pair<std::string, unsigned>>
ProgressReport::rootBlockerRanking() const {
  return rankTags(items, offGraphItems, &ProgressItem::rootBlockerTag);
}

llvm::StringSet<> collectEmittedSymbols(ModuleOp module) {
  llvm::StringSet<> symbols;
  for (Operation &op : module.getBody()->getOperations())
    if (auto symbol = llvm::dyn_cast<SymbolOpInterface>(&op))
      symbols.insert(symbol.getName());
  return symbols;
}

ProgressReport buildProgressReport(llvm::StringRef crateName,
                                   const ItemGraph *graph,
                                   const ItemColoring *coloring,
                                   llvm::ArrayRef<RejectedItem> rejected,
                                   const llvm::StringSet<> &emittedSymbols) {
  ProgressReport report;
  report.crateName = crateName.str();
  report.haveItemGraph = graph != nullptr;

  std::vector<CollapsedRejection> collapsed = collapseRejections(rejected);
  // The coloring is keyed by the graph's node symbols, so it is only
  // meaningful alongside a graph; a `ledger-only` run has no denominator and
  // no chains either, and every item there falls back to its own tag.
  ColorIndex colors = indexColoring(graph ? coloring : nullptr);

  // Which collapsed rejections belong to a graph node, and which do not. A
  // node absorbs every rejection carrying its symbol: a definition and the
  // prototype in its header reject at different locations but are one item.
  llvm::StringSet<> nodeSymbols;
  if (graph)
    for (const ItemNode &node : graph->nodes)
      nodeSymbols.insert(node.symbol);

  llvm::StringMap<std::vector<const CollapsedRejection *>> bySymbol;
  for (const CollapsedRejection &entry : collapsed) {
    if (graph && nodeSymbols.contains(entry.symbol)) {
      bySymbol[entry.symbol].push_back(&entry);
      continue;
    }
    ProgressItem item;
    item.symbol = entry.symbol;
    item.status = entry.stubbed ? ItemStatus::Stubbed : ItemStatus::Dropped;
    item.blockerTag = entry.blockerTag;
    item.diagnostic = entry.diagnostic;
    item.file = entry.file;
    item.line = entry.line;
    item.column = entry.column;
    // FR-49: an off-graph item has no color of its own — it is not a node —
    // so it borrows its enclosing class's chain. `ownerSymbol` is the only
    // available key: the symbol here is a bare member spelling that three
    // sibling classes may share.
    attributeRoot(item, /*own=*/nullptr,
                  lookupColor(colors, entry.ownerSymbol), entry.ownerSymbol);
    report.offGraphItems.push_back(std::move(item));
  }

  if (graph) {
    for (const ItemNode &node : graph->nodes) {
      ProgressItem item;
      item.symbol = node.symbol;
      item.kind = itemKindName(node.kind).str();
      item.linkage = itemLinkageName(node.linkage).str();
      item.tuIndex = node.tuIndex;
      item.file = node.file;
      item.line = node.line;
      item.column = node.column;

      auto found = bySymbol.find(node.symbol);
      if (found != bySymbol.end()) {
        const std::vector<const CollapsedRejection *> &entries = found->second;
        bool stubbed = llvm::any_of(
            entries, [](const CollapsedRejection *e) { return e->stubbed; });
        item.status = stubbed ? ItemStatus::Stubbed : ItemStatus::Dropped;
        // The FIRST rejection in declaration-walk order names the construct a
        // reader should look at; a later one is the same fault seen through
        // another translation unit.
        const CollapsedRejection *primary = entries.front();
        item.blockerTag = primary->blockerTag;
        item.diagnostic = primary->diagnostic;
        item.file = primary->file;
        item.line = primary->line;
        item.column = primary->column;
      } else if (!node.isDefinition) {
        item.status = ItemStatus::Declared;
      } else if (emittedSymbols.contains(node.symbol)) {
        item.status = ItemStatus::Ported;
      } else {
        item.status = ItemStatus::Missing;
        // FR-115: a definition node with no ledger row and no emitted symbol
        // was never VISITED by the import — it was only ever demanded from
        // code that was itself rejected first. The tag is an honest
        // attribution of that fact, not a fabricated diagnostic: `blocker`
        // gets the tag, `diagnostic` stays empty (nothing was ever
        // diagnosed), and `attributeRoot` below either credits a real root
        // from the item's poison chain or self-roots it as unreached. Rides
        // the existing schema; the 5-value `status` vocabulary the RealWorld
        // ratchet parses is untouched.
        item.blockerTag = "unreached-by-import";
      }
      // FR-49: a graph item has its own chain, so it needs no owner.
      attributeRoot(item, lookupColor(colors, node.symbol),
                    /*ownerColor=*/nullptr, /*ownerSymbol=*/"");
      report.items.push_back(std::move(item));
    }
  }

  llvm::stable_sort(report.items,
                    [](const ProgressItem &a, const ProgressItem &b) {
                      unsigned rankA = itemStatusRank(a.status);
                      unsigned rankB = itemStatusRank(b.status);
                      return std::tie(rankA, a.blockerTag, a.symbol) <
                             std::tie(rankB, b.blockerTag, b.symbol);
                    });
  // Off-graph symbols are NOT unique — three classes may each define
  // `area_x100`, and a constructor is spelled like its class — so the
  // location is part of the sort key that makes the order total.
  llvm::stable_sort(report.offGraphItems,
                    [](const ProgressItem &a, const ProgressItem &b) {
                      return std::tie(a.symbol, a.file, a.line, a.column) <
                             std::tie(b.symbol, b.file, b.line, b.column);
                    });
  return report;
}

std::string renderPortingMarkdown(const ProgressReport &report) {
  std::string text;
  llvm::raw_string_ostream os(text);

  os << "# Porting status: `" << report.crateName << "`\n\n";
  os << "Generated by `emitrust-cc --emit=crate --incremental` (FR-44). The\n"
        "crate next to this file BUILDS: every item below that is not "
        "`ported`\n"
        "is either an `unimplemented!()` stub carrying the original signature\n"
        "(`stubbed`) or absent entirely (`dropped`). Work down the ROOT "
        "blocker\n"
        "table: it ranks constructs by how many items each one is actually "
        "holding\n"
        "back, after crediting every cascaded rejection to the construct that\n"
        "caused it (FR-49). The direct table under it is what each item "
        "reported.\n\n";

  if (!report.haveItemGraph) {
    os << "> **The project item graph could not be built**, so there is no\n"
          "> denominator: the table below lists only what the importer\n"
          "> REJECTED, and the absence of an item means nothing. Do not read\n"
          "> a percentage out of this run.\n\n";
  } else {
    unsigned portable = report.portableItems();
    os << "**" << report.count(ItemStatus::Ported) << " of " << portable
       << " item" << (portable == 1 ? "" : "s") << " ported ("
       << permilleAsPercent(report.portedPermille()) << "%).**\n\n";
    os << "| status | count |\n| --- | ---: |\n";
    for (ItemStatus status :
         {ItemStatus::Ported, ItemStatus::Stubbed, ItemStatus::Dropped,
          ItemStatus::Missing, ItemStatus::Declared})
      os << "| " << itemStatusName(status) << " | " << report.count(status)
         << " |\n";
    os << "\n";
  }

  std::vector<std::pair<std::string, unsigned>> rootRanking =
      report.rootBlockerRanking();
  if (!rootRanking.empty()) {
    os << "## Root blockers, most items first\n\n";
    os << "This is the work queue (FR-49). Each unported item is credited "
          "here\n"
          "to the construct at the END of its FR-41 blame chain — the one "
          "whose\n"
          "support would actually unblock it — rather than to the diagnostic "
          "it\n"
          "happened to raise. A cascade therefore counts once against its "
          "cause,\n"
          "not once per symptom. These tags come from the FR-41 "
          "admissibility\n"
          "probe's construct vocabulary (`base-class`, `destructor`, "
          "`template`,\n"
          "...) when a chain was available; an item with no chain is its own "
          "root\n"
          "and keeps its FR-42 blocker tag, so the two vocabularies can both\n"
          "appear in this table and are deliberately not merged.\n\n";
    renderRankingTable(os, "root blocker", rootRanking);
  }

  std::vector<std::pair<std::string, unsigned>> ranking =
      report.blockerRanking();
  if (!ranking.empty()) {
    os << "## Direct blockers, as reported\n\n";
    os << "The same items, credited instead to the diagnostic each one "
          "actually\n"
          "raised. Where this table disagrees with the one above, the "
          "difference\n"
          "is exactly the cascade: these are the symptoms, those are the "
          "causes.\n"
          "Kept because it is what the importer said, and because a project "
          "the\n"
          "coloring has nothing to say about produces the two tables "
          "identical.\n\n";
    renderRankingTable(os, "blocker", ranking);
  }

  os << "## Project items\n\n";
  if (report.items.empty()) {
    os << "_No items in the project item graph._\n\n";
  } else {
    os << kItemTableHeader;
    for (const ProgressItem &item : report.items)
      renderMarkdownRow(os, item);
    os << "\n";
  }

  if (!report.offGraphItems.empty()) {
    os << "## Rejected items outside the item graph\n\n";
    os << "The FR-40 item graph does not model C++ member functions, "
          "anonymous\n"
          "or block-scope records, or records whose emitted name depends on\n"
          "accumulated importer state (see `ItemGraph.h`). Those items are\n"
          "invisible when they succeed, so counting them only when they fail\n"
          "would make a project look worse the more of it ported; they are\n"
          "listed here instead and are NOT part of the fraction above.\n\n"
          "They ARE part of both blocker tables. A member function has no\n"
          "color of its own, so its root blocker is its enclosing CLASS's:\n"
          "the blame chain below reads `<method> -> <class> -> ... -> "
          "<root>`,\n"
          "and its second entry is the class the attribution went through.\n"
          "A method whose class the graph does not model keeps its own tag.\n\n";
    os << kItemTableHeader;
    for (const ProgressItem &item : report.offGraphItems)
      renderMarkdownRow(os, item);
    os << "\n";
  }

  os << "The same data, machine-readable and diffable between two runs, is "
        "in\n`emitrust-progress.json` next to this file.\n";
  return text;
}

std::string renderProgressJson(const ProgressReport &report) {
  std::string text;
  llvm::raw_string_ostream os(text);

  os << "{\n";
  os << "  \"schema\": \"emitrust-progress/1\",\n";
  os << "  \"crate\": \"" << jsonEscape(report.crateName) << "\",\n";
  os << "  \"denominator_source\": \""
     << (report.haveItemGraph ? "item-graph" : "ledger-only") << "\",\n";
  os << "  \"totals\": {\n"
     << "    \"graph_items\": " << report.portableItems() << ",\n"
     << "    \"ported\": " << report.count(ItemStatus::Ported) << ",\n"
     << "    \"stubbed\": " << report.count(ItemStatus::Stubbed) << ",\n"
     << "    \"dropped\": " << report.count(ItemStatus::Dropped) << ",\n"
     << "    \"missing\": " << report.count(ItemStatus::Missing) << ",\n"
     << "    \"declared\": " << report.count(ItemStatus::Declared) << ",\n"
     << "    \"off_graph_rejected\": " << report.offGraphItems.size() << ",\n"
     << "    \"ported_permille\": " << report.portedPermille() << "\n"
     << "  },\n";

  // `blockers` keeps its FR-44 meaning — the DIRECT tally — so an
  // `emitrust-progress/1` consumer reads the same numbers it always did;
  // FR-49's root tally is the new key beside it.
  renderRankingJson(os, "blockers", report.blockerRanking());
  renderRankingJson(os, "root_blockers", report.rootBlockerRanking());

  auto renderArray = [&os](llvm::StringRef name,
                           llvm::ArrayRef<ProgressItem> bucket, bool last) {
    os << "  \"" << name << "\": [";
    if (bucket.empty()) {
      os << "]";
    } else {
      os << "\n";
      for (size_t index = 0, end = bucket.size(); index != end; ++index) {
        renderJsonItem(os, bucket[index]);
        os << (index + 1 == end ? "\n" : ",\n");
      }
      os << "  ]";
    }
    os << (last ? "\n" : ",\n");
  };
  renderArray("items", report.items, /*last=*/false);
  renderArray("off_graph_items", report.offGraphItems, /*last=*/true);

  os << "}\n";
  return text;
}

} // namespace emitrustcc
