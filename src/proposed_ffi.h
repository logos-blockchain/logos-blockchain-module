#pragma once

// C bindings proposed in logos-blockchain-module#108 that liblogos_blockchain
// doesn't have yet. Only the mock variant defines LOGOS_BLOCKCHAIN_PROPOSED_FFI
// and implements them (mock/src/mock_ffi.cpp). Delete each declaration once the
// node's header ships the real one.
//
// Include after logos_blockchain.h, which has no include guard.

#ifdef __cplusplus
extern "C" {
#endif

FfiStatusResult_____c_char blend_status(const struct LogosBlockchainNode* node);
FfiStatusResult_____c_char blend_reachability(const struct LogosBlockchainNode* node);
// Needs no running node: reads the deployment (NULL = the built-in one).
FfiStatusResult_____c_char blend_requirements(const char* custom_deployment_path);
// Withdraws this node's own Blend declaration (the node HTTP API's /sdp/withdrawal).
FfiStatusResult_____c_char blend_withdraw(const struct LogosBlockchainNode* node);
#ifdef __cplusplus
}
#endif
