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
