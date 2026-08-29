//===- RustPreludeShadow.h - FR-150 prelude-name shadowing ------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-150: an emitted TYPE item whose name collides with a Rust PRELUDE name
/// shadows it for the whole crate, after which every prelude spelling the
/// EMITTER writes resolves to the user's type instead. systemd's
/// `src/shared/options.h` ends `} Option;`, so the importer emits
/// `pub struct Option { .. }` and every `Option<fn(..)>` written for a function
/// pointer becomes rustc E0107 ("struct takes 0 generic arguments but 1 generic
/// argument was supplied"). The tool exits 0 and says nothing: silent
/// unbuildable output, the FR-140/141/142/146 class.
///
/// The fix is CONDITIONAL qualification. The bare spelling is kept normally;
/// only in a crate where an emitted item shadows the name do the emitter's own
/// uses become fully qualified paths. The alternatives were both worse:
///   * unconditional qualification shifts emitted bytes across the ENTIRE
///     corpus -- `--emit=crate` output is pinned byte-for-byte by golden
///     tests, so that is a behavior change, not a cleanup -- and makes every
///     fn-ptr signature unreadable, for a defect that affects a handful of
///     crates;
///   * renaming the user's type is USER-VISIBLE: `pub struct Option` is part
///     of the emitted API surface, and FR-139's `--c-abi-exports` and the
///     lib-crate story both depend on emitted names matching what a caller
///     expects.
/// Conditional qualification is byte-neutral everywhere except the colliding
/// crate, which is exactly the blast radius the defect has.
///
/// Shared between the emitter (lib/Target/Rust) and the emitrust-cc driver
/// (tools/emitrust-cc/CrateEmitter.cpp) for the same reason RustCasing.h is
/// shared: the crate root is rendered in TWO places -- the emitter writes the
/// items, the driver appends the `fn main()` wrapper whose argv collection
/// spells `Vec<Vec<i8>>` -- and a duplicated shadow set would silently drift.
///
/// SAFETY POSTURE. A site this file does not reach keeps today's behavior,
/// which is a LOUD rustc error in the colliding crate, never a miscompile: the
/// change can only turn unbuildable crates into buildable ones. The one thing
/// that WOULD be a defect is qualifying a spelling that meant the user's own
/// type, so the rewrite is deliberately narrow -- see
/// `qualifyShadowedPreludeNames` (string literals are skipped; an identifier
/// already preceded by `::` or `.` is left alone) and the emitter's callee
/// allowlist (`isEmitterPreludePath`), which is what keeps a user static
/// method `Node::default` -- printed through the very same code path as the
/// `Box::new` that IS qualified -- untouched.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_RUSTPRELUDESHADOW_H
#define EMITRUST_RUSTPRELUDESHADOW_H

#include "EmitRust/EmitRustOps.h"

#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

#include <string>

namespace mlir {
namespace emitrust {

/// The fully qualified Rust path for prelude name `name`, or an empty
/// StringRef when `name` is not a prelude name this project can shadow.
///
/// FOUR names, and they are the four the EMITTER genuinely writes (counted
/// over the emitter's own output sites: `Option<` 11, `Box<` 11, `String` 7,
/// `Vec<` 6; `Result<` is 0 today and is therefore deliberately absent -- an
/// entry for a name nothing writes would be untested code).
///
/// `::std::`, not `::core::`/`::alloc::`: emitted crates are std crates
/// (`std::process::exit`, `println!`, `std::io::stdout`), and `alloc` is NOT
/// in the extern prelude of a std crate without an `extern crate alloc;`, so
/// `::alloc::boxed::Box` would fail to resolve. The leading `::` makes the
/// path absolute, so no emitted item can capture its first segment.
inline llvm::StringRef preludeQualifiedPath(llvm::StringRef name) {
  return llvm::StringSwitch<llvm::StringRef>(name)
      .Case("Option", "::std::option::Option")
      .Case("Box", "::std::boxed::Box")
      .Case("String", "::std::string::String")
      .Case("Vec", "::std::vec::Vec")
      .Default(llvm::StringRef());
}

/// The prelude names shadowed by `module`'s emitted TYPE items.
///
/// Detection is on the EMITTED name, never the C spelling: emitted type names
/// go through `toUpperCamelCase`, which DROPS underscores, so C's `my_option`
/// is safe (`MyOption`) while `option`, `OPTION` and `Option` all land on
/// `Option`. Whether the C construct was a struct, an enum, a union or a
/// typedef is likewise irrelevant -- what matters is the item name that
/// reaches the crate, which is exactly what these three op kinds carry.
///
/// Only the TYPE namespace participates. A `global` or a `func` named `Option`
/// would shadow the VALUE namespace, which no emitter-written prelude spelling
/// resolves through (`Option<..>`, `Vec::new()` and `Box::new(..)` are all
/// type-namespace paths), and `Some`/`None` -- value-namespace prelude items
/// the emitter does write -- are not shadowed by a braced struct at all.
inline llvm::StringSet<> collectShadowedPreludeNames(ModuleOp module) {
  llvm::StringSet<> shadowed;
  auto note = [&](llvm::StringRef name) {
    if (!preludeQualifiedPath(name).empty())
      shadowed.insert(name);
  };
  for (auto def : module.getOps<StructDefOp>())
    note(def.getSymName());
  for (auto def : module.getOps<EnumDefOp>())
    note(def.getSymName());
  for (auto def : module.getOps<DataEnumDefOp>())
    note(def.getSymName());
  return shadowed;
}

/// `name` itself when nothing shadows it, its fully qualified path when
/// something does. The bare return is what keeps every non-colliding crate
/// byte-identical.
inline llvm::StringRef preludeSpelling(llvm::StringRef name,
                                       const llvm::StringSet<> &shadowed) {
  return shadowed.contains(name) ? preludeQualifiedPath(name) : name;
}

/// Rewrites every SHADOWED prelude name in `text` to its qualified path.
///
/// For emitter-owned Rust fragments: opaque type spellings (`Vec<i8>`,
/// `Box<Node>`, `Option<i64>`), the verbatim runtime helpers, and the driver's
/// `fn main()` wrapper. Returns `text` unchanged when `shadowed` is empty,
/// which is the byte-neutrality fast path.
///
/// Two narrowings keep it from ever rewriting something that meant the user's
/// own type:
///   * a DOUBLE-QUOTED string literal is copied through untouched -- its bytes
///     are program OUTPUT (a C program that prints "Vec" must keep printing
///     "Vec"), never a path;
///   * an identifier already preceded by `::` or `.` is a path segment or a
///     field/method selector, not a bare prelude reference.
inline std::string
qualifyShadowedPreludeNames(llvm::StringRef text,
                            const llvm::StringSet<> &shadowed) {
  if (shadowed.empty())
    return text.str();
  auto isIdentStart = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  auto isIdentChar = [&](char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
  };
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    char c = text[i];
    if (c == '"') {
      // Copy the whole literal, honouring backslash escapes so an escaped
      // quote does not end it early.
      out += c;
      ++i;
      while (i < n) {
        if (text[i] == '\\' && i + 1 < n) {
          out += text[i];
          out += text[i + 1];
          i += 2;
          continue;
        }
        out += text[i];
        bool closing = text[i] == '"';
        ++i;
        if (closing)
          break;
      }
      continue;
    }
    if (!isIdentStart(c)) {
      out += c;
      ++i;
      continue;
    }
    size_t start = i;
    while (i < n && isIdentChar(text[i]))
      ++i;
    llvm::StringRef ident = text.substr(start, i - start);
    bool alreadyQualified =
        (start >= 2 && text.substr(start - 2, 2) == "::") ||
        (start >= 1 && text[start - 1] == '.');
    if (!alreadyQualified && shadowed.contains(ident))
      out += preludeQualifiedPath(ident);
    else
      out += ident;
  }
  return out;
}

/// The closed set of `Type::assoc` paths the EMITTER and the IMPORTER mint for
/// the prelude families, spelled exactly as they are created
/// (ImportCStatements.cpp `Box::new`/`String::from`/`String::new`/`Vec::new`/
/// `Vec::from_iter`, plus the emitter's own default-value renderings).
///
/// An `emitrust.call_opaque` callee is qualified ONLY when it is in this set.
/// This is the narrowing that matters: a USER static method rides the same
/// callee-printing path -- `Node::default()` is emitted right beside
/// `Box::new(..)` in every std::unique_ptr crate -- and a C++ class named
/// `box` with its own static `new` would otherwise have its call rewritten to
/// the prelude's. Widening this set widens that hazard.
inline bool isEmitterPreludePath(llvm::StringRef callee) {
  return callee == "Box::new" || callee == "String::new" ||
         callee == "String::from" || callee == "Vec::new" ||
         callee == "Vec::from_iter";
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_RUSTPRELUDESHADOW_H
