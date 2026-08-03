//===- emitrust-clang.cpp - Compiler-shim front end -------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file is the FR-56 compiler-shim front end `emitrust-clang`. It is
/// meant to be dropped into an unmodified build system as the C compiler
/// (`make CC=emitrust-clang`, or through a `cc`/`gcc`-named symlink —
/// argv[0] is parsed with clang's own program-name mechanism and its
/// driver-mode/target implications are honored on both the classification
/// and the delegation): every invocation is DELEGATED verbatim to the
/// real clang — so object files, configure probes, `--version` checks,
/// preprocessing, depfile generation, and the final native link all behave
/// exactly as a plain-clang build — and, additionally, each driver
/// invocation that compiles a C source with `-c` side-emits one
/// `<object>.emitrust.mlirbc` artifact holding the EmitRust import of that
/// translation unit, lowered through the pinned emitrust-cc pipeline to the
/// converted emitrust-dialect module, serialized as MLIR bytecode (the
/// FR-57 per-TU parse cache) with the TU's item-graph shard and
/// rejection-ledger entries attached as module attributes
/// (EmitRust/ShardMetadata.h). The same payload is then embedded into the
/// just-produced object file as a non-alloc `.emitrust` ELF section (the
/// gllvm model), so `ar` archives and existing link lines carry it with no
/// build-system cooperation; the sidecar file stays as the non-ELF
/// fallback.
///
/// Argument classification is done with clang's OWN driver, never a
/// hand-rolled filter: the argv is handed to `clang::driver::Driver`, and
/// the `-cc1` frontend jobs of the resulting `Compilation` are the
/// principled definition of "affects the Rust result". The FR-57 cache key
/// is the PAIR logged when `EMITRUST_CLANG_LOG` names a log file:
/// `cc1-key`, the llvm::MD5 of the CANONICALIZED cc1 argument vector
/// (workflow-only arguments and the input path stripped, see Cc1Key.h), and
/// `src-hash`, the order-independent content hash of the main file PLUS
/// every user header it transitively includes — the dependency list is
/// collected by replaying the cc1 line through clang's preprocessor
/// in-process (DepScan.h), never by parsing the build's own depfile — so a
/// header-only edit misses the key while an mtime touch or an `-MF` rename
/// keeps it. A dependency that cannot be scanned or read fails toward "no
/// artifact + warning", never a wrong cache hit; the test-only
/// `EMITRUST_TEST_UNREADABLE_DEP` environment hook (path or filename)
/// forces that failure so the direction stays pinned.
///
/// The shim's cardinal rule is that the IMPORT NEVER FAILS THE BUILD: an
/// import error is logged (and warned about on stderr) and the exit code of
/// the real clang is returned unchanged. That rule covers CRASHES too: the
/// whole artifact side-emission runs in a forked child per compile job
/// (`runIsolatedSideEmit`), so an importer abort costs one TU's artifact —
/// with any partial sidecar removed — never the build; the test-only
/// `EMITRUST_TEST_CRASH_IMPORT` env hook forces that crash. The real
/// compiler to delegate to is `EMITRUST_REAL_CC` when set, else `clang`
/// found on PATH.
//
//===----------------------------------------------------------------------===//

#include "Cc1Key.h"
#include "DepScan.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/ImportC.h"
#include "EmitRust/Project/ItemGraph.h"
#include "EmitRust/ShardMetadata.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/ToolChain.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ObjCopy/ConfigManager.h"
#include "llvm/ObjCopy/ObjCopy.h"
#include "llvm/Object/Binary.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

/// One `-cc1` frontend job that compiles a C source to an object file: the
/// unit of FR-57 side-emission. Everything the import needs is copied out of
/// the driver's Compilation here, because the Compilation (and the Driver
/// that owns its strings) dies before the import runs.
struct CompileJob {
  /// The C source file the cc1 job parses.
  std::string input;
  /// The object file the cc1 job writes; the artifact lands beside it as
  /// `<output>.emitrust.mlirbc` and is embedded into it as the `.emitrust`
  /// section.
  std::string output;
  /// The semantic clang driver arguments recovered from the cc1 line
  /// (include paths, macro definitions, language standard), in cc1 order.
  std::vector<std::string> importArgs;
  /// Hex MD5 of the CANONICALIZED cc1 argument vector (Cc1Key.h): the
  /// command-line half of the FR-57 cache key. Workflow-only arguments and
  /// the input path are stripped before hashing, so the key is invariant
  /// under output/depfile renames while remaining independent of
  /// response-file packaging and argv spelling differences the driver
  /// normalizes away.
  std::string cc1Hash;
  /// The FULL cc1 argument vector (leading "-cc1" included), copied out of
  /// the Compilation because the src-hash's dependency scan (DepScan.h)
  /// replays it through the preprocessor after the delegation, when the
  /// Compilation is long gone.
  std::vector<std::string> cc1Args;
  /// FR-56: target/ABI cc1 flags in the measured CANNOT-HONOR set
  /// (`-fpack-struct[=N]` today: the importer models no struct packing and
  /// the emitted Rust has no repr story). A nonempty list rejects the WHOLE
  /// translation unit into the artifact's ledger — an empty module carrying
  /// the located rejection — because importing with a layout the flag
  /// changed and the import ignored would be silently wrong code.
  std::vector<std::string> unsupportedAbiFlags;
};

/// The optional shim log sink (`EMITRUST_CLANG_LOG`). Append-mode so that
/// the many compiler invocations of one `make` run accumulate into a single
/// readable transcript.
class ShimLog {
public:
  ShimLog() {
    if (const char *path = std::getenv("EMITRUST_CLANG_LOG")) {
      std::error_code ec;
      stream = std::make_unique<llvm::raw_fd_ostream>(
          path, ec, llvm::sys::fs::OF_Append | llvm::sys::fs::OF_Text);
      if (ec)
        stream.reset();
    }
  }

  /// The log stream, or null when logging is disabled.
  llvm::raw_ostream *os() { return stream.get(); }

private:
  std::unique_ptr<llvm::raw_fd_ostream> stream;
};

} // namespace

/// Resolves the real compiler to delegate to: `EMITRUST_REAL_CC` when set
/// (resolved on PATH if it is a bare name), else `clang` on PATH.
///
/// \returns the absolute path of the real compiler, or std::nullopt with an
///          error already printed to stderr.
static std::optional<std::string> findRealCompiler() {
  llvm::StringRef requested = "clang";
  if (const char *env = std::getenv("EMITRUST_REAL_CC"))
    requested = env;
  if (llvm::sys::fs::can_execute(requested))
    return requested.str();
  llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName(requested);
  if (!found) {
    llvm::errs() << "emitrust-clang: error: cannot find real compiler '"
                 << requested << "': " << found.getError().message()
                 << " (set EMITRUST_REAL_CC)\n";
    return std::nullopt;
  }
  return *found;
}

/// Classifies the (response-file-expanded) argv with clang's own driver and
/// collects every `-cc1` job that compiles a C source to an object file.
///
/// The driver is run with an ignoring diagnostic consumer: any diagnostics
/// the invocation deserves will be produced — once — by the real clang the
/// shim delegates to, so the classification pass must stay silent to keep
/// stderr transparent. A classification failure returns an empty list; it
/// never blocks the delegation.
///
/// \param args the expanded argv, argv[0] included.
/// \param realCC the real compiler path, used as the driver's notion of the
///        executable so resource-dir and toolchain deduction match it.
/// \param targetAndMode the FR-56 argv0 aliasing facts: what clang's own
///        program-name parsing derived from the name the shim was invoked
///        under (`cc`, `gcc`, `g++`, a target-prefixed cross name, ...),
///        applied to the driver exactly as clang's main applies it.
/// \param log the optional shim log, receiving each cc1 line and its hash.
/// \returns the C compile jobs found; empty when there are none.
static std::vector<CompileJob>
classifyCompileJobs(llvm::ArrayRef<const char *> args, llvm::StringRef realCC,
                    const clang::driver::ParsedClangName &targetAndMode,
                    llvm::raw_ostream *log) {
  std::vector<CompileJob> jobs;

  clang::DiagnosticOptions diagOpts;
  clang::IgnoringDiagConsumer silentConsumer;
  clang::DiagnosticsEngine diags(
      llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), diagOpts,
      &silentConsumer, /*ShouldOwnClient=*/false);
  clang::driver::Driver driver(realCC, llvm::sys::getDefaultTargetTriple(),
                               diags);
  driver.setTargetAndMode(targetAndMode);
  driver.setCheckInputsExist(false);

  std::unique_ptr<clang::driver::Compilation> compilation(
      driver.BuildCompilation(args));
  if (!compilation)
    return jobs;

  for (const clang::driver::Command &command : compilation->getJobs()) {
    const llvm::opt::ArgStringList &cc1 = command.getArguments();
    if (cc1.empty() || llvm::StringRef(cc1.front()) != "-cc1")
      continue;

    CompileJob job;
    bool emitsObject = false;
    std::string language;
    for (size_t i = 0, e = cc1.size(); i != e; ++i) {
      llvm::StringRef arg(cc1[i]);
      if (arg == "-emit-obj") {
        emitsObject = true;
      } else if (arg == "-o" && i + 1 != e) {
        job.output = cc1[++i];
      } else if (arg == "-x" && i + 1 != e) {
        language = cc1[++i];
      } else if (arg == "-I" && i + 1 != e) {
        job.importArgs.push_back(("-I" + llvm::StringRef(cc1[++i])).str());
      } else if (arg.starts_with("-I")) {
        job.importArgs.push_back(arg.str());
      } else if (arg == "-D" && i + 1 != e) {
        job.importArgs.push_back(("-D" + llvm::StringRef(cc1[++i])).str());
      } else if (arg.starts_with("-D") && arg.size() > 2) {
        job.importArgs.push_back(arg.str());
      } else if (arg == "-isystem" && i + 1 != e) {
        job.importArgs.push_back("-isystem");
        job.importArgs.push_back(cc1[++i]);
      } else if ((arg == "-include" || arg == "-idirafter") && i + 1 != e) {
        // Semantic preprocessor inputs, same class as -D: a kernel TU's
        // semantics live behind `-include compiler_types.h`, so dropping
        // the flag would import a different program than the one the real
        // compile built.
        job.importArgs.push_back(arg.str());
        job.importArgs.push_back(cc1[++i]);
      } else if (arg.starts_with("-std=")) {
        job.importArgs.push_back(arg.str());
      } else if (arg == "-triple" && i + 1 != e) {
        // FR-56: the cc1 triple is the truth about the target the real
        // compile laid types out for (`-target`, `-m32` and friends all
        // land here), and the import must reflect it. Forwarded as the
        // driver-level `--target=` since the import drives its own clang
        // driver; for a host compile this is the default triple and a
        // measured no-op.
        job.importArgs.push_back(
            ("--target=" + llvm::StringRef(cc1[++i])).str());
      } else if (arg == "-fshort-enums" || arg == "-fno-signed-char" ||
                 arg == "-fsigned-char") {
        // FR-56: layout-affecting ABI flags the import pipeline HONORS —
        // the clang AST applies them, so sizeof folds and type mappings
        // follow (pinned in test/Driver/emitrust-clang-target-abi.c). The
        // cc1 spellings are also valid driver spellings.
        job.importArgs.push_back(arg.str());
      } else if (arg == "-fpack-struct" || arg.starts_with("-fpack-struct=")) {
        // FR-56: the measured cannot-honor set; see the field comment.
        job.unsupportedAbiFlags.push_back(arg.str());
      } else if (!arg.starts_with("-") && !language.empty()) {
        // cc1 places its single input after the `-x <language>` pair; any
        // earlier bare argument is an option value already consumed above.
        job.input = arg.str();
      }
    }

    llvm::MD5 md5;
    for (const std::string &arg :
         emitrust::canonicalizeCc1Args(llvm::ArrayRef<const char *>(
             cc1.data(), cc1.size()))) {
      md5.update(arg);
      md5.update(llvm::StringRef("\0", 1));
    }
    llvm::MD5::MD5Result digest = md5.final();
    llvm::SmallString<32> hex;
    llvm::MD5::stringifyResult(digest, hex);
    job.cc1Hash = std::string(hex);

    job.cc1Args.assign(cc1.begin(), cc1.end());

    if (log) {
      *log << "cc1:";
      for (const char *arg : cc1)
        *log << " " << arg;
      *log << "\ncc1-key: " << job.cc1Hash << "\n";
    }

    if (emitsObject && language == "c" && !job.input.empty() &&
        !job.output.empty())
      jobs.push_back(std::move(job));
  }
  return jobs;
}

/// Runs the pinned emitrust-cc lowering pipeline on `module`:
/// lower-containers, mem2reg, canonicalize, lift-cf-to-scf, canonicalize,
/// convert-to-emitrust — so the artifact holds the fully converted
/// emitrust-dialect module (`emitrust.func` bodies), the same stage
/// `emitrust-cc --emit=mlir` writes and the stage the FR-58 link step will
/// materialize Rust from.
static mlir::LogicalResult runPipeline(mlir::ModuleOp module) {
  mlir::PassManager pm(module.getContext(),
                       mlir::ModuleOp::getOperationName());
  pm.addPass(mlir::emitrust::createEmitRustLowerContainers());
  pm.addPass(mlir::createMem2Reg());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createLiftControlFlowToSCFPass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::emitrust::createConvertToEmitRust());
  return pm.run(module);
}

/// Embeds the bytecode artifact at `payloadPath` into the object file at
/// `objectPath` as a `.emitrust` section, using LLVM's objcopy-as-a-library
/// (the exact engine behind `llvm-objcopy --add-section`, so the section is
/// non-alloc by default and the spike's link/`ar` survival results carry
/// over). The rewritten object is staged to a temporary file and renamed
/// into place so a failure never leaves a truncated `.o`.
///
/// Embedding failure must NOT fail the build (nor even remove the sidecar,
/// which doubles as the non-ELF fallback): every error path warns and
/// returns.
static void embedArtifactSection(llvm::StringRef objectPath,
                                 llvm::StringRef payloadPath,
                                 llvm::raw_ostream *log) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> payload =
      llvm::MemoryBuffer::getFile(payloadPath);
  if (!payload) {
    llvm::errs() << "emitrust-clang: warning: cannot read '" << payloadPath
                 << "' for embedding: " << payload.getError().message()
                 << "\n";
    return;
  }

  llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binary =
      llvm::object::createBinary(objectPath);
  if (!binary) {
    llvm::errs() << "emitrust-clang: warning: cannot open '" << objectPath
                 << "' for embedding: "
                 << llvm::toString(binary.takeError()) << "\n";
    return;
  }

  llvm::objcopy::ConfigManager config;
  config.Common.InputFilename = objectPath;
  config.Common.OutputFilename = objectPath;
  config.Common.AddSection.emplace_back(".emitrust", std::move(*payload));

  llvm::SmallString<256> stagedPath(objectPath);
  stagedPath += ".emitrust-stage";
  std::error_code ec;
  {
    llvm::raw_fd_ostream out(stagedPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
      llvm::errs() << "emitrust-clang: warning: cannot write '" << stagedPath
                   << "': " << ec.message() << "\n";
      return;
    }
    if (llvm::Error error = llvm::objcopy::executeObjcopyOnBinary(
            config, *binary->getBinary(), out)) {
      llvm::errs() << "emitrust-clang: warning: cannot embed .emitrust "
                      "section into '"
                   << objectPath << "': " << llvm::toString(std::move(error))
                   << "\n";
      out.close();
      llvm::sys::fs::remove(stagedPath);
      return;
    }
  }
  if ((ec = llvm::sys::fs::rename(stagedPath, objectPath))) {
    llvm::errs() << "emitrust-clang: warning: cannot rename '" << stagedPath
                 << "' over '" << objectPath << "': " << ec.message() << "\n";
    llvm::sys::fs::remove(stagedPath);
    return;
  }
  if (log)
    *log << "embedded: .emitrust section in " << objectPath << "\n";
}

/// Computes and logs the `src-hash` half of the FR-57 key for one compile
/// job: the dependency list is collected by replaying the job's cc1 line
/// through the preprocessor in-process (DepScan.h), and the CONTENTS of the
/// main file plus every user header are hashed as an order-independent
/// digest multiset.
///
/// Runs after the delegation on purpose: the scan replays the exact cc1
/// line the real compile ran, and its failure must cost only the artifact.
///
/// \returns true with the hash logged as `src-hash: <hex>`; false after a
///          stderr warning when the scan fails or a dependency cannot be
///          read — the caller must then emit NO artifact, because a key
///          that missed a dependency could later be a wrong cache hit.
static bool computeAndLogSrcHash(const CompileJob &job,
                                 llvm::raw_ostream *log) {
  std::optional<std::vector<std::string>> deps =
      emitrust::scanCompileDependencies(job.cc1Args);
  if (!deps) {
    llvm::errs() << "emitrust-clang: warning: dependency scan failed for '"
                 << job.input << "'; no artifact written\n";
    if (log)
      *log << "src-hash-unavailable: dependency scan failed for " << job.input
           << "\n";
    return false;
  }
  std::optional<std::string> testUnreadable;
  if (const char *env = std::getenv("EMITRUST_TEST_UNREADABLE_DEP"))
    testUnreadable = env;
  std::string unreadablePath;
  std::optional<std::string> hash =
      emitrust::hashDependencyContents(*deps, testUnreadable, unreadablePath);
  if (!hash) {
    llvm::errs() << "emitrust-clang: warning: cannot hash dependency '"
                 << unreadablePath << "' for '" << job.input
                 << "'; no artifact written\n";
    if (log)
      *log << "src-hash-unavailable: cannot read " << unreadablePath << "\n";
    return false;
  }
  if (log)
    *log << "src-hash: " << *hash << "\n";
  return true;
}

/// The job's input path made absolute for the artifact's `emitrust.source`
/// record: the link step re-imports from whatever directory the link runs
/// in, so a cwd-relative path the build used would dangle there.
static std::string absoluteInputPath(const CompileJob &job) {
  llvm::SmallString<256> path(job.input);
  llvm::sys::fs::make_absolute(path);
  return std::string(path);
}

/// Serializes `module` as MLIR bytecode to `<output>.emitrust.mlirbc` and
/// embeds that payload into the object file as its `.emitrust` section (the
/// sidecar stays as the non-ELF fallback and the easy-inspection path).
/// Every failure warns and returns; the build's outcome is never touched.
static void writeAndEmbedArtifact(mlir::ModuleOp module, const CompileJob &job,
                                  llvm::raw_ostream *log) {
  std::string artifactPath = job.output + ".emitrust.mlirbc";
  {
    std::error_code ec;
    llvm::raw_fd_ostream out(artifactPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
      llvm::errs() << "emitrust-clang: warning: cannot write '" << artifactPath
                   << "': " << ec.message() << "\n";
      return;
    }
    if (mlir::failed(mlir::writeBytecodeToFile(module, out))) {
      llvm::errs() << "emitrust-clang: warning: bytecode serialization "
                      "failed for '"
                   << artifactPath << "'\n";
      out.close();
      llvm::sys::fs::remove(artifactPath);
      return;
    }
  }
  if (log)
    *log << "artifact: " << artifactPath << " (key " << job.cc1Hash << ")\n";

  embedArtifactSection(job.output, artifactPath, log);
}

/// FR-56: the artifact for a translation unit whose cc1 line carries a
/// target/ABI flag the import CANNOT honor (`CompileJob::
/// unsupportedAbiFlags`): an EMPTY module whose ledger records one located
/// whole-TU rejection per flag. Importing anyway would bake a layout the
/// real compile did not use — silently wrong code — while emitting no
/// artifact at all would lose the reason; this way the FR-58 link step
/// surfaces the rejection shard-attributed, and any symbol another TU
/// needs from this one fails loudly at merge. The item-graph attribute is
/// attached EMPTY (not omitted): the TU deliberately contributes no items,
/// which is different from an artifact that predates metadata support.
static void emitRejectedTuArtifact(const CompileJob &job,
                                   llvm::raw_ostream *log) {
  mlir::MLIRContext context;
  mlir::Location loc = mlir::FileLineColLoc::get(
      mlir::StringAttr::get(&context, job.input), 1, 1);
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::ModuleOp::create(loc);
  mlir::emitrust::RejectionLedger ledger;
  for (const std::string &flag : job.unsupportedAbiFlags) {
    std::string diagnostic =
        "unsupported target/ABI flag '" + flag +
        "': the import cannot honor it, so the translation unit is rejected";
    llvm::errs() << "emitrust-clang: warning: unsupported target/ABI flag '"
                 << flag << "' for '" << job.input
                 << "'; translation unit rejected into the artifact's "
                    "ledger\n";
    if (log)
      *log << "abi-reject: " << flag << " for " << job.input << "\n";
    ledger.record(mlir::emitrust::RejectedItem{
        "<translation unit>", loc, diagnostic,
        mlir::emitrust::classifyBlocker(diagnostic, loc), /*stubbed=*/false,
        ""});
  }
  mlir::emitrust::attachShardMetadata(*module, "", ledger.getItems(),
                                      absoluteInputPath(job), job.importArgs);
  writeAndEmbedArtifact(*module, job, log);
}

/// Runs the EmitRust import + lowering pipeline on one compile job, writes
/// the converted module as MLIR bytecode to `<output>.emitrust.mlirbc`, and
/// embeds that payload into the object file as its `.emitrust` section (the
/// sidecar stays as the non-ELF fallback and the easy-inspection path).
///
/// Import diagnostics are captured into the shim log rather than stderr so
/// the build transcript stays byte-transparent; only a one-line warning is
/// printed when the import fails. A failure of any kind — import, pipeline,
/// file write, or embedding — is logged and swallowed: the build's outcome
/// belongs to the real clang alone.
static void sideEmitArtifact(const CompileJob &job, llvm::raw_ostream *log) {
  // FR-56: a cc1 target/ABI flag the import cannot honor rejects the whole
  // TU into the artifact's ledger instead of importing with wrong layout.
  if (!job.unsupportedAbiFlags.empty()) {
    emitRejectedTuArtifact(job, log);
    return;
  }

  mlir::MLIRContext context;
  std::string diagnostics;
  llvm::raw_string_ostream diagStream(diagnostics);
  context.getDiagEngine().registerHandler([&](mlir::Diagnostic &diag) {
    diagStream << diag.str() << "\n";
    for (mlir::Diagnostic &note : diag.getNotes())
      diagStream << "  note: " << note.str() << "\n";
  });

  context.loadDialect<mlir::scf::SCFDialect, mlir::ub::UBDialect>();

  mlir::emitrust::ImportOptions options;
  // Recover-or-skip posture: an unsupported item must cost at most itself,
  // never the artifact, and the artifact must never cost the build.
  options.recover = true;
  // FR-57d: the recovered rejections ride the artifact as module
  // attributes, so the FR-58 link step can identify this TU's fact-starved
  // items without re-parsing any C.
  mlir::emitrust::RejectionLedger ledger;
  options.ledger = &ledger;
  // FR-57a: this TU is one shard of a project whose other TUs the shim
  // never sees, so a referenced external the TU does not define is a
  // link-time obligation recorded in the artifact (a declaration stub
  // marked `emitrust.extern_decl` for the FR-58 link step), not an
  // import failure that would cost the whole artifact.
  options.deferExternals = true;
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::emitrust::importC(job.input, job.importArgs, options, context);

  if (log && !diagnostics.empty())
    *log << "import-diagnostics for " << job.input << ":\n" << diagnostics;

  if (!module || mlir::failed(runPipeline(*module))) {
    llvm::errs() << "emitrust-clang: warning: emitrust "
                 << (module ? "lowering" : "import") << " failed for '"
                 << job.input << "'; no artifact written\n";
    if (log)
      *log << (module ? "pipeline" : "import") << ": FAILED for " << job.input
           << "\n"
           << diagnostics;
    return;
  }

  // FR-57d: the artifact also carries the TU's item-graph shard and the
  // rejections the recovering import just accumulated, as module attributes
  // (ShardMetadata.h) — one payload, one section, one bytecode round-trip.
  // The graph is a second, purely analytical parse of the same TU through
  // the same clang shell the import used, so its node keys are the module's
  // emitted symbols by construction; if that parse fails after the import
  // succeeded, something is genuinely wrong and the safe direction is no
  // artifact, never an artifact missing the facts FR-58 selects by.
  mlir::FailureOr<mlir::emitrust::ItemGraph> graph =
      mlir::emitrust::buildItemGraph(
          llvm::ArrayRef<std::string>(job.input), job.importArgs);
  if (mlir::failed(graph)) {
    llvm::errs() << "emitrust-clang: warning: item-graph construction "
                    "failed for '"
                 << job.input << "'; no artifact written\n";
    if (log)
      *log << "item-graph: FAILED for " << job.input << "\n";
    return;
  }
  mlir::emitrust::attachShardMetadata(*module, graph->print(),
                                      ledger.getItems(),
                                      absoluteInputPath(job), job.importArgs);

  writeAndEmbedArtifact(*module, job, log);
}

/// FR-56 import-crash isolation: runs the whole artifact side-emission for
/// one compile job — src-hash dependency scan, import, pipeline,
/// serialization, embedding — in a FORKED child, so a crash anywhere in it
/// (importer assertion, clang parser abort, MLIR verifier trap) is
/// observed by the parent as an abnormal child exit instead of killing the
/// build. The real compile was already delegated separately, so isolation
/// only ever guards the side-emission; on a crash the parent warns,
/// removes any partial sidecar (a child dying mid-serialization must not
/// leave a truncated artifact a later link would trip over), and the
/// delegated clang's exit code stays the shim's exit code.
///
/// The test-only `EMITRUST_TEST_CRASH_IMPORT` env hook aborts the child
/// before the scan, pinning the crash path
/// (test/Driver/emitrust-clang-crash-isolation.c); no cheap C input
/// crashes the importer on demand.
///
/// A fork failure (resource exhaustion) falls back to in-process emission:
/// losing isolation is recoverable, silently losing every artifact is not.
static void runIsolatedSideEmit(const CompileJob &job,
                                llvm::raw_ostream *log) {
  // Flush inherited buffers BEFORE forking, or the child's copy of the
  // buffered log/stderr bytes would be written twice.
  if (log)
    log->flush();
  llvm::errs().flush();

  pid_t child = fork();
  if (child == 0) {
    if (std::getenv("EMITRUST_TEST_CRASH_IMPORT"))
      abort();
    if (computeAndLogSrcHash(job, log))
      sideEmitArtifact(job, log);
    if (log)
      log->flush();
    llvm::errs().flush();
    _exit(0);
  }
  if (child < 0) {
    if (computeAndLogSrcHash(job, log))
      sideEmitArtifact(job, log);
    return;
  }
  int status = 0;
  if (waitpid(child, &status, 0) < 0 ||
      !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    llvm::errs() << "emitrust-clang: warning: emitrust artifact emission "
                    "crashed for '"
                 << job.input << "'; no artifact written\n";
    if (log)
      *log << "side-emit: CRASHED for " << job.input << "\n";
    llvm::sys::fs::remove(job.output + ".emitrust.mlirbc");
  }
}

/// Shim entry point: expand response files, classify with the clang driver,
/// delegate the ORIGINAL argv to the real clang, side-emit one artifact per
/// C compile job, and return the real clang's exit code unchanged.
int main(int argc, char **argv) {
  llvm::InitLLVM initLlvm(argc, argv);
  // FR-53: match emitrust-cc's default naming so the artifact and a later
  // emitrust-cc run over the same project agree symbol for symbol.
  mlir::emitrust::idiomaticRenameEnabled() = true;

  ShimLog shimLog;
  llvm::raw_ostream *log = shimLog.os();

  std::optional<std::string> realCC = findRealCompiler();
  if (!realCC)
    return 1;

  // FR-56 argv0 aliasing: `make CC=cc` (or gcc, g++, a target-prefixed
  // cross name) with the name symlinked to the shim must behave the way
  // clang invoked under that name would. The name is parsed with clang's
  // OWN mechanism — the same ends-with suffix table clang's main consults —
  // never a hand-rolled table; the result feeds the classification driver
  // below and is re-inserted into the delegated argv, because the
  // subprocess runs under the real clang's own name and would otherwise
  // lose the alias.
  clang::driver::ParsedClangName targetAndMode =
      clang::driver::ToolChain::getTargetAndModeFromProgramName(argv[0]);

  // Response-file expansion, for the shim's OWN classification only; the
  // delegation below forwards the original argv untouched and lets the real
  // clang expand for itself.
  llvm::BumpPtrAllocator allocator;
  llvm::StringSaver saver(allocator);
  llvm::SmallVector<const char *, 64> expandedArgs(argv, argv + argc);
  llvm::cl::ExpansionContext expansion(allocator,
                                       llvm::cl::TokenizeGNUCommandLine);
  if (llvm::Error error = expansion.expandResponseFiles(expandedArgs)) {
    // A malformed response file is the real clang's diagnostic to make, not
    // ours; skip classification and still delegate.
    if (log)
      *log << "response-file expansion failed: "
           << llvm::toString(std::move(error)) << "\n";
    expandedArgs.assign(argv, argv + argc);
  }

  // Mirror clang's own main exactly: the name-derived driver mode is an
  // ARGUMENT the driver reads back out of argv (`setTargetAndMode` alone
  // carries only the name parts), so it is inserted right after argv[0]
  // before classification.
  if (targetAndMode.DriverMode)
    expandedArgs.insert(expandedArgs.begin() + 1, targetAndMode.DriverMode);

  if (log) {
    *log << "invocation:";
    for (const char *arg : expandedArgs)
      *log << " " << arg;
    *log << "\n";
  }

  // Classify only invocations that can possibly be a `-c` compile. The gate
  // matters for correctness, not just cost: BuildCompilation handles
  // immediate arguments such as --version and -print-search-dirs by PRINTING
  // to stdout, which would corrupt the transparent pass-through.
  bool hasDashC = llvm::any_of(expandedArgs, [](const char *arg) {
    return llvm::StringRef(arg) == "-c";
  });
  std::vector<CompileJob> jobs;
  if (hasDashC)
    jobs = classifyCompileJobs(expandedArgs, *realCC, targetAndMode, log);

  // Delegate to the real clang with the ORIGINAL argv (argv[0] replaced), so
  // every invocation shape — compile, link, -E, -M*, --version, configure
  // probes — behaves exactly as a plain-clang build. Stdout/stderr are
  // inherited, never captured. The argv0-derived driver mode and target
  // prefix are re-inserted FIRST (an explicit user flag later on the line
  // still wins, matching clang's own precedence for name-derived defaults).
  llvm::SmallVector<llvm::StringRef, 64> delegatedArgs;
  delegatedArgs.push_back(*realCC);
  std::string aliasTargetFlag;
  if (targetAndMode.DriverMode)
    delegatedArgs.push_back(targetAndMode.DriverMode);
  if (targetAndMode.TargetIsValid) {
    aliasTargetFlag = "--target=" + targetAndMode.TargetPrefix;
    delegatedArgs.push_back(aliasTargetFlag);
  }
  for (int i = 1; i < argc; ++i)
    delegatedArgs.push_back(argv[i]);
  std::string execError;
  bool executionFailed = false;
  int exitCode = llvm::sys::ExecuteAndWait(
      *realCC, delegatedArgs, /*Env=*/std::nullopt, /*Redirects=*/{},
      /*SecondsToWait=*/0, /*MemoryLimit=*/0, &execError, &executionFailed);
  if (executionFailed || exitCode < 0) {
    llvm::errs() << "emitrust-clang: error: cannot execute '" << *realCC
                 << "'";
    if (!execError.empty())
      llvm::errs() << ": " << execError;
    llvm::errs() << "\n";
    return exitCode > 0 ? exitCode : 1;
  }

  // Side-emit only after a SUCCESSFUL real compile: a TU the real compiler
  // rejected has no object file for the artifact to ride with. Each job's
  // emission runs in its own forked child (import-crash isolation); inside
  // it the src-hash gate comes first — a key that missed a dependency
  // could later be a wrong cache hit, so a failed scan costs the artifact,
  // never the build.
  if (exitCode == 0)
    for (const CompileJob &job : jobs)
      runIsolatedSideEmit(job, log);

  return exitCode;
}
