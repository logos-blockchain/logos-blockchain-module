# Logos Blockchain Module

A Logos core module that wraps the [logos-blockchain](https://github.com/logos-blockchain/logos-blockchain) C bindings.

### Build and inspect

```bash
nix build '.#lgx'
```


### Use the node from another Logos instance

The module runs on the Qt-free plain transport (`"transport": "qt_remote_plain"`), so
a `logoscore` daemon can export it over peering. An app or another daemon that pairs
with that daemon imports `blockchain_module` and calls it like a local module. Its
events come along too. In the daemon's configuration:

```yaml
peering:
  control: { enabled: true, host: 0.0.0.0, port: 7443 }
  exports: { enabled: true, modules: { blockchain_module: { events: true } } }
```

Keep the control port fixed: a peer remembers the port it paired with.

- **Everything is exported.** Peering grants access per module, not per method. A
  runtime your policy lets in can call every method here, including `stop`,
  `purge_state`, the wallet methods and the key methods.
- **Calls run one at a time.** `concurrency` is `single`, and a remote caller's
  calls wait behind `start()` while it replays the stored chain.
- **Paths are the daemon's.** `start`, the config methods and the keystore methods
  take paths on the daemon's machine; use `use_persistence_paths` with
  `generate_user_config`.
