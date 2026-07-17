# EmitRust

**Turn C into Rust you can actually read — and prove it still does the same thing.**

EmitRust is an out-of-tree [MLIR](https://mlir.llvm.org/) dialect and toolchain that
transpiles C into safe Rust. It is modeled on upstream EmitC: where EmitC models C/C++
so `mlir-translate --mlir-to-cpp` can emit readable C++, EmitRust models Rust constructs
so `emitrust-translate --mlir-to-rust` can emit readable Rust.

What makes it different from the usual C-to-Rust story:

- **No `unsafe`.** Not "less unsafe" — none. Mutable globals become `thread_local!` +
  `Cell`, not `static mut`. If a construct can't be expressed in safe Rust, it is
  rejected with a located diagnostic instead of being papered over.
- **No silently wrong output.** Every construct the emitter can't represent produces a
  diagnostic tied to a source location and a failed translation. The subset boundary is
  a feature, not an accident.
- **Differentially tested.** End-to-end programs are compiled by both clang and cargo,
  and the two binaries must produce byte-identical stdout. The
  [c-testsuite](https://github.com/c-testsuite/c-testsuite) conformance ledger runs the
  same way, with a two-way ratchet: a regression fails the build, and so does an
  unrecorded pass.
- **Real structured output.** C control flow is recovered into structured Rust — `switch`
  becomes `match`, C enums become `#[repr(i32)]` Rust enums, loops become loops — rather
  than a goto-emulating state machine.

Under the hood: clang LibTooling imports C into a hybrid `cf`/`arith`/`memref` + EmitRust
module, upstream MLIR passes (`mem2reg`, `canonicalize`, `lift-cf-to-scf`) do the heavy
lifting of promoting scalars and recovering structure, `convert-to-emitrust` lowers the
remainder, and the Rust emitter does a direct, local, syntax-directed translation with no
cleverness.

## Demo: a C project to a Rust crate in three lines

```sh
nix develop                                            # LLVM/MLIR/Clang 21.1.8 + cargo, pinned
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_EXTERNAL_LIT=$(which lit) && ninja -C build
build/bin/emitrust-cc --emit=crate src/*.c -Iinclude -o mycrate --build
```

The third line is the whole transpiler: it imports every translation unit, merges them
into one flat crate (externs unified across units, file-`static`s mangled per unit), emits
`mycrate/Cargo.toml` and `mycrate/src/main.rs`, and `--build` runs
`cargo build --release --offline` on the result.

## Using the tools

`emitrust-cc` is the end-to-end driver. `--emit` selects how far down the pipeline to go,
which is also how you debug it:

| Command | Output |
|---|---|
| `emitrust-cc --emit=import f.c` | raw imported MLIR, before any pass |
| `emitrust-cc --emit=mlir f.c` | MLIR after the full pass pipeline — the emitter's input |
| `emitrust-cc --emit=rust f.c` | Rust source text on stdout |
| `emitrust-cc --emit=crate f.c -o out` | complete cargo crate directory (default) |

Include paths work as expected: `-I`, `-isystem`, and `--extra-arg` are passed through to
clang, and clang's builtin resource directory is wired in at configure time. `--crate-name`
overrides the crate name, which otherwise defaults to the input's stem.

The other tools are the MLIR-native pieces, useful on their own:

- `emitrust-opt` — standard opt tool with the dialect and conversion passes registered
- `emitrust-translate --mlir-to-rust` — EmitRust IR to Rust source
- `emitrust-import-c` — the importer alone, printing the imported module

Release mode is load-bearing for differential runs: debug Rust panics on overflow where C
wraps, and the test programs avoid undefined behavior by construction.

## Scope

C `main` is imported as `c_main`; crate emission adds a `main` wrapper that exits with its
result. The supported subset covers scalar and unsigned types, bitwise and shift
operators, all the loop forms, `switch` (including fall-through, shared labels, nesting,
and negative or 64-bit case values), C enums, the conditional and comma operators,
`sizeof`, value-position assignment and increment/decrement, file-scope globals, static
locals, structs, arrays, and a `printf` subset.

Rejected with located diagnostics — not miscompiled: `goto`, unions, bitfields,
int-to-enum conversions, pointer arithmetic and pointer locals, multi-dimensional arrays,
aggregate initializers, variadic definitions, and string literals outside `printf`. The
current c-testsuite ledger stands at 220 tests: 75 transpiled, 75 passed, 0 miscompiled,
145 unsupported.

One documented divergence remains: the *sign* of a NaN produced by `0.0/0.0`, which C
leaves unspecified (Annex F) and which observably differs between gcc and clang on
identical source, so it cannot be diffed against any single reference.

## Building and testing

Build only inside `nix develop` — the flake pins LLVM/MLIR/Clang 21.1.8 plus a matching
cargo and rustc. The explicit `-DLLVM_EXTERNAL_LIT=$(which lit)` is required because the
nixpkgs LLVM ships no `llvm-lit`; lit comes from the shell's Python environment.

```sh
ninja -C build check-emitrust      # 78 lit/FileCheck tests; EndToEnd tests gate on cargo
```

PDLL twins of the arith/ub conversion patterns live behind
`-DEMITRUST_ENABLE_PDLL=ON` (needs `nix develop .#pdll`); the C++ patterns are the
shipping default.

## Design

Architecture, the dialect contract, the C99 roadmap, and the FR-1..FR-27 requirements
traceability all live in [design.md](design.md). The theory underlying the pipeline's
transformations (with citations and per-stage guarantees) is surveyed in
[docs/transformation-theory.md](docs/transformation-theory.md).
