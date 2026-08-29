# Auto-improvement loop — clippy as the fitness function

The emitter's **idiomatic-Rust debt** is measured by running default clippy
(`clippy::all`) over the crates it emits for the EndToEnd corpus. The ranked
tally is the work queue; `total_warnings` is a **ratchet** that must only fall.
Each iteration removes one systematic emitter pattern, so a single emitter
change retires hundreds of warnings at once.

## The invariant that makes this safe

Every candidate lint here is about *how* the emitted Rust is spelled, not what
it computes: `print!("x\n")` → `println!("x")`, `v = v + 1` → `v += 1`,
`(*p)[i]` → `p[i]`, `x == false` → `!x`. These are **stdout-preserving**, so the
**EndToEnd byte-diff oracle is unchanged** — it is the arbiter that a
"prettier" emission did not become a wrong one. Golden FileCheck tests that pin
the old spelling are updated in the same change; `cargo build` clean is never
sufficient evidence (a codegen change cannot be trusted on compile-clean alone).

## One iteration

1. **Measure.** `python3 nix/clippy-eval/clippy_eval.py` prints the ranked
   tally and the ratchet delta vs `clippy-baseline-epoch4.json` — the
   **epoch-pinned** baseline. The measured population is epoch-4's frozen file
   list, not whatever `.c` happens to be in `test/EndToEnd`, so the delta is a
   paired comparison attributable to the emitter alone; the tool re-hashes the
   pinned files first and refuses (exit 2) if the epoch drifted, was closed, or
   does not match the hash the baseline was measured under (FR-144).
   `clippy-baseline.json` is the pre-FR-144 unpinned document, kept as history
   and refused as a ratchet baseline.
2. **Pick** the top lint (highest count = widest systematic pattern).
3. **Locate** where the emitter produces it (usually `lib/Target/Rust/
   TranslateToRust.cpp` or a `lib/ImportC` lowering) and change it to emit the
   idiomatic form, gated on the exact condition clippy checks (e.g. only fold a
   trailing single `\n` into `println!`).
4. **Validate — byte-diff first.** Full `nix develop -c ninja -C build
   check-emitrust` at 100% (the EndToEnd byte-diff proves stdout is unchanged);
   update every golden the new spelling shifts, in the same change.
5. **Re-measure.** `clippy_eval.py` — `total_warnings` must fall. If the suite
   is green AND the total dropped, commit the emitter change; otherwise revert
   (the loop never regresses).
6. **Ratchet.** `clippy_eval.py --update` to lower the committed baseline
   (`clippy-baseline-epoch4.json` — the same path `signals.py` ranks and
   `controller.py accept` commits; all three must agree), and commit it. Adding
   EndToEnd tests does NOT change this number: they are not in epoch-4. When
   the corpus should grow, freeze a NEW epoch, `close` the old one, and
   re-`--update` — never compare across epochs.

## Running it

- **By hand / one iteration:** follow the steps above.
- **Agent-driven (this repo's style):** dispatch one implementation subagent
  per iteration with the located lint and the byte-diff-oracle spec; the ratchet
  gates the commit.
- **Continuous:** wrap steps 1-6 in a self-paced loop (e.g. Claude Code
  `/loop`), stopping when the top remaining lint is no longer a systematic
  emitter pattern (the long tail of per-program lints is not worth an emitter
  change and should be left alone).

## Do not chase

`needless_late_init` (the `let v; … v = …` SSA shape) is tempting at 296, but
folding a declaration into its initializer is a **liveness** change, not a
spelling one — cross-iteration loop liveness has miscompiled three times here.
It is out of scope for the spelling loop; only attempt it with the byte-diff
suite in the loop and a genuinely new idea (see CLAUDE.md).
