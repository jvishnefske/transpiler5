//===- SectionTable.h - section-registered test entry points ----*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-160 Phase C: recover a C project's SECTION-REGISTERED test table from
/// the sources, so `--test-entry-section=<name>` can synthesise the FR-160
/// entry list with no build system in the loop.
///
/// The pattern this recognises is the one systemd uses
/// (`src/shared/tests.h`): `TEST(name)` expands to a file-scope
/// `_section_("SYSTEMD_TEST_TABLE") static const TestFunc <unique> = { &fn,
/// STRINGIFY(fn), ... }`, and `DEFINE_TEST_MAIN` walks
/// `__start_/__stop_SYSTEMD_TEST_TABLE` at run time. The registration lives
/// entirely in the linker section, so the entry OBJECT's own name is
/// generated and never stable: the section attribute is the only key.
///
/// Why this is a second, purely analytical parse rather than a query over
/// the converted module — the posture the FR-40 item graph already takes.
/// The registration objects do not survive import in any readable shape:
/// with a `main` in the unit they sink into `c_main` as `emitrust.variable`
/// locals, and with no `main` the FR-62 actor lift rewrites them into
/// `struct_def`/`impl` pairs. Both shapes destroy the section string, and
/// which one happens depends on an unrelated property of the translation
/// unit. Parsing the sources again also makes the scan independent of
/// import SUCCESS: an entry whose record type the importer rejects still
/// registers its function.
///
/// Two invariants this shares with the item graph, for the same reason:
///
///  1. SYMBOLS ARE EMITTED SYMBOLS. A collected function is named through
///     `EmitRust/CSymbolNaming.h`'s `cFunctionSymbolName` with the same
///     `tu<i>_` tag the importer's project entry point applies, so the
///     derived name IS the emitted name by construction. Re-deriving it
///     from the C spelling desyncs under FR-53's idiomatic rename (a C
///     `CamelCase` emits as `camel_case`) — measured, not hypothesised.
///
///  2. DETERMINISM. The result is in registration order (translation unit
///     order, then declaration order, then initializer traversal order),
///     deduplicated on first occurrence. Dedup is mandatory rather than
///     tidy: clang traverses an `InitListExpr` twice (syntactic and
///     semantic form), so systemd's `(union f) &(func)` entries are each
///     collected twice, and FR-53 name folding can map two distinct C
///     functions onto one emitted symbol.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_PROJECT_SECTIONTABLE_H
#define EMITRUST_PROJECT_SECTIONTABLE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Support/LLVM.h"

#include <string>

namespace clang {
class ASTUnit;
} // namespace clang

namespace mlir {
namespace emitrust {

/// Collects the emitted symbols of every function whose address appears in
/// the initializer of a file-scope object carrying `section`, over the
/// already-parsed `units`.
///
/// A unit's index is its `tu<i>_` tag, exactly as `importCProject` and
/// `buildItemGraph` assign it, so the caller MUST pass the same unit list in
/// the same order the import consumed.
///
/// Only file-scope objects with an initializer are considered. A
/// `SectionAttr` on a FUNCTION is not a registration (and clang rejects a
/// section holding both code and data anyway), and a block-scope static is
/// not walked: the pattern is a file-scope table by construction.
///
/// \param units the parsed translation units, in project order.
/// \param section the section name to match, verbatim as written in the
///        attribute (clang does not decorate it).
/// \param matchedObjects incremented once per object carrying `section`,
///        so a caller can tell "the table is empty" from "no such table".
/// \returns the emitted symbols, in registration order, deduplicated.
llvm::SmallVector<std::string>
collectSectionTestEntries(llvm::ArrayRef<clang::ASTUnit *> units,
                          llvm::StringRef section, unsigned &matchedObjects);

/// `collectSectionTestEntries` driven by the shared clang shell: parses
/// `paths` (or the whole compilation database when `paths` is empty) through
/// `buildProjectASTs` — the same shell the importer and the item graph use,
/// so the translation-unit order, and therefore every `tu<i>_` tag, is the
/// one the emitted module carries — and scans the result.
///
/// Failure is reserved for a parse that did not produce a usable project:
/// a compilation-database load failure, a nonzero `buildProjectASTs` status
/// or a null unit. A caller must be LOUD about it rather than proceeding
/// with an empty entry list, which would be a silent vacuous pass.
///
/// \param paths the source files to parse; empty means "the whole database".
/// \param extraClangArgs additional clang arguments applied to every input.
/// \param compilationDatabasePath a directory holding a
///        `compile_commands.json`, the JSON file itself, or empty for none.
/// \param section the section name to match.
/// \param matchedObjects receives the number of objects carrying `section`.
/// \param error receives the reason on failure; untouched otherwise.
/// \returns the emitted symbols in registration order, or failure with
///          `error` set.
FailureOr<llvm::SmallVector<std::string>>
collectSectionTestEntries(llvm::ArrayRef<std::string> paths,
                          llvm::ArrayRef<std::string> extraClangArgs,
                          llvm::StringRef compilationDatabasePath,
                          llvm::StringRef section, unsigned &matchedObjects,
                          std::string &error);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_PROJECT_SECTIONTABLE_H
