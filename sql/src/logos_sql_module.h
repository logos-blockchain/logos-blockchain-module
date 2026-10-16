#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <logos_module_context.h>
#include <logos_result.h>

class LogosSqlModule : public LogosModuleContext {
public:
    LogosSqlModule();
    ~LogosSqlModule() override;

    // Returns a replica ID. Each open replica needs its own state directory.
    // Replicas follow the channel read-only.
    [[nodiscard]] StdLogosResult open(const std::string& config_json, uint64_t timeout_ms);
    [[nodiscard]] StdLogosResult close(const std::string& database_id);

    // view is "latest" or "finalized". Parameters and returned cells use the
    // C binding's typed JSON format; results include columns, rows and truncated.
    [[nodiscard]] StdLogosResult query(
        const std::string& database_id,
        const std::string& view,
        const std::string& sql,
        const std::string& parameters_json,
        uint64_t max_rows,
        uint64_t max_result_bytes
    );
private:
    struct Replica;

    Replica* find_replica(const std::string& database_id);

    // C handles require exclusive access. Calls are blocking; the UI must use
    // asynchronous module calls rather than executing them on its render thread.
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Replica>> replicas_;
    uint64_t next_id_ = 1;
};
