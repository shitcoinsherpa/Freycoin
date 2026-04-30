// Copyright (c) 2025-present The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_KERNEL_CHECKPOINTDATA_H
#define FREYCOIN_KERNEL_CHECKPOINTDATA_H

#include <kernel/chainparams.h>

/**
 * Freycoin Checkpoint Data
 *
 * Each entry maps Hash(serialized_2000_headers) to (start_height, batch_size).
 * During initial sync, peers must feed full 2000-header batches whose hash
 * matches one of these entries; matching batches skip per-header PoW (which
 * costs ~30 s/header at mainnet shift). Above assumedValidBlockHeight,
 * normal headers-first sync applies.
 *
 * REGENERATE THIS FILE EACH RELEASE via scripts/extract_header_batches.py.
 */

static const CheckpointData mainCheckpointData = {
    .knownHeaderBatchesHashes = {
        { uint256{"84533176f53a9a81b3db7df34b44c4573b873d72a63182ecb0e362de4f6faa40"}, { 1, 2000 } },
        { uint256{"f638adca1bc61e8cc8775edaa496fa4ea5fedc2cef0d074be8c0e26e6823a248"}, { 2001, 2000 } },
        { uint256{"1d96cf4d3e2c3e267f7d47972c14f25187d8ad5f436dc6164972f61db5499cdb"}, { 4001, 2000 } },
        { uint256{"45facb5e3c9380b10abd81f9ad4e7ad35e886cb55e2bf0f59e65a6f1a617eace"}, { 6001, 2000 } },
        { uint256{"4e32ca39f1477103d9c52837afaab4792a6da4db0a7170280b275b11f3db6e0d"}, { 8001, 2000 } },
    },
    .assumedValidBlockHash = uint256{"dd21ddd60e659effb5aefa3b0d9d7d5b861d9116205ddb66db264cc61cfb84af"},
    .assumedValidBlockHeight = 10000
};

static const CheckpointData testCheckpointData = {
    .knownHeaderBatchesHashes = {},  // testnet — refresh separately when needed
    .assumedValidBlockHash = uint256{},
    .assumedValidBlockHeight = 0
};

#endif // FREYCOIN_KERNEL_CHECKPOINTDATA_H
