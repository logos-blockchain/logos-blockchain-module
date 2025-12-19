cmake_minimum_required(VERSION 3.20)
project(logos-blockchain-module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------
# Options / Inputs
# -------------------------
option(UNTITLED_USE_QT "Enable Qt6 dependency" ON)
option(COPY_PLUGIN_TO_SOURCE_DIR "Copy built plugin to source dir root (dev convenience)" ON)

# Default OFF: keep normal builds fast; deps are built when needed by link step,
# and you can force-refresh via your Justfile update target.
option(LOGOS_BUILD_DEPS_BY_DEFAULT "Force stage+cargo targets as explicit deps of the plugin" OFF)

set(LOGOS_CPP_SDK_ROOT "" CACHE PATH "Path to logos-cpp-sdk package root (must contain include/core and include/cpp)")
set(LOGOS_BLOCKCHAIN_ROOT "" CACHE PATH "Path to logos-blockchain source root (read-only in Nix store is OK)")

if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  message(FATAL_ERROR "LOGOS_CPP_SDK_ROOT not set. Pass -DLOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk")
endif()

if(NOT DEFINED LOGOS_BLOCKCHAIN_ROOT OR LOGOS_BLOCKCHAIN_ROOT STREQUAL "")
  message(FATAL_ERROR "LOGOS_BLOCKCHAIN_ROOT not set. Pass -DLOGOS_BLOCKCHAIN_ROOT=/path/to/logos-blockchain")
endif()

# -------------------------
# Qt6
# -------------------------
if(UNTITLED_USE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTOUIC ON)
  set(CMAKE_AUTORCC ON)
else()
  message(FATAL_ERROR "UNTITLED_USE_QT=OFF is not supported for this module (it's a Qt plugin).")
endif()

# -------------------------
# Writable stage of logos-blockchain source
# (Fixes: build.rs/cbindgen writing into source dir inside /nix/store)
# -------------------------
set(LOGOS_BLOCKCHAIN_WORKDIR     "${CMAKE_BINARY_DIR}/logos_blockchain_src")
set(LOGOS_BLOCKCHAIN_STAGE_STAMP "${LOGOS_BLOCKCHAIN_WORKDIR}/.staged")

# NOTE:
# - We do NOT rm -rf here; normal builds stay stable.
# - Force refresh is handled by deleting workdir/stamp in your Justfile update target.
add_custom_command(
        OUTPUT "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E echo "Staging logos-blockchain into writable dir: ${LOGOS_BLOCKCHAIN_WORKDIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_BLOCKCHAIN_WORKDIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LOGOS_BLOCKCHAIN_ROOT}" "${LOGOS_BLOCKCHAIN_WORKDIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
        VERBATIM
)

# Not ALL: only runs when required.
add_custom_target(logos_blockchain_stage
        DEPENDS "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
)

# -------------------------
# Build nomos-c (Rust) and stage outputs
# -------------------------
set(LOGOS_STAGE_DIR   "${CMAKE_BINARY_DIR}/logos_stage")
set(LOGOS_INSTALL_DIR "${LOGOS_STAGE_DIR}")
file(MAKE_DIRECTORY "${LOGOS_INSTALL_DIR}/include")
file(MAKE_DIRECTORY "${LOGOS_INSTALL_DIR}/lib")

# Platform naming
if(APPLE)
  set(NOMOS_EXT ".dylib")
elseif(WIN32)
  set(NOMOS_EXT ".dll")
else()
  set(NOMOS_EXT ".so")
endif()

# What cargo produces and what we stage as
set(NOMOS_C_SOURCE_DYLIB_NAME "liblibnomos${NOMOS_EXT}" CACHE STRING "Shared library filename produced by cargo in target/release")
set(NOMOS_C_STAGED_DYLIB_NAME "libnomos${NOMOS_EXT}" CACHE STRING "Staged shared library filename")
set(NOMOS_C_HEADER_NAME "libnomos.h" CACHE STRING "Header file name (in nomos-c crate root)")

set(NOMOS_STAGED_DYLIB  "${LOGOS_INSTALL_DIR}/lib/${NOMOS_C_STAGED_DYLIB_NAME}")
set(NOMOS_STAGED_HEADER "${LOGOS_INSTALL_DIR}/include/${NOMOS_C_HEADER_NAME}")

# Build output dir for cargo (writable)
set(CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-target")

# Linux-only SONAME patching command list
set(PATCHELF_COMMANDS "")
if(UNIX AND NOT APPLE AND NOT WIN32)
  find_program(PATCHELF_EXE patchelf REQUIRED)
  set(PATCHELF_COMMANDS
          COMMAND ${CMAKE_COMMAND} -E echo "Patching SONAME of ${NOMOS_C_STAGED_DYLIB_NAME}"
          COMMAND ${PATCHELF_EXE} --set-soname "${NOMOS_C_STAGED_DYLIB_NAME}" "${NOMOS_STAGED_DYLIB}"
  )
endif()

add_custom_command(
        OUTPUT "${NOMOS_STAGED_DYLIB}"
        COMMAND ${CMAKE_COMMAND} -E echo "Building nomos-c (Rust) from writable copy: ${LOGOS_BLOCKCHAIN_WORKDIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CARGO_TARGET_DIR}"
        COMMAND ${CMAKE_COMMAND} -E env
        CARGO_TERM_COLOR=always
        CARGO_TARGET_DIR=${CARGO_TARGET_DIR}
        cargo build --release --package nomos-c
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_INSTALL_DIR}/lib"
        COMMAND ${CMAKE_COMMAND} -E copy
        "${CARGO_TARGET_DIR}/release/${NOMOS_C_SOURCE_DYLIB_NAME}"
        "${NOMOS_STAGED_DYLIB}"
        ${PATCHELF_COMMANDS}
        DEPENDS "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
        WORKING_DIRECTORY "${LOGOS_BLOCKCHAIN_WORKDIR}"
        VERBATIM
)

add_custom_command(
        OUTPUT "${NOMOS_STAGED_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E echo "Staging ${NOMOS_C_HEADER_NAME} from writable copy"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_INSTALL_DIR}/include"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LOGOS_BLOCKCHAIN_WORKDIR}/nomos-c/${NOMOS_C_HEADER_NAME}"
        "${NOMOS_STAGED_HEADER}"
        DEPENDS "${NOMOS_STAGED_DYLIB}"
        VERBATIM
)

# Not ALL: only runs when required.
add_custom_target(logos_cargo_build
        DEPENDS "${NOMOS_STAGED_DYLIB}" "${NOMOS_STAGED_HEADER}"
)

# Imported library for consumers (this is what the plugin links)
add_library(nomos_c SHARED IMPORTED GLOBAL)
set_target_properties(nomos_c PROPERTIES
        IMPORTED_LOCATION "${NOMOS_STAGED_DYLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_INSTALL_DIR}/include"
)

# Ensure the imported target is materialized before consumers link
add_dependencies(nomos_c logos_cargo_build)

# -------------------------
# logos-cpp-sdk headers (viewer-style)
# -------------------------
add_library(logos_cpp_sdk INTERFACE)
target_include_directories(logos_cpp_sdk INTERFACE
        "${LOGOS_CPP_SDK_ROOT}/include/core"
        "${LOGOS_CPP_SDK_ROOT}/include/cpp"
        "${LOGOS_CPP_SDK_ROOT}/include"
)

# -------------------------
# Qt plugin target
# -------------------------
set(PLUGIN_TARGET blockchainmodulelib)

qt_add_plugin(${PLUGIN_TARGET}
        CLASS_NAME LogosBlockchainModule
)

target_sources(${PLUGIN_TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/library.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
)

target_link_libraries(${PLUGIN_TARGET} PRIVATE
        Qt6::Core
        nomos_c
        logos_cpp_sdk
)

target_compile_definitions(${PLUGIN_TARGET} PRIVATE USING_QT)

set_target_properties(${PLUGIN_TARGET} PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON
)

# Optional: force deps as explicit build dependencies of the plugin target.
# This is mainly useful if you want "build plugin" to always stage+build first.
if(LOGOS_BUILD_DEPS_BY_DEFAULT)
  add_dependencies(${PLUGIN_TARGET} logos_blockchain_stage logos_cargo_build)
endif()

# Runtime: prefer libnomos next to the plugin
if(APPLE)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
          BUILD_RPATH "@loader_path"
          INSTALL_RPATH "@loader_path"
  )
elseif(UNIX)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
          BUILD_RPATH "$ORIGIN"
          INSTALL_RPATH "$ORIGIN"
  )
endif()

# Copy libnomos next to plugin in build output for runtime loading
add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NOMOS_STAGED_DYLIB}"
        "$<TARGET_FILE_DIR:${PLUGIN_TARGET}>/${NOMOS_C_STAGED_DYLIB_NAME}"
        COMMENT "Copying ${NOMOS_C_STAGED_DYLIB_NAME} next to plugin for runtime loading"
        VERBATIM
)

# Dev convenience: copy the plugin to repo root
if(COPY_PLUGIN_TO_SOURCE_DIR)
  add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:${PLUGIN_TARGET}>"
          "${CMAKE_SOURCE_DIR}/$<TARGET_FILE_NAME:${PLUGIN_TARGET}>"
          COMMENT "Copying plugin to source root"
          VERBATIM
  )
endif()
