# Transformation Theory: Data Structures, Algorithms, and Theorems Behind the Pipeline

This survey names the theory the transpiler already embodies, states what each
result guarantees, points at where the pipeline uses it, and maps each theory
area onto the remaining c-testsuite failure clusters (the CTS-* checklist in
design.md). It informs the CTS-P/R/S design decisions; it does not make them.

Every section follows one template: *theory* (with citations), *guarantee*
(what property it establishes, in pipeline-composition terms), *where the
pipeline uses it* (file:line references into this repository), *which CTS-\*
items it applies to*, and a *fit verdict* — **adopt** (use as-is or already in
use), **adapt** (borrow the idea, not the machinery), or **reject-by-design**
(incompatible with the project's constraints). The constraints applied in every
verdict are the ones design.md commits to: output is 100% safe Rust (no
`unsafe`, ever), every pass is deterministic and bounded, unsupported
constructs are rejected with a source location (never miscompiled), and any
silent wrong-output regression is fatal in CI (the MISCOMPILE ratchet,
`test/CTestSuite/run_c_testsuite.py:12`).

Citation discipline: every named theorem, algorithm, and paper below was
verified against a fetched primary or authoritative source during the survey.
Where only a bibliographic record or a peer-reviewed secondary source could be
fetched, the References section says so explicitly. No citation is from memory.

The pipeline under discussion, end to end
(`tools/emitrust-cc/emitrust-cc.cpp:168`): clang AST → hybrid
cf/arith/memref+EmitRust CFG (importer, `lib/ImportC/ImportC.cpp`) → mem2reg →
canonicalize → lift-cf-to-scf → canonicalize → convert-to-emitrust
(`lib/Conversion/`) → Rust text (`lib/Target/Rust/TranslateToRust.cpp`). Each
numbered section below covers one stage of that chain; section 7 states how the
stages compose; section 8 is the complete CTS coverage map.

---

## 1. Control-flow structuring

**Theory.** The Böhm–Jacopini theorem [BJ66] establishes that any flowchart
computation can be expressed with sequencing, selection, and iteration alone —
but only *semantically*, by enlarging the state: their construction threads
auxiliary boolean values through the computation, and the paper left open
whether that auxiliary state is strictly necessary. Harel [Har80]
documents that the version most authors cite is actually a folk theorem (the
single-loop-plus-program-counter simulation, traceable to Kleene), so the
theorem must not be cited as a guarantee of structure or cost preservation.
The cost question was settled by three results. Peterson, Kasami, and Tokura
[PKT73] prove that a translation that adds no variables and no run-time
computation exists exactly for *well-formed* (reducible) control flow, that
multi-level exit is genuinely necessary (single-level exit is insufficient even
with node splitting), and that the remaining cases require node splitting
(code duplication). Ashcroft–Manna [AM71] and Kosaraju [Kos74] answered
Böhm–Jacopini's open question: auxiliary variables are unavoidable in general
if program length and execution order must be preserved, and multi-level
breaks of depth *n* form a strict expressiveness hierarchy (Kozen and Tseng
[KT08] re-prove the negative results propositionally). Reducibility itself is
characterized by Hecht–Ullman: a CFG is reducible iff the T1/T2
transformations collapse it to a single node, and the T1/T2 system is finite
Church–Rosser (order-independent) [HU72]; among the further equivalent
characterizations in their JACM follow-up [HU74], reducible means every loop
has a unique entry. Making an irreducible graph reducible by node splitting is
exponentially expensive in the worst case — Carter, Ferrante, and Thomborson
[CFT03] prove a 2^(n−1) lower bound — while their footnote records that the
bound does not apply to add-a-dispatch-variable restructuring, whose
polynomial size is the standard consequence. Dominator trees, the substrate of both SSA construction and
structuring, are computable in near-linear time [LT79] or by the simpler
iterative algorithm of Cooper, Harvey, and Kennedy [CHK01].

Practical structurers occupy the corners of this trade-off space: Emscripten's
Relooper [Zak11] is a heuristic recoverer with a label-dispatch fallback; the
LLVM WebAssembly backend splits the job into a bounded dispatch pre-pass that
makes loops single-entry (FixIrreducibleControlFlow, explicitly no code
duplication) followed by a duplication-free stackifier; Ramsey [Ram22]
reimplements the PKT translation as a single recursion over the dominator tree
that is *ideal* (no added code, no added computation) precisely on reducible
input.

MLIR's `transformCFGToSCF` — the engine behind the lift-cf-to-scf pass this
pipeline runs — implements the structuring of Bahmann et al. [BRJM15] as
documented in the upstream source (`mlir/lib/Transforms/Utils/CFGToSCF.cpp`):
it translates any single-entry single-exit region into do-while-style loop
regions plus conditional dispatch, and it explicitly handles irreducible
control flow by inserting an *edge multiplexer* — a new block that receives
all entries, takes an integer discriminator from each predecessor, and
switches on it. It adds dispatch values; it never duplicates user code. Where
entry blocks disagree on block arguments, predecessors that do not supply an
argument pass an undef placeholder (`ub.poison` under the default interface).
The documented guarantee: if a region contains only a single kind of
return-like operation, *all* ControlFlow ops are replaced; otherwise one
residual `cf.switch` remains (the pass is deliberately named *lift*, not
*convert*); infinite loops need an unreachable terminator, currently supported
only inside `func.func`.

**Guarantee.** Under two checkable preconditions — one return-like kind per
function, infinite loops only inside `func.func` — the pass's postcondition is
that zero cf-dialect ops remain and all control flow is a structured
scf.if/scf.while tree, with every introduced construct a fresh integer
discriminator plus dispatch. This is the theoretically optimal corner for this
project's constraints: dispatch variables are unavoidable in general
([BJ66][AM71][Kos74]), and dispatch escapes the exponential lower bound that
dooms duplication [CFT03]. The transformation is structural and deterministic
(SCC iteration plus dominance; no search, no backtracking), so it satisfies
the bounded-pass requirement by construction. This is the theoretical
explanation for the empirical Phase 4d result: full C goto — including goto
into loop bodies, which creates multi-entry (irreducible) loops — lowered
cleanly and passed differential tests, because multi-entry loops are exactly
what the edge multiplexer absorbs.

**Where the pipeline uses it.** The pass pipeline invokes lift-cf-to-scf
between the two canonicalizer runs (`tools/emitrust-cc/emitrust-cc.cpp:173`).
The importer deliberately emits block-based cf so this pass does the heavy
lifting (design.md:61). The undef-placeholder seam is already handled: the
`ub.poison` values the multiplexer can introduce are lowered to the zero/
default constant of their type (`lib/Conversion/UBToEmitRust/UBToEmitRust.cpp:9`,
with a PDLL twin at `lib/Conversion/UBToEmitRust/UBToEmitRust.pdll:51`), so
never-read placeholders become safe constants rather than anything resembling
an uninitialized read. The importer's reachability sweep
(`lib/ImportC/ImportC.cpp:3109`) is a standard worklist reachability sweep that erases
blocks unreachable from the entry — the same graph-traversal vocabulary,
applied to keep the CFG handed to mem2reg well-formed.

**CTS mapping.** CTS-S2 (switch bodies that are not plain compound statements;
case labels nested inside inner statements) is the main beneficiary, and the
finding is clarifying: Duff-adjacent switch shapes are *not* a structuring
problem. Once the frontend turns every case label into an ordinary block
target — wherever it is syntactically buried — the result is just another
possibly-irreducible CFG, which lift-cf-to-scf provably absorbs exactly as it
absorbed goto-into-loop. The entire CTS-S2 risk therefore lives in the
importer's AST-to-cf lowering: every case label must become a real branch
target, and any shape the AST walk cannot faithfully flatten must be rejected
there with its location. This matches the checklist's own plan of reusing the
goto labelBlocks machinery (design.md:957). PKT's theorems also say the
resulting Rust cannot in general be a nested match/loop without duplication or
a dispatch value — so dispatch-shaped output for these tests is theoretically
mandated, not a quality regression.

**Verdict.** **Adopt** (already adopted), with three guardrails derived from
the verified caveats: (1) keep functions normalized to a single return-like
kind before lifting, or hard-error with a location if a residual `cf.switch`
survives the pass rather than letting it reach the emitter; (2) keep a
regression test for the infinite-loop caveat (`while(1)` shapes ride the
`func.func` path); (3) keep the poison-to-default lowering under differential
test on goto-heavy inputs, since it is the one seam where "lowered cleanly"
and "trivially safe Rust" are distinct claims. Two adaptation opportunities
are worth recording: Rust's labeled break/continue is an arbitrary-depth
multi-level exit, i.e. Rust sits at the top of Kosaraju's hierarchy, so a
future Ramsey-style dominator-tree emitter could produce ideal (dispatch-free)
output on reducible regions; and Hecht–Ullman's T1/T2 gives a cheap
reducible/irreducible classifier useful for triaging CTS-S2 test shapes.
Node-splitting restructuring is **rejected by design** as a primary strategy
(exponential worst case violates the bounded-pass constraint).

---

## 2. SSA and dataflow

**Theory.** Cytron, Ferrante, Rosen, Wegman, and Zadeck [CFRWZ91] construct
SSA by placing phi-functions at iterated dominance frontiers; the construction
works on arbitrary CFGs (reducibility not required) and is minimal in the
number of placed phis. Braun et al. [BBH+13] give a simpler on-the-fly
construction and supply the standard vocabulary: *minimal* (no redundant phi)
and *pruned* (no dead phi), with minimality cheap exactly on reducible CFGs —
the same precondition under which section 1's structuring is ideal. The
termination and soundness of any dataflow analysis in the pipeline rests on
the classical stack: Kildall's lattice framework [Kil73], Kam and Ullman's
monotone generalization [KU77] (the maximal fixed point exists and is computed
by the iterative algorithm; for monotone-but-not-distributive frameworks MFP
soundly over-approximates the meet-over-paths solution, and no algorithm
computes MOP in general), the Knaster–Tarski fixpoint theorem [Tar55]
(existence), and the ascending-chain condition (finite-height lattices make
Kleene iteration terminate in at most height-times-program-points steps).
Abstract interpretation [CC77] is the umbrella: analyses connected to concrete
semantics by a Galois connection are sound *by construction*, and composing
sound over-approximations stays sound — imprecision accumulates toward "don't
know", never toward a wrong answer. Finally, Appel [App98] and Kelsey [Kel95]
establish that SSA *is* functional programming: phi-functions are the formal
parameters of block functions, and the dominator tree is their lexical scoping.

MLIR's mem2reg pass implements exactly this theory, and its contract was
verified against the upstream source: promotion is two-phase (a pure analysis
phase that validates feasibility, then transformation), merge points are
computed with iterated dominance frontiers (Cytron's construction), and — the
load-bearing sentence from the pass documentation — *"if any of this is not
possible, the IR will be left without mutation."* For the memref dialect
specifically, an alloca offers a promotable slot only if it has static shape
and exactly one element, and loads/stores are removable only when they match
the slot pointer and element type exactly, with an explicit escape guard.

**Guarantee.** Promotion is never wrong, only incomplete. Any local whose
address escapes, whose type is punned, or whose shape is not a single static
element survives promotion byte-identically, where the transpiler can reject
it with a location — mem2reg's "refuses when unsure" composes exactly with the
project's reject-don't-miscompile policy. For what *is* promoted, reaching
definitions over a finite definition set is a distributive bit-vector
framework, so MFP equals MOP [KU77]: the promotion is exact, not merely sound.
And the Appel/Kelsey correspondence explains why the composition
mem2reg → lift-cf-to-scf → Rust is a change of notation rather than a semantic
transformation: MLIR block arguments are phis are function parameters, so an
SSA-form CFG becomes a structured value-passing scf tree, whose values become
immutable Rust bindings and whose loop-carried arguments become value-passing
loop state — no mutable aliasing is ever needed for promoted locals. This is
the deep reason the emitted Rust is naturally safe.

**Where the pipeline uses it.** `tools/emitrust-cc/emitrust-cc.cpp:171` runs
upstream mem2reg first in the pinned pipeline. The importer's design leans on
the all-or-nothing contract: scalar locals are emitted as rank-0 memref cells
precisely so mem2reg promotes them like int locals, and address-taken scalars
are deliberately kept as `emitrust.variable` so they never enter promotion
(`lib/ImportC/ImportC.cpp:25`). Pointer cursors are stored in ordinary rank-0
`memref<i64>` cells for the same reason (`lib/ImportC/ImportC.cpp:29`) — the
pointer *model* stays out of mem2reg's way while the pointer *index* enjoys
full promotion.

**CTS mapping.** CTS-S1 (compound assignment with operand promotion): after
mem2reg, the load/widen/operate/narrow/store sequence collapses to a pure SSA
value chain, so correctness reduces entirely to the importer emitting the
right cast ops — mem2reg cannot introduce a width or ordering bug because it
only forwards stored values that dominate their loads. The residual risk is
exactly the zero-vs-sign-extension trap design.md:949 already flags, which is
a frontend cast-semantics question (design.md:423 documents the adopted `as`
semantics) covered by section 6's mutation-adequacy argument. CTS-S5
(variable-length arrays) is settled by the verified memref promotability
condition: a VLA is a non-static-shape alloca, which mem2reg refuses and the
fixed-size backing-array model cannot represent — confirming the checklist's
recommendation (design.md:965) to document VLAs as a permanent by-design
rejection. CTS-F2 (unreferenced declarations must not demand definitions) is
plain dependency-graph reachability — the same worklist-reachability
vocabulary as `finalizeFunction`'s sweep, applied at the declaration level; no
further theory is required.

**Verdict.** **Adopt** (already adopted, transitively through upstream
passes). Two documentation-level adoptions are recommended: state the
minimal/pruned taxonomy and the reducibility precondition in design.md
acceptance language, and add the explicit invariant that after mem2reg every
remaining local-variable memref is either matched by a supported pattern or
rejected with a location — converting mem2reg's *silent* incompleteness into
this project's *explicit* rejection contract. Widening/narrowing machinery
from abstract interpretation is **rejected by design**: all lattices in this
pipeline are finite-height, so termination never needs precision-losing
widening, and that absence is worth documenting as an invariant.

---

## 3. Term rewriting and pattern-based lowering

**Theory.** An abstract rewriting system has unique normal forms when it is
terminating and confluent (Church–Rosser [CR36]); Newman's lemma [New42]
reduces confluence to *local* confluence — but only for terminating systems,
so a greedy pattern set without a termination argument has no route to any
confluence guarantee at all. For term rewriting, local confluence is decidable
by critical-pair analysis, and Knuth–Bendix completion [KB70] attempts to
repair non-joinable critical pairs under a termination ordering; simplification
orderings such as the recursive path ordering [Der82] provide compositional
termination proofs from a single symbol precedence. The standard textbook
treatment is Baader and Nipkow [BN98]. Whitfield and Soffa [WS97] frame the
compiler-engineering face of non-confluence: transformations *enable* and
*disable* one another, so application order changes results — the
phase-ordering problem. The principled escape is equality saturation: Tate et
al. [TSTL09] replace destructive rewriting with non-destructive accumulation
of equalities in an e-graph followed by global cost-based extraction, and egg
[WNW+21] made it practical.

MLIR's machinery, verified against the official docs and source, occupies
specific points in this space. The *dialect conversion* framework is
legality-driven: a conversion target declares which ops are legal, the driver
searches (with rollback) for a pattern application sequence reaching legality,
and on failure it emits an error anchored at the offending op — the verified
diagnostic is *"failed to legalize operation …"* attached via the op's own
location. Folding is attempted during legalization, before patterns. The
*greedy pattern rewrite driver* iterates to a fixpoint under a configurable
iteration cap, integrates folding and simple DCE, and offers *no* confluence
guarantee; MLIR's canonicalization doc places the convergence obligation on
pattern authors ("unstable or cyclic rewrites are considered a bug") and
states the rule this project should treat as law: *"pass pipelines should not
rely on the canonicalizer pass for correctness."* PDLL patterns compile to the
PDL dialect and run as interpreted bytecode under these same drivers; their
value is that a declarative pattern is *data*, mechanically comparable in a
way arbitrary C++ matchAndRewrite bodies are not, though region-carrying
patterns are outside PDL's expressible fragment.

**Guarantee.** Dialect conversion's contract — drive the IR to the declared
legal target or fail with a located diagnostic — is the strongest composition
primitive MLIR offers, and it is natively this project's
located-rejection-over-miscompile requirement. The greedy driver guarantees
only bounded execution and best-effort simplification: it is Newman's lemma
with both hypotheses unproven. Confluence-plus-termination, where it can be
argued, upgrades a rewrite stage from a relation to a *function* — same input
IR, same output IR, regardless of pattern order — which is the formal content
of "deterministic bounded passes" at the rewrite level.

**Where the pipeline uses it.** The final lowering runs
`applyPartialConversion` against an explicit target
(`lib/Conversion/ConvertToEmitRust/ConvertToEmitRust.cpp:71`); the located
failed-to-legalize diagnostic is regression-pinned as the rejection mechanism
for unsupported shapes (`test/Conversion/ArithToEmitRust/unsigned-invalid.mlir:11`).
The fold-during-legalization fact is the doctrinal root of a trap this
project already hit: conversion-only pass tests were never fold-free, so test
expectations had to account for the driver folding trivial arithmetic before
patterns ran (`test/Conversion/ArithToEmitRust/arith-to-emitrust.mlir:23`
documents the idiom). That incident was, in critical-pair vocabulary, an
unjoined overlap between the fold set and the conversion set, resolved by
staging — pinning the order and treating each stage's output as a committed
normal form, which is the standard engineering response when confluence is not
proven. The enabling-dependency between passes that Whitfield–Soffa names is
recorded in this repo's working notes as "ops synthesized by lift-cf-to-scf
that conversions must lower" — a textbook enabling interaction, handled by the
pinned pass order in `runPipeline`. The PDLL twins
(`lib/Conversion/ArithToEmitRust/ArithToEmitRust.pdll:7`) cover the
PDLL-expressible inventory, with the region-carrying patterns staying C++-only
exactly as the expressiveness limit predicts
(`lib/Conversion/ArithToEmitRust/ArithToEmitRust.pdll:36`).

**CTS mapping.** No CTS item is sole-blocked on rewriting theory, but every
CTS implementation lands as new conversion patterns, so the discipline applies
across the board — most directly to the pattern-heavy items CTS-S4
(multi-dimensional arrays: recursive type/initializer lowering), CTS-S7
(void-cast discard shapes), and CTS-S6 (the generated exhaustive-match
helper). The verified machinery supplies four project rules: greedy
canonicalization is never load-bearing for legality (only the conversion
target is); every pattern set carries a stated termination measure (the
dialect-rank precedence — core dialects above EmitRust — is the RPO-style
ordering already implicit in the conversion target); every known pattern
overlap gets a FileCheck test pinning the intended winner (critical-pair
analysis reduced to regression discipline); and PDLL twins are executable
specifications for drift-testing, never co-registered production paths.

**Verdict.** **Adopt** the dialect-conversion contract (already adopted;
tighten the final boundary's illegal set to be exhaustive so nothing
non-Rust-expressible survives a *partial* conversion silently). **Adapt**
critical pairs and termination orderings as review discipline rather than
formal machinery. **Reject by design** equality saturation for the lowering
pipeline: global cost-based extraction has no per-op located-rejection story
and its resource profile fits an optimizer, not a legality-obligated lowering
— worth keeping on the radar only as a bounded post-lowering idiom selector.

---

## 4. Pointer, alias, region, and ownership analysis

**Theory.** Union-find with path compression runs in inverse-Ackermann
amortized time [Tar75]; Tarjan and van Leeuwen [TvL84] show one-pass variants
(path halving/splitting with union by rank) are equally optimal and simpler;
Fredman and Saks [FS89] supply the matching lower bound, closing the question
of faster equality solvers. Steensgaard [Ste96] builds points-to analysis *as*
unification: every location a pointer may reference is merged into one
equivalence node, the storage shape graph is linear in program size, and the
result is a sound over-approximation whose alias relation is transitive —
merging can only lose precision, never soundness. Andersen [And94] formulates
the subset-based alternative: assignment propagates points-to sets rather than
merging them, cubic in the worst case, with modern solvers collapsing
constraint-graph cycles *into a union-find* [FFSA98][HL07] to reach near-linear
practice at scale. Region-based memory management [TT94][TT97] is the
type-theoretic ancestor of the region model: all values live in regions,
regions live on a lexically scoped stack, and region polymorphism lets callees
work in their callers' regions.

What "compilable to safe Rust" formally requires is answered by the Rust
formalization line. RustBelt [JJKD18] gives a machine-checked semantic safety
proof for a realistic Rust model; Polonius reformulates borrow checking as
datalog over loan sets; Stacked Borrows [JDKD20] and Tree Borrows [VHDJ25]
define the aliasing discipline — and both frame it as binding *only* unsafe
code. Separation logic [Rey02][ORY01] supplies the vocabulary for what the
region model enforces: the separating conjunction asserts disjoint heap
footprints, and the frame rule licenses local reasoning about code that
touches only one conjunct.

Prior art on C→Rust translation brackets this project's position. The
lift-after-translate line (c2rust output made safer post hoc) plateaus on
aliasing: Emre et al. pioneered lifting raw pointers to references [ESD+21],
and their follow-up raises safely-convertible pointers from 12% to 21% while
identifying rustc's checker conservatism as the wall [EBP+23]. Crown
[ZDYW23] infers heap-pointer ownership at scale. Hong and Ryu [HR24] translate
out-parameters into return values with algebraic data types. Closest of all,
Fromherz and Protzenko's Scylla [FP24] independently converged on this
project's architecture: an eligible subset of C, pointer arithmetic realized
through safe slice operations, borrow inference, zero unsafe, and *rejection*
of ineligible shapes — validated by translating an 80,000-line verified
cryptographic library. DARPA's TRACTOR program [DARPA24] is the programmatic
context for the problem.

**Guarantee.** Three composable guarantees emerge. *Algorithmic:* both
union-find analyses are deterministic, terminating, and near-linear — bounded
analysis cost by construction, and provably not improvable, so all future
precision work is analysis design, not solver shopping. *Soundness polarity:*
equality-based merging over-approximates aliasing, so its failure mode is a
too-coarse region (causing rejection or an oversized backing array), never a
missed alias (which could miscompile) — exactly the polarity the fatal ratchet
requires. *Composition with Rust:* the transpiler needs no aliasing soundness
proof of its own for memory safety, because (i) it emits only safe Rust, so
rustc's borrow checker gatekeeps every output — an ownership violation is a
compile error, i.e. a transpiler-visible rejection, never a silent
unsoundness; (ii) RustBelt proves well-typed programs in the Rust model are
safe, with the unsafe-library side conditions discharged for the core APIs the
output uses; (iii) Stacked/Tree Borrows impose obligations only on unsafe
code, of which the output contains none. Three caveats belong in the record:
RustBelt models λRust, not the rustc implementation; the trust extends to the
specific std APIs used (slices, arrays, Option — the best-trodden ones); and
this argument covers memory safety only — functional correctness is exactly
the residue the differential ratchet covers (section 6). The index-cursor
design is what makes the composition work at all: a cursor is a plain integer,
`Copy` and `'static`, so stored cursors carry no borrows, and two cursors into
one region are two integers plus one slice borrow — shapes the borrow checker
always accepts. In separation-logic terms, the disjoint-region invariant the
analysis enforces is a global separating conjunction maintained structurally,
and the union-find is its decision procedure: merging is how the analysis
restores separation when it cannot prove it.

**Where the pipeline uses it.** The equality-based analysis is named in the
importer's module docs (`lib/ImportC/ImportC.cpp:29`) and implemented twice:
the intra-function `PointerRegionAnalysis` (`lib/ImportC/ImportC.cpp:313`,
class at `:327`) unions pointer locals with their base objects on assignment
and address-taking, with iterative path-compressed find (`:1444`) and union
(`:1461`); the interprocedural `planOwners` (`lib/ImportC/ImportC.cpp:2009`)
runs a second union-find over storage bases and data-pointer parameters to
decide which function owns each backing array — the comment at `:2011` even
states the fixpoint argument (union-find transitively closes as edges are
added, so one walk reaches the fixpoint). Region validation happens at the
declaration, and every undecomposable region records the *first construct*
that made it so (`:319`) — the located-rejection discipline, in the analysis
itself. The owner/borrower split is Tofte–Talpin's stack of regions at
function-frame granularity: the owner's frame is the letregion scope, callee
borrows are region-polymorphic access, and the emitted Rust lifetime parameter
is the region variable, inferred by rustc.

**CTS mapping** (the 30-test cluster; design.md:871).

- **CTS-P1** (string literals): a read-only region kind whose base is the
  literal — in Rust, a `'static` shared borrow, the one lifetime that never
  escapes anything. Steensgaard's own type language already separates location
  kinds, so a new region *kind* stays inside the unification framework.
  **Adopt.**
- **CTS-P2** (pointers beyond the parameter/local-cursor model): two-part
  answer. For out-parameter shapes, adopt Hong–Ryu's rewrite — translate the
  pointer away into a return value. For the rest, typed cursors are a
  principal-type inference problem over a cursor-kind term algebra, solved by
  unification on the existing union-find (section 5 states this precisely).
  **Adopt the rewrite; adapt typed regions.**
- **CTS-P3** (integer↔pointer round-trips): no sound region assignment exists
  for general integer traffic — modeling it would require the analysis to
  invent aliasing facts it cannot verify. **Reject by design**, with one
  carve-out: the null-constant idiom is representable as None (CTS-P8).
- **CTS-P4** (pointer-typed globals): a distinguished outermost region —
  Tofte–Talpin's stack discipline says a whole-program lifetime must be the
  outermost letregion. Because cursors are borrow-free integers, a *stored*
  global cursor carries no borrow, which dissolves most of the borrow-escape
  problem with the `thread_local!` Cell model; what must still be rejected,
  with a location, is storing a borrow of a *local* region into a global —
  the exact program rustc would refuse. **Adapt.**
- **CTS-P5** (pointer-to-pointer): a second-order cursor is an index stored in
  a region of indices — indices being plain values, reference-to-reference
  shapes never arise. The analysis side is the standard indirect-constraint
  rules of subset-based analysis. **Adapt.**
- **CTS-P6** (pointers into global aggregates): the outermost region plus
  typed sub-object offsets; subset-based edges keep global regions from
  contaminating every local region they flow into. **Adapt.**
- **CTS-P7** (one pointer over several objects): *the* textbook precision loss
  of equality-based analysis — unification transitively equates the objects
  into one region. The literature's remedy is layered, not wholesale: add
  intra-function inclusion constraints on SSA form (MLIR gives flow
  disambiguation of pointer SSA values for free; only address-taken,
  memory-resident pointers need flow-insensitive treatment), reusing the
  existing union-find as the cycle-collapse structure a modern Andersen solver
  needs anyway. The emitter then chooses a per-region cursor where the
  points-to set is a singleton at each use, or a closed enum-of-cursors
  otherwise — both preserve the disjoint-region invariant. Highest payoff of
  the cluster. **Adapt.**
- **CTS-P8** (NULL constants): Option-of-cursor — the sum-type answer to the
  null reference Hoare later called his billion-dollar mistake [Hoa09]. An
  option of a small integer
  index has a niche representation and no lifetime, so nullability composes
  with every other item without touching the borrow story; every latent C
  null-deref becomes a compiler-checked match or a located rejection.
  **Adopt.**

The same machinery covers the blocked library items: CTS-L3 (initializers for
pointer-typed objects) is downstream of P1/P4 by the checklist's own analysis
(design.md:998), and CTS-L1/L2 (`strcpy`, `%s` of a char-pointer parameter)
are safe wrappers over exactly the slice/cursor parameter classes FR-28
already defines — the composition-with-rustc argument above is what makes
"safe helper over a mutable byte slice, bounds known at compile time" sound.

**Verdict.** **Adopt** the existing equality-based baseline and the
composition framing (with the three caveats recorded); **adapt** by layering
subset-based precision intra-function (P7) and new region kinds (P1, P2, P4,
P5, P6) onto the existing union-find substrate rather than replacing it;
**reject by design** general integer↔pointer traffic (P3) and any move to
whole-hog flow-sensitive analysis — SSA sparsity gives the needed flow
precision within bounded, deterministic machinery. The Tarjan–van Leeuwen
result licenses one zero-risk simplification: the hand-rolled two-pass
compress-to-root loops can become one-pass path halving with identical merge
semantics.

---

## 5. Tree formalisms, attribute grammars, and type inference

**Theory.** Regular tree languages (finite tree automata) are closed under the
boolean operations with deterministic product constructions, have linear-time
membership and emptiness, and admit a Myhill–Nerode-style unique minimal
deterministic bottom-up automaton; deterministic *top-down* automata are
strictly weaker — they recognize exactly the path-closed languages, so any
property correlating sibling subtrees must be computed bottom-up [TATA]. Tree
*transducers* are not closed under composition in general — neither top-down
nor bottom-up, and alternating copying with nondeterminism yields a strict
hierarchy — but the deterministic fragments compose: deterministic bottom-up
transductions are closed under composition, and chains of deterministic
top-down transductions reduce to a single deterministic pass combined with a
linear relabeling (the general statement needs regular look-ahead)
[TATA][EV85]. Macro tree transducers [EV85] add context parameters to top-down
transducers, and attribute grammars translate into them — synthesized
attributes become states, inherited attributes become the parameters. Knuth's
attribute grammars [Knu68] define meaning over trees via synthesized
(upward) and inherited (downward) attributes, well-defined iff dependencies
are non-circular; the general circularity test is expensive (Knuth's own
corrected algorithm is worst-case exponential [Knu71][KnuGen]), but the
L-attributed subclass [Boc76] is evaluable by construction in a single
left-to-right pass. Unification [Rob65] gives the most-general-unifier
theorem — a unifiable constraint set has a canonical solution through which
every other factors — with failure confined to exactly two locatable events
(symbol clash, cycle) and near-linear algorithms [MM82][PW78]. Hindley–Milner
inference [Hin69][Mil78][DM82] builds on it: constraint generation plus
unification yields a *principal* type, of which every valid typing is an
instance. Bidirectional type checking [PT00][DK21] is the discipline that
keeps context propagation local: a checking mode pushes expected shapes down,
a synthesis mode pushes computed types up, and errors localize at the node
where the modes disagree.

**Guarantee.** Three literatures converge on one contract for the emitter:
an L-attributed attribute grammar, a total deterministic macro tree
transducer, and a bidirectional checker are the same shape — inherited
context down, synthesized results up, no same-pass dependence on a right
sibling's synthesized results, one bounded traversal, failures reported at the
node. The transducer composition theorems make the *stage structure* of the
pipeline load-bearing: general tree transductions do not compose within a
class, so a staged translation is not mere engineering convenience, while the
deterministic stages this pipeline uses fuse safely if compile time ever
demands it. Domain recognizability makes every stage boundary a linear-time,
decidable, located checkpoint. For inference, the mgu theorem is an
order-independence guarantee: however a deterministic traversal orders the
use sites, a unifiable constraint system yields the same principal answer.

**Where the pipeline uses it.** The Rust emitter is the pipeline's one
parameter-carrying (MTT-class) stage: it threads an indented output stream and
a per-function value-name map as its only state
(`lib/Target/Rust/TranslateToRust.cpp:12`, `:54`), pushes indentation and
binding context down (`:73`), and synthesizes rendered text up — L-attributed
in fact if not yet in name. The dialect verifiers and FileCheck suites are the
membership checks of section 5's first paragraph. And the region analysis's
union-find *is* the core of near-linear unification — the same data structure,
already in the tree (`lib/ImportC/ImportC.cpp:1444`).

**CTS mapping.** CTS-P2 (typed cursors) is the headline application: cursor
kinds form a small first-order term algebra; each pointer use site contributes
one located equation (subscript use forces an array-index kind, member access
forces a struct kind, binding to a literal forces the read-only string kind,
cursor arithmetic equates kinds); solving is unification on the existing
union-find with a clash check at merge time; and the answer is a *principal
cursor kind* — the most general classification consistent with all uses —
with a clash rejected at the two conflicting use sites. This is Hindley–Milner
minus everything that makes it subtle (no let-generalization; every pointer is
a monomorphic unknown), so the occur-check is vacuous and the whole pass stays
near-linear. CTS-R1 (bare anonymous structs) is canonical naming of
structurally equal tree types — the shape-keyed synthesis the checklist
proposes (design.md:915) is a Myhill–Nerode-style canonical form, and reusing
the existing shape-dedup machinery is the right instinct. CTS-R4 and CTS-R5
(block-scope tag shadowing; C's separate tag/ordinary namespaces) are
attribute-grammar problems: a symbol table is an inherited environment
attribute, C's scoping rules require the record key to include scope depth
and namespace kind, and the emitter's single symbol table is the point where
the two C namespaces must be re-separated by deterministic mangling — no new
theory, just the environment-attribute discipline applied consistently.
CTS-R2 (anonymous member injection) is the same environment question one
level down (fields joining the parent's namespace). CTS-R6 (empty structs)
and CTS-S7 (void casts) are the unit type: a zero-field struct and an
evaluate-and-discard expression both map to Rust's canonical unit shapes.
CTS-S6 (integer-to-enum) is sum-type totality: safe Rust admits no unchecked
construction of a fielded enum from a discriminant, so the choices are the
checklist's generated exhaustive-match helper (making partiality explicit and
checked) or rejection. CTS-S4 (multi-dimensional arrays) is recursive
structural typing — nested array types, nested initializer attributes, and
row-major index synthesis are one structural recursion in the type mapper,
the initializer converter, and the subscript lowerer. CTS-E1 (the
thread-local closure binder shadowing a C global named the same) is variable
capture — the classic hygiene failure: the emitter's generated binder at
`lib/Target/Rust/TranslateToRust.cpp:219` and `:1142` lives in the same
namespace as user identifiers, and the fix is the standard one, a reserved
namespace for generated names. CTS-S3 (block-scope prototypes) is
environment hoisting: a declaration whose semantic scope is wider than its
syntactic position, handled by hoisting into the module-level environment.
CTS-F1 (variadic calls beyond the printf intrinsics) has *no known
safe-Rust-compatible general technique*: C-style varargs have no safe Rust
counterpart, so the options are arity-specialized monomorphization per call
site (a whole-program specialization, sound but of limited generality) or
continued rejection — a genuine design decision, exactly as the checklist
says (design.md:982).

**Verdict.** **Adopt** the L-attributed/bidirectional discipline for the
emitter (it formalizes the existing design at zero cost and pinpoints where a
needed-reference/got-value mismatch is *reported* rather than silently
patched) and the unification-based principal-kind engine for CTS-P2 (the
substrate already exists). **Adapt** tree-automata vocabulary for stage
boundary checks. **Reject by design** general circularity-tested attribute
grammars (exponential check) and any nondeterministic transducer machinery
(undecidable equivalence, no composition closure).

---

## 6. Correctness composition and translation validation

**Theory.** CompCert [Ler09a][Ler09b] is the template for whole-pipeline
correctness from per-pass arguments: semantic preservation is stated as a
forward simulation for safe source programs, per-pass simulations compose
transitively (vertical composition), and forward simulation plus target
determinism yields the refinement direction that matters. Two of its
definitional choices are load-bearing here. First, a verified compiler
*"either reports an error or produces code that satisfies the desired
semantic preservation property"* — rejection is always correct; completeness
is a quality-of-implementation metric tracked separately. Second, the
obligation is restricted to *safe* sources: on programs whose executions go
wrong (C undefined behavior), the compiler owes nothing, so the target may
lawfully be more defined than the source. Translation validation [PSS98]
replaces "verify the compiler" with "verify each run": Necula's validator for
GCC [Nec00] checks each pass's before/after IR symbolically and must err on
the side of caution. The limits of testing are classical: Dijkstra's dictum
that testing shows the presence of bugs, never their absence [Dij70][NATO69];
McKeeman named differential testing and its oracle caveat — two systems may
differ and both be right where the standard leaves behavior open [McK98];
Csmith demonstrated that every production C compiler silently miscompiles
valid inputs and that fixed test suites are inadequate long-run quality
control [YCER11]; EMI generates equivalence-modulo-inputs variants to the
same end [LAS14]. The coupling effect of DeMillo, Lipton, and Sayward [DLS78]
is the theory of the adversarial-vector mitigation: test data that kills all
simple mutants tends to kill complex ones, and data that fails to distinguish
a simple mutant pair is inadequate by definition. Wang et al. [WCC+12]
document what optimizing compilers actually do with undefined behavior —
deleting null checks, reordering division before its guard — and that the
result is not even stable across compilers or flags.

**Guarantee — what the ledger is and is not.** The differential ledger
(`test/CTestSuite/run_c_testsuite.py`) is *empirical translation validation*:
per-program like Pnueli's proposal, per-input like testing — it validates
each translation run by concrete output comparison rather than symbolic
proof, sitting strictly between plain regression testing and full translation
validation. The fatal-MISCOMPILE rule (`run_c_testsuite.py:12`, enforcement
at `:305`) is Leroy's Definition-8 contract made empirical: silent wrong
output is the one inexcusable state, and the script's classification even
follows Necula's fail-closed rule — a missing binary, a timeout, or an output
mismatch all classify as MISCOMPILE (`:224`, `:234`, `:242`), never as pass.
The ratchet manifest (`test/CTestSuite/expected-pass.txt`) is the monotone
completeness frontier: no canonical academic citation exists for the ratchet
pattern (it is engineering folklore, and this survey declines to invent one),
but its two halves map cleanly onto Leroy's split — soundness absolute,
completeness a tracked frontier. Dijkstra bounds what green CI means: zero
*observed* miscompiles on those programs with those vectors, never absence of
bugs. The zero-vs-sign-extension trap is the coupling-effect story in the
original vocabulary: correct zero-extension and buggy sign-extension differ
by one simple mutation, early vectors contained no value with the top bit
set, so the mutant stayed live for exactly DeMillo's reason (1) —
insufficient sensitivity; the adopted mitigation (adversarial value shapes,
design.md:255) is mutant-killing test data, and Csmith's own Figure 1 — a
signed/unsigned char comparison miscompiled by a shipping (Ubuntu-patched) GCC — shows this
mutant class is mainstream, not exotic. A cheap systematization follows from
the theory: enumerate the abstraction pairs each conversion could confuse
(zero/sign extension, wrapping/saturating, arithmetic/logical shift,
signed/unsigned compare, truncation boundaries) and require at least one
killing vector per pair, which the FR-25 audit already approximates.

One refinement of the methodology falls out of the UB analysis and is worth
recording. On sources with C-undefined executions, emitting a Rust panic is a
*refinement choice licensed by the same clause that licenses CompCert*: the
obligation covers safe sources only, and a clean located panic is the most
diagnosable admissible behavior — morally identical to CompCert turning UB
into defined going-wrong states. The consequence for the ledger: on a
UB-exercising test, native cc's output is one arbitrary resolution of the UB
and a transpiled panic is another, so a divergence there is *not* evidence of
a miscompile, and the MISCOMPILE verdict is fully sound exactly on the
UB-free fragment of the corpus. Today's corpus is curated and this is
theoretical; as the corpus grows (Csmith says it must), the ledger should
learn to classify native-vs-panic divergence on UB-exercising tests as
refinement rather than regression — for example by UBSan-instrumenting the
native runs.

**Where the pipeline uses it.** The reject-with-location policy is enforced
end to end: importer rejections carry clang source locations, conversion
rejections carry op locations (section 3), and the ledger records that all 70
remaining failures are located build-time rejections with zero miscompiles
(design.md:839). The per-pass composition license — each stage's obligations
stated over defined intermediate semantics — is exactly what sections 1–3
supply for the pinned pipeline in `runPipeline`
(`tools/emitrust-cc/emitrust-cc.cpp:168`): mem2reg's all-or-nothing contract,
lift-cf-to-scf's postcondition under its two preconditions, and dialect
conversion's legal-or-located-failure contract are the empirical stand-ins
for CompCert's per-pass simulation obligations.

**CTS mapping.** This section is methodological rather than item-specific,
but it binds two items directly: CTS-S1's checklist entry already demands
adversarial negative/width-extreme differential tests (design.md:949) — that
requirement *is* the mutation-adequacy criterion, now with its citation — and
every CTS item's acceptance criterion ("ledger ratchets with zero new
miscompiles", design.md:845) inherits the epistemics stated above.

**Verdict.** **Adopt** the framing wholesale: it is the theory of what the
project already does, and naming it (empirical translation validation;
refinement, not equivalence; mutation-adequate vectors) sharpens design.md's
claims at zero implementation cost. **Adapt** two upgrades when warranted:
UB-aware verdict classification as the corpus grows beyond curated tests, and
Csmith/EMI-style generative corpus growth once the fixed suite saturates —
the literature is unambiguous that it will. Full symbolic translation
validation per run is **rejected by design** for now (cost and machinery),
with per-pass differential checking inside the MLIR pipeline as the natural
intermediate step if a pass-attribution need ever arises.

---

## 7. How the stages compose

The pipeline is a chain of five contracts, each verified in the sections
above, each stated as a postcondition the next stage consumes as its
precondition:

1. **Importer → mem2reg.** The importer emits scalar locals and pointer
   cursors as single-element static-shape memref cells — exactly the
   promotable-slot condition mem2reg's memref interface demands — and keeps
   everything it cannot vouch for (address-taken scalars, aggregates) out of
   promotion's reach as opaque EmitRust ops (`lib/ImportC/ImportC.cpp:25`,
   design.md:61). Anything else it rejects with a clang location.
2. **mem2reg → lift-cf-to-scf.** Promotion is all-or-nothing per slot and
   never wrong, only incomplete; its output is minimal SSA over block
   arguments (Cytron), which is precisely the value-passing form the
   structurer consumes — block arguments are phis are function parameters
   (Appel/Kelsey), so structuring is a notation change, not a semantic one.
3. **lift-cf-to-scf → conversion.** Under its two preconditions the lifter
   guarantees zero residual cf ops, delivering a structured scf tree whose
   only novelties are integer dispatch values and never-read poison
   placeholders — both of which the conversion layer already lowers
   (dispatch as ordinary arithmetic; poison to zero/default constants at
   `lib/Conversion/UBToEmitRust/UBToEmitRust.cpp:9`).
4. **Conversion → emitter.** Dialect conversion drives the module to the
   declared EmitRust-legal target or fails with a located diagnostic; the
   emitter therefore only ever sees trees drawn from the dialect contract
   (design.md:69), a regular tree language whose membership its verifiers
   check in linear time.
5. **Emitter → rustc.** The emitter is the pipeline's single
   context-threading (L-attributed) stage, and its output discharges the last
   obligation externally: rustc's borrow checker re-proves memory safety of
   every emitted program, RustBelt proves that acceptance means safety, and
   the differential ledger empirically validates the one property the type
   argument cannot reach — functional equivalence with the source.

The composition claim, stated once: each stage either transforms within its
verified contract or fails with a location; therefore the only way wrong
output can ship is a *semantic* error inside a contract (a pattern that
lowers to the wrong arithmetic, a cast with the wrong extension), and that
residual class is exactly what the fatal-MISCOMPILE differential ratchet with
mutation-adequate vectors is aimed at. Safety by construction and typing;
semantics by empirical translation validation.

## 8. CTS coverage map

Every open checklist item in design.md, the section(s) that bear on it, and
the fit verdict of the applicable technique:

| Item | Section(s) | Technique / verdict |
|---|---|---|
| CTS-E1 | 5 | Hygiene: reserved namespace for generated binders — adopt (engineering fix; no new theory needed) |
| CTS-F1 | 5 | No known safe-Rust-compatible general technique for varargs; arity-specialized monomorphization or rejection — design decision, honestly open |
| CTS-F2 | 2 | Declaration-level reachability (worklist sweep) — adopt |
| CTS-S1 | 2, 6 | mem2reg-exactness + frontend cast discipline + mutation-adequate vectors — adopt |
| CTS-S2 | 1 | Frontend flattening of case labels to block targets; structurer provably absorbs the rest — adopt (risk lives in the importer) |
| CTS-S3 | 5 | Environment hoisting to module scope — adopt |
| CTS-S4 | 3, 5 | Recursive structural typing + conversion patterns — adopt |
| CTS-S5 | 2 | VLA violates the static-shape promotable-slot condition and bounded design — reject-by-design (permanent, documented) |
| CTS-S6 | 5 | Sum-type totality: generated exhaustive-match helper or rejection — adapt |
| CTS-S7 | 5 | Unit type; evaluate-and-discard — adopt |
| CTS-P1 | 4 | Read-only ('static) region kind — adopt |
| CTS-P2 | 4, 5 | Out-param-to-return rewrite + principal cursor kinds via unification — adopt rewrite, adapt typed regions |
| CTS-P3 | 4 | No sound region assignment — reject-by-design, except the NULL idiom (→ P8) |
| CTS-P4 | 4 | Outermost/global region; borrow-free stored cursors; reject local-region escape — adapt |
| CTS-P5 | 4 | Second-order cursor = index into a region of indices — adapt |
| CTS-P6 | 4 | Global region + typed sub-object offsets — adapt |
| CTS-P7 | 4 | Intra-function subset constraints on SSA over the existing union-find; enum-of-cursors emission — adapt (highest payoff) |
| CTS-P8 | 4 | Option-of-cursor (niche-friendly, lifetime-free) — adopt |
| CTS-R1 | 5 | Canonical naming of structurally equal tree types (shape-keyed) — adopt |
| CTS-R2 | 5 | Environment attribute discipline (fields joining parent namespace) — adapt or reject-by-design per checklist |
| CTS-R3 | 4, 5 | Sum types: data-carrying enum when accesses are type-consistent (the tagged-union translation of [HR24]'s companion line); byte-array storage for true punning — adapt |
| CTS-R4 | 5 | Scope-depth-keyed record identity — adopt |
| CTS-R5 | 5 | Namespace-kind-aware symbol environment + deterministic mangling — adopt |
| CTS-R6 | 5 | Unit-like struct — adopt |
| CTS-L1 | 4 | Safe helper over a mutable byte slice; bounds compile-time known — adopt |
| CTS-L2 | 4 | Extend %s shapes to the FR-28 slice parameter class — adopt |
| CTS-L3 | 4 | Downstream of P1/P4 — inherits their verdicts |

## References

Verification status is noted where the primary text could not be fetched;
"corroborated" means the claim was verified against a fetched peer-reviewed
secondary source or the implementation that cites it.

**Control-flow structuring**

- [BJ66] C. Böhm and G. Jacopini. Flow Diagrams, Turing Machines and
  Languages with Only Two Formation Rules. *Communications of the ACM*
  9(5):366–371, 1966. (Primary scan read.)
- [Har80] D. Harel. On Folk Theorems. *Communications of the ACM*
  23(7):379–389, 1980. (Bibliography verified; content via secondary.)
- [PKT73] W. W. Peterson, T. Kasami, N. Tokura. On the Capabilities of While,
  Repeat, and Exit Statements. *Communications of the ACM* 16(8):503–512,
  1973. (Primary scan read.)
- [AM71] E. Ashcroft and Z. Manna. The Translation of 'Go To' Programs to
  'While' Programs. *Proc. IFIP Congress 1971*, 250–255. (Result verified via
  [KT08]; primary unfetched.)
- [Kos74] S. R. Kosaraju. Analysis of Structured Programs. *Journal of
  Computer and System Sciences* 9(3):232–255, 1974. (Result verified via
  [KT08]; primary unfetched.)
- [KT08] D. Kozen and W.-L. D. Tseng. The Böhm–Jacopini Theorem Is False,
  Propositionally. *MPC 2008*, LNCS 5133. (Primary read.)
- [HU72] M. S. Hecht and J. D. Ullman. Flow Graph Reducibility. *Proc. 4th
  ACM STOC*, 238–250, 1972. (Primary scan read.)
- [HU74] M. S. Hecht and J. D. Ullman. Characterizations of Reducible Flow
  Graphs. *Journal of the ACM* 21(3):367–375, 1974. (Bibliographic record
  verified.)
- [CFT03] L. Carter, J. Ferrante, C. Thomborson. Folklore Confirmed:
  Reducible Flow Graphs are Exponentially Larger. *POPL 2003*, 106–114.
  (Author's copy read.)
- [LT79] T. Lengauer and R. E. Tarjan. A Fast Algorithm for Finding
  Dominators in a Flowgraph. *ACM TOPLAS* 1(1):121–141, 1979. (Primary scan
  read.)
- [CHK01] K. D. Cooper, T. J. Harvey, K. Kennedy. A Simple, Fast Dominance
  Algorithm. Rice University technical report, 2001. (Primary read.)
- [Zak11] A. Zakai. Emscripten: An LLVM-to-JavaScript Compiler. *OOPSLA 2011
  Companion*, 301–312. (Primary read.)
- [Ram22] N. Ramsey. Beyond Relooper: Recursive Translation of Unstructured
  Control Flow to Structured Control Flow (Functional Pearl). *Proc. ACM
  Program. Lang. (ICFP)*, 2022. (Primary read.)
- [BRJM15] H. Bahmann, N. Reissmann, M. Jahre, J. C. Meyer. Perfect
  Reconstructability of Control Flow from Demand Dependence Graphs. *ACM
  TACO* 11(4), article 66, 2015. (Bibliography verified; algorithmic content
  corroborated via the MLIR implementation, CFGToSCF.cpp, which cites it.)
- MLIR lift-cf-to-scf: upstream source `mlir/lib/Transforms/Utils/CFGToSCF.cpp`
  and the pass documentation at mlir.llvm.org/docs/Passes. (Primary read.)

**SSA and dataflow**

- [CFRWZ91] R. Cytron, J. Ferrante, B. K. Rosen, M. N. Wegman, F. K. Zadeck.
  Efficiently Computing Static Single Assignment Form and the Control
  Dependence Graph. *ACM TOPLAS* 13(4):451–490, 1991. (Primary read.)
- [BBH+13] M. Braun, S. Buchwald, S. Hack, R. Leißa, C. Mallon, A. Zwinkau.
  Simple and Efficient Construction of Static Single Assignment Form.
  *Compiler Construction (CC)*, LNCS 7791, 2013. (Primary read.)
- [Kil73] G. A. Kildall. A Unified Approach to Global Program Optimization.
  *POPL 1973*, 194–206. (Primary read.)
- [KU77] J. B. Kam and J. D. Ullman. Monotone Data Flow Analysis Frameworks.
  *Acta Informatica* 7(3):305–317, 1977. (Abstract and key results verified;
  companion [JACM 23(1):158–171, 1976] metadata only.)
- [Tar55] A. Tarski. A Lattice-Theoretical Fixpoint Theorem and Its
  Applications. *Pacific Journal of Mathematics* 5(2):285–309, 1955.
  (Primary read.)
- [CC77] P. Cousot and R. Cousot. Abstract Interpretation: A Unified Lattice
  Model for Static Analysis of Programs by Construction or Approximation of
  Fixpoints. *POPL 1977*, 238–252. (Primary read.)
- [App98] A. W. Appel. SSA is Functional Programming. *ACM SIGPLAN Notices*
  33(4):17–20, 1998. (Primary read.)
- [Kel95] R. A. Kelsey. A Correspondence between Continuation Passing Style
  and Static Single Assignment Form. *ACM SIGPLAN Notices* 30(3):13–22, 1995.
  (Metadata and abstract verified; full text paywalled.)
- MLIR mem2reg: upstream source `mlir/lib/Transforms/Mem2Reg.cpp`,
  `MemorySlotInterfaces.td`, `MemRefMemorySlot.cpp`, and the pass
  documentation. (Primary read.)

**Term rewriting**

- [CR36] A. Church and J. B. Rosser. Some Properties of Conversion.
  *Transactions of the AMS* 39(3):472–482, 1936. (Bibliographic record
  verified.)
- [New42] M. H. A. Newman. On Theories with a Combinatorial Definition of
  "Equivalence". *Annals of Mathematics* 43(2):223–243, 1942. (Bibliographic
  record verified.)
- [KB70] D. E. Knuth and P. B. Bendix. Simple Word Problems in Universal
  Algebras. In *Computational Problems in Abstract Algebra*, Pergamon,
  263–297, 1970. (Bibliographic record verified.)
- [Der82] N. Dershowitz. Orderings for Term-Rewriting Systems. *Theoretical
  Computer Science* 17(3):279–301, 1982. (Bibliographic record verified.)
- [BN98] F. Baader and T. Nipkow. *Term Rewriting and All That*. Cambridge
  University Press, 1998. (Publisher record verified.)
- [WS97] D. L. Whitfield and M. L. Soffa. An Approach for Exploring Code
  Improving Transformations. *ACM TOPLAS* 19(6):1053–1084, 1997.
  (Bibliographic record verified.)
- [TSTL09] R. Tate, M. Stepp, Z. Tatlock, S. Lerner. Equality Saturation: a
  New Approach to Optimization. *POPL 2009*; extended version *LMCS* 7(1),
  2011, arXiv:1012.1802. (Extended version read.)
- [WNW+21] M. Willsey, C. Nandi, Y. R. Wang, O. Flatt, Z. Tatlock,
  P. Panchekha. egg: Fast and Extensible Equality Saturation. *POPL 2021*,
  arXiv:2004.03082. (Primary read.)
- MLIR dialect conversion, greedy rewrite driver, canonicalization, PDLL:
  official documentation at mlir.llvm.org and upstream source
  `DialectConversion.cpp`, `GreedyPatternRewriteDriver.h`. (Primary read;
  the located failed-to-legalize diagnostics verified in source.)

**Pointers, regions, ownership**

- [Tar75] R. E. Tarjan. Efficiency of a Good But Not Linear Set Union
  Algorithm. *Journal of the ACM* 22(2):215–225, 1975. (Bibliography
  verified; bound corroborated via [TvL84].)
- [TvL84] R. E. Tarjan and J. van Leeuwen. Worst-Case Analysis of Set Union
  Algorithms. *Journal of the ACM* 31(2):245–281, 1984. (Primary read.)
- [FS89] M. L. Fredman and M. E. Saks. The Cell Probe Complexity of Dynamic
  Data Structures. *STOC 1989*. (Bibliography verified; the union-find lower
  bound attribution corroborated via secondary literature.)
- [Ste96] B. Steensgaard. Points-to Analysis in Almost Linear Time. *POPL
  1996*, 32–41. (Primary read.)
- [And94] L. O. Andersen. *Program Analysis and Specialization for the C
  Programming Language*. PhD thesis, DIKU, University of Copenhagen, 1994.
  (Title page read; technical characterization verified via [HL07].)
- [FFSA98] M. Fähndrich, J. S. Foster, Z. Su, A. Aiken. Partial Online Cycle
  Elimination in Inclusion Constraint Graphs. *PLDI 1998*. (Bibliographic
  record verified.)
- [HL07] B. Hardekopf and C. Lin. The Ant and the Grasshopper: Fast and
  Accurate Pointer Analysis for Millions of Lines of Code. *PLDI 2007*.
  (Primary read.)
- [TT94] M. Tofte and J.-P. Talpin. Implementation of the Typed Call-by-Value
  λ-calculus using a Stack of Regions. *POPL 1994*. (Abstract verified.)
- [TT97] M. Tofte and J.-P. Talpin. Region-Based Memory Management.
  *Information and Computation* 132(2):109–176, 1997. (Bibliographic record
  verified.)
- [JJKD18] R. Jung, J.-H. Jourdan, R. Krebbers, D. Dreyer. RustBelt: Securing
  the Foundations of the Rust Programming Language. *Proc. ACM Program.
  Lang.* 2(POPL), article 66, 2018. (Primary read.)
- [JDKD20] R. Jung, H.-H. Dang, J. Kang, D. Dreyer. Stacked Borrows: An
  Aliasing Model for Rust. *Proc. ACM Program. Lang.* 4(POPL), article 41,
  2020. (Primary read.)
- [VHDJ25] N. Villani, J. Hostert, D. Dreyer, R. Jung. Tree Borrows. *Proc.
  ACM Program. Lang.* 9(PLDI), article 188, 2025. (Primary read.)
- Polonius: the Polonius book (rust-lang.github.io/polonius) and N. Matsakis,
  "An alias-based formulation of the borrow checker", 2018. (Both read;
  experimental status noted — not a peer-reviewed source.)
- [Rey02] J. C. Reynolds. Separation Logic: A Logic for Shared Mutable Data
  Structures. *LICS 2002*. (Primary read.)
- [ORY01] P. W. O'Hearn, J. C. Reynolds, H. Yang. Local Reasoning about
  Programs that Alter Data Structures. *CSL 2001*. (Bibliographic record
  verified.)
- [ESD+21] M. Emre, R. Schroeder, K. Dewey, B. Hardekopf. Translating C to
  Safer Rust. *Proc. ACM Program. Lang.* 5(OOPSLA), article 121, 2021.
  (Primary read.)
- [EBP+23] M. Emre, P. Boyland, A. Parekh, R. Schroeder, K. Dewey,
  B. Hardekopf. Aliasing Limits on Translating C to Safe Rust. *Proc. ACM
  Program. Lang.* (OOPSLA1), 2023. (Bibliographic record and abstract
  verified.)
- [ZDYW23] H. Zhang, C. David, Y. Yu, M. Wang. Ownership Guided C to Rust
  Translation. *CAV 2023*, LNCS. (arXiv record read; venue corroborated.)
- [FP24] A. Fromherz and J. Protzenko. Compiling C to Safe Rust, Formalized
  (arXiv:2412.15042, 2024); current version: Scylla: Translating an
  Applicative Subset of C to Safe Rust, *Proc. ACM Program. Lang.* (OOPSLA),
  2026. (arXiv records read.)
- [HR24] J. Hong and S. Ryu. Don't Write, but Return: Replacing Output
  Parameters with Algebraic Data Types in C-to-Rust Translation. *PLDI 2024*.
  (Artifact record verified; companion union and lock papers corroborated
  only.)
- [DARPA24] DARPA. TRACTOR: Translating All C to Rust. Program announcement,
  darpa.mil, 2024. (Primary read.)
- [Hoa09] T. Hoare. Null References: The Billion Dollar Mistake. QCon talk,
  2009, recorded by InfoQ. (Talk page read.)

**Tree formalisms and typing**

- [TATA] H. Comon, M. Dauchet, R. Gilleron, F. Jacquemard, D. Lugiez,
  C. Löding, S. Tison, M. Tommasi. *Tree Automata Techniques and
  Applications*. Release of November 18, 2008. (Primary read; chapters 1
  and 6.)
- [EV85] J. Engelfriet and H. Vogler. Macro Tree Transducers. *Journal of
  Computer and System Sciences* 31(1):71–146, 1985. (Primary read. The
  attribute-grammar-to-MTT translation cites B. Courcelle and
  P. Franchi-Zannettacci, *TCS* 17(2):163–191 and 17(3):235–257, 1982; only the AG→MTT inclusion is
  claimed here, not an exact subclass equivalence.)
- [Knu68] D. E. Knuth. Semantics of Context-Free Languages. *Mathematical
  Systems Theory* 2:127–145, 1968. [Knu71] Correction, *Mathematical Systems
  Theory* 5:95–96, 1971. (Citations verified; content verified via [KnuGen].)
- [KnuGen] D. E. Knuth. The Genesis of Attribute Grammars. 1990. (Primary
  read; source for the circularity-test error and its exponential-worst-case
  correction.)
- [Boc76] G. V. Bochmann. Semantic Evaluation from Left to Right. *CACM*
  19(2):55–62, 1976. (Bibliographic record and abstract verified.)
- [Rob65] J. A. Robinson. A Machine-Oriented Logic Based on the Resolution
  Principle. *Journal of the ACM* 12(1):23–41, 1965. (Primary read.)
- [MM82] A. Martelli and U. Montanari. An Efficient Unification Algorithm.
  *ACM TOPLAS* 4(2):258–282, 1982. (Primary read.)
- [PW78] M. S. Paterson and M. N. Wegman. Linear Unification. *Journal of
  Computer and System Sciences* 16(2):158–167, 1978. (Bibliographic record
  verified; linearity claim corroborated via [MM82].)
- [Hin69] R. Hindley. The Principal Type-Scheme of an Object in Combinatory
  Logic. *Transactions of the AMS* 146:29–60, 1969. (Citation verified via
  [Mil78]'s bibliography; primary paywalled.)
- [Mil78] R. Milner. A Theory of Type Polymorphism in Programming. *Journal
  of Computer and System Sciences* 17:348–375, 1978. (Primary read.)
- [DM82] L. Damas and R. Milner. Principal Type-Schemes for Functional
  Programs. *POPL 1982*, 207–212. (Primary read.)
- [PT00] B. C. Pierce and D. N. Turner. Local Type Inference. *ACM TOPLAS*
  22(1):1–44, 2000. (Primary read.)
- [DK21] J. Dunfield and N. Krishnaswami. Bidirectional Typing. *ACM
  Computing Surveys* 54(5), 2021, arXiv:1908.05839. (arXiv record read.)

**Correctness and validation**

- [Ler09a] X. Leroy. Formal Verification of a Realistic Compiler.
  *Communications of the ACM* 52(7), 2009. (Primary read.)
- [Ler09b] X. Leroy. A Formally Verified Compiler Back-end. *Journal of
  Automated Reasoning* 43(4), 2009. (Primary read.)
- [PSS98] A. Pnueli, M. Siegel, E. Singerman. Translation Validation. *TACAS
  1998*, LNCS 1384, 151–166. (Abstract verified via institutional
  repository.)
- [Nec00] G. C. Necula. Translation Validation for an Optimizing Compiler.
  *PLDI 2000*, 83–94. (Primary read.)
- [Dij70] E. W. Dijkstra. *Notes on Structured Programming* (EWD249).
  Technological University Eindhoven, T.H.-Report 70-WSK-03, 2nd ed., 1970.
  (Primary read.) [NATO69] J. N. Buxton and B. Randell (eds.). *Software
  Engineering Techniques*, NATO Science Committee, Rome 1969 conference
  report, 1970. (Primary read.)
- [McK98] W. M. McKeeman. Differential Testing for Software. *Digital
  Technical Journal* 10(1):100–107, 1998. (Primary read.)
- [YCER11] X. Yang, Y. Chen, E. Eide, J. Regehr. Finding and Understanding
  Bugs in C Compilers. *PLDI 2011*. (Primary read.)
- [LAS14] V. Le, M. Afshari, Z. Su. Compiler Validation via Equivalence
  Modulo Inputs. *PLDI 2014*. (Primary read.)
- [DLS78] R. A. DeMillo, R. J. Lipton, F. G. Sayward. Hints on Test Data
  Selection: Help for the Practicing Programmer. *IEEE Computer*
  11(4):34–41, 1978. (Primary scan read.)
- [WCC+12] X. Wang, H. Chen, A. Cheung, Z. Jia, N. Zeldovich, M. F. Kaashoek.
  Undefined Behavior: What Happened to My Code? *APSYS 2012*. (Primary
  read.)
- The CI ratchet pattern has no canonical academic citation; it is recorded
  here as engineering folklore rather than attributed.
