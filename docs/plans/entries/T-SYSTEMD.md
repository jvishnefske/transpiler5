- [x] T-SYSTEMD EXTERNAL PROBE (measured 2026-08-28 on branch `probe/systemd`,
  cherry-picked into the main line 2026-08-29): systemd `39cf979` (v262 dev),
  1600 translation units, configured with meson under a nix shell so all 58
  generated sources resolve. **The largest external corpus this project has
  measured -- roughly 19x the 85-unit probe set and 6x TRACTOR.**
  TOOLING CHERRY-PICKED: `scripts/compdb-probe/{measure,aggregate,buildall,
  shards}.py`, 242 lines, ZERO systemd references -- they drive off a
  `compile_commands.json`, so they generalize to any configured C project. They
  complement what already exists rather than duplicating it:
  `scripts/external-probe.py` (FR-145) enumerates repos by DIRECTORY and needs
  no build system; these need a compile database and reach projects that
  require one. `shards.py` covers the FR-58 `--link` whole-program path, which
  nothing committed reached before. `measure.py` already carries the
  FUNCTION-items ranking lesson in its docstring.
  NOT CHERRY-PICKED, deliberately and consistently with the standing stance
  that corpora and generated output are not vendored (FR-145, FR-138): the 24M
  archive of 1585 emitted crates, 2.2M of unpacked exemplars, a 1.4M
  build-oracle record, and an 8.1M VM harness tree. They remain in the
  worktree `.claude/worktrees/systemd-probe`.
  IMPORT COVERAGE, deduped by symbol+file+line -- the raw per-TU totals count a
  header record once per including TU and measure header fan-out, not progress:
  record 820/1045 (78.5%), enum 565/595 (95.0%), global 802/6675 (12.0%),
  **FUNCTION (defined) 1618/26084 (6.2%)**. Functions defined in the unit's own
  `.c` (excluding `static inline` from headers): 1154 ported over 434 of 1585
  crates -- **1151 crates port no function of their own at all.**
  By directory: src/basic 442/2922 (15.1%), src/shared 325/3683 (8.8%),
  src/core 47/3079 (1.5%), src/systemd 4/1464 (0.3%).
  BUILD ORACLE (`cargo build --release --offline`, every crate): **1427/1585
  build clean. The 158 failures are THREE emitter bugs, not 158** -- which is
  the FR-145 build-oracle thesis confirmed on a corpus 19x larger, and the
  reason a per-crate failure count is a misleading headline.
  RANKED FOLLOW-UPS, filed as FR-149..FR-153 below:
    1. enum-valued array subscript (5 units) -- gates `log.c`, and `log.c`
       gates EVERY whole-program link.
    2. prelude-name collision for emitted types (123 crates).
    3. TU-unique anonymous struct names (blocks the 493-shard merge).
    4. `scf.while` legalization in gperf lookup code (7 units).
    5. E0596 mutable-borrow-through-shared (35 crates).
  Items 1-3 are narrow and mechanical. **6.2% function coverage is the deep
  front and it is pointer-model work** -- incomplete struct types 3964,
  pointer-to-pointer parameter 1878 + shape-escape 1775, address-of-pointer
  1735, void pointer parameter 1453 -- not one increment.
  RUNNING THE OUTPUT, recorded because it is the strongest end-to-end evidence
  this project has: the probe's `vm/` boots a 6.18.46 kernel under QEMU with a
  Rust PID 1 that links six emitted crates and calls transpiled `src/basic`
  functions on the live kernel (13/13 checks pass), then runs a Rust TCP echo
  service the host tests byte-exactly over a forwarded port. It is NOT systemd
  -- nothing here links into an init system -- but it is emitted Rust EXECUTING
  on a real kernel rather than only compiling.

