mod generated;

use crate::generated::{install, BlockchainModule}; 

#[derive(Default)]
struct DemoImpl;

impl BlockchainModule for DemoImpl {
    // Implement required methods, ignoring unused ones with `_` or placeholders
    fn start(&mut self, _config_path: String, _deployment: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn stop(&mut self) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn generate_user_config(&mut self, _json_args: String) -> String { "".to_string() }
    fn update_user_config(&mut self, _user_config_path: String, _keystore_path: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn migrate_user_config(&mut self, _output_path: String, _keystore_path: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn migrate_user_config_0_1_2(&mut self, _new_config_path: String, _old_config_path: String, _keystore_path: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn participate(&mut self, _config_path: String, _keystore_path: String, _output_dir: String, _external_address: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn generate_key(&mut self, _user_config_path: String, _keystore_path: String, _key_type: String, _key_title: String) -> String { "".to_string() }
    fn add_key(&mut self, _user_config_path: String, _keystore_path: String, _key_type: String, _key_hex: String, _key_title: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn remove_key(&mut self, _user_config_path: String, _keystore_path: String, _key_title: String) -> Result<serde_json::Value, String> { Ok(serde_json::Value::Null) }
    fn get_peer_id(&mut self, _config_path: String) -> String { "".to_string() }
    fn wallet_get_balance(&mut self, _address_hex: String) -> String { "".to_string() }
    fn wallet_transfer_funds(&mut self, _change_public_key: String, _sender_addresses: serde_json::Value, _recipient_address: String, _amount: String, _optional_tip_hex: String) -> String { "".to_string() }
    fn wallet_get_known_addresses(&mut self) -> serde_json::Value { serde_json::Value::Null }
    fn wallet_get_notes(&mut self, _wallet_address_hex: String, _optional_tip_hex: String) -> String { "".to_string() }
    fn leader_claim(&mut self) -> String { "".to_string() }
    fn wallet_get_claimable_vouchers(&mut self) -> String { "".to_string() }
    fn wallet_fund_tx(&mut self, _request_json: String) -> String { "".to_string() }
    fn submit_signed_transaction(&mut self, _signed_tx_json: String) -> String { "".to_string() }
    fn channel_deposit(&mut self, _channel_id_hex: String, _funding_public_key_hex: String, _amount: String, _metadata_hex: String, _optional_tip_hex: String) -> String { "".to_string() }
    fn channel_deposit_with_notes(&mut self, _a: String, _b: serde_json::Value, _c: String, _d: String, _e: serde_json::Value, _f: String, _g: String) -> String { "".to_string() }
    fn get_channel_state(&mut self, _channel_id_hex: String) -> String { "".to_string() }
    fn blend_join_as_core_node(&mut self, _locator: String, _locked_note_id_hex: String) -> String { "".to_string() }
    fn get_block(&mut self, _header_id_hex: String) -> String { "".to_string() }
    fn get_blocks(&mut self, _from_slot: i64, _to_slot: i64) -> String { "".to_string() }
    fn get_transaction(&mut self, _tx_hash_hex: String) -> String { "".to_string() }
    fn get_cryptarchia_info(&mut self) -> String { "".to_string() }
    fn get_block_events(&mut self, _header_id_hex: String) -> String { "".to_string() }
    fn get_time_info(&mut self) -> String { "".to_string() }
}

#[no_mangle]
pub extern "Rust" fn logos_module_install() {
    install::<DemoImpl>();
}
