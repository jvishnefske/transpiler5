//===- RejectionLedger.cpp - Recoverable-import rejection ledger -*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the FR-42 rejection ledger and its blocker-tag heuristic: the
/// pure, IR-free half of recoverable import. Nothing here touches the clang
/// AST or the MLIR module — it maps diagnostic STRINGS to categories and
/// renders a report — which is why it lives in its own file rather than in
/// the importer's already very large translation units.
///
/// The heuristic is a direct port of `classify_blocker` in
/// `test/RealWorld/run_realworld.py`, table for table and in the same order:
/// the system-header symbol, `_BLOCKER_SUBSTRINGS`, `_CXX_BLOCKER_SUBSTRINGS`,
/// `_NODE_NAMED_RE`, `_AMBIGUOUS_POINTER`, then `other`.
/// Two tags sit OUTSIDE that shared sequence and are tested before it,
/// because both name wordings the Python twin can never observe (it reads a
/// non-recovering whole-program run's stderr): `search-excluded` (FR-43's
/// synthetic exclusion) and `cxx-cascaded-method` (FR-49's cascade, which
/// only a recovering import can reach). Each is documented at its test.
/// It is duplicated rather than shared
/// because the two run in different languages at different times (a Python
/// survey over subprocess stderr vs. an in-process C++ import), and the ONE
/// thing that must stay in lockstep is the tag vocabulary, not the code. Any
/// change to the table below must be mirrored there, and vice versa; the
/// comments name the Python counterpart of each rule so the pairing is
/// discoverable from either side.
///
/// Rejected alternative: classifying on structured data (a diagnostic ID or
/// an enum threaded through every `emitError` call site) instead of on the
/// message text. That would be sturdier, but the importer raises rejections
/// from several hundred sites with no such id, and the RealWorld survey can
/// only ever see the text — so a text heuristic is the only classification
/// the two ledgers can actually agree on today.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/ImportC.h"

#include "mlir/IR/Location.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

using namespace mlir;

namespace {

/// The allocator family, split out of the generic `libc:<name>` tag exactly
/// as the survey does (`_DYNMEM_NAMES`): heap use is the project's single
/// most common blocker and is tracked as its own category.
bool isDynamicMemoryName(llvm::StringRef name) {
  return name == "malloc" || name == "calloc" || name == "realloc" ||
         name == "free" || name == "aligned_alloc";
}

/// Substrings that identify an allocation on the CITED SOURCE LINE, used to
/// disambiguate the shared pointer wordings below (`_ALLOC_KEYWORDS`).
/// `free` is absent on purpose: a `free(p)` line never produces one of those
/// wordings, and the substring would match `free_list` and friends.
bool citedLineAllocates(llvm::StringRef line) {
  return line.contains("malloc") || line.contains("calloc") ||
         line.contains("realloc") || line.contains("aligned_alloc");
}

/// The ordered substring table (`_BLOCKER_SUBSTRINGS`). First match wins, so
/// the order is part of the contract: `returned pointer value` must beat the
/// generic pointer wordings, and the entries are listed in the survey's
/// order so a diff against the Python table is a line-for-line read.
struct BlockerSubstring {
  llvm::StringLiteral needle;
  llvm::StringLiteral tag;
};
constexpr BlockerSubstring kBlockerSubstrings[] = {
    {llvm::StringLiteral("use of main's argv"), llvm::StringLiteral("argv")},
    {llvm::StringLiteral("pointer-to-pointer"),
     llvm::StringLiteral("ptr-to-ptr")},
    {llvm::StringLiteral("returned pointer value"),
     llvm::StringLiteral("returned-pointer")},
    {llvm::StringLiteral("pointer return type"),
     llvm::StringLiteral("returned-pointer")},
    {llvm::StringLiteral("return sites disagree"),
     llvm::StringLiteral("returned-pointer")},
    {llvm::StringLiteral("global pointer bound to a string literal"),
     llvm::StringLiteral("global-string-cursor")},
    {llvm::StringLiteral("unsupported: allocation"),
     llvm::StringLiteral("dynamic-memory")},
    {llvm::StringLiteral("pointer struct member of an externally"),
     llvm::StringLiteral("pointer-member-cross-tu")},
    {llvm::StringLiteral("static-binding model"),
     llvm::StringLiteral("self-ref-pointer-member")},
    {llvm::StringLiteral("variadic function"),
     llvm::StringLiteral("variadic-cross-tu")},
};

/// The C++-input table (`_CXX_BLOCKER_SUBSTRINGS`), consulted after the table
/// above and in the survey's order. Every wording here is raised only from a
/// C++-only code path (a `CXXRecordDecl` walk, `mapType`'s reference case, or
/// the STL recognition table), so a C input can never match one and the C
/// corpus's tabulation is unaffected by this table's existence.
constexpr BlockerSubstring kCxxBlockerSubstrings[] = {
    {llvm::StringLiteral("base classes are not supported"),
     llvm::StringLiteral("cxx-inheritance")},
    {llvm::StringLiteral("user-declared destructor"),
     llvm::StringLiteral("cxx-destructor")},
    {llvm::StringLiteral("virtual or unresolved member call"),
     llvm::StringLiteral("cxx-virtual-call")},
    {llvm::StringLiteral("unsupported: virtual method"),
     llvm::StringLiteral("cxx-virtual")},
    {llvm::StringLiteral("overloaded operator"),
     llvm::StringLiteral("cxx-operator-overload")},
    // FR-48 landed reference PARAMETERS; the four wordings below are the
    // residual reference positions, all still tagged `cxx-references` so
    // the backlog keeps ranking them as one blocker. "rvalue reference
    // types are not yet supported" needs no entry of its own — it ends
    // with the generic wording and matches it as a substring.
    {llvm::StringLiteral("reference return types are not yet supported"),
     llvm::StringLiteral("cxx-references")},
    {llvm::StringLiteral("reference struct members are not yet supported"),
     llvm::StringLiteral("cxx-references")},
    {llvm::StringLiteral("reference-to-pointer parameter"),
     llvm::StringLiteral("cxx-references")},
    {llvm::StringLiteral("reference-to-array parameter"),
     llvm::StringLiteral("cxx-references")},
    {llvm::StringLiteral("reference types are not yet supported"),
     llvm::StringLiteral("cxx-references")},
    {llvm::StringLiteral("is not a recognized STL type"),
     llvm::StringLiteral("stl-unrecognized-type")},
    {llvm::StringLiteral("is not a recognized STL method"),
     llvm::StringLiteral("stl-unrecognized-method")},
    {llvm::StringLiteral("receiver is not a recognized STL"),
     llvm::StringLiteral("stl-unrecognized-receiver")},
    {llvm::StringLiteral("unsupported top-level declaration"),
     llvm::StringLiteral("unsupported-top-level-decl")},
};

/// The generic dispatch fallbacks that NAME the offending clang AST node
/// class (`_NODE_NAMED_RE`). These are language-agnostic — a C input reaches
/// them too — and refining them from the catch-all `other` into a node-named
/// tag is what makes the tabulation a directly actionable backlog rather than
/// one giant bucket. The survey uses `(\w+)` after each prefix; the same
/// character class is spelled out here.
struct NodeNamedPrefix {
  llvm::StringLiteral needle;
  llvm::StringLiteral tagPrefix;
};
constexpr NodeNamedPrefix kNodeNamedPrefixes[] = {
    {llvm::StringLiteral("unsupported assignable expression: "),
     llvm::StringLiteral("unsupported-assign-expr:")},
    {llvm::StringLiteral("unsupported expression: "),
     llvm::StringLiteral("unsupported-expr:")},
    {llvm::StringLiteral("unsupported statement: "),
     llvm::StringLiteral("unsupported-stmt:")},
};

/// The `\w` character class the survey's node-named patterns use.
bool isWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

/// Reads the source line `loc` points at, or an empty string when the file
/// cannot be read. The survey's `_cited_source_line` does the same over the
/// `file:line:col` prefix it parses back out of the message; in-process the
/// location is already structured, so no parsing is needed.
std::string citedSourceLine(Location loc) {
  auto fileLoc = llvm::dyn_cast<FileLineColLoc>(loc);
  if (!fileLoc)
    return {};
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
      llvm::MemoryBuffer::getFile(fileLoc.getFilename().getValue());
  if (!buffer)
    return {};
  llvm::StringRef text = (*buffer)->getBuffer();
  unsigned wanted = fileLoc.getLine();
  if (wanted == 0)
    return {};
  for (unsigned current = 1; !text.empty(); ++current) {
    auto [line, rest] = text.split('\n');
    if (current == wanted)
      return line.str();
    if (rest.size() == text.size())
      break; // No newline left: `line` was the last one and did not match.
    text = rest;
  }
  return {};
}

} // namespace

std::string mlir::emitrust::classifyBlocker(llvm::StringRef diagnostic,
                                            Location loc) {
  // FR-43's synthetic rejection, tested FIRST and outside the shared table.
  // It is the one wording that does not come from a C construct at all: the
  // item was excluded by a search state, so no blocker of the project's is to
  // blame and no heuristic below could say anything true about it. The tag is
  // deliberately absent from `classify_blocker` in run_realworld.py — that
  // twin tags whole-PROGRAM rejections observed through a subprocess, and a
  // search exclusion is never one — so the two vocabularies stay in step.
  static constexpr llvm::StringLiteral kSearchExcluded =
      "excluded by the search state";
  if (diagnostic.starts_with(kSearchExcluded))
    return "search-excluded";

  // FR-49: the CASCADE wording, tested second and, like the one above,
  // outside the shared table. `importFunction` raises it when a C++ member
  // function's class has no assigned struct name, which can only happen
  // AFTER the class was rejected and dropped — i.e. only under recovering
  // import (FR-42/FR-43). A non-recovering whole-program run dies on the
  // class itself and never reaches the method, so `classify_blocker` in
  // run_realworld.py — which tags whole-PROGRAM rejections observed through
  // a subprocess — can never see this wording, exactly as it can never see
  // `search-excluded`. Adding it to the shared C++ table would therefore add
  // an entry the Python twin could never exercise; it is kept here instead,
  // and the two vocabularies stay in step.
  //
  // The tag names the item as a SYMPTOM on purpose: the construct actually
  // to blame is whatever sank the class, and FR-49's root attribution in
  // ProgressReport.h credits it there through the class's FR-41 chain. This
  // tag is what the item reports when no chain is available at all.
  static constexpr llvm::StringLiteral kUnimportedClassMethod =
      "method of an unimported class";
  if (diagnostic.contains(kUnimportedClassMethod))
    return "cxx-cascaded-method";

  // System-header rejections name the symbol they tripped over; the name is
  // the most informative tag available, so it is parsed out first
  // (`_SYS_HEADER_RE`). The wording is fixed by `rejectSystemHeaderUse`.
  static constexpr llvm::StringLiteral kSysHeadPrefix = "call to '";
  static constexpr llvm::StringLiteral kSysHeadSuffix =
      "' declared in a system header";
  if (size_t start = diagnostic.find(kSysHeadPrefix);
      start != llvm::StringRef::npos) {
    llvm::StringRef rest = diagnostic.drop_front(start + kSysHeadPrefix.size());
    if (size_t end = rest.find(kSysHeadSuffix); end != llvm::StringRef::npos) {
      llvm::StringRef name = rest.take_front(end);
      if (isDynamicMemoryName(name))
        return "dynamic-memory";
      return ("libc:" + name).str();
    }
  }

  for (const BlockerSubstring &entry : kBlockerSubstrings)
    if (diagnostic.contains(entry.needle))
      return entry.tag.str();

  for (const BlockerSubstring &entry : kCxxBlockerSubstrings)
    if (diagnostic.contains(entry.needle))
      return entry.tag.str();

  // The node-named fallbacks: `<prefix><NodeClass>` becomes
  // `<tag-prefix><NodeClass>`, the node class being the run of word
  // characters that follows.
  for (const NodeNamedPrefix &entry : kNodeNamedPrefixes) {
    size_t start = diagnostic.find(entry.needle);
    if (start == llvm::StringRef::npos)
      continue;
    llvm::StringRef rest = diagnostic.drop_front(start + entry.needle.size());
    size_t end = 0;
    while (end != rest.size() && isWordChar(rest[end]))
      ++end;
    if (end == 0)
      continue; // No node class actually named; fall through as the survey's
                // regex does when it fails to match.
    return (entry.tagPrefix + rest.take_front(end)).str();
  }

  // Two wordings are raised by several unrelated blockers (a local bound to
  // a `malloc` result, a `strchr` result, and a genuinely unanalyzable
  // pointer all reach them), so they are refined by what the cited source
  // line actually does (`_AMBIGUOUS_POINTER`).
  if (diagnostic.contains("pointer assigned a non-address value") ||
      diagnostic.contains("with no known target object")) {
    std::string source = citedSourceLine(loc);
    if (citedLineAllocates(source))
      return "dynamic-memory";
    if (llvm::StringRef(source).contains("strchr") ||
        llvm::StringRef(source).contains("strrchr"))
      return "strchr-result-bind";
    return "pointer-local-nonaddress";
  }
  return "other";
}

std::map<std::string, unsigned> mlir::emitrust::RejectionLedger::tally() const {
  std::map<std::string, unsigned> counts;
  for (const RejectedItem &item : items)
    ++counts[item.blockerTag];
  return counts;
}

void mlir::emitrust::RejectionLedger::printSummary(llvm::raw_ostream &os)
    const {
  if (items.empty())
    return;
  os << "recovered " << items.size()
     << (items.size() == 1 ? " rejected top-level item:\n"
                           : " rejected top-level items:\n");
  for (const RejectedItem &item : items) {
    os << "  ";
    if (auto fileLoc = llvm::dyn_cast<FileLineColLoc>(item.loc))
      os << fileLoc.getFilename().getValue() << ":" << fileLoc.getLine() << ":"
         << fileLoc.getColumn() << ": ";
    os << (item.stubbed ? "stubbed" : "dropped") << " '" << item.symbol
       << "' [" << item.blockerTag << "] " << item.diagnostic << "\n";
  }
  os << "blocker tabulation (recovered items by tag):\n";
  for (const auto &[tag, count] : tally())
    os << "  " << tag << " " << count << "\n";
}
