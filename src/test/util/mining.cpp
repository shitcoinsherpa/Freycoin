// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2013-present The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/mining.h>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <key_io.h>
#include <node/context.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <test/util/script.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbits.h>

#include <gmp.h>
#include <algorithm>
#include <memory>

using node::BlockAssembler;
using node::NodeContext;

/**
 * Find a valid prime gap PoW for a block.
 *
 * For each nonce, compute hash and search for an adder that makes
 * start = hash * 2^shift + adder a prime. With regtest's low difficulty,
 * almost any prime gap will satisfy the difficulty requirement.
 *
 * @param block Block to mine (nNonce, nShift, nAdd will be set)
 * @param params Consensus parameters
 * @return true if valid PoW found
 */
bool FindValidPoW(CBlock& block, const Consensus::Params& params)
{
    // Start with reasonable defaults
    block.nShift = 20;  // Small shift for faster testing
    block.nAdd.SetNull();

    mpz_t mpz_hash, mpz_base, mpz_start, mpz_adder;
    mpz_init(mpz_hash);
    mpz_init(mpz_base);
    mpz_init(mpz_start);
    mpz_init(mpz_adder);

    // Try nonces until we find valid PoW
    for (uint32_t nonce = 0; nonce < 1000000; ++nonce) {
        block.nNonce = nonce;

        // Get block hash and convert to mpz
        uint256 hash = block.GetHash();
        mpz_import(mpz_hash, 32, -1, 1, -1, 0, hash.begin());

        // base = hash * 2^shift
        mpz_mul_2exp(mpz_base, mpz_hash, block.nShift);

        // Search for an adder that makes start prime
        // Start with small adders for efficiency
        for (uint64_t adder = 1; adder < (1ULL << block.nShift); adder += 2) {
            // start = base + adder
            mpz_set_ui(mpz_adder, adder);
            mpz_add(mpz_start, mpz_base, mpz_adder);

            // Check if start is prime
            if (mpz_probab_prime_p(mpz_start, 25) > 0) {
                // Found a prime! Set the adder in the block
                block.nAdd.SetNull();
                // Store adder in little-endian format using data() pointer
                unsigned char* add_data = block.nAdd.data();
                if (adder <= 0xFF) {
                    add_data[0] = static_cast<uint8_t>(adder);
                } else if (adder <= 0xFFFF) {
                    add_data[0] = static_cast<uint8_t>(adder);
                    add_data[1] = static_cast<uint8_t>(adder >> 8);
                } else if (adder <= 0xFFFFFF) {
                    add_data[0] = static_cast<uint8_t>(adder);
                    add_data[1] = static_cast<uint8_t>(adder >> 8);
                    add_data[2] = static_cast<uint8_t>(adder >> 16);
                } else {
                    // For larger adders, use mpz export
                    size_t count;
                    mpz_export(add_data, &count, -1, 1, -1, 0, mpz_adder);
                }

                // Verify the PoW is valid
                if (CheckProofOfWork(block, params)) {
                    mpz_clear(mpz_hash);
                    mpz_clear(mpz_base);
                    mpz_clear(mpz_start);
                    mpz_clear(mpz_adder);
                    return true;
                }
            }
        }
    }

    mpz_clear(mpz_hash);
    mpz_clear(mpz_base);
    mpz_clear(mpz_start);
    mpz_clear(mpz_adder);

    return false;
}

COutPoint generatetoaddress(const NodeContext& node, const std::string& address)
{
    const auto dest = DecodeDestination(address);
    assert(IsValidDestination(dest));
    BlockAssembler::Options assembler_options;
    assembler_options.coinbase_output_script = GetScriptForDestination(dest);

    return MineBlock(node, assembler_options);
}

std::vector<std::shared_ptr<CBlock>> CreateBlockChain(size_t total_height, const CChainParams& params)
{
    std::vector<std::shared_ptr<CBlock>> ret{total_height};
    auto time{params.GenesisBlock().nTime};

    for (size_t height{0}; height < total_height; ++height) {
        CBlock& block{*(ret.at(height) = std::make_shared<CBlock>())};

        CMutableTransaction coinbase_tx;
        coinbase_tx.nLockTime = static_cast<uint32_t>(height);
        coinbase_tx.vin.resize(1);
        coinbase_tx.vin[0].prevout.SetNull();
        coinbase_tx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
        coinbase_tx.vout.resize(1);
        coinbase_tx.vout[0].scriptPubKey = P2WSH_OP_TRUE;
        coinbase_tx.vout[0].nValue = GetBlockSubsidy(height + 1, params.GetConsensus());
        coinbase_tx.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        block.vtx = {MakeTransactionRef(std::move(coinbase_tx))};

        block.nVersion = VERSIONBITS_LAST_OLD_BLOCK_VERSION;
        block.hashPrevBlock = (height >= 1 ? *ret.at(height - 1) : params.GenesisBlock()).GetHash();
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nTime = ++time;
        block.nDifficulty = params.GenesisBlock().nDifficulty;

        // Find valid PoW
        bool found = FindValidPoW(block, params.GetConsensus());
        assert(found);
    }
    return ret;
}

COutPoint MineBlock(const NodeContext& node, const node::BlockAssembler::Options& assembler_options)
{
    auto block = PrepareBlock(node, assembler_options);
    auto valid = MineBlock(node, block);
    assert(!valid.IsNull());
    return valid;
}

struct BlockValidationStateCatcher : public CValidationInterface {
    const uint256 m_hash;
    std::optional<BlockValidationState> m_state;

    BlockValidationStateCatcher(const uint256& hash)
        : m_hash{hash},
          m_state{} {}

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        if (block->GetHash() != m_hash) return;
        m_state = state;
    }
};

COutPoint MineBlock(const NodeContext& node, std::shared_ptr<CBlock>& block)
{
    // Find valid prime gap PoW
    bool found = FindValidPoW(*block, Assert(node.chainman)->GetConsensus());
    if (!found) {
        return {};
    }

    return ProcessBlock(node, block);
}

COutPoint ProcessBlock(const NodeContext& node, const std::shared_ptr<CBlock>& block)
{
    auto& chainman{*Assert(node.chainman)};
    const auto old_height = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight());
    bool new_block;
    BlockValidationStateCatcher bvsc{block->GetHash()};
    node.validation_signals->RegisterValidationInterface(&bvsc);
    const bool processed{chainman.ProcessNewBlock(block, true, &new_block)};
    const bool duplicate{!new_block && processed};
    assert(!duplicate);
    node.validation_signals->UnregisterValidationInterface(&bvsc);
    node.validation_signals->SyncWithValidationInterfaceQueue();
    const bool was_valid{bvsc.m_state && bvsc.m_state->IsValid()};
    assert(old_height + was_valid == WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight()));

    if (was_valid) return {block->vtx[0]->GetHash(), 0};
    return {};
}

std::shared_ptr<CBlock> PrepareBlock(const NodeContext& node,
                                     const BlockAssembler::Options& assembler_options)
{
    auto block = std::make_shared<CBlock>(
        BlockAssembler{Assert(node.chainman)->ActiveChainstate(), Assert(node.mempool.get()), assembler_options}
            .CreateNewBlock()
            ->block);

    LOCK(cs_main);
    block->nTime = Assert(node.chainman)->ActiveChain().Tip()->GetMedianTimePast() + 1;
    block->hashMerkleRoot = BlockMerkleRoot(*block);

    return block;
}

std::shared_ptr<CBlock> PrepareBlock(const NodeContext& node, const CScript& coinbase_scriptPubKey)
{
    BlockAssembler::Options assembler_options;
    assembler_options.coinbase_output_script = coinbase_scriptPubKey;
    ApplyArgsManOptions(*node.args, assembler_options);
    return PrepareBlock(node, assembler_options);
}
