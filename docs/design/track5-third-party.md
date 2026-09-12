## Track 5 Third-party validation (external demand signal)

Track 4's corpus is authored by this project. That has now produced two
demonstrated blind spots in one week -- every program was written with a
`main`, which hid the fact that LIBRARY projects were unreachable under any
flag (FR-51); and the paper harness enumerated multi-TU tests as single files,
which manufactured six spurious repair-search "rescues" (recorded under
FR-52). A self-authored benchmark encodes its authors' assumptions twice, once
in the code and once in the harness, and neither encoding is visible from
inside.

Track 5 therefore measures UNMODIFIED THIRD-PARTY C, cloned at pinned commits
and compiled with the project's OWN flags (via `cmake
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` feeding FR-45's `--compdb` wherever the
upstream build supports it). Selection within a repository must be MECHANICAL
and stated -- every `.c` in a directory, or the compile database wholesale --
because hand-picking the files that happen to work would reproduce exactly the
defect this track exists to correct.

**The measurement separates two failure kinds, and conflating them would make
the result meaningless:**
 - `PARSE_FAIL` -- clang could not build an AST at all (a missing config
   header, an absent target define). This is a CONFIGURATION limitation of the
   harness, and says nothing about the supported subset. How much
   configuration a real codebase needs before the tool can even look at it is
   itself a product finding, not a footnote.
 - `REJECT` -- parsed cleanly, but could not be translated. This is the real
   demand signal, and the ranked root-blocker tally over it (FR-49) is what
   should sequence work after this track.

Outcome vocabulary per unit: `TRANSLATED_FULL` (crate builds, zero stubbed,
zero dropped), `PARTIAL` (crate builds, some stubbed or dropped), `NO_CRATE`,
`PARSE_FAIL`.

Initial target set, chosen to span best case to realistic case rather than to
flatter: small self-contained libraries (cJSON, heatshrink, tinycbor); RTOS
and networking cores (FreeRTOS-Kernel, lwIP `src/core`); and crypto/DSP
(mbedTLS `library/`, tinycrypt, CMSIS-DSP). CMSIS-DSP is the deliberate best
case -- large amounts of pure integer and fixed-point math, close to the
supported subset -- and mbedTLS the realistic one; the GAP between those two
is the most informative number the track can produce.

**RESULTS (2026-07-31). 289 per-unit measurements across 11 repositories;
288 parsed.**

```
                     units parsed  full partial no-crate builds  items ported
mbedtls                163   163     0      77       31     30   3976/8415 47%
lwip src/core           69    69     1*     34        5     30   1139/2354 48%
tinycrypt               32    32     0      15        0      5     66/177  37%
FreeRTOS-Kernel         31    31     0       6        3      6      61/254 24%
CMSIS-DSP               92    92     0       0       91      0        0/0   0%
tinycbor                13    13     0       0       13      0        0/0   0%
cJSON / heatshrink /
  nanopb / tiny-AES     34    33     1*      8        4     10      70/385 18%
TOTAL                  289   288     2*    143      141     85  5328/11773
```

`*` BOTH `TRANSLATED_FULL` results are VACUOUS -- empty translation units
behind a disabled `#ifdef` (`graph_items=0`, a two-line crate). **The real
count of third-party translation units translated in full is ZERO.**

Findings, in order of how much they should change what happens next:

1. **`PARSE_FAIL` = 1 of 289.** Every project's own compile database was
   consumed successfully and clang built an AST for essentially everything.
   There are no configuration excuses in this data; every number below is a
   genuine translation limit. Configuration effort was also LOWER than
   expected -- both FreeRTOS and lwIP ship usable config headers, and the
   measurement authored none, only ~36 lines of build glue.

2. **The item fraction flatters. By KIND, functions are 1-6%.** lwIP reads
   48% of items only because enums (100%) and records (45-63%) translate well
   while functions do not: FreeRTOS 5/129 (3.9%), lwIP 45/714 (6.3%), nanopb
   1/81 (1.2%). Nor is `ported` transitive -- walking the emitted call graphs,
   STUB-FREE functions are 3/39, 26/88 and 1/5. The 39 functions with real
   bodies are byte-swappers, config-empty inits and one-line wrappers. No
   queue operation, no scheduler function, no TCP state-machine function.

3. **A pointer stored in a struct field or a global is the dominant
   construct.** `rejected-type-cascade` alone is 51.6% of the corpus-wide
   root tally (3384 of 6553), and it is a cascade from that root:
   one rejected struct disqualifies every type and function naming it. The
   roots are each project's central types -- `netif`, `pbuf_custom`,
   `stats_`, the `xSTATIC_*` FreeRTOS types, `pb_callback_s`, mbedTLS's
   context structs. This is **C99-43**, which was the last unchecked box on
   the C99 roadmap (now closed on its four-front disposition — C1 landed,
   C2 NO-GO, C3 landed, C4 on FR-58's axis); the pointer-in-struct-field
   cascade this data ranks first is the member-pointer front, admitted
   only for the FR-37/FR-38 array-rooted subset and otherwise still the
   located rejection this tabulation counts.

4. **Dynamic memory is NOT the barrier: 4 occurrences in 289 units (0.06%),
   and twice in the whole of lwIP.** This is the sharpest correction the
   third-party data delivers. FR-39 and the container/fat-op work invested
   substantially in modelling `malloc` because the SELF-AUTHORED corpus
   ranked `dynamic-memory` as its top blocker twice. Real embedded C
   allocates statically and threads pointers through structs. The demand
   signal was measuring its own authors.

5. Defects the run exposed, each reproduced in a handful of lines and fixed
   or filed: the Pass-A planner recovery hole (FR-53), the canonicalized
   unsigned-op legalization gap (FR-54), and two classes of emitted crate
   that `rustc` rejects (FR-55). Plus, still open: a SEGFAULT on lwIP's
   121-TU target (no diagnostic, deterministic); NON-TERMINATION of
   `--incremental` on two FreeRTOS files where strict mode finishes in
   seconds; and `--compdb` refusing a GCC-produced compile database
   (`-Wlogical-op` reaches clang as `-Werror,-Wunknown-warning-option`),
   which is the first thing a real embedded user would hit since almost every
   such project ships a gcc build.

6. **FR-41's colouring is not predictive on third-party code.** CMSIS-DSP
   colours 100% green and ports 0%; mbedTLS colours 99.6% green and ports
   47%. The CONTRACT holds -- these are false greens, the deliberately safe
   direction, and false reds remain zero -- but the ARGUMENT does not. FR-43's
   permissive-unsoundness rationale was that a false green costs one probe
   because the search repairs it; the search repairs nothing here. On a
   self-authored corpus the colouring looked exact; on real code it is 50-100
   points out with no repair behind it.

**CORRECTION (2026-07-31, same day).** The first aggregation of this track
reported 434 units and a 73%/52% blocker split. Both were inflated by a
defect in the AGGREGATOR, not in the measurements. It globbed
`thirdparty_*.csv` and excluded only `_blockers.csv` by NAME, so the
per-diagnostic tally files (137 rows) and the project-level aggregate rows
were counted as translation units; and its blocker sum matched
`*_blockers.csv` against a group that ships BOTH a direct and a root tally,
double-counting it. Corrected: **289 distinct per-unit measurements**,
`rejected-type-cascade` **51.6%** of 6553 root-tagged rejections,
`dynamic-memory` **4**. The aggregator now selects per-unit files by SCHEMA
(the presence of an `outcome` column) rather than by filename. A further
correction: the "functions are 1-6%" figure holds for the RTOS/networking
group only -- mbedTLS is 36.1% and tiny-AES-c 50.0% by function. None of the
qualitative findings change; the numbers do, and the wrong ones were
reported before this note.

**PREMISE REFUTED (C99-43 spike, 2026-07-31).** The conclusion above --
"73% of rejections reduce to a pointer stored in a struct field or global,
therefore C99-43" -- was derived from the blocker TAG
(`rejected-type-cascade`) without checking what the cascade roots actually
were. A spike measured them, and **a pointer stored in a struct field is
ALREADY SUPPORTED**: `mapStructFieldType` maps every single-level
non-function pointer field -- including `void *`, `char *` and `T **` -- to a
plain `i64` cursor and never fails. Verified directly: `struct A { struct A
*next; }`, `{ void *payload; }`, `{ char *name; }`, `{ int **pp; }` and
`{ void (*cb)(int); }` all import cleanly today.

What rejects is a pointer type in a **composite type position**, all four
shapes funnelling into one residual in `mapType`. Over 80 deduped root record
rejections:

| root cause | records | share |
|--|--:|--:|
| fn-ptr field whose signature names a pointer | 47 | 58.8% |
| union with a pointer arm | 9 | 11.2% |
| union arm cannot alias the storage slot (**not a pointer problem**) | 8 | 10.0% |
| array-of-pointer field | 7 | 8.8% |
| other (volatile, fn-ptr pointer result, nested) | 9 | 11.2% |

And the survey's #1 root is not a pointer problem at all: **`netif` fails on
`ip_addr`'s `union { ip6_addr_t; ip4_addr_t; }`** -- the C99-44 one-slot
union model. `netif` + `ip_addr` alone are 258 cascade-blocked items, 27.5%
of all cascade damage in the corpus, from one union of two small structs.

**The ownership census is the decisive table.** Of the 661 pointer fields the
importer ALREADY accepts: 48.5% store a pointer received from a caller (a
borrow needing a lifetime), 19.8% are never written, 17.7% copy another
pointer, and only **6.4%** are the allocation-or-array-element shapes that
index handles and arenas address. FR-37/38/39 -- the direction this project
has been extending -- covers 6.4% of real pointer fields. Lifetimes would
cover the 48.5%, and would break the `Copy + Default` struct invariant the
whole dialect rests on.

**Recommendation: CASCADE CONTAINMENT, not pointer translation.** Emit a
rejected composite-position field as a placeholder (and array-of-pointer as
`[i64; N]`, which is the cursor convention already used for scalar pointer
fields), keeping the record importable, and reject at the ACCESS site. This
translates no new pointers. Measured over the 212 TUs reporting in both
configurations: records dropped 117 -> 5 (-96%), cascade-blocked items
705 -> 29 (-96%), items ported +18.9%, **functions ported 8.3% -> 11.7%
(+37.7%)**. The precedent is already in the tree (`unionByteArrayArms`, the
FAM field omission): a type-level concession with rejection deferred to use.

Three findings that must travel with it:
 - **43 TUs stop emitting a crate**, because containment makes previously
   cascaded structs import and thereby makes PRE-EXISTING non-recoverable
   failures reachable -- 39 are `extern global variable not defined in any
   translation unit`, 2 the FR-42 recovery non-termination, 2 legalization.
   Stage 1 must not ship without fixing those first.
 - **The union placeholder is NOT ready**: it silently emitted invalid Rust
   with no diagnostic in its first cut, and even guarded it breaks 42 crates
   with `E0609` while buying 16 items. Rejected.
 - **FR-41's doctrine becomes false.** `ItemColoring.h`'s claim that "a
   missing TYPE cannot be replaced… so type-poisoning is transitive" is
   exactly what containment refutes. The colouring and FR-49's root
   attribution would model a cascade that no longer happens. This is also
   Contribution 1 of the accompanying paper, and needs qualifying there.

**Revised priority.** Not C99-43. In order: cascade containment (Stage 1,
gated on the enabling fixes), then fn-ptr components through the parameter
mapper (Stage 2, which turns 47 of 80 roots into working callbacks), then the
UNION model (Stage 3 -- 30.7% of the cascade and the real `netif` blocker,
and a C99-44 question, not a pointer one). Index handles and lifetimes: never,
on the measured 6.4% and the invariant break respectively.

**Track 5 re-measured after FR-53/54/55 (same pinned SHAs, same compile
databases, same harness).** The three defects the first run exposed were
fixed and the affected repositories re-measured:

| | before | after |
|--|--|--|
| small libs: units emitting NO crate | 14 of 22 | **2 of 22** |
| small libs: crates that `cargo build` | 8 | **20** |
| small libs: ported items | 52 | **169** |
| CMSIS-DSP: units emitting a crate | 0 of 91 | **83 of 91** |
| tinycrypt + tiny-AES: emitted crates building | 6 of 25 | **25 of 25** |

The small-libs ported FRACTION falls 18.9% -> 12.0% while absolute ported
items more than triple, because the denominator grew fivefold: the twelve
rescued units contribute their entire item graphs, which previously counted
as nothing at all. A fraction that falls because a benchmark stopped hiding
its failures is the honest direction, and it is recorded here rather than
quietly replaced by the absolute count.

**The FR-42 caveat was wrong and is corrected.** It claimed planner
rejections "cannot be attributed to a single droppable item". All seven
`emitError` sites in the two rejecting planners carry a location inside one
declaration, and FR-53 attributes every one. The claim survived because
nothing in the self-authored corpus exercised it -- the same failure mode as
the `main`-only blind spot and the harness mis-invocation, and the third time
a documented caveat turned out to be the dominant real-world behaviour.

**Recovery still stops at the IMPORT boundary, and that is now the leading
blocker.** The two units that still yield nothing fail AFTER the declaration
walk: `cannot translate non-finite floating-point constant` from the Rust
emitter, and `failed to legalize operation 'scf.if'` from the conversion
pipeline. A single un-legalizable operation costs the entire crate exactly
the way a planner rejection used to. FR-42's per-item recovery has no
counterpart in the pass pipeline or the emitter; giving it one is the natural
successor to FR-53.

**Track 5 spot re-measurement (2026-08-14, post FR-67/68/69/70, fresh
HEAD clones, not the pinned SHAs — a smoke check, not a comparable
re-run).** cJSON (2 units) + tinycrypt (15 units), solo-TU
`--emit=crate --crate-type=lib --incremental` with each project's own
include dir: **17/17 units emit crates and every crate builds.**
Aggregate 95 ported / 76 stubbed / 21 dropped over 522 graph items.
The ranked residual diagnostics on rejected items (deduped): void
pointer parameter x10 (tinycrypt's `_set` memset-alike is the root of
most of its cascade), pointer assigned a non-address value x11,
struct-cascade roots x12, call to unimported function x10, aliasing
mutable pointer arguments x8, ArrayToPointerDecay x7, pointer struct
member x7 — i.e. the demand ranking that motivated Stage 2/3 holds on
live code, and no NEW defect class appeared (no segfault, no
non-termination, no rustc-rejected crate on these 17 units). The
strict-mode (no `--incremental`) baseline is 0/17 — recovery mode is
what carries external code, consistent with Contribution 3-4's
disposition.
FOLLOW-UP (same day, after FR-71/72/74/75 landed from this probe's
ranking): tinycrypt utils.c is 4/4 real items (_set/_copy/_compare all
ported); the solo-TU sha256/aes stubs correctly PERSIST because their
TUs call `_set((uint8_t*)s, 0, sizeof(*s))` — a STRUCT erased to bytes,
which the FR-75 consensus scan rightly declines (transmute territory,
not a byte view). The residual external demand is now exclusively the
recorded wave-scale fronts: struct-as-bytes erasure, the member-pointer
linked-list shape (cJSON x11), and aliasing mutable arguments
(ecc/cbc/hmac x8) — i.e. Stage 2/3 of the cascade plan plus C99-44.

**SESSION CODA (2026-08-14/15, the autonomous factory run,
FR-67..FR-98).** One continuous session ran the house protocol
end to end thirty-two times: 30 FRs landed, 1 spike NO-GO recorded
with its measured table (FR-82, whose redirect became FR-83), and
1 candidate refuted before any code (the FR-79 evidence clause).
Alongside: W2.14 std::variant closed the Cpp17Suite frontier, the
FR-63 quality harness ran 8 accepted iterations across 3 epochs to
its designed plateau (full-corpus clippy 1621 -> 331, -80%,
held-out improving at every step), and the gcc-15/libstdc++-15
host drift was fixed. External outcomes, all measured: lwIP
src/core 20 -> 36 of 38 units importing; tinycrypt/cJSON 95 -> 109
of 326 items; tiny-AES-c to its 23/24 ceiling (a complete
real-world AES in safe byte-identical Rust); heatshrink encoder
16 -> 23/28 and decoder 8 -> 12/17 (the FAM representation, owned
Option members, and the two-wall tail-local composition). The
suite grew 562 -> 665 tests, 100% at every one of ~80 commits;
the byte-diff oracle caught two real miscompiles and one wrong
constant mid-wave that compile-clean evidence would have missed;
three latent defects (E0609 leak, dangling fn-ptr names, the
over-count malloc shape) were found by the external probes and
closed with backstops. The method held: every claim
STRUCTURAL-or-MEASURED, every refusal located, every spike
verdict recorded with its evidence, and the measurement loop —
land, re-probe, let the diagnostic name the next FR — drove the
entire arc without a single human-picked work item. Remaining
recorded fronts for the next session: the five heatshrink encoder
walls (returned-pointer alloc first), the decoder's output_info
members, cJSON's linked-list member pointers, ecc's aliasing
in-place bignum ops, tcp_in/udp's contract machinery, and the
Sema-fact side-channel RFC (CFG dominance for FR-88-class guards
is the ranked first spike).

**THE TINY-AES MILESTONE (2026-08-15, FR-91..FR-93).** The widened
corpus (tiny-AES-c, heatshrink) was mined the same way: FR-91 member
windows on byte-region roots (the CTS-BR boundary opened), FR-92 the
2D-array-pointer cast + state_t* param family, FR-93 window-backed
pointer locals + multi-base call dispatch with the split_at_mut arm
(the deepest wave — the naive borrow image failed measured borrowck
and the sound dispatch was byte-diff-proven). Outcome: **tiny-AES-c
at 23/24 items — its measured ceiling** (record `missing` is inherent
to the byte-region model): a complete real-world AES (ECB/CBC/CTR)
transpiles to safe Rust byte-identical against the clang native.
Heatshrink's residue is the FAM representation (recorded NO-GO,
future FR); tinycrypt's is member-FIELD escaping stores.

**THE TINYCRYPT SEAM ARC (2026-08-15, FR-86..FR-90).** After the lwIP
campaign, the per-unit diagnostics of the tinycrypt/cJSON corpus were
mined seam by seam, each FR's re-probe naming the next: FR-86 offset
member-arrays + the decomposed-root bin its census discovered (and two
design-entry bins DISPROVEN), FR-87 member-array regions for the
hosted byte family (byte-diff caught a real miscompile
mid-implementation), FR-88 nullable byte-slice params as Option<&[u8]>
(the fold-flip trap measured and closed), FR-89 the void*-BitCast
peel (one line of peel; the spike corrected the entry's motivating
site), FR-90 member-address arguments on decomposed roots (the
"loop breaks it" framing refuted — the trigger was null-compare
demotion). Corpus: 95 -> 109 of 326 items ported (+15%), all 17
crates building throughout. TERMINAL RANKING of this corpus: the
residuals are exclusively the recorded new-idea fronts — the
member-pointer linked-list shape (cJSON, x11+x12 cascade), aliasing
mutable arguments (ecc's in-place bignum ops, x9 — the copy-in image
is UNSOUND because C callees may observe their own writes through
the alias, the same hazard class FR-82 measured), and pointer struct
members (x9). Near-reach seam mining here is done; further external
progress needs either the deferred fronts or a WIDER corpus.

**THE LWIP CAMPAIGN (2026-08-14/15, FR-76..FR-84).** The
cascade-containment stages plus the measurement loop ran as one
continuous arc — each landed FR's lwIP re-probe named the next FR —
and src/core went from 20 of 38 units importing to **35 of 38**
(+75%): FR-76 fn-ptr components (Stage 2), FR-77 fn-ptr-constant
fail-loudly (the probe's own regression, caught and fixed same-day),
FR-78 opaque-union containment (Stage 3 — the historically-rejected
model landed with COMPLETE access interception; the corpus-wide
cascade wording vanished), FR-79/80/81 the requirement-type matrix
(const-struct getters, &'static address-carrying items, mutable
struct get/set), FR-82 a NO-GO whose measured shape table REDIRECTED
the effort, FR-83 union-arm byte-views + the deref-cancelled-& fold
(seven units flipped in one wave), FR-84 the module-first lookup
defect (two more). The three residuals have recorded root causes:
tcp_in/udp keep CALL-CONST/ESCAPE/PTRCMP address shapes of IP_DATA
(the FR-82 table — admission needs explicit callee-non-mutation
contract machinery, never a silent assumption), etharp needs a
byte-region const requirement for the packed ETHBROADCAST record.
Method note for the paper: two design refusals (FR-52 globals,
the Track 5 union placeholder) were REVISED on new measured evidence
with new mechanisms, one candidate (FR-82) was refuted BY its own
spike's measurement before any code was written, and one evidence
clause (FR-79's) was falsified and rewritten — the
STRUCTURAL/MEASURED discipline holding under autonomous operation.

**External-validation lessons, consolidated (and the paper's disposition).**
The accompanying paper's four contributions do NOT survive third-party
validation equally, and the split is the durable lesson -- more than any
single number, because the numbers move (this section was re-measured after
FR-53/54/55; the small-libs table above is the current state).
 - **Contribution 1 (emission-derived colouring) is corpus-bound in its
   MEASUREMENT, sound in its RULE.** Zero false negatives across 1337 items
   is a fact about the corpus we wrote; on unmodified third-party C the same
   probe admits ~99.6-99.7% of items (CMSIS-DSP 100% green / 0% ported,
   mbedTLS 99.6% / 47%). The contract holds -- false greens are the safe
   direction and false reds stay zero -- but the precision claim does not
   generalise (finding 6; the `ItemColoring.h` doctrine qualification at the
   cascade-containment record above).
 - **Contribution 2 (subtractive search) splits: the asymmetry SURVIVES, the
   cost model does NOT.** That the search is unsound in exactly one direction
   is STRUCTURAL -- it follows from the search being subtractive, holds for
   any approximation seeding any subtractive search, and no corpus can refute
   it. But FR-43's rationale that a false green "costs only one probe because
   the search repairs it" fails: the search repairs NOTHING on non-synthetic
   input, so a false green costs an over-promising colouring with nothing
   behind it.
 - **Contributions 3-4 (yield, ratcheted progress) are real but small, and
   about PARTIAL translation.** Whole-program yield on unmodified third-party
   embedded C is ZERO (both `TRANSLATED_FULL` results are vacuous empty TUs).
   The value the data supports is PARTIAL translation as a demand-and-backlog
   instrument (post-FR-53/54/55: CMSIS-DSP 83/91 crates building, small libs
   20/22) plus the methodology itself -- not end-to-end translation of
   arbitrary embedded C, which the paper's headline measurement set would let
   a reader assume.
 - **The durable rule.** Every claim this project validated ONLY on
   self-authored input has later failed on real input -- the `main`-only
   blind spot (FR-51), the harness mis-invocation (FR-52), the FR-42
   attribution caveat, and now Contribution 1's precision: FOUR recurrences,
   which is itself the finding. Going forward every claim is classified as
   STRUCTURAL (true by construction, corpus-independent -- e.g. the search
   asymmetry, the byte-diff oracle's soundness) or MEASURED (state its corpus
   and treat as un-generalised until an EXTERNAL corpus confirms it). A
   self-authored benchmark is not evidence of external validity; it encodes
   its authors' assumptions twice, in the code and in the harness.
 - **Paper commit disposition.** The orphaned worktree commit `006ca14`
   ("paper: add external-validation section and requalify the claims it
   breaks", 2026-07-31) is SUPERSEDED, not rebased: its numbers predate
   FR-53/54/55 (it records CMSIS-DSP at 0 crates against the re-measured 83),
   and it is a full paper snapshot on a stale base rather than a surgical
   delta. The lessons are captured here against current data; if the paper's
   External Validation section is written into `paper/paper.tex`, it must draw
   from the re-measured Track 5 numbers above, not from that commit.


### The C++ demand measurement, 2026-08-21 (and what it says about the roadmap)

Track 5's methodology, run for the first time on C++. Eleven pinned
repos -- tinyxml2, pugixml, jsoncpp, 2048.cpp, CHIP-8-Emulator, cxxopts,
docopt.cpp, spdlog, unordered_dense, tinyrenderer,
raytracing.github.io -- 105 translation units, every one parsing clean
under `clang++ -std=c++17` before measurement. Per unit:
`--emit=crate --crate-type=lib --incremental` for the progress JSON,
plus `--emit=coloring` to root the cascades FR-49 leaves unattributed.
Counts are DEDUPED by (project, source location, symbol), so a header
item shared by 43 pugixml TUs counts once.

Outcome: **0 of 105 TRANSLATED_FULL**, 95 PARTIAL, 10 NO_CRATE.
2020 of 8851 graph items ported (22.8%). 6669 deduped blocked items
across 97 distinct wordings. The ranking is stable under three
weightings (corpus-wide, projects-equal-weighted, pugixml-excluded), so
pugixml's 42 units do not manufacture it.

| construct | items | % | root or cascade | wave |
|---|---|---|---|---|
| overloaded operator on a value class | 2212 | 33.2% | root 587 + cascade 1625 | W2.25 (new) |
| base class with a destructor and/or virtuals | 2229 | 33.4% | root | partly W2.19; the destructor half was on NO wave |
| method of an unimported class | 502 | 7.5% | pure cascade | FR-112 |
| destructor / value-semantics use sites | 343 | 5.1% | root, 80 of 105 units | W2.17 follow-on |
| `struct X was rejected` where X is probe-green | 284 | 4.3% | cascade, unreported root | mostly FR-102/FR-107 |
| virtual method, no base involved | 295 | 4.4% | root | W2.19 -- and this is ALL it buys alone |
| C pointer model | 113 | 1.7% | root | C-side |
| references | 70 | 1.0% | root | -- |
| STL not recognized (ostream 20, tuple 18, istream 16, shared_ptr 6) | 62 | 0.9% | root | partly W2.21 |
| exceptions | 11 | 0.2% | root | W2.24 |
| copy/move constructor semantics | 4 | 0.06% | root | W2.23 |
| **`std::unique_ptr` / `make_unique`** | **0** | **0%** | -- | **W2.21** |

**SUPERSEDED (2026-08-22): the full 11-repo re-sweep below replaces
this table.** It predates fifteen landed increments AND the FR-115
attribution fix that was skewing every count. Kept for the audit trail;
size nothing off it.

**THE HEADLINE, and it is the thing naive counting gets wrong: 59.2% of
all blocked items -- 3949 of 6669 -- never had their own construct
examined.** Their diagnostic is `method of an unimported class` or
`struct 'X' was rejected`. ONE unsupported member function rejects the
ENTIRE class.

**CORRECTION, 2026-08-21, from FR-112's spike -- this table's own
attribution of that 59.2% was wrong.** The cascade is real, but its
ROOT is not the overloaded operator. Attributing tinyxml2's 180
cascaded methods to their owning classes puts `base class with a
destructor` and `virtual destructor` at five of the six biggest roots;
only three classes root on an operator and all three immediately hit
the copy/move-constructor gate behind it. jsoncpp: 34 destructor-family
roots, 4 operator roots. So the front with the demand behind it is
**W2.26 polymorphic RAII**, not FR-112 and not W2.25. FR-112 measured
tinyxml2 items 19/48 -> 22/48 and jsoncpp unchanged. It is still worth
doing -- it converts opaque cascades into rankable diagnostics, which
is the FR-115 problem -- but it is not an unlock. The alphabetic-minimum
root tag described below is exactly how this table mis-attributed it.

**THE ROADMAP WAS AIMED AT 4.7% OF MEASURED DEMAND.** W2.19 + W2.21 +
W2.23 + W2.24 together are 316 of 6669 items. `std::unique_ptr` appears
in ZERO diagnostics across 105 real C++17 units; it shipped anyway
(W2.21) because it was already in flight and the work is correct, but
the sequencing was wrong and this table is why the order changed.

TWO CAVEATS, both in the honest direction. Cascade shadowing
UNDERSTATES the later waves: raytracing.github.io is built on
`shared_ptr<hittable>` throughout yet yields ONE `shared_ptr`
diagnostic, because its classes die at inheritance and `vec3`'s
operators first. And the FR-41 probe's root tag is the ALPHABETIC
MINIMUM of a class's tag set (`base-class` < `destructor` <
`overloaded-operator` < `virtual-method`), so every reported root
shadows the constructs after it. 4.7% measures what is REACHABLE today,
not eventual need.

The run also produced FR-113, FR-114 and FR-115 as minimal repros, and
FR-115 is the reason this table needed two tools instead of one: 6193
of 8851 items (70%) are status `missing` with no diagnostic at all.

### Re-measurement, 2026-08-22 (post-FR-115, fifteen increments later)

Same 11 repos, same SHAs, same 105 TUs, ONE tool run per TU -- FR-115's
premise held: every unported item now carries `root_blocker`, so the
second `--emit=coloring` pass and the hand repros the original sweep
needed are gone. The before column is the original results re-crunched
under identical rules. Artifacts: scratchpad `cxx-demand-2/`.

INDEPENDENT CORROBORATION (same day, second session, deliberately
different method: current upstream HEADs not the pinned SHAs, 110
qualifying TUs not 105, rankings from the JSON `root_blocker` alone):
silent items 0 at full corpus scale; the operator collapse reproduces
(183 all-kinds, 0 function-only on that unit set); the top actionable
cluster is the same {copy-move-constructor, rejected-type-cascade,
unreached-by-import}. Ranking-level agreement under both rev sets and
both unit sets -- the steering below does not hinge on either sweep's
particulars. (Artifacts: scratchpad `cpp-corpus/`.)

HEADLINES. Crates: 0 FULL / 95 PARTIAL / 10 NO_CRATE -> **0 / 103 / 2**
(all 8 fallen NO_CRATEs are spdlog = FR-113's effect; the remaining 2
are one environmental fuzz header and one FR-103-class extern global --
FR-103 makes real NO_CRATE zero). Ported: 22.8% -> 22.9% flat ONLY
because spdlog's 3,369-item denominator arrived at 13.9%;
**ex-spdlog 22.8% -> 26.3%, +3.4pp real**. Movers: spdlog 0 -> 13.9%,
tinyrenderer +8.7pp, docopt +6.2pp, tinyxml2 +4.5pp (FR-112),
raytracing +2.9pp with its overload collisions at ZERO (FR-114). Two
repos moved DOWN (jsoncpp -1.5pp, unordered_dense -3.3pp) -- plausibly
FR-118/FR-122 converting silently-wrong merges into loud rejections,
correctness-positive, not root-caused: flagged honestly.

THE NEW RANKING (deduped, n=9,598; silent mass 4,647 -> **ZERO**):
copy-move-constructor **2,389 = 24.9% #1** (was 4 items in the original
table!); rejected-type-cascade 1,434 (chain-length-1 residue -- the
rejected type's own root still uncomputed); unreached-by-import 1,234
(the honest c2 tag awaiting per-instantiation attribution -- together
~28% of demand still opaque, the FR-115 follow-on); **cxx-drop-global
936 #4, ON NO WAVE** (930/936 is pugixml's TEST-runner globals --
equal-weighted only 1.9%, size with that caveat); cxx-cascaded-method
843 (grew because spdlog became measurable); destructor 745 (jsoncpp
505 -- the residue no open item owns); cxx-operator-overload **183**
(was 587+1,459: of the 1,459 old operator locations only THREE are
actually gone -- the mass re-rooted to copy-move behind them, confirming
FR-112's cascade-artifact correction); template family ~249, NO template
wave exists; virtual-method **0** (W2.19a/b); cxx-drop-base **0**
(W2.26); exceptions 40, still last.

VERIFICATIONS: the raytracing shared_ptr shadowing is FIXED (1 corpus
diagnostic -> 41 root items, 4 projects); the alphabetic-minimum
root-tag artifact is FIXED (base-class 236 -> 36 while destructor
106 -> 745).

WAVE-SIZING: W2.23 is the #1 front under every weighting -- promoted,
and in flight as this is written. W2.25's true residue is 183
(~47 FREE operators admissible via FR-114's machinery per FR-119's
note; 136 member out-of-line needing real semantics) -- demoted
accordingly. FR-121 confirmed exactly as filed (2x E0596 in 7 of 8
spdlog units, zero elsewhere) but unblocks only 2 units alone -- the
other 5 also carry FR-124.

DEFECT WATCH: 19 of 103 emitted crates are exit-0-but-unbuildable, in
four shapes: **FR-124** (NEW, 60 errors/8 crates), **FR-125** (NEW, 33
errors/9 crates), FR-121 (14, as filed), FR-106's class (2). No
verifier kills survived recovery, no importer crashes -- the FR-113/
FR-119 backstops held.

### Emitted-Rust quality, measured 2026-08-21 (the clippy metric)

First measurement of the C++ surface, and a regression check on C. The
frozen epoch-3 slices reproduce EXACTLY -- train 283 = 283 and held-out
98 = 98, with identical per-lint breakdowns -- so FR-101..FR-107 and
W2.15..W2.22 are clippy-neutral on emitted C.

C++: 52 crates (29 Cpp17Suite entries + 23 EndToEnd), 71 warnings, ZERO
rustc errors, and 14 of 29 suite crates completely clean. Normalized and
excluding the off-limits `needless_late_init`, C++ is 0.10 warnings per
crate against C's 0.15 -- **the new C++ lowerings are as clean as or
cleaner than the mature C path.** Inheritance emits idiomatic
`struct Derived { base: Base, y: i32 }` with no Deref hack; `impl Drop`
is clean and correctly separate from the inherent impl; and the
`std::cout` chains FUSE into `println!("i={} l={}", i, l)`, segmenting
only where sequencing demands it.

Two findings the metric itself cannot express. The `std::map`
ordered-key-snapshot loop is the least idiomatic code in the corpus --
fully-qualified UFCS everywhere and a hand-rolled counted loop -- and
default clippy has no lint for either, so **the metric has a blind spot
and must not be the sole quality oracle**. And the biggest idiomaticity
gap in emitted C++, FR-110, is invisible to it entirely.

One tooling follow-up, **DONE 2026-08-28: epoch 4 is frozen** over the union
corpus. THE NUMBERS IN THIS NOTE WERE STALE IN THREE PLACES and the measured
ones are: `test/EndToEnd` holds **186** `.c` (not 167) and **72** `.cpp`, of
which **68** are measurable (not 52). Part of that growth is the same day's
FR-140/141/142/143 test additions.
MEASURABILITY, defined here because an epoch whose metric is undefined for
some member is not frozen in any useful sense: a file is measurable iff
`--emit=crate` produces a crate AND `cargo clippy` yields a COMPLETE tally --
it may fail on a deny-by-default clippy lint (that failure IS the tally) but
must not fail on a rustc error, which truncates the count. It is a property of
the PINNING REV: a file that starts transpiling later does not join the epoch.
All 258 files were probed individually. **20 excluded**, every one for
criterion (a) -- no crate at all: 16 multi-TU/link tests fed as a single
standalone TU, 1 split-file lit test that is not a standalone TU, and 3
by-design rejections (`unsigned __int128`, a ptr-to-ptr escape, a volatile
type). ZERO hit criterion (b), so nothing was quietly dropped; the reasons are
recorded per file in `nix/harness/epoch-4.exclude.txt`.
EPOCH 4: file_count **238** (170 `.c` + 68 `.cpp`), corpus_hash
`sha256:28f71ffa...`, created_rev `556677d`, split seed=4 frac=0.25 ->
**178 train / 60 held-out**, partition verified (178+60 = 238, overlap 0, union
== the pinned set). Seed follows the ACTUAL convention in epochs 1/2/3, which
is `seed == epoch_id`, not a fixed 3.
CLIPPY AT EPOCH 4 (rev 556677d): train **166** warnings over 178 crates,
held-out **72** over 60, union 238; ZERO skipped on either slice, which
independently confirms the measurability filter did its job. Written to
`nix/clippy-eval/clippy-baseline-epoch4.json` as a FRESH document --
`clippy-baseline.json` is deliberately untouched, because overwriting it would
silently redefine the committed ratchet's population.
**These numbers are NOT comparable to epoch-3's** -- different population
(union vs `.c`-only, measurable-filtered vs not). No trajectory claim is made
in either direction, which is the whole reason the epoch mechanism exists.
Worth noting for whoever optimizes next: `borrowed_box` (19 occurrences) lands
ENTIRELY in held-out, so it is invisible to a train-only optimizer -- exactly
the blind spot the held-out split is for.


