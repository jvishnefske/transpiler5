//===- ClangProjectParser.cpp - clang parse shell for a project -*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `EmitRust/ClangProjectParser.h`: `buildCommandLine`,
/// `isCxxSourcePath`, the `PerFileCompilationDatabase`, and the
/// `ClangTool::buildASTs` call they exist to feed. Moved out of ImportC.cpp
/// (where they lived in an anonymous namespace) by pure code motion when
/// FR-40's item graph became a second caller; the only edits are the loss
/// of the anonymous namespace and the addition of `buildProjectASTs`, which
/// is the three lines `importC`/`importCProject` used to spell inline.
///
/// It stays inside MLIREmitRustImportC rather than becoming its own library
/// for one concrete reason: the `EMITRUST_CLANG_RESOURCE_DIR` compile
/// definition is set on that target by lib/ImportC/CMakeLists.txt's
/// configure-time `clang -print-resource-dir` probe. Moving this file
/// elsewhere would either duplicate that probe or silently drop the
/// resource dir, breaking `<stdint.h>` and friends.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/ClangProjectParser.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <cstdlib>
#include <optional>

using namespace mlir;

namespace {

/// Returns the `-resource-dir=<dir>` argument every parse needs so that
/// clang finds its builtin headers (`<stdint.h>`, `<stdarg.h>`, ...),
/// taken from the `EMITRUST_RESOURCE_DIR` environment variable or, failing
/// that, from the compile-time `EMITRUST_CLANG_RESOURCE_DIR` macro when the
/// build configured one. `std::nullopt` when neither is available, in which
/// case clang falls back to deducing the directory from its own binary
/// location (the historical behavior when the macro is undefined).
///
/// Split out of `buildCommandLine` because an FR-45 `compile_commands.json`
/// entry needs the very same injection: the recorded command line was
/// written for the project's own compiler and never carries OUR resource
/// dir, yet the importer's system-header handling depends on it.
std::optional<std::string> clangResourceDirArg() {
  std::string resourceDir;
  if (const char *env = std::getenv("EMITRUST_RESOURCE_DIR"))
    resourceDir = env;
#ifdef EMITRUST_CLANG_RESOURCE_DIR
  if (resourceDir.empty())
    resourceDir = EMITRUST_CLANG_RESOURCE_DIR;
#endif
  if (resourceDir.empty())
    return std::nullopt;
  return "-resource-dir=" + resourceDir;
}

#ifdef __APPLE__
/// Returns `-isysroot<path>` so clang can find Darwin's system headers
/// (`<stdio.h>`, `<math.h>`, ...). `FixedCompilationDatabase`/`ClangTool`
/// invoke clang's frontend directly and never run `clang::driver::Driver`'s
/// SDK auto-detection, so without this every host header the importer's
/// test corpora `#include` is invisible on macOS.
///
/// Deliberately NOT resolved by reading `$SDKROOT`/shelling out to `xcrun`
/// at run time, the way an earlier version of this function did: lit
/// sandboxes the test environment (`with_system_environment` only forwards
/// a small allowlist, and `SDKROOT` isn't in it), and even when a stray
/// `DEVELOPER_DIR` from the nix dev shell DID leak through, it pointed
/// `xcrun` at a bare SDK store path with no `Info.plist`, so the real
/// system `xcrun` failed to resolve `-sdk macosx` and printed its error
/// text to stdout, which then got parsed as if it were the path. Baking
/// the SDK path in at CONFIGURE time — the same trick `clangResourceDirArg`
/// already uses for the resource dir, in the CMakeLists.txt/meson.build
/// probes next to this file — sidesteps the whole problem: it runs inside
/// the real nix dev shell, once, and the answer is immune to whatever
/// environment a later test invocation happens to have. `EMITRUST_SYSROOT`
/// is still honored at run time, for a user overriding the SDK outside the
/// nix shell.
std::optional<std::string> darwinSysrootArg() {
  std::string sysroot;
  if (const char *env = std::getenv("EMITRUST_SYSROOT"))
    sysroot = env;
#ifdef EMITRUST_CLANG_SYSROOT
  if (sysroot.empty())
    sysroot = EMITRUST_CLANG_SYSROOT;
#endif
  if (sysroot.empty())
    return std::nullopt;
  return "-isysroot" + sysroot;
}

/// Returns `-isystem<dir>` for libc++'s headers (`<vector>`, `<string>`,
/// ...), needed for the same reason `darwinSysrootArg` is: the nix clang
/// WRAPPER injects this path into every invocation of the `clang`/`clang++`
/// shell script by default (independent of `NIX_CFLAGS_COMPILE` — it is
/// part of the wrapper's own baked-in configuration), but `FixedCompilationDatabase`/
/// `ClangTool` call into the frontend in-process and never run that wrapper
/// script at all, so C++ imports otherwise fail on the very first standard
/// header. Baked in at configure time (`EMITRUST_CLANG_LIBCXX_DIR`), same
/// as the resource dir and sysroot probes next to this file.
std::optional<std::string> darwinLibcxxArg() {
  std::string libcxxDir;
  if (const char *env = std::getenv("EMITRUST_LIBCXX_DIR"))
    libcxxDir = env;
#ifdef EMITRUST_CLANG_LIBCXX_DIR
  if (libcxxDir.empty())
    libcxxDir = EMITRUST_CLANG_LIBCXX_DIR;
#endif
  if (libcxxDir.empty())
    return std::nullopt;
  return "-isystem" + libcxxDir;
}
#endif

/// Assembles the clang command line for one input, selecting the C or C++
/// frontend from `isCxx` (W2.0 per-input language selection): plain C
/// stays `-std=c11` (historical, unchanged); C++ opens with `-x c++
/// -std=c++17`, entirely DROPPING `-std=c11` (clang hard-errors on
/// `-std=c11 -x c++`, which is exactly why this used to reject every
/// `.cpp` input outright). Both add clang's builtin `-resource-dir`
/// (needed for system headers such as `<stdint.h>`) taken from the
/// `EMITRUST_RESOURCE_DIR` environment variable or, failing that, the
/// compile-time `EMITRUST_CLANG_RESOURCE_DIR` macro when defined, and
/// finally the caller's extra arguments in order.
std::vector<std::string>
buildCommandLine(bool isCxx, llvm::ArrayRef<std::string> extraClangArgs) {
  // C89-era programs (the c-testsuite corpus, e.g. 00144's
  // `q = i ? 0 : 0`) assign integer expressions to pointers, which clang
  // >= 15 hard-errors by default; demote it back to the historical
  // warning — the importer itself classifies integer-to-pointer traffic
  // and rejects the unsupported shapes with located diagnostics. The same
  // demotion applies to the C++ frontend for symmetry (the importer's own
  // classification is language-agnostic).
  std::vector<std::string> commandLine =
      isCxx ? std::vector<std::string>{"-x", "c++", "-std=c++17",
                                       "-Wno-error=int-conversion"}
            : std::vector<std::string>{"-std=c11", "-Wno-error=int-conversion"};
  if (std::optional<std::string> resourceDirArg = clangResourceDirArg())
    commandLine.push_back(*resourceDirArg);
#ifdef __APPLE__
  if (std::optional<std::string> sysrootArg = darwinSysrootArg())
    commandLine.push_back(*sysrootArg);
  if (isCxx)
    if (std::optional<std::string> libcxxArg = darwinLibcxxArg())
      commandLine.push_back(*libcxxArg);
  // The Darwin SDK defaults `_USE_FORTIFY_LEVEL` to 2 when `_FORTIFY_SOURCE`
  // is undefined, macro-rewriting `strcpy`/`sprintf`/... call sites into
  // `__builtin___*_chk` builtins the importer does not model (the importer's
  // hosted surface is the ISO C names). Level 0 keeps the standard spellings;
  // the check-vs-unchecked call is behavior-identical for a well-defined
  // program, so the EndToEnd byte-diff against the (fortified) native build
  // is unaffected.
  commandLine.push_back("-D_FORTIFY_SOURCE=0");
#endif
  commandLine.insert(commandLine.end(), extraClangArgs.begin(),
                     extraClangArgs.end());
  return commandLine;
}

/// A `CompilationDatabase` that selects the C or C++ command line
/// (`buildCommandLine`) per file by extension (`isCxxSourcePath`),
/// delegating to one `FixedCompilationDatabase` per language (their
/// well-tested `CompileCommand` construction is reused verbatim — only
/// which instance answers a given file differs). This is what lets
/// `importCProject`'s multi-file `ClangTool` compile each input in its
/// own language mode: a mixed C+C++ project is out of scope for W2.0 (no
/// cross-language linkage), but nothing stops each file from compiling
/// correctly in isolation.
class PerFileCompilationDatabase : public clang::tooling::CompilationDatabase {
public:
  explicit PerFileCompilationDatabase(
      llvm::ArrayRef<std::string> extraClangArgs)
      : cDatabase(".", buildCommandLine(/*isCxx=*/false, extraClangArgs)),
        cxxDatabase(".", buildCommandLine(/*isCxx=*/true, extraClangArgs)) {}

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef filePath) const override {
    return (mlir::emitrust::isCxxSourcePath(filePath) ? cxxDatabase : cDatabase)
        .getCompileCommands(filePath);
  }

private:
  clang::tooling::FixedCompilationDatabase cDatabase;
  clang::tooling::FixedCompilationDatabase cxxDatabase;
};

/// True when a recorded `argv[0]` names a C++ driver (`clang++`, `g++`,
/// `c++`, `arm-none-eabi-g++`, ...), i.e. when the entry relies on the
/// driver's own "compile even a `.c` file as C++" behavior (FR-45).
///
/// The recorded compiler is dropped from the rewritten command line
/// (`filterRecordedCommandLine`), so this is the one bit of it that must
/// survive: `clang::driver::Driver` derives its mode from `argv[0]`, and
/// losing the `++` suffix would silently retarget such an entry to C.
bool isCxxDriverName(llvm::StringRef argv0) {
  return llvm::sys::path::stem(argv0).ends_with("++");
}

/// Rewrites one `compile_commands.json` entry's recorded command line into
/// something `ToolInvocation` can drive (FR-45).
///
/// Three transformations, in order:
///
///  1. `argv[0]` — the project's own compiler, often an absolute path to a
///     cross-compiler — is REPLACED by a neutral `clang-tool` (the name
///     `FixedCompilationDatabase` uses) or `clang++` when the recorded
///     driver was a C++ one. It cannot simply be deleted: LibTooling reads
///     `CommandLine[0]` as the driver's binary name, so dropping it would
///     eat the first real flag; and it cannot be kept, because clang would
///     then search that foreign toolchain's directories for its builtin
///     headers. Substituting a neutral name preserves exactly one recorded
///     property, the driver's C-vs-C++ mode (`isCxxDriverName`).
///  2. Driver-only arguments are stripped: `-c`, `-o <path>`/`-o<path>`,
///     and the dependency-generation family `-M`, `-MD`, `-MMD`,
///     `-MF/-MT/-MQ` (separate or joined operand). These direct code
///     generation and side files that a syntax-only AST build must never
///     perform; the operand-taking ones would additionally leave a bare
///     path in the argument list, which the driver would take for a second
///     INPUT FILE and reject. (`ClangTool` installs overlapping default
///     `ArgumentsAdjuster`s, but filtering here keeps the contract
///     explicit and independent of that default set.) FR-67 additionally
///     drops the error-promotion flags `-Werror`, `-Werror=<diag>`, and
///     `-pedantic-errors`: a database recorded from a GCC build replays
///     GCC-only warning spellings through the clang frontend, where the
///     project's own `-Werror` turns them into hard errors with zero
///     bearing on the AST. Everything else — `-I`, `-isystem`, `-D`,
///     `-std`, `-x`, `-f*`, the remaining `-W*` flags, and the input file
///     itself — is preserved verbatim (`-f*`/`-m*` are ABI-relevant, so an
///     unknown one stays a loud frontend error by design).
///  3. The importer's own required arguments are appended: the resource
///     dir (`clangResourceDirArg`), the `-Wno-error=int-conversion`
///     demotion `buildCommandLine` documents, and the FR-67
///     `-Wno-unknown-warning-option` demotion (so a surviving GCC-only
///     `-W` spelling degrades below even clang's note) — none of which an
///     external database entry would ever carry — and finally the caller's
///     `extraClangArgs` LAST so that `--extra-arg` can override the
///     database (a USER-written `-Werror` is therefore never stripped).
///
/// Trailing position is safe for all three: none of them is
/// input-position-sensitive the way `-x` is (which is why `-x` is left
/// exactly where the entry put it, ahead of its input file).
std::vector<std::string>
filterRecordedCommandLine(llvm::ArrayRef<std::string> recorded,
                          llvm::ArrayRef<std::string> extraClangArgs) {
  std::vector<std::string> args;
  if (recorded.empty())
    return args;
  bool isCxx = isCxxDriverName(recorded.front());
  args.push_back(isCxx ? "clang++" : "clang-tool");

  // Driver-only flags that consume the following argument when spelled
  // separately, and that also accept it joined ("-ofoo.o", "-MFfoo.d").
  static constexpr llvm::StringRef operandFlags[] = {"-o", "-MF", "-MT",
                                                     "-MQ"};
  // Driver-only flags that stand alone.
  static constexpr llvm::StringRef standaloneFlags[] = {"-c", "-M", "-MD",
                                                        "-MMD"};

  for (size_t index = 1, size = recorded.size(); index != size; ++index) {
    llvm::StringRef arg = recorded[index];
    if (llvm::is_contained(standaloneFlags, arg))
      continue;
    if (llvm::is_contained(operandFlags, arg)) {
      // Skip the operand too; a truncated entry ending in the flag simply
      // ends the scan.
      ++index;
      continue;
    }
    if (llvm::any_of(operandFlags, [&](llvm::StringRef flag) {
          return arg.size() > flag.size() && arg.starts_with(flag);
        }))
      continue;
    // FR-67: recorded error promotion is dropped — only the promotion, never
    // the warnings themselves (`-Wunused-variable` et al. survive and demote
    // to warnings). User-supplied promotion arrives via `extraClangArgs`
    // below and is untouched.
    if (arg == "-Werror" || arg == "-pedantic-errors" ||
        arg.starts_with("-Werror="))
      continue;
    args.push_back(arg.str());
  }

  if (std::optional<std::string> resourceDirArg = clangResourceDirArg())
    args.push_back(*resourceDirArg);
#ifdef __APPLE__
  if (std::optional<std::string> sysrootArg = darwinSysrootArg())
    args.push_back(*sysrootArg);
  if (isCxx)
    if (std::optional<std::string> libcxxArg = darwinLibcxxArg())
      args.push_back(*libcxxArg);
  // See buildCommandLine: keep the SDK from fortify-rewriting libc calls.
  args.push_back("-D_FORTIFY_SOURCE=0");
#endif
  args.push_back("-Wno-error=int-conversion");
  // FR-67: GCC-only `-W` spellings recorded by a foreign compiler must
  // degrade to nothing, not an unknown-warning-option diagnostic. Trailing
  // position suppresses earlier unknown `-W` flags; sitting BEFORE
  // `extraClangArgs` keeps `--extra-arg` able to re-enable it.
  args.push_back("-Wno-unknown-warning-option");
  args.insert(args.end(), extraClangArgs.begin(), extraClangArgs.end());
  return args;
}

/// A `CompilationDatabase` backed by a real `compile_commands.json`
/// (FR-45): every query is answered from the recorded entry, with the
/// command line rewritten by `filterRecordedCommandLine`, and only files
/// the database does NOT mention fall back to the extension-guessing
/// `PerFileCompilationDatabase`.
///
/// Rewriting the entry in place — rather than reading its flags out and
/// handing `ClangTool` a hand-built command line — is deliberate, and is
/// what keeps the classic compilation-database bug out of this importer:
/// each entry's `directory` field is the working directory its relative
/// paths (`-I../include`, the input file itself) resolve against, and
/// `ClangTool` applies `CompileCommand::Directory` to its virtual file
/// system before running the invocation. A hand-built command line would
/// have to re-resolve every relative path itself, and would get it wrong
/// for exactly the projects that need a database.
class RecordedCompilationDatabase : public clang::tooling::CompilationDatabase {
public:
  RecordedCompilationDatabase(
      std::unique_ptr<clang::tooling::CompilationDatabase> recorded,
      llvm::ArrayRef<std::string> extraClangArgs)
      : recorded(std::move(recorded)),
        extraClangArgs(extraClangArgs.begin(), extraClangArgs.end()),
        fallback(extraClangArgs) {}

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef filePath) const override {
    std::vector<clang::tooling::CompileCommand> commands =
        recorded->getCompileCommands(filePath);
    if (commands.empty())
      return fallback.getCompileCommands(filePath);
    return rewrite(std::move(commands));
  }

  std::vector<std::string> getAllFiles() const override {
    return recorded->getAllFiles();
  }

  std::vector<clang::tooling::CompileCommand>
  getAllCompileCommands() const override {
    return rewrite(recorded->getAllCompileCommands());
  }

private:
  std::vector<clang::tooling::CompileCommand>
  rewrite(std::vector<clang::tooling::CompileCommand> commands) const {
    for (clang::tooling::CompileCommand &command : commands)
      command.CommandLine =
          filterRecordedCommandLine(command.CommandLine, extraClangArgs);
    return commands;
  }

  std::unique_ptr<clang::tooling::CompilationDatabase> recorded;
  std::vector<std::string> extraClangArgs;
  PerFileCompilationDatabase fallback;
};

/// Builds the compilation database every import runs against — the single
/// seam through which the C-vs-C++ command line for each translation unit
/// is chosen.
///
/// With an empty `compilationDatabasePath` this is the historical
/// `PerFileCompilationDatabase` (language guessed from the extension), so
/// the no-`--compdb` behavior is bit-for-bit what it was. Otherwise the
/// path is loaded as a `compile_commands.json`: `loadFromDirectory` when
/// it names a directory (which asks every registered database plugin, so
/// a build directory holding a `compile_flags.txt` instead is accepted
/// too), and `JSONCompilationDatabase::loadFromFile` when it names the
/// JSON file itself. The parent-directory search of
/// `autoDetectFromDirectory` is deliberately NOT used: `--compdb` is an
/// explicit path the user typed, and silently compiling against some
/// ancestor's unrelated database would be worse than the load error.
/// `JSONCommandLineSyntax::AutoDetect` lets clang decide between GNU and
/// Windows argument quoting from the database's own contents, which is
/// what every other LibTooling consumer does.
///
/// \returns the database, or null with `error` set to clang's own error
///          text (a bad database is a user error, never a crash). The
///          message is RETURNED rather than diagnosed because this shell has
///          no `MLIRContext`: the importer wraps it in a located MLIR
///          diagnostic naming the database path, the item-graph tool prints
///          it on stderr.
std::unique_ptr<clang::tooling::CompilationDatabase>
makeCompilationDatabase(llvm::StringRef compilationDatabasePath,
                        llvm::ArrayRef<std::string> extraClangArgs,
                        std::string &error) {
  if (compilationDatabasePath.empty())
    return std::make_unique<PerFileCompilationDatabase>(extraClangArgs);

  std::string errorMessage;
  std::unique_ptr<clang::tooling::CompilationDatabase> recorded;
  if (llvm::sys::fs::is_directory(compilationDatabasePath))
    recorded = clang::tooling::CompilationDatabase::loadFromDirectory(
        compilationDatabasePath, errorMessage);
  else
    recorded = clang::tooling::JSONCompilationDatabase::loadFromFile(
        compilationDatabasePath, errorMessage,
        clang::tooling::JSONCommandLineSyntax::AutoDetect);
  if (!recorded) {
    // `loadFromDirectory` aggregates one line per registered database
    // plugin, so flatten the message onto a single line: every other
    // diagnostic this tool prints is one line, and the lit tests match
    // them line-wise.
    llvm::SmallVector<llvm::StringRef, 4> reasons;
    llvm::StringRef(errorMessage).split(reasons, '\n', /*MaxSplit=*/-1,
                                        /*KeepEmpty=*/false);
    for (llvm::StringRef &reason : reasons)
      reason = reason.trim();
    error = llvm::join(reasons, "; ");
    return nullptr;
  }
  return std::make_unique<RecordedCompilationDatabase>(std::move(recorded),
                                                       extraClangArgs);
}

/// Returns the translation units to import: `paths` when the caller named
/// any, otherwise — and only when a database was given — every file the
/// database lists (FR-45's "no positional sources means the whole
/// project").
///
/// The database's file list is sorted and deduplicated. Both matter:
/// `JSONCompilationDatabase::getAllFiles` iterates a `StringMap`, whose
/// order is a hash-order artifact, and translation-unit ORDER is
/// observable in the imported module (the `tu<N>_` prefix that keeps
/// same-named file statics apart, and the module's own location); a
/// database may also carry several entries for one file (different
/// configurations), and importing the same TU twice would be a spurious
/// duplicate-definition error.
/// FR-68: the diagnostic consumer installed on the `ClangTool` run so that
/// error-severity diagnostics FORCE a nonzero status. A driver-level error —
/// clang's `error: unknown argument: '-fbogus-flag-xyz'` for a GCC-only flag
/// in a recorded compdb entry or a typo'd `--extra-arg` — is produced by
/// `ToolInvocation`'s private driver-side engine: it reaches neither
/// `ClangTool::buildASTs`' return status nor the ASTUnit's
/// `DiagnosticsEngine`, so without this consumer the import would proceed
/// and silently emit a module built with a rejected (possibly ABI-relevant)
/// command line.
///
/// Installing a consumer REPLACES `ToolInvocation`'s fallback
/// `TextDiagnosticPrinter`, so this one must be a byte-transparent wrapper:
/// it forwards `BeginSourceFile`/`EndSourceFile` (the CompilerInstance
/// drives them here, and caret rendering breaks without them),
/// `HandleDiagnostic`, and `finish` to a real printer over `llvm::errs()`
/// whose options set `ShowOptionNames` — the ASTUnit path's default —
/// because a default-constructed `DiagnosticOptions` would drop the
/// `[-Wunused-variable]` suffix warnings carry today. Only diagnostics at
/// `Error` severity or above are counted: warnings must stay non-fatal
/// (FR-67's demoted `-Werror` flows depend on it), and notes ride along
/// with their parent.
class ForwardingErrorCountingConsumer : public clang::DiagnosticConsumer {
public:
  ForwardingErrorCountingConsumer(
      const std::string &currentFile,
      mlir::emitrust::ProjectParseError &firstError)
      : currentFile(currentFile), firstError(firstError),
        printer(llvm::errs(), diagOpts) {
    // Read by `printer` at diagnostic time; the member order keeps
    // `diagOpts` alive for the printer's whole life (it holds a reference).
    diagOpts.ShowOptionNames = 1;
  }

  void BeginSourceFile(const clang::LangOptions &langOpts,
                       const clang::Preprocessor *pp) override {
    printer.BeginSourceFile(langOpts, pp);
  }
  void EndSourceFile() override { printer.EndSourceFile(); }
  void finish() override { printer.finish(); }

  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    // Keep the base class's NumErrors/NumWarnings bookkeeping contract.
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level >= clang::DiagnosticsEngine::Error) {
      ++errorCount;
      if (firstError.file.empty() && firstError.message.empty()) {
        llvm::SmallString<128> text;
        info.FormatDiagnostic(text);
        firstError.file = currentFile;
        firstError.message = std::string(text);
      }
    }
    printer.HandleDiagnostic(level, info);
  }

  /// Diagnostics at `Error` severity or above seen during the whole run,
  /// across every TU. Nonzero means the run failed even if
  /// `ClangTool::buildASTs` says otherwise.
  unsigned errorCount = 0;

private:
  /// The TU the tool is currently building, maintained by the arguments
  /// adjuster in `buildProjectASTs` — the adjuster fires before the driver
  /// parses the command line, so even an unlocated driver error can be
  /// attributed to its TU.
  const std::string &currentFile;
  mlir::emitrust::ProjectParseError &firstError;
  clang::DiagnosticOptions diagOpts;
  clang::TextDiagnosticPrinter printer;
};

std::vector<std::string>
selectSourcePaths(llvm::ArrayRef<std::string> paths,
                  const clang::tooling::CompilationDatabase &compilations,
                  bool haveDatabase) {
  if (!paths.empty() || !haveDatabase)
    return std::vector<std::string>(paths.begin(), paths.end());
  std::vector<std::string> sources = compilations.getAllFiles();
  llvm::sort(sources);
  sources.erase(llvm::unique(sources), sources.end());
  return sources;
}

} // namespace

bool mlir::emitrust::isCxxSourcePath(llvm::StringRef path) {
  llvm::StringRef ext = llvm::sys::path::extension(path);
  return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C" ||
         ext == ".c++" || ext == ".hpp";
}

int mlir::emitrust::buildProjectASTs(
    llvm::ArrayRef<std::string> paths,
    llvm::ArrayRef<std::string> extraClangArgs,
    std::vector<std::unique_ptr<clang::ASTUnit>> &asts) {
  std::vector<std::string> resolvedSources;
  std::string error;
  // An empty database path cannot fail to load, so `error` stays empty and
  // this is exactly the historical PerFileCompilationDatabase run. The
  // FR-68 attribution is dropped here — this overload's contract reports
  // failure through the status alone, and clang has already printed the
  // diagnostic itself.
  ProjectParseError firstClangError;
  return buildProjectASTs(paths, extraClangArgs,
                          /*compilationDatabasePath=*/"", asts,
                          resolvedSources, error, firstClangError);
}

int mlir::emitrust::buildProjectASTs(
    llvm::ArrayRef<std::string> paths,
    llvm::ArrayRef<std::string> extraClangArgs,
    llvm::StringRef compilationDatabasePath,
    std::vector<std::unique_ptr<clang::ASTUnit>> &asts,
    std::vector<std::string> &resolvedSources, std::string &error,
    ProjectParseError &firstClangError) {
  std::unique_ptr<clang::tooling::CompilationDatabase> compilations =
      makeCompilationDatabase(compilationDatabasePath, extraClangArgs, error);
  if (!compilations) {
    // A load failure is reported through `error`; the nonzero status keeps
    // the "did every input compile" contract true for callers that only
    // check the return value.
    resolvedSources.clear();
    return 1;
  }
  resolvedSources = selectSourcePaths(
      paths, *compilations, /*haveDatabase=*/!compilationDatabasePath.empty());
  clang::tooling::ClangTool tool(*compilations, resolvedSources);
  // FR-68: attribution side-channel. The adjuster fires once per TU, BEFORE
  // the driver parses that TU's command line, so `currentFile` names the
  // right unit even for unlocated driver-level errors. Both it and the
  // consumer are per-call locals: attribution state must not leak across
  // concurrent or successive runs.
  std::string currentFile;
  tool.appendArgumentsAdjuster(
      [&currentFile](const clang::tooling::CommandLineArguments &args,
                     llvm::StringRef filename) {
        currentFile = filename.str();
        return args;
      });
  ForwardingErrorCountingConsumer consumer(currentFile, firstClangError);
  tool.setDiagnosticConsumer(&consumer);
  int status = tool.buildASTs(asts);
  // Fold the consumer's verdict into the status, but never overwrite a
  // nonzero one: status 2 (a TU skipped for want of a compile command,
  // which produces no error diagnostic) must keep its meaning.
  if (status == 0 && consumer.errorCount > 0)
    status = 1;
  return status;
}
