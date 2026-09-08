use logos_rust_sdk::LogosError;
use serde::de::DeserializeOwned;

#[derive(Debug, thiserror::Error)]
pub enum ModuleError {
    #[error("{method}: transport: {source}")]
    Transport {
        method: &'static str,
        #[source]
        source: LogosError,
    },
    #[error("{method}: the node is not running")]
    NodeNotRunning { method: &'static str },
    #[error("{method}: not found: {message}")]
    NotFound {
        method: &'static str,
        message: String,
    },
    #[error("{method}: {message}")]
    Module {
        method: &'static str,
        message: String,
    },
    #[error("{method}: decode reply: {message}")]
    Decode {
        method: &'static str,
        message: String,
    },
}

impl ModuleError {
    #[must_use]
    pub const fn method(&self) -> &'static str {
        match self {
            Self::Transport { method, .. }
            | Self::NodeNotRunning { method }
            | Self::NotFound { method, .. }
            | Self::Module { method, .. }
            | Self::Decode { method, .. } => method,
        }
    }

    #[must_use]
    pub const fn is_not_found(&self) -> bool {
        matches!(self, Self::NotFound { .. })
    }

    #[must_use]
    pub const fn is_node_not_running(&self) -> bool {
        matches!(self, Self::NodeNotRunning { .. })
    }

    pub(crate) fn decode(method: &'static str, e: impl std::fmt::Display) -> Self {
        Self::Decode {
            method,
            message: e.to_string(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StreamItem<T> {
    Item(T),
    End,
}

pub fn unwrap(
    method: &'static str,
    reply: Result<serde_json::Value, LogosError>,
) -> Result<serde_json::Value, ModuleError> {
    let reply = reply.map_err(|source| ModuleError::Transport { method, source })?;
    unwrap_value(method, reply)
}

pub fn unwrap_value(
    method: &'static str,
    reply: serde_json::Value,
) -> Result<serde_json::Value, ModuleError> {
    let Some(obj) = reply.as_object() else {
        return Ok(reply);
    };
    let Some(success) = obj.get("success").and_then(serde_json::Value::as_bool) else {
        return Ok(reply);
    };
    if !success {
        let message = match obj.get("error") {
            Some(serde_json::Value::String(s)) => s.clone(),
            Some(serde_json::Value::Null) | None => "unknown error".to_owned(),
            Some(other) => other.to_string(),
        };
        return Err(classify(method, message));
    }
    Ok(match obj.get("value") {
        Some(serde_json::Value::String(s)) => {
            serde_json::from_str(s).unwrap_or_else(|_| serde_json::Value::String(s.clone()))
        }
        Some(v) => v.clone(),
        None => serde_json::Value::Null,
    })
}

pub fn decode<T: DeserializeOwned>(
    method: &'static str,
    reply: Result<serde_json::Value, LogosError>,
) -> Result<T, ModuleError> {
    let value = unwrap(method, reply)?;
    serde_json::from_value(value).map_err(|e| ModuleError::decode(method, e))
}

pub fn decode_opt<T: DeserializeOwned>(
    method: &'static str,
    reply: Result<serde_json::Value, LogosError>,
) -> Result<Option<T>, ModuleError> {
    match decode(method, reply) {
        Ok(v) => Ok(Some(v)),
        Err(e) if e.is_not_found() => Ok(None),
        Err(e) => Err(e),
    }
}

pub fn unit(
    method: &'static str,
    reply: Result<serde_json::Value, LogosError>,
) -> Result<(), ModuleError> {
    unwrap(method, reply).map(|_| ())
}

/// The module reports not-found and not-running by message, not by code.
fn classify(method: &'static str, message: String) -> ModuleError {
    let lower = message.to_ascii_lowercase();
    if lower.contains("node is not running") {
        ModuleError::NodeNotRunning { method }
    } else if lower.contains("no block found")
        || lower.contains("no channel found")
        || lower.contains("not found")
    {
        ModuleError::NotFound { method, message }
    } else {
        ModuleError::Module { method, message }
    }
}

pub fn stream_item<T: DeserializeOwned>(
    event: &'static str,
    payload_json: &str,
) -> Result<StreamItem<T>, ModuleError> {
    if payload_json.trim() == "null" {
        return Ok(StreamItem::End);
    }
    serde_json::from_str(payload_json)
        .map(StreamItem::Item)
        .map_err(|e| ModuleError::decode(event, e))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn value_string_is_parsed() {
        let reply = serde_json::json!({ "success": true, "value": "{\"height\":9205}", "error": null });
        assert_eq!(unwrap_value("m", reply).unwrap()["height"], 9205);
    }

    #[test]
    fn plain_values_pass_through() {
        let reply = serde_json::json!({ "success": true, "value": true, "error": null });
        assert_eq!(unwrap_value("m", reply).unwrap(), serde_json::Value::Bool(true));
        let reply = serde_json::json!({ "success": true, "value": "abcd", "error": null });
        assert_eq!(unwrap_value("m", reply).unwrap(), serde_json::json!("abcd"));
    }

    #[test]
    fn errors_are_classified() {
        let err = |msg: &str| {
            unwrap_value("m", serde_json::json!({ "success": false, "value": null, "error": msg }))
                .unwrap_err()
        };
        assert!(err("The node is not running.").is_node_not_running());
        assert!(err("No block found for header id HeaderId(ab)").is_not_found());
        assert!(err("No channel found for id ChannelId(ab)").is_not_found());
        assert!(matches!(err("Keystore file exists."), ModuleError::Module { .. }));
    }

    #[test]
    fn stream_end_marker() {
        assert_eq!(stream_item::<u64>("e", "null").unwrap(), StreamItem::End);
        assert_eq!(stream_item::<u64>("e", "7").unwrap(), StreamItem::Item(7));
    }
}
