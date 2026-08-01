# Paper: compiling partial translations

LaTeX source for a paper on the non-obvious design decisions behind
EmitRust's incremental C/C++-to-Rust translation, with a measured evaluation.

## Layout

- `paper.tex` — the paper.
- `tables/edges.tex`, `tables/field.tex` — hand-written structural tables
  (the edge-kind taxonomy and the field-edge rule). These describe the
  implementation, not measurements.
- `tables/e1.tex`, `tables/e2.tex` — **generated** from measurement CSVs by
  `scripts/tables.py`. Do not edit; they are overwritten.
- `figures/*.pdf` — **generated** by `scripts/plots.py`. Do not edit.
- `Makefile` — regenerates everything and compiles.

## Rebuilding

    make DATA=/path/to/paper-data

`DATA` is the directory the experiment harnesses wrote their CSVs into. The
harnesses themselves live with the data, alongside a README documenting every
column and the timing methodology.

Every number and every figure in the paper is derived from those CSVs by the
two scripts. Nothing is transcribed by hand, so re-running against fresh data
updates the paper wholesale rather than leaving prose and tables to drift
apart. Prose that cites a specific figure (for example the false-negative
count) is the exception and must be re-read when the data changes; those
citations are confined to the results subsections.

## Experiments

| ID | Question |
|----|----------|
| E1 | Colouring precision against ground truth; is the false-negative rate zero? |
| E2 | Fraction of projects yielding a *compiling* artifact, all-or-nothing vs partial |
| E3 | Repair-search cost and benefit |
| E4 | Longitudinal translated-item fraction on a fixed benchmark, with an all-or-nothing control |
| E5 | Work-list compression from root-cause attribution |
| E6 | Pure-AST analysis cost relative to translation |

E4 holds the benchmark fixed and varies only the tool: the corpus is
extracted once from the newest revision measured and every historical build
runs against that copy. It also carries a control series (all-or-nothing mode
at every revision) so the effect of partial translation is isolated from
growth of the supported subset.
