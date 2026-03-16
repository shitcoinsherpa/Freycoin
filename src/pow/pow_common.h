// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Common definitions for Freycoin Proof-of-Work mining.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 * His prime gap discoveries still hold records on global merit rankings.
 */

#ifndef FREYCOIN_POW_COMMON_H
#define FREYCOIN_POW_COMMON_H

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <gmp.h>

// Include consensus constants (MIN_DIFFICULTY, MIN_SHIFT, MAX_SHIFT)
#include <pow.h>

/**
 * Fixed-point precision for difficulty/merit calculations.
 * 1.0 merit = 2^48 = 0x0001_0000_0000_0000
 */
static constexpr uint64_t TWO_POW48 = 1ULL << 48;

/**
 * Minimum test difficulty (merit ~1 for easy testing)
 */
static constexpr uint64_t MIN_TEST_DIFFICULTY = TWO_POW48;

/**
 * L1 cache size in bytes (32KB typical for modern CPUs)
 */
#ifndef L1D_CACHE_SIZE
#define L1D_CACHE_SIZE 32768
#endif

/**
 * L2 cache size in bytes
 */
#ifndef L2_CACHE_SIZE
#define L2_CACHE_SIZE 262144
#endif

/**
 * Segment size for L1-optimized sieving
 */
#define SEGMENT_SIZE_BITS (L1D_CACHE_SIZE * 8)
#define SEGMENT_SIZE_NUMBERS (SEGMENT_SIZE_BITS * 2)

/**
 * Wheel-2310 constants
 */
#define WHEEL_PRIMORIAL 2310
#define WHEEL_SIZE 480

/**
 * Primorial for GCD pre-filtering
 */
#define PRIMORIAL_23 223092870ULL

/**
 * Default number of sieving primes.
 *
 * More primes = more composites eliminated before expensive primality tests.
 * 250K primes covers primes up to ~3.5M, eliminating the vast majority of
 * composites from the sieve before any BPSW/Fermat test is needed.
 * Trade-off: slightly more memory (~1MB) and longer calc_starts() per hash,
 * but significantly fewer wasted primality tests.
 *
 * Benchmarks: 250K gives 15-25% net speedup over 100K for typical shift values.
 */
static constexpr uint64_t DEFAULT_SIEVE_PRIMES = 250000;

/**
 * Mining tier enumeration
 */
enum class MiningTier {
    CPU_ONLY = 1,      // Pure CPU mining with BPSW
    CPU_OPENCL = 2,    // CPU sieve + OpenCL primality (AMD, Intel, NVIDIA)
    CPU_CUDA = 3,      // CPU sieve + CUDA primality (NVIDIA, preferred)
};

/**
 * Bucket entry for large prime storage (Oliveira e Silva's algorithm)
 */
struct BucketEntry {
    uint32_t prime_idx;
    uint32_t next_hit;
};

/**
 * Bucket list for a segment
 */
struct Bucket {
    std::vector<BucketEntry> entries;
    uint32_t segment_idx;
};

/**
 * Candidate batch for GPU processing
 */
struct CandidateBatch {
    std::vector<uint32_t> candidates;  // Limb-packed candidate numbers
    std::vector<uint32_t> indices;     // Sieve indices for each candidate
    uint32_t bits;                     // Bit width (rounded up to 32-bit boundary)
    uint32_t count;                    // Number of candidates
};

/**
 * Compute GPU batch bit width from shift.
 * Total prime size = 256 (hash) + shift bits, rounded up to 32-bit limb boundary.
 * Minimum 320 bits — the GPU kernels have fixed 320-bit and 352-bit variants,
 * so the batch buffer must be padded to at least 10 limbs (320 bits).
 * Post-fork (shift >= 8192): uses cooperative kernel with arbitrary width.
 */
inline uint32_t gpu_bits_for_shift(uint16_t shift) {
    uint32_t total_bits = 256 + static_cast<uint32_t>(shift);
    uint32_t rounded = ((total_bits + 31) / 32) * 32;
    return std::max(rounded, uint32_t{320});
}

/**
 * Snapshot of mining statistics (copyable, thread-safe read)
 */
struct MiningStatsSnapshot {
    uint64_t primes_found;
    uint64_t tests_performed;
    uint64_t gaps_found;
    uint64_t nonces_tested;
    uint64_t sieve_runs;
    uint64_t cache_misses;
    uint64_t time_sieving_us;
    uint64_t time_testing_us;
};

/**
 * Thread-safe mining statistics
 */
struct MiningStats {
    std::atomic<uint64_t> primes_found{0};
    std::atomic<uint64_t> tests_performed{0};
    std::atomic<uint64_t> gaps_found{0};
    std::atomic<uint64_t> nonces_tested{0};
    std::atomic<uint64_t> sieve_runs{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> time_sieving_us{0};
    std::atomic<uint64_t> time_testing_us{0};

    void reset() {
        primes_found = 0;
        tests_performed = 0;
        gaps_found = 0;
        nonces_tested = 0;
        sieve_runs = 0;
        cache_misses = 0;
        time_sieving_us = 0;
        time_testing_us = 0;
    }

    MiningStatsSnapshot snapshot() const {
        return {
            primes_found.load(std::memory_order_relaxed),
            tests_performed.load(std::memory_order_relaxed),
            gaps_found.load(std::memory_order_relaxed),
            nonces_tested.load(std::memory_order_relaxed),
            sieve_runs.load(std::memory_order_relaxed),
            cache_misses.load(std::memory_order_relaxed),
            time_sieving_us.load(std::memory_order_relaxed),
            time_testing_us.load(std::memory_order_relaxed)
        };
    }
};

/**
 * Thread-safe validation statistics (for profiling CheckProofOfWork)
 */
struct ValidationStats {
    std::atomic<uint64_t> blocks_validated{0};
    std::atomic<uint64_t> total_checkpow_us{0};
    std::atomic<uint64_t> total_primality_us{0};
    std::atomic<uint64_t> total_nextprime_us{0};
    std::atomic<uint64_t> total_difficulty_us{0};
    std::atomic<uint64_t> tier_gpu{0};
    std::atomic<uint64_t> tier_gwnum{0};
    std::atomic<uint64_t> tier_gmp{0};
    std::atomic<uint64_t> gpu_lock_wait_us{0};
    std::atomic<uint64_t> gpu_lock_hold_us{0};
    // Fine-grained fast_nextprime profiling
    std::atomic<uint64_t> total_sieve_init_us{0};
    std::atomic<uint64_t> total_sieve_segment_us{0};
    std::atomic<uint64_t> total_fermat_count{0};
    std::atomic<uint64_t> total_fermat_us{0};
    std::atomic<uint64_t> total_bpsw_confirm_us{0};
    std::atomic<uint64_t> total_survivors{0};

    struct Snapshot {
        uint64_t blocks_validated;
        uint64_t total_checkpow_us;
        uint64_t total_primality_us;
        uint64_t total_nextprime_us;
        uint64_t total_difficulty_us;
        uint64_t tier_gpu;
        uint64_t tier_gwnum;
        uint64_t tier_gmp;
        uint64_t gpu_lock_wait_us;
        uint64_t gpu_lock_hold_us;
        uint64_t total_sieve_init_us;
        uint64_t total_sieve_segment_us;
        uint64_t total_fermat_count;
        uint64_t total_fermat_us;
        uint64_t total_bpsw_confirm_us;
        uint64_t total_survivors;
    };

    Snapshot snapshot() const {
        return {
            blocks_validated.load(std::memory_order_relaxed),
            total_checkpow_us.load(std::memory_order_relaxed),
            total_primality_us.load(std::memory_order_relaxed),
            total_nextprime_us.load(std::memory_order_relaxed),
            total_difficulty_us.load(std::memory_order_relaxed),
            tier_gpu.load(std::memory_order_relaxed),
            tier_gwnum.load(std::memory_order_relaxed),
            tier_gmp.load(std::memory_order_relaxed),
            gpu_lock_wait_us.load(std::memory_order_relaxed),
            gpu_lock_hold_us.load(std::memory_order_relaxed),
            total_sieve_init_us.load(std::memory_order_relaxed),
            total_sieve_segment_us.load(std::memory_order_relaxed),
            total_fermat_count.load(std::memory_order_relaxed),
            total_fermat_us.load(std::memory_order_relaxed),
            total_bpsw_confirm_us.load(std::memory_order_relaxed),
            total_survivors.load(std::memory_order_relaxed),
        };
    }

    void reset() {
        blocks_validated = 0;
        total_checkpow_us = 0;
        total_primality_us = 0;
        total_nextprime_us = 0;
        total_difficulty_us = 0;
        tier_gpu = 0;
        tier_gwnum = 0;
        tier_gmp = 0;
        gpu_lock_wait_us = 0;
        gpu_lock_hold_us = 0;
        total_sieve_init_us = 0;
        total_sieve_segment_us = 0;
        total_fermat_count = 0;
        total_fermat_us = 0;
        total_bpsw_confirm_us = 0;
        total_survivors = 0;
    }
};

/** Global validation stats, accumulated by CheckProofOfWork and fast_nextprime */
extern ValidationStats g_validation_stats;

/**
 * SIMD capability levels
 */
enum class SIMDLevel {
    NONE = 0,
    SSE2 = 1,
    AVX2 = 2,
    AVX512 = 3
};

/**
 * Utility macros for bit manipulation in 64-bit arrays
 */
#define set_bit64(ary, i) ((ary)[(i) >> 6] |= (1ULL << ((i) & 0x3f)))
#define clear_bit64(ary, i) ((ary)[(i) >> 6] &= ~(1ULL << ((i) & 0x3f)))
#define test_bit64(ary, i) (((ary)[(i) >> 6] & (1ULL << ((i) & 0x3f))) != 0)

/**
 * Utility: round up to multiple
 */
#define round_up(x, y) ((((x) + (y) - 1) / (y)) * (y))

/**
 * Convert uint256-like byte array to mpz_t (little-endian)
 */
inline void ary_to_mpz(mpz_t result, const uint8_t* ary, size_t len) {
    mpz_import(result, len, -1, sizeof(uint8_t), -1, 0, ary);
}

/**
 * Convert mpz_t to byte array (little-endian)
 * Returns pointer to allocated buffer that caller must free
 */
inline uint8_t* mpz_to_ary(mpz_t src, size_t* len) {
    size_t count;
    uint8_t* result = static_cast<uint8_t*>(mpz_export(nullptr, &count, -1, sizeof(uint8_t), -1, 0, src));
    if (len) *len = count;
    return result;
}

/**
 * Set mpz_t from uint64_t (portable across 32/64-bit)
 *
 * Uses sizeof(unsigned long) to pick the right path: GMP's mpz_set_ui takes
 * unsigned long, which is 8 bytes on Linux LP64 but only 4 bytes on Windows
 * LLP64 (even on x86_64). Architecture defines like __x86_64__ are NOT
 * reliable indicators of unsigned long width.
 */
inline void mpz_set_ui64(mpz_t mpz, uint64_t val) {
    if constexpr (sizeof(unsigned long) >= sizeof(uint64_t)) {
        mpz_set_ui(mpz, val);
    } else {
        mpz_set_ui(mpz, static_cast<uint32_t>(val >> 32));
        mpz_mul_2exp(mpz, mpz, 32);
        mpz_add_ui(mpz, mpz, static_cast<uint32_t>(val));
    }
}

/**
 * Initialize and set mpz_t from uint64_t
 */
inline void mpz_init_set_ui64(mpz_t mpz, uint64_t val) {
    if constexpr (sizeof(unsigned long) >= sizeof(uint64_t)) {
        mpz_init_set_ui(mpz, val);
    } else {
        mpz_init_set_ui(mpz, static_cast<uint32_t>(val >> 32));
        mpz_mul_2exp(mpz, mpz, 32);
        mpz_add_ui(mpz, mpz, static_cast<uint32_t>(val));
    }
}

/**
 * Get uint64_t from mpz_t (lower 64 bits)
 */
inline uint64_t mpz_get_ui64(mpz_t mpz) {
    if constexpr (sizeof(unsigned long) >= sizeof(uint64_t)) {
        return mpz_get_ui(mpz);
    } else {
        mpz_t tmp;
        mpz_init_set(tmp, mpz);
        uint64_t lo = mpz_get_ui(tmp) & 0xffffffff;
        mpz_fdiv_q_2exp(tmp, tmp, 32);
        uint64_t hi = mpz_get_ui(tmp) & 0xffffffff;
        mpz_clear(tmp);
        return (hi << 32) | lo;
    }
}

/**
 * Check if mpz fits in uint64_t
 */
inline bool mpz_fits_ui64(mpz_t mpz) {
    if constexpr (sizeof(unsigned long) >= sizeof(uint64_t)) {
        return mpz_fits_ulong_p(mpz);
    } else {
        return mpz_sizeinbase(mpz, 2) <= 64;
    }
}

#endif // FREYCOIN_POW_COMMON_H
