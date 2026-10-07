// The node-facing half of the liblogos_blockchain C API, answered by the
// simulated node in mock_chain.cpp.
//
// Built into blockchain_module_plugin only by the mock variant (see
// CMakeLists.txt). These definitions are hidden and so bind the module's calls
// locally; everything not defined here (config generation, keys, peer id, ...)
// still resolves to the real liblogos_blockchain, which works without a node.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include <dlfcn.h>

#pragma GCC visibility push(hidden)
extern "C" {
#include <logos_blockchain.h>
}
#pragma GCC visibility pop

#include "mock_chain.h"

using lbmock::Bytes32;
using lbmock::Error;
using lbmock::Node;

namespace {

std::mutex g_allocMutex;
std::set<void*> g_allocated;

char* ownedString(const std::string& s) {
    char* p = strdup(s.c_str());
    std::lock_guard lock(g_allocMutex);
    g_allocated.insert(p);
    return p;
}

OperationStatus ok() { return {Ok, nullptr}; }

OperationStatus status(const Error& e) {
    if (!e)
        return ok();
    return {static_cast<OperationStatusCode>(e.code), ownedString(e.message)};
}

OperationStatus status(OperationStatusCode code, const std::string& message) {
    return {code, ownedString(message)};
}

OperationStatus nullNode() { return status(NullPointer, "Received a null `node` pointer."); }

Node* sim(const LogosBlockchainNode* node) {
    return node ? static_cast<Node*>(node->overwatch) : nullptr;
}

// A real FFI call from a stream callback runs block_on on a tokio worker
// thread, which panics; a panic in an extern "C" fn aborts the process.
void guard(const char* fn) {
    if (!lbmock::inStreamCallback())
        return;
    const char* v = std::getenv("LB_MOCK_PANIC_ON_REENTRY");
    if (v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0)) {
        std::fprintf(stderr, "[lb-mock] WARNING: %s called from a stream callback; the real node would abort here\n",
                     fn);
        return;
    }
    std::fprintf(stderr,
                 "thread 'tokio-runtime-worker' panicked (lb-mock, in %s):\n"
                 "Cannot start a runtime from within a runtime. This happens because a function (like `block_on`) "
                 "attempted to block the current thread while the thread is being used to drive asynchronous "
                 "tasks.\n",
                 fn);
    std::abort();
}

#define NODE_OR_RETURN(result_type)                                                                                    \
    guard(__func__);                                                                                                   \
    Node* n = sim(node);                                                                                               \
    if (!n) {                                                                                                          \
        result_type r{};                                                                                               \
        r.error = nullNode();                                                                                          \
        return r;                                                                                                      \
    }

template <class R> R stringResult(const std::optional<std::string>& value, const Error& notFound) {
    R r{};
    if (value)
        r.value = ownedString(*value);
    else
        r.error = status(notFound);
    return r;
}

template <class R> R hashResult(const Bytes32& h, const Error& e) {
    R r{};
    if (e)
        r.error = status(e);
    else
        std::memcpy(r.value, h.data(), 32);
    return r;
}

std::string debugBytes(const uint8_t* b) {
    std::string s = "[";
    for (int i = 0; i < 32; ++i)
        s += (i ? ", " : "") + std::to_string(b[i]);
    return s + "]";
}

std::vector<Bytes32> keyList(const uint8_t* const* keys, size_t len) {
    std::vector<Bytes32> out;
    for (size_t i = 0; i < len; ++i)
        if (keys[i])
            out.push_back(lbmock::fromPtr(keys[i]));
    return out;
}

} // namespace

extern "C" {

// ---- memory -----------------------------------------------------------------

OperationStatus free_cstring(char* pointer) {
    if (!pointer)
        return status(NullPointer, "Received a null pointer.");
    {
        std::lock_guard lock(g_allocMutex);
        if (g_allocated.erase(pointer)) {
            std::free(pointer);
            return ok();
        }
    }
    // Allocated by the real library (config/key helpers): hand it back.
    using Fn = OperationStatus (*)(char*);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "free_cstring"));
    if (real)
        return real(pointer);
    return ok();
}

// ---- lifecycle ----------------------------------------------------------------

FfiInitializedLogosBlockchainNodeResult start_lb_node(const char* config_path, const char* custom_deployment_path) {
    guard(__func__);
    FfiInitializedLogosBlockchainNodeResult r{};
    Error err;
    std::unique_ptr<Node> node = Node::start(config_path, custom_deployment_path, err);
    if (!node) {
        r.error = status(err);
        return r;
    }
    auto* handle = new LogosBlockchainNode{};
    handle->chain_id = strdup(node->chainId().c_str());
    handle->overwatch = node.release();
    r.value = handle;
    r.error = ok();
    return r;
}

OperationStatus shutdown_node(LogosBlockchainNode* node) {
    guard(__func__);
    if (!node)
        return nullNode();
    Node* n = sim(node);
    n->shutdown();
    delete n;
    std::free(node->chain_id);
    delete node;
    return ok();
}

OperationStatus subscribe_to_new_blocks(const LogosBlockchainNode* node, CCallback______c_char cb) {
    guard(__func__);
    Node* n = sim(node);
    return n ? status(n->subscribe(lbmock::Stream::NewBlocks, cb)) : nullNode();
}

OperationStatus subscribe_to_processed_blocks(const LogosBlockchainNode* node, CCallback______c_char cb) {
    guard(__func__);
    Node* n = sim(node);
    return n ? status(n->subscribe(lbmock::Stream::ProcessedBlocks, cb)) : nullNode();
}

OperationStatus subscribe_to_lib_blocks(const LogosBlockchainNode* node, CCallback______c_char cb) {
    guard(__func__);
    Node* n = sim(node);
    return n ? status(n->subscribe(lbmock::Stream::LibBlocks, cb)) : nullNode();
}

// ---- chain --------------------------------------------------------------------

FfiGetChainIdResult get_chain_id(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiGetChainIdResult)
    FfiGetChainIdResult r{};
    r.value = ownedString(n->chainId());
    r.error = ok();
    return r;
}

FfiCryptarchiaInfoResult get_cryptarchia_info(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiCryptarchiaInfoResult)
    const lbmock::CryptarchiaView v = n->cryptarchiaInfo();
    auto* info = new CryptarchiaInfo{};
    std::memcpy(info->lib, v.lib.data(), 32);
    info->lib_slot = v.libSlot;
    std::memcpy(info->tip, v.tip.data(), 32);
    info->slot = v.slot;
    info->height = v.height;
    info->mode = static_cast<State>(v.mode);
    FfiCryptarchiaInfoResult r{};
    r.value = info;
    r.error = ok();
    return r;
}

OperationStatus free_cryptarchia_info(CryptarchiaInfo* pointer) {
    if (!pointer)
        return status(NullPointer, "Received a null pointer.");
    delete pointer;
    return ok();
}

FfiTimeInfoResult get_time_info(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiTimeInfoResult)
    const lbmock::TimeView v = n->timeInfo();
    FfiTimeInfoResult r{};
    r.value = new TimeInfo{v.slotMs, v.genesisMs, v.slot, v.epoch};
    r.error = ok();
    return r;
}

OperationStatus free_time_info(TimeInfo* pointer) {
    if (!pointer)
        return status(NullPointer, "Received a null pointer.");
    delete pointer;
    return ok();
}

FfiNetworkInfoResult get_network_info(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiNetworkInfoResult)
    const lbmock::NetworkView v = n->networkInfo();
    FfiNetworkInfoResult r{};
    r.value = {v.peers, v.connections, v.pending, v.discovered};
    r.error = ok();
    return r;
}

FfiGetBlockResult get_block(const LogosBlockchainNode* node, const HeaderId* header_id) {
    NODE_OR_RETURN(FfiGetBlockResult)
    if (!header_id)
        return {nullptr, status(NullPointer, "Received a null `header_id` pointer.")};
    return stringResult<FfiGetBlockResult>(
        n->block(lbmock::fromPtr(*header_id)),
        {lbmock::kNotFound, "No block found for header id " + debugBytes(*header_id)});
}

FfiGetBlocksResult get_blocks(const LogosBlockchainNode* node, uint64_t from_slot, uint64_t to_slot) {
    NODE_OR_RETURN(FfiGetBlocksResult)
    Error err;
    std::string out = n->blocks(from_slot, to_slot, err);
    if (err)
        return {nullptr, status(err)};
    return {ownedString(out), ok()};
}

FfiGetTransactionResult get_transaction(const LogosBlockchainNode* node, const TxHash* tx_hash) {
    NODE_OR_RETURN(FfiGetTransactionResult)
    if (!tx_hash)
        return {nullptr, status(NullPointer, "Received a null `tx_hash` pointer.")};
    const Bytes32 h = lbmock::fromPtr(*tx_hash);
    return stringResult<FfiGetTransactionResult>(n->transaction(h),
                                                 {lbmock::kNotFound, "No transaction found for hash " + lbmock::hex(h)});
}

FfiGetBlockEventsResult get_block_events(const LogosBlockchainNode* node, const HeaderId* header_id) {
    NODE_OR_RETURN(FfiGetBlockEventsResult)
    if (!header_id)
        return {nullptr, status(NullPointer, "Received a null `header_id` pointer.")};
    return stringResult<FfiGetBlockEventsResult>(
        n->blockEvents(lbmock::fromPtr(*header_id)),
        {lbmock::kNotFound, "No block found for header id " + debugBytes(*header_id)});
}

FfiGetChannelStateResult get_channel_state(const LogosBlockchainNode* node, const uint8_t* channel_id) {
    NODE_OR_RETURN(FfiGetChannelStateResult)
    if (!channel_id)
        return {nullptr, status(NullPointer, "Received a null `channel_id` pointer.")};
    const Bytes32 id = lbmock::fromPtr(channel_id);
    return stringResult<FfiGetChannelStateResult>(n->channelState(id),
                                                  {lbmock::kNotFound, "No channel found for id " + lbmock::hex(id)});
}

// ---- blend ----------------------------------------------------------------------

FfiBlendInfoResult blend_info(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiBlendInfoResult)
    return {ownedString(n->blendInfo()), ok()};
}

FfiStatusResult_DeclarationId blend_join_as_core_node(const LogosBlockchainNode* node, const char* locator,
                                                       const uint8_t* service_note_id) {
    NODE_OR_RETURN(FfiStatusResult_DeclarationId)
    FfiStatusResult_DeclarationId r{};
    if (!locator) {
        r.error = status(NullPointer, "Received a null `locator` pointer.");
        return r;
    }
    if (!service_note_id) {
        r.error = status(NullPointer, "Received a null `service_note_id` pointer.");
        return r;
    }
    Error err;
    const Bytes32 id = n->blendJoin(locator, lbmock::fromPtr(service_note_id), err);
    if (err)
        r.error = status(err);
    else {
        std::memcpy(r.value._0, id.data(), 32);
        r.error = ok();
    }
    return r;
}

// ---- wallet -----------------------------------------------------------------------

FfiKnownAddressesResult get_known_addresses(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiKnownAddressesResult)
    const std::vector<Bytes32> keys = n->knownAddresses();
    // Non-null even when empty, like the leaked empty Box the real one returns.
    auto** addrs = new uint8_t*[keys.size() + 1];
    for (size_t i = 0; i < keys.size(); ++i) {
        addrs[i] = new uint8_t[32];
        std::memcpy(addrs[i], keys[i].data(), 32);
    }
    FfiKnownAddressesResult r{};
    r.value = {addrs, keys.size()};
    r.error = ok();
    return r;
}

OperationStatus free_known_addresses(KnownAddresses addresses) {
    if (!addresses.addresses)
        return status(NullPointer, "Received a null pointer.");
    for (size_t i = 0; i < addresses.len; ++i)
        delete[] addresses.addresses[i];
    delete[] addresses.addresses;
    return ok();
}

FfiBalanceResult get_balance(const LogosBlockchainNode* node, const uint8_t* wallet_address,
                             const HeaderId* optional_tip) {
    NODE_OR_RETURN(FfiBalanceResult)
    if (!wallet_address)
        return {0, status(NullPointer, "Received a null `wallet_address` pointer.")};
    Error err;
    Bytes32 tip{};
    if (optional_tip)
        tip = lbmock::fromPtr(*optional_tip);
    const auto v = n->balance(lbmock::fromPtr(wallet_address), optional_tip ? &tip : nullptr, err);
    if (!v)
        return {0, status(err)};
    return {*v, ok()};
}

FfiWalletNotesResult get_wallet_notes(const LogosBlockchainNode* node, const uint8_t* wallet_address,
                                      const HeaderId* optional_tip) {
    NODE_OR_RETURN(FfiWalletNotesResult)
    FfiWalletNotesResult r{};
    if (!wallet_address) {
        r.error = status(NullPointer, "Received a null `wallet_address` pointer.");
        return r;
    }
    Error err;
    Bytes32 tip{};
    if (optional_tip)
        tip = lbmock::fromPtr(*optional_tip);
    const auto v = n->walletNotes(lbmock::fromPtr(wallet_address), optional_tip ? &tip : nullptr, err);
    if (!v) {
        r.error = status(err);
        return r;
    }
    std::memcpy(r.value.tip, v->tip.data(), 32);
    r.value.notes = new WalletNote[v->notes.size() + 1];
    r.value.len = v->notes.size();
    for (size_t i = 0; i < v->notes.size(); ++i) {
        std::memcpy(r.value.notes[i].id, v->notes[i].id.data(), 32);
        r.value.notes[i].value = v->notes[i].value;
    }
    r.error = ok();
    return r;
}

OperationStatus free_wallet_notes(WalletNotes notes) {
    delete[] notes.notes;
    return ok();
}

FfiLeaderAgedNotesResult get_leader_aged_notes(const LogosBlockchainNode* node, const HeaderId* optional_tip) {
    NODE_OR_RETURN(FfiLeaderAgedNotesResult)
    FfiLeaderAgedNotesResult r{};
    Error err;
    Bytes32 tip{};
    if (optional_tip)
        tip = lbmock::fromPtr(*optional_tip);
    const lbmock::WalletNotesView v = n->agedNotes(optional_tip ? &tip : nullptr, err);
    if (err) {
        r.error = status(err);
        return r;
    }
    std::memcpy(r.value.tip, v.tip.data(), 32);
    r.value.notes = new LeaderAgedNote[v.notes.size() + 1];
    r.value.len = v.notes.size();
    uint64_t total = 0;
    for (size_t i = 0; i < v.notes.size(); ++i) {
        std::memcpy(r.value.notes[i].id, v.notes[i].id.data(), 32);
        r.value.notes[i].value = v.notes[i].value;
        std::memcpy(r.value.notes[i].public_key, v.notes[i].pk.data(), 32);
        total = total + v.notes[i].value < total ? UINT64_MAX : total + v.notes[i].value;
    }
    r.value.total_value = total;
    r.error = ok();
    return r;
}

OperationStatus free_leader_aged_notes(LeaderAgedNotes notes) {
    delete[] notes.notes;
    return ok();
}

FfiClaimableVouchersResult get_claimable_vouchers(const LogosBlockchainNode* node, const HeaderId* optional_tip) {
    NODE_OR_RETURN(FfiClaimableVouchersResult)
    (void)optional_tip;
    const lbmock::VouchersView v = n->claimableVouchers();
    FfiClaimableVouchersResult r{};
    std::memcpy(r.value.tip, v.tip.data(), 32);
    r.value.vouchers = new ClaimableVoucher[v.vouchers.size() + 1];
    r.value.len = v.vouchers.size();
    for (size_t i = 0; i < v.vouchers.size(); ++i) {
        std::memcpy(r.value.vouchers[i].commitment, v.vouchers[i].commitment.data(), 32);
        std::memcpy(r.value.vouchers[i].nullifier, v.vouchers[i].nullifier.data(), 32);
    }
    r.value.reward_amount = v.vouchers.empty() ? 0 : v.rewardAmount;
    r.value.total_claimable = r.value.reward_amount * v.vouchers.size();
    r.error = ok();
    return r;
}

OperationStatus free_claimable_vouchers(ClaimableVouchers vouchers) {
    if (!vouchers.vouchers)
        return status(NullPointer, "Received a null pointer.");
    delete[] vouchers.vouchers;
    return ok();
}

FfiTransferFundsResult transfer_funds(const LogosBlockchainNode* node, const TransferFundsArguments* arguments) {
    NODE_OR_RETURN(FfiTransferFundsResult)
    if (!arguments)
        return hashResult<FfiTransferFundsResult>({}, {lbmock::kNullPointer, "Received a null `arguments` pointer."});
    if (!arguments->change_public_key)
        return hashResult<FfiTransferFundsResult>({}, {lbmock::kNullPointer, "Received a null `change_public_key` pointer."});
    if (!arguments->funding_public_keys)
        return hashResult<FfiTransferFundsResult>({}, {lbmock::kNullPointer, "Received a null `funding_public_keys` pointer."});
    for (size_t i = 0; i < arguments->funding_public_keys_len; ++i)
        if (!arguments->funding_public_keys[i])
            return hashResult<FfiTransferFundsResult>(
                {}, {lbmock::kNullPointer, "Received a null pointer in `funding_public_keys`."});
    if (!arguments->recipient_public_key)
        return hashResult<FfiTransferFundsResult>({}, {lbmock::kNullPointer, "Received a null `recipient_public_key` pointer."});
    Error err;
    Bytes32 tip{};
    if (arguments->optional_tip)
        tip = lbmock::fromPtr(*arguments->optional_tip);
    const Bytes32 h = n->transfer(keyList(arguments->funding_public_keys, arguments->funding_public_keys_len),
                                  lbmock::fromPtr(arguments->change_public_key),
                                  lbmock::fromPtr(arguments->recipient_public_key), arguments->amount,
                                  arguments->optional_tip ? &tip : nullptr, err);
    return hashResult<FfiTransferFundsResult>(h, err);
}

FfiChannelDepositResult channel_deposit(const LogosBlockchainNode* node, const ChannelDepositArguments* arguments) {
    NODE_OR_RETURN(FfiChannelDepositResult)
    if (!arguments || !arguments->channel_id || !arguments->funding_public_key ||
        (!arguments->metadata && arguments->metadata_len))
        return hashResult<FfiChannelDepositResult>({}, {lbmock::kNullPointer, "Received a null pointer."});
    Error err;
    std::vector<uint8_t> metadata(arguments->metadata, arguments->metadata + arguments->metadata_len);
    const Bytes32 h = n->channelDeposit(lbmock::fromPtr(arguments->channel_id),
                                        lbmock::fromPtr(arguments->funding_public_key), arguments->amount, metadata,
                                        err);
    return hashResult<FfiChannelDepositResult>(h, err);
}

FfiChannelDepositResult channel_deposit_with_notes(const LogosBlockchainNode* node,
                                                   const ChannelDepositWithNotesArguments* arguments) {
    NODE_OR_RETURN(FfiChannelDepositResult)
    if (!arguments || !arguments->channel_id || !arguments->change_public_key || !arguments->funding_public_keys ||
        (!arguments->input_note_ids && arguments->input_note_ids_len) || (!arguments->metadata && arguments->metadata_len))
        return hashResult<FfiChannelDepositResult>({}, {lbmock::kNullPointer, "Received a null pointer."});
    std::vector<Bytes32> notes;
    for (size_t i = 0; i < arguments->input_note_ids_len; ++i)
        notes.push_back(lbmock::fromPtr(arguments->input_note_ids[i]));
    std::vector<uint8_t> metadata(arguments->metadata, arguments->metadata + arguments->metadata_len);
    Error err;
    const Bytes32 h = n->channelDepositWithNotes(
        lbmock::fromPtr(arguments->channel_id), notes, metadata, lbmock::fromPtr(arguments->change_public_key),
        keyList(arguments->funding_public_keys, arguments->funding_public_keys_len), arguments->max_tx_fee, err);
    return hashResult<FfiChannelDepositResult>(h, err);
}

FfiWalletFundResult wallet_fund_tx(const LogosBlockchainNode* node, const char* request_json) {
    NODE_OR_RETURN(FfiWalletFundResult)
    if (!request_json)
        return {nullptr, status(NullPointer, "Received a null `request_json` pointer.")};
    Error err;
    std::string out = n->fundTx(request_json, err);
    if (err)
        return {nullptr, status(err)};
    return {ownedString(out), ok()};
}

FfiSubmitTransactionResult submit_signed_transaction(const LogosBlockchainNode* node, const char* signed_tx_json) {
    NODE_OR_RETURN(FfiSubmitTransactionResult)
    if (!signed_tx_json)
        return hashResult<FfiSubmitTransactionResult>({}, {lbmock::kNullPointer, "Received a null pointer."});
    Error err;
    const Bytes32 h = n->submitSigned(signed_tx_json, err);
    return hashResult<FfiSubmitTransactionResult>(h, err);
}

FfiLeaderClaimResult leader_claim(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiLeaderClaimResult)
    Error err;
    const Bytes32 h = n->leaderClaim(err);
    return hashResult<FfiLeaderClaimResult>(h, err);
}

// ---- PoW ------------------------------------------------------------------------

OperationStatus pow_start_mining(const LogosBlockchainNode* node) {
    guard(__func__);
    Node* n = sim(node);
    if (!n)
        return nullNode();
    n->powSetMining(true);
    return ok();
}

OperationStatus pow_stop_mining(const LogosBlockchainNode* node) {
    guard(__func__);
    Node* n = sim(node);
    if (!n)
        return nullNode();
    n->powSetMining(false);
    return ok();
}

OperationStatus pow_start_auto_claim(const LogosBlockchainNode* node) {
    guard(__func__);
    Node* n = sim(node);
    if (!n)
        return nullNode();
    n->powSetAutoClaim(true);
    return ok();
}

OperationStatus pow_stop_auto_claim(const LogosBlockchainNode* node) {
    guard(__func__);
    Node* n = sim(node);
    if (!n)
        return nullNode();
    n->powSetAutoClaim(false);
    return ok();
}

FfiPoWClaimResult pow_claim(const LogosBlockchainNode* node, const uint8_t* claim_address) {
    NODE_OR_RETURN(FfiPoWClaimResult)
    Error err;
    Bytes32 address{};
    if (claim_address)
        address = lbmock::fromPtr(claim_address);
    const Bytes32 h = n->powClaim(claim_address ? &address : nullptr, err);
    return hashResult<FfiPoWClaimResult>(h, err);
}

FfiPoWClaimableRewardsResult pow_claimable_rewards(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiPoWClaimableRewardsResult)
    Error err;
    const std::vector<uint64_t> expiry = n->powClaimable(err);
    FfiPoWClaimableRewardsResult r{};
    if (err) {
        r.error = status(err);
        return r;
    }
    r.value.slots_until_expiry = new uint64_t[expiry.size() + 1];
    for (size_t i = 0; i < expiry.size(); ++i)
        r.value.slots_until_expiry[i] = expiry[i];
    r.value.len = expiry.size();
    r.value.claimable_tickets = expiry.size();
    r.error = ok();
    return r;
}

OperationStatus free_pow_claimable_rewards(PoWClaimableRewards rewards) {
    if (!rewards.slots_until_expiry)
        return status(NullPointer, "Received a null pointer.");
    delete[] rewards.slots_until_expiry;
    return ok();
}

FfiPoWStatusResult pow_status(const LogosBlockchainNode* node) {
    NODE_OR_RETURN(FfiPoWStatusResult)
    Error err;
    const lbmock::PowStatusView v = n->powStatus(err);
    FfiPoWStatusResult r{};
    if (err) {
        r.error = status(err);
        return r;
    }
    r.value.is_mining = v.mining;
    r.value.are_rewards_enabled = true;
    r.value.auto_claim.is_armed = v.armed;
    r.value.auto_claim.tick = v.tick;
    r.value.auto_claim.tick_unit = v.tickInSlots ? Slots : Seconds;
    r.value.auto_claim.targets = new PoWClaimTargetStatus[v.targets.size() + 1];
    r.value.auto_claim.targets_len = v.targets.size();
    for (size_t i = 0; i < v.targets.size(); ++i) {
        std::memcpy(r.value.auto_claim.targets[i].public_key, v.targets[i].pk.data(), 32);
        r.value.auto_claim.targets[i].threshold = v.targets[i].threshold;
        r.value.auto_claim.targets[i].balance = {true, v.targets[i].balance};
    }
    r.error = ok();
    return r;
}

OperationStatus free_pow_status(PoWStatus s) {
    delete[] s.auto_claim.targets;
    return ok();
}

} // extern "C"
