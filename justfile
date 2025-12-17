default:
    just build

# Configure the build directory (run this once, or whenever CMake config changes)
configure:
    cmake -S . -B build -G "Unix Makefiles"

build:
    cmake --build build --parallel --target blockchainmodulelib

clean:
    cmake --build build --target clean
