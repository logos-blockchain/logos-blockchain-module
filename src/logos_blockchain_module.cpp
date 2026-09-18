#include "logos_blockchain_module.h"

#include <algorithm>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;

namespace operation_status {
    // Takes the Rust-allocated message out of an OperationStatus and frees it.
    std::string take_message(OperationStatus& status) {
        std::string message;
        if (status.message) {
            message = status.message;
            (void)free_cstring(status.message);
            status.message = nullptr;
        }
        return message;
    }
} // namespace operation_status

// Shorthands for building StdLogosResult values.
namespace result {
    StdLogosResult ok() {
        return {true};
    }

    template <typename T>
    StdLogosResult ok(T value) { // NOLINT(performance-unnecessary-value-param)
        return {true, std::move(value)};
    }

    StdLogosResult err(std::string message) {
        return {false, {}, std::move(message)};
    }

    StdLogosResult from_operation_status(OperationStatus& status) {
        if (is_ok(&status)) {
            return ok();
        }
        return err(operation_status::take_message(status));
    }
} // namespace result

namespace {
    // Rust `File::open` / `deserialize_config_at_path` only accept real filesystem paths. QML often
    // passes `file:///...` URLs; strip to a local path when applicable.
    std::string localPathFromFileUrl(const std::string& s) {
        if (s.size() >= 7 && s.substr(0, 7) == "file://")
            return s.substr(7);
        if (s.size() >= 5 && s.substr(0, 5) == "file:")
            return s.substr(5);
        return s;
    }

    // Use the C API type Hash (from logos_blockchain.h) to define address/hash byte size.
    constexpr int ADDRESS_BYTES = sizeof(Hash);
    constexpr int TX_HASH_BYTES = sizeof(TxHash);
    constexpr int ADDRESS_HEX_LEN = ADDRESS_BYTES * 2;

    std::vector<uint8_t> parse_address_hex(const std::string& address_hex) {
        std::string hex = address_hex;
        boost::algorithm::trim(hex);
        if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
            hex = hex.substr(2);
        if (static_cast<int>(hex.size()) != ADDRESS_HEX_LEN)
            return {};
        try {
            std::string decoded;
            boost::algorithm::unhex(hex.begin(), hex.end(), std::back_inserter(decoded));
            return {decoded.begin(), decoded.end()};
        } catch (const boost::algorithm::non_hex_input&) {
            return {};
        }
    }

    // Parse arbitrary-length hex (optional 0x prefix) into bytes. Unlike
    // parse_address_hex this does not enforce a fixed length; used for the
    // variable-length channel deposit metadata. Returns false on odd length or
    // non-hex input.
    bool parse_hex_bytes(const std::string& hex_in, std::vector<uint8_t>& out) {
        std::string hex = hex_in;
        boost::algorithm::trim(hex);
        if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
            hex = hex.substr(2);
        if (hex.size() % 2 != 0)
            return false;
        try {
            std::string decoded;
            boost::algorithm::unhex(hex.begin(), hex.end(), std::back_inserter(decoded));
            out.assign(decoded.begin(), decoded.end());
            return true;
        } catch (const boost::algorithm::non_hex_input&) {
            return false;
        }
    }

    std::string bytes_to_hex(const uint8_t* data, const size_t len) {
        std::string out;
        out.reserve(len * 2);
        boost::algorithm::hex_lower(data, data + len, std::back_inserter(out));
        return out;
    }

    // Maps an `ed25519`/`zk` string (case-insensitive) to the C KeyType enum.
    bool parse_key_type(const std::string& s, KeyType& out) {
        std::string lower = s;
        boost::algorithm::trim(lower);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
            return std::tolower(c);
        });
        if (lower == "ed25519") {
            out = KeyType::Ed25519;
            return true;
        }
        if (lower == "zk") {
            out = KeyType::Zk;
            return true;
        }
        return false;
    }

    constexpr auto STATE_DIR = "state";

    // Path to the node's state persistence directory
    //
    // This is not authoritative, just expected.
    // The true authoritative path is the one set in the config.
    fs::path state_dir(const std::string& persistence_path) {
        return fs::path(persistence_path) / STATE_DIR;
    }

    // Wrapper that owns data and provides GenerateConfigArgs
    struct OwnedGenerateConfigArgs {
        std::vector<std::string> initial_peers_data;
        std::vector<const char*> initial_peers_ptrs;
        uint32_t initial_peers_count_val;
        std::string output_data;
        uint16_t net_port_val;
        uint16_t blend_port_val;
        std::string http_addr_data;
        std::string external_address_data;
        std::string state_path_data;
        std::string storage_path_data;
        std::string logs_path_data;
        bool skip_ibd_val;
        std::string log_filter_data;
        std::string kms_file_data;

        // The FFI struct with pointers into owned data
        GenerateConfigArgs ffi_args{};

        // Constructor that populates both owned data and FFI struct from JSON
        explicit OwnedGenerateConfigArgs(const json& args) {
            // initial_peers (JSON array -> const char**)
            if (args.contains("initial_peers") && args["initial_peers"].is_array()) {
                for (const auto& peer : args["initial_peers"]) {
                    initial_peers_data.push_back(peer.get<std::string>());
                }
                initial_peers_count_val = static_cast<uint32_t>(initial_peers_data.size());

                for (const std::string& data : initial_peers_data) {
                    initial_peers_ptrs.push_back(data.c_str());
                }

                ffi_args.initial_peers = initial_peers_ptrs.data();
                ffi_args.initial_peers_count = &initial_peers_count_val;
            } else {
                ffi_args.initial_peers = nullptr;
                ffi_args.initial_peers_count = nullptr;
            }

            // output (string -> const char*)
            if (args.contains("output") && args["output"].is_string()) {
                output_data = args["output"].get<std::string>();
                ffi_args.output = output_data.c_str();
            } else {
                ffi_args.output = nullptr;
            }

            // net_port (int -> const uint16_t*)
            if (args.contains("net_port") && args["net_port"].is_number_integer()) {
                net_port_val = static_cast<uint16_t>(args["net_port"].get<int>());
                ffi_args.net_port = &net_port_val;
            } else {
                ffi_args.net_port = nullptr;
            }

            // blend_port (int -> const uint16_t*)
            if (args.contains("blend_port") && args["blend_port"].is_number_integer()) {
                blend_port_val = static_cast<uint16_t>(args["blend_port"].get<int>());
                ffi_args.blend_port = &blend_port_val;
            } else {
                ffi_args.blend_port = nullptr;
            }

            // http_addr (string -> const char*)
            if (args.contains("http_addr") && args["http_addr"].is_string()) {
                http_addr_data = args["http_addr"].get<std::string>();
                ffi_args.http_addr = http_addr_data.c_str();
            } else {
                ffi_args.http_addr = nullptr;
            }

            // external_address (string -> const char*)
            if (args.contains("external_address") && args["external_address"].is_string()) {
                external_address_data = args["external_address"].get<std::string>();
                ffi_args.external_address = external_address_data.c_str();
            } else {
                ffi_args.external_address = nullptr;
            }

            // state_path (string -> const char*)
            if (args.contains("state_path") && args["state_path"].is_string()) {
                state_path_data = args["state_path"].get<std::string>();
                ffi_args.state_path = state_path_data.c_str();
            } else {
                ffi_args.state_path = nullptr;
            }

            // storage_path (string -> const char*) — maps to storage.backend.folder_name
            if (args.contains("storage_path") && args["storage_path"].is_string()) {
                storage_path_data = args["storage_path"].get<std::string>();
                ffi_args.storage_path = storage_path_data.c_str();
            } else {
                ffi_args.storage_path = nullptr;
            }

            // logs_path (string -> const char*) — maps to tracing.logger.file.directory
            if (args.contains("logs_path") && args["logs_path"].is_string()) {
                logs_path_data = args["logs_path"].get<std::string>();
                ffi_args.logs_path = logs_path_data.c_str();
            } else {
                ffi_args.logs_path = nullptr;
            }

            // skip_ibd (bool -> const bool*)
            if (args.contains("skip_ibd") && args["skip_ibd"].is_boolean()) {
                skip_ibd_val = args["skip_ibd"].get<bool>();
                ffi_args.skip_ibd = &skip_ibd_val;
            } else {
                ffi_args.skip_ibd = nullptr;
            }

            // log_filter (string -> const char*)
            if (args.contains("log_filter") && args["log_filter"].is_string()) {
                log_filter_data = args["log_filter"].get<std::string>();
                ffi_args.log_filter = log_filter_data.c_str();
            } else {
                ffi_args.log_filter = nullptr;
            }

            // kms_file (string -> const char*)
            if (args.contains("kms_file") && args["kms_file"].is_string()) {
                kms_file_data = args["kms_file"].get<std::string>();
                ffi_args.kms_file = kms_file_data.c_str();
            } else {
                ffi_args.kms_file = nullptr;
            }
        }
    };

    // Block-YAML reading and editing.
    //
    // The node writes its user config with serde_yaml: block mappings, two-space
    // indentation, no anchors, and no flow collections on the paths touched here.
    // Walking that subset by indentation is enough to read a scalar and replace a
    // single value, and it spares the module a YAML library it would then have to
    // ship inside every portable bundle — and a whole-document round-trip through
    // a second implementation, which would put the config's KMS key tags at risk
    // for the sake of two lines.
    namespace yaml {
        constexpr size_t NPOS = static_cast<size_t>(-1);

        // Indentation of a line, or -1 when it holds nothing addressable.
        int indent_of(const std::string& line) {
            size_t i = 0;
            while (i < line.size() && line[i] == ' ')
                ++i;
            if (i == line.size() || line[i] == '#' || line[i] == '\r')
                return -1;
            return static_cast<int>(i);
        }

        bool is_sequence_item(const std::string& line) {
            const int indent = indent_of(line);
            if (indent < 0)
                return false;
            const size_t at = static_cast<size_t>(indent);
            return line[at] == '-' && (at + 1 == line.size() || line[at + 1] == ' ');
        }

        bool declares_key(const std::string& line, int indent, const std::string& key) {
            if (indent_of(line) != indent)
                return false;
            const size_t at = static_cast<size_t>(indent);
            return line.size() > at + key.size() && line.compare(at, key.size(), key) == 0 &&
                   line[at + key.size()] == ':';
        }

        // Line declaring `key` at `indent`, searched from `begin` and stopped
        // where the enclosing block ends — the first line shallower than `indent`.
        size_t find_key(const std::vector<std::string>& lines, size_t begin, int indent, const std::string& key) {
            for (size_t i = begin; i < lines.size(); ++i) {
                const int line_indent = indent_of(lines[i]);
                if (line_indent < 0)
                    continue;
                if (line_indent < indent)
                    return NPOS;
                if (declares_key(lines[i], indent, key))
                    return i;
            }
            return NPOS;
        }

        // One past the last line of the value opened at `key_line`: everything
        // deeper, plus sequence items, which serde_yaml writes at the key's own
        // indentation rather than below it.
        size_t value_end(const std::vector<std::string>& lines, size_t key_line, int indent) {
            size_t end = key_line + 1;
            for (size_t i = key_line + 1; i < lines.size(); ++i) {
                const int line_indent = indent_of(lines[i]);
                if (line_indent < 0)
                    continue;
                if (line_indent < indent || (line_indent == indent && !is_sequence_item(lines[i])))
                    break;
                end = i + 1;
            }
            return end;
        }

        // Indentation this key's children are written at, or -1 when it opens no
        // block. Read from the file rather than assumed, so a config indented
        // some other way still resolves.
        int child_indent(const std::vector<std::string>& lines, size_t key_line, int indent) {
            for (size_t i = key_line + 1; i < lines.size(); ++i) {
                const int line_indent = indent_of(lines[i]);
                if (line_indent < 0)
                    continue;
                return line_indent > indent ? line_indent : -1;
            }
            return -1;
        }

        // Scalar after "key:", trimmed. Empty when the key opens a block.
        std::string scalar_of(const std::string& line) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                return {};
            std::string value = line.substr(colon + 1);
            boost::algorithm::trim(value);
            return value;
        }

        std::vector<std::string> split_lines(const std::string& text) {
            std::vector<std::string> lines;
            size_t start = 0;
            while (true) {
                const size_t newline = text.find('\n', start);
                if (newline == std::string::npos) {
                    lines.push_back(text.substr(start));
                    return lines;
                }
                lines.push_back(text.substr(start, newline - start));
                start = newline + 1;
            }
        }

        std::string join_lines(const std::vector<std::string>& lines) {
            std::string text;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i > 0)
                    text += '\n';
                text += lines[i];
            }
            return text;
        }

        // Line declaring the last segment of `path`, or NPOS when any segment is
        // missing.
        size_t find_path(const std::vector<std::string>& lines, const std::vector<std::string>& path) {
            size_t begin = 0;
            int indent = 0;
            for (size_t depth = 0; depth < path.size(); ++depth) {
                const size_t found = find_key(lines, begin, indent, path[depth]);
                if (found == NPOS)
                    return NPOS;
                if (depth + 1 == path.size())
                    return found;
                const int child = child_indent(lines, found, indent);
                if (child < 0)
                    return NPOS;
                begin = found + 1;
                indent = child;
            }
            return NPOS;
        }

        // The first segment of `path` carrying a scalar where this reader
        // expects a block mapping, or empty when the whole path is walkable.
        //
        // The node's own YAML loader resolves `!include other.yaml` tags, and a
        // config assembled that way is outside what indentation-walking can
        // follow. Reporting the segment lets a caller say so, rather than read
        // an included section as an empty block — which for wallet.known_keys
        // would look like a wallet holding no keys at all and turn every claim
        // target into a spurious "not tracked" rejection.
        std::string scalar_segment(
            const std::vector<std::string>& lines,
            const std::vector<std::string>& path
        ) {
            size_t begin = 0;
            int indent = 0;
            std::string walked;
            for (size_t depth = 0; depth < path.size(); ++depth) {
                const size_t found = find_key(lines, begin, indent, path[depth]);
                if (found == NPOS)
                    return {};
                if (!walked.empty())
                    walked += '.';
                walked += path[depth];
                if (!scalar_of(lines[found]).empty())
                    return walked;
                if (depth + 1 == path.size())
                    return {};
                const int child = child_indent(lines, found, indent);
                if (child < 0)
                    return {};
                begin = found + 1;
                indent = child;
            }
            return {};
        }

        // Whether the mapping at `path` holds an entry whose value is `value`.
        // Used to check a claim target against wallet.known_keys, which maps key
        // id to public key — the public keys are the values.
        bool maps_to_value(
            const std::vector<std::string>& lines,
            const std::vector<std::string>& path,
            const std::string& value
        ) {
            const size_t key_line = find_path(lines, path);
            if (key_line == NPOS)
                return false;
            const int indent = indent_of(lines[key_line]);
            const int child = child_indent(lines, key_line, indent);
            if (child < 0)
                return false;
            const size_t end = value_end(lines, key_line, indent);
            for (size_t i = key_line + 1; i < end; ++i) {
                if (indent_of(lines[i]) != child)
                    continue;
                std::string entry = scalar_of(lines[i]);
                std::transform(entry.begin(), entry.end(), entry.begin(), [](const unsigned char c) {
                    return std::tolower(c);
                });
                if (entry == value)
                    return true;
            }
            return false;
        }

        // Values of the mapping at `path`, in file order. The companion to
        // maps_to_value for callers that want the whole set rather than a
        // membership test: wallet.known_keys maps key id to public key, and it
        // is the public keys a claim target is matched against.
        std::vector<std::string> values_under(
            const std::vector<std::string>& lines,
            const std::vector<std::string>& path
        ) {
            std::vector<std::string> values;
            const size_t key_line = find_path(lines, path);
            if (key_line == NPOS)
                return values;
            const int indent = indent_of(lines[key_line]);
            const int child = child_indent(lines, key_line, indent);
            if (child < 0)
                return values;
            const size_t end = value_end(lines, key_line, indent);
            for (size_t i = key_line + 1; i < end; ++i) {
                if (indent_of(lines[i]) != child)
                    continue;
                std::string entry = scalar_of(lines[i]);
                if (entry.empty())
                    continue;
                std::transform(entry.begin(), entry.end(), entry.begin(), [](const unsigned char c) {
                    return std::tolower(c);
                });
                values.push_back(std::move(entry));
            }
            return values;
        }

        // Key/value pairs of the mapping at `path`, in file order. The companion
        // to values_under for callers that need the KEY as well: keystore.yaml
        // writes `public_keys` as `Title: <public key>`, and the title is the
        // point. Values are lowercased to match the rest of this reader; titles
        // are left as written.
        std::vector<std::pair<std::string, std::string>> pairs_under(
            const std::vector<std::string>& lines,
            const std::vector<std::string>& path
        ) {
            std::vector<std::pair<std::string, std::string>> pairs;
            const size_t key_line = find_path(lines, path);
            if (key_line == NPOS)
                return pairs;
            const int indent = indent_of(lines[key_line]);
            const int child = child_indent(lines, key_line, indent);
            if (child < 0)
                return pairs;
            const size_t end = value_end(lines, key_line, indent);
            for (size_t i = key_line + 1; i < end; ++i) {
                if (indent_of(lines[i]) != child)
                    continue;
                const size_t colon = lines[i].find(':');
                if (colon == std::string::npos)
                    continue;
                std::string key = lines[i].substr(0, colon);
                boost::algorithm::trim(key);
                std::string value = scalar_of(lines[i]);
                if (key.empty() || value.empty())
                    continue;
                std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
                    return std::tolower(c);
                });
                pairs.emplace_back(std::move(key), std::move(value));
            }
            return pairs;
        }

        // Replaces the value written under `path` with `render(indent)`, where
        // `indent` is the column the last segment sits at. Missing blocks along
        // the way are created: a missing top-level section is appended as a new
        // block (leaving every existing line untouched), a missing nested one is
        // added as its parent's first child.
        template <typename Render>
        bool set_value_at(
            std::vector<std::string>& lines,
            const std::vector<std::string>& path,
            const Render& render,
            std::string& error
        ) {
            size_t begin = 0;
            int indent = 0;
            for (size_t depth = 0; depth < path.size(); ++depth) {
                const size_t found = find_key(lines, begin, indent, path[depth]);

                if (found == NPOS) {
                    // Build the rest of the path as a nested block.
                    std::vector<std::string> block;
                    int at = indent;
                    for (size_t rest = depth; rest + 1 < path.size(); ++rest) {
                        block.push_back(std::string(static_cast<size_t>(at), ' ') + path[rest] + ":");
                        at += 2;
                    }
                    const std::vector<std::string> tail = render(at);
                    block.insert(block.end(), tail.begin(), tail.end());

                    // A trailing newline leaves an empty last element; keep the
                    // file ending in one by inserting before it.
                    size_t at_line = begin;
                    if (depth == 0) {
                        at_line = lines.size();
                        if (at_line > 0 && lines.back().empty())
                            --at_line;
                    }
                    lines.insert(
                        lines.begin() + static_cast<std::ptrdiff_t>(at_line), block.begin(), block.end()
                    );
                    return true;
                }

                if (depth + 1 == path.size()) {
                    const size_t end = value_end(lines, found, indent);
                    const std::vector<std::string> value = render(indent);
                    lines.erase(
                        lines.begin() + static_cast<std::ptrdiff_t>(found),
                        lines.begin() + static_cast<std::ptrdiff_t>(end)
                    );
                    lines.insert(
                        lines.begin() + static_cast<std::ptrdiff_t>(found), value.begin(), value.end()
                    );
                    return true;
                }

                // Descending into something that is not a block mapping — a flow
                // map, say — would splice children under a scalar and produce a
                // config the node cannot parse. Refuse instead.
                if (!scalar_of(lines[found]).empty()) {
                    error = path[depth] + " is not a block mapping; refusing to edit the config.";
                    return false;
                }
                const int child = child_indent(lines, found, indent);
                begin = found + 1;
                indent = child > indent ? child : indent + 2;
            }
            error = "No path to set.";
            return false;
        }

        bool read_file(const fs::path& path, std::string& text, std::string& error) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                error = "Failed to open " + path.string() + ".";
                return false;
            }
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (file.bad()) {
                error = "Failed to read " + path.string() + ".";
                return false;
            }
            return true;
        }

        // Write via a sibling temp file and rename, so an interrupted write can
        // never leave a half-written config behind.
        //
        // The file holds the node's private keys, so the temp file is created
        // by mkstemp rather than by an ofstream: it is opened O_EXCL at 0600,
        // which keeps the key material owner-only for the whole of its life
        // instead of landing at whatever the process umask allows and being
        // narrowed afterwards. The unique name mkstemp picks also means two
        // concurrent writers get two inodes rather than trampling one.
        bool write_file_atomic(const fs::path& path, const std::string& text, std::string& error) {
            const std::string temp_pattern = path.string() + ".XXXXXX";
            std::vector<char> temp_name(temp_pattern.begin(), temp_pattern.end());
            temp_name.push_back('\0');

            const int fd = ::mkstemp(temp_name.data());
            if (fd < 0) {
                error = "Failed to create a temporary file beside " + path.string() + ": " +
                        std::strerror(errno);
                return false;
            }
            const fs::path temp = temp_name.data();

            const auto fail = [&error, &temp](const std::string& what, const int fd_to_close) {
                error = what;
                if (fd_to_close >= 0)
                    ::close(fd_to_close);
                std::error_code remove_ec;
                fs::remove(temp, remove_ec);
                return false;
            };

            const char* cursor = text.data();
            size_t remaining = text.size();
            while (remaining > 0) {
                const ssize_t written = ::write(fd, cursor, remaining);
                if (written < 0) {
                    if (errno == EINTR)
                        continue;
                    return fail(
                        "Failed to write " + temp.string() + ": " + std::strerror(errno), fd
                    );
                }
                cursor += written;
                remaining -= static_cast<size_t>(written);
            }

            // Flush before the rename, so a crash leaves either the old config
            // or the new one rather than an empty file under the real name.
            if (::fsync(fd) != 0) {
                return fail("Failed to flush " + temp.string() + ": " + std::strerror(errno), fd);
            }
            if (::close(fd) != 0) {
                return fail("Failed to close " + temp.string() + ": " + std::strerror(errno), -1);
            }

            // Carry the original's permissions over. A failure here is fatal
            // rather than ignored: silently swapping a config's mode is the
            // same class of mistake as writing the keys world-readable, and
            // leaving the original untouched is the recoverable outcome.
            std::error_code error_code;
            const fs::perms original = fs::status(path, error_code).permissions();
            if (!error_code && original != fs::perms::unknown) {
                fs::permissions(temp, original, error_code);
                if (error_code) {
                    return fail(
                        "Failed to carry the permissions of " + path.string() + " over to the new "
                        "config: " + error_code.message(),
                        -1
                    );
                }
            }

            fs::rename(temp, path, error_code);
            if (error_code) {
                return fail(
                    "Failed to replace " + path.string() + ": " + error_code.message(), -1
                );
            }
            return true;
        }
    } // namespace yaml
} // namespace

void LogosBlockchainModule::on_new_block_callback(const char* block) {
    if (!s_instance || !block) {
        return;
    }
    fprintf(stderr, "Received new block: %s\n", block);
    json j;
    j["block"] = std::string(block);
    s_instance->newBlock(j.dump());
    // SAFETY:
    // We are getting an owned pointer here which is freed after this callback is called, so there is no need to
    // free the resource here as we are copying the data!
}

// The stream callbacks pass the FFI's JSON through unwrapped (it is already a
// complete JSON document with the node's HTTP stream schema). A NULL pointer
// means the stream ended; it is forwarded as the JSON literal `null` so
// termination stays in-band on the same event.

void LogosBlockchainModule::on_processed_block_callback(const char* event) {
    if (!s_instance) {
        return;
    }
    if (!event) {
        fprintf(stderr, "Processed block stream ended.\n");
        s_instance->processedBlock("null");
        return;
    }
    s_instance->processedBlock(std::string(event));
}

void LogosBlockchainModule::on_lib_block_callback(const char* event) {
    if (!s_instance) {
        return;
    }
    if (!event) {
        fprintf(stderr, "LIB block stream ended.\n");
        s_instance->libBlock("null");
        return;
    }
    s_instance->libBlock(std::string(event));
}

LogosBlockchainModule::LogosBlockchainModule() {
    node = nullptr;
}

LogosBlockchainModule::~LogosBlockchainModule() {
    s_instance = nullptr;
    if (node) {
        (void)stop();
    }
}

// ---- Node ----

// Lifecycle

StdLogosResult LogosBlockchainModule::generate_user_config(const std::string& json_args) const {
    json parsed_args;
    try {
        parsed_args = json::parse(json_args);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "Failed to parse JSON args: %s\n", e.what());
        return result::err(std::string("Failed to parse JSON args: ") + e.what());
    }

    // The module-context getters are populated by every logos-core host
    // (logoscore-cli and Basecamp alike), so their mere presence can't tell the
    // two apart. The bundled app therefore opts in explicitly by passing
    // "use_persistence_paths": true; only then do we route the node's runtime
    // directories — state, storage (db) and logs — under the host-owned
    // per-instance persistence dir, so they all share one writable base. CLI and
    // standalone callers omit the flag and keep their own paths (or the node
    // defaults). Any path the caller set explicitly is left untouched.
    bool use_persistence_paths = false;
    if (const auto it = parsed_args.find("use_persistence_paths"); it != parsed_args.end() && it->is_boolean()) {
        use_persistence_paths = it->get<bool>();
    }
    parsed_args.erase("use_persistence_paths"); // not an FFI field

    if (use_persistence_paths) {
        const std::string& persistence = instancePersistencePath();
        if (!persistence.empty()) {
            const fs::path base(persistence);
            // Only fill a path the caller didn't pin (non-empty string wins).
            const auto set_if_absent = [&parsed_args](const char* key, const std::string& value) {
                const bool provided = parsed_args.contains(key) && parsed_args[key].is_string() &&
                                      !parsed_args[key].get<std::string>().empty();
                if (!provided)
                    parsed_args[key] = value;
            };
            set_if_absent("state_path", state_dir(persistence).string());
            set_if_absent("storage_path", (base / "db").string());
            set_if_absent("logs_path", (base / "logs").string());

            // The config file itself is written under the same base, using the
            // caller's path as the relative part below it ("config/user_config.yaml"
            // → "<base>/config/user_config.yaml"). Absolute / root-anchored inputs
            // (e.g. "//user_config.yaml" from QDir::currentPath()=="/") are treated
            // as relative to the base; a missing output defaults to
            // "<base>/user_config.yaml".
            fs::path output_rel = "user_config.yaml";
            if (parsed_args.contains("output") && parsed_args["output"].is_string() &&
                !parsed_args["output"].get<std::string>().empty()) {
                const fs::path given(localPathFromFileUrl(parsed_args["output"].get<std::string>()));
                const fs::path rel = given.relative_path();
                output_rel = rel.empty() ? given.filename() : rel;
                if (output_rel.empty())
                    output_rel = "user_config.yaml";
            }
            parsed_args["output"] = (base / output_rel).lexically_normal().string();

            fprintf(
                stderr,
                "generate_user_config: routing output/state/storage/logs under instance persistence path: %s\n",
                persistence.c_str()
            );
        } else {
            fprintf(
                stderr,
                "generate_user_config: use_persistence_paths requested but no instance persistence path is set; "
                "leaving paths unchanged.\n"
            );
        }
    }

    // The path the config is actually written to (after any persistence routing).
    // Returned to the caller so it can hand the exact path to start(). Empty only
    // when no output was given and no routing applied (the node wrote its own
    // default relative to the cwd, which the module can't resolve).
    std::string resolved_output;
    if (parsed_args.contains("output") && parsed_args["output"].is_string())
        resolved_output = parsed_args["output"].get<std::string>();

    const OwnedGenerateConfigArgs owned_args(parsed_args);

    OperationStatus status = ::generate_user_config(owned_args.ffi_args);
    if (!is_ok(&status)) {
        return result::err(operation_status::take_message(status));
    }

    return result::ok(resolved_output);
}

StdLogosResult LogosBlockchainModule::start(const std::string& config_path, const std::string& deployment) {
    if (node) {
        fprintf(stderr, "Could not execute the operation: The node is already running.\n");
        return result::err("The node is already running.");
    }

    std::string effective_config_path = config_path;

    if (effective_config_path.empty()) {
        const char* env = std::getenv("LB_CONFIG_PATH");
        if (env && *env) {
            effective_config_path = env;
            fprintf(stderr, "Using config from LB_CONFIG_PATH: %s\n", effective_config_path.c_str());
        } else {
            fprintf(stderr, "Config path was not specified and LB_CONFIG_PATH is not set.\n");
            return result::err("Config path was not specified and LB_CONFIG_PATH is not set.");
        }
    }

    effective_config_path = localPathFromFileUrl(effective_config_path);
    const std::string deployment_path = localPathFromFileUrl(deployment);

    const char* config_path_ptr = effective_config_path.empty() ? nullptr : effective_config_path.c_str();
    const char* deployment_ptr = deployment_path.empty() ? nullptr : deployment_path.c_str();

    auto [value, error] = start_lb_node(config_path_ptr, deployment_ptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    node = value;

    if (!node) {
        return result::err("Could not subscribe to block events: the node is not running.");
    }

    s_instance = this;
    OperationStatus subscribe_status = subscribe_to_new_blocks(node, on_new_block_callback);
    if (!is_ok(&subscribe_status)) {
        return result::err(operation_status::take_message(subscribe_status));
    }
    OperationStatus processed_status = subscribe_to_processed_blocks(node, on_processed_block_callback);
    if (!is_ok(&processed_status)) {
        return result::err(operation_status::take_message(processed_status));
    }
    OperationStatus lib_status = subscribe_to_lib_blocks(node, on_lib_block_callback);
    return result::from_operation_status(lib_status);
}

StdLogosResult LogosBlockchainModule::stop() {
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return result::err("The node is not running.");
    }

    LogosBlockchainNode* const shutting_down = node;
    node = nullptr;
    s_instance = nullptr;

    OperationStatus status = shutdown_node(shutting_down);
    if (!is_ok(&status)) {
        const std::string message = operation_status::take_message(status);
        fprintf(stderr, "Could not stop the node: %s\n", message.c_str());
        return result::err("Could not stop the node: " + message);
    }

    return result::ok();
}

// State management

StdLogosResult LogosBlockchainModule::does_state_exist() const {
    const std::string& persistence_path = instancePersistencePath();
    if (persistence_path.empty()) {
        return result::err("This instance has no persistence path, so the module laid out no state directory.");
    }

    const fs::path state_path = state_dir(persistence_path);

    std::error_code error_code;
    const bool does_exist = fs::exists(state_path, error_code);
    if (error_code) {
        return result::err("Failed to check " + state_path.string() + ": " + error_code.message());
    }

    return result::ok(does_exist);
}

StdLogosResult LogosBlockchainModule::purge_state() const {
    if (node) {
        fprintf(stderr, "Could not purge state: the node is running.\n");
        return result::err("The node is running. Stop it before purging state.");
    }

    const std::string& persistence_path = instancePersistencePath();
    if (persistence_path.empty()) {
        return result::err("This instance has no persistence path, so the module laid out no state directory.");
    }

    const fs::path state_path = state_dir(persistence_path);
    std::error_code error_code;
    fs::remove_all(state_path, error_code);
    if (error_code) {
        fprintf(stderr, "Failed to purge %s: %s\n", state_path.string().c_str(), error_code.message().c_str());
        return result::err("Failed to remove " + state_path.string() + ": " + error_code.message());
    }

    return result::ok();
}

// Config management

StdLogosResult LogosBlockchainModule::update_user_config(
    const std::string& user_config_path,
    const std::string& keystore_path
) {
    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::update_user_config(config.c_str(), keystore.c_str());
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::migrate_user_config(
    const std::string& output_path,
    const std::string& keystore_path
) {
    const std::string output = localPathFromFileUrl(output_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::migrate_user_config(output.c_str(), keystore.c_str());
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::migrate_user_config_0_1_2(
    const std::string& new_config_path,
    const std::string& old_config_path,
    const std::string& keystore_path
) {
    const std::string new_config = localPathFromFileUrl(new_config_path);
    const std::string old_config = localPathFromFileUrl(old_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::migrate_user_config_0_1_2(new_config.c_str(), old_config.c_str(), keystore.c_str());
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::participate(
    const std::string& config_path,
    const std::string& keystore_path,
    const std::string& output_dir,
    const std::string& external_address
) {
    const std::string config = localPathFromFileUrl(config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const std::string output = localPathFromFileUrl(output_dir);
    const char* external_address_ptr = external_address.empty() ? nullptr : external_address.c_str();

    OperationStatus status = ::participate(config.c_str(), keystore.c_str(), output.c_str(), external_address_ptr);
    return result::from_operation_status(status);
}

// Keystore

StdLogosResult LogosBlockchainModule::generate_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_type,
    const std::string& key_title
) {
    KeyType type{};
    if (!parse_key_type(key_type, type)) {
        return result::err(R"(Invalid key_type (expected "ed25519" or "zk").)");
    }

    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const char* key_title_ptr = key_title.empty() ? nullptr : key_title.c_str();

    auto [value, error] = ::generate_key(config.c_str(), keystore.c_str(), type, key_title_ptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    const std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free key id string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(out);
}

StdLogosResult LogosBlockchainModule::add_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_type,
    const std::string& key_hex,
    const std::string& key_title
) {
    KeyType type{};
    if (!parse_key_type(key_type, type)) {
        fprintf(stderr, "Invalid key_type (expected \"ed25519\" or \"zk\").\n");
        return result::err(R"(Invalid key_type (expected "ed25519" or "zk").)");
    }

    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const char* key_title_ptr = key_title.empty() ? nullptr : key_title.c_str();

    OperationStatus status = ::add_key(config.c_str(), keystore.c_str(), type, key_hex.c_str(), key_title_ptr);
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::remove_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_title
) {
    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::remove_key(config.c_str(), keystore.c_str(), key_title.c_str());
    return result::from_operation_status(status);
}

// Identity

StdLogosResult LogosBlockchainModule::get_peer_id(const std::string& config_path) {
    const std::string config = localPathFromFileUrl(config_path);

    auto [value, error] = ::get_peer_id(config.c_str());
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    const std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free peer id string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(out);
}

// Wallet

StdLogosResult LogosBlockchainModule::wallet_get_balance(const std::string& address_hex) const {
    fprintf(stderr, "wallet_get_balance: address_hex=%s\n", address_hex.c_str());
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(address_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Address must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = get_balance(node, bytes.data(), nullptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return result::ok(std::to_string(value));
}

StdLogosResult LogosBlockchainModule::wallet_transfer_funds(
    const std::string& change_public_key,
    const std::vector<std::string>& sender_addresses,
    const std::string& recipient_address,
    const std::string& amount,
    const std::string& optional_tip_hex
) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    std::string amount_trimmed = amount;
    boost::algorithm::trim(amount_trimmed);
    uint64_t amount_val = 0;
    auto [ptr, ec] = std::from_chars(amount_trimmed.data(), amount_trimmed.data() + amount_trimmed.size(), amount_val);
    if (ec != std::errc{} || ptr != amount_trimmed.data() + amount_trimmed.size() || amount_trimmed.empty()) {
        return result::err("Invalid amount (positive integer required).");
    }

    const std::vector<uint8_t> change_bytes = parse_address_hex(change_public_key);
    if (change_bytes.empty() || static_cast<int>(change_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid change_public_key (64 hex characters required).");
    }
    const std::vector<uint8_t> recipient_bytes = parse_address_hex(recipient_address);
    if (recipient_bytes.empty() || static_cast<int>(recipient_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid recipient_address (64 hex characters required).");
    }
    if (sender_addresses.empty()) {
        return result::err("At least one sender address is required.");
    }
    std::vector<std::vector<uint8_t>> funding_bytes;
    for (const std::string& hex : sender_addresses) {
        std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return result::err("Invalid sender address (64 hex characters required).");
        }
        funding_bytes.push_back(std::move(b));
    }
    std::vector<const uint8_t*> funding_ptrs;
    for (const auto& b : funding_bytes)
        funding_ptrs.push_back(b.data());

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    TransferFundsArguments args{};
    args.optional_tip = optional_tip;
    args.change_public_key = change_bytes.data();
    args.funding_public_keys = funding_ptrs.data();
    args.funding_public_keys_len = funding_ptrs.size();
    args.recipient_public_key = recipient_bytes.data();
    args.amount = amount_val;

    auto [value, error] = transfer_funds(node, &args);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES));
}

StdLogosResult LogosBlockchainModule::wallet_get_known_addresses() const {
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return result::err("The node is not running.");
    }
    auto [value, error] = get_known_addresses(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < value.len; ++i) {
        // ReSharper disable once CppTooWideScope
        const uint8_t* ptr = value.addresses[i];
        if (ptr) {
            out.push_back(bytes_to_hex(ptr, ADDRESS_BYTES));
        }
    }
    OperationStatus free_status = free_known_addresses(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free known addresses: %s\n", operation_status::take_message(free_status).c_str());
    }
    fprintf(
        stderr,
        "blockchain lib: known addresses, count=%zu sample:%s\n",
        out.size(),
        out.empty() ? "(none)" : out.front().c_str()
    );
    return result::ok(std::move(out));
}

StdLogosResult LogosBlockchainModule::wallet_get_notes(
    const std::string& wallet_address_hex,
    const std::string& optional_tip_hex
) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> address_bytes = parse_address_hex(wallet_address_hex);
    if (address_bytes.empty() || static_cast<int>(address_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid wallet address (64 hex characters required).");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    auto [value, error] = get_wallet_notes(node, address_bytes.data(), optional_tip);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["tip"] = bytes_to_hex(value.tip, TX_HASH_BYTES);
    json notes = json::array();
    for (size_t i = 0; i < value.len; ++i) {
        const auto& [note_id, note_value] = value.notes[i];
        json n;
        n["id"] = bytes_to_hex(note_id, TX_HASH_BYTES);
        // Value is u64; serialized as a string to avoid JSON number precision loss.
        n["value"] = std::to_string(note_value);
        notes.push_back(std::move(n));
    }
    obj["notes"] = std::move(notes);

    OperationStatus free_status = free_wallet_notes(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free wallet notes: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(obj.dump());
}

StdLogosResult LogosBlockchainModule::wallet_get_leader_aged_notes(const std::string& optional_tip_hex) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    auto [value, error] = get_leader_aged_notes(node, optional_tip);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["tip"] = bytes_to_hex(value.tip, TX_HASH_BYTES);
    json notes = json::array();
    for (size_t i = 0; i < value.len; ++i) {
        const auto& [note_id, note_value, public_key] = value.notes[i];
        json n;
        n["id"] = bytes_to_hex(note_id, TX_HASH_BYTES);
        // Value is u64; serialized as a string to avoid JSON number precision loss.
        n["value"] = std::to_string(note_value);
        n["public_key"] = bytes_to_hex(public_key, ADDRESS_BYTES);
        notes.push_back(std::move(n));
    }
    obj["notes"] = std::move(notes);
    obj["total_value"] = std::to_string(value.total_value);

    OperationStatus free_status = free_leader_aged_notes(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free leader aged notes: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(obj.dump());
}

StdLogosResult LogosBlockchainModule::leader_claim() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::leader_claim(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), TX_HASH_BYTES));
}

// Channel

StdLogosResult LogosBlockchainModule::channel_deposit(
    const std::string& channel_id_hex,
    const std::string& funding_public_key_hex,
    const std::string& amount,
    const std::string& metadata_hex,
    const std::string& optional_tip_hex
) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    std::string amount_trimmed = amount;
    boost::algorithm::trim(amount_trimmed);
    uint64_t amount_val = 0;
    auto [ptr, ec] = std::from_chars(amount_trimmed.data(), amount_trimmed.data() + amount_trimmed.size(), amount_val);
    if (ec != std::errc{} || ptr != amount_trimmed.data() + amount_trimmed.size() || amount_trimmed.empty()) {
        return result::err("Invalid amount (positive integer required).");
    }
    if (amount_val == 0) {
        return result::err("Invalid amount (must be greater than zero).");
    }

    const std::vector<uint8_t> channel_bytes = parse_address_hex(channel_id_hex);
    if (channel_bytes.empty() || static_cast<int>(channel_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid channel_id (64 hex characters required).");
    }

    const std::vector<uint8_t> funding_bytes = parse_address_hex(funding_public_key_hex);
    if (funding_bytes.empty() || static_cast<int>(funding_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid funding_public_key (64 hex characters required).");
    }

    std::vector<uint8_t> metadata_bytes;
    if (!metadata_hex.empty() && !parse_hex_bytes(metadata_hex, metadata_bytes)) {
        return result::err("Invalid metadata (even-length hex string required).");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    ChannelDepositArguments args{};
    args.optional_tip = optional_tip;
    args.channel_id = channel_bytes.data();
    args.funding_public_key = funding_bytes.data();
    args.amount = amount_val;
    args.metadata = metadata_bytes.empty() ? nullptr : metadata_bytes.data();
    args.metadata_len = metadata_bytes.size();

    auto [value, error] = ::channel_deposit(node, &args);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES));
}

StdLogosResult LogosBlockchainModule::channel_deposit_with_notes(
    const std::string& channel_id_hex,
    const std::vector<std::string>& input_note_id_hexes,
    const std::string& metadata_hex,
    const std::string& change_public_key_hex,
    const std::vector<std::string>& funding_public_key_hexes,
    const std::string& max_tx_fee,
    const std::string& optional_tip_hex
) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> channel_bytes = parse_address_hex(channel_id_hex);
    if (channel_bytes.empty() || static_cast<int>(channel_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid channel_id (64 hex characters required).");
    }

    if (input_note_id_hexes.empty()) {
        return result::err("At least one input note is required.");
    }
    // Note IDs are 32-byte values stored contiguously so the buffer can be passed
    // as a `NoteId` (uint8_t[32]) array.
    std::vector<uint8_t> note_ids_flat;
    note_ids_flat.reserve(input_note_id_hexes.size() * ADDRESS_BYTES);
    for (const std::string& hex : input_note_id_hexes) {
        const std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return result::err("Invalid input note id (64 hex characters required).");
        }
        note_ids_flat.insert(note_ids_flat.end(), b.begin(), b.end());
    }

    const std::vector<uint8_t> change_bytes = parse_address_hex(change_public_key_hex);
    if (change_bytes.empty() || static_cast<int>(change_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid change_public_key (64 hex characters required).");
    }

    if (funding_public_key_hexes.empty()) {
        return result::err("At least one funding public key is required.");
    }
    std::vector<std::vector<uint8_t>> funding_bytes;
    for (const std::string& hex : funding_public_key_hexes) {
        std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return result::err("Invalid funding public key (64 hex characters required).");
        }
        funding_bytes.push_back(std::move(b));
    }
    std::vector<const uint8_t*> funding_ptrs;
    funding_ptrs.reserve(funding_bytes.size());
    for (const auto& b : funding_bytes)
        funding_ptrs.push_back(b.data());

    std::string fee_trimmed = max_tx_fee;
    boost::algorithm::trim(fee_trimmed);
    uint64_t max_tx_fee_val = 0;
    auto [ptr, ec] = std::from_chars(fee_trimmed.data(), fee_trimmed.data() + fee_trimmed.size(), max_tx_fee_val);
    if (ec != std::errc{} || ptr != fee_trimmed.data() + fee_trimmed.size() || fee_trimmed.empty()) {
        return result::err("Invalid max_tx_fee (non-negative integer required).");
    }

    std::vector<uint8_t> metadata_bytes;
    if (!metadata_hex.empty() && !parse_hex_bytes(metadata_hex, metadata_bytes)) {
        return result::err("Invalid metadata (even-length hex string required).");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    ChannelDepositWithNotesArguments args{};
    args.optional_tip = optional_tip;
    args.channel_id = channel_bytes.data();
    args.input_note_ids = reinterpret_cast<const NoteId*>(note_ids_flat.data());
    args.input_note_ids_len = input_note_id_hexes.size();
    args.metadata = metadata_bytes.empty() ? nullptr : metadata_bytes.data();
    args.metadata_len = metadata_bytes.size();
    args.change_public_key = change_bytes.data();
    args.funding_public_keys = funding_ptrs.data();
    args.funding_public_keys_len = funding_ptrs.size();
    args.max_tx_fee = max_tx_fee_val;

    auto [value, error] = ::channel_deposit_with_notes(node, &args);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES));
}

StdLogosResult LogosBlockchainModule::get_channel_state(const std::string& channel_id_hex) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(channel_id_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid channel_id (64 hex characters required).");
    }

    auto [value, error] = ::get_channel_state(node, bytes.data());
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(
            stderr, "Failed to free channel state string: %s\n", operation_status::take_message(free_status).c_str()
        );
    }
    return result::ok(std::move(out));
}

StdLogosResult LogosBlockchainModule::wallet_get_claimable_vouchers() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = get_claimable_vouchers(node, nullptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["tip"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(&value.tip), ADDRESS_BYTES);
    obj["vouchers"] = json::array();

    for (size_t i = 0; i < value.len; ++i) {
        const auto& [commitment, nullifier] = value.vouchers[i];
        obj["vouchers"].push_back({
            {"commitment", bytes_to_hex(reinterpret_cast<const uint8_t*>(&commitment), ADDRESS_BYTES)},
            {"nullifier", bytes_to_hex(reinterpret_cast<const uint8_t*>(&nullifier), ADDRESS_BYTES)},
        });
    }

    // Value is u64; serialized as a string to avoid JSON number precision loss.
    obj["reward_amount"] = std::to_string(value.reward_amount);
    obj["total_claimable"] = std::to_string(value.total_claimable);

    OperationStatus free_status = free_claimable_vouchers(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free claimable vouchers: %s\n", operation_status::take_message(free_status).c_str());
    }

    return result::ok(obj.dump());
}

StdLogosResult LogosBlockchainModule::wallet_fund_tx(const std::string& request_json) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::wallet_fund_tx(node, request_json.c_str());
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free funded tx string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(std::move(out));
}

// Transactions

StdLogosResult LogosBlockchainModule::submit_signed_transaction(const std::string& signed_tx_json) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::submit_signed_transaction(node, signed_tx_json.c_str());
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), TX_HASH_BYTES));
}

// Blend

StdLogosResult LogosBlockchainModule::blend_join_as_core_node(
    const std::string& locator,
    const std::string& locked_note_id_hex
) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    if (locator.empty()) {
        return result::err("Invalid locator (must not be empty).");
    }

    const std::vector<uint8_t> locked_note_id_bytes = parse_address_hex(locked_note_id_hex);
    if (locked_note_id_bytes.empty() || static_cast<int>(locked_note_id_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid locked_note_id_hex (64 hex characters required).");
    }

    auto [value, error] = ::blend_join_as_core_node(node, locator.c_str(), locked_note_id_bytes.data());
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string declaration_id = bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    fprintf(stderr, "Successfully joined as a core node. DeclarationId: %s\n", declaration_id.c_str());
    return result::ok(std::move(declaration_id));
}

StdLogosResult LogosBlockchainModule::blend_info() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::blend_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free blend info string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(std::move(out));
}

// Chain

StdLogosResult LogosBlockchainModule::get_chain_id() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_chain_id(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free chain ID string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(std::move(out));
}

// Network

StdLogosResult LogosBlockchainModule::get_network_info() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_network_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["n_peers"] = static_cast<int64_t>(value.n_peers);
    obj["n_connections"] = value.n_connections;
    obj["n_pending_connections"] = value.n_pending_connections;
    obj["n_discovered_peers"] = static_cast<int64_t>(value.n_discovered_peers);
    return result::ok(obj.dump());
}

// The ONLY call in this module that reads the keystore, and it reads the public
// half of it: keystore.yaml holds `public_keys` (title -> key id) and
// `secret_keys` (title -> key) as two separate mappings, and this is scoped to
// the first. Titles and key ids are not secrets.
//
// Kept apart from config_get_wallet_keys deliberately. That one reads the config
// and is harmless; this one opens a file that is expected to become
// password-protected, so it gets its own name, its own failure, and its own
// audit point — and can be swapped for a node-side API without touching the
// config path.
//
// Titles live ONLY in this file. The running node discards them at load (the
// wallet keeps ZkPublicKey -> KeyId, and a KeyId is not a KeyTitle), so no
// node-side call can recover them.
//
// Fails closed by construction. If the file is absent, unreadable, or encrypted
// into an envelope with no `public_keys` mapping, the result is an empty set
// rather than an error: titles are decoration, and every caller must render
// without them. If instead only `secret_keys` is encrypted — the likely shape,
// given the two are separate mappings — this keeps working with no password.
StdLogosResult LogosBlockchainModule::get_key_titles(const std::string& config_path) {
    const fs::path config = localPathFromFileUrl(config_path);
    if (config.empty()) {
        return result::err("Config path was not specified.");
    }

    // The node's own default: "Defaults to 'keystore.yaml' in the same directory
    // as --output", and the file update/migrate/participate are handed.
    const fs::path keystore = config.parent_path() / "keystore.yaml";

    nlohmann::json titles = nlohmann::json::object();
    std::string text;
    std::string error;
    if (!yaml::read_file(keystore, text, error)) {
        return result::ok(titles.dump());
    }
    const std::vector<std::string> lines = yaml::split_lines(text);
    for (auto& [title, public_key] : yaml::pairs_under(lines, {"public_keys"}))
        titles[public_key] = title;
    return result::ok(titles.dump());
}

// Explorer

StdLogosResult LogosBlockchainModule::get_block(const std::string& header_id_hex) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(header_id_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Header ID must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_block(node, reinterpret_cast<const HeaderId*>(bytes.data()));
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free block string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(std::move(out));
}

StdLogosResult LogosBlockchainModule::get_blocks(const uint64_t from_slot, const uint64_t to_slot) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_blocks(node, from_slot, to_slot);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free blocks string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(std::move(out));
}

StdLogosResult LogosBlockchainModule::get_transaction(const std::string& tx_hash_hex) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(tx_hash_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Transaction hash must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_transaction(node, reinterpret_cast<const TxHash*>(bytes.data()));
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free transaction string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(std::move(out));
}

// Cryptarchia

StdLogosResult LogosBlockchainModule::get_cryptarchia_info() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_cryptarchia_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["lib"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->lib), ADDRESS_BYTES);
    obj["lib_slot"] = static_cast<int64_t>(value->lib_slot);
    obj["tip"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->tip), ADDRESS_BYTES);
    obj["slot"] = static_cast<int64_t>(value->slot);
    obj["height"] = static_cast<int64_t>(value->height);
    switch (value->mode) {
    case State::Online:
        obj["mode"] = "Online";
        break;
    case State::NotStarted:
        obj["mode"] = "NotStarted";
        break;
    default:
        obj["mode"] = "Bootstrapping";
        break;
    }

    OperationStatus free_status = free_cryptarchia_info(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free cryptarchia info: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(obj.dump());
}

StdLogosResult LogosBlockchainModule::get_block_events(const std::string& header_id_hex) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(header_id_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Header ID must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_block_events(node, reinterpret_cast<const HeaderId*>(bytes.data()));
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(
            stderr, "Failed to free block events string: %s\n", operation_status::take_message(free_status).c_str()
        );
    }
    return result::ok(std::move(out));
}

// Time

StdLogosResult LogosBlockchainModule::get_time_info() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_time_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["slot_duration_ms"] = static_cast<int64_t>(value->slot_duration_ms);
    obj["genesis_time_unix_ms"] = value->genesis_time_unix_ms;
    obj["current_slot"] = static_cast<int64_t>(value->current_slot);
    obj["current_epoch"] = value->current_epoch;

    OperationStatus free_status = free_time_info(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free time info: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(obj.dump());
}

// PoW

StdLogosResult LogosBlockchainModule::pow_start_mining() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    OperationStatus status = ::pow_start_mining(node);
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::pow_stop_mining() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    OperationStatus status = ::pow_stop_mining(node);
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::pow_start_auto_claim() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    OperationStatus status = ::pow_start_auto_claim(node);
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::pow_stop_auto_claim() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    OperationStatus status = ::pow_stop_auto_claim(node);
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::pow_claim(const std::string& claim_address_hex) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    // A null claim address pays out to whichever auto-claim target is furthest below its threshold.
    std::vector<uint8_t> claim_address_bytes;
    const uint8_t* claim_address = nullptr;
    if (!claim_address_hex.empty()) {
        claim_address_bytes = parse_address_hex(claim_address_hex);
        if (static_cast<int>(claim_address_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid claim address (64 hex characters or empty).");
        }
        claim_address = claim_address_bytes.data();
    }

    auto [value, error] = ::pow_claim(node, claim_address);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), TX_HASH_BYTES));
}

StdLogosResult LogosBlockchainModule::pow_claimable_rewards() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::pow_claimable_rewards(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["claimable_tickets"] = static_cast<int64_t>(value.claimable_tickets);
    obj["slots_until_expiry"] = json::array();
    for (size_t i = 0; i < value.len; ++i) {
        obj["slots_until_expiry"].push_back(static_cast<int64_t>(value.slots_until_expiry[i]));
    }

    OperationStatus free_status = free_pow_claimable_rewards(value);
    if (!is_ok(&free_status)) {
        fprintf(
            stderr, "Failed to free PoW claimable rewards: %s\n", operation_status::take_message(free_status).c_str()
        );
    }
    return result::ok(obj.dump());
}

namespace {
    // One validated auto-claim destination, in the shape the node's config uses.
    struct ClaimTargetEntry {
        std::string public_key;
        uint64_t threshold;
    };

    // Canonicalises one target against an already-read config. An empty key
    // resolves to the leader's funding key — PoS stakes from it, so paying mined
    // rewards there is what turns mining into stake. The result is checked
    // against wallet.known_keys the way the node's own validate_claim_targets
    // does, turning a node that would refuse to boot into an error the caller
    // can act on while the config is still being written.
    bool canonical_claim_target(
        const std::vector<std::string>& lines,
        const std::string& config_path,
        const std::string& requested_hex,
        std::string& out_hex,
        std::string& error
    ) {
        std::string target_hex = requested_hex;
        boost::algorithm::trim(target_hex);
        if (target_hex.empty()) {
            const size_t line = yaml::find_path(lines, {"cryptarchia", "leader", "wallet", "funding_pk"});
            if (line == yaml::NPOS) {
                error = "cryptarchia.leader.wallet.funding_pk not found in " + config_path + ".";
                return false;
            }
            target_hex = yaml::scalar_of(lines[line]);
        }

        // Canonicalise before comparing and writing: the caller may pass a 0x
        // prefix or upper case, neither of which the node's own config uses.
        const std::vector<uint8_t> target_bytes = parse_address_hex(target_hex);
        if (static_cast<int>(target_bytes.size()) != ADDRESS_BYTES) {
            error = "Invalid claim address (64 hex characters or empty).";
            return false;
        }
        out_hex = bytes_to_hex(target_bytes.data(), target_bytes.size());

        // An included wallet section would read as holding no keys at all, so
        // every target below would be rejected as untracked. Say what is
        // actually wrong instead.
        if (const std::string segment = yaml::scalar_segment(lines, {"wallet", "known_keys"});
            !segment.empty()) {
            error = segment + " in " + config_path +
                    " is not written as a block mapping (an !include tag or a flow mapping); this "
                    "module can only edit a config as generate_user_config writes it.";
            return false;
        }

        // The wallet only indexes the keys it is told to track, so an unlisted
        // target reports an empty balance forever and the node aborts startup
        // rather than claim into it.
        if (!yaml::maps_to_value(lines, {"wallet", "known_keys"}, out_hex)) {
            error =
                "Claim address " + out_hex + " is not in wallet.known_keys, so the node would refuse to start.";
            return false;
        }
        return true;
    }

    // Writes `targets` into pow.auto_claim.targets. An empty list renders the
    // flow-style `[]` a generated config already carries, which is what leaves
    // auto-claim off.
    bool write_claim_targets(
        std::vector<std::string>& lines,
        const std::vector<ClaimTargetEntry>& targets,
        std::string& error
    ) {
        const auto render = [&targets](const int indent) {
            const std::string pad(static_cast<size_t>(indent), ' ');
            std::vector<std::string> out;
            if (targets.empty()) {
                out.push_back(pad + "targets: []");
                return out;
            }
            out.push_back(pad + "targets:");
            for (const ClaimTargetEntry& target : targets) {
                out.push_back(pad + "- public_key: " + target.public_key);
                out.push_back(pad + "  threshold: " + std::to_string(target.threshold));
            }
            return out;
        };
        return yaml::set_value_at(lines, {"pow", "auto_claim", "targets"}, render, error);
    }

    // A u64 does not survive a round trip through QML's doubles, so thresholds
    // arrive as text; accept a JSON number too for callers that can send one.
    bool parse_threshold(const nlohmann::json& raw, uint64_t& out, std::string& error) {
        if (raw.is_number_unsigned()) {
            out = raw.get<uint64_t>();
            return true;
        }
        if (!raw.is_string()) {
            error = "threshold must be a non-negative integer or a decimal string.";
            return false;
        }
        std::string text = raw.get<std::string>();
        boost::algorithm::trim(text);
        const char* const begin = text.data();
        const char* const end = begin + text.size();
        const auto [ptr, ec] = std::from_chars(begin, end, out);
        if (ec != std::errc() || ptr != end) {
            error = "Invalid threshold '" + text + "'.";
            return false;
        }
        return true;
    }
} // namespace

namespace {
    // How a caller named an optional field. Absent and Null are deliberately
    // distinct: max_threads is an Option on the node's side, so "let rayon
    // decide" is a value the API has to be able to express, and an absent field
    // already means "leave this alone". The fields the node types as non-Option
    // reject Null instead — writing one would only produce a config that fails
    // to deserialize at startup.
    enum class FieldState { Absent, Null, Set };

    bool read_optional_u64(
        const nlohmann::json& obj,
        const char* key,
        uint64_t& out,
        FieldState& state,
        std::string& error
    ) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            state = FieldState::Absent;
            return true;
        }
        if (it->is_null()) {
            state = FieldState::Null;
            return true;
        }
        state = FieldState::Set;
        if (!parse_threshold(*it, out, error)) {
            error = std::string(key) + ": " + error;
            return false;
        }
        return true;
    }

    // Rejects field names this API does not know. A typo like "max_thread"
    // would otherwise be dropped on the floor and reported back as a successful
    // write, so a frontend a version ahead of (or behind) this module looks
    // like it changed a setting it never changed. The node reads its own config
    // the same way — OnUnknownKeys::Fail — so this only matches the strictness
    // the file is going to meet at startup anyway.
    bool reject_unknown_fields(
        const nlohmann::json& obj,
        const std::vector<std::string>& known,
        const std::string& what,
        std::string& error
    ) {
        std::string unknown;
        size_t count = 0;
        for (const auto& item : obj.items()) {
            if (std::find(known.begin(), known.end(), item.key()) != known.end())
                continue;
            if (count > 0)
                unknown += ", ";
            unknown += item.key();
            ++count;
        }
        if (count == 0) {
            return true;
        }
        error = "Unknown " + what + (count > 1 ? " fields: " : " field: ") + unknown + ".";
        return false;
    }
} // namespace

StdLogosResult LogosBlockchainModule::pow_configure(
    const std::string& config_path,
    const std::string& config_json
) {
    const fs::path config = localPathFromFileUrl(config_path);
    if (config.empty()) {
        return result::err("Config path was not specified.");
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(config_json);
    } catch (const nlohmann::json::exception& e) {
        return result::err(std::string("Invalid PoW config JSON: ") + e.what());
    }
    if (!parsed.is_object()) {
        return result::err("config_json must be a JSON object.");
    }

    std::string error;
    if (!reject_unknown_fields(
            parsed,
            {"max_threads", "max_tickets_per_block", "tick_seconds", "auto_claim_targets"},
            "PoW config", error
        )) {
        return result::err(std::move(error));
    }

    std::string text;
    if (!yaml::read_file(config, text, error)) {
        return result::err(std::move(error));
    }
    std::vector<std::string> lines = yaml::split_lines(text);

    // Everything is validated against the file as read before a single line is
    // edited. A config half-written with one good setting and one bad one is a
    // node that will not boot, which is strictly worse than changing nothing.
    // max_threads is the one Option on the node's side: null puts the search
    // pool back to rayon's own default, which is the only way to undo a
    // previously pinned thread count.
    uint64_t max_threads = 0;
    FieldState max_threads_state = FieldState::Absent;
    if (!read_optional_u64(parsed, "max_threads", max_threads, max_threads_state, error)) {
        return result::err(std::move(error));
    }
    // The node types this as a NonZeroUsize, so a 0 is a deserialization error
    // it only reports at startup.
    if (max_threads_state == FieldState::Set && max_threads == 0) {
        return result::err("Invalid max_threads (must be at least 1, or null for automatic).");
    }

    uint64_t max_tickets = 0;
    FieldState max_tickets_state = FieldState::Absent;
    if (!read_optional_u64(parsed, "max_tickets_per_block", max_tickets, max_tickets_state, error)) {
        return result::err(std::move(error));
    }
    // Not an Option on the node's side: a null here would be written straight
    // into a config that then fails to load, so it is refused rather than
    // quietly treated as "leave it alone".
    if (max_tickets_state == FieldState::Null) {
        return result::err("max_tickets_per_block cannot be null; omit it to leave it unchanged.");
    }
    if (max_tickets_state == FieldState::Set && max_tickets == 0) {
        return result::err("Invalid max_tickets_per_block (must be at least 1).");
    }

    uint64_t tick_seconds = 0;
    FieldState tick_state = FieldState::Absent;
    if (!read_optional_u64(parsed, "tick_seconds", tick_seconds, tick_state, error)) {
        return result::err(std::move(error));
    }
    if (tick_state == FieldState::Null) {
        return result::err("tick_seconds cannot be null; omit it to leave it unchanged.");
    }
    if (tick_state == FieldState::Set && tick_seconds == 0) {
        return result::err("Invalid tick_seconds (must be at least 1).");
    }

    const bool has_max_threads = max_threads_state != FieldState::Absent;
    const bool has_max_tickets = max_tickets_state != FieldState::Absent;
    const bool has_tick = tick_state != FieldState::Absent;

    // Absent leaves the existing target list alone; an empty array clears it,
    // which is how auto-claim is turned off.
    std::vector<ClaimTargetEntry> targets;
    const auto targets_it = parsed.find("auto_claim_targets");
    const bool has_targets = targets_it != parsed.end() && !targets_it->is_null();
    if (has_targets) {
        if (!targets_it->is_array()) {
            return result::err("auto_claim_targets must be a JSON array.");
        }
        targets.reserve(targets_it->size());
        for (const nlohmann::json& entry : *targets_it) {
            if (!entry.is_object()) {
                return result::err("Each target must be an object with public_key and threshold.");
            }
            if (!reject_unknown_fields(entry, {"public_key", "threshold"}, "claim target", error)) {
                return result::err(std::move(error));
            }
            // Required, because the empty string is a real value here — it
            // names the leader's funding key — and a missing field silently
            // meaning "pay the leader" turns a malformed payload into a change
            // of where the node's mining income lands.
            const auto key = entry.find("public_key");
            if (key == entry.end() || key->is_null()) {
                return result::err(
                    "Each target needs a public_key (an empty string means the leader's funding key)."
                );
            }
            if (!key->is_string()) {
                return result::err("public_key must be a string.");
            }
            const std::string requested = key->get<std::string>();
            uint64_t threshold = 0;
            const auto raw = entry.find("threshold");
            if (raw == entry.end() || raw->is_null()) {
                return result::err("Each target needs a threshold.");
            }
            if (!parse_threshold(*raw, threshold, error)) {
                return result::err(std::move(error));
            }

            std::string canonical;
            if (!canonical_claim_target(lines, config.string(), requested, canonical, error)) {
                return result::err(std::move(error));
            }
            // The node pays the neediest target per tick, so a key listed twice
            // only makes which threshold applies ambiguous.
            for (const ClaimTargetEntry& seen : targets) {
                if (seen.public_key == canonical) {
                    return result::err("Duplicate claim target " + canonical + ".");
                }
            }
            targets.push_back(ClaimTargetEntry{std::move(canonical), threshold});
        }
    }

    const auto scalar = [](const char* key, const std::string& value) {
        return [key, value](const int indent) {
            return std::vector<std::string>{
                std::string(static_cast<size_t>(indent), ' ') + key + ": " + value,
            };
        };
    };

    // An explicit null is rendered as `null`, the same way serde_yaml writes a
    // None, so the node falls back to one search thread per logical CPU.
    const std::string max_threads_text =
        max_threads_state == FieldState::Null ? "null" : std::to_string(max_threads);
    if (has_max_threads
        && !yaml::set_value_at(
            lines, {"pow", "mining", "max_threads"}, scalar("max_threads", max_threads_text), error)) {
        return result::err(std::move(error));
    }
    if (has_max_tickets
        && !yaml::set_value_at(
            lines, {"pow", "mining", "max_tickets_per_block"},
            scalar("max_tickets_per_block", std::to_string(max_tickets)), error)) {
        return result::err(std::move(error));
    }
    if (has_tick) {
        // Renders the key itself, the way every other renderer here does: the
        // path names what is being replaced, so emitting only the children would
        // hoist unit/value into auto_claim and the node would reject the file.
        const auto render_tick = [tick_seconds](const int indent) {
            const std::string pad(static_cast<size_t>(indent), ' ');
            return std::vector<std::string>{
                pad + "tick:",
                pad + "  unit: seconds",
                pad + "  value: " + std::to_string(tick_seconds),
            };
        };
        if (!yaml::set_value_at(lines, {"pow", "auto_claim", "tick"}, render_tick, error)) {
            return result::err(std::move(error));
        }
    }
    if (has_targets && !write_claim_targets(lines, targets, error)) {
        return result::err(std::move(error));
    }

    if (!yaml::write_file_atomic(config, yaml::join_lines(lines), error)) {
        return result::err(std::move(error));
    }

    nlohmann::json written = nlohmann::json::object();
    if (max_threads_state == FieldState::Null) {
        written["max_threads"] = nullptr;
    } else if (has_max_threads) {
        written["max_threads"] = max_threads;
    }
    if (has_max_tickets) {
        written["max_tickets_per_block"] = max_tickets;
    }
    if (has_tick) {
        written["tick_seconds"] = tick_seconds;
    }
    if (has_targets) {
        nlohmann::json list = nlohmann::json::array();
        for (const ClaimTargetEntry& target : targets) {
            // Both halves: a target says nothing on its own, since the threshold
            // is what decides whether auto-claim pays it or reports it as
            // already funded and stops. Thresholds go out as decimal strings for
            // the same reason they come in as them — a u64 does not survive a
            // round trip through a JSON reader that parses numbers as doubles.
            list.push_back(nlohmann::json{
                {"public_key", target.public_key},
                {"threshold", std::to_string(target.threshold)},
            });
        }
        written["auto_claim_targets"] = std::move(list);
    }
    fprintf(
        stderr,
        "pow_configure: %s written; auto-claim is %s\n",
        written.dump().c_str(),
        has_targets ? (targets.empty() ? "off" : "on") : "unchanged"
    );
    return result::ok(written.dump());
}

StdLogosResult LogosBlockchainModule::config_get_wallet_keys(const std::string& config_path) {
    const fs::path config = localPathFromFileUrl(config_path);
    if (config.empty()) {
        return result::err("Config path was not specified.");
    }

    std::string text;
    std::string error;
    if (!yaml::read_file(config, text, error)) {
        return result::err(std::move(error));
    }
    const std::vector<std::string> lines = yaml::split_lines(text);

    // An empty known_keys list is a legitimate answer, so it must not double as
    // the report for a wallet section this reader simply could not follow.
    if (const std::string segment = yaml::scalar_segment(lines, {"wallet", "known_keys"});
        !segment.empty()) {
        return result::err(
            segment + " in " + config.string() +
            " is not written as a block mapping (an !include tag or a flow mapping); this module can "
            "only read a config as generate_user_config writes it."
        );
    }

    const auto scalar_at = [&lines](const std::vector<std::string>& path) {
        const size_t line = yaml::find_path(lines, path);
        return line == yaml::NPOS ? std::string() : yaml::scalar_of(lines[line]);
    };

    nlohmann::json obj;
    obj["known_keys"] = yaml::values_under(lines, {"wallet", "known_keys"});
    obj["leader_funding_pk"] = scalar_at({"cryptarchia", "leader", "wallet", "funding_pk"});
    obj["voucher_master_key_id"] = scalar_at({"wallet", "voucher_master_key_id"});
    // The other two jobs a key in this config can hold. A key may hold several
    // at once — a generated config points the leader and SDP wallets at the same
    // funding key — so these are reported separately rather than as one role per
    // key, and the caller composes them.
    obj["sdp_funding_pk"] = scalar_at({"sdp", "wallet", "funding_pk"});
    obj["blend_signing_key_id"] = scalar_at({"blend", "non_ephemeral_signing_key_id"});
    // Deliberately no key types: kms.backend.keys maps each id to "!<Type>
    // <secret>", so reporting the tag means reading lines that carry private key
    // material, and every claim-target candidate is a wallet key anyway.
    return result::ok(obj.dump());
}
