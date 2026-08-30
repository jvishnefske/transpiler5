// FR-160b fixture: an ordinary one-TU C test binary. Its ONLY entry point is
// `main`, which is exactly what scripts/test-entries-meson.py's default mode
// writes into the registry (`syms = ["main"]`) for every one-TU test -- 337 of
// them on systemd. The emitter renames it to `c_main` UNCONDITIONALLY
// (CSymbolNaming.h; `--preserve-c-names` does not turn this one off), and
// `c_main` is a RESERVED name, so the alias back is injective.
int main(void) { return 0; }
