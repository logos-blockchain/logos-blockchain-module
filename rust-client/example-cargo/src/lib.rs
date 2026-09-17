use std::{thread, time::Duration};

use logos_blockchain_client::{BlockchainModuleClient, LogosError};

pub struct HeightWatcher {
    client: BlockchainModuleClient,
    interval: Duration,
}

impl HeightWatcher {
    pub fn new(interval: Duration) -> Self {
        Self { client: BlockchainModuleClient::new(), interval }
    }

    pub fn poll(&self) -> Result<Option<u64>, LogosError> {
        Ok(height_of(&self.client.get_cryptarchia_info()?))
    }

    pub fn spawn<F>(self, mut on_height: F) -> thread::JoinHandle<()>
    where
        F: FnMut(Result<Option<u64>, LogosError>) + Send + 'static,
    {
        thread::spawn(move || loop {
            on_height(self.poll());
            thread::sleep(self.interval);
        })
    }
}

pub fn height_of(reply: &serde_json::Value) -> Option<u64> {
    if reply.get("success")?.as_bool()? == false {
        return None;
    }
    let value = match reply.get("value")? {
        serde_json::Value::String(s) => serde_json::from_str(s).ok()?,
        v => v.clone(),
    };
    value.get("height")?.as_u64()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_height_from_the_envelope() {
        let ok = serde_json::json!({ "success": true, "value": "{\"height\":9205,\"slot\":1}", "error": null });
        assert_eq!(height_of(&ok), Some(9205));
        let refused = serde_json::json!({ "success": false, "value": null, "error": "The node is not running." });
        assert_eq!(height_of(&refused), None);
    }
}
