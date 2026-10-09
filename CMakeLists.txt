cmake_minimum_required(VERSION 3.14)
project(LogosBlockchainModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

set(BLOCKCHAIN_MODULE_SOURCES
    src/logos_blockchain_module.h
    src/logos_blockchain_module.cpp
    src/user_config_reader.h
    src/user_config_reader.cpp
)

# The mock variant (`nix build .#mock`) answers the node half of the C API from
# a simulated chain. See mock/README.md.
set(BLOCKCHAIN_MODULE_LINK_LIBRARIES fyaml)
if(DEFINED ENV{LOGOS_BLOCKCHAIN_MOCK})
    list(APPEND BLOCKCHAIN_MODULE_SOURCES
        mock/src/mock_chain.h
        mock/src/mock_chain.cpp
        mock/src/mock_ffi.cpp
        src/proposed_ffi.h
    )
    list(APPEND BLOCKCHAIN_MODULE_LINK_LIBRARIES ${CMAKE_DL_LIBS})
endif()

# Universal module — generated_code/ is picked up automatically by LogosModule.cmake
logos_module(
    NAME blockchain_module
    SOURCES
        ${BLOCKCHAIN_MODULE_SOURCES}
    EXTERNAL_LIBS
        logos_blockchain
    LINK_LIBRARIES
        ${BLOCKCHAIN_MODULE_LINK_LIBRARIES}
)

if(DEFINED ENV{LOGOS_BLOCKCHAIN_MOCK})
    target_compile_definitions(blockchain_module_module_plugin PRIVATE LOGOS_BLOCKCHAIN_PROPOSED_FFI)
endif()
