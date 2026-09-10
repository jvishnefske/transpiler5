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
///
/// FR-230: `alloca` is here because its absence MIS-RANKED the census. A
/// stack allocation raised the same shared "non-address value" wording and,
/// unrefined, landed in `pointer-local-nonaddress` next to four unrelated
/// pointer-member loads -- five different features read as one lever. A
/// refusal's wording is part of the ranking instrument. The substring also
/// covers `__builtin_alloca`, the spelling `<alloca.h>`'s macro expands to.
bool citedLineAllocates(llvm::StringRef line) {
  return line.contains("malloc") || line.contains("calloc") ||
         line.contains("realloc") || line.contains("aligned_alloc") ||
         line.contains("alloca");
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
    // C99-43 slice 1: the cursor-parameter rejections split out of the
    // generic ptr-to-ptr bucket, listed ABOVE it so first-match-wins
    // routes them (every wording also contains "pointer-to-pointer" or
    // "cursor parameter").
    {llvm::StringLiteral("escapes the cursor-parameter shape"),
     llvm::StringLiteral("ptr-to-ptr-shape-escape")},
    {llvm::StringLiteral("write through a cursor parameter"),
     llvm::StringLiteral("ptr-to-ptr-shape-escape")},
    {llvm::StringLiteral("written with a null pointer"),
     llvm::StringLiteral("ptr-to-ptr-null-write")},
    // C99-43 C1 narrowed this family: single-global-or-NULL writes are
    // admitted (the Option-cell mapping), so the needle widened from
    // "written with a global address" to catch the residual wordings —
    // "more than one global address" and "a global address outside the
    // single-global-or-NULL shape" — which stay the FR-62 front.
    {llvm::StringLiteral("global address"),
     llvm::StringLiteral("ptr-to-ptr-global-target")},
    {llvm::StringLiteral("write sites disagree on the source region"),
     llvm::StringLiteral("ptr-to-ptr-shape-escape")},
    {llvm::StringLiteral("write must execute unconditionally"),
     llvm::StringLiteral("ptr-to-ptr-shape-escape")},
    {llvm::StringLiteral("does not root in a sibling slice parameter"),
     llvm::StringLiteral("ptr-to-ptr-shape-escape")},
    {llvm::StringLiteral("pointer-to-pointer"),
     llvm::StringLiteral("ptr-to-ptr")},
    // A negative element index below a Phase-1b slice parameter's origin.
    // The slice starts at the pointee, so the earlier elements the C code
    // reads are not in it; the owner lowering (an i64 cursor into the whole
    // object) represents the same access exactly, which makes this a model
    // gap rather than a language gap and worth its own row. Mirrored into
    // test/RealWorld/run_realworld.py at the same position.
    {llvm::StringLiteral("negative element index through a slice parameter"),
     llvm::StringLiteral("slice-param-negative-index")},
    // FR-229: the object-representation BYTE VIEW family. The three
    // wordings below are the boundary of the admitted view, and each is a
    // distinct piece of work rather than one bucket: a padded aggregate
    // has no determinate byte image at all (C leaves the holes
    // indeterminate, measured), a global's image would have to be built
    // from a staged copy, and a mutable view reaching a call path with no
    // post-call flush would silently lose the callee's writes. They sit
    // ABOVE the generic wordings so first-match-wins routes them, and none
    // of the three overlaps the older CTS-BR `byte view of an aggregate
    // with non-byte members`, which keeps its historical `other` bucket.
    // Mirrors test/RealWorld/run_realworld.py's classify_blocker.
    {llvm::StringLiteral("byte view of an aggregate with interior padding"),
     llvm::StringLiteral("byte-view-padding")},
    {llvm::StringLiteral("byte view of the global object"),
     llvm::StringLiteral("byte-view-global")},
    {llvm::StringLiteral("byte view with no write-back point"),
     llvm::StringLiteral("byte-view-writeback")},
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
    // FR-77: a fn-ptr constant naming a function no TU defines is refused at
    // the address-taking site, so under recovery the CONTAINING item (a
    // global initializer, typically) carries the blocker.
    {llvm::StringLiteral("address of undefined function"),
     llvm::StringLiteral("fnptr-undefined-target")},
    // FR-103: an extern global no TU defines costs each item still
    // referencing it at finalize (stubbed with the finalize wording); the
    // needle also catches the single-TU "without a definition" spelling,
    // which is the same front. Previously tabulated [other].
    {llvm::StringLiteral("extern global variable"),
     llvm::StringLiteral("undefined-extern-global")},
    // FR-129 half (a): glibc's <ctype.h> classifiers are macros over a
    // locale table reached through __ctype_b_loc(); the read previously
    // tabulated [other] behind the generic pointer-cast wording, which
    // hid a ubiquitous C idiom inside the corpus's largest junk bucket.
    {llvm::StringLiteral("locale ctype table lookup"),
     llvm::StringLiteral("ctype-table")},
    // FR-129 half (b): the classifier family is admitted in boolean context
    // only, and ONLY when nothing installs a locale -- the ASCII image was
    // measured in the "C" locale. A fenced TU's classifiers get their own
    // tag so the ledger distinguishes "we cannot lift this shape" from "we
    // will not lift it here".
    {llvm::StringLiteral("classifier in a translation unit that calls"),
     llvm::StringLiteral("ctype-locale")},
    // FR-113 admitted scoped enums; the RESIDUAL enum-definition gates
    // (keyword-named enums and enumerators, values outside i32, an empty
    // enum, a cross-TU shape conflict) previously tabulated [other]. The
    // "unsupported: enumerator" needle also catches the value-range and
    // incomplete-enum wordings, which are the same front.
    {llvm::StringLiteral("unsupported: enum name"),
     llvm::StringLiteral("enum-def-rejected")},
    {llvm::StringLiteral("unsupported: enumerator"),
     llvm::StringLiteral("enum-def-rejected")},
    {llvm::StringLiteral("enum with no enumerators"),
     llvm::StringLiteral("enum-def-rejected")},
    {llvm::StringLiteral("conflicting definition of enum"),
     llvm::StringLiteral("enum-def-rejected")},
    // FR-108 enum arm: the SAME-TU half of the enum dedup guard, split out
    // of the cross-TU wording above because it is a different blocker --
    // two enums of one unit composing one emitted symbol, which no amount
    // of shard merging can resolve. Language-agnostic (a plain C program
    // reaches it through a block-scope `enum E` beside a file-scope one),
    // so the tag carries no `cxx-` prefix, exactly like `record-name-clash`.
    // Mirrored into test/RealWorld/run_realworld.py.
    {llvm::StringLiteral("collides with the emitted name of a different enum"),
     llvm::StringLiteral("enum-name-clash")},
    // FR-122: the cross-TU record dedup key now folds in the ODR hash of
    // the member surface, so a name reused with divergent members (dtor
    // presence/body, method bodies) rejects under the same wording the
    // field-shape conflict always used. Fires in plain C too (the C table
    // is the right home), previously tabulated [other].
    {llvm::StringLiteral("conflicting definition of struct"),
     llvm::StringLiteral("struct-shape-conflict")},
    {llvm::StringLiteral("was rejected, so a type naming it"),
     llvm::StringLiteral("rejected-type-cascade")},
    // FR-224: the curated <math.h> POLICY rejections (pow/exp/log and
    // their `f` forms). They previously tabulated [other] behind the
    // generic bucket, and FR-224 made that worse rather than better: it
    // moved `expf` OUT of the `libc:expf` tag the generic system-header
    // wording gave it and into [other], so the census lost the one tag
    // that told a reader "this is refused on purpose, do not rank it as
    // missing work". A tag of its own restores that, for the f64 forms
    // too.
    {llvm::StringLiteral("has no bit-exact Rust mapping"),
     llvm::StringLiteral("libm-not-bit-exact")},
    // FR-224: `abort`, for the same reason and with the same history --
    // it was `libc:abort` until this wave gave it a wording that says
    // WHY. The tag records that the blocker is the emitted crate's
    // stdout buffering model, not the abort call.
    {llvm::StringLiteral("terminates without flushing"),
     llvm::StringLiteral("abort-stdout-flush")},
};

/// The C++-input table (`_CXX_BLOCKER_SUBSTRINGS`), consulted after the table
/// above and in the survey's order. Every wording here is raised only from a
/// C++-only code path (a `CXXRecordDecl` walk, `mapType`'s reference case, or
/// the STL recognition table), so a C input can never match one and the C
/// corpus's tabulation is unaffected by this table's existence.
constexpr BlockerSubstring kCxxBlockerSubstrings[] = {
    {llvm::StringLiteral("was not reached: sibling specialization"),
     llvm::StringLiteral("template-sibling-not-reached")},
    {llvm::StringLiteral("base classes are not supported"),
     llvm::StringLiteral("cxx-inheritance")},
    // W2.18 admitted the SINGLE public non-virtual base as an ordinary
    // first field; the wording above is RETAINED for the residual shapes
    // (multiple, virtual and non-public inheritance, an undefined or
    // class-template-specialization base). The wordings below are the
    // measured miscompile channels around the admitted subset, each with
    // its own tag so the backlog can rank them separately -- the same
    // discipline W2.17 applied to the destructor family.
    //
    // W2.26 retired `cxx-drop-base` and `cxx-virtual-destructor`: a
    // destructor-carrying single public base and a sole-virtual-dtor class
    // are ADMITTED (the transitive drop predicate closes the lost-drop and
    // E0204 channels), and each wording had exactly one emitter, so both
    // rows went with their gates. W2.19a retired `cxx-virtual` the same
    // way: the class-level `virtual method` gate is gone (virtual methods
    // on VALUES statically bind), and the dynamic residual -- a virtual
    // call through any pointer-shaped receiver, including implicit `this`
    // -- is a call-site fence with its own wording, tagged below beside
    // the unresolved-callee wording it was split from.
    // An empty base carries no field to project through: both wordings are
    // the residual accesses that would otherwise name a field that does
    // not exist, or drop a base constructor body.
    {llvm::StringLiteral("inherited member of an empty base class"),
     llvm::StringLiteral("cxx-inheritance-empty-base")},
    {llvm::StringLiteral("constructor of an empty base class"),
     llvm::StringLiteral("cxx-inheritance-empty-base")},
    {llvm::StringLiteral("inherited member through a pointer to a derived "
                         "class"),
     llvm::StringLiteral("cxx-inheritance-upcast")},
    // FR-120 admitted method calls and upcast bindings through LOCAL
    // struct pointers; a pointer to a GLOBAL object resolves through a
    // staged copy and the method-call path has no writeback flush, so a
    // mutating method's effect on the copy would be silently dropped —
    // rejected with its own wording instead of miscompiled.
    {llvm::StringLiteral("mutating method call through a pointer to a "
                         "global object"),
     llvm::StringLiteral("cxx-method-global-receiver")},
    {llvm::StringLiteral("inherited access through a non-struct place"),
     llvm::StringLiteral("cxx-inheritance")},
    {llvm::StringLiteral("base constructor initializer"),
     llvm::StringLiteral("cxx-inheritance")},
    // W2.17 admitted the non-virtual, same-TU-defined destructor; the
    // generic wording below is RETAINED for the residual shape (a union
    // destructor), and every measured miscompile channel around the
    // admitted subset gets its own tag so the backlog can rank them
    // separately instead of collapsing them into one `cxx-destructor`.
    {llvm::StringLiteral("destructor with no definition in this translation "
                         "unit"),
     llvm::StringLiteral("cxx-destructor-no-body")},
    {llvm::StringLiteral("destructor collides with the member function "
                         "'dtor'"),
     llvm::StringLiteral("cxx-destructor-name-clash")},
    // FR-185: the same hazard on the constructor side, live only since the
    // constructor's fixed base name became `ctor` (a legal C++ member
    // spelling, where `new` was a keyword and could never clash).
    {llvm::StringLiteral("constructor collides with the member function "),
     llvm::StringLiteral("cxx-constructor-name-clash")},
    {llvm::StringLiteral("struct member of a class with a destructor"),
     llvm::StringLiteral("cxx-drop-member")},
    {llvm::StringLiteral("array of a class with a destructor"),
     llvm::StringLiteral("cxx-drop-array")},
    {llvm::StringLiteral("global or static object of a class with a "
                         "destructor"),
     llvm::StringLiteral("cxx-drop-global")},
    {llvm::StringLiteral("class with a destructor passed or returned by "
                         "value"),
     llvm::StringLiteral("cxx-drop-by-value")},
    {llvm::StringLiteral("value copy of a class with a destructor"),
     llvm::StringLiteral("cxx-drop-by-value")},
    {llvm::StringLiteral("outside a function, loop, or branch body"),
     llvm::StringLiteral("cxx-drop-scope")},
    {llvm::StringLiteral("in a loop whose increment has side effects"),
     llvm::StringLiteral("cxx-drop-scope")},
    // FR-193 item 6: the same family one step further out -- the scope IS
    // modelled, but the CF-to-SCF lift relocates a loop-exit path past the
    // drop, so a side effect on it crosses the destructor.
    {llvm::StringLiteral("in a loop whose exit path has side effects"),
     llvm::StringLiteral("cxx-drop-scope")},
    // FR-193 item 6: the do-while CONDITION, the `for` increment's twin --
    // C++ destroys the body's locals before evaluating it, the emitted loop
    // renders it ahead of the drop.
    {llvm::StringLiteral("do-while loop whose condition has side effects"),
     llvm::StringLiteral("cxx-drop-scope")},
    {llvm::StringLiteral("user-declared destructor"),
     llvm::StringLiteral("cxx-destructor")},
    {llvm::StringLiteral("virtual method call through a pointer"),
     llvm::StringLiteral("cxx-virtual-call")},
    {llvm::StringLiteral("virtual or unresolved member call"),
     llvm::StringLiteral("cxx-virtual-call")},
    // FR-112 containment: a member-level shape no longer rejects the CLASS;
    // the member is omitted and its USES are the rejections. Both wordings
    // below are those use sites -- the spelled/implicit operator call
    // (whose message names the omitted member and its class) and the call
    // to a method omitted because its own signature or body failed (the
    // static-method variant takes the same wording; see the C8 note at the
    // call dispatch). Tagged separately from the operator family because
    // FR-112 was itself ranked from a tabulation these used to blind by
    // landing in the catch-all `other`. ORDER MATTERS TWICE: the first
    // needle must sit ABOVE the "overloaded operator" row (its message
    // contains both substrings, and first match wins), and both rows are
    // mirrored AT THE SAME POSITION into test/RealWorld/run_realworld.py.
    {llvm::StringLiteral("omitted from class"),
     llvm::StringLiteral("cxx-omitted-member")},
    {llvm::StringLiteral("call to unimported method"),
     llvm::StringLiteral("cxx-omitted-member")},
    {llvm::StringLiteral("overloaded operator"),
     llvm::StringLiteral("cxx-operator-overload")},
    // FR-114: the residual overload-set collisions the widened suffix
    // table cannot split (frozen member `i` pairs, long/long long, two
    // fn-pointer params, separator-free member code aliasing). Previously
    // these hid inside the cross-TU "conflicting definition" wording and
    // tabulated [other], which is exactly how the raytracing ranking
    // mis-attributed interval's cascade. No substring overlap with the
    // "overloaded operator" row above. Mirrored into
    // test/RealWorld/run_realworld.py.
    {llvm::StringLiteral("C++ overload set for"),
     llvm::StringLiteral("cxx-overload-collision")},
    // FR-117: a user-defined CONVERSION FUNCTION. The member itself is
    // OMITTED from the class rather than rejected (so no ledger entry is
    // raised for the class), but its residual USE positions -- the explicit
    // `c.operator int()` call spelling and an out-of-line definition, which
    // is a top-level item in its own right -- are located rejections and
    // need a tag of their own, because they are a distinct backlog item from
    // the operator-overload family above.
    {llvm::StringLiteral("unsupported: conversion function"),
     llvm::StringLiteral("cxx-conversion-function")},
    // The IMPLICIT half of the same construct: every implicit and
    // `static_cast` use of a conversion function reaches `importCast` as
    // `CK_UserDefinedConversion`. Measured untagged before FR-117, so all of
    // it tabulated as the catch-all `other` -- and since FR-112 was ranked
    // #1 FROM that tabulation, leaving it there would blind the next
    // ranking exactly where FR-117 shifts mass into it.
    {llvm::StringLiteral("unsupported cast (UserDefinedConversion)"),
     llvm::StringLiteral("cxx-user-conversion")},
    // FR-118: the class-level gate whose late position was the half-import
    // miscompile. Also measured untagged (`dropped 'C' [other]`), which is
    // why the tinyxml2/jsoncpp cascade attribution could not see it.
    {llvm::StringLiteral("copy/move/delegating constructor"),
     llvm::StringLiteral("cxx-copy-ctor")},
    // W2.23 admitted the user-provided `T(const T&)` copy constructor;
    // the broad "copy constructor" needle below catches every residual
    // wording of the wave in one row -- the non-const-`T&` shape
    // ("copy constructor taking a non-const reference"), the
    // implementation-defined NRVO-candidate return ("NRVO-candidate
    // return of a class with a copy constructor"), and the E0277 array
    // boundary ("array of a class with a copy constructor"). No earlier
    // needle is a substring of any of them (the droppy array row says
    // "destructor"), and the combined-gate wording above still matches
    // first for the move/delegating/defaulted rejections. Mirrored into
    // test/RealWorld/run_realworld.py.
    {llvm::StringLiteral("copy constructor"),
     llvm::StringLiteral("cxx-copy-ctor")},
    // W2.23's other half: whole-object assignment. The IMPLICIT copy
    // assignment lowers memberwise in statement position; the residual
    // shapes (non-scalar members, a value use of the `T&` result) carry
    // this honest wording -- replacing the misleading "omitted from
    // class" message an implicit member used to borrow. Tagged apart from
    // cxx-copy-ctor because copy-ASSIGNMENT is a different AST node and a
    // different backlog item. Mirrored into test/RealWorld/run_realworld.py.
    {llvm::StringLiteral("implicit copy assignment"),
     llvm::StringLiteral("cxx-copy-assign")},
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
    // W2.22 admitted `std::cout`/`std::cerr` `<<` chains in statement
    // position; the wordings below are the residual frontier around that
    // subset, each with its own tag so the backlog can rank the ostream
    // front instead of collapsing it into `other`. Every one is emitted
    // only from `CImporter::emitOstreamChain` or the two value-use guards
    // that feed it, so a C program can never match one.
    {llvm::StringLiteral("result of a std::ostream << chain must be unused"),
     llvm::StringLiteral("cxx-ostream-value-use")},
    {llvm::StringLiteral("pointer std::ostream << operand prints a "
                         "nondeterministic address"),
     llvm::StringLiteral("cxx-ostream-pointer-operand")},
    {llvm::StringLiteral("std::ostream << operand must be a string literal"),
     llvm::StringLiteral("cxx-ostream-cstr-operand")},
    {llvm::StringLiteral("is not a recognized std::ostream manipulator"),
     llvm::StringLiteral("cxx-ostream-manipulator")},
    {llvm::StringLiteral("std::ostream << string literal"),
     llvm::StringLiteral("cxx-ostream-string-literal")},
    {llvm::StringLiteral("std::ostream << operand"),
     llvm::StringLiteral("cxx-ostream-operand-type")},
    // W2.20 std::map / std::set frontier. Every row here sits BEFORE the
    // three generic STL rows below so first-match keeps them
    // distinguishable in the ledger; mirrored into
    // test/RealWorld/run_realworld.py. The first two are PERMANENT
    // rejections, not backlog items: an unordered container's iteration
    // order is unspecified and a multi- container holds duplicate keys,
    // so no Rust container reproduces either byte for byte.
    {llvm::StringLiteral("iteration order is unspecified"),
     llvm::StringLiteral("stl-unordered-container")},
    {llvm::StringLiteral("stores duplicate keys"),
     llvm::StringLiteral("stl-multi-container")},
    {llvm::StringLiteral("with a comparator other than std::less"),
     llvm::StringLiteral("stl-map-comparator")},
    {llvm::StringLiteral("is not in the supported ordered key set"),
     llvm::StringLiteral("stl-map-key-type")},
    {llvm::StringLiteral("is not in the supported value set"),
     llvm::StringLiteral("stl-map-value-type")},
    {llvm::StringLiteral("does not overwrite an existing key"),
     llvm::StringLiteral("stl-map-insert")},
    {llvm::StringLiteral(
         "iterators are only recognized in the find(k) != end() idiom"),
     llvm::StringLiteral("stl-map-iterator")},
    {llvm::StringLiteral("is a read-only place"),
     llvm::StringLiteral("stl-map-at-write")},
    {llvm::StringLiteral("requires a structured binding"),
     llvm::StringLiteral("stl-map-ranged-for")},
    // W2.21 std::unique_ptr frontier. Every row here sits BEFORE the three
    // generic STL rows below so first-match keeps them distinguishable in
    // the ledger; mirrored into test/RealWorld/run_realworld.py.
    // `stl-shared-ptr` is a PERMANENT rejection rather than a backlog item:
    // Rc/Arc have different aliasing rules from shared_ptr and this subset
    // has no model for shared ownership at all. `stl-unique-ptr-nullable`
    // is THE wave boundary -- std::unique_ptr maps to a bare Box<T>, which
    // cannot be null, so every default-construct / nullptr-compare /
    // reset() spelling lands there and a later Option<Box<T>> wave is
    // exactly the ranking signal this tag carries.
    {llvm::StringLiteral("the std::unique_ptr<T[]> array form"),
     llvm::StringLiteral("stl-unique-ptr-array-form")},
    {llvm::StringLiteral("with a deleter other than std::default_delete"),
     llvm::StringLiteral("stl-unique-ptr-deleter")},
    {llvm::StringLiteral("is not in the supported payload set"),
     llvm::StringLiteral("stl-unique-ptr-payload-type")},
    {llvm::StringLiteral("std::unique_ptr payload does not match"),
     llvm::StringLiteral("stl-unique-ptr-payload-type")},
    {llvm::StringLiteral("no model for shared ownership"),
     llvm::StringLiteral("stl-shared-ptr")},
    {llvm::StringLiteral("a Box<T> cannot be null"),
     llvm::StringLiteral("stl-unique-ptr-nullable")},
    {llvm::StringLiteral("moved-from std::unique_ptr"),
     llvm::StringLiteral("stl-unique-ptr-move")},
    {llvm::StringLiteral("hands out a raw pointer to the payload"),
     llvm::StringLiteral("stl-unique-ptr-raw-pointer")},
    {llvm::StringLiteral("std::make_unique is only recognized as the initializer"),
     llvm::StringLiteral("stl-make-unique-position")},
    {llvm::StringLiteral("std::make_unique argument type does not match"),
     llvm::StringLiteral("stl-make-unique-argument")},
    {llvm::StringLiteral("this std::unique_ptr initializer shape"),
     llvm::StringLiteral("stl-unique-ptr-construct")},
    {llvm::StringLiteral("std::unique_ptr shape could not be determined"),
     llvm::StringLiteral("stl-unique-ptr-construct")},
    {llvm::StringLiteral("std::make_unique of a class with in-class member initializers"),
     llvm::StringLiteral("stl-unique-ptr-construct")},
    {llvm::StringLiteral("std::make_unique constructor"),
     llvm::StringLiteral("stl-unique-ptr-construct")},
    {llvm::StringLiteral("called through a std::unique_ptr"),
     llvm::StringLiteral("stl-unique-ptr-ref-argument")},
    {llvm::StringLiteral("called through std::make_unique"),
     llvm::StringLiteral("stl-unique-ptr-ref-argument")},
    // FR-188 shares `stl-unique-ptr-ref-argument` with the two rows above
    // rather than minting a tag: all three name the SAME future work (make
    // the payload borrow follow the callee's parameter mutability), so
    // splitting them would split one backlog item's ranking signal in two.
    {llvm::StringLiteral("mutable reference argument borrowed from a std::unique_ptr"),
     llvm::StringLiteral("stl-unique-ptr-ref-argument")},
    {llvm::StringLiteral("is not a recognized STL type"),
     llvm::StringLiteral("stl-unrecognized-type")},
    {llvm::StringLiteral("is not a recognized STL method"),
     llvm::StringLiteral("stl-unrecognized-method")},
    {llvm::StringLiteral("receiver is not a recognized STL"),
     llvm::StringLiteral("stl-unrecognized-receiver")},
    // W2.16 class-template frontier. Every wording below is emitted only
    // from `CImporter::importRecordUncached`'s specialization checks or
    // its same-TU name-clash guard, so a C program can never match one;
    // without these entries all five tabulate as `other` and the backlog
    // cannot rank the class-template front at all.
    {llvm::StringLiteral("explicit class template specialization"),
     llvm::StringLiteral("cxx-class-template-explicit-spec")},
    {llvm::StringLiteral("partial class template specialization"),
     llvm::StringLiteral("cxx-class-template-partial-spec")},
    {llvm::StringLiteral("non-type template argument in class template instantiation"),
     llvm::StringLiteral("cxx-class-template-nttp")},
    {llvm::StringLiteral("variadic class template (template parameter pack)"),
     llvm::StringLiteral("cxx-class-template-pack")},
    {llvm::StringLiteral("class template instantiation collides with the existing struct"),
     llvm::StringLiteral("cxx-class-template-name-clash")},
    // FR-108: the non-template half of the same guard. Language-agnostic
    // — a plain C program reaches it (`typedef struct { int v; } Box;`
    // beside `struct Box { int v; };`) — so the tag carries no `cxx-`
    // prefix. Without this entry the clash tabulates as `other` and the
    // backlog cannot see a silent-wrong-code channel turning into a
    // located rejection.
    {llvm::StringLiteral("collides with the emitted name of a different struct"),
     llvm::StringLiteral("record-name-clash")},
    // W2.24 exceptions-as-Result-threading frontier. Every wording is
    // emitted only from the C++-only throw plan (planThrows) or the
    // try/throw emission paths, so a C program can never match one. The
    // uncaught row sits FIRST: the call-outside-try wording contains both
    // "potentially-throwing function" and "cannot propagate exceptions",
    // and first-match must file it under the uncaught tag. Mirrored into
    // test/RealWorld/run_realworld.py.
    {llvm::StringLiteral("cannot propagate exceptions"),
     llvm::StringLiteral("cxx-exception-uncaught")},
    {llvm::StringLiteral("thrown exception payload"),
     llvm::StringLiteral("cxx-exception-payload")},
    {llvm::StringLiteral("potentially-throwing function"),
     llvm::StringLiteral("cxx-exception-closure")},
    {llvm::StringLiteral("a throw reaching a noexcept function"),
     llvm::StringLiteral("cxx-exception-noexcept")},
    {llvm::StringLiteral("catch by reference"),
     llvm::StringLiteral("cxx-exception-catch")},
    {llvm::StringLiteral("catch of a type other than"),
     llvm::StringLiteral("cxx-exception-catch")},
    {llvm::StringLiteral("more than one catch handler"),
     llvm::StringLiteral("cxx-exception-catch")},
    {llvm::StringLiteral("nested inside another try or catch"),
     llvm::StringLiteral("cxx-exception-catch")},
    {llvm::StringLiteral("rethrow outside a catch handler"),
     llvm::StringLiteral("cxx-exception-catch")},
    {llvm::StringLiteral("try/catch outside a translation-unit-level"),
     llvm::StringLiteral("cxx-exception-catch")},
    {llvm::StringLiteral("declared inside a try statement"),
     llvm::StringLiteral("cxx-exception-catch")},
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

  // FR-141: the C twin of the wording above, and outside the shared table
  // for exactly the same reason. `rejectOrphanOwnerMethods` raises it when a
  // Phase-4 owner method survives an import that never created its owner
  // struct, which can only happen once the OWNING function has already been
  // rejected and recovered from (FR-42/FR-43). A non-recovering whole-program
  // run dies on the owner and never emits the method at all, so
  // `classify_blocker` in run_realworld.py — which tags whole-PROGRAM
  // rejections seen through a subprocess — can never observe this wording;
  // putting it in the shared table would add an entry the Python twin could
  // not exercise and would break that table's line-for-line correspondence.
  //
  // Like `cxx-cascaded-method` the tag names a SYMPTOM: the construct to
  // blame is whatever sank the owning function, and the ledger row carries
  // that function's graph key as its cascade source so FR-49's chain credits
  // the real root.
  static constexpr llvm::StringLiteral kOrphanOwnerMethod =
      "method of an owner struct that was never created";
  if (diagnostic.contains(kOrphanOwnerMethod))
    return "owner-method-not-reached";

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
