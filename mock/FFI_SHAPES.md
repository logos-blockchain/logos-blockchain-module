# liblogos_blockchain FFI: exact shapes and semantics (for the mock)

Source: logos-blockchain @ `2c65264` ("genesis ceremony for devnet version 0.3.0-rc.5").
All paths below are relative to that repo root (github.com/logos-blockchain/logos-blockchain) unless prefixed with `module:`
(= `/workspace/repos/logos-blockchain-module`). `cb/` = `c-bindings/src/`.

---

## 0. Conventions that apply everywhere

### 0.1 Return envelopes
- `OperationStatus { code, message }` - `message` is a Rust `CString` (free with `free_cstring`), NULL on OK (`cb/errors.rs:4-56`).
- `FfiResult<V> { value, error }` - on error `value` is `V::default()` (null pointer / zeroed struct) (`cb/result.rs:7-44`).
- Codes (`cb/errors.rs:6-20`): `Ok=0 NotFound=1 NullPointer=2 RelayError=3 ChannelSendError=4 ChannelReceiveError=5 ServiceError=6 RuntimeError=7 DynError=8 InitializationError=9 ShutdownError=0xA ConfigurationError=0xB ValidationError=0xC`.
  - The stub header (`module:tests/stubs/logos_blockchain.h:36`) calls 0xA `StopError`. The real cbindgen header (no prefix config, `c-bindings/cbindgen.toml`) emits bare variant names.
- A NULL `node` (or any other required pointer) returns `NullPointer` with the message ``Received a null `node` pointer.`` (`cb/macros.rs:13-25`).
- Message strings are human-readable `format!` output. The module forwards them verbatim as the `StdLogosResult` error (`module:src/logos_blockchain_module.cpp:25-55`).

### 0.2 Every node call blocks the calling thread
Every node-touching function is `runtime.block_on(async {...})` on the node's tokio runtime (e.g. `cb/storage.rs:36-38`).
- **No timeouts anywhere**, apart from the 100 ms wallet-ready probe in `get_claimable_vouchers` (`cb/api/wallet.rs:270-292`). A wedged service hangs the caller forever.
- **Calling any of them from a tokio worker thread panics** ("Cannot start a runtime from within a runtime"). Stream callbacks run on tokio worker threads (§1), so calling the FFI from inside a callback panics. The module header warns about this (`module:src/logos_blockchain_module.h:31-35`).
- `CryptarchiaServiceApi::from_overwatch_handle` does `.expect` (`services/chain/chain-service/src/api.rs:106`), and `WalletApi::from_overwatch_handle` does `.unwrap()` (`services/wallet/src/api.rs:112`). Both panic across the FFI boundary if the relay is gone (e.g. after shutdown).

### 0.3 Byte and value encodings (serde_json is human-readable, so the hex branches apply)

| Rust type | JSON encoding | Source |
|---|---|---|
| `HeaderId`, `ContentId`, `TxHash`, `ChannelId`, `MsgId`, `DeclarationId` (32 B) | lowercase hex, **no `0x`**, 64 chars. Deserialize accepts an optional `0x`. | `utils/src/lib.rs:19-50`, `core/src/utils/mod.rs:17-46` |
| Field elements via `serde_fr`: `NoteId`, `ZkPublicKey`, `VoucherCm`, `VoucherNullifier`, `PowNullifier`, `RewardsRoot`, `entropy_contribution`, `epoch_nonce`, `zk_id` | hex of the **32 little-endian bytes** of the canonical integer, no `0x`. Values >= the BN254 modulus are rejected on input, so the most significant (last) byte must be <= `0x30`. Fr(1) = `"01"+"00"*31`. | `zk/groth16/src/serde.rs:1-22`, `zk/groth16/src/lib.rs:47-53` |
| bare `Hash = [u8;32]` (`Utxo.op_id`, `TxEvent.op_id`, `ClaimPowRewardOp.block_hash`) | **JSON array of 32 integers** | `core/src/crypto.rs:4`, `core/src/mantle/ledger.rs:491-495`, `core/src/mantle/ops/pow.rs:88` |
| `Value` (u64), `Slot` (u64), `Epoch` (u32), `Nonce`, gas newtypes, `height` | JSON **numbers** (never strings). Note that u64 values such as the faucet's `18446744073709551615` exceed 2^53. | `core/src/mantle/ledger.rs:89`, `consensus/cryptarchia-engine/src/time.rs:30,39`, `core/src/mantle/gas.rs:14,43,72` |
| Ed25519 public key / `ProviderId` | hex, 64 chars | `kms/keys/src/keys/ed25519/public.rs:14-23` |
| Ed25519 signature | hex, 128 chars | `kms/keys/src/keys/ed25519/signature.rs:14-20` |
| `ZkSignature` | `{"pi_a": hex64, "pi_b": hex128, "pi_c": hex64}` | `kms/keys/src/keys/zk/signature.rs:13-26,69-109` |
| Leader proof (PoL) / claim proof (PoC) | hex, 256 chars (128 B compressed) | `core/src/proofs/leader_proof.rs:352-362`, `core/src/proofs/leader_claim_proof.rs:172-181` |
| `Version` | the string `"Bedrock"` | `core/src/header/mod.rs:115-127` |
| `InscriptionOp.inscription` | hex string | `core/src/mantle/ops/channel/inscribe.rs:38-48` |
| `DepositOp.metadata` | **array of integers** | `core/src/mantle/ops/channel/deposit.rs:31` |
| `ServiceType::BlendNetwork` | `"BN"` | `core/src/sdp/mod.rs:274-277` |
| libp2p `PeerId` | base58 string `"12D3KooW..."` | libp2p serde |
| enums in general | serde default **externally tagged** (`{"Variant": {...}}`), no `rename_all` | per type, below |

FFI byte arrays (`HeaderId`, `TxHash`, `NoteId`, public keys in structs) are the **same 32 bytes** the JSON hex encodes. For Fr types that is the LE encoding (`fr_to_bytes`, e.g. `cb/api/wallet.rs:587-590`). The module hex-encodes them lowercase without `0x` (`module:...cpp:131-136`), so the module's hex equals the JSON hex.

### 0.4 Hashes
- **Tx hash** = Blake2b-256(`"MANTLE_TXHASH_V1"` ‖ canonical binary encoding of the ops). The encoding is the op count, then `opcode‖payload` for each op. **Proofs are excluded** (`core/src/mantle/transactions/tx_list/op_refs.rs:40-49`, `.../tx_list/hash.rs:6-9`). For a mock, any 32 random bytes are fine, but they must be stable per tx.
- **Header id** = Blake2b-256(`"BLOCK_ID_V1"` ‖ version byte ‖ parent ‖ slot u64 LE ‖ body_root ‖ voucher_cm ‖ entropy (Fr LE) ‖ proof (128 B) ‖ leader_key) (`core/src/header/mod.rs:193-210`). `body_root` = Blake2b(`"BODY_ROOT_V1"` ‖ uncles ‖ tx merkle root) (`core/src/block/mod.rs:381-390`).
- **Note id** = Fr hash (`NOTE_ID_V1`, op_id, output_index, note) (`core/src/mantle/ledger.rs:497+`).
- **DeclarationId** = Blake2b-256(`"BN"` ‖ provider_id ‖ fr_to_bytes(zk_id) ‖ encoded locators) (`core/src/sdp/mod.rs:512-531`).

---

## 1. Stream callbacks

All three take `void (*cb)(const char*)` and return an `OperationStatus` saying whether the subscription was set up. On error the callback is never called.
- Each call does `block_on` to subscribe, then `runtime.spawn`s a task that invokes the callback. **Callbacks therefore run on a tokio worker thread of the node runtime**, not the caller's thread.
- The `char*` is only valid during the call: the Rust `CString` is dropped right after (`cb/api/subscriptions.rs:131-138`).
- No user-data pointer is passed. The module uses a static `s_instance` (`module:src/logos_blockchain_module.h:307-315`).

### 1.1 `subscribe_to_new_blocks` (deprecated) - `cb/api/subscriptions.rs:57-160`
- **Source:** chain service `subscribe_new_blocks()` is a tokio `broadcast` receiver. For each event the block is loaded from storage and re-serialized as core `Block<TxWithId>`, where `TxWithId = {id: TxHash, #[serde(flatten)] SignedOps}` (`:31-37`, `:85-102`, `cb/api/types/block.rs:16-27`).
- **Coverage:** fires for **every processed block, including forks** (not canonical-only) (`services/chain/chain-service/src/service/mod.rs:820-833`).
- **End of stream:** `while let Ok(event) = block_stream.recv()` ends on the first error, **including `RecvError::Lagged`**, i.e. a slow consumer kills the stream. After that the callback gets `NULL` exactly once (`:85`, `:111-116`).
- **Missing block:** if the block is not in storage it is logged and skipped.
- **Shape:** same as `get_block` (§2.1), except every tx has a leading `"id"`:
```json
{"header":{"version":"Bedrock","parent_block":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4","slot":689412,
  "body_root":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6",
  "proof_of_leadership":{"proof":"bbb88bea9127dc82cb879e7e3766b36b9a724d7ae57699f2bf997dc3add836a409bfba579d0fe2a109c0af88050ffeae752be223c0a783ebec6ce768cd7cd443a464fc5099bea6b9086fea654dcb8306fbae601c8465498dc76a3d9160e6416c12285dc441471704c6032aa54fa15c77a676004c1d25b11257bdda1cad38d161",
    "entropy_contribution":"ac426b92c1ddd0f98717ab61925aed75f7676526738992275b67682c9aafa90b",
    "leader_key":"a741dfbcffd8c8c76843a96e4d509d61c5da0001d434a1c0746e7438401ff7df",
    "voucher_cm":"5e0d1c2b3a49586776859463a2b1c0dfeefd0c1b2a39485766758493a2b1c00f"}},
 "signature":"944cb27ca30c7085b157374d5721ca0fdd03bda0cb7ab817917ae41ef12555b475c5504eb646cdc4911f100b813b07c054c83782b8f2269bd4fb91a77200dfd1",
 "uncle_headers":[],
 "transactions":[
  {"id":"9f3c1e0a7b2d4c6e8f00112233445566778899aabbccddeeff0011223344aa55",
   "mantle_tx":{"ops":[{"opcode":0,"payload":{
     "inputs":["3a5f0e1d2c3b4a59687766554433221100ffeeddccbbaa99887766554433220b"],
     "outputs":[{"value":1000,"pk":"6d1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e50a"},
                {"value":4249,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}}]},
   "ops_proofs":[{"ZkSig":{"pi_a":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4",
     "pi_b":"766866c9e4092606130b0923be06f96955ca161bbb3443cb95d02a7c5a375270a35998f1cd3b78fe6925ce80bcee53d01663d9bbb6d47a39f037cbe11f0c59c0",
     "pi_c":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6"}}]}]}
```
- **No header id anywhere**, so consumers cannot get a block's own id from this stream (they must take it from the next block's `parent_block`).
- **Module post-processing** (`module:...cpp:480-498`): it emits `newBlock` with `{"block": "<the JSON above as a STRING>"}`, i.e. **double-encoded**. On NULL it emits the literal `null` and clears `is_new_blocks_subscribed`. It also `fprintf`s every block to stderr.

### 1.2 `subscribe_to_processed_blocks` - `cb/api/subscriptions.rs:162-242`
- **Source:** `lb_api_service::http::mantle::get_new_blocks_stream` → `ApiProcessedBlockEventOwned` (`nodes/node/binary/src/api/serializers/blocks.rs:86-155`, `.../serializers/transactions.rs:9-31`). It is the same schema as HTTP `/cryptarchia/blocks/stream`.
- **Lag:** errors and lag are **silently skipped** (`filter_map(event.ok()?)`, `services/api/src/http/mantle.rs:250-263`), and blocks missing from storage are skipped too. The stream only ends (NULL once, `:221`) when the underlying stream ends, e.g. at shutdown.
- **Coverage:** also fires for fork blocks. `tip`/`lib` are the chain state **after** processing this block.
- **Shape** (struct field order):
```json
{"block":{
   "header":{"id":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281",
             "parent_block":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4",
             "slot":689412,
             "body_root":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6",
             "proof_of_leadership":{"proof":"bbb8...(256 hex)","entropy_contribution":"ac42...0b","leader_key":"a741...f7df","voucher_cm":"5e0d...c00f"}},
   "uncle_headers":[],
   "transactions":[
     {"mantle_tx":{"hash":"9f3c1e0a7b2d4c6e8f00112233445566778899aabbccddeeff0011223344aa55",
                   "ops":[{"opcode":0,"payload":{"inputs":["3a5f...220b"],"outputs":[{"value":1000,"pk":"6d1e...e50a"},{"value":4249,"pk":"41fe...b60c"}]}}]},
      "ops_proofs":[{"ZkSig":{"pi_a":"...64","pi_b":"...128","pi_c":"...64"}}]}]},
 "tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281",
 "tip_slot":689412,
 "lib":"0b7d2e9f4a1c3e5d7f9b0a2c4e6d8f1a3b5c7d9e0f2a4b6c8d0e1f3a5b7c9d0e",
 "lib_slot":685801}
```
- **Differences from the core shape:**
  - `header.id` is present.
  - There is **no `version`** and **no block `signature`**.
  - Uncle headers are `{"header": <API header with id>, "signature": hex128}`.
  - The tx hash lives at `mantle_tx.hash`.
- **Module post-processing:** emits `processedBlock(<raw string>)` unchanged. NULL becomes `"null"` and clears `is_processed_blocks_subscribed` (`module:...cpp:504-515`).

### 1.3 `subscribe_to_lib_blocks` - `cb/api/subscriptions.rs:244-315`
- **Source:** `BlockBroadcastMsg::SubscribeToFinalizedBlocks` wrapped in a `BroadcastStream` (`services/api/src/http/mantle.rs:171-193`).
- **Item:** `BlockInfo {height: u64, header_id: HeaderId}` (`services/chain/broadcast-service/src/lib.rs:22-26`).
  ```json
  {"height":22984,"header_id":"0b7d2e9f4a1c3e5d7f9b0a2c4e6d8f1a3b5c7d9e0f2a4b6c8d0e1f3a5b7c9d0e"}
  ```
- **Frequency:** **one event per LIB change, not per finalized block.** If LIB jumps N blocks you only get the new LIB (`services/chain/chain-service/src/service/mod.rs:836-857`). When the node switches Bootstrapping→Online, LIB jumps from genesis to tip−k in a single event (§4.1).
- **End of stream:** the first stream error (including broadcast `Lagged`) is logged, the loop `break`s, and then NULL is delivered once (`:263-279`).
- **Module post-processing:** emits `libBlock(<raw>)`. NULL becomes `"null"` (`module:...cpp:517-528`).

### 1.4 Module subscription behaviour
`start()` calls `start_lb_node` and then subscribes **all three** streams in order. A failure stops it and returns the error **with the node left running** (`module:...cpp:625-669`). Re-subscribing while subscribed is refused by the module (`:58-73`), not by the FFI. The FFI itself happily creates multiple subscriptions.

---

## 2. String-returning functions (caller frees with `free_cstring`)

### 2.1 `get_block(node, const HeaderId*)` - `cb/api/storage.rs:31-117`
- **Returns** `Option<Block<SignedOps<Unverified,StandardMode>>>` straight from storage (`services/api/src/http/mantle.rs:691-713`), serialized in the **core** shape (`core/src/block/mod.rs:103-110`; header `core/src/header/mod.rs:166-173`; leader proof `core/src/proofs/leader_proof.rs:27-37`; uncle `core/src/block/uncle.rs:64-68`; tx `core/src/mantle/transactions/tx_list/signed_ops.rs:327-356`).
  - Block: `{header, signature, uncle_headers (<=4), transactions (<=1024)}`.
  - Header: `{version, parent_block, slot, body_root, proof_of_leadership{proof, entropy_contribution, leader_key, voucher_cm}}`.
  - Tx: `{"mantle_tx":{"ops":[...]},"ops_proofs":[...]}`. **No hash, no header id, and no gas or fee fields** (fees are implicit, see §5.4).
- **Lookup:** any stored block, canonical or not, is found.
- **Errors:**
  - Not found (or a stored block that fails decode/verification): `NotFound` "No block found for header id [..]". The id is printed as a Debug byte array.
  - Relay failure: `RelayError` "Failed to get block: ..".
- **Example:** identical to §1.1 without the `"id"` key in transactions.
```json
{"header":{"version":"Bedrock","parent_block":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4","slot":689412,"body_root":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6","proof_of_leadership":{"proof":"bbb88bea9127dc82cb879e7e3766b36b9a724d7ae57699f2bf997dc3add836a409bfba579d0fe2a109c0af88050ffeae752be223c0a783ebec6ce768cd7cd443a464fc5099bea6b9086fea654dcb8306fbae601c8465498dc76a3d9160e6416c12285dc441471704c6032aa54fa15c77a676004c1d25b11257bdda1cad38d161","entropy_contribution":"ac426b92c1ddd0f98717ab61925aed75f7676526738992275b67682c9aafa90b","leader_key":"a741dfbcffd8c8c76843a96e4d509d61c5da0001d434a1c0746e7438401ff7df","voucher_cm":"5e0d1c2b3a49586776859463a2b1c0dfeefd0c1b2a39485766758493a2b1c00f"}},"signature":"944cb27ca30c7085b157374d5721ca0fdd03bda0cb7ab817917ae41ef12555b475c5504eb646cdc4911f100b813b07c054c83782b8f2269bd4fb91a77200dfd1","uncle_headers":[],"transactions":[{"mantle_tx":{"ops":[{"opcode":0,"payload":{"inputs":["3a5f0e1d2c3b4a59687766554433221100ffeeddccbbaa99887766554433220b"],"outputs":[{"value":1000,"pk":"6d1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e50a"},{"value":4249,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}}]},"ops_proofs":[{"ZkSig":{"pi_a":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4","pi_b":"766866c9e4092606130b0923be06f96955ca161bbb3443cb95d02a7c5a375270a35998f1cd3b78fe6925ce80bcee53d01663d9bbb6d47a39f037cbe11f0c59c0","pi_c":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6"}}]},{"mantle_tx":{"ops":[{"opcode":48,"payload":{"rewards_root":"0d4c3b2a19087f6e5d4c3b2a19087f6e5d4c3b2a19087f6e5d4c3b2a19087f0e","voucher_nullifier":"1d2c3b4a5968778695a4b3c2d1e0f00112233445566778899aabbccddeeff00a","pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}},{"opcode":0,"payload":{"inputs":["7b6a5948372615049382716051403928170615049382716051403928170615a0"],"outputs":[{"value":9999390,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}}]},"ops_proofs":[{"PoC":{"proof":"aa11...(256 hex)"}},{"ZkSig":{"pi_a":"...","pi_b":"...","pi_c":"..."}}]}]}
```
- **Module post-processing:** passes the string through unchanged (`module:...cpp:1754-1775`).

### 2.2 `get_blocks(node, from_slot, to_slot)` - `cb/api/storage.rs:224-327`
- **Returns** a JSON **array** of core-shape blocks (§2.1) from `get_immutable_blocks` (`services/api/src/http/mantle.rs:597-675`).
- **Semantics:**
  - Only **immutable (≤ LIB) canonical** blocks, read from the immutable slot index.
  - Both ends inclusive, ascending by slot.
  - `to_slot` is silently clamped to `lib_slot`, so the LIB block itself is included.
  - `from_slot > lib_slot` returns `[]`, not an error.
  - Slots without a block are just absent. Indexed blocks with a missing body are skipped with a warning (`:315-322`).
  - No max-range cap.
- **Errors:**
  - `to < from`: an error from the service, reported as `RelayError` "Failed to get blocks: ..".
  - Values over usize: `ValidationError` "from_slot overflow." / "to_slot overflow." (`cb/api/storage.rs:304-316`).
- **During Bootstrapping LIB = genesis**, so `get_blocks` returns at most the genesis block.
- **Example** (2 blocks):
```json
[{"header":{"version":"Bedrock","parent_block":"ff01...","slot":685790,"body_root":"...","proof_of_leadership":{...}},"signature":"...","uncle_headers":[],"transactions":[]},
 {"header":{"version":"Bedrock","parent_block":"<id of previous block>","slot":685801,"body_root":"...","proof_of_leadership":{...}},"signature":"...","uncle_headers":[],"transactions":[]}]
```
- **Module post-processing:** pass-through (`module:...cpp:1777-1793`).

### 2.3 `get_transaction(node, const TxHash*)` - `cb/api/storage.rs:119-215`
- **Returns** a single core-shape `SignedOps`: `{"mantle_tx":{"ops":[...]},"ops_proofs":[...]}`. **No hash, no block reference, no inclusion status** (`services/api/src/http/mantle.rs:750-764`).
- **Coverage:** the storage key `TxHash` is **only written by the mempool** (`services/tx-service/src/storage/adapters/rocksdb.rs:45-50`).
  - Found: txs currently in this node's mempool (local or gossiped), plus included ones for ~10 min after mempool removal (`REMOVED_ITEM_GRACE_PERIOD`, pruned lazily on the next canonical block; `services/tx-service/src/backend/pool.rs:23,207-224,360-381`).
  - Older txs, or txs only seen inside synced blocks, give `NotFound`. **It is not a chain index.**
- **Errors:** `NotFound` "No transaction found for hash ..", `RuntimeError` "Failed to get transaction: ..".
- **Example:**
```json
{"mantle_tx":{"ops":[{"opcode":0,"payload":{"inputs":["3a5f0e1d2c3b4a59687766554433221100ffeeddccbbaa99887766554433220b"],"outputs":[{"value":1000,"pk":"6d1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e50a"},{"value":4249,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}}]},"ops_proofs":[{"ZkSig":{"pi_a":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4","pi_b":"766866c9e4092606130b0923be06f96955ca161bbb3443cb95d02a7c5a375270a35998f1cd3b78fe6925ce80bcee53d01663d9bbb6d47a39f037cbe11f0c59c0","pi_c":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6"}}]}
```

### 2.4 Op and OpProof catalogue (used by every block, tx and fund shape)

**Op:** untagged enum over `{"opcode": <u8 number>, "payload": {...}}` (`core/src/mantle/ops/internal.rs:20-34`, `core/src/mantle/ops/serde_.rs:28-32`). On input, the opcode must match exactly.

**OpProof:** **externally tagged** (`core/src/mantle/ops/op_proof_ref.rs:15-23`). `ops_proofs[i]` pairs with `ops[i]`.

| Op | opcode | payload | required proof |
|---|---|---|---|
| Transfer | 0 | `{"inputs":[NoteId Fr-hex], "outputs":[{"value":u64,"pk":Fr-hex}]}` (`core/src/mantle/ops/transfer.rs:32-36`, `core/src/mantle/ledger.rs:478-481`) | `{"ZkSig":{pi_a,pi_b,pi_c}}` |
| ChannelConfig | 16 | `{"channel":hex,"parent":hex,"keys":[ed-hex],"posting_timeframe":u32,"posting_timeout":u32,"configuration_threshold":u16,"transfer_threshold":u16}` (`.../channel/config.rs`) | `{"ChannelMultiSigProof":{"signatures":[{"signature":hex128,"channel_key_index":u16}]}}` |
| ChannelInscribe | 17 | `{"channel_id":hex,"inscription":hex,"parent":MsgId hex,"signer":ed-hex}` (`.../channel/inscribe.rs:60-73`) | `{"Ed25519Sig":hex128}` |
| ChannelDeposit | 18 | `{"channel_id":hex,"inputs":[NoteId],"metadata":[ints]}` (`.../channel/deposit.rs:33-38`) | ZkSig |
| ChannelWithdraw | 19 | `{"channel_id","inputs"}` (`.../channel/withdraw.rs:36-40`) | ChannelMultiSigProof |
| ChannelTransfer | 20 | `{"channel_id","inputs","outputs"}` (`.../channel/channel_transfer.rs`) | ChannelMultiSigProof |
| SDPDeclare | 32 | `{"service_type":"BN","locators":["/ip4/../udp/../quic-v1"] (1-8),"provider_id":ed-hex,"zk_id":Fr-hex,"service_note_id":Fr-hex}` (`core/src/sdp/mod.rs:503-510`) | `{"ZkAndEd25519Sigs":{"zk_sig":{..},"ed25519_sig":hex128}}` |
| SDPWithdraw | 33 | `{"declaration_id":hex,"nonce":u64,"service_note_id":Fr-hex}` (`core/src/sdp/mod.rs:564-568`) | ZkSig |
| SDPActive | 34 | `{"declaration_id","nonce","metadata":{"Blend":{"epoch","signing_key","proof_of_quota":{"key_nullifier":Fr-hex,"proof":{"pi_a":[ints],"pi_b":[ints],"pi_c":[ints]}},"proof_of_selection":{"selection_randomness":..}}}}` (`core/src/sdp/mod.rs:571-581`, `core/src/sdp/blend.rs:27-33`) | ZkSig |
| LeaderClaim | 48 | `{"rewards_root":Fr-hex,"voucher_nullifier":Fr-hex,"pk":Fr-hex}` (`.../leader_claim.rs:59-64`) | `{"PoC":{"proof":hex256}}` |
| ClaimPowReward | 64 | `{"epoch_nonce":Fr-hex,"block_hash":[32 ints],"public_key":Fr-hex}` (`.../pow.rs:81-91`) | `{"None":null}` |

---

### 2.5 `get_block_events(node, const HeaderId*)` - `cb/api/cryptarchia.rs:162-266`
- **Returns** `Events(Vec<Event>)`, a newtype, so the output is a **top-level JSON array** (`core/src/events/mod.rs:23-24`). Every enum is externally tagged.
- **Top level:** `Event = {"Header": HeaderEvent} | {"Tx": TxEvent}` (`core/src/events/mod.rs:64-70`).
- **`TxEvent`** = `{"tx_hash": hex, "op_id": [32 ints], "payload": TxEventPayload}` (`:85-90`).
- **`TxEventPayload`** (`:121-141`). These are **all** the variants:
  - `{"Deposit": {"channel_id": hex, "amount": u64, "metadata": [ints], "notes": [{"note_id": Fr-hex, "value": u64, "pk": Fr-hex}]}}`
    - Emitted in `core/src/mantle/ops/channel/deposit.rs:165-195`.
    - `notes` are the channel notes re-created from the inputs; `pk` = depositor.
  - `{"LeaderRewardClaimed": {"voucher_nullifier": Fr-hex, "utxo": Utxo}}`
    - Emitted in `core/src/mantle/ops/leader_claim.rs:283-311`.
    - `utxo = {"op_id": <claim op_id, 32 ints>, "output_index": 0, "note": {"value": reward_amount, "pk": LeaderClaimOp.pk}}`.
    - The value is the **gross** per-voucher reward; the claim fee is not in it.
  - `{"PoWRewardClaimed": {"pow_nullifier": Fr-hex, "utxo": Utxo}}`
    - Emitted in `core/src/mantle/ops/pow.rs:330-370`.
    - `note.value` = `epoch_reward` (25,000,000 on devnet); `note.pk` = the ticket's `public_key`. This is NOT the claim address (§4.4).
- **`HeaderEvent`** (`:144-158`):
  - `{"SdpNoteUnlocked": {"note_id": Fr-hex, "service_type": "BN", "declaration_id": hex}}`
    - Emitted in `ledger/src/mantle/sdp/mod.rs:225-265` once `epoch > withdraw_at`.
  - `{"SdpRewardDistributed": {"service_type": "BN", "utxo": Utxo}}`
    - Emitted at epoch rollover in `ledger/src/mantle/sdp/mod.rs:185-210`.
    - `op_id` = `create_reward_op_id(epoch, service)`; `note.pk` = the provider's `zk_id`.
- **`Utxo`** = `{"op_id": [32 ints], "output_index": usize, "note": {"value": u64, "pk": Fr-hex}}` (`core/src/mantle/ledger.rs:490-495`). There are no serde attributes, so `op_id` is a number array.
- **No channel inscribe/config/withdraw/transfer events exist, and no Transfer event either.**
- **Order:** header events first, then tx events in tx/op order (`ledger/src/lib.rs:269-302`).
- **Storage:** stored per block in the same batch as the block (`services/storage/src/rocksdb/mod.rs:120-145`). A block with no events gives `[]`.
- **Errors:**
  - Unknown/pruned block: `NotFound` "No block found for header id ..".
  - Relay failure: `ServiceError` "Failed to get block events: ..".
- **Example:**
```json
[{"Header":{"SdpRewardDistributed":{"service_type":"BN","utxo":{"op_id":[12,34,56,78,90,11,22,33,44,55,66,77,88,99,100,111,122,133,144,155,166,177,188,199,200,211,222,233,244,255,1,7],"output_index":0,"note":{"value":1520,"pk":"5a1f0e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d20e"}}}}},
 {"Header":{"SdpNoteUnlocked":{"note_id":"0b9e8d7c6b5a49382716051423324150607f8e9dacbbcad9e8f70615243320a","service_type":"BN","declaration_id":"7c4d3e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d"}}},
 {"Tx":{"tx_hash":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","op_id":[201,4,17,99,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,88],"payload":{"LeaderRewardClaimed":{"voucher_nullifier":"1d2c3b4a5968778695a4b3c2d1e0f00112233445566778899aabbccddeeff00a","utxo":{"op_id":[201,4,17,99,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,88],"output_index":0,"note":{"value":4200,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}}}}}},
 {"Tx":{"tx_hash":"aa01bb02cc03dd04ee05ff060718293a4b5c6d7e8f90a1b2c3d4e5f607182930","op_id":[3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,2,3,8,4,6,2,6,4,3,3,8,3,2,7,9,5],"payload":{"PoWRewardClaimed":{"pow_nullifier":"44e0112233445566778899aabbccddeeff00112233445566778899aabbccdd0e","utxo":{"op_id":[3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,2,3,8,4,6,2,6,4,3,3,8,3,2,7,9,5],"output_index":0,"note":{"value":25000000,"pk":"c0de0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e05"}}}}}},
 {"Tx":{"tx_hash":"bb02cc03dd04ee05ff060718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c","op_id":[9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9],"payload":{"Deposit":{"channel_id":"00ff112233445566778899aabbccddeeff00112233445566778899aabbccddee","amount":5000,"metadata":[1,2,3],"notes":[{"note_id":"6e0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d70f","value":5000,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}}}}]
```
- **Module post-processing:** pass-through (`module:...cpp:1855-1880`).

### 2.6 `get_channel_state(node, const uint8_t* channel_id /*32B*/)` - `cb/api/channel.rs:28-120`
- **Source:** the ledger at the **current tip** (`services/api/src/http/mantle.rs:74-88`). Schema is `ChannelState` (`core/src/mantle/channel.rs:109-135`).
- **Fields:**
  - `accredited_keys`: [ed-hex], non-empty
  - `configuration_threshold`: u16
  - `tip_message`: MsgId hex, all-zero = root
  - `config_tip_hash`: hex
  - `tip_slot`: u64
  - `tip_sequencer`: u16 index into `accredited_keys`
  - `tip_sequencer_starting_slot`: u64
  - `posting_timeframe`: u32 (0 = infinite)
  - `posting_timeout`: u32 (0 = none)
  - `transfer_threshold`: u16
- **There is no balance field** (channel notes are not exposed).
- **Errors:**
  - Unknown channel: `NotFound` "No channel found for id ..".
  - Service failure: `ServiceError` "Failed to get channel state: ..".
- **Example:**
```json
{"accredited_keys":["3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29","d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"],"configuration_threshold":1,"tip_message":"9a0c1d2e3f405162738495a6b7c8d9eafb0c1d2e3f405162738495a6b7c8d9ea","config_tip_hash":"0000000000000000000000000000000000000000000000000000000000000000","tip_slot":689300,"tip_sequencer":0,"tip_sequencer_starting_slot":689000,"posting_timeframe":0,"posting_timeout":0,"transfer_threshold":1}
```
- **Module post-processing:** pass-through (`module:...cpp:1574-1597`).

### 2.7 `blend_info(node)` - `cb/api/blend.rs:118-189`
- **Shape:** serializes `Option<NetworkInfo<PeerId>>`, where `NetworkInfo = {"node_id": PeerId, "core_info": null | {"current_epoch_peers": [[PeerId, bool healthy], ...], "old_epoch_peers": null | [PeerId]}}` (`services/blend/src/message.rs:15-28`).
- **By mode:**
  - Core node: `core_info` is populated (`services/blend/src/core/backends/libp2p/swarm.rs:491-508`).
  - Edge/broadcast node: `{"node_id":..., "core_info": null}`.
  - Top-level `null` only if the core swarm channel fails (`services/blend/src/core/backends/libp2p/mod.rs:155-167`).
- **Errors:** relay failure gives `RelayError` "Failed to get blend info: ..".
- **Examples:**
```json
{"node_id":"12D3KooWQ9vB1ccbN3Jx5qXcS8cVZLbqGq2yYw1Hkz7K2nYfVQ1e","core_info":{"current_epoch_peers":[["12D3KooWAo3Zr6mJb5kX8d2tYq1HcVb7uN4sLp9eR3fW6gT2yKxM",true],["12D3KooWBx7nL2pQ9rS4tU6vW8yZ1aC3dE5fG7hJ9kM2nP4qR6sT",false]],"old_epoch_peers":null}}
```
```json
{"node_id":"12D3KooWQ9vB1ccbN3Jx5qXcS8cVZLbqGq2yYw1Hkz7K2nYfVQ1e","core_info":null}
```
- **Module post-processing:** pass-through (`module:...cpp:1694-1710`).

### 2.8 `wallet_fund_tx(node, const char* request_json)` - `cb/api/wallet.rs:1567-1740`
**Input `WalletFundRequestBody`** (`nodes/api-common/src/bodies/wallet.rs`, mod `fund`):

| field | type | required |
|---|---|---|
| `tip` | HeaderId hex or null | optional; null means current tip |
| `tx_builder` | `MantleTxBuilder` | **yes, all four sub-fields required** (`core/src/mantle/transactions/builder.rs:64-71`) |
| `tx_builder.mantle_tx` | `{"ops":[Op...]}` | yes |
| `tx_builder.ledger_inputs` | `[Utxo]` (§2.5 Utxo shape) | yes |
| `tx_builder.pending_transfer` | Transfer payload `{"inputs":[], "outputs":[]}`; `inputs` must equal the ids of `ledger_inputs` | yes |
| `tx_builder.channel_multi_sig_proofs` | object, key = op index as a string, value = `{"signatures":[...]}` | yes |
| `change_public_key` | Fr-hex | yes |
| `funding_public_keys` | [Fr-hex] | yes |
| `max_tx_fee` | u64 | yes |
| `priority_fee_percent` | u64 | optional, `#[serde(default)]` = 0 |

**Output `WalletFundResponseBody`:**
- `tip`: hex.
- `funded_tx`: `{"ops":[...]}`, with the fee-paying Transfer appended as the **last** op.
- `transfer_proof`: `{"ZkSig":{...}}` signed over `funded_tx`'s hash by the input owners, or `null` when no inputs were added.

The caller signs its own ops and then submits `{mantle_tx: funded_tx, ops_proofs: [...own proofs..., transfer_proof]}`.

**Errors:**
- Non-UTF-8 input or a parse failure: `ValidationError` "Request is not valid UTF-8: .." / "Failed to parse fund request: <serde msg>".
- Funding failure: `DynError` "Failed to fund tx: <WalletError>" (§5.4).
- Fee cap: `DynError` "tx_fee(X) exceeds max_tx_fee(Y)".
- Signing failure: `DynError` "Failed to sign fee transfer: ..".

**Notes:**
- It **does not submit**.
- Funded notes are **reserved** in the wallet until they are seen spent or `pending_note_expiry_blocks` (default 10) immutable blocks pass. The reservation holds even when the call fails the fee cap (`services/wallet/src/lib.rs:665-677`).

**Examples:**
```json
{"tip":null,"tx_builder":{"mantle_tx":{"ops":[{"opcode":17,"payload":{"channel_id":"00ff112233445566778899aabbccddeeff00112233445566778899aabbccddee","inscription":"68656c6c6f","parent":"0000000000000000000000000000000000000000000000000000000000000000","signer":"3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29"}}]},"ledger_inputs":[],"pending_transfer":{"inputs":[],"outputs":[]},"channel_multi_sig_proofs":{}},"change_public_key":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c","funding_public_keys":["41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"],"max_tx_fee":5000,"priority_fee_percent":0}
```
```json
{"tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281","funded_tx":{"ops":[{"opcode":17,"payload":{"channel_id":"00ff112233445566778899aabbccddeeff00112233445566778899aabbccddee","inscription":"68656c6c6f","parent":"0000000000000000000000000000000000000000000000000000000000000000","signer":"3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29"}},{"opcode":0,"payload":{"inputs":["6e0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d70f"],"outputs":[{"value":98765,"pk":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}}]},"transfer_proof":{"ZkSig":{"pi_a":"21b3b6e64dde10c41949c30139844c46d556bbc94794703bb870396d1358dec4","pi_b":"766866c9e4092606130b0923be06f96955ca161bbb3443cb95d02a7c5a375270a35998f1cd3b78fe6925ce80bcee53d01663d9bbb6d47a39f037cbe11f0c59c0","pi_c":"1a09520fd6696cef9bc9c9dad5cc1283964261983c9aef664d8d4b40f1a7aac6"}}}
```
- **Module post-processing:** pass-through (`module:...cpp:1633-1649`).

### 2.9 `get_chain_id(node)` - `cb/api/chain.rs:41-62`
- **Returns** a plain C string (not JSON), e.g. `0.3.0-rc.5` on devnet.
- It is captured at node construction, so there is **no service call and it never blocks** (`cb/node.rs:36-57`).
- **Error:** `RuntimeError` if the id contains NUL.
- **Module post-processing:** pass-through (`module:...cpp:1714-1730`).

### 2.10 Others returning strings
- `get_peer_id(config_path)`: a base58 PeerId, no node needed.
- `generate_key(...)`: the key id.
- `get_build_version_info()`: JSON.
- `merge_user_config(...)`: NULL means no conflicts.

These are config-time helpers (`cb/api/{peer,keys,version,config}.rs`). They are synchronous file I/O with `ConfigurationError`/`ValidationError`.

---

## 3. Hash-returning and struct-returning functions

The module renders each of these into JSON or hex. **The FFI never returns JSON for any of them.**

### 3.1 `submit_signed_transaction(node, const char* json) -> {Hash value}` - `cb/api/wallet.rs:1764-1816`
- **Input:** `{"mantle_tx":{"ops":[...]},"ops_proofs":[...]}`, i.e. `SignedOps<Unverified>` (`core/src/mantle/transactions/tx_list/signed_ops.rs:285-391`).
  - Proofs must be one per op, in order, of the type the op requires (§2.4).
  - It then goes through `preverify()`, which checks signatures.
- **Errors:**
  - Non-UTF-8, parse failure or preverify failure: `ValidationError` ("Transaction is not valid UTF-8: ..", "Failed to parse signed transaction: ..", "Failed to preverify signed transaction: ..").
  - Mempool rejection: `DynError` "Failed to add transaction to mempool: ..".
- **Returns** the tx hash (§0.4), computed before the submit.
- **Module:** returns lowercase hex (`module:...cpp:1653-1663`).

### 3.2 `get_cryptarchia_info(node) -> CryptarchiaInfo*` - `cb/api/cryptarchia.rs:61-160`
- **Fields** (from `services/chain/chain-service/src/lib.rs:366-377`):
  - `lib` / `lib_slot`: the last irreversible block and its slot.
  - `tip`: the canonical tip header id.
  - `slot`: **the tip block's slot**, not wall-clock (wall-clock is `get_time_info.current_slot`).
  - `height`: the tip branch length; genesis = 0 (`services/chain/chain-service/src/states.rs:105`).
  - `mode`: `Bootstrapping=0 | Online=1 | NotStarted=2`. NotStarted is reported whenever the service phase is `AwaitingGenesisTime`, regardless of engine state (`cb/api/cryptarchia.rs:15-32`).
- **Availability:** works in every phase, including Bootstrapping. It blocks only until the chain service has recovered from storage at startup (`services/chain/chain-service/src/lib.rs:707-763`).
- **Error:** `RelayError` "Failed to get cryptarchia info.".
- **Module JSON** (`module:...cpp:1820-1853`):
```json
{"lib":"0b7d2e9f4a1c3e5d7f9b0a2c4e6d8f1a3b5c7d9e0f2a4b6c8d0e1f3a5b7c9d0e","lib_slot":685801,"tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281","slot":689412,"height":23104,"mode":"Online"}
```

### 3.3 `get_time_info(node) -> TimeInfo*` - `cb/api/time.rs:13-121`
- **Fields:** `{slot_duration_ms u64, genesis_time_unix_ms i64, current_slot u64, current_epoch u32}`, taken from the time service's latest wall-clock tick (`services/time/src/lib.rs:169-195`).
  - slot = floor((now − genesis) / slot_duration), 0 before genesis (`consensus/cryptarchia-engine/src/time.rs`).
  - epoch = slot / epoch_length (§4.2).
- **Errors:** `RelayError`, `ChannelSendError`, `ChannelReceiveError`, `ServiceError`.
- **Module JSON** (`module:...cpp:1882-1905`), devnet at 2026-10-06T12:00Z:
```json
{"slot_duration_ms":1000,"genesis_time_unix_ms":1790598600000,"current_slot":689400,"current_epoch":19}
```

### 3.4 `get_network_info(node) -> NetworkInfo` (by value) - `cb/api/network.rs:9-94`
- **Fields:**
  - `n_peers`: size_t, connected peers.
  - `n_connections`: u32; one peer can have more than one connection.
  - `n_pending_connections`: u32.
  - `n_discovered_peers`: size_t, Kademlia peers whether connected or not.
- No free needed.
- **Error:** `RelayError` "Failed to get network info: ..".
- **Module JSON** (`module:...cpp:1734-1752`):
  ```json
  {"n_peers":4,"n_connections":5,"n_pending_connections":0,"n_discovered_peers":9}
  ```

### 3.5 `get_balance(node, const uint8_t* addr32, const HeaderId* optional_tip) -> u64` - `cb/api/wallet.rs:401-484`
- **Tip resolution:** a NULL tip first calls `get_cryptarchia_info` and uses its tip.
- **Errors:**
  - Address bytes that are not a valid Fr: `DynError` "Invalid wallet address: ..".
  - **`NotFound` "Unknown wallet address."** whenever the address has no entry in the wallet's per-key note index at that tip (`wallet/src/lib.rs:330-337`). That covers:
    - a key not in `wallet.known_keys` (never indexed, `wallet/src/lib.rs:519`);
    - **a known key that currently holds zero notes** (the entry is removed when its last note is spent, `wallet/src/lib.rs:633-641`).

    A fresh known key with no funds therefore gets NotFound, not 0.
  - Tip not reachable by the wallet: `DynError` "Failed to get balance: Ledger state corresponding to block .. not found".
- **Balance contents:** channel notes are excluded.
- **Blocking:** waits (unbounded) for the wallet service to be ready. It does **not** require Online.
- **Stub mismatch:** `module:tests/stubs/logos_blockchain.h:299` declares the third parameter as `const void* reserved`.
- **Module:** returns the decimal string, e.g. `"2968004000000000"` (`module:...cpp:1195-1212`). It always passes a NULL tip.

### 3.6 `get_wallet_notes(node, addr32, optional_tip) -> WalletNotes{tip, notes*, len}` - `cb/api/wallet.rs:502-605`
- **Contents:** same index and the same NotFound rule as `get_balance`. `tip` is the resolved tip.
- **Note fields:** `{id: NoteId (Fr LE bytes), value: u64}`.
- **Free:** with `free_wallet_notes`; it tolerates a NULL `notes` pointer.
- **Module JSON** (`module:...cpp:1311-1357`); values are **strings**:
```json
{"tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281","notes":[{"id":"6e0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d70f","value":"8000000000000"},{"id":"7b6a5948372615049382716051403928170615049382716051403928170615a0","value":"9999390"}]}
```

### 3.7 `get_leader_aged_notes(node, optional_tip) -> LeaderAgedNotes{tip, notes*, len, total_value}` - `cb/api/wallet.rs:622-714`, `cb/api/types/leader_aged_notes.rs`
- **Eligible notes:** wallet UTXOs at the tip that are also in the **tip epoch's** aged UTXO snapshot and belong to a known key (`services/wallet/src/lib.rs:1181-1213`, `wallet/src/lib.rs:316-335`).
- **Aging:** the snapshot used for epoch M+1 is the UTXO set at the end of epoch M−1 (`ledger/src/cryptarchia/mod.rs:145-155,332-339`). **A note created in epoch E becomes eligible in epoch E+2**: 10–20 h on devnet, 100–200 min standalone.
- **Totals:** `total_value` is a saturating sum. `len == 0` means the node cannot win slots.
- **Faucet:** the faucet note is not filtered out here (the leader service skips it separately).
- **Errors:** `DynError` "Failed to get leader aged notes: ..".
- **Free:** `free_leader_aged_notes` tolerates NULL.
- **Module JSON** (`module:...cpp:1359-1399`):
```json
{"tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281","notes":[{"id":"6e0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d70f","value":"8000000000000","public_key":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}],"total_value":"8000000000000"}
```

### 3.8 `get_known_addresses(node) -> KnownAddresses{uint8_t** addresses, len}` - `cb/api/wallet.rs:74-250`
- **Contents:** the configured `wallet.known_keys`, each a separately boxed 32 B Fr (LE).
- **Error:** any error maps to `NotFound` with the Debug-formatted error.
- **Free:** `free_known_addresses` returns `NullPointer` if `addresses` is NULL. For an empty list Rust hands out a dangling **non-NULL** pointer from `Box::leak` of an empty slice, so a mock should also return non-NULL for len 0. The same applies to `free_claimable_vouchers` and `free_pow_claimable_rewards`.
- **Module:** returns a `std::vector<std::string>` of hex strings (`module:...cpp:1281-1309`).

### 3.9 `get_claimable_vouchers(node, optional_tip) -> ClaimableVouchers{tip, vouchers*, len, reward_amount, total_claimable}` - `cb/api/wallet.rs:265-390`
- **Readiness:** the only call with a timeout. It first waits **100 ms** for the wallet to be `Ready`; otherwise it returns `ServiceError` "Wallet service is not ready: ..".
- **Voucher fields:** `{commitment, nullifier}`, 32 B each.
- **Amounts:**
  - `reward_amount` = `claimable_rewards / (snapshot_count − used_nullifiers)`, or 0 (`ledger/src/mantle/leader.rs:175-183`).
  - `total_claimable` = `reward_amount × len` (`services/wallet/src/lib.rs:266-273`).
- **Which vouchers count:** only the wallet's own vouchers that are in the last epoch snapshot and not reserved by an in-flight claim (`services/wallet/src/states.rs:343-366`).
- **Other errors:** `DynError` (Debug-formatted).
- **Module JSON** (`module:...cpp:1599-1631`); it always passes a NULL tip:
```json
{"tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281","vouchers":[{"commitment":"5e0d1c2b3a49586776859463a2b1c0dfeefd0c1b2a39485766758493a2b1c00f","nullifier":"1d2c3b4a5968778695a4b3c2d1e0f00112233445566778899aabbccddeeff00a"}],"reward_amount":"4200","total_claimable":"4200"}
```

### 3.10 `transfer_funds(node, const TransferFundsArguments*) -> Hash` - `cb/api/wallet.rs:733-930`
- **Arguments:** `{optional_tip, change_public_key, funding_public_keys**, funding_public_keys_len, recipient_public_key, amount}`.
- **Validation:** NULL `change_public_key`, `funding_public_keys`, `funding_public_keys[i]` or `recipient_public_key` each give `NullPointer` with a specific message (`:744-779`). Invalid key bytes give `DynError` "Invalid public key: ..".
- **Flow:**
  1. Resolve the tip; NULL means `get_cryptarchia_info`.
  2. `WalletApi::transfer_funds` builds one output `Note(amount, recipient)`.
  3. Funding is chosen from `funding_public_keys` (largest notes first); the fee is paid on top and the change goes to `change_public_key` (priority 0%) (`services/wallet/src/api.rs:193-207`).
  4. Sign, then `mempool::add_tx`.
- **Errors:**
  - Wallet failure: `DynError` "Failed to transfer funds: <err>". Typical messages: "Wallet does not have enough funds, available=N"; "Requested wallet state for unknown block: 0x.."; "Transaction builder error: ..".
  - Mempool failure: `DynError` "Failed to add transaction to mempool: ..".
- **Unknown or foreign funding keys are silently skipped**, so they surface as "Wallet does not have enough funds, available=0". There is no "unknown address" error.
- **`amount == 0`:** the FFI does not check it. (The module only checks that it parses.)
- **Returns** the tx hash. **Module:** returns hex (`module:...cpp:1214-1279`).

### 3.11 `channel_deposit(node, const ChannelDepositArguments*) -> Hash` - `cb/api/wallet.rs:1257-1555`
- **Arguments:** `{optional_tip, channel_id, funding_public_key, amount, metadata*, metadata_len}`.
- **Validation:**
  - `amount == 0`: `RuntimeError` "ChannelDeposit `amount` must be greater than zero.".
  - NULL pointers: `NullPointer`.
- **Flow:**
  1. `get_balance(funding_pk)` at the tip. If there is no index entry, return `NotFound` "Unknown funding address.".
  2. Greedily select the largest notes until they cover `amount`. If they can't, return `DynError` "Insufficient funds to cover deposit amount.".
  3. Build `[Transfer(selected → [Note(amount, pk), change?]), ChannelDeposit(channel, inputs=[transfer output 0], metadata)]`.
  4. One ZkSig is used for both ops.
  5. Submit to the mempool.
- **No fee is funded here**: the Transfer has no fee margin, so the ledger may reject the tx for insufficient balance for gas.
- **Other errors:** `DynError` "Invalid metadata: .." / "Failed to sign deposit tx: .." / "Failed to add transaction to mempool: ..".
- **Returns** the tx hash.

### 3.12 `channel_deposit_with_notes(node, const ChannelDepositWithNotesArguments*) -> Hash` - `cb/api/wallet.rs:950-1235`
- **Arguments:** `{optional_tip, channel_id, input_note_ids*, input_note_ids_len, metadata*, metadata_len, change_public_key, funding_public_keys**, funding_public_keys_len, max_tx_fee}`.
- **Validation:** `input_note_ids_len == 0` gives `RuntimeError` "ChannelDeposit requires at least one input note.".
- **Flow:** build a `ChannelDeposit` op, `fund_tx` with priority 0, then check the fee: `DynError` "tx_fee(X) exceeds max_tx_fee(Y)". Then `sign_tx`, then the mempool.
- **Errors:** wrapped as `DynError` "Failed to push deposit op / fund tx / compute tx fee / sign tx: ..".
- **Returns** the tx hash.

### 3.13 `leader_claim(node) -> TxHash` - `cb/api/leader.rs:36-91`
- **Flow:** the leader service builds a tx with one `LeaderClaim{rewards_root, voucher_nullifier, pk: leader funding_pk}`. The fee is funded from `funding_pk` (`max_tx_fee` default u64::MAX). The tx goes to the **mempool** and the hash is returned (`services/chain/chain-leader/src/lib.rs:784-806`, `services/wallet/src/lib.rs:1267-1435`).
- **Nothing to claim:** wallet error `NoClaimableVoucher` (`services/wallet/src/lib.rs:1274`) gives **`ServiceError` "Failed to claim leader rewards: .."**. It is not NotFound, and no tx is created.
- **Other errors:** relay failure gives `RelayError` "Failed to get ChainLeader relay: ..".
- **Blocking:** **blocks with no timeout until the leader service is serving**, which needs chain Online, chain-network past IBD and Blend ready (`services/chain/chain-leader/src/lib.rs:413-439`). Calling it while Bootstrapping hangs.
- **Module:** returns hex.

### 3.14 `blend_join_as_core_node(node, const char* locator, const uint8_t* service_note_id32) -> DeclarationId([u8;32])` - `cb/api/blend.rs:30-111`
- **Locator:** a multiaddr string, e.g. `/ip4/203.0.113.5/udp/3000/quic-v1`, ≤ 329 B.
  - The code rejects an unspecified IP (`0.0.0.0` / `::`) and a `/p2p/` component (`core/src/sdp/mod.rs:189-213`).
  - Bad UTF-8 or a bad parse gives `ValidationError` "`locator` is not valid UTF-8." / "`locator` is not a valid locator.".
- **`service_note_id`:** Fr LE bytes. Invalid gives `ValidationError` "Invalid `service_note_id` bytes.".
- **Flow:** compute the DeclarationId (§0.4), fund from the SDP `funding_pk`, check `max_tx_fee`, sign, mempool, `Ok(id)` (`services/blend/src/lib.rs:294-342`, `services/sdp/src/lib.rs:493-563`, `nodes/node/binary/src/generic_services/sdp/wallet.rs:40-79`).
- **Synchronous failures** (funding, fee cap, signing because the wallet doesn't own the note, mempool) **drop the reply channel**. That surfaces as a generic `RelayError` "Failed to join blend network: Failed to receive a message from the SDP service: channel closed" (`services/sdp/src/api.rs:17-24`).
- **Ledger-level rejections** (duplicate declaration/provider/zk id, note < min stake 1e9, note already used, nonexistent note, bad signatures; `core/src/mantle/ops/sdp/mod.rs:22-80`) happen later and are **never reported** back to the caller.
- **No note-age requirement** for declaring.
- **Module:** returns hex (`module:...cpp:1667-1692`).

### 3.15 PoW - `cb/api/pow.rs`
- **`pow_start_mining` / `pow_stop_mining` / `pow_start_auto_claim` / `pow_stop_auto_claim`** (`:31-230`)
  - Each just enqueues a message and returns `OK` immediately, **even before Online**. The message is processed once the PoW service starts its loop.
  - Errors are `RelayError` only.
  - Mining state is runtime-only and is not persisted.
- **`pow_status` → `PoWStatus`** (`:485-605`):
  - `is_mining` (false at boot).
  - `are_rewards_enabled` = deployment `rate_num > 0`, static and true on all shipped deployments (`nodes/node/binary/src/lib.rs:180`).
  - `auto_claim`:
    - `is_armed`: initially `rewards_enabled && targets non-empty`.
    - `tick` + `tick_unit` (Seconds=0 / Slots=1): default Seconds 300 (`services/pow/src/service.rs:245,549`).
    - `targets[]`: `{public_key[32], threshold u64, balance FfiOption<u64>}`. `balance.is_some=false` means the wallet read failed; a tracked key with no notes reads 0 (`services/pow/src/service.rs:800-835`).
  - Error: `RelayError`.
  - **Blocks until the chain is Online** (see below).
- **`pow_claimable_rewards` → `{claimable_tickets, slots_until_expiry*, len}`** (`:316-445`):
  - `claimable_tickets` = ready tickets, with expired ones pruned first.
  - `slots_until_expiry[i]` = `block_slot + slot_window(300) − tip_slot` (`services/pow/src/service.rs:1214-1240,1336-1350`).
  - **Blocks until Online.**
- **`pow_claim(node, claim_address32 | NULL) → Hash`** (`:232-314`):
  - No ready tickets (or all anchored on non-canonical blocks): **`NotFound` "No PoW rewards available to claim."** (`:136-141`).
  - NULL address with no target below its threshold: `ServiceError` "Failed to claim PoW rewards: .. NoClaimTarget". Other service errors (`RewardsDisabled`, `RewardPoolExhausted`, `RewardBelowFee`) are also `ServiceError` (`services/pow/src/service.rs:1032-1129`).
  - The tx is `ClaimPowReward` ops (one per ticket, each minting `epoch_reward` to the ticket's own key), followed after every 32 by a Transfer that sweeps the minted notes, minus the fee, to the claim address. It is **published via Blend, not the mempool** (`services/pow/src/service.rs:1449-1500`).
  - **Blocks until Online.**
- **Why some calls block:** the PoW service waits (no timeout) for the chain, wallet, time and blend services, **then for the chain to become Online**, before it drains its inbox (`services/pow/src/service.rs:465-562`). Every request/reply call therefore hangs while Bootstrapping, and so does the module thread that made it.
- **Freeing:** `free_pow_status` tolerates a NULL target list. `free_pow_claimable_rewards` returns `NullPointer` on NULL.
- **Module JSON** (`module:...cpp:1967-2034`):
```json
{"is_mining":true,"are_rewards_enabled":true,"auto_claim":{"is_armed":true,"tick":{"unit":"seconds","value":300},"targets":[{"public_key":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c","threshold":"100000000","balance":"25000000"}]}}
```
```json
{"claimable_tickets":2,"slots_until_expiry":[271,12]}
```

### 3.16 Lifecycle - `cb/api/lifecycle.rs`
- **`start_lb_node(config_path, deployment_path|NULL) → LogosBlockchainNode*`** (`:42-106`):
  - Parses the user config, rejecting unknown keys.
  - A NULL deployment uses the **embedded devnet** settings (`nodes/node/binary/src/config/deployment/mod.rs:17,46-50`).
  - Returns once services have been *started*, not when they are ready.
  - Errors are `InitializationError`.
  - `config_path` is not null-checked.
  - `Runtime::new().expect` can panic.
- **`shutdown_node(node)`** (`:196+`, `cb/node.rs:113-125`):
  - Blocks until Overwatch has finished.
  - Errors are `ShutdownError`.
  - **Can hang forever if called within roughly 250 ms–2 s of start** (`cb/api/lifecycle.rs:235-246`).
  - Stream tasks die with the runtime. A NULL callback on shutdown is likely but not guaranteed (not verified). The module clears `s_instance` *before* shutdown, so it ignores late callbacks anyway (`module:...cpp:698-718`).

---

## 4. Behaviours a simulator must reproduce

### 4.1 Mode / phase machine (`services/chain/chain-service`)
- **Initial engine state** (`src/bootstrap/state.rs:11-55`): Bootstrapping if any of these hold:
  - LIB is genesis (fresh node);
  - `force_bootstrap`;
  - offline longer than `offline_grace_period` (default 20 min; the last state is recorded every 1 min);
  - the previous run ended in Bootstrapping.

  Otherwise Online immediately.
- **Phases** run in order (`src/lib.rs:793-801`):
  1. **AwaitingGenesisTime** (reported as `NotStarted`): sleeps until the genesis time, or is skipped if that time is past. Blocks are rejected (`phases/awaiting_genesis_time.rs:115-180`).
  2. **InitialBlockDownload**: done when chain-network reports `IbdCompleted`.
     - No IBD peers (`skip_ibd`) means it completes immediately.
     - If every peer fails (3 attempts, 250 ms–1 s backoff) **the whole node shuts down** (`services/chain/chain-network/src/lib.rs:340-350`). The streams then end with NULL.
  3. **ProlongedBootstrapPeriod**: skipped if already Online. Otherwise it sleeps `prolonged_bootstrap_period` measured from IBD completion, then switches to **Online** (`phases/pbp.rs:58-104`).
     - Default PBP is 1 h (`nodes/node/binary/src/config/cryptarchia/serde/service.rs:49,71`); the standalone config uses 5 s.
     - On the switch, **LIB jumps to the k-th ancestor of the tip** (k = 120 devnet) (`consensus/cryptarchia-engine/src/lib.rs:61-73,677-682`).
  4. **Following**: forever.
- **While Bootstrapping, LIB stays at the starting LIB** (genesis on a fresh node). So `get_blocks` returns nothing, and the LIB stream is silent until the switch.
- **When Online**, LIB = the k-th block below the tip on the canonical chain (k = 120 blocks ≈ 1 h on devnet at 30 s/block).
- **Fork choice** is `maxvalid_bg` with s_gen = floor(k / (4f)) slots (`consensus/cryptarchia-engine/src/config.rs:105-111`).
- **What works when:**
  - Wallet calls work while Bootstrapping; they need only chain-ready plus backfill (`services/wallet/src/lib.rs:443-536`).
  - PoW request/reply calls and `leader_claim` need Online.

### 4.2 Time and epochs
- **Formulas:**
  - slot = floor((now − genesis) / slot_duration).
  - epoch_length = (stake_stab + nonce_buffer + nonce_stab) × floor(k/f) slots (`consensus/cryptarchia-engine/src/config.rs:128-170`).
- **Snapshots** (`ledger/src/config.rs:71-112`):
  - The stake-distribution snapshot for epoch N is taken at the start of N−1.
  - The nonce/total-stake snapshot is taken at the start of N−1 + 6·base_period.

| deployment | chain_id | genesis | slot | k | f | base period | epoch | s_gen | mean block time | blocks/epoch |
|---|---|---|---|---|---|---|---|---|---|---|
| devnet (embedded default, `nodes/node/binary/src/config/deployment/settings.yaml`) | `0.3.0-rc.5` | 1790598600 s = 2026-09-28T12:30:00Z | 1 s | 120 | 1/30 | 3600 slots | **36000 slots (10 h)** | 900 | 30 s | 1200 |
| testnet template (`deployment/ceremony/genesis/testnet/`) | `X.Y.Z` placeholder | placeholder | 1 s | 120 | 1/30 | 3600 | 36000 | 900 | 30 s | 1200 |
| standalone template | `standalone/X.Y.Z` | placeholder | 1 s | 30 | 1/20 | 600 | 6000 (100 min) | 150 | 20 s | 300 |
| `nodes/node/standalone-deployment-config.yaml` | `standalone-local` | 1778280849 s | 1 s | 30 | 1/20 | 600 | 6000 | 150 | 20 s | 300 |

- **Other consensus settings:**
  - Epoch split 3/3/4.
  - Learning rate 1.0 on devnet/testnet, 0.5 standalone.
  - Uncles: at most 4 per block, window 12 blocks.

### 4.3 Deployment economics (devnet; testnet is identical apart from keys/IPs)
- **Genesis distribution** (`settings.yaml:79-358`): 137 transfer outputs.
  - 4×1e9 provider stake notes.
  - 4×1e14.
  - 8×2e14.
  - 121×8e12 to a single key.
  - Total 2,968,004,000,000,000.
  - Plus a faucet note of u64::MAX to `faucet_pk` `3956ddb6ed8cb0b002f9839bfecd1d31f852594b46c3dee8d72df926be3bb625` (`settings.yaml:427`).
  - Plus 4 BN declarations (opcode 32) at 65.108.203.235 on UDP ports 3400/3401/3402/50002.
- **SDP:**
  - BN `min_stake` 1e9 (`settings.yaml:59-60`).
  - `inactivity_period` 2 epochs.
  - No lock-period parameter.
- **Blend:**
  - 1 layer; min network size 2; 1 round = 1 slot.
  - Cover traffic 1 msg/round; max release delay 1 round; peering degree 4.
  - Epoch transition period = 1 block interval.
  - Blend PoW `base_difficulty` 2^-19.
- **PoW rewards:**
  - Pool genesis 3e11.
  - **`epoch_reward_genesis` 25,000,000 per ticket on devnet** (`settings.yaml:43`); 1e8 standalone.
  - `minimum_difficulty` 19 (≈ 2^-19 per attempt).
  - Target 1 claim per block; 1/10 of the pool per epoch.
  - 10% of fees go to the PoW pool.
  - **`slot_window` 300 slots**: tickets expire when tip_slot − block_slot > 300 (`settings.yaml:53`, `services/pow/src/service.rs:1187`).
- **Mining loop:** for each new block within the window, search for a winning ticket. Winners join `ready_to_claim` only while `is_mining` (`services/pow/src/service.rs:636`). Claimed tickets move to pending and are retired when a block carries a matching `PoWRewardClaimed` event (`:1270-1330`).
- **Auto-claim:**
  - Each tick it picks the target below its threshold with the smallest balance and drains all ready tickets into it.
  - When every target is at or above its threshold it disarms **and stops mining** (`:666-671,880-1010`).
  - A configured target that is not in `wallet.known_keys` makes PoW startup fail, and the node with it (`:515,718-745`).
- **Leader block reward** (`ledger/src/lib.rs:63-97,388-447`): with devnet's total stake (~3e15) the inflationary term is 0. The reward is ≈ fees: leaders get 40% plus tips into `pending_rewards`, Blend gets 60%.
- **Leader rewards and vouchers:**
  - Every header's `voucher_cm` goes into an MMR.
  - At the first block of a new epoch the snapshot `{root, count}` is taken and `pending_rewards` becomes `claimable_rewards` (`ledger/src/mantle/leader.rs:109-162`).
  - **So a voucher won in epoch E is claimable from the first block of E+1.**
  - The pool is split evenly across unclaimed vouchers, so `reward_amount` changes as others claim.
  - The wallet stops tracking a voucher once its claim reaches the LIB (`wallet/src/lib.rs:854`).
- **Who stakes:** leader elections draw on aged notes of *any* known key (§3.7). `funding_pk` only pays the claim fee and receives the reward.

### 4.4 Fees (`core/src/mantle/transactions/tx_list/ops.rs:122-162`, `ledger/src/lib.rs:484-523`)
- **Formula:** fee = exec_gas × `execution_base_gas_price` + storage_gas × `storage_gas_price`. `storage_gas` = the signed tx's serialized size in bytes.
- **Execution gas per op:**

| op | exec gas |
|---|---|
| Transfer / Deposit / SDPWithdraw / SDPActive | 590 |
| SDPDeclare | 646 |
| LeaderClaim | 580 |
| ClaimPowReward | 1 |
| Inscribe | 56 |
| ChannelConfig | 56 × config threshold |
| Withdraw / ChannelTransfer | 56 × transfer threshold |

- **Gas prices:** both start at **1** at genesis (`core/src/mantle/transactions/gas.rs:9-12`).
  - The execution price moves **every block** by an EMA rule (`ledger/src/cryptarchia/mod.rs:41-49,464-480`).
  - The storage price moves **every epoch**, clamped to ×7/8–×9/8 (`:801-835`).
  - So a small transfer costs ~590 + ~300–600 bytes ≈ **900–1200 units** at genesis prices.
- **Fees are implicit:** the fee is the inputs − outputs surplus (anything above the required fee is a tip). There is no fee field in the tx JSON.
- **Ledger rejection:** surplus below the required fee fails with "Insufficient balance".
- **Wallet `fund_tx`** (`wallet/src/lib.rs:239-338`):
  - Candidates: the funding keys' notes minus service, channel and in-flight-reserved notes, largest first.
  - Inputs are added until they cover outputs + fee (+ priority %).
  - The change note (≥1) goes to `change_pk`.
  - If nothing works: `InsufficientFunds{available}`.

### 4.5 Chain linkage and ids
- **Parent:** `parent_block` is the parent's header id. Genesis has slot 0 and height 0.
- **Ids per shape:**
  - The block's own id appears only in the processed-blocks stream (`block.header.id`).
  - Elsewhere, derive it from the child's `parent_block`, or compute it as in §0.4.
- **Slots:** consecutive canonical blocks have strictly increasing slots with gaps. On average 1 block per 1/f slots (30 slots on devnet).
- **Tx hashes:** 32 B Blake2b, rendered as 64 lowercase hex chars without `0x`. In blocks they appear only as `id` (new-blocks stream) or `mantle_tx.hash` (processed stream).

---

## 5. What the module adds versus what the FFI returns

| Module method | FFI | Module transformation |
|---|---|---|
| `newBlock` event | `subscribe_to_new_blocks` | wraps it as `{"block":"<json string>"}` (double-encoded); NULL becomes `null` |
| `processedBlock` / `libBlock` | streams | raw pass-through; NULL becomes `null` |
| `get_block`, `get_blocks`, `get_transaction`, `get_block_events`, `get_channel_state`, `blend_info`, `wallet_fund_tx`, `get_chain_id` | string | pass-through |
| `get_cryptarchia_info` | struct | JSON; ids as hex, slots and height as numbers, `mode` as "Online"/"Bootstrapping"/"NotStarted" |
| `get_time_info`, `get_network_info`, `pow_claimable_rewards` | struct | JSON with numbers |
| `wallet_get_balance` | u64 | decimal **string** |
| `wallet_get_notes`, `wallet_get_leader_aged_notes`, `wallet_get_claimable_vouchers` | structs | JSON with hex ids and u64 values as **strings** |
| `pow_status` | struct | JSON; `tick` as `{unit:"seconds"/"slots", value}`; `threshold`/`balance` as strings; `balance:null` when the FFI says none |
| `wallet_transfer_funds`, `channel_deposit*`, `leader_claim`, `pow_claim`, `submit_signed_transaction`, `blend_join_as_core_node` | Hash | lowercase hex |
| `wallet_get_known_addresses` | addresses | vector of hex strings |

**Input checks the module does before reaching the FFI** (so the mock never sees these inputs):
- Every 32-byte argument must be 64 hex chars; `0x` is allowed.
- Amounts must parse as u64; channel_deposit also rejects 0.
- Sender and funding lists must be non-empty.
- Metadata must be even-length hex.
- The node handle must exist.

**Tip arguments:** the module always passes a NULL tip to `get_balance` and `get_claimable_vouchers`. `get_wallet_notes`, `get_leader_aged_notes`, `transfer_funds` and `channel_deposit*` forward the caller's optional tip (NULL when it is empty).
