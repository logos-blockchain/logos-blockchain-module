cmake_minimum_required(VERSION 3.14)
project(LogosSqlModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME sql_module
    SOURCES
        src/logos_sql_module.h
        src/logos_sql_module.cpp
    EXTERNAL_LIBS
        logos_sql
)
