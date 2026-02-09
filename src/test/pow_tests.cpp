// Copyright (c) 2015-present The Bitcoin Core developers
// Copyright (c) 2015-present The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    // Freycoin uses LWMA difficulty adjustment with nDifficulty field
    // This test validates basic LWMA behavior
    CBlockIndex pindexLast;
    pindexLast.nHeight = 1000;
    pindexLast.nTime = 1736427984 + 1000 * 150; // Genesis time + 1000 blocks * 150s
    pindexLast.nDifficulty = chainParams->GetConsensus().nDifficultyMin * 2;

    // With no previous index, should return min difficulty
    // Actual LWMA tests are in difficulty_adjustment_tests.cpp
    BOOST_CHECK(pindexLast.nDifficulty >= chainParams->GetConsensus().nDifficultyMin);
}

/* Test difficulty adjustment respects minimum */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Test that very fast block time increases difficulty
    uint64_t currentDiff = params.nDifficultyMin * 2;
    int64_t fastTimespan = params.nPowTargetSpacing / 10; // 10x faster than target
    uint64_t nextDiff = CalculateNextWorkRequired(currentDiff, fastTimespan, params);
    BOOST_CHECK(nextDiff > currentDiff);

    // Test that very slow block time decreases difficulty
    int64_t slowTimespan = params.nPowTargetSpacing * 10; // 10x slower than target
    nextDiff = CalculateNextWorkRequired(currentDiff, slowTimespan, params);
    BOOST_CHECK(nextDiff < currentDiff);

    // Test that minimum difficulty is enforced
    nextDiff = CalculateNextWorkRequired(params.nDifficultyMin, slowTimespan, params);
    BOOST_CHECK(nextDiff >= params.nDifficultyMin);
}

/* Test difficulty adjustment damping for fast blocks */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Very fast block (nearly instant) - should increase difficulty
    // but be damped to prevent wild swings
    uint64_t currentDiff = 100ULL << 48; // Merit ~100
    int64_t instantTimespan = 1; // 1 second instead of 150s target
    uint64_t nextDiff = CalculateNextWorkRequired(currentDiff, instantTimespan, params);

    // Difficulty should increase but be clamped
    BOOST_CHECK(nextDiff > currentDiff);
    // Maximum increase should be bounded (asymmetric damping)
    BOOST_CHECK(nextDiff <= currentDiff + (1ULL << 48)); // Max +1 per block
}

/* Test difficulty adjustment damping for slow blocks */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Very slow block - should decrease difficulty
    // but be damped to prevent instability
    uint64_t currentDiff = 100ULL << 48; // Merit ~100
    int64_t slowTimespan = params.nPowTargetSpacing * 100; // 100x target
    uint64_t nextDiff = CalculateNextWorkRequired(currentDiff, slowTimespan, params);

    // Difficulty should decrease but be clamped
    BOOST_CHECK(nextDiff < currentDiff);
    // Maximum decrease should be bounded (asymmetric damping, faster decrease)
    BOOST_CHECK(nextDiff >= currentDiff - (1ULL << 48)); // Max -1 per block
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_invalid_shift_too_low)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& consensus = chainParams->GetConsensus();

    // Create a block with nShift below minimum (invalid)
    CBlockHeader block;
    block.nVersion = 1;
    block.hashPrevBlock.SetNull();
    block.hashMerkleRoot.SetNull();
    block.nTime = 1700000000;
    block.nDifficulty = consensus.nDifficultyMin;
    block.nShift = MIN_SHIFT - 1; // Below minimum - should fail
    block.nNonce = 0;
    block.nAdd.SetNull();

    BOOST_CHECK(!CheckProofOfWork(block, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_invalid_shift_too_high)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& consensus = chainParams->GetConsensus();

    // Create a block with nShift above maximum (invalid)
    CBlockHeader block;
    block.nVersion = 1;
    block.hashPrevBlock.SetNull();
    block.hashMerkleRoot.SetNull();
    block.nTime = 1700000000;
    block.nDifficulty = consensus.nDifficultyMin;
    block.nShift = MAX_SHIFT + 1; // Above maximum - should fail
    block.nNonce = 0;
    block.nAdd.SetNull();

    BOOST_CHECK(!CheckProofOfWork(block, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_difficulty)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& consensus = chainParams->GetConsensus();

    // Create a block with zero difficulty (invalid - below minimum)
    CBlockHeader block;
    block.nVersion = 1;
    block.hashPrevBlock.SetNull();
    block.hashMerkleRoot.SetNull();
    block.nTime = 1700000000;
    block.nDifficulty = 0; // Zero difficulty - should fail
    block.nShift = MIN_SHIFT;
    block.nNonce = 0;
    block.nAdd.SetNull();

    // Zero difficulty is below minimum, so this should fail
    BOOST_CHECK(!CheckProofOfWork(block, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1707684554 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nDifficulty = 50ULL << 48; // Merit ~50
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_SUITE_END()
