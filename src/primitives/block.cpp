// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Copyright (c) 2013-2021 The Freycoin developers
// Copyright (c) 2014-2017 Jonnie Frey (Gapcoin)
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <tinyformat.h>

/**
 * Compute the block hash from consensus fields only (84 bytes).
 *
 * The hash excludes proof fields (nShift, nAdd, nReserved) so that miners
 * can iterate these values without changing the hash. The hash becomes
 * the "puzzle", and the proof fields are the "solution" that constructs
 * a prime gap meeting the difficulty target.
 *
 * Layout hashed:
 *   nVersion        4 bytes
 *   hashPrevBlock  32 bytes
 *   hashMerkleRoot 32 bytes
 *   nTime           4 bytes
 *   nDifficulty     8 bytes
 *   nNonce          4 bytes
 *   Total:         84 bytes
 */
uint256 CBlockHeader::GetHash() const
{
    // Manually construct the 84-byte consensus data
    // This ensures the hash is independent of serialization order
    uint8_t data[84];
    size_t pos = 0;

    // nVersion (4 bytes, little-endian)
    memcpy(&data[pos], &nVersion, 4);
    pos += 4;

    // hashPrevBlock (32 bytes)
    memcpy(&data[pos], hashPrevBlock.begin(), 32);
    pos += 32;

    // hashMerkleRoot (32 bytes)
    memcpy(&data[pos], hashMerkleRoot.begin(), 32);
    pos += 32;

    // nTime (4 bytes, little-endian)
    memcpy(&data[pos], &nTime, 4);
    pos += 4;

    // nDifficulty (8 bytes, little-endian)
    memcpy(&data[pos], &nDifficulty, 8);
    pos += 8;

    // nNonce (4 bytes, little-endian)
    memcpy(&data[pos], &nNonce, 4);
    pos += 4;

    // Double SHA256
    return Hash(data);
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, "
                   "nTime=%u, nDifficulty=%llu, nNonce=%u, nShift=%u, nAdd=%s, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime,
        static_cast<unsigned long long>(nDifficulty),
        nNonce,
        nShift,
        nAdd.ToString(),
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
