#include "mock_chain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>

#include <libfyaml.h>

namespace fs = std::filesystem;

#define LBLOG(...)                                                                                                     \
    do {                                                                                                               \
        std::fprintf(stderr, "[lb-mock] ");                                                                            \
        std::fprintf(stderr, __VA_ARGS__);                                                                             \
        std::fprintf(stderr, "\n");                                                                                    \
    } while (0)

namespace lbmock {

namespace {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}


// ---- deterministic randomness -------------------------------------------

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t key(uint64_t seed, uint64_t tag, uint64_t a, uint64_t b = 0) {
    return mix(seed ^ mix(tag ^ mix(a ^ mix(b))));
}

double unit(uint64_t seed, uint64_t tag, uint64_t a, uint64_t b = 0) {
    return static_cast<double>(key(seed, tag, a, b) >> 11) * 0x1.0p-53;
}

void fill(uint8_t* out, size_t len, uint64_t s) {
    for (size_t i = 0; i < len; i += 8) {
        s = mix(s);
        std::memcpy(out + i, &s, std::min<size_t>(8, len - i));
    }
}

Bytes32 bytes(uint64_t seed, uint64_t tag, uint64_t a, uint64_t b = 0) {
    Bytes32 out{};
    fill(out.data(), out.size(), key(seed, tag, a, b));
    return out;
}

// A field element: the last (most significant, LE) byte stays below the BN254
// modulus's 0x30, so the value is canonical.
Bytes32 fr(uint64_t seed, uint64_t tag, uint64_t a, uint64_t b = 0) {
    Bytes32 out = bytes(seed, tag, a, b);
    out[31] &= 0x1f;
    return out;
}

uint64_t fold(const Bytes32& b) {
    uint64_t h = 0;
    for (size_t i = 0; i < b.size(); i += 8) {
        uint64_t w;
        std::memcpy(&w, b.data() + i, 8);
        h = mix(h ^ w);
    }
    return h;
}

std::string hexOf(uint64_t seed, size_t len) {
    std::vector<uint8_t> buf(len);
    fill(buf.data(), len, seed);
    return hex(buf.data(), len);
}

json intArray(const Bytes32& b) {
    json a = json::array();
    for (uint8_t v : b)
        a.push_back(v);
    return a;
}

json zkSig(uint64_t s) {
    return {{"ZkSig", {{"pi_a", hexOf(s + 1, 32)}, {"pi_b", hexOf(s + 2, 64)}, {"pi_c", hexOf(s + 3, 32)}}}};
}

// ---- libp2p PeerId of an ed25519 public key ------------------------------

std::string base58(const std::vector<uint8_t>& in) {
    static const char* alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<uint8_t> digits;
    for (uint8_t byte : in) {
        int carry = byte;
        for (auto& d : digits) {
            carry += d << 8;
            d = static_cast<uint8_t>(carry % 58);
            carry /= 58;
        }
        while (carry) {
            digits.push_back(static_cast<uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out;
    for (size_t i = 0; i < in.size() && in[i] == 0; ++i)
        out.push_back('1');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        out.push_back(alphabet[*it]);
    return out;
}

std::string peerId(const Bytes32& ed25519Pk) {
    std::vector<uint8_t> mh = {0x00, 0x24, 0x08, 0x01, 0x12, 0x20};
    mh.insert(mh.end(), ed25519Pk.begin(), ed25519Pk.end());
    return base58(mh);
}

// ---- YAML ----------------------------------------------------------------

class Yaml {
public:
    ~Yaml() {
        if (m_doc)
            fy_document_destroy(m_doc);
    }
    bool load(const std::string& path) {
        m_doc = fy_document_build_from_file(nullptr, path.c_str());
        return m_doc != nullptr;
    }
    fy_node* at(const char* path, fy_node* from = nullptr) const {
        if (!m_doc)
            return nullptr;
        return fy_node_by_path(from ? from : fy_document_root(m_doc), path, FY_NT, FYNWF_DONT_FOLLOW);
    }
    static std::string scalar(fy_node* n) {
        if (!n || !fy_node_is_scalar(n))
            return {};
        const char* v = fy_node_get_scalar0(n);
        return v ? v : "";
    }
    std::string str(const char* path, fy_node* from = nullptr) const { return scalar(at(path, from)); }
    std::optional<double> num(const char* path, fy_node* from = nullptr) const {
        const std::string s = str(path, from);
        if (s.empty() || s == "null")
            return std::nullopt;
        try {
            return std::stod(s);
        } catch (...) {
            return std::nullopt;
        }
    }
    std::optional<uint64_t> u64(const char* path, fy_node* from = nullptr) const {
        const std::string s = str(path, from);
        if (s.empty() || s == "null")
            return std::nullopt;
        try {
            return std::stoull(s);
        } catch (...) {
            return std::nullopt;
        }
    }
    std::vector<fy_node*> items(const char* path, fy_node* from = nullptr) const {
        std::vector<fy_node*> out;
        fy_node* n = at(path, from);
        if (!n || !fy_node_is_sequence(n))
            return out;
        void* it = nullptr;
        while (fy_node* item = fy_node_sequence_iterate(n, &it))
            out.push_back(item);
        return out;
    }
    std::vector<std::string> keys(const char* path) const {
        std::vector<std::string> out;
        fy_node* n = at(path);
        if (!n || !fy_node_is_mapping(n))
            return out;
        void* it = nullptr;
        while (fy_node_pair* p = fy_node_mapping_iterate(n, &it))
            out.push_back(scalar(fy_node_pair_key(p)));
        return out;
    }

private:
    fy_document* m_doc = nullptr;
};

// Serde Duration from the config: either '3600.000000000' or {secs, nanos}.
std::optional<double> duration(const Yaml& y, const char* path) {
    if (auto v = y.num(path))
        return v;
    const std::string p(path);
    auto secs = y.num((p + "/secs").c_str());
    if (!secs)
        return std::nullopt;
    return *secs + y.num((p + "/nanos").c_str()).value_or(0) / 1e9;
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

double envNum(const char* name, double fallback) {
    if (const char* v = env(name)) {
        try {
            return std::stod(v);
        } catch (...) {
        }
    }
    return fallback;
}

bool envFlag(const char* name, bool fallback) {
    const char* v = env(name);
    if (!v)
        return fallback;
    return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0;
}

// Devnet's genesis Blend providers (deployment/settings.yaml).
const char* kDevnetProviders[][2] = {
    {"5818cc65db81aeedc499082d5d2320d0174a3b7b04191191153da535ca753cdf",
     "daebceddc9ef45acd72a7ca35f3d2d1c74e207eda6e274a4ca360159de8e0210"},
    {"188245c021abfe362c07d3aa887d727dc45828d5d7d94de7843ed56c5c738aee",
     "dc9803310f8ea8571723c6e3e568ff0d6bb81eccd28777ed99fc1a4f68cf0400"},
    {"7eef1970220c1ebbb7ecfde7a1402a1c1dd56f887c19b54f074f42414a14080f",
     "8a9f02e0c130a6ee1a86ec4bd76ceef2e2c37e733f3efc4e256eec96df6d352a"},
    {"015e73fefc8b54813f28636293edd83cd9b3384830e7dd483e58502ab399c00e",
     "8362830159cc5f0967936720a0f5c4990339c6c4b7b5f53efac848e2a66e2007"},
};

// The first genesis op is an inscription carrying [len][chain_id][genesis secs u32 LE]...
void readGenesis(const Yaml& y, Params& p) {
    for (fy_node* tx : y.items("/cryptarchia/genesis_block/transactions")) {
        for (fy_node* op : y.items("/mantle_tx/ops", tx)) {
            const std::string inscription = y.str("/payload/inscription", op);
            if (!inscription.empty() && p.genesisProviders.empty()) {
                std::vector<uint8_t> b;
                for (size_t i = 0; i + 1 < inscription.size(); i += 2)
                    b.push_back(static_cast<uint8_t>(std::stoul(inscription.substr(i, 2), nullptr, 16)));
                if (!b.empty() && b.size() >= 1u + b[0] + 4) {
                    p.chainId.assign(b.begin() + 1, b.begin() + 1 + b[0]);
                    uint32_t secs = 0;
                    std::memcpy(&secs, b.data() + 1 + b[0], 4);
                    p.genesisMs = static_cast<int64_t>(secs) * 1000;
                }
            }
            if (y.str("/payload/service_type", op) == "BN") {
                Bytes32 provider{}, zk{};
                if (parseHex32(y.str("/payload/provider_id", op), provider) &&
                    parseHex32(y.str("/payload/zk_id", op), zk))
                    p.genesisProviders.emplace_back(provider, zk);
            }
        }
    }
}

Params loadParams(const char* deploymentPath) {
    Params p;
    if (deploymentPath && *deploymentPath) {
        Yaml y;
        if (y.load(deploymentPath)) {
            p.slotMs = static_cast<uint64_t>(duration(y, "/time/slot_duration").value_or(1) * 1000);
            p.securityParam = y.u64("/cryptarchia/security_param").value_or(p.securityParam);
            const double num = y.num("/cryptarchia/slot_activation_coeff/numerator").value_or(1);
            const double den = y.num("/cryptarchia/slot_activation_coeff/denominator").value_or(30);
            p.activeSlotCoeff = num / den;
            const uint64_t periods = y.u64("/cryptarchia/epoch_config/epoch_stake_distribution_stabilization").value_or(3) +
                                     y.u64("/cryptarchia/epoch_config/epoch_period_nonce_buffer").value_or(3) +
                                     y.u64("/cryptarchia/epoch_config/epoch_period_nonce_stabilization").value_or(4);
            p.epochSlots = periods * static_cast<uint64_t>(std::floor(p.securityParam / p.activeSlotCoeff));
            p.minStake = y.u64("/cryptarchia/sdp_config/min_stake/threshold").value_or(p.minStake);
            p.inactivityPeriod = static_cast<uint32_t>(
                y.u64("/cryptarchia/sdp_config/service_params/BN/inactivity_period").value_or(p.inactivityPeriod));
            p.minimumNetworkSize =
                static_cast<uint32_t>(y.u64("/blend/common/minimum_network_size").value_or(p.minimumNetworkSize));
            p.powEpochReward = y.u64("/cryptarchia/pow_config/reward/epoch_reward_genesis").value_or(p.powEpochReward);
            p.powSlotWindow = y.u64("/cryptarchia/pow_config/reward/slot_window").value_or(p.powSlotWindow);
            readGenesis(y, p);
        } else {
            LBLOG("could not parse deployment %s; using devnet values", deploymentPath);
        }
    }
    if (p.genesisProviders.empty()) {
        for (auto& pair : kDevnetProviders) {
            Bytes32 provider{}, zk{};
            parseHex32(pair[0], provider);
            parseHex32(pair[1], zk);
            p.genesisProviders.emplace_back(provider, zk);
        }
    }

    if (const char* profile = env("LB_MOCK_PROFILE"); profile && std::strcmp(profile, "fast") == 0) {
        // Short epochs so a Blend declaration activates in minutes, not a day.
        p.securityParam = 3;
        p.activeSlotCoeff = 1.0 / 5;
        p.epochSlots = 10 * static_cast<uint64_t>(std::floor(p.securityParam / p.activeSlotCoeff));
        p.pbpSecondsOverride = 10;
        p.genesisMs = 0; // set from the state file (first start minus a minute)
    }
    p.seed = static_cast<uint64_t>(envNum("LB_MOCK_SEED", static_cast<double>(p.seed)));
    p.ibdBlocksPerSecond = envNum("LB_MOCK_IBD_BLOCKS_PER_SECOND", p.ibdBlocksPerSecond);
    p.powTicketsPerBlock = envNum("LB_MOCK_POW_TICKETS_PER_BLOCK", p.powTicketsPerBlock);
    p.blendActivityHitRate = envNum("LB_MOCK_BLEND_HIT_RATE", p.blendActivityHitRate);
    p.networkStake = static_cast<uint64_t>(envNum("LB_MOCK_NETWORK_STAKE", static_cast<double>(p.networkStake)));
    p.backgroundTxPerBlock = envNum("LB_MOCK_BACKGROUND_TX_PER_BLOCK", p.backgroundTxPerBlock);
    p.blendReachable = envFlag("LB_MOCK_BLEND_REACHABLE", p.blendReachable);
    p.assumePeers = envFlag("LB_MOCK_ASSUME_PEERS", p.assumePeers);
    p.panicOnReentry = envFlag("LB_MOCK_PANIC_ON_REENTRY", p.panicOnReentry);
    if (env("LB_MOCK_PBP_SECONDS"))
        p.pbpSecondsOverride = envNum("LB_MOCK_PBP_SECONDS", 0);
    return p;
}

bool loadConfig(const char* path, NodeConfig& c, std::string& error) {
    Yaml y;
    if (!path || !y.load(path)) {
        error = std::string("Failed to load user config ") + (path ? path : "<null>");
        return false;
    }
    for (const auto& k : y.keys("/wallet/known_keys")) {
        Bytes32 b{};
        if (parseHex32(k, b))
            c.knownKeys.push_back(b);
    }
    parseHex32(y.str("/cryptarchia/leader/wallet/funding_pk"), c.leaderFundingPk);
    c.leaderMaxTxFee = y.u64("/cryptarchia/leader/wallet/max_tx_fee").value_or(UINT64_MAX);
    parseHex32(y.str("/sdp/wallet/funding_pk"), c.sdpFundingPk);
    c.sdpMaxTxFee = y.u64("/sdp/wallet/max_tx_fee").value_or(UINT64_MAX);
    if (Bytes32 d{}; parseHex32(y.str("/sdp/declaration_id"), d))
        c.sdpDeclarationId = d;
    parseHex32(y.str("/blend/non_ephemeral_signing_key_id"), c.blendSigningKey);
    parseHex32(y.str("/blend/core/zk/secret_key_kms_id"), c.blendZkKey);
    c.blendListeningAddress = y.str("/blend/core/backend/listening_address");
    c.initialPeers = y.items("/network/backend/initial_peers").size();
    c.ibdPeers = y.items("/cryptarchia/network/bootstrap/ibd/peers").size();
    c.pbpSeconds = duration(y, "/cryptarchia/service/bootstrap/prolonged_bootstrap_period").value_or(3600);
    c.forceBootstrap = y.str("/cryptarchia/service/bootstrap/force_bootstrap") == "true";
    c.offlineGraceSeconds =
        duration(y, "/cryptarchia/service/bootstrap/offline_grace_period/grace_period").value_or(1200);
    c.maxTicketsPerBlock = static_cast<uint32_t>(y.u64("/pow/mining/max_tickets_per_block").value_or(4));
    for (fy_node* t : y.items("/pow/auto_claim/targets")) {
        NodeConfig::Target target{};
        if (parseHex32(y.str("/public_key", t), target.pk)) {
            target.threshold = y.u64("/threshold", t).value_or(UINT64_MAX);
            c.powTargets.push_back(target);
        }
    }
    c.powTick = y.u64("/pow/auto_claim/tick/value").value_or(300);
    c.powTickInSlots = y.str("/pow/auto_claim/tick/unit") == "slots";
    c.pendingNoteExpiryBlocks = y.u64("/wallet/pending_note_expiry_blocks").value_or(10);
    c.stateFolder = y.str("/state/base_folder");
    if (c.stateFolder.empty())
        c.stateFolder = (fs::path(path).parent_path() / "state").string();
    return true;
}

// ---- gas -----------------------------------------------------------------

uint64_t execGas(const json& op) {
    switch (op.value("opcode", -1)) {
    case 0:
    case 18:
    case 33:
    case 34:
        return 590;
    case 32:
        return 646;
    case 48:
        return 580;
    case 64:
        return 1;
    case 16:
        return 56 * op["payload"].value("configuration_threshold", 1);
    default:
        return 56;
    }
}

uint64_t opBytes(const json& op) {
    const json& p = op["payload"];
    switch (op.value("opcode", -1)) {
    case 0:
        return 8 + 32 * p.value("inputs", json::array()).size() + 40 * p.value("outputs", json::array()).size();
    case 18:
        return 40 + 32 * p.value("inputs", json::array()).size() + p.value("metadata", json::array()).size();
    case 17:
        return 100 + p.value("inscription", std::string()).size() / 2;
    case 32:
        return 140 + 64 * p.value("locators", json::array()).size();
    case 34:
        return 400;
    default:
        return 96;
    }
}

uint64_t proofBytes(const json& proof) {
    if (proof.contains("ZkSig") || proof.contains("PoC"))
        return 128;
    if (proof.contains("Ed25519Sig"))
        return 64;
    if (proof.contains("ZkAndEd25519Sigs"))
        return 192;
    if (proof.contains("ChannelMultiSigProof"))
        return 66 * std::max<size_t>(1, proof["ChannelMultiSigProof"].value("signatures", json::array()).size());
    return 1;
}

json transferOp(const std::vector<Note>& inputs, const std::vector<std::pair<uint64_t, Bytes32>>& outputs) {
    json in = json::array(), out = json::array();
    for (const auto& n : inputs)
        in.push_back(hex(n.id));
    for (const auto& [value, pk] : outputs)
        out.push_back({{"value", value}, {"pk", hex(pk)}});
    return {{"opcode", 0}, {"payload", {{"inputs", in}, {"outputs", out}}}};
}

bool validFr(const Bytes32& b) { return b[31] <= 0x30; }

thread_local bool t_inCallback = false;

} // namespace

bool inStreamCallback() { return t_inCallback; }

std::string hex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0xf];
    }
    return out;
}

bool parseHex32(const std::string& in, Bytes32& out) {
    std::string s = in;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    if (s.size() != 64)
        return false;
    for (size_t i = 0; i < 32; ++i) {
        char* end = nullptr;
        const std::string byte = s.substr(2 * i, 2);
        const unsigned long v = std::strtoul(byte.c_str(), &end, 16);
        if (*end != '\0')
            return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

Bytes32 fromPtr(const uint8_t* p) {
    Bytes32 b{};
    std::memcpy(b.data(), p, 32);
    return b;
}

// ==== lifecycle ==============================================================

std::unique_ptr<Node> Node::start(const char* configPath, const char* deploymentPath, Error& err) {
    std::unique_ptr<Node> node(new Node());
    std::string error;
    if (!loadConfig(configPath, node->m_cfg, error)) {
        err = {kInitializationError, error};
        return nullptr;
    }
    node->m_params = loadParams(deploymentPath);
    node->m_statePath = (fs::path(node->m_cfg.stateFolder) / "mock_node_state.json").string();
    node->m_isolated = !node->m_params.assumePeers && node->m_cfg.initialPeers == 0 && node->m_cfg.ibdPeers == 0;
    node->m_startedMs = nowMs();
    node->m_autoClaimArmed = !node->m_cfg.powTargets.empty();
    node->m_lastAutoClaimMs = node->m_startedMs;
    node->loadState();
    if (node->m_isolated)
        LBLOG("no initial_peers or IBD peers in the config: the node is isolated and sees no network blocks, as a "
             "real node would. Set LB_MOCK_ASSUME_PEERS=1 to pretend it has peers.");
    LBLOG("started: chain %s, epoch %llu slots, %s, state %s", node->m_params.chainId.c_str(),
         static_cast<unsigned long long>(node->m_params.epochSlots),
         node->m_mode == Mode::Online ? "Online (within the offline grace period)" : "Bootstrapping",
         node->m_statePath.c_str());
    node->m_thread = std::thread([n = node.get()] { n->run(); });
    return node;
}

Node::~Node() { shutdown(); }

void Node::shutdown() {
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_stopping = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard lock(m_mutex);
    saveState();
    LBLOG("stopped");
}

Error Node::subscribe(Stream stream, Callback cb) {
    std::lock_guard lock(m_mutex);
    if (m_failed)
        return {kRelayError, "Failed to subscribe: the node has shut down."};
    m_streams[static_cast<int>(stream)] = cb;
    return {};
}

void Node::run() {
    while (true) {
        std::vector<Emitted> out;
        {
            std::unique_lock lock(m_mutex);
            if (m_stopping)
                break;
            tick(nowMs(), out);
        }
        dispatch(out);
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_stopping; });
    }
}

void Node::dispatch(std::vector<Emitted>& out) {
    for (auto& e : out) {
        t_inCallback = true;
        e.cb(e.end ? nullptr : e.payload.c_str());
        t_inCallback = false;
    }
}

void Node::endStreams(std::vector<Emitted>& out) {
    for (auto& cb : m_streams) {
        if (cb)
            out.push_back({cb, {}, true});
        cb = nullptr;
    }
}

// ==== chain ==================================================================

uint64_t Node::clockSlot(int64_t ms) const {
    if (ms < m_params.genesisMs)
        return 0;
    return static_cast<uint64_t>(ms - m_params.genesisMs) / m_params.slotMs;
}

bool Node::networkHasBlock(uint64_t slot) const {
    return slot == 0 || unit(m_params.seed, 1, slot) < m_params.activeSlotCoeff;
}

Bytes32 Node::blockId(uint64_t slot) const { return bytes(m_params.seed, 2, slot); }

size_t Node::indexOfSlot(uint64_t slot) const {
    auto it = std::lower_bound(m_chain.begin(), m_chain.end(), slot);
    return static_cast<size_t>(it - m_chain.begin());
}

void Node::extendNetworkChain(uint64_t toSlot) {
    const bool synced = m_tip + 1 >= m_chain.size();
    const bool canLead = m_mode == Mode::Online && synced;
    double share = 0;
    if (canLead) {
        Error ignored;
        uint64_t aged = 0;
        for (const auto& n : agedNotesLocked(nullptr, ignored).notes)
            aged += n.value;
        share = std::min(1.0, static_cast<double>(aged) / static_cast<double>(m_params.networkStake));
    }
    const double winP = share > 0 ? 1 - std::pow(1 - m_params.activeSlotCoeff, share) : 0;
    for (uint64_t s = m_networkScanned + 1; s <= toSlot; ++s) {
        const bool network = !m_isolated && networkHasBlock(s);
        const bool ours = winP > 0 && std::uniform_real_distribution<double>(0, 1)(m_rng) < winP;
        if (!network && !ours)
            continue;
        m_chain.push_back(s);
        m_idToSlot[hex(blockId(s))] = s;
        if (ours) {
            m_ourSlots.insert(s);
            m_vouchers.push_back({fr(m_params.seed, 7, s), fr(m_params.seed, 10, s), epochOf(s), false});
            LBLOG("won the leader lottery at slot %llu (voucher claimable from epoch %u)",
                 static_cast<unsigned long long>(s), epochOf(s) + 1);
        }
    }
    m_networkScanned = std::max(m_networkScanned, toSlot);
}

json Node::headerJson(size_t index, bool withId) const {
    const uint64_t slot = m_chain[index];
    const bool ours = m_ourSlots.count(slot) > 0;
    json h = json::object();
    if (withId)
        h["id"] = hex(blockId(slot));
    else
        h["version"] = "Bedrock";
    h["parent_block"] = index == 0 ? std::string(64, '0') : hex(blockId(m_chain[index - 1]));
    h["slot"] = slot;
    h["body_root"] = hex(bytes(m_params.seed, 3, slot));
    h["proof_of_leadership"] = {
        {"proof", hexOf(key(m_params.seed, 4, slot), 128)},
        {"entropy_contribution", hex(fr(m_params.seed, 5, slot))},
        {"leader_key", hex(ours ? fr(m_params.seed, 6, slot, 1) : fr(m_params.seed, 6, slot % 97))},
        {"voucher_cm", hex(fr(m_params.seed, 7, slot))},
    };
    return h;
}

json Node::backgroundTxs(uint64_t slot) const {
    json txs = json::array();
    if (slot == 0)
        return txs;
    const double rate = m_params.backgroundTxPerBlock;
    const int n = static_cast<int>(std::floor(rate)) + (unit(m_params.seed, 8, slot) < rate - std::floor(rate) ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        const uint64_t s = key(m_params.seed, 9, slot, i);
        const int inputs = 1 + static_cast<int>(s % 2);
        json in = json::array(), out = json::array();
        for (int j = 0; j < inputs; ++j)
            in.push_back(hex(fr(s, 1, j)));
        const uint64_t value = 1000 + (mix(s) % 5000000);
        out.push_back({{"value", value}, {"pk", hex(fr(s, 2, 0))}});
        out.push_back({{"value", mix(s + 1) % 90000000}, {"pk", hex(fr(s, 2, 1))}});
        json ops = json::array({{{"opcode", 0}, {"payload", {{"inputs", in}, {"outputs", out}}}}});
        txs.push_back({{"hash", hex(bytes(s, 3, 0))}, {"ops", ops}, {"proofs", json::array({zkSig(s)})}});
    }
    return txs;
}

json Node::txsAt(uint64_t slot) const {
    json txs = backgroundTxs(slot);
    if (auto it = m_includedTxs.find(slot); it != m_includedTxs.end())
        for (const auto& t : it->second)
            txs.push_back(t);
    return txs;
}

json Node::blockCore(size_t index, bool withTxId) const {
    json txs = json::array();
    for (const auto& t : txsAt(m_chain[index])) {
        json tx = json::object();
        if (withTxId)
            tx["id"] = t["hash"];
        tx["mantle_tx"] = {{"ops", t["ops"]}};
        tx["ops_proofs"] = t["proofs"];
        txs.push_back(tx);
    }
    return {{"header", headerJson(index, false)},
            {"signature", hexOf(key(m_params.seed, 11, m_chain[index]), 64)},
            {"uncle_headers", json::array()},
            {"transactions", txs}};
}

json Node::blockProcessed(size_t index) const {
    json txs = json::array();
    for (const auto& t : txsAt(m_chain[index]))
        txs.push_back({{"mantle_tx", {{"hash", t["hash"]}, {"ops", t["ops"]}}}, {"ops_proofs", t["proofs"]}});
    return {{"block", {{"header", headerJson(index, true)}, {"uncle_headers", json::array()}, {"transactions", txs}}},
            {"tip", hex(blockId(m_chain[index]))},
            {"tip_slot", m_chain[index]},
            {"lib", hex(blockId(m_chain[m_lib]))},
            {"lib_slot", m_chain[m_lib]}};
}

uint64_t Node::fee(const json& ops, const json& proofs) const {
    uint64_t exec = 0, size = 64;
    for (const auto& op : ops) {
        exec += execGas(op);
        size += opBytes(op);
    }
    for (const auto& p : proofs)
        size += proofBytes(p);
    return exec + size;
}

uint64_t Node::blockFees(uint64_t slot) const {
    uint64_t total = 0;
    for (const auto& t : txsAt(slot))
        total += fee(t["ops"], t["proofs"]);
    return total;
}

// Blend's 60% of the epoch's block rewards (fees; devnet's inflation term is 0).
uint64_t Node::epochBlendIncome(uint32_t epoch) const {
    const uint64_t from = static_cast<uint64_t>(epoch) * m_params.epochSlots;
    const uint64_t to = from + m_params.epochSlots;
    uint64_t total = 0;
    for (size_t i = indexOfSlot(from); i < m_chain.size() && m_chain[i] < to; ++i)
        total += blockFees(m_chain[i]);
    return total * 6 / 10;
}

// ==== the clock ==============================================================

void Node::tick(int64_t now, std::vector<Emitted>& out) {
    if (m_failed)
        return;
    if (m_lastTickMs == 0) {
        for (const auto& t : m_cfg.powTargets) {
            if (!isKnown(t.pk)) {
                LBLOG("PoW auto-claim target %s is not in wallet.known_keys: the PoW service fails to start and "
                     "takes the node down with it",
                     hex(t.pk).c_str());
                m_failed = true;
                endStreams(out);
                return;
            }
        }
    }
    const double dt = m_lastTickMs ? static_cast<double>(now - m_lastTickMs) / 1000.0 : 0.1;
    m_lastTickMs = now;

    if (now < m_params.genesisMs) {
        m_mode = Mode::NotStarted;
        return;
    }
    if (m_mode == Mode::NotStarted)
        m_mode = Mode::Bootstrapping;

    const uint64_t slot = clockSlot(now);
    extendNetworkChain(slot);

    // Download / follow.
    const size_t head = m_chain.size() - 1;
    if (m_tip < head) {
        size_t budget = head - m_tip;
        if (budget > 2) {
            m_syncCredit = std::min(m_syncCredit + m_params.ibdBlocksPerSecond * dt, 1e6);
            budget = std::min(budget, static_cast<size_t>(m_syncCredit));
            m_syncCredit -= static_cast<double>(budget);
        }
        for (size_t i = 0; i < budget; ++i)
            processBlock(m_tip + 1, out);
    }

    if (m_mode == Mode::Bootstrapping) {
        if (!m_ibdDone && (m_tip == m_chain.size() - 1 || m_cfg.ibdPeers == 0)) {
            m_ibdDone = true;
            m_pbpStartMs = now;
            LBLOG("initial block download complete at height %zu; prolonged bootstrap period %.0fs", m_tip,
                 m_params.pbpSecondsOverride.value_or(m_cfg.pbpSeconds));
        }
        const double pbp = m_params.pbpSecondsOverride.value_or(m_cfg.pbpSeconds);
        if (m_ibdDone && static_cast<double>(now - m_pbpStartMs) >= pbp * 1000) {
            m_mode = Mode::Online;
            LBLOG("Online at height %zu", m_tip);
            setLib(m_tip > m_params.securityParam ? m_tip - m_params.securityParam : 0, out);
            m_cv.notify_all();
        }
    }
    if (m_mode == Mode::Online)
        m_onlineEpochs.insert(epochOf(slot));

    autoClaimTick(now);

    if (now - m_lastSaveMs > 5000) {
        saveState();
        m_lastSaveMs = now;
    }
}

void Node::setLib(size_t index, std::vector<Emitted>& out) {
    if (index == m_lib)
        return;
    m_lib = index;
    if (Callback cb = m_streams[static_cast<int>(Stream::LibBlocks)])
        out.push_back({cb, json{{"height", index}, {"header_id", hex(blockId(m_chain[index]))}}.dump()});
}

void Node::processBlock(size_t index, std::vector<Emitted>& out) {
    const uint64_t slot = m_chain[index];
    const uint64_t prevSlot = index > 0 ? m_chain[index - 1] : 0;
    json events = json::array();
    for (uint32_t e = epochOf(prevSlot) + 1; index > 0 && e <= epochOf(slot); ++e)
        onEpochStart(e, slot, events);

    // Mempool: a tx is in the first block after it was submitted. An isolated
    // node's txs never leave it, so only its own blocks can carry them.
    const bool ours = m_ourSlots.count(slot) > 0;
    std::vector<PendingTx> keep;
    for (auto& tx : m_mempool) {
        if (tx.submitSlot >= slot || (m_isolated && !ours)) {
            keep.push_back(std::move(tx));
            continue;
        }
        std::string why;
        if (applyTx(tx, slot, events, why)) {
            LBLOG("tx %s included at slot %llu", hex(tx.hash).c_str(), static_cast<unsigned long long>(slot));
        } else {
            LBLOG("tx %s rejected by the ledger at slot %llu: %s (dropped; the caller is not told)",
                 hex(tx.hash).c_str(), static_cast<unsigned long long>(slot), why.c_str());
            for (const auto& id : tx.reserves)
                m_reserved[id] = index + m_cfg.pendingNoteExpiryBlocks;
            for (const auto& op : tx.ops)
                if (op.value("opcode", -1) == 64)
                    for (const auto& t : m_tickets)
                        if (hex(t.pk) == op["payload"].value("public_key", std::string()))
                            m_ticketsInFlight.erase(t.nullifier);
        }
        if (auto it = m_txIndex.find(hex(tx.hash)); it != m_txIndex.end())
            it->second.second = nowMs();
    }
    m_mempool = std::move(keep);
    if (!events.empty())
        m_events[slot] = events;
    m_tip = index;

    for (auto it = m_reserved.begin(); it != m_reserved.end();)
        it = (it->second != 0 && it->second <= index) ? m_reserved.erase(it) : std::next(it);

    const bool atHead = index + 1 >= m_chain.size();
    if (m_mode == Mode::Online && atHead) {
        const int64_t now = nowMs();
        for (const auto& t : backgroundTxs(slot))
            m_txIndex[t["hash"].get<std::string>()] = {
                json{{"mantle_tx", {{"ops", t["ops"]}}}, {"ops_proofs", t["proofs"]}}.dump(), now};
        for (auto it = m_txIndex.begin(); it != m_txIndex.end();)
            it = (it->second.second != 0 && now - it->second.second > 600000) ? m_txIndex.erase(it) : std::next(it);

        if (m_mining) {
            std::poisson_distribution<int> found(m_params.powTicketsPerBlock);
            const int n = std::min<int>(found(m_rng), static_cast<int>(m_cfg.maxTicketsPerBlock));
            for (int i = 0; i < n; ++i) {
                const uint64_t s = m_rng();
                m_tickets.push_back({slot, blockId(slot), fr(s, 1, 0), fr(s, 2, 0)});
            }
        }
    }
    m_tickets.erase(std::remove_if(m_tickets.begin(), m_tickets.end(),
                                   [&](const Ticket& t) {
                                       return !m_ticketsInFlight.count(t.nullifier) &&
                                              slot > t.blockSlot + m_params.powSlotWindow;
                                   }),
                    m_tickets.end());

    if (Callback cb = m_streams[static_cast<int>(Stream::NewBlocks)])
        out.push_back({cb, blockCore(index, true).dump()});
    if (Callback cb = m_streams[static_cast<int>(Stream::ProcessedBlocks)])
        out.push_back({cb, blockProcessed(index).dump()});
    if (m_mode == Mode::Online)
        setLib(index > m_params.securityParam ? index - m_params.securityParam : 0, out);
}

bool Node::memberOf(uint32_t epoch) const {
    if (!m_declaration)
        return false;
    return m_declaration->created + 2 <= epoch && m_declaration->active + m_params.inactivityPeriod >= epoch;
}

void Node::onEpochStart(uint32_t epoch, uint64_t slot, json& headerEvents) {
    // Activity for the epoch that just ended: a proof only exists if the node
    // served it as a core node, and the draw can still miss.
    if (m_declaration && epoch >= 1) {
        const uint32_t served = epoch - 1;
        if (memberOf(served)) {
            const bool online = m_onlineEpochs.count(served) > 0;
            const bool drawn = std::uniform_real_distribution<double>(0, 1)(m_rng) < m_params.blendActivityHitRate;
            if (!online)
                LBLOG("no Blend activity proof for epoch %u: the node was not online", served);
            else if (!m_params.blendReachable || m_isolated)
                LBLOG("no Blend activity proof for epoch %u: no Blend peers reached this node", served);
            else if (!drawn)
                LBLOG("no Blend activity proof for epoch %u: lost the draw", served);
            else {
                json op = {{"opcode", 34},
                           {"payload",
                            {{"declaration_id", hex(m_declaration->id)},
                             {"nonce", m_declaration->nonce + 1},
                             {"metadata",
                              {{"Blend",
                                {{"epoch", served},
                                 {"signing_key", hex(m_declaration->providerId)},
                                 {"proof_of_quota",
                                  {{"key_nullifier", hex(fr(m_rng(), 1, 0))},
                                   {"proof",
                                    {{"pi_a", intArray(bytes(m_rng(), 1, 0))},
                                     {"pi_b", json::array()},
                                     {"pi_c", intArray(bytes(m_rng(), 2, 0))}}}}},
                                 {"proof_of_selection", {{"selection_randomness", hex(fr(m_rng(), 3, 0))}}}}}}}}}};
                json ops = json::array({op});
                json proofs = json::array({zkSig(m_rng())});
                std::vector<Note> picked;
                uint64_t txFee = 0, available = 0;
                if (!selectFunding({m_cfg.sdpFundingPk}, 0, ops, proofs, picked, txFee, available) ||
                    txFee > m_cfg.sdpMaxTxFee) {
                    LBLOG("Blend activity for epoch %u not posted: the SDP funding key cannot pay the fee "
                         "(available=%llu)",
                         served, static_cast<unsigned long long>(available));
                } else {
                    uint64_t in = 0;
                    std::set<Bytes32> reserves;
                    for (auto& n : picked) {
                        in += n.value;
                        reserves.insert(n.id);
                    }
                    std::vector<std::pair<uint64_t, Bytes32>> outs;
                    if (in > txFee)
                        outs.push_back({in - txFee, m_cfg.sdpFundingPk});
                    ops.push_back(transferOp(picked, outs));
                    m_declaration->nonce += 1;
                    proofs.push_back(zkSig(m_rng()));
                    const Bytes32 h = enqueue(ops, proofs, reserves);
                    for (auto& tx : m_mempool)
                        if (tx.hash == h)
                            tx.submitSlot = slot;
                    LBLOG("posted Blend activity for epoch %u", served);
                }
            }
        }
    }

    // Rewards for epoch-2: split across the providers whose activity was
    // accepted, the closest proof (the "premium" one) counting double.
    if (epoch >= 2) {
        const uint32_t paid = epoch - 2;
        struct Recipient {
            Bytes32 zk;
            bool mine;
        };
        std::vector<Recipient> recipients;
        for (size_t i = 0; i < m_params.genesisProviders.size(); ++i)
            if (unit(m_params.seed, 12, paid, i) < m_params.blendActivityHitRate)
                recipients.push_back({m_params.genesisProviders[i].second, false});
        if (m_declaration && m_activityAccepted.count(paid))
            recipients.push_back({m_declaration->zkId, true});
        const size_t networkSize = m_params.genesisProviders.size() + (memberOf(paid) ? 1 : 0);
        const uint64_t income = epochBlendIncome(paid);
        if (!recipients.empty() && networkSize >= m_params.minimumNetworkSize && income > 0) {
            const size_t premium = key(m_params.seed, 13, paid) % recipients.size();
            const uint64_t share = income / (recipients.size() + 1);
            const Bytes32 opId = bytes(m_params.seed, 14, paid);
            for (size_t i = 0; i < recipients.size(); ++i) {
                const uint64_t value = share * (i == premium ? 2 : 1);
                headerEvents.push_back(
                    {{"Header",
                      {{"SdpRewardDistributed",
                        {{"service_type", "BN"},
                         {"utxo",
                          {{"op_id", intArray(opId)},
                           {"output_index", i},
                           {"note", {{"value", value}, {"pk", hex(recipients[i].zk)}}}}}}}}}});
                if (recipients[i].mine && isKnown(recipients[i].zk)) {
                    Note n{fr(fold(opId), 1, i), value, recipients[i].zk, slot};
                    m_notes[n.id] = n;
                    LBLOG("Blend reward for epoch %u: %llu to the BlendZk key", paid,
                         static_cast<unsigned long long>(value));
                }
            }
        }
    }
}

void Node::autoClaimTick(int64_t now) {
    if (!m_autoClaimArmed || m_mode != Mode::Online || m_chain.empty())
        return;
    const uint64_t slot = m_chain[m_tip];
    if (m_cfg.powTickInSlots ? slot - m_lastAutoClaimSlot < m_cfg.powTick
                             : now - m_lastAutoClaimMs < static_cast<int64_t>(m_cfg.powTick) * 1000)
        return;
    m_lastAutoClaimMs = now;
    m_lastAutoClaimSlot = slot;
    const NodeConfig::Target* best = nullptr;
    uint64_t bestBalance = 0;
    for (const auto& t : m_cfg.powTargets) {
        const uint64_t b = balanceOf(t.pk);
        if (b < t.threshold && (!best || b < bestBalance)) {
            best = &t;
            bestBalance = b;
        }
    }
    if (!best) {
        LBLOG("every PoW auto-claim target reached its threshold: auto-claim disarmed, mining stopped");
        m_autoClaimArmed = false;
        m_mining = false;
        return;
    }
    Error err;
    claimPow(best->pk, err);
}

// ==== ledger =================================================================

bool Node::isKnown(const Bytes32& pk) const {
    return std::find(m_cfg.knownKeys.begin(), m_cfg.knownKeys.end(), pk) != m_cfg.knownKeys.end();
}

uint64_t Node::balanceOf(const Bytes32& pk) const {
    uint64_t total = 0;
    for (const auto& [id, n] : m_notes)
        if (n.pk == pk && !n.channel)
            total += n.value;
    return total;
}

// The wallet's fund_tx: the funding keys' spendable notes, largest first,
// until they cover `needed` plus the fee of the tx with the funding transfer
// (inputs -> one change output) appended.
// The wallet's fund_tx: the funding keys' spendable notes, largest first,
// until they cover `needed` plus the fee of the whole tx: `ops` with their
// `proofs`, then the funding transfer (inputs -> `transferOutputs` outputs) and
// its ZK signature.
bool Node::selectFunding(const std::vector<Bytes32>& funding, uint64_t needed, const json& ops, const json& proofs,
                         std::vector<Note>& picked, uint64_t& feeOut, uint64_t& available,
                         const std::set<Bytes32>& exclude, size_t transferOutputs) const {
    std::vector<Note> candidates;
    for (const auto& [id, n] : m_notes) {
        if (n.service || n.channel || m_reserved.count(id) || exclude.count(id))
            continue;
        if (std::find(funding.begin(), funding.end(), n.pk) != funding.end())
            candidates.push_back(n);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Note& a, const Note& b) { return a.value > b.value; });
    available = 0;
    for (const auto& c : candidates)
        available += c.value;
    const std::vector<std::pair<uint64_t, Bytes32>> outputs(transferOutputs, {0, Bytes32{}});
    json allProofs = proofs;
    allProofs.push_back(zkSig(0));
    picked.clear();
    uint64_t in = 0;
    for (const auto& c : candidates) {
        picked.push_back(c);
        in += c.value;
        json all = ops;
        all.push_back(transferOp(picked, outputs));
        feeOut = fee(all, allProofs);
        if (in >= needed + feeOut)
            return true;
    }
    return false;
}

Bytes32 Node::txHash(const json& ops) {
    std::hash<std::string> h;
    return bytes(m_params.seed, 15, h(ops.dump()), m_rng());
}

Bytes32 Node::enqueue(json ops, json proofs, std::set<Bytes32> reserves, bool viaBlend) {
    PendingTx tx;
    tx.hash = txHash(ops);
    tx.ops = std::move(ops);
    tx.proofs = std::move(proofs);
    tx.submitSlot = clockSlot(nowMs());
    tx.reserves = std::move(reserves);
    tx.viaBlend = viaBlend;
    for (const auto& id : tx.reserves)
        m_reserved[id] = 0;
    m_txIndex[hex(tx.hash)] = {json{{"mantle_tx", {{"ops", tx.ops}}}, {"ops_proofs", tx.proofs}}.dump(), 0};
    const Bytes32 hash = tx.hash;
    m_mempool.push_back(std::move(tx));
    return hash;
}

namespace {
Bytes32 outputNoteId(const Bytes32& txHash, size_t op, size_t output) { return fr(fold(txHash), 16, op, output); }
Bytes32 powNoteId(const std::string& pk, const json& blockHash) {
    return fr(std::hash<std::string>{}(pk + blockHash.dump()), 17, 0);
}
} // namespace

bool Node::applyTx(const PendingTx& tx, uint64_t slot, json& events, std::string& why) {
    try {
        return applyTxUnchecked(tx, slot, events, why);
    } catch (const std::exception& e) {
        why = std::string("malformed transaction: ") + e.what();
        return false;
    }
}

bool Node::applyTxUnchecked(const PendingTx& tx, uint64_t slot, json& events, std::string& why) {
    std::map<Bytes32, Note> created; // this tx's outputs, any owner
    std::set<Bytes32> spent;
    std::vector<Note> channelNotes;
    std::optional<Declaration> declared;
    std::optional<std::pair<uint64_t, uint32_t>> activity; // nonce, epoch
    std::vector<Bytes32> claimedVouchers, retiredTickets;
    std::map<std::string, json> channels = m_channels;
    json txEvents = json::array();
    __int128 surplus = 0;

    auto take = [&](const std::string& idHex, Note& out, bool allowService) -> bool {
        Bytes32 id{};
        if (!parseHex32(idHex, id) || spent.count(id))
            return false;
        if (auto it = created.find(id); it != created.end()) {
            out = it->second;
            created.erase(it);
            spent.insert(id);
            return true;
        }
        auto it = m_notes.find(id);
        if (it == m_notes.end() || it->second.channel || (it->second.service && !allowService))
            return false;
        out = it->second;
        spent.insert(id);
        return true;
    };

    for (size_t i = 0; i < tx.ops.size(); ++i) {
        const json& op = tx.ops[i];
        json p = op.value("payload", json::object());
        const Bytes32 opId = bytes(fold(tx.hash), 18, i);
        switch (op.value("opcode", -1)) {
        case 0: {
            for (const auto& in : p["inputs"]) {
                Note n;
                if (!take(in.get<std::string>(), n, false)) {
                    why = "input note " + in.get<std::string>() + " does not exist or is already spent";
                    return false;
                }
                surplus += n.value;
            }
            size_t j = 0;
            for (const auto& o : p["outputs"]) {
                Note n;
                n.id = outputNoteId(tx.hash, i, j++);
                n.value = o["value"].get<uint64_t>();
                parseHex32(o["pk"].get<std::string>(), n.pk);
                n.createdSlot = slot;
                created[n.id] = n;
                surplus -= n.value;
            }
            break;
        }
        case 18: {
            json notes = json::array();
            uint64_t amount = 0;
            for (const auto& in : p["inputs"]) {
                Note n;
                if (!take(in.get<std::string>(), n, false)) {
                    why = "deposit input " + in.get<std::string>() + " does not exist";
                    return false;
                }
                amount += n.value;
                n.channel = true;
                channelNotes.push_back(n);
                notes.push_back({{"note_id", hex(n.id)}, {"value", n.value}, {"pk", hex(n.pk)}});
            }
            txEvents.push_back({{"Tx",
                                 {{"tx_hash", hex(tx.hash)},
                                  {"op_id", intArray(opId)},
                                  {"payload",
                                   {{"Deposit",
                                     {{"channel_id", p["channel_id"]},
                                      {"amount", amount},
                                      {"metadata", p.value("metadata", json::array())},
                                      {"notes", notes}}}}}}}});
            break;
        }
        case 32: {
            Bytes32 noteId{}, provider{}, zk{};
            parseHex32(p.value("service_note_id", std::string()), noteId);
            parseHex32(p.value("provider_id", std::string()), provider);
            parseHex32(p.value("zk_id", std::string()), zk);
            auto it = m_notes.find(noteId);
            if (it == m_notes.end() || spent.count(noteId)) {
                why = "service note does not exist";
                return false;
            }
            if (it->second.service) {
                why = "service note already used for BN";
                return false;
            }
            if (it->second.channel) {
                why = "service note is a channel note";
                return false;
            }
            if (it->second.value < m_params.minStake) {
                why = "service note insufficient value: " + std::to_string(it->second.value) + " < min_stake " +
                      std::to_string(m_params.minStake);
                return false;
            }
            for (const auto& [prov, z] : m_params.genesisProviders)
                if (prov == provider || z == zk) {
                    why = "duplicate provider_id or zk_id";
                    return false;
                }
            if (m_declaration && (m_declaration->providerId == provider || m_declaration->zkId == zk)) {
                why = "duplicate provider_id or zk_id";
                return false;
            }
            Declaration d;
            const std::string locators = p.value("locators", json::array()).dump();
            d.id = bytes(fold(provider) ^ fold(zk), 19, std::hash<std::string>{}(locators));
            d.providerId = provider;
            d.zkId = zk;
            d.noteId = noteId;
            d.locator = p["locators"].empty() ? "" : p["locators"][0].get<std::string>();
            d.created = epochOf(slot);
            d.active = d.created + 2;
            declared = d;
            break;
        }
        case 34: {
            Bytes32 id{};
            parseHex32(p.value("declaration_id", std::string()), id);
            const uint64_t nonce = p.value("nonce", uint64_t{0});
            if (!m_declaration || m_declaration->id != id) {
                why = "unknown declaration";
                return false;
            }
            activity = {nonce, p.value("metadata", json::object()).value("Blend", json::object()).value("epoch", 0u)};
            break;
        }
        case 48: {
            Bytes32 nullifier{}, pk{};
            parseHex32(p.value("voucher_nullifier", std::string()), nullifier);
            parseHex32(p.value("pk", std::string()), pk);
            auto v = std::find_if(m_vouchers.begin(), m_vouchers.end(),
                                  [&](const Voucher& x) { return x.nullifier == nullifier; });
            if (v == m_vouchers.end() || v->claimed || v->epoch >= epochOf(slot)) {
                why = "voucher is not claimable";
                return false;
            }
            claimedVouchers.push_back(nullifier);
            const uint32_t prev = epochOf(slot) - 1;
            const uint64_t from = static_cast<uint64_t>(prev) * m_params.epochSlots;
            const size_t a = indexOfSlot(from), b = indexOfSlot(from + m_params.epochSlots);
            const uint64_t blocks = std::max<size_t>(1, b - a);
            const uint64_t reward = epochBlendIncome(prev) * 4 / 6 / blocks;
            Note n{outputNoteId(tx.hash, i, 0), reward, pk, slot};
            created[n.id] = n;
            txEvents.push_back(
                {{"Tx",
                  {{"tx_hash", hex(tx.hash)},
                   {"op_id", intArray(opId)},
                   {"payload",
                    {{"LeaderRewardClaimed",
                      {{"voucher_nullifier", hex(nullifier)},
                       {"utxo",
                        {{"op_id", intArray(opId)}, {"output_index", 0}, {"note", {{"value", reward}, {"pk", hex(pk)}}}}}}}}}}}});
            break;
        }
        case 64: {
            Bytes32 pk{};
            parseHex32(p.value("public_key", std::string()), pk);
            auto t = std::find_if(m_tickets.begin(), m_tickets.end(), [&](const Ticket& x) { return x.pk == pk; });
            if (t == m_tickets.end()) {
                why = "unknown PoW ticket";
                return false;
            }
            retiredTickets.push_back(t->nullifier);
            Note n{powNoteId(p["public_key"].get<std::string>(), p["block_hash"]), m_params.powEpochReward, pk, slot};
            created[n.id] = n;
            txEvents.push_back({{"Tx",
                                 {{"tx_hash", hex(tx.hash)},
                                  {"op_id", intArray(opId)},
                                  {"payload",
                                   {{"PoWRewardClaimed",
                                     {{"pow_nullifier", hex(t->nullifier)},
                                      {"utxo",
                                       {{"op_id", intArray(opId)},
                                        {"output_index", 0},
                                        {"note", {{"value", n.value}, {"pk", hex(pk)}}}}}}}}}}}});
            break;
        }
        case 16: {
            const std::string id = p.value("channel", std::string());
            channels[id] = {{"accredited_keys", p.value("keys", json::array())},
                            {"configuration_threshold", p.value("configuration_threshold", 1)},
                            {"tip_message", channels.count(id) ? channels[id]["tip_message"] : json(std::string(64, '0'))},
                            {"config_tip_hash", hex(opId)},
                            {"tip_slot", slot},
                            {"tip_sequencer", 0},
                            {"tip_sequencer_starting_slot", slot},
                            {"posting_timeframe", p.value("posting_timeframe", 0)},
                            {"posting_timeout", p.value("posting_timeout", 0)},
                            {"transfer_threshold", p.value("transfer_threshold", 1)}};
            break;
        }
        case 17: {
            const std::string id = p.value("channel_id", std::string());
            if (!channels.count(id)) {
                if (p.value("parent", std::string()) != std::string(64, '0')) {
                    why = "inscription into an unknown channel";
                    return false;
                }
                channels[id] = {{"accredited_keys", json::array({p.value("signer", std::string())})},
                                {"configuration_threshold", 1},
                                {"tip_message", std::string(64, '0')},
                                {"config_tip_hash", std::string(64, '0')},
                                {"tip_slot", slot},
                                {"tip_sequencer", 0},
                                {"tip_sequencer_starting_slot", slot},
                                {"posting_timeframe", 0},
                                {"posting_timeout", 0},
                                {"transfer_threshold", 1}};
            }
            channels[id]["tip_message"] = hex(bytes(fold(tx.hash), 20, i));
            channels[id]["tip_slot"] = slot;
            break;
        }
        default:
            break;
        }
    }

    const uint64_t required = fee(tx.ops, tx.proofs);
    if (surplus < static_cast<__int128>(required)) {
        why = "insufficient balance for the fee (needs " + std::to_string(required) + ")";
        return false;
    }

    // Commit.
    for (const auto& id : spent) {
        m_notes.erase(id);
        m_reserved.erase(id);
    }
    for (const auto& id : tx.reserves)
        m_reserved.erase(id);
    for (auto& [id, n] : created)
        if (isKnown(n.pk))
            m_notes[id] = n;
    for (auto& n : channelNotes)
        if (isKnown(n.pk))
            m_notes[n.id] = n;
    m_channels = std::move(channels);
    if (declared) {
        m_notes[declared->noteId].service = true;
        if (declared->providerId == m_cfg.blendSigningKey) {
            m_declaration = declared;
            LBLOG("Blend declaration %s on-chain in epoch %u; in the core set from epoch %u",
                 hex(declared->id).c_str(), declared->created, declared->created + 2);
        }
    }
    if (activity && m_declaration) {
        m_declaration->active = std::max(m_declaration->active, epochOf(slot));
        m_activityAccepted.insert(activity->second);
    }
    for (const auto& n : claimedVouchers)
        for (auto& v : m_vouchers)
            if (v.nullifier == n)
                v.claimed = true;
    for (const auto& n : retiredTickets) {
        m_ticketsInFlight.erase(n);
        m_tickets.erase(std::remove_if(m_tickets.begin(), m_tickets.end(),
                                       [&](const Ticket& t) { return t.nullifier == n; }),
                        m_tickets.end());
    }
    for (auto& e : txEvents)
        events.push_back(e);
    m_includedTxs[slot].push_back({{"hash", hex(tx.hash)}, {"ops", tx.ops}, {"proofs", tx.proofs}});
    return true;
}

// ==== queries ================================================================

CryptarchiaView Node::cryptarchiaInfo() {
    std::lock_guard lock(m_mutex);
    CryptarchiaView v;
    v.lib = blockId(m_chain[m_lib]);
    v.libSlot = m_chain[m_lib];
    v.tip = blockId(m_chain[m_tip]);
    v.slot = m_chain[m_tip];
    v.height = m_tip;
    v.mode = m_mode;
    return v;
}

TimeView Node::timeInfo() {
    std::lock_guard lock(m_mutex);
    const uint64_t slot = clockSlot(nowMs());
    return {m_params.slotMs, m_params.genesisMs, slot, epochOf(slot)};
}

NetworkView Node::networkInfo() {
    std::lock_guard lock(m_mutex);
    if (m_isolated)
        return {};
    const size_t up = static_cast<size_t>((nowMs() - m_startedMs) / 3000);
    const size_t peers = std::min<size_t>(8, 1 + up);
    return {peers, static_cast<uint32_t>(peers + 1), peers < 8 ? 1u : 0u, peers * 2 + 3};
}

std::optional<std::string> Node::block(const Bytes32& id) {
    std::lock_guard lock(m_mutex);
    auto it = m_idToSlot.find(hex(id));
    if (it == m_idToSlot.end())
        return std::nullopt;
    const size_t index = indexOfSlot(it->second);
    if (index > m_tip)
        return std::nullopt;
    return blockCore(index, false).dump();
}

std::string Node::blocks(uint64_t from, uint64_t to, Error& err) {
    std::lock_guard lock(m_mutex);
    if (to < from) {
        err = {kRelayError, "Failed to get blocks: Invalid slot range: from_slot > to_slot"};
        return {};
    }
    json out = json::array();
    to = std::min(to, m_chain[m_lib]);
    for (size_t i = indexOfSlot(from); i <= m_lib && i < m_chain.size() && m_chain[i] <= to; ++i)
        out.push_back(blockCore(i, false));
    return out.dump();
}

std::optional<std::string> Node::transaction(const Bytes32& hash) {
    std::lock_guard lock(m_mutex);
    auto it = m_txIndex.find(hex(hash));
    if (it == m_txIndex.end())
        return std::nullopt;
    return it->second.first;
}

std::optional<std::string> Node::blockEvents(const Bytes32& id) {
    std::lock_guard lock(m_mutex);
    auto it = m_idToSlot.find(hex(id));
    if (it == m_idToSlot.end() || indexOfSlot(it->second) > m_tip)
        return std::nullopt;
    auto ev = m_events.find(it->second);
    return ev == m_events.end() ? std::string("[]") : ev->second.dump();
}

std::optional<std::string> Node::channelState(const Bytes32& id) {
    std::lock_guard lock(m_mutex);
    auto it = m_channels.find(hex(id));
    if (it == m_channels.end())
        return std::nullopt;
    return it->second.dump();
}

std::string Node::blendInfo() {
    std::lock_guard lock(m_mutex);
    json info = {{"node_id", peerId(m_cfg.blendSigningKey)}, {"core_info", nullptr}};
    const uint32_t epoch = epochOf(m_chain[m_tip]);
    const size_t networkSize = m_params.genesisProviders.size() + (memberOf(epoch) ? 1 : 0);
    if (m_mode == Mode::Online && memberOf(epoch) && networkSize >= m_params.minimumNetworkSize) {
        json peers = json::array();
        for (size_t i = 0; i < m_params.genesisProviders.size() && i < 4; ++i)
            peers.push_back(json::array({peerId(m_params.genesisProviders[i].first),
                                         m_params.blendReachable || unit(m_params.seed, 21, epoch, i) < 0.5}));
        info["core_info"] = {{"current_epoch_peers", peers}, {"old_epoch_peers", nullptr}};
    }
    return info.dump();
}

std::vector<Bytes32> Node::knownAddresses() {
    std::lock_guard lock(m_mutex);
    return m_cfg.knownKeys;
}

std::optional<uint64_t> Node::balance(const Bytes32& pk, const Bytes32* tip, Error& err) {
    std::lock_guard lock(m_mutex);
    if (!validFr(pk)) {
        err = {kDynError, "Invalid wallet address: not a valid field element"};
        return std::nullopt;
    }
    if (tip && !m_idToSlot.count(hex(*tip))) {
        err = {kDynError, "Failed to get balance: Ledger state corresponding to block 0x" + hex(*tip) + " not found"};
        return std::nullopt;
    }
    bool any = false;
    uint64_t total = 0;
    for (const auto& [id, n] : m_notes)
        if (n.pk == pk) {
            any = true;
            if (!n.channel)
                total += n.value;
        }
    if (!any) {
        err = {kNotFound, "Unknown wallet address."};
        return std::nullopt;
    }
    return total;
}

std::optional<WalletNotesView> Node::walletNotes(const Bytes32& pk, const Bytes32* tip, Error& err) {
    std::lock_guard lock(m_mutex);
    if (tip && !m_idToSlot.count(hex(*tip))) {
        err = {kDynError, "Failed to get wallet notes: Ledger state corresponding to block 0x" + hex(*tip) +
                              " not found"};
        return std::nullopt;
    }
    WalletNotesView v;
    v.tip = tip ? *tip : blockId(m_chain[m_tip]);
    bool any = false;
    for (const auto& [id, n] : m_notes)
        if (n.pk == pk) {
            any = true;
            if (!n.channel)
                v.notes.push_back(n);
        }
    if (!any) {
        err = {kNotFound, "Unknown wallet address."};
        return std::nullopt;
    }
    return v;
}

WalletNotesView Node::agedNotes(const Bytes32* tip, Error& err) {
    std::lock_guard lock(m_mutex);
    return agedNotesLocked(tip, err);
}

WalletNotesView Node::agedNotesLocked(const Bytes32* tip, Error& err) const {
    WalletNotesView v;
    uint64_t tipSlot = m_chain[m_tip];
    if (tip) {
        auto it = m_idToSlot.find(hex(*tip));
        if (it == m_idToSlot.end()) {
            err = {kDynError, "Failed to get leader aged notes: unknown block 0x" + hex(*tip)};
            return v;
        }
        tipSlot = it->second;
    }
    v.tip = blockId(tipSlot);
    const uint32_t epoch = epochOf(tipSlot);
    for (const auto& [id, n] : m_notes)
        if (!n.channel && isKnown(n.pk) && epochOf(n.createdSlot) + 2 <= epoch)
            v.notes.push_back(n);
    return v;
}

VouchersView Node::claimableVouchers() {
    std::lock_guard lock(m_mutex);
    VouchersView v;
    v.tip = blockId(m_chain[m_tip]);
    const uint32_t epoch = epochOf(m_chain[m_tip]);
    std::set<std::string> inFlight;
    for (const auto& tx : m_mempool)
        for (const auto& op : tx.ops)
            if (op.value("opcode", -1) == 48)
                inFlight.insert(op["payload"]["voucher_nullifier"].get<std::string>());
    for (const auto& vc : m_vouchers)
        if (!vc.claimed && vc.epoch < epoch && !inFlight.count(hex(vc.nullifier)))
            v.vouchers.push_back(vc);
    if (epoch > 0) {
        const uint64_t from = static_cast<uint64_t>(epoch - 1) * m_params.epochSlots;
        const size_t a = indexOfSlot(from), b = indexOfSlot(from + m_params.epochSlots);
        v.rewardAmount = epochBlendIncome(epoch - 1) * 4 / 6 / std::max<size_t>(1, b - a);
    }
    return v;
}

// ==== wallet writes ==========================================================

Bytes32 Node::transfer(const std::vector<Bytes32>& funding, const Bytes32& change, const Bytes32& recipient,
                       uint64_t amount, const Bytes32* tip, Error& err) {
    std::lock_guard lock(m_mutex);
    if (tip && !m_idToSlot.count(hex(*tip))) {
        err = {kDynError, "Failed to transfer funds: Requested wallet state for unknown block: 0x" + hex(*tip)};
        return {};
    }
    std::vector<Note> picked;
    uint64_t txFee = 0, available = 0;
    const json none = json::array();
    if (!selectFunding(funding, amount, none, none, picked, txFee, available, {}, 2)) {
        err = {kDynError, "Failed to transfer funds: Wallet does not have enough funds, available=" +
                              std::to_string(available)};
        return {};
    }
    uint64_t in = 0;
    std::set<Bytes32> reserves;
    for (auto& n : picked) {
        in += n.value;
        reserves.insert(n.id);
    }
    std::vector<std::pair<uint64_t, Bytes32>> outs = {{amount, recipient}};
    if (in > amount + txFee)
        outs.push_back({in - amount - txFee, change});
    return enqueue(json::array({transferOp(picked, outs)}), json::array({zkSig(m_rng())}), reserves);
}

Bytes32 Node::channelDeposit(const Bytes32& channel, const Bytes32& fundingPk, uint64_t amount,
                             const std::vector<uint8_t>& metadata, Error& err) {
    std::lock_guard lock(m_mutex);
    if (amount == 0) {
        err = {kRuntimeError, "ChannelDeposit `amount` must be greater than zero."};
        return {};
    }
    std::vector<Note> notes;
    for (const auto& [id, n] : m_notes)
        if (n.pk == fundingPk && !n.channel)
            notes.push_back(n);
    if (notes.empty()) {
        err = {kNotFound, "Unknown funding address."};
        return {};
    }
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.value > b.value; });
    std::vector<Note> picked;
    uint64_t total = 0;
    for (const auto& n : notes) {
        if (total >= amount)
            break;
        picked.push_back(n);
        total += n.value;
    }
    if (total < amount) {
        err = {kDynError, "Insufficient funds to cover deposit amount."};
        return {};
    }
    // Like the real call, no fee is funded: the ledger rejects the tx unless gas is free.
    std::vector<std::pair<uint64_t, Bytes32>> outs = {{amount, fundingPk}};
    if (total > amount)
        outs.push_back({total - amount, fundingPk});
    json transfer = transferOp(picked, outs);
    json meta = json::array();
    for (uint8_t b : metadata)
        meta.push_back(b);
    const Bytes32 provisional = txHash(transfer);
    json deposit = {{"opcode", 18},
                    {"payload",
                     {{"channel_id", hex(channel)},
                      {"inputs", json::array({hex(outputNoteId(provisional, 0, 0))})},
                      {"metadata", meta}}}};
    // The deposit spends the transfer's first output; keep the ids consistent by
    // hashing with the provisional hash the output id was derived from.
    PendingTx tx;
    tx.hash = provisional;
    tx.ops = json::array({transfer, deposit});
    const uint64_t s = m_rng();
    tx.proofs = json::array({zkSig(s), zkSig(s)});
    tx.submitSlot = clockSlot(nowMs());
    m_txIndex[hex(tx.hash)] = {json{{"mantle_tx", {{"ops", tx.ops}}}, {"ops_proofs", tx.proofs}}.dump(), 0};
    m_mempool.push_back(tx);
    return tx.hash;
}

Bytes32 Node::channelDepositWithNotes(const Bytes32& channel, const std::vector<Bytes32>& noteIds,
                                      const std::vector<uint8_t>& metadata, const Bytes32& change,
                                      const std::vector<Bytes32>& funding, uint64_t maxFee, Error& err) {
    std::lock_guard lock(m_mutex);
    if (noteIds.empty()) {
        err = {kRuntimeError, "ChannelDeposit requires at least one input note."};
        return {};
    }
    json inputs = json::array();
    std::set<Bytes32> reserves;
    for (const auto& id : noteIds) {
        auto it = m_notes.find(id);
        if (it == m_notes.end() || it->second.channel) {
            err = {kDynError, "Failed to sign tx: MissingInputNote(" + hex(id) + ")"};
            return {};
        }
        inputs.push_back(hex(id));
        reserves.insert(id);
    }
    json meta = json::array();
    for (uint8_t b : metadata)
        meta.push_back(b);
    json ops = json::array(
        {{{"opcode", 18}, {"payload", {{"channel_id", hex(channel)}, {"inputs", inputs}, {"metadata", meta}}}}});
    std::vector<Note> picked;
    uint64_t txFee = 0, available = 0;
    json proofs = json::array({zkSig(m_rng())});
    if (!selectFunding(funding, 0, ops, proofs, picked, txFee, available, reserves)) {
        err = {kDynError, "Failed to fund tx: Wallet does not have enough funds, available=" + std::to_string(available)};
        return {};
    }
    uint64_t in = 0;
    for (auto& n : picked) {
        in += n.value;
        reserves.insert(n.id);
    }
    if (txFee > maxFee) {
        err = {kDynError, "tx_fee(" + std::to_string(txFee) + ") exceeds max_tx_fee(" + std::to_string(maxFee) + ")"};
        return {};
    }
    std::vector<std::pair<uint64_t, Bytes32>> outs;
    if (in > txFee)
        outs.push_back({in - txFee, change});
    ops.push_back(transferOp(picked, outs));
    proofs.push_back(zkSig(m_rng()));
    return enqueue(ops, proofs, reserves);
}

std::string Node::fundTx(const std::string& request, Error& err) {
    std::lock_guard lock(m_mutex);
    json req;
    try {
        req = json::parse(request);
        for (const char* k : {"tx_builder", "change_public_key", "funding_public_keys", "max_tx_fee"})
            if (!req.contains(k))
                throw std::runtime_error(std::string("missing field `") + k + "`");
        for (const char* k : {"mantle_tx", "ledger_inputs", "pending_transfer", "channel_multi_sig_proofs"})
            if (!req["tx_builder"].contains(k))
                throw std::runtime_error(std::string("missing field `") + k + "`");
    } catch (const std::exception& e) {
        err = {kValidationError, std::string("Failed to parse fund request: ") + e.what()};
        return {};
    }
    std::vector<Bytes32> funding;
    for (const auto& k : req["funding_public_keys"]) {
        Bytes32 b{};
        if (parseHex32(k.get<std::string>(), b))
            funding.push_back(b);
    }
    Bytes32 change{};
    parseHex32(req["change_public_key"].get<std::string>(), change);
    json ops = req["tx_builder"]["mantle_tx"].value("ops", json::array());
    json proofs = json::array();
    for (size_t i = 0; i < ops.size(); ++i)
        proofs.push_back(ops[i].value("opcode", -1) == 17 ? json{{"Ed25519Sig", hexOf(i, 64)}} : zkSig(i));
    std::vector<Note> picked;
    uint64_t txFee = 0, available = 0;
    if (!selectFunding(funding, 0, ops, proofs, picked, txFee, available)) {
        err = {kDynError, "Failed to fund tx: Wallet does not have enough funds, available=" + std::to_string(available)};
        return {};
    }
    uint64_t in = 0;
    for (auto& n : picked) {
        in += n.value;
        m_reserved[n.id] = 0; // held even when the fee cap fails, like the real wallet
    }
    const uint64_t maxFee = req["max_tx_fee"].get<uint64_t>();
    if (txFee > maxFee) {
        err = {kDynError, "tx_fee(" + std::to_string(txFee) + ") exceeds max_tx_fee(" + std::to_string(maxFee) + ")"};
        return {};
    }
    std::vector<std::pair<uint64_t, Bytes32>> outs;
    if (in > txFee)
        outs.push_back({in - txFee, change});
    if (!picked.empty())
        ops.push_back(transferOp(picked, outs));
    json out = {{"tip", hex(blockId(m_chain[m_tip]))},
                {"funded_tx", {{"ops", ops}}},
                {"transfer_proof", picked.empty() ? json(nullptr) : zkSig(m_rng())}};
    return out.dump();
}

Bytes32 Node::submitSigned(const std::string& signedTx, Error& err) {
    std::lock_guard lock(m_mutex);
    json tx;
    try {
        tx = json::parse(signedTx);
        if (!tx.contains("mantle_tx") || !tx["mantle_tx"].contains("ops") || !tx.contains("ops_proofs"))
            throw std::runtime_error("missing field `mantle_tx` or `ops_proofs`");
    } catch (const std::exception& e) {
        err = {kValidationError, std::string("Failed to parse signed transaction: ") + e.what()};
        return {};
    }
    if (tx["mantle_tx"]["ops"].size() != tx["ops_proofs"].size()) {
        err = {kValidationError, "Failed to preverify signed transaction: proof count does not match op count"};
        return {};
    }
    std::set<Bytes32> reserves;
    for (const auto& op : tx["mantle_tx"]["ops"])
        if (op.value("opcode", -1) == 0)
            for (const auto& in : op["payload"]["inputs"]) {
                Bytes32 id{};
                if (parseHex32(in.get<std::string>(), id) && m_notes.count(id))
                    reserves.insert(id);
            }
    return enqueue(tx["mantle_tx"]["ops"], tx["ops_proofs"], reserves);
}

void Node::blockUntilOnline(std::unique_lock<std::mutex>& lock, Error& err) {
    // No timeout, like the services behind these calls.
    m_cv.wait(lock, [this] { return m_mode == Mode::Online || m_stopping || m_failed; });
    if (m_stopping || m_failed)
        err = {kRelayError, "Failed to reach the service: the node is shutting down."};
}

Bytes32 Node::leaderClaim(Error& err) {
    std::unique_lock lock(m_mutex);
    blockUntilOnline(lock, err);
    if (err)
        return {};
    lock.unlock();
    VouchersView v = claimableVouchers();
    lock.lock();
    if (v.vouchers.empty()) {
        err = {kServiceError, "Failed to claim leader rewards: NoClaimableVoucher"};
        return {};
    }
    json ops = json::array({{{"opcode", 48},
                             {"payload",
                              {{"rewards_root", hex(fr(m_rng(), 1, 0))},
                               {"voucher_nullifier", hex(v.vouchers.front().nullifier)},
                               {"pk", hex(m_cfg.leaderFundingPk)}}}}});
    json proofs = json::array({{{"PoC", {{"proof", hexOf(m_rng(), 128)}}}}});
    std::vector<Note> picked;
    uint64_t txFee = 0, available = 0;
    if (!selectFunding({m_cfg.leaderFundingPk}, 0, ops, proofs, picked, txFee, available) ||
        txFee > m_cfg.leaderMaxTxFee) {
        err = {kServiceError, "Failed to claim leader rewards: InsufficientFunds { available: " +
                                  std::to_string(available) + " }"};
        return {};
    }
    uint64_t in = 0;
    std::set<Bytes32> reserves;
    for (auto& n : picked) {
        in += n.value;
        reserves.insert(n.id);
    }
    std::vector<std::pair<uint64_t, Bytes32>> outs;
    if (in > txFee)
        outs.push_back({in - txFee, m_cfg.leaderFundingPk});
    ops.push_back(transferOp(picked, outs));
    proofs.push_back(zkSig(m_rng()));
    return enqueue(ops, proofs, reserves);
}

Bytes32 Node::blendJoin(const std::string& locator, const Bytes32& noteId, Error& err) {
    std::lock_guard lock(m_mutex);
    static const std::regex kLocator(R"(^/(ip4|ip6|dns|dns4|dns6)/([^/]+)/udp/(\d{1,5})/quic-v1$)");
    std::smatch m;
    if (locator.size() > 329 || !std::regex_match(locator, m, kLocator) || m[2] == "0.0.0.0" || m[2] == "::") {
        err = {kValidationError, "`locator` is not a valid locator."};
        return {};
    }
    if (!validFr(noteId)) {
        err = {kValidationError, "Invalid `service_note_id` bytes."};
        return {};
    }
    const Error closed{kRelayError,
                       "Failed to join blend network: Failed to receive a message from the SDP service: channel closed"};
    if (!m_notes.count(noteId)) {
        LBLOG("blend join: the wallet does not own note %s, so it cannot sign the declaration", hex(noteId).c_str());
        err = closed;
        return {};
    }
    json ops = json::array({{{"opcode", 32},
                             {"payload",
                              {{"service_type", "BN"},
                               {"locators", json::array({locator})},
                               {"provider_id", hex(m_cfg.blendSigningKey)},
                               {"zk_id", hex(m_cfg.blendZkKey)},
                               {"service_note_id", hex(noteId)}}}}});
    const uint64_t s = m_rng();
    json proofs =
        json::array({{{"ZkAndEd25519Sigs", {{"zk_sig", zkSig(s)["ZkSig"]}, {"ed25519_sig", hexOf(s, 64)}}}}});
    std::vector<Note> picked;
    uint64_t txFee = 0, available = 0;
    if (!selectFunding({m_cfg.sdpFundingPk}, 0, ops, proofs, picked, txFee, available, {noteId})) {
        LBLOG("blend join: the SDP funding key cannot pay the declaration fee (available=%llu)",
             static_cast<unsigned long long>(available));
        err = closed;
        return {};
    }
    if (txFee > m_cfg.sdpMaxTxFee) {
        LBLOG("blend join: fee %llu exceeds sdp.wallet.max_tx_fee", static_cast<unsigned long long>(txFee));
        err = closed;
        return {};
    }
    uint64_t in = 0;
    std::set<Bytes32> reserves;
    for (auto& n : picked) {
        in += n.value;
        reserves.insert(n.id);
    }
    std::vector<std::pair<uint64_t, Bytes32>> outs;
    if (in > txFee)
        outs.push_back({in - txFee, m_cfg.sdpFundingPk});
    ops.push_back(transferOp(picked, outs));
    proofs.push_back(zkSig(s + 7));
    enqueue(ops, proofs, reserves);
    const std::string locators = json::array({locator}).dump();
    const Bytes32 id =
        bytes(fold(m_cfg.blendSigningKey) ^ fold(m_cfg.blendZkKey), 19, std::hash<std::string>{}(locators));
    m_cfg.sdpDeclarationId = id;
    return id;
}

// ==== PoW ====================================================================

void Node::powSetMining(bool on) {
    std::lock_guard lock(m_mutex);
    m_mining = on;
}

void Node::powSetAutoClaim(bool on) {
    std::lock_guard lock(m_mutex);
    m_autoClaimArmed = on;
}

PowStatusView Node::powStatus(Error& err) {
    std::unique_lock lock(m_mutex);
    blockUntilOnline(lock, err);
    PowStatusView v;
    v.mining = m_mining;
    v.armed = m_autoClaimArmed;
    v.tick = m_cfg.powTick;
    v.tickInSlots = m_cfg.powTickInSlots;
    for (const auto& t : m_cfg.powTargets)
        v.targets.push_back({t.pk, t.threshold, balanceOf(t.pk)});
    return v;
}

std::vector<uint64_t> Node::powClaimable(Error& err) {
    std::unique_lock lock(m_mutex);
    blockUntilOnline(lock, err);
    std::vector<uint64_t> out;
    const uint64_t tipSlot = m_chain[m_tip];
    for (const auto& t : m_tickets)
        if (!m_ticketsInFlight.count(t.nullifier) && tipSlot <= t.blockSlot + m_params.powSlotWindow)
            out.push_back(t.blockSlot + m_params.powSlotWindow - tipSlot);
    return out;
}

Bytes32 Node::powClaim(const Bytes32* address, Error& err) {
    std::unique_lock lock(m_mutex);
    blockUntilOnline(lock, err);
    if (err)
        return {};
    Bytes32 target{};
    if (address) {
        target = *address;
    } else {
        const NodeConfig::Target* best = nullptr;
        uint64_t bestBalance = 0;
        for (const auto& t : m_cfg.powTargets) {
            const uint64_t b = balanceOf(t.pk);
            if (b < t.threshold && (!best || b < bestBalance)) {
                best = &t;
                bestBalance = b;
            }
        }
        if (!best) {
            bool any = false;
            for (const auto& t : m_tickets)
                any = any || !m_ticketsInFlight.count(t.nullifier);
            if (!any) {
                err = {kNotFound, "No PoW rewards available to claim."};
                return {};
            }
            err = {kServiceError, "Failed to claim PoW rewards: NoClaimTarget"};
            return {};
        }
        target = best->pk;
    }
    return claimPow(target, err);
}

Bytes32 Node::claimPow(const Bytes32& address, Error& err) {
    std::vector<Ticket> ready;
    for (const auto& t : m_tickets)
        if (!m_ticketsInFlight.count(t.nullifier))
            ready.push_back(t);
    if (ready.empty()) {
        err = {kNotFound, "No PoW rewards available to claim."};
        return {};
    }
    json ops = json::array(), proofs = json::array();
    std::vector<std::string> minted;
    auto sweep = [&]() {
        json in = json::array();
        for (auto& id : minted)
            in.push_back(id);
        ops.push_back({{"opcode", 0},
                       {"payload", {{"inputs", in}, {"outputs", json::array({{{"value", 0}, {"pk", hex(address)}}})}}}});
        proofs.push_back(zkSig(m_rng()));
        minted.clear();
    };
    for (size_t i = 0; i < ready.size(); ++i) {
        const json blockHash = intArray(ready[i].blockId);
        ops.push_back({{"opcode", 64},
                       {"payload",
                        {{"epoch_nonce", hex(fr(m_params.seed, 22, epochOf(ready[i].blockSlot)))},
                         {"block_hash", blockHash},
                         {"public_key", hex(ready[i].pk)}}}});
        proofs.push_back({{"None", nullptr}});
        minted.push_back(hex(powNoteId(hex(ready[i].pk), blockHash)));
        if (minted.size() == 32 || i + 1 == ready.size())
            sweep();
    }
    const uint64_t total = m_params.powEpochReward * ready.size();
    const uint64_t txFee = fee(ops, proofs);
    if (txFee >= total) {
        err = {kServiceError, "Failed to claim PoW rewards: RewardBelowFee"};
        return {};
    }
    // Every sweep pays out to the claim address; the last one carries the fee.
    uint64_t left = total - txFee;
    for (auto& op : ops) {
        if (op["opcode"] != 0)
            continue;
        const uint64_t share = std::min<uint64_t>(left, op["payload"]["inputs"].size() * m_params.powEpochReward);
        const bool last = &op == &ops.back();
        const uint64_t value = last ? left : share;
        op["payload"]["outputs"] = json::array({{{"value", value}, {"pk", hex(address)}}});
        left -= value;
    }
    for (const auto& t : ready)
        m_ticketsInFlight.insert(t.nullifier);
    LBLOG("claiming %zu PoW ticket(s) to %s", ready.size(), hex(address).c_str());
    return enqueue(ops, proofs, {}, true);
}

// ==== persistence ============================================================

void Node::loadState() {
    json s = json::object();
    if (std::ifstream f(m_statePath); f) {
        try {
            s = json::parse(f);
            if (!s.is_object())
                s = json::object();
        } catch (...) {
            LBLOG("ignoring unreadable state file %s", m_statePath.c_str());
        }
    }
    auto b32 = [](const json& j) {
        Bytes32 b{};
        if (j.is_string())
            parseHex32(j.get<std::string>(), b);
        return b;
    };
    if (m_params.genesisMs == 0)
        m_params.genesisMs = s.value("genesis_ms", int64_t{0}) ? s["genesis_ms"].get<int64_t>() : nowMs() - 60000;
    for (const auto& n : s.value("notes", json::array())) {
        Note note{b32(n["id"]), n["value"].get<uint64_t>(), b32(n["pk"]), n["created_slot"].get<uint64_t>(),
                  n.value("service", false), n.value("channel", false)};
        m_notes[note.id] = note;
    }
    for (const auto& v : s.value("vouchers", json::array()))
        m_vouchers.push_back(
            {b32(v["commitment"]), b32(v["nullifier"]), v["epoch"].get<uint32_t>(), v["claimed"].get<bool>()});
    if (s.contains("declaration") && s["declaration"].is_object()) {
        const json& d = s["declaration"];
        m_declaration = Declaration{b32(d["id"]),
                                    b32(d["provider_id"]),
                                    b32(d["zk_id"]),
                                    b32(d["note_id"]),
                                    d["locator"].get<std::string>(),
                                    d["created"].get<uint32_t>(),
                                    d["active"].get<uint32_t>(),
                                    d["nonce"].get<uint64_t>()};
    }
    for (const auto& e : s.value("activity_accepted", json::array()))
        m_activityAccepted.insert(e.get<uint32_t>());
    for (const auto& e : s.value("online_epochs", json::array()))
        m_onlineEpochs.insert(e.get<uint32_t>());
    const json included = s.value("included_txs", json::object());
    for (const auto& [slot, txs] : included.items())
        m_includedTxs[std::stoull(slot)] = txs;
    const json events = s.value("events", json::object());
    for (const auto& [slot, ev] : events.items())
        m_events[std::stoull(slot)] = ev;
    const json channels = s.value("channels", json::object());
    for (const auto& [id, c] : channels.items())
        m_channels[id] = c;
    for (const auto& slot : s.value("our_slots", json::array()))
        m_ourSlots.insert(slot.get<uint64_t>());
    if (s.contains("sdp_declaration_id"))
        m_cfg.sdpDeclarationId = b32(s["sdp_declaration_id"]);

    // Rebuild the chain this node had stored, then resume from it.
    const uint64_t tipSlot = s.value("tip_slot", uint64_t{0});
    const uint64_t libSlot = s.value("lib_slot", uint64_t{0});
    m_chain = {0};
    m_idToSlot[hex(blockId(0))] = 0;
    for (uint64_t slot = 1; slot <= tipSlot; ++slot)
        if (networkHasBlock(slot) || m_ourSlots.count(slot)) {
            m_chain.push_back(slot);
            m_idToSlot[hex(blockId(slot))] = slot;
        }
    m_networkScanned = tipSlot;
    m_tip = indexOfSlot(tipSlot);
    if (m_tip >= m_chain.size())
        m_tip = m_chain.size() - 1;
    m_lib = std::min(indexOfSlot(libSlot), m_tip);

    const int64_t now = nowMs();
    const bool recent = s.value("last_mode", std::string()) == "Online" &&
                        now - s.value("last_online_ms", int64_t{0}) < static_cast<int64_t>(m_cfg.offlineGraceSeconds * 1000);
    if (recent && !m_cfg.forceBootstrap && m_lib > 0) {
        m_mode = Mode::Online;
        m_ibdDone = true;
    }
}

void Node::saveState() {
    json s;
    s["version"] = 1;
    s["genesis_ms"] = m_params.genesisMs;
    s["tip_slot"] = m_chain[m_tip];
    s["lib_slot"] = m_chain[m_lib];
    s["last_mode"] = m_mode == Mode::Online ? "Online" : "Bootstrapping";
    s["last_online_ms"] = m_mode == Mode::Online ? nowMs() : int64_t{0};
    json notes = json::array();
    for (const auto& [id, n] : m_notes)
        notes.push_back({{"id", hex(n.id)},
                         {"value", n.value},
                         {"pk", hex(n.pk)},
                         {"created_slot", n.createdSlot},
                         {"service", n.service},
                         {"channel", n.channel}});
    s["notes"] = notes;
    json vouchers = json::array();
    for (const auto& v : m_vouchers)
        vouchers.push_back({{"commitment", hex(v.commitment)},
                            {"nullifier", hex(v.nullifier)},
                            {"epoch", v.epoch},
                            {"claimed", v.claimed}});
    s["vouchers"] = vouchers;
    if (m_declaration)
        s["declaration"] = {{"id", hex(m_declaration->id)},
                            {"provider_id", hex(m_declaration->providerId)},
                            {"zk_id", hex(m_declaration->zkId)},
                            {"note_id", hex(m_declaration->noteId)},
                            {"locator", m_declaration->locator},
                            {"created", m_declaration->created},
                            {"active", m_declaration->active},
                            {"nonce", m_declaration->nonce}};
    if (m_cfg.sdpDeclarationId)
        s["sdp_declaration_id"] = hex(*m_cfg.sdpDeclarationId);
    s["activity_accepted"] = m_activityAccepted;
    s["online_epochs"] = m_onlineEpochs;
    json txs = json::object(), events = json::object(), channels = json::object();
    for (const auto& [slot, t] : m_includedTxs)
        txs[std::to_string(slot)] = t;
    for (const auto& [slot, e] : m_events)
        events[std::to_string(slot)] = e;
    for (const auto& [id, c] : m_channels)
        channels[id] = c;
    s["included_txs"] = txs;
    s["events"] = events;
    s["channels"] = channels;
    s["our_slots"] = m_ourSlots;

    std::error_code ec;
    fs::create_directories(fs::path(m_statePath).parent_path(), ec);
    const std::string tmp = m_statePath + ".tmp";
    if (std::ofstream f(tmp); f) {
        f << s.dump(1);
        f.close();
        fs::rename(tmp, m_statePath, ec);
    }
}

} // namespace lbmock
