// RUN: split-file %s %t
// RUN: sed -e "s|@DIR@|%t|g" %t/ok/compile_commands.json.in > %t/ok/compile_commands.json
// RUN: emitrust-cc --emit=import --compdb %t/ok -o - 2>%t/ok.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=OKERR --implicit-check-not=error: --implicit-check-not="unknown warning option" --input-file=%t/ok.err
// RUN: sed -e "s|@DIR@|%t|g" %t/fbogus/compile_commands.json.in > %t/fbogus/compile_commands.json
// RUN: emitrust-cc --emit=import --compdb %t/fbogus -o %t/fbogus.mlir 2>&1 | FileCheck %s --check-prefix=FBOGUS
// RUN: sed -e "s|@DIR@|%t|g" %t/extra/compile_commands.json.in > %t/extra/compile_commands.json
// RUN: not emitrust-cc --emit=import --compdb %t/extra --extra-arg=-Werror -o /dev/null 2>&1 | FileCheck %s --check-prefix=EXTRA

// FR-67 GCC compile-database tolerance: a `compile_commands.json` recorded
// from a GCC build carries GCC-only warning flags (`-Wlogical-op`) plus the
// project's own `-Werror`; replayed through the clang frontend that becomes
// `-Werror,-Wunknown-warning-option` and the import dies on a flag with
// ZERO bearing on the AST. This test pins the recorded-entry filter's
// tolerance policy AND its frontier:
//
//  - `werr.c`'s entry (`-Werror -Wlogical-op`) imports cleanly: `-Werror`
//    is dropped and the appended `-Wno-unknown-warning-option` demotes the
//    GCC-only spelling below even the note, so the OKERR run's
//    --implicit-check-not pins a stderr with no "error:" and no
//    "unknown warning option" text at all.
//  - `unused.c`'s entry drops `-Werror=unused-variable` (and
//    `-Werror=format`, riding along), but the plain `-Wunused-variable`
//    SURVIVES — the filter removes error promotion, never the warning
//    itself — so the witness diagnostic is demoted to a warning, not
//    silenced.
//  - `pedantic.c`'s entry drops `-pedantic-errors`, admitting the `0b101`
//    binary literal that is a hard `-Wc23-extensions` error under it.
//  - FRONTIER (fbogus): `-f*`/`-m*` flags are ABI-relevant and are NOT
//    filtered; an unknown one still reaches the driver and fails loudly
//    with clang's own located rejection. (Pinned as stderr text: today the
//    driver-level unknown-argument error is not reflected in the exit code
//    on either the compdb or no-compdb path — a pre-existing gap at
//    ImportC.cpp's status wiring, recorded as follow-up work, not FR-67.)
//  - FRONTIER (extra): only RECORDED command lines are softened. The
//    user's own `--extra-arg=-Werror` is appended after the database's
//    flags and must still promote a real warning to a fatal error.

//--- ok/compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/werr.c",
    "command": "gcc -Werror -Wlogical-op -c -o werr.o werr.c"
  },
  {
    "directory": "@DIR@",
    "file": "@DIR@/unused.c",
    "command": "gcc -Werror=unused-variable -Werror=format -Wunused-variable -c -o unused.o unused.c"
  },
  {
    "directory": "@DIR@",
    "file": "@DIR@/pedantic.c",
    "command": "gcc -pedantic-errors -c -o pedantic.o pedantic.c"
  }
]

//--- fbogus/compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/werr.c",
    "command": "gcc -fbogus-flag-xyz -c -o werr.o werr.c"
  }
]

//--- extra/compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/extra.c",
    "command": "gcc -Wunused-variable -c -o extra.o extra.c"
  }
]

//--- werr.c
int gcc_only(int x) { return x + 1; }

//--- unused.c
int with_unused(int x) { int u = 3; return x + 1; }

//--- pedantic.c
int binlit(void) { return 0b101; }

//--- extra.c
int extra_fn(void) { int e = 1; return 0; }

// All three softened entries import into one module.
// CHECK-DAG: func.func @gcc_only
// CHECK-DAG: func.func @with_unused
// CHECK-DAG: func.func @binlit

// The dropped `-Werror=unused-variable` leaves the surviving
// `-Wunused-variable` as a demoted warning (the frontend may print the
// diagnostic more than once; one match plus the implicit-check-nots on the
// whole stream is the pin).
// OKERR: warning: unused variable 'u' [-Wunused-variable]

// FBOGUS: error: unknown argument: '-fbogus-flag-xyz'

// EXTRA: error: unused variable 'e' [-Werror,-Wunused-variable]
