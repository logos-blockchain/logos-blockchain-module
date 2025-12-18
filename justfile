default:
    just build

# One-time (or when CMakeLists.txt changes)
configure:
    test -n "${LOGOS_CPP_SDK_ROOT}" || (echo "LOGOS_CPP_SDK_ROOT not set" && exit 1)
    test -n "${LOGOS_BLOCKCHAIN_ROOT}" || (echo "LOGOS_BLOCKCHAIN_ROOT not set" && exit 1)
    cmake -S . -B build -G Ninja \
      -DUNTITLED_USE_QT=ON \
      -DLOGOS_CPP_SDK_ROOT="${LOGOS_CPP_SDK_ROOT}" \
      -DLOGOS_BLOCKCHAIN_ROOT="${LOGOS_BLOCKCHAIN_ROOT}" \
      -DCOPY_PLUGIN_TO_SOURCE_DIR=ON

# Build only (assumes configure already ran)
build:
    cmake --build build --parallel --target blockchainmodulelib

# Build via Nix
nix:
    nix build .#default -L

# Enter dev shell
dev:
    nix develop .#

clean:
    rm -rf build
    rm -f libblockchainmodulelib.so
