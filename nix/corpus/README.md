# Transpile corpus — emitrust-cc vs. upstream embedded C, via Nix

Test the transpiler against **unmodified** CMSIS-DSP, lwIP, and FreeRTOS,
fetched directly at pinned commits (**not** from nixpkgs). Each project is
swept translation-unit by translation-unit through `emitrust-cc` with the
project's real include set and target flags, producing a Track-5-style per-TU
report: `TRANSPILED` vs `REJECT`, split from `PARSE_FAIL`.

```
nix build .#corpus-cmsis-dsp    # ARM target + CMSIS-Core + newlib sysroot
nix build .#corpus-lwip         # host target, portable protocol core
nix build .#corpus-freertos     # host target, POSIX port
nix build .#corpus-all          # all three
cat result/summary.txt          # the tally; result/report.jsonl is per-TU
```

## Why a CC shim, not `CC=emitrust-cc`

`emitrust-cc` is **not** a C compiler: it emits a Rust crate, not a `.o`, and it
does not link. So it cannot be `CC` literally for a project that compiles to
objects and links. `cc-shim.py` is the equivalent: presented as `CC` to an
unmodified build, on each `... -c foo.c -o foo.o` it

1. runs `emitrust-cc` with the same `-I/-isystem/-D/-std/--target` flags the
   build chose (passed through as `--extra-arg=…`, since `emitrust-cc` is an
   LLVM-CommandLine tool that does not accept clang flags directly — the same
   flags a `--compdb` entry would carry), records the per-TU verdict as one
   JSONL line, then
2. **shadow-compiles** the TU with a real compiler (`EMITRUST_SHIM_REAL_CC`) so
   the project build keeps progressing and can link.

Because the flags are the build's own, `PARSE_FAIL` is near zero by
construction — the property that made Track 5's 289-unit run credible.

The corpus derivations here run the shim in **sweep mode**
(`EMITRUST_SHIM_REAL_CC=none`): no shadow build, `emitrust-cc`'s own diagnostic
is the only signal. To instead drive a project's *real* build system (keeping
`make`/`cmake` alive), set `CC` to the shim and `EMITRUST_SHIM_REAL_CC` to your
real compiler — that is what the shadow-compile path exists for.

## PARSE_FAIL is a config gap, not a translation limit

`PARSE_FAIL` means clang could not build an AST — a header the include set does
not supply, a target the config does not select. It is a property of the
harness, never of the supported subset, and the tabulator keeps it strictly
separate from `REJECT` (parsed fine, could not be translated — the real demand
signal). Building this corpus was an exercise in driving `PARSE_FAIL` to zero,
and each layer of `nix/corpus/default.nix` is one gap the sweep surfaced:

- CMSIS-DSP alone → `'cmsis_compiler.h' file not found` ⇒ add **CMSIS-Core**
  (`ARM-software/CMSIS_6`).
- + CMSIS-Core on host clang → `Compiler must support ACLE V2.0` ⇒ CMSIS-Core is
  **ARM-only**; parse for the real target (`--target=arm-none-eabi -mcpu=…`).
- + ARM target → `'string.h' file not found` ⇒ add a libc; host glibc then
  fights the ARM multilib (`gnu/stubs-32.h`), so the correct libc is ARM
  **newlib** (from `gcc-arm-embedded`).

lwIP and FreeRTOS are portable C and parse host-native (glibc), so they need
only their own include set plus a small config header
(`config/lwip/`, `config/freertos/`).

## Status & knobs

- **Mechanism (the shim):** validated end-to-end against the built
  `emitrust-cc` — a transpilable TU reports `TRANSPILED`, an unsupported one
  reports `REJECT` with its located diagnostic.
- **CMSIS-DSP recipe:** derived by probing real source (the three gaps above);
  ARM target + CMSIS-Core + newlib sysroot are all wired. Adjust `-mcpu` /
  `ARM_MATH_*` in `default.nix` to sweep a different core.
- **lwIP / FreeRTOS:** the source sets, include dirs, and starter config headers
  are in place; widen the config (`config/lwip/lwipopts.h`,
  `config/freertos/FreeRTOSConfig.h`) to cover more of each tree.
- `nix build` will build the `emitrust` package first, and `corpus-cmsis-dsp`
  pulls the ARM toolchain (`gcc-arm-embedded`) for its newlib headers.

Pinned revisions: CMSIS-DSP v1.17.1, CMSIS_6 v6.1.0, lwIP STABLE-2_2_0_RELEASE,
FreeRTOS-Kernel V11.1.0 — bump the `rev`/`hash` pairs in `default.nix` to move
them (regenerate a hash with `nix-prefetch-url --unpack <archive-url>`).
