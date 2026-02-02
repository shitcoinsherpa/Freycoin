// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2013-2023 The Freycoin developers
// Copyright (c) 2014-2017 Jonnie Frey (Gapcoin)
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/time.h>

/**
 * Freycoin Block Header (120 bytes)
 *
 * Proof-of-Work: Prime Gap Mining
 * Miners search for prime gaps (consecutive primes far apart).
 * The starting prime is: hash * 2^nShift + nAdd
 * Difficulty is based on gap merit: gap_size / ln(start_prime)
 *
 * Layout:
 *   Consensus fields (hashed, 84 bytes):
 *     nVersion        4B   Block version
 *     hashPrevBlock  32B   Previous block hash
 *     hashMerkleRoot 32B   Merkle root of transactions
 *     nTime           4B   Unix timestamp
 *     nDifficulty     8B   Target difficulty (2^48 fixed-point merit)
 *     nNonce          4B   Miner-iterated nonce
 *
 *   Proof fields (not hashed, 36 bytes):
 *     nShift          2B   Left-shift amount for starting prime
 *     nAdd           32B   Adder to construct starting prime
 *     nReserved       2B   Reserved for future use
 */
class CBlockHeader
{
public:
    // Consensus fields (included in GetHash())
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint64_t nDifficulty;
    uint32_t nNonce;

    // Proof fields (excluded from GetHash())
    uint16_t nShift;
    uint256 nAdd;
    uint16_t nReserved;

    CBlockHeader()
    {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj)
    {
        READWRITE(
            obj.nVersion,
            obj.hashPrevBlock,
            obj.hashMerkleRoot,
            obj.nTime,
            obj.nDifficulty,
            obj.nNonce,
            obj.nShift,
            obj.nAdd,
            obj.nReserved
        );
    }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nDifficulty = 0;
        nNonce = 0;
        nShift = 0;
        nAdd.SetNull();
        nReserved = 0;
    }

    bool IsNull() const
    {
        return (nDifficulty == 0);
    }

    /** Compute block hash (consensus fields only, 84 bytes) */
    uint256 GetHash() const;

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return static_cast<int64_t>(nTime);
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    // Memory-only flags for caching expensive checks
    mutable bool fChecked;                            // CheckBlock()
    mutable bool m_checked_witness_commitment{false}; // CheckWitnessCommitment()
    mutable bool m_checked_merkle_root{false};        // CheckMerkleRoot()

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITE(AsBase<CBlockHeader>(obj), obj.vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
        m_checked_witness_commitment = false;
        m_checked_merkle_root = false;
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion       = nVersion;
        block.hashPrevBlock  = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nDifficulty    = nDifficulty;
        block.nNonce         = nNonce;
        block.nShift         = nShift;
        block.nAdd           = nAdd;
        block.nReserved      = nReserved;
        return block;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    /** Historically CBlockLocator's version field has been written to network
     * streams as the negotiated protocol version and to disk streams as the
     * client version, but the value has never been used.
     *
     * Hard-code to the highest protocol version ever written to a network stream.
     * SerParams can be used if the field requires any meaning in the future,
     **/
    static constexpr int DUMMY_VERSION = 70016;

    std::vector<uint256> vHave;

    CBlockLocator() = default;

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = DUMMY_VERSION;
        READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
