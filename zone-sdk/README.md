# logos-blockchain-zone-sdk-module-backend

Zone SDK `adapter::Node` backend that reads Bedrock through `blockchain_module` via
[`logos-blockchain-client`](../rust-client), a drop-in for `adapter::NodeHttpClient`:

```rust
let node = NodeModuleClient::new();   // "blockchain_module"
let mut sequencer = ZoneSequencer::init(channel_id, signing_key, node, funding, None);
```
Runs inside a Logos module loaded next to `blockchain_module`.
