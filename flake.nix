{
  description = "Logos blockchain module - CMake + Qt6 + Rust (flake)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachSystem [
      "x86_64-linux"
      "aarch64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
    ] (system:
      let
        pkgs = import nixpkgs { inherit system; };
        qt = pkgs.qt6;

        # Pin circuits v0.3.1 assets per platform (add more as needed)
        circuitsInfo =
          if system == "aarch64-darwin" then {
            url = "https://github.com/logos-blockchain/logos-blockchain-circuits/releases/download/v0.3.1/nomos-circuits-v0.3.1-macos-aarch64.tar.gz";
            sha256 = "sha256-UfTK/MJOoUY+dvGdspodhZWfZ5c298K6pwoMaQcboHE=";
          } else if system == "x86_64-linux" then {
            url = "https://github.com/logos-blockchain/logos-blockchain-circuits/releases/download/v0.3.1/nomos-circuits-v0.3.1-linux-x86_64.tar.gz";
            sha256 = "1if58dmly4cvb1lz6dzyg5135vavji91hdayipi6i09w6hdvhyk3";
          } else null;

        circuitsTar = if circuitsInfo != null then pkgs.fetchurl { inherit (circuitsInfo) url sha256; } else null;

        # Helper to build the project with a selected CMAKE_BUILD_TYPE
        buildProject = buildType: pkgs.stdenv.mkDerivation {
          pname = "logos-blockchain-module";
          version = "unstable-${builtins.substring 0 8 self.lastModifiedDate or "dev"}";
          src = ./.;

          # Tools needed at build time
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            qt.wrapQtAppsHook
            pkgs.cacert
            pkgs.curl
            pkgs.jq
            pkgs.unzip
            pkgs.gnutar
          ];

          # Libraries and toolchains required
          buildInputs = [
            qt.qtbase
            pkgs.rustc
            pkgs.cargo
            pkgs.git
          ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [
            pkgs.libiconv
          ];

          # Ensure network tools (git/curl in cmake ExternalProject) can validate TLS
          SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
          GIT_SSL_CAINFO = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

          # Ensure CMake can find Qt6 first (inline flags in configurePhase to avoid scope issues)
          # Note: We do not reference a sibling attribute here because mkDerivation attrsets
          # are not recursive; instead, we expand the flags directly below.

          # ExternalProject clones during build; Nix sandboxes usually block network.
          # If you use a sandboxed Nix, either disable sandbox for this build
          # or vendor/pin those deps as flake inputs and patch CMake to use them.

          # The upstream CMakeLists has a post-build step copying the built
          # library back to the source tree, which is read-only in Nix.
          # Remove that step during the build.
          patchPhase = ''
            echo "Patching CMakeLists.txt to disable post-build copy to source tree"
            # Delete the block starting at the add_custom_command(TARGET ... POST_BUILD)
            # and ending before the next blank line or comment trailer
            sed -i.bak \
              -e '/add_custom_command(TARGET[[:space:]]\+blockchainmodulelib[[:space:]]\+POST_BUILD/,/VERBATIM)/d' \
              CMakeLists.txt
          '';

          # Use out-of-source build rooted in $NIX_BUILD_TOP for robustness across phases
          configurePhase = ''
            # Help CMake locate Qt6 provided by Nix
            export CMAKE_PREFIX_PATH='${qt.qtbase}'
            # Ensure cargo can write its cache in a writable location
            export CARGO_HOME="$NIX_BUILD_TOP/cargo-home"
            mkdir -p "$CARGO_HOME"
            mkdir -p "$NIX_BUILD_TOP/build"
            cmake -G Ninja -S . -B "$NIX_BUILD_TOP/build" \
              -DCMAKE_BUILD_TYPE=${buildType} \
              -DUNTITLED_USE_QT=ON
          '';

          buildPhase = ''
            # Keep cargo cache writable for ExternalProject-driven cargo builds
            export CARGO_HOME="$NIX_BUILD_TOP/cargo-home"
            mkdir -p "$CARGO_HOME"

            # Provide NOMOS_CIRCUITS from pinned artifact if not set by caller
            if [ -z "$NOMOS_CIRCUITS" ]; then
              if [ -n "${circuitsTar}" ]; then
                echo "Using pinned circuits archive for system ${system}"
                unpackDir="$NIX_BUILD_TOP/circuits-unpack"
                mkdir -p "$unpackDir"
                # circuitsTar is a fixed-output path in the Nix store; just unpack
                tar -xzf "${circuitsTar}" -C "$unpackDir"
                circuit_root=$(find "$unpackDir" -mindepth 1 -maxdepth 1 -type d | head -n1)
                if [ -z "$circuit_root" ]; then
                  echo "Could not determine circuits directory inside pinned archive" >&2
                  exit 1
                fi
                export NOMOS_CIRCUITS="$circuit_root"
                echo "NOMOS_CIRCUITS=$NOMOS_CIRCUITS"
              else
                echo "No pinned circuits for system ${system}. Set NOMOS_CIRCUITS to a local path and rebuild with --impure." >&2
                exit 1
              fi
            fi
            ninja -C "$NIX_BUILD_TOP/build" blockchainmodulelib
          '';

          installPhase = ''
            mkdir -p $out/lib $out/include
            # Install the produced shared library
            set -e
            if ls "$NIX_BUILD_TOP/build"/libblockchainmodulelib.* >/dev/null 2>&1; then
              install -m755 "$NIX_BUILD_TOP/build"/libblockchainmodulelib.* $out/lib/
            elif ls "$sourceRoot"/libblockchainmodulelib.* >/dev/null 2>&1; then
              install -m755 "$sourceRoot"/libblockchainmodulelib.* $out/lib/
              else
              # Fallback: search within source root
              found=$(find "$NIX_BUILD_TOP" -maxdepth 3 -name 'libblockchainmodulelib.*' | head -n1)
              if [ -n "$found" ]; then
                install -m755 "$found" $out/lib/
              else
                echo "Error: built library not found" >&2
                exit 1
              fi
            fi

            # Optionally expose the module's public header
            install -m644 ${./library.h} $out/include/library.h
          '';

          meta = with pkgs.lib; {
            description = "Logos blockchain module (Qt6 + Rust)";
            homepage = "https://github.com/logos-co";
            platforms = platforms.all;
            # Use a permissive placeholder to avoid unfree gating during builds.
            # Adjust to the correct license once finalized.
            license = licenses.mit;
            maintainers = [];
          };
        };
      in
      {
        packages = rec {
          debug = buildProject "Debug";
          release = buildProject "Release";
          default = release;
        };

        # Developer shells with all tools available
        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.rustc
            pkgs.cargo
            pkgs.git
            qt.qtbase
          ];
          # Help CMake find Qt6 in ad-hoc builds/CLion
          CMAKE_PREFIX_PATH = qt.qtbase;
          # Expose both Debug and Release convenience commands
          shellHook = ''
            echo "Dev shell for logos-blockchain-module"
            echo "Build (Debug):   cmake -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUNTITLED_USE_QT=ON -DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH . && cmake --build cmake-build-debug --target blockchainmodulelib"
            echo "Build (Release): cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNTITLED_USE_QT=ON -DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH . && cmake --build cmake-build-release --target blockchainmodulelib"
          '';
        };
      }
    );
}
