//===- CrateEmitter.cpp - Pure Rust crate content rendering --------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the functional core of the emitrust-cc driver: the
/// crate-name sanitizer, the FR-51 crate-shape selection, and the pure
/// renderers that turn a fully converted EmitRust module into `Cargo.toml`
/// and crate-root (`src/main.rs` or `src/lib.rs`) text.
//
//===----------------------------------------------------------------------===//

#include "CrateEmitter.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/RustCasing.h"
#include "EmitRust/RustPreludeShadow.h"
#include "EmitRust/Target/TranslateToRust.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace emitrustcc {

/// Attribute header prepended to a generated crate root.
///
/// FR-220: under the default idiomatic rename this is EMPTY. `dead_code` was
/// the last surviving blanket allow, and a crate-root allow is a blindfold --
/// it hid every kind of dead item at once, including the ONE kind that would
/// signal an emitter defect. Measured over the whole epoch-7 population (294
/// crates) with the blanket allow stripped: 33 crates warn, 56 warnings, and
/// **zero** of them are `function X is never used`. Every warning that DOES
/// fire is faithful translation of a declaration the C program made and did
/// not use -- an unmentioned enumerator, an unread field, an unconstructed
/// record, an uncalled C++ method. Those now carry a TARGETED
/// `#[allow(dead_code)]` on the item the emitter knows it is writing (the
/// enum's transparent struct and its associated-constant `impl`, the
/// `struct_def`, the data enum, and the inherent `impl` block -- see
/// `RustEmitter::emitStructDef` / `emitEnumDef` / `emitDataEnumDef` /
/// `emitImpl` in `TranslateToRust.cpp`). A plain `fn` is deliberately left
/// UNCOVERED, so a future dead emitted function draws a rustc `dead_code`
/// warning instead of being hidden.
///
/// It is NOT `dead_code = "deny"` in `Cargo.toml`, and must not become one.
/// That was measured and rejected twice over. (1) It regresses the
/// c-testsuite ledger by 10: an external-linkage C function uncalled in its
/// own TU is not even a C warning, and the deadness is an artifact of the
/// emitter privatizing it for a `bin` crate. (2) It breaks FR-44's headline
/// guarantee -- `RealWorld/Cpp` `polygon`'s `--incremental` crate stops
/// building -- because recovery drops a rejected CALLER and orphans every
/// function only that caller reached, so RECOVERY STRUCTURALLY MANUFACTURES
/// DEAD FUNCTIONS. The tripwire comes from the RATCHET instead:
/// `nix/clippy-eval/clippy_eval.py` tracks `dead_code` in
/// `TRACKED_RUSTC_LINTS` and the frozen epoch corpus measures zero of them,
/// so a newly dead emitted function raises the tally and fails the gate --
/// without a hard error, so recovery still builds. The lit-level pins are
/// test/Driver/dead-code-allow-targeted.c (emit) and
/// test/Driver/dead-code-tripwire.c (rustc, including the recovery case).
///
/// No reachability analysis is involved, and none may be added: the FR-40
/// item graph is CLOSED and skips C++ member functions and non-file-scope
/// records, so it under-reports reachability and would call live items dead.
///
/// `unused_assignments` was formerly allowed here to mask a single residual: a
/// dead store inside a loop body, whose sound cross-iteration liveness would
/// risk a miscompile and is deliberately not attempted. FR-61f lifts canonical
/// C counting loops to `emitrust.for` range heads (no explicit backedge store),
/// removing most of that residual; the lint is DENIED in `Cargo.toml` like
/// every other lint, so a regression fails the build instead of hiding.
/// The residual that survives FR-61f -- a store in a loop body that the
/// fenced cross-iteration elision may not delete -- is handled PER FUNCTION
/// and NOT re-allowed here: FR-106's all-path detector in `TranslateToRust`
/// puts `#[allow(unused_assignments)]` on the individual `fn` it can prove
/// holds such a store (measured: 4 of 3475 emitted functions across 472
/// rustc-clean crates). Moving the allow back into this header would delete
/// the tripwire on the other 3471. `dead_code` followed it out of this header
/// for exactly the same reason.
/// Every other lint the old blanket header silenced is now DENIED in
/// `Cargo.toml`'s `[lints.rust]` table (see `renderCargoToml`), so a regression
/// fails the build. `dead_code` is the sole exception, for the measured
/// reasons above; it is held at zero by the ratchet instead.
///
/// Under `--preserve-c-names` the three NAMING lints stay in the allow list
/// instead of the deny table: verbatim C spellings legitimately trip them, and
/// the flag's whole point is to keep those spellings. Only `dead_code` left.
static constexpr llvm::StringLiteral kAllowHeader = "";
static constexpr llvm::StringLiteral kAllowHeaderPreserveNames =
    "#![allow(non_snake_case, "
    "non_upper_case_globals, non_camel_case_types)]\n";

/// Verbatim entry-point wrapper: forwards the imported C `main`'s return
/// value as the process exit code.
static constexpr llvm::StringLiteral kMainWrapper =
    "fn main() { std::process::exit(c_main()); }\n";

/// Wrapper for a C `main(int argc, char **argv)` imported with its `argv`
/// dropped: the process argument count (program name included, matching
/// C's argc) is passed as the sole parameter. `args_os` is used so an
/// argument that is not valid Unicode still counts (`args` would panic).
static constexpr llvm::StringLiteral kMainArgcWrapper =
    "fn main() { std::process::exit(c_main(std::env::args_os().len() as "
    "i32)); }\n";

/// FR-62 slice 5c: the async flavor's entry wrappers — `c_main` renders as
/// an `async fn` when an async actor runtime anchor exists, so the shim
/// builds a tokio CURRENT_THREAD runtime (E4's determinism substrate:
/// single-threaded scheduling + the wrappers' immediate await keep effect
/// order equal to program order) and drives `c_main` to completion with
/// `block_on`. The explicit Builder form is chosen over
/// `#[tokio::main(flavor = "current_thread")]` because it works without
/// the "macros" feature: both were probe-built, and the attribute macro
/// pulls tokio-macros/syn/quote/proc-macro2 (10 crates in the tree vs 3)
/// for zero behavioral difference.
static constexpr llvm::StringLiteral kMainWrapperAsync =
    "fn main() { "
    "std::process::exit(tokio::runtime::Builder::new_current_thread()"
    ".enable_all().build().expect(\"tokio runtime build failed\")"
    ".block_on(c_main())); }\n";

/// The async shim for a `c_main` that takes the imported argc parameter.
static constexpr llvm::StringLiteral kMainArgcWrapperAsync =
    "fn main() { "
    "std::process::exit(tokio::runtime::Builder::new_current_thread()"
    ".enable_all().build().expect(\"tokio runtime build failed\")"
    ".block_on(c_main(std::env::args_os().len() as i32))); }\n";

/// C99-43 C3: wrapper for a C `main(int argc, char **argv)` whose argv was
/// admitted into the table form (`c_main(argc, &[Vec<i8>])`). The argument
/// vector is collected from `args_os` as RAW BYTES — each OS argument's
/// bytes widened to `i8` with a trailing NUL appended, reproducing C's
/// NUL-terminated `char*` strings byte-for-byte (an argument that is not
/// valid Unicode still round-trips, where a `String` collection would panic
/// or lossily replace). The `&[Vec<i8>]` borrow is passed alongside the
/// `argc` count. `OsStrExt::as_bytes` is the unix raw-bytes accessor.
static constexpr llvm::StringLiteral kMainArgvWrapper =
    "fn main() {\n"
    "    use std::os::unix::ffi::OsStrExt;\n"
    "    let __emitrust_argv: Vec<Vec<i8>> = std::env::args_os()\n"
    "        .map(|a| {\n"
    "            a.as_bytes().iter().map(|&b| b as i8)"
    ".chain(std::iter::once(0i8)).collect()\n"
    "        })\n"
    "        .collect();\n"
    "    std::process::exit(c_main(__emitrust_argv.len() as i32, "
    "&__emitrust_argv));\n"
    "}\n";

/// The async shim for a `c_main` that takes the imported argv table: the
/// same raw-bytes argv collection as the sync wrapper, driving the async
/// `c_main` to completion on the tokio current_thread runtime.
static constexpr llvm::StringLiteral kMainArgvWrapperAsync =
    "fn main() {\n"
    "    use std::os::unix::ffi::OsStrExt;\n"
    "    let __emitrust_argv: Vec<Vec<i8>> = std::env::args_os()\n"
    "        .map(|a| {\n"
    "            a.as_bytes().iter().map(|&b| b as i8)"
    ".chain(std::iter::once(0i8)).collect()\n"
    "        })\n"
    "        .collect();\n"
    "    std::process::exit(tokio::runtime::Builder::new_current_thread()\n"
    "        .enable_all().build().expect(\"tokio runtime build failed\")\n"
    "        .block_on(c_main(__emitrust_argv.len() as i32, "
    "&__emitrust_argv)));\n"
    "}\n";

/// FR-228: whether `module` carries the FULLY BUFFERED stdout runtime, so the
/// entry wrapper has to install it and flush it.
///
/// The importer emits that runtime for every module that writes to stdout
/// (`CImporter::emitStdoutRuntime`) -- one text, deliberately, so a `--link`
/// merge's textual dedup cannot end up with two shapes -- and it is a
/// VERBATIM item, not a symbol, so the question has to be asked of the text.
/// Deriving the answer here instead (from `hasCMain` plus a guess at whether
/// anything prints) would be a second spelling of the same rule, and the two
/// disagreeing would either fail to compile or, worse, build a crate that
/// buffers and never flushes.
///
/// A module WITHOUT the runtime prints nothing, so its entry wrapper stays
/// byte-identical to the historical one.
static bool hasBufferedStdout(mlir::ModuleOp module) {
  for (auto verbatim : module.getOps<mlir::emitrust::VerbatimOp>())
    if (verbatim.getValue().contains("fn __emitrust_stdout_init()"))
      return true;
  return false;
}

/// FR-228: the entry wrapper for a crate that carries the buffered stdout
/// runtime.
///
/// C flushes `stdout` at NORMAL termination and at nothing else, so the three
/// things this adds are exactly the three normal exits the wrapper owns:
/// installing the buffering (and the panic hook that flushes it, so a crate
/// that panics still shows what it printed), and flushing after `c_main`
/// returns -- BEFORE `std::process::exit`, which runs no destructors and
/// would otherwise discard the buffer. C's own `exit(status)` calls get the
/// same flush spliced in at import. `abort` gets none, deliberately.
///
/// `arity` and `asyncMain` select the same four bodies the unbuffered
/// wrappers spell; the argv collection is character-for-character the one in
/// `kMainArgvWrapper` so the two flavors cannot drift.
static std::string renderBufferedMainWrapper(unsigned arity, bool asyncMain) {
  std::string call = "c_main(";
  if (arity == 1)
    call += "std::env::args_os().len() as i32";
  else if (arity == 2)
    call += "__emitrust_argv.len() as i32, &__emitrust_argv";
  call += ")";
  std::string body;
  body += "fn main() {\n";
  body += "    __emitrust_stdout_init();\n";
  if (arity == 2) {
    body += "    use std::os::unix::ffi::OsStrExt;\n";
    body += "    let __emitrust_argv: Vec<Vec<i8>> = std::env::args_os()\n";
    body += "        .map(|a| {\n";
    body += "            a.as_bytes().iter().map(|&b| b as i8)"
            ".chain(std::iter::once(0i8)).collect()\n";
    body += "        })\n";
    body += "        .collect();\n";
  }
  if (asyncMain) {
    body += "    let __emitrust_status = "
            "tokio::runtime::Builder::new_current_thread()\n";
    body += "        .enable_all().build()"
            ".expect(\"tokio runtime build failed\")\n";
    body += "        .block_on(" + call + ");\n";
  } else {
    body += "    let __emitrust_status = " + call + ";\n";
  }
  body += "    __emitrust_out_flush();\n";
  body += "    std::process::exit(__emitrust_status);\n";
  body += "}\n";
  return body;
}

CrateType selectCrateType(CrateTypeRequest request, mlir::ModuleOp module) {
  switch (request) {
  case CrateTypeRequest::Bin:
    return CrateType::Bin;
  case CrateTypeRequest::Lib:
    return CrateType::Lib;
  case CrateTypeRequest::Auto:
    return hasCMain(module) ? CrateType::Bin : CrateType::Lib;
  }
  llvm_unreachable("covered switch");
}

llvm::StringRef crateRootFileName(CrateType type) {
  return type == CrateType::Bin ? "main.rs" : "lib.rs";
}

bool isUsableCrateName(llvm::StringRef name) {
  if (name.empty())
    return false;
  if (name.front() >= '0' && name.front() <= '9')
    return false;
  return llvm::all_of(name, [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

std::string sanitizeCrateName(llvm::StringRef stem) {
  std::string name;
  name.reserve(stem.size());
  for (char c : stem) {
    if (c >= 'A' && c <= 'Z')
      name.push_back(static_cast<char>(c - 'A' + 'a'));
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
      name.push_back(c);
    else
      name.push_back('_');
  }
  if (name.empty())
    name = "transpiled";
  if (name.front() >= '0' && name.front() <= '9')
    name.insert(name.begin(), '_');
  return name;
}

bool hasCMain(mlir::ModuleOp module) {
  mlir::Operation *symbol =
      mlir::SymbolTable::lookupSymbolIn(module, "c_main");
  return symbol != nullptr && llvm::isa<mlir::emitrust::FuncOp>(symbol);
}

bool hasAsyncActorRuntime(mlir::ModuleOp module) {
  for (auto runtime : module.getOps<mlir::emitrust::ActorRuntimeOp>())
    if (runtime.getMode() == mlir::emitrust::ActorMode::async)
      return true;
  return false;
}

/// Returns the number of parameters the module's `c_main` takes, selecting
/// the entry wrapper: 0 for `main(void)`, 1 for a `main(int argc, ...)` whose
/// argv was dropped at import (C99-43 C3 leaves argc-only programs at arity
/// 1, byte-identical), 2 for a `main(int argc, char **argv)` whose argv was
/// admitted into the `!emitrust.argv_table` (C99-43 C3). Callers have already
/// established `hasCMain`.
static unsigned cMainInputCount(mlir::ModuleOp module) {
  auto funcOp = llvm::dyn_cast_if_present<mlir::emitrust::FuncOp>(
      mlir::SymbolTable::lookupSymbolIn(module, "c_main"));
  return funcOp ? funcOp.getFunctionType().getNumInputs() : 0;
}

/// FR-139: would rustc's `non_snake_case` lint fire on a CRATE named `name`?
/// The predicate exists so the emitted manifest's own deny table cannot veto
/// the crate name the caller asked for -- see `renderCargoToml`.
///
/// FR-140 moved the body to `EmitRust/RustCasing.h` and shares it with the
/// Rust emitter, which asks the SAME question about every item name it
/// renders. Two spellings of "would rustc reject this?" that could drift is
/// precisely the failure this crate's naming headers exist to prevent.
static bool crateNameTripsNonSnakeCase(llvm::StringRef name) {
  return mlir::emitrust::tripsNonSnakeCase(name);
}

std::string renderCargoToml(llvm::StringRef crateName, CrateType type,
                            bool asyncActorRuntime, bool cAbiExports) {
  std::string toml;
  llvm::raw_string_ostream os(toml);
  os << "[package]\n"
     << "name = \"" << crateName << "\"\n"
     << "version = \"0.1.0\"\n"
     << "edition = \"2021\"\n";
  if (type == CrateType::Lib) {
    os << "\n"
       << "[lib]\n"
       << "name = \"" << crateName << "\"\n"
       << "path = \"src/lib.rs\"\n";
    // FR-139: the crate is only dlopen-able if cargo actually builds a shared
    // object for it. Default-off, so every crate emitted before this flag
    // existed keeps its rlib manifest byte for byte.
    if (cAbiExports)
      os << "crate-type = [\"cdylib\"]\n";
  }
  // FR-53: the lints the old blanket allow header silenced are now DENIED, so
  // any regression in the emitter's warning-clean codegen fails `cargo build`.
  // `unused_assignments` joined the deny table once FR-61f's range-for lift
  // removed the last loop-body residual. The three naming lints are denied
  // only under the idiomatic rename -- `--preserve-c-names` keeps verbatim C
  // spellings, which legitimately trip them (allowed in the header instead).
  // FR-220: `dead_code` was the last lint still allowed crate-wide, and it left
  // the header WITHOUT joining this table -- it is the one lint deliberately
  // neither allowed nor denied. The item kinds that legitimately produce it (C
  // enumerators, unread fields, unconstructed records, uncalled C++ methods)
  // carry their own targeted `#[allow(dead_code)]`; a plain `fn` does NOT, so a
  // dead emitted function -- measured ZERO times across 294 epoch-7 crates, and
  // the one kind that would indicate an emitter defect -- now draws a rustc
  // warning. Denying it here was measured and rejected: c-testsuite -10, and it
  // breaks FR-44 because recovery structurally manufactures dead functions (see
  // `kAllowHeader`). The zero is held by the clippy-eval ratchet instead.
  os << "\n"
     << "[lints.rust]\n"
     << "unused_variables = \"deny\"\n"
     << "unused_assignments = \"deny\"\n"
     << "unused_mut = \"deny\"\n"
     << "unused_parens = \"deny\"\n"
     << "unpredictable_function_pointer_comparisons = \"deny\"\n";
  // FR-139: `non_snake_case` also applies to the CRATE NAME, which is the
  // caller's and not the emitter's -- a `--crate-name=Sieve` crate died on
  // `error: crate 'Sieve' should have a snake case name` requested by this
  // very table, and a CamelCase library name is exactly what a host looking
  // for `libSieve.so` needs. The tripwire relaxes EXACTLY where it must:
  // when the name itself would trip it, and nowhere else. The two sibling
  // lints never see the crate name, so they are never dropped.
  if (mlir::emitrust::idiomaticRenameEnabled()) {
    if (!crateNameTripsNonSnakeCase(crateName))
      os << "non_snake_case = \"deny\"\n";
    os << "non_upper_case_globals = \"deny\"\n"
       << "non_camel_case_types = \"deny\"\n";
  }
  // FR-62 slice 5c: the ASYNC crate flavor (E4) appends its tokio
  // dependency UNCONDITIONALLY — never behind a cargo feature, whose mere
  // declaration breaks `cargo build --offline` (the measured NO-GO) — and
  // everything above stays byte-identical to the default manifest. The
  // feature list is the measured minimum for the emitted shape: "rt"
  // (Builder + task::spawn), "sync" (mpsc + oneshot); no "macros" because
  // the main shim is the explicit Builder, not the attribute macro.
  if (asyncActorRuntime)
    os << "\n"
       << "[dependencies]\n"
       << "tokio = { version = \"1\", features = [\"rt\", \"sync\"] }\n";
  return toml;
}

mlir::FailureOr<std::string> renderCrateRoot(mlir::ModuleOp module,
                                             CrateType type,
                                             bool cAbiExports) {
  return renderCrateRoot(module, type, /*depCrates=*/{}, cAbiExports);
}

mlir::FailureOr<std::string>
renderCrateRoot(mlir::ModuleOp module, CrateType type,
                llvm::ArrayRef<std::string> depCrates, bool cAbiExports) {
  const bool wrapMain = type == CrateType::Bin;
  mlir::emitrust::RustEmitOptions emitOptions;
  // FR-51: only a library crate exports anything. A binary crate's items stay
  // private, which is both what they were and what keeps this rendering
  // byte-identical to every crate emitted before FR-51.
  emitOptions.exportItems = type == CrateType::Lib;
  // FR-139: the C-ABI shape is a property of an EXPORTED item, so it rides on
  // top of `exportItems` and is meaningless without it. The driver rejects
  // `--c-abi-exports` on a binary crate before ever getting here; the `&&`
  // makes the invariant local anyway.
  emitOptions.cAbiExports = cAbiExports && emitOptions.exportItems;
  std::string source;
  llvm::raw_string_ostream os(source);
  llvm::StringRef header = mlir::emitrust::idiomaticRenameEnabled()
                               ? kAllowHeader
                               : kAllowHeaderPreserveNames;
  if (depCrates.empty()) {
    // FR-220: the idiomatic header is EMPTY now that `dead_code` moved onto
    // the individual items. Emitting nothing (rather than a lone blank line)
    // keeps the crate root's first byte the first emitted item.
    if (!header.empty())
      os << header << "\n";
  } else {
    // FR-59 workspace member: the allow list additionally admits
    // unused_imports (a member gets every dependency it references
    // anywhere, not per item), and the glob imports follow — they are how
    // the emitter's bare cross-crate names resolve against FR-51's pubs.
    // FR-220: with an empty base header there is no list to extend, so the
    // member header is the single-lint allow.
    if (header.empty()) {
      os << "#![allow(unused_imports)]\n";
    } else {
      llvm::StringRef closer = ")]\n";
      os << header.drop_back(closer.size()) << ", unused_imports" << closer;
    }
    for (const std::string &dep : depCrates)
      os << "use " << dep << "::*;\n";
    os << "\n";
  }
  if (mlir::failed(mlir::emitrust::translateToRust(module, os, emitOptions)))
    return mlir::failure();
  if (wrapMain) {
    // FR-62 slice 5c: an async actor runtime anchor means `c_main` rendered
    // as an `async fn`, so the wrapper is the tokio current_thread shim.
    const bool asyncMain = hasAsyncActorRuntime(module);
    // C99-43 C3: 3-way select on `c_main`'s arity. Arity 0/1 stay
    // byte-identical to the pre-C3 emitter; arity 2 carries the admitted
    // argv table and gets the raw-bytes `args_os` collection wrapper.
    const unsigned arity = cMainInputCount(module);
    // FR-228: a crate carrying the buffered stdout runtime gets the wrapper
    // that installs and flushes it. A crate without the runtime keeps the
    // historical wrapper byte for byte -- nothing it emits can print, so
    // there is nothing to buffer and nothing to flush.
    std::string bufferedWrapper;
    llvm::StringRef wrapper;
    if (hasBufferedStdout(module)) {
      bufferedWrapper = renderBufferedMainWrapper(arity, asyncMain);
      wrapper = bufferedWrapper;
    } else {
      switch (arity) {
      case 2:
        wrapper = asyncMain ? kMainArgvWrapperAsync : kMainArgvWrapper;
        break;
      case 1:
        wrapper = asyncMain ? kMainArgcWrapperAsync : kMainArgcWrapper;
        break;
      default:
        wrapper = asyncMain ? kMainWrapperAsync : kMainWrapper;
        break;
      }
    }
    // FR-150: the wrapper text is rendered HERE, outside the emitter, and the
    // argv flavor spells `let __emitrust_argv: Vec<Vec<i8>>`. A crate whose
    // own items shadow `Vec` shadows it for the wrapper too, so the same
    // conditional qualification the emitter applies to its items applies to
    // this last block. Byte-neutral for every crate that shadows nothing --
    // `qualifyShadowedPreludeNames` returns the text unchanged on an empty
    // set, which is every crate in the corpus.
    os << "\n"
       << mlir::emitrust::qualifyShadowedPreludeNames(
              wrapper, mlir::emitrust::collectShadowedPreludeNames(module));
  }
  return source;
}

std::string renderMemberCargoToml(llvm::StringRef crateName, CrateType type,
                                  llvm::ArrayRef<std::string> depCrates) {
  std::string toml = renderCargoToml(crateName, type);
  if (depCrates.empty())
    return toml;
  llvm::raw_string_ostream os(toml);
  os << "\n[dependencies]\n";
  for (const std::string &dep : depCrates)
    os << dep << " = { path = \"../" << dep << "\" }\n";
  return toml;
}

std::string renderWorkspaceToml(llvm::ArrayRef<std::string> members) {
  std::string toml;
  llvm::raw_string_ostream os(toml);
  os << "[workspace]\n"
     << "resolver = \"2\"\n"
     << "members = [";
  for (auto [index, member] : llvm::enumerate(members))
    os << (index ? ", " : "") << "\"" << member << "\"";
  os << "]\n";
  return toml;
}

/// FR-160: is `fn` a RECOVERED STUB, i.e. does its body contain the
/// `unimplemented!` the `--recover` path emits for a construct it rejected?
/// Returns the rejection diagnostic when so, and "" otherwise.
///
/// Any occurrence counts, not just a whole-body one: a call that reaches an
/// `unimplemented!` panics, and a `#[test]` that panics fails. Wrapping such a
/// body is still worth doing -- `#[ignore]`d, it reports the gap by name in
/// `cargo test` output -- but it must never be counted as a passing test.
static std::string recoveredStubReason(mlir::emitrust::FuncOp fn) {
  std::string reason;
  fn.walk([&](mlir::emitrust::CallOpaqueOp call) {
    if (call.getCallee() != "unimplemented!" || !reason.empty())
      return;
    if (auto args = call->getAttrOfType<mlir::ArrayAttr>("args"))
      if (!args.empty())
        if (auto first = llvm::dyn_cast<mlir::StringAttr>(args[0]))
          reason = first.getValue().str();
    if (reason.empty())
      reason = "recovered: this item was not imported";
  });
  return reason;
}

namespace {

/// FR-160b: drops a leading `tu<N>_` internal-linkage tag, if there is one.
///
/// The tag is applied by `CSymbolNaming.h` to every `static` function, so an
/// entry point spelled as C wrote it can miss for that reason ALONE, with no
/// casing involved.
llvm::StringRef stripTuTag(llvm::StringRef name) {
  llvm::StringRef rest = name;
  if (!rest.consume_front("tu"))
    return name;
  llvm::StringRef digits =
      rest.take_while([](char c) { return c >= '0' && c <= '9'; });
  if (digits.empty())
    return name;
  rest = rest.drop_front(digits.size());
  if (!rest.consume_front("_"))
    return name;
  return rest;
}

/// FR-160b: the `emitrust.func` named `symbol` inside one of `module`'s
/// `emitrust.impl` blocks, together with the impl that owns it.
///
/// `emitrust.impl` carries the `SymbolTable` trait and is a direct child of
/// the `ModuleOp`, so the module-level `SymbolTable::lookupSymbolIn` this
/// file was built on structurally CANNOT see an arm the FR-62 actor lift
/// moved into one. That is what made the old `emitrust.method_of` arm dead
/// code twice over: the conversion both NESTS the func and STRIPS the
/// attribute the arm tested for, so the predicate was false on the one shape
/// it was written for and the caller was told "no function of that name in
/// this crate" -- a reason that is not true. One `getOps` loop and no walk,
/// because the nesting is exactly one level deep by construction.
std::pair<mlir::emitrust::ImplOp, mlir::emitrust::FuncOp>
lookupImplMethod(mlir::ModuleOp module, llvm::StringRef symbol) {
  for (auto impl : module.getOps<mlir::emitrust::ImplOp>())
    if (auto fn = llvm::dyn_cast_if_present<mlir::emitrust::FuncOp>(
            mlir::SymbolTable::lookupSymbolIn(impl, symbol)))
      return {impl, fn};
  return {mlir::emitrust::ImplOp(), mlir::emitrust::FuncOp()};
}

/// FR-160b: is `symbol` a member of this module's `Externals` trait?
///
/// This replaces an `isExternal()` arm that was UNREACHABLE by construction.
/// A referenced extern is lowered to an `Externals` trait member and never to
/// a module-level func; an unreferenced one is absent from the module
/// entirely; and a module-level body-less `emitrust.func` cannot be RENDERED
/// at all (the Rust translator refuses it, and the FR-52 marker contract
/// rejects a deferred external), while this function runs only after
/// rendering succeeded. So the only true statement left about an extern is
/// the one below, and it is made from the trait.
bool isExternalsTraitMember(mlir::ModuleOp module, llvm::StringRef symbol) {
  for (auto trait : module.getOps<mlir::emitrust::TraitDefOp>()) {
    if (trait.getSymName() != mlir::emitrust::kExternalsTraitName)
      continue;
    for (mlir::Attribute name : trait.getFnNames())
      if (llvm::cast<mlir::StringAttr>(name).getValue() == symbol)
        return true;
  }
  return false;
}

/// FR-160b: a function this crate DOES define whose emitted name is what the
/// C spelling `entry` would have become -- FR-53's idiomatic rename, the
/// `tu<N>_` internal-linkage tag, or both.
///
/// A HINT ONLY. Resolving the request through it was measured UNSOUND: two
/// translation units, one defining `foo_bar` and one defining `fooBar`, both
/// emit `foo_bar`, and a whole-project registry naming `fooBar` applied one
/// TU at a time -- FR-160's actual mode -- would then wrap the wrong
/// function. A wrong wrap in a differential oracle is a false RED or a false
/// GREEN, strictly worse than the skip it would replace. The tag is ambiguous
/// for the same reason under `--link`, where `tu0_test_foo` and
/// `tu1_test_foo` can both exist. So the candidate goes into the diagnostic
/// and nowhere else.
mlir::emitrust::FuncOp renameCandidate(mlir::ModuleOp module,
                                       llvm::StringRef entry) {
  const std::string wanted = mlir::emitrust::toSnakeCase(entry);
  auto matches = [&](mlir::emitrust::FuncOp fn) {
    llvm::StringRef name = fn.getSymName();
    return name != entry && stripTuTag(name) == wanted;
  };
  for (auto fn : module.getOps<mlir::emitrust::FuncOp>())
    if (matches(fn))
      return fn;
  // An actor-lifted arm can be renamed too, and a registry naming it must not
  // fall through to the bare "no function of that name" message.
  for (auto impl : module.getOps<mlir::emitrust::ImplOp>())
    for (auto fn : impl.getBody().getOps<mlir::emitrust::FuncOp>())
      if (matches(fn))
        return fn;
  return {};
}

/// FR-160b: why `entry` names no module-level function, as truthfully as this
/// module can say it.
///
/// `at` receives the operation to locate the diagnostic at, when there is a
/// real definition worth pointing the caller at, and is left alone otherwise.
/// A nonnull `at` also LIFTS the `--test-entries` file's silent-skip rule in
/// the driver's fold: the file's rationale is "another unit owns this
/// symbol", and a definition found right here is proof that it does not.
std::string describeMissingEntry(mlir::ModuleOp module, llvm::StringRef entry,
                                 mlir::Operation *&at) {
  auto [impl, method] = lookupImplMethod(module, entry);
  if (method) {
    at = method.getOperation();
    return "rendered as a method of impl '" + impl.getStructName().str() +
           "', not callable as a free function; the FR-62 actor lift moved it "
           "there because it touches a file-local global, and a "
           "Default-constructed receiver would not carry that global's C "
           "initializer";
  }
  if (isExternalsTraitMember(module, entry))
    return "declared in this unit but defined in another; link the shards to "
           "wrap it";
  if (mlir::emitrust::FuncOp candidate = renameCandidate(module, entry)) {
    at = candidate.getOperation();
    return "no function of that name in this crate; did you mean '" +
           candidate.getSymName().str() +
           "'? A test entry is spelled as EMITTED, and the rename is not "
           "reversed automatically because two C spellings can fold onto one "
           "Rust name";
  }
  return "no function of that name in this crate";
}

} // namespace

std::string renderTestModule(mlir::ModuleOp module,
                             llvm::ArrayRef<std::string> entries,
                             llvm::SmallVectorImpl<TestEntryReport> &reports) {
  struct Wrapped {
    /// The generated `fn`'s name: the request, verbatim.
    std::string symbol;
    /// The crate symbol it CALLS, which differs from the request only for the
    /// `main` -> `c_main` alias below.
    std::string callSymbol;
    bool returnsInt;
    std::string ignoreReason;
  };
  llvm::SmallVector<Wrapped> wrapped;

  for (const std::string &entry : entries) {
    TestEntryReport report;
    report.symbol = entry;
    // The symbol the generated test CALLS. The request itself, except for the
    // one alias below.
    std::string callSymbol = entry;
    auto fn = llvm::dyn_cast_if_present<mlir::emitrust::FuncOp>(
        mlir::SymbolTable::lookupSymbolIn(module, entry));
    if (!fn && entry == "main") {
      // FR-160b: `main` is the ONE C name the emitter renames
      // UNCONDITIONALLY -- `--preserve-c-names` does not turn it off -- and
      // `c_main` is a RESERVED name: a unit defining both is rejected with a
      // located error, so the mapping is injective on every path and this
      // retry cannot reach the wrong function. That injectivity is exactly
      // what the casing and `tu<N>_` aliases lack, which is why they hint
      // and this one wraps.
      //
      // It is also the whole of the motivating path:
      // `scripts/test-entries-meson.py`'s default mode writes the literal
      // symbol `main` for every one-TU test, and consumed through
      // `--test-entries` that yielded zero tests and zero words.
      if (auto renamed = llvm::dyn_cast_if_present<mlir::emitrust::FuncOp>(
              mlir::SymbolTable::lookupSymbolIn(module,
                                                llvm::StringRef("c_main")))) {
        fn = renamed;
        callSymbol = "c_main";
      }
    }
    if (!fn) {
      // NOT necessarily an error: an entries file describes a whole project
      // and is applied one translation unit at a time. But the reason must be
      // TRUE, and "no function of that name in this crate" was being told to
      // callers whose function this crate does define -- under the emitted
      // spelling, as an actor arm, or as an `Externals` trait member.
      report.skipReason = describeMissingEntry(module, entry, report.op);
      reports.push_back(std::move(report));
      continue;
    }
    report.op = fn.getOperation();
    if (fn->hasAttr(mlir::emitrust::kExternalsGenericAttrName)) {
      report.skipReason =
          "generic over the Externals trait -- its callees are undefined in "
          "a solo-TU import, so the test could only panic inside a trait "
          "method; link the shards first";
    } else if (fn.getFunctionType().getNumInputs() != 0) {
      report.skipReason = "takes arguments; a test entry point must take none";
    } else {
      llvm::ArrayRef<mlir::Type> results = fn.getFunctionType().getResults();
      const bool returnsNothing = results.empty();
      const bool returnsInt = results.size() == 1 && results[0].isIntOrIndex();
      if (!returnsNothing && !returnsInt) {
        report.skipReason =
            "returns a value that is not an exit code; a test entry point "
            "must return an integer or nothing";
      } else {
        report.ignoreReason = recoveredStubReason(fn);
        wrapped.push_back({entry, callSymbol, returnsInt, report.ignoreReason});
      }
    }
    reports.push_back(std::move(report));
  }

  if (wrapped.empty())
    return "";

  std::string text;
  llvm::raw_string_ostream os(text);
  // `non_snake_case` is denied by the emitted manifest and a C test symbol
  // need not be snake case; the allow is scoped to this generated module so
  // the deny still governs every item the emitter itself writes.
  os << "\n#[cfg(test)]\n#[allow(non_snake_case)]\nmod emitrust_tests {\n";
  for (auto [index, entry] : llvm::enumerate(wrapped)) {
    if (index)
      os << "\n";
    os << "    #[test]\n";
    if (!entry.ignoreReason.empty()) {
      os << "    #[ignore = \"";
      // A diagnostic is emitter-authored prose, but it can quote C source.
      for (char c : entry.ignoreReason) {
        if (c == '"' || c == '\\')
          os << '\\';
        os << c;
      }
      os << "\"]\n";
    }
    // The `fn` is named for the REQUEST and calls the resolved symbol, so
    // requesting both `main` and `c_main` yields two distinctly named
    // wrappers rather than the rustc E0428 two identical ones would be.
    os << "    fn " << entry.symbol << "() {\n";
    if (entry.returnsInt)
      os << "        assert_eq!(super::" << entry.callSymbol << "(), 0);\n";
    else
      os << "        super::" << entry.callSymbol << "();\n";
    os << "    }\n";
  }
  os << "}\n";
  return text;
}

} // namespace emitrustcc
