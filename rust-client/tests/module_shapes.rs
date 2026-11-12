//! The replies `blockchain_module` sends, decoded into the types the client
//! hands out. The fixtures are the samples of `mock/FFI_SHAPES.md`; a failure
//! here means the node types the client pins no longer read what the module
//! sends. `get_block` and `get_blocks` are covered by the unit tests in
//! `src/lib.rs`, next to the conversion they need.

use logos_blockchain_client::{
    reply::{self, StreamItem},
    types::{
        Accounts, BlockInfo, ChannelState, Events, LeaderAgedNotes, NetworkInfo,
        ProcessedBlockEvent, TimeInfo, WalletFundRequestBody, WalletFundResponseBody,
    },
};
use serde::de::DeserializeOwned;

macro_rules! fixture {
    ($name:literal) => {
        include_str!(concat!("fixtures/", $name, ".json"))
    };
}

/// A successful module reply carrying `json` the way the module sends it: as a string value.
fn reply_with(json: &str) -> Result<serde_json::Value, logos_blockchain_client::LogosError> {
    Ok(serde_json::json!({ "success": true, "value": json, "error": null }))
}

fn decode<T: DeserializeOwned>(json: &str) -> T {
    reply::decode::<T>("test", reply_with(json)).unwrap_or_else(|e| panic!("{e}"))
}

#[test]
fn processed_block_event_decodes() {
    let item = reply::stream_item::<ProcessedBlockEvent>("processedBlock", fixture!("processed_block"))
        .unwrap_or_else(|e| panic!("{e}"));
    let StreamItem::Item(event) = item else { panic!("expected an item") };
    assert_eq!(u64::from(event.block.header.slot), 689_412);
    assert_eq!(event.block.transactions.len(), 1);
}

#[test]
fn lib_block_decodes() {
    let item = reply::stream_item::<BlockInfo>("libBlock", fixture!("lib_block")).unwrap_or_else(|e| panic!("{e}"));
    let StreamItem::Item(info) = item else { panic!("expected an item") };
    assert_eq!(info.height, 22_984);
}

#[test]
fn block_events_decode() {
    let _events: Events = decode(fixture!("block_events"));
}

#[test]
fn channel_state_decodes() {
    let _state: ChannelState = decode(fixture!("channel_state"));
}

#[test]
fn fund_tx_request_and_response_decode() {
    let request: WalletFundRequestBody =
        serde_json::from_str(fixture!("fund_request")).unwrap_or_else(|e| panic!("{e}"));
    // The client sends the request as the node type's own JSON; it must read back.
    let sent = serde_json::to_string(&request).unwrap();
    let _: WalletFundRequestBody = serde_json::from_str(&sent).unwrap_or_else(|e| panic!("{e}"));
    let _response: WalletFundResponseBody = decode(fixture!("fund_response"));
}

#[test]
fn time_info_decodes() {
    let _info: TimeInfo = decode(fixture!("time_info"));
}

#[test]
fn network_info_decodes() {
    let info: NetworkInfo =
        decode(r#"{"n_peers":4,"n_connections":5,"n_pending_connections":1,"n_discovered_peers":9}"#);
    assert_eq!(info.n_peers, 4);
}

#[test]
fn leader_aged_notes_decode() {
    let notes: LeaderAgedNotes = decode(
        r#"{"tip":"c41a7e0b5d3f2a1908e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281","total_value":"1500",
            "notes":[{"id":"0100000000000000000000000000000000000000000000000000000000000000","value":"1500",
                      "public_key":"41fe8c0a3b1d2e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394a5b60c"}]}"#,
    );
    assert_eq!(notes.total_value, 1500);
    assert_eq!(notes.notes[0].value, 1500);
}

#[test]
fn accounts_decode() {
    let accounts: Accounts = decode(
        r#"{"accounts":[{"public_key":"41fe","title":"main","roles":["leader_funding","voucher_master"]}],
            "keystore_keys":[{"key_id":"41fe","title":"main"}]}"#,
    );
    assert_eq!(accounts.accounts[0].roles.len(), 2);
    assert_eq!(accounts.keystore_keys.len(), 1);
}

#[test]
fn text_replies_stay_text() {
    // A numeric chain id and a one-word YAML document both parse as JSON.
    assert_eq!(reply::text("get_chain_id", reply_with("7")).unwrap(), "7");
    assert_eq!(reply::text("show_config", reply_with("true")).unwrap(), "true");
    assert_eq!(reply::text("get_peer_id", reply_with("12D3KooW")).unwrap(), "12D3KooW");
    let failed = Ok(serde_json::json!({ "success": false, "value": null, "error": "The node is not running." }));
    assert!(reply::text("get_chain_id", failed).unwrap_err().is_node_not_running());
}
