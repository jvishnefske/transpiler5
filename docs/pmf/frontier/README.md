# emitrust-bx function frontier on real modules (FR-258)

The weekly PMF indicator from FR-256: how many **functions** of two real C++
codecs, google/woff2 and google/brunsli, sit fully inside the emitrust-bx
subset. Each figure carries its **stage**, and stages are never mixed.

| file | what |
|---|---|
| `modules.toml` | the pinned modules (repo, exact SHA, TU globs, include dirs, exclusions with reasons, generated-header stubs) |
| `baseline-<date>-<bx sha>.json` | machine-readable result: per module and combined, plus every function with its blocker tags |
| `baseline-<date>-<bx sha>.md` | the same numbers as tables |
| `../../../scripts/bx-frontier.py` | the instrument. It reuses `scripts/bx-census.py` (classification, function attribution, HIDDEN marking, greedy set cover) |

## Stages

- **EMIT**: a main-file function definition for which `emitrust-bx
  --emit=kernel` reports no residual. That means none of its own and none
  inherited from an enclosing class, namespace or template that bx rejected
  whole. EMIT says nothing about rustc accepting the output or about its
  behaviour.
- **VERIFY**: EMIT, plus byte-identical output against the clang-built
  original under a per-function differential harness. **That harness does
  not exist yet.** The VERIFY column is emitted as
  `{"status": "NOT_IMPLEMENTED", "functions": null}` by design. Never put
  EMIT numbers in it.

There are two denominators, and every number names its own:

- **% of ALL** counts every main-file function definition with a body
  (free functions, methods, constructors, destructors). Definitions are
  deduped by source range within their TU. Header-defined functions are not
  counted once per includer.
- **% of OBSERVED** counts only the functions whose body bx actually
  inspected. A **HIDDEN** function sits inside a construct that bx rejected
  whole (a namespace, class or template), or is a top-level out-of-line
  method, constructor or destructor, which bx rejects as one item. Its
  blocker set is just the container's tag, so it is a pure lower bound.

## Caveat: lower and upper bounds

Every blocker set is a **LOWER BOUND** and every yield is an **UPPER BOUND**.
bx does not re-report cascades, and it does not look inside the constructs it
rejects. Supporting a tag **will** expose more tags. The census's single
calibration point (CLAUDE.md, TRACTOR section) predicted +14 and delivered +7,
so **halve any set-cover yield** before planning with it.

## Baseline 2026-11-01, bx 275e62a, EMIT stage

| module | TUs | functions | observed | hidden | EMIT | EMIT % of ALL | EMIT % of OBSERVED | VERIFY |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| woff2 @7b67e12 | 13 | 114 | 4 | 110 | 0 | 0.0 | 0.0 | empty |
| brunsli @c18b962 | 29 | 278 | 15 | 263 | 0 | 0.0 | 0.0 | empty |
| combined | 42 | 392 | 19 | 373 | 0 | 0.0 | 0.0 | empty |

All 42 TUs parse. 373 of the 392 functions (95%) are HIDDEN behind a single
tag, `namespace`: both projects wrap all of their code in `namespace woff2 {` or
`namespace brunsli {`. The 19 observed functions are CLI drivers (`main`,
file I/O helpers) and the two `extern "C"` entry points.

### Exposure probe (NOT a frontier number)

`--probe-erase-namespaces` erases the projects' own namespaces from a copy of
each checkout. It removes the opening lines, the `} // namespace` lines, the
`X::` qualifiers and `using namespace X;`. Every rewritten TU must still parse
under clang. The probe then re-runs the census, which shows what bx sees once
the namespace wall is gone.

- **woff2:** 13 of 13 TUs survive the rewrite.
- **brunsli:** 27 of 29 TUs survive. The rewrite collapses `internal::dec`
  and `internal::enc`, which causes a redefinition in `c/common/context.cc` and
  an ambiguous call in `c/enc/brunsli_encode.cc`. That drops 71 brunsli
  functions from the probe.
- **Result:** 276 of the remaining 321 functions are now OBSERVED, and **1** of
  them is EMIT-clean (`woff2_common.cc::CollectionHeaderSize`).
- **The greedy cover over observed functions is flat.** The best single tag
  frees 2 functions, and 15 steps reach 115 of 276. That figure is an upper
  bound; halve it.

## Reproduce

```sh
cd /home/j/transpiler5
SNAP=$(mktemp -d); git archive HEAD tools/emitrust-bx | tar -x -C "$SNAP"
nix develop -c python3 scripts/bx-frontier.py \
    --src  <empty-or-existing dir for checkouts> --fetch \
    --bx   "$SNAP/tools/emitrust-bx/emitrust-bx.py" --bx-rev "$(git rev-parse --short HEAD)" \
    --out  <private out dir> -j 4 --probe-erase-namespaces
```

`--fetch` shallow-fetches exactly the pinned SHAs: woff2, brunsli and the
brotli headers, about 95 MB in all. If an existing checkout is at any other
commit, the script refuses to run. Downloaded code is treated as data. No
project build script is run. The listed TUs are only passed to `clang++
-std=c++17 -fsyntax-only` (inside bx and the census). Include dirs reach clang
through `CPLUS_INCLUDE_PATH`, because emitrust-bx has no `-I` flag. brunsli's
CMake-generated `brunsli_export.h` is replaced by the empty-macro stub declared
in `modules.toml`. Re-running on the same pins and the same bx snapshot gives
the same numbers. This was checked from a fresh `--fetch`.

## Comparison to Cpp2Rust

The verifiable facts are these. Cpp2Rust (PLDI'26) reports translating these
two programs **whole**, with **runtime ownership checks** in the generated
Rust. The number in this directory measures a **different stage**: how many
functions a bijective, zero-runtime-check subset translator accepts
syntactically (EMIT). It is not a correctness claim, and it is not a
whole-program claim. **It must not be presented as head-to-head with Cpp2Rust
until the VERIFY stage exists**, and even then the units differ (functions
versus programs).
