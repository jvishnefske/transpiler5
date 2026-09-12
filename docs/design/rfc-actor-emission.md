## RFC: Actor-model emission — deferred

Status: design RFC only. No ops, passes, analyses, or tests for actor
emission exist in the tree, and this cycle adds none. FR-30 shipped the
project's only "actor" — the owner struct, an *ownership boundary, not a
thread*. This RFC examines the threaded reading (actors as spawned
threads with mailboxes) and concludes it must stay deferred; the
go/no-go criterion at the end is the falsifiable condition for ever
reopening it. Throughout, claims about existing machinery cite the
actual code; everything labeled "sketch" is speculative and unbuilt.

### Input subset where actor decomposition is semantically justified

The candidate subset is: programs whose entry point performs two or more
independent top-level work items — call trees that share no mutable
state, meaning (1) the pointer-region classes reachable from each
candidate's call tree are pairwise disjoint, and (2) the candidates
share no mutable globals, whether reached through pointers or accessed
directly by name. The certifying analysis would be the Pass-A
interprocedural machinery: `planOwners` (lib/ImportC/ImportC.cpp)
already builds a program-wide (per-TU) union-find over storage bases and
data-pointer parameters on the shared `VarDeclUnionFind` scaffold,
adding one edge per data-pointer call argument via the
`forEachDataPointerCallArg` call-edge walk, and `planCellSlices` reuses
the same scaffold for global-array-backed parameter classes. Two
candidate actors would be independent iff their reachable region classes
are disjoint and their mutable-global footprints do not intersect.

Honest assessment: **this claim is NOT dischargeable by today's Pass-A
machinery.** Three gaps, verified against the code as of this writing.
First, there is no call graph: `forEachDataPointerCallArg` visits call
expressions only to add pointer-argument union edges; nothing computes
the transitive callee closure from a candidate root, which is the very
object whose footprint must be certified. Second, the union-find models
*pointer-carried* state only — a global scalar or array read and written
directly by name (the `global_load`/`global_store` path) never enters
the analysis at all; `globalPtrFacts` merges region views of
pointer-typed globals, not the by-name access footprint of ordinary
globals, so the "shares no mutable globals" half of the independence
condition has no computer today. Third, hosted-library effects are
outside the region model entirely: `printf`/`putchar` mutate the one
resource — stdout — that essentially every corpus program touches, and
no analysis attributes output effects to functions. What is genuinely
reusable is the scaffold: the per-body `PointerRegionAnalysis`, the
`VarDeclUnionFind` fixpoint, and the Pass-A "walk every definition,
merge program-wide facts" pattern would host a new effect-footprint
planner (per-function: mutable globals touched by name, region classes
touched, output effects; then a transitive closure over a real call
graph). That planner is new work, not an incremental tweak, and its
absence tightens the go/no-go criterion below.

### Dialect surface sketch (sketch only — no code this cycle)

The ops would follow the `emitrust.global_cells` model: region-based
statement ops with verifier-pinned structure, so no handle or borrow
ever escapes into general SSA circulation.

`actor.spawn` — sketch. A statement op carrying one FlatSymbolRefAttr
naming the actor's state definition (an FR-30 owner struct or a
dedicated actor `struct_def`), no operands and no results, with a single
sized region whose entry block takes exactly one argument: an opaque
actor-reference value typed against the named definition. Like
`global_cells`, the actor lives exactly for the region's extent — the
region's end is the join point where the mailbox sender drops and the
actor thread is joined. Verifier obligations: the symbol resolves to an
actor definition (SymbolUserOpInterface), the region is single-block
with the one correctly typed entry argument, the terminator is
`emitrust.yield`, and the reference argument is used only as the target
operand of `actor.send`/`actor.call` ops nested inside the region.

`actor.send` — sketch. A statement op taking the actor reference, a
message-tag attribute naming a variant of the actor's declared message
enum, and one value operand per payload field. No results. Verifier
obligations: the op is nested inside the `actor.spawn` region that
introduced the reference; the tag names a declared variant; each payload
operand's type equals the variant's declared field type and lies in the
sendable value set (see soundness below) — never an lvalue, reference,
or cell-slice type.

`actor.call` — sketch. Operands exactly as `actor.send`, plus one
result whose type equals the tagged variant's declared reply type.
Verifier obligations: everything `actor.send` requires, plus the reply
type match, plus the structural nesting guarantee that the call cannot
outlive the spawn region (so the blocking receive always has a live
counterparty and the reply channel cannot dangle).

### Lowering: std::sync::mpsc loops only

The lowering posture is std-only — `std::thread` plus
`std::sync::mpsc` — consistent with the zero-dependency emitted crates
(no tokio, no actix; rejection recorded below). Sketch of the emitted
shape, per actor: one message enum with one variant per handled message
(fields are the payload values; call-style variants carry an extra
`mpsc::Sender` for the reply); one `mpsc::channel` pair created at the
spawn point; one `std::thread::spawn` whose move closure takes ownership
of the actor's state struct and the receiver and runs the mailbox loop —
a `for`-over-receiver whose body is a single `match` on the message
enum, each arm invoking the corresponding `&mut self` method on the
owned state, call-style arms sending the method result back on the
carried reply sender. The loop terminates when the last sender drops at
the end of the spawn region, after which the spawn's join completes.
`actor.call` lowers to a send of the variant carrying a fresh reply
channel's sender, immediately followed by a blocking receive on the
reply receiver; a disconnected reply channel is a deterministic panic,
consistent with the project's policy of refining UB and impossible
states into deterministic panics rather than unsafe.

### Soundness obligations mapped to existing machinery

No shared mutable state across actors: exactly the region-disjointness
plus global-footprint condition of the subset section — dischargeable
only after the Pass-A effect-footprint extension described there. One
project-specific landmine makes this obligation stricter than in an
ordinary Rust codebase: emitted globals are `thread_local!` + Cell, so a
spawned actor thread observes *fresh, reinitialized* globals, not the
main thread's — any actor whose closure touches any global is silently
wrong today, and the certification must therefore prove a zero-global
footprint (or the global model must be reworked), not merely a
non-conflicting one.

Message payloads are Copy/owned values: the existing value model already
supplies the sendable set — scalar ints/floats/bools, C-like enums, and
derive-Default Copy structs are plain values. Two existing value kinds
must be *excluded* despite being Copy: region cursors (a cursor is a
plain i64 only meaningful against its region's base — sending one across
an actor boundary would smuggle aliasing into the receiver) and function
pointers whose targets' footprints fall outside the receiving actor's
certified partition.

Deterministic output interleaving: not an obligation existing machinery
can discharge at all; it is the hard constraint of the next section, and
under it at most one actor per program may produce output, with all
cross-actor interaction synchronous.

### Non-goals and the hard constraint

The project's oracle is byte-identical stdout and exit status against
the natively compiled C program — enforced by every EndToEnd
differential test, by the c-testsuite ledger in CI, and by the three-way
fuzz contract (genprog's Python evaluator, clang, and emitrust-cc must
all agree byte-for-byte). That oracle encodes *sequential* semantics.
Any concurrency the emitter introduces must therefore be observationally
sequential, and that requirement hollows out the actor model:

- Synchronous request-response (`actor.call` = send + blocking receive)
  is the only shape that preserves sequential observation — and under
  it, the mailbox is pure ceremony. The run loop is a function call with
  extra steps: strictly more machinery, an extra thread, and identical
  observable behavior to the FR-30 method call that already exists.
  This RFC states that plainly rather than dressing it up.
- Mutual-recursion SCCs collapse into one actor. A synchronous call
  from actor A blocked on actor B that calls back into A deadlocks a
  single-mailbox actor, so every call-graph SCC (the Hanoi shape —
  CTS-P10's permuted mutual recursion — is the canonical corpus example)
  must be assigned to a single actor. Hanoi-shaped programs therefore
  decompose into exactly one actor: zero benefit.
- Genuinely concurrent actors (asynchronous sends, interleaved
  progress) produce nondeterministically interleaved output and would
  need an ordering-insensitive oracle — per-actor output streams, or
  sorted/multiset output comparison — which the project deliberately
  does not have and will not build for this: the byte-exact three-way
  contract is the project's core defense against miscompiles, and
  weakening it to enable a feature with no demonstrated demand inverts
  the project's priorities.

Recorded rejections. tokio/actix: the emitted crates are
zero-dependency by contract (plain cargo build, no network, no
third-party audit surface), an async runtime adds a large dependency
tree and scheduler-dependent execution order, and the RFC's std-only
posture already covers the only admissible shape. Deep-copy-across-
boundary: the cell-slice wave (CTS-P10) proved staged copies unsound for
interleaved global readers — 00181's Move mutates through its
parameters while PrintAll reads the SAME globals directly mid-call, so
only the shared-Cell lowering is coherent. An actor design that
deep-copies shared state into messages and writes it back on reply is
that same staged copy wearing a costume: any reader interleaved between
the copy and the write-back observes stale state. Sharing must be
compiled away by the independence certification, never papered over by
copying.

### Go/no-go criterion

Concrete and falsifiable, tightened by the honest finding above that
the independence certification is not dischargeable by today's Pass A.
Implement actor emission only when ALL of the following hold:

1. A Pass-A effect-footprint planner exists (call-graph transitive
   closure; per-function by-name mutable-global footprint; output-effect
   footprint; merged with the existing region classes) — this is a
   prerequisite build, not a given — AND, run over a target corpus of at
   least 10 real programs (c-testsuite members or user-supplied), it
   certifies in each program at least 2 independent top-level work items
   with pairwise-disjoint region classes, zero shared mutable globals,
   zero per-actor global footprint under the thread_local! model (or a
   reworked global model), and at most one output-producing actor.
2. Each certified program's output is provably order-independent or
   fully serialized through the single output-producing actor, so the
   byte-exact oracle still applies unchanged.
3. A differential oracle for any reordered output exists and is wired
   into the three-way fuzz contract without weakening the byte-exact
   comparison for the sequential corpus.

Until all three hold simultaneously, this RFC stays deferred, and the
FR-30 owner struct remains the shipped meaning of "actor". Criterion 3
is expected to remain unmet indefinitely by deliberate choice; the RFC
exists to record why, not to schedule the work.

