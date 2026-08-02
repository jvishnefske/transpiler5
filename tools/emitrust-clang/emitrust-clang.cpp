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
/// (`make CC=emitrust-clang`): every invocation is DELEGATED verbatim to the
/// real clang — so object files, configure probes, `--version` checks,
/// preprocessing, depfile generation, and the final native link all behave
/// exactly as a plain-clang build — and, additionally, each driver
/// invocation that compiles a C source with `-c` side-emits one
/// `<object>.emitrust.mlirbc` artifact holding the EmitRust import of that
/// translation unit, lowered through the pinned emitrust-cc pipeline to the
/// converted emitrust-dialect module, serialized as MLIR bytecode (the
/// FR-57 per-TU parse cache). The same payload is then embedded into the
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
/// `src-hash`, the MD5 of the source file's bytes.
///
/// The shim's cardinal rule is that the IMPORT NEVER FAILS THE BUILD: an
/// import error is logged (and warned about on stderr) and the exit code of
/// the real clang is returned unchanged. The real compiler to delegate to is
/// `EMITRUST_REAL_CC` when set, else `clang` found on PATH.
//
//===----------------------------------------------------------------------===//

#include "Cc1Key.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/ImportC.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
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
/// \param log the optional shim log, receiving each cc1 line and its hash.
/// \returns the C compile jobs found; empty when there are none.
static std::vector<CompileJob>
classifyCompileJobs(llvm::ArrayRef<const char *> args, llvm::StringRef realCC,
                    llvm::raw_ostream *log) {
  std::vector<CompileJob> jobs;

  clang::DiagnosticOptions diagOpts;
  clang::IgnoringDiagConsumer silentConsumer;
  clang::DiagnosticsEngine diags(
      llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), diagOpts,
      &silentConsumer, /*ShouldOwnClient=*/false);
  clang::driver::Driver driver(realCC, llvm::sys::getDefaultTargetTriple(),
                               diags);
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
      } else if (arg.starts_with("-std=")) {
        job.importArgs.push_back(arg.str());
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

    // The other half of the FR-57 key: the input's CONTENT, so the key pair
    // is invariant to where the file lives yet misses when it changes.
    // FUTURE: fold in the transitively included headers via the depfile's
    // file list; for now only the main file's bytes are hashed.
    std::string srcHash = "<unavailable>";
    if (!job.input.empty()) {
      if (llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
              llvm::MemoryBuffer::getFile(job.input)) {
        llvm::MD5 srcMd5;
        srcMd5.update((*buffer)->getBuffer());
        llvm::MD5::MD5Result srcDigest = srcMd5.final();
        llvm::SmallString<32> srcHex;
        llvm::MD5::stringifyResult(srcDigest, srcHex);
        srcHash = std::string(srcHex);
      }
    }

    if (log) {
      *log << "cc1:";
      for (const char *arg : cc1)
        *log << " " << arg;
      *log << "\ncc1-key: " << job.cc1Hash << "\nsrc-hash: " << srcHash
           << "\n";
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
  std::string artifactPath = job.output + ".emitrust.mlirbc";

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

  {
    std::error_code ec;
    llvm::raw_fd_ostream out(artifactPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
      llvm::errs() << "emitrust-clang: warning: cannot write '" << artifactPath
                   << "': " << ec.message() << "\n";
      return;
    }
    if (mlir::failed(mlir::writeBytecodeToFile(*module, out))) {
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
    jobs = classifyCompileJobs(expandedArgs, *realCC, log);

  // Delegate to the real clang with the ORIGINAL argv (argv[0] replaced), so
  // every invocation shape — compile, link, -E, -M*, --version, configure
  // probes — behaves exactly as a plain-clang build. Stdout/stderr are
  // inherited, never captured.
  llvm::SmallVector<llvm::StringRef, 64> delegatedArgs;
  delegatedArgs.push_back(*realCC);
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
  // rejected has no object file for the artifact to ride with.
  if (exitCode == 0)
    for (const CompileJob &job : jobs)
      sideEmitArtifact(job, log);

  return exitCode;
}
