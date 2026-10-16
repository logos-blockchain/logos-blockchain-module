cmake_minimum_required(VERSION 3.14)
project(SqlModuleTests LANGUAGES CXX)

include(LogosTest)

logos_test(
    NAME sql_module_tests
    MODULE_SOURCES
        ../src/logos_sql_module.cpp
    TEST_SOURCES
        main.cpp
        test_sql.cpp
    MOCK_C_SOURCES
        mocks/mock_logos_sql.cpp
    EXTRA_INCLUDES
        stubs
)
