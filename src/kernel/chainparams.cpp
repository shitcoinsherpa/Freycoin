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

        // Genesis block — Jonnie Frey proved PoW can advance human knowledge
        const char* pszTimestamp = "Jonnie Frey (1989-2017) proved PoW can advance human knowledge";
        const CScript genesisOutputScript = CScript() << OP_RETURN; // Unspendable

        genesis = CreateGenesisBlock(
            pszTimestamp,
            genesisOutputScript,
            1770592944,       // 2026-02-09: Freycoin mainnet genesis
            MIN_DIFFICULTY,   // Initial difficulty
            0,                // nNonce (will be mined)
            14,               // nShift (minimum)
            uint256{},        // nAdd (empty for fresh genesis)
            1,                // nVersion
            0                 // No coinbase reward for genesis
        );

        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"72aa06e01904c931e7554bd1c50a30978004c0f7e9a74c3b314f8ba1493e1148"});
        assert(genesis.hashMerkleRoot == uint256{"87925d0a69a0e00b2aab9512c4771fad6d918d370bf8bab5fd951789b20ddd29"});

        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "frey";

        vSeeds.emplace_back("seed1.freycoin.tech");
        vSeeds.emplace_back("seed2.freycoin.tech");
        vSeeds.emplace_back("seed3.freycoin.tech");
        vFixedSeeds.clear();  // Until VPS IPs are hardcoded

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

        // Prime gap PoW parameters - merit ~1 minimum for practical CPU testnet mining
        consensus.nDifficultyMin = 1ULL << 48;
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
        nDefaultPort = 31473;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // Genesis block for testnet — same memorial timestamp as mainnet
        const char* pszTimestamp = "Freycoin Testnet - In memory of Jonnie Frey";
        const CScript genesisOutputScript = CScript() << OP_RETURN;

        genesis = CreateGenesisBlock(
            pszTimestamp,
            genesisOutputScript,
            1770592944,       // 2026-02-09: Freycoin testnet genesis (same timestamp as mainnet)
            1ULL << 48,       // Initial difficulty (merit ~1 for testnet)
            0,                // nNonce
            14,               // nShift (minimum)
            uint256{},        // nAdd
            1,                // nVersion
            50 * COIN         // 50 FREY coinbase
        );

        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"88640a81286fe4c3c1d17b113ff4efa656a3e4e345c26286bebe3b66f8579ad8"});
        assert(genesis.hashMerkleRoot == uint256{"3d88cf475c00c0a831fb98c7816aa8ad8dae0edcaca6d012cbb4cef3bc6402d5"});

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.emplace_back("testseed1.freycoin.tech");
        vSeeds.emplace_back("testseed2.freycoin.tech");

        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tfrey";

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

        // Regtest genesis
        const char* pszTimestamp = "Freycoin Regtest";
        const CScript genesisOutputScript = CScript() << OP_RETURN;

        genesis = CreateGenesisBlock(
            pszTimestamp,
            genesisOutputScript,
            1770499200,       // 2026-02-08: Freycoin fresh genesis
            consensus.nDifficultyMin,
            0,                // nNonce
            14,               // Minimum nShift
            uint256{},        // nAdd
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
