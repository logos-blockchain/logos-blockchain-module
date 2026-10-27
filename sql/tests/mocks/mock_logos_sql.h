#pragma once

#include <logos_sql.h>

#include <string>

namespace mock_sql {
    struct State {
        int databases = 0;
        int strings = 0;
        int messages = 0;
        uint32_t view = 0;
        uint32_t max_rows = 0;
        uintptr_t max_result_bytes = 0;
        uint64_t timeout = 0;
        std::string config;
        std::string sql;
        std::string parameters;
        SqlStatusCode open_error = SqlOk;
        SqlStatusCode close_error = SqlOk;
        SqlStatusCode query_error = SqlOk;
    };

    extern State state;
} // namespace mock_sql
