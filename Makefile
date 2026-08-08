# Thin routing layer to the project's REAL oracle (this is a CMake/meson +
# lit project, not a cargo one): `make verify` must run the full
# check-emitrust lit suite -- EndToEnd byte-diffs, both conformance
# ledgers (CTestSuite and Cpp17Suite), dialect round-trips, Driver
# goldens. `cargo build` success alone cannot see a miscompile.
#
# The checked-out build/ directory is meson-configured (tools land in
# build/tools/); `meson test` runs the same lit suite as the CMake
# check-emitrust target.

.PHONY: verify build
build:
	nix develop -c meson compile -C build
verify: build
	nix develop -c meson test -C build --print-errorlogs
