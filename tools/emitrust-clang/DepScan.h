//===- DepScan.h - FR-57 dependency scan for the src-hash -------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The dependency half of the FR-57 `src-hash`: the cache key must cover the
/// WHOLE preprocessed input — a header-only edit changes what the import
/// sees, so it must miss the key — yet stay invariant under every kind of
/// depfile workflow noise (`-MF` renames, caller-chosen `-MT` targets, or no
/// depfile request at all).
///
/// Measured spike verdict (design.md FR-57): parsing the depfile the build
/// itself produced is FRAGILE — it only exists when the build asked for one,
/// and its leading target is caller-controlled text (`-MT 'custom target'`
/// puts unescaped spaces, and potentially colons, before the `:`). Driving
/// dependency collection internally is uniform, and since the shim already
/// links clang's frontend, the most robust form needs no subprocess and no
/// depfile syntax at all: rebuild a `CompilerInvocation` from the job's own
/// classified cc1 line, clear its dependency-output and output-file options
/// (the delegated compile already wrote the real ones; the scan must never
/// touch them), and run a `PreprocessOnlyAction` with a
/// `DependencyCollector` attached. The collector's default policy — user
/// files only, no system headers — is exactly the `-MMD` set; system-header
/// content is deliberately out of scope because the system include PATHS are
/// already in the cc1-key half (under nix, a toolchain change is a store-
/// path change and misses the key there).
///
/// The hash itself is an order-independent digest multiset: each
/// dependency's CONTENT is hashed to a per-file MD5, the digests are sorted,
/// and the sorted list is hashed again. Content-only, so an mtime `touch`
/// keeps the key; path-free, so the same sources at another location (or a
/// main file whose path sorts differently against its headers) keep it too.
///
/// Failure direction is pinned by test/Driver/emitrust-clang-src-hash.c: a
/// scan failure or an unreadable dependency yields "no hash", which the
/// shim turns into "no artifact + warning" — never a wrong cache hit.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CLANG_DEPSCAN_H
#define EMITRUST_TOOLS_EMITRUST_CLANG_DEPSCAN_H

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/Utils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace emitrust {

/// Collects the user-file dependency list of one compile job by running
/// clang's preprocessor in-process over the job's own cc1 argument vector.
///
/// The invocation is rebuilt with `CompilerInvocation::CreateFromArgs` from
/// `cc1Args` (the leading "-cc1" skipped), then neutered before it runs:
/// the dependency-output options are cleared so the build's real depfile is
/// never rewritten, and the output file is cleared so the just-produced
/// object cannot be touched. Diagnostics are swallowed entirely — any
/// diagnostic the TU deserves was already printed once by the delegated
/// real clang, and the shim's stderr must stay byte-transparent.
///
/// The returned list is the `DependencyCollector` default set: the main
/// file plus every user header it transitively includes, no system headers
/// (see the file comment for why), each path exactly as the preprocessor
/// resolved it, deduplicated.
///
/// \param cc1Args the compile job's cc1 argument vector, leading "-cc1"
///        included.
/// \returns the dependency paths, or std::nullopt when the invocation
///          cannot be rebuilt or the preprocess run fails — the caller must
///          fail toward "no artifact", never guess.
inline std::optional<std::vector<std::string>>
scanCompileDependencies(llvm::ArrayRef<std::string> cc1Args) {
  std::vector<const char *> argv;
  argv.reserve(cc1Args.size());
  for (const std::string &arg : cc1Args)
    argv.push_back(arg.c_str());
  if (!argv.empty() && llvm::StringRef(argv.front()) == "-cc1")
    argv.erase(argv.begin());

  clang::DiagnosticOptions parseDiagOpts;
  clang::IgnoringDiagConsumer silentConsumer;
  clang::DiagnosticsEngine parseDiags(
      llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), parseDiagOpts,
      &silentConsumer, /*ShouldOwnClient=*/false);

  auto invocation = std::make_shared<clang::CompilerInvocation>();
  if (!clang::CompilerInvocation::CreateFromArgs(*invocation, argv,
                                                 parseDiags))
    return std::nullopt;

  // Neuter every output the cc1 line asked for: the delegated real clang
  // has already produced the object and any depfile, and the scan must not
  // touch either.
  invocation->getDependencyOutputOpts() = clang::DependencyOutputOptions();
  invocation->getFrontendOpts().OutputFile.clear();
  invocation->getFrontendOpts().ProgramAction =
      clang::frontend::RunPreprocessorOnly;

  clang::CompilerInstance instance(std::move(invocation));
  instance.createDiagnostics(*llvm::vfs::getRealFileSystem(), &silentConsumer,
                             /*ShouldOwnClient=*/false);
  if (!instance.hasDiagnostics())
    return std::nullopt;

  auto collector = std::make_shared<clang::DependencyCollector>();
  instance.addDependencyCollector(collector);

  clang::PreprocessOnlyAction action;
  if (!instance.ExecuteAction(action) ||
      instance.getDiagnostics().hasErrorOccurred())
    return std::nullopt;

  return std::vector<std::string>(collector->getDependencies().begin(),
                                  collector->getDependencies().end());
}

/// Hashes the CONTENTS of `deps` into the FR-57 `src-hash`: per-file MD5
/// digests, sorted, then hashed together — an order-independent multiset of
/// content digests, so the hash depends on what the preprocessed input SAYS
/// and on nothing about where it lives or when it was touched.
///
/// `testUnreadable`, when set, makes the dependency whose path or filename
/// equals it count as unreadable — the test-only EMITRUST_TEST_UNREADABLE_DEP
/// hook, needed because no real build can delete a header between the
/// delegated compile and this hash within one shim invocation.
///
/// \param deps the dependency paths from `scanCompileDependencies`.
/// \param testUnreadable test-only path/filename to treat as unreadable.
/// \param unreadablePath receives the offending path on failure.
/// \returns the 32-hex-digit hash, or std::nullopt when any dependency
///          cannot be read (with `unreadablePath` set): a missing input must
///          become "no artifact", never a wrong cache hit.
inline std::optional<std::string>
hashDependencyContents(llvm::ArrayRef<std::string> deps,
                       const std::optional<std::string> &testUnreadable,
                       std::string &unreadablePath) {
  std::vector<llvm::SmallString<32>> digests;
  digests.reserve(deps.size());
  for (const std::string &path : deps) {
    if (testUnreadable &&
        (path == *testUnreadable ||
         llvm::sys::path::filename(path) == *testUnreadable)) {
      unreadablePath = path;
      return std::nullopt;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
        llvm::MemoryBuffer::getFile(path);
    if (!buffer) {
      unreadablePath = path;
      return std::nullopt;
    }
    llvm::MD5 md5;
    md5.update((*buffer)->getBuffer());
    llvm::MD5::MD5Result digest = md5.final();
    llvm::SmallString<32> hex;
    llvm::MD5::stringifyResult(digest, hex);
    digests.push_back(hex);
  }
  llvm::sort(digests);
  llvm::MD5 combined;
  for (const llvm::SmallString<32> &digest : digests) {
    combined.update(digest);
    combined.update(llvm::StringRef("\0", 1));
  }
  llvm::MD5::MD5Result digest = combined.final();
  llvm::SmallString<32> hex;
  llvm::MD5::stringifyResult(digest, hex);
  return std::string(hex);
}

} // namespace emitrust

#endif // EMITRUST_TOOLS_EMITRUST_CLANG_DEPSCAN_H
