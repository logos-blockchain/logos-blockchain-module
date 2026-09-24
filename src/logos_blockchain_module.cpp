#include "logos_blockchain_module.h"

#include "user_config_reader.h"

#include <algorithm>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <map>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
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

namespace stream {
    // Subscribes unless already subscribed. The flag is set first so two callers can't both subscribe.
    static StdLogosResult subscribe(
        std::atomic<bool>& subscribed,
        const std::function<OperationStatus()>& subscribe_fn
    ) {
        bool expected = false;
        if (!subscribed.compare_exchange_strong(expected, true)) {
            return result::err("The stream is already subscribed.");
        }
        OperationStatus status = subscribe_fn();
        if (!is_ok(&status)) {
            subscribed = false;
        }
        return result::from_operation_status(status);
    }
} // namespace stream

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

    // Whether wallet.known_keys maps any entry to `value`. The wallet only
    // indexes keys it is told to track.
    bool tracks_key(const UserConfigReader& config, const std::string& value) {
        const std::vector<UserConfigReader::Entry> keys = config.entriesAt("/wallet/known_keys");
        return std::any_of(keys.begin(), keys.end(), [&value](const UserConfigReader::Entry& e) {
            return e.second == value;
        });
    }

    // One validated auto-claim destination, in the shape the node's config uses.
    struct ClaimTargetEntry {
        std::string public_key;
        uint64_t threshold;
    };

    // Canonicalises one target against an already-read config. An empty key
    // resolves to the leader's funding key, which PoS stakes from. Checked
    // against wallet.known_keys the way the node's own validate_claim_targets
    // does, so a config that would fail to boot is an error the caller can still
    // act on.
    bool canonical_claim_target(
        const UserConfigReader& config,
        const std::string& config_path,
        const std::string& requested_hex,
        std::string& out_hex,
        std::string& error
    ) {
        std::string target_hex = requested_hex;
        boost::algorithm::trim(target_hex);
        if (target_hex.empty()) {
            target_hex = config.scalarAt("/cryptarchia/leader/wallet/funding_pk");
            if (target_hex.empty()) {
                error = "cryptarchia.leader.wallet.funding_pk not found in " + config_path + ".";
                return false;
            }
        }

        // Canonicalise before comparing and writing: the caller may pass a 0x
        // prefix or upper case, neither of which the node's own config uses.
        const std::vector<uint8_t> target_bytes = parse_address_hex(target_hex);
        if (static_cast<int>(target_bytes.size()) != ADDRESS_BYTES) {
            error = "Invalid claim address (64 hex characters or empty).";
            return false;
        }
        out_hex = bytes_to_hex(target_bytes.data(), target_bytes.size());

        // A wallet section that is not a mapping would read as holding no keys,
        // rejecting every target as untracked. Say what is actually wrong.
        if (!config.isMappingAt("/wallet/known_keys")) {
            error = "wallet.known_keys in " + config_path + " is not a block mapping.";
            return false;
        }

        // The wallet only indexes keys it is told to track; the node aborts
        // startup rather than claim into an unlisted one.
        if (!tracks_key(config, out_hex)) {
            error =
                "Claim address " + out_hex + " is not in wallet.known_keys, so the node would refuse to start.";
            return false;
        }
        return true;
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

    // Absent and Null are distinct: Absent means "leave this alone", and
    // max_threads is an Option on the node's side, so null is a real value.
    // Non-Option fields reject Null rather than write a config that then fails
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

    // Rejects field names this API does not know, so a typo like "max_thread" is
    // not dropped and reported back as a successful write. Matches the node's own
    // OnUnknownKeys::Fail.
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

    // The pow section as a YAML fragment carrying only the fields the caller
    // set. merge_user_config merges it key by key, so an omitted field is left
    // alone and a list is replaced whole — which is exactly the contract this
    // API promises. Empty parents are never emitted: `mining:` with nothing
    // under it is a null, and merging a null would wipe the section.
    std::string pow_fragment(
        const std::string& max_threads, const std::string& max_tickets,
        const std::string& tick_seconds, const std::vector<ClaimTargetEntry>* targets
    ) {
        std::string mining;
        if (!max_threads.empty())
            mining += "    max_threads: " + max_threads + "\n";
        if (!max_tickets.empty())
            mining += "    max_tickets_per_block: " + max_tickets + "\n";

        std::string auto_claim;
        if (!tick_seconds.empty()) {
            auto_claim += "    tick:\n";
            auto_claim += "      unit: seconds\n";
            auto_claim += "      value: " + tick_seconds + "\n";
        }
        if (targets) {
            if (targets->empty()) {
                auto_claim += "    targets: []\n";
            } else {
                auto_claim += "    targets:\n";
                for (const ClaimTargetEntry& target : *targets) {
                    auto_claim += "    - public_key: " + target.public_key + "\n";
                    auto_claim += "      threshold: " + std::to_string(target.threshold) + "\n";
                }
            }
        }

        std::string yaml = "pow:\n";
        if (!mining.empty())
            yaml += "  mining:\n" + mining;
        if (!auto_claim.empty())
            yaml += "  auto_claim:\n" + auto_claim;
        return yaml;
    }
} // namespace

void LogosBlockchainModule::on_new_block_callback(const char* block) {
    if (!s_instance) {
        return;
    }
    if (!block) {
        fprintf(stderr, "New block stream ended.\n");
        s_instance->is_new_blocks_subscribed = false;
        s_instance->newBlock("null");
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
        s_instance->is_processed_blocks_subscribed = false;
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
        s_instance->is_lib_blocks_subscribed = false;
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
    if (StdLogosResult rc = subscribe_to_new_blocks(); !rc.success) {
        return rc;
    }
    if (StdLogosResult rc = subscribe_to_processed_blocks(); !rc.success) {
        return rc;
    }
    return subscribe_to_lib_blocks();
}

StdLogosResult LogosBlockchainModule::subscribe_to_new_blocks() {
    if (!node) {
        return result::err("The node is not running.");
    }
    return stream::subscribe(is_new_blocks_subscribed, [this] {
        return ::subscribe_to_new_blocks(node, on_new_block_callback);
    });
}

StdLogosResult LogosBlockchainModule::subscribe_to_processed_blocks() {
    if (!node) {
        return result::err("The node is not running.");
    }
    return stream::subscribe(is_processed_blocks_subscribed, [this] {
        return ::subscribe_to_processed_blocks(node, on_processed_block_callback);
    });
}

StdLogosResult LogosBlockchainModule::subscribe_to_lib_blocks() {
    if (!node) {
        return result::err("The node is not running.");
    }
    return stream::subscribe(is_lib_blocks_subscribed, [this] {
        return ::subscribe_to_lib_blocks(node, on_lib_block_callback);
    });
}

StdLogosResult LogosBlockchainModule::stop() {
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return result::err("The node is not running.");
    }

    s_instance = nullptr;

    OperationStatus status = shutdown_node(node);
    if (!is_ok(&status)) {
        fprintf(stderr, "Could not stop the node: %s\n", operation_status::take_message(status).c_str());
    }

    node = nullptr;
    is_new_blocks_subscribed = false;
    is_processed_blocks_subscribed = false;
    is_lib_blocks_subscribed = false;
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

StdLogosResult LogosBlockchainModule::merge_user_config(
    const std::string& source_path,
    const std::string& destination_path,
    const std::string& extra_yaml,
    const bool source_insert_missing,
    const bool extra_insert_missing
) {
    const std::string source = localPathFromFileUrl(source_path);
    const std::string destination = localPathFromFileUrl(destination_path);
    const char* extra_yaml_ptr = extra_yaml.empty() ? nullptr : extra_yaml.c_str();
    const MergeConfigFlags flags{source_insert_missing, extra_insert_missing};

    auto [value, error] = ::merge_user_config(source.c_str(), destination.c_str(), extra_yaml_ptr, flags);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    if (!value) {
        return result::ok(std::string{});
    }

    const std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free merge conflicts report: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(out);
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

    UserConfigReader reader;
    if (!reader.load(config, error)) {
        return result::err(std::move(error));
    }

    // Everything is validated against the file as read before anything is
    // written. The write itself is one merge_user_config call, so a rejected
    // field cannot leave the accepted ones applied.
    struct ScalarField {
        const char* name;
        bool nullable;
    };
    static constexpr ScalarField kScalars[] = {
        {"max_threads", true},
        {"max_tickets_per_block", false},
        {"tick_seconds", false},
    };
    enum ScalarIndex { MaxThreads, MaxTickets, TickSeconds, ScalarCount };

    uint64_t values[ScalarCount] = {};
    FieldState states[ScalarCount] = {};
    for (size_t i = 0; i < ScalarCount; ++i) {
        const ScalarField& field = kScalars[i];
        if (!read_optional_u64(parsed, field.name, values[i], states[i], error)) {
            return result::err(std::move(error));
        }
        if (!field.nullable && states[i] == FieldState::Null) {
            return result::err(
                std::string(field.name) + " cannot be null; omit it to leave it unchanged."
            );
        }
        if (states[i] == FieldState::Set && values[i] == 0) {
            return result::err(
                "Invalid " + std::string(field.name) + " (must be at least 1" +
                (field.nullable ? ", or null for automatic" : "") + ")."
            );
        }
    }

    const uint64_t max_threads = values[MaxThreads];
    const uint64_t max_tickets = values[MaxTickets];
    const uint64_t tick_seconds = values[TickSeconds];
    const FieldState max_threads_state = states[MaxThreads];

    const bool has_max_threads = states[MaxThreads] != FieldState::Absent;
    const bool has_max_tickets = states[MaxTickets] != FieldState::Absent;
    const bool has_tick = states[TickSeconds] != FieldState::Absent;

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
            if (!canonical_claim_target(reader, config.string(), requested, canonical, error)) {
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

    // Nothing asked for: merging an empty `pow:` would write a null over the
    // whole section.
    if (!has_max_threads && !has_max_tickets && !has_tick && !has_targets) {
        return result::ok(nlohmann::json::object().dump());
    }

    const std::string max_threads_text = !has_max_threads ? std::string()
        : (max_threads_state == FieldState::Null ? "null" : std::to_string(max_threads));
    const std::string extra_yaml = pow_fragment(
        max_threads_text,
        has_max_tickets ? std::to_string(max_tickets) : std::string(),
        has_tick ? std::to_string(tick_seconds) : std::string(),
        has_targets ? &targets : nullptr
    );

    const StdLogosResult merged = merge_user_config(
        config.string(), config.string(), extra_yaml, false, /*extra_insert_missing=*/true
    );
    if (!merged.success) {
        return merged;
    }

    if (const std::string conflicts = merged.value.get<std::string>(); !conflicts.empty()) {
        return result::err("Could not apply the PoW config:\n" + conflicts);
    }

    fprintf(stderr, "pow_configure: applied\n%s", extra_yaml.c_str());
    return result::ok();
}


// The accounts a config records, named from the keystore beside it
StdLogosResult LogosBlockchainModule::read_accounts(const std::string& config_path) {
    const fs::path config = localPathFromFileUrl(config_path);
    if (config.empty()) {
        return result::err("Config path was not specified.");
    }

    UserConfigReader cfg;
    std::string error;
    if (!cfg.load(config, error)) {
        return result::err(std::move(error));
    }

    if (!cfg.isMappingAt("/wallet/known_keys")) {
        return result::err("wallet.known_keys in " + config.string() + " is not a block mapping.");
    }

    const auto lowered = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return v;
    };

    std::map<std::string, std::string> title_by_id;
    nlohmann::json keystore_keys = nlohmann::json::array();
    {
        UserConfigReader keystore;
        std::string ignored;
        if (keystore.load(config.parent_path() / "keystore.yaml", ignored)) {
            for (const auto& [title, key_id] : keystore.entriesAt("/public_keys")) {
                title_by_id[lowered(key_id)] = title;
                keystore_keys.push_back(nlohmann::json{{"key_id", key_id}, {"title", title}});
            }
        }
    }

    // Roles are whatever the config's own fields point at. A key can hold more
    // than one — a generated config funds leader and SDP from the same key — so
    // they are collected rather than resolved to a single label.
    const std::string leader = cfg.scalarAt("/cryptarchia/leader/wallet/funding_pk");
    const std::string sdp = cfg.scalarAt("/sdp/wallet/funding_pk");
    const std::string voucher = cfg.scalarAt("/wallet/voucher_master_key_id");
    const std::string blend = cfg.scalarAt("/blend/non_ephemeral_signing_key_id");

    nlohmann::json accounts = nlohmann::json::array();
    for (const auto& [key_id, public_key] : cfg.entriesAt("/wallet/known_keys")) {
        const std::string id = lowered(key_id);
        nlohmann::json roles = nlohmann::json::array();
        if (!leader.empty() && public_key == lowered(leader))
            roles.push_back("leader_funding");
        if (!sdp.empty() && public_key == lowered(sdp))
            roles.push_back("sdp_funding");
        // The voucher and blend fields name a key *id*, not a public key.
        if (!voucher.empty() && id == lowered(voucher))
            roles.push_back("voucher_master");
        if (!blend.empty() && id == lowered(blend))
            roles.push_back("blend_signing");

        const auto title = title_by_id.find(id);
        accounts.push_back(nlohmann::json{
            {"public_key", public_key},
            {"title", title == title_by_id.end() ? std::string() : title->second},
            {"roles", std::move(roles)},
        });
    }

    nlohmann::json obj;
    obj["accounts"] = std::move(accounts);
    obj["keystore_keys"] = std::move(keystore_keys);
    return result::ok(obj.dump());
}

// The pow section as the config holds it, in the shape pow_configure takes, so
// a caller can show what is set and write the same object back. A generated
// config already carries one auto-claim target, so an empty list here means
// auto-claim is off rather than unconfigured.
StdLogosResult LogosBlockchainModule::read_pow_config(const std::string& config_path) {
    const fs::path config = localPathFromFileUrl(config_path);
    if (config.empty()) {
        return result::err("Config path was not specified.");
    }

    UserConfigReader cfg;
    std::string error;
    if (!cfg.load(config, error)) {
        return result::err(std::move(error));
    }

    nlohmann::json obj;
    const std::string max_threads = cfg.scalarAt("/pow/mining/max_threads");
    if (max_threads.empty() || max_threads == "null") {
        obj["max_threads"] = nullptr;
    } else {
        obj["max_threads"] = max_threads;
    }
    obj["max_tickets_per_block"] = cfg.scalarAt("/pow/mining/max_tickets_per_block");
    obj["tick_seconds"] = cfg.scalarAt("/pow/auto_claim/tick/value");

    nlohmann::json targets = nlohmann::json::array();
    for (size_t i = 0;; ++i) {
        const std::string base = "/pow/auto_claim/targets/" + std::to_string(i);
        const std::string public_key = cfg.scalarAt((base + "/public_key").c_str());
        if (public_key.empty())
            break;
        targets.push_back(nlohmann::json{
            {"public_key", public_key},
            {"threshold", cfg.scalarAt((base + "/threshold").c_str())},
        });
    }
    obj["auto_claim_targets"] = std::move(targets);
    return result::ok(obj.dump());
}

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
