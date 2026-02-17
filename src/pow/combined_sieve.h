// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Combined Sieve: Batch multiple nonce intervals through shared sieve passes
 *
 * Based on Seth Troisi's combined sieve technique (2020), which demonstrated
 * 30-70% speedup for prime gap searches by amortizing sieve infrastructure
 * across multiple intervals.
 *
 * Key insight: When sieving K intervals for K different nonces, iterating
 * the prime table once and applying each prime to all K bit arrays keeps
 * primes in L1 cache and reduces total cache pressure by factor K.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_COMBINED_SIEVE_H
#define FREYCOIN_POW_COMBINED_SIEVE_H

#include <pow/pow_common.h>
#include <pow/simd_presieve.h>

#include <cstdint>
#include <vector>

#include <gmp.h>

/**
 * Number of intervals to process simultaneously.
 * 4 is optimal for most L1 cache configurations:
 * 4 × 32KB segments = 128KB, fits comfortably in L2 (256KB+).
 * Each segment individually fits in L1 (32KB).
 */
static constexpr int COMBINED_SIEVE_BATCH = 4;

/**
 * Per-interval state within the combined sieve.
 * Tracks the sieve bit array, starting positions, and presieve offsets
 * for one nonce's search interval.
 */
struct IntervalState {
    uint64_t* segment;         // Bit array for this interval's current segment
    uint32_t* small_starts;    // Starting bit position for each small prime
    uint32_t* large_starts;    // Starting bit position for each large prime
    mpz_t mpz_start;           // hash << shift for this nonce
    uint64_t base_mod;         // mpz_start mod 2310 (for wheel filter)
    bool active;               // Whether this slot is in use
};

/**
 * CombinedSegmentedSieve: Process K intervals through shared prime table
 *
 * The critical optimization is in sieve_small_primes_combined(): for each
 * prime p, we apply it to all K intervals before moving to prime p+1.
 * This keeps p in a register while accessing K different segment arrays,
 * versus the naive approach of loading the entire prime table K times.
 *
 * For 250K primes, this reduces prime table reads from 250K×K to 250K,
 * saving ~1MB of L2 cache bandwidth per segment per batch.
 */
class CombinedSegmentedSieve {
public:
    /**
     * Create a combined sieve for K intervals.
     *
     * @param n_primes Number of sieving primes (DEFAULT_SIEVE_PRIMES)
     * @param total_sieve_size Total sieve range per interval in bits
     */
    CombinedSegmentedSieve(uint64_t n_primes, uint64_t total_sieve_size);
    ~CombinedSegmentedSieve();

    // Non-copyable
    CombinedSegmentedSieve(const CombinedSegmentedSieve&) = delete;
    CombinedSegmentedSieve& operator=(const CombinedSegmentedSieve&) = delete;

    /**
     * Initialize interval k for sieving with the given starting number.
     *
     * @param k     Interval index [0, COMBINED_SIEVE_BATCH)
     * @param start The sieve starting number (hash << shift)
     */
    void init_interval(int k, mpz_t start);

    /** Mark interval k as inactive (nonce exhausted or not needed) */
    void deactivate_interval(int k);

    /** Reset segment counter for a new batch of nonces */
    void reset_segments();

    /** Get number of active intervals */
    int active_count() const;

    /**
     * Sieve the next segment for ALL active intervals simultaneously.
     *
     * This is the core optimization: iterates the prime table once,
     * applying each prime to all active intervals' segments.
     *
     * @return true if more segments remain, false when all segments processed
     */
    bool sieve_next_segment();

    /**
     * Get candidate offsets from interval k's current segment.
     * Candidates pass the wheel-2310 coprimality filter.
     *
     * @param k          Interval index
     * @param candidates Output vector of sieve offsets
     */
    void get_candidates(int k, std::vector<uint64_t>& candidates);

    /** Get current segment offset (same for all intervals) */
    uint64_t get_segment_offset() const;

    /** Get the starting mpz for interval k */
    void get_start(int k, mpz_t result) const;

    /** Get base_mod for interval k */
    uint64_t get_base_mod(int k) const;

    /** Get total sieve size */
    uint64_t get_total_size() const;

private:
    uint64_t n_primes;
    uint32_t* primes;            // Shared prime table

    uint64_t segment_words;
    uint64_t current_segment;
    uint64_t total_segments;
    uint64_t total_size;
    uint32_t small_prime_limit;  // Index where primes exceed segment size

    IntervalState intervals[COMBINED_SIEVE_BATCH];

    // Bucket storage for large primes (shared structure, per-interval entries)
    struct CombinedBucketEntry {
        uint32_t prime_idx;
        uint32_t next_hit;
        uint8_t interval;        // Which interval this entry belongs to
    };
    std::vector<CombinedBucketEntry> buckets[2]; // Double-buffered

    void init_primes(uint64_t n);
    void calc_starts(int k);
    void sieve_small_primes_combined();
    void process_buckets_combined();
    void advance_buckets();
    void apply_presieve(int k, uint64_t seg_low);
};

#endif // FREYCOIN_POW_COMBINED_SIEVE_H
