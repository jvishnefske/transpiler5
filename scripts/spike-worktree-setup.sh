#!/usr/bin/env bash
# Bootstrap an agent/spike git worktree of transpiler5 so it can actually
# build. Three failure modes have each cost a spike real time (see the
# agent-worktree-setup memory note and the W2.19/W2.23/W2.24 spike reports):
#
#   1. The worktree branch is checked out at a STALE commit (one spike
#      started 101 commits behind and measured pre-W2.15 machinery).
#   2. meson's `dependency('llvm', method: 'config-tool')` prefers the
#      host's /usr/bin/llvm-config-22 over the devshell's LLVM 21, mixing
#      headers and failing the build with confusing template errors.
#   3. The first full build is slow enough to trip a 600s no-progress
#      watchdog when run in the foreground.
#
# This script fixes 1 and 2 and prints the backgrounding advice for 3.
# Run it from INSIDE the worktree, with nix available.
set -euo pipefail

wt=$(git rev-parse --show-toplevel)
main=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")
cd "$wt"

# --- 1. fast-forward to the main tree's HEAD -------------------------------
main_head=$(git -C "$main" rev-parse HEAD)
if [ "$(git rev-parse HEAD)" != "$main_head" ]; then
  echo "worktree is at $(git rev-parse --short HEAD); main tree is at" \
       "$(git -C "$main" rev-parse --short HEAD) -- resetting"
  git reset --hard "$main_head"
else
  echo "worktree already at main HEAD $(git rev-parse --short HEAD)"
fi

# --- 2. pin llvm-config to the devshell's LLVM -----------------------------
# `nix develop` prints the devshell BANNER on stdout (three lines: the shell
# name, MLIR_DIR, LIBCLANG_PATH), so the command substitution captures four
# lines, not one, and the naive form wrote a multi-line garbage value into
# build-native.ini. meson then failed on it in a way that reads as a
# toolchain problem rather than a quoting bug, which has cost real time more
# than once. `tail -1` keeps only the path `command -v` actually printed.
llvm_config=$(nix develop "$main" -c sh -c 'command -v llvm-config' | tail -1)
case "$llvm_config" in
  /*) ;;
  *) echo "spike-worktree-setup: llvm-config did not resolve to an absolute" \
          "path (got '$llvm_config'); refusing to write a corrupt native file" >&2
     exit 1 ;;
esac
native_ini="$wt/build-native.ini"
printf '[binaries]\nllvm-config = %s\n' "'$llvm_config'" > "$native_ini"
echo "pinned llvm-config = $llvm_config ($native_ini)"

if [ ! -d "$wt/build" ]; then
  nix develop "$main" -c meson setup --native-file "$native_ini" build
else
  echo "build/ already configured; to reconfigure:"
  echo "  nix develop $main -c meson setup --wipe --native-file $native_ini build"
fi

# --- 3. the build itself ----------------------------------------------------
cat <<'EOF'
Next: compile in the BACKGROUND and poll (a cold build of the MLIR/clang
toolchain trips foreground watchdogs):
  nix develop <main> -c meson compile -C build     # run_in_background
Until it finishes, measure UNPATCHED behavior with read-only COPIES of the
main tree's built tools (cp them out first; never run a build in the main
tree while another wave is live there).
EOF
