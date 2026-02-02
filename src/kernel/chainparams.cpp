// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2013-present The Freycoin developers
// Copyright (c) 2014-2017 Jonnie Frey (Gapcoin)
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <kernel/checkpointdata.h>
#include <kernel/messagestartchars.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>

using namespace util::hex_literals;

/**
 * Build the genesis block for prime gap PoW.
 *
 * Genesis blocks are special - they don't require valid PoW since there's no
 * previous block to validate against. The nDifficulty sets the initial target
 * for the next block.
 */
static CBlock CreateGenesisBlock(const char* pszTimestamp,
                                  const CScript& genesisOutputScript,
                                  uint32_t nTime,
                                  uint64_t nDifficulty,
                                  uint32_t nNonce,
                                  uint16_t nShift,
                                  const uint256& nAdd,
                                  int32_t nVersion,
                                  const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4)
        << std::vector<unsigned char>((const unsigned char*)pszTimestamp,
                                       (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nVersion     = nVersion;
    genesis.nTime        = nTime;
    genesis.nDifficulty  = nDifficulty;
    genesis.nNonce       = nNonce;
    genesis.nShift       = nShift;
    genesis.nAdd         = nAdd;
    genesis.nReserved    = 0;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Minimum difficulty constant.
 * Merit ~16 is achievable by anyone with basic hardware in reasonable time.
 * 16 * 2^48 = 0x0010_0000_0000_0000
 */
static constexpr uint64_t MIN_DIFFICULTY = 16ULL << 48;

/** Main network: Freycoin mainnet (NOT YET LAUNCHED) */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        m_chain_type = ChainType::MAIN;

        // Subsidy halves every 840,000 blocks (~4 years at 150s blocks)
        consensus.nSubsidyHalvingInterval = 840000;
        consensus.MinBIP9WarningHeight = 0;

        // Prime gap PoW parameters
        consensus.nDifficultyMin = MIN_DIFFICULTY;
        consensus.nPowTargetSpacing = 150; // 2.5 minutes
        consensus.fPowNoRetargeting = false;

        // BIP9 deployments
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 3024; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 4032;

        consensus.nMinimumChainWork = uint256{}; // Will be set after mainnet launch

        // Network message magic (Freycoin mainnet)
        pchMessageStart[0] = 0xf7;
        pchMessageStart[1] = 0xe9;
        pchMessageStart[2] = 0xc3;
        pchMessageStart[3] = 0xa1;
        nDefaultPort = 31470;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // Genesis block - In memory of Jonnie Frey
        const char* pszTimestamp = "In memory of Jonnie Frey (1989-2017) - Prime gaps advance human knowledge";
        const CScript genesisOutputScript = CScript() << OP_RETURN; // Unspendable

        // Genesis parameters from Gapcoin-Revival mining
        // nonce=2039, adder={247,35,12}, shift=20, difficulty=min, merit ~20.2
        uint256 genesisAdd;
        genesisAdd.SetNull();
        // adder = 0x0C23F7 in little-endian
        unsigned char* add_data = genesisAdd.data();
        add_data[0] = 0xF7;
        add_data[1] = 0x23;
        add_data[2] = 0x0C;

        genesis = CreateGenesisBlock(
            pszTimestamp,
            genesisOutputScript,
            1736427984,       // 2026-01-09 13:06:24 UTC
            MIN_DIFFICULTY,   // Initial difficulty
            2039,             // nNonce (mined)
            20,               // nShift
            genesisAdd,       // nAdd
            1,                // nVersion
            0                 // No coinbase reward for genesis
        );

        consensus.hashGenesisBlock = genesis.GetHash();

        // TODO: Calculate and verify these hashes after finalization
        // assert(consensus.hashGenesisBlock == uint256{"..."});
        // assert(genesis.hashMerkleRoot == uint256{"..."});

        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "frey";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_main), std::end(chainparams_seed_main));

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        checkpointData = mainCheckpointData;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0.0,
        };
    }
};

/** Testnet: public test network for development and testing */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        m_chain_type = ChainType::TESTNET;

        consensus.nSubsidyHalvingInterval = 840000;
        consensus.MinBIP9WarningHeight = 0;

        // Prime gap PoW parameters - lower difficulty for testing
        consensus.nDifficultyMin = MIN_DIFFICULTY;
        consensus.nPowTargetSpacing = 150; // 2.5 minutes
        consensus.fPowNoRetargeting = false;

        // BIP9 deployments
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        consensus.nMinimumChainWork = uint256{};

        // Network message magic (Freycoin testnet)
        pchMessageStart[0] = 0x0f;
        pchMessageStart[1] = 0x7e;
        pchMessageStart[2] = 0x9c;
        pchMessageStart[3] = 0x3a;
        nDefaultPort = 31471;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // Genesis block for testnet
        const char* pszTimestamp = "Freycoin Testnet - Prime Gap Mining";
        const CScript genesisOutputScript = CScript() << OP_RETURN;

        // Testnet genesis from Gapcoin-Revival: nonce=12, adder={95}, merit ~22.4
        uint256 genesisAdd;
        genesisAdd.SetNull();
        genesisAdd.data()[0] = 0x5F; // 95

        genesis = CreateGenesisBlock(
            pszTimestamp,
            genesisOutputScript,
            1736528612,       // 2026-01-10 17:03:32 UTC
            MIN_DIFFICULTY,   // Initial difficulty
            12,               // nNonce (mined)
            20,               // nShift
            genesisAdd,       // nAdd
            1,                // nVersion
            50 * COIN         // 50 FREY coinbase
        );

        consensus.hashGenesisBlock = genesis.GetHash();

        // TODO: Calculate and verify these hashes
        // assert(consensus.hashGenesisBlock == uint256{"..."});

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tfrey";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_test), std::end(chainparams_seed_test));

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        checkpointData = testCheckpointData;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0.0,
        };
    }
};

/** Regtest: private regression testing network with minimal difficulty */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;

        consensus.nSubsidyHalvingInterval = 150;
        consensus.MinBIP9WarningHeight = 0;

        // Regtest uses very low difficulty for instant block generation
        // Merit ~1 means any gap is acceptable
        consensus.nDifficultyMin = 1ULL << 48;
        consensus.nPowTargetSpacing = 150;
        consensus.fPowNoRetargeting = true; // No difficulty adjustment

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144;

        for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
            consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
            consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
            consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
        }

        consensus.nMinimumChainWork = uint256{};

        // Network message magic (Freycoin regtest)
        pchMessageStart[0] = 0xfc;
        pchMessageStart[1] = 0x7e;
        pchMessageStart[2] = 0x9c;
        pchMessageStart[3] = 0x01;
        nDefaultPort = 18444;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        // Regtest genesis - simple, instantly mineable
        const char* pszTimestamp = "Freycoin Regtest";
        const CScript genesisOutputScript = CScript() << OP_RETURN;
        uint256 genesisAdd;
        genesisAdd.SetNull();

        genesis = CreateGenesisBlock(
            pszTimestamp,
            genesisOutputScript,
            1707684554,
            consensus.nDifficultyMin,
            0,                // nNonce (stub PoW accepts all)
            14,               // Minimum nShift
            genesisAdd,
            1,
            50 * COIN
        );

        consensus.hashGenesisBlock = genesis.GetHash();

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {},
            uint256{}, 0
        };

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001,
        };

        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "rfrey";
    }
};

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<const CMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet()
{
    return std::make_unique<const CTestNetParams>();
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest({})->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    }
    return std::nullopt;
}
