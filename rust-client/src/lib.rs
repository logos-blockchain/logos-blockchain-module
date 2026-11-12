pub mod generated;
pub mod reply;
pub mod types;

pub use generated::BlockchainModuleClient;
pub use logos_rust_sdk::{EventData, EventSubscription, LogosError};
pub use reply::{ModuleError, StreamItem};

use serde::de::DeserializeOwned;

use reply::stream_item;
use types::{
    id_hex, parse_amount, Accounts, ApiBlock, BlockInfo, ChannelId, ChannelState, ClaimableVouchers,
    CryptarchiaInfo, Events, HeaderId, LeaderAgedNotes, NetworkInfo, PowClaimableRewards, ProcessedBlockEvent, SignedOps,
    Slot, StandardMode, StoredBlock, TimeInfo, Unverified, TxHash, Value, WalletFundRequestBody, WalletFundResponseBody, WalletNotes,
};

type Reply = Result<serde_json::Value, LogosError>;

fn adapt<T, F, D>(finish: D, callback: F) -> impl FnOnce(Reply) + Send + 'static
where
    F: FnOnce(Result<T, ModuleError>) + Send + 'static,
    D: FnOnce(Reply) -> Result<T, ModuleError> + Send + 'static,
{
    move |reply| callback(finish(reply))
}

fn tx_hash(method: &'static str) -> impl FnOnce(Reply) -> Result<TxHash, ModuleError> + Send + 'static {
    move |reply| reply::decode::<TxHash>(method, reply)
}

fn string(method: &'static str) -> impl FnOnce(Reply) -> Result<String, ModuleError> + Send + 'static {
    move |reply| reply::decode::<String>(method, reply)
}

/// A reply that is text, kept as text even when it happens to parse as JSON
/// (a numeric chain id, a YAML document that is also a JSON scalar).
fn text(method: &'static str) -> impl FnOnce(Reply) -> Result<String, ModuleError> + Send + 'static {
    move |reply| reply::text(method, reply)
}

fn typed<T: DeserializeOwned>(method: &'static str) -> impl FnOnce(Reply) -> Result<T, ModuleError> + Send + 'static {
    move |reply| reply::decode::<T>(method, reply)
}

fn typed_opt<T: DeserializeOwned>(method: &'static str) -> impl FnOnce(Reply) -> Result<Option<T>, ModuleError> + Send + 'static {
    move |reply| reply::decode_opt::<T>(method, reply)
}

fn block_opt(method: &'static str) -> impl FnOnce(Reply) -> Result<Option<ApiBlock>, ModuleError> + Send + 'static {
    move |reply| Ok(reply::decode_opt::<StoredBlock>(method, reply)?.map(ApiBlock::from))
}

fn block_list(method: &'static str) -> impl FnOnce(Reply) -> Result<Vec<ApiBlock>, ModuleError> + Send + 'static {
    move |reply| Ok(reply::decode::<Vec<StoredBlock>>(method, reply)?.into_iter().map(ApiBlock::from).collect())
}

fn unit(method: &'static str) -> impl FnOnce(Reply) -> Result<(), ModuleError> + Send + 'static {
    move |reply| reply::unit(method, reply)
}

fn strings(items: &[String]) -> serde_json::Value {
    serde_json::Value::Array(items.iter().map(|s| serde_json::Value::from(s.as_str())).collect())
}

fn encode<T: serde::Serialize>(method: &'static str, v: &T) -> Result<String, ModuleError> {
    serde_json::to_string(v).map_err(|e| ModuleError::decode(method, format!("encode request: {e}")))
}

fn tip_hex(method: &'static str, tip: Option<HeaderId>) -> Result<String, ModuleError> {
    Ok(tip.map(|t| id_hex(method, &t)).transpose()?.unwrap_or_default())
}

impl BlockchainModuleClient {
    pub fn start_node(&self, config_path: &str, deployment: &str) -> Result<(), ModuleError> {
        unit("start")(self.start(config_path, deployment))
    }
    pub fn start_node_async<F>(&self, config_path: &str, deployment: &str, callback: F)
    where
        F: FnOnce(Result<(), ModuleError>) + Send + 'static,
    {
        self.start_async(config_path, deployment, adapt(unit("start"), callback));
    }

    pub fn stop_node(&self) -> Result<(), ModuleError> {
        unit("stop")(self.stop())
    }
    pub fn stop_node_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<(), ModuleError>) + Send + 'static,
    {
        self.stop_async(adapt(unit("stop"), callback));
    }

    pub fn state_exists(&self) -> Result<bool, ModuleError> {
        typed::<bool>("does_state_exist")(self.does_state_exist())
    }
    pub fn state_exists_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<bool, ModuleError>) + Send + 'static,
    {
        self.does_state_exist_async(adapt(typed::<bool>("does_state_exist"), callback));
    }

    pub fn purge_node_state(&self) -> Result<(), ModuleError> {
        unit("purge_state")(self.purge_state())
    }
    pub fn purge_node_state_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<(), ModuleError>) + Send + 'static,
    {
        self.purge_state_async(adapt(unit("purge_state"), callback));
    }

    pub fn generate_config(&self, args: &serde_json::Value) -> Result<String, ModuleError> {
        string("generate_user_config")(self.generate_user_config(&args.to_string()))
    }
    pub fn generate_config_async<F>(&self, args: &serde_json::Value, callback: F)
    where
        F: FnOnce(Result<String, ModuleError>) + Send + 'static,
    {
        self.generate_user_config_async(&args.to_string(), adapt(string("generate_user_config"), callback));
    }

    /// Writes the node's participation settings into the config at `config_path`.
    /// `external_port` is 65535 at most; 0 means none.
    pub fn participate_in_network(
        &self,
        config_path: &str,
        output_dir: &str,
        external_address: &str,
        external_port: u16,
    ) -> Result<(), ModuleError> {
        unit("participate")(self.participate(config_path, output_dir, external_address, u64::from(external_port)))
    }

    /// The config a node started from `config_path` would run with, as YAML with
    /// the keystore merged in and the secrets redacted.
    pub fn effective_config(&self, config_path: &str) -> Result<String, ModuleError> {
        text("show_config")(self.show_config(config_path))
    }

    pub fn peer_id(&self, config_path: &str) -> Result<String, ModuleError> {
        string("get_peer_id")(self.get_peer_id(config_path))
    }
    pub fn peer_id_async<F>(&self, config_path: &str, callback: F)
    where
        F: FnOnce(Result<String, ModuleError>) + Send + 'static,
    {
        self.get_peer_id_async(config_path, adapt(string("get_peer_id"), callback));
    }

    /// `key_type` is "ed25519" or "zk". `key_title` may be empty (auto-generated).
    pub fn generate_wallet_key(&self, keystore_path: &str, key_type: &str, key_title: &str) -> Result<String, ModuleError> {
        text("generate_key")(self.generate_key(keystore_path, key_type, key_title))
    }

    pub fn add_wallet_key(
        &self,
        keystore_path: &str,
        key_type: &str,
        key_hex: &str,
        key_title: &str,
    ) -> Result<(), ModuleError> {
        unit("add_key")(self.add_key(keystore_path, key_type, key_hex, key_title))
    }

    pub fn remove_wallet_key(&self, keystore_path: &str, key_title: &str) -> Result<(), ModuleError> {
        unit("remove_key")(self.remove_key(keystore_path, key_title))
    }

    pub fn balance(&self, address_hex: &str) -> Result<Value, ModuleError> {
        let v = reply::unwrap("wallet_get_balance", self.wallet_get_balance(address_hex))?;
        parse_amount("wallet_get_balance", &v)
    }
    pub fn balance_async<F>(&self, address_hex: &str, callback: F)
    where
        F: FnOnce(Result<Value, ModuleError>) + Send + 'static,
    {
        self.wallet_get_balance_async(
            address_hex,
            adapt(
                |reply| reply::unwrap("wallet_get_balance", reply).and_then(|v| parse_amount("wallet_get_balance", &v)),
                callback,
            ),
        );
    }

    pub fn known_addresses(&self) -> Result<Vec<String>, ModuleError> {
        typed::<Vec<String>>("wallet_get_known_addresses")(self.wallet_get_known_addresses())
    }
    pub fn known_addresses_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<Vec<String>, ModuleError>) + Send + 'static,
    {
        self.wallet_get_known_addresses_async(adapt(typed::<Vec<String>>("wallet_get_known_addresses"), callback));
    }

    pub fn notes(&self, address_hex: &str, tip: Option<HeaderId>) -> Result<WalletNotes, ModuleError> {
        let tip = tip_hex("wallet_get_notes", tip)?;
        typed::<WalletNotes>("wallet_get_notes")(self.wallet_get_notes(address_hex, &tip))
    }
    pub fn notes_async<F>(&self, address_hex: &str, tip: Option<HeaderId>, callback: F)
    where
        F: FnOnce(Result<WalletNotes, ModuleError>) + Send + 'static,
    {
        let tip = match tip_hex("wallet_get_notes", tip) {
            Ok(t) => t,
            Err(e) => return callback(Err(e)),
        };
        self.wallet_get_notes_async(address_hex, &tip, adapt(typed::<WalletNotes>("wallet_get_notes"), callback));
    }

    pub fn transfer_funds(
        &self,
        change_public_key: &str,
        sender_addresses: &[String],
        recipient_address: &str,
        amount: Value,
        tip: Option<HeaderId>,
    ) -> Result<TxHash, ModuleError> {
        let tip = tip_hex("wallet_transfer_funds", tip)?;
        tx_hash("wallet_transfer_funds")(self.wallet_transfer_funds(
            change_public_key,
            &strings(sender_addresses),
            recipient_address,
            &amount.to_string(),
            &tip,
        ))
    }

    /// Wallet notes old enough to take part in the leadership lottery. `tip` of
    /// `None` queries at the current tip.
    pub fn leader_aged_notes(&self, tip: Option<HeaderId>) -> Result<LeaderAgedNotes, ModuleError> {
        let tip = tip_hex("wallet_get_leader_aged_notes", tip)?;
        typed::<LeaderAgedNotes>("wallet_get_leader_aged_notes")(self.wallet_get_leader_aged_notes(&tip))
    }

    pub fn claim_leader_reward(&self) -> Result<TxHash, ModuleError> {
        tx_hash("leader_claim")(self.leader_claim())
    }

    pub fn claimable_vouchers(&self) -> Result<ClaimableVouchers, ModuleError> {
        typed::<ClaimableVouchers>("wallet_get_claimable_vouchers")(self.wallet_get_claimable_vouchers())
    }
    pub fn claimable_vouchers_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<ClaimableVouchers, ModuleError>) + Send + 'static,
    {
        self.wallet_get_claimable_vouchers_async(adapt(typed::<ClaimableVouchers>("wallet_get_claimable_vouchers"), callback));
    }

    pub fn fund_tx(&self, request: &WalletFundRequestBody) -> Result<WalletFundResponseBody, ModuleError> {
        let body = encode("wallet_fund_tx", request)?;
        typed::<WalletFundResponseBody>("wallet_fund_tx")(self.wallet_fund_tx(&body))
    }
    pub fn fund_tx_async<F>(&self, request: &WalletFundRequestBody, callback: F)
    where
        F: FnOnce(Result<WalletFundResponseBody, ModuleError>) + Send + 'static,
    {
        let body = match encode("wallet_fund_tx", request) {
            Ok(b) => b,
            Err(e) => return callback(Err(e)),
        };
        self.wallet_fund_tx_async(&body, adapt(typed::<WalletFundResponseBody>("wallet_fund_tx"), callback));
    }

    pub fn submit_transaction(&self, tx: &SignedOps<Unverified, StandardMode>) -> Result<TxHash, ModuleError> {
        let body = encode("submit_signed_transaction", tx)?;
        tx_hash("submit_signed_transaction")(self.submit_signed_transaction(&body))
    }
    pub fn submit_transaction_async<F>(&self, tx: &SignedOps<Unverified, StandardMode>, callback: F)
    where
        F: FnOnce(Result<TxHash, ModuleError>) + Send + 'static,
    {
        let body = match encode("submit_signed_transaction", tx) {
            Ok(b) => b,
            Err(e) => return callback(Err(e)),
        };
        self.submit_signed_transaction_async(&body, adapt(tx_hash("submit_signed_transaction"), callback));
    }

    pub fn transaction(&self, hash: TxHash) -> Result<Option<serde_json::Value>, ModuleError> {
        let hex = id_hex("get_transaction", &hash)?;
        typed_opt::<serde_json::Value>("get_transaction")(self.get_transaction(&hex))
    }

    pub fn channel_state(&self, channel_id: ChannelId) -> Result<Option<ChannelState>, ModuleError> {
        typed_opt::<ChannelState>("get_channel_state")(self.get_channel_state(&channel_id.to_string()))
    }
    pub fn channel_state_async<F>(&self, channel_id: ChannelId, callback: F)
    where
        F: FnOnce(Result<Option<ChannelState>, ModuleError>) + Send + 'static,
    {
        self.get_channel_state_async(&channel_id.to_string(), adapt(typed_opt::<ChannelState>("get_channel_state"), callback));
    }

    pub fn deposit_to_channel(
        &self,
        channel_id: ChannelId,
        funding_public_key_hex: &str,
        amount: Value,
        metadata_hex: &str,
        tip: Option<HeaderId>,
    ) -> Result<TxHash, ModuleError> {
        let tip = tip_hex("channel_deposit", tip)?;
        tx_hash("channel_deposit")(self.channel_deposit(
            &channel_id.to_string(),
            funding_public_key_hex,
            &amount.to_string(),
            metadata_hex,
            &tip,
        ))
    }

    #[allow(clippy::too_many_arguments)]
    pub fn deposit_notes_to_channel(
        &self,
        channel_id: ChannelId,
        input_note_ids: &[String],
        metadata_hex: &str,
        change_public_key_hex: &str,
        funding_public_keys: &[String],
        max_tx_fee: Value,
        tip: Option<HeaderId>,
    ) -> Result<TxHash, ModuleError> {
        let tip = tip_hex("channel_deposit_with_notes", tip)?;
        tx_hash("channel_deposit_with_notes")(self.channel_deposit_with_notes(
            &channel_id.to_string(),
            &strings(input_note_ids),
            metadata_hex,
            change_public_key_hex,
            &strings(funding_public_keys),
            &max_tx_fee.to_string(),
            &tip,
        ))
    }

    pub fn block(&self, id: HeaderId) -> Result<Option<ApiBlock>, ModuleError> {
        let hex = id_hex("get_block", &id)?;
        block_opt("get_block")(self.get_block(&hex))
    }
    pub fn block_async<F>(&self, id: HeaderId, callback: F)
    where
        F: FnOnce(Result<Option<ApiBlock>, ModuleError>) + Send + 'static,
    {
        let hex = match id_hex("get_block", &id) {
            Ok(h) => h,
            Err(e) => return callback(Err(e)),
        };
        self.get_block_async(&hex, adapt(block_opt("get_block"), callback));
    }

    pub fn blocks(&self, from: Slot, to: Slot) -> Result<Vec<ApiBlock>, ModuleError> {
        block_list("get_blocks")(self.get_blocks(from.into_inner(), to.into_inner()))
    }
    pub fn blocks_async<F>(&self, from: Slot, to: Slot, callback: F)
    where
        F: FnOnce(Result<Vec<ApiBlock>, ModuleError>) + Send + 'static,
    {
        self.get_blocks_async(from.into_inner(), to.into_inner(), adapt(block_list("get_blocks"), callback));
    }

    pub fn block_events(&self, id: HeaderId) -> Result<Option<Events>, ModuleError> {
        let hex = id_hex("get_block_events", &id)?;
        typed_opt::<Events>("get_block_events")(self.get_block_events(&hex))
    }
    pub fn block_events_async<F>(&self, id: HeaderId, callback: F)
    where
        F: FnOnce(Result<Option<Events>, ModuleError>) + Send + 'static,
    {
        let hex = match id_hex("get_block_events", &id) {
            Ok(h) => h,
            Err(e) => return callback(Err(e)),
        };
        self.get_block_events_async(&hex, adapt(typed_opt::<Events>("get_block_events"), callback));
    }

    pub fn cryptarchia_info(&self) -> Result<CryptarchiaInfo, ModuleError> {
        typed::<CryptarchiaInfo>("get_cryptarchia_info")(self.get_cryptarchia_info())
    }
    pub fn cryptarchia_info_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<CryptarchiaInfo, ModuleError>) + Send + 'static,
    {
        self.get_cryptarchia_info_async(adapt(typed::<CryptarchiaInfo>("get_cryptarchia_info"), callback));
    }

    pub fn time_info(&self) -> Result<TimeInfo, ModuleError> {
        typed::<TimeInfo>("get_time_info")(self.get_time_info())
    }
    pub fn time_info_async<F>(&self, callback: F)
    where
        F: FnOnce(Result<TimeInfo, ModuleError>) + Send + 'static,
    {
        self.get_time_info_async(adapt(typed::<TimeInfo>("get_time_info"), callback));
    }

    pub fn blend_network_info(&self) -> Result<serde_json::Value, ModuleError> {
        reply::unwrap("blend_info", self.blend_info())
    }

    /// Declares this node as a blend core node and returns the declaration id.
    /// `service_note_id_hex` may be empty: the node then picks the note.
    pub fn join_blend_as_core_node(&self, locator: &str, service_note_id_hex: &str) -> Result<String, ModuleError> {
        text("blend_join_as_core_node")(self.blend_join_as_core_node(locator, service_note_id_hex))
    }

    pub fn chain_id(&self) -> Result<String, ModuleError> {
        text("get_chain_id")(self.get_chain_id())
    }

    pub fn network_info(&self) -> Result<NetworkInfo, ModuleError> {
        typed::<NetworkInfo>("get_network_info")(self.get_network_info())
    }

    /// The running node's metrics as an OpenMetrics text document. Empty if the
    /// node is not running or the metrics cannot be read.
    pub fn open_metrics_text(&self) -> Result<String, ModuleError> {
        self.collect_open_metrics_text()
            .map_err(|source| ModuleError::Transport { method: "collectOpenMetricsText", source })
    }

    /// Call after a stream ended to receive `newBlock` events again.
    pub fn resubscribe_to_new_blocks(&self) -> Result<(), ModuleError> {
        unit("subscribe_to_new_blocks")(self.subscribe_to_new_blocks())
    }
    /// Call after a stream ended to receive `processedBlock` events again.
    pub fn resubscribe_to_processed_blocks(&self) -> Result<(), ModuleError> {
        unit("subscribe_to_processed_blocks")(self.subscribe_to_processed_blocks())
    }
    /// Call after a stream ended to receive `libBlock` events again.
    pub fn resubscribe_to_lib_blocks(&self) -> Result<(), ModuleError> {
        unit("subscribe_to_lib_blocks")(self.subscribe_to_lib_blocks())
    }

    /// Accounts the config at `config_path` names and the keys its keystore
    /// holds. An empty path reads the running node's config.
    pub fn accounts(&self, config_path: &str) -> Result<Accounts, ModuleError> {
        typed::<Accounts>("read_accounts")(self.read_accounts(config_path))
    }

    pub fn mining_status(&self) -> Result<serde_json::Value, ModuleError> {
        reply::unwrap("pow_status", self.pow_status())
    }
    /// Writes the pow section into the config at `config_path`; the node reads
    /// it on the next start.
    pub fn configure_mining(&self, config_path: &str, config: &serde_json::Value) -> Result<(), ModuleError> {
        unit("pow_configure")(self.pow_configure(config_path, &config.to_string()))
    }
    /// The pow section of the config at `config_path`, in the shape
    /// [`Self::configure_mining`] takes.
    pub fn mining_config(&self, config_path: &str) -> Result<serde_json::Value, ModuleError> {
        reply::unwrap("read_pow_config", self.read_pow_config(config_path))
    }

    pub fn start_mining(&self) -> Result<(), ModuleError> {
        unit("pow_start_mining")(self.pow_start_mining())
    }
    pub fn stop_mining(&self) -> Result<(), ModuleError> {
        unit("pow_stop_mining")(self.pow_stop_mining())
    }
    pub fn start_auto_claim(&self) -> Result<(), ModuleError> {
        unit("pow_start_auto_claim")(self.pow_start_auto_claim())
    }
    pub fn stop_auto_claim(&self) -> Result<(), ModuleError> {
        unit("pow_stop_auto_claim")(self.pow_stop_auto_claim())
    }
    pub fn claim_mining_rewards(&self, claim_address_hex: &str) -> Result<TxHash, ModuleError> {
        tx_hash("pow_claim")(self.pow_claim(claim_address_hex))
    }
    pub fn claimable_mining_rewards(&self) -> Result<PowClaimableRewards, ModuleError> {
        typed::<PowClaimableRewards>("pow_claimable_rewards")(self.pow_claimable_rewards())
    }

    pub fn processed_block_item(ev: &EventData) -> Result<StreamItem<ProcessedBlockEvent>, ModuleError> {
        let payload = Self::decode_processed_block(ev)
            .ok_or_else(|| ModuleError::decode("processedBlock", "malformed event payload"))?;
        stream_item("processedBlock", &payload.event_json)
    }

    pub fn lib_block_item(ev: &EventData) -> Result<StreamItem<BlockInfo>, ModuleError> {
        let payload = Self::decode_lib_block(ev)
            .ok_or_else(|| ModuleError::decode("libBlock", "malformed event payload"))?;
        stream_item("libBlock", &payload.block_info_json)
    }

    pub fn new_block_item(ev: &EventData) -> Result<StreamItem<serde_json::Value>, ModuleError> {
        let payload = Self::decode_new_block(ev)
            .ok_or_else(|| ModuleError::decode("newBlock", "malformed event payload"))?;
        stream_item("newBlock", &payload.block_json)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const STORED_BLOCK: &str = include_str!("../tests/fixtures/get_block.json");

    fn reply_with(json: &str) -> Reply {
        Ok(serde_json::json!({ "success": true, "value": json, "error": null }))
    }

    #[test]
    fn stored_block_gets_its_header_id() {
        let block = block_opt("get_block")(reply_with(STORED_BLOCK)).unwrap().expect("a block");
        assert_eq!(block.header.slot, Slot::from(689_412));
        assert_eq!(block.transactions.len(), 1);
        // The id is the header's hash: stable, and not the parent's.
        let again = block_opt("get_block")(reply_with(STORED_BLOCK)).unwrap().expect("a block");
        assert_eq!(block.header.id, again.header.id);
        assert_ne!(block.header.id, block.header.parent_block);
    }

    #[test]
    fn stored_blocks_decode_as_a_list() {
        let blocks = block_list("get_blocks")(reply_with(&format!("[{STORED_BLOCK},{STORED_BLOCK}]"))).unwrap();
        assert_eq!(blocks.len(), 2);
        assert!(block_list("get_blocks")(reply_with("[]")).unwrap().is_empty());
    }

    #[test]
    fn missing_block_is_none() {
        let reply = Ok(serde_json::json!({ "success": false, "value": null, "error": "No block found for header id [1, 2]" }));
        assert!(block_opt("get_block")(reply).unwrap().is_none());
    }
}
