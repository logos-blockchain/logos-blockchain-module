cmake_minimum_required(VERSION 3.14)
project(BlockchainClientExamplePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LOGOS_MODULE_BUILDER_ROOT is not set.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json
               ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

logos_module(
    NAME blockchain_client_example
)

# Rust staticlib staged into lib/ by the flake (or built with cargo).
find_library(LIBBLOCKCHAIN_CLIENT_EXAMPLE
    NAMES libblockchain_client_example.a blockchain_client_example
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target/release
        ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target/debug
    NO_DEFAULT_PATH
)

if(NOT LIBBLOCKCHAIN_CLIENT_EXAMPLE)
    message(FATAL_ERROR
        "Rust static library (libblockchain_client_example.a) not found in ${CMAKE_CURRENT_SOURCE_DIR}/lib. "
        "Ensure the preConfigure hook staged the buildRustPackage output."
    )
endif()

target_link_libraries(blockchain_client_example_module_plugin PRIVATE
    ${LIBBLOCKCHAIN_CLIENT_EXAMPLE}
    logos-protocol::logos_protocol
)

if(APPLE)
    target_link_libraries(blockchain_client_example_module_plugin PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
    )
else()
    target_link_libraries(blockchain_client_example_module_plugin PRIVATE pthread dl)
endif()
