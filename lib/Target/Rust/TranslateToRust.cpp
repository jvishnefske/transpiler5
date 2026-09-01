//===- TranslateToRust.cpp - Translating EmitRust to Rust ----------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the EmitRust-to-Rust emitter: a syntax-directed
/// translation from the EmitRust dialect to Rust source text. The emitter is
/// structured after upstream EmitC's CppEmitter: an internal emitter class
/// holds the indented output stream and a per-function value-name map, and a
/// llvm::TypeSwitch dispatches over the supported operations. Place
/// operations (variable/member/subscript/deref) emit nothing at their
/// program point; the place expressions they denote are rendered on demand
/// by a recursive helper when a load, assign, or borrow consumes them.
///
/// So the emitted Rust compiles clean under the standard lints, each function
/// is pre-analyzed before emission (all analyses are use-list/reachability
/// walks over the IR; they never change semantics, only rendering):
/// unreachable statements after a diverging op are skipped, never-read
/// bindings are `_`-prefixed, a dead initializer is dropped in favor of
/// Rust's deferred initialization (`let v: T;`), entry-block dead stores are
/// elided, and `mut` is emitted only for bindings that are actually mutated.
/// Expression-oriented renderings keep the output rustacean (FR-61): the
/// function-final `return v;` renders as the tail expression `v` -- folding
/// away a single-use binding defined by the immediately preceding let-
/// producing op -- and a function-final `return;` is omitted (FR-61a); a
/// deferred `let x: T;` whose immediately following `if` assigns `x` exactly
/// once at the end of both arms renders as an if-expression binding
/// `let x: T = if c { .. } else { .. };` (FR-61b); a PURE single-real-use
/// value (constant, arithmetic, comparison, cast, bitcast, load, alias let)
/// renders inline at its consumer -- parenthesized by one precedence table
/// (`needsParens`), numeric constants carrying literal type suffixes -- and
/// a pure value with no emitted use at all emits nothing, cascading through
/// pure chains (FR-61d).
/// A drop is made only where the analysis PROVES no read on any path; a
/// wrong drop can only surface as a hard rustc error (E0381/E0384), never as
/// silently different behavior. Every construct that cannot be represented
/// in Rust fails the translation with a located diagnostic; no silently
/// wrong output is ever produced.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Target/TranslateToRust.h"

#include "EmitRust/CSymbolLinkage.h"
#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"
#include "EmitRust/RustCasing.h"
#include "EmitRust/RustPreludeShadow.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Support/IndentedOstream.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

#include "llvm/ADT/Twine.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

using namespace mlir;

namespace {

/// FR-159: the module PATH of an emitted item symbol, or an empty StringRef
/// when the symbol is a plain item name.
///
/// A module-level item may carry the ABSOLUTE Rust path
/// `crate::<module>::<leaf>` as its symbol; `emitModule` then renders it
/// inside `mod <module> { ... }` under `<leaf>` while every USE site keeps
/// spelling the whole path, unchanged. Absolute is a requirement, not a
/// style choice: paths written inside a `mod` are module-relative, so a
/// relative `tu0::add1` is rustc E0433 in precisely the position where it
/// matters most -- an fn-ptr TABLE payload, which renders INSIDE the module.
static StringRef itemModulePath(StringRef symbol) {
  if (!symbol.consume_front("crate::"))
    return StringRef();
  size_t leafSep = symbol.rfind("::");
  if (leafSep == StringRef::npos)
    return StringRef();
  return symbol.take_front(leafSep);
}

/// FR-159: the LEAF of an emitted item symbol -- the text after its last
/// `::`, or the whole symbol when it carries no path.
///
/// Only DEFINITION sites print this. Everything keyed BY symbol name -- the
/// Copy/Drop fixpoints, the fn-item set the dangling-target backstop
/// consults, every `SymbolTable` lookup -- keeps using the whole symbol, so
/// a module item whose leaf collides with a root item's name (the entire
/// point of the link-time sink) can never be mistaken for it.
static StringRef itemLeafName(StringRef symbol) {
  size_t leafSep = symbol.rfind("::");
  return leafSep == StringRef::npos ? symbol : symbol.drop_front(leafSep + 2);
}

/// FR-159: whether `text` is a Rust ITEM PATH -- one or more `::`-separated
/// identifier segments (`helper`, `tu0_helper`, `crate::tu0::helper`).
///
/// This is the shape of an importer-produced fn-ptr payload's `Some(<t>)`
/// target. It exists because three predicates each spelled the test inline
/// as "alphanumerics and underscores only", and so silently no-opped the
/// moment an item symbol grew a path: the FR-77 dangling-target backstop
/// stopped diagnosing entirely (measured: `Some(crate::tu0::nope)` was
/// emitted into the crate with NO diagnostic, while `Some(tu0_nope)` was
/// refused), and FR-63's const-block decision and FR-133's fn-ptr unwrap
/// both quietly changed emitted bytes. A digit-led segment still fails,
/// which is what keeps the va-cursor spelling `Some(0i64)` out, and any
/// other punctuation still fails, which keeps FR-52's `Some(f::<E>)`
/// requirement rewrite out.
static bool isRustItemPath(StringRef text) {
  if (text.empty())
    return false;
  while (true) {
    auto [segment, rest] = text.split("::");
    if (segment.empty() || llvm::isDigit(segment.front()) ||
        !llvm::all_of(segment,
                      [](char c) { return llvm::isAlnum(c) || c == '_'; }))
      return false;
    if (rest.empty())
      // A trailing `::` splits to an empty rest but leaves `text` longer
      // than the segment; that is not a path.
      return text.size() == segment.size();
    text = rest;
  }
}

/// FR-61d: the precedence rank of a rendered Rust expression, mirroring the
/// Rust reference's operator table exactly (higher binds tighter). An
/// inlined expression carries its rank so a consumer position can decide
/// parenthesization with one pure table (`needsParens`) instead of
/// per-case reasoning.
enum class Prec : int {
  Compare = 1, ///< `== != < <= > >=` (non-associative: chains are errors)
  BitOr = 2,   ///< `|`
  BitXor = 3,  ///< `^`
  BitAnd = 4,  ///< `&`
  Shift = 5,   ///< `<< >>`
  AddSub = 6,  ///< `+ -`
  MulDiv = 7,  ///< `* / %`
  Cast = 8,    ///< `expr as T`
  Unary = 9,   ///< leading `-` or `*` (negative literal, deref-rooted load)
  Postfix = 10 ///< atoms: names, suffixed literals, calls, `a.method(b)`
};

/// The rank as a comparable integer.
static constexpr int precValue(Prec p) { return static_cast<int>(p); }

/// FR-61d: the syntactic position an operand is rendered in. Every
/// `emitOperand` call site names its position; the position plus the
/// inlined expression's rank fully decides parenthesization.
struct ExprPos {
  enum class Kind {
    Stmt,      ///< whole right-hand side / tail expression / assign value
    Cond,      ///< `if`/`match` scrutinee position
    Delimited, ///< inside `( )`, `[ ]` (call args, index without cast)
    Receiver,  ///< before `.method(..)` or `[i]`
    CastSource, ///< before ` as T` (appended by the consumer)
    BinLhs,    ///< left operand of the infix operator of rank `rank`
    BinRhs     ///< right operand of the infix operator of rank `rank`
  };
  Kind kind;
  Prec rank; ///< meaningful for BinLhs/BinRhs only

  static ExprPos stmt() { return {Kind::Stmt, Prec::Postfix}; }
  static ExprPos cond() { return {Kind::Cond, Prec::Postfix}; }
  static ExprPos delimited() { return {Kind::Delimited, Prec::Postfix}; }
  static ExprPos receiver() { return {Kind::Receiver, Prec::Postfix}; }
  static ExprPos castSource() { return {Kind::CastSource, Prec::Postfix}; }
  static ExprPos binLhs(Prec rank) { return {Kind::BinLhs, rank}; }
  static ExprPos binRhs(Prec rank) { return {Kind::BinRhs, rank}; }
};

/// The whole parenthesization policy in one pure function. The `Stmt`,
/// `Cond`, and `Delimited` positions are exactly the ones the DENIED
/// `unused_parens` lint watches (see tools/emitrust-cc/CrateEmitter.cpp,
/// the `[lints.rust]` table): they never parenthesize, structurally, so no
/// inlining decision can ever trip the lint. The remaining positions wrap
/// by rank:
///  - Receiver: anything looser than an atom binds the trailing `.method`
///    / `[i]` wrongly (`-1.5f64.to_bits()` negates the call result);
///  - CastSource: `as` accepts a postfix atom or another cast (chains are
///    legal bare); everything else wraps -- including Unary, so a negative
///    literal renders `(-5i32) as u32`;
///  - BinLhs: strictly-looser operands wrap; equal ranks are left-
///    associative and stay bare -- EXCEPT under a comparison, which is
///    non-associative in Rust (`a == b == c` is a parse error), so equal
///    ranks wrap there too. One grammar quirk on top of pure precedence:
///    text ENDING in a bare cast (`.. as T`) directly left of a shift or
///    comparison wraps even when its top-level rank binds tighter, because
///    rustc parses `x as i64 << y`, `a << b as i64 < c` (and the `<`
///    forms) as generic arguments on the type -- a hard error. The
///    trailing-cast property (`endsInCast`) is tracked per captured
///    expression, since a bare-rhs cast leaks to the end of any infix
///    text;
///  - BinRhs: equal-or-looser operands wrap (`a - (b - c)`).
static bool needsParens(Prec rank, bool endsInCast, ExprPos pos) {
  switch (pos.kind) {
  case ExprPos::Kind::Stmt:
  case ExprPos::Kind::Cond:
  case ExprPos::Kind::Delimited:
    return false;
  case ExprPos::Kind::Receiver:
    return precValue(rank) < precValue(Prec::Postfix);
  case ExprPos::Kind::CastSource:
    return rank != Prec::Postfix && rank != Prec::Cast;
  case ExprPos::Kind::BinLhs:
    if (endsInCast &&
        (pos.rank == Prec::Shift || pos.rank == Prec::Compare))
      return true; // `.. as T << ..` / `.. as T < ..`: generic-args misparse
    return pos.rank == Prec::Compare
               ? precValue(rank) <= precValue(pos.rank)
               : precValue(rank) < precValue(pos.rank);
  case ExprPos::Kind::BinRhs:
    return precValue(rank) <= precValue(pos.rank);
  }
  llvm_unreachable("unknown expression position");
}

/// Emitter that translates EmitRust operations into Rust source text.
///
/// The emitter is a functional core over an output stream: besides the
/// indented stream wrapper it owns only per-function analysis caches (the
/// value-name map plus the warning-clean rendering sets: `unreachableOps`,
/// `deadStores`, `deferredInits`, `valueReadCache`, and the FR-61 tail/
/// if-expression rendering state), all recomputed from scratch at each
/// function entry. Names are assigned in emission order: within each
/// function, first the entry block arguments, then every op result (and
/// for-loop induction variable) as it is encountered top-down.
///
/// The emitter renders into an internal string buffer and `finish()` copies
/// it to the caller's stream. Buffering exists for two edits, both of which
/// reuse the per-op emitters so no printing logic is duplicated: the FR-61a
/// tail-expression fold emits the folded op's right-hand side as a normal
/// statement and removes its trailing `;` when the function-final return is
/// reached, and the FR-61d single-use inlining emits an inlined op's
/// statement, captures the text (minus indentation and `;`), and erases it
/// from the buffer so the consumer can print it inline.
class RustEmitter {
public:
  /// Creates an emitter writing to `os` under `options`.
  RustEmitter(raw_ostream &os, const emitrust::RustEmitOptions &options)
      : finalOS(os), bufferOS(buffer), os(bufferOS), options(options) {}

  /// Emits `op` as Rust source text; dispatches over all supported ops.
  /// Unsupported operations fail with a located "unable to translate op"
  /// diagnostic.
  LogicalResult emitOperation(Operation &op);

  /// Flushes the buffered translation to the caller's stream. Must be called
  /// exactly once, after the last `emitOperation`.
  void finish() {
    os.flush();
    finalOS << buffer;
    // FR-61d instrumentation: opt-in stderr count of inline sites / drops.
    if (::getenv("EMITRUST_INLINE_STATS"))
      llvm::errs() << "[fr61d] inline captures: " << inlineCaptureCount
                   << ", drops: " << dropCount << "\n";
  }

private:
  //===--------------------------------------------------------------------===//
  // Structural helpers
  //===--------------------------------------------------------------------===//

  /// Increases the output indentation by one nesting level (4 spaces).
  void increaseIndent() {
    // raw_indented_ostream uses a fixed 2-space step; apply it twice to get
    // the 4-space Rust convention.
    os.indent();
    os.indent();
  }

  /// Decreases the output indentation by one nesting level (4 spaces).
  void decreaseIndent() {
    os.unindent();
    os.unindent();
  }

  /// Returns the Rust name bound to `value`, assigning the next sequential
  /// name (v0, v1, ...) if the value has not been named yet.
  std::string assignName(Value value);

  /// FR-61e: binds `value` under the carried spelling `base`, applying the
  /// `_`-prefix rule for never-read values and uniquifying against every
  /// name already bound in the function. Shared by named variables
  /// (`assignName`) and named parameters (`emitFunc`).
  std::string claimName(Value value, StringRef base);

  /// Returns the Rust name previously bound to `value`, or a located error
  /// if the value has not been defined yet.
  FailureOr<std::string> lookupName(Location loc, Value value);

  /// FR-140: records that `name` is being RENDERED as a binding (an item
  /// name, a parameter, a `let`) inside the item currently being emitted,
  /// so that item can be given `#[allow(non_snake_case)]` if rustc would
  /// otherwise reject the spelling. A doubled underscore is perfectly legal
  /// C (`m__em`) and the emitter preserves the C spelling verbatim, so
  /// without this the emitted crate failed its OWN denied lint table: exit
  /// 0, unbuildable, no diagnostic anywhere.
  ///
  /// Only under the idiomatic rename: `--preserve-c-names` allows the three
  /// naming lints in the crate root instead of denying them (verbatim C
  /// spellings legitimately trip them, and keeping them is the flag's whole
  /// point), so a per-item attribute there would be redundant noise on
  /// every existing golden.
  void noteBoundName(StringRef name) {
    if (noteBoundNames && emitrust::idiomaticRenameEnabled() &&
        emitrust::tripsNonSnakeCase(name))
      itemTripsNonSnakeCase = true;
  }

  /// FR-140: opens the `#[allow(non_snake_case)]` scope of an item whose
  /// tripping names are only discovered while its BODY is rendered (a
  /// function: its locals). Returns the buffer offset the item starts at,
  /// for `closeNonSnakeCaseScope`. Items whose names are all known up front
  /// (a struct's fields, a trait's methods) test `tripsNonSnakeCase`
  /// directly and never open a scope.
  size_t openNonSnakeCaseScope() {
    os.flush();
    itemTripsNonSnakeCase = false;
    return buffer.size();
  }

  /// FR-140: prefixes the item rendered from `itemStart` onward with
  /// `#[allow(non_snake_case)]` when it bound a name rustc's lint would
  /// reject. The attribute goes on the ITEM because that is where rustc
  /// honours it: measured, an allow on a struct FIELD is ignored outright
  /// and the lint still fires, while one on the struct, on the fn, or on
  /// the let works. The item's own indentation is copied off its first
  /// rendered line, so a method nested in an `emitrust.impl` lines up.
  ///
  /// This ADDS an attribute and removes nothing -- the reason the direction
  /// was chosen over collapsing the underscore run, which would need a
  /// uniquifier and a collision diagnostic (`m__base` -> `m_base` against a
  /// C source that also spells `m_base`). The worst a wrong answer here can
  /// do is suppress one lint on one item; it cannot miscompile.
  void closeNonSnakeCaseScope(size_t itemStart) {
    if (!itemTripsNonSnakeCase)
      return;
    itemTripsNonSnakeCase = false;
    os.flush();
    StringRef item = StringRef(buffer).substr(itemStart);
    size_t indent = item.size() - item.ltrim(' ').size();
    buffer.insert(itemStart,
                  std::string(indent, ' ') + "#[allow(non_snake_case)]\n");
  }

  /// Emits `value` at the syntactic position `pos`: its captured inline
  /// expression (parenthesized per `needsParens`) when FR-61d inlined it,
  /// otherwise its Rust name; fails with a located diagnostic for an
  /// undefined value. The position is mandatory so every call site names
  /// where its operand lands.
  LogicalResult emitOperand(Location loc, Value value, ExprPos pos);

  /// Emits the Rust rendering of `type`, or fails with a located
  /// "cannot translate type" diagnostic.
  LogicalResult emitType(Location loc, Type type);

  /// Emits the Rust expression for a constant `attr`, or fails with a
  /// located diagnostic for unsupported attributes.
  LogicalResult emitAttribute(Location loc, Attribute attr);

  /// Emits a floating-point literal with round-trip precision, guaranteeing
  /// that either a decimal point or an exponent appears (e.g. 4.2, 1.0).
  LogicalResult emitFloatValue(Location loc, double value, bool isF32);

  /// Emits the Rust place expression denoted by the lvalue-typed `value` by
  /// recursing through its chain of variable/member/subscript/deref/enum_raw
  /// producers; any other producer (or a block argument) is an error.
  /// `derefNeedsParens` is set by
  /// the caller when the emitted place will have a projection or index
  /// appended (`.field`, `[i]`, `.0`, `.method()`) or otherwise sits where a
  /// leading unary `*` would misparse; a `emitrust.deref` renders as `(*..)`
  /// only then, and as a bare `*..` when the place stands alone (a whole-place
  /// assignment target, an rvalue load, or an `&`/`&mut` operand).
  LogicalResult emitPlaceExpr(Location loc, Value value,
                              bool derefNeedsParens);

  /// Emits the default value of `type`: `0` for integers and index, `0.0`
  /// for floats, `false` for `i1`, `Name::default()` for structs and
  /// enums, `None` for function pointers, and `[<element-default>; N]` for
  /// arrays.
  LogicalResult emitDefaultValue(Location loc, Type type);

  /// Emits the Rust expression for a global initializer of value type
  /// `type`: an ArrayAttr renders as an array literal `[e0, e1, ...]` for
  /// an array type or a struct literal `Name { f0: e0, ... }` for a struct
  /// type (fields resolved from the `emitrust.struct_def` visible from
  /// `op`, in declaration order); any other attribute renders through
  /// emitAttribute. Nested aggregate elements recurse.
  LogicalResult emitAggregateInit(Operation *op, Location loc, Attribute init,
                                  Type type);

  /// Emits `value` as a quoted Rust string literal, escaping backslashes,
  /// quotes, newlines, tabs, and carriage returns.
  void emitEscapedStringLiteral(StringRef value);

  /// Emits "let vN: T = " (with `mut` when `isMut`), naming `result`.
  LogicalResult emitLetPrologue(Value result, bool isMut);

  /// Emits the statements of a single-block `region` one indentation level
  /// deeper. Fails on multi-block regions; an empty region emits nothing.
  LogicalResult emitRegionBody(Operation *parent, Region &region);
  /// Emits the operations of `block` in order, stopping after the first
  /// diverging op so unreachable trailing statements are not rendered.
  LogicalResult emitBlockBody(Block &block);

  //===--------------------------------------------------------------------===//
  // Per-operation emitters
  //===--------------------------------------------------------------------===//

  /// Emits the children of a module in order; only use, verbatim, func,
  /// impl, struct/enum definition, and global operations are allowed at
  /// module level.
  LogicalResult emitModule(ModuleOp moduleOp);
  /// Emits `impl <name> { ... }` with the contained functions rendered as
  /// `&mut self` methods one indentation level deeper.
  LogicalResult emitImpl(emitrust::ImplOp implOp);
  /// FR-179: emits the module-scope epilogue that makes an FR-62
  /// actor-lifted C function reachable by dlsym again -- one thread_local
  /// `RefCell<Owner>` singleton per owner struct, then one
  /// `#[no_mangle] extern "C"` free function per collected wrapper that
  /// borrows it and delegates to the `&mut self` method. Emits nothing at
  /// all when `cAbiActorWrappers` is empty, which is every run without
  /// `--c-abi-exports`.
  LogicalResult emitCAbiActorWrappers();
  /// FR-182: emits the module-scope epilogue that gives a C-ABI symbol to
  /// each exported function taking exactly ONE pointer to an ABI-faithful
  /// struct -- an `#[export_name = "<sym>"] unsafe extern "C"` wrapper whose
  /// whole body is the single delegating call `f(&mut *p, ...)`. The
  /// translated function is untouched, byte for byte, and the `unsafe` is
  /// confined to the generated item. Emits nothing when
  /// `cAbiPointerWrappers` is empty, which is every run without
  /// `--c-abi-exports`.
  LogicalResult emitCAbiPointerWrappers();
  /// FR-182: emits the `const _: () = assert!(...)` items that pin a
  /// `#[repr(C)]` struct's Rust layout to CLANG's -- size, alignment and
  /// every field offset, from the importer's `emitrust.abi_layout`. A
  /// divergence is `error[E0080]` at `cargo build`, which is the repo's
  /// mandated hard-error direction; `cargo build` succeeding is otherwise
  /// no evidence at all about an FFI boundary.
  LogicalResult emitAbiLayoutAsserts(emitrust::StructDefOp structDefOp,
                                     StringRef name);
  /// FR-62 slice 5b: emits the per-actor runtime an `emitrust.actor_runtime`
  /// anchor stands for, derived entirely from the referenced struct's impl —
  /// the `<Actor>Msg` enum (variant = UpperCamel(method), payload fields =
  /// the method's named parameters plus the per-call typed reply channel),
  /// the `<Actor>Handle` alias of `actor_rt::Handle<Msg>`, the `spawn`
  /// associated function holding the mailbox loop, and one wrapper per
  /// method on the Handle's inherent impl. The shared `mod actor_rt` text
  /// is appended once by `emitModule`. Slice 5c: the anchor's mode picks
  /// the substrate — std::sync::mpsc + std::thread (threaded) or tokio
  /// unbounded mailbox + oneshot replies + `async` wrappers with immediate
  /// await (async, E4's separately emitted crate flavor); the derivation
  /// is identical.
  LogicalResult emitActorRuntime(emitrust::ActorRuntimeOp op);
  /// Emits `<place>.<method>(args);`, bound with a `let` when the call
  /// produces a result. Rust's auto-ref scopes the `&mut` borrow of the
  /// receiver place to the call expression. A call on an ASYNC actor
  /// handle appends `.await` (FR-62 slice 5c).
  LogicalResult emitMethodCall(emitrust::MethodCallOp callOp);
  /// Emits `use <path>;`.
  LogicalResult emitUse(emitrust::UseOp useOp);
  /// Emits the verbatim string on its own line(s).
  LogicalResult emitVerbatim(emitrust::VerbatimOp verbatimOp);
  /// Emits a `fn` item with typed parameters, return type, and body.
  LogicalResult emitFunc(emitrust::FuncOp funcOp);
  /// Emits `return;` or `return vX;`.
  LogicalResult emitReturn(emitrust::ReturnOp returnOp);
  /// Emits an opaque call as a statement, a let, or a tuple-destructuring
  /// let depending on the number of results. When the `args` attribute is
  /// present, the rendered argument list is taken from it: index-typed
  /// integer attributes reference operands, string attributes render as
  /// escaped Rust string literals, and other attributes render as constants.
  LogicalResult emitCallOpaque(emitrust::CallOpaqueOp callOp);
  /// Emits an indirect call through a function pointer as
  /// `vF.expect("null function pointer")(vA, vB)`, bound with a `let` when
  /// the call produces a result and as a bare statement otherwise.
  LogicalResult emitCallIndirect(emitrust::CallIndirectOp callOp);
  /// Emits `let vN: T = <constant>;`.
  LogicalResult emitConstant(emitrust::ConstantOp constantOp);
  /// Emits `let vN: T = <verbatim expression>;`.
  LogicalResult emitLiteral(emitrust::LiteralOp literalOp);
  /// Emits `let vN: T = vInit;` or `let mut vN: T = vInit;`.
  LogicalResult emitLet(emitrust::LetOp letOp);
  /// Emits `vDst = vSrc;`, rendering an lvalue-typed destination as its
  /// place expression.
  LogicalResult emitAssign(emitrust::AssignOp assignOp);
  /// Emits `let vN: T = vA <symbol> vB;` for binary and comparison ops.
  LogicalResult emitBinary(Operation *op, StringRef symbol);
  /// Emits `let vN: T = vA.<method>(vB);` for the method-call binary form.
  LogicalResult emitBinaryMethod(Operation *op, StringRef method);
  /// Emits an add/sub/mul: the `wrapping_*` method-call form when the
  /// result type is an unsigned integer (C defines unsigned overflow as
  /// wrap-around; Rust's infix operators panic on overflow in debug
  /// builds), the infix `symbol` form otherwise.
  LogicalResult emitWrappingBinary(Operation *op, StringRef symbol,
                                   StringRef method);
  /// Emits `let vN: bool = vA <pred> vB;` -- except that an eq/ne
  /// comparison against a literal bool constant renders the idiomatic
  /// fold `boolCmpShape` classifies (clippy::bool_comparison).
  LogicalResult emitCmp(emitrust::CmpOp cmpOp);
  LogicalResult emitFnPtrCmp(emitrust::CmpOp cmpOp, Value lhs, Value rhs,
                             bool isEq);

  /// FR-63 (clippy::bool_comparison): how an eq/ne comparison against a
  /// literal bool constant renders. The classification is shared by
  /// `emitCmp` (the rendering), `capturedPrec`, and `capturedEndsInCast`
  /// (the captured text's rank), so the parenthesization table always sees
  /// the shape that was actually emitted.
  enum class BoolCmpShape {
    None,        ///< no single literal-bool side: the ordinary infix form
    Identity,    ///< `x == true` / `x != false`: the operand's text alone
    NegateName,  ///< `x == false` / `x != true`, `x` rendered by name: `!x`
    InvertInner, ///< the operand is an inlined comparison whose predicate
                 ///< inverts exactly: its infix text, predicate inverted
    Unfolded     ///< negative polarity with no lint-free spelling (an
                 ///< inlined float ORDER comparison -- NaN makes `!(a < b)`
                 ///< differ from `a >= b`, and the `!(..)` spelling trips
                 ///< `nonminimal_bool` -- or an inlined non-comparison
                 ///< text): keep the literal comparison
  };
  /// Classifies `cmpOp` and, for every shape but `None`, sets `operand` to
  /// the non-constant side. Consulted at emission/capture time only: the
  /// `InvertInner`/`Unfolded` split depends on `inlineExprs`, which is
  /// final for the operand once its def has been emitted.
  BoolCmpShape boolCmpShape(emitrust::CmpOp cmpOp, Value &operand);
  /// Emits `let vN: T = vA as T;`.
  LogicalResult emitCast(emitrust::CastOp castOp);
  /// Emits the bit-exact float/integer reinterpretation:
  /// `let vN: fW = fW::from_bits(vA);` for an integer-to-float bitcast,
  /// `let vN: TInt = vA.to_bits();` for a float-to-integer one. A
  /// signless integer side wraps the same-width (bit-preserving) `as`
  /// conversion `from_bits`/`to_bits`'s `uW` requires.
  LogicalResult emitBitcast(emitrust::BitcastOp bitcastOp);
  /// Emits `let vN: T = if vCond { vA } else { vB };`.
  LogicalResult emitSelect(emitrust::SelectOp selectOp);
  /// Emits an `if` statement, with `} else {` when the else region is
  /// present and non-empty.
  LogicalResult emitIf(emitrust::IfOp ifOp);
  /// Emits `for vI in (vLB..vUB).step_by(vS as usize) { ... }`.
  LogicalResult emitFor(emitrust::ForOp forOp);
  /// Emits `loop { ... }`.
  LogicalResult emitLoop(emitrust::LoopOp loopOp);
  /// FR-61c: emits `while <cond> {` -- the condition region folded into
  /// the head through the FR-61d capture machinery -- then the body.
  LogicalResult emitWhile(emitrust::WhileOp whileOp);
  /// Emits a `match` statement with one literal integer arm per case region
  /// (each case value rendered in the discriminator's type) and a trailing
  /// `_ =>` arm for the default region.
  LogicalResult emitSwitch(emitrust::SwitchOp switchOp);
  /// FR-52: emits `pub trait <name> { fn m(v0: T, ...) -> R; ... }`, the
  /// crate's declaration of what its environment must supply. Always `pub`
  /// regardless of `RustEmitOptions::exportItems`: a requirement nobody
  /// outside the crate can name is a requirement nobody can satisfy, and the
  /// op is only ever created for a library crate in the first place.
  LogicalResult emitTraitDef(emitrust::TraitDefOp traitDefOp);
  /// Emits a `#[derive(Clone, Copy, Default)]` struct item with its fields.
  /// `Copy` drops out when the struct's name is in `nonCopyStructNames`
  /// (FR-124: the module pre-pass fixpoint over local triggers and
  /// struct-typed fields). When some field type is outside the reach of the
  /// `Default` derive (`derivedDefaultCovers`), `Default` drops out of the
  /// derive list and an explicit, value-identical `impl Default` follows the
  /// item instead.
  LogicalResult emitStructDef(emitrust::StructDefOp structDefOp);
  /// Emits a C enum as a value-preserving open enum: a
  /// `#[repr(transparent)]` tuple struct over the storage integer (`i32`,
  /// or `u32` with the `unsigned_underlying` marker), one associated
  /// constant per variant, and a `Default` impl returning the first
  /// variant.
  LogicalResult emitEnumDef(emitrust::EnumDefOp enumDefOp);
  /// FR-62 slice 5a: emits a CLOSED data enum as a real Rust enum item —
  /// `#[derive(Clone, Copy)]` (no `Default`: a closed enum has no
  /// canonical default; no `PartialEq`: a struct payload derives none),
  /// unit variants bare, data variants with named fields.
  LogicalResult emitDataEnumDef(emitrust::DataEnumDefOp defOp);
  /// FR-62 slice 5a: emits a variant construction as a `let` binding of
  /// the variant literal — `Name::Variant { field: value, ... }`, or the
  /// bare path for a unit variant.
  LogicalResult emitEnumVariant(emitrust::EnumVariantOp variantOp);
  /// FR-62 slice 5a: emits a closed-enum match — one variant-pattern arm
  /// per case region, payload fields bound to the block-argument names,
  /// NO default arm (the verifier made the match exhaustive). Statement
  /// mode renders a match statement; result mode renders the match
  /// expression as a `let` right-hand side (tail-foldable per FR-61a),
  /// each arm's yielded value becoming that arm's tail expression.
  LogicalResult emitMatch(emitrust::MatchOp matchOp);
  /// Emits a global: `static NAME: T = <init-or-default>;` for `const`
  /// globals, and a `thread_local!` `std::cell::Cell` item otherwise.
  LogicalResult emitGlobal(emitrust::GlobalOp globalOp);
  /// Emits `let vN: T = NAME;` (const global) or
  /// `let vN: T = NAME.with(|__emitrust_tl| __emitrust_tl.get());`
  /// (mutable global). The binder name is reserved by the importer:
  /// identifier patterns cannot shadow statics, so a translated C global
  /// spelled like the binder would break every accessor.
  LogicalResult emitGlobalLoad(emitrust::GlobalLoadOp loadOp);
  /// Emits `NAME.with(|__emitrust_tl| __emitrust_tl.set(vX));`.
  LogicalResult emitGlobalStore(emitrust::GlobalStoreOp storeOp);
  /// Emits `let vN: T = vS[vI as usize].get();`.
  LogicalResult emitCellGet(emitrust::CellGetOp getOp);
  /// Emits `vS[vI as usize].set(vV);`.
  LogicalResult emitCellSet(emitrust::CellSetOp setOp);
  /// Emits the global's `with` accessor wrapping the region body:
  /// `NAME.with(|__emitrust_tl| {`, an unsizing coercion of the binder to
  /// `&std::cell::Cell<[T]>`, the region argument's
  /// `let vN: &[std::cell::Cell<T>] = __emitrust_tl.as_slice_of_cells();`
  /// binding, the region's statements, and the closing `});`. Nested
  /// `emitrust.global_cells` render as nested `with` closures, so no
  /// borrow ever escapes its accessor.
  LogicalResult emitGlobalCells(emitrust::GlobalCellsOp cellsOp);
  /// Emits `let mut vN: T = <init-or-default>;` for a local variable.
  LogicalResult emitVariable(emitrust::VariableOp variableOp);
  /// Emits `let vN: String = "<fill>".repeat((<count>) as usize);` (FR-64).
  LogicalResult emitStringRepeat(emitrust::StringRepeatOp op);
  /// Emits `let vN: Vec<T> = vec![<fill>; (<count>) as usize];` (FR-65).
  LogicalResult emitVecFill(emitrust::VecFillOp op);
  /// Emits `let vN: T = <place-expr>;`.
  LogicalResult emitLoad(emitrust::LoadOp loadOp);
  /// Emits `let vN: &T = &<place>;` or `let vN: &mut T = &mut <place>;`.
  LogicalResult emitAddrOf(emitrust::AddrOfOp addrOfOp);
  /// Emits `let vN: &[T] = &<place>[idx as usize..];` or the `&mut` form
  /// with the `mut` marker; the `as usize` cast is omitted for an
  /// index-typed index.
  LogicalResult emitSliceOf(emitrust::SliceOfOp sliceOfOp);
  /// Emits `let vN: &[i8] = &<table>[idx as usize][..];` — one command-line
  /// argument's byte run borrowed out of the argv table (C99-43 C3); the
  /// `as usize` cast is omitted for an index-typed index.
  LogicalResult emitArgvArg(emitrust::ArgvArgOp argvArgOp);

  /// FR-51: the `pub ` an exported item is prefixed with, or the empty
  /// string. Returns nothing at all unless `RustEmitOptions::exportItems` is
  /// set, which is what keeps binary-crate output byte-identical.
  ///
  /// \param symbol the emitted item name, consulted only for its
  ///        internal-linkage marker.
  StringRef itemVisibility(StringRef symbol) const {
    // FR-159: inside a `mod tu<N>` the visibility is fixed and uniform (see
    // `moduleItemVisibility`); the FR-51 export decision does not apply,
    // because a per-TU module holds exactly the items whose C linkage was
    // INTERNAL and which therefore have no business in a library surface.
    if (!moduleItemVisibility.empty())
      return moduleItemVisibility;
    if (!options.exportItems || emitrust::isInternalLinkageSymbolName(symbol))
      return "";
    return "pub ";
  }

  /// FR-159: the visibility every item rendered inside a `mod tu<N> { .. }`
  /// carries -- `pub(crate) ` while such a module is open, empty otherwise.
  ///
  /// MEASURED, not reasoned: it must be `pub(crate)` on the item AND on a
  /// struct's FIELDS. The crate root constructs a sunk record by naming its
  /// fields, and a private field there is rustc E0616; `pub` would be a
  /// lie about a translation-unit-local item and `pub(super)` does not
  /// reach a root item nested any deeper.
  StringRef moduleItemVisibility;

  /// The `pub ` prefix for a part of an exported TYPE — a struct field, a
  /// tuple element, an enum variant constant. Unlike `itemVisibility` this
  /// takes no symbol: types are exported unconditionally in library mode
  /// (see `RustEmitOptions::exportItems` for why), so their parts must be
  /// reachable too or the type is exported but unusable.
  StringRef typePartVisibility() const {
    if (!moduleItemVisibility.empty())
      return moduleItemVisibility;
    return options.exportItems ? "pub " : "";
  }

  /// The caller's stream; `finish()` copies the buffered translation here.
  raw_ostream &finalOS;

  /// The whole translation, accumulated so the FR-61a fold can remove the
  /// trailing `;` of the statement it turns into the tail expression.
  std::string buffer;

  /// Adapter presenting `buffer` as a stream (declared before `os`, which
  /// wraps it; initialization follows declaration order).
  llvm::raw_string_ostream bufferOS;

  /// Output stream tracking the current indentation.
  raw_indented_ostream os;

  /// The emission knobs this translation runs under.
  emitrust::RustEmitOptions options;

  /// FR-110: mangled impl-member symbol -> (impl struct name, in-impl Rust
  /// spelling), collected once per module (`emitModule`) from the
  /// `emitrust.method_rust_name` attributes C++ method members carry. The
  /// strip is PRINT-time only -- the IR, the receiver-mutability query, and
  /// FR-52's shared call/impl symbol namespace all keep the module-unique
  /// mangled names (module-level symbol uniqueness is what makes this flat
  /// map sound; a receiver-TYPE lookup is not, measured: a make_unique
  /// constructor's method_call receiver is `!emitrust.opaque<"Box<T>">`).
  /// Builtin method spellings (`unwrap`, `len`, ...) and Phase-4 C owner
  /// methods never appear as keys, so they pass through verbatim.
  llvm::StringMap<std::pair<llvm::StringRef, llvm::StringRef>> methodRustNames;

  /// FR-179: one pending `#[no_mangle] extern "C"` free-function wrapper
  /// for an FR-62 actor-lifted C owner method, collected by `emitFunc`
  /// (which is where the method's parameter SPELLINGS are known) and
  /// rendered by `emitCAbiActorWrappers` as a module-scope epilogue. The
  /// method itself is untouched, byte for byte.
  struct CAbiActorWrapper {
    /// The method's location, for the wrapper's type diagnostics.
    Location loc;
    /// The owner struct the FR-62 lift synthesized (`Tu0MBaseActor`).
    StringRef owner;
    /// The bare C symbol, which is also the exported function's name.
    StringRef symbol;
    /// The method's in-impl spelling, i.e. what the wrapper calls.
    StringRef printedName;
    /// The `pub ` the method itself carries.
    StringRef visibility;
    /// The non-receiver parameters, in order, exactly as the method
    /// rendered them.
    SmallVector<std::string> argNames;
    SmallVector<Type> argTypes;
    /// The single result type, or null for a void function.
    Type resultType;
  };
  SmallVector<CAbiActorWrapper> cAbiActorWrappers;

  /// FR-182: one pending C-ABI wrapper for an exported function whose
  /// signature is scalars plus exactly ONE reference to an ABI-faithful
  /// struct (class 1). The wrapper cannot simply BE that function: its
  /// parameter is a raw `*mut T` where the function's is `&mut T`, so the
  /// two are distinct items and only the wrapper carries the C symbol
  /// (`#[export_name]`, which is what lets the translated function keep its
  /// own name and every internal call site keep its bytes).
  ///
  /// A HARD STRUCTURAL CAP rides this shape: at most ONE reference or slice
  /// parameter, ever. Two references are `noalias` to LLVM and a C caller
  /// may legally alias them -- measured at rustc 1.96.1 -O3, a two-`&mut`
  /// kernel called with equal pointers returned 10 where the clang native
  /// returned 104, exit 0, no diagnostic.
  struct CAbiPointerWrapper {
    /// The function's location, for the wrapper's type diagnostics.
    Location loc;
    /// The bare C symbol, which `#[export_name]` binds.
    StringRef symbol;
    /// The translated function's own name, i.e. what the wrapper calls.
    StringRef printedName;
    /// The `pub ` the function itself carries.
    StringRef visibility;
    /// The parameters, in order, exactly as the function rendered them.
    SmallVector<std::string> argNames;
    SmallVector<Type> argTypes;
    /// The single result type, or null for a void function.
    Type resultType;
    /// Which parameter is the struct reference.
    unsigned refIndex;
    /// Whether that parameter is `&mut T` (`*mut T`) or `&T` (`*const T`).
    bool refIsMut;
    /// The struct_def symbol the reference points at.
    StringRef refStructName;
  };
  SmallVector<CAbiPointerWrapper> cAbiPointerWrappers;

  /// FR-182: the struct_def symbols rendered `#[repr(C)]` with clang's
  /// layout asserted. Computed once in `emitModule`, before any item is
  /// rendered, because a struct_def is emitted long before the exported
  /// function that names it. It is the TRANSITIVE closure over the faithful
  /// fields of every struct reachable from an admitted C-ABI signature: if
  /// exported `A` contains `B`, A's layout depends on B's, so B must be
  /// `#[repr(C)]` too -- a correctness requirement, not an optimisation.
  /// Empty without `--c-abi-exports`, which is what keeps every existing
  /// crate golden byte-identical.
  llvm::StringSet<> cAbiReprCStructs;

  /// FR-179: the owner structs an `emitrust.actor_runtime` anchor manages.
  /// Such an owner's instance is the one the spawned mailbox loop holds, so
  /// a second, thread_local instance beside it would be a SECOND copy of the
  /// state -- the wrapper refuses those rather than export a symbol that
  /// mutates the wrong actor.
  llvm::StringSet<> actorRuntimeOwners;

  /// FR-179: the module-scope singleton name for `owner`.
  static std::string actorSingletonName(StringRef owner) {
    return ("__EMITRUST_ACTOR_" + owner.upper());
  }

  /// FR-140: set when a name RENDERED inside the item under emission would
  /// trip rustc's `non_snake_case` lint; consumed (and reset) by
  /// `closeNonSnakeCaseScope`.
  bool itemTripsNonSnakeCase = false;

  /// FR-140: cleared around the `assignName` calls that name a binding which
  /// is known NOT to render -- an FR-61a tail fold, an FR-61d inline capture
  /// or drop, all of which still claim a name so the surviving v-numbering is
  /// unchanged. A spelling that never reaches the output cannot trip a lint,
  /// and an attribute nobody needs is a byte shift in a golden.
  bool noteBoundNames = true;

  /// Per-function map from SSA values to their Rust binding names.
  DenseMap<Value, std::string> valueNames;

  /// Counter feeding the sequential v0, v1, ... naming scheme.
  unsigned valueCount = 0;

  /// FR-61e: every binding name already emitted in the current function
  /// (named locals AND generated vN), plus the reserved `self` and
  /// `__emitrust_tl` spellings. A named local colliding with an earlier
  /// binding uniquifies with `_1`, `_2`, ... (a shadowing re-`let` is
  /// never emitted), and the vN auto-namer skips numbers whose spelling a
  /// named local has claimed.
  llvm::StringSet<> usedBindingNames;

  /// Ops that are unreachable in the current function (they follow a diverging
  /// op in their block, or are nested inside such an op). They are not
  /// emitted, and a use appearing in one does not count as a real read when
  /// deciding whether a binding is live. Populated per function by
  /// `computeUnreachable`.
  llvm::SmallPtrSet<Operation *, 32> unreachableOps;

  /// Memoizes `valueIsReadUncached` results within a function.
  DenseMap<Value, bool> valueReadCache;

  /// Recomputes `unreachableOps` for `funcBody`'s single block.
  void computeUnreachable(Block &block);

  /// Ops (`emitrust.let` / `emitrust.variable`) whose initializer is a dead
  /// store -- the binding is written on every path before it is read -- so the
  /// emitted binding drops its initializer and relies on Rust's
  /// definite-assignment. Membership means "defer"; the mapped bool is whether
  /// the deferred binding still needs `mut` (some path assigns it more than
  /// once, reassigns across loop iterations, or mutates it after init).
  /// Populated per function by `computeDeferredInits`.
  DenseMap<Operation *, bool> deferredInits;

  /// W2.17: the names of every `emitrust.struct_def` carrying
  /// `emitrust.has_drop`, collected once per module before any function is
  /// emitted. A binding of such a struct is DROPPED at scope end, and
  /// `drop()` reads every field, so no store to it is ever dead and its
  /// initializer can never be deferred -- `let x: T;` (uninitialized) is
  /// never dropped in Rust, so eliding the initializer silently deletes the
  /// destructor's side effects. Measured: clang++ printed `dtor 5`, the
  /// hand-driven Rust printed nothing, and the crate compiled clean.
  llvm::StringSet<> dropStructNames;

  /// FR-124: names of every struct_def whose emitted item must NOT
  /// derive `Copy` -- the local triggers (FR-94 owned-tail opaque field,
  /// W2.17 has_drop, W2.23 has_copy_ctor) closed transitively over
  /// struct-typed fields, because `derive(Copy)` over a non-Copy field is
  /// rustc E0204 (the derived-over-`virtual ~B() = default`-base channel).
  /// Computed once per module in the same pre-pass as dropStructNames.
  llvm::StringSet<> nonCopyStructNames;

  /// FR-150: the Rust PRELUDE names this module's own emitted type items
  /// shadow (`Option`, `Box`, `String`, `Vec` -- the four the emitter
  /// writes). Collected once per module in `emitModule`; EMPTY for every
  /// crate that shadows nothing, which is what keeps the corpus
  /// byte-identical. See include/EmitRust/RustPreludeShadow.h for why
  /// qualification is conditional rather than unconditional, and why the
  /// user's type is never renamed.
  llvm::StringSet<> shadowedPreludeNames;

  /// FR-150: how the emitter must spell prelude name `name` in THIS module --
  /// bare when nothing shadows it, fully qualified when something does.
  llvm::StringRef preludeSpelling(llvm::StringRef name) const {
    return emitrust::preludeSpelling(name, shadowedPreludeNames);
  }

  /// FR-150: the same decision over an emitter-owned Rust FRAGMENT (an opaque
  /// type spelling, a verbatim runtime helper), token by token. Returns the
  /// text unchanged when nothing is shadowed.
  std::string qualifyPrelude(llvm::StringRef text) const {
    return emitrust::qualifyShadowedPreludeNames(text, shadowedPreludeNames);
  }

  /// Whether `binding`'s (possibly lvalue-wrapped, possibly array-element)
  /// type is a struct that carries `emitrust.has_drop`.
  bool bindingHasDrop(Value binding) const {
    Type type = binding.getType();
    if (auto lvalueType = dyn_cast<emitrust::LValueType>(type))
      type = lvalueType.getValueType();
    while (auto arrayType = dyn_cast<emitrust::ArrayType>(type))
      type = arrayType.getElementType();
    auto structType = dyn_cast<emitrust::StructType>(type);
    return structType && dropStructNames.contains(structType.getName());
  }

  /// Summary of how a binding is accessed across a straight-line/structured op
  /// sequence, used to decide whether its initializer is a dead store.
  struct Liveness {
    bool readFirst = false;    ///< some path reads the binding before writing
    bool writtenAtExit = false; ///< all fall-through paths have written it
    bool diverges = false;     ///< all paths diverge (no fall-through)
    bool hasLoopWrite = false; ///< the binding is assigned inside a loop
    unsigned maxWrites = 0;    ///< max whole-binding writes on any single path
    bool loopReassign = false; ///< the binding is reassigned across iterations
    /// FR-105: the ANY-path counterpart of `writtenAtExit` -- SOME path
    /// through this construct leaves it by FALLING THROUGH with the binding
    /// written. `enteringAnyWrite` seeds it, and every loop body is entered
    /// with that seed FALSE, so inside a loop body the flag means exactly
    /// "written on this path since the top of THIS iteration".
    bool anyWriteFallThrough = false;
  };

  /// The liveness of an EMPTY region: no reads, and the entering write-state
  /// simply falls through. A named factory instead of a braced literal so the
  /// meaning cannot silently drift with the struct's field order.
  static Liveness passThroughLiveness(bool enteringWritten,
                                      bool enteringAnyWrite) {
    Liveness l;
    l.writtenAtExit = enteringWritten;
    l.anyWriteFallThrough = enteringAnyWrite;
    return l;
  }

  /// Tracks, across a loop body, whether every `break` that exits the loop
  /// has written the binding on its path.
  struct BreakInfo {
    bool sawBreak = false;
    bool allWritten = true;
    /// FR-105: some `break` path had written the binding SINCE THE TOP
    /// OF THE ITERATION (a write from before the loop does not count).
    bool anyBreakWritten = false;
    /// FR-105: some `continue` -- a back edge -- was reached with the
    /// binding written since the top of the iteration.
    bool anyContinueWritten = false;
  };

  /// `partialWriteBlocks` selects how a projection use of the binding
  /// (`v.x`, `v[i]`) that precedes the first whole write is treated: `true`
  /// (deferral) counts it as a read, since Rust rejects a partial write to an
  /// uninitialized binding (E0381); `false` (dead-store liveness) counts it as
  /// a read only when the projected place is itself read, so a pure partial
  /// write does not keep an earlier store live.
  Liveness analyzeSeq(Block::iterator begin, Block::iterator end, Value binding,
                      bool enteringWritten, bool enteringAnyWrite,
                      BreakInfo *brk, bool partialWriteBlocks);
  Liveness analyzeControl(Operation *op, Value binding, bool enteringWritten,
                          bool enteringAnyWrite, BreakInfo *brk,
                          bool partialWriteBlocks);
  /// Decides, for each candidate binding in the function, whether its init is a
  /// dead store; fills `deferredInits`.
  void computeDeferredInits(Block &block);

  /// Emits the deferred declaration `let [mut] <name>: <type>;` for a binding
  /// whose dead initializer was dropped (see `deferredInits`).
  LogicalResult emitDeferredBinding(Operation *op, Value result, Type type);

  /// Whether any emitted (reachable, non-dead-store) assign targets `value`.
  bool letHasEmittedAssign(Value value);

  /// FR-63 (assign_op_pattern over projection places): whether dropping one
  /// textual render of `value` is provably effect-free. A value outside
  /// `inlineExprs` renders as a NAME (its `let`/argument binding still
  /// emits), so a dropped occurrence is inert; an inline render must be a
  /// pure expression tree (constant, cast chain, load of a pure place) --
  /// anything else (a call, an unclassified producer) is not provably
  /// effect-free and blocks the fold.
  bool isPureRenderedValue(Value value);

  /// FR-63: whether `place`'s projection chain renders as a provably pure
  /// expression: member/enum_raw/subscript/deref links down to a root
  /// `emitrust.variable`, with every non-place operand (subscript index,
  /// deref base) passing `isPureRenderedValue`.
  bool isPureRenderedPlace(Value place);

  /// FR-63: whether `kept` and `dropped` denote the SAME runtime value at
  /// the statement being emitted, such that dropping `dropped`'s render is
  /// effect-free. Same SSA value: yes iff pure (the kept render still uses
  /// every shared name). Distinct defs must be structurally identical pure
  /// twins -- equal constants, cast pairs, or load pairs of the same place
  /// -- and (constants aside) BOTH must render inline HERE: a side hoisted
  /// to an earlier `let` read the place at an earlier program point, which
  /// an intervening write could distinguish.
  bool isSamePureReadValue(Value kept, Value dropped);

  /// FR-63: structural identity of two place-projection chains (the importer
  /// materializes the LHS place and the RHS load's place as separate SSA
  /// chains), pure per `isPureRenderedPlace` so one render can be dropped.
  bool isSamePureReadPlace(Value kept, Value dropped);

  /// Whole-binding `emitrust.assign` stores whose written value is never read
  /// before the binding is overwritten again -- dead stores that emit nothing.
  /// Treated as removed by every other phase (liveness, deferral, mut, naming).
  llvm::SmallPtrSet<Operation *, 16> deadStores;

  /// Populates `deadStores` for the function's entry block.
  void computeDeadStores(Block &entryBlock);

  /// Extends `deadStores` with the one narrow loop-body shape that is decidable
  /// without cross-iteration liveness: a partial store whose binding is wholly
  /// overwritten at the top of every iteration and is not live-out of the loop.
  void computeLoopBodyDeadStores(Block &entryBlock);

  //===--------------------------------------------------------------===//
  // FR-106: the `unused_assignments` risk detector.
  //
  // rustc's `unused_assignments` is an ALL-PATH liveness result, and the
  // crate-wide `unused_assignments = "deny"` (FR-53) turns any residual
  // into a hard build failure with no emitter diagnostic at all. The dead-
  // store elision above cannot reach a store nested in a loop body --
  // deciding that needs the back edge, and CLAUDE.md fences cross-iteration
  // liveness because DELETING such a store miscompiled three times.
  //
  // This detector asks the same question and spends the answer on an
  // ATTRIBUTE instead of a deletion: a marked function is given
  // `#[allow(unused_assignments)]`. Nothing is elided, dropped or
  // reordered, and `deadStores` is never written here -- so a wrong "dead"
  // costs one redundant allow on ONE function, and a wrong "live" costs the
  // loud build error that exists without the detector at all. The crate-
  // wide deny, and therefore the tripwire on every other function, stays.
  //
  // The predicate is ALL-PATHS ("no continuation reads this store"), never
  // some-path. Measured over 3475 emitted functions in 472 rustc-clean
  // crates against rustc 1.96.0, which flags 4 of them: some-path marks
  // 127 (forward only) to 147 (with the back edge) -- a 32-37:1
  // over-application putting an allow in 77+ crates, which is the
  // crate-wide re-allow the deny exists to prevent. All-paths marks exactly
  // those 4 (precision 1.00, recall 1.00). Every shape the analysis does
  // not understand must therefore ANSWER "assume a read" and NOT mark.
  //===--------------------------------------------------------------===//

  /// Set while the current item is rendered; consumed after the body walk.
  bool itemAllowsUnusedAssign = false;

  /// Whether `op` renders a store of the WHOLE `binding` at its own program
  /// point.
  bool uaStoresBinding(Operation *op, Value binding);

  /// Whether ANY continuation of the program point just after `from` reads
  /// `binding` before overwriting it -- following fall-through, the
  /// enclosing construct's exit, a loop's BACK EDGE, and `break`/`continue`.
  bool uaReadAfter(Operation *from, Value binding);

  /// Whether an iteration entered at the TOP of `loopOp` (condition first,
  /// for a `while`) reads `binding` before overwriting it.
  bool uaReadFromLoopTop(Operation *loopOp, Value binding);

  /// Whether the function holds a store rustc's `unused_assignments` may
  /// flag: one with no read on ANY continuation.
  bool uaFunctionAtRisk(Operation *funcOp);

  /// FR-61a: the function-final `emitrust.return` of the current function --
  /// the last op the entry-block walk emits -- rendered as a tail expression
  /// (operand only, no `return`, no `;`) or, when it has no operand, omitted.
  /// Null when the entry block's last emitted op is not a return (e.g. the
  /// body ends in a diverging call). Set per function by `emitFunc`.
  Operation *tailReturn = nullptr;

  /// FR-61a: the single-result let-producing op immediately preceding
  /// `tailReturn` whose only-use result the tail return returns; its binding
  /// never renders and its right-hand side becomes the tail expression. Null
  /// when no such fold applies. Set per function by `emitFunc`.
  Operation *tailFoldCandidate = nullptr;

  /// FR-61a: set by `emitBlockBody` while `tailFoldCandidate` is being
  /// emitted; `emitLetPrologue` consumes it by naming the result (keeping the
  /// v-numbering of every other value unchanged) while printing nothing.
  bool pendingTailFold = false;

  /// FR-61a: the candidate's prologue was suppressed, so the buffer now ends
  /// with `<rhs>;\n`; `emitReturn` removes the `;`, leaving the right-hand
  /// side as the function's tail expression.
  bool tailFoldActive = false;

  /// FR-61b: deferred bindings (`deferredInits` members that need no `mut`)
  /// rendered as `let x: T = if c { .. } else { .. };` -- the mapped
  /// `emitrust.if` immediately follows the binding, both its arms end with
  /// the binding's only two assignments, and each arm's final assignment
  /// becomes the arm's tail expression. Populated per function by
  /// `computeIfExprBindings`.
  DenseMap<Operation *, emitrust::IfOp> ifExprBindings;

  /// FR-61b: the `emitrust.if` ops consumed into an if-expression binding;
  /// the normal statement walk skips them.
  llvm::SmallPtrSet<Operation *, 8> consumedIfs;

  /// Fills `ifExprBindings`/`consumedIfs` from the `deferredInits` already
  /// computed for the current function.
  void computeIfExprBindings();

  // --- FR-63 (clippy::field_reassign_with_default): default + field-store
  //     fusion into a functional-update struct literal ---

  /// Per struct-typed, default-initialized (`let [mut] s: S = S::default();`)
  /// `emitrust.variable`, the maximal PREFIX of consecutive statement-position
  /// assigns to DISTINCT single-level fields of that variable, fused into one
  /// `let s: S = S { f1: v1, .., ..S::default() };` literal (field order =
  /// program assign order; Rust evaluates literal fields in written order and
  /// the derived `S::default()` is pure, so evaluation order is preserved
  /// observably). Populated per function by `computeFieldInitFuses`.
  DenseMap<Operation *, SmallVector<emitrust::AssignOp, 4>> fieldInitFuses;

  /// Reverse map: fused `emitrust.assign` -> its `emitrust.variable`. The
  /// LAST assign of a fuse renders the whole `let` (every fused value's
  /// inline text has been captured by then); the variable and the earlier
  /// fused assigns render nothing. `lvalueIsMutated` skips members, so the
  /// binding's mut-ness is recomputed over the SURVIVING mutations only.
  DenseMap<Operation *, Operation *> fusedAssignOwner;

  /// Fills `fieldInitFuses`/`fusedAssignOwner`. Must run AFTER
  /// `computeDeferredInits` (a deferred binding never fuses) and
  /// `computeInlineCandidates` (the statement scan skips inline-consumed and
  /// dropped ops, which render nothing at their program point).
  void computeFieldInitFuses(emitrust::FuncOp funcOp);

  /// Emits the fused functional-update `let` for `variableOp` (see
  /// `fieldInitFuses`); called from `emitAssign` at the fuse's last assign.
  LogicalResult emitFusedFieldInit(emitrust::VariableOp variableOp);

  // --- FR-132: the late-init merge (clippy::needless_late_init) ---

  /// Deferred declaration -> the SAME-BLOCK `emitrust.assign` that renders the
  /// whole `let [mut] x: T = rhs;` in its place. The declaration itself then
  /// renders nothing at all, so the binding has ONE program point instead of
  /// two. Sound because `computeDeferredInits` has already proved nothing
  /// reads the binding before that write (rustc E0381 would reject today's
  /// output otherwise): a declaration that emits no code, sunk past statements
  /// that cannot observe it, is inert.
  DenseMap<Operation *, Operation *> lateInitMerges;

  /// Reverse map: merging assign -> its declaration. `emitAssign` looks the
  /// assign up here exactly as it looks up `fusedAssignOwner`.
  DenseMap<Operation *, Operation *> lateInitOwner;

  /// Fills `lateInitMerges`/`lateInitOwner`. Must run LAST -- after
  /// `computeIfExprBindings` (FR-61b keeps priority), `computeDroppedOps`,
  /// `computeInlineCandidates`, `computeDeadStores` and
  /// `computeFieldInitFuses` -- because the gap scan asks which ops render.
  void computeLateInitMerges(emitrust::FuncOp funcOp);

  /// Conservative "a binding of this type may own drop glue" test, used by the
  /// merge's drop-order gate. Deliberately WIDER than `bindingHasDrop`, which
  /// only sees `emitrust.has_drop` structs and misses `Box<T>`/STL owners.
  bool typeMayDrop(Type type) const;

  /// Emits the merged `let [mut] <name>: <T> = <rhs>;` at `assignOp`, on
  /// behalf of the declaration `declOp` that rendered nothing.
  LogicalResult emitMergedLateInit(Operation *declOp,
                                   emitrust::AssignOp assignOp);

  // --- FR-133: the literal-Some fn-ptr local (clippy::unnecessary_literal_
  // unwrap) ---

  /// Values whose `!emitrust.fn_ptr` type renders WITHOUT the `Option`
  /// wrapper: the `emitrust.variable` lvalue of a fn-ptr LOCAL proved to be
  /// initialized once from a literal `Some(<fn>)` and only ever CALLED, plus
  /// that initializer constant and every `emitrust.load` of the local. A
  /// member's `Option<fn(..)>` becomes `fn(..)`, its `Some(f)` becomes `f`,
  /// and its `emitrust.call_indirect` drops the `.expect("null function
  /// pointer")` that was redundant the moment the literal was proved.
  ///
  /// RENDERING ONLY: no MLIR type changes, so `call_indirect` still verifies
  /// against `!emitrust.fn_ptr` and the round-trip is untouched.
  DenseSet<Value> unwrappedFnPtrs;

  /// Fills `unwrappedFnPtrs`. Must run LAST, after `computeDeferredInits`
  /// (only a deferred declaration is a candidate -- every other rendering of
  /// a fn-ptr variable emits the synthesized `None` default, which has no
  /// spelling without the `Option`), `computeDeadStores`, `computeDroppedOps`
  /// and `computeLateInitMerges`.
  void computeUnwrappedFnPtrs(emitrust::FuncOp funcOp);

  /// Emits the bare `fn(A, B) -> R` spelling (no `Option` wrapper).
  LogicalResult emitFnPtrSignature(Location loc, emitrust::FnPtrType type);

  /// Emits `type` as the declared type of the binding `value`: the FR-133
  /// unwrapped `fn(..)` when `value` is in `unwrappedFnPtrs`, `emitType`
  /// otherwise.
  LogicalResult emitBindingType(Location loc, Value value, Type type);

  // --- FR-61d: single-use expression inlining + unused-pure-value drops ---

  /// Ops whose single-real-use result renders inline at its consumer
  /// instead of through a `let` binding. Populated per function by
  /// `computeInlineCandidates`; membership is decided entirely up front,
  /// the capture in `emitBlockBody` only records the rendered text.
  llvm::SmallPtrSet<Operation *, 32> inlinedOps;

  /// The captured right-hand-side text of an inlined op's result, plus the
  /// precedence rank the text renders at and whether it ends in a bare
  /// `.. as T` cast (both consulted by `needsParens`).
  struct InlineExpr {
    std::string text;
    Prec prec;
    bool endsInCast;
  };
  DenseMap<Value, InlineExpr> inlineExprs;

  /// Analysis-time flag per accepted candidate: the candidate's transitive
  /// inlined tree contains an `emitrust.load`. A load's text re-reads its
  /// place when rendered, so such a tree must not be inlined into a place
  /// PROJECTION consumer (subscript index, deref operand): projections
  /// render on demand at every consuming load/assign/borrow, which may sit
  /// past a barrier or render more than once. Pure arithmetic over
  /// immutable `let` names is position-independent and stays eligible.
  DenseMap<Value, bool> inlineTreeReadsPlace;

  /// Pure ops with no emitted use at all: they emit nothing, and every
  /// analysis treats their uses as never-rendered (so drops cascade).
  /// Dropping is legal only because `valueIsRead` PROVES no emitted read;
  /// a wrong drop of a read value is a hard rustc E0425, never silent
  /// misbehavior. Dropping an unused `emitrust.div`/`emitrust.rem`
  /// additionally removes a divide-by-zero panic -- that panic exists only
  /// where the C program divided by zero, which is C UB, so the elision
  /// refines UB in the same legal direction as the dead-store elision.
  llvm::SmallPtrSet<Operation *, 16> droppedOps;

  /// Set while an inlined op's statement is being captured;
  /// `emitLetPrologue` consumes it by naming the result (keeping surviving
  /// v-numbering unchanged) while printing nothing, mirroring
  /// `pendingTailFold`.
  bool pendingInlineCapture = false;

  /// Instrumentation: inline captures / drops across the whole translation
  /// (not reset per function; reported behind EMITRUST_INLINE_STATS).
  unsigned inlineCaptureCount = 0;
  unsigned dropCount = 0;

  /// Fills `droppedOps` for the current function with a reverse-program-
  /// order pass (users precede defs, so cascades converge in one sweep).
  /// Must run AFTER `tailFoldCandidate` selection and BEFORE
  /// `computeInlineCandidates`.
  void computeDroppedOps(emitrust::FuncOp funcOp);

  /// FR-130: whether `value` renders without performing any side effect, so
  /// deleting the statement that carries it deletes no effect. Purpose-built
  /// and REFUSE-BY-DEFAULT: a bare name (block argument) and the effect-free
  /// producers pass, everything else -- calls above all -- refuses.
  ///
  /// `isPureRenderedValue` is deliberately NOT reused here: it is keyed on
  /// `inlineExprs`, which `computeInlineCandidates` fills only AFTER
  /// `computeDroppedOps` has run, so at this point it would answer `true`
  /// unconditionally and gate nothing.
  bool isEffectFreeDropCandidate(Value value) const;

  /// FR-130 increment 2: collects into `writes` every `emitrust.assign` that
  /// would have to be deleted along with the never-read binding `letOp`, and
  /// answers whether deleting them is provably effect-free. Refuses -- and
  /// then NOTHING is dropped for this binding, no partial drops -- when the
  /// initializer or any write carries a value that is not effect-free, or
  /// when the binding has a surviving use that is not a whole-binding write
  /// (a place projection, say: dropping the `let` under it would be E0425).
  bool collectDroppableBindingWrites(emitrust::LetOp letOp,
                                     SmallVectorImpl<Operation *> &writes);

  /// Fills `inlinedOps` for the current function. Must run AFTER
  /// `tailFoldCandidate` is selected (the candidate itself is excluded)
  /// and after `computeDroppedOps` (dropped consumers are not real uses).
  void computeInlineCandidates(emitrust::FuncOp funcOp);

  /// The precedence rank `op`'s just-captured text renders at.
  Prec capturedPrec(Operation *op, StringRef text);

  /// FR-61d slice 3: the per-op prelude every statement-emitting loop
  /// shares -- a dropped pure op emits nothing (its name is still
  /// assigned), an inlined op is captured into `inlineExprs` (its buffer
  /// text removed). Returns true when the op was consumed and the caller
  /// must not emit it. Used by `emitBlockBody` and the region loops that
  /// cannot route through it (`emitArmBodyWithTail`, `emitFor`,
  /// `emitGlobalCells`), so candidates inside those bodies inline exactly
  /// like entry-block ones.
  FailureOr<bool> emitDropOrCapture(Operation &op);

  /// Whether `op`'s captured text ends in a bare `.. as T` cast (its own
  /// cast tail, or a bare-rendered inlined right operand's).
  bool capturedEndsInCast(Operation *op);

  /// Whether infix `op`'s right operand renders as bare trailing-cast text.
  bool rhsEndsInCast(Operation *op, Prec rank);

  /// FR-61b: emits `let <name>: <type> = if <cond> {` .. `} else {` .. `};`
  /// for the deferred binding `op` consumed together with `ifOp`.
  LogicalResult emitIfExprBinding(Operation *op, Value result, Type valueType,
                                  emitrust::IfOp ifOp);

  /// FR-61b: emits one arm of an if-expression binding: every statement
  /// except the final assignment to `binding`, whose right-hand side is
  /// emitted as the arm's tail expression.
  LogicalResult emitArmBodyWithTail(Region &region, Value binding);

  /// Returns whether `value` is read (as opposed to only written or unused)
  /// somewhere in reachable code -- used to decide `_`-prefixing and dead
  /// stores. A place-refining projection counts as a read only when the
  /// projected value it produces is itself read.
  bool valueIsRead(Value value);
  bool valueIsReadUncached(Value value);

  /// Returns whether the `!emitrust.lvalue` place `value` is ever mutated in
  /// reachable code (assigned whole, assigned through a projection, mutably
  /// borrowed, or used as a method-call receiver). Conservative toward `true`:
  /// a missed mutation would surface as a hard `cannot assign to immutable`
  /// error at build time, never as silent wrong output.
  bool lvalueIsMutated(Value value);
  /// Whether `call` takes its receiver by `&mut` (so the receiver needs `mut`).
  bool methodCallMutatesReceiver(emitrust::MethodCallOp call);
};

} // namespace

//===----------------------------------------------------------------------===//
// Structural helpers
//===----------------------------------------------------------------------===//

/// Whether `op` renders as a diverging Rust expression (defined below).
static bool opDiverges(Operation *op);

/// Whether `op` is a place-refining projection (`v.x`, `v[i]`, `*p`, `v.0`).
/// Projections emit nothing at their program point; their read/write is
/// attributed to the load/store/borrow that consumes the refined place.
static bool isPlaceProjection(Operation *op) {
  return isa<emitrust::MemberOp, emitrust::SubscriptOp, emitrust::DerefOp,
             emitrust::EnumRawOp>(op);
}

/// Whether `op` is a place projection whose refined BASE (operand 0) is
/// `base`. False for a projection that merely uses `base` as a non-base
/// operand, e.g. a subscript index.
static bool refinesBase(Operation *op, Value base) {
  return isPlaceProjection(op) && op->getOperand(0) == base;
}

/// Follows place-refining projections back to the place they ultimately
/// refine, so a use of `v.x` can be recognized as a use of `v`.
static Value projectionBase(Value value) {
  while (Operation *def = value.getDefiningOp()) {
    if (!isPlaceProjection(def))
      break;
    value = def->getOperand(0); // operand 0 is the refined base
  }
  return value;
}

/// Whether `op` is an `emitrust.assign` whose target is the whole `binding`.
static bool isBindingWrite(Operation *op, Value binding) {
  auto assign = dyn_cast<emitrust::AssignOp>(op);
  return assign && assign.getVar() == binding;
}

/// Returns whether `value` is the function-pointer null constant, i.e. the
/// `emitrust.constant` carrying the opaque `None` produced by the importer's
/// `createFnPtrNone` for a C null function pointer.
static bool isFnPtrNone(Value value) {
  auto constant = value.getDefiningOp<emitrust::ConstantOp>();
  if (!constant)
    return false;
  auto opaque = dyn_cast<emitrust::OpaqueAttr>(constant.getValue());
  return opaque && opaque.getValue() == "None";
}

std::string RustEmitter::claimName(Value value, StringRef base) {
  // The `_`-prefix rule for never-read bindings applies to carried names
  // exactly as to generated ones, and a collision with any earlier binding
  // uniquifies with `_1`, `_2`, ... -- a shadowing re-`let` of the same
  // spelling is never emitted.
  std::string stem = ((valueIsRead(value) ? "" : "_") + base).str();
  std::string candidate = stem;
  for (unsigned i = 1; usedBindingNames.contains(candidate); ++i)
    candidate = (stem + "_" + Twine(i)).str();
  usedBindingNames.insert(candidate);
  valueNames[value] = candidate;
  // FR-140: the ONLY producer of a binding spelling that can trip
  // `non_snake_case` -- a generated `vN`/`_vN` never can -- so noting it
  // here covers every named local, named parameter, loop induction variable
  // and match payload binding at once.
  noteBoundName(candidate);
  return candidate;
}

std::string RustEmitter::assignName(Value value) {
  std::string &name = valueNames[value];
  if (!name.empty())
    return name;
  // FR-61e: a variable carrying its C name (pre-mangled importer-side)
  // binds under that spelling. (Named PARAMETERS are claimed up front by
  // `emitFunc`, so they never reach this path.)
  if (auto variable = value.getDefiningOp<emitrust::VariableOp>())
    if (std::optional<StringRef> cName = variable.getCName())
      return claimName(value, *cName);
  // FR-61e: a signed scalar local kept on the SSA path (alloca -> mem2reg)
  // carries its source name as a `NameLoc` wrapped around the promoted init
  // value's location (set at the declaration store, guarded so only a fresh
  // single-use computation is named). Reaching here means the value is a
  // surviving `let` binding — a single-use temp is inlined by FR-61 and never
  // asks for a name — so bind it under that spelling, collision-uniquified by
  // `claimName` exactly like a `VariableOp` c_name. The name is pre-mangled
  // importer-side, so no re-mangling here.
  if (auto nameLoc = dyn_cast<NameLoc>(value.getLoc()))
    return claimName(value, nameLoc.getName().strref());
  // Generated names skip forward past spellings a named local claimed, so
  // a C local literally named `v3` can never collide with the counter.
  bool read = valueIsRead(value);
  std::string candidate;
  do {
    candidate = ((read ? "v" : "_v") + Twine(valueCount++)).str();
  } while (usedBindingNames.contains(candidate));
  usedBindingNames.insert(candidate);
  name = candidate;
  return name;
}

/// FR-63 (clippy::assertions_on_constants): the importer's deterministic
/// null guard is `emitrust.call_opaque "assert!"(<flag>)` with
/// `args = [<index>, <message>]` (ImportC: a C null deref is UB, so the
/// deterministic panic is a legal refinement). When the flag operand
/// constant-folded to the literal `true` the assertion can never fire, so
/// emitting NOTHING for the statement is behavior-identical -- and the
/// rendered `assert!(true, ...)` is exactly what clippy flags. Only the
/// exact `assert!` callee with a provably-true constant condition
/// qualifies: `assert!(false, ...)` fires (a behavioral panic the
/// byte-diff oracle observes), a non-constant flag guards a real path, and
/// other callees -- notably diverging `panic!`, which is load-bearing for
/// match arms and `opDiverges` -- keep today's rendering.
static bool isVacuousAssert(Operation *op) {
  auto call = dyn_cast<emitrust::CallOpaqueOp>(op);
  if (!call || call.getCallee() != "assert!" || op->getNumResults() != 0)
    return false;
  // The condition is `assert!`'s FIRST argument: with an `args` attribute an
  // index attr selects the operand; without one it is the first operand.
  Value condition;
  if (std::optional<ArrayAttr> args = call.getArgs()) {
    if (args->empty())
      return false;
    auto argIndex = dyn_cast<IntegerAttr>((*args)[0]);
    if (!argIndex || !isa<IndexType>(argIndex.getType()) ||
        argIndex.getValue().uge(op->getNumOperands()))
      return false;
    condition = op->getOperand(argIndex.getInt());
  } else {
    if (op->getNumOperands() == 0)
      return false;
    condition = op->getOperand(0);
  }
  auto constant = condition.getDefiningOp<emitrust::ConstantOp>();
  if (!constant)
    return false;
  auto flag = dyn_cast<IntegerAttr>(constant.getValue());
  return flag && flag.getType().isInteger(1) && !flag.getValue().isZero();
}

/// FR-63 (clippy::identity_op): the lhs of a shift whose amount is the
/// literal integer 0, or null. `x << 0` / `x >> 0` compute exactly `x` for
/// every integer type (a zero amount shifts in no bits, and it is always
/// below the width, so no panic path exists either;
/// `SameOperandsAndResultType` makes the lhs's type the result's), so
/// `emitBinary` renders the lhs ALONE. Only the literal-zero constant
/// amount qualifies: a non-zero or non-constant amount keeps the infix
/// rendering, and no other identity (`+ 0`, `* 1`, `| 0`, ...) folds here.
static Value zeroShiftLhs(Operation *op) {
  if (!op || !isa<emitrust::ShlOp, emitrust::ShrOp>(op))
    return Value();
  auto constant = op->getOperand(1).getDefiningOp<emitrust::ConstantOp>();
  if (!constant)
    return Value();
  auto amount = dyn_cast<IntegerAttr>(constant.getValue());
  if (!amount || !amount.getValue().isZero())
    return Value();
  return op->getOperand(0);
}

/// FR-63 (clippy::identity_op): whether `use` is the never-rendered zero
/// amount of a folded shift, so read-tracking must not count it (a zero
/// constant read only by folded shifts would otherwise orphan a `let`).
/// One carve-out keeps read-tracking aligned with emission: a zero-shift
/// consumed as an assign's VALUE may render through the FR-63
/// compound-assign path (`x <<= 0`), which prints the amount verbatim --
/// that pre-existing dead-statement shape is out of this fold's scope --
/// so the amount stays read there.
static bool isDroppedZeroShiftAmount(OpOperand &use) {
  Operation *owner = use.getOwner();
  if (use.getOperandNumber() != 1 || !zeroShiftLhs(owner))
    return false;
  for (Operation *user : owner->getResult(0).getUsers())
    if (auto assign = dyn_cast<emitrust::AssignOp>(user))
      if (assign.getValue() == owner->getResult(0))
        return false;
  return true;
}

/// FR-63 (clippy::unnecessary_cast): the enum def `enumType` names, or null
/// when none is visible from `op` (no def keeps today's rendering -- the
/// conservative direction). Resolved through EnumDefOp::lookupFrom
/// (FR-113): `emitrust.impl` is a SymbolTable, so a nearest-table lookup
/// from inside a C++ method could never see the module-level enum_def.
static emitrust::EnumDefOp lookupEnumDef(Operation *op,
                                         emitrust::EnumType enumType) {
  return emitrust::EnumDefOp::lookupFrom(op, enumType.getName());
}

/// FR-166: the Rust spelling of the open enum's tuple-struct storage
/// integer. The two unit markers on the def are orthogonal, so the storage
/// is one of four: `i32` (neither marker, the pre-FR-166 default and hence
/// the byte-identical path), `u32`, `i64`, `u64`. This is the SINGLE source
/// of that spelling -- `emitEnumDef` declares the field with it and
/// `emitCast`'s constructor branch converts to it, and the two must never
/// disagree.
static llvm::StringRef enumStorageSpelling(emitrust::EnumDefOp enumDef) {
  if (enumDef.getWideUnderlying())
    return enumDef.getUnsignedUnderlying() ? "u64" : "i64";
  return enumDef.getUnsignedUnderlying() ? "u32" : "i32";
}

/// FR-63 (clippy::unnecessary_cast): whether `type` renders exactly as the
/// open enum's tuple-struct raw field -- the spelling
/// `enumStorageSpelling` prints, matched on both width (FR-166's
/// `wide_underlying`) and signedness.
static bool rendersAsEnumUnderlying(emitrust::EnumDefOp enumDef, Type type) {
  auto intType = dyn_cast<IntegerType>(type);
  unsigned storageWidth = enumDef.getWideUnderlying() ? 64 : 32;
  return intType && intType.getWidth() == storageWidth &&
         intType.isUnsigned() == enumDef.getUnsignedUnderlying();
}

/// FR-63 (clippy::unnecessary_cast): whether the ` as T` tail of a
/// non-enum-result cast is the identity -- the rendered source expression
/// already has the rendered target type -- so `emitCast` drops it. Two
/// source shapes qualify: an enum-typed source reads its raw field
/// (`<recv>.0`, typed by the def's u32/i32 underlying), identity exactly
/// when T matches that underlying (`c.0 as i32` over a u32 field is a REAL
/// conversion and keeps its cast); any other source renders at its own
/// type, identity when source and target types render alike -- `emitType`
/// spells signless and signed integers identically (`iN`), so rendered,
/// not nominal, equality is the authority. `i1` renders `bool`: a cast TO
/// bool is rejected upstream and a bool source is a real conversion, so
/// neither joins. The enum-CONSTRUCTION identity (`Name(x as i32)` ->
/// `Name(x)`) is a separate site inside the ctor parens, handled in
/// `emitCast`'s enum-result branch; it never changes the captured
/// Postfix/no-tail classification, while this tail drop does (see
/// `capturedPrec` / `capturedEndsInCast`).
static bool isIdentityCastTail(emitrust::CastOp castOp) {
  Operation *op = castOp.getOperation();
  Type resultType = op->getResult(0).getType();
  if (isa<emitrust::EnumType>(resultType) || resultType.isInteger(1))
    return false;
  Type sourceType = op->getOperand(0).getType();
  if (auto enumType = dyn_cast<emitrust::EnumType>(sourceType)) {
    auto enumDef = lookupEnumDef(op, enumType);
    return enumDef && rendersAsEnumUnderlying(enumDef, resultType);
  }
  if (sourceType == resultType)
    return true;
  auto sourceInt = dyn_cast<IntegerType>(sourceType);
  auto resultInt = dyn_cast<IntegerType>(resultType);
  return sourceInt && resultInt &&
         sourceInt.getWidth() == resultInt.getWidth() &&
         sourceInt.getWidth() != 1 &&
         sourceInt.isUnsigned() == resultInt.isUnsigned();
}

bool RustEmitter::valueIsRead(Value value) {
  auto it = valueReadCache.find(value);
  if (it != valueReadCache.end())
    return it->second;
  // Seed `false` first so a projection cycle (which SSA place graphs do not
  // form, but defensively) terminates rather than recursing forever.
  valueReadCache[value] = false;
  bool result = valueIsReadUncached(value);
  valueReadCache[value] = result;
  return result;
}

bool RustEmitter::valueIsReadUncached(Value value) {
  for (OpOperand &use : value.getUses()) {
    Operation *owner = use.getOwner();
    // A use inside unreachable code, a dropped dead store, an FR-61d
    // dropped pure op, or an FR-63 vacuous assert (constant-true null
    // guard) never emits, so it is not a real read.
    if (unreachableOps.count(owner) || deadStores.count(owner) ||
        droppedOps.count(owner) || isVacuousAssert(owner))
      continue;
    // FR-63 (clippy::identity_op): the literal-zero amount of a folded
    // shift never renders (`emitBinary` emits the lhs alone), so that use
    // is not a read. The lhs use (operand 0) keeps its ordinary read.
    if (isDroppedZeroShiftAmount(use))
      continue;
    // A `let` whose dead initializer we dropped no longer reads its init
    // operand, so that use does not keep `value` live.
    if (deferredInits.count(owner))
      if (auto letOp = dyn_cast<emitrust::LetOp>(owner))
        if (use.get() == letOp.getInit())
          continue;
    // A fn-pointer null constant eq/ne-compared renders as an Option null-test
    // (`.is_none()`/`.is_some()`) on the other operand; the null side is never
    // emitted, so it is not a read. The guard mirrors `emitCmp`'s routing to
    // `emitFnPtrCmp` exactly.
    if (auto cmp = dyn_cast<emitrust::CmpOp>(owner))
      if ((cmp.getPredicate() == emitrust::CmpPredicate::eq ||
           cmp.getPredicate() == emitrust::CmpPredicate::ne) &&
          isFnPtrNone(value))
        continue;
    // The destination of an assignment is a write, not a read.
    if (auto assign = dyn_cast<emitrust::AssignOp>(owner))
      if (assign.getVar() == value)
        continue;
    // A place-refining op emits nothing itself.
    if (isPlaceProjection(owner)) {
      Value place = owner->getResult(0);
      if (refinesBase(owner, value)) {
        // `value` is the refined BASE: read through the projection only when
        // the projected place is itself read.
        if (valueIsRead(place))
          return true;
        continue;
      }
      // `value` is a non-base operand (a subscript index). It is emitted, and
      // so read, only when the projected place is actually emitted -- read,
      // or written ANYWHERE in its refinement tree (`v[i] = ..` but also
      // `v[i].x = ..`, a mutable borrow, a mutating method call), which is
      // exactly `lvalueIsMutated`. FR-61d made this exactness load-bearing:
      // a dropped index def with a still-rendered place is a hard E0425.
      if (valueIsRead(place) || lvalueIsMutated(place))
        return true;
      continue;
    }
    return true;
  }
  return false;
}

/// Whether an STL method of the given name takes `&mut self`. The importer
/// emits a small, closed set of STL method names; the mutating ones are listed
/// here.
static bool isMutatingStlMethod(llvm::StringRef name) {
  static const llvm::StringSet<> mutating = {
      "push",   "push_str", "pop",       "clear",   "insert",
      "remove", "truncate", "resize",    "reserve", "append",
      "extend", "retain",   "sort",      "reverse", "swap",
      "set",    "drain",    "split_off", "shrink_to_fit"};
  return mutating.contains(name);
}

/// The module's `emitrust.actor_runtime` anchor whose "<Actor>Handle"
/// opaque handle type is the receiver of `call`, or null when the receiver
/// is not an actor handle (the FR-62 slice-5b type decision: the handle is
/// `!emitrust.opaque<"<Actor>Handle">`, resolved through the anchors — no
/// new handle type crosses the surface).
static emitrust::ActorRuntimeOp handleAnchorFor(emitrust::MethodCallOp call) {
  Type receiverType = call.getReceiver().getType();
  Type valueType = isa<emitrust::LValueType>(receiverType)
                       ? cast<emitrust::LValueType>(receiverType).getValueType()
                       : receiverType;
  auto opaqueType = dyn_cast<emitrust::OpaqueType>(valueType);
  if (!opaqueType)
    return {};
  auto module = call->getParentOfType<ModuleOp>();
  if (!module)
    return {};
  for (auto runtime : module.getOps<emitrust::ActorRuntimeOp>())
    if (opaqueType.getValue() == (runtime.getActor() + "Handle").str())
      return runtime;
  return {};
}

/// FR-62 slice 5c: whether `call` is a driver call on an ASYNC actor
/// handle — exactly the sites that append `.await` (immediate await = at
/// most one message in flight, so effect order equals program order) and
/// force their containing function to render `async fn`.
static bool isAsyncHandleCall(emitrust::MethodCallOp call) {
  emitrust::ActorRuntimeOp anchor = handleAnchorFor(call);
  return anchor && anchor.getMode() == emitrust::ActorMode::async;
}

bool RustEmitter::methodCallMutatesReceiver(emitrust::MethodCallOp call) {
  Type receiverType = call.getReceiver().getType();
  Type valueType = isa<emitrust::LValueType>(receiverType)
                       ? cast<emitrust::LValueType>(receiverType).getValueType()
                       : receiverType;
  // An opaque receiver is either a recognized STL container or an actor
  // HANDLE ("<Actor>Handle", the FR-62 slice-5b type decision: no new
  // handle type crosses the surface). Resolve through the module's
  // actor_runtime anchors FIRST: every handle wrapper takes `&mut self`
  // (each call sends through the mailbox) and `shutdown` consumes the
  // handle, while the closed STL name list would classify the read-style
  // wrapper names (`get_*`) as non-mutating and render the handle binding
  // without `mut` — a hard E0596 in the emitted crate (found by the spike).
  if (isa<emitrust::OpaqueType>(valueType)) {
    if (handleAnchorFor(call))
      return true;
    // FR-96: the Option-member payload projection (`as_mut().unwrap()`)
    // takes `&mut self` — the receiver binding must be mutable even though
    // the composite spelling is outside the closed STL name set. Without
    // this, a container whose only remaining projection is a read (its
    // final `None` store is a proven-dead store the emitter drops) renders
    // a non-mut binding and dies as rustc E0596 (found by the FR-96
    // owned-free-wrapper shape).
    if (call.getMethod().starts_with("as_mut"))
      return true;
    // W2.21: a `Box<Struct>` receiver (std::unique_ptr). The method is the
    // PAYLOAD struct's own, called through Rust auto-deref, so its
    // mutability is the payload method's `self` — not anything the closed
    // STL name set can answer. Without this branch a `Box<Node>` whose only
    // uses are user-named methods renders `let q: Box<Node>` and dies as
    // rustc E0596 ("cannot borrow `*q` as mutable"), measured on the
    // spike's loop-body box.
    if (call.getMethod() != "as_ref" && call.getMethod() != "as_mut") {
      llvm::StringRef spelling =
          cast<emitrust::OpaqueType>(valueType).getValue();
      if (spelling.starts_with("Box<") && spelling.ends_with(">")) {
        llvm::StringRef payload = spelling.drop_front(4).drop_back(1);
        if (!payload.contains('<'))
          valueType = emitrust::StructType::get(call.getContext(), payload);
      }
    }
    if (!isa<emitrust::StructType>(valueType))
      // A recognized STL container: classify by the (closed) method-name
      // set.
      return isMutatingStlMethod(call.getMethod());
  }
  // A user struct method: the receiver is mutable exactly when the method's
  // `self` parameter is a `&mut` reference.
  auto structType = dyn_cast<emitrust::StructType>(valueType);
  if (!structType)
    return true; // unknown receiver shape: assume it can mutate
  auto module = call->getParentOfType<ModuleOp>();
  if (!module)
    return true;
  for (auto implOp : module.getOps<emitrust::ImplOp>()) {
    // W2.17: skip trait impls -- `impl Drop for T`'s single member is named
    // `drop`, which would otherwise answer a method-mutability query for a
    // user method spelled `drop` (a legal C++ member name, measured to take
    // the module symbol `<Struct>_drop` on its own).
    if (implOp.getStructName() != structType.getName() ||
        implOp.getTraitName())
      continue;
    for (auto funcOp : implOp.getBody().front().getOps<emitrust::FuncOp>()) {
      if (SymbolTable::getSymbolName(funcOp).getValue() != call.getMethod())
        continue;
      if (funcOp->hasAttr(emitrust::kStaticMethodAttrName))
        return false; // an associated function has no receiver
      Block &entry = funcOp.getFunctionBody().front();
      return entry.getNumArguments() > 0 &&
             isa<emitrust::MutRefType>(entry.getArgument(0).getType());
    }
  }
  return true; // method not found in this module: assume it can mutate
}

bool RustEmitter::lvalueIsMutated(Value value) {
  for (OpOperand &use : value.getUses()) {
    Operation *owner = use.getOwner();
    if (unreachableOps.count(owner) || deadStores.count(owner) ||
        droppedOps.count(owner))
      continue;
    // FR-63 (field_reassign_with_default): a fused field store renders as a
    // literal field, not as a mutation, so it must not force `mut` on the
    // fused binding (deny(unused_mut) makes a stale `mut` a hard rustc
    // failure). Pre-fuse analyses see the maps empty and keep today's
    // conservative answer; only the fused binding's own emission consults
    // the post-fuse answer. (For a VALUE use of an lvalue the assign case
    // below `continue`s anyway, so this skip cannot hide a real mutation.)
    if (fusedAssignOwner.count(owner))
      continue;
    if (auto assign = dyn_cast<emitrust::AssignOp>(owner)) {
      if (assign.getVar() == value)
        return true;
      continue; // used as the assigned value: a read, not a mutation
    }
    if (auto addrOf = dyn_cast<emitrust::AddrOfOp>(owner)) {
      if (addrOf.getIsMut())
        return true;
      continue;
    }
    // No base/index identity test here, unlike the FR-142 guard in
    // `computeDeferredInits`: `value` is always an LVALUE on this path, and an
    // `emitrust.slice_of` INDEX is `AnyInteger|Index`, so an lvalue can never
    // reach this branch as the index. The unguarded shape is latent, not live
    // -- do not "harden" it blind, since a wrong narrowing here drops a `mut`
    // that a real mutable borrow needs (rustc E0596).
    if (auto sliceOf = dyn_cast<emitrust::SliceOfOp>(owner)) {
      if (sliceOf.getIsMut())
        return true;
      continue;
    }
    // A place refined by a projection is mutated when the refined place is --
    // but only when `value` is the refined BASE, not a subscript index.
    if (refinesBase(owner, value)) {
      if (lvalueIsMutated(owner->getResult(0)))
        return true;
      continue;
    }
    // A mutating method call (`&mut self`) mutates its receiver.
    if (auto call = dyn_cast<emitrust::MethodCallOp>(owner))
      if (call.getReceiver() == value && methodCallMutatesReceiver(call))
        return true;
  }
  return false;
}

void RustEmitter::computeUnreachable(Block &block) {
  bool diverged = false;
  for (Operation &op : block) {
    if (diverged) {
      // This op and everything nested in it is unreachable.
      op.walk([&](Operation *nested) { unreachableOps.insert(nested); });
      continue;
    }
    for (Region &region : op.getRegions())
      for (Block &nested : region)
        computeUnreachable(nested);
    if (opDiverges(&op))
      diverged = true;
  }
}

RustEmitter::Liveness RustEmitter::analyzeControl(Operation *op, Value binding,
                                                  bool enteringWritten,
                                                  bool enteringAnyWrite,
                                                  BreakInfo *brk,
                                                  bool partialWriteBlocks) {
  Liveness r;
  if (auto ifOp = dyn_cast<emitrust::IfOp>(op)) {
    auto arm = [&](Region &region) {
      return region.empty()
                 ? Liveness{}
                 : analyzeSeq(region.front().begin(), region.front().end(),
                              binding, enteringWritten, enteringAnyWrite, brk,
                              partialWriteBlocks);
    };
    Liveness thenL = arm(ifOp.getThenRegion());
    // An if with no else falls through with only the entering write-state.
    Liveness elseL = ifOp.getElseRegion().empty()
                         ? passThroughLiveness(enteringWritten, enteringAnyWrite)
                         : arm(ifOp.getElseRegion());
    r.readFirst = thenL.readFirst || elseL.readFirst;
    r.hasLoopWrite = thenL.hasLoopWrite || elseL.hasLoopWrite;
    r.loopReassign = thenL.loopReassign || elseL.loopReassign;
    bool thenW = thenL.diverges || thenL.writtenAtExit;
    bool elseW = elseL.diverges || elseL.writtenAtExit;
    r.writtenAtExit = thenW && elseW;
    r.anyWriteFallThrough =
        thenL.anyWriteFallThrough || elseL.anyWriteFallThrough;
    r.diverges = thenL.diverges && elseL.diverges;
    r.maxWrites = std::max(thenL.maxWrites, elseL.maxWrites);
    return r;
  }
  if (isa<emitrust::ForOp, emitrust::LoopOp, emitrust::WhileOp>(op)) {
    // FR-61c: `emitrust.while` analyzes its BODY like the other loops; its
    // condition region is a pure chain that can only READ the binding, and
    // it runs before the body on every iteration (including a zeroth
    // iteration that never enters the body), so its reads merge in below.
    Region &body = isa<emitrust::WhileOp>(op) ? op->getRegion(1)
                                              : op->getRegion(0);
    BreakInfo bodyBreaks;
    Liveness bodyL =
        body.empty()
            ? Liveness{}
            : analyzeSeq(body.front().begin(), body.front().end(), binding,
                         enteringWritten, /*enteringAnyWrite=*/false,
                         &bodyBreaks, partialWriteBlocks);
    if (auto whileOp = dyn_cast<emitrust::WhileOp>(op)) {
      Region &condition = whileOp.getCondition();
      Liveness condL = analyzeSeq(condition.front().begin(),
                                  condition.front().end(), binding,
                                  enteringWritten, /*enteringAnyWrite=*/false,
                                  /*brk=*/nullptr, partialWriteBlocks);
      // The condition runs first: its read precedes any body write.
      bodyL.readFirst = condL.readFirst || bodyL.readFirst;
    }
    r.readFirst = bodyL.readFirst;
    r.hasLoopWrite = bodyL.hasLoopWrite || bodyL.maxWrites > 0;
    // `mut` is needed only when a write can recur, and it recurs exactly when
    // SOME body path writes the binding and reaches the BACK EDGE -- by
    // falling through the body, or by a `continue`. A write whose every path
    // ends in `break`/`return` never reaches it and keeps its bare `let`.
    // FR-105: this is an ANY-path question, and asking the ALL-path
    // `writtenAtExit` instead lost the `mut` on the commonest shape there is
    // (a lifted C `for`, whose write sits under the loop's own condition) --
    // `error[E0384]` at build time. `anyWriteFallThrough` is reseeded false
    // at the body entry, so only writes from THIS iteration can qualify.
    bool backEdgeWrite =
        bodyL.anyWriteFallThrough || bodyBreaks.anyContinueWritten;
    r.loopReassign = bodyL.loopReassign || backEdgeWrite;
    // Leaving the loop with the binding written since the top of the
    // iteration: through a `break` that had written it, or -- for a `for`/
    // `while`, whose head can end the loop -- by a body fall-through.
    r.anyWriteFallThrough = bodyBreaks.anyBreakWritten;
    if (!isa<emitrust::LoopOp>(op))
      r.anyWriteFallThrough |= bodyL.anyWriteFallThrough;
    r.maxWrites = bodyL.maxWrites;
    if (isa<emitrust::LoopOp>(op)) {
      // An `emitrust.loop` is a bare `loop {}`; it exits only through a
      // `break`, so the binding is written at exit exactly when every break
      // wrote it first. With no break it never exits (diverges).
      r.writtenAtExit = bodyBreaks.sawBreak && bodyBreaks.allWritten;
      r.diverges = !bodyBreaks.sawBreak;
    }
    // A `for` may exhaust its range and fall through with the binding still
    // unwritten, so it never guarantees a write at exit.
    return r;
  }
  if (auto sw = dyn_cast<emitrust::SwitchOp>(op)) {
    auto seq = [&](Region &region) {
      return region.empty()
                 ? passThroughLiveness(enteringWritten, enteringAnyWrite)
                 : analyzeSeq(region.front().begin(), region.front().end(),
                              binding, enteringWritten, enteringAnyWrite, brk,
                              partialWriteBlocks);
    };
    Liveness d = seq(sw.getDefaultRegion());
    bool anyWriteFT = d.anyWriteFallThrough;
    bool anyRead = d.readFirst;
    bool allWritten = d.diverges || d.writtenAtExit;
    bool allDiverge = d.diverges;
    bool loopWrite = d.hasLoopWrite;
    bool loopReassign = d.loopReassign;
    unsigned maxWrites = d.maxWrites;
    for (Region &caseRegion : sw.getCaseRegions()) {
      Liveness ci = seq(caseRegion);
      anyRead |= ci.readFirst;
      anyWriteFT |= ci.anyWriteFallThrough;
      allWritten = allWritten && (ci.diverges || ci.writtenAtExit);
      allDiverge = allDiverge && ci.diverges;
      loopWrite |= ci.hasLoopWrite;
      loopReassign |= ci.loopReassign;
      maxWrites = std::max(maxWrites, ci.maxWrites);
    }
    r.readFirst = anyRead;
    r.writtenAtExit = allWritten; // the `_` arm covers every unmatched value
    r.anyWriteFallThrough = anyWriteFT;
    r.diverges = allDiverge;
    r.hasLoopWrite = loopWrite;
    r.loopReassign = loopReassign;
    r.maxWrites = maxWrites;
    return r;
  }
  // Any other region-carrying op: conservatively assume its regions may read
  // the binding and never guarantee a write.
  for (Region &region : op->getRegions())
    if (!region.empty()) {
      Liveness sub =
          analyzeSeq(region.front().begin(), region.front().end(), binding,
                     enteringWritten, enteringAnyWrite, brk, partialWriteBlocks);
      r.readFirst |= sub.readFirst;
      r.hasLoopWrite |= sub.hasLoopWrite || sub.maxWrites > 0;
      r.loopReassign |= sub.loopReassign;
      r.anyWriteFallThrough |= sub.anyWriteFallThrough;
    }
  return r;
}

RustEmitter::Liveness RustEmitter::analyzeSeq(Block::iterator begin,
                                              Block::iterator end, Value binding,
                                              bool enteringWritten,
                                              bool enteringAnyWrite,
                                              BreakInfo *brk,
                                              bool partialWriteBlocks) {
  Liveness r;
  bool written = enteringWritten; // binding written on the current path
  unsigned writes = 0;            // whole-binding writes on the current path
  // FR-105: written on SOME path reaching here, counting only writes
  // since the seed point (the top of the enclosing loop iteration).
  bool anyWritten = enteringAnyWrite;
  for (auto it = begin; it != end; ++it) {
    Operation *op = &*it;
    if (unreachableOps.count(op) || deadStores.count(op))
      continue;
    // A `break` exits the enclosing loop: record whether the binding is
    // written on this path, then end the path.
    if (isa<emitrust::BreakOp>(op)) {
      if (brk) {
        brk->sawBreak = true;
        brk->allWritten = brk->allWritten && written;
        brk->anyBreakWritten |= anyWritten;
      }
      r.diverges = true;
      r.maxWrites = std::max(r.maxWrites, writes);
      return r;
    }
    // A `continue` returns to the loop head; this straight-line path ends
    // without exiting the loop.
    if (isa<emitrust::ContinueOp>(op)) {
      if (brk)
        brk->anyContinueWritten |= anyWritten;
      r.diverges = true;
      r.maxWrites = std::max(r.maxWrites, writes);
      return r;
    }
    // Classify an assignment as a whole write, a partial write, and/or a read
    // of the binding through its value operand.
    if (auto assign = dyn_cast<emitrust::AssignOp>(op)) {
      Value target = assign.getVar();
      if (target == binding) {
        written = true;
        anyWritten = true;
        ++writes;
      } else if (projectionBase(target) == binding) {
        // A partial write (`v.x = ..`). It reads nothing of the binding, but
        // Rust forbids it before the binding is initialized (E0381), so under
        // `partialWriteBlocks` it blocks deferral.
        if (partialWriteBlocks && !written)
          r.readFirst = true;
      }
      if (!written && projectionBase(assign.getValue()) == binding)
        r.readFirst = true; // the assigned value reads the binding
      continue; // `emitrust.assign` carries no regions
    }
    // A place-refining projection emits nothing and is not itself an event; its
    // read/write is attributed to the load/store/borrow that consumes it. Only
    // a non-base operand (e.g. an index that is the binding) reads it here.
    unsigned baseOperands = isPlaceProjection(op) ? 1 : 0;
    if (!written)
      for (unsigned i = baseOperands, e = op->getNumOperands(); i < e; ++i)
        if (projectionBase(op->getOperand(i)) == binding) {
          r.readFirst = true;
          break;
        }
    if (op->getNumRegions() > 0) {
      Liveness sub = analyzeControl(op, binding, written, anyWritten, brk,
                                    partialWriteBlocks);
      if (sub.readFirst && !written)
        r.readFirst = true;
      r.hasLoopWrite |= sub.hasLoopWrite;
      r.loopReassign |= sub.loopReassign;
      // A construct that never falls through ends this path; otherwise the
      // ANY-write state is its own fall-through state, or the entering one
      // carried across it.
      anyWritten = sub.anyWriteFallThrough || (anyWritten && !sub.diverges);
      writes += sub.maxWrites;
      if (!written && sub.writtenAtExit)
        written = true;
      if (sub.diverges) {
        r.diverges = true;
        r.maxWrites = std::max(r.maxWrites, writes);
        return r;
      }
      continue;
    }
    if (opDiverges(op)) {
      r.diverges = true;
      r.maxWrites = std::max(r.maxWrites, writes);
      return r;
    }
  }
  r.writtenAtExit = written;
  r.anyWriteFallThrough = anyWritten;
  r.maxWrites = std::max(r.maxWrites, writes);
  return r;
}

void RustEmitter::computeDeferredInits(Block &block) {
  block.getParentOp()->walk([&](Operation *op) {
    Value binding;
    if (auto letOp = dyn_cast<emitrust::LetOp>(op)) {
      binding = letOp.getResult();
    } else if (auto variableOp = dyn_cast<emitrust::VariableOp>(op)) {
      // Only a synthesized default (no explicit initializer attribute) is a
      // candidate. The definite-assignment analysis is what keeps deferral
      // safe: a projection of the binding (a partial write `v.x = ..` or a
      // read) counts as a use, so if it precedes the first whole write the
      // binding is `readFirst` and is not deferred (an aggregate filled purely
      // field-by-field keeps its synthesized default). A whole reassignment
      // (`v = other`) of any type fully initializes the binding, after which
      // partial writes are fine.
      if (variableOp.getInitAttr())
        return;
      binding = variableOp.getResult();
    } else {
      return;
    }
    // W2.17: a binding whose struct carries a `Drop` impl is (almost) never
    // deferred. `let x: T;` is an uninitialized Rust binding and is never
    // dropped, so deferring the initializer deletes the destructor's side
    // effects outright -- a compile-clean miscompile. (`drop()` is a read
    // of the whole object on every path, so the initializer is by
    // definition live.)
    //
    // W2.23 carves out the ONE shape where the placeholder itself is the
    // miscompile: a droppy binding whose first touch on EVERY path is a
    // WHOLE-value assignment (`Loud lt = loud_two(..)`, and the SCF
    // result-lets of a two-return copy function). There the synthesized
    // `Loud::default()` stands in for an object the C++ program never
    // constructed, and the whole assignment DROPS it -- a phantom `dtor 0`
    // against nothing in native (measured). Deferring is exactly correct:
    // definite assignment on every fall-through path (`writtenAtExit`, or
    // a diverging path on which native constructs nothing either)
    // reproduces "the object exists from its constructing assignment".
    // Every W2.17 shape keeps its placeholder: a constructor call is a
    // &mut borrow (a READ, so `readFirst` holds), a never-touched guard
    // binding has no whole write (`writtenAtExit` fails), and the
    // reassignment/loop-carry shapes the importer never emits for droppy
    // bindings stay conservatively undeferred.
    bool hasDrop = bindingHasDrop(binding);
    // Only worth deferring when the binding is actually read; an unread
    // binding is `_`-prefixed instead (its dead init then draws no
    // warning). A droppy binding proceeds regardless: its scope-end drop
    // is an observable read no user list shows.
    bool isRead = false;
    for (Operation *user : binding.getUsers()) {
      if (unreachableOps.count(user) || droppedOps.count(user) ||
          isBindingWrite(user, binding))
        continue;
      isRead = true;
      break;
    }
    if (!isRead && !hasDrop)
      return;
    Liveness info =
        analyzeSeq(std::next(op->getIterator()), op->getBlock()->end(), binding,
                   /*enteringWritten=*/false, /*enteringAnyWrite=*/false,
                   /*brk=*/nullptr, /*partialWriteBlocks=*/true);
    if (info.readFirst)
      return; // the initializer is live: some path reads before writing
    if (hasDrop && (!(info.writtenAtExit || info.diverges) ||
                    info.maxWrites >= 2 || info.loopReassign))
      return; // W2.17 posture for every shape but the proven one above
    // `mut` is needed when a path assigns more than once, when the binding is
    // reassigned across loop iterations, or when it is mutably borrowed after
    // its (single) initializing assignment.
    bool postInitMutation = false;
    for (Operation *user : binding.getUsers()) {
      if (unreachableOps.count(user))
        continue;
      // `emitrust.addr_of` has a SINGLE operand, so a use through it is
      // necessarily a borrow OF the binding; the identity test is spelled out
      // anyway so this arm cannot rot into the `slice_of` defect below if the
      // op ever grows an operand.
      if (auto addrOf = dyn_cast<emitrust::AddrOfOp>(user))
        postInitMutation |= addrOf.getIsMut() && addrOf.getOperand() == binding;
      // FR-142: `emitrust.slice_of` has TWO operands -- `$base` and `$index` --
      // and `getUsers()` returns the op through EITHER. Only a mutable borrow
      // of the BASE mutates the binding; `&mut other[binding as usize..]`
      // merely READS the binding as the start index (the same rule the comment
      // below states for projections, and the same identity test the
      // `MethodCallOp` arm applies to its receiver). Scoring the index use as
      // a mutation left the binding `let mut` with no reassignment anywhere,
      // which the emitted crate's own `unused_mut = "deny"` rejects: exit 0,
      // an unbuildable crate, and no diagnostic.
      if (auto sliceOf = dyn_cast<emitrust::SliceOfOp>(user))
        postInitMutation |= sliceOf.getIsMut() && sliceOf.getBase() == binding;
      if (auto call = dyn_cast<emitrust::MethodCallOp>(user))
        postInitMutation |=
            call.getReceiver() == binding && methodCallMutatesReceiver(call);
      // A partial write (`v.x = ..`, `v[i] = ..`) mutates the binding after
      // its initializing whole assignment -- but only when the binding is the
      // refined BASE, not a subscript index (`other[binding]` merely reads it).
      if (refinesBase(user, binding))
        postInitMutation |= lvalueIsMutated(user->getResult(0));
    }
    deferredInits[op] =
        info.maxWrites >= 2 || info.loopReassign || postInitMutation;
  });
}

/// Whether `binding` is ever borrowed (`&v` / `&mut v` / a slice borrow). A
/// borrow can alias the binding, so a store's value could be observed through
/// the reference even when no direct read of the binding follows -- such a
/// binding is excluded from loop-body dead-store elision.
static bool bindingIsBorrowed(Value binding) {
  for (Operation *user : binding.getUsers())
    if (isa<emitrust::AddrOfOp, emitrust::SliceOfOp>(user))
      return true;
  return false;
}

/// Whether the first op in `body` (in program order) that touches `binding` is
/// an unconditional whole overwrite of it (`binding = ..` at the top level of
/// the body block). This is the per-iteration kill that lets a partial store
/// later in the body be dropped without cross-iteration liveness: entering the
/// body, the binding is clobbered before anything reads it, so nothing a prior
/// iteration left in it survives. A first touch that is a read, a partial
/// write, or nested under control flow does not qualify.
static bool firstBodyTouchIsWholeWrite(Block &body, Value binding) {
  Operation *first = nullptr;
  for (Operation *user : binding.getUsers()) {
    // Lift the user to its top-level ancestor inside `body`; skip users that
    // live outside this loop body entirely.
    Operation *top = user;
    while (top && top->getBlock() != &body)
      top = top->getParentOp();
    if (!top)
      continue;
    if (!first || top->isBeforeInBlock(first))
      first = top;
  }
  if (!first)
    return false;
  auto assign = dyn_cast<emitrust::AssignOp>(first);
  return assign && assign.getVar() == binding;
}

void RustEmitter::computeDeadStores(Block &entryBlock) {
  llvm::SmallPtrSet<Operation *, 16> dead;
  // Only stores in the function's entry block are analyzed: `analyzeSeq` from
  // just after such a store covers every forward path (nested regions of later
  // ops are visited), so a store proven dead here is dead on every path. A
  // store inside a loop needs cross-iteration liveness (a value can survive a
  // `break` to a read after the loop), which is deliberately not attempted --
  // a mistaken drop there is a miscompile.
  for (Operation &op : entryBlock) {
    auto assign = dyn_cast<emitrust::AssignOp>(&op);
    if (!assign || unreachableOps.count(&op))
      continue;
    // The store targets either a whole binding (`v = ..`, target defined by a
    // let/variable) or a component of one (`v.x = ..`, target a projection
    // whose base is a variable). In both cases the store is dead when the
    // binding's value is not read before the whole binding is overwritten or
    // the function ends -- a whole overwrite kills every earlier component
    // store too.
    Value binding = projectionBase(assign.getVar());
    Operation *def = binding.getDefiningOp();
    if (!def || !isa<emitrust::LetOp, emitrust::VariableOp>(def))
      continue;
    // W2.17: `drop()` reads every field of a destructor-carrying object on
    // every path, so no store to one is ever dead. Dropping such a store
    // would leave the destructor printing a different value (or, when the
    // initializer went with it, not running at all).
    if (bindingHasDrop(binding))
      continue;
    Liveness after = analyzeSeq(std::next(op.getIterator()), entryBlock.end(),
                                binding, /*enteringWritten=*/false,
                                /*enteringAnyWrite=*/false,
                                /*brk=*/nullptr, /*partialWriteBlocks=*/false);
    if (!after.readFirst)
      dead.insert(&op);
  }
  deadStores = std::move(dead);
  computeLoopBodyDeadStores(entryBlock);
}

void RustEmitter::computeLoopBodyDeadStores(Block &entryBlock) {
  // The one loop-body dead store that FR-61f's range-for lift did not already
  // remove: a partial store (`v[i] = ..`, `v.f = ..`) inside a loop body whose
  // binding is unconditionally overwritten AS A WHOLE at the top of every
  // iteration and is never read after the loop. That top-of-body overwrite is a
  // local, per-iteration kill -- the value the partial store leaves behind
  // cannot reach the next iteration's reads (they follow the overwrite) nor
  // escape the loop (the binding is not live-out), so the property is decided
  // from the body block plus the post-loop tail ALONE. No cross-iteration
  // liveness is involved: that path miscompiled three times (CLAUDE.md) and is
  // still off-limits. This exception is kept deliberately narrow -- loops that
  // are direct children of the entry block, single-block bodies, a binding
  // hoisted above the loop with no escaping borrow.
  for (Operation &loopOp : entryBlock) {
    if (!isa<emitrust::ForOp, emitrust::WhileOp, emitrust::LoopOp>(&loopOp))
      continue;
    // `emitrust.while` keeps its body in region 1 (region 0 is the condition);
    // `for`/`loop` bodies are region 0.
    Region &bodyRegion = isa<emitrust::WhileOp>(&loopOp) ? loopOp.getRegion(1)
                                                         : loopOp.getRegion(0);
    if (bodyRegion.empty() || !bodyRegion.hasOneBlock())
      continue;
    Block &body = bodyRegion.front();
    for (Operation &op : body) {
      auto assign = dyn_cast<emitrust::AssignOp>(&op);
      if (!assign || unreachableOps.count(&op) || deadStores.count(&op))
        continue;
      Value target = assign.getVar();
      Value binding = projectionBase(target);
      // Only PARTIAL stores are candidates: a whole write is either the very
      // overwrite that provides the per-iteration kill or has its own analysis.
      if (target == binding)
        continue;
      Operation *def = binding.getDefiningOp();
      if (!def || !isa<emitrust::LetOp, emitrust::VariableOp>(def))
        continue;
      // W2.17: see `computeDeadStores` -- `drop()` reads the whole object.
      if (bindingHasDrop(binding))
        continue;
      // The binding must be hoisted ABOVE the loop; a loop-local `let` is a
      // different, unhandled shape.
      if (def->getBlock() == &body)
        continue;
      if (bindingIsBorrowed(binding))
        continue;
      // Per-iteration kill: the binding's first touch in the body is a whole
      // overwrite (so the back-edge cannot carry this store's value forward).
      if (!firstBodyTouchIsWholeWrite(body, binding))
        continue;
      // Not read in the remainder of this iteration, up to the back-edge.
      if (analyzeSeq(std::next(op.getIterator()), body.end(), binding,
                     /*enteringWritten=*/false, /*enteringAnyWrite=*/false,
                     /*brk=*/nullptr, /*partialWriteBlocks=*/false)
              .readFirst)
        continue;
      // Not live-out: not read after the loop before the function ends.
      if (analyzeSeq(std::next(loopOp.getIterator()), entryBlock.end(), binding,
                     /*enteringWritten=*/false, /*enteringAnyWrite=*/false,
                     /*brk=*/nullptr, /*partialWriteBlocks=*/false)
              .readFirst)
        continue;
      deadStores.insert(&op);
    }
  }
}

//===----------------------------------------------------------------------===//
// FR-106: the `unused_assignments` risk detector.
//
// The same all-path liveness question `computeDeadStores` asks, but over the
// FULL continuation of a store -- outward through the enclosing `if`/`switch`
// arm, and for a store inside a loop through the BACK EDGE and the post-loop
// tail. That is cross-iteration reasoning, and it is admissible here for
// exactly one reason: NOTHING IS DELETED. The answer gates an
// `#[allow(unused_assignments)]` on the enclosing fn, so an error in the
// "dead" direction costs a redundant allow on one function and an error in the
// "live" direction costs the loud build failure that exists without it. If
// this analysis is ever wired to an elision it becomes the fenced
// cross-iteration dead-store deletion; it must not be.
//===----------------------------------------------------------------------===//

/// Whether `op` renders a store of the WHOLE `binding` at its own program
/// point: an `emitrust.assign` to it, or the binding's own defining
/// `let`/`variable` carrying an initializer (both render as `let x = ..`).
///
/// A DEFERRED initializer is NOT a store: `deferredInits` means the emitter
/// renders `let x: T;` and drops the initializer, so counting it fires on
/// every FR-61b if-expression binding there is -- measured on heatshrink's
/// encoder, that marked 9 of 14 functions for stores that never reach the
/// output.
bool RustEmitter::uaStoresBinding(Operation *op, Value binding) {
  if (auto assign = dyn_cast<emitrust::AssignOp>(op))
    return assign.getVar() == binding;
  if (auto letOp = dyn_cast<emitrust::LetOp>(op))
    return letOp.getResult() == binding && !deferredInits.count(op);
  if (auto var = dyn_cast<emitrust::VariableOp>(op))
    return var.getResult() == binding && var.getInitAttr() &&
           !deferredInits.count(op);
  return false;
}

namespace {
/// The ways a remaining statement sequence can leave its enclosing loop, so
/// the read search can follow each one. `analyzeSeq` folds `break`, `continue`
/// and `return` into ONE `diverges` bit, which is enough to decide a
/// fall-through but not enough to decide where a path GOES -- the
/// store-then-`break` shape (heatshrink's `heatshrink_encoder_poll` has
/// `emitrust.assign %22 = %50` immediately before an `emitrust.break`, and the
/// value IS read after the loop) is a false positive without this.
struct UAExits {
  bool brk = false;
  bool cont = false;
  /// A shape the scan cannot classify (a multi-block nested region). Forces
  /// the caller to answer "assume a read".
  bool unknown = false;
};
} // namespace

/// Records which loop exits `[begin, end)` can take. A `break`/`continue`
/// inside a NESTED loop binds to that loop and is not reported here.
static void uaExitKinds(Block::iterator begin, Block::iterator end,
                        UAExits &k) {
  for (auto it = begin; it != end; ++it) {
    Operation *op = &*it;
    if (isa<emitrust::BreakOp>(op)) {
      k.brk = true;
      continue;
    }
    if (isa<emitrust::ContinueOp>(op)) {
      k.cont = true;
      continue;
    }
    if (isa<emitrust::ForOp, emitrust::WhileOp, emitrust::LoopOp>(op))
      continue; // an inner loop swallows its own break/continue
    for (Region &r : op->getRegions()) {
      if (r.empty())
        continue;
      if (!r.hasOneBlock()) {
        k.unknown = true;
        continue;
      }
      uaExitKinds(r.front().begin(), r.front().end(), k);
    }
  }
}

/// The nearest enclosing loop op of `blk`, or null.
static Operation *uaEnclosingLoop(Block *blk) {
  for (Operation *p = blk->getParentOp(); p; p = p->getParentOp())
    if (isa<emitrust::ForOp, emitrust::WhileOp, emitrust::LoopOp>(p))
      return p;
  return nullptr;
}

bool RustEmitter::uaReadFromLoopTop(Operation *loopOp, Value binding) {
  bool isWhile = isa<emitrust::WhileOp>(loopOp);
  if (isWhile) {
    // `emitrust.while` keeps its condition in region 0 and its body in
    // region 1; the condition runs first on every iteration.
    Region &cond = loopOp->getRegion(0);
    if (cond.empty() || !cond.hasOneBlock())
      return true; // unknown shape: assume a read
    Liveness c = analyzeSeq(cond.front().begin(), cond.front().end(), binding,
                            /*enteringWritten=*/false,
                            /*enteringAnyWrite=*/false, /*brk=*/nullptr,
                            /*partialWriteBlocks=*/false);
    if (c.readFirst)
      return true;
    // The condition already overwrote it (or never falls through into the
    // body): the body cannot see the incoming value.
    if (c.writtenAtExit || c.diverges)
      return false;
  }
  Region &body = isWhile ? loopOp->getRegion(1) : loopOp->getRegion(0);
  if (body.empty() || !body.hasOneBlock())
    return true; // unknown shape: assume a read
  return analyzeSeq(body.front().begin(), body.front().end(), binding,
                    /*enteringWritten=*/false, /*enteringAnyWrite=*/false,
                    /*brk=*/nullptr, /*partialWriteBlocks=*/false)
      .readFirst;
}

bool RustEmitter::uaReadAfter(Operation *from, Value binding) {
  // A loop head is asked about at most once: a `false` answer is a proof that
  // no path from that head reads the binding first, and a `true` answer has
  // already returned.
  llvm::SmallPtrSet<Operation *, 16> loopTopsSeen;
  llvm::SmallPtrSet<Operation *, 16> visited;
  llvm::SmallVector<Operation *, 8> work{from};
  auto loopTopReads = [&](Operation *loop) {
    return loopTopsSeen.insert(loop).second && uaReadFromLoopTop(loop, binding);
  };
  while (!work.empty()) {
    Operation *cur = work.pop_back_val();
    if (!visited.insert(cur).second)
      continue;
    Block *blk = cur->getBlock();
    // Unknown shape (no block, or a multi-block region whose successors this
    // scan does not model): assume a read.
    if (!blk || !blk->getParent() || !blk->getParent()->hasOneBlock())
      return true;
    Liveness l = analyzeSeq(std::next(cur->getIterator()), blk->end(), binding,
                            /*enteringWritten=*/false,
                            /*enteringAnyWrite=*/false, /*brk=*/nullptr,
                            /*partialWriteBlocks=*/false);
    if (l.readFirst)
      return true; // SOME path reads it: the store is live, do not mark

    // The loop exits are followed FIRST and independently of `writtenAtExit`:
    // that flag summarizes the FALL-THROUGH paths only, and a `break` path
    // that leaves before the overwrite still carries this store's value out
    // of the loop.
    UAExits k;
    uaExitKinds(std::next(cur->getIterator()), blk->end(), k);
    if (k.unknown)
      return true;
    if (k.brk || k.cont) {
      Operation *loop = uaEnclosingLoop(blk);
      if (!loop)
        return true; // a break with no enclosing loop: assume a read
      if (k.cont && loopTopReads(loop))
        return true;
      if (k.brk)
        work.push_back(loop); // a `break` continues after the loop
    }

    if (l.writtenAtExit)
      continue; // every fall-through path overwrote it first
    Operation *parent = blk->getParentOp();
    if (l.diverges || !parent || isa<emitrust::FuncOp>(parent))
      continue; // no fall-through, or the function simply ends
    // Falling out of a LOOP BODY is the back edge: the next iteration starts
    // at the head, and its reads count. Falling past the loop op itself (the
    // head's test failing) is covered by pushing the loop op.
    if (isa<emitrust::ForOp, emitrust::WhileOp, emitrust::LoopOp>(parent) &&
        loopTopReads(parent))
      return true;
    work.push_back(parent);
  }
  return false;
}

bool RustEmitter::uaFunctionAtRisk(Operation *funcOp) {
  bool risk = false;
  funcOp->walk([&](Operation *op) {
    if (risk)
      return;
    Value binding;
    if (auto assign = dyn_cast<emitrust::AssignOp>(op)) {
      binding = assign.getVar();
      Operation *def = binding.getDefiningOp();
      // Only a local `let`/`variable` binding is an `unused_assignments`
      // site the emitter owns. A store through a projection, a global cell
      // or a parameter is left to answer "assume a read" -- the safe
      // direction, since a missed mark is the loud build error.
      if (!def || !isa<emitrust::LetOp, emitrust::VariableOp>(def))
        return;
    } else if (isa<emitrust::LetOp, emitrust::VariableOp>(op)) {
      binding = op->getResult(0);
      if (!uaStoresBinding(op, binding))
        return;
    } else {
      return;
    }
    // Stores that render nothing cannot be flagged by rustc.
    if (unreachableOps.count(op) || deadStores.count(op) || droppedOps.count(op))
      return;
    // A borrowed binding can be read through the reference, which this scan
    // does not track; rustc's own liveness gives up on borrowed locals too.
    if (bindingIsBorrowed(binding))
      return;
    // A never-read binding is emitted `_`-prefixed (`claimName`), and rustc
    // EXEMPTS `_`-prefixed locals from `unused_assignments` outright --
    // measured with a rustc probe, not assumed.
    if (!valueIsRead(binding))
      return;
    if (!uaReadAfter(op, binding))
      risk = true;
  });
  return risk;
}

/// FR-61b: returns the `emitrust.assign` to `binding` that is `region`'s
/// final emitted statement, or null when the arm is empty, multi-block, or
/// ends differently. The walk mirrors emission: it stops after the first
/// diverging op and ignores the implicit `emitrust.yield` terminator (which
/// emits nothing).
static Operation *armTailAssign(Region &region, Value binding) {
  if (region.empty() || !region.hasOneBlock())
    return nullptr;
  Operation *last = nullptr;
  for (Operation &op : region.front()) {
    if (isa<emitrust::YieldOp>(op))
      continue;
    last = &op;
    if (opDiverges(&op))
      break;
  }
  return last && isBindingWrite(last, binding) ? last : nullptr;
}

void RustEmitter::computeIfExprBindings() {
  for (auto [op, needsMut] : deferredInits) {
    // A deferred binding that still needs `mut` is assigned more than once
    // per path or mutated after init -- not the both-arms-assign-once shape.
    if (needsMut)
      continue;
    auto ifOp = dyn_cast_or_null<emitrust::IfOp>(op->getNextNode());
    if (!ifOp || ifOp.getElseRegion().empty())
      continue;
    Value binding = op->getResult(0);
    Operation *thenAssign = armTailAssign(ifOp.getThenRegion(), binding);
    Operation *elseAssign = armTailAssign(ifOp.getElseRegion(), binding);
    if (!thenAssign || !elseAssign)
      continue;
    // The two arm-final assignments must be the binding's only assignments
    // in the whole function; any other write keeps the statement form.
    bool onlyAssignments = true;
    for (Operation *user : binding.getUsers())
      if (isBindingWrite(user, binding) && user != thenAssign &&
          user != elseAssign) {
        onlyAssignments = false;
        break;
      }
    if (!onlyAssignments)
      continue;
    ifExprBindings[op] = ifOp;
    consumedIfs.insert(ifOp.getOperation());
  }
}

// --- FR-61d helpers ---

/// The Rust literal suffix an inlined constant of `type` carries so the
/// let-binding's type annotation can be dropped without orphaning an
/// inference anchor, or "" when the type is not suffixable (which
/// disqualifies the constant from inlining; any residue would be a loud
/// E0308, never a silent retype).
static std::string inlineSuffixFor(Type type) {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    unsigned width = intType.getWidth();
    if (width == 1)
      return "";
    if (width == 8 || width == 16 || width == 32 || width == 64)
      return ((intType.isUnsigned() ? "u" : "i") + Twine(width)).str();
    return "";
  }
  if (isa<IndexType>(type))
    return "usize";
  if (type.isF32())
    return "f32";
  if (type.isF64())
    return "f64";
  return "";
}

/// Whether `op` is a scalar comparison (the fn-ptr comparison renders
/// through `emitFnPtrCmp` -- Option null-tests and a `match` -- and is
/// excluded from both inlining and dropping).
static bool isScalarCmp(Operation *op) {
  auto cmp = dyn_cast<emitrust::CmpOp>(op);
  return cmp && !isa<emitrust::FnPtrType>(op->getOperand(0).getType()) &&
         !isa<emitrust::FnPtrType>(op->getOperand(1).getType());
}

/// FR-61d: whether `op` belongs to the PURE producer set -- it renders one
/// side-effect-free `let <name>: <type> = <rhs>;` statement. Membership
/// gates both mechanisms: an unused pure op is dropped, a single-use pure
/// op is inlined. Calls, literals, borrows, selects, and the global/cell
/// accessors (FR-61d-2) stay out.
static bool isPureProducer(Operation *op) {
  if (isa<emitrust::CmpOp>(op))
    return isScalarCmp(op);
  // A cast to bool is an unsupported construct `emitCast` REJECTS with a
  // located diagnostic; dropping an unused one would silently accept it,
  // and rejection is a feature -- keep it out of the pure set so it always
  // reaches the emitter's error path.
  if (isa<emitrust::CastOp>(op))
    return !op->getResult(0).getType().isInteger(1);
  return isa<emitrust::ConstantOp, emitrust::AddOp, emitrust::SubOp,
             emitrust::MulOp, emitrust::DivOp, emitrust::RemOp,
             emitrust::AndOp, emitrust::OrOp, emitrust::XorOp,
             emitrust::ShlOp, emitrust::ShrOp, emitrust::BitcastOp,
             emitrust::LoadOp, emitrust::LetOp,
             emitrust::StringRepeatOp, emitrust::VecFillOp>(op);
}

/// Ops that may sit between an inlined def and its use without blocking the
/// textual move: pure let-producing ops and never-emitted place/terminator
/// markers. Complement-of-allowlist: anything unknown blocks by default
/// (calls, assigns, stores, region-carrying ops, terminators). A MUTABLE
/// borrow (`&mut` addr-of / slice-of) also blocks: moving a load's text
/// past it could turn baseline-legal code into an E0502 borrow error.
static bool isInlineNonBarrier(Operation *op) {
  if (auto addrOf = dyn_cast<emitrust::AddrOfOp>(op))
    return !addrOf.getIsMut();
  if (auto sliceOf = dyn_cast<emitrust::SliceOfOp>(op))
    return !sliceOf.getIsMut();
  return isa<emitrust::ConstantOp, emitrust::LiteralOp, emitrust::AddOp,
             emitrust::SubOp, emitrust::MulOp, emitrust::DivOp,
             emitrust::RemOp, emitrust::AndOp, emitrust::OrOp,
             emitrust::XorOp, emitrust::ShlOp, emitrust::ShrOp,
             emitrust::CmpOp, emitrust::CastOp, emitrust::BitcastOp,
             emitrust::LoadOp, emitrust::LetOp, emitrust::MemberOp,
             emitrust::SubscriptOp, emitrust::DerefOp, emitrust::EnumRawOp,
             emitrust::GlobalLoadOp, emitrust::CellGetOp,
             emitrust::VariableOp, emitrust::YieldOp>(op);
}

/// The precedence rank an infix operator `symbol` renders its statement at
/// (also the consumer rank its operand positions carry).
static Prec infixPrec(StringRef symbol) {
  return llvm::StringSwitch<Prec>(symbol)
      .Cases("*", "/", "%", Prec::MulDiv)
      .Cases("+", "-", Prec::AddSub)
      .Cases("<<", ">>", Prec::Shift)
      .Case("&", Prec::BitAnd)
      .Case("^", Prec::BitXor)
      .Case("|", Prec::BitOr)
      .Default(Prec::Compare); // == != < <= > >=
}

/// Whether the add/sub/mul `op` renders in the postfix `a.wrapping_*(b)`
/// method form -- the same predicate `emitWrappingBinary` routes on.
static bool rendersWrappingMethod(Operation *op) {
  auto intType = dyn_cast<IntegerType>(op->getResult(0).getType());
  return intType && intType.isUnsigned();
}

/// FR-61d slice 2: the longest suffixed literal a MULTI-use constant may
/// duplicate at its uses. Measured over the EndToEnd corpus (2026-08-03):
/// suffixed lengths of surviving multi-use constant bindings were p50=4,
/// p90=6, and everything beyond 12 was a pathological literal
/// (`-9223372036854775808i64`, `4000000000.75f64` at 52 uses,
/// `-2147483648i32` at 12 uses) that reads better behind a name; 12
/// collapses 98% of the 1098 measured bindings while keeping every one of
/// those.
static constexpr size_t kInlineConstantDupMaxLen = 12;

/// The length of the suffixed literal text an inlined constant renders as
/// (mirrors `emitAttribute` + the capture's suffix append), or nullopt for
/// an attribute kind that never inlines. Used only for the duplication
/// threshold, so a small drift from the real rendering is a heuristic
/// wobble, never a correctness issue.
static std::optional<size_t>
inlineConstantTextLength(emitrust::ConstantOp constant) {
  Attribute value = constant.getValue();
  if (auto intAttr = dyn_cast<IntegerAttr>(value)) {
    if (intAttr.getType().isInteger(1))
      return intAttr.getValue().getBoolValue() ? 4 : 5; // true / false
    auto intType = dyn_cast<IntegerType>(intAttr.getType());
    bool isUnsigned = intType && intType.isUnsigned();
    SmallString<32> digits;
    intAttr.getValue().toString(digits, /*Radix=*/10, /*Signed=*/!isUnsigned);
    return digits.size() +
           inlineSuffixFor(constant.getResult().getType()).size();
  }
  if (auto floatAttr = dyn_cast<FloatAttr>(value)) {
    char buffer[64];
    double v = floatAttr.getValueAsDouble();
    std::to_chars_result res{};
    if (floatAttr.getType().isF32())
      res = std::to_chars(buffer, buffer + sizeof(buffer),
                          static_cast<float>(v));
    else
      res = std::to_chars(buffer, buffer + sizeof(buffer), v);
    if (res.ec != std::errc())
      return std::nullopt;
    StringRef text(buffer, static_cast<size_t>(res.ptr - buffer));
    size_t n = text.size();
    if (!text.contains('.') && !text.contains('e') && !text.contains('E'))
      n += 2; // the appended `.0`
    return n + inlineSuffixFor(constant.getResult().getType()).size();
  }
  if (auto opaque = dyn_cast<emitrust::OpaqueAttr>(value))
    return opaque.getValue().size();
  return std::nullopt;
}

bool RustEmitter::isEffectFreeDropCandidate(Value value) const {
  Operation *def = value.getDefiningOp();
  // A block argument (parameter, loop-carried value) renders as a bare name:
  // reading it costs nothing and deleting the read deletes nothing.
  if (!def)
    return true;
  // The pure-producer set is exactly "renders one side-effect-free
  // `let <name> = <rhs>;`" -- constants, arithmetic, casts, scalar compares,
  // loads, nested lets. Calls, borrows, selects and the cell accessors are
  // deliberately outside it.
  if (isPureProducer(def))
    return true;
  // Two more effect-free renderings that predate the pure set: a global read
  // and a literal (both are FR-61d-2 inline candidates, never statements).
  return isa<emitrust::GlobalLoadOp, emitrust::LiteralOp>(def);
}

bool RustEmitter::collectDroppableBindingWrites(
    emitrust::LetOp letOp, SmallVectorImpl<Operation *> &writes) {
  Value result = letOp.getResult();
  // The initializer disappears with the `let`, so it is gated exactly like a
  // write.
  if (!isEffectFreeDropCandidate(letOp.getInit()))
    return false;
  for (OpOperand &use : result.getUses()) {
    Operation *owner = use.getOwner();
    // Uses that already emit nothing need no handling: they are not going to
    // be orphaned by dropping the binding.
    if (unreachableOps.count(owner) || deadStores.count(owner) ||
        droppedOps.count(owner) || isVacuousAssert(owner))
      continue;
    // Refuse-by-default on the USE side too: only a whole-binding write is a
    // shape we know how to delete. A place projection (`v.x = ..`, `v[i]`) is
    // NOT a read by `valueIsRead`'s reckoning, yet it renders the binding's
    // name -- dropping the `let` under one would be a hard E0425.
    if (!isBindingWrite(owner, result))
      return false;
    if (!isEffectFreeDropCandidate(cast<emitrust::AssignOp>(owner).getValue()))
      return false;
    writes.push_back(owner);
  }
  return true;
}

void RustEmitter::computeDroppedOps(emitrust::FuncOp funcOp) {
  // Reverse program order: users always render after their defs (nested
  // users belong to later parent ops), so one reverse sweep sees every
  // consumer's fate before deciding its producers -- cascades converge in a
  // single pass. The default post-order walk visits nested ops before
  // their parent; reversed, parents come first, which still keeps every
  // user ahead of its def.
  //
  // FR-130 broke the single-pass property in ONE direction: dropping a
  // never-read binding also deletes its WRITES, and a write's right-hand
  // side sits LATER in program order than the `let` (so the reverse sweep
  // already passed it). Re-sweep while anything changed. The sets only ever
  // grow -- an op never leaves `droppedOps`/`deadStores` -- so the fixpoint
  // is monotone and terminates; in practice one extra confirming sweep is
  // all it costs.
  SmallVector<Operation *> ops;
  funcOp->walk([&](Operation *op) { ops.push_back(op); });
  bool changed = true;
  while (changed) {
    changed = false;
    for (Operation *op : llvm::reverse(ops)) {
      if (op->getNumResults() != 1 || !isPureProducer(op))
        continue;
      if (droppedOps.count(op))
        continue; // decided on an earlier sweep
      if (unreachableOps.count(op))
        continue; // already never emitted; dropping would double-count
      // FR-130: the whole-binding writes that must be deleted along with a
      // never-read `let`. Empty for every other droppable op.
      SmallVector<Operation *> deadBindingWrites;
      if (auto letOp = dyn_cast<emitrust::LetOp>(op)) {
        // W2.17: a destructor-carrying binding is observable even when never
        // read -- dropping it deletes the destructor's side effects.
        if (bindingHasDrop(letOp.getResult()))
          continue;
        // A deferred `let` renders its declaration and its initializing write
        // at two different program points; joint elimination of that shape is
        // out of scope here.
        if (deferredInits.count(op))
          continue;
        if (letOp.getIsMut() || letHasEmittedAssign(letOp.getResult())) {
          // FR-130 increment 2 (dead-binding elimination): a binding with
          // assignments used to be undroppable outright, because dropping it
          // alone would orphan them (E0425). Drop them TOGETHER instead. This
          // is not the forbidden cross-iteration dead-store reasoning and
          // computes no liveness at all: `valueIsRead` classifies an assignment
          // DESTINATION as a write and never as a read, so the `!valueIsRead`
          // proof below means the binding is read on NO path in NO region --
          // every write to it is trivially dead. The only real risk is deleting
          // an EFFECT along with a write, which the refuse-by-default purity
          // gate in `collectDroppableBindingWrites` rules out; one refusing
          // write keeps the entire binding (no partial drops).
          if (!collectDroppableBindingWrites(letOp, deadBindingWrites))
            continue;
        }
      }
      // `valueIsRead` (with the droppedOps guard active) is the proof of "no
      // emitted read". The cache is cleared each round because every new drop
      // can un-read further values upstream.
      valueReadCache.clear();
      if (!valueIsRead(op->getResult(0))) {
        droppedOps.insert(op);
        ++dropCount;
        // The joint-dropped writes are recorded in `deadStores`, which means
        // exactly "this assign emits nothing" and which all nine consumption
        // sites already consult (valueIsRead, lvalueIsMutated, the path-liveness
        // scan, the FR-105 loop extension, computeInlineCandidates' real-use
        // count and its barrier scan, computeFieldInitFuses, letHasEmittedAssign
        // and emitAssign). A parallel set would have to be threaded through all
        // nine and would silently miscompile at any site that missed it.
        // Extending the set HERE is safe: every `deadStores` producer
        // (`computeDeadStores` and its loop-body extension) has already run, so
        // a late insertion cannot invalidate an earlier producer's decision.
        for (Operation *write : deadBindingWrites)
          deadStores.insert(write);
        changed = true;
      }
    }
  }
  // Later phases (naming, mut/`_` decisions) must see read-ness with the
  // final drop set applied.
  valueReadCache.clear();
}

/// FR-61d: whether `use` sits in an operand position the emitter renders
/// through a classified `emitOperand` site. Anything else (for-loop
/// bounds, which render by name lookup; place-typed operands; structural
/// yields) disqualifies the candidate feeding it. The exception among
/// yields is the value-carrying `emitrust.yield` of a result-mode
/// `emitrust.match`, whose operand renders in the never-parenthesized arm
/// tail position; `emitrust.enum_variant` field values and the
/// `emitrust.match` scrutinee are classified positions too (FR-62 slice
/// 5a).
static bool isClassifiedConsumerUse(OpOperand &use) {
  Operation *owner = use.getOwner();
  return llvm::TypeSwitch<Operation *, bool>(owner)
      .Case<emitrust::AddOp, emitrust::SubOp, emitrust::MulOp,
            emitrust::DivOp, emitrust::RemOp, emitrust::AndOp,
            emitrust::OrOp, emitrust::XorOp, emitrust::ShlOp,
            emitrust::ShrOp, emitrust::CastOp, emitrust::BitcastOp,
            emitrust::LetOp, emitrust::ReturnOp, emitrust::CallOpaqueOp,
            emitrust::CallIndirectOp, emitrust::IfOp, emitrust::SelectOp,
            emitrust::SwitchOp, emitrust::GlobalStoreOp,
            emitrust::EnumVariantOp, emitrust::MatchOp, emitrust::YieldOp>(
          [](auto) { return true; })
      .Case<emitrust::CmpOp>([&](auto) { return isScalarCmp(owner); })
      .Case<emitrust::AssignOp>([&](emitrust::AssignOp assign) {
        // Only the assigned VALUE is an expression position; the target
        // renders as a name/place.
        return use.get() == assign.getValue() && use.get() != assign.getVar();
      })
      .Case<emitrust::MethodCallOp>([&](emitrust::MethodCallOp call) {
        // The receiver is a place (rendered by emitPlaceExpr); arguments
        // are delimited expression positions.
        return use.get() != call.getReceiver();
      })
      // FR-64: the `.repeat` count is a delimited expression position, so a
      // single-use cast/load bound inlines into `"c".repeat(<count> as usize)`
      // instead of forcing a `let vN =` above the binding.
      .Case<emitrust::StringRepeatOp>([](auto) { return true; })
      // FR-65: the `vec![_; <count>]` count is a delimited expression
      // position, so a single-use cast/load bound inlines into it instead of
      // forcing a `let vN =` above the binding.
      .Case<emitrust::VecFillOp>([](auto) { return true; })
      .Case<emitrust::CellGetOp>([&](emitrust::CellGetOp get) {
        return use.get() == get.getIndex();
      })
      .Case<emitrust::CellSetOp>([&](emitrust::CellSetOp set) {
        return use.get() != set.getSlice();
      })
      .Case<emitrust::SliceOfOp>([&](emitrust::SliceOfOp sliceOf) {
        return use.get() == sliceOf.getIndex();
      })
      .Case<emitrust::ArgvArgOp>([&](emitrust::ArgvArgOp argvArg) {
        // The table renders as a parameter name; only the index is a
        // delimited expression position (mirrors the slice-of index).
        return use.get() == argvArg.getIndex();
      })
      .Case<emitrust::SubscriptOp>([&](emitrust::SubscriptOp subscript) {
        return use.get() == subscript.getIndex();
      })
      // FR-61f: all three `emitrust.for` operands (lower/upper/step) render in
      // the range head as expression positions, so a constant or single-use
      // pure bound inlines into `for i in LO..HI` instead of forcing a
      // `let vN =` above the loop.
      .Case<emitrust::ForOp>([](auto) { return true; })
      .Case<emitrust::DerefOp>([](auto) { return true; })
      // FR-61c: the while-condition terminator consumes its operand in the
      // never-parenthesized head position; classifying it is what lets the
      // condition chain's final op fold.
      .Case<emitrust::ConditionOp>([](auto) { return true; })
      .Default([](Operation *) { return false; });
}

void RustEmitter::computeInlineCandidates(emitrust::FuncOp funcOp) {
  funcOp->walk([&](Operation *op) {
    // FR-61d-2: `emitrust.global_load` joins the SINGLE-use inline set (a
    // pure module-state read behind the same barrier wall as local loads);
    // it stays out of `isPureProducer` so unused ones are not dropped.
    // `emitrust.cell_get` stays out entirely: its defs live in
    // `emitrust.global_cells` bodies, which are rendered by a loop that
    // bypasses `emitBlockBody`'s capture, so promotion would be inert.
    if (op->getNumResults() != 1 ||
        (!isPureProducer(op) && !isa<emitrust::GlobalLoadOp>(op)))
      return;
    if (unreachableOps.count(op) || droppedOps.count(op))
      return;
    // Producer-specific gates.
    bool selfReadsPlace = false;
    if (auto constant = dyn_cast<emitrust::ConstantOp>(op)) {
      Attribute value = constant.getValue();
      Type type = constant.getResult().getType();
      if (isa<emitrust::FnPtrType>(type))
        return; // `None` inlined loses its inference anchor (E0282)
      auto intAttr = dyn_cast<IntegerAttr>(value);
      auto floatAttr = dyn_cast<FloatAttr>(value);
      if (floatAttr && !floatAttr.getValue().isFinite())
        return; // renders as a path expr; a suffix would corrupt it
      bool isBool = intAttr && intAttr.getType().isInteger(1);
      bool isOpaque = isa<emitrust::OpaqueAttr>(value);
      // Numeric constants must carry a literal suffix; a type with no
      // suffix spelling cannot inline.
      if (!isBool && !isOpaque && inlineSuffixFor(type).empty())
        return;
      if (!isBool && !isOpaque && !intAttr && !floatAttr)
        return; // unknown attribute kind: refuse by default
    } else if (auto letOp = dyn_cast<emitrust::LetOp>(op)) {
      if (letOp.getIsMut() || deferredInits.count(op) ||
          letHasEmittedAssign(letOp.getResult()))
        return;
    } else if (isa<emitrust::LoadOp, emitrust::GlobalLoadOp>(op)) {
      selfReadsPlace = true;
    }
    // The tail-fold candidate keeps the FR-61a rendering (its consumer is
    // the function-final return); excluding it here keeps the two
    // mechanisms disjoint by construction.
    if (op == tailFoldCandidate)
      return;
    // Collect the REAL uses (uses inside unreachable code, dropped dead
    // stores, dropped pure ops, FR-63 vacuous asserts, or the FR-63 folded
    // zero-shift amount never render).
    SmallVector<OpOperand *, 4> realUses;
    for (OpOperand &use : op->getResult(0).getUses()) {
      Operation *owner = use.getOwner();
      if (unreachableOps.count(owner) || deadStores.count(owner) ||
          droppedOps.count(owner) || isVacuousAssert(owner) ||
          isDroppedZeroShiftAmount(use))
        continue;
      realUses.push_back(&use);
    }
    if (realUses.empty())
      return;
    if (auto constant = dyn_cast<emitrust::ConstantOp>(op)) {
      // FR-61d-2: constants are position-independent literals, so neither
      // the same-block nor the barrier requirement applies at ANY use
      // count -- but every use must be a classified consumer position. A
      // MULTI-use constant additionally duplicates its literal at every
      // site, which only reads well within the measured length threshold: a
      // long literal repeated at several sites reads worse than a name
      // (single-use constants stay unthresholded, as in slice 1).
      if (realUses.size() > 1) {
        std::optional<size_t> len = inlineConstantTextLength(constant);
        if (!len || *len > kInlineConstantDupMaxLen)
          return;
      }
      for (OpOperand *use : realUses)
        if (!isClassifiedConsumerUse(*use))
          return;
      inlinedOps.insert(op);
      inlineTreeReadsPlace[op->getResult(0)] = false;
      return;
    }
    if (realUses.size() > 1)
      return; // only constants duplicate
    OpOperand *realUse = realUses.front();
    Operation *consumer = realUse->getOwner();
    if (consumer->getBlock() != op->getBlock())
      return;
    if (!isClassifiedConsumerUse(*realUse))
      return;
    // Does the candidate's transitive inlined tree re-read a place when its
    // text renders? (Its own load, or an inlined operand's tree.)
    bool treeReadsPlace = selfReadsPlace;
    for (Value operand : op->getOperands())
      if (Operation *def = operand.getDefiningOp())
        if (inlinedOps.count(def))
          treeReadsPlace |= inlineTreeReadsPlace.lookup(operand);
    // A place-projection consumer renders on demand at every consuming
    // load/assign/borrow -- possibly past a barrier, possibly repeatedly --
    // so only position-independent (place-read-free) text may inline there.
    if (isa<emitrust::SubscriptOp, emitrust::DerefOp>(consumer) &&
        treeReadsPlace)
      return;
    // No barrier strictly between def and use.
    for (Operation *between = op->getNextNode(); between != consumer;
         between = between->getNextNode()) {
      if (unreachableOps.count(between) || deadStores.count(between) ||
          droppedOps.count(between))
        continue;
      if (!isInlineNonBarrier(between))
        return;
    }
    inlinedOps.insert(op);
    inlineTreeReadsPlace[op->getResult(0)] = treeReadsPlace;
  });
}

Prec RustEmitter::capturedPrec(Operation *op, StringRef text) {
  return llvm::TypeSwitch<Operation *, Prec>(op)
      .Case<emitrust::ConstantOp>([&](auto) {
        return text.starts_with("-") ? Prec::Unary : Prec::Postfix;
      })
      .Case<emitrust::AddOp, emitrust::SubOp, emitrust::MulOp>(
          [&](Operation *binary) {
            if (rendersWrappingMethod(binary))
              return Prec::Postfix; // `a.wrapping_add(b)`
            return isa<emitrust::MulOp>(binary) ? Prec::MulDiv : Prec::AddSub;
          })
      .Case<emitrust::DivOp, emitrust::RemOp>([](auto) { return Prec::MulDiv; })
      .Case<emitrust::AndOp>([](auto) { return Prec::BitAnd; })
      .Case<emitrust::OrOp>([](auto) { return Prec::BitOr; })
      .Case<emitrust::XorOp>([](auto) { return Prec::BitXor; })
      .Case<emitrust::ShlOp, emitrust::ShrOp>([&](Operation *shift) {
        // FR-63 (clippy::identity_op): a zero-amount shift renders its
        // lhs's text alone -- inherit its rank (a plain name is an atom),
        // or a consumer would parenthesize for the Shift rank that was
        // never emitted.
        if (Value lhs = zeroShiftLhs(shift)) {
          auto it = inlineExprs.find(lhs);
          return it != inlineExprs.end() ? it->second.prec : Prec::Postfix;
        }
        return Prec::Shift;
      })
      .Case<emitrust::CmpOp>([&](emitrust::CmpOp cmp) {
        // FR-63 (clippy::bool_comparison): the rank must match the shape
        // `emitCmp` actually rendered, or a consumer drops needed parens.
        Value operand;
        switch (boolCmpShape(cmp, operand)) {
        case BoolCmpShape::Identity: {
          // The fold renders the operand's text alone: inherit its rank
          // (a plain name is an atom).
          auto it = inlineExprs.find(operand);
          return it != inlineExprs.end() ? it->second.prec : Prec::Postfix;
        }
        case BoolCmpShape::NegateName:
          return Prec::Unary; // renders `!name`
        case BoolCmpShape::None:
        case BoolCmpShape::InvertInner:
        case BoolCmpShape::Unfolded:
          break; // all render an infix comparison
        }
        return Prec::Compare;
      })
      .Case<emitrust::CastOp>([&](emitrust::CastOp castOp) {
        // The enum-target form renders `Name(x as i32)`, a postfix call.
        if (isa<emitrust::EnumType>(op->getResult(0).getType()))
          return Prec::Postfix;
        // FR-63 (clippy::unnecessary_cast): a dropped identity tail leaves
        // the source text alone -- `<recv>.0` (a postfix chain) for an
        // enum source; otherwise inherit the operand's rank (a plain name
        // is an atom), or a consumer would parenthesize for the Cast rank
        // that was never emitted -- or worse, DROP parens a looser
        // inherited rank still needs.
        if (isIdentityCastTail(castOp)) {
          if (isa<emitrust::EnumType>(op->getOperand(0).getType()))
            return Prec::Postfix;
          auto it = inlineExprs.find(op->getOperand(0));
          return it != inlineExprs.end() ? it->second.prec : Prec::Postfix;
        }
        return Prec::Cast;
      })
      .Case<emitrust::BitcastOp>([&](auto) {
        // `fW::from_bits(..)` is postfix; `x.to_bits()` is too unless the
        // signless-result ` as iW` tail demotes the whole text to a cast.
        if (isa<FloatType>(op->getResult(0).getType()))
          return Prec::Postfix;
        auto intType = cast<IntegerType>(op->getResult(0).getType());
        return intType.isUnsigned() ? Prec::Postfix : Prec::Cast;
      })
      .Case<emitrust::LoadOp>([&](emitrust::LoadOp loadOp) {
        // A deref-rooted place renders with a leading `*` (unary); any
        // other root renders a postfix name/field/index chain.
        Operation *placeDef = loadOp.getOperand().getDefiningOp();
        return isa_and_nonnull<emitrust::DerefOp>(placeDef) ? Prec::Unary
                                                            : Prec::Postfix;
      })
      .Case<emitrust::LetOp>([&](emitrust::LetOp letOp) {
        // An alias let renders its initializer: inherit an inlined
        // initializer's rank; a plain name is an atom.
        auto it = inlineExprs.find(letOp.getInit());
        return it != inlineExprs.end() ? it->second.prec : Prec::Postfix;
      })
      .Case<emitrust::GlobalLoadOp>([](auto) {
        // `NAME` (const) or `NAME.with(|..| ..get())`: postfix either way.
        return Prec::Postfix;
      })
      .Default([&](Operation *) {
        llvm_unreachable("capturedPrec: op is not an inline producer");
        return Prec::Postfix;
      });
}

bool RustEmitter::rhsEndsInCast(Operation *op, Prec rank) {
  auto it = inlineExprs.find(op->getOperand(1));
  if (it == inlineExprs.end())
    return false;
  const InlineExpr &rhs = it->second;
  // The right operand's trailing cast leaks to the end of the infix text
  // only when it renders bare in the BinRhs position.
  return rhs.endsInCast &&
         !needsParens(rhs.prec, rhs.endsInCast, ExprPos::binRhs(rank));
}

bool RustEmitter::capturedEndsInCast(Operation *op) {
  return llvm::TypeSwitch<Operation *, bool>(op)
      .Case<emitrust::CastOp>([&](emitrust::CastOp castOp) {
        // The enum-target form `Name(x as i32)` ends in `)`.
        if (isa<emitrust::EnumType>(op->getResult(0).getType()))
          return false;
        // FR-63 (clippy::unnecessary_cast): the dropped identity tail's
        // text ends in `.0` for an enum source (never a cast); otherwise
        // it is the operand's own property -- which may itself be true
        // when the operand ends in a surviving REAL cast
        // (`x as u32 as u32` drops only the outer identity).
        if (isIdentityCastTail(castOp)) {
          if (isa<emitrust::EnumType>(op->getOperand(0).getType()))
            return false;
          auto it = inlineExprs.find(op->getOperand(0));
          return it != inlineExprs.end() && it->second.endsInCast;
        }
        return true;
      })
      .Case<emitrust::BitcastOp>([&](auto) {
        // Only the signless-int result appends the ` as iW` tail.
        auto intType = dyn_cast<IntegerType>(op->getResult(0).getType());
        return intType && !intType.isUnsigned();
      })
      .Case<emitrust::AddOp, emitrust::SubOp, emitrust::MulOp>(
          [&](Operation *binary) {
            if (rendersWrappingMethod(binary))
              return false; // `a.wrapping_add(b)` ends in `)`
            return rhsEndsInCast(binary, isa<emitrust::MulOp>(binary)
                                             ? Prec::MulDiv
                                             : Prec::AddSub);
          })
      .Case<emitrust::DivOp, emitrust::RemOp>(
          [&](Operation *b) { return rhsEndsInCast(b, Prec::MulDiv); })
      .Case<emitrust::AndOp>(
          [&](Operation *b) { return rhsEndsInCast(b, Prec::BitAnd); })
      .Case<emitrust::OrOp>(
          [&](Operation *b) { return rhsEndsInCast(b, Prec::BitOr); })
      .Case<emitrust::XorOp>(
          [&](Operation *b) { return rhsEndsInCast(b, Prec::BitXor); })
      .Case<emitrust::ShlOp, emitrust::ShrOp>([&](Operation *b) {
        // FR-63 (clippy::identity_op): the folded zero-shift's text is the
        // lhs's alone -- inherit its trailing-cast property (a bare cast
        // left of a shift/comparison must still wrap).
        if (Value lhs = zeroShiftLhs(b)) {
          auto it = inlineExprs.find(lhs);
          return it != inlineExprs.end() && it->second.endsInCast;
        }
        return rhsEndsInCast(b, Prec::Shift);
      })
      .Case<emitrust::CmpOp>([&](emitrust::CmpOp cmp) {
        // FR-63 (clippy::bool_comparison): the trailing-cast property must
        // match the shape `emitCmp` actually rendered.
        Value operand;
        switch (boolCmpShape(cmp, operand)) {
        case BoolCmpShape::Identity: {
          // The operand's text alone: inherit its trailing-cast property.
          auto it = inlineExprs.find(operand);
          return it != inlineExprs.end() && it->second.endsInCast;
        }
        case BoolCmpShape::NegateName:
          return false; // `!name` never ends in a cast
        case BoolCmpShape::InvertInner:
          // The rendered right operand is the INNER comparison's.
          return rhsEndsInCast(operand.getDefiningOp(), Prec::Compare);
        case BoolCmpShape::None:
        case BoolCmpShape::Unfolded:
          break;
        }
        return rhsEndsInCast(cmp.getOperation(), Prec::Compare);
      })
      .Case<emitrust::LetOp>([&](emitrust::LetOp letOp) {
        auto it = inlineExprs.find(letOp.getInit());
        return it != inlineExprs.end() && it->second.endsInCast;
      })
      .Default([](Operation *) { return false; });
}

FailureOr<std::string> RustEmitter::lookupName(Location loc, Value value) {
  auto it = valueNames.find(value);
  if (it == valueNames.end()) {
    emitError(loc) << "operand value is used before it is defined";
    return failure();
  }
  return it->second;
}

/// FR-63 (field_reassign_with_default): whether `value`'s transitive operand
/// tree reaches a place rooted at `root`. Any such reach (a load's place, a
/// borrow, a method receiver, the variable itself) would observe the struct
/// mid-construction, so it disqualifies the fuse from that assign onward.
/// Conservative toward `true`: a false positive only keeps the plain
/// statement rendering.
static bool valueTreeReachesPlace(Value value, Value root,
                                  llvm::SmallPtrSetImpl<Operation *> &visited) {
  if (value == root || projectionBase(value) == root)
    return true;
  Operation *def = value.getDefiningOp();
  if (!def)
    return false; // a block argument never aliases a local variable place
  if (!visited.insert(def).second)
    return false;
  bool reaches = false;
  def->walk([&](Operation *nested) {
    for (Value operand : nested->getOperands())
      if (valueTreeReachesPlace(operand, root, visited))
        reaches = true;
  });
  return reaches;
}

void RustEmitter::computeFieldInitFuses(emitrust::FuncOp funcOp) {
  funcOp->walk([&](emitrust::VariableOp variableOp) {
    // Gate 1: exactly the `let [mut] s: S = S::default();` rendering — a
    // struct-typed variable with no initializer whose dead default was NOT
    // dropped in favor of deferred initialization. (Enums are skipped: an
    // `!emitrust.enum` place has no fields, only the `.0` raw slot.)
    Operation *varOp = variableOp.getOperation();
    if (variableOp.getInitAttr() || variableOp.getIsConst() ||
        deferredInits.count(varOp) || unreachableOps.count(varOp))
      return;
    Value result = variableOp.getResult();
    if (!isa<emitrust::StructType>(
            cast<emitrust::LValueType>(result.getType()).getValueType()))
      return;
    // Gate 2: the maximal prefix of consecutive statement-position assigns
    // to DISTINCT single-level fields of this variable. Ops that render
    // nothing at their own program point — place projections, inlined
    // single-use producers, dropped pure ops — are skipped: their renders
    // happen at consumers at-or-after the fuse point, where every field
    // holds the same value as after the sequential stores. ANY other
    // statement (a `let`, a call, a nested/indexed or whole-binding store,
    // a dead store, a repeated field, a value that reads this variable)
    // ends the prefix; everything from there on renders unchanged.
    SmallVector<emitrust::AssignOp, 4> fused;
    llvm::StringSet<> seenFields;
    for (Operation *op = varOp->getNextNode(); op; op = op->getNextNode()) {
      if (isPlaceProjection(op) || droppedOps.count(op) ||
          inlinedOps.count(op))
        continue;
      auto assign = dyn_cast<emitrust::AssignOp>(op);
      if (!assign || deadStores.count(op))
        break;
      auto member = assign.getVar().getDefiningOp<emitrust::MemberOp>();
      if (!member || member.getOperand() != result)
        break; // whole-binding store, or a nested/indexed place
      if (!seenFields.insert(member.getMember()).second)
        break; // a literal cannot repeat a field
      llvm::SmallPtrSet<Operation *, 16> visited;
      if (valueTreeReachesPlace(assign.getValue(), result, visited))
        break; // the value reads the variable being built
      fused.push_back(assign);
    }
    if (fused.empty())
      return;
    // W2.17 x FR-111: a destructor-carrying struct fuses ONLY when the
    // prefix covers EVERY field, because that is exactly the case whose
    // rendering drops the `..S::default()` functional-update base
    // (emitFusedFieldInit). A kept base is a whole extra `S` -- constructed,
    // moved out of field-wise, and then DROPPED; measured: one spurious
    // `dtor 0 0` line ahead of the real one. `seenFields` keeps the prefix
    // distinct, so the size compare is exact. The struct_def is a
    // module-level symbol and `emitrust.impl` is itself a SymbolTable (a
    // `lookupNearestSymbolFrom` from a method body would miss it): resolve
    // in the enclosing module directly, like the emission side does. An
    // UNRESOLVED def must NOT fuse here -- emission's fallback keeps the
    // base, which has the wrong polarity for Drop types.
    if (bindingHasDrop(result)) {
      auto structType = cast<emitrust::StructType>(
          cast<emitrust::LValueType>(result.getType()).getValueType());
      emitrust::StructDefOp structDef;
      if (auto module = varOp->getParentOfType<ModuleOp>())
        structDef = dyn_cast_or_null<emitrust::StructDefOp>(
            SymbolTable::lookupSymbolIn(
                module, StringAttr::get(varOp->getContext(),
                                        structType.getName())));
      if (!structDef || fused.size() != structDef.getFieldNames().size())
        return;
    }
    for (emitrust::AssignOp assign : fused)
      fusedAssignOwner[assign.getOperation()] = varOp;
    fieldInitFuses[varOp] = std::move(fused);
  });
}

/// Whether `op` or anything nested in its regions uses `v` as an operand.
static bool opTouchesValue(Operation *op, Value v) {
  bool found = false;
  op->walk([&](Operation *nested) {
    for (Value operand : nested->getOperands())
      if (operand == v)
        found = true;
  });
  return found;
}

bool RustEmitter::typeMayDrop(Type type) const {
  if (auto lvalueType = dyn_cast<emitrust::LValueType>(type))
    type = lvalueType.getValueType();
  while (auto arrayType = dyn_cast<emitrust::ArrayType>(type))
    type = arrayType.getElementType();
  // An opaque type is a hand-written Rust spelling (`Box<T>`, `String`,
  // `Vec<T>`, an STL owner, ..): assume it owns. A data enum carries payloads.
  if (isa<emitrust::OpaqueType, emitrust::DataEnumType>(type))
    return true;
  if (auto structType = dyn_cast<emitrust::StructType>(type))
    return nonCopyStructNames.contains(structType.getName()) ||
           dropStructNames.contains(structType.getName());
  return false;
}

/// FR-132: find, for each deferred binding, the assignment that may render its
/// whole `let`. The gate is SCOPE, not adjacency -- intervening statements that
/// cannot observe the binding are the common case.
void RustEmitter::computeLateInitMerges(emitrust::FuncOp funcOp) {
  funcOp->walk([&](Operation *op) {
    if (!deferredInits.count(op))
      return;
    // FR-61b keeps priority: an if-expression binding already folds both
    // program points, and into a strictly better rendering.
    if (ifExprBindings.count(op))
      return;
    // A declaration that renders nothing (or renders as the function's tail)
    // has no two points to merge.
    if (unreachableOps.count(op) || droppedOps.count(op) ||
        inlinedOps.count(op) || op == tailFoldCandidate)
      return;
    Value binding = op->getResult(0);
    // Walk forward IN THE SAME BLOCK. Leaving the block is not an option: the
    // initializing write inside an `if`/`match`/`loop` region would sink the
    // declaration into that region and put the binding out of scope for every
    // read after it.
    for (Operation *cur = op->getNextNode(); cur; cur = cur->getNextNode()) {
      if (isBindingWrite(cur, binding)) {
        // A dead store renders nothing, so it is not the program point the
        // declaration would move to; keep scanning for the surviving write.
        if (deadStores.count(cur))
          continue;
        if (unreachableOps.count(cur) || droppedOps.count(cur) ||
            fusedAssignOwner.count(cur) || lateInitOwner.count(cur))
          return;
        // DROP ORDER. Rust drops in reverse DECLARATION order, so sinking a
        // may-drop declaration past ANOTHER may-drop declaration flips two
        // destructors. Measured on the two hand-written orderings of one
        // program under rustc -O: `dtor 1; dtor 2` became `dtor 2; dtor 1`,
        // and BOTH crates compiled clean -- a miscompile no build can see.
        if (typeMayDrop(binding.getType())) {
          for (Operation *gap = op->getNextNode(); gap != cur;
               gap = gap->getNextNode()) {
            // Skip what renders no binding of its own at this point.
            if (isPlaceProjection(gap) || inlinedOps.count(gap) ||
                droppedOps.count(gap))
              continue;
            for (Value res : gap->getResults())
              if (typeMayDrop(res.getType()))
                return;
          }
        }
        lateInitMerges[op] = cur;
        lateInitOwner[cur] = op;
        return;
      }
      // ANY other mention of the binding before its initializing write -- a
      // read, a projection, or a write nested in a region op -- refuses.
      // Refusing on the mention rather than reasoning about which mentions
      // render is what keeps the scope rule a rule.
      if (opTouchesValue(cur, binding))
        return;
      // Emission stops at a diverging op; nothing after it renders.
      if (opDiverges(cur))
        return;
    }
  });
}

/// FR-77's `Some(<identifier>)` reader, defined below alongside the dangling-
/// target backstop; FR-133's admission test asks it the same question.
static StringRef fnPtrConstantTargetIdent(Attribute attr);

/// FR-133 (clippy::unnecessary_literal_unwrap): find the fn-ptr LOCALS that
/// need no `Option` at all -- initialized from a LITERAL `Some(f)`, never
/// reassigned, never compared against null.
///
/// The three conditions ARE the safety fence, and each is load-bearing: a
/// reassignment could introduce `None`, and a null comparison needs the
/// `Option` to have something to compare. They are checked here as one rule
/// over the binding's USE SET, which is what makes them decidable rather than
/// assumed -- every use must be either
///   * the ONE whole-binding `emitrust.assign` whose value is an
///     `emitrust.constant` carrying identifier-shaped `Some(<fn>)` opaque
///     text and feeding nothing else, or
///   * an `emitrust.load` whose every use is the CALLEE (operand 0) of an
///     `emitrust.call_indirect`.
/// A second assign refuses (even an all-literal one on the other arm of an
/// `if`: that is a reassignment by this rule, and admitting it would need
/// definite-assignment reasoning a rendering fold does not have). A load
/// reaching an `emitrust.cmp` -- how `fp == 0` and `if (!fp)` arrive -- is not
/// a callee, so it refuses. A load that ESCAPES into a call argument, a
/// `return`, or a store refuses too: those positions are typed
/// `Option<fn(..)>` and an unwrapped value would not fit them.
///
/// The declaration must also be a DEFERRED binding: every other rendering of
/// a fn-ptr variable emits the synthesized `None` default
/// (`emitDefaultValue`), which has no spelling once the `Option` is gone.
///
/// If any leg cannot be proved the binding KEEPS its `Option`. The safe
/// direction is a redundant unwrap, never a dropped wrapper a later `None`
/// needs.
void RustEmitter::computeUnwrappedFnPtrs(emitrust::FuncOp funcOp) {
  funcOp->walk([&](emitrust::VariableOp variableOp) {
    Value binding = variableOp.getResult();
    auto lvalueType = dyn_cast<emitrust::LValueType>(binding.getType());
    if (!lvalueType || !isa<emitrust::FnPtrType>(lvalueType.getValueType()))
      return;
    Operation *declOp = variableOp.getOperation();
    if (!deferredInits.count(declOp) || unreachableOps.count(declOp) ||
        droppedOps.count(declOp) || ifExprBindings.count(declOp))
      return;
    emitrust::ConstantOp init;
    SmallVector<Value> loadResults;
    for (OpOperand &use : binding.getUses()) {
      Operation *user = use.getOwner();
      if (unreachableOps.count(user) || droppedOps.count(user))
        return; // a use this emitter silences is still a use: refuse
      if (auto assign = dyn_cast<emitrust::AssignOp>(user)) {
        // A partial write reaches the binding through a projection, never
        // through the assign itself, so `getVar() == binding` is the
        // whole-binding test; `init` being set already is the SECOND write.
        if (assign.getVar() != binding || init || deadStores.count(user))
          return;
        auto constant =
            assign.getValue().getDefiningOp<emitrust::ConstantOp>();
        if (!constant || !constant.getResult().hasOneUse() ||
            fnPtrConstantTargetIdent(constant.getValue()).empty())
          return;
        init = constant;
        continue;
      }
      auto load = dyn_cast<emitrust::LoadOp>(user);
      if (!load || load.getOperand() != binding)
        return;
      for (OpOperand &loadUse : load.getResult().getUses()) {
        auto call = dyn_cast<emitrust::CallIndirectOp>(loadUse.getOwner());
        if (!call || loadUse.getOperandNumber() != 0)
          return; // not a callee: the consuming position keeps the Option
      }
      loadResults.push_back(load.getResult());
    }
    if (!init)
      return; // no literal initializer proved
    unwrappedFnPtrs.insert(binding);
    unwrappedFnPtrs.insert(init.getResult());
    unwrappedFnPtrs.insert_range(loadResults);
  });
}

LogicalResult RustEmitter::emitMergedLateInit(Operation *declOp,
                                              emitrust::AssignOp assignOp) {
  Value binding = assignOp.getVar();
  Type valueType = binding.getType();
  if (auto lvalueType = dyn_cast<emitrust::LValueType>(valueType))
    valueType = lvalueType.getValueType();
  os << "let ";
  // `deferredInits`'s mapped bool stays the single source of truth for `mut`:
  // the merging assign is still the initializing write, so it must not be
  // subtracted from the mutation count the way a fused field store is.
  if (deferredInits.lookup(declOp))
    os << "mut ";
  os << assignName(binding) << ": ";
  if (failed(emitBindingType(assignOp.getLoc(), binding, valueType)))
    return failure();
  os << " = ";
  if (failed(
          emitOperand(assignOp.getLoc(), assignOp.getValue(), ExprPos::stmt())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitFusedFieldInit(emitrust::VariableOp variableOp) {
  Operation *op = variableOp.getOperation();
  Value result = variableOp.getResult();
  Location loc = variableOp.getLoc();
  Type valueType = cast<emitrust::LValueType>(result.getType()).getValueType();
  auto structType = cast<emitrust::StructType>(valueType);
  const SmallVector<emitrust::AssignOp, 4> &assigns = fieldInitFuses[op];
  // Mut-ness recomputed over the SURVIVING mutations only: `lvalueIsMutated`
  // skips the fused assigns, so a binding whose only stores fused loses its
  // `mut` while any later store/borrow/mutating call keeps it. Both failure
  // directions are loud rustc errors (unused_mut is denied; a missing `mut`
  // is E0596/E0384), never silent misbehavior.
  bool isMut = lvalueIsMutated(result);
  os << (isMut ? "let mut " : "let ") << assignName(result) << ": ";
  if (failed(emitBindingType(loc, result, valueType)))
    return failure();
  // When the fused fields cover the whole struct the functional-update base
  // would be dead (clippy::needless_update), so it is dropped and the
  // literal matches the aggregate-initializer style exactly. The struct_def
  // is a module-level symbol, and `emitrust.impl` is itself a SymbolTable
  // (so `lookupNearestSymbolFrom` from a method body would miss it): resolve
  // in the enclosing module directly. An unresolved def keeps the base —
  // conservative, never wrong.
  bool coversAllFields = false;
  if (auto module = op->getParentOfType<ModuleOp>())
    if (auto structDef = dyn_cast_or_null<emitrust::StructDefOp>(
            SymbolTable::lookupSymbolIn(
                module, StringAttr::get(op->getContext(),
                                        structType.getName()))))
      coversAllFields = assigns.size() == structDef.getFieldNames().size();
  os << " = " << structType.getName() << " { ";
  for (emitrust::AssignOp assign : assigns) {
    auto member = cast<emitrust::MemberOp>(assign.getVar().getDefiningOp());
    StringRef field = member.getMember();
    // clippy::redundant_field_names: a value whose rendered text is exactly
    // the field's name (a same-named binding, e.g. a C local lifted from
    // `s.a = a;`) collapses to the field shorthand `S { a, .. }`. Every
    // value reaching here is already named (its `let`/argument emitted
    // before the fuse point) or captured in `inlineExprs`, so the compare
    // is against the final text.
    auto inlined = inlineExprs.find(assign.getValue());
    std::string namedText = valueNames.lookup(assign.getValue());
    StringRef valueText = inlined != inlineExprs.end()
                              ? StringRef(inlined->second.text)
                              : StringRef(namedText);
    if (valueText == field) {
      os << field << ", ";
      continue;
    }
    os << field << ": ";
    if (failed(emitOperand(assign.getLoc(), assign.getValue(),
                           ExprPos::delimited())))
      return failure();
    os << ", ";
  }
  if (!coversAllFields)
    os << ".." << structType.getName() << "::default() ";
  os << "};\n";
  return success();
}

LogicalResult RustEmitter::emitOperand(Location loc, Value value,
                                       ExprPos pos) {
  // FR-61d: a captured single-use expression prints inline; everything else
  // falls through to the ordinary by-name rendering. A candidate that was
  // marked but never captured (e.g. it sits in a region emitted outside
  // `emitBlockBody`) misses the map and falls back to its name, which its
  // normal `let` still bound.
  auto it = inlineExprs.find(value);
  if (it != inlineExprs.end()) {
    bool parens = needsParens(it->second.prec, it->second.endsInCast, pos);
    if (parens)
      os << "(";
    os << it->second.text;
    if (parens)
      os << ")";
    return success();
  }
  FailureOr<std::string> name = lookupName(loc, value);
  if (failed(name))
    return failure();
  os << *name;
  return success();
}

LogicalResult RustEmitter::emitType(Location loc, Type type) {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    unsigned width = intType.getWidth();
    if (width == 1 && intType.isSignless()) {
      os << "bool";
      return success();
    }
    if (width == 8 || width == 16 || width == 32 || width == 64) {
      os << (intType.isUnsigned() ? 'u' : 'i') << width;
      return success();
    }
    return emitError(loc) << "cannot translate type " << type;
  }
  if (isa<IndexType>(type)) {
    os << "usize";
    return success();
  }
  if (type.isF32()) {
    os << "f32";
    return success();
  }
  if (type.isF64()) {
    os << "f64";
    return success();
  }
  if (auto opaqueType = dyn_cast<emitrust::OpaqueType>(type)) {
    // FR-150: an opaque spelling is emitter-owned Rust text (`Vec<i8>`,
    // `Box<Node>`, `Option<i64>`, `String`), so a shadowed prelude name in it
    // is qualified; with nothing shadowed this is the value verbatim.
    os << qualifyPrelude(opaqueType.getValue());
    return success();
  }
  if (auto refType = dyn_cast<emitrust::RefType>(type)) {
    os << "&";
    return emitType(loc, refType.getPointee());
  }
  if (auto mutRefType = dyn_cast<emitrust::MutRefType>(type)) {
    os << "&mut ";
    return emitType(loc, mutRefType.getPointee());
  }
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    os << "[";
    if (failed(emitType(loc, arrayType.getElementType())))
      return failure();
    os << "; " << arrayType.getSize() << "]";
    return success();
  }
  if (auto sliceType = dyn_cast<emitrust::SliceType>(type)) {
    // Unsized; reaches the output only behind a reference (`&mut [T]`).
    os << "[";
    if (failed(emitType(loc, sliceType.getElementType())))
      return failure();
    os << "]";
    return success();
  }
  if (auto cellSliceType = dyn_cast<emitrust::CellSliceType>(type)) {
    // Unsized; reaches the output only behind the shared reference
    // (`&[std::cell::Cell<T>]`, always fully qualified).
    os << "[std::cell::Cell<";
    if (failed(emitType(loc, cellSliceType.getElementType())))
      return failure();
    os << ">]";
    return success();
  }
  if (isa<emitrust::ArgvTableType>(type)) {
    // C99-43 C3: the argv table parameter — one owned NUL-terminated byte
    // vector per command-line argument, borrowed shared. The reference is
    // part of the rendering (the type only ever appears as a parameter).
    os << "&[" << preludeSpelling("Vec") << "<i8>]";
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    os << structType.getName();
    return success();
  }
  if (auto enumType = dyn_cast<emitrust::EnumType>(type)) {
    os << enumType.getName();
    return success();
  }
  if (auto dataEnumType = dyn_cast<emitrust::DataEnumType>(type)) {
    os << dataEnumType.getName();
    return success();
  }
  if (auto fnPtrType = dyn_cast<emitrust::FnPtrType>(type)) {
    // Nullable function pointer: the C null pointer is None, so no unsafe
    // sentinel is ever needed. FR-133's proved-non-null LOCALS render the
    // inner signature alone through `emitBindingType`; every other position
    // (parameter, result, struct field, global, temporary) keeps the wrapper.
    os << preludeSpelling("Option") << "<";
    if (failed(emitFnPtrSignature(loc, fnPtrType)))
      return failure();
    os << ">";
    return success();
  }
  // Lvalue types are never rendered; they fall through to the error below.
  return emitError(loc) << "cannot translate type " << type;
}

LogicalResult RustEmitter::emitFnPtrSignature(Location loc,
                                              emitrust::FnPtrType type) {
  // The `-> R` clause is omitted for a void result, matching Rust's `fn(...)`
  // spelling.
  os << "fn(";
  bool first = true;
  for (Type input : type.getInputs()) {
    if (!first)
      os << ", ";
    first = false;
    if (failed(emitType(loc, input)))
      return failure();
  }
  os << ")";
  if (!type.getResults().empty()) {
    os << " -> ";
    if (failed(emitType(loc, type.getResults().front())))
      return failure();
  }
  return success();
}

LogicalResult RustEmitter::emitBindingType(Location loc, Value value,
                                           Type type) {
  if (unwrappedFnPtrs.contains(value))
    return emitFnPtrSignature(loc, cast<emitrust::FnPtrType>(type));
  return emitType(loc, type);
}

LogicalResult RustEmitter::emitAttribute(Location loc, Attribute attr) {
  if (auto intAttr = dyn_cast<IntegerAttr>(attr)) {
    Type type = intAttr.getType();
    if (type.isInteger(1)) {
      os << (intAttr.getValue().getBoolValue() ? "true" : "false");
      return success();
    }
    auto intType = dyn_cast<IntegerType>(type);
    bool isUnsigned = intType && intType.isUnsigned();
    SmallString<32> digits;
    intAttr.getValue().toString(digits, /*Radix=*/10, /*Signed=*/!isUnsigned);
    os << digits;
    return success();
  }
  if (auto floatAttr = dyn_cast<FloatAttr>(attr)) {
    // A fold of `1.0 / 0.0` produces an infinity constant; Rust spells it
    // deterministically. NaN constants render as `fW::from_bits(0x..uW)`
    // with the exact APFloat bit pattern: Rust's NAN constant pins neither
    // sign nor payload, and the importer's %f/%g shims print nan/-nan by
    // sign bit, so only the bits are byte-exact — deterministic
    // sign+payload by construction (FR-69). from_bits is a const fn, so
    // this spelling is legal in every rendering context (locals, statics,
    // const-block thread_local initializers).
    if (floatAttr.getValue().isInfinity()) {
      os << (floatAttr.getType().isF32() ? "f32" : "f64")
         << (floatAttr.getValue().isNegative() ? "::NEG_INFINITY"
                                               : "::INFINITY");
      return success();
    }
    if (!floatAttr.getValue().isFinite()) {
      llvm::APInt bits = floatAttr.getValue().bitcastToAPInt();
      SmallString<32> hex;
      bits.toString(hex, /*Radix=*/16, /*Signed=*/false);
      bool isF32 = floatAttr.getType().isF32();
      os << (isF32 ? "f32" : "f64") << "::from_bits(0x" << hex
         << (isF32 ? "u32)" : "u64)");
      return success();
    }
    return emitFloatValue(loc, floatAttr.getValueAsDouble(),
                          floatAttr.getType().isF32());
  }
  if (auto opaqueAttr = dyn_cast<emitrust::OpaqueAttr>(attr)) {
    os << opaqueAttr.getValue();
    return success();
  }
  return emitError(loc) << "cannot translate constant attribute " << attr;
}

LogicalResult RustEmitter::emitFloatValue(Location loc, double value,
                                          bool isF32) {
  // Shortest representation that round-trips through the source type.
  char buffer[64];
  std::to_chars_result result{};
  if (isF32)
    result =
        std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(value));
  else
    result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc())
    return emitError(loc) << "failed to format floating-point constant";
  StringRef text(buffer, static_cast<size_t>(result.ptr - buffer));
  os << text;
  // Rust float literals need a decimal point or an exponent.
  if (!text.contains('.') && !text.contains('e') && !text.contains('E'))
    os << ".0";
  return success();
}

LogicalResult RustEmitter::emitPlaceExpr(Location loc, Value value,
                                         bool derefNeedsParens) {
  Operation *def = value.getDefiningOp();
  if (!def)
    return emitError(loc)
           << "cannot emit a place expression for a block argument";
  return llvm::TypeSwitch<Operation *, LogicalResult>(def)
      .Case<emitrust::VariableOp>([&](emitrust::VariableOp variableOp) {
        // A variable renders its name (never inlined): position is inert.
        return emitOperand(loc, variableOp.getResult(), ExprPos::stmt());
      })
      .Case<emitrust::MemberOp>([&](emitrust::MemberOp memberOp) {
        // The base is followed by `.field`, so a deref base must parenthesize.
        Value base = memberOp.getOperand();
        if (auto derefOp = base.getDefiningOp<emitrust::DerefOp>()) {
          // clippy::explicit_auto_deref: (*x).field -> x.field. The deref
          // operand is always a reference (EmitRustOps.td), so Deref coercion
          // re-adds the deref.
          if (failed(emitOperand(loc, derefOp.getOperand(),
                                 ExprPos::receiver())))
            return failure();
        } else {
          if (failed(emitPlaceExpr(loc, base, /*derefNeedsParens=*/true)))
            return failure();
        }
        os << "." << memberOp.getMember();
        return success();
      })
      .Case<emitrust::SubscriptOp>([&](emitrust::SubscriptOp subscriptOp) {
        // The base is followed by `[i]`, so a deref base must parenthesize.
        Value base = subscriptOp.getArray();
        if (auto derefOp = base.getDefiningOp<emitrust::DerefOp>()) {
          // clippy::explicit_auto_deref: (*x)[i] -> x[i]. The deref operand is
          // always a reference (EmitRustOps.td), so Deref coercion re-adds the
          // deref.
          if (failed(emitOperand(loc, derefOp.getOperand(),
                                 ExprPos::receiver())))
            return failure();
        } else {
          if (failed(emitPlaceExpr(loc, base, /*derefNeedsParens=*/true)))
            return failure();
        }
        os << "[";
        // An index-typed index sits bare between the delimiting brackets;
        // any other integer gets ` as usize` appended, making it a cast
        // source (`v1[(v0 - 1i32) as usize]`).
        bool isIndexTyped = isa<IndexType>(subscriptOp.getIndex().getType());
        if (failed(emitOperand(loc, subscriptOp.getIndex(),
                               isIndexTyped ? ExprPos::delimited()
                                            : ExprPos::castSource())))
          return failure();
        if (!isIndexTyped)
          os << " as usize";
        os << "]";
        return success();
      })
      .Case<emitrust::DerefOp>([&](emitrust::DerefOp derefOp) {
        os << (derefNeedsParens ? "(*" : "*");
        // The `*` must bind the whole inlined expression: receiver-strength
        // parenthesization.
        if (failed(emitOperand(loc, derefOp.getOperand(),
                               ExprPos::receiver())))
          return failure();
        if (derefNeedsParens)
          os << ")";
        return success();
      })
      .Case<emitrust::EnumRawOp>([&](emitrust::EnumRawOp enumRawOp) {
        // The base is followed by `.0`, so a deref base must parenthesize.
        Value base = enumRawOp.getOperand();
        if (auto derefOp = base.getDefiningOp<emitrust::DerefOp>()) {
          // clippy::explicit_auto_deref: (*x).0 -> x.0. The deref operand is
          // always a reference (EmitRustOps.td), so Deref coercion re-adds the
          // deref.
          if (failed(emitOperand(loc, derefOp.getOperand(),
                                 ExprPos::receiver())))
            return failure();
        } else {
          if (failed(emitPlaceExpr(loc, base, /*derefNeedsParens=*/true)))
            return failure();
        }
        os << ".0";
        return success();
      })
      .Default([&](Operation *) -> LogicalResult {
        return emitError(loc) << "expected a place-producing operation "
                                 "(variable, member, subscript, deref, or "
                                 "enum_raw)";
      });
}

LogicalResult RustEmitter::emitDefaultValue(Location loc, Type type) {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    if (intType.getWidth() == 1 && intType.isSignless())
      os << "false";
    else
      os << "0";
    return success();
  }
  if (isa<IndexType>(type)) {
    os << "0";
    return success();
  }
  if (type.isF32() || type.isF64()) {
    os << "0.0";
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    os << structType.getName() << "::default()";
    return success();
  }
  if (auto enumType = dyn_cast<emitrust::EnumType>(type)) {
    os << enumType.getName() << "::default()";
    return success();
  }
  if (isa<emitrust::FnPtrType>(type)) {
    // A default-initialized function pointer is the C null pointer.
    os << "None";
    return success();
  }
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    os << "[";
    if (failed(emitDefaultValue(loc, arrayType.getElementType())))
      return failure();
    os << "; " << arrayType.getSize() << "]";
    return success();
  }
  if (auto opaqueType = dyn_cast<emitrust::OpaqueType>(type)) {
    // The opaque types a variable is declared with are the String staging
    // binding of the sprintf lowering (default: the empty string), the
    // owned FILE* handle of the stdio lowering (default: C's NULL), and
    // (W2.3) a recognized `std::vector<T>`/`std::string` local (default:
    // `Vec::new()`/`String::new()`, mirroring the real C++ default
    // constructor's empty-container result) — every STL local's importer-
    // side construction (`emitStlConstruct`) always follows up with an
    // explicit `emitrust.assign` immediately after, so this default value
    // is dead code for every STL-recognized program the importer produces,
    // but `emitVariable` renders it unconditionally for every
    // no-initializer `emitrust.variable`.
    if (opaqueType.getValue() == "String") {
      os << preludeSpelling("String") << "::new()";
      return success();
    }
    if (opaqueType.getValue() == "__EmitrustFile") {
      os << "__EmitrustFile::Null";
      return success();
    }
    if (opaqueType.getValue().starts_with("Vec<")) {
      os << preludeSpelling("Vec") << "::new()";
      return success();
    }
    // W2.20: the same defensive arm for the ordered associative
    // families. Like the Vec/String arms above these are dead for every
    // program the importer produces (`emitStlConstruct` always follows a
    // map/set local with an explicit `emitrust.assign` of
    // `BTreeMap::new()`), but the empty container is the right default
    // for a no-initializer declaration and matches C++'s default ctor.
    if (opaqueType.getValue().starts_with("BTreeMap<")) {
      os << "BTreeMap::new()";
      return success();
    }
    if (opaqueType.getValue().starts_with("BTreeSet<")) {
      os << "BTreeSet::new()";
      return success();
    }
    // The C99-43 C1 Option-of-cursor cell (`Option<i64>`, the staged
    // out-cell temp of a single-global-or-NULL call): the default is the
    // C null pointer, mirroring `!emitrust.fn_ptr`'s None above.
    if (opaqueType.getValue().starts_with("Option<")) {
      os << "None";
      return success();
    }
    // W2.21: the defensive arm for a `Box<T>` (std::unique_ptr) place. Like
    // the arms above this is dead for every program the importer produces
    // (`emitStlBoxLocalInit` always follows the place with an explicit
    // `emitrust.assign` of `Box::new(..)`, and a Box local with no
    // initializer is a located rejection because C++'s default-constructed
    // unique_ptr is NULL), but `emitVariable` renders a default for every
    // no-initializer `emitrust.variable` and dies at translate time
    // otherwise. `Default::default()` resolves under the emitted
    // `let x: Box<T> =` annotation.
    if (opaqueType.getValue().starts_with("Box<")) {
      os << "Default::default()";
      return success();
    }
  }
  return emitError(loc) << "no default value for type " << type;
}

void RustEmitter::emitEscapedStringLiteral(StringRef value) {
  os << '"';
  for (char c : value) {
    switch (c) {
    case '\\':
      os << "\\\\";
      break;
    case '"':
      os << "\\\"";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\t':
      os << "\\t";
      break;
    case '\r':
      os << "\\r";
      break;
    default:
      os << c;
      break;
    }
  }
  os << '"';
}

LogicalResult RustEmitter::emitLetPrologue(Value result, bool isMut) {
  // FR-140: the two folds below assign a name that never renders; read the
  // pending flags BEFORE `assignName` so such a name cannot force an
  // `#[allow(non_snake_case)]` onto the enclosing function.
  llvm::SaveAndRestore<bool> notes(noteBoundNames,
                                   !pendingTailFold && !pendingInlineCapture);
  std::string name = assignName(result);
  // FR-61a fold: the binding never renders -- the right-hand side that
  // follows becomes the function's tail expression (its trailing `;` is
  // removed when the tail return is reached). The name is still assigned
  // above so the v-numbering of every other value is unchanged.
  if (pendingTailFold) {
    pendingTailFold = false;
    tailFoldActive = true;
    return success();
  }
  // Spike 61d-0: the inlined op's statement is being captured; the binding
  // never renders, but `assignName` above still ran so surviving v-numbering
  // is unchanged.
  if (pendingInlineCapture) {
    pendingInlineCapture = false;
    return success();
  }
  os << "let ";
  if (isMut)
    os << "mut ";
  os << name << ": ";
  if (failed(emitBindingType(result.getLoc(), result, result.getType())))
    return failure();
  os << " = ";
  return success();
}

/// Returns whether `op` renders as a diverging (`!`-typed) Rust expression,
/// after which the rest of its block is unreachable. Emitting that tail would
/// produce dead code that trips `unreachable_code` (and cascading `unused_*`),
/// so block emission stops here. `panic!` and `std::process::exit` are the
/// importer's diverging opaque calls; the structured terminators never have
/// successors within their block but are covered for completeness.
static bool opDiverges(Operation *op) {
  if (auto call = dyn_cast<emitrust::CallOpaqueOp>(op)) {
    llvm::StringRef callee = call.getCallee();
    // `unimplemented!` (emitted by `--recover` for unsupported constructs)
    // diverges even though it yields a value into a `let`.
    return callee == "panic!" || callee == "std::process::exit" ||
           callee == "unimplemented!";
  }
  return isa<emitrust::ReturnOp, emitrust::BreakOp, emitrust::ContinueOp>(op);
}

/// FR-61a: whether `op`'s emitter renders exactly one
/// `let <name>: <type> = <rhs>;` statement through `emitLetPrologue`, so that
/// suppressing the prologue leaves `<rhs>;` -- the fold's tail expression.
/// `emitrust.variable` (a place, not a value) and every multi-result form are
/// excluded by the caller's single-result check; a deferred `emitrust.let`
/// cannot arise because deferral requires assignments, i.e. more than the
/// fold's single use.
static bool isTailFoldableProducer(Operation *op) {
  // FR-62 slice 5a: a variant construction and a result-mode match both
  // render exactly one `let <name>: <type> = <rhs>;` through the prologue
  // (the match's multi-line `};` still ends in `;`, exactly like the
  // folded if-expression binding), so both fold. Neither joins the FR-61d
  // pure-inlining set: a brace variant literal is illegal in Rust's
  // scrutinee positions, which never parenthesize, and a match is a
  // region op like `if`.
  return isa<emitrust::ConstantOp, emitrust::LiteralOp, emitrust::LetOp,
             emitrust::CallOpaqueOp, emitrust::CallIndirectOp,
             emitrust::MethodCallOp, emitrust::AddOp, emitrust::SubOp,
             emitrust::MulOp, emitrust::DivOp, emitrust::RemOp,
             emitrust::AndOp, emitrust::OrOp, emitrust::XorOp,
             emitrust::ShlOp, emitrust::ShrOp, emitrust::CmpOp,
             emitrust::CastOp, emitrust::BitcastOp, emitrust::SelectOp,
             emitrust::GlobalLoadOp, emitrust::CellGetOp, emitrust::LoadOp,
             emitrust::AddrOfOp, emitrust::SliceOfOp,
             emitrust::EnumVariantOp, emitrust::MatchOp>(op);
}

FailureOr<bool> RustEmitter::emitDropOrCapture(Operation &op) {
  // FR-61d: a dropped pure op emits nothing. Its result is still named so
  // the surviving v-numbering matches the un-dropped rendering exactly.
  if (droppedOps.count(&op)) {
    // FR-140: a dropped op renders nothing at all, so its name cannot trip a
    // lint either.
    llvm::SaveAndRestore<bool> notes(noteBoundNames, false);
    assignName(op.getResult(0));
    return true;
  }
  if (!inlinedOps.count(&op))
    return false;
  // FR-61d: an inlined op renders into the buffer with its `let` prologue
  // suppressed, then the statement text is captured, stripped of
  // indentation and its trailing `;\n`, and removed from the buffer; the
  // consumer prints it inline. Invariants are hard runtime checks (the
  // tree builds -DNDEBUG, a plain assert would vanish): a violation must
  // fail the translation loudly, never emit silently wrong text.
  if (&op == tailFoldCandidate)
    return op.emitOpError("FR-61d: op is both tail-folded and inlined");
  os.flush();
  size_t start = buffer.size();
  pendingInlineCapture = true;
  LogicalResult captured = emitOperation(op);
  pendingInlineCapture = false; // defensive; the prologue consumed it
  if (failed(captured))
    return failure();
  os.flush();
  StringRef text = StringRef(buffer).substr(start);
  text = text.ltrim();
  if (!text.ends_with(";\n"))
    return op.emitOpError(
        "FR-61d: captured inline statement does not end with ';'");
  text = text.drop_back(2);
  std::string expr = text.str();
  // A numeric constant carries its literal type suffix so the dropped
  // binding's type annotation cannot orphan an inference anchor.
  if (auto constant = dyn_cast<emitrust::ConstantOp>(&op))
    if (auto typed = dyn_cast<TypedAttr>(constant.getValue()))
      if (isa<IntegerAttr, FloatAttr>(constant.getValue()) &&
          !typed.getType().isInteger(1))
        expr += inlineSuffixFor(constant.getResult().getType());
  Prec prec = capturedPrec(&op, expr);
  bool endsInCast = capturedEndsInCast(&op);
  inlineExprs[op.getResult(0)] = {std::move(expr), prec, endsInCast};
  buffer.resize(start);
  ++inlineCaptureCount;
  return true; // pure producers never diverge
}

LogicalResult RustEmitter::emitBlockBody(Block &block) {
  for (Operation &op : block) {
    FailureOr<bool> consumed = emitDropOrCapture(op);
    if (failed(consumed))
      return failure();
    if (*consumed)
      continue;
    // FR-61a: while the fold candidate is emitted, `emitLetPrologue` names
    // its result but prints nothing, leaving only `<rhs>;`.
    pendingTailFold = (&op == tailFoldCandidate);
    if (failed(emitOperation(op)))
      return failure();
    // A candidate whose emitter never reached `emitLetPrologue` abandons the
    // fold; the tail return then falls back to the binding's name.
    pendingTailFold = false;
    // Statements after a diverging op are unreachable; stop to keep the
    // emitted body free of dead code.
    if (opDiverges(&op))
      break;
  }
  return success();
}

LogicalResult RustEmitter::emitRegionBody(Operation *parent, Region &region) {
  if (region.empty())
    return success();
  if (!region.hasOneBlock())
    return parent->emitOpError("multi-block regions are not supported");
  increaseIndent();
  if (failed(emitBlockBody(region.front())))
    return failure();
  decreaseIndent();
  return success();
}

//===----------------------------------------------------------------------===//
// Per-operation emitters
//===----------------------------------------------------------------------===//

/// FR-62 slice 5b: the shared actor runtime, generic over the message type.
/// Fixed text measured by the SLICE-5b SPIKE (37 non-blank lines) and proven
/// by its byte-diff and panic probes: `call` keeps every message synchronous
/// (send + blocking recv), and the reap/shutdown pair preserves single-panic
/// provenance (join + resume_unwind re-raises the actor's own payload in the
/// caller, exit 101, no hang).
static constexpr char kActorRtModule[] =
    R"(mod actor_rt {
    pub struct Handle<M> {
        pub tx: std::sync::mpsc::Sender<M>,
        pub join: Option<std::thread::JoinHandle<()>>,
    }
    impl<M> Handle<M> {
        /// Actor is unreachable: join it and re-raise its panic payload here.
        pub fn reap(&mut self) -> ! {
            let join = self.join.take().expect("actor already reaped");
            match join.join() {
                Err(payload) => std::panic::resume_unwind(payload),
                Ok(()) => panic!("actor exited without replying"),
            }
        }
        /// send + blocking recv; disconnection on either side reaps the actor.
        pub fn call<T>(&mut self, msg: M, rrx: std::sync::mpsc::Receiver<T>) -> T {
            if self.tx.send(msg).is_err() {
                self.reap();
            }
            match rrx.recv() {
                Ok(v) => v,
                Err(_) => self.reap(),
            }
        }
        /// Graceful shutdown: drop the sender to end the mailbox loop, join,
        /// and propagate a late actor panic with the original payload.
        pub fn shutdown(self) {
            let Handle { tx, join } = self;
            drop(tx);
            if let Some(join) = join {
                if let Err(payload) = join.join() {
                    std::panic::resume_unwind(payload);
                }
            }
        }
    }
}
)";

/// FR-62 slice 5c: the ASYNC flavor of the shared actor runtime (E4's
/// separately emitted tokio crate flavor). Same shape and same
/// single-panic-provenance contract as the threaded text above, on the
/// tokio substrate: an UNBOUNDED mpsc mailbox (send never awaits, so the
/// wrapper's only suspension point is the reply await — at most one
/// message in flight, effect order equals program order on the
/// current_thread runtime), oneshot replies, and a tokio JoinHandle whose
/// JoinError re-raises the actor's own panic payload via `into_panic`.
/// Probe-proven (crate builds warning-clean and runs on features
/// ["rt", "sync"] only).
static constexpr char kActorRtModuleAsync[] =
    R"(mod actor_rt {
    pub struct Handle<M> {
        pub tx: tokio::sync::mpsc::UnboundedSender<M>,
        pub join: Option<tokio::task::JoinHandle<()>>,
    }
    impl<M> Handle<M> {
        /// Actor is unreachable: await its task and re-raise its panic here.
        pub async fn reap(&mut self) -> ! {
            let join = self.join.take().expect("actor already reaped");
            match join.await {
                Err(err) if err.is_panic() => std::panic::resume_unwind(err.into_panic()),
                Err(_) => panic!("actor task cancelled"),
                Ok(()) => panic!("actor exited without replying"),
            }
        }
        /// Unbounded send + immediate await of the reply; disconnection on
        /// either side reaps the actor.
        pub async fn call<T>(&mut self, msg: M, rrx: tokio::sync::oneshot::Receiver<T>) -> T {
            if self.tx.send(msg).is_err() {
                self.reap().await;
            }
            match rrx.await {
                Ok(v) => v,
                Err(_) => self.reap().await,
            }
        }
        /// Graceful shutdown: drop the sender to end the mailbox loop, await
        /// the task, and propagate a late actor panic with its own payload.
        pub async fn shutdown(self) {
            let Handle { tx, join } = self;
            drop(tx);
            if let Some(join) = join {
                if let Err(err) = join.await {
                    if err.is_panic() {
                        std::panic::resume_unwind(err.into_panic());
                    }
                }
            }
        }
    }
}
)";

/// FR-77 backstop, the parse-back half: the fn-item identifier inside an
/// importer-produced fn-ptr constant spelling `Some(<identifier>)`, or an
/// empty StringRef for every other opaque text. Deliberately restricted to
/// ITEM-PATH-shaped payloads on fn_ptr-typed positions (`isRustItemPath`):
/// the va-cursor spelling `Some(0i64)` (digit-led) and the FR-52
/// requirement rewrite's `Some(f::<E>)` spelling are other contracts'
/// opaque text and must pass untouched. FR-159 widened the accepted shape
/// from a bare identifier to a PATH, because an item rendered inside a
/// per-TU module is named `crate::tu0::helper` and the narrow test switched
/// this whole backstop off for it, silently.
static StringRef fnPtrConstantTargetIdent(Attribute attr) {
  auto opaque = dyn_cast_if_present<emitrust::OpaqueAttr>(attr);
  if (!opaque)
    return StringRef();
  StringRef text = opaque.getValue();
  if (!text.consume_front("Some(") || !text.consume_back(")"))
    return StringRef();
  if (!isRustItemPath(text))
    return StringRef();
  return text;
}

/// FR-77 backstop, the recursive half: walks an initializer attribute
/// alongside its type — recursing through array elements and struct fields
/// exactly as `emitAggregateInit` will render them (the FR-52 global check
/// this generalizes stopped at the top level, which would miss every fn-ptr
/// TABLE) — and refuses any fn_ptr-typed `Some(<identifier>)` leaf naming a
/// function absent from `fnItems`. `op` anchors the diagnostic and the
/// struct-def symbol lookup.
static LogicalResult
verifyFnPtrTargetsPresent(Operation *op, Attribute init, Type type,
                          const llvm::StringSet<> &fnItems) {
  if (isa<emitrust::FnPtrType>(type)) {
    StringRef target = fnPtrConstantTargetIdent(init);
    if (!target.empty() && !fnItems.contains(target))
      return op->emitError()
             << "dangling function pointer target '" << target
             << "': the module defines no function with that name";
    return success();
  }
  auto elements = dyn_cast_if_present<ArrayAttr>(init);
  if (!elements)
    return success();
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    for (Attribute element : elements)
      if (failed(verifyFnPtrTargetsPresent(op, element,
                                           arrayType.getElementType(),
                                           fnItems)))
        return failure();
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    // FR-84: module-first — the walked emitrust.constant may sit inside an
    // impl method, and a nearest-table miss here would silently SKIP the
    // located refusal this check exists to give (rustc E0425 instead).
    emitrust::StructDefOp structDef =
        emitrust::StructDefOp::lookupFrom(op, structType.getName());
    if (!structDef || elements.size() != structDef.getFieldTypes().size())
      return success(); // Shape mismatch is the aggregate renderer's error.
    for (auto [element, fieldType] :
         llvm::zip_equal(elements, structDef.getFieldTypes()))
      if (failed(verifyFnPtrTargetsPresent(
              op, element, cast<TypeAttr>(fieldType).getValue(), fnItems)))
        return failure();
  }
  return success();
}

//===----------------------------------------------------------------------===//
// FR-182: the C-ABI export gate over the two admitted STRUCT shapes
//===----------------------------------------------------------------------===//

// Defined below, beside FR-139's own predicates; declared here because the
// module pre-pass that decides which structs are rendered `#[repr(C)]` runs
// before any item is emitted.
static bool isCAbiScalarType(Type type);

namespace {
/// FR-182: what `--c-abi-exports` admits a function signature as.
enum class CAbiClass {
  /// Refused; the verdict's `blocker` says why, in that shape's own words.
  Refused,
  /// FR-139: every parameter and every result is a builtin scalar.
  Scalar,
  /// FR-182 class 0: scalars and ABI-faithful structs passed and returned BY
  /// VALUE. A plain `#[no_mangle] extern "C"` item; ZERO `unsafe`.
  ByValueStruct,
  /// FR-182 class 1: exactly ONE parameter is a reference to an ABI-faithful
  /// struct and every other parameter and the result is a scalar. A
  /// one-statement `unsafe extern "C"` wrapper delegates to the untouched
  /// translated function.
  StructPointer
};

struct CAbiVerdict {
  CAbiClass kind = CAbiClass::Refused;
  /// Why not, phrased as the tail of "no C-ABI export for 'f': ...".
  std::string blocker;
  /// For `StructPointer`: which parameter carries the reference, whether it
  /// is `&mut`, and the struct it points at.
  unsigned refIndex = 0;
  bool refIsMut = false;
  StringRef refStructName;
};
} // namespace

/// FR-139's original refusal sentence, kept VERBATIM for every shape FR-182
/// did not widen. A slice parameter above all: it is a two-register fat
/// pointer, rustc compiles the `extern "C"` mismatch with only a
/// non-FFI-safe warning, and every later argument shifts (measured on crc16
/// -- native 21983, export 25322, exit 0, no diagnostic). That wording is
/// pinned by test/Driver/c-abi-exports.c and test/Driver/c-abi-exports-actor.c
/// and must not drift.
static constexpr llvm::StringLiteral kCAbiNotAllScalarBlocker =
    "its signature is not all-scalar (a C-ABI entry point may only take and "
    "return builtin integer and floating-point types)";

/// FR-182: is the struct named `name` ABI-faithful, per the importer's
/// whitelist? On `false`, `reason` receives the recorded divergence -- that
/// string is the whole diagnostic value of the predicate, so it is never
/// summarized here.
static bool cAbiStructIsFaithful(Operation *from, StringRef name,
                                 std::string &reason) {
  emitrust::StructDefOp def = emitrust::StructDefOp::lookupFrom(from, name);
  if (!def) {
    reason = "this module carries no definition of it";
    return false;
  }
  if (def->hasAttr(emitrust::kAbiFaithfulAttrName))
    return true;
  if (auto why = def->getAttrOfType<StringAttr>(
          emitrust::kAbiUnfaithfulReasonAttrName))
    reason = why.getValue().str();
  else
    reason = "no layout model was recorded for it at import";
  return false;
}

/// FR-182: classifies `type` against the C ABI, naming the refusal per shape.
///
/// Before FR-182 every refused signature got FR-139's identical "not
/// all-scalar" sentence, which is factually wrong for most of them. The
/// residual classes each say what is actually in the way, because the whole
/// point of the faithfulness predicate is that "the pointer member 'buffer',
/// emitted as an i64 data-pointer cursor rather than an address" tells the
/// person who wrote the C something the generic sentence never could.
static CAbiVerdict classifyCAbiSignature(Operation *from, FunctionType type) {
  CAbiVerdict verdict;
  unsigned sliceCount = 0;
  unsigned refCount = 0;
  bool sawOther = false;
  bool sawByValueStruct = false;
  bool refIsStruct = false;
  bool refFaithful = false;
  StringRef refStructName;
  std::string refBlocker;
  std::string byValueBlocker;
  StringRef byValueStructName;
  bool byValueBlockerIsResult = false;

  auto noteByValueStruct = [&](emitrust::StructType structType, bool isResult) {
    sawByValueStruct = true;
    std::string reason;
    if (!cAbiStructIsFaithful(from, structType.getName(), reason) &&
        byValueBlocker.empty()) {
      byValueBlocker = reason;
      byValueStructName = structType.getName();
      byValueBlockerIsResult = isResult;
    }
  };

  for (auto [index, input] : llvm::enumerate(type.getInputs())) {
    if (isCAbiScalarType(input))
      continue;
    if (auto structType = dyn_cast<emitrust::StructType>(input)) {
      noteByValueStruct(structType, /*isResult=*/false);
      continue;
    }
    Type pointee;
    bool isMut = false;
    if (auto mutRef = dyn_cast<emitrust::MutRefType>(input)) {
      pointee = mutRef.getPointee();
      isMut = true;
    } else if (auto ref = dyn_cast<emitrust::RefType>(input)) {
      pointee = ref.getPointee();
    }
    if (!pointee) {
      sawOther = true;
      continue;
    }
    // A slice is a REFERENCE too, and counts against the same cap; it is
    // separated here only so its refusal keeps FR-139's wording.
    if (isa<emitrust::SliceType>(pointee)) {
      ++sliceCount;
      continue;
    }
    ++refCount;
    verdict.refIndex = index;
    verdict.refIsMut = isMut;
    if (auto structType = dyn_cast<emitrust::StructType>(pointee)) {
      refIsStruct = true;
      refStructName = structType.getName();
      refFaithful = cAbiStructIsFaithful(from, refStructName, refBlocker);
    }
  }
  for (Type result : type.getResults()) {
    if (isCAbiScalarType(result))
      continue;
    if (auto structType = dyn_cast<emitrust::StructType>(result)) {
      noteByValueStruct(structType, /*isResult=*/true);
      continue;
    }
    sawOther = true;
  }

  // A slice, and anything with no C spelling at all, keeps FR-139's wording
  // whatever else is in the signature. (A slice beside a reference is also
  // over the structural cap below; either refusal is correct and this one is
  // the pinned one.)
  if (sliceCount != 0 || sawOther) {
    verdict.blocker = kCAbiNotAllScalarBlocker.str();
    return verdict;
  }
  // THE structural cap: at most ONE reference parameter, ever. Two are
  // `noalias` to LLVM and a C caller may legally alias them -- measured at
  // rustc 1.96.1 -O3, `kernel(a,b)` called with `a == b` returned 10 where
  // the clang native returned 104, exit 0, no diagnostic. No library can
  // disprove that its caller aliases, so this is not relaxable by analysis.
  if (refCount >= 2) {
    verdict.blocker =
        ("it takes " + llvm::Twine(refCount) +
         " pointer arguments; a C caller may legally alias them, and two "
         "&mut references built from aliased pointers miscompile (measured)")
            .str();
    return verdict;
  }
  if (refCount == 1) {
    if (!refIsStruct) {
      verdict.blocker = kCAbiNotAllScalarBlocker.str();
      return verdict;
    }
    if (!refFaithful) {
      verdict.blocker =
          ("it takes a pointer to '" + refStructName +
           "', whose layout model is not ABI-faithful (" + refBlocker + ")")
              .str();
      return verdict;
    }
    if (sawByValueStruct) {
      verdict.blocker =
          "it combines a pointer parameter with a by-value aggregate "
          "elsewhere in the signature, a mixture this wave does not admit";
      return verdict;
    }
    verdict.kind = CAbiClass::StructPointer;
    verdict.refStructName = refStructName;
    return verdict;
  }
  if (!byValueBlocker.empty()) {
    verdict.blocker =
        ("it " + llvm::Twine(byValueBlockerIsResult ? "returns" : "takes") +
         " '" + byValueStructName +
         "' by value, whose layout model is not ABI-faithful (" +
         byValueBlocker + ")")
            .str();
    return verdict;
  }
  verdict.kind =
      sawByValueStruct ? CAbiClass::ByValueStruct : CAbiClass::Scalar;
  return verdict;
}

LogicalResult RustEmitter::emitModule(ModuleOp moduleOp) {
  // FR-57a: a module carrying an `emitrust.extern_decl`-marked declaration
  // is one translation unit's SHARD, not a program — the marked symbol's
  // definition lives in another TU. Refuse it up front, mirroring FR-52's
  // contract that forgetting a resolution step cannot silently emit a
  // broken crate. Walk (not just the top level): a marked body-less method
  // declaration sits inside an `emitrust.impl`.
  Operation *deferred = nullptr;
  moduleOp.walk([&](Operation *op) {
    if (!op->hasAttr(emitrust::kExternDeclAttrName))
      return WalkResult::advance();
    deferred = op;
    return WalkResult::interrupt();
  });
  if (deferred) {
    auto symbol =
        deferred->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
    return deferred->emitError()
           << "unresolved deferred external '"
           << (symbol ? symbol.getValue() : llvm::StringRef("<unknown>"))
           << "': the module must be linked against the defining translation "
              "unit before Rust emission";
  }
  // FR-110: collect every C++ method member's in-impl spelling up front --
  // method_call and qualified call_opaque sites print through this map, and
  // both can appear in functions emitted BEFORE the impl that owns the
  // member (the impls are appended at the end of the module).
  for (auto implOp : moduleOp.getOps<emitrust::ImplOp>())
    for (auto funcOp : implOp.getBody().front().getOps<emitrust::FuncOp>())
      if (auto rustName = funcOp->getAttrOfType<StringAttr>(
              emitrust::kMethodRustNameAttrName))
        methodRustNames[SymbolTable::getSymbolName(funcOp).getValue()] = {
            implOp.getStructName(), rustName.getValue()};
  // FR-70: a module reaching emission with an external-requirement GLOBAL
  // still marked skipped `emitrust-lower-external-requirements`. Unlike a
  // marked FUNCTION (body-less, so translation dies naturally), a
  // declaration-only global is perfectly renderable — as DEFAULTED
  // thread_local storage the C program never had — so silence here would be
  // a miscompile, not an error. Refuse instead, completing the FR-52 marker
  // contract for the global side.
  for (auto globalOp : moduleOp.getOps<emitrust::GlobalOp>())
    if (globalOp->hasAttr(emitrust::kExternalRequirementAttrName))
      return globalOp.emitError()
             << "unlowered external-requirement global '"
             << globalOp.getSymName()
             << "': emitrust-lower-external-requirements must run before "
                "Rust emission";
  // FR-80, the ADDRESS side of the same contract: a surviving
  // `emitrust.global_addr` was never rewritten to its `E::<getter>()` call
  // — the lowering was skipped, or the target was never a requirement (a
  // DEFINED const global's address has no supported Rust spelling). The op
  // has no rendering of its own, so silence is impossible, but the named
  // refusal beats a generic translation failure.
  {
    emitrust::GlobalAddrOp leakedAddr;
    moduleOp.walk([&](emitrust::GlobalAddrOp addrOp) {
      leakedAddr = addrOp;
      return WalkResult::interrupt();
    });
    if (leakedAddr)
      return leakedAddr.emitError()
             << "unlowered external-requirement address of '"
             << leakedAddr.getGlobal()
             << "': emitrust-lower-external-requirements must run before "
                "Rust emission";
  }
  // FR-78 marker contract: an `emitrust.opaque_union`-marked struct_def is
  // a differing-aggregate-arm C union imported as an opaque byte blob. The
  // importer rejects every access through any arm at its own site; if one
  // leaks here anyway, nothing structural catches it — `emitrust.member`
  // field names are not cross-checked against the struct_def, so the
  // verifier passes and the rendered Rust selects a field the struct does
  // not have: rustc E0609, a whole-crate loss with no source location
  // (exactly how the pre-FR-78 union placeholder died). Refuse ANY member
  // selection on a marked type — with ONE enumerated allowance (FR-83):
  // a selection of the marked struct_def's single BLOB field by its own
  // name, cross-checked against the def right here, so the selected field
  // provably exists and the E0609 hazard cannot recur. That is how FR-83
  // renders arm accesses as blob byte views; an arm name, any other name,
  // or a marked def without exactly one field still refuses.
  llvm::StringMap<StringAttr> opaqueUnionBlobFields;
  for (auto structDefOp : moduleOp.getOps<emitrust::StructDefOp>())
    if (structDefOp->hasAttr(emitrust::kOpaqueUnionAttrName)) {
      ArrayAttr fieldNames = structDefOp.getFieldNames();
      StringAttr blobField =
          fieldNames.size() == 1 ? dyn_cast<StringAttr>(fieldNames[0])
                                 : StringAttr();
      opaqueUnionBlobFields[structDefOp.getSymName()] = blobField;
    }
  if (!opaqueUnionBlobFields.empty()) {
    emitrust::MemberOp leaked;
    llvm::StringRef leakedUnion;
    moduleOp.walk([&](emitrust::MemberOp memberOp) {
      auto lvalueType =
          dyn_cast<emitrust::LValueType>(memberOp.getOperand().getType());
      auto structType =
          lvalueType ? dyn_cast<emitrust::StructType>(lvalueType.getValueType())
                     : emitrust::StructType();
      if (!structType)
        return WalkResult::advance();
      auto it = opaqueUnionBlobFields.find(structType.getName());
      if (it == opaqueUnionBlobFields.end())
        return WalkResult::advance();
      if (it->second && memberOp.getMember() == it->second.getValue())
        return WalkResult::advance();
      leaked = memberOp;
      leakedUnion = structType.getName();
      return WalkResult::interrupt();
    });
    if (leaked)
      return leaked.emitError()
             << "opaque union '" << leakedUnion
             << "' member access leaked to emission; the importer must "
                "reject this at the access site";
  }
  // FR-173: the GENERAL structural backstop the marker check above is one
  // special case of. FR-78 only inspects member ops whose BASE TYPE is the
  // marked union, so a selection made against the PARENT struct sailed
  // straight through -- measured at HEAD, a module selecting "opaque" on a
  // `struct S` with fields ["cmd", "u"] translated with rc=0 and rendered
  // `v0.opaque[...]`, which is the very rustc E0609 whole-crate loss FR-78
  // exists to prevent, just reached by a different route. So cross-check
  // EVERY `emitrust.member` whose base names a struct_def OF THIS MODULE
  // against that def's field list; a struct type this module carries no
  // def for (an extern or opaque record) has no field list here and is not
  // this check's business.
  //
  // ORDER IS LOAD-BEARING: this runs AFTER the marker check, never before.
  // An ARM selection on a marked union violates both rules at once, and
  // FR-78's wording is the one that names the broken contract and the
  // importer's obligation (test/Target/Rust/errors.mlir pins it, and
  // test/Target/Rust/member-field-backstop.mlir pins the ordering).
  {
    llvm::StringMap<llvm::StringSet<>> structFields;
    for (auto structDefOp : moduleOp.getOps<emitrust::StructDefOp>()) {
      llvm::StringSet<> &names = structFields[structDefOp.getSymName()];
      for (Attribute nameAttr : structDefOp.getFieldNames())
        if (auto str = dyn_cast<StringAttr>(nameAttr))
          names.insert(str.getValue());
    }
    emitrust::MemberOp bad;
    llvm::StringRef badStruct;
    moduleOp.walk([&](emitrust::MemberOp memberOp) {
      auto lvalueType =
          dyn_cast<emitrust::LValueType>(memberOp.getOperand().getType());
      auto structType =
          lvalueType ? dyn_cast<emitrust::StructType>(lvalueType.getValueType())
                     : emitrust::StructType();
      if (!structType)
        return WalkResult::advance();
      auto it = structFields.find(structType.getName());
      if (it == structFields.end())
        return WalkResult::advance();
      if (it->second.contains(memberOp.getMember()))
        return WalkResult::advance();
      bad = memberOp;
      badStruct = structType.getName();
      return WalkResult::interrupt();
    });
    if (bad)
      return bad.emitError()
             << "member '" << bad.getMember() << "' does not exist on struct '"
             << badStruct << "'";
  }
  // FR-77: a fn-ptr constant is rendered by writing its opaque
  // `Some(<name>)` text verbatim, and that name is NOT a symbol use — no
  // verifier, no walk, nothing structural keeps a planner change from
  // handing this emitter a module that spells out a function it does not
  // contain. The rendered crate would be rustc E0425: a whole-crate loss
  // this emitter could have refused with a located diagnostic. So, marker-
  // contract style, refuse any identifier-shaped fn_ptr-typed
  // `Some(<name>)` — global initializers (recursed through aggregates) and
  // `emitrust.constant` rvalues alike — whose name is not a function item
  // of this module.
  {
    llvm::StringSet<> fnItems;
    moduleOp.walk(
        [&](emitrust::FuncOp fn) { fnItems.insert(fn.getSymName()); });
    for (auto globalOp : moduleOp.getOps<emitrust::GlobalOp>())
      if (Attribute init = globalOp.getInitAttr())
        if (failed(verifyFnPtrTargetsPresent(globalOp.getOperation(), init,
                                             globalOp.getType(), fnItems)))
          return failure();
    WalkResult dangling = moduleOp.walk([&](emitrust::ConstantOp constant) {
      if (failed(verifyFnPtrTargetsPresent(constant.getOperation(),
                                           constant.getValue(),
                                           constant.getType(), fnItems)))
        return WalkResult::interrupt();
      return WalkResult::advance();
    });
    if (dangling.wasInterrupted())
      return failure();
  }
  // W2.17: the has_drop struct set, collected once before any function is
  // emitted -- the per-function liveness analyses consult it to keep every
  // binding of a destructor-carrying struct initialized and its stores live.
  dropStructNames.clear();
  for (auto structDefOp : moduleOp.getOps<emitrust::StructDefOp>())
    if (structDefOp->hasAttr(emitrust::kHasDropAttrName))
      dropStructNames.insert(structDefOp.getSymName());
  // FR-124: the non-Copy struct set. Seed with the per-struct local
  // triggers (the exact predicates emitStructDef used before this pass),
  // then propagate through struct-typed fields to a fixpoint: a struct
  // with a non-Copy field cannot derive Copy (E0204). Fields form a DAG
  // in the emitted image, so the fixpoint terminates trivially.
  // FR-150: the shadowed-prelude set, collected before ANY item is rendered
  // because the very first struct_def may already carry a fn-ptr field whose
  // `Option<..>` needs the qualified spelling. A module that shadows nothing
  // leaves this empty and every rendering below is byte-identical to what it
  // was before this FR.
  shadowedPreludeNames = emitrust::collectShadowedPreludeNames(moduleOp);
  nonCopyStructNames.clear();
  {
    llvm::SmallVector<emitrust::StructDefOp> structDefs(
        moduleOp.getOps<emitrust::StructDefOp>());
    auto locallyNonCopy = [](emitrust::StructDefOp def) {
      if (def->hasAttr(emitrust::kHasDropAttrName))
        return true;
      // W2.23: a class with a user copy constructor loses `Copy` too -- the
      // imported ctor call is the ONLY copy point, and a bitwise `Copy` at a
      // by-value pass would silently substitute for the user's constructor.
      if (def->hasAttr(emitrust::kHasCopyCtorAttrName))
        return true;
      return llvm::any_of(def.getFieldTypes(), [](Attribute a) {
        auto opaque =
            dyn_cast<emitrust::OpaqueType>(cast<TypeAttr>(a).getValue());
        return opaque && (opaque.getValue().starts_with("Vec<") ||
                          opaque.getValue() == "String");
      });
    };
    for (auto def : structDefs)
      if (locallyNonCopy(def))
        nonCopyStructNames.insert(def.getSymName());
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto def : structDefs) {
        if (nonCopyStructNames.contains(def.getSymName()))
          continue;
        bool fieldNonCopy =
            llvm::any_of(def.getFieldTypes(), [&](Attribute a) {
              Type type = cast<TypeAttr>(a).getValue();
              while (auto arrayType = dyn_cast<emitrust::ArrayType>(type))
                type = arrayType.getElementType();
              auto structType = dyn_cast<emitrust::StructType>(type);
              return structType &&
                     nonCopyStructNames.contains(structType.getName());
            });
        if (fieldNonCopy) {
          nonCopyStructNames.insert(def.getSymName());
          changed = true;
        }
      }
    }
  }
  // FR-182: the structs rendered `#[repr(C)]`, decided BEFORE any item is
  // emitted -- a struct_def is rendered long before the exported function
  // that names it, so the answer cannot be discovered in `emitFunc`.
  //
  // The closure is TRANSITIVE and that is a correctness requirement, not an
  // optimisation: if exported `A` contains `B`, A's own field offsets depend
  // on B's layout, so B must be `#[repr(C)]` too or A's asserted offsets are
  // not the ones rustc will use. MEASURED (FR-181 spike): `#[repr(C)]` makes
  // flac_validate byte-identical to the clang native on all 6 corpus
  // vectors, where the default repr is WRONG and SILENT -- the struct is the
  // same SIZE under both reprs, so no size check catches it, and
  // `improper_ctypes_definitions` does not fire for a raw pointer to a
  // non-`repr(C)` struct.
  //
  // Empty without `--c-abi-exports`; that is what keeps every crate golden
  // byte-identical, and it is checked, not assumed
  // (test/Driver/c-abi-exports-structs.c diffs the two roots).
  cAbiReprCStructs.clear();
  cAbiPointerWrappers.clear();
  if (options.cAbiExports) {
    llvm::StringMap<emitrust::StructDefOp> structDefsByName;
    for (auto structDefOp : moduleOp.getOps<emitrust::StructDefOp>())
      structDefsByName[structDefOp.getSymName()] = structDefOp;
    llvm::SmallVector<StringRef> worklist;
    auto noteReachable = [&](Type type) {
      Type inner = type;
      if (auto mutRef = dyn_cast<emitrust::MutRefType>(inner))
        inner = mutRef.getPointee();
      else if (auto ref = dyn_cast<emitrust::RefType>(inner))
        inner = ref.getPointee();
      while (auto arrayType = dyn_cast<emitrust::ArrayType>(inner))
        inner = arrayType.getElementType();
      if (auto structType = dyn_cast<emitrust::StructType>(inner))
        if (cAbiReprCStructs.insert(structType.getName()).second)
          worklist.push_back(structType.getName());
    };
    for (auto funcOp : moduleOp.getOps<emitrust::FuncOp>()) {
      StringAttr symbolAttr = SymbolTable::getSymbolName(funcOp);
      if (!symbolAttr)
        continue;
      StringRef symbol = symbolAttr.getValue();
      // The same three gates `emitFunc` applies: a per-TU module item is
      // translation-unit-local and never exported (FR-159), an unexported
      // item has no C symbol to give, and a requirement-generic item has no
      // single symbol at all (FR-52).
      if (!itemModulePath(symbol).empty() || itemVisibility(symbol).empty() ||
          funcOp->hasAttr(emitrust::kExternalsGenericAttrName))
        continue;
      // ...and FR-62 slice 5c's: an `async fn` has no C calling convention.
      bool isAsyncFn = false;
      funcOp->walk([&](emitrust::MethodCallOp call) {
        if (isAsyncHandleCall(call))
          isAsyncFn = true;
      });
      if (isAsyncFn)
        continue;
      CAbiVerdict verdict =
          classifyCAbiSignature(funcOp, funcOp.getFunctionType());
      if (verdict.kind == CAbiClass::Refused ||
          verdict.kind == CAbiClass::Scalar)
        continue;
      for (Type input : funcOp.getFunctionType().getInputs())
        noteReachable(input);
      for (Type result : funcOp.getFunctionType().getResults())
        noteReachable(result);
    }
    while (!worklist.empty()) {
      StringRef name = worklist.pop_back_val();
      auto found = structDefsByName.find(name);
      if (found == structDefsByName.end())
        continue;
      for (Attribute typeAttr : found->second.getFieldTypes())
        noteReachable(cast<TypeAttr>(typeAttr).getValue());
    }
  }
  // FR-62 slice 5c: the shared `mod actor_rt` epilogue exists once per
  // crate and its text is flavor-specific, so every anchor in one module
  // must agree on the mode. The driver never produces a mixed module
  // (--actor-mode is global); reaching emission with one must fail loudly
  // at the disagreeing anchor, never emit one flavor's runtime under the
  // other's wrappers.
  emitrust::ActorRuntimeOp firstAnchor;
  for (auto runtime : moduleOp.getOps<emitrust::ActorRuntimeOp>()) {
    // FR-179: record every runtime-managed owner before any function is
    // rendered -- the C-ABI wrapper decision in `emitFunc` needs it.
    actorRuntimeOwners.insert(runtime.getActor());
    if (!firstAnchor) {
      firstAnchor = runtime;
      continue;
    }
    if (runtime.getMode() != firstAnchor.getMode())
      return runtime.emitError("actor '")
             << runtime.getActor() << "' mode disagrees with actor '"
             << firstAnchor.getActor()
             << "': one module carries one actor_rt runtime flavor";
  }
  // FR-159: per-TU Rust modules. A top-level item whose SYMBOL is the
  // absolute path `crate::<module>::<leaf>` is bucketed here and rendered
  // below inside `mod <module> { .. }` under its leaf name; use sites
  // already spell the whole path, so nothing else in the emitter changes. A
  // module whose symbols carry no path leaves both containers empty and
  // takes the historical flat path, byte for byte.
  llvm::SmallVector<StringRef> modulePaths;
  llvm::StringMap<llvm::SmallVector<Operation *>> moduleItems;
  for (Operation &op : *moduleOp.getBody()) {
    if (!isa<emitrust::UseOp, emitrust::VerbatimOp, emitrust::FuncOp,
             emitrust::ImplOp, emitrust::StructDefOp, emitrust::EnumDefOp,
             emitrust::DataEnumDefOp, emitrust::GlobalOp,
             emitrust::TraitDefOp, emitrust::ActorRuntimeOp>(&op))
      return op.emitOpError("unable to translate op");
    StringAttr symbol = SymbolTable::getSymbolName(&op);
    StringRef path = symbol ? itemModulePath(symbol.getValue()) : StringRef();
    if (!path.empty()) {
      // Only the item kinds whose emitters print `itemLeafName` may be
      // bucketed. Anything else would render its FULL path where an item
      // name belongs (`fn crate::tu0::add1`), which is not Rust -- refuse
      // it here with a location rather than emit it.
      if (!isa<emitrust::FuncOp, emitrust::StructDefOp, emitrust::EnumDefOp,
               emitrust::DataEnumDefOp, emitrust::GlobalOp>(&op))
        return op.emitOpError()
               << "cannot render '" << symbol.getValue()
               << "' inside a Rust module: this item kind has no module form";
      auto [it, inserted] = moduleItems.try_emplace(path);
      if (inserted)
        modulePaths.push_back(it->first());
      it->second.push_back(&op);
      continue;
    }
    if (failed(emitOperation(op)))
      return failure();
  }
  // FR-173 D2: a module item may still NAME a crate-ROOT item, and inside
  // `mod tu<N>` a bare name resolves in the MODULE's namespace. The
  // measured case is the FR-159 sink itself: only a shape-CONFLICTING
  // record sinks, so a sunk `struct S { struct P p; }` leaves `P` at the
  // root and the emitted `pub(crate) p: P` is rustc
  // `error[E0425]: cannot find type 'P' in this scope` -- the whole crate
  // lost to a rustc error with no source location, which is the direction
  // FR-159's own escape guard exists to refuse. `use super::*;` at the head
  // of the module resolves the whole class at once, and a module item's own
  // name still wins over it (an explicit item shadows a glob import), which
  // is exactly what the sink needs.
  //
  // The import is emitted only for a module that actually REACHES a root
  // name: an unused `use super::*;` is a rustc `unused_imports` warning
  // (measured), and every module the emitter produced before FR-173 names
  // nothing at the root and so keeps its bytes exactly
  // (test/Target/Rust/module-items.mlir,
  // test/Driver/link-merge-module-sink.c,
  // test/EndToEnd/link-module-sink-e2e.c).
  llvm::StringSet<> rootItemNames;
  if (!modulePaths.empty())
    for (Operation &op : *moduleOp.getBody())
      if (StringAttr symbol = SymbolTable::getSymbolName(&op))
        if (itemModulePath(symbol.getValue()).empty())
          rootItemNames.insert(symbol.getValue());
  // Whether any item of `path` names a crate-root item that the module does
  // not itself define. The carriers are the same ones FR-159's escape
  // analysis enumerates: named TYPES (however deeply nested), symbol
  // references, and the plain-TEXT payloads (`emitrust.call_opaque`
  // callees, `emitrust.opaque` attributes, an impl's struct name) that no
  // structural rewrite can reach.
  auto moduleNeedsRootImport = [&](StringRef path) {
    llvm::StringSet<> owned;
    for (Operation *item : moduleItems[path])
      if (StringAttr symbol = SymbolTable::getSymbolName(item))
        owned.insert(itemLeafName(symbol.getValue()));
    bool needs = false;
    auto note = [&](StringRef name) {
      if (name.empty() || name.contains("::") || owned.contains(name))
        return;
      if (rootItemNames.contains(name))
        needs = true;
    };
    auto noteNamedType = [&](Type sub) {
      if (auto structType = dyn_cast<emitrust::StructType>(sub))
        note(structType.getName());
      else if (auto enumType = dyn_cast<emitrust::EnumType>(sub))
        note(enumType.getName());
      else if (auto dataEnumType = dyn_cast<emitrust::DataEnumType>(sub))
        note(dataEnumType.getName());
    };
    // Whole identifiers of a plain-text carrier, skipping any that is
    // already `::`-qualified: `Some(crate::tu0::add1)` names nothing at the
    // root even when the root happens to define an `add1`.
    auto noteText = [&](StringRef text) {
      auto isIdentChar = [](char c) { return llvm::isAlnum(c) || c == '_'; };
      size_t index = 0;
      while (index < text.size()) {
        if (!isIdentChar(text[index])) {
          ++index;
          continue;
        }
        size_t start = index;
        while (index < text.size() && isIdentChar(text[index]))
          ++index;
        bool qualified =
            start >= 2 && text[start - 1] == ':' && text[start - 2] == ':';
        if (!qualified)
          note(text.substr(start, index - start));
      }
    };
    for (Operation *item : moduleItems[path])
      item->walk([&](Operation *inner) {
        for (Type type : inner->getOperandTypes())
          type.walk(noteNamedType);
        for (Type type : inner->getResultTypes())
          type.walk(noteNamedType);
        if (auto call = dyn_cast<emitrust::CallOpaqueOp>(inner))
          noteText(call.getCallee());
        if (auto implOp = dyn_cast<emitrust::ImplOp>(inner))
          noteText(implOp.getStructName());
        for (NamedAttribute attr : inner->getAttrs()) {
          attr.getValue().walk(noteNamedType);
          attr.getValue().walk([&](Attribute sub) {
            if (auto symbol = dyn_cast<FlatSymbolRefAttr>(sub))
              note(symbol.getValue());
            else if (auto opaque = dyn_cast<emitrust::OpaqueAttr>(sub))
              noteText(opaque.getValue());
          });
        }
      });
    return needs;
  };
  // One `mod` per distinct path, as an epilogue in first-appearance order,
  // each item in the order it had at the top level.
  for (StringRef path : modulePaths) {
    os << "mod " << path << " {\n";
    increaseIndent();
    if (moduleNeedsRootImport(path))
      os << "use super::*;\n";
    llvm::SaveAndRestore moduleScope(moduleItemVisibility,
                                     StringRef("pub(crate) "));
    for (Operation *item : moduleItems[path])
      if (failed(emitOperation(*item)))
        return failure();
    decreaseIndent();
    os << "}\n";
  }
  // FR-179: the C-ABI wrappers for FR-62 actor-lifted owner methods, at
  // MODULE scope (a `#[no_mangle] extern "C"` item cannot live inside an
  // `impl`). Nothing is collected without `--c-abi-exports`, so a default
  // run reaches this with an empty list and emits not one byte.
  if (failed(emitCAbiActorWrappers()))
    return failure();
  // FR-182: and the C-ABI wrappers for exported functions taking one pointer
  // to an ABI-faithful struct, at MODULE scope for the same reason -- and,
  // like the actor wrappers, a pure APPEND, so the default (flagless) crate
  // root is reproduced byte for byte by deleting them.
  if (failed(emitCAbiPointerWrappers()))
    return failure();
  // FR-62 slice 5b: the shared actor runtime — Handle<M>, the synchronous
  // call protocol, and the reap/shutdown panic-provenance plumbing — is one
  // fixed, message-type-generic module, emitted once per crate when any
  // actor_runtime anchor exists (the same once-per-module epilogue posture
  // as the importer's __emitrust_fmt_f64 helper, but emitter-owned: the
  // anchor op is the trigger, no verbatim op carries the text). Slice 5c:
  // the anchors' shared mode picks the flavor — std::thread/mpsc for
  // threaded, tokio for async.
  if (firstAnchor)
    os << (firstAnchor.getMode() == emitrust::ActorMode::async
               ? kActorRtModuleAsync
               : kActorRtModule);
  return success();
}

LogicalResult RustEmitter::emitImpl(emitrust::ImplOp implOp) {
  // W2.17: a `trait_name` turns the block into `impl <Trait> for <Struct>`
  // (the verifier has already pinned that the trait is `Drop` and that the
  // body is exactly `fn drop(&mut self)`). Absent the attribute -- which no
  // pre-W2.17 module can carry -- this is the historical inherent header,
  // byte for byte.
  if (std::optional<StringRef> traitName = implOp.getTraitName())
    os << "impl " << *traitName << " for " << implOp.getStructName() << " {\n";
  else
    os << "impl " << implOp.getStructName() << " {\n";
  increaseIndent();
  for (Operation &op : implOp.getBody().front()) {
    if (failed(emitOperation(op)))
      return failure();
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitCAbiActorWrappers() {
  if (cAbiActorWrappers.empty())
    return success();
  // ONE singleton per OWNER, in first-appearance order, shared by every
  // wrapper on that owner: two exported methods of one C translation unit's
  // lifted statics must see the SAME state, exactly as the C file-scope
  // variables they came from did.
  //
  // `thread_local!` and not a process-global `Mutex` or a `static mut`: it
  // is the same substrate this emitter already renders every mutable C
  // file-scope global into (see `emitGlobal`), it is exact for the
  // single-threaded programs the importer accepts, and it keeps the crate
  // free of `unsafe`.
  //
  // `RefCell` makes the aliasing rule dynamic. A C callback that re-enters
  // an exported symbol while the borrow is live PANICS ("already borrowed")
  // inside an `extern "C"` frame, which aborts. That is loud and located,
  // never silent state corruption -- the repo's required safe failure
  // direction.
  SmallVector<StringRef> owners;
  llvm::StringSet<> seen;
  for (const CAbiActorWrapper &wrapper : cAbiActorWrappers)
    if (seen.insert(wrapper.owner).second)
      owners.push_back(wrapper.owner);
  for (StringRef owner : owners) {
    std::string singleton = actorSingletonName(owner);
    os << "thread_local! {\n";
    increaseIndent();
    os << "static " << singleton << ": std::cell::RefCell<" << owner
       << "> = std::cell::RefCell::new(" << owner << "::new());\n";
    decreaseIndent();
    os << "}\n";
  }
  for (const CAbiActorWrapper &wrapper : cAbiActorWrappers) {
    // FR-140: the wrapper binds the bare C name and the method's parameter
    // spellings, either of which can trip `non_snake_case`; it opens its own
    // allow-scope for the same reason the function item does.
    const size_t itemStart = openNonSnakeCaseScope();
    noteBoundName(wrapper.symbol);
    for (const std::string &argName : wrapper.argNames)
      noteBoundName(argName);
    // The closure binding cannot collide with a parameter: `__`-leading
    // spellings are reserved in C, but a name that arrived anyway would
    // otherwise pass the ACTOR where an argument belongs.
    std::string actorName = "__emitrust_actor";
    while (llvm::is_contained(wrapper.argNames, actorName))
      actorName += "_";
    std::string singleton = actorSingletonName(wrapper.owner);
    os << "#[no_mangle]\n"
       << wrapper.visibility << "extern \"C\" fn " << wrapper.symbol << "(";
    for (auto [index, argName] : llvm::enumerate(wrapper.argNames)) {
      if (index)
        os << ", ";
      os << argName << ": ";
      if (failed(emitType(wrapper.loc, wrapper.argTypes[index])))
        return failure();
    }
    os << ")";
    if (wrapper.resultType) {
      os << " -> ";
      if (failed(emitType(wrapper.loc, wrapper.resultType)))
        return failure();
    }
    os << " {\n";
    increaseIndent();
    os << singleton << ".with(|" << actorName << "| " << actorName
       << ".borrow_mut()." << wrapper.printedName << "(";
    llvm::interleaveComma(wrapper.argNames, os);
    os << "))\n";
    decreaseIndent();
    os << "}\n";
    closeNonSnakeCaseScope(itemStart);
  }
  return success();
}

LogicalResult RustEmitter::emitCAbiPointerWrappers() {
  for (const CAbiPointerWrapper &wrapper : cAbiPointerWrappers) {
    // FR-140: the wrapper binds an item name derived from the bare C symbol
    // and the function's own parameter spellings, either of which can trip
    // `non_snake_case`; it opens its own allow-scope for exactly the reason
    // the function item does.
    const size_t itemStart = openNonSnakeCaseScope();
    // The ITEM cannot be named for the C symbol: the translated function
    // already holds that name at module scope and a second item of the same
    // name is rustc E0428. `#[export_name]` supplies the BARE symbol a dlsym
    // host needs while leaving the translated function -- and every internal
    // call site -- byte for byte unchanged. The `__`-leading prefix is
    // reserved in C, so the derived spelling cannot collide with an imported
    // one.
    std::string itemName = ("__emitrust_cabi_" + wrapper.symbol).str();
    noteBoundName(itemName);
    for (const std::string &argName : wrapper.argNames)
      noteBoundName(argName);
    os << "#[export_name = \"" << wrapper.symbol << "\"]\n"
       << wrapper.visibility << "unsafe extern \"C\" fn " << itemName << "(";
    StringRef pointeeName = itemLeafName(wrapper.refStructName);
    for (auto [index, argName] : llvm::enumerate(wrapper.argNames)) {
      if (index)
        os << ", ";
      os << argName << ": ";
      if (index == wrapper.refIndex) {
        // The C side has a POINTER here; the translated function has a Rust
        // reference. Spelling the wrapper's parameter as the raw pointer is
        // what makes the two agree, and it is the only reason the wrapper
        // needs `unsafe` at all.
        os << (wrapper.refIsMut ? "*mut " : "*const ") << pointeeName;
        continue;
      }
      if (failed(emitType(wrapper.loc, wrapper.argTypes[index])))
        return failure();
    }
    os << ")";
    if (wrapper.resultType) {
      os << " -> ";
      if (failed(emitType(wrapper.loc, wrapper.resultType)))
        return failure();
    }
    os << " {\n";
    increaseIndent();
    // ONE statement. Everything the function does stays in the function,
    // which is what confines the `unsafe` to this generated item.
    os << wrapper.printedName << "(";
    for (auto [index, argName] : llvm::enumerate(wrapper.argNames)) {
      if (index)
        os << ", ";
      if (index == wrapper.refIndex)
        os << (wrapper.refIsMut ? "&mut *" : "&*");
      os << argName;
    }
    os << ")\n";
    decreaseIndent();
    os << "}\n";
    closeNonSnakeCaseScope(itemStart);
  }
  return success();
}

/// UpperCamelCases a snake_case method name for its message-variant
/// spelling: `bump` -> `Bump`, `fill_table` -> `FillTable`.
static std::string upperCamel(llvm::StringRef snake) {
  std::string out;
  bool upper = true;
  for (char ch : snake) {
    if (ch == '_') {
      upper = true;
      continue;
    }
    out += upper ? llvm::toUpper(ch) : ch;
    upper = false;
  }
  return out;
}

/// The carried parameter name of `fn`'s input `index` (the
/// `emitrust.param_names` slot), or empty when unnamed. The
/// actor_runtime verifier requires a name for every non-receiver input of
/// a runtime actor's method.
static llvm::StringRef carriedParamName(emitrust::FuncOp fn, unsigned index) {
  auto paramNames =
      fn->getAttrOfType<ArrayAttr>(emitrust::kParamNamesAttrName);
  if (paramNames && index < paramNames.size())
    if (auto slot = dyn_cast<StringAttr>(paramNames[index]))
      return slot.getValue();
  return llvm::StringRef();
}

LogicalResult RustEmitter::emitActorRuntime(emitrust::ActorRuntimeOp op) {
  // FR-62 slice 5c: mode picks the substrate — std::sync::mpsc +
  // std::thread for threaded, tokio (unbounded mailbox + oneshot replies +
  // async wrappers with immediate await) for async. The DERIVATION is
  // identical (B-prime: everything from the impl), so both flavors share
  // this function with the substrate strings switched.
  const bool isAsync = op.getMode() == emitrust::ActorMode::async;
  auto module = op->getParentOfType<ModuleOp>();
  llvm::StringRef actor = op.getActor();
  emitrust::ImplOp impl;
  for (emitrust::ImplOp candidate : module.getOps<emitrust::ImplOp>())
    // W2.17: only the INHERENT impl is the actor's message surface.
    if (candidate.getStructName() == actor && !candidate.getTraitName()) {
      impl = candidate;
      break;
    }
  if (!impl) // The verifier guarantees this; keep the failure located.
    return op.emitError("actor '") << actor << "' has no impl to derive from";
  std::string msgName = (actor + "Msg").str();
  std::string handleName = (actor + "Handle").str();

  auto methods = impl.getBody().front().getOps<emitrust::FuncOp>();
  // Emits the reply payload type: the method's result type, or `()`.
  auto emitReplyType = [&](emitrust::FuncOp fn) -> LogicalResult {
    if (fn.getNumResults() == 0) {
      os << "()";
      return success();
    }
    return emitType(fn.getLoc(), fn.getResultTypes().front());
  };
  // Emits "name: Type, " for each non-receiver parameter.
  auto emitParamFields = [&](emitrust::FuncOp fn) -> LogicalResult {
    FunctionType type = fn.getFunctionType();
    for (unsigned i = 1; i < type.getNumInputs(); ++i) {
      llvm::StringRef name = carriedParamName(fn, i);
      if (name.empty())
        return fn.emitError("actor method parameter #")
               << i << " has no name to derive its message field from";
      os << name << ": ";
      if (failed(emitType(fn.getLoc(), type.getInput(i))))
        return failure();
      os << ", ";
    }
    return success();
  };
  // Emits "name, " for each non-receiver parameter (pattern bindings and
  // shorthand struct-literal fields alike).
  auto emitParamNames = [&](emitrust::FuncOp fn) {
    FunctionType type = fn.getFunctionType();
    for (unsigned i = 1; i < type.getNumInputs(); ++i)
      os << carriedParamName(fn, i) << ", ";
  };

  // 1. The message enum: one variant per impl method, the per-call typed
  //    reply channel as the trailing field (`Sender<()>` keeps a void call
  //    synchronous and C-sequenced under the same rule, E3; the async
  //    flavor's reply channel is a tokio oneshot).
  os << "enum " << msgName << " {\n";
  for (emitrust::FuncOp fn : methods) {
    os << "    " << upperCamel(fn.getSymName()) << " { ";
    if (failed(emitParamFields(fn)))
      return failure();
    os << "reply: "
       << (isAsync ? "tokio::sync::oneshot::Sender<"
                   : "std::sync::mpsc::Sender<");
    if (failed(emitReplyType(fn)))
      return failure();
    os << "> },\n";
  }
  os << "}\n";

  // 2. The handle alias.
  os << "type " << handleName << " = actor_rt::Handle<" << msgName << ">;\n";

  // 3. The inherent impl: spawn (channel + moved state + mailbox loop, one
  //    arm per variant delegating to the existing method and replying), then
  //    one wrapper per method (fresh reply channel + self.call). The async
  //    spawn stays a plain fn: tokio::task::spawn needs only the runtime
  //    CONTEXT (the main shim's block_on), not an async caller, and an
  //    unbounded sender's send never awaits.
  os << "impl actor_rt::Handle<" << msgName << "> {\n";
  os << "    fn spawn(mut state: " << actor << ") -> Self {\n";
  if (isAsync) {
    os << "        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<"
       << msgName << ">();\n";
    os << "        let join = tokio::task::spawn(async move {\n";
    os << "            while let Some(msg) = rx.recv().await {\n";
  } else {
    os << "        let (tx, rx) = std::sync::mpsc::channel::<" << msgName
       << ">();\n";
    os << "        let join = std::thread::spawn(move || {\n";
    os << "            for msg in rx {\n";
  }
  os << "                match msg {\n";
  for (emitrust::FuncOp fn : methods) {
    os << "                    " << msgName << "::"
       << upperCamel(fn.getSymName()) << " { ";
    emitParamNames(fn);
    os << "reply } => {\n";
    // The loud-failure form differs by substrate: mpsc's SendError is
    // Debug regardless of the payload (expect works), while a tokio
    // oneshot returns the unsent value ITSELF on failure — no Debug bound
    // exists, so the async arm checks is_err and panics with the same
    // message.
    if (isAsync)
      os << "                        if reply.send(state." << fn.getSymName()
         << "(";
    else
      os << "                        reply.send(state." << fn.getSymName()
         << "(";
    bool first = true;
    for (unsigned i = 1; i < fn.getFunctionType().getNumInputs(); ++i) {
      if (!first)
        os << ", ";
      first = false;
      os << carriedParamName(fn, i);
    }
    if (isAsync) {
      os << ")).is_err() {\n";
      os << "                            panic!(\"actor caller dropped "
            "reply receiver\");\n";
      os << "                        }\n";
    } else {
      os << ")).expect(\"actor caller dropped reply receiver\");\n";
    }
    os << "                    }\n";
  }
  os << "                }\n";
  os << "            }\n";
  os << "        });\n";
  os << "        Self { tx, join: Some(join) }\n";
  os << "    }\n";
  for (emitrust::FuncOp fn : methods) {
    os << (isAsync ? "    async fn " : "    fn ") << fn.getSymName()
       << "(&mut self";
    FunctionType type = fn.getFunctionType();
    for (unsigned i = 1; i < type.getNumInputs(); ++i) {
      os << ", " << carriedParamName(fn, i) << ": ";
      if (failed(emitType(fn.getLoc(), type.getInput(i))))
        return failure();
    }
    os << ")";
    if (fn.getNumResults() == 1) {
      os << " -> ";
      if (failed(emitType(fn.getLoc(), fn.getResultTypes().front())))
        return failure();
    }
    os << " {\n";
    os << (isAsync
               ? "        let (rtx, rrx) = tokio::sync::oneshot::channel();\n"
               : "        let (rtx, rrx) = std::sync::mpsc::channel();\n");
    os << "        self.call(" << msgName << "::"
       << upperCamel(fn.getSymName()) << " { ";
    emitParamNames(fn);
    // The IMMEDIATE await is the async ordering contract (E4): the wrapper
    // suspends until the reply, so at most one message is ever in flight.
    os << "reply: rtx }, rrx)" << (isAsync ? ".await" : "") << "\n";
    os << "    }\n";
  }
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitUse(emitrust::UseOp useOp) {
  os << "use " << useOp.getPath() << ";\n";
  return success();
}

LogicalResult RustEmitter::emitVerbatim(emitrust::VerbatimOp verbatimOp) {
  // FR-150: every verbatim op in the image is one of the importer's fixed
  // `__emitrust_*` runtime helpers (ImportCFunctions.cpp is the only producer),
  // so its text is emitter-owned boilerplate -- and several of them spell
  // `String` and `Vec<u8>`, which shadow like everything else. The rewrite
  // skips string literals, so a helper's panic message and its output bytes
  // are untouched.
  os << qualifyPrelude(verbatimOp.getValue()) << "\n";
  return success();
}

/// FR-139: is `type` a type that crosses the C ABI unchanged, needing neither
/// an `unsafe` shim nor a repr promise?
///
/// Exactly the BUILTIN scalars: `mlir::IntegerType` (`i32`, `u8`, ...) and
/// `mlir::FloatType` (`f32`, `f64`). Every EmitRust dialect type is excluded
/// on purpose -- `!emitrust.slice` is a two-register fat pointer, `!emitrust.
/// struct` carries no `#[repr(C)]`, `!emitrust.opaque` is whatever the
/// importer could not model -- and so is every other builtin (an `index` has
/// no C spelling). Widening this predicate widens the FFI surface, which is
/// the one thing FR-139 must not do.
static bool isCAbiScalarType(Type type) {
  return isa<IntegerType>(type) || isa<FloatType>(type);
}

// FR-139's `hasAllScalarSignature` lived here. FR-182 SUBSUMED it: the
// all-scalar answer is `classifyCAbiSignature`'s `CAbiClass::Scalar`, which a
// zero-result (void) function and a zero-parameter one still reach vacuously
// -- `extern "C" fn f()` is a complete C signature. The receiver-dropping
// variant below stays: the FR-179 actor path is all-scalar-only by design and
// admits no struct shape.

/// FR-179: the same predicate over the C signature of an FR-62 actor-lifted
/// owner method, i.e. the method's signature with block argument 0 -- the
/// SYNTHESIZED `&mut self` receiver, which the C function never had --
/// dropped. `uint16_t float2half(float)` is all-scalar in C whether or not
/// its lookup tables became fields of an owner struct.
static bool hasAllScalarSignatureWithoutReceiver(FunctionType type) {
  if (type.getNumInputs() < 1)
    return false;
  return llvm::all_of(type.getInputs().drop_front(), isCAbiScalarType) &&
         llvm::all_of(type.getResults(), isCAbiScalarType);
}

/// FR-179: true when `implOp` holds the zero-argument associated `fn new()`
/// the FR-62 lift synthesizes for an EXPORTED owner. Checked, never assumed:
/// a non-exported actor's impl has no constructor at all, and a wrapper that
/// named one would be a rustc error in the emitted crate.
static bool ownerHasNullaryNew(emitrust::ImplOp implOp) {
  if (!implOp)
    return false;
  for (auto fn : implOp.getBody().front().getOps<emitrust::FuncOp>()) {
    StringAttr name = SymbolTable::getSymbolName(fn);
    if (!name || name.getValue() != "new")
      continue;
    if (!fn->hasAttr(emitrust::kStaticMethodAttrName))
      continue;
    FunctionType type = fn.getFunctionType();
    if (type.getNumInputs() == 0 && type.getNumResults() == 1)
      return true;
  }
  return false;
}

LogicalResult RustEmitter::emitFunc(emitrust::FuncOp funcOp) {
  Operation *op = funcOp.getOperation();
  auto fn = cast<FunctionOpInterface>(op);

  // FR-140: a function is the item that carries the allow for its OWN name,
  // its parameters and its locals -- all three are `non_snake_case` sites and
  // rustc honours the attribute on the fn for all of them. Which locals exist
  // is only known once the body has been walked, so the item is rendered into
  // the emitter's buffer first and the attribute inserted ahead of it after
  // the fact; deciding up front would mean guessing, and a guess that says
  // "yes" too often is a byte shift in a golden.
  const size_t itemStart = openNonSnakeCaseScope();

  // Each function opens a fresh value-naming scope: v0, v1, ...
  valueNames.clear();
  valueCount = 0;
  usedBindingNames.clear();
  // Reserved spellings never claimed by assignName's direct-map paths.
  usedBindingNames.insert("self");
  usedBindingNames.insert("__emitrust_tl");
  unreachableOps.clear();
  valueReadCache.clear();
  deferredInits.clear();
  deadStores.clear();
  ifExprBindings.clear();
  consumedIfs.clear();
  tailReturn = nullptr;
  tailFoldCandidate = nullptr;
  pendingTailFold = false;
  tailFoldActive = false;
  inlinedOps.clear();
  inlineExprs.clear();
  inlineTreeReadsPlace.clear();
  droppedOps.clear();
  itemAllowsUnusedAssign = false;
  pendingInlineCapture = false;
  fieldInitFuses.clear();
  fusedAssignOwner.clear();
  lateInitMerges.clear();
  lateInitOwner.clear();
  unwrappedFnPtrs.clear();

  Region &body = fn.getFunctionBody();
  if (body.empty())
    return op->emitOpError("cannot translate a function without a body");
  if (!body.hasOneBlock())
    return op->emitOpError("multi-block regions are not supported");
  computeUnreachable(body.front());
  computeDeadStores(body.front());
  computeDeferredInits(body.front());
  computeIfExprBindings();
  if (fn.getNumResults() > 1)
    return op->emitOpError(
        "cannot translate a function with more than one result");

  Block &entryBlock = body.front();
  // FR-61a: find the entry block's last EMITTED op -- the walk stops after
  // the first diverging op, so this is either that op or the block's last.
  // When it is a return, render it as a tail expression / omit it (void);
  // when additionally the returned value's defining op immediately precedes
  // the return, is a single-result let-producing op, and the value's only
  // use is the return, fold the binding away entirely.
  Operation *lastEmitted = nullptr;
  for (Operation &bodyOp : entryBlock) {
    lastEmitted = &bodyOp;
    if (opDiverges(&bodyOp))
      break;
  }
  if (auto finalReturn = dyn_cast_or_null<emitrust::ReturnOp>(lastEmitted)) {
    tailReturn = finalReturn;
    if (finalReturn->getNumOperands() == 1) {
      Value returned = finalReturn->getOperand(0);
      Operation *def = returned.getDefiningOp();
      if (def && def == finalReturn->getPrevNode() && returned.hasOneUse() &&
          def->getNumResults() == 1 && isTailFoldableProducer(def)) {
        tailFoldCandidate = def;
      } else if (def && ifExprBindings.count(def)) {
        // FR-61d slice 3: an if-expression binding whose consumed `if`
        // immediately precedes the tail return, and whose only READ is
        // that return (its other uses are exactly the two arm assignments
        // the if-expression rendering consumes), folds too: the function
        // body ends in the bare if-expression.
        emitrust::IfOp ifOp = ifExprBindings.lookup(def);
        if (def->getNextNode() == ifOp.getOperation() &&
            ifOp->getNextNode() == finalReturn) {
          bool returnIsOnlyRead = true;
          for (OpOperand &use : returned.getUses()) {
            Operation *owner = use.getOwner();
            if (isBindingWrite(owner, returned))
              continue; // an arm assignment, consumed by the rendering
            if (owner != finalReturn) {
              returnIsOnlyRead = false;
              break;
            }
          }
          if (returnIsOnlyRead)
            tailFoldCandidate = def;
        }
      }
    }
  }
  // FR-61d: both run after the tail-fold candidate is known so the
  // mechanisms stay disjoint; drops run first so a dropped consumer's
  // operands can inline into (or drop with) the survivors.
  computeDroppedOps(funcOp);
  computeInlineCandidates(funcOp);
  // FR-63 (field_reassign_with_default): after the inline/drop sets — the
  // fuse's statement scan skips exactly the ops those passes silenced.
  computeFieldInitFuses(funcOp);
  // FR-132 (clippy::needless_late_init): LAST -- the gap scan asks which ops
  // render, so every set that silences an op must already be populated, and
  // `ifExprBindings` must already have claimed the bindings it folds better.
  computeLateInitMerges(funcOp);
  // FR-133: after every set that decides how a binding renders -- the
  // candidate must be a DEFERRED declaration, and its initializing store must
  // be one that actually renders.
  computeUnwrappedFnPtrs(funcOp);
  // FR-106: decided LAST, so every set that silences a store (`deadStores`,
  // `droppedOps`, `deferredInits`, the late-init merges) is already
  // populated and a store that never renders cannot be counted. The answer
  // is applied to the ITEM after its body is rendered, because the attribute
  // goes on the `fn` -- that is where rustc honours it.
  itemAllowsUnusedAssign = uaFunctionAtRisk(op);
  // A function directly inside an `emitrust.impl` is a method, UNLESS it
  // carries the `static_method` marker (W2.2), in which case it is a
  // receiverless associated function (`Struct::name(...)`) and every
  // argument renders like an ordinary parameter. A genuine (receiver-
  // having) method's first argument is the receiver, named `self` and
  // rendered as `&mut self` or `&self` depending on whether the impl
  // verifier-checked receiver type is `!emitrust.mut_ref` (mutating,
  // historical behavior) or `!emitrust.ref` (const, W2.2). The existing
  // deref place rendering then yields `(*self).field...` naturally either
  // way.
  bool isMethod = isa<emitrust::ImplOp>(op->getParentOp()) &&
                  !op->hasAttr(emitrust::kStaticMethodAttrName);
  // W2.17: a TRAIT impl's member never takes a visibility prefix -- measured
  // rustc "error[E0449]: visibility qualifiers are not permitted here". Only
  // export mode emits one at all, so this cannot move a binary crate's bytes.
  auto parentImpl = dyn_cast<emitrust::ImplOp>(op->getParentOp());
  bool inTraitImpl = parentImpl && parentImpl.getTraitName().has_value();
  StringRef symbol = SymbolTable::getSymbolName(op).getValue();
  // FR-62 slice 5c: a function that method_calls an ASYNC actor handle
  // (the driver of an async-mode actor — the pass guarantees handles never
  // escape the constructing driver) renders `async fn`: its handle calls
  // all carry `.await`, which needs the async context. Everything else is
  // byte-identical to the pre-async rendering.
  bool isAsyncFn = false;
  op->walk([&](emitrust::MethodCallOp call) {
    if (isAsyncHandleCall(call))
      isAsyncFn = true;
  });
  // FR-110: a C++ method member prints its in-impl spelling (`fn get`, not
  // `fn box_i32_get`) while KEEPING the mangled symbol in the IR --
  // visibility, the mutability query, and FR-52's symbol namespace all
  // still key on `symbol`. Attribute-keyed, never prefix-parsed (a Phase-4
  // C owner method shares `method_of` but its symbol was never
  // struct-prefixed).
  // FR-159: an item rendered inside `mod tu<N>` prints its LEAF name; the
  // symbol keeps the whole path, so every use site, every lookup and every
  // linkage query above still sees `crate::tu0::helper`.
  StringRef printedName = itemLeafName(symbol);
  if (auto rustName =
          op->getAttrOfType<StringAttr>(emitrust::kMethodRustNameAttrName))
    printedName = rustName.getValue();
  // FR-140: the function's own name. A file-static's FR-73 per-TU prefix
  // composes onto the C spelling (`tu0_mix__up`), so a name that was clean
  // in C can still arrive here tripping.
  noteBoundName(printedName);
  // FR-139: an EXPORTED function whose whole signature is C-ABI scalar is
  // additionally given a bare C symbol, so a dlopen/dlsym host can call it
  // through an ordinary C declaration. Everything else keeps the plain
  // `pub fn` it has always had -- and says why, because the alternative
  // failure mode is a dlsym that returns null in somebody else's harness
  // with no explanation anywhere. A warning rather than an error: a crate
  // holds many functions and typically only one of them is the target.
  //
  // The blockers below are not stylistic. `extern "C"` on a `&[T]` parameter
  // compiles with only a non-FFI-safe warning and then MISCOMPILES across
  // the boundary (FR-138, measured). `#[no_mangle]` on a generic item has no
  // single symbol to name. `extern "C" async fn` is not Rust at all.
  bool cAbiExport = false;
  // FR-179: set when this function is an FR-62 actor-lifted C owner method
  // whose C signature (the receiver dropped) is all-scalar. The method
  // itself keeps the `pub fn ...(&mut self, ...)` it has always had, byte
  // for byte; a module-scope `#[no_mangle] extern "C"` wrapper delegating
  // through a per-owner thread_local singleton is emitted for it instead.
  bool cAbiActorWrapper = false;
  // FR-182: set when this function's signature is scalars plus exactly ONE
  // reference to an ABI-faithful struct (class 1). The function itself keeps
  // the `pub fn f(t: &mut T, ...)` it has always had, byte for byte; a
  // module-scope `#[export_name] unsafe extern "C"` wrapper taking `*mut T`
  // and delegating in one statement is emitted for it instead.
  bool cAbiPointerWrapper = false;
  // FR-182: the signature's classification, and the storage its per-shape
  // refusal wording lives in (`blocker` is a StringRef).
  CAbiVerdict cAbiVerdict;
  std::string cAbiBlockerStorage;
  // FR-179: the discriminator between the two kinds of impl member that
  // reach here, attribute-keyed and never prefix-parsed (see FR-110 above).
  // A genuine C++ member carries `emitrust.method_rust_name`; a receiverless
  // C++ static member fails `isMethod`; an imported destructor lands in a
  // TRAIT impl and fails `!inTraitImpl`. What is left is exactly the
  // Phase-4 C owner method, whose symbol is still the bare C name -- the
  // one case for which exporting that name is what the C source said.
  bool isCOwnerMethod =
      isMethod && !op->hasAttr(emitrust::kMethodRustNameAttrName);
  // FR-159: never for an item inside a per-TU module. Such an item is
  // translation-unit-local by construction, so a `#[no_mangle] extern "C"`
  // symbol for it would export a name C never had.
  if (options.cAbiExports && !inTraitImpl && moduleItemVisibility.empty() &&
      !itemVisibility(symbol).empty()) {
    StringRef blocker;
    if (isAsyncFn)
      blocker = "it is an async fn, which has no C calling convention";
    else if (op->hasAttr(emitrust::kExternalsGenericAttrName))
      blocker = "it is generic over the external-requirements trait (FR-52), "
                "so it has no single symbol to name";
    else if (isCOwnerMethod) {
      // FR-179: the FR-62 lift moved this C function onto an owner struct.
      // The C signature it had is the method's minus the synthesized
      // receiver, and refusing the export for the receiver alone would be a
      // defect: the lifted STATE is real (the body reads and writes it), so
      // "do not lift it" is not the alternative -- a singleton the wrapper
      // borrows is.
      if (!hasAllScalarSignatureWithoutReceiver(funcOp.getFunctionType()))
        blocker = "its signature is not all-scalar (a C-ABI entry point may "
                  "only take and return builtin integer and floating-point "
                  "types)";
      else if (actorRuntimeOwners.contains(parentImpl.getStructName()))
        blocker = "its FR-62 owner struct is managed by an actor runtime, "
                  "whose single instance lives in the spawned mailbox loop; "
                  "a C-ABI singleton beside it would be a second copy of the "
                  "state";
      else if (!ownerHasNullaryNew(parentImpl))
        blocker = "its FR-62 owner struct has no zero-argument new(), so a "
                  "C-ABI singleton has nothing to construct from";
      else
        cAbiActorWrapper = true;
    } else {
      // FR-182 widens FR-139's all-scalar gate by exactly TWO struct shapes
      // and refuses everything else PER SHAPE, in that shape's own words.
      cAbiVerdict = classifyCAbiSignature(op, funcOp.getFunctionType());
      // FR-182's two STRUCT classes are for MODULE-SCOPE free functions
      // only. Inside an `emitrust.impl` the first argument is a synthesized
      // receiver and not a C parameter at all, the emitted symbol is not a
      // name C ever had, and a delegating wrapper would have no module-scope
      // callee to name; the one impl member that does carry a real C
      // signature is the FR-62 C owner method, and it took the actor path
      // above. Such a member keeps FR-139's wording -- which is exactly what
      // it got before FR-182 (pinned for `box_step` by
      // test/Driver/c-abi-exports-actor-cxx.cpp).
      if (isa<emitrust::ImplOp>(op->getParentOp()) &&
          cAbiVerdict.kind != CAbiClass::Scalar)
        cAbiVerdict = {CAbiClass::Refused, kCAbiNotAllScalarBlocker.str()};
      if (cAbiVerdict.kind == CAbiClass::Refused) {
        cAbiBlockerStorage = cAbiVerdict.blocker;
        blocker = cAbiBlockerStorage;
      }
    }
    if (blocker.empty() && !cAbiActorWrapper) {
      // Class 1 gets a wrapper and NOT the C ABI on the function itself:
      // its parameter is a Rust reference where C has a pointer.
      cAbiPointerWrapper = cAbiVerdict.kind == CAbiClass::StructPointer;
      cAbiExport = !cAbiPointerWrapper;
    } else if (!blocker.empty())
      // On the LOCATION, not the op: this is a message for the person who
      // wrote the C, and attaching it to the operation makes MLIR dump the
      // whole `emitrust.func` after it.
      mlir::emitWarning(op->getLoc())
          << "--c-abi-exports: no C-ABI export for '" << symbol
          << "': " << blocker
          << "; it stays a plain 'pub fn' and is not reachable by dlsym";
  }
  if (cAbiExport)
    os << "#[no_mangle]\n";
  os << (inTraitImpl ? StringRef("") : itemVisibility(symbol))
     << (cAbiExport ? "extern \"C\" " : "")
     << (isAsyncFn ? "async fn " : "fn ") << printedName;
  // FR-52: a function in the transitive closure of a caller of an external
  // requirement is generic over the requirement trait. Everything else keeps
  // the signature it always had, so a project with no requirements is
  // byte-identical.
  if (auto generic =
          op->getAttrOfType<StringAttr>(emitrust::kExternalsGenericAttrName))
    os << "<" << emitrust::kExternalsTypeParam << ": " << generic.getValue()
       << ">";
  os << "(";
  // FR-61e slice 2: a function carrying `emitrust.param_names` binds each
  // non-receiver argument under its slot's spelling through the same
  // uniquifier as named locals (arguments are named first, so a same-named
  // body local uniquifies to `<name>_1`, never the parameter). Empty
  // slots -- unnamed C parameters, by-value shadows whose variable already
  // took the name, cursor inputs -- keep the generated vN.
  auto paramNames =
      op->getAttrOfType<ArrayAttr>(emitrust::kParamNamesAttrName);
  // FR-179: the wrapper's parameters must be spelled exactly like the
  // method's, so they are captured here rather than re-derived later.
  SmallVector<std::string> wrapperArgNames;
  SmallVector<Type> wrapperArgTypes;
  bool first = true;
  for (BlockArgument argument : entryBlock.getArguments()) {
    if (!first)
      os << ", ";
    first = false;
    if (isMethod && argument.getArgNumber() == 0) {
      // The receiver is hard-wired `self`; its slot, if any, is ignored.
      valueNames[argument] = "self";
      os << (isa<emitrust::RefType>(argument.getType()) ? "&self"
                                                         : "&mut self");
      continue;
    }
    StringRef carried;
    if (paramNames && argument.getArgNumber() < paramNames.size())
      if (auto slot =
              dyn_cast<StringAttr>(paramNames[argument.getArgNumber()]))
        carried = slot.getValue();
    std::string argName =
        carried.empty() ? assignName(argument) : claimName(argument, carried);
    os << argName << ": ";
    if (failed(emitType(argument.getLoc(), argument.getType())))
      return failure();
    if (cAbiActorWrapper || cAbiPointerWrapper) {
      wrapperArgNames.push_back(std::move(argName));
      wrapperArgTypes.push_back(argument.getType());
    }
  }
  os << ")";
  if (fn.getNumResults() == 1) {
    os << " -> ";
    if (failed(emitType(op->getLoc(), fn.getResultTypes().front())))
      return failure();
  }
  if (cAbiActorWrapper)
    cAbiActorWrappers.push_back(
        {op->getLoc(), parentImpl.getStructName(), symbol, printedName,
         itemVisibility(symbol), std::move(wrapperArgNames),
         std::move(wrapperArgTypes),
         fn.getNumResults() == 1 ? fn.getResultTypes().front() : Type()});
  if (cAbiPointerWrapper)
    cAbiPointerWrappers.push_back(
        {op->getLoc(), symbol, printedName, itemVisibility(symbol),
         std::move(wrapperArgNames), std::move(wrapperArgTypes),
         fn.getNumResults() == 1 ? fn.getResultTypes().front() : Type(),
         cAbiVerdict.refIndex, cAbiVerdict.refIsMut,
         cAbiVerdict.refStructName});
  os << " {\n";
  increaseIndent();
  if (failed(emitBlockBody(entryBlock)))
    return failure();
  decreaseIndent();
  os << "}\n";
  if (itemAllowsUnusedAssign) {
    // FR-106 reuses FR-140's insertion mechanism: render the item, then
    // prefix it at its OWN indentation, so a method inside an
    // `emitrust.impl` lines up with its `fn`. Inserting before
    // `closeNonSnakeCaseScope` keeps a stacked pair in a fixed order
    // (`non_snake_case` first).
    itemAllowsUnusedAssign = false;
    os.flush();
    StringRef item = StringRef(buffer).substr(itemStart);
    size_t indent = item.size() - item.ltrim(' ').size();
    buffer.insert(itemStart,
                  std::string(indent, ' ') + "#[allow(unused_assignments)]\n");
  }
  closeNonSnakeCaseScope(itemStart);
  return success();
}

LogicalResult RustEmitter::emitReturn(emitrust::ReturnOp returnOp) {
  Operation *op = returnOp.getOperation();
  // FR-61a: the function-final return renders expression-oriented: a void
  // `return;` is omitted, `return v;` becomes the tail expression `v`, and a
  // folded single-use binding contributes its right-hand side directly.
  if (op == tailReturn) {
    if (op->getNumOperands() == 0)
      return success();
    if (tailFoldActive) {
      tailFoldActive = false;
      os.flush();
      // The candidate emitted `<rhs>;\n` with its `let` prologue suppressed;
      // removing the `;` leaves the right-hand side as the tail expression.
      assert(StringRef(buffer).ends_with(";\n") &&
             "tail-fold candidate must end its statement with ';'");
      buffer.erase(buffer.size() - 2, 1);
      return success();
    }
    if (failed(emitOperand(op->getLoc(), op->getOperand(0), ExprPos::stmt())))
      return failure();
    os << "\n";
    return success();
  }
  // Non-final returns are unrepresentable today (`emitrust.return` is a
  // terminator constrained to `emitrust.func`), but the statement rendering
  // is kept so a future dialect relaxation cannot silently emit nothing.
  if (op->getNumOperands() == 0) {
    os << "return;\n";
    return success();
  }
  os << "return ";
  if (failed(emitOperand(op->getLoc(), op->getOperand(0), ExprPos::stmt())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitCallOpaque(emitrust::CallOpaqueOp callOp) {
  Operation *op = callOp.getOperation();
  // FR-63 (clippy::assertions_on_constants): a null guard whose flag folded
  // to literal `true` can never fire; emit nothing. Read-tracking already
  // treats this statement as never-emitted (see `isVacuousAssert`), so a
  // true constant read only by dropped asserts leaves no orphaned `let`.
  if (isVacuousAssert(op))
    return success();
  Location loc = op->getLoc();
  unsigned numResults = op->getNumResults();

  if (numResults == 1) {
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
  } else if (numResults > 1) {
    os << "let (";
    bool first = true;
    for (Value result : op->getResults()) {
      if (!first)
        os << ", ";
      first = false;
      os << assignName(result);
    }
    os << "): (";
    first = true;
    for (Value result : op->getResults()) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitType(loc, result.getType())))
        return failure();
    }
    os << ") = ";
  }

  // FR-110: a qualified static-method call carries the importer's
  // mangled-on-both-sides `"<Struct>::<mangled>"` spelling (the methods.cpp
  // RED-pinned scheme); print the right half's in-impl spelling instead
  // (`BoxI32::origin(`), and ONLY when the left half is that member's own
  // impl -- `println!`/`assert!`, `Struct::default`, and a foreign-struct
  // qualification are not two-part mapped spellings and pass through
  // verbatim.
  StringRef callee = callOp.getCallee();
  auto [qualifier, memberSymbol] = callee.split("::");
  auto mappedMember = methodRustNames.find(memberSymbol);
  if (!memberSymbol.empty() && mappedMember != methodRustNames.end() &&
      mappedMember->second.first == qualifier)
    os << qualifier << "::" << mappedMember->second.second << "(";
  else if (shadowedPreludeNames.contains(qualifier) &&
           emitrust::isEmitterPreludePath(callee))
    // FR-150: `Box::new` / `String::from` / `Vec::new` and friends -- the
    // closed set of associated paths the importer and emitter mint for the
    // prelude families. The check is on the WHOLE callee, not just its first
    // segment, because a USER static method prints through this very branch:
    // `Node::default()` sits beside `Box::new(..)` in every unique_ptr crate,
    // and a C++ class named `box` keeps its own `Box::<method>` calls.
    os << emitrust::preludeQualifiedPath(qualifier) << "::" << memberSymbol
       << "(";
  else
    os << callee << "(";
  bool first = true;
  if (std::optional<ArrayAttr> args = callOp.getArgs()) {
    for (Attribute arg : *args) {
      if (!first)
        os << ", ";
      first = false;
      auto intAttr = dyn_cast<IntegerAttr>(arg);
      if (intAttr && isa<IndexType>(intAttr.getType())) {
        if (failed(emitOperand(loc, op->getOperand(intAttr.getInt()),
                               ExprPos::delimited())))
          return failure();
      } else if (auto strAttr = dyn_cast<StringAttr>(arg)) {
        emitEscapedStringLiteral(strAttr.getValue());
      } else if (failed(emitAttribute(loc, arg))) {
        return failure();
      }
    }
  } else {
    for (Value operand : op->getOperands()) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitOperand(loc, operand, ExprPos::delimited())))
        return failure();
    }
  }
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitCallIndirect(emitrust::CallIndirectOp callOp) {
  Operation *op = callOp.getOperation();
  Location loc = op->getLoc();
  if (op->getNumResults() == 1 &&
      failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  // Calling a null C function pointer is undefined behavior; the expect
  // refines it into a deterministic panic. FR-133: a callee proved to hold a
  // literal `Some` carries no `Option` at all, so there is nothing to unwrap
  // and no null to refine.
  bool unwrapped = unwrappedFnPtrs.contains(callOp.getCallee());
  if (failed(emitOperand(loc, callOp.getCallee(), ExprPos::receiver())))
    return failure();
  if (!unwrapped)
    os << ".expect(\"null function pointer\")";
  os << "(";
  bool first = true;
  for (Value argument : callOp.getArgs()) {
    if (!first)
      os << ", ";
    first = false;
    if (failed(emitOperand(loc, argument, ExprPos::delimited())))
      return failure();
  }
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitMethodCall(emitrust::MethodCallOp callOp) {
  Operation *op = callOp.getOperation();
  Location loc = op->getLoc();
  if (op->getNumResults() == 1 &&
      failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  // The receiver is followed by `.method(..)`, so a deref receiver needs parens.
  Value receiver = callOp.getReceiver();
  if (auto derefOp = receiver.getDefiningOp<emitrust::DerefOp>()) {
    // clippy::explicit_auto_deref: (*x).method() -> x.method(). The deref
    // operand is always a reference (EmitRustOps.td), so Deref coercion re-adds
    // the deref.
    if (failed(emitOperand(loc, derefOp.getOperand(), ExprPos::receiver())))
      return failure();
  } else {
    if (failed(emitPlaceExpr(loc, receiver, /*derefNeedsParens=*/true)))
      return failure();
  }
  // FR-110: a call whose method attribute is a mapped C++ member symbol
  // prints the member's in-impl spelling (`.get(`, not `.box_i32_get(`).
  // Builtin spellings and C owner methods are never map keys and pass
  // through verbatim; the IR keeps the mangled symbol either way.
  StringRef method = callOp.getMethod();
  auto strippedMethod = methodRustNames.find(method);
  os << "."
     << (strippedMethod == methodRustNames.end() ? method
                                                 : strippedMethod->second.second)
     << "(";
  bool first = true;
  for (Value argument : callOp.getArgs()) {
    if (!first)
      os << ", ";
    first = false;
    if (failed(emitOperand(loc, argument, ExprPos::delimited())))
      return failure();
  }
  os << ")";
  // FR-62 slice 5c: a call on an async actor handle awaits IMMEDIATELY —
  // the wrapper (and the consuming shutdown) is an `async fn`, and the
  // instant await keeps at most one message in flight, so effect order
  // equals program order on the current_thread runtime (E4).
  if (isAsyncHandleCall(callOp))
    os << ".await";
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitConstant(emitrust::ConstantOp constantOp) {
  Operation *op = constantOp.getOperation();
  Value result = op->getResult(0);
  if (failed(emitLetPrologue(result, /*isMut=*/false)))
    return failure();
  // FR-133: the initializer of an unwrapped fn-ptr local renders as the bare
  // function item -- `Some(addc)` would not fit the `fn(..)` type the binding
  // now has. `computeUnwrappedFnPtrs` admitted this constant only after
  // `fnPtrConstantTargetIdent` matched, so the payload is always there.
  if (unwrappedFnPtrs.contains(result)) {
    os << fnPtrConstantTargetIdent(constantOp.getValue()) << ";\n";
    return success();
  }
  if (failed(emitAttribute(op->getLoc(), constantOp.getValue())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitLiteral(emitrust::LiteralOp literalOp) {
  Operation *op = literalOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << literalOp.getValue() << ";\n";
  return success();
}

LogicalResult RustEmitter::emitEnumVariant(emitrust::EnumVariantOp variantOp) {
  Operation *op = variantOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  emitrust::DataEnumDefOp def =
      emitrust::DataEnumDefOp::lookupFrom(op, variantOp.getEnumDef());
  if (!def)
    return op->emitOpError("requires a visible emitrust.data_enum_def");
  os << variantOp.getEnumDef() << "::" << variantOp.getVariant();
  if (!variantOp.getArgs().empty()) {
    // Field names come from the definition (the verifier pinned the
    // variant's existence and the operand arity/types). Field values sit
    // between `{ }` delimited by commas — a complete expression position
    // like a call argument, so inlined operands never parenthesize.
    ArrayAttr fieldNames =
        def.variantFieldNames(*def.variantIndex(variantOp.getVariant()));
    os << " { ";
    bool first = true;
    for (auto [fieldNameAttr, argument] :
         llvm::zip_equal(fieldNames, variantOp.getArgs())) {
      if (!first)
        os << ", ";
      first = false;
      os << cast<StringAttr>(fieldNameAttr).getValue() << ": ";
      if (failed(emitOperand(loc, argument, ExprPos::delimited())))
        return failure();
    }
    os << " }";
  }
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitDeferredBinding(Operation *op, Value result,
                                               Type type) {
  // FR-61b: a deferred binding whose immediately following `if` assigns it
  // exactly once at the end of both arms renders as an if-expression binding
  // instead of the `let x: T;` + statement-`if` pair.
  if (auto ifOp = ifExprBindings.lookup(op))
    return emitIfExprBinding(op, result, type, ifOp);
  // FR-132: the declaration of a merged binding renders NOTHING -- its
  // initializing assign renders the whole `let`. The name is assigned
  // unconditionally so v-numbering stays stable even when the binding folds
  // away.
  if (lateInitMerges.count(op)) {
    assignName(result);
    return success();
  }
  os << "let ";
  if (deferredInits.lookup(op))
    os << "mut ";
  os << assignName(result) << ": ";
  if (failed(emitBindingType(result.getLoc(), result, type)))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitIfExprBinding(Operation *op, Value result,
                                             Type valueType,
                                             emitrust::IfOp ifOp) {
  (void)op;
  // The name is assigned unconditionally so v-numbering stays stable even
  // when the binding folds away. FR-140: when it does fold away it never
  // renders, so it must not pull an `#[allow(non_snake_case)]` onto the fn.
  llvm::SaveAndRestore<bool> notes(noteBoundNames, !pendingTailFold);
  std::string name = assignName(result);
  // FR-61d slice 3: when this binding is the tail-fold candidate the `let`
  // prefix never renders -- the if-expression itself becomes the
  // function's tail (the statement's trailing `;` is erased by the tail
  // return, exactly like the FR-61a fold).
  if (pendingTailFold) {
    pendingTailFold = false;
    tailFoldActive = true;
  } else {
    os << "let " << name << ": ";
    if (failed(emitBindingType(result.getLoc(), result, valueType)))
      return failure();
    os << " = ";
  }
  os << "if ";
  if (failed(emitOperand(ifOp.getLoc(), ifOp.getCondition(),
                         ExprPos::cond())))
    return failure();
  os << " {\n";
  if (failed(emitArmBodyWithTail(ifOp.getThenRegion(), result)))
    return failure();
  os << "} else {\n";
  if (failed(emitArmBodyWithTail(ifOp.getElseRegion(), result)))
    return failure();
  os << "};\n";
  return success();
}

LogicalResult RustEmitter::emitArmBodyWithTail(Region &region, Value binding) {
  increaseIndent();
  for (Operation &op : region.front()) {
    // The arm's final assignment (its only one, per `computeIfExprBindings`):
    // its right-hand side is the arm's tail expression.
    if (isBindingWrite(&op, binding)) {
      auto assign = cast<emitrust::AssignOp>(&op);
      if (failed(emitOperand(assign.getLoc(), assign.getValue(),
                             ExprPos::stmt())))
        return failure();
      os << "\n";
      break;
    }
    // FR-61d slice 3: arm-local candidates route through the shared
    // drop/capture prelude, so they inline exactly like entry-block ops
    // (the inlined arm tail then renders through the never-parens Stmt
    // position above).
    FailureOr<bool> consumed = emitDropOrCapture(op);
    if (failed(consumed))
      return failure();
    if (*consumed)
      continue;
    if (failed(emitOperation(op)))
      return failure();
    if (opDiverges(&op))
      break;
  }
  decreaseIndent();
  return success();
}

/// Returns whether any EMITTED `emitrust.assign` writes directly to `value`
/// (uses inside unreachable code or dropped dead stores never render). A
/// `emitrust.let` result is a plain SSA value (never an lvalue place), so the
/// only way to mutate it is a direct assignment; this suffices to decide
/// whether the emitted `let` needs `mut`, avoiding a spurious `unused_mut`
/// on the default-initialized SSA-destruction lets the SCF lowering marks
/// mutable up front (before the arm assignments that may or may not exist).
bool RustEmitter::letHasEmittedAssign(Value value) {
  for (Operation *user : value.getUsers())
    if (isBindingWrite(user, value) && !unreachableOps.count(user) &&
        !deadStores.count(user))
      return true;
  return false;
}

LogicalResult RustEmitter::emitLet(emitrust::LetOp letOp) {
  Operation *op = letOp.getOperation();
  Value result = op->getResult(0);
  // A dead initializer is dropped: the binding is written on every path before
  // it is read, so Rust's definite-assignment lets us defer initialization and
  // avoid the `unused_assignments` the overwritten init would draw.
  if (deferredInits.count(op))
    return emitDeferredBinding(op, result, result.getType());
  bool isMut = letOp.getIsMut() && letHasEmittedAssign(result);
  if (failed(emitLetPrologue(result, isMut)))
    return failure();
  if (failed(emitOperand(op->getLoc(), op->getOperand(0), ExprPos::stmt())))
    return failure();
  os << ";\n";
  return success();
}

/// Returns the Rust compound-assignment operator (`+=`, `-=`, ...) for a
/// self-referential integer binary op eligible for `clippy::assign_op_pattern`
/// folding, or an empty `StringRef` if the op is not foldable. `+`/`-`/`*` are
/// excluded on unsigned types: there the emitter renders the `.wrapping_*`
/// method form (see `emitWrappingBinary`) and `+=` would silently change the
/// overflow semantics. `/ % & | ^ << >>` are always infix on every integer
/// type, so they fold unconditionally.
static StringRef compoundAssignSymbol(Operation *op) {
  auto isUnsignedResult = [&]() {
    auto intType = dyn_cast<IntegerType>(op->getResult(0).getType());
    return intType && intType.isUnsigned();
  };
  return TypeSwitch<Operation *, StringRef>(op)
      .Case<emitrust::AddOp>([&](auto) { return isUnsignedResult() ? "" : "+="; })
      .Case<emitrust::SubOp>([&](auto) { return isUnsignedResult() ? "" : "-="; })
      .Case<emitrust::MulOp>([&](auto) { return isUnsignedResult() ? "" : "*="; })
      .Case<emitrust::DivOp>([&](auto) -> StringRef { return "/="; })
      .Case<emitrust::RemOp>([&](auto) -> StringRef { return "%="; })
      .Case<emitrust::AndOp>([&](auto) -> StringRef { return "&="; })
      .Case<emitrust::OrOp>([&](auto) -> StringRef { return "|="; })
      .Case<emitrust::XorOp>([&](auto) -> StringRef { return "^="; })
      .Case<emitrust::ShlOp>([&](auto) -> StringRef { return "<<="; })
      .Case<emitrust::ShrOp>([&](auto) -> StringRef { return ">>="; })
      .Default([](Operation *) { return StringRef(); });
}

bool RustEmitter::isPureRenderedValue(Value value) {
  // Outside `inlineExprs` the value renders as its binding's name (the
  // binding statement itself still emits, and the fold's kept render keeps
  // the name alive), so one dropped occurrence cannot change behaviour.
  if (!inlineExprs.count(value))
    return true;
  Operation *def = value.getDefiningOp();
  if (!def)
    return true; // block arguments render as names
  if (isa<emitrust::ConstantOp>(def))
    return true;
  if (isa<emitrust::CastOp>(def))
    return isPureRenderedValue(def->getOperand(0));
  if (auto load = dyn_cast<emitrust::LoadOp>(def))
    return isPureRenderedPlace(load.getOperand());
  return false; // not provably effect-free: refuse by default
}

bool RustEmitter::isPureRenderedPlace(Value place) {
  while (true) {
    Operation *def = place.getDefiningOp();
    if (!def)
      return false; // block-argument places never render (emitPlaceExpr errs)
    if (isa<emitrust::VariableOp>(def))
      return true; // root: renders its name
    if (auto member = dyn_cast<emitrust::MemberOp>(def)) {
      place = member.getOperand();
      continue;
    }
    if (auto enumRaw = dyn_cast<emitrust::EnumRawOp>(def)) {
      place = enumRaw.getOperand();
      continue;
    }
    if (auto subscript = dyn_cast<emitrust::SubscriptOp>(def)) {
      if (!isPureRenderedValue(subscript.getIndex()))
        return false;
      place = subscript.getArray();
      continue;
    }
    if (auto deref = dyn_cast<emitrust::DerefOp>(def))
      return isPureRenderedValue(deref.getOperand()); // ref base ends chain
    return false; // unknown place producer: refuse by default
  }
}

bool RustEmitter::isSamePureReadValue(Value kept, Value dropped) {
  if (kept == dropped)
    return isPureRenderedValue(kept);
  if (kept.getType() != dropped.getType())
    return false;
  Operation *keptDef = kept.getDefiningOp();
  Operation *droppedDef = dropped.getDefiningOp();
  if (!keptDef || !droppedDef)
    return false; // distinct block arguments are distinct values
  // Equal constants are the same value at ANY program point; the dropped
  // one must still render inline, or its orphaned `let` would draw an
  // unused-variable diagnostic once the fold removes its only use.
  auto keptConst = dyn_cast<emitrust::ConstantOp>(keptDef);
  auto droppedConst = dyn_cast<emitrust::ConstantOp>(droppedDef);
  if (keptConst || droppedConst)
    return keptConst && droppedConst &&
           keptConst.getValue() == droppedConst.getValue() &&
           inlineExprs.count(dropped);
  // Everything below re-reads state when its text renders, so BOTH sides
  // must render inline in the statement being emitted -- a side hoisted to
  // an earlier `let` captured an earlier value.
  if (!inlineExprs.count(kept) || !inlineExprs.count(dropped))
    return false;
  if (isa<emitrust::CastOp>(keptDef) && isa<emitrust::CastOp>(droppedDef))
    return isSamePureReadValue(keptDef->getOperand(0),
                               droppedDef->getOperand(0));
  if (isa<emitrust::LoadOp>(keptDef) && isa<emitrust::LoadOp>(droppedDef))
    return isSamePureReadPlace(keptDef->getOperand(0),
                               droppedDef->getOperand(0));
  return false; // unknown producer pair: refuse by default
}

bool RustEmitter::isSamePureReadPlace(Value kept, Value dropped) {
  if (kept == dropped)
    return isPureRenderedPlace(kept);
  Operation *keptDef = kept.getDefiningOp();
  Operation *droppedDef = dropped.getDefiningOp();
  if (!keptDef || !droppedDef)
    return false;
  auto keptMember = dyn_cast<emitrust::MemberOp>(keptDef);
  auto droppedMember = dyn_cast<emitrust::MemberOp>(droppedDef);
  if (keptMember || droppedMember)
    return keptMember && droppedMember &&
           keptMember.getMember() == droppedMember.getMember() &&
           isSamePureReadPlace(keptMember.getOperand(),
                               droppedMember.getOperand());
  auto keptRaw = dyn_cast<emitrust::EnumRawOp>(keptDef);
  auto droppedRaw = dyn_cast<emitrust::EnumRawOp>(droppedDef);
  if (keptRaw || droppedRaw)
    return keptRaw && droppedRaw &&
           isSamePureReadPlace(keptRaw.getOperand(), droppedRaw.getOperand());
  auto keptSub = dyn_cast<emitrust::SubscriptOp>(keptDef);
  auto droppedSub = dyn_cast<emitrust::SubscriptOp>(droppedDef);
  if (keptSub || droppedSub)
    return keptSub && droppedSub &&
           isSamePureReadValue(keptSub.getIndex(), droppedSub.getIndex()) &&
           isSamePureReadPlace(keptSub.getArray(), droppedSub.getArray());
  auto keptDeref = dyn_cast<emitrust::DerefOp>(keptDef);
  auto droppedDeref = dyn_cast<emitrust::DerefOp>(droppedDef);
  if (keptDeref || droppedDeref)
    return keptDeref && droppedDeref &&
           isSamePureReadValue(keptDeref.getOperand(),
                               droppedDeref.getOperand());
  // Distinct `emitrust.variable` roots (or unknown producers) never match.
  return false;
}

LogicalResult RustEmitter::emitAssign(emitrust::AssignOp assignOp) {
  Operation *op = assignOp.getOperation();
  // A dead store emits nothing: its written value is never read.
  if (deadStores.count(op))
    return success();
  // FR-63 (clippy::field_reassign_with_default): a fused field store is a
  // literal field, not a statement; the fuse's LAST assign renders the whole
  // `let` for its variable (see `computeFieldInitFuses`).
  if (Operation *owner = fusedAssignOwner.lookup(op)) {
    auto fuse = fieldInitFuses.find(owner);
    if (op != fuse->second.back().getOperation())
      return success();
    return emitFusedFieldInit(cast<emitrust::VariableOp>(owner));
  }
  // FR-132 (clippy::needless_late_init): this assign is the initializing write
  // of a deferred binding whose declaration rendered nothing, so it renders the
  // whole `let [mut] x: T = rhs;` here (see `computeLateInitMerges`).
  if (Operation *declOp = lateInitOwner.lookup(op))
    return emitMergedLateInit(declOp, assignOp);
  Location loc = op->getLoc();
  Value var = assignOp.getVar();

  // FR-63: fold a self-referential compound assignment `v = v <op> e` into the
  // Rust compound-assign operator `v <op>= e` (clippy::assign_op_pattern).
  // Conservative gate (single evaluation, no double-compute):
  //   1. the RHS renders inline here (a value hoisted to its own `let` would be
  //      double-computed if folded), so it must be in `inlineExprs`;
  //   2. the RHS op is a foldable integer binary op (`compoundAssignSymbol`);
  //   3. the RHS's first operand is the target's current value. Two shapes:
  //      - SSA binding: the operand IS the target SSA value (`i = i + 1`);
  //      - PLACE target (FR-61f bare variable, FR-63 projection): the operand
  //        is an `emitrust.load` of the SAME place, rendering INLINE (a load
  //        bound to its own `let vN` -- e.g. hoisted before a method-call
  //        barrier -- must NOT fold, or dropping the `<op>=` operand orphans
  //        that binding: rustc unused-variable error, not a miscompile,
  //        caught here). The importer materializes the LHS place and the RHS
  //        load's place as SEPARATE SSA chains, so SSA identity is not
  //        required: `isSamePureReadPlace` accepts structurally identical,
  //        provably PURE projection chains (member/enum_raw/subscript/deref
  //        down to a root `emitrust.variable`, every index/deref-base operand
  //        a constant, a name, or a pure inline twin). Today such a place
  //        renders twice (LHS place + RHS load); the fold renders it once --
  //        legal only because re-rendering was effect-free, so dropping one
  //        render cannot change behaviour. When purity or identity is
  //        unprovable (a call in an index, differing indices, a hoisted
  //        operand), the assign keeps its `p = p <op> e` form.
  // `v = v + e` and `v += e` compute the same value for such a side-effect-free
  // target (identical overflow behaviour on the signed/infix path -- unsigned
  // `+ - *` render `.wrapping_*` and are excluded upstream), so stdout is
  // unchanged and the byte-diff oracle stays green.
  {
    Value val = assignOp.getValue();
    Operation *binOp = val.getDefiningOp();
    // FR-63 (assign_op_pattern residue): the lost-copy cycle breaker wraps
    // every rotated back-edge value in a pass-through alias
    // (`%t = emitrust.let %rhs`) before the backedge assign, so `binOp` is
    // the LetOp and the identity checks below never fired. When the target
    // is an SSA binding and BOTH the alias and its initializer render
    // inline in this statement (the alias's own `let` never emits -- its
    // text IS the initializer's text), look through the alias chain.
    // Restricted to the SSA-binding leg: place targets keep the exact
    // load-identity gate pinned by compound-assign-place/-projection.mlir.
    // A zero-shift initializer is refused: it renders its lhs ALONE
    // (`v = v;`), so "folding" it would resurrect the dropped amount as
    // `v <<= 0`.
    if (!isa<emitrust::LValueType>(var.getType())) {
      while (auto alias = dyn_cast_if_present<emitrust::LetOp>(binOp)) {
        Value init = alias.getInit();
        Operation *initDef = init.getDefiningOp();
        if (!initDef || !inlineExprs.count(val) || !inlineExprs.count(init) ||
            zeroShiftLhs(initDef))
          break;
        val = init;
        binOp = initDef;
      }
    }
    if (binOp && inlineExprs.count(val)) {
      StringRef sym = compoundAssignSymbol(binOp);
      bool selfRef = false;
      // When set, the target is operand ONE and the fold renders operand
      // ZERO as the compound-assign right-hand side (`a = e + a` -> `a += e`).
      bool foldOperandZero = false;
      if (!sym.empty()) {
        if (!isa<emitrust::LValueType>(var.getType())) {
          selfRef = binOp->getOperand(0) == var;
          // FR-63 (assign_op_pattern residue), commutative leg: clippy also
          // flags `a = e + a`. Fold only when the op is commutative with an
          // identical infix rendering on INTEGERS (`+ * & | ^`; unsigned
          // `+`/`*` already returned an empty symbol above via the
          // wrapping-method exclusion, floats are excluded because swapping
          // addition/multiplication operands is NaN-payload-order sensitive)
          // and operand 0 is provably pure to reorder against the read of
          // the target (a name, a literal, or a pure inline load/cast chain;
          // anything the purity walker cannot prove keeps the plain form).
          // `a = a + e` and `a += e` both evaluate `e`, read `a`, add, and
          // store -- only the operand order of the commutative op differs.
          if (!selfRef && binOp->getOperand(1) == var &&
              isa<IntegerType>(binOp->getResult(0).getType()) &&
              isa<emitrust::AddOp, emitrust::MulOp, emitrust::AndOp,
                  emitrust::OrOp, emitrust::XorOp>(binOp) &&
              isPureRenderedValue(binOp->getOperand(0))) {
            selfRef = true;
            foldOperandZero = true;
          }
        } else {
          Value op0val = binOp->getOperand(0);
          Operation *op0 = op0val.getDefiningOp();
          selfRef = op0 && isa<emitrust::LoadOp>(op0) &&
                    inlineExprs.count(op0val) &&
                    isSamePureReadPlace(var, op0->getOperand(0));
        }
      }
      if (selfRef) {
        if (isa<emitrust::LValueType>(var.getType())) {
          if (failed(emitPlaceExpr(loc, var, /*derefNeedsParens=*/false)))
            return failure();
        } else if (failed(emitOperand(loc, var, ExprPos::stmt()))) {
          return failure();
        }
        os << " " << sym << " ";
        if (failed(emitOperand(loc, binOp->getOperand(foldOperandZero ? 0 : 1),
                               ExprPos::stmt())))
          return failure();
        os << ";\n";
        return success();
      }
    }
  }

  if (isa<emitrust::LValueType>(var.getType())) {
    // Whole-place assignment target: a bare `*p = ..` needs no parens.
    if (failed(emitPlaceExpr(loc, var, /*derefNeedsParens=*/false)))
      return failure();
  } else if (failed(emitOperand(loc, var, ExprPos::stmt()))) {
    // Assign targets are mut bindings, which never inline: name rendering.
    return failure();
  }
  os << " = ";
  if (failed(emitOperand(loc, assignOp.getValue(), ExprPos::stmt())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitBinary(Operation *op, StringRef symbol) {
  // FR-63 (clippy::identity_op): a shift by the literal 0 is the identity
  // on every integer type; render the lhs ALONE. The right-hand side is
  // the never-parenthesized Stmt position, so the lhs's text renders
  // exactly as it would at its own statement, and `capturedPrec` /
  // `capturedEndsInCast` inherit the lhs's classification so consumers
  // parenthesize the text that was ACTUALLY emitted. Spelling only: the
  // computed value is bit-identical, and the dropped amount is excluded
  // from read-tracking (`isDroppedZeroShiftAmount`) so it cannot orphan a
  // binding.
  if (Value lhs = zeroShiftLhs(op)) {
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    if (failed(emitOperand(op->getLoc(), lhs, ExprPos::stmt())))
      return failure();
    os << ";\n";
    return success();
  }
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  Prec rank = infixPrec(symbol);
  if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                         ExprPos::binLhs(rank))))
    return failure();
  os << " " << symbol << " ";
  if (failed(emitOperand(op->getLoc(), op->getOperand(1),
                         ExprPos::binRhs(rank))))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitBinaryMethod(Operation *op, StringRef method) {
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                         ExprPos::receiver())))
    return failure();
  os << "." << method << "(";
  if (failed(emitOperand(op->getLoc(), op->getOperand(1),
                         ExprPos::delimited())))
    return failure();
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitWrappingBinary(Operation *op, StringRef symbol,
                                              StringRef method) {
  auto intType = dyn_cast<IntegerType>(op->getResult(0).getType());
  if (intType && intType.isUnsigned())
    return emitBinaryMethod(op, method);
  return emitBinary(op, symbol);
}

/// Returns the Rust operator spelling for `predicate`, or an empty string if
/// the predicate is unknown.
static StringRef cmpPredicateSymbol(emitrust::CmpPredicate predicate) {
  switch (predicate) {
  case emitrust::CmpPredicate::eq:
    return "==";
  case emitrust::CmpPredicate::ne:
    return "!=";
  case emitrust::CmpPredicate::lt:
    return "<";
  case emitrust::CmpPredicate::le:
    return "<=";
  case emitrust::CmpPredicate::gt:
    return ">";
  case emitrust::CmpPredicate::ge:
    return ">=";
  }
  return "";
}

/// FR-63 (clippy::bool_comparison): the logically complementary predicate
/// (`!(a PRED b)` == `a INV(PRED) b` -- exact only where
/// `cmpPredicateInvertsExactly` says so).
static emitrust::CmpPredicate invertCmpPredicate(
    emitrust::CmpPredicate predicate) {
  switch (predicate) {
  case emitrust::CmpPredicate::eq:
    return emitrust::CmpPredicate::ne;
  case emitrust::CmpPredicate::ne:
    return emitrust::CmpPredicate::eq;
  case emitrust::CmpPredicate::lt:
    return emitrust::CmpPredicate::ge;
  case emitrust::CmpPredicate::le:
    return emitrust::CmpPredicate::gt;
  case emitrust::CmpPredicate::gt:
    return emitrust::CmpPredicate::le;
  case emitrust::CmpPredicate::ge:
    return emitrust::CmpPredicate::lt;
  }
  llvm_unreachable("unknown comparison predicate");
}

/// Whether inverting `cmp`'s predicate renders the exact complement.
/// eq/ne are complementary for EVERY operand type (IEEE included: a NaN
/// makes `==` false and `!=` true), the order predicates only over totally
/// ordered integer/index operands (`!(a < b)` is not `a >= b` when a NaN
/// is involved).
static bool cmpPredicateInvertsExactly(emitrust::CmpOp cmp) {
  switch (cmp.getPredicate()) {
  case emitrust::CmpPredicate::eq:
  case emitrust::CmpPredicate::ne:
    return true;
  default:
    break;
  }
  return isa<IntegerType, IndexType>(cmp.getLhs().getType());
}

/// The literal value of a bool (i1) `emitrust.constant` defining `value`,
/// or nullopt for anything else.
static std::optional<bool> boolConstantValue(Value value) {
  auto constant = value.getDefiningOp<emitrust::ConstantOp>();
  if (!constant)
    return std::nullopt;
  auto intAttr = dyn_cast<IntegerAttr>(constant.getValue());
  if (!intAttr || !intAttr.getType().isInteger(1))
    return std::nullopt;
  return intAttr.getValue().getBoolValue();
}

RustEmitter::BoolCmpShape RustEmitter::boolCmpShape(emitrust::CmpOp cmpOp,
                                                    Value &operand) {
  emitrust::CmpPredicate predicate = cmpOp.getPredicate();
  bool isEq = predicate == emitrust::CmpPredicate::eq;
  bool isNe = predicate == emitrust::CmpPredicate::ne;
  if (!isEq && !isNe)
    return BoolCmpShape::None;
  // Fn-pointer comparisons render through `emitFnPtrCmp`; keep the
  // classifier aligned with `emitCmp`'s routing.
  if (isa<emitrust::FnPtrType>(cmpOp.getLhs().getType()) ||
      isa<emitrust::FnPtrType>(cmpOp.getRhs().getType()))
    return BoolCmpShape::None;
  std::optional<bool> lhsLit = boolConstantValue(cmpOp.getLhs());
  std::optional<bool> rhsLit = boolConstantValue(cmpOp.getRhs());
  // Exactly ONE side must be the literal: two literals are a constant-fold
  // opportunity, not a spelling change (out of scope), and none is an
  // ordinary comparison.
  if (lhsLit.has_value() == rhsLit.has_value())
    return BoolCmpShape::None;
  bool literal = lhsLit ? *lhsLit : *rhsLit;
  operand = lhsLit ? cmpOp.getRhs() : cmpOp.getLhs();
  // (eq,true)/(ne,false) keep the operand's truth value; the other two
  // pairings negate it.
  if (isEq == literal)
    return BoolCmpShape::Identity;
  if (!inlineExprs.count(operand))
    return BoolCmpShape::NegateName;
  auto inner = operand.getDefiningOp<emitrust::CmpOp>();
  if (!inner || !cmpPredicateInvertsExactly(inner))
    return BoolCmpShape::Unfolded;
  // An inner comparison that is ITSELF a bool-literal fold captured a
  // non-infix text (`!x` or a bare operand); inverting its predicate would
  // not describe that text. Conservative non-fold.
  Value ignored;
  if (boolCmpShape(inner, ignored) != BoolCmpShape::None)
    return BoolCmpShape::Unfolded;
  return BoolCmpShape::InvertInner;
}

/// Lowers an equality/inequality comparison whose operands are `Option<fn..>`
/// function pointers. A literal `==`/`!=` would trip
/// `unpredictable_function_pointer_comparisons`, so:
///   - `fp == NULL` / `fp != NULL` (one side the `None` constant) becomes
///     `fp.is_none()` / `fp.is_some()`;
///   - `NULL == NULL` folds to the static truth value;
///   - `fp == gp` (neither side null) becomes a `None`-aware `match` that
///     compares the payload addresses with `core::ptr::fn_addr_eq`.
LogicalResult RustEmitter::emitFnPtrCmp(emitrust::CmpOp cmpOp, Value lhs,
                                        Value rhs, bool isEq) {
  Operation *op = cmpOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  bool lhsNone = isFnPtrNone(lhs);
  bool rhsNone = isFnPtrNone(rhs);
  if (lhsNone != rhsNone) {
    // One side is the null constant: an Option null-test is exact and warning
    // free, matching C's `fp == NULL` / `fp != NULL` semantics.
    if (failed(emitOperand(loc, lhsNone ? rhs : lhs, ExprPos::receiver())))
      return failure();
    os << (isEq ? ".is_none()" : ".is_some()") << ";\n";
    return success();
  }
  if (lhsNone) { // rhsNone too: lhsNone == rhsNone was established above
    // Both sides are the null constant; the result is statically known.
    os << (isEq ? "true" : "false") << ";\n";
    return success();
  }
  // Neither side is null: compare payload function addresses, `None`-aware.
  os << "match (";
  if (failed(emitOperand(loc, lhs, ExprPos::delimited())))
    return failure();
  os << ", ";
  if (failed(emitOperand(loc, rhs, ExprPos::delimited())))
    return failure();
  os << ") { (Some(l), Some(r)) => "
     << (isEq ? "core::ptr::fn_addr_eq(l, r)" : "!core::ptr::fn_addr_eq(l, r)")
     << ", (None, None) => " << (isEq ? "true" : "false") << ", _ => "
     << (isEq ? "false" : "true") << " };\n";
  return success();
}

LogicalResult RustEmitter::emitCmp(emitrust::CmpOp cmpOp) {
  emitrust::CmpPredicate predicate = cmpOp.getPredicate();
  Operation *op = cmpOp.getOperation();
  bool isEq = predicate == emitrust::CmpPredicate::eq;
  bool isNe = predicate == emitrust::CmpPredicate::ne;
  if ((isEq || isNe) &&
      (isa<emitrust::FnPtrType>(op->getOperand(0).getType()) ||
       isa<emitrust::FnPtrType>(op->getOperand(1).getType())))
    return emitFnPtrCmp(cmpOp, op->getOperand(0), op->getOperand(1), isEq);
  // FR-63 (clippy::bool_comparison): a comparison against a literal bool
  // constant renders the idiomatic identity/negation instead of the
  // literal infix form. Spelling only -- every shape computes exactly what
  // the infix comparison did.
  Location loc = op->getLoc();
  Value boolOperand;
  switch (boolCmpShape(cmpOp, boolOperand)) {
  case BoolCmpShape::Identity:
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    // The whole right-hand side: the never-parenthesized Stmt position, so
    // the operand's text renders exactly as it did at its own statement.
    if (failed(emitOperand(loc, boolOperand, ExprPos::stmt())))
      return failure();
    os << ";\n";
    return success();
  case BoolCmpShape::NegateName:
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    os << "!";
    // The shape's precondition is a by-name rendering (an atom, never
    // parenthesized); the Receiver rank still guards the `!` binding
    // should that precondition ever loosen.
    if (failed(emitOperand(loc, boolOperand, ExprPos::receiver())))
      return failure();
    os << ";\n";
    return success();
  case BoolCmpShape::InvertInner: {
    auto inner = cast<emitrust::CmpOp>(boolOperand.getDefiningOp());
    StringRef symbol =
        cmpPredicateSymbol(invertCmpPredicate(inner.getPredicate()));
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    // Re-render the inner comparison's operands at the same infix
    // positions `emitBinary` gave them, under the inverted operator: the
    // text is byte-identical to the inner capture except for the symbol.
    if (failed(emitOperand(loc, inner.getLhs(),
                           ExprPos::binLhs(Prec::Compare))))
      return failure();
    os << " " << symbol << " ";
    if (failed(emitOperand(loc, inner.getRhs(),
                           ExprPos::binRhs(Prec::Compare))))
      return failure();
    os << ";\n";
    return success();
  }
  case BoolCmpShape::None:
  case BoolCmpShape::Unfolded:
    break; // the ordinary infix rendering below
  }
  StringRef symbol = cmpPredicateSymbol(predicate);
  if (symbol.empty())
    return op->emitOpError("unknown comparison predicate");
  return emitBinary(op, symbol);
}

LogicalResult RustEmitter::emitCast(emitrust::CastOp castOp) {
  Operation *op = castOp.getOperation();
  // Rust has no `as bool`; a boolean-producing conversion must be modeled
  // as a comparison instead of a cast.
  if (op->getResult(0).getType().isInteger(1))
    return op->emitOpError("cannot translate a cast to bool");
  // Integer-to-enum: the open-enum tuple struct is constructed around the
  // source converted to the enum's storage type, preserving the value
  // exactly as C's conversion to the underlying type does.
  if (auto enumType = dyn_cast<emitrust::EnumType>(op->getResult(0).getType())) {
    // FR-113: resolved in the enclosing module's table (EnumDefOp::
    // lookupFrom) -- `emitrust.impl` is a SymbolTable, so the previous
    // nearest-table lookup could never see a module-level enum_def from
    // inside a C++ method and hard-errored on every cast-to-enum there.
    auto enumDef =
        emitrust::EnumDefOp::lookupFrom(op, enumType.getName());
    if (!enumDef)
      return op->emitOpError("cast to enum type ")
             << enumType << " requires a visible emitrust.enum_def";
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    os << enumType.getName() << "(";
    // FR-63 (clippy::unnecessary_cast): the constructor argument converts
    // to the raw field's type; when the operand already renders at exactly
    // that type the ` as u32/i32` is the identity and drops -- the operand
    // then sits bare between the call parens (Delimited, which never
    // parenthesizes), so an infix operand also sheds its cast-source
    // parens: `Temp((v - 4i32) as i32)` -> `Temp(v - 4i32)`. Any width or
    // signedness conversion keeps the cast; the ctor's Postfix/no-tail
    // classification is unchanged either way.
    if (rendersAsEnumUnderlying(enumDef, op->getOperand(0).getType())) {
      if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                             ExprPos::delimited())))
        return failure();
      os << ");\n";
      return success();
    }
    if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                           ExprPos::castSource())))
      return failure();
    os << " as " << enumStorageSpelling(enumDef) << ");\n";
    return success();
  }
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  // An enum-typed source gets `.0` appended (postfix binds tighter than
  // `as`), so it needs the stricter receiver parenthesization.
  bool enumSource = isa<emitrust::EnumType>(op->getOperand(0).getType());
  // FR-63 (clippy::unnecessary_cast): an identity ` as T` tail drops. The
  // enum source keeps its `.0` raw read (the field IS the target type);
  // any other source renders exactly as it would at its own statement
  // (Stmt never parenthesizes), and `capturedPrec`/`capturedEndsInCast`
  // inherit its classification so consumers parenthesize the text that
  // was ACTUALLY emitted. Spelling only: the operand is still read, so no
  // binding is orphaned and read-tracking is untouched.
  if (isIdentityCastTail(castOp)) {
    if (enumSource) {
      if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                             ExprPos::receiver())))
        return failure();
      os << ".0";
    } else if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                                  ExprPos::stmt()))) {
      return failure();
    }
    os << ";\n";
    return success();
  }
  if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                         enumSource ? ExprPos::receiver()
                                    : ExprPos::castSource())))
    return failure();
  // Enum-to-integer: the raw value is read out of the tuple struct's only
  // field before the `as` conversion.
  if (enumSource)
    os << ".0";
  os << " as ";
  if (failed(emitType(op->getLoc(), op->getResult(0).getType())))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitBitcast(emitrust::BitcastOp bitcastOp) {
  Operation *op = bitcastOp.getOperation();
  Location loc = op->getLoc();
  Type resultType = op->getResult(0).getType();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (auto floatType = dyn_cast<FloatType>(resultType)) {
    // Integer-to-float: `fW::from_bits` takes `uW`, so a signless source
    // first converts (same-width `as`, bit-preserving) to unsigned.
    auto intType = cast<IntegerType>(op->getOperand(0).getType());
    os << (floatType.isF32() ? "f32" : "f64") << "::from_bits(";
    // A signless source gets ` as uW` appended: cast source; an unsigned
    // one sits bare between the call parens: delimited.
    if (failed(emitOperand(loc, op->getOperand(0),
                           intType.isUnsigned() ? ExprPos::delimited()
                                                : ExprPos::castSource())))
      return failure();
    if (!intType.isUnsigned())
      os << " as u" << intType.getWidth();
    os << ");\n";
    return success();
  }
  // Float-to-integer: `to_bits` yields `uW`; a signless result converts
  // from it (same-width `as`, bit-preserving).
  auto intType = cast<IntegerType>(resultType);
  if (failed(emitOperand(loc, op->getOperand(0), ExprPos::receiver())))
    return failure();
  os << ".to_bits()";
  if (!intType.isUnsigned())
    os << " as i" << intType.getWidth();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitSelect(emitrust::SelectOp selectOp) {
  Operation *op = selectOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << "if ";
  if (failed(emitOperand(loc, selectOp.getCondition(), ExprPos::cond())))
    return failure();
  os << " { ";
  if (failed(emitOperand(loc, selectOp.getTrueValue(), ExprPos::stmt())))
    return failure();
  os << " } else { ";
  if (failed(emitOperand(loc, selectOp.getFalseValue(), ExprPos::stmt())))
    return failure();
  os << " };\n";
  return success();
}

LogicalResult RustEmitter::emitIf(emitrust::IfOp ifOp) {
  Operation *op = ifOp.getOperation();
  // FR-61b: an `if` consumed into an if-expression binding was already
  // rendered by `emitIfExprBinding`.
  if (consumedIfs.count(op))
    return success();
  os << "if ";
  if (failed(emitOperand(op->getLoc(), op->getOperand(0), ExprPos::cond())))
    return failure();
  os << " {\n";
  if (failed(emitRegionBody(op, ifOp.getThenRegion())))
    return failure();
  Region &elseRegion = ifOp.getElseRegion();
  if (!elseRegion.empty()) {
    os << "} else {\n";
    if (failed(emitRegionBody(op, elseRegion)))
      return failure();
  }
  os << "}\n";
  return success();
}

/// FR-61f: whether `value` is the integer constant one -- the step of the
/// overwhelmingly common C counting loop (`i++`), rendered as a bare range
/// with no `.step_by`.
static bool isConstantIntOne(Value value) {
  auto constant = value.getDefiningOp<emitrust::ConstantOp>();
  if (!constant)
    return false;
  auto intAttr = dyn_cast<IntegerAttr>(constant.getValue());
  return intAttr && intAttr.getValue().isOne();
}

LogicalResult RustEmitter::emitFor(emitrust::ForOp forOp) {
  Operation *op = forOp.getOperation();
  Location loc = op->getLoc();

  Region &region = op->getRegion(0);
  if (region.empty())
    return op->emitOpError("expected a non-empty loop body region");
  if (!region.hasOneBlock())
    return op->emitOpError("multi-block regions are not supported");
  Block &body = region.front();
  if (body.getNumArguments() != 1)
    return op->emitOpError(
        "expected a single induction-variable block argument");

  // The induction variable is named when the loop is emitted, i.e. after all
  // values defined above the loop and before the loop body's results.
  std::string induction = assignName(body.getArgument(0));
  // FR-61f: render the range head idiomatically. Bounds and step flow through
  // `emitOperand`, so a constant / single-use pure bound inlines into the head
  // (`for i in 0..n`) instead of forcing a `let vN =` above the loop. A unit
  // step (`i++`) drops `.step_by` entirely and the range needs no wrapping
  // parens; a non-unit step keeps `(lo..hi).step_by(k as usize)`. Range `..`
  // is Rust's lowest-precedence operator, so a `delimited` bound never needs
  // parens (`0..n + 1` parses as `0..(n + 1)`).
  bool unitStep = isConstantIntOne(op->getOperand(2));
  os << "for " << induction << " in ";
  if (!unitStep)
    os << "(";
  if (failed(emitOperand(loc, op->getOperand(0), ExprPos::delimited())))
    return failure();
  // FR-61f widening: the `inclusive` attribute renders Rust's inclusive
  // range (`..=`), the image of C's ascending `i <= HI` loop.
  os << (forOp.getInclusive() ? "..=" : "..");
  if (failed(emitOperand(loc, op->getOperand(1), ExprPos::delimited())))
    return failure();
  if (!unitStep) {
    os << ").step_by(";
    if (failed(emitOperand(loc, op->getOperand(2), ExprPos::delimited())))
      return failure();
    os << " as usize)";
  }
  os << " {\n";
  increaseIndent();
  // FR-61d slice 3: body-local candidates route through the shared
  // drop/capture prelude, so they inline exactly like entry-block ops.
  // (The induction variable was named above, before any body op.)
  for (Operation &child : body) {
    FailureOr<bool> consumed = emitDropOrCapture(child);
    if (failed(consumed))
      return failure();
    if (*consumed)
      continue;
    if (failed(emitOperation(child)))
      return failure();
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitLoop(emitrust::LoopOp loopOp) {
  os << "loop {\n";
  if (failed(emitRegionBody(loopOp.getOperation(), loopOp.getRegion())))
    return failure();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitWhile(emitrust::WhileOp whileOp) {
  // FR-61c: the condition region is a pure single-use chain the FR-61d
  // capture machinery folds into the head expression -- every op must be
  // consumed (captured or dropped); a leftover statement cannot render
  // inside a `while` head and fails loudly instead of emitting wrong code.
  Block &conditionBlock = whileOp.getCondition().front();
  // Consume the condition chain FIRST: each capture erases its statement
  // and leaves the stream at line start, so the `while ` printed after
  // them gets its indentation exactly once.
  for (Operation &op : conditionBlock.without_terminator()) {
    if (isPlaceProjection(&op))
      continue;
    FailureOr<bool> consumed = emitDropOrCapture(op);
    if (failed(consumed))
      return failure();
    if (!*consumed)
      return op.emitOpError("emitrust.while condition op does not fold into "
                            "the head expression");
  }
  os << "while ";
  auto conditionOp =
      cast<emitrust::ConditionOp>(conditionBlock.getTerminator());
  // The head is the never-parenthesized condition position.
  if (failed(emitOperand(conditionOp.getLoc(), conditionOp.getCondition(),
                         ExprPos::cond())))
    return failure();
  os << " {\n";
  if (failed(emitRegionBody(whileOp.getOperation(), whileOp.getBody())))
    return failure();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitSwitch(emitrust::SwitchOp switchOp) {
  Operation *op = switchOp.getOperation();
  os << "match ";
  if (failed(emitOperand(op->getLoc(), switchOp.getDiscriminator(),
                         ExprPos::cond())))
    return failure();
  os << " {\n";
  increaseIndent();
  Type discriminatorType = switchOp.getDiscriminator().getType();
  for (auto [value, region] :
       llvm::zip(switchOp.getCases(), switchOp.getCaseRegions())) {
    // The cases attribute stores the raw 64-bit case pattern; render it
    // interpreted in the discriminator's Rust type so the literal arm
    // compares equal at runtime. A usize (index) or unsigned discriminator
    // prints the low bits as unsigned decimal (a C `case -1:` on `long`
    // reaches a usize scrutinee as the bit pattern 2^64 - 1); a signed iN
    // discriminator prints the sign-interpreted value of its width.
    if (isa<IndexType>(discriminatorType)) {
      os << static_cast<uint64_t>(value);
    } else {
      auto intType = cast<IntegerType>(discriminatorType);
      unsigned width = intType.getWidth();
      if (intType.isUnsigned())
        os << (static_cast<uint64_t>(value) &
               llvm::maskTrailingOnes<uint64_t>(width));
      else
        os << llvm::SignExtend64(value, width);
    }
    os << " => {\n";
    if (failed(emitRegionBody(op, region)))
      return failure();
    os << "}\n";
  }
  os << "_ => {\n";
  if (failed(emitRegionBody(op, switchOp.getDefaultRegion())))
    return failure();
  os << "}\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitMatch(emitrust::MatchOp matchOp) {
  Operation *op = matchOp.getOperation();
  Location loc = op->getLoc();
  bool hasResult = op->getNumResults() == 1;
  // Result mode is a let-producing expression statement, so it routes
  // through the shared prologue: `let vN: T = match ... };` — which is
  // exactly the shape the FR-61a tail fold suppresses into a bare tail
  // `match` expression.
  if (hasResult &&
      failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  auto enumType = cast<emitrust::DataEnumType>(matchOp.getScrutinee().getType());
  emitrust::DataEnumDefOp def =
      emitrust::DataEnumDefOp::lookupFrom(op, enumType.getName());
  if (!def)
    return op->emitOpError("scrutinee type ")
           << enumType << " requires a visible emitrust.data_enum_def";
  os << "match ";
  if (failed(emitOperand(loc, matchOp.getScrutinee(), ExprPos::cond())))
    return failure();
  os << " {\n";
  increaseIndent();
  for (auto [index, region] : llvm::enumerate(matchOp.getCaseRegions())) {
    StringRef variantName =
        cast<StringAttr>(matchOp.getVariants()[index]).getValue();
    Block &block = region.front();
    os << enumType.getName() << "::" << variantName;
    if (block.getNumArguments() != 0) {
      // One binding per payload field, in field order; `assignName`'s
      // never-read `_` prefixing keeps an unused binding lint-clean.
      ArrayAttr fieldNames = def.variantFieldNames(index);
      os << " { ";
      bool first = true;
      for (auto [fieldNameAttr, argument] :
           llvm::zip_equal(fieldNames, block.getArguments())) {
        if (!first)
          os << ", ";
        first = false;
        os << cast<StringAttr>(fieldNameAttr).getValue() << ": "
           << assignName(argument);
      }
      os << " }";
    }
    os << " => {\n";
    if (!hasResult) {
      if (failed(emitRegionBody(op, region)))
        return failure();
      os << "}\n";
      continue;
    }
    // Result mode: the arm body renders like an entry block (drops and
    // single-use captures included) until the terminator, whose yielded
    // value renders as the arm's tail expression in the never-parenthesized
    // statement position.
    increaseIndent();
    for (Operation &child : block) {
      if (auto yield = dyn_cast<emitrust::YieldOp>(&child)) {
        if (failed(emitOperand(yield.getLoc(), yield.getResults().front(),
                               ExprPos::stmt())))
          return failure();
        os << "\n";
        break;
      }
      FailureOr<bool> consumed = emitDropOrCapture(child);
      if (failed(consumed))
        return failure();
      if (*consumed)
        continue;
      if (failed(emitOperation(child)))
        return failure();
      if (opDiverges(&child))
        break;
    }
    decreaseIndent();
    os << "}\n";
  }
  decreaseIndent();
  os << (hasResult ? "};\n" : "}\n");
  return success();
}

LogicalResult RustEmitter::emitTraitDef(emitrust::TraitDefOp traitDefOp) {
  Location loc = traitDefOp.getLoc();
  // FR-140: an FR-52 requirement trait declares its methods under the C
  // function spellings that must satisfy them, so a `__` in the C name lands
  // on a trait method declaration -- a `non_snake_case` site. The binder
  // names the declaration synthesizes are `v0, v1, ...` and never trip.
  if (emitrust::idiomaticRenameEnabled() &&
      llvm::any_of(traitDefOp.getFnNames(), [](Attribute nameAttr) {
        return emitrust::tripsNonSnakeCase(
            cast<StringAttr>(nameAttr).getValue());
      }))
    os << "#[allow(non_snake_case)]\n";
  os << "pub trait " << traitDefOp.getSymName() << " {\n";
  increaseIndent();
  for (auto [nameAttr, typeAttr] : llvm::zip_equal(traitDefOp.getFnNames(),
                                                   traitDefOp.getFnTypes())) {
    auto fnType = cast<FunctionType>(cast<TypeAttr>(typeAttr).getValue());
    os << "fn " << cast<StringAttr>(nameAttr).getValue() << "(";
    // Rust removed anonymous trait-method parameters in edition 2018, so the
    // declaration needs binder names; `v0, v1, ...` is the same scheme the
    // emitter uses for a function's own values.
    for (auto [index, input] : llvm::enumerate(fnType.getInputs())) {
      if (index)
        os << ", ";
      os << "v" << index << ": ";
      if (failed(emitType(loc, input)))
        return failure();
    }
    os << ")";
    if (fnType.getNumResults() == 1) {
      os << " -> ";
      // FR-80: a REFERENCE result on a trait item spells its lifetime out
      // as `&'static`. A bare `&` return on a zero-argument signature is
      // rustc E0106 (nothing to elide from), and 'static is the truthful
      // lifetime: the only producer of a ref-result item is the
      // address-carrying requirement getter, whose implementor lends its
      // own static item. Confined to trait-item results on purpose —
      // everywhere else a ref renders bare `&` through `emitType`.
      if (auto refResult =
              dyn_cast<emitrust::RefType>(fnType.getResult(0))) {
        os << "&'static ";
        if (failed(emitType(loc, refResult.getPointee())))
          return failure();
      } else if (failed(emitType(loc, fnType.getResult(0)))) {
        return failure();
      }
    }
    os << ";\n";
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

/// The largest array length Rust's blanket `impl<T: Default> Default for
/// [T; N]` covers. The library provides the impl for `N` in `0..=32` only;
/// beyond that there is no `Default` for `[T; N]` at all, so a
/// `#[derive(Default)]` on a struct holding one does not compile
/// (`error[E0277]: the trait bound `[u8; 64]: Default` is not satisfied`).
static constexpr uint64_t kMaxDerivedDefaultArrayLength = 32;

/// Whether `#[derive(Default)]` covers a field of `type`.
///
/// Every type EmitRust admits as a struct field implements `Default` — a
/// scalar through the standard library, an `!emitrust.fn_ptr` because it
/// renders `Option<fn(..)>`, and an `!emitrust.struct`/`!emitrust.enum`
/// because `emitStructDef`/`emitEnumDef` give every one of them a `Default`
/// (derived or, for exactly the case this predicate detects, explicit).
/// Arrays are the sole exception, and only above the blanket impl's length
/// ceiling. The recursion stops at a named struct on purpose: that struct
/// carries its own `Default`, so a long array BEHIND one is already handled
/// where it was defined and does not disqualify the field here.
static bool derivedDefaultCovers(Type type) {
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type))
    return arrayType.getSize() <= kMaxDerivedDefaultArrayLength &&
           derivedDefaultCovers(arrayType.getElementType());
  return true;
}

LogicalResult
RustEmitter::emitAbiLayoutAsserts(emitrust::StructDefOp structDefOp,
                                  StringRef name) {
  // The numbers are CLANG'S, recorded at import from its own
  // ASTRecordLayout; nothing here re-derives a layout, because two layout
  // models on the emitter side would only give the two ways to be wrong
  // together. A struct in `cAbiReprCStructs` is ABI-faithful by
  // construction, so the attribute is always present -- refuse loudly rather
  // than emit a `#[repr(C)]` with nothing backing it up if it ever is not.
  auto layout =
      structDefOp->getAttrOfType<DictionaryAttr>(emitrust::kAbiLayoutAttrName);
  if (!layout)
    return structDefOp.emitError()
           << "struct '" << structDefOp.getSymName()
           << "' is exported across the C ABI but carries no "
           << emitrust::kAbiLayoutAttrName
           << "; a #[repr(C)] with no asserted layout is an unbacked promise";
  auto size = dyn_cast_or_null<IntegerAttr>(layout.get("size"));
  auto align = dyn_cast_or_null<IntegerAttr>(layout.get("align"));
  auto offsets = dyn_cast_or_null<ArrayAttr>(layout.get("offsets"));
  if (!size || !align || !offsets ||
      offsets.size() != structDefOp.getFieldNames().size())
    return structDefOp.emitError()
           << "struct '" << structDefOp.getSymName() << "' carries a malformed "
           << emitrust::kAbiLayoutAttrName;
  // `const _: () = assert!(..)` is evaluated at MONOMORPHIZATION-independent
  // const time: a mismatch is `error[E0080]` at `cargo build`, never a wrong
  // answer at run time. That is this repo's mandated safe failure direction,
  // and it is the only thing that would catch a future rustc laying a
  // `#[repr(C)]` struct out differently from the C compiler.
  os << "const _: () = assert!(core::mem::size_of::<" << name
     << ">() == " << size.getInt() << ");\n";
  os << "const _: () = assert!(core::mem::align_of::<" << name
     << ">() == " << align.getInt() << ");\n";
  for (auto [nameAttr, offsetAttr] :
       llvm::zip_equal(structDefOp.getFieldNames(), offsets)) {
    auto offset = dyn_cast<IntegerAttr>(offsetAttr);
    if (!offset)
      return structDefOp.emitError()
             << "struct '" << structDefOp.getSymName()
             << "' carries a malformed " << emitrust::kAbiLayoutAttrName;
    os << "const _: () = assert!(core::mem::offset_of!(" << name << ", "
       << cast<StringAttr>(nameAttr).getValue() << ") == " << offset.getInt()
       << ");\n";
  }
  return success();
}

LogicalResult RustEmitter::emitStructDef(emitrust::StructDefOp structDefOp) {
  Location loc = structDefOp.getLoc();
  // FR-55: the `Default` derive is kept wherever it works — it is the
  // shorter, more idiomatic item, and switching structs that do not need
  // the explicit form would perturb the emitted text of every existing
  // program for nothing. It is dropped only for a struct the derive cannot
  // cover, which is replaced by a hand-written `impl Default` below that
  // reproduces the derive's values exactly (the derive defaults each field
  // independently, which is precisely what `emitDefaultValue` renders).
  bool derivable = llvm::all_of(structDefOp.getFieldTypes(), [](Attribute a) {
    return derivedDefaultCovers(cast<TypeAttr>(a).getValue());
  });
  // FR-124: the Copy decision consults ONLY nonCopyStructNames — the
  // emitModule pre-pass owns every local trigger (FR-94 owned-tail opaque
  // `Vec<`/`String` field, W2.17 has_drop, W2.23 has_copy_ctor) plus the
  // transitive closure over struct-typed fields, because `derive(Copy)`
  // beside any non-Copy field is rustc E0204 — for a struct that is itself
  // clean but embeds a non-Copy struct just as much as for the seeds
  // (measured: the derived-over-`virtual ~B() = default`-base corpus
  // channel). INVARIANT: no per-struct Copy-suppression logic may live
  // here; add new triggers to locallyNonCopy in emitModule so the fixpoint
  // propagates them to embedding structs, or the failure surfaces as rustc
  // E0204 instead of failing review.
  bool copyable = !nonCopyStructNames.contains(structDefOp.getSymName());
  // FR-140: a FIELD spelled with an interior double underscore (C's `m__em`,
  // kept verbatim) trips rustc's denied `non_snake_case`, and the allow is
  // IGNORED on the field itself (measured -- the lint still fires); it is
  // honoured on the struct, which covers every field at once. The struct's
  // own name cannot trip: type names go through `toUpperCamelCase`, which
  // drops underscores entirely. Emitted only when some field trips, so a
  // struct of clean names stays byte-identical.
  if (emitrust::idiomaticRenameEnabled() &&
      llvm::any_of(structDefOp.getFieldNames(), [](Attribute nameAttr) {
        return emitrust::tripsNonSnakeCase(
            cast<StringAttr>(nameAttr).getValue());
      }))
    os << "#[allow(non_snake_case)]\n";
  // FR-182: an ABI-faithful struct REACHABLE from a `--c-abi-exports`
  // signature is laid out the way clang laid it out. `#[repr(C)]` is the
  // only thing that promises that; the default Rust repr is free to reorder
  // fields and MEASURABLY does (the FR-181 spike: `struct tflac` is the same
  // SIZE under both reprs, so nothing catches the difference, but the field
  // OFFSETS differ and flac_validate returned wrong answers across the
  // boundary with exit 0 and no diagnostic). The set is empty without the
  // flag, so no existing crate golden moves a byte.
  const bool reprC = cAbiReprCStructs.contains(structDefOp.getSymName());
  if (reprC)
    os << "#[repr(C)]\n";
  os << "#[derive(Clone" << (copyable ? ", Copy" : "")
     << (derivable ? ", Default" : "") << ")]\n";
  // A field-less struct_def (C's `struct T {};`) prints unit-like with an
  // empty brace body; the derives keep declaration, copy, and default
  // construction working exactly as for the non-empty shape. (A struct with
  // no fields is always derivable, so it never reaches the explicit impl.)
  if (structDefOp.getFieldNames().empty()) {
    os << typePartVisibility() << "struct "
       << itemLeafName(structDefOp.getSymName()) << " {}\n";
    if (reprC)
      return emitAbiLayoutAsserts(structDefOp,
                                  itemLeafName(structDefOp.getSymName()));
    return success();
  }
  // FR-62 F2: an exported owner struct (`emitrust.private_fields`) keeps
  // its FIELDS private even in export mode — the struct itself stays pub,
  // but its state is reachable only through the synthesized `new()` and
  // the exported methods. Without exportItems the prefix is empty either
  // way, so the attribute cannot shift a binary-crate byte.
  StringRef fieldVisibility =
      structDefOp->hasAttr(emitrust::kPrivateFieldsAttrName)
          ? ""
          : typePartVisibility();
  // FR-159: the printed name is the LEAF; `nonCopyStructNames` and
  // `dropStructNames` above are keyed by the WHOLE symbol, which is what
  // keeps a sunk `crate::tu1::Buf` from inheriting a root `Buf`'s verdict.
  StringRef name = itemLeafName(structDefOp.getSymName());
  os << typePartVisibility() << "struct " << name << " {\n";
  increaseIndent();
  for (auto [nameAttr, typeAttr] :
       llvm::zip_equal(structDefOp.getFieldNames(),
                       structDefOp.getFieldTypes())) {
    os << fieldVisibility << cast<StringAttr>(nameAttr).getValue() << ": ";
    if (failed(emitType(loc, cast<TypeAttr>(typeAttr).getValue())))
      return failure();
    os << ",\n";
  }
  decreaseIndent();
  os << "}\n";
  if (reprC && failed(emitAbiLayoutAsserts(structDefOp, name)))
    return failure();
  if (derivable)
    return success();
  os << "impl Default for " << name << " {\n";
  increaseIndent();
  os << "fn default() -> " << name << " {\n";
  increaseIndent();
  os << name << " {\n";
  increaseIndent();
  for (auto [nameAttr, typeAttr] :
       llvm::zip_equal(structDefOp.getFieldNames(),
                       structDefOp.getFieldTypes())) {
    os << cast<StringAttr>(nameAttr).getValue() << ": ";
    if (failed(emitDefaultValue(loc, cast<TypeAttr>(typeAttr).getValue())))
      return failure();
    os << ",\n";
  }
  decreaseIndent();
  os << "}\n";
  decreaseIndent();
  os << "}\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitEnumDef(emitrust::EnumDefOp enumDefOp) {
  StringRef name = itemLeafName(enumDefOp.getSymName()); // FR-159
  StringRef storage = enumStorageSpelling(enumDefOp);
  os << "#[repr(transparent)]\n";
  os << "#[derive(Clone, Copy, PartialEq)]\n";
  StringRef pub = typePartVisibility();
  os << pub << "struct " << name << "(" << pub << storage << ");\n";
  os << "impl " << name << " {\n";
  increaseIndent();
  for (auto [nameAttr, value] : llvm::zip_equal(enumDefOp.getVariantNames(),
                                                enumDefOp.getVariantValues()))
    os << pub << "const " << cast<StringAttr>(nameAttr).getValue() << ": "
       << name << " = " << name << "(" << value << ");\n";
  decreaseIndent();
  os << "}\n";
  StringRef firstVariant =
      cast<StringAttr>(enumDefOp.getVariantNames()[0]).getValue();
  os << "impl Default for " << name << " {\n";
  increaseIndent();
  os << "fn default() -> " << name << " { " << name << "::" << firstVariant
     << " }\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitDataEnumDef(emitrust::DataEnumDefOp defOp) {
  Location loc = defOp.getLoc();
  // The derive set is deliberately minimal (measured against the deny
  // manifest, FR-62 slice 5a): `Clone, Copy` always hold — every permitted
  // payload type is `Copy` — but no `Default` (a closed enum has no
  // canonical default variant, in contrast to struct_def's unconditional
  // one) and no `PartialEq` (a struct payload field derives none).
  // FR-124 RECORDED FUTURE GAP: this unconditional `Copy` is safe ONLY
  // while data-enum payloads are scalar-gated at import. Any wave that
  // admits struct-typed payloads must extend the nonCopyStructNames
  // closure (emitModule's pre-pass) to walk variant payload fields here
  // too, or a non-Copy payload resurrects rustc E0204 under a derive the
  // emitter chose.
  // FR-140 RECORDED NON-GAP: a data variant's payload FIELDS would be
  // `non_snake_case` sites exactly like a struct's, but no C or C++ spelling
  // can reach one today -- both producers hard-code the field name "v"
  // (`CImporter::getOrCreateThrowsEnum` and the std::variant mapping in
  // ImportCTypes), as they do the variant names V0/V1 and Ok0/Err0. So there
  // is deliberately NO allow here: it would be emitter code no test could
  // reach. A wave that admits a SOURCE-SPELLED payload field name must add
  // one, the same shape as `emitStructDef`'s, or resurrect the exit-0
  // unbuildable crate this FR exists to remove.
  os << "#[derive(Clone, Copy)]\n";
  StringRef pub = typePartVisibility();
  os << pub << "enum " << itemLeafName(defOp.getSymName()) << " {\n"; // FR-159
  increaseIndent();
  for (auto [nameAttr, fieldNamesAttr, fieldTypesAttr] :
       llvm::zip_equal(defOp.getVariantNames(), defOp.getVariantFieldNames(),
                       defOp.getVariantFieldTypes())) {
    os << cast<StringAttr>(nameAttr).getValue();
    auto fieldNames = cast<ArrayAttr>(fieldNamesAttr);
    auto fieldTypes = cast<ArrayAttr>(fieldTypesAttr);
    if (!fieldNames.empty()) {
      os << " { ";
      bool first = true;
      for (auto [fieldNameAttr, fieldTypeAttr] :
           llvm::zip_equal(fieldNames, fieldTypes)) {
        if (!first)
          os << ", ";
        first = false;
        os << cast<StringAttr>(fieldNameAttr).getValue() << ": ";
        if (failed(emitType(loc, cast<TypeAttr>(fieldTypeAttr).getValue())))
          return failure();
      }
      os << " }";
    }
    os << ",\n";
  }
  decreaseIndent();
  os << "}\n";
  return success();
}

LogicalResult RustEmitter::emitAggregateInit(Operation *op, Location loc,
                                             Attribute init, Type type) {
  auto elements = dyn_cast<ArrayAttr>(init);
  if (!elements)
    return emitAttribute(loc, init);
  if (auto arrayType = dyn_cast<emitrust::ArrayType>(type)) {
    os << "[";
    bool first = true;
    for (Attribute element : elements) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitAggregateInit(op, loc, element,
                                   arrayType.getElementType())))
        return failure();
    }
    os << "]";
    return success();
  }
  if (auto structType = dyn_cast<emitrust::StructType>(type)) {
    // FR-84: module-first resolution — `op` may be an impl-nested
    // emitrust.variable (the actor plan's staged initializer in the
    // owner's new()), whose nearest symbol table is the def-less
    // emitrust.impl.
    emitrust::StructDefOp structDef =
        emitrust::StructDefOp::lookupFrom(op, structType.getName());
    if (!structDef)
      return op->emitOpError("aggregate init for struct type ")
             << type << " requires a visible emitrust.struct_def";
    os << structType.getName() << " { ";
    for (auto [element, name, fieldType] :
         llvm::zip_equal(elements, structDef.getFieldNames(),
                         structDef.getFieldTypes())) {
      os << cast<StringAttr>(name).getValue() << ": ";
      if (failed(emitAggregateInit(op, loc, element,
                                   cast<TypeAttr>(fieldType).getValue())))
        return failure();
      os << ", ";
    }
    os << "}";
    return success();
  }
  // The GlobalOp verifier rejects list initializers on non-aggregate types.
  return emitError(loc) << "cannot translate list initializer for type "
                        << type;
}

/// Returns true when the initializer expression `emitGlobal` renders for a
/// mutable global of `type` — the `init` attribute when non-null, the
/// type's default value otherwise — is provably const-evaluable, so the
/// thread-local Cell initializer may be wrapped in a `const { ... }` block
/// (clippy::missing_const_for_thread_local; `std::cell::Cell::new` is a
/// const fn, so the wrap is legal whenever its argument is). The predicate
/// mirrors the emitInit dispatch exactly: scalar literals, `[<const>; N]`
/// defaults, aggregate literals with const leaves, and the fn-ptr `None` /
/// `Some(<fn item>)` opaque spellings (fn-item-to-fn-pointer coercion is
/// allowed in const context). Everything else — notably `S::default()`,
/// whose derived Default impl is not a const fn — conservatively stays
/// unwrapped: a missed wrap is only a lint, a wrong wrap is a compile
/// error.
static bool isConstEvaluableInit(Operation *op, Attribute init, Type type) {
  if (!init) {
    // Default value (emitDefaultValue): scalar zero literals, fn-ptr None,
    // and `[<default>; N]` arrays are const; struct `S::default()` is not.
    if (isa<IntegerType, IndexType>(type) || type.isF32() || type.isF64())
      return true;
    if (isa<emitrust::FnPtrType>(type))
      return true;
    if (auto arrayType = dyn_cast<emitrust::ArrayType>(type))
      return isConstEvaluableInit(op, nullptr, arrayType.getElementType());
    return false;
  }
  // Scalar leaves (emitAttribute): integer/bool/float literals, the
  // f32/f64 INFINITY constants, and NaN `fW::from_bits(..)` renderings
  // (from_bits is a const fn) are all const expressions.
  if (isa<IntegerAttr, FloatAttr>(init))
    return true;
  // Opaque fn-ptr initializers: only the two importer-produced spellings
  // are proven — `None` and `Some(<identifier>)` naming a fn item.
  if (auto opaque = dyn_cast<emitrust::OpaqueAttr>(init)) {
    if (!isa<emitrust::FnPtrType>(type))
      return false;
    StringRef text = opaque.getValue();
    if (text == "None")
      return true;
    // FR-159: an item PATH, not just a bare identifier -- a target rendered
    // inside a per-TU module is `crate::tu0::helper`, and the narrow test
    // dropped the `const { .. }` wrapper from every such table: an
    // emitted-byte change plus a clippy::missing_const_for_thread_local
    // regression, with nothing to notice either.
    if (text.consume_front("Some(") && text.consume_back(")"))
      return isRustItemPath(text);
    return false;
  }
  // Aggregate list initializers (emitAggregateInit): an array or struct
  // literal is const iff every leaf is.
  if (auto elements = dyn_cast<ArrayAttr>(init)) {
    if (auto arrayType = dyn_cast<emitrust::ArrayType>(type))
      return llvm::all_of(elements, [&](Attribute element) {
        return isConstEvaluableInit(op, element, arrayType.getElementType());
      });
    if (auto structType = dyn_cast<emitrust::StructType>(type)) {
      // The nearest-table lookup is exact here: `op` is always the
      // GlobalOp under emitGlobal, and emitrust.global carries
      // HasParent<ModuleOp>, so the nearest table IS the module — the
      // FR-84 impl-nesting pitfall cannot occur at this site.
      auto structDef =
          SymbolTable::lookupNearestSymbolFrom<emitrust::StructDefOp>(
              op, StringAttr::get(op->getContext(), structType.getName()));
      if (!structDef || elements.size() != structDef.getFieldTypes().size())
        return false;
      for (auto [element, fieldType] :
           llvm::zip_equal(elements, structDef.getFieldTypes()))
        if (!isConstEvaluableInit(op, element,
                                  cast<TypeAttr>(fieldType).getValue()))
          return false;
      return true;
    }
    return false;
  }
  return false;
}

LogicalResult RustEmitter::emitGlobal(emitrust::GlobalOp globalOp) {
  Location loc = globalOp.getLoc();
  Type type = globalOp.getType();

  // Emits the initializer expression: the (possibly aggregate) init
  // attribute when present, the type's default value otherwise (C
  // zero-initialization of static storage).
  auto emitInit = [&]() -> LogicalResult {
    if (Attribute init = globalOp.getInitAttr())
      return emitAggregateInit(globalOp.getOperation(), loc, init, type);
    return emitDefaultValue(loc, type);
  };

  if (globalOp.getIsConst()) {
    // Never-written global: a plain static item read directly. The
    // verifier guarantees the type is a scalar or array, whose default and
    // initializer expressions are const-evaluable.
    // FR-159: `moduleItemVisibility` is the only visibility a global takes
    // today -- FR-51 export mode still emits none at all, a RECORDED GAP
    // (an exported crate's globals are unreachable from outside it). Phase
    // 1 does not need it; the phase that moves globals into the library
    // surface must close it.
    os << moduleItemVisibility << "static "
       << itemLeafName(globalOp.getSymName()) << ": ";
    if (failed(emitType(loc, type)))
      return failure();
    os << " = ";
    if (failed(emitInit()))
      return failure();
    os << ";\n";
    return success();
  }

  // Mutable global: interior mutability through a thread-local Cell keeps
  // the generated crate free of `unsafe` and `static mut`. Exact for the
  // single-threaded programs the importer accepts. When the initializer is
  // provably const-evaluable it renders inside a `const { ... }` block
  // (clippy::missing_const_for_thread_local) — same value, same semantics
  // for these single-threaded programs.
  bool constInit = isConstEvaluableInit(globalOp.getOperation(),
                                        globalOp.getInitAttr(), type);
  os << "thread_local! {\n";
  increaseIndent();
  os << moduleItemVisibility << "static "
     << itemLeafName(globalOp.getSymName()) << ": std::cell::Cell<";
  if (failed(emitType(loc, type)))
    return failure();
  os << "> = ";
  if (constInit)
    os << "const { ";
  os << "std::cell::Cell::new(";
  if (failed(emitInit()))
    return failure();
  os << ")";
  if (constInit)
    os << " }";
  os << ";\n";
  decreaseIndent();
  os << "}\n";
  return success();
}

/// Resolves the `emitrust.global` referenced by the load or store `op`, or
/// fails with a located diagnostic when the symbol does not name one. The
/// lookup runs in the enclosing MODULE's symbol table (GlobalOp::lookupFrom):
/// `emitrust.impl` is itself a SymbolTable, so a nearest-table lookup from
/// an accessor nested in an owner impl's method would never see the
/// module-level globals.
static FailureOr<emitrust::GlobalOp> lookupGlobal(Operation *op,
                                                  FlatSymbolRefAttr symbol) {
  emitrust::GlobalOp global =
      emitrust::GlobalOp::lookupFrom(op, symbol.getValue());
  if (!global) {
    op->emitOpError("'") << symbol.getValue()
                         << "' does not reference a valid emitrust.global";
    return failure();
  }
  return global;
}

LogicalResult RustEmitter::emitGlobalLoad(emitrust::GlobalLoadOp loadOp) {
  Operation *op = loadOp.getOperation();
  FailureOr<emitrust::GlobalOp> global =
      lookupGlobal(op, loadOp.getGlobalAttr());
  if (failed(global))
    return failure();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << global->getSymName();
  if (!global->getIsConst())
    os << ".with(|__emitrust_tl| __emitrust_tl.get())";
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitGlobalStore(emitrust::GlobalStoreOp storeOp) {
  Operation *op = storeOp.getOperation();
  FailureOr<emitrust::GlobalOp> global =
      lookupGlobal(op, storeOp.getGlobalAttr());
  if (failed(global))
    return failure();
  if (global->getIsConst())
    return op->emitOpError("cannot store to the immutable global @")
           << storeOp.getGlobal();
  os << global->getSymName() << ".with(|__emitrust_tl| __emitrust_tl.set(";
  if (failed(emitOperand(op->getLoc(), storeOp.getValue(),
                         ExprPos::delimited())))
    return failure();
  os << "));\n";
  return success();
}

LogicalResult RustEmitter::emitCellGet(emitrust::CellGetOp getOp) {
  Operation *op = getOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  if (failed(emitOperand(loc, getOp.getSlice(), ExprPos::receiver())))
    return failure();
  os << "[";
  // The ` as usize` conversion is always appended: cast source.
  if (failed(emitOperand(loc, getOp.getIndex(), ExprPos::castSource())))
    return failure();
  os << " as usize].get();\n";
  return success();
}

LogicalResult RustEmitter::emitCellSet(emitrust::CellSetOp setOp) {
  Operation *op = setOp.getOperation();
  Location loc = op->getLoc();
  if (failed(emitOperand(loc, setOp.getSlice(), ExprPos::receiver())))
    return failure();
  os << "[";
  // The ` as usize` conversion is always appended: cast source.
  if (failed(emitOperand(loc, setOp.getIndex(), ExprPos::castSource())))
    return failure();
  os << " as usize].set(";
  if (failed(emitOperand(loc, setOp.getValue(), ExprPos::delimited())))
    return failure();
  os << ");\n";
  return success();
}

LogicalResult RustEmitter::emitGlobalCells(emitrust::GlobalCellsOp cellsOp) {
  Operation *op = cellsOp.getOperation();
  Location loc = op->getLoc();
  FailureOr<emitrust::GlobalOp> global =
      lookupGlobal(op, cellsOp.getGlobalAttr());
  if (failed(global))
    return failure();
  Block &body = cellsOp.getBody().front();
  BlockArgument cells = body.getArgument(0);
  auto refType = cast<emitrust::RefType>(cells.getType());
  Type elementType =
      cast<emitrust::CellSliceType>(refType.getPointee()).getElementType();

  os << global->getSymName() << ".with(|__emitrust_tl| {\n";
  increaseIndent();
  // Rust resolves `as_slice_of_cells` on `Cell<[T]>` only, and method
  // lookup performs no unsizing on the wrapped array, so the binder is
  // coerced to the unsized `&Cell<[T]>` first (the documented
  // `as_slice_of_cells` idiom).
  os << "let __emitrust_tl: &std::cell::Cell<[";
  if (failed(emitType(loc, elementType)))
    return failure();
  os << "]> = __emitrust_tl;\n";
  os << "let " << assignName(cells) << ": ";
  if (failed(emitType(loc, cells.getType())))
    return failure();
  os << " = __emitrust_tl.as_slice_of_cells();\n";
  // FR-61d slice 3: routed through the shared drop/capture prelude like
  // `emitFor` (this also un-blocks a future `cell_get` promotion -- its
  // defs live here -- deliberately NOT taken in this slice).
  for (Operation &child : body) {
    FailureOr<bool> consumed = emitDropOrCapture(child);
    if (failed(consumed))
      return failure();
    if (*consumed)
      continue;
    if (failed(emitOperation(child)))
      return failure();
  }
  decreaseIndent();
  os << "});\n";
  return success();
}

LogicalResult RustEmitter::emitVariable(emitrust::VariableOp variableOp) {
  Value result = variableOp.getResult();
  Location loc = variableOp.getLoc();
  Type valueType =
      cast<emitrust::LValueType>(result.getType()).getValueType();
  // A dead synthesized default is dropped in favor of Rust's deferred
  // initialization (see `computeDeferredInits`), removing the
  // `unused_assignments` the overwritten default would otherwise draw.
  if (deferredInits.count(variableOp.getOperation()))
    return emitDeferredBinding(variableOp.getOperation(), result, valueType);
  // FR-63 (clippy::field_reassign_with_default): the default and its
  // immediately-following field stores render as ONE functional-update
  // literal at the fuse's LAST assign (every fused value's inline text has
  // been captured by then; nothing renders in between). The name is still
  // claimed here so the surviving v-numbering matches the unfused rendering.
  if (fieldInitFuses.count(variableOp.getOperation())) {
    assignName(result);
    return success();
  }
  // A `const`-marked variable is never written after its initializer, and a
  // variable with no reachable mutation likewise needs no `mut`; either way
  // it becomes an immutable `let` binding.
  bool isMut = !variableOp.getIsConst() && lvalueIsMutated(result);
  os << (isMut ? "let mut " : "let ") << assignName(result) << ": ";
  if (failed(emitType(loc, valueType)))
    return failure();
  os << " = ";
  if (Attribute init = variableOp.getInitAttr()) {
    if (failed(emitAggregateInit(variableOp.getOperation(), loc, init,
                                 valueType)))
      return failure();
  } else if (failed(emitDefaultValue(loc, valueType))) {
    return failure();
  }
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitStringRepeat(emitrust::StringRepeatOp op) {
  if (failed(emitLetPrologue(op.getResult(), /*isMut=*/false)))
    return failure();
  // `"<fill>".repeat((<count>) as usize)` — the fused C malloc + constant-fill
  // loop + NUL terminator (FR-64). The fill's single ASCII byte is escaped as
  // a Rust string literal; the i64 count widens to `usize`.
  emitEscapedStringLiteral(op.getFill());
  os << ".repeat(";
  if (failed(emitOperand(op.getLoc(), op.getCount(), ExprPos::castSource())))
    return failure();
  os << " as usize);\n";
  return success();
}

LogicalResult RustEmitter::emitVecFill(emitrust::VecFillOp op) {
  if (failed(emitLetPrologue(op.getResult(), /*isMut=*/false)))
    return failure();
  // `vec![<fill>; (<count>) as usize]` — the runtime-sized heap buffer of a
  // non-char scalar element type (FR-65, the Vec arm of the {array, Vec, span,
  // Option} representation match). The fill is the element type's suffixed
  // zero literal (`0i32`, `0.0f64`, ...) emitted verbatim; the i64 count
  // widens to `usize`.
  os << "vec![" << op.getFill() << "; ";
  if (failed(emitOperand(op.getLoc(), op.getCount(), ExprPos::castSource())))
    return failure();
  os << " as usize];\n";
  return success();
}

LogicalResult RustEmitter::emitLoad(emitrust::LoadOp loadOp) {
  Operation *op = loadOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  // Rvalue load: a bare `*p` needs no parens.
  if (failed(emitPlaceExpr(op->getLoc(), loadOp.getOperand(),
                           /*derefNeedsParens=*/false)))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitAddrOf(emitrust::AddrOfOp addrOfOp) {
  Operation *op = addrOfOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << (addrOfOp.getIsMut() ? "&mut " : "&");
  // `&*p` / `&mut *p`: the `&` binds the whole place, so no parens.
  if (failed(emitPlaceExpr(op->getLoc(), addrOfOp.getOperand(),
                           /*derefNeedsParens=*/false)))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitSliceOf(emitrust::SliceOfOp sliceOfOp) {
  Operation *op = sliceOfOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << (sliceOfOp.getIsMut() ? "&mut " : "&");
  // The base is followed by `[i..]`, so a deref base must parenthesize.
  if (failed(emitPlaceExpr(op->getLoc(), sliceOfOp.getBase(),
                           /*derefNeedsParens=*/true)))
    return failure();
  os << "[";
  // Same delimited-vs-cast-source split as the subscript index.
  bool isIndexTyped = isa<IndexType>(sliceOfOp.getIndex().getType());
  if (failed(emitOperand(op->getLoc(), sliceOfOp.getIndex(),
                         isIndexTyped ? ExprPos::delimited()
                                      : ExprPos::castSource())))
    return failure();
  if (!isIndexTyped)
    os << " as usize";
  os << "..];\n";
  return success();
}

LogicalResult RustEmitter::emitArgvArg(emitrust::ArgvArgOp argvArgOp) {
  Operation *op = argvArgOp.getOperation();
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  os << "&";
  // The table is always a parameter name (nothing constructs a table
  // inside a function), rendered like an indirect-call callee.
  if (failed(emitOperand(op->getLoc(), argvArgOp.getTable(),
                         ExprPos::receiver())))
    return failure();
  os << "[";
  // Same delimited-vs-cast-source split as the slice-of index.
  bool isIndexTyped = isa<IndexType>(argvArgOp.getIndex().getType());
  if (failed(emitOperand(op->getLoc(), argvArgOp.getIndex(),
                         isIndexTyped ? ExprPos::delimited()
                                      : ExprPos::castSource())))
    return failure();
  if (!isIndexTyped)
    os << " as usize";
  os << "][..];\n";
  return success();
}

//===----------------------------------------------------------------------===//
// Dispatch
//===----------------------------------------------------------------------===//

LogicalResult RustEmitter::emitOperation(Operation &op) {
  return llvm::TypeSwitch<Operation *, LogicalResult>(&op)
      .Case<ModuleOp>([&](ModuleOp moduleOp) { return emitModule(moduleOp); })
      .Case<emitrust::UseOp>(
          [&](emitrust::UseOp useOp) { return emitUse(useOp); })
      .Case<emitrust::VerbatimOp>([&](emitrust::VerbatimOp verbatimOp) {
        return emitVerbatim(verbatimOp);
      })
      .Case<emitrust::FuncOp>(
          [&](emitrust::FuncOp funcOp) { return emitFunc(funcOp); })
      .Case<emitrust::ImplOp>(
          [&](emitrust::ImplOp implOp) { return emitImpl(implOp); })
      .Case<emitrust::ActorRuntimeOp>([&](emitrust::ActorRuntimeOp op) {
        return emitActorRuntime(op);
      })
      .Case<emitrust::TraitDefOp>([&](emitrust::TraitDefOp traitDefOp) {
        return emitTraitDef(traitDefOp);
      })
      .Case<emitrust::MethodCallOp>([&](emitrust::MethodCallOp callOp) {
        return emitMethodCall(callOp);
      })
      .Case<emitrust::ReturnOp>(
          [&](emitrust::ReturnOp returnOp) { return emitReturn(returnOp); })
      .Case<emitrust::CallOpaqueOp>([&](emitrust::CallOpaqueOp callOp) {
        return emitCallOpaque(callOp);
      })
      .Case<emitrust::VecFillOp>(
          [&](emitrust::VecFillOp op) { return emitVecFill(op); })
      .Case<emitrust::StringRepeatOp>([&](emitrust::StringRepeatOp op) {
        return emitStringRepeat(op);
      })
      .Case<emitrust::CallIndirectOp>([&](emitrust::CallIndirectOp callOp) {
        return emitCallIndirect(callOp);
      })
      .Case<emitrust::ConstantOp>([&](emitrust::ConstantOp constantOp) {
        return emitConstant(constantOp);
      })
      .Case<emitrust::LiteralOp>([&](emitrust::LiteralOp literalOp) {
        return emitLiteral(literalOp);
      })
      .Case<emitrust::LetOp>(
          [&](emitrust::LetOp letOp) { return emitLet(letOp); })
      .Case<emitrust::AssignOp>(
          [&](emitrust::AssignOp assignOp) { return emitAssign(assignOp); })
      .Case<emitrust::AddOp>([&](emitrust::AddOp addOp) {
        return emitWrappingBinary(addOp.getOperation(), "+", "wrapping_add");
      })
      .Case<emitrust::SubOp>([&](emitrust::SubOp subOp) {
        return emitWrappingBinary(subOp.getOperation(), "-", "wrapping_sub");
      })
      .Case<emitrust::MulOp>([&](emitrust::MulOp mulOp) {
        return emitWrappingBinary(mulOp.getOperation(), "*", "wrapping_mul");
      })
      // Division, remainder, bitwise, and shift operators keep the infix
      // form on every integer type: Rust's `/`, `%`, `&`, `|`, `^`, `<<`,
      // and `>>` already match C's semantics on the matching signedness
      // (division by zero panics where C is undefined).
      .Case<emitrust::DivOp>([&](emitrust::DivOp divOp) {
        return emitBinary(divOp.getOperation(), "/");
      })
      .Case<emitrust::RemOp>([&](emitrust::RemOp remOp) {
        return emitBinary(remOp.getOperation(), "%");
      })
      .Case<emitrust::AndOp>([&](emitrust::AndOp andOp) {
        return emitBinary(andOp.getOperation(), "&");
      })
      .Case<emitrust::OrOp>([&](emitrust::OrOp orOp) {
        return emitBinary(orOp.getOperation(), "|");
      })
      .Case<emitrust::XorOp>([&](emitrust::XorOp xorOp) {
        return emitBinary(xorOp.getOperation(), "^");
      })
      .Case<emitrust::ShlOp>([&](emitrust::ShlOp shlOp) {
        return emitBinary(shlOp.getOperation(), "<<");
      })
      .Case<emitrust::ShrOp>([&](emitrust::ShrOp shrOp) {
        return emitBinary(shrOp.getOperation(), ">>");
      })
      .Case<emitrust::CmpOp>(
          [&](emitrust::CmpOp cmpOp) { return emitCmp(cmpOp); })
      .Case<emitrust::CastOp>(
          [&](emitrust::CastOp castOp) { return emitCast(castOp); })
      .Case<emitrust::BitcastOp>([&](emitrust::BitcastOp bitcastOp) {
        return emitBitcast(bitcastOp);
      })
      .Case<emitrust::SelectOp>([&](emitrust::SelectOp selectOp) {
        return emitSelect(selectOp);
      })
      .Case<emitrust::IfOp>([&](emitrust::IfOp ifOp) { return emitIf(ifOp); })
      .Case<emitrust::ForOp>(
          [&](emitrust::ForOp forOp) { return emitFor(forOp); })
      .Case<emitrust::LoopOp>(
          [&](emitrust::LoopOp loopOp) { return emitLoop(loopOp); })
      .Case<emitrust::WhileOp>(
          [&](emitrust::WhileOp whileOp) { return emitWhile(whileOp); })
      .Case<emitrust::SwitchOp>([&](emitrust::SwitchOp switchOp) {
        return emitSwitch(switchOp);
      })
      .Case<emitrust::MatchOp>([&](emitrust::MatchOp matchOp) {
        return emitMatch(matchOp);
      })
      .Case<emitrust::EnumVariantOp>([&](emitrust::EnumVariantOp variantOp) {
        return emitEnumVariant(variantOp);
      })
      .Case<emitrust::BreakOp>([&](emitrust::BreakOp) {
        os << "break;\n";
        return success();
      })
      .Case<emitrust::ContinueOp>([&](emitrust::ContinueOp) {
        os << "continue;\n";
        return success();
      })
      .Case<emitrust::StructDefOp>([&](emitrust::StructDefOp structDefOp) {
        return emitStructDef(structDefOp);
      })
      .Case<emitrust::EnumDefOp>([&](emitrust::EnumDefOp enumDefOp) {
        return emitEnumDef(enumDefOp);
      })
      .Case<emitrust::DataEnumDefOp>([&](emitrust::DataEnumDefOp defOp) {
        return emitDataEnumDef(defOp);
      })
      .Case<emitrust::GlobalOp>([&](emitrust::GlobalOp globalOp) {
        return emitGlobal(globalOp);
      })
      .Case<emitrust::GlobalLoadOp>([&](emitrust::GlobalLoadOp loadOp) {
        return emitGlobalLoad(loadOp);
      })
      .Case<emitrust::GlobalStoreOp>([&](emitrust::GlobalStoreOp storeOp) {
        return emitGlobalStore(storeOp);
      })
      .Case<emitrust::CellGetOp>(
          [&](emitrust::CellGetOp getOp) { return emitCellGet(getOp); })
      .Case<emitrust::CellSetOp>(
          [&](emitrust::CellSetOp setOp) { return emitCellSet(setOp); })
      .Case<emitrust::GlobalCellsOp>([&](emitrust::GlobalCellsOp cellsOp) {
        return emitGlobalCells(cellsOp);
      })
      .Case<emitrust::VariableOp>([&](emitrust::VariableOp variableOp) {
        return emitVariable(variableOp);
      })
      // Place-refining operations emit nothing at their program point; the
      // place expressions they denote are rendered by their consumers.
      .Case<emitrust::MemberOp, emitrust::SubscriptOp, emitrust::DerefOp,
            emitrust::EnumRawOp>([&](auto) { return success(); })
      .Case<emitrust::LoadOp>(
          [&](emitrust::LoadOp loadOp) { return emitLoad(loadOp); })
      .Case<emitrust::AddrOfOp>([&](emitrust::AddrOfOp addrOfOp) {
        return emitAddrOf(addrOfOp);
      })
      .Case<emitrust::SliceOfOp>([&](emitrust::SliceOfOp sliceOfOp) {
        return emitSliceOf(sliceOfOp);
      })
      .Case<emitrust::ArgvArgOp>([&](emitrust::ArgvArgOp argvArgOp) {
        return emitArgvArg(argvArgOp);
      })
      .Case<emitrust::YieldOp>(
          [&](emitrust::YieldOp) { return success(); })
      .Default([&](Operation *unsupported) -> LogicalResult {
        return unsupported->emitOpError("unable to translate op");
      });
}

//===----------------------------------------------------------------------===//
// Public entry point
//===----------------------------------------------------------------------===//

LogicalResult mlir::emitrust::translateToRust(Operation *op, raw_ostream &os) {
  return translateToRust(op, os, RustEmitOptions());
}

LogicalResult mlir::emitrust::translateToRust(Operation *op, raw_ostream &os,
                                              const RustEmitOptions &options) {
  if (!op)
    return failure();
  RustEmitter emitter(os, options);
  LogicalResult result = emitter.emitOperation(*op);
  // Buffered emission (see `RustEmitter`): copy out whatever was rendered,
  // preserving the pre-buffering behavior of partial output on failure.
  emitter.finish();
  return result;
}
