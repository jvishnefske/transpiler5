# Transpile-corpus: run emitrust-cc against unmodified upstream embedded C
# (CMSIS-DSP, lwIP, FreeRTOS) fetched directly at pinned commits -- NOT from
# nixpkgs. Each project is swept translation-unit by translation-unit through
# emitrust-cc via the CC shim (nix/corpus/cc-shim.py), with the project's real
# include set / target flags, producing a Track-5-style per-TU report:
# TRANSPILED vs REJECT, split from PARSE_FAIL (a config/include gap, never a
# translation limit). Because the flags are the project's own, PARSE_FAIL is
# near zero by construction -- the property that made Track 5 credible.
#
# Usage (from the repo flake):
#   nix build .#corpus-cmsis-dsp   # ARM target + CMSIS-Core + newlib sysroot
#   nix build .#corpus-lwip        # host target, portable core
#   nix build .#corpus-freertos    # host target, POSIX port
#   nix build .#corpus-all         # all three, one $out with each summary
# Each result has $out/summary.txt (the tally) and $out/report.jsonl (per TU).
{ pkgs
, emitrust # the emitrust package: provides bin/emitrust-cc
, gccArmEmbedded ? pkgs.gcc-arm-embedded # ARM newlib sysroot for CMSIS
}:

let
  inherit (pkgs) lib;
  clang = pkgs.llvmPackages_21.clang;
  libcInclude = "${pkgs.glibc.dev}/include"; # host libc headers for lwIP/FreeRTOS
  shim = ./cc-shim.py;
  tabulate = ./tabulate.py;
  fetch = pkgs.fetchFromGitHub;

  # --- upstream sources, pinned (hashes from `nix-prefetch-url --unpack`) ---
  cmsisDsp = fetch {
    owner = "ARM-software"; repo = "CMSIS-DSP";
    rev = "dd265b0f63938091a2a9cddd536d95ca582a6408"; # v1.17.1
    hash = "sha256-etzBaNz5t/vnj1pvtELqTWA7y7r01liHIT3aID7vM6w=";
  };
  cmsisCore = fetch {
    owner = "ARM-software"; repo = "CMSIS_6";
    rev = "960b9c3e22ab12bac4634b3ec39552fd6c89cb20"; # v6.1.0
    hash = "sha256-Nu7Pxs7/npC3/zBgDcY3zvL14SrQPoU8tIybIr60rUs=";
  };
  lwip = fetch {
    owner = "lwip-tcpip"; repo = "lwip";
    rev = "07a0dec3d4fa08ec332e5c297c40899ae6c727c6"; # STABLE-2_2_0_RELEASE
    hash = "sha256-ZUnFmvzC4Pg4v+oGm3mb1GdPX0s2ycJg4kXnN6a9a/w=";
  };
  freertos = fetch {
    owner = "FreeRTOS"; repo = "FreeRTOS-Kernel";
    rev = "f388a5c8078e152913e4eb3c5d75bf89561392df"; # V11.1.0
    hash = "sha256-wcayA4YCfHGD6pwzG40IW2L0RhZ6KwumtbwNZpxUk7s=";
  };

  # Generic sweep. `sources` is a bash glob of ABSOLUTE .c paths; `flags` is the
  # list of clang args (targets, -I, -isystem, -D, -std) the project compiles
  # with. Each TU is driven through emitrust-cc by the shim (REAL_CC=none, so
  # emitrust-cc is the only signal); the report and its summary land in $out.
  mkTranspileCorpus = { name, sources, flags }:
    pkgs.stdenv.mkDerivation {
      pname = "transpile-corpus-${name}";
      version = "1.0";
      dontUnpack = true;
      dontConfigure = true;
      dontFixup = true;
      nativeBuildInputs = [ emitrust pkgs.python3 ];
      buildPhase = ''
        runHook preBuild
        export EMITRUST_SHIM_EMITRUST_CC="${emitrust}/bin/emitrust-cc"
        export EMITRUST_SHIM_REAL_CC=none
        export EMITRUST_SHIM_LOG="$PWD/report.jsonl"
        export EMITRUST_SHIM_CRATES="$TMPDIR/crates"
        export EMITRUST_RESOURCE_DIR="$(${clang}/bin/clang -print-resource-dir)"
        : > "$EMITRUST_SHIM_LOG"; mkdir -p "$EMITRUST_SHIM_CRATES"
        shopt -s globstar nullglob
        n=0
        for f in ${sources}; do
          python3 ${shim} -c "$f" -o "$TMPDIR/$n.o" ${lib.concatStringsSep " " flags} || true
          n=$((n + 1))
        done
        echo "swept $n translation units for ${name}"
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out
        cp report.jsonl $out/report.jsonl
        python3 ${tabulate} $out/report.jsonl ${name} | tee $out/summary.txt
      '';
    };

  # CMSIS-DSP is ARM-only at the header level (CMSIS-Core needs ACLE + an Arm
  # architecture profile), so it is parsed for its real target with the ARM
  # newlib sysroot for libc. This is how the code is actually compiled.
  cmsis-dsp = mkTranspileCorpus {
    name = "cmsis-dsp";
    sources = "${cmsisDsp}/Source/**/*.c";
    flags = [
      "--target=arm-none-eabi" "-mcpu=cortex-m4"
      "-mfloat-abi=hard" "-mfpu=fpv4-sp-d16" "-DARM_MATH_CM4"
      "-I${cmsisDsp}/Include" "-I${cmsisDsp}/PrivateInclude"
      "-I${cmsisCore}/CMSIS/Core/Include"
      "-isystem" "${gccArmEmbedded}/arm-none-eabi/include"
    ];
  };

  # lwIP is portable C; parsed host-native. NO_SYS build of the protocol core.
  lwip-corpus = mkTranspileCorpus {
    name = "lwip";
    sources = "${lwip}/src/core/*.c ${lwip}/src/core/ipv4/*.c ${lwip}/src/core/ipv6/*.c ${lwip}/src/netif/*.c";
    flags = [
      "-std=gnu11"
      "-I${lwip}/src/include"
      "-I${./config/lwip}"
      "-isystem" libcInclude
    ];
  };

  # FreeRTOS-Kernel with the POSIX port; parsed host-native.
  freertos-corpus = mkTranspileCorpus {
    name = "freertos";
    sources = lib.concatStringsSep " " (map (f: "${freertos}/${f}") [
      "tasks.c" "queue.c" "list.c" "timers.c" "event_groups.c"
      "stream_buffer.c" "portable/ThirdParty/GCC/Posix/port.c"
      "portable/MemMang/heap_4.c"
    ]);
    flags = [
      "-std=gnu11"
      "-I${freertos}/include"
      "-I${freertos}/portable/ThirdParty/GCC/Posix"
      "-I${./config/freertos}"
      "-isystem" libcInclude
    ];
  };

  corpus-all = pkgs.runCommand "transpile-corpus-all" { } ''
    mkdir -p $out
    for c in ${cmsis-dsp} ${lwip-corpus} ${freertos-corpus}; do
      cat "$c/summary.txt" >> $out/summary.txt
      echo >> $out/summary.txt
    done
    cat $out/summary.txt
  '';
in
{
  corpus-cmsis-dsp = cmsis-dsp;
  corpus-lwip = lwip-corpus;
  corpus-freertos = freertos-corpus;
  corpus-all = corpus-all;
}
