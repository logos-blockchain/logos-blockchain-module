#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct fy_document;

// This exists because the node exposes no way to read a config it has not
// started against, and the setup wizard has to offer a choice of accounts while
// the config is still being written (logos-blockchain-module#90).
class UserConfigReader {
public:
    using Entry = std::pair<std::string, std::string>;

    ~UserConfigReader();

    [[nodiscard]] bool load(const std::filesystem::path& path, std::string& error);

    // Scalar at `path`. Empty when absent, or when the key opens a block.
    [[nodiscard]] std::string scalarAt(const char* path) const;

    // Entries of the mapping at `path`, in file order. Values are lowercased for
    // comparison; keys are left as written.
    [[nodiscard]] std::vector<Entry> entriesAt(const char* path) const;

    // False when `path` is absent or is not a mapping — an `!include` tag or a
    // flow scalar — which callers report rather than read as an empty mapping.
    [[nodiscard]] bool isMappingAt(const char* path) const;

private:
    fy_document* m_doc = nullptr;
};
