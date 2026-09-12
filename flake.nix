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

        # nixpkgs' mlir package ships no mlir-tblgen; the synthetic
        # llvmPackages.tblgen package provides it (binary-cached). The
        # tblgenWithPdll override and the .#pdll shell that built
        # mlir-pdll from source were deleted with the PDLL showcase
        # (FR-245); git history preserves both.
        mkMlirShell = tblgen:
          # Default (gcc) stdenv: linking gcc-built objects against the
          # clang/MLIR libs is fine (same libstdc++ ABI), and this nixpkgs
          # revision's llvmPackages.stdenv fails to rpath libstdc++ into
          # produced binaries. Matching clang is still on PATH.
          pkgs.mkShell {
            packages = [
              llvmPackages.clang

              # Build tooling. Meson is the build system (the CMake build
              # was deleted in FR-245); meson is pinned here so
              # `nix develop -c meson ...` cannot silently fall through to
              # a host meson of unknown version. cmake stays NOT for this
              # repo's build but for the TRACTOR corpus configure step
              # (tractor-eval.py drives cmake-based corpus projects).
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
              # non-terminating EndToEnd binary wedges check-emitrust forever.
              # The remaining packages are web/'s runtime and test deps
              # (web/server/requirements.txt): the service is part of this
              # repo, so `nix develop -c python3 -m pytest web/tests` must
              # work without a pip install. httpx is test-only -- it drives
              # the ASGI app in-process via httpx.ASGITransport, so the API
              # tests need no network and no live uvicorn.
              (pkgs.python3.withPackages (ps: [
                ps.lit
                ps.psutil
                ps.fastapi
                ps.uvicorn
                ps.pydantic
                ps.google-auth
                # google-auth's `transport.requests` module -- which
                # verify_oauth2_token needs -- imports `requests` lazily and
                # raises ImportError without it. Measured: without this the
                # service answers every live compile 503 "google-auth is not
                # installed on the server" while `import google.auth` works
                # fine. It is the `google-auth[requests]` extra.
                ps.requests
                ps.httpx
                ps.pytest
              ]))

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
        };

        packages =
          let
            emitrust = pkgs.stdenv.mkDerivation {
              pname = "emitrust";
              version = "0.1.0";
              src = ./.;

              nativeBuildInputs = [
                pkgs.meson
                pkgs.ninja
                pkgs.pkg-config
                llvmPackages.tblgen
                llvmPackages.llvm.dev
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
            };
            # Transpile-corpus derivations: run emitrust-cc against upstream
            # embedded C (CMSIS-DSP, lwIP, FreeRTOS) fetched directly, NOT from
            # nixpkgs. See nix/corpus/README.md.
            corpus = import ./nix/corpus { inherit pkgs emitrust; };
          in
          {
            default = emitrust;
            inherit emitrust;
          } // corpus;
      });
}
