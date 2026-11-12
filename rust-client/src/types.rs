use lb_common_http_client::{ApiHeader, ApiSignedHeader};
pub use lb_common_http_client::{
    ApiBlock, BlockInfo, ChainServiceInfo, CryptarchiaInfo as NodeCryptarchiaInfo, Events,
    PhaseTag, ProcessedBlockEvent, Slot, State, TimeInfo,
};
use lb_core::{block::SignedHeader, header::Header};
pub use lb_core::{
    header::HeaderId,
    mantle::{
        NoteId, SignedOps, TxHash, Value, channel::ChannelState, ledger::verification_mode::StandardMode,
        ops::channel::ChannelId, transactions::states::Unverified,
    },
};
pub use lb_http_api_common::bodies::wallet::{
    claimable_vouchers::ClaimableVoucherInfoResponseBody as ClaimableVoucher,
    fund::{WalletFundRequestBody, WalletFundResponseBody},
};
use serde::{Deserialize, Deserializer};

/// A block as `get_block` and `get_blocks` send it: the node's stored block,
/// whose headers carry no id. The id is the header's hash, so it is computed
/// here to hand out the same [`ApiBlock`] the block streams and the node's
/// HTTP API give.
///
/// The node's own `Block` is not used to read it: that type re-checks the
/// body root and the signature on every read, which the node already did
/// before storing the block.
#[derive(Deserialize)]
pub(crate) struct StoredBlock {
    header: Header,
    #[serde(default)]
    uncle_headers: Vec<SignedHeader>,
    transactions: Vec<SignedOps<Unverified, StandardMode>>,
}

fn api_header(header: &Header) -> ApiHeader {
    ApiHeader {
        id: header.id(),
        parent_block: header.parent_block(),
        slot: header.slot(),
        body_root: *header.body_root(),
        proof_of_leadership: header.leader_proof().clone(),
    }
}

impl From<StoredBlock> for ApiBlock {
    fn from(block: StoredBlock) -> Self {
        Self {
            header: api_header(&block.header),
            uncle_headers: block
                .uncle_headers
                .iter()
                .map(|uncle| ApiSignedHeader {
                    header: api_header(uncle.header()),
                    signature: *uncle.signature(),
                })
                .collect(),
            transactions: block.transactions,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
pub enum Mode {
    Bootstrapping,
    Online,
    NotStarted,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct CryptarchiaInfo {
    pub lib: HeaderId,
    pub lib_slot: Slot,
    pub tip: HeaderId,
    pub slot: Slot,
    pub height: u64,
    pub mode: Mode,
}

// TODO: Types in this file are defined only because module does not map them directly to the
// types in logos-blockchain crate. All other maps, ideally we'll make blockchain-module to reuse
// all remaining structs.
impl CryptarchiaInfo {
    #[must_use]
    pub fn to_chain_service_info(&self) -> ChainServiceInfo {
        let (state, phase) = match self.mode {
            Mode::Online => (State::Online, PhaseTag::Following),
            Mode::Bootstrapping => (State::Bootstrapping, PhaseTag::ProlongedBootstrapPeriod),
            Mode::NotStarted => (State::Bootstrapping, PhaseTag::AwaitingGenesisTime),
        };
        ChainServiceInfo {
            cryptarchia_info: NodeCryptarchiaInfo {
                lib: self.lib,
                lib_slot: self.lib_slot,
                tip: self.tip,
                slot: self.slot,
                height: self.height,
                state,
            },
            phase,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct WalletNote {
    pub id: NoteId,
    #[serde(deserialize_with = "u64_from_string_or_number")]
    pub value: Value,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct WalletNotes {
    pub tip: HeaderId,
    pub notes: Vec<WalletNote>,
}

#[derive(Deserialize)]
pub struct ClaimableVouchers {
    pub tip: HeaderId,
    pub vouchers: Vec<ClaimableVoucher>,
    #[serde(deserialize_with = "u64_from_string_or_number")]
    pub reward_amount: Value,
    #[serde(deserialize_with = "u64_from_string_or_number")]
    pub total_claimable: Value,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct LeaderAgedNote {
    pub id: NoteId,
    #[serde(deserialize_with = "u64_from_string_or_number")]
    pub value: Value,
    pub public_key: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct LeaderAgedNotes {
    pub tip: HeaderId,
    #[serde(deserialize_with = "u64_from_string_or_number")]
    pub total_value: Value,
    pub notes: Vec<LeaderAgedNote>,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct NetworkInfo {
    pub n_peers: u64,
    pub n_connections: u64,
    pub n_pending_connections: u64,
    pub n_discovered_peers: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Account {
    pub public_key: String,
    pub title: String,
    pub roles: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct KeystoreKey {
    pub key_id: String,
    pub title: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Accounts {
    pub accounts: Vec<Account>,
    pub keystore_keys: Vec<KeystoreKey>,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct PowClaimableRewards {
    pub claimable_tickets: u64,
    pub slots_until_expiry: Vec<u64>,
}

fn u64_from_string_or_number<'de, D: Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Raw {
        Number(u64),
        Text(String),
    }
    match Raw::deserialize(d)? {
        Raw::Number(n) => Ok(n),
        Raw::Text(s) => s.trim().parse().map_err(serde::de::Error::custom),
    }
}

pub(crate) fn parse_amount(method: &'static str, v: &serde_json::Value) -> Result<Value, crate::ModuleError> {
    match v {
        serde_json::Value::Number(n) => n
            .as_u64()
            .ok_or_else(|| crate::ModuleError::decode(method, format!("not a u64: {n}"))),
        serde_json::Value::String(s) => s
            .trim()
            .parse()
            .map_err(|e| crate::ModuleError::decode(method, format!("{e}: {s:?}"))),
        other => Err(crate::ModuleError::decode(method, format!("expected amount, got {other}"))),
    }
}

pub(crate) fn id_hex<T: serde::Serialize>(method: &'static str, id: &T) -> Result<String, crate::ModuleError> {
    match serde_json::to_value(id) {
        Ok(serde_json::Value::String(s)) => Ok(s),
        Ok(other) => Err(crate::ModuleError::decode(method, format!("id is not a hex string: {other}"))),
        Err(e) => Err(crate::ModuleError::decode(method, e)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const H: &str = "18166a9d227daa68de7cbd12097b497fe5f43f381872924c5faeefddfb3b55fd";
    // Note ids and vouchers are field elements, so use a small one.
    const N: &str = "0100000000000000000000000000000000000000000000000000000000000000";

    #[test]
    fn cryptarchia_info_decodes_the_module_json() {
        let json = format!(
            r#"{{"height":9205,"lib":"{H}","lib_slot":299599,"mode":"Bootstrapping","slot":299599,"tip":"{H}"}}"#
        );
        let info: CryptarchiaInfo = serde_json::from_str(&json).unwrap();
        assert_eq!(info.height, 9205);
        assert_eq!(info.mode, Mode::Bootstrapping);
        assert_eq!(info.slot, Slot::from(299_599));
        assert_eq!(id_hex("t", &info.tip).unwrap(), H);
        let csi = info.to_chain_service_info();
        assert_eq!(csi.cryptarchia_info.state, State::Bootstrapping);
        assert_eq!(csi.phase, PhaseTag::ProlongedBootstrapPeriod);
    }

    #[test]
    fn wallet_notes_decode_string_values() {
        let json = format!(r#"{{"tip":"{H}","notes":[{{"id":"{N}","value":"42"}}]}}"#);
        let notes: WalletNotes = serde_json::from_str(&json).unwrap();
        assert_eq!(notes.notes[0].value, 42);
        assert_eq!(id_hex("t", &notes.notes[0].id).unwrap(), N);
    }

    #[test]
    fn vouchers_and_rewards_decode() {
        let json = format!(
            r#"{{"tip":"{H}","vouchers":[{{"commitment":"{N}","nullifier":"{N}"}}],"reward_amount":"5","total_claimable":"5"}}"#
        );
        let v: ClaimableVouchers = serde_json::from_str(&json).unwrap();
        assert_eq!(id_hex("t", &v.vouchers[0].commitment).unwrap(), N);
        assert_eq!(v.total_claimable, 5);
        let r: PowClaimableRewards =
            serde_json::from_str(r#"{"claimable_tickets":2,"slots_until_expiry":[10,20]}"#).unwrap();
        assert_eq!(r.slots_until_expiry, vec![10, 20]);
    }
}
