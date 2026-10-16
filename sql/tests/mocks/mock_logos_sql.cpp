#include "mock_logos_sql.h"

#include <cassert>
#include <cstring>

struct SqlDatabase {};

namespace mock_sql {
    State state;
}

namespace {
    char* copy(const char* value) {
        auto* result = new char[std::strlen(value) + 1];
        std::strcpy(result, value);
        return result;
    }

    SqlStatus status(SqlStatusCode code) {
        if (code == SqlOk) {
            return {SqlOk, nullptr};
        }

        ++mock_sql::state.messages;
        return {code, copy("SQL operation failed")};
    }

    SqlStringResult text(const char* value) {
        ++mock_sql::state.strings;
        return {copy(value), status(SqlOk)};
    }
} // namespace

extern "C" {
SqlDatabaseResult logos_sql_open(const char* config, uint64_t timeout) {
    mock_sql::state.config = config;
    mock_sql::state.timeout = timeout;
    if (mock_sql::state.open_error != SqlOk) {
        return {nullptr, status(mock_sql::state.open_error)};
    }

    ++mock_sql::state.databases;
    return {new SqlDatabase, status(SqlOk)};
}

SqlStatus logos_sql_close(SqlDatabase* database) {
    assert(database);
    delete database;
    --mock_sql::state.databases;
    return status(mock_sql::state.close_error);
}

SqlStringResult logos_sql_query(
    SqlDatabase* database,
    uint32_t view,
    const char* sql,
    const char* parameters,
    uint32_t max_rows,
    uintptr_t max_result_bytes
) {
    assert(database);
    mock_sql::state.view = view;
    mock_sql::state.sql = sql;
    mock_sql::state.parameters = parameters;
    mock_sql::state.max_rows = max_rows;
    mock_sql::state.max_result_bytes = max_result_bytes;
    if (mock_sql::state.query_error != SqlOk) {
        return {nullptr, status(mock_sql::state.query_error)};
    }
    return text(
        R"({"columns":["id","data"],"rows":[[{"type":"integer","value":"9223372036854775807"},{"type":"blob","value":"00ff"}]],"truncated":true})"
    );
}

void logos_sql_free_string(char* value) {
    if (value) {
        delete[] value;
        --mock_sql::state.strings;
    }
}

void logos_sql_free_status(SqlStatus result) {
    if (result.message) {
        delete[] result.message;
        --mock_sql::state.messages;
    }
}
}
