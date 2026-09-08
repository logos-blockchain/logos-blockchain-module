use std::sync::Arc;

use async_trait::async_trait;
use futures::stream;
use logos_blockchain_client::{
    BlockchainModuleClient, EventData, EventSubscription, LogosError, ModuleError, StreamItem,
};
use logos_blockchain_zone_sdk::{
    adapter::{BoxStream, Node},
    node_types::{
        ApiBlock, BlockInfo, ChainServiceInfo, ChannelId, ChannelState, Error, Events, HeaderId,
        ProcessedBlockEvent, SignedMantleTx, Slot, TimeInfo, Unverified, WalletFundRequestBody,
        WalletFundResponseBody,
    },
};
use tokio::sync::{mpsc, oneshot};
use tracing::{debug, warn};

const TARGET: &str = "zone_sdk::module_backend";

type Callback<T> = Box<dyn FnOnce(Result<T, ModuleError>) + Send + 'static>;

#[derive(Clone)]
pub struct NodeModuleClient {
    module: Arc<str>,
    client: Arc<BlockchainModuleClient>,
}

impl Default for NodeModuleClient {
    fn default() -> Self {
        Self::new()
    }
}

impl NodeModuleClient {
    #[must_use]
    pub fn new() -> Self {
        Self::bind("blockchain_module")
    }

    #[must_use]
    pub fn bind(module_name: &str) -> Self {
        Self {
            module: Arc::from(module_name),
            client: Arc::new(BlockchainModuleClient::bind(module_name)),
        }
    }

    #[must_use]
    pub fn module_name(&self) -> &str {
        &self.module
    }

    async fn call<T, S>(&self, start: S) -> Result<T, Error>
    where
        T: Send + 'static,
        S: FnOnce(&BlockchainModuleClient, Callback<T>),
    {
        let (tx, rx) = oneshot::channel::<Result<T, ModuleError>>();
        start(
            &self.client,
            Box::new(move |result| {
                let _ = tx.send(result);
            }),
        );
        rx.await
            .map_err(|_| Error::Client("module reply channel closed".to_owned()))?
            .map_err(to_sdk_error)
    }

    fn event_stream<T, Sub, Dec>(
        &self,
        event: &'static str,
        subscribe: Sub,
        decode_item: Dec,
    ) -> Result<BoxStream<T>, Error>
    where
        T: Send + 'static,
        Sub: FnOnce(&mut BlockchainModuleClient) -> Result<EventSubscription, LogosError>,
        Dec: Fn(&EventData) -> Result<StreamItem<T>, ModuleError> + Send + 'static,
    {
        let mut client = BlockchainModuleClient::bind(&self.module);
        let subscription =
            subscribe(&mut client).map_err(|e| Error::Client(format!("{event}: subscribe: {e}")))?;

        let (tx, rx) = mpsc::unbounded_channel::<T>();
        std::thread::Builder::new()
            .name(format!("zone-sdk-{event}"))
            .spawn(move || {
                for ev in subscription {
                    match decode_item(&ev) {
                        Ok(StreamItem::Item(item)) => {
                            if tx.send(item).is_err() {
                                break;
                            }
                        }
                        Ok(StreamItem::End) => {
                            debug!(target: TARGET, event, "module ended the stream");
                            break;
                        }
                        Err(e) => {
                            warn!(target: TARGET, event, error = %e, "undecodable event, skipping");
                        }
                    }
                }
            })
            .map_err(|e| Error::Client(format!("{event}: spawn listener thread: {e}")))?;

        Ok(Box::pin(stream::unfold(rx, |mut rx| async move {
            rx.recv().await.map(|item| (item, rx))
        })))
    }
}

fn to_sdk_error(e: ModuleError) -> Error {
    match e {
        ModuleError::Transport { .. } => Error::Client(e.to_string()),
        ModuleError::NodeNotRunning { .. }
        | ModuleError::NotFound { .. }
        | ModuleError::Module { .. }
        | ModuleError::Decode { .. } => Error::Server(e.to_string()),
    }
}

#[async_trait]
impl Node for NodeModuleClient {
    async fn consensus_info(&self) -> Result<ChainServiceInfo, Error> {
        self.call(|c, cb| c.cryptarchia_info_async(cb))
            .await
            .map(|info| info.to_chain_service_info())
    }

    async fn time_info(&self) -> Result<TimeInfo, Error> {
        self.call(|c, cb| c.time_info_async(cb)).await
    }

    async fn channel_state(&self, channel_id: ChannelId) -> Result<Option<ChannelState>, Error> {
        self.call(|c, cb| c.channel_state_async(channel_id, cb)).await
    }

    async fn block_stream(&self) -> Result<BoxStream<ProcessedBlockEvent>, Error> {
        self.event_stream(
            "processedBlock",
            |c| c.on_processed_block(),
            BlockchainModuleClient::processed_block_item,
        )
    }

    async fn lib_stream(&self) -> Result<BoxStream<BlockInfo>, Error> {
        self.event_stream(
            "libBlock",
            |c| c.on_lib_block(),
            BlockchainModuleClient::lib_block_item,
        )
    }

    async fn block(&self, id: HeaderId) -> Result<Option<ApiBlock>, Error> {
        self.call(|c, cb| c.block_async(id, cb)).await
    }

    async fn block_events(&self, id: HeaderId) -> Result<Option<Events>, Error> {
        self.call(|c, cb| c.block_events_async(id, cb)).await
    }

    async fn immutable_blocks(
        &self,
        slot_from: Slot,
        slot_to: Slot,
    ) -> Result<Vec<ApiBlock>, Error> {
        self.call(|c, cb| c.blocks_async(slot_from, slot_to, cb)).await
    }

    async fn post_transaction(&self, tx: SignedMantleTx<Unverified>) -> Result<(), Error> {
        self.call(|c, cb| c.submit_transaction_async(&tx, cb))
            .await
            .map(|_hash| ())
    }

    async fn fund_tx(
        &self,
        request: WalletFundRequestBody,
    ) -> Result<WalletFundResponseBody, Error> {
        self.call(|c, cb| c.fund_tx_async(&request, cb)).await
    }
}
