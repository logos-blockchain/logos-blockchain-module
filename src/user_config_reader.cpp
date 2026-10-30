#include "user_config_reader.h"

#include <algorithm>
#include <cctype>
#include <libfyaml.h>
#include <map>

namespace fs = std::filesystem;

namespace {
    std::string lowered(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    // Title → scalar value of the mapping at `path`, for the keystore's two
    // halves. Keys the wallet tracks are the ones tagged !Zk in the secret half.
    std::map<std::string, std::string> titled_at(fy_node* root, const char* path, const bool zk_only) {
        std::map<std::string, std::string> entries;
        fy_node* node = root ? fy_node_by_path(root, path, FY_NT, FYNWF_DONT_FOLLOW) : nullptr;
        if (!node || !fy_node_is_mapping(node))
            return entries;
        void* iter = nullptr;
        while (fy_node_pair* pair = fy_node_mapping_iterate(node, &iter)) {
            const char* title = fy_node_get_scalar0(fy_node_pair_key(pair));
            fy_node* value = fy_node_pair_value(pair);
            const char* text = value ? fy_node_get_scalar0(value) : nullptr;
            if (!title || !text || !*title || !*text)
                continue;
            if (zk_only) {
                size_t len = 0;
                const char* tag = fy_node_get_tag(value, &len);
                if (!tag || len < 2 || lowered(std::string(tag + len - 2, 2)) != "zk")
                    continue;
            }
            entries[title] = text;
        }
        return entries;
    }

    // The values the node fills in from the keystore beside a config, as its
    // Keystore::config_values does: the funding and signing keys the services
    // use, and the keys the wallet tracks — a zk key's id is its public key.
    // Empty without a readable keystore.
    std::string keystore_values(const fs::path& keystore_path) {
        std::error_code ec;
        if (!fs::is_regular_file(keystore_path, ec))
            return {};
        fy_document* keystore = fy_document_build_from_file(nullptr, keystore_path.string().c_str());
        if (!keystore)
            return {};
        const std::map<std::string, std::string> ids = titled_at(fy_document_root(keystore), "/public_keys", false);
        const std::map<std::string, std::string> secrets = titled_at(fy_document_root(keystore), "/secret_keys", true);
        fy_document_destroy(keystore);

        const auto id_of = [&ids](const char* title) {
            const auto it = ids.find(title);
            return it == ids.end() ? std::string() : it->second;
        };
        std::string yaml;
        if (const std::string pk = id_of("LeaderFunding"); !pk.empty())
            yaml += "cryptarchia:\n  leader:\n    wallet:\n      funding_pk: " + pk + "\n";
        if (const std::string pk = id_of("SdpFunding"); !pk.empty())
            yaml += "sdp:\n  wallet:\n    funding_pk: " + pk + "\n";
        if (const std::string id = id_of("BlendSigning"); !id.empty())
            yaml += "blend:\n  non_ephemeral_signing_key_id: " + id + "\n";
        yaml += "wallet:\n";
        if (const std::string id = id_of("VaucherMaster"); !id.empty())
            yaml += "  voucher_master_key_id: " + id + "\n";
        std::string known_keys;
        for (const auto& [title, secret] : secrets) {
            if (const std::string id = id_of(title.c_str()); !id.empty())
                known_keys += "    " + id + ": " + id + "\n";
        }
        yaml += known_keys.empty() ? "  known_keys: {}\n" : "  known_keys:\n" + known_keys;
        return yaml;
    }
} // namespace

bool merge_yaml_mapping(fy_document* doc, fy_node* dst, fy_node* src) {
    void* iter = nullptr;
    while (fy_node_pair* pair = fy_node_mapping_iterate(src, &iter)) {
        fy_node* key = fy_node_pair_key(pair);
        fy_node* value = fy_node_pair_value(pair);
        const char* key_text = fy_node_get_scalar0(key);
        if (!key_text)
            return false;

        fy_node_pair* existing = fy_node_mapping_lookup_pair_by_string(dst, key_text, FY_NT);
        if (existing && fy_node_is_mapping(fy_node_pair_value(existing)) && fy_node_is_mapping(value)) {
            if (!merge_yaml_mapping(doc, fy_node_pair_value(existing), value))
                return false;
            continue;
        }

        fy_node* copied = fy_node_copy(doc, value);
        if (!copied)
            return false;
        if (existing) {
            if (fy_node_pair_set_value(existing, copied) != 0)
                return false;
        } else {
            fy_node* copied_key = fy_node_copy(doc, key);
            if (!copied_key || fy_node_mapping_append(dst, copied_key, copied) != 0)
                return false;
        }
    }
    return true;
}

UserConfigReader::~UserConfigReader() {
    if (m_doc)
        fy_document_destroy(m_doc);
}

bool UserConfigReader::load(const fs::path& path, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "Failed to open " + path.string() + ".";
        return false;
    }
    m_doc = fy_document_build_from_file(nullptr, path.string().c_str());
    if (!m_doc) {
        error = "Failed to parse " + path.string() + " as YAML.";
        return false;
    }
    fillFromKeystore(path.parent_path() / "keystore.yaml");
    return true;
}

// The keystore's values go underneath and the file's own on top, which is how
// the node composes them. Only this reader sees the result: nothing here is
// written back, so a sparse config stays sparse.
void UserConfigReader::fillFromKeystore(const fs::path& keystore_path) {
    m_keystoreValues = keystore_values(keystore_path);
    if (m_keystoreValues.empty())
        return;
    fy_document* merged =
        fy_document_build_from_string(nullptr, m_keystoreValues.data(), m_keystoreValues.size());
    if (!merged)
        return;
    fy_node* config = fy_document_root(m_doc);
    const bool ok = !config || (fy_node_is_mapping(config) && merge_yaml_mapping(merged, fy_document_root(merged), config));
    if (!ok) {
        fy_document_destroy(merged);
        return;
    }
    fy_document_destroy(m_doc);
    m_doc = merged;
}

std::string UserConfigReader::scalarAt(const char* path) const {
    if (!m_doc)
        return {};
    fy_node* node = fy_node_by_path(fy_document_root(m_doc), path, FY_NT, FYNWF_DONT_FOLLOW);
    if (!node || !fy_node_is_scalar(node))
        return {};
    const char* value = fy_node_get_scalar0(node);
    return value ? std::string(value) : std::string();
}

std::vector<UserConfigReader::Entry> UserConfigReader::entriesAt(const char* path) const {
    std::vector<Entry> entries;
    if (!m_doc)
        return entries;
    fy_node* node = fy_node_by_path(fy_document_root(m_doc), path, FY_NT, FYNWF_DONT_FOLLOW);
    if (!node || !fy_node_is_mapping(node))
        return entries;

    void* iter = nullptr;
    while (fy_node_pair* pair = fy_node_mapping_iterate(node, &iter)) {
        const char* key = fy_node_get_scalar0(fy_node_pair_key(pair));
        const char* value = fy_node_get_scalar0(fy_node_pair_value(pair));
        if (key && value && *key && *value)
            entries.emplace_back(key, lowered(value));
    }
    return entries;
}

bool UserConfigReader::isMappingAt(const char* path) const {
    if (!m_doc)
        return false;
    fy_node* node = fy_node_by_path(fy_document_root(m_doc), path, FY_NT, FYNWF_DONT_FOLLOW);
    return node && fy_node_is_mapping(node);
}
