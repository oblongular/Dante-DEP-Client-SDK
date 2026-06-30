{
  description = "Dante DEP Client SDK";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs   = nixpkgs.legacyPackages.${system};
      lib    = pkgs.lib;
      cross  = pkgs.pkgsCross.aarch64-multiplatform;

      # Build dep_sync_fanoutd for any stdenv — host or cross.
      # cmake is always from the host; $CC/$CXX come from stdenv and point at
      # the appropriate compiler for the target platform.
      makeDaemon = stdenv:
        let
          isCross = stdenv.hostPlatform != stdenv.buildPlatform;
        in
        stdenv.mkDerivation {
          pname   = "dep-sync-fanoutd";
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
            cmake --build _build --parallel "$NIX_BUILD_CORES"
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            cmake --install _build
            runHook postInstall
          '';
        };

    in {
      packages.${system} = {
        # Native x86-64 build — useful for smoke-testing on the dev machine
        dep-sync-fanoutd         = makeDaemon pkgs.stdenv;

        # Cross-compiled for the aarch64 Linux target
        dep-sync-fanoutd-aarch64 = makeDaemon cross.stdenv;

        default = self.packages.${system}.dep-sync-fanoutd;
      };

      # Dev shell: host toolchain + aarch64 cross-compiler for manual build steps
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.cmake
          pkgs.gcc
          cross.stdenv.cc   # aarch64-unknown-linux-gnu-g++
        ];
        shellHook = ''
          echo "Dante DEP SDK build targets:"
          echo "  nix build .#dep-sync-fanoutd           (x86-64, smoke test)"
          echo "  nix build .#dep-sync-fanoutd-aarch64   (aarch64, deploy to target)"
        '';
      };
    };
}
