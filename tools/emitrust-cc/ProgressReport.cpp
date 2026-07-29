//===- ProgressReport.cpp - Pure per-item porting report rendering --------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the FR-44 functional core declared in ProgressReport.h: the
/// join of item graph, rejection ledger and emitted symbol table, and the two
/// pure renderers (`PORTING.md`, `emitrust-progress.json`).
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
                         item.blockerTag, item.stubbed});
  }
  return collapsed;
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

/// Renders one `| ... |` markdown row for `item`.
void renderMarkdownRow(llvm::raw_ostream &os,
                       const emitrustcc::ProgressItem &item) {
  os << "| " << emitrustcc::itemStatusName(item.status) << " | "
     << emitrustcc::itemStatusColor(item.status) << " | `"
     << markdownCell(item.symbol) << "` | "
     << (item.kind.empty() ? "-" : markdownCell(item.kind)) << " | "
     << (item.blockerTag.empty() ? "-" : markdownCell(item.blockerTag)) << " | "
     << (item.diagnostic.empty() ? "-" : markdownCell(item.diagnostic)) << " | "
     << markdownCell(locationText(item)) << " |\n";
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
     << "      \"diagnostic\": \"" << jsonEscape(item.diagnostic) << "\"\n"
     << "    }";
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

std::vector<std::pair<std::string, unsigned>>
ProgressReport::blockerRanking() const {
  std::map<std::string, unsigned> counts;
  for (const std::vector<ProgressItem> *bucket : {&items, &offGraphItems})
    for (const ProgressItem &item : *bucket)
      if (!item.blockerTag.empty())
        ++counts[item.blockerTag];
  std::vector<std::pair<std::string, unsigned>> ranking(counts.begin(),
                                                        counts.end());
  llvm::stable_sort(ranking, [](const auto &a, const auto &b) {
    if (a.second != b.second)
      return a.second > b.second;
    return a.first < b.first;
  });
  return ranking;
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
                                   llvm::ArrayRef<RejectedItem> rejected,
                                   const llvm::StringSet<> &emittedSymbols) {
  ProgressReport report;
  report.crateName = crateName.str();
  report.haveItemGraph = graph != nullptr;

  std::vector<CollapsedRejection> collapsed = collapseRejections(rejected);

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
      }
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
        "(`stubbed`) or absent entirely (`dropped`). Work down the blocker\n"
        "table: the tags are ranked by how many items each one is holding "
        "back.\n\n";

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

  std::vector<std::pair<std::string, unsigned>> ranking =
      report.blockerRanking();
  if (!ranking.empty()) {
    os << "## Blockers, most items first\n\n";
    os << "| blocker | items |\n| --- | ---: |\n";
    for (const auto &[tag, count] : ranking)
      os << "| " << markdownCell(tag) << " | " << count << " |\n";
    os << "\n";
  }

  os << "## Project items\n\n";
  if (report.items.empty()) {
    os << "_No items in the project item graph._\n\n";
  } else {
    os << "| status | color | symbol | kind | blocker | diagnostic | location "
          "|\n"
       << "| --- | --- | --- | --- | --- | --- | --- |\n";
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
          "listed here instead and are NOT part of the fraction above.\n\n";
    os << "| status | color | symbol | kind | blocker | diagnostic | location "
          "|\n"
       << "| --- | --- | --- | --- | --- | --- | --- |\n";
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

  os << "  \"blockers\": [";
  std::vector<std::pair<std::string, unsigned>> ranking =
      report.blockerRanking();
  if (ranking.empty()) {
    os << "],\n";
  } else {
    os << "\n";
    for (size_t index = 0, end = ranking.size(); index != end; ++index)
      os << "    { \"tag\": \"" << jsonEscape(ranking[index].first)
         << "\", \"count\": " << ranking[index].second << " }"
         << (index + 1 == end ? "\n" : ",\n");
    os << "  ],\n";
  }

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
