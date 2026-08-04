{
  description = "Dev environment for libclang + MLIR pattern development";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # Pin the LLVM major version; libclang, clang, and MLIR must all
        # come from the same package set or headers and libs will mismatch.
        llvmPackages = pkgs.llvmPackages_21;

        mlir = llvmPackages.mlir;
        libclang = llvmPackages.libclang;
        llvm = llvmPackages.llvm;

        # nixpkgs' mlir package ships neither mlir-tblgen nor mlir-pdll;
        # the synthetic llvmPackages.tblgen package provides mlir-tblgen
        # (binary-cached). mlir-pdll must be added by override, which
        # builds from source — kept out of the default shell so entering
        # it stays a pure cache download.
        tblgenWithPdll = llvmPackages.tblgen.overrideAttrs (old: {
          targets = old.targets ++ [ "mlir-pdll" ];
          ninjaFlags = old.targets ++ [ "mlir-pdll" ];
        });

        mkMlirShell = tblgen:
          # Default (gcc) stdenv: linking gcc-built objects against the
          # clang/MLIR libs is fine (same libstdc++ ABI), and this nixpkgs
          # revision's llvmPackages.stdenv fails to rpath libstdc++ into
          # produced binaries. Matching clang is still on PATH.
          pkgs.mkShell {
            packages = [
              llvmPackages.clang

              # Build tooling. Meson drives the parallel build defined by
              # the meson.build files (CMake stays canonical for CI); it is
              # pinned here so `nix develop -c meson ...` cannot silently
              # fall through to a host meson of unknown version.
              pkgs.cmake
              pkgs.meson
              pkgs.ninja
              pkgs.pkg-config
              llvmPackages.lld

              # LLVM/MLIR toolchain: mlir-opt, mlir-translate, FileCheck,
              # llvm-config, clangd, clang-query, etc.
              llvm
              llvm.dev
              mlir
              mlir.dev
              libclang

              # mlir-tblgen / llvm-tblgen / clang-tblgen; MLIRConfig.cmake
              # resolves MLIR_TABLEGEN_EXE from PATH.
              tblgen
              llvmPackages.clang-tools

              # lit test runner for FileCheck-style pattern tests; psutil
              # enables lit's per-test timeout -- without it a miscompiled
              # non-terminating EndToEnd binary wedges check-emitrust forever
              (pkgs.python3.withPackages (ps: [ ps.lit ps.psutil ]))

              # Rust toolchain for the differential end-to-end tests:
              # emitrust-cc emits a cargo crate that is built and executed
              # against the clang-compiled original. Pinned here for
              # hermeticity rather than relying on a system toolchain.
              pkgs.cargo
              pkgs.rustc
            ];

            # For bindgen / clang-sys style consumers (e.g. Rust crates
            # linking libclang) and for locating headers at runtime.
            LIBCLANG_PATH = "${libclang.lib}/lib";

            # CMake package configs: find_package(MLIR), find_package(Clang)
            MLIR_DIR = "${mlir.dev}/lib/cmake/mlir";
            LLVM_DIR = "${llvm.dev}/lib/cmake/llvm";
            Clang_DIR = "${libclang.dev}/lib/cmake/clang";

            shellHook = ''
              echo "libclang/MLIR dev shell — LLVM ${llvm.version}"
              echo "  MLIR_DIR=$MLIR_DIR"
              echo "  LIBCLANG_PATH=$LIBCLANG_PATH"
            '';
          };
      in
      {
        devShells = {
          default = mkMlirShell llvmPackages.tblgen;
          # `nix develop .#pdll` — adds mlir-pdll for PDLL pattern
          # development; first entry compiles tblgen+pdll from source.
          pdll = mkMlirShell tblgenWithPdll;
        };

        packages = rec {
          default = emitrust;
          emitrust = pkgs.stdenv.mkDerivation {
            pname = "emitrust";
            version = "0.1.0";
            src = ./.;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              llvmPackages.tblgen
            ];

            buildInputs = [
              llvmPackages.llvm
              llvmPackages.mlir
              llvmPackages.libclang
              llvmPackages.clang
            ];

            MLIR_DIR = "${llvmPackages.mlir.dev}/lib/cmake/mlir";
            LLVM_DIR = "${llvmPackages.llvm.dev}/lib/cmake/llvm";
            Clang_DIR = "${llvmPackages.libclang.dev}/lib/cmake/clang";
            LIBCLANG_PATH = "${llvmPackages.libclang.lib}/lib";

            postInstall = ''
              mkdir -p $out/bin
              cp bin/emitrust-* $out/bin/
            '';
          };
          tblgen-with-pdll = tblgenWithPdll;
        };
      });
}
