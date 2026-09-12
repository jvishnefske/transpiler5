# Thin routing layer to the project's REAL oracle (this is a meson + lit
# project, not a cargo one): `make verify` must run the full lit suite --
# EndToEnd byte-diffs, the conformance ledgers (CTestSuite, Cpp17Suite,
# CppStdSuite), dialect round-trips, Driver goldens. `cargo build`
# success alone cannot see a miscompile.
#
# The checked-out build/ directory is meson-configured (tools land in
# build/tools/).

.PHONY: verify build
build:
	nix develop -c meson compile -C build
verify: build
	nix develop -c meson test -C build --print-errorlogs
