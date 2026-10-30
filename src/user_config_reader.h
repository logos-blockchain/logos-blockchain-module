#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct fy_document;
struct fy_node;

// Merges `src` onto `dst` key by key: mappings recurse, anything else (a
// scalar, a list, a null) replaces the destination value whole, and a key the
// destination lacks is appended. `dst` belongs to `doc`; `src` may belong to
// another document. The node used to offer this as merge_user_config; since
// 0.3.3 the module does it itself.
bool merge_yaml_mapping(fy_document* doc, fy_node* dst, fy_node* src);

// This exists because the node exposes no way to read a config it has not
// started against, and the setup wizard has to offer a choice of accounts while
// the config is still being written (logos-blockchain-module#90).
class UserConfigReader {
public:
    using Entry = std::pair<std::string, std::string>;

    ~UserConfigReader();

    // Reads the config at `path`. Since node 0.3.3 a config holds only what
    // differs from the defaults and the keys live in the keystore beside it, so
    // the values the node takes from that keystore are filled in underneath the
    // file's own.
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
    void fillFromKeystore(const std::filesystem::path& keystore_path);

    fy_document* m_doc = nullptr;
    // libfyaml parses a string in place, so the text a document was built from
    // has to outlive it.
    std::string m_keystoreValues;
};
