# Mock node

`blockchain_module` with the running node simulated. The module's C++ is the real
code; only the node half of `liblogos_blockchain` (everything that takes a
`LogosBlockchainNode*`) is replaced by a simulated chain. Config generation,
keys, `get_peer_id` and the other helpers that work without a node still call
the real library, so the configs and keystores you get are genuine.

Use it to build and test UIs against flows a real network makes slow or
impossible to reproduce. These include bootstrapping, mining, claiming,
transfers and joining Blend as a core node, plus their failure modes. It only
implements what the C API on master offers, and does not add anything to it.

## Build and run

```bash
nix build .#mock            # the module (plugin + libs), like .#default
nix build .#mock-lgx        # every output of the real module has a mock-* twin
```

To run an app against it, override its `blockchain_module` input with the
`mock/` subflake:

```bash
# from logos-blockchain-ui
nix run . --override-input blockchain_module \
  "path:../logos-blockchain-module?dir=mock"
```

The mock keeps its chain in `mock_node_state.json` under the config's
`state.base_folder`. `purge_state` deletes the state directory and with it the
mock chain. Everything it does is logged to stderr with a `[lb-mock]` prefix,
including the things a real node does not tell the caller, such as ledger
rejections and missed Blend activity.

## What it reproduces

The behaviour comes from the pinned node (`2c65264`). `FFI_SHAPES.md` has the
exact shapes and where each one comes from.

- **Lifecycle.** The node goes `Bootstrapping` → IBD → prolonged bootstrap period
  (`prolonged_bootstrap_period` from the config, one hour by default) → `Online`.
  LIB stays at the starting LIB until Online, then jumps to tip − k. After a
  restart within `offline_grace_period` of being Online, the node comes back
  Online at once.
  - A config with no `initial_peers` and no IBD peers is isolated: it never sees a
    network block.
- **Chain.**
  - Blocks follow the deployment's slot duration and active-slot coefficient, with
    background transfer traffic.
  - The three streams fire on the node's own thread: new blocks, processed blocks
    (with `tip`/`lib`) and LIB changes.
  - `get_blocks` returns finalized blocks only. `get_transaction` only knows this
    node's txs and those it saw in the last 10 minutes.
- **Calls that block until Online, with no timeout.** `pow_status`,
  `pow_claimable_rewards`, `pow_claim` and `leader_claim`.
- **Calling the FFI from a stream callback aborts the process,** like the real
  panic.
- **Wallet.**
  - A known key with no notes is `NotFound "Unknown wallet address."`, not 0.
  - Funding picks the largest notes first and reserves them until the tx lands,
    and for `pending_note_expiry_blocks` after it is rejected.
  - Fees are execution gas plus bytes, with both prices at 1 (about 900–1200 for a
    transfer).
  - `channel_deposit` funds no fee, so the ledger drops it. Use
    `channel_deposit_with_notes`.
- **PoW.**
  - While mining and Online, each block may yield tickets, capped at
    `max_tickets_per_block`. Tickets expire after `slot_window` slots.
  - Claims mint `epoch_reward` per ticket, minus the fee, to the claim address.
    Auto-claim follows the config targets and stops mining once they are all met.
- **Leader.** Aged notes (created ≥ 2 epochs ago) win slots in proportion to their
  stake. A voucher becomes claimable the next epoch, and `leader_claim` mints the
  reward to the leader funding key.
- **Blend.**
  - `blend_join_as_core_node` validates the locator and note, pays the fee from the
    SDP funding key and returns the declaration id once the tx is in the mempool.
    Any funding or signing failure is the generic "channel closed" `RelayError`.
  - The ledger rejects a declaration that is a duplicate, uses a spent note, uses a
    note below `min_stake`, or uses an already locked note. It does so silently.
  - An accepted declaration from epoch E puts the node in the core set from E+2.
    `blend_info().core_info` is set while it is a member.
  - Each epoch the node posts an activity proof from the SDP funding key (if it was
    online, reachable and won the draw). Missing more than `inactivity_period`
    epochs drops it out.
  - Rewards for epoch N arrive in the first block of N+2, as `SdpRewardDistributed`
    header events and a note on the BlendZk key.
  - The locked stake note stays in `wallet_get_notes` and the balance, as on the
    real node.

## Knobs

Environment variables, read when the node starts:

| Variable | Default | Effect |
|---|---|---|
| `LB_MOCK_PROFILE=fast` | off | 1 block per 5 slots, k = 3, 150-slot (2.5 min) epochs, 10 s prolonged bootstrap, genesis a minute before the first start. Blend activation takes minutes instead of a day. |
| `LB_MOCK_ASSUME_PEERS` | 0 | Treat a config with no peers as connected. |
| `LB_MOCK_PBP_SECONDS` | config | Override the prolonged bootstrap period. |
| `LB_MOCK_IBD_BLOCKS_PER_SECOND` | 400 | Download speed while catching up. |
| `LB_MOCK_POW_TICKETS_PER_BLOCK` | 1.0 | Mean tickets found per block while mining. |
| `LB_MOCK_NETWORK_STAKE` | 2.968e15 | Total stake; lower it to win leader slots with test-sized stake. |
| `LB_MOCK_BLEND_HIT_RATE` | 0.9 | Chance a core node gets an activity proof in an epoch. |
| `LB_MOCK_BLEND_REACHABLE` | 1 | 0 = no Blend peers reach the node (no activity proofs). |
| `LB_MOCK_BACKGROUND_TX_PER_BLOCK` | 0.7 | Other users' transfers per block (these feed the fees and the rewards). |
| `LB_MOCK_PANIC_ON_REENTRY` | 1 | 0 = only warn when the FFI is called from a stream callback. |
| `LB_MOCK_SEED` | fixed | Seed of the network's chain. |

## Not simulated

- Forks and reorgs: every block is canonical.
- The genesis block's contents: it has no transactions.
- Other providers' declarations beyond the deployment's genesis ones.
- Exact tx hashes, note ids and fees: they have the right shapes and magnitudes,
  not the real hash functions.
- Withdrawing a declaration: the C API has no call for it.
- Stream lag: the new-blocks and LIB streams never end because the consumer is
  slow.
