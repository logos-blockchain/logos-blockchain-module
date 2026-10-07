#pragma once

// A simulated node behind the liblogos_blockchain C API. See mock/README.md.
//
// Nothing in here includes logos_blockchain.h: mock_ffi.cpp is the only file
// that speaks the C ABI, and it translates to and from these types.

#include <array>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace lbmock {

using json = nlohmann::ordered_json;
using Bytes32 = std::array<uint8_t, 32>;
using Callback = void (*)(const char*);

// Same values as the C header's OperationStatusCode.
enum Code : int {
    kOk = 0,
    kNotFound = 1,
    kNullPointer = 2,
    kRelayError = 3,
    kServiceError = 6,
    kRuntimeError = 7,
    kDynError = 8,
    kInitializationError = 9,
    kValidationError = 12,
};

struct Error {
    int code = kOk;
    std::string message;
    explicit operator bool() const { return code != kOk; }
};

enum class Mode { Bootstrapping = 0, Online = 1, NotStarted = 2 };
enum class Stream { NewBlocks, ProcessedBlocks, LibBlocks };

std::string hex(const uint8_t* data, size_t len);
inline std::string hex(const Bytes32& b) { return hex(b.data(), b.size()); }
bool parseHex32(const std::string& s, Bytes32& out);
Bytes32 fromPtr(const uint8_t* p);

// True on the thread running a stream callback; the real FFI panics if called from one.
bool inStreamCallback();

// Deployment rules (what a deployment file fixes) plus the mock's own knobs.
struct Params {
    std::string chainId = "0.3.0-rc.5";
    int64_t genesisMs = 1790598600000;
    uint64_t slotMs = 1000;
    uint64_t securityParam = 120; // k, in blocks
    double activeSlotCoeff = 1.0 / 30;
    uint64_t epochSlots = 36000;
    uint64_t minStake = 1000000000;
    uint32_t inactivityPeriod = 2;
    uint32_t minimumNetworkSize = 2;
    uint64_t powEpochReward = 25000000;
    uint64_t powSlotWindow = 300;
    std::vector<std::pair<Bytes32, Bytes32>> genesisProviders; // provider_id, zk_id

    // Mock knobs (LB_MOCK_* env vars, see README).
    uint64_t seed = 0x6c6f676f73ULL;
    double ibdBlocksPerSecond = 400;
    double powTicketsPerBlock = 1.0;
    double blendActivityHitRate = 0.9;
    uint64_t networkStake = 2968004000000000ULL;
    double backgroundTxPerBlock = 0.7;
    bool blendReachable = true;
    bool assumePeers = false;
    bool panicOnReentry = true;
    std::optional<double> pbpSecondsOverride;
};

// The parts of user_config.yaml the simulated services read.
struct NodeConfig {
    std::vector<Bytes32> knownKeys;
    Bytes32 leaderFundingPk{};
    uint64_t leaderMaxTxFee = UINT64_MAX;
    Bytes32 sdpFundingPk{};
    uint64_t sdpMaxTxFee = UINT64_MAX;
    std::optional<Bytes32> sdpDeclarationId;
    Bytes32 blendSigningKey{}; // provider_id
    Bytes32 blendZkKey{};      // zk_id
    std::string blendListeningAddress;
    size_t initialPeers = 0;
    size_t ibdPeers = 0;
    double pbpSeconds = 3600;
    bool forceBootstrap = false;
    double offlineGraceSeconds = 1200;
    uint32_t maxTicketsPerBlock = 4;
    struct Target {
        Bytes32 pk;
        uint64_t threshold;
    };
    std::vector<Target> powTargets;
    uint64_t powTick = 300;
    bool powTickInSlots = false;
    uint64_t pendingNoteExpiryBlocks = 10;
    std::string stateFolder;
};

struct Note {
    Bytes32 id{};
    uint64_t value = 0;
    Bytes32 pk{};
    uint64_t createdSlot = 0;
    bool service = false; // locked as SDP collateral
    bool channel = false; // deposited into a channel
};

struct Voucher {
    Bytes32 commitment{};
    Bytes32 nullifier{};
    uint32_t epoch = 0;
    bool claimed = false;
};

struct Ticket {
    uint64_t blockSlot = 0;
    Bytes32 blockId{};
    Bytes32 pk{};
    Bytes32 nullifier{};
};

struct Declaration {
    Bytes32 id{};
    Bytes32 providerId{};
    Bytes32 zkId{};
    Bytes32 noteId{};
    std::string locator;
    uint32_t created = 0;
    uint32_t active = 0;
    uint64_t nonce = 0;
};

struct PendingTx {
    Bytes32 hash{};
    json ops = json::array();
    json proofs = json::array();
    uint64_t submitSlot = 0;
    std::set<Bytes32> reserves;
    bool viaBlend = false;
};

struct WalletNotesView {
    Bytes32 tip{};
    std::vector<Note> notes;
};

struct VouchersView {
    Bytes32 tip{};
    std::vector<Voucher> vouchers;
    uint64_t rewardAmount = 0;
};

struct PowStatusView {
    bool mining = false;
    bool armed = false;
    uint64_t tick = 0;
    bool tickInSlots = false;
    struct Target {
        Bytes32 pk;
        uint64_t threshold;
        uint64_t balance;
    };
    std::vector<Target> targets;
};

struct CryptarchiaView {
    Bytes32 lib{};
    uint64_t libSlot = 0;
    Bytes32 tip{};
    uint64_t slot = 0;
    uint64_t height = 0;
    Mode mode = Mode::Bootstrapping;
};

struct TimeView {
    uint64_t slotMs = 0;
    int64_t genesisMs = 0;
    uint64_t slot = 0;
    uint32_t epoch = 0;
};

struct NetworkView {
    size_t peers = 0;
    uint32_t connections = 0;
    uint32_t pending = 0;
    size_t discovered = 0;
};

class Node {
public:
    static std::unique_ptr<Node> start(const char* configPath, const char* deploymentPath, Error& err);
    ~Node();

    void shutdown();
    std::string chainId() const { return m_params.chainId; }

    Error subscribe(Stream stream, Callback cb);

    CryptarchiaView cryptarchiaInfo();
    TimeView timeInfo();
    NetworkView networkInfo();
    std::optional<std::string> block(const Bytes32& id);
    std::string blocks(uint64_t from, uint64_t to, Error& err);
    std::optional<std::string> transaction(const Bytes32& hash);
    std::optional<std::string> blockEvents(const Bytes32& id);
    std::optional<std::string> channelState(const Bytes32& id);
    std::string blendInfo();

    std::vector<Bytes32> knownAddresses();
    std::optional<uint64_t> balance(const Bytes32& pk, const Bytes32* tip, Error& err);
    std::optional<WalletNotesView> walletNotes(const Bytes32& pk, const Bytes32* tip, Error& err);
    WalletNotesView agedNotes(const Bytes32* tip, Error& err);
    VouchersView claimableVouchers();

    Bytes32 transfer(const std::vector<Bytes32>& funding, const Bytes32& change, const Bytes32& recipient,
                     uint64_t amount, const Bytes32* tip, Error& err);
    Bytes32 channelDeposit(const Bytes32& channel, const Bytes32& fundingPk, uint64_t amount,
                           const std::vector<uint8_t>& metadata, Error& err);
    Bytes32 channelDepositWithNotes(const Bytes32& channel, const std::vector<Bytes32>& noteIds,
                                    const std::vector<uint8_t>& metadata, const Bytes32& change,
                                    const std::vector<Bytes32>& funding, uint64_t maxFee, Error& err);
    std::string fundTx(const std::string& request, Error& err);
    Bytes32 submitSigned(const std::string& signedTx, Error& err);
    Bytes32 leaderClaim(Error& err);
    Bytes32 blendJoin(const std::string& locator, const Bytes32& noteId, Error& err);

    void powSetMining(bool on);
    void powSetAutoClaim(bool on);
    PowStatusView powStatus(Error& err);
    std::vector<uint64_t> powClaimable(Error& err);
    Bytes32 powClaim(const Bytes32* address, Error& err);

private:
    Node() = default;

    // ---- chain ----
    struct Emitted {
        Callback cb;
        std::string payload;
        bool end = false;
    };
    uint64_t clockSlot(int64_t nowMs) const;
    uint32_t epochOf(uint64_t slot) const { return static_cast<uint32_t>(slot / m_params.epochSlots); }
    bool networkHasBlock(uint64_t slot) const;
    Bytes32 blockId(uint64_t slot) const;
    void extendNetworkChain(uint64_t toSlot);
    size_t indexOfSlot(uint64_t slot) const;
    json headerJson(size_t index, bool withId) const;
    json backgroundTxs(uint64_t slot) const;
    json txsAt(uint64_t slot) const;
    json blockCore(size_t index, bool withTxId) const;
    json blockProcessed(size_t index) const;
    uint64_t blockFees(uint64_t slot) const;
    uint64_t epochBlendIncome(uint32_t epoch) const;

    void run();
    void tick(int64_t nowMs, std::vector<Emitted>& out);
    void processBlock(size_t index, std::vector<Emitted>& out);
    void onEpochStart(uint32_t epoch, uint64_t slot, json& headerEvents);
    void setLib(size_t index, std::vector<Emitted>& out);
    void evaluateLeadership(uint64_t fromSlot, uint64_t toSlot);
    void autoClaimTick(int64_t nowMs);
    void dispatch(std::vector<Emitted>& out);
    void endStreams(std::vector<Emitted>& out);

    // ---- ledger / wallet ----
    bool isKnown(const Bytes32& pk) const;
    uint64_t fee(const json& ops, const json& proofs) const;
    bool selectFunding(const std::vector<Bytes32>& funding, uint64_t needed, const json& ops, const json& proofs,
                       std::vector<Note>& picked, uint64_t& feeOut, uint64_t& available,
                       const std::set<Bytes32>& exclude = {}, size_t transferOutputs = 1) const;
    Bytes32 txHash(const json& ops);
    Bytes32 enqueue(json ops, json proofs, std::set<Bytes32> reserves, bool viaBlend = false);
    bool applyTx(const PendingTx& tx, uint64_t slot, json& events, std::string& why);
    bool applyTxUnchecked(const PendingTx& tx, uint64_t slot, json& events, std::string& why);
    bool memberOf(uint32_t epoch) const;
    WalletNotesView agedNotesLocked(const Bytes32* tip, Error& err) const;
    void blockUntilOnline(std::unique_lock<std::mutex>& lock, Error& err);
    uint64_t balanceOf(const Bytes32& pk) const;
    Bytes32 claimPow(const Bytes32& address, Error& err);

    void loadState();
    void saveState();

    Params m_params;
    NodeConfig m_cfg;
    std::string m_statePath;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_stopping = false;
    bool m_failed = false;
    std::mt19937_64 m_rng{std::random_device{}()};

    // network chain: every canonical slot up to the network's head
    std::vector<uint64_t> m_chain;
    std::unordered_map<std::string, uint64_t> m_idToSlot;
    std::set<uint64_t> m_ourSlots;
    uint64_t m_networkScanned = 0;

    // this node's view
    Mode m_mode = Mode::Bootstrapping;
    size_t m_tip = 0;
    size_t m_lib = 0;
    double m_syncCredit = 0;
    bool m_ibdDone = false;
    int64_t m_pbpStartMs = 0;
    int64_t m_startedMs = 0;
    int64_t m_lastTickMs = 0;
    int64_t m_lastSaveMs = 0;
    uint64_t m_leaderEvaluatedTo = 0;
    bool m_isolated = false;

    Callback m_streams[3] = {nullptr, nullptr, nullptr};

    // ledger (only this node's notes are tracked)
    std::map<Bytes32, Note> m_notes;
    std::map<Bytes32, uint64_t> m_reserved; // note -> release at height (0 = while in flight)
    std::vector<PendingTx> m_mempool;
    std::map<uint64_t, json> m_includedTxs; // slot -> [ {hash, ops, proofs} ]
    std::map<uint64_t, json> m_events;      // slot -> events array
    std::map<std::string, std::pair<std::string, int64_t>> m_txIndex; // hash -> (json, removedAtMs or 0)
    std::map<std::string, json> m_channels;
    std::vector<Voucher> m_vouchers;
    std::optional<Declaration> m_declaration;
    std::set<uint32_t> m_activityAccepted;
    std::set<uint32_t> m_onlineEpochs;
    std::map<std::string, Declaration> m_networkDeclarations;

    // PoW (runtime-only, like the real service)
    bool m_mining = false;
    bool m_autoClaimArmed = false;
    int64_t m_lastAutoClaimMs = 0;
    uint64_t m_lastAutoClaimSlot = 0;
    std::vector<Ticket> m_tickets;
    std::set<Bytes32> m_ticketsInFlight;
};

} // namespace lbmock
