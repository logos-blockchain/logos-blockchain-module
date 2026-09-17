{
  description = "Logos Blockchain Module - Qt6 Plugin";

  # Pull pre-built artifacts from the self-hosted Logos Attic cache(Nix binary cache).
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
    logos-blockchain.url = "github:logos-blockchain/logos-blockchain?ref=master";
  };

  outputs = inputs@{ self, logos-module-builder, ... }:
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = fn: nixpkgs.lib.genAttrs systems fn;

      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;

        externalLibInputs = {
          logos_blockchain = inputs.logos-blockchain;
        };

        tests = {
          dir = ./tests;
          mockCLibs = [ "logos_blockchain" ];
        };

        postInstall = ''
          # Remove nix references to make the module portable.
          find "$out" -type f | while read -r binary; do
            if file "$binary" | grep -E -q "Mach-O|shared library|executable|archive"; then
              echo "Scrubbing references inside verified target: $binary"
              chmod +w "$binary" 2>/dev/null || true

              perl -pi -e 's|/nix/store/[a-z0-9]{32}-boost|/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-boost|g' "$binary" 2>/dev/null || true
              perl -pi -e 's|/nix/store/[a-z0-9]{32}-nlohmann_json|/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-nlohmann_json|g' "$binary" 2>/dev/null || true
              perl -pi -e 's|/nix/store/[a-z0-9]{32}-vendor-cargo-deps|/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-vendor-cargo-deps|g' "$binary" 2>/dev/null || true
              perl -pi -e 's|/nix/var/nix/b/[a-z0-9]{26}/|/tmp/eeeeeeeeeeeeeeeeeeeeeeeeee/|g' "$binary" 2>/dev/null || true
            fi
          done
        '';
      };

      # Rust client codegen inputs.
      rustSdk = logos-module-builder.inputs.logos-rust-sdk;
      rustSdkRev = rustSdk.rev or "unknown";

      # The logos-protocol semver the builder links.
      protocolVersion =
        let
          header = builtins.readFile
            "${logos-module-builder.inputs.logos-protocol}/cpp/logos_protocol.h";
          parts = builtins.split "LOGOS_PROTOCOL_VERSION_STRING \"([^\"]*)\"" header;
        in
          if builtins.length parts < 2 then "0.1.0"
          else builtins.head (builtins.elemAt parts 1);

      # The client's node crates need rapidsnark, the circuits and the node's
      # toolchain, all taken from the logos-blockchain input.
      mkExampleRustLib = { pkgs, system }:
        let
          nodeFlake = inputs.logos-blockchain;
          rapidsnark = nodeFlake.inputs.rust-rapidsnark.packages.${system}.rapidsnark;
          circuits = nodeFlake.inputs.logos-blockchain-circuits.packages.${system}.default;
          rustChannel =
            (builtins.fromTOML (builtins.readFile "${nodeFlake}/rust-toolchain.toml")).toolchain.channel;
          rpkgs = import nixpkgs {
            inherit system;
            overlays = [ nodeFlake.inputs.rust-overlay.overlays.default ];
          };
          toolchain = rpkgs.rust-bin.stable.${rustChannel}.default;
          rustPlatform = rpkgs.makeRustPlatform { cargo = toolchain; rustc = toolchain; };
        in
        rustPlatform.buildRustPackage {
          pname = "blockchain_client_example";
          version = "0.0.999";
          src = pkgs.runCommand "blockchain-client-example-src" {} ''
            mkdir -p $out
            cp -r ${./rust-client} $out/rust-client
          '';
          sourceRoot = "blockchain-client-example-src/rust-client/example-module/rust-lib";
          cargoLock = {
            lockFile = ./rust-client/example-module/rust-lib/Cargo.lock;
            allowBuiltinFetchGit = true;
          };
          nativeBuildInputs = [ pkgs.pkg-config pkgs.cmake pkgs.clang pkgs.llvmPackages.libclang.lib ];
          buildInputs = [ pkgs.openssl ];
          env = {
            RAPIDSNARK_LIB_DIR = "${rapidsnark}";
            LBC_ROOT_DIR = "${circuits}";
            LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
          };
          doCheck = false;
        };

      mkExampleModule = { pkgs, system }:
        logos-module-builder.lib.mkLogosModule {
          src = ./rust-client/example-module;
          configFile = ./rust-client/example-module/metadata.json;
          flakeInputs = { blockchain_module = self; } // inputs;
          preConfigure = ''
            mkdir -p lib
            cp ${mkExampleRustLib { inherit pkgs system; }}/lib/libblockchain_client_example.a lib/
          '';
        };
    in
    module // {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          example = (mkExampleModule { inherit pkgs system; }).packages.${system};
        in
        module.packages.${system} // {
          rust-client-example = example.default;
          rust-client-example-lgx = example.lgx;
          rust-client-example-lgx-portable = example.lgx-portable;
          rust-client-example-install = example.install;
          rust-client-example-install-portable = example.install-portable;
        });

      # `nix run .#generate` regenerates the .lidl, the client and the example scaffold.
      apps = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          lidlGen = rustSdk.packages.${system}.lidl-gen;
          lidlPkg = module.packages.${system}.lidl;
          generate = pkgs.writeShellApplication {
            name = "blockchain-module-generate";
            runtimeInputs = [ lidlGen pkgs.git pkgs.gnugrep ];
            text = ''
              root="$(git rev-parse --show-toplevel)"

              echo "blockchain_module.lidl <- derived from src/logos_blockchain_module.h"
              install -m 644 "${lidlPkg}/blockchain_module.lidl" "$root/blockchain_module.lidl"

              echo "rust-client/src/generated.rs <- logos-lidl-gen (client backend)"
              logos-lidl-gen "$root/blockchain_module.lidl" \
                -o "$root/rust-client/src/generated.rs"

              echo "rust-client/example-module/rust-lib/src/provider_gen.rs <- logos-lidl-gen --provider (protocol ${protocolVersion})"
              logos-lidl-gen "$root/rust-client/example-module/rust-lib/blockchain_client_example.lidl" \
                --provider --protocol-version "${protocolVersion}" \
                -o "$root/rust-client/example-module/rust-lib/src/provider_gen.rs"

              for manifest in rust-client/Cargo.toml rust-client/example-module/rust-lib/Cargo.toml; do
                if ! grep -q 'rev = "${rustSdkRev}"' "$root/$manifest"; then
                  echo "WARNING: $manifest does not pin logos-rust-sdk rev ${rustSdkRev} (the builder's pin); update it." >&2
                fi
              done
              echo "done."
            '';
          };
        in {
          generate = {
            type = "app";
            program = "${generate}/bin/blockchain-module-generate";
          };
        });
    };
}
