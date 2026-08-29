//===- RustCasing.h - C identifier to Rust casing conventions ---*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The pure string-casing primitives of the FR-53 idiomatic rename:
/// `snake_case`, `SCREAMING_SNAKE_CASE`, and `UpperCamelCase` conversions of
/// a C identifier.
///
/// Split out of CSymbolNaming.h (whose primitives are functions of the clang
/// AST and which therefore includes clang headers) so that clang-free
/// consumers can share the EXACT byte-for-byte derivation: the FR-70
/// external-requirement lowering pass derives a global requirement's trait
/// accessor names (`g_config` / `set_g_config` from the emitted symbol
/// `G_CONFIG` or `g_config`) inside lib/Conversion, which links MLIR only.
/// Duplicating the casing there would let the importer's emitted symbols and
/// the pass's derived item names drift -- the same silent-divergence failure
/// mode CSymbolNaming.h exists to prevent -- so the functions live here once
/// and CSymbolNaming.h re-exports them by inclusion.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_RUSTCASING_H
#define EMITRUST_RUSTCASING_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace mlir {
namespace emitrust {

/// FR-53 idiomatic rename. Process-wide because the SAME naming primitives
/// feed independent driver paths that must agree byte-for-byte: the importer
/// that creates MLIR/Rust items, the FR-40 item graph that runs its own clang
/// parse with no importer in scope, and (FR-140) the Rust emitter, which is
/// clang-free and lives in lib/Target. A single source of truth makes drift
/// between them impossible; a per-call parameter threaded through every path
/// could silently diverge on a missed site. It is set once, at startup, by
/// the driver (`emitrust-cc`; default = rename ON, disabled by
/// `--preserve-c-names`). Tools that do not set it (e.g. `emitrust-import-c`)
/// keep verbatim C spellings, so their golden tests are unaffected.
///
/// Declared here rather than in CSymbolNaming.h (its home until FR-140)
/// because that header includes clang and the emitter must not; CSymbolNaming.h
/// includes this one, so every existing caller is unaffected.
inline bool &idiomaticRenameEnabled() {
  static bool enabled = false;
  return enabled;
}

/// Would rustc's `non_snake_case` lint fire on an item named `name`?
///
/// Mirrors rustc's own `is_snake_case`: leading and trailing underscores are
/// ignored, and what remains may hold no uppercase letter and no DOUBLED
/// underscore. The doubled-underscore half is the one that bites a faithful
/// transpile -- `m__em` is perfectly legal C and the emitter preserves the C
/// spelling verbatim -- so `a__` and `_lead` are clean while `m__em` is not.
///
/// Two consumers share it and must not drift (FR-140): `renderCargoToml`
/// decides with it whether the emitted manifest may deny the lint on the
/// CRATE NAME (FR-139), and the Rust emitter decides with it whether an item
/// needs `#[allow(non_snake_case)]` for a name it renders inside.
///
/// The two sibling naming lints need no such predicate, measured: a type name
/// goes through `toUpperCamelCase`, which DROPS every underscore, so
/// `non_camel_case_types` can never see a doubled run; and
/// `non_upper_case_globals` only ever complains about lowercase characters,
/// which underscores are not.
inline bool tripsNonSnakeCase(llvm::StringRef name) {
  llvm::StringRef core = name.trim('_');
  if (core.empty())
    return false;
  bool previousWasUnderscore = false;
  for (char c : core) {
    if (c >= 'A' && c <= 'Z')
      return true;
    if (c == '_') {
      if (previousWasUnderscore)
        return true;
      previousWasUnderscore = true;
    } else {
      previousWasUnderscore = false;
    }
  }
  return false;
}

/// Converts a C identifier to `snake_case`: a `_` is inserted before every
/// uppercase letter that begins a new word (one following a lowercase letter
/// or a digit, or one that ends an acronym -- an uppercase followed by a
/// lowercase), and all letters are lowercased. Existing underscores are
/// preserved, so an already-snake name is unchanged. Used for function and
/// struct-field names (FR-53 idiomatic rename) and for the FR-70 global
/// requirement accessor names.
static inline std::string toSnakeCase(llvm::StringRef name) {
  std::string out;
  out.reserve(name.size() + 4);
  for (size_t i = 0, e = name.size(); i < e; ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      char prev = i > 0 ? name[i - 1] : '\0';
      char next = i + 1 < e ? name[i + 1] : '\0';
      bool prevLower = prev >= 'a' && prev <= 'z';
      bool prevDigit = prev >= '0' && prev <= '9';
      bool prevUpper = prev >= 'A' && prev <= 'Z';
      bool nextLower = next >= 'a' && next <= 'z';
      if (i > 0 && (prevLower || prevDigit || (prevUpper && nextLower)))
        out.push_back('_');
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

/// Converts a C identifier to `SCREAMING_SNAKE_CASE` (snake-case, uppercased).
/// Used for global/static/const names and enum-variant associated constants.
static inline std::string toScreamingSnakeCase(llvm::StringRef name) {
  std::string s = toSnakeCase(name);
  for (char &c : s)
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  return s;
}

/// Converts a C identifier to `UpperCamelCase`: the snake-case word boundaries
/// are removed and each word is capitalized. Used for struct/enum type names
/// (including synthesized `Owner_<fn>_<base>` owners, which become
/// `OwnerFnBase`).
static inline std::string toUpperCamelCase(llvm::StringRef name) {
  std::string snake = toSnakeCase(name);
  std::string out;
  out.reserve(snake.size());
  bool capitalizeNext = true;
  for (char c : snake) {
    if (c == '_') {
      capitalizeNext = true;
      continue;
    }
    if (capitalizeNext && c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
    capitalizeNext = false;
    out.push_back(c);
  }
  return out;
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_RUSTCASING_H
