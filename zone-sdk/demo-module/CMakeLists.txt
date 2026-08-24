cmake_minimum_required(VERSION 3.14)
project(DemoModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LOGOS_MODULE_BUILDER_ROOT is not set.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json
               ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

logos_module(
    NAME demo_module
)

find_library(LIBDEMO
    NAMES libdemo_module.a demo_module
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/lib
    NO_DEFAULT_PATH
)

if(NOT LIBDEMO)
    message(FATAL_ERROR
        "Rust static library (libdemo_module.a) not found in ${CMAKE_CURRENT_SOURCE_DIR}/lib. "
        "Ensure the preConfigure hook staged the library."
    )
endif()

target_link_libraries(demo_module_module_plugin PRIVATE
    ${LIBDEMO}
    logos-protocol::logos_protocol
)

if(APPLE)
    target_link_libraries(demo_module_module_plugin PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
    )
else()
    target_link_libraries(demo_module_module_plugin PRIVATE pthread dl)
endif()
