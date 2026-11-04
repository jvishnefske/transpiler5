# Hybrid-wedge spike evidence (FR-265, 2026-11-03)

Archived from the spike's tmpfs scratch (`/tmp/hyb`); the scripts keep their original
absolute paths (`/tmp/hyb`, `/tmp/fr264/fsrc` = the pinned woff2/brunsli/brotli
checkouts of docs/pmf/frontier/modules.toml) and are a record, not a runnable tool.

- `sample.md`, `sel.py`, `extents.py` -- the pre-registered sample rule (20 of 274 blocked
  functions with bodies <= 40 lines, sha1-ordered, stratified by module / member / tag count).
- `results.jsonl` -- one record per sampled function.
- `cert.sh` -- the five-leg certification (orig ASan/UBSan + g++ -O1, rewrite ASan/UBSan,
  bx Rust under rustc -O, Rust printed back to C++ under g++), stdout byte-diffed.
- `mut.py` -- the injected-LLM-error mutation check (17/17 behaviour-changing mutants caught).
- `encode-eoi-adversarial/` -- the alias->copy hazard: certified under the natural driver,
  50/50 lines differ under this one.
