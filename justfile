default: build

configure:
    test -n "${LOGOS_CPP_SDK_ROOT}" || (echo "LOGOS_CPP_SDK_ROOT not set" && exit 1)
    test -n "${LOGOS_BLOCKCHAIN_ROOT}" || (echo "LOGOS_BLOCKCHAIN_ROOT not set" && exit 1)
    cmake -S . -B build -G Ninja \
      -DUNTITLED_USE_QT=ON \
      -DLOGOS_CPP_SDK_ROOT="${LOGOS_CPP_SDK_ROOT}" \
      -DLOGOS_BLOCKCHAIN_ROOT="${LOGOS_BLOCKCHAIN_ROOT}" \
      -DCOPY_PLUGIN_TO_SOURCE_DIR=ON

build:
    cmake --build build --parallel --target blockchainmodulelib

update:
    rm -rf build/logos_blockchain_src
    rm -f build/logos_blockchain_src/.staged
    rm -rf build/logos_stage
    cmake --build build --parallel --target logos_blockchain_stage
    cmake --build build --parallel --target logos_cargo_build
    just build

clean:
    rm -rf build
    rm -f libblockchainmodulelib.so

rebuild: clean configure build

run:
    ../logos-module-viewer/result/bin/logos-module-viewer --module libblockchainmodulelib.so > libblockchainmodulelib.log 2>&1
