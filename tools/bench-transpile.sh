#!/usr/bin/env bash
#
# tools/bench-transpile.sh — transpile / transpile+build timing baseline.
#
# Every EndToEnd differential test finishes in milliseconds at runtime, so
# runtime profiling of the *generated* binaries tells us nothing about the
# cost of this project. The actual cost that regresses or improves as the
# pipeline changes is wall time spent (a) transpiling C to Rust and
# (b) transpiling *and* `cargo build --release`-ing the result. This script
# measures both, over a fixed, explicitly-listed set of representative
# EndToEnd programs, so the numbers are reproducible from one run to the
# next and comparable across commits.
#
# hyperfine is intentionally NOT used (not in the dev shell); this uses only
# the bash builtin `time`.
#
# Usage:
#   nix develop -c bash tools/bench-transpile.sh <path-to-emitrust-cc> [N]
#
# N is the number of iterations per program per mode (default 5). The
# reported figure per program/mode is the MEDIAN wall-clock time across the
# N iterations; the reported total is the sum of those medians.
#
# This script is read-only with respect to the repository: every transpiled
# file and built crate goes into a `mktemp -d` workdir that is removed on
# exit (including on error/interrupt).

set -uo pipefail

# ---------------------------------------------------------------------------
# Fixed, explicitly-listed program set (test/EndToEnd), spanning the cost
# spectrum from trivial control flow to varargs monomorphization:
#
#   loops.c                 - trivial: a couple of for/while loops, no I/O
#   stdio-include.c         - trivial `#include <stdio.h>` + one printf
#   bitfields-flags.c       - bit-field packing/unpacking
#   stdio-file-roundtrip.c  - FILE* fopen/fread/fwrite/fclose round-trip
#   varargs-monomorph.c     - va_list monomorphization (heaviest single case)
#   byte-region-walk.c      - byte-region / raw memory walk lowering
#   unions.c                - tagged union layout
#   switch-dispatch.c       - switch-to-jump-table dispatch lowering
#   pointers-multi-base.c   - multi-base pointer provenance tracking
#   printf-formats.c        - variadic printf format-string dispatch
#
# Keep this list in sync by hand; do not glob test/EndToEnd so the baseline
# stays reproducible as new tests are added.
# ---------------------------------------------------------------------------
PROGRAMS=(
  loops.c
  stdio-include.c
  bitfields-flags.c
  stdio-file-roundtrip.c
  varargs-monomorph.c
  byte-region-walk.c
  unions.c
  switch-dispatch.c
  pointers-multi-base.c
  printf-formats.c
)

usage() {
  echo "usage: $0 <path-to-emitrust-cc> [N=5]" >&2
  exit 2
}

[ $# -ge 1 ] || usage
EMITRUST_CC=$1
N=${2:-5}

case "$EMITRUST_CC" in
  /*) : ;;
  *) EMITRUST_CC="$(pwd)/$EMITRUST_CC" ;;
esac

if [ ! -x "$EMITRUST_CC" ]; then
  echo "error: '$EMITRUST_CC' is not an executable file" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ENDTOEND_DIR="$REPO_ROOT/test/EndToEnd"

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/bench-transpile.XXXXXX")"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT INT TERM

# `time` (the bash builtin/reserved word) writes its report to stderr in
# whatever format $TIMEFORMAT specifies; '%R' is just the elapsed wall
# seconds, so a single float is all we have to parse back out.
export TIMEFORMAT='%R'

# Prints the median of its numeric ("%R"-formatted seconds) arguments.
median() {
  local -a sorted
  mapfile -t sorted < <(printf '%s\n' "$@" | sort -g)
  local n=${#sorted[@]}
  echo "${sorted[$(((n - 1) / 2))]}"
}

# run_timed CMD... — runs CMD, discarding its stdout/stderr, and echoes the
# elapsed wall-clock seconds. Non-zero exit from CMD is reported but does
# not abort the run (a stray build failure for one program/iteration should
# not blank out the rest of the baseline).
run_timed() {
  local t status
  t=$(
    { time "$@" >/dev/null 2>&1; } 2>&1
  )
  status=$?
  echo "$t"
  return "$status"
}

printf '%s\n' "emitrust-cc: $EMITRUST_CC"
printf '%s\n' "iterations per program per mode (N): $N"
printf '%s\n' "programs: ${PROGRAMS[*]}"
echo

printf '%-24s %14s %14s\n' "program" "transpile(s)" "transpile+build(s)"
printf '%-24s %14s %14s\n' "-------" "------------" "-------------------"

total_transpile=0
total_build=0
fail_count=0

for prog in "${PROGRAMS[@]}"; do
  src="$ENDTOEND_DIR/$prog"
  if [ ! -f "$src" ]; then
    echo "error: missing fixture $src" >&2
    fail_count=$((fail_count + 1))
    continue
  fi
  base="${prog%.c}"

  transpile_times=()
  for i in $(seq 1 "$N"); do
    out_rs="$WORKDIR/${base}.rs"
    t=$(run_timed "$EMITRUST_CC" --emit=rust "$src" -o "$out_rs") || fail_count=$((fail_count + 1))
    transpile_times+=("$t")
    rm -f "$out_rs"
  done

  build_times=()
  for i in $(seq 1 "$N"); do
    out_crate="$WORKDIR/${base}-crate-$i"
    t=$(run_timed "$EMITRUST_CC" --emit=crate "$src" -o "$out_crate" --build) || fail_count=$((fail_count + 1))
    build_times+=("$t")
    rm -rf "$out_crate"
  done

  t_med=$(median "${transpile_times[@]}")
  b_med=$(median "${build_times[@]}")

  printf '%-24s %14s %14s\n' "$prog" "$t_med" "$b_med"

  total_transpile=$(awk -v a="$total_transpile" -v b="$t_med" 'BEGIN{printf "%.3f", a+b}')
  total_build=$(awk -v a="$total_build" -v b="$b_med" 'BEGIN{printf "%.3f", a+b}')
done

echo
printf '%-24s %14s %14s\n' "TOTAL (sum of medians)" "$total_transpile" "$total_build"

if [ "$fail_count" -gt 0 ]; then
  echo
  echo "warning: $fail_count individual run(s) exited non-zero; see numbers above with care" >&2
fi

exit 0
