// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2013-present The Freycoin developers
// Copyright (c) 2014-2017 Jonnie Frey (Gapcoin)
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POW_H
#define BITCOIN_POW_H

#include <consensus/params.h>
#include <cstdint>

class CBlockHeader;
class CBlockIndex;
class uint256;

/**
 * Check whether a block satisfies the proof-of-work requirement.
 *
 * Prime Gap PoW Validation:
 * 1. Construct starting prime: start = GetHash() * 2^nShift + nAdd
 * 2. Verify start is prime (BPSW primality test)
 * 3. Find next prime p2 after start
 * 4. Compute gap merit: gap_size / ln(start)
 * 5. Compute difficulty with randomness: merit + rand(start, p2) % (2/ln(start))
 * 6. Accept if achieved difficulty >= nDifficulty
 *
 * @param[in] block   The block header to validate
 * @param[in] params  Consensus parameters
 * @return true if the proof-of-work is valid
 */
bool CheckProofOfWork(const CBlockHeader& block, const Consensus::Params& params);

/**
 * Calculate the next required difficulty for the block following pindexLast.
 *
 * Uses logarithmic difficulty adjustment:
 *   next = current + log(target_spacing / actual_spacing)
 *
 * With damping: increases at 1/256 rate, decreases at 1/64 rate.
 * Clamped to ±1 per block to prevent instability.
 *
 * @param[in] pindexLast  The last block in the chain
 * @param[in] params      Consensus parameters
 * @return The required nDifficulty for the next block
 */
uint64_t GetNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params);

/**
 * Calculate next difficulty given previous difficulty and solve time.
 *
 * @param[in] nDifficulty     Previous block's difficulty
 * @param[in] nActualTimespan Time between prev and prev-prev blocks
 * @param[in] params          Consensus parameters
 * @return The required nDifficulty for the next block
 */
uint64_t CalculateNextWorkRequired(uint64_t nDifficulty, int64_t nActualTimespan, const Consensus::Params& params);

/**
 * Minimum difficulty constant (merit ~16).
 * Corresponds to gaps achievable by anyone with basic hardware.
 */
constexpr uint64_t MIN_DIFFICULTY = 16ULL << 48;

/**
 * Maximum shift for starting prime construction.
 * Limits starting prime size to hash * 2^256, preventing DoS via huge numbers.
 */
constexpr uint16_t MAX_SHIFT = 256;

/**
 * Minimum shift for starting prime construction.
 * Ensures starting primes are large enough for meaningful gaps.
 */
constexpr uint16_t MIN_SHIFT = 14;

#endif // BITCOIN_POW_H
