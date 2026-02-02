// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * PoW class representing a prime gap proof-of-work solution.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_POW_H
#define FREYCOIN_POW_POW_H

#include <pow/pow_common.h>
#include <pow/pow_utils.h>
#include <cstdint>
#include <string>
#include <vector>
#include <gmp.h>

/**
 * Represents a prime gap proof-of-work.
 *
 * A valid PoW consists of:
 *   - hash: The block header hash (76 bytes, from GetHash())
 *   - shift: Left-shift amount (14-256)
 *   - adder: Value added after shift to make start prime
 *   - nonce: 32-bit nonce that produced the hash
 *
 * The prime gap is:
 *   start = hash * 2^shift + adder  (must be prime)
 *   end = next_prime(start)
 *   gap = end - start
 *   merit = gap / ln(start)
 *
 * The PoW is valid if:
 *   difficulty(start, end) >= target_difficulty
 */
class PoW {
public:
    /**
     * Create PoW from computation format (mpz values).
     *
     * @param mpz_hash Block header hash as mpz
     * @param shift Left-shift amount
     * @param mpz_adder Adder value as mpz
     * @param difficulty Target difficulty (2^48 fixed-point)
     * @param nonce Block nonce
     */
    PoW(mpz_t mpz_hash, uint16_t shift, mpz_t mpz_adder,
        uint64_t difficulty, uint32_t nonce = 0);

    /**
     * Create PoW from block header format (byte arrays).
     *
     * @param hash Block header hash (32 bytes, little-endian)
     * @param shift Left-shift amount
     * @param adder Adder value (up to 32 bytes, little-endian)
     * @param difficulty Target difficulty
     * @param nonce Block nonce
     */
    PoW(const std::vector<uint8_t>& hash, uint16_t shift,
        const std::vector<uint8_t>& adder, uint64_t difficulty,
        uint32_t nonce = 0);

    ~PoW();

    // Non-copyable (holds GMP state)
    PoW(const PoW&) = delete;
    PoW& operator=(const PoW&) = delete;

    /**
     * Get the achieved difficulty of this PoW.
     * @return Difficulty * 2^48 (fixed-point)
     */
    uint64_t difficulty();

    /**
     * Get the merit of this PoW.
     * @return Merit * 2^48 (fixed-point)
     */
    uint64_t merit();

    /**
     * Get the prime gap endpoints.
     * @param start Output: start prime (caller must mpz_init)
     * @param end Output: end prime (next prime after start)
     * @return true if gap was computed successfully
     */
    bool get_gap(std::vector<uint8_t>& start, std::vector<uint8_t>& end);

    /**
     * Get the gap length.
     * @return Gap length (end - start)
     */
    uint64_t gap_len();

    /**
     * Check if this PoW is valid.
     * @return true if achieved difficulty >= target difficulty
     */
    bool valid();

    /**
     * Get target gap size for difficulty.
     * @param mpz_start Start prime
     * @return Target gap size
     */
    uint64_t target_size(mpz_t mpz_start);

    /**
     * Get string representation for debugging.
     */
    std::string to_string();

    // Getters
    void get_hash(mpz_t result);
    uint16_t get_shift() const { return shift; }
    uint32_t get_nonce() const { return nonce; }
    void get_adder(mpz_t result);
    void get_adder(std::vector<uint8_t>& result);
    uint64_t get_target() const { return target_difficulty; }

    // Setters
    void set_shift(uint16_t s) { shift = s; }
    void set_adder(mpz_t mpz_adder);
    void set_adder(const std::vector<uint8_t>& adder);

private:
    mpz_t mpz_hash;           // Block header hash
    uint32_t nonce;           // Block nonce
    uint16_t shift;           // Shift amount
    mpz_t mpz_adder;          // Adder value
    uint64_t target_difficulty;  // Target difficulty

    PoWUtils* utils;          // Utility functions

    /**
     * Calculate start and end primes for this PoW.
     * @param mpz_start Output: start prime
     * @param mpz_end Output: end prime
     * @return true if valid prime gap found
     */
    bool get_end_points(mpz_t mpz_start, mpz_t mpz_end);
};

#endif // FREYCOIN_POW_POW_H
