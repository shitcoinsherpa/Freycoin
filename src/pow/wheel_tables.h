// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Pre-computed wheel factorization tables for prime sieving.
 *
 * Wheel factorization eliminates candidates divisible by small primes
 * without explicit testing. A wheel of primorial P# = p1 * p2 * ... * pn
 * only tests positions coprime to P#.
 *
 * Wheel 2310 = 2 * 3 * 5 * 7 * 11 = 2310
 * - 480 residues coprime to 2310 (out of 2310)
 * - Eliminates 79.2% of candidates
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_WHEEL_TABLES_H
#define FREYCOIN_POW_WHEEL_TABLES_H

#include <cstdint>
#include <cstddef>

/**
 * Wheel 2310 = 2 * 3 * 5 * 7 * 11
 */
static constexpr uint32_t WHEEL_2310_SIZE = 480;
static constexpr uint32_t WHEEL_2310_PRIMORIAL = 2310;

/**
 * The 480 residues coprime to 2310.
 * All odd positions not divisible by 3, 5, 7, or 11.
 */
extern const uint16_t WHEEL_2310_RESIDUES[WHEEL_2310_SIZE];

/**
 * Delta table: differences between consecutive wheel residues.
 * next_pos = current_pos + WHEEL_2310_DELTAS[wheel_index]
 */
extern const uint8_t WHEEL_2310_DELTAS[WHEEL_2310_SIZE];

/**
 * Lookup table for wheel index from position mod 2310.
 * Returns index into WHEEL_2310_RESIDUES, or -1 if not coprime to 2310.
 */
extern const int16_t WHEEL_2310_INDEX[WHEEL_2310_PRIMORIAL];

/**
 * Check if a number is coprime to 2310 using lookup table.
 */
inline bool is_coprime_2310_lookup(uint64_t n) {
    uint16_t r = n % WHEEL_2310_PRIMORIAL;
    return WHEEL_2310_INDEX[r] >= 0;
}

/**
 * Quick coprimality check without lookup table.
 */
inline bool is_coprime_2310_quick(uint64_t n) {
    if ((n & 1) == 0) return false;  // divisible by 2
    if (n % 3 == 0) return false;
    if (n % 5 == 0) return false;
    if (n % 7 == 0) return false;
    if (n % 11 == 0) return false;
    return true;
}

/**
 * Get next position coprime to 2310.
 */
inline uint64_t next_coprime_2310(uint64_t pos, size_t& wheel_idx) {
    uint64_t delta = WHEEL_2310_DELTAS[wheel_idx];
    wheel_idx = (wheel_idx + 1) % WHEEL_2310_SIZE;
    return pos + delta;
}

/**
 * Initialize wheel iteration from starting position.
 * Returns first position >= start that is coprime to 2310.
 */
inline uint64_t wheel_init_2310(uint64_t start, size_t& wheel_idx) {
    uint16_t r = start % WHEEL_2310_PRIMORIAL;

    // Find next residue >= r
    for (size_t i = 0; i < WHEEL_2310_SIZE; i++) {
        if (WHEEL_2310_RESIDUES[i] >= r) {
            wheel_idx = i;
            return start - r + WHEEL_2310_RESIDUES[i];
        }
    }

    // Wrap to next cycle
    wheel_idx = 0;
    return start - r + WHEEL_2310_PRIMORIAL + WHEEL_2310_RESIDUES[0];
}

#endif // FREYCOIN_POW_WHEEL_TABLES_H
