//! Rust client for the Logos `blockchain_module`.
//!
//! `generated.rs` is emitted by `logos-lidl-gen` from `blockchain_module.lidl`
//! (`nix run .#generate`). The client speaks the logos-protocol `lp_*` ABI, so
//! it runs inside a Logos module loaded next to `blockchain_module`.

pub mod generated;

pub use generated::BlockchainModuleClient;
pub use logos_rust_sdk::{EventData, EventSubscription, LogosError};
