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
and negative or 64-bit case values), C enums including int-to-enum conversions, the
conditional and comma operators, `sizeof`, value-position assignment and
increment/decrement, file-scope globals, static locals, structs (nested structs and
whole-struct assignment included), arrays (multi-dimensional arrays and arrays of structs
included), aggregate and designated initializers, pointer arithmetic, pointer locals, and
pointer/array slice parameters, function pointers with call-site devirtualization,
fixed-prototype variadic definitions, GNU statement expressions and `__builtin_expect`,
`long double` mapped to `f64`, unions as a one-slot struct model, bit-fields via
mask-and-shift accessors over packed backing integers, the full `printf` format language
(`%s`, `%c`, `%u`, `%x`, `%o`, `%e`, `%g`, and friends), a curated
`string.h`/`stdlib.h`/`math.h` libc subset, and `FILE*` I/O
(`fopen`/`fread`/`fwrite`/`fgetc`/`fgets`/`fclose`).

Rejected with located diagnostics — not miscompiled: computed goto (plain `goto` and
labels are supported); general pointer-to-pointer beyond a single bounded `T **` local
that statically selects one first-order pointer — third-order pointers,
pointer-to-pointer parameters and struct fields, and multi-target or copied second-order
pointers; general dynamic memory (`malloc`/`calloc`/`realloc`/`free`) beyond the one
constant-size-allocation-to-static-array carve-out; `char *` variables bound directly to
a string literal (as opposed to `char s[] = "..."` and the printf/puts `%s` shapes, which
are supported); row-pointer walking arithmetic and slices of rows into multi-dimensional
arrays; union arms that are bit-fields, unnamed/anonymous, pointers, of differing sizes,
or non-identical aggregates/enums, plus taking the address of a union member and `++`/`--`
through a float-pun arm; bit-fields that are zero-width or anonymous, runs wider than 64
bits, bit-field arms inside unions, compound assignment or increment on a bit-field, and
`sizeof`/`_Alignof` of a struct containing one; and, on the Rust-output side, generics,
lifetimes beyond simple references, traits and impls, pattern matching beyond literal
match arms, data-carrying enums, and expression inlining (every value is a named `let`
binding). The c-testsuite ledger is complete: 220 total, 220 passed, 0 miscompiled, 0
unsupported.

### Known issues

- The *sign* of a NaN produced by `0.0/0.0` is a documented divergence: C leaves it
  unspecified (Annex F), and it observably differs between gcc and clang on identical
  source, so it cannot be diffed against any single reference.
- Piping the raw `--emit=import` dump (or `emitrust-import-c`'s output) back into
  `emitrust-opt` can fail with `integer value too large` on a `switch` with a negative or
  above-`i64::MAX` 64-bit case label: upstream MLIR's `cf.switch` printer renders such
  values in unsigned decimal (a `-1` label prints as `18446744073709551615`), but its
  parser only accepts signed `i64` literals. This is not currently reproducible anywhere
  past that one raw debug-dump stage: by the `--emit=mlir` stage the switch has already
  lowered into EmitRust's own `emitrust.switch`, whose case values print and parse
  symmetrically, and the differential end-to-end tests (including
  test/EndToEnd/switch-general.c, which pins `INT_MIN` and sub-`INT32_MIN` labels through
  the full pipeline) pass byte-identical regardless. If it resurfaces outside the raw
  import dump, treat it as an upstream MLIR `cf.switch` serialization asymmetry, not an
  EmitRust importer bug — see test/Import/C/switch-unsigned64.c, which documents the same
  asymmetry and deliberately skips the reparse pipe.

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
