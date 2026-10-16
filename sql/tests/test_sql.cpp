#include <logos_test.h>

#include "logos_sql_module.h"
#include "mocks/mock_logos_sql.h"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace {
    struct TempDir {
        std::filesystem::path path;

        TempDir() {
            char pattern[] = "/tmp/logos-sql-test-XXXXXX";
            if (const auto directory = mkdtemp(pattern)) {
                path = directory;
            }
        }

        ~TempDir() {
            if (!path.empty()) {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        }
    };

    std::string open_replica(LogosSqlModule& module, const std::string& directory = "/tmp/sql-module-test") {
        const auto config = nlohmann::json{
            {"channel_id", std::string(64, 'a')}, {"node_url", "http://127.0.0.1:8080"}, {"state_dir", directory}
        };
        const auto result = module.open(config.dump(), 30000);
        LOGOS_ASSERT_TRUE(result.success);
        return result.value.get<std::string>();
    }

    void assert_released() {
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 0);
        LOGOS_ASSERT_EQ(mock_sql::state.strings, 0);
        LOGOS_ASSERT_EQ(mock_sql::state.messages, 0);
    }
} // namespace

LOGOS_TEST(replica_lifecycle_is_independent_and_releases_handles) {
    mock_sql::state = {};
    {
        LogosSqlModule module;
        const auto first = open_replica(module);
        const auto second = open_replica(module, "/tmp/sql-module-other");
        LOGOS_ASSERT_NE(first, second);
        LOGOS_ASSERT_EQ(mock_sql::state.timeout, 30000u);
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 2);

        LOGOS_ASSERT_TRUE(module.close(first).success);
        LOGOS_ASSERT_FALSE(module.query(first, "latest", "SELECT 1", "[]", 1, 1024).success);
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 1);
    }
    assert_released();
}

LOGOS_TEST(queries_preserve_views_parameters_and_typed_cells) {
    mock_sql::state = {};
    {
        LogosSqlModule module;
        const auto id = open_replica(module);
        const std::string parameters = R"([{"type":"integer","value":"9223372036854775807"}])";

        const auto result = module.query(id, "finalized", "SELECT ?1", parameters, 50, 4096);
        LOGOS_ASSERT_TRUE(result.success);
        LOGOS_ASSERT_EQ(mock_sql::state.view, static_cast<uint32_t>(SqlFinalized));
        LOGOS_ASSERT_EQ(mock_sql::state.sql, std::string("SELECT ?1"));
        LOGOS_ASSERT_EQ(mock_sql::state.parameters, parameters);
        LOGOS_ASSERT_EQ(mock_sql::state.max_rows, 50u);
        LOGOS_ASSERT_EQ(mock_sql::state.max_result_bytes, 4096u);
        LOGOS_ASSERT_EQ(result.value["rows"][0][0]["value"], "9223372036854775807");
        LOGOS_ASSERT_EQ(result.value["rows"][0][1]["value"], "00ff");
        LOGOS_ASSERT_TRUE(result.value["truncated"].get<bool>());

        LOGOS_ASSERT_TRUE(module.query(id, "latest", "SELECT 1", "[]", 10, 1024).success);
        LOGOS_ASSERT_EQ(mock_sql::state.view, static_cast<uint32_t>(SqlLatest));
        LOGOS_ASSERT_EQ(mock_sql::state.max_result_bytes, 1024u);
        LOGOS_ASSERT_FALSE(module.query(id, "unknown", "SELECT 1", "[]", 10, 1024).success);
    }
    assert_released();
}

LOGOS_TEST(query_errors_preserve_the_binding_message) {
    mock_sql::state = {};
    {
        LogosSqlModule module;
        const auto id = open_replica(module);

        for (const auto code : {SqlFailure, SqlRuntimeStopped}) {
            mock_sql::state.query_error = code;
            const auto result = module.query(id, "latest", "SELECT 1", "[]", 10, 1024);
            LOGOS_ASSERT_FALSE(result.success);
            LOGOS_ASSERT_TRUE(result.value.is_null());
            LOGOS_ASSERT_EQ(result.error, std::string("SQL operation failed"));
        }
    }
    assert_released();
}

LOGOS_TEST(failed_open_and_close_do_not_leave_handles) {
    mock_sql::state = {};
    {
        LogosSqlModule module;
        mock_sql::state.open_error = SqlTimeout;
        const auto failed = module.open(R"({"state_dir":"/tmp/sql-module-test"})", 1);
        LOGOS_ASSERT_FALSE(failed.success);
        LOGOS_ASSERT_TRUE(failed.value.is_null());
        LOGOS_ASSERT_EQ(failed.error, std::string("SQL operation failed"));
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 0);

        mock_sql::state.open_error = SqlOk;
        const auto id = open_replica(module);
        mock_sql::state.close_error = SqlFailure;
        const auto closed = module.close(id);
        LOGOS_ASSERT_FALSE(closed.success);
        LOGOS_ASSERT_EQ(closed.error, std::string("SQL operation failed"));
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 0);

        const auto closed_again = module.close(id);
        LOGOS_ASSERT_FALSE(closed_again.success);
        LOGOS_ASSERT_EQ(closed_again.error, std::string("Unknown replica; open a channel first"));

        // Shutdown must release resources even when closing fails.
        open_replica(module);
    }
    assert_released();
}

LOGOS_TEST(reports_invalid_configuration) {
    mock_sql::state = {};
    LogosSqlModule module;

    const auto check = [&](const std::string& config, const std::string& message) {
        const auto result = module.open(config, 1);
        LOGOS_ASSERT_FALSE(result.success);
        LOGOS_ASSERT_TRUE(result.value.is_null());
        LOGOS_ASSERT_EQ(result.error, message);
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 0);
    };

    check("{", "Configuration must be valid JSON");
    check("[]", "Configuration must be a JSON object");
    check("{}", "Configuration must include a state_dir string");
    check(R"({"state_dir":42})", "Configuration must include a state_dir string");
    check(R"({"state_dir":""})", "State directory must not be empty");
    check(R"({"state_dir":"/tmp/sql\u0000ignored"})", "State directory must not contain NUL bytes");

    assert_released();
}

LOGOS_TEST(rejects_duplicate_directories_and_truncated_c_strings) {
    mock_sql::state = {};
    {
        LogosSqlModule module;
        const auto id = open_replica(module);
        LOGOS_ASSERT_FALSE(module.open(R"({"state_dir":"/tmp/sql-module-test/../sql-module-test"})", 1).success);
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 1);

        const std::string sql("SELECT 1\0ignored", 16);
        LOGOS_ASSERT_FALSE(module.query(id, "latest", sql, "[]", 1, 1024).success);
    }
    assert_released();
}

LOGOS_TEST(rejects_duplicate_directories_with_trailing_slashes) {
    mock_sql::state = {};
    const TempDir temporary;
    LOGOS_ASSERT_FALSE(temporary.path.empty());

    const auto directory = (temporary.path / "replica").string();
    LOGOS_ASSERT_FALSE(std::filesystem::exists(directory));

    for (const bool trailing_slash_first : {true, false}) {
        LogosSqlModule module;
        const auto first = trailing_slash_first ? directory + "/" : directory;
        const auto second = trailing_slash_first ? directory : directory + "/";
        open_replica(module, first);

        const auto expected_config = nlohmann::json{
            {"channel_id", std::string(64, 'a')},
            {"node_url", "http://127.0.0.1:8080"},
            {"state_dir", (std::filesystem::canonical(temporary.path) / "replica").string()}
        };
        LOGOS_ASSERT_EQ(nlohmann::json::parse(mock_sql::state.config), expected_config);

        const auto config = nlohmann::json{{"state_dir", second}};
        const auto result = module.open(config.dump(), 1);
        LOGOS_ASSERT_FALSE(result.success);
        LOGOS_ASSERT_EQ(result.error, std::string("This state directory is already open"));
        LOGOS_ASSERT_EQ(mock_sql::state.databases, 1);
    }
    assert_released();
}
