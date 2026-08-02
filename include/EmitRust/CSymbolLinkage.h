//===- CSymbolLinkage.h - Internal linkage in emitted symbols ---*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The INVERSE of the internal-linkage half of `CSymbolNaming.h`: given an
/// emitted Rust item name, was the C/C++ declaration it came from
/// internal-linkage (`static` at file scope, or in an anonymous namespace)?
///
/// Why this is its own header rather than a function in `CSymbolNaming.h`:
/// the only consumer is the Rust emitter (`lib/Target/Rust`), and
/// `CSymbolNaming.h` includes `clang/AST/Decl.h`. Pulling clang's AST into
/// `MLIREmitRustTargetRust` — a library whose entire input is an MLIR module
/// and whose entire output is text — would be a real layering regression for
/// the sake of six lines of `StringRef` arithmetic. So the predicate lives
/// here, depends on nothing but `StringRef`, and `CSymbolNaming.h` includes
/// it so the mangler and its inverse are read together.
///
/// # Why a NAME predicate at all, and what it costs
///
/// Nothing in the IR records linkage. `emitrust.func` and `emitrust.global`
/// have no visibility or linkage attribute (see `EmitRustOps.td`), and MLIR's
/// builtin `sym_visibility` is already spoken for by the importer with an
/// unrelated meaning — `private` marks a body-less DECLARATION, so a
/// file-`static` definition is non-private and an `extern` prototype of an
/// external-linkage function IS private, exactly backwards from linkage.
/// Linkage is modelled only at the AST level, in the FR-40 item graph
/// (`ItemGraph::ItemLinkage`), which the crate emitter does not have and
/// which costs a second parse to obtain.
///
/// What DOES survive into the module is the mangle itself, and it survives
/// because it has to: two translation units may each define a `static
/// helper`, and both become items of one flat Rust module, so the tag is
/// load-bearing for correctness rather than decorative. Reading it back is
/// therefore reading a fact the importer deliberately wrote down.
///
/// The one imprecision, stated rather than hidden: a C identifier literally
/// spelled `tu0_x`, or one containing `ns_anon_`, is indistinguishable from a
/// tagged one. The failure mode is CONSERVATIVE in the only place this
/// predicate is used — such an item is kept private in a library crate, so it
/// is under-exported, never mis-exported — and the reverse error (leaking a
/// TU-private item into a crate's public API) cannot occur.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CSYMBOLLINKAGE_H
#define EMITRUST_CSYMBOLLINKAGE_H

#include "llvm/ADT/StringRef.h"

namespace mlir {
namespace emitrust {

/// The fixed prefix `namespacePrefix` contributes for an anonymous namespace,
/// whose contents have internal linkage. It may appear at the front of a
/// symbol or after an outer namespace's prefix (`a::(anonymous)::f` becomes
/// `ns_a_ns_anon_f`), hence the substring test in
/// `isInternalLinkageSymbolName`.
inline constexpr llvm::StringLiteral kAnonNamespaceTag = "ns_anon_";

/// Returns whether `name` — an emitted Rust item name, i.e. the output of
/// `cFunctionSymbolName` or `cGlobalSymbolName` — carries an
/// internal-linkage marker.
///
/// The two markers, which are exactly the two `CSymbolNaming.h` produces:
///  - the per-TU tag `tu<N>_` (decimal `N`), prepended to a `static` function
///    and to any non-externally-visible file-scope global under a project
///    import;
///  - the anonymous-namespace tag `ns_anon_` anywhere in the namespace
///    flattening prefix.
///
/// \param name the emitted item name.
/// \returns true when the originating declaration had internal linkage.
inline bool isInternalLinkageSymbolName(llvm::StringRef name) {
  // The tags are lowercase on a snake_case function name and uppercased on a
  // SCREAMING_SNAKE_CASE global (FR-53 idiomatic rename), so both spellings of
  // each tag are accepted.
  if (name.contains(kAnonNamespaceTag) || name.contains("NS_ANON_"))
    return true;
  if (!name.consume_front("tu") && !name.consume_front("TU"))
    return false;
  // At least one digit, then the separating underscore: `tu12_helper` is
  // tagged, `tuple_size` and `tu_helper` are not.
  size_t digits = 0;
  while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9')
    ++digits;
  return digits > 0 && digits < name.size() && name[digits] == '_';
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CSYMBOLLINKAGE_H
