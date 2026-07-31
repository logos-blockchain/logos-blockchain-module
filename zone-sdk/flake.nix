{
  description = "Logos Zone SDK Module";

  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
    logos-blockchain-module.url = "path:..";
  };

  outputs = inputs@{ self, logos-module-builder, logos-blockchain-module, ... }:
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = fn: nixpkgs.lib.genAttrs systems fn;

      module = system:
        logos-module-builder.lib.mkLogosModule {
          src = ./.;
          configFile = ./metadata.json;
          flakeInputs = {
            blockchain_module = logos-blockchain-module;
          } // inputs;
        };
    in
    {
      packages = forAllSystems (system:
        let m = (module system).packages.${system};
        in m // {
          zone_sdk = m.default;
          "blockchain_module-lgx" = logos-blockchain-module.packages.${system}.lgx;
        });

      # `nix run .#generate` materialises `rust-lib/generated/` and `logos-rust-sdk-src/`
      # so bare `cargo build/test` works in rust-lib/ directly during development.
      apps = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          lidlGen = logos-module-builder.inputs.logos-rust-sdk.packages.${system}.lidl-gen;
          sdkSrc = logos-module-builder.packages.${system}.rust-sdk-src;
          generate = pkgs.writeShellApplication {
            name = "zone-sdk-generate";
            runtimeInputs = [ lidlGen pkgs.git ];
            text = ''
              root="$(git rev-parse --show-toplevel)"
              echo "generating rust-lib/generated/provider_gen.rs ..."
              mkdir -p "$root/rust-lib/generated"
              logos-lidl-gen "$root/rust-lib/zone_sdk.lidl" --provider \
                --dep blockchain_module="$root/rust-lib/deps/blockchain_module.lidl" \
                -o "$root/rust-lib/generated/provider_gen.rs"
              echo "staging the SDK source at logos-rust-sdk-src/ ..."
              rm -rf "''${root:?}/logos-rust-sdk-src"
              cp -RL "${sdkSrc}" "$root/logos-rust-sdk-src"
              chmod -R u+w "$root/logos-rust-sdk-src"
              echo "done. bare 'cargo build' now works in rust-lib/"
            '';
          };
        in {
          generate = {
            type = "app";
            program = "${generate}/bin/zone-sdk-generate";
          };
        });

      devShells = forAllSystems (system:
        let pkgs = import nixpkgs { inherit system; };
        in {
          default = pkgs.mkShell {
            packages = [ pkgs.protobuf ];
          };
        });
    };
}
