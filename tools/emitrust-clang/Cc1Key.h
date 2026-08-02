//===- Cc1Key.h - FR-57 cc1 cache-key canonicalization ----------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Pure canonicalization of a clang `-cc1` argument vector into the FR-57
/// cache-key form: workflow-only arguments — those that change between
/// otherwise-identical compiles (output paths, depfile plumbing, working
/// directories) without affecting the Rust result — are removed before
/// hashing, so that a rebuild into a different object path reuses the cache
/// while any semantic or layout flag change misses it.
///
/// The blacklist is the one enumerated EMPIRICALLY by the FR-58 spike
/// (design.md, FR-57 SPIKE 2): `-o`, `-dependency-file`, `-MT`,
/// `-main-file-name`, `-sys-header-deps`, `-fdebug-compilation-dir=`,
/// `-fcoverage-compilation-dir=`. The positional input path is also removed:
/// the key must be invariant to WHERE the source lives; WHAT it contains is
/// the separate `src-hash` half of the FR-57 key pair.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CLANG_CC1KEY_H
#define EMITRUST_TOOLS_EMITRUST_CLANG_CC1KEY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace emitrust {

/// Canonicalizes a `-cc1` argument vector for FR-57 cache-key hashing.
///
/// Removes, per the empirically measured workflow-noise blacklist:
///   - `-o`, `-dependency-file`, `-MT`, `-main-file-name` — the flag AND its
///     value argument;
///   - `-sys-header-deps` — the single flag;
///   - `-fdebug-compilation-dir=...`, `-fcoverage-compilation-dir=...` —
///     joined-form single arguments, matched by prefix;
///   - the positional input path (cc1 places its single input after the
///     `-x <language>` pair; the pair itself is semantic and stays).
///
/// Everything else is kept verbatim, in order: clang's cc1 argument order is
/// deterministic for a given driver line, so order is signal, not noise.
///
/// \param cc1 the raw cc1 argument vector (leading "-cc1" included).
/// \returns the canonicalized vector, ready to hash.
inline std::vector<std::string>
canonicalizeCc1Args(llvm::ArrayRef<const char *> cc1) {
  std::vector<std::string> canonical;
  canonical.reserve(cc1.size());
  bool sawLanguage = false;
  for (size_t i = 0, e = cc1.size(); i != e; ++i) {
    llvm::StringRef arg(cc1[i]);
    if (arg == "-o" || arg == "-dependency-file" || arg == "-MT" ||
        arg == "-main-file-name") {
      if (i + 1 != e)
        ++i; // the value falls with its flag
      continue;
    }
    if (arg == "-sys-header-deps")
      continue;
    if (arg.starts_with("-fdebug-compilation-dir=") ||
        arg.starts_with("-fcoverage-compilation-dir="))
      continue;
    if (arg == "-x") {
      // `-x <language>` is semantic and stays; it also marks where cc1
      // places its single positional input.
      sawLanguage = true;
      canonical.push_back(arg.str());
      if (i + 1 != e)
        canonical.push_back(cc1[++i]);
      continue;
    }
    if (!arg.starts_with("-") &&
        (sawLanguage || i + 1 == e)) {
      // The positional input path: after the `-x <language>` pair, or —
      // defensively, should a cc1 line ever omit `-x` — a bare trailing
      // argument. Its PATH is workflow; its CONTENT is the src-hash.
      continue;
    }
    canonical.push_back(arg.str());
  }
  return canonical;
}

} // namespace emitrust

#endif // EMITRUST_TOOLS_EMITRUST_CLANG_CC1KEY_H
