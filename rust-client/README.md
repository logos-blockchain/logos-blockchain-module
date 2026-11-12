# logos-blockchain-client

Rust client for `blockchain_module`, generated from [`../blockchain_module.lidl`](../blockchain_module.lidl)
by `logos-lidl-gen` and committed, so consumers need cargo only.

```toml
[dependencies]
logos-blockchain-client = { git = "https://github.com/logos-blockchain/logos-blockchain-module", rev = "<sha>" }
logos-rust-sdk = { git = "https://github.com/logos-co/logos-rust-sdk", rev = "b572e8172ca512b41c102d555f00258b859629c0" }  # same as the client's pin
```

```rust
use logos_blockchain_client::{BlockchainModuleClient, StreamItem};

let chain = BlockchainModuleClient::new();
let info = chain.cryptarchia_info()?;          // types::CryptarchiaInfo
let block = chain.block(info.tip)?;            // Option<types::ApiBlock>
let raw = chain.get_cryptarchia_info()?;       // serde_json::Value, the generated layer

let mut sub = BlockchainModuleClient::new();
for ev in sub.on_processed_block()? {
    match BlockchainModuleClient::processed_block_item(&ev)? {
        StreamItem::Item(e) => println!("{:?}", e.block.header.id),
        StreamItem::End => break,
    }
}
```

Replies decode into the node's own types (`logos-blockchain-core` and friends, pinned to the
node rev in `flake.lock`); the `get_*` methods from `generated.rs` return the raw reply.

## Example

[`example-module/`](example-module/) is a module that polls the chain height every 5 s. From the repo root:

```bash
nix build .#rust-client-example-lgx --out-link example-lgx
lgpm --modules-dir ./modules install --dir ./example-lgx
logoscore daemon --modules-dir ./modules --persistence-path ./data
logoscore load-module blockchain_client_example        # loads blockchain_module too
logoscore call blockchain_module start <user_config.yaml> ""
logoscore call blockchain_client_example last_height
```

Without nix: `cargo build --release --manifest-path example-module/rust-lib/Cargo.toml` (needs a C/C++
toolchain, `pkg-config`, `cmake`, `clang`).

[`example-cargo/`](example-cargo/) is the cargo-only shape of a consumer: a library using the
client, built and tested with `cargo test --manifest-path example-cargo/Cargo.toml`.

## Regenerating

```bash
nix run .#generate
```

Rewrites `blockchain_module.lidl`, `src/generated.rs` and `example-module/rust-lib/src/provider_gen.rs`.
Run it whenever `src/logos_blockchain_module.h` or the builder changes, and do not edit those
files by hand: CI regenerates them and fails if the committed copies differ.

`src/lib.rs` and `src/types.rs` are written by hand on top of the generated layer. A method
added to the module shows up in `generated.rs` only; give it a typed wrapper there.

## Pins

```bash
rust-client/check-pins.sh
```

The Cargo manifests must pin the node and `logos-rust-sdk` at the revisions `flake.lock` has:
the node's types are what the module's replies are decoded into, and `logos-rust-sdk` is what
the generated code is written against. CI runs the check. After bumping the node or the
builder in `flake.nix`, bump the manifests, refresh the three `Cargo.lock` files and regenerate.

## Tests

```bash
cargo test                      # in rust-client
```

`tests/module_shapes.rs` decodes sample module replies (`tests/fixtures/`) into the node's
types. When a node bump changes a reply's shape, these fail; update the fixture to what the
module now sends and fix the typed layer.
