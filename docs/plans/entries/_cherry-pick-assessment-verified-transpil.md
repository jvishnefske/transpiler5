### Cherry-pick assessment: `verified_transpilation_pipeline`

The archived prototype (`~/src/archive/verified_transpilation_pipeline` on
`rainier`; a ~21 kloc Rust crate: libclang parser, own C AST, petgraph
graphs, Z3-backed verification conditions) is a PARALLEL architecture to
this one, so nothing lifts as code. Three of its ideas are load-bearing
here and are cherry-picked as design:

- Its `ir/graph.rs` `ConstraintDependencyGraph` (interprocedural dependency
  edges with cycle detection and topological order, indices rather than
  `Rc<RefCell>`) is the shape FR-40's item graph takes.
- Its `ir/transform.rs` `TransformationSpace` — several candidate Rust
  types per C construct, each with a cost, "lower cost = more idiomatic",
  selection ranked by cost — is FR-43's representation dimension. Its Z3
  validity check is NOT adopted: this project's soundness argument is
  differential execution against a native build, and adding a solver
  dependency to decide what a recovering import can answer by attempting
  the import is a worse trade.
- Its `analysis/integrated.rs` "OnlyWhenRequired" loop (start conservative,
  attempt compilation, refine from the errors, repeat to convergence or an
  iteration cap) is exactly FR-43's search loop, and its iteration cap is
  why FR-43 is bounded rather than exhaustive.

Its `examples/parse_compilation_database.rs` is the one directly portable
piece and becomes FR-45.

