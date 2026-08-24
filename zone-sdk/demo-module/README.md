# Demo Module

This is a demo module for testing the Logos Rust SDK integration.

## Building the Module

This project uses `Nix` and `CMake` for building.

### Prerequisites
- Ensure you have `Nix` installed.
- Ensure you are in the project root directory.

### Build Steps

1. **Enter the development shell:**
   ```bash
   nix develop
   ```

2. **Generate necessary source files:**
   ```bash
   nix run .#generate
   ```

3. **Build the Rust library:**
   ```bash
   cd zone-sdk/demo-module/rust-lib
   cargo build --release
   # Copy the static library to the expected location for CMake
   cp target/release/libdemo_module.a ../lib/
   cd ../../..
   ```

4. **Configure and build the module with CMake:**
   ```bash
   cmake -S zone-sdk/demo-module -B zone-sdk/demo-module/build -G Ninja
   cmake --build zone-sdk/demo-module/build --parallel
   ```

The resulting plugin will be located at `zone-sdk/demo-module/build/modules/demo_module_plugin.dylib`.
