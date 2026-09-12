## Track 4 RealWorld corpus (demand signal)

`test/RealWorld/` is a corpus of small, realistic, deterministic C programs
whose job is to GENERATE DEMAND — not to be a conformance target. The fixed
c-testsuite (220/220) no longer forces new work, so the remaining deep designs
(C99-43 ptr-to-ptr, C99-46 dynamic memory, and the pointer-model extensions)
are ranked by how often real programs actually hit them, measured, rather than
by speculation. The corpus need not be cleared; a program that stays rejected
is a standing backlog item, and one that starts transpiling (after a deep-design
wave) ratchets into the transpiled set as a regression guard and runtime perf
workload.

**Harness.** `test/RealWorld/run_realworld.py` (modeled on the c-testsuite
ledger runner, `test/CTestSuite/run_c_testsuite.py`) drives each program
through `emitrust-cc --emit=crate --build`. A program is a single top-level
`Inputs/<name>.c` (one TU) or an `Inputs/<name>/` subdirectory whose `*.c`
compile together (multi-TU). Outcomes: **REJECTED** (a located diagnostic, rc≠0
— the demand signal, tagged by blocker category), **TRANSPILED** (the crate
built and its stdout matched a `clang -std=c11` native build of the same
sources — the differential oracle, the same philosophy as the EndToEnd
differentials), or **MISCOMPILE** (built but crashed / exited non-zero /
diverged from native — always fails, quarantine aside). A two-way ratchet
against `expected-transpile.txt` (programs expected to transpile) plus an
(empty) `known-miscompiles.txt` quarantine mirrors the ledger; the runner also
prints a **blocker-tag tabulation** — the survey signal W4.1 reads. Gated in
`ninja check-emitrust` via the `test/RealWorld/realworld.c` lit stub
(`REQUIRES: cargo`); the corpus lives under `Inputs/` so it is excluded from
lit discovery (`config.excludes = ["Inputs"]`) yet regression-protected through
the stub. `argv` VALUES that fit the C99-43 C3 admitted read grammar (whole
`argv[i]` fed to `printf` `%s`, `argv[i][j]` bytes, argc loops) now import as
the argv cursor table (`argv-echo` transpiles); every other argv use is still
dropped at import, so those command-line-argument programs reject.

**Blocker tags** are a heuristic over the first diagnostic line: the
system-header symbol is parsed out (`free`/`realloc`/`malloc`/`calloc` →
`dynamic-memory`, else `libc:<name>`); the shared "pointer assigned a
non-address value" / "no known target object" wording is refined by reading the
cited source line (an alloc call → `dynamic-memory`, `strchr`/`strrchr` →
`strchr-result-bind`, else `pointer-local-nonaddress`); a compiler crash
("PLEASE submit a bug report" / "Stack dump") tags `crash`.

**W4.0 snapshot (13 programs): 5 transpiled, 8 rejected, 0 miscompiled.**
Transpiled (pinned regression guards / perf workloads): `base64`, `calc`
(two-TU), `logger` (two-TU), `sieve`, `word-count`. Rejected, by blocker
frequency: **`dynamic-memory` ×2** (`linked-list`, `malloc-stack` — local
`malloc` + `free`, C99-46), then one each of `returned-pointer` (`binary-tree`,
C99-43), `strchr-result-bind` (`grep-lite`), `self-ref-pointer-member`
(`union-find`, C99-43), `global-string-cursor` (`expr-eval`), `argv`
(`argv-echo`, C99-43 / argv-values-dropped), and **`crash` ×1** — `crc32`
SEGFAULTS the importer when a `const char *` VARIABLE pointing into a string
literal is passed to a subscripted (slice) parameter: `emitBorrowArgument`
resolved a base-less literal-backed pointer and fell through every base-keyed
guard to a null-base error branch that dereferenced `pointer->base->getName()`
(the `(unsigned char)` cast is a red herring — `crc32("literal", n)` and an
array argument both already transpile; the trigger is the const-`char*`
variable into a literal × slice parameter). The crash is
a robustness bug (the importer must emit a located rejection, never a segfault);
it is pinned here as the highest-priority survey finding and mapped to a
follow-up fix wave, not fixed in the test-only W4.0. W4.1 tabulates and ranks
these to drive W4.2+ (the ranking overrides the plan's pre-baked ladder order).

**W4.2 update: `union-find` cleared.** The owner-struct self-reference
extension (FR-37/FR-38, six stages) resolved the `self-ref-pointer-member`
blocker entirely: `union-find` now TRANSPILES and differentially matches a
clang-native build byte-for-byte (`test/RealWorld/expected-transpile.txt`
ratchet-updated forward; verified live via `run_realworld.py`:
`total=13 transpiled=6 rejected=7 miscompiled=0`, zero regressions/
improvements against the manifest). The `self-ref-pointer-member` tag is
retired — no remaining rejected corpus program carries it. `binary-tree`
(`returned-pointer`) was separately re-checked after this work landed and
still rejects, unchanged, at the same `insert` call site: the returned
pointer there roots in a `malloc`'d node (a callee-local, non-array-backed
allocation), which the array-rooted owner-index-return mechanism (FR-36)
does not and cannot cover — the blocker is dynamic memory (C99-46), not
the returned-pointer machinery itself. Current tally: **6 transpiled, 7
rejected, 0 miscompiled** — `dynamic-memory` ×2 (`linked-list`,
`malloc-stack`), and one each of `returned-pointer` (`binary-tree`,
now confirmed malloc-rooted), `strchr-result-bind` (`grep-lite`),
`global-string-cursor` (`expr-eval`), `argv` (`argv-echo`), and `crash`
(`crc32`, still unfixed, still the highest-priority robustness finding).

**W4.1 ranked survey.** The corpus rejections, ranked by
programs-unblocked-per-cost (RFC-gated features sink; a robustness crash
floats to the top regardless of feature value). This ranking OVERRIDES the
plan's pre-baked ladder order — the demand signal, not speculation, sequences
the remaining deep designs. `self-ref-pointer-member` is retired (union-find
transpiles); seven rejected programs remain.

| Rank | Blocker | Programs | RFC? | Cost | Next wave |
|--|--|--|--|--|--|
| 1 | crash | 1 (`crc32`) | no | low | **RESOLVED** — literal-backed slice-argument fix (W4.1 commit 2) |
| 2 | dynamic-memory | 2 (`linked-list`, `malloc-stack`) | no | med | W4.2 ladder a (local const-size malloc + free) |
| 3 | strchr-result-bind | 1 (`grep-lite`) | no | med | bind a strchr result to a pointer local |
| 4 | global-string-cursor | 1 (`expr-eval`) | no | med | global `char*` into a literal, walked as a cursor |
| 5 | returned-pointer | 1 (`binary-tree`, ALSO dynamic-memory) | likely | high | pointer to a heap object |
| 6 | argv | 1 (`argv-echo`) | — | high | **LANDED** — C99-43 C3 argv cursor table (see the C99-43 FRONT C3 record) |

Rationale: the `crc32` crash is a robustness override — a compiler must never
segfault, and the fix is cheap and localized, so it precedes the entire feature
ladder. `dynamic-memory` clears the most single-blocker programs (2) at moderate
non-RFC cost, so it leads the ladder (W4.2). `binary-tree` is DOUBLE-blocked
(returned-pointer AND dynamic-memory — its returned pointer roots in a
`malloc`'d node, not an array), so it will not clear until both land; it sits at
the RFC-likely tail. `argv` needed the second-order cursor-table
generalization (W4.3), now LANDED as C99-43 C3 (see the FRONT C3 record).
Ranks 2–6 are future waves; only rank 1 is actioned this session.

**W4.1 update: `crc32` crash fixed.** `emitBorrowArgument` gained a
`literalBacking` branch (right after `emitPointerRValue`, before any base-keyed
path): a `const char *` variable pointing into a string literal passed to a
subscripted slice parameter now reslices the literal's backing rather than
crashing. A subscripted `const char *` parameter is a MUTABLE slice
(`!emitrust.mut_ref<!emitrust.slice<i8>>`) — so the fix rematerializes a fresh
mutable backing of the literal at the call site (mirroring the direct-literal
argument path `f("abc", n)`; writing through a pointer to a string literal is
UB, so the per-call copy is unobservable), while a shared byte-slice parameter
borrows the const backing read-only. (This is a deliberate refinement of the
W4.1 plan, which assumed the parameter was a shared const ref and would
"reject if mutable"; the parameter is in fact mutable, and rejecting it would
have kept `crc32` blocked, so the sound copy path is used instead.) `crc32`
now TRANSPILES and matches a `clang -std=c11` native build byte-for-byte;
the manifest ratcheted forward to include it. RED→GREEN pin:
`test/Import/C/pointers-param-literal-slice.c`. The branch fires only for the
previously-crashing literal-into-slice shape, so it perturbs no existing
output. **Current tally: 7 transpiled, 6 rejected, 0 miscompiled** — transpiled
add `crc32` to `base64`/`calc`/`logger`/`sieve`/`union-find`/`word-count`;
rejected are `dynamic-memory` ×2 (`linked-list`, `malloc-stack`), and one each
of `returned-pointer` (`binary-tree`), `strchr-result-bind` (`grep-lite`),
`global-string-cursor` (`expr-eval`), and `argv` (`argv-echo`). The `crash`
tag is retired.

**W4.2e update: `dynamic-memory` resolved (rank 2).** The two
`dynamic-memory` programs both TRANSPILE now (FR-39, C99-46 Stage 1):
`malloc-stack` via the Part A local flat buffer, `linked-list` via the
Part B index-handle node pool. The `dynamic-memory` blocker tag is retired
from the corpus — no remaining rejected program carries it. `binary-tree`
stays rejected but as `returned-pointer` ONLY (a returned `malloc`'d node
dangles; the pool model deliberately keeps it out — see FR-39). **Current
tally: 9 transpiled, 4 rejected, 0 miscompiled** — transpiled add
`malloc-stack` and `linked-list`; the four rejected are `returned-pointer`
(`binary-tree`), `strchr-result-bind` (`grep-lite`), `global-string-cursor`
(`expr-eval`), and `argv` (`argv-echo`). Remaining ranked demand (rank 3+):
strchr-result-bind, global-string-cursor, returned-pointer (RFC-gated,
W4.5), and argv (W4.3).

**Csmith DEFERRED** (documented decline): the flake toolchain is off-limits
this cycle, so no new generator dependency is added. The seeded differential
fuzzer (`test/Fuzz`, generator v5) plus this hand-authored corpus are the
differential coverage; a Csmith leg — which would need a flake input — is
future work if the fuzzer's coverage gaps demand it.

