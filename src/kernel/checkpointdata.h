// Copyright (c) 2025-present The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_KERNEL_CHECKPOINTDATA_H
#define FREYCOIN_KERNEL_CHECKPOINTDATA_H

#include <kernel/chainparams.h>

/**
 * Freycoin Checkpoint Data
 *
 * Checkpoints are empty for the new chain. They will be populated
 * after mainnet launch and sufficient chain growth.
 *
 * Format: CheckpointData {
 *   knownHeaderBatchesHashes: map of hash -> (start_height, batch_size)
 *   assumedValidBlockHash: hash of last known valid block
 *   assumedValidBlockHeight: height of that block
 * }
 */

static const CheckpointData mainCheckpointData = {
    .knownHeaderBatchesHashes = {},  // No checkpoints yet for new chain
    .assumedValidBlockHash = uint256{},
    .assumedValidBlockHeight = 0
};

static const CheckpointData testCheckpointData = {
    .knownHeaderBatchesHashes = {},  // No checkpoints yet for testnet
    .assumedValidBlockHash = uint256{},
    .assumedValidBlockHeight = 0
};

#endif // FREYCOIN_KERNEL_CHECKPOINTDATA_H
