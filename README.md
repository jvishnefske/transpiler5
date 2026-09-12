# EmitRust

**Turn C and C++ into Rust you can actually read — and demonstrate it still does the same thing.**

EmitRust is an out-of-tree [MLIR](https://mlir.llvm.org/) dialect and toolchain that
transpiles C and a growing subset of C++17 into safe Rust. It is modeled on upstream
EmitC: where EmitC models C/C++ so `mlir-translate --mlir-to-cpp` can emit readable C++,
EmitRust models Rust constructs so `emitrust-translate --mlir-to-rust` can emit readable
Rust.

What makes it different from the usual C-to-Rust story:

- **No `unsafe`.** Not "less unsafe" — none. Mutable globals become `thread_local!` +
  `Cell`, not `static mut`. If a construct can't be expressed in safe Rust, it is
  rejected with a located diagnostic instead of being papered over. (Measured: zero
  `unsafe` tokens across the 47 crates emitted from the third-party corpus sweep.)
- **No silently wrong output.** Every construct the emitter can't represent produces a
  diagnostic tied to a source location and a failed translation. The subset boundary is
  a feature, not an accident.
- **Differentially tested.** End-to-end programs are compiled by both clang and cargo,
  and the two binaries must produce byte-identical stdout. The
  [c-testsuite](https://github.com/c-testsuite/c-testsuite) conformance ledger and the
  in-repo C++17 feature corpus run the same way, with a two-way ratchet: a regression
  fails the build, and so does an unrecorded pass.
- **Real structured output.** C control flow is recovered into structured Rust — `switch`
  becomes `match`, C enums become `#[repr(i32)]` Rust enums, loops become loops — rather
  than a goto-emulating state machine. C++ classes become `struct` + `impl`, destructors
  become `impl Drop`, and `throw`/`catch` becomes `Result` threading.
- **Partial ports are first-class.** Real projects are never entirely inside the subset,
  so `--incremental` recovers from the parts that aren't: unsupported items become
  loud `unimplemented!()` stubs or are dropped, the rest still compiles, and the run
  writes `PORTING.md` and `emitrust-progress.json` — every project item with its status,
  blocker tag, root cause and source location. Two runs diff item by item, which is how
  the subset frontier is measured rather than guessed.

Under the hood: clang LibTooling imports C/C++ into a hybrid `cf`/`arith`/`memref` +
EmitRust module, upstream MLIR passes (`mem2reg`, `canonicalize`, `lift-cf-to-scf`) do the
heavy lifting of promoting scalars and recovering structure, `convert-to-emitrust` lowers
the remainder, and the Rust emitter does a direct, local, syntax-directed translation with
no cleverness.

## Demo: a C or C++ project to a Rust crate in three lines

```sh
nix develop                                            # LLVM/MLIR/Clang 21.1.8 + cargo, pinned
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_EXTERNAL_LIT=$(which lit) && ninja -C build
build/tools/emitrust-cc --emit=crate src/*.c -Iinclude -o mycrate --build
```

The third line is the whole transpiler: it imports every translation unit, merges them
into one flat crate (externs unified across units, file-`static`s mangled per unit), emits
`mycrate/Cargo.toml` and `mycrate/src/main.rs`, and `--build` runs
`cargo build --release --offline` on the result.

For a project that is only partly inside the subset — which is most real projects — add
`--incremental` and read the `PORTING.md` it writes next to the crate.

## Using the tools

`emitrust-cc` is the end-to-end driver. `--emit` selects how far down the pipeline to go,
which is also how you debug it:

| Command | Output |
|---|---|
| `emitrust-cc --emit=item-graph f.c` | project item graph: one node per function/record/enum/global, plus dependency edges (no import) |
| `emitrust-cc --emit=coloring f.c` | three-color lattice over that graph: what is inside the subset, what is blocked, and the blame chain (no import) |
| `emitrust-cc --emit=import f.c` | raw imported MLIR, before any pass |
| `emitrust-cc --emit=mlir f.c` | MLIR after the full pass pipeline — the emitter's input |
| `emitrust-cc --emit=rust f.c` | Rust source text on stdout |
| `emitrust-cc --emit=crate f.c -o out` | complete cargo crate directory (default) |

Useful flags beyond `--emit`:

- `--incremental` — recover from out-of-subset items and write the porting artifacts
  (implies `--recover`). Without it the compile is byte-identical to one built without it.
- `--recover` — recover without the artifacts: report each unsupported top-level
  declaration as a warning and keep going.
- `--crate-type` / `--crate-name` — shape and name of the emitted crate. An input defining
  `main` emits a binary crate, one that doesn't emits a library crate.
- `--preserve-c-names` — emit C symbol spellings verbatim. By default the crate is renamed
  to idiomatic Rust (snake_case functions and fields, SCREAMING_SNAKE_CASE globals,
  UpperCamelCase types) so it compiles clean under the standard naming lints.
- `--link` — merge per-TU shards produced by the `emitrust-clang` compiler shim, so a
  project can be transpiled through its own build system one TU at a time.

Include paths work as expected: `-I`, `-isystem`, and `--extra-arg` are passed through to
clang, and clang's builtin resource directory is wired in at configure time.

The other tools are the MLIR-native pieces, useful on their own:

- `emitrust-opt` — standard opt tool with the dialect and conversion passes registered
- `emitrust-translate --mlir-to-rust` — EmitRust IR to Rust source
- `emitrust-import-c` — the importer alone, printing the imported module

Release mode is load-bearing for differential runs: debug Rust panics on overflow where C
wraps, and the test programs avoid undefined behavior by construction.

## Scope

### C

C `main` is imported as `c_main`; crate emission adds a `main` wrapper that exits with its
result. The supported subset covers scalar and unsigned types, bitwise and shift
operators, all the loop forms, `switch` (including fall-through, shared labels, nesting,
and negative or 64-bit case values), C enums including int-to-enum conversions, the
conditional and comma operators, `sizeof`, value-position assignment and
increment/decrement, file-scope globals, static locals, structs (nested structs and
whole-struct assignment included), arrays (multi-dimensional arrays and arrays of structs
included), aggregate and designated initializers, pointer arithmetic, pointer locals,
pointer/array slice parameters, and cursors returned into a caller-supplied region,
function pointers with call-site devirtualization, fixed-prototype variadic definitions,
GNU statement expressions and `__builtin_expect`, `long double` mapped to `f64`, unions as
a one-slot struct model, bit-fields via mask-and-shift accessors over packed backing
integers, the full `printf` format language (`%s`, `%c`, `%u`, `%x`, `%o`, `%e`, `%g`, and
friends), a curated `string.h`/`stdlib.h`/`math.h` libc subset, the `<ctype.h>` classifiers
in boolean context, and `FILE*` I/O (`fopen`/`fread`/`fwrite`/`fgetc`/`fgets`/`fclose`).

The c-testsuite ledger is complete: 220 total, 220 passed, 0 miscompiled, 0 unsupported.

### C++17

Namespaces and `extern "C"`; classes with non-virtual methods and constructors
(member-initializer lists included), with methods emitted as ordinary Rust inherent
methods; single non-virtual inheritance as a first `base` field; user-declared destructors
as `impl Drop`, including the polymorphic value-only subset; copy constructors and C++
value semantics; virtual methods on values (static dispatch is exact there) and through a
base pointer that provably binds one object (devirtualized to the final overrider);
operator overloading for the free and member by-value flat-call subset; `try`/`throw`/
`catch` threaded as `Result`; function- and class-template monomorphization, plus non-type
template parameters and explicit and partial specializations; by-value structured
bindings, ranged-for over container locals, and by-value-capture lambdas via lambda
lifting; `if`/`switch` init-statements.

STL recognition covers `std::vector<T>`, `std::string`, `std::array<T, N>`,
`std::pair<T1, T2>`, `std::optional<T>` → `Option<T>`, `std::variant<int, double>`,
`std::string_view` over a literal, `std::map`/`std::set` → `BTreeMap`/`BTreeSet`,
`std::unique_ptr<T>` → `Box<T>` with `std::make_unique`, and `std::cout`/`std::cerr` `<<`
chains → `print!`/`eprint!`.

The in-repo C++17 feature corpus (`test/Cpp17Suite/`) is complete: 35 of 35 transpile,
build, and match their clang-built reference byte for byte, with 0 quarantined
miscompiles.

### Rejected with located diagnostics — not miscompiled

On the C side: computed goto (plain `goto` and labels are supported); general
pointer-to-pointer beyond a single bounded `T **` local that statically selects one
first-order pointer; general dynamic memory (`malloc`/`calloc`/`realloc`/`free`) beyond
the one constant-size-allocation-to-static-array carve-out; `char *` variables bound
directly to a string literal (as opposed to `char s[] = "..."` and the printf/puts `%s`
shapes, which are supported); row-pointer walking arithmetic and slices of rows into
multi-dimensional arrays; union arms that are bit-fields, unnamed/anonymous, pointers, of
differing sizes, or non-identical aggregates/enums; bit-fields that are zero-width or
anonymous, runs wider than 64 bits, or inside unions; and returned pointers into
callee-local, global-table, or heap regions.

On the C++ side: trait objects and any virtual call whose receiver's dynamic type is not
statically known; `operator=`, reference-returning operators, `++`/`--`, and stream
operators; parameter packs; globals or statics of a class with a destructor; and
`tolower`/`toupper`.

On the Rust-output side: generics, lifetimes beyond simple references, user traits and
impls, pattern matching beyond literal match arms, data-carrying enums, and expression
inlining (every value is a named `let` binding).

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
ninja -C build check-emitrust        # full suite: 817 lit tests; EndToEnd gates on cargo
ninja -C build check-emitrust-fast   # inner loop: everything except the EndToEnd tier
```

The suite splits into two complementary tiers: 576 fast tests (importer goldens, dialect
round-trips, driver goldens, the conformance ledgers) and 241 EndToEnd tests, which are
the byte-diff oracle and account for most of the wall time. `check-emitrust` is the
pre-commit gate.

A parallel Meson build exists for faster iteration; CMake remains canonical for CI. It
links the monolithic libMLIR/libclang-cpp dylibs instead of static archives and runs the
same lit suite:

```sh
meson setup build-meson && meson compile -C build-meson
meson test -C build-meson --suite fast   # fast tier only
meson test -C build-meson                # both tiers — the same gate as check-emitrust
```

New tools and sources must be added to *both* the CMake and Meson build files. The PDLL
twins of the arith/ub conversion patterns live behind `-DEMITRUST_ENABLE_PDLL=ON` (needs
`nix develop .#pdll`, CMake only); the C++ patterns are the shipping default.

## Design

Architecture, the dialect contract, the C99 and C++17 roadmaps, the FR requirements
traceability, and the evidence ledger — every spike verdict, measured corpus number, and
recorded NO-GO — live in the split ledger: [design.md](design.md) is the preamble, each
FR/W2 entry is its own file under [docs/plans/entries/](docs/plans/entries/), and the
frozen design chapters live under [docs/design/](docs/design/), joined in
[docs/plans/ledger.manifest](docs/plans/ledger.manifest) order
(`python3 docs/plans/plan.py join` prints the whole stream). The open queue is indexed
machine-readably in [docs/plans/backlog.toml](docs/plans/backlog.toml); query it with
`python3 docs/plans/plan.py next`. The theory underlying the pipeline's transformations
(with citations and per-stage guarantees) is surveyed in
[docs/transformation-theory.md](docs/transformation-theory.md).

## License

Copyright (C) 2026 John Vishnefske

EmitRust is free software: you can redistribute it and/or modify it under the terms
of the GNU Affero General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version. See
[LICENSE](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

SPDX-License-Identifier: AGPL-3.0-or-later
