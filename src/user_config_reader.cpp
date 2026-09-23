#include "user_config_reader.h"

#include <algorithm>
#include <cctype>
#include <libfyaml.h>

namespace fs = std::filesystem;

namespace {
    std::string lowered(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }
} // namespace

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
    return true;
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
