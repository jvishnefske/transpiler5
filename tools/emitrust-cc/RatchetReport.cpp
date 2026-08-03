//===- RatchetReport.cpp - FR-60 rejection report + ratchet manifest ------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements RatchetReport.h. Everything is deterministic string work over
/// decoded shard facts: std::map keeps every tabulation ordered without a
/// sort at the boundary, mirroring `RejectionLedger::tally`.
//
//===----------------------------------------------------------------------===//

#include "RatchetReport.h"

#include "mlir/IR/Location.h"

#include <set>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace emitrustcc;

std::string emitrustcc::normalizeDiagnostic(llvm::StringRef diagnostic) {
  std::string normalized;
  normalized.reserve(diagnostic.size());
  size_t i = 0;
  while (i < diagnostic.size()) {
    if (diagnostic[i] == '\'') {
      size_t close = diagnostic.find('\'', i + 1);
      if (close != llvm::StringRef::npos) {
        normalized += "'<name>'";
        i = close + 1;
        continue;
      }
    }
    normalized += diagnostic[i++];
  }
  return normalized;
}

/// `file:line:col` of a rejection's location, or "<unknown>" for a
/// non-file location.
static std::string formatLocation(mlir::Location loc) {
  if (auto fileLoc = llvm::dyn_cast<mlir::FileLineColLoc>(loc)) {
    std::string text;
    llvm::raw_string_ostream os(text);
    os << fileLoc.getFilename().getValue() << ":" << fileLoc.getLine() << ":"
       << fileLoc.getColumn();
    return text;
  }
  return "<unknown>";
}

std::string
emitrustcc::renderRejectionReport(llvm::ArrayRef<ShardFacts> shards) {
  struct TagFacts {
    unsigned items = 0;
    std::set<std::string> tus;
    std::map<std::string, unsigned> wordings;
    std::string firstLocation;
  };
  std::map<std::string, TagFacts> byTag;
  unsigned total = 0;
  unsigned rejectingShards = 0;
  for (const ShardFacts &shard : shards) {
    if (!shard.rejections.empty())
      ++rejectingShards;
    for (const mlir::emitrust::RejectedItem &item : shard.rejections) {
      TagFacts &facts = byTag[item.blockerTag];
      ++facts.items;
      ++total;
      facts.tus.insert(shard.sourcePath);
      ++facts.wordings[normalizeDiagnostic(item.diagnostic)];
      if (facts.firstLocation.empty())
        facts.firstLocation = formatLocation(item.loc);
    }
  }

  // Rank: item count descending, then tag name.
  std::vector<const std::pair<const std::string, TagFacts> *> ranked;
  for (const auto &entry : byTag)
    ranked.push_back(&entry);
  llvm::stable_sort(ranked, [](const auto *a, const auto *b) {
    if (a->second.items != b->second.items)
      return a->second.items > b->second.items;
    return a->first < b->first;
  });

  std::string report;
  llvm::raw_string_ostream os(report);
  os << "rejection report: " << total << " rejected item"
     << (total == 1 ? "" : "s") << " in " << rejectingShards << " of "
     << shards.size() << " translation units\n";
  for (auto [index, entry] : llvm::enumerate(ranked)) {
    const auto &[tag, facts] = *entry;
    os << "rank " << (index + 1) << ". " << tag << "  items=" << facts.items
       << "  tus=" << facts.tus.size() << "\n";
    os << "  first: " << facts.firstLocation << "\n";
    std::vector<const std::pair<const std::string, unsigned> *> wordings;
    for (const auto &wording : facts.wordings)
      wordings.push_back(&wording);
    llvm::stable_sort(wordings, [](const auto *a, const auto *b) {
      if (a->second != b->second)
        return a->second > b->second;
      return a->first < b->first;
    });
    for (const auto *wording : wordings)
      os << "  " << wording->second << "x " << wording->first << "\n";
  }
  return report;
}

/// The common directory prefix of every shard's source path (whole path
/// components only); empty when the shards share nothing.
static std::string commonDirPrefix(llvm::ArrayRef<ShardFacts> shards) {
  std::string prefix;
  bool first = true;
  for (const ShardFacts &shard : shards) {
    std::string dir(llvm::sys::path::parent_path(shard.sourcePath));
    if (first) {
      prefix = dir;
      first = false;
      continue;
    }
    // Trim `prefix` to the longest leading run of whole components shared
    // with `dir`.
    while (!prefix.empty() && !(llvm::StringRef(dir).starts_with(prefix) &&
                                (dir.size() == prefix.size() ||
                                 dir[prefix.size()] == '/')))
      prefix = std::string(llvm::sys::path::parent_path(prefix));
  }
  return prefix;
}

RatchetManifest
emitrustcc::buildRatchetManifest(llvm::ArrayRef<ShardFacts> shards,
                                 unsigned crates,
                                 unsigned condensationWarnings) {
  RatchetManifest manifest;
  manifest.crates = crates;
  manifest.condensationWarnings = condensationWarnings;
  std::string prefix = commonDirPrefix(shards);
  for (const ShardFacts &shard : shards) {
    unsigned stubbed = 0;
    for (const mlir::emitrust::RejectedItem &item : shard.rejections)
      if (item.stubbed)
        ++stubbed;
    unsigned admitted =
        shard.definitionCount >= stubbed ? shard.definitionCount - stubbed : 0;
    llvm::StringRef key = shard.sourcePath;
    if (!prefix.empty() && key.starts_with(prefix) &&
        key.size() > prefix.size() && key[prefix.size()] == '/')
      key = key.drop_front(prefix.size() + 1);
    manifest.shards.emplace_back(
        key.str(), std::make_pair(
                       admitted, static_cast<unsigned>(shard.rejections.size())));
    manifest.admittedTotal += admitted;
    manifest.rejectedTotal += shard.rejections.size();
    for (const mlir::emitrust::RejectedItem &item : shard.rejections)
      ++manifest.tags[item.blockerTag];
  }
  return manifest;
}

std::string
emitrustcc::renderRatchetManifest(const RatchetManifest &manifest) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << "emitrust ratchet manifest v1\n"
     << "admitted-total: " << manifest.admittedTotal << "\n"
     << "rejected-total: " << manifest.rejectedTotal << "\n"
     << "crates: " << manifest.crates << "\n"
     << "condensation-warnings: " << manifest.condensationWarnings << "\n";
  for (const auto &[key, counts] : manifest.shards)
    os << "shard '" << key << "': admitted=" << counts.first
       << " rejected=" << counts.second << "\n";
  for (const auto &[tag, count] : manifest.tags)
    os << "tag '" << tag << "': " << count << "\n";
  return text;
}

bool emitrustcc::parseRatchetManifest(llvm::StringRef text,
                                      RatchetManifest &manifest,
                                      std::string &error) {
  bool sawHeader = false;
  auto parseCount = [&](llvm::StringRef value, unsigned &out) {
    if (value.trim().getAsInteger(10, out)) {
      error = ("unparsable count '" + value + "'").str();
      return false;
    }
    return true;
  };
  for (llvm::StringRef line : llvm::split(text, '\n')) {
    line = line.trim();
    if (line.empty() || line.starts_with("#"))
      continue;
    if (!sawHeader) {
      if (line != "emitrust ratchet manifest v1") {
        error = "missing 'emitrust ratchet manifest v1' header";
        return false;
      }
      sawHeader = true;
      continue;
    }
    if (line.consume_front("admitted-total:")) {
      if (!parseCount(line, manifest.admittedTotal))
        return false;
    } else if (line.consume_front("rejected-total:")) {
      if (!parseCount(line, manifest.rejectedTotal))
        return false;
    } else if (line.consume_front("crates:")) {
      if (!parseCount(line, manifest.crates))
        return false;
    } else if (line.consume_front("condensation-warnings:")) {
      if (!parseCount(line, manifest.condensationWarnings))
        return false;
    } else if (line.consume_front("shard '")) {
      auto [key, rest] = line.split("':");
      unsigned admitted = 0, rejected = 0;
      for (llvm::StringRef token : llvm::split(rest.trim(), ' ')) {
        if (token.consume_front("admitted=")) {
          if (!parseCount(token, admitted))
            return false;
        } else if (token.consume_front("rejected=")) {
          if (!parseCount(token, rejected))
            return false;
        }
      }
      manifest.shards.emplace_back(key.str(),
                                   std::make_pair(admitted, rejected));
    } else if (line.consume_front("tag '")) {
      auto [tag, rest] = line.split("':");
      unsigned count = 0;
      if (!parseCount(rest, count))
        return false;
      manifest.tags[tag.str()] = count;
    } else {
      error = ("unrecognized manifest line '" + line + "'").str();
      return false;
    }
  }
  if (!sawHeader) {
    error = "empty manifest";
    return false;
  }
  return true;
}

llvm::SmallVector<std::string>
emitrustcc::compareRatchetManifests(const RatchetManifest &baseline,
                                    const RatchetManifest &current) {
  llvm::SmallVector<std::string> regressions;
  if (current.admittedTotal < baseline.admittedTotal)
    regressions.push_back(
        ("admitted-total shrank from " + llvm::Twine(baseline.admittedTotal) +
         " to " + llvm::Twine(current.admittedTotal))
            .str());
  std::map<llvm::StringRef, std::pair<unsigned, unsigned>> currentShards;
  for (const auto &[key, counts] : current.shards)
    currentShards[key] = counts;
  for (const auto &[key, counts] : baseline.shards) {
    auto it = currentShards.find(key);
    if (it == currentShards.end()) {
      regressions.push_back("shard '" + key +
                            "' is in the baseline but not in this build");
      continue;
    }
    if (it->second.first < counts.first)
      regressions.push_back(("shard '" + key + "' admitted shrank from " +
                             llvm::Twine(counts.first) + " to " +
                             llvm::Twine(it->second.first))
                                .str());
  }
  if (current.condensationWarnings > baseline.condensationWarnings)
    regressions.push_back(("condensation-warnings grew from " +
                           llvm::Twine(baseline.condensationWarnings) +
                           " to " + llvm::Twine(current.condensationWarnings))
                              .str());
  return regressions;
}

llvm::SmallVector<std::string>
emitrustcc::ratchetImprovements(const RatchetManifest &baseline,
                                const RatchetManifest &current) {
  llvm::SmallVector<std::string> improvements;
  if (current.admittedTotal > baseline.admittedTotal)
    improvements.push_back(
        ("admitted-total grew from " + llvm::Twine(baseline.admittedTotal) +
         " to " + llvm::Twine(current.admittedTotal))
            .str());
  return improvements;
}
