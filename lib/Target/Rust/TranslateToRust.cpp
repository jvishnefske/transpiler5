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

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Support/IndentedOstream.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

#include "llvm/ADT/Twine.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

using namespace mlir;

namespace {

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
  /// Emits `let vN: bool = vA <pred> vB;`.
  LogicalResult emitCmp(emitrust::CmpOp cmpOp);
  LogicalResult emitFnPtrCmp(emitrust::CmpOp cmpOp, Value lhs, Value rhs,
                             bool isEq);
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
  /// When some field type is outside the reach of the `Default` derive
  /// (`derivedDefaultCovers`), `Default` drops out of the derive list and an
  /// explicit, value-identical `impl Default` follows the item instead.
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
    if (!options.exportItems || emitrust::isInternalLinkageSymbolName(symbol))
      return "";
    return "pub ";
  }

  /// The `pub ` prefix for a part of an exported TYPE — a struct field, a
  /// tuple element, an enum variant constant. Unlike `itemVisibility` this
  /// takes no symbol: types are exported unconditionally in library mode
  /// (see `RustEmitOptions::exportItems` for why), so their parts must be
  /// reachable too or the type is exported but unusable.
  StringRef typePartVisibility() const {
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

  /// Summary of how a binding is accessed across a straight-line/structured op
  /// sequence, used to decide whether its initializer is a dead store.
  struct Liveness {
    bool readFirst = false;    ///< some path reads the binding before writing
    bool writtenAtExit = false; ///< all fall-through paths have written it
    bool diverges = false;     ///< all paths diverge (no fall-through)
    bool hasLoopWrite = false; ///< the binding is assigned inside a loop
    unsigned maxWrites = 0;    ///< max whole-binding writes on any single path
    bool loopReassign = false; ///< the binding is reassigned across iterations
  };

  /// The liveness of an EMPTY region: no reads, and the entering write-state
  /// simply falls through. A named factory instead of a braced literal so the
  /// meaning cannot silently drift with the struct's field order.
  static Liveness passThroughLiveness(bool enteringWritten) {
    Liveness l;
    l.writtenAtExit = enteringWritten;
    return l;
  }

  /// Tracks, across a loop body, whether every `break` that exits the loop
  /// has written the binding on its path.
  struct BreakInfo {
    bool sawBreak = false;
    bool allWritten = true;
  };

  /// `partialWriteBlocks` selects how a projection use of the binding
  /// (`v.x`, `v[i]`) that precedes the first whole write is treated: `true`
  /// (deferral) counts it as a read, since Rust rejects a partial write to an
  /// uninitialized binding (E0381); `false` (dead-store liveness) counts it as
  /// a read only when the projected place is itself read, so a pure partial
  /// write does not keep an earlier store live.
  Liveness analyzeSeq(Block::iterator begin, Block::iterator end, Value binding,
                      bool enteringWritten, BreakInfo *brk,
                      bool partialWriteBlocks);
  Liveness analyzeControl(Operation *op, Value binding, bool enteringWritten,
                          BreakInfo *brk, bool partialWriteBlocks);
  /// Decides, for each candidate binding in the function, whether its init is a
  /// dead store; fills `deferredInits`.
  void computeDeferredInits(Block &block);

  /// Emits the deferred declaration `let [mut] <name>: <type>;` for a binding
  /// whose dead initializer was dropped (see `deferredInits`).
  LogicalResult emitDeferredBinding(Operation *op, Value result, Type type);

  /// Whether any emitted (reachable, non-dead-store) assign targets `value`.
  bool letHasEmittedAssign(Value value);

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
    // A use inside unreachable code, a dropped dead store, or an FR-61d
    // dropped pure op never emits, so it is not a real read.
    if (unreachableOps.count(owner) || deadStores.count(owner) ||
        droppedOps.count(owner))
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
    // A recognized STL container: classify by the (closed) method-name set.
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
    if (implOp.getStructName() != structType.getName())
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
                                                  BreakInfo *brk,
                                                  bool partialWriteBlocks) {
  Liveness r;
  if (auto ifOp = dyn_cast<emitrust::IfOp>(op)) {
    auto arm = [&](Region &region) {
      return region.empty()
                 ? Liveness{}
                 : analyzeSeq(region.front().begin(), region.front().end(),
                              binding, enteringWritten, brk, partialWriteBlocks);
    };
    Liveness thenL = arm(ifOp.getThenRegion());
    // An if with no else falls through with only the entering write-state.
    Liveness elseL = ifOp.getElseRegion().empty()
                         ? passThroughLiveness(enteringWritten)
                         : arm(ifOp.getElseRegion());
    r.readFirst = thenL.readFirst || elseL.readFirst;
    r.hasLoopWrite = thenL.hasLoopWrite || elseL.hasLoopWrite;
    r.loopReassign = thenL.loopReassign || elseL.loopReassign;
    bool thenW = thenL.diverges || thenL.writtenAtExit;
    bool elseW = elseL.diverges || elseL.writtenAtExit;
    r.writtenAtExit = thenW && elseW;
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
                         enteringWritten, &bodyBreaks, partialWriteBlocks);
    if (auto whileOp = dyn_cast<emitrust::WhileOp>(op)) {
      Region &condition = whileOp.getCondition();
      Liveness condL = analyzeSeq(condition.front().begin(),
                                  condition.front().end(), binding,
                                  enteringWritten, /*brk=*/nullptr,
                                  partialWriteBlocks);
      // The condition runs first: its read precedes any body write.
      bodyL.readFirst = condL.readFirst || bodyL.readFirst;
    }
    r.readFirst = bodyL.readFirst;
    r.hasLoopWrite = bodyL.hasLoopWrite || bodyL.maxWrites > 0;
    // `mut` is needed only when a write can recur: it does when a body path
    // that writes the binding loops back (falls through with it written),
    // but not when every write is immediately followed by a `break`.
    r.loopReassign =
        bodyL.loopReassign || (r.hasLoopWrite && bodyL.writtenAtExit);
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
                 ? passThroughLiveness(enteringWritten)
                 : analyzeSeq(region.front().begin(), region.front().end(),
                              binding, enteringWritten, brk, partialWriteBlocks);
    };
    Liveness d = seq(sw.getDefaultRegion());
    bool anyRead = d.readFirst;
    bool allWritten = d.diverges || d.writtenAtExit;
    bool allDiverge = d.diverges;
    bool loopWrite = d.hasLoopWrite;
    bool loopReassign = d.loopReassign;
    unsigned maxWrites = d.maxWrites;
    for (Region &caseRegion : sw.getCaseRegions()) {
      Liveness ci = seq(caseRegion);
      anyRead |= ci.readFirst;
      allWritten = allWritten && (ci.diverges || ci.writtenAtExit);
      allDiverge = allDiverge && ci.diverges;
      loopWrite |= ci.hasLoopWrite;
      loopReassign |= ci.loopReassign;
      maxWrites = std::max(maxWrites, ci.maxWrites);
    }
    r.readFirst = anyRead;
    r.writtenAtExit = allWritten; // the `_` arm covers every unmatched value
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
      Liveness sub = analyzeSeq(region.front().begin(), region.front().end(),
                                binding, enteringWritten, brk, partialWriteBlocks);
      r.readFirst |= sub.readFirst;
      r.hasLoopWrite |= sub.hasLoopWrite || sub.maxWrites > 0;
      r.loopReassign |= sub.loopReassign;
    }
  return r;
}

RustEmitter::Liveness RustEmitter::analyzeSeq(Block::iterator begin,
                                              Block::iterator end, Value binding,
                                              bool enteringWritten,
                                              BreakInfo *brk,
                                              bool partialWriteBlocks) {
  Liveness r;
  bool written = enteringWritten; // binding written on the current path
  unsigned writes = 0;            // whole-binding writes on the current path
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
      }
      r.diverges = true;
      r.maxWrites = std::max(r.maxWrites, writes);
      return r;
    }
    // A `continue` returns to the loop head; this straight-line path ends
    // without exiting the loop.
    if (isa<emitrust::ContinueOp>(op)) {
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
      Liveness sub = analyzeControl(op, binding, written, brk, partialWriteBlocks);
      if (sub.readFirst && !written)
        r.readFirst = true;
      r.hasLoopWrite |= sub.hasLoopWrite;
      r.loopReassign |= sub.loopReassign;
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
    // Only worth deferring when the binding is actually read; an unread
    // binding is `_`-prefixed instead (its dead init then draws no warning).
    bool isRead = false;
    for (Operation *user : binding.getUsers()) {
      if (unreachableOps.count(user) || droppedOps.count(user) ||
          isBindingWrite(user, binding))
        continue;
      isRead = true;
      break;
    }
    if (!isRead)
      return;
    Liveness info =
        analyzeSeq(std::next(op->getIterator()), op->getBlock()->end(), binding,
                   /*enteringWritten=*/false, /*brk=*/nullptr,
                   /*partialWriteBlocks=*/true);
    if (info.readFirst)
      return; // the initializer is live: some path reads before writing
    // `mut` is needed when a path assigns more than once, when the binding is
    // reassigned across loop iterations, or when it is mutably borrowed after
    // its (single) initializing assignment.
    bool postInitMutation = false;
    for (Operation *user : binding.getUsers()) {
      if (unreachableOps.count(user))
        continue;
      if (auto addrOf = dyn_cast<emitrust::AddrOfOp>(user))
        postInitMutation |= addrOf.getIsMut();
      if (auto sliceOf = dyn_cast<emitrust::SliceOfOp>(user))
        postInitMutation |= sliceOf.getIsMut();
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
    Liveness after = analyzeSeq(std::next(op.getIterator()), entryBlock.end(),
                                binding, /*enteringWritten=*/false,
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
                     /*enteringWritten=*/false, /*brk=*/nullptr,
                     /*partialWriteBlocks=*/false)
              .readFirst)
        continue;
      // Not live-out: not read after the loop before the function ends.
      if (analyzeSeq(std::next(loopOp.getIterator()), entryBlock.end(), binding,
                     /*enteringWritten=*/false, /*brk=*/nullptr,
                     /*partialWriteBlocks=*/false)
              .readFirst)
        continue;
      deadStores.insert(&op);
    }
  }
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

void RustEmitter::computeDroppedOps(emitrust::FuncOp funcOp) {
  // Reverse program order: users always render after their defs (nested
  // users belong to later parent ops), so one reverse sweep sees every
  // consumer's fate before deciding its producers -- cascades converge in a
  // single pass. The default post-order walk visits nested ops before
  // their parent; reversed, parents come first, which still keeps every
  // user ahead of its def.
  SmallVector<Operation *> ops;
  funcOp->walk([&](Operation *op) { ops.push_back(op); });
  for (Operation *op : llvm::reverse(ops)) {
    if (op->getNumResults() != 1 || !isPureProducer(op))
      continue;
    if (unreachableOps.count(op))
      continue; // already never emitted; dropping would double-count
    if (auto letOp = dyn_cast<emitrust::LetOp>(op)) {
      // A mutable or deferred `let` has assignments; dropping the binding
      // would orphan them (E0425). Only the plain alias form can drop.
      if (letOp.getIsMut() || deferredInits.count(op) ||
          letHasEmittedAssign(letOp.getResult()))
        continue;
    }
    // `valueIsRead` (with the droppedOps guard active) is the proof of "no
    // emitted read". The cache is cleared each round because every new drop
    // can un-read further values upstream.
    valueReadCache.clear();
    if (!valueIsRead(op->getResult(0))) {
      droppedOps.insert(op);
      ++dropCount;
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
    // stores, or dropped pure ops never render).
    SmallVector<OpOperand *, 4> realUses;
    for (OpOperand &use : op->getResult(0).getUses()) {
      Operation *owner = use.getOwner();
      if (unreachableOps.count(owner) || deadStores.count(owner) ||
          droppedOps.count(owner))
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
      .Case<emitrust::ShlOp, emitrust::ShrOp>([](auto) { return Prec::Shift; })
      .Case<emitrust::CmpOp>([](auto) { return Prec::Compare; })
      .Case<emitrust::CastOp>([&](auto) {
        // The enum-target form renders `Name(x as i32)`, a postfix call.
        return isa<emitrust::EnumType>(op->getResult(0).getType())
                   ? Prec::Postfix
                   : Prec::Cast;
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
      .Case<emitrust::CastOp>([&](auto) {
        // The enum-target form `Name(x as i32)` ends in `)`.
        return !isa<emitrust::EnumType>(op->getResult(0).getType());
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
      .Case<emitrust::ShlOp, emitrust::ShrOp>(
          [&](Operation *b) { return rhsEndsInCast(b, Prec::Shift); })
      .Case<emitrust::CmpOp>(
          [&](Operation *b) { return rhsEndsInCast(b, Prec::Compare); })
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
    os << opaqueType.getValue();
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
    os << "&[Vec<i8>]";
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
    // sentinel is ever needed. The `-> R` clause is omitted for a void
    // result, matching Rust's `fn(...)` spelling.
    os << "Option<fn(";
    bool first = true;
    for (Type input : fnPtrType.getInputs()) {
      if (!first)
        os << ", ";
      first = false;
      if (failed(emitType(loc, input)))
        return failure();
    }
    os << ")";
    if (!fnPtrType.getResults().empty()) {
      os << " -> ";
      if (failed(emitType(loc, fnPtrType.getResults().front())))
        return failure();
    }
    os << ">";
    return success();
  }
  // Lvalue types are never rendered; they fall through to the error below.
  return emitError(loc) << "cannot translate type " << type;
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
    // deterministically. NaN constants stay rejected: Rust does not
    // guarantee the sign/payload of its NAN constant, so a byte pattern
    // could silently diverge from C's.
    if (floatAttr.getValue().isInfinity()) {
      os << (floatAttr.getType().isF32() ? "f32" : "f64")
         << (floatAttr.getValue().isNegative() ? "::NEG_INFINITY"
                                               : "::INFINITY");
      return success();
    }
    if (!floatAttr.getValue().isFinite())
      return emitError(loc)
             << "cannot translate non-finite floating-point constant";
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
      os << "String::new()";
      return success();
    }
    if (opaqueType.getValue() == "__EmitrustFile") {
      os << "__EmitrustFile::Null";
      return success();
    }
    if (opaqueType.getValue().starts_with("Vec<")) {
      os << "Vec::new()";
      return success();
    }
    // The C99-43 C1 Option-of-cursor cell (`Option<i64>`, the staged
    // out-cell temp of a single-global-or-NULL call): the default is the
    // C null pointer, mirroring `!emitrust.fn_ptr`'s None above.
    if (opaqueType.getValue().starts_with("Option<")) {
      os << "None";
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
  if (failed(emitType(result.getLoc(), result.getType())))
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
  // FR-62 slice 5c: the shared `mod actor_rt` epilogue exists once per
  // crate and its text is flavor-specific, so every anchor in one module
  // must agree on the mode. The driver never produces a mixed module
  // (--actor-mode is global); reaching emission with one must fail loudly
  // at the disagreeing anchor, never emit one flavor's runtime under the
  // other's wrappers.
  emitrust::ActorRuntimeOp firstAnchor;
  for (auto runtime : moduleOp.getOps<emitrust::ActorRuntimeOp>()) {
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
  for (Operation &op : *moduleOp.getBody()) {
    if (!isa<emitrust::UseOp, emitrust::VerbatimOp, emitrust::FuncOp,
             emitrust::ImplOp, emitrust::StructDefOp, emitrust::EnumDefOp,
             emitrust::DataEnumDefOp, emitrust::GlobalOp,
             emitrust::TraitDefOp, emitrust::ActorRuntimeOp>(&op))
      return op.emitOpError("unable to translate op");
    if (failed(emitOperation(op)))
      return failure();
  }
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
    if (candidate.getStructName() == actor) {
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
  os << verbatimOp.getValue() << "\n";
  return success();
}

LogicalResult RustEmitter::emitFunc(emitrust::FuncOp funcOp) {
  Operation *op = funcOp.getOperation();
  auto fn = cast<FunctionOpInterface>(op);

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
  pendingInlineCapture = false;

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
  os << itemVisibility(symbol) << (isAsyncFn ? "async fn " : "fn ") << symbol;
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
    os << (carried.empty() ? assignName(argument)
                           : claimName(argument, carried))
       << ": ";
    if (failed(emitType(argument.getLoc(), argument.getType())))
      return failure();
  }
  os << ")";
  if (fn.getNumResults() == 1) {
    os << " -> ";
    if (failed(emitType(op->getLoc(), fn.getResultTypes().front())))
      return failure();
  }
  os << " {\n";
  increaseIndent();
  if (failed(emitBlockBody(entryBlock)))
    return failure();
  decreaseIndent();
  os << "}\n";
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

  os << callOp.getCallee() << "(";
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
  // refines it into a deterministic panic.
  if (failed(emitOperand(loc, callOp.getCallee(), ExprPos::receiver())))
    return failure();
  os << ".expect(\"null function pointer\")(";
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
  os << "." << callOp.getMethod() << "(";
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
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
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
  os << "let ";
  if (deferredInits.lookup(op))
    os << "mut ";
  os << assignName(result) << ": ";
  if (failed(emitType(result.getLoc(), type)))
    return failure();
  os << ";\n";
  return success();
}

LogicalResult RustEmitter::emitIfExprBinding(Operation *op, Value result,
                                             Type valueType,
                                             emitrust::IfOp ifOp) {
  (void)op;
  // The name is assigned unconditionally so v-numbering stays stable even
  // when the binding folds away.
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
    if (failed(emitType(result.getLoc(), valueType)))
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

LogicalResult RustEmitter::emitAssign(emitrust::AssignOp assignOp) {
  Operation *op = assignOp.getOperation();
  // A dead store emits nothing: its written value is never read.
  if (deadStores.count(op))
    return success();
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
  //      - FR-61f bare-variable PLACE: the operand is an `emitrust.load` of
  //        the SAME `emitrust.variable` place (`s = s + i`). A projection
  //        place (`*p`/`a[i]`) is EXCLUDED -- re-reading it under `<op>=`
  //        could re-run a side effect and evaluate twice.
  // `v = v + e` and `v += e` compute the same value for such a side-effect-free
  // target (identical overflow behaviour on the signed/infix path), so stdout
  // is unchanged and the byte-diff oracle stays green.
  {
    Value val = assignOp.getValue();
    Operation *binOp = val.getDefiningOp();
    if (binOp && inlineExprs.count(val)) {
      StringRef sym = compoundAssignSymbol(binOp);
      bool selfRef = false;
      if (!sym.empty()) {
        if (!isa<emitrust::LValueType>(var.getType())) {
          selfRef = binOp->getOperand(0) == var;
        } else if (var.getDefiningOp<emitrust::VariableOp>()) {
          // Bare local place: operand 0 must be a load OF this place that
          // renders INLINE (as the place read). A load bound to its own
          // `let vN` -- e.g. hoisted before a method-call barrier -- must NOT
          // fold, or dropping the `<op>=` operand orphans that binding
          // (rustc unused-variable error, not a miscompile, caught here).
          Value op0val = binOp->getOperand(0);
          Operation *op0 = op0val.getDefiningOp();
          selfRef = op0 && isa<emitrust::LoadOp>(op0) &&
                    op0->getOperand(0) == var && inlineExprs.count(op0val);
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
        if (failed(emitOperand(loc, binOp->getOperand(1), ExprPos::stmt())))
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
    auto enumDef = SymbolTable::lookupNearestSymbolFrom<emitrust::EnumDefOp>(
        op, StringAttr::get(op->getContext(), enumType.getName()));
    if (!enumDef)
      return op->emitOpError("cast to enum type ")
             << enumType << " requires a visible emitrust.enum_def";
    if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
      return failure();
    os << enumType.getName() << "(";
    if (failed(emitOperand(op->getLoc(), op->getOperand(0),
                           ExprPos::castSource())))
      return failure();
    os << " as " << (enumDef.getUnsignedUnderlying() ? "u32" : "i32")
       << ");\n";
    return success();
  }
  if (failed(emitLetPrologue(op->getResult(0), /*isMut=*/false)))
    return failure();
  // An enum-typed source gets `.0` appended (postfix binds tighter than
  // `as`), so it needs the stricter receiver parenthesization.
  bool enumSource = isa<emitrust::EnumType>(op->getOperand(0).getType());
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
  os << "..";
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
      if (failed(emitType(loc, fnType.getResult(0))))
        return failure();
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
  os << "#[derive(Clone, Copy" << (derivable ? ", Default" : "") << ")]\n";
  // A field-less struct_def (C's `struct T {};`) prints unit-like with an
  // empty brace body; the derives keep declaration, copy, and default
  // construction working exactly as for the non-empty shape. (A struct with
  // no fields is always derivable, so it never reaches the explicit impl.)
  if (structDefOp.getFieldNames().empty()) {
    os << typePartVisibility() << "struct " << structDefOp.getSymName()
       << " {}\n";
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
  StringRef name = structDefOp.getSymName();
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
  StringRef name = enumDefOp.getSymName();
  StringRef storage = enumDefOp.getUnsignedUnderlying() ? "u32" : "i32";
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
  os << "#[derive(Clone, Copy)]\n";
  StringRef pub = typePartVisibility();
  os << pub << "enum " << defOp.getSymName() << " {\n";
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
    auto structDef = SymbolTable::lookupNearestSymbolFrom<
        emitrust::StructDefOp>(op, StringAttr::get(op->getContext(),
                                                   structType.getName()));
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
    os << "static " << globalOp.getSymName() << ": ";
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
  // single-threaded programs the importer accepts.
  os << "thread_local! {\n";
  increaseIndent();
  os << "static " << globalOp.getSymName() << ": std::cell::Cell<";
  if (failed(emitType(loc, type)))
    return failure();
  os << "> = std::cell::Cell::new(";
  if (failed(emitInit()))
    return failure();
  os << ");\n";
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
