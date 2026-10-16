# Logos SQL module

`sql_module` exposes Logos SQL to Basecamp applications. It connects to a node's HTTP API; it does not start a node or depend on `blockchain_module`.

## Build

From the repository root:

```sh
nix build .#sql
nix build .#sql-lgx-portable
nix build .#sql-unit-tests
```

The `logos-blockchain` input must provide the `logos-sql-c` package. Until the C bindings are published, use a local checkout containing them:

```sh
nix build .#sql --override-input logos-blockchain path:/path/to/logos-blockchain --no-write-lock-file
```

The existing node targets remain unchanged. `sql/flake.nix` exposes just the SQL module for an application's dependency input, following the same pattern as `mock/flake.nix`:

```nix
inputs.sql_module.url = "github:logos-blockchain/logos-blockchain-module?dir=sql";
```

The consuming app lists `sql_module` in its metadata dependencies.

## Open and query a channel

Call `open(config_json, timeout_ms)` with the C binding's configuration:

```json
{
  "channel_id": "<64 hex digits>",
  "node_url": "http://127.0.0.1:8080",
  "state_dir": "/path/to/channel-replica"
}
```

Replicas are read-only. SQL parameter formats are documented in [the C bindings](https://github.com/logos-blockchain/logos-blockchain/tree/master/logos_sql/c-bindings).

All methods return `StdLogosResult`. On success, `open` returns an opaque replica ID in `value`; pass it to subsequent calls. Open another replica to browse another channel, and call `close` when finished. Closing removes the handle even if shutdown reports an error. Module teardown closes any remaining replicas.

Use a distinct state directory per replica. The module rejects directories already open in this module instance. Do not share a directory with another process or module instance.

`query(database_id, view, sql, parameters_json, max_rows, max_result_bytes)` accepts `"latest"` or `"finalized"`. For example, use `SELECT name, sql FROM sqlite_schema WHERE type = 'table' ORDER BY name` with parameters `[]` to browse tables. The result value contains `columns`, typed `rows`, and `truncated`. Integers remain decimal strings and blobs remain hex, without losing precision through JSON.

`max_rows` limits the number of returned rows (1–100,000). `max_result_bytes` must be positive and limits their serialized data size; exceeding it returns an error rather than a partial result.

Calls are blocking and serialized within the module. The UI should invoke them asynchronously through the module API. There is no polling loop or automatic retry in this module.

## Tests

The SQL tests use the existing `LogosTest` framework with a mock C library. They cover replica ownership, query/result conversion and error messages. `tests/stubs/logos_sql.h` is the generated C header; keep it in sync with the bindings when changing that API.
