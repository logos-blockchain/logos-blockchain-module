cmake_minimum_required(VERSION 3.20)
project(logos-blockchain-module)

set(CMAKE_CXX_STANDARD 20)

add_library(blockchainmodulelib SHARED library.cpp)

# --- Optional Qt dependency (Qt6 preferred, fallback to Qt5) ---
# Enable this to link Qt Core into the `logos-blockchain-module` library without breaking
# environments where Qt isn't installed yet.
option(UNTITLED_USE_QT "Enable Qt (Qt6/Qt5) dependency for blockchain-module" ON)
if(UNTITLED_USE_QT)
  # Auto tools make using Qt headers/classes easier when added later
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTOUIC ON)
  set(CMAKE_AUTORCC ON)

  # Try to find Qt6 first, then Qt5. Require Core component at minimum.
  find_package(QT NAMES Qt6 Qt5 QUIET COMPONENTS Core)
  if(QT_FOUND)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
    target_link_libraries(blockchainmodulelib PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    target_compile_definitions(blockchainmodulelib PRIVATE USING_QT)
  else()
    message(WARNING "UNTITLED_USE_QT=ON but Qt was not found. Proceeding without Qt. Set CMAKE_PREFIX_PATH to your Qt installation or turn OFF the option.")
  endif()
endif()

# --- External dependency: logos-blockchain ---
# This scaffolding downloads, builds, and stages artifacts from
# https://github.com/logos-blockchain/logos-blockchain
# Fill in the TODOs (e.g., GIT_TAG and the actual produced library name).

include(ExternalProject)

set(LOGOS_STAGE_DIR    "${CMAKE_BINARY_DIR}/logos_stage")
set(LOGOS_INSTALL_DIR  "${LOGOS_STAGE_DIR}")

# Ensure staged include/lib directories exist at configure time to avoid
# INTERFACE_INCLUDE_DIRECTORIES pointing to a non-existent path errors
file(MAKE_DIRECTORY "${LOGOS_INSTALL_DIR}/include")
file(MAKE_DIRECTORY "${LOGOS_INSTALL_DIR}/lib")

# --- External dependency: logos-cpp-sdk ---
# Fetch and stage the C++ SDK so we can include "core/interface.h"
set(LOGOS_CPP_SDK_STAGE_DIR "${CMAKE_BINARY_DIR}/logos_cpp_sdk_stage")
file(MAKE_DIRECTORY "${LOGOS_CPP_SDK_STAGE_DIR}/include")

ExternalProject_Add(logos_cpp_sdk_ep
  GIT_REPOSITORY https://github.com/logos-co/logos-cpp-sdk
  UPDATE_DISCONNECTED 1
  GIT_SHALLOW OFF
  GIT_SUBMODULES ""
  GIT_SUBMODULES_RECURSE TRUE
  # The SDK is header-first. We clone it and copy headers to a stable include dir.
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
)

ExternalProject_Get_Property(logos_cpp_sdk_ep SOURCE_DIR)

# Stage headers to a predictable location in our build tree.
add_custom_command(
  OUTPUT "${LOGOS_CPP_SDK_STAGE_DIR}/include/.staged"
  COMMAND ${CMAKE_COMMAND} -E echo "Staging logos-cpp-sdk headers (core -> include/core)"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_CPP_SDK_STAGE_DIR}/include/core"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOURCE_DIR}/core" "${LOGOS_CPP_SDK_STAGE_DIR}/include/core"
  COMMAND ${CMAKE_COMMAND} -E echo "Staging logos-cpp-sdk headers (cpp -> include/cpp)"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_CPP_SDK_STAGE_DIR}/include/cpp"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOURCE_DIR}/cpp" "${LOGOS_CPP_SDK_STAGE_DIR}/include/cpp"
  COMMAND ${CMAKE_COMMAND} -E touch "${LOGOS_CPP_SDK_STAGE_DIR}/include/.staged"
  DEPENDS logos_cpp_sdk_ep
  VERBATIM
)

add_custom_target(logos_cpp_sdk_stage ALL DEPENDS "${LOGOS_CPP_SDK_STAGE_DIR}/include/.staged")

# Expose an interface target so consumers can include <core/interface.h>
add_library(logos_cpp_sdk INTERFACE)
add_dependencies(logos_cpp_sdk logos_cpp_sdk_stage)
target_include_directories(logos_cpp_sdk INTERFACE "${LOGOS_CPP_SDK_STAGE_DIR}/include")

ExternalProject_Add(logos_ep
  GIT_REPOSITORY https://github.com/logos-blockchain/logos-blockchain
  # Use the repository's default branch (no explicit tag) to avoid invalid refs
  UPDATE_DISCONNECTED 1
  GIT_SHALLOW   OFF
  GIT_SUBMODULES ""                 # ensure submodules are fetched
  GIT_SUBMODULES_RECURSE TRUE
  UPDATE_COMMAND ""                  # disable auto-update during configure
  CONFIGURE_COMMAND ""               # repo is not a CMake project; skip configure
  BUILD_COMMAND ""                   # build is driven by cargo in a separate step
  INSTALL_COMMAND ""                 # installation handled by custom staging
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LOGOS_INSTALL_DIR}
    -DCMAKE_BUILD_TYPE=$<IF:$<CONFIG:>,${CMAKE_BUILD_TYPE},Release>
    -DBUILD_SHARED_LIBS=ON           # TODO: keep/adjust depending on project options
  INSTALL_DIR ${LOGOS_INSTALL_DIR}
)

# --- Extra step: build Rust crate with cargo after download ---
# The crate lives at the repo root and we build package `nomos-c` in release mode.
# Artifacts are expected in <repo>/target/release and headers at <repo>/nomos-c/ (crate root).
ExternalProject_Get_Property(logos_ep SOURCE_DIR)

# Allow easy adjustments from CLion cache: dylib file names and header file name
# Source (as produced by cargo) and staged (the name we expose to consumers)
set(NOMOS_C_SOURCE_DYLIB_NAME "liblibnomos.dylib" CACHE STRING "Actual dylib filename produced by cargo in target/release")
set(NOMOS_C_DYLIB_NAME "libnomos.dylib" CACHE STRING "Staged dylib filename to expose to consumers")
set(NOMOS_C_HEADER_NAME "libnomos.h" CACHE STRING "Header file to stage from nomos-c crate root")

set(NOMOS_STAGED_DYLIB "${LOGOS_INSTALL_DIR}/lib/${NOMOS_C_DYLIB_NAME}")
set(NOMOS_STAGED_HEADER "${LOGOS_INSTALL_DIR}/include/${NOMOS_C_HEADER_NAME}")

# Provide actual rules that produce each staged file independently so the
# build regenerates whichever is missing. Each custom command uses the same
# cargo build step (idempotent if already built) and then copies the needed
# artifact. Declaring them separately ensures CMake will re-run the rule when
# either output is absent.

add_custom_command(
  OUTPUT "${NOMOS_STAGED_DYLIB}"
  COMMAND ${CMAKE_COMMAND} -E echo "Ensuring libnomos.dylib exists (building nomos-c if needed)"
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TERM_COLOR=always cargo build --release --package nomos-c
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_INSTALL_DIR}/lib"
  COMMAND ${CMAKE_COMMAND} -E copy "${SOURCE_DIR}/target/release/${NOMOS_C_SOURCE_DYLIB_NAME}" "${LOGOS_INSTALL_DIR}/lib/${NOMOS_C_DYLIB_NAME}"
  DEPENDS logos_ep
  WORKING_DIRECTORY ${SOURCE_DIR}
  VERBATIM
)

add_custom_command(
  OUTPUT "${NOMOS_STAGED_HEADER}"
  COMMAND ${CMAKE_COMMAND} -E echo "Ensuring libnomos.h exists (building/staging if needed)"
  # Building also ensures any generated headers are up-to-date; harmless if not needed
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TERM_COLOR=always cargo build --release --package nomos-c
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_INSTALL_DIR}/include"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SOURCE_DIR}/nomos-c/${NOMOS_C_HEADER_NAME}" "${LOGOS_INSTALL_DIR}/include/"
  DEPENDS logos_ep
  WORKING_DIRECTORY ${SOURCE_DIR}
  VERBATIM
)

add_custom_target(logos_cargo_build ALL
  DEPENDS "${NOMOS_STAGED_DYLIB}" "${NOMOS_STAGED_HEADER}"
)

# Import the staged dynamic library from the cargo build
set(LOGOS_IMPORTED_LIB "${LOGOS_INSTALL_DIR}/lib/${NOMOS_C_DYLIB_NAME}")
add_library(logos SHARED IMPORTED GLOBAL
        )
set_target_properties(logos PROPERTIES
  IMPORTED_LOCATION "${LOGOS_IMPORTED_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_INSTALL_DIR}/include"
)
add_dependencies(logos logos_cargo_build)

# Link the imported dependency if you plan to use it from this target
target_link_libraries(blockchainmodulelib PRIVATE logos)

# Make SDK headers available to this target and ensure they are staged before build
target_link_libraries(blockchainmodulelib PRIVATE logos_cpp_sdk)
add_dependencies(blockchainmodulelib logos_cpp_sdk_stage)

# Ensure our C++ target waits for the cargo-built artifacts to be staged before linking
add_dependencies(blockchainmodulelib logos_cargo_build)

# --- Post-build: copy the compiled library to project root ---
add_custom_command(TARGET blockchainmodulelib POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:blockchainmodulelib>"
          "${CMAKE_SOURCE_DIR}/$<TARGET_FILE_NAME:blockchainmodulelib>"
  COMMENT "Copying $<TARGET_FILE_NAME:blockchainmodulelib> to project root"
  VERBATIM)

# Optional manual staging template (use if the project doesn't install what you need)
# ExternalProject_Get_Property(logos_ep BINARY_DIR SOURCE_DIR)
# add_custom_target(logos_stage_manual ALL
#   COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_INSTALL_DIR}/lib"
#   COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_INSTALL_DIR}/include"
#   # TODO: copy actual produced dylib from ${BINARY_DIR} to ${LOGOS_INSTALL_DIR}/lib
#   # COMMAND ${CMAKE_COMMAND} -E copy "${BINARY_DIR}/path/to/libactual.dylib" "${LOGOS_INSTALL_DIR}/lib/"
#   # TODO: copy public headers from ${SOURCE_DIR}/include (or other) to staged include dir
#   # COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOURCE_DIR}/include" "${LOGOS_INSTALL_DIR}/include"
#   DEPENDS logos_ep
#   VERBATIM
# )
# add_dependencies(logos logos_stage_manual)
