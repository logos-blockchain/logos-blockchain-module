#include "logos_sql_module.h"

#include <logos_sql.h>

#include <cstdio>
#include <filesystem>
#include <limits>
#include <utility>

namespace {
    using Json = nlohmann::json;

    struct OwnedStatus {
        SqlStatus value;

        ~OwnedStatus() {
            logos_sql_free_status(value);
        }
    };

    StdLogosResult failure(const std::string& message) {
        return {false, {}, message};
    }

    StdLogosResult take_status(SqlStatus status) {
        const OwnedStatus owned{status};
        if (status.code == SqlOk) {
            return {true, {}, {}};
        }

        return failure(status.message ? status.message : "Logos SQL operation failed");
    }

    StdLogosResult take_json(SqlStringResult result) {
        auto status = take_status(result.error);
        if (!status.success) {
            return status;
        }

        const std::unique_ptr<char, decltype(&logos_sql_free_string)> value(result.value, logos_sql_free_string);
        if (!value) {
            return failure("Logos SQL returned an empty result pointer");
        }

        auto parsed = Json::parse(value.get(), nullptr, false);
        if (parsed.is_discarded()) {
            return failure("Logos SQL returned invalid JSON");
        }

        return {true, std::move(parsed), {}};
    }

    void close_replica(SqlDatabase* database) {
        const OwnedStatus status{logos_sql_close(database)};
        if (status.value.code != SqlOk) {
            std::fprintf(stderr, "Failed to close Logos SQL database: %s\n",
                status.value.message ? status.value.message : "Unknown error");
        }
    }

    StdLogosResult unknown_replica() {
        return failure("Unknown replica; open a channel first");
    }
} // namespace

struct LogosSqlModule::Replica {
    std::filesystem::path state_dir;
    std::unique_ptr<SqlDatabase, decltype(&close_replica)> database{nullptr, close_replica};
};

LogosSqlModule::LogosSqlModule() = default;
LogosSqlModule::~LogosSqlModule() = default;

LogosSqlModule::Replica* LogosSqlModule::find_replica(const std::string& database_id) {
    const auto found = replicas_.find(database_id);
    return found == replicas_.end() ? nullptr : found->second.get();
}

StdLogosResult LogosSqlModule::open(const std::string& config_json, uint64_t timeout_ms) {
    const std::lock_guard lock(mutex_);

    auto config = Json::parse(config_json, nullptr, false);
    if (config.is_discarded()) {
        return failure("Configuration must be valid JSON");
    }
    if (!config.is_object()) {
        return failure("Configuration must be a JSON object");
    }
    if (!config.contains("state_dir") || !config["state_dir"].is_string()) {
        return failure("Configuration must include a state_dir string");
    }

    const auto directory = config["state_dir"].get<std::string>();
    if (directory.empty()) {
        return failure("State directory must not be empty");
    }
    if (directory.find('\0') != std::string::npos) {
        return failure("State directory must not contain NUL bytes");
    }

    std::error_code error;
    const auto absolute = std::filesystem::absolute(directory, error);
    if (error) {
        return failure("Cannot resolve the state directory: " + error.message());
    }
    auto state_dir = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        return failure("Cannot resolve the state directory: " + error.message());
    }

    // A nonexistent directory can retain a trailing slash after canonicalisation.
    if (!state_dir.has_filename()) {
        state_dir = state_dir.parent_path();
    }

    for (const auto& [id, replica] : replicas_) {
        if (replica->state_dir == state_dir) {
            return failure("This state directory is already open");
        }
    }

    auto replica = std::make_unique<Replica>();
    config["state_dir"] = state_dir.string();
    const auto opened = logos_sql_open(config.dump().c_str(), timeout_ms);
    auto status = take_status(opened.error);
    if (!status.success) {
        return status;
    }

    replica->database.reset(opened.value);
    replica->state_dir = state_dir;
    const auto id = std::to_string(next_id_++);
    replicas_.emplace(id, std::move(replica));
    return {true, id, {}};
}

StdLogosResult LogosSqlModule::close(const std::string& database_id) {
    const std::lock_guard lock(mutex_);
    auto* replica = find_replica(database_id);
    if (!replica) {
        return unknown_replica();
    }

    const auto status = logos_sql_close(replica->database.release());
    replicas_.erase(database_id);
    return take_status(status);
}

StdLogosResult LogosSqlModule::query(
    const std::string& database_id,
    const std::string& view,
    const std::string& sql,
    const std::string& parameters_json,
    uint64_t max_rows,
    uint64_t max_result_bytes
) {
    const std::lock_guard lock(mutex_);
    auto* replica = find_replica(database_id);
    if (!replica) {
        return unknown_replica();
    }

    SqlReadView read_view;
    if (view == "latest") {
        read_view = SqlLatest;
    } else if (view == "finalized") {
        read_view = SqlFinalized;
    } else {
        return failure("Read view must be latest or finalized");
    }

    if (max_rows > std::numeric_limits<uint32_t>::max()) {
        return failure("Row limit is too large");
    }
    if (max_result_bytes > std::numeric_limits<uintptr_t>::max()) {
        return failure("Result byte limit is too large");
    }
    if (sql.find('\0') != std::string::npos || parameters_json.find('\0') != std::string::npos) {
        return failure("SQL and parameters must not contain NUL bytes");
    }

    return take_json(logos_sql_query(
        replica->database.get(),
        read_view,
        sql.c_str(),
        parameters_json.c_str(),
        static_cast<uint32_t>(max_rows),
        static_cast<uintptr_t>(max_result_bytes)
    ));
}
