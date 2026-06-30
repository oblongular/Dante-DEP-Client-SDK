{
  description = "Dante DEP Client SDK";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs   = nixpkgs.legacyPackages.${system};
      lib    = pkgs.lib;
      cross  = pkgs.pkgsCross.aarch64-multiplatform;

      #
      # Build the full SDK package for any stdenv — host or cross.
      #
      # Produces: $out/bin/dep_sync_fanoutd
      #           $out/lib/libDanteAudio.a
      #           $out/include/dante/DanteAudio.hpp
      #           $out/cmake/libDanteAudio.cmake
      #           $out/lib/systemd/system/dep_sync_fanoutd.service
      #
      # TODO: make this work for ARM
      #
      makePackage = stdenv:
        let
          isCross = stdenv.hostPlatform != stdenv.buildPlatform;
        in
        stdenv.mkDerivation {
          pname   = "dep-client-sdk";
          version = "0.1.0";
          src     = ./.;

          nativeBuildInputs = [ pkgs.cmake ];

          configurePhase = ''
            runHook preConfigure
            cmake -S daemon -B _build \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_INSTALL_PREFIX="$out" \
              -DCMAKE_C_COMPILER="$CC" \
              -DCMAKE_CXX_COMPILER="$CXX" \
              ${lib.optionalString isCross ''
                -DCMAKE_CROSSCOMPILING=TRUE \
                -DCMAKE_SYSTEM_NAME=Linux \
                -DCMAKE_SYSTEM_PROCESSOR=${stdenv.hostPlatform.parsed.cpu.name}
              ''}
            runHook postConfigure
          '';

          buildPhase = ''
            runHook preBuild

            # Build the daemon
            cmake --build _build --parallel "$NIX_BUILD_CORES"

            # Compile DanteAudio.cpp for the target platform
            $CXX -O2 -std=c++17 -c src/DanteAudio.cpp -Iinclude -o _DanteAudio.o

            # Bundle into libDanteAudio.a.
            # Native: include libdep_audio.a objects for a self-contained archive.
            # Cross:  DanteAudio.o only — libdep_audio.a is x86-64, not bundleable.
            ${if isCross then ''
              ar rcs _libDanteAudio.a _DanteAudio.o
            '' else ''
              mkdir -p _dep_objs
              (cd _dep_objs && ar x $src/lib/libdep_audio.a)
              ar rcs _libDanteAudio.a _DanteAudio.o _dep_objs/*.o
            ''}

            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall

            # Daemon binary + systemd unit (via cmake install)
            cmake --install _build

            # Library, header, cmake integration
            mkdir -p $out/lib $out/include/dante $out/cmake
            cp _libDanteAudio.a         $out/lib/libDanteAudio.a
            cp include/dante/DanteAudio.hpp $out/include/dante/
            cp cmake/libDanteAudio.cmake    $out/cmake/

            runHook postInstall
          '';
        };

    in {
      packages.${system} = {
        # Native x86-64 — library + daemon for dev/test on the build machine
        dep-client-sdk         = makePackage pkgs.stdenv;

        # Cross-compiled for the aarch64 Linux target
        dep-client-sdk-aarch64 = makePackage cross.stdenv;

        default = self.packages.${system}.dep-client-sdk;
      };

      # Dev shell: host toolchain + aarch64 cross-compiler
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.cmake
          pkgs.gcc
          cross.stdenv.cc   # aarch64-unknown-linux-gnu-g++
        ];
        shellHook = ''
          echo "Dante DEP SDK build targets:"
          echo "  nix build .#dep-client-sdk           (x86-64, dev/test)"
          echo "  nix build .#dep-client-sdk-aarch64   (aarch64, deploy to target)"
        '';
      };
    };
}
