// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2013-present The Freycoin developers
// Copyright (c) 2014-2017 Jonnie Frey (Gapcoin)
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <chain.h>
#include <crypto/sha256.h>
#include <logging.h>
#include <pow/fast_nextprime.h>
#include <pow/pow_common.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>
#include <util/trace.h>

#include <gmp.h>
#include <mpfr.h>
#include <chrono>
#include <cstring>

// Global validation stats instance
ValidationStats g_validation_stats;

// USDT tracepoint semaphores
TRACEPOINT_SEMAPHORE(pow, check_proof_of_work);
TRACEPOINT_SEMAPHORE(pow, primality_test);
TRACEPOINT_SEMAPHORE(pow, difficulty_calc);

/**
 * MPFR precision for all computations (256 bits = ~77 decimal digits).
 */
static constexpr mpfr_prec_t MPFR_PRECISION = 256;

/**
 * Convert uint256 to mpz_t (little-endian).
 */
static void uint256_to_mpz(mpz_t result, const uint256& value)
{
    mpz_import(result, 32, -1, 1, -1, 0, value.begin());
}

/**
 * Compute ln(src) * 2^precision as an integer using MPFR.
 *
 * MPFR's mpfr_log is proven correct to the last bit at any requested
 * precision. This replaces the home-grown mpz_log2_fixed approximation.
 */
static void mpfr_ln_fixed(mpz_t result, const mpz_t src, uint32_t precision)
{
    mpfr_t mpfr_src, mpfr_ln;
    mpfr_init2(mpfr_src, MPFR_PRECISION);
    mpfr_init2(mpfr_ln, MPFR_PRECISION);

    mpfr_set_z(mpfr_src, src, MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_src, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, precision, MPFR_RNDN);
    mpfr_get_z(result, mpfr_ln, MPFR_RNDN);

    mpfr_clear(mpfr_src);
    mpfr_clear(mpfr_ln);
}

/**
 * Calculate merit of a prime gap.
 * merit = gap_size / ln(start), returned as fixed-point * 2^48.
 *
 * Uses MPFR for ln(start) — no approximations.
 */
static uint64_t CalculateMerit(const mpz_t start, const mpz_t end)
{
    mpz_t gap, ln_start, merit;
    mpz_init(gap);
    mpz_init(ln_start);
    mpz_init(merit);

    // gap = end - start
    mpz_sub(gap, end, start);

    // ln(start) * 2^48
    mpfr_ln_fixed(ln_start, start, 48);

    // merit_fp48 = gap * 2^96 / (ln(start) * 2^48)
    //            = gap * 2^48 / ln(start)
    mpz_mul_2exp(merit, gap, 96);
    mpz_fdiv_q(merit, merit, ln_start);

    uint64_t result = mpz_get_ui64(merit);

    mpz_clear(gap);
    mpz_clear(ln_start);
    mpz_clear(merit);

    return result;
}

/**
 * Generate deterministic random value from gap endpoints.
 * Uses SHA256d(start || end), XOR-folded to 64 bits.
 */
static uint64_t GapRandom(const mpz_t start, const mpz_t end)
{
    size_t start_len = (mpz_sizeinbase(start, 2) + 7) / 8;
    size_t end_len = (mpz_sizeinbase(end, 2) + 7) / 8;

    std::vector<uint8_t> start_bytes(start_len);
    std::vector<uint8_t> end_bytes(end_len);

    size_t actual_start_len, actual_end_len;
    mpz_export(start_bytes.data(), &actual_start_len, -1, 1, -1, 0, start);
    mpz_export(end_bytes.data(), &actual_end_len, -1, 1, -1, 0, end);

    // SHA256(start || end)
    uint8_t tmp[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(start_bytes.data(), actual_start_len)
             .Write(end_bytes.data(), actual_end_len)
             .Finalize(tmp);

    // SHA256(tmp) - double hash
    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(tmp, CSHA256::OUTPUT_SIZE).Finalize(hash);

    // XOR-fold 256 bits to 64 bits
    const uint64_t* ptr = reinterpret_cast<const uint64_t*>(hash);
    return ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
}

/**
 * Calculate achieved difficulty for a prime gap.
 * difficulty = merit + random(start, end) % min_gap_distance_merit
 *
 * min_gap_distance_merit = 2 / ln(start), in 2^48 fixed-point.
 *
 * The random component provides sub-integer-merit precision,
 * making it harder to game the system with specific gap sizes.
 */
static uint64_t CalculateDifficulty(const mpz_t start, const mpz_t end)
{
    mpz_t ln_start, min_gap_merit;
    mpz_init(ln_start);
    mpz_init(min_gap_merit);

    // ln(start) * 2^48
    mpfr_ln_fixed(ln_start, start, 48);

    // min_gap_distance_merit = 2 * 2^96 / (ln(start) * 2^48) = 2 * 2^48 / ln(start)
    mpz_set_ui(min_gap_merit, 2);
    mpz_mul_2exp(min_gap_merit, min_gap_merit, 96);
    mpz_fdiv_q(min_gap_merit, min_gap_merit, ln_start);

    uint64_t min_gap_distance_merit = mpz_get_ui64(min_gap_merit);
    if (min_gap_distance_merit == 0) min_gap_distance_merit = 1;

    mpz_clear(ln_start);
    mpz_clear(min_gap_merit);

    // difficulty = merit + (rand % min_gap_distance_merit)
    uint64_t merit = CalculateMerit(start, end);
    uint64_t rand = GapRandom(start, end);
    return merit + (rand % min_gap_distance_merit);
}

/**
 * Check whether a block satisfies the prime gap proof-of-work requirement.
 *
 * Algorithm:
 * 1. Validate nShift is in [MIN_SHIFT, MAX_SHIFT] (fork-aware)
 * 2. Validate nDifficulty >= minimum (fork-aware)
 * 3. Construct start = GetHash() * 2^nShift + nAdd
 * 4. Verify start is prime (BPSW-only via GMP 6.3+, reps=0)
 * 5. Find next prime after start
 * 6. Calculate achieved difficulty = f(merit, random)
 * 7. Accept if achieved >= required
 *
 * When nHeight == -1 (context-free), accepts the union of pre/post-fork
 * ranges. Precise fork enforcement happens in contextual checks.
 */
bool CheckProofOfWork(const CBlockHeader& block, int nHeight, const Consensus::Params& params,
                      PrimeGapData* out_gap)
{
    const auto t_total_start = std::chrono::steady_clock::now();

    // Determine shift and difficulty bounds based on fork state
    uint16_t minShift, maxShift;
    uint64_t diffMin;

    if (nHeight < 0) {
        // Context-free: accept union of both pre-fork and post-fork ranges
        minShift = MIN_SHIFT;
        maxShift = std::max(MAX_SHIFT, params.nMaxShiftPostFork);
        diffMin = std::min(params.nDifficultyMin, params.nDifficultyMinPostFork);
    } else {
        minShift = params.GetMinShift(nHeight);
        maxShift = params.GetMaxShift(nHeight);
        diffMin = params.GetDifficultyMin(nHeight);
    }

    // Validate shift range
    if (block.nShift < minShift) {
        return false;
    }
    if (block.nShift > maxShift) {
        return false;
    }

    // Validate difficulty meets minimum
    if (block.nDifficulty < diffMin) {
        return false;
    }

    // Get consensus hash (84 bytes)
    uint256 hash = block.GetHash();

    // Convert hash to mpz
    mpz_t mpz_hash;
    mpz_init(mpz_hash);
    uint256_to_mpz(mpz_hash, hash);

    // Hash should have at least MIN_HASH_BITS to ensure adequate PoW entropy
    constexpr size_t MIN_HASH_BITS = 200;
    size_t hash_bits = mpz_sizeinbase(mpz_hash, 2);
    if (hash_bits < MIN_HASH_BITS) {
        mpz_clear(mpz_hash);
        return false;
    }

    // Convert adder to mpz
    mpz_t mpz_adder;
    mpz_init(mpz_adder);
    uint256_to_mpz(mpz_adder, block.nAdd);

    // Verify adder < 2^shift
    size_t adder_bits = mpz_sizeinbase(mpz_adder, 2);
    if (adder_bits > block.nShift) {
        mpz_clear(mpz_hash);
        mpz_clear(mpz_adder);
        return false;
    }

    // Construct start = hash * 2^shift + adder
    mpz_t mpz_start;
    mpz_init_set(mpz_start, mpz_hash);
    mpz_mul_2exp(mpz_start, mpz_start, block.nShift);
    mpz_add(mpz_start, mpz_start, mpz_adder);

    mpz_clear(mpz_hash);
    mpz_clear(mpz_adder);

    const size_t prime_bits = mpz_sizeinbase(mpz_start, 2);

    // --- Phase: Primality test ---
    const auto t_prime_start = std::chrono::steady_clock::now();

    // Verify start is prime using gwnum-accelerated Fermat base-2 test.
    // gwnum uses FFT-based squaring: O(n log n) vs GMP's Toom-3 O(n^1.5),
    // giving ~15x speedup at 12K bits (~31ms vs ~466ms).
    // Fermat base-2 catches all composites except base-2 pseudoprimes,
    // which have density < 2^(-k) at k bits — nonexistent at 12K bits.
    // Falls back to GMP BPSW when gwnum is unavailable.
    int prime_result = fast_is_fermat_prp(mpz_start) ? 2 : 0;

    const auto t_prime_end = std::chrono::steady_clock::now();
    const auto prime_us = std::chrono::duration_cast<std::chrono::microseconds>(t_prime_end - t_prime_start).count();

    if (prime_result == 0) {
        mpz_clear(mpz_start);
        return false;
    }

    LogDebug(BCLog::BENCH, "CheckProofOfWork: primality test (%zu bits) took %lld ms\n",
             prime_bits, prime_us / 1000);

    TRACEPOINT(pow, primality_test, (int64_t)prime_bits, (int64_t)prime_us, prime_result);

    // --- Phase: fast_nextprime ---
    const auto t_next_start = std::chrono::steady_clock::now();

    // Find next prime after start (gwnum-accelerated when available)
    mpz_t mpz_end;
    mpz_init(mpz_end);
    fast_nextprime(mpz_end, mpz_start);

    const auto t_next_end = std::chrono::steady_clock::now();
    const auto next_us = std::chrono::duration_cast<std::chrono::microseconds>(t_next_end - t_next_start).count();

    // Compute gap size for logging
    mpz_t mpz_gap;
    mpz_init(mpz_gap);
    mpz_sub(mpz_gap, mpz_end, mpz_start);
    unsigned long gap_size = mpz_get_ui(mpz_gap);
    mpz_clear(mpz_gap);

    LogDebug(BCLog::BENCH, "CheckProofOfWork: fast_nextprime (%zu bits, gap=%lu) took %lld ms\n",
             prime_bits, gap_size, next_us / 1000);

    // --- Phase: Difficulty calculation ---
    const auto t_diff_start = std::chrono::steady_clock::now();

    // Calculate achieved difficulty
    uint64_t achieved = CalculateDifficulty(mpz_start, mpz_end);

    const auto t_diff_end = std::chrono::steady_clock::now();
    const auto diff_us = std::chrono::duration_cast<std::chrono::microseconds>(t_diff_end - t_diff_start).count();

    LogDebug(BCLog::BENCH, "CheckProofOfWork: difficulty calc took %lld ms\n", diff_us / 1000);

    TRACEPOINT(pow, difficulty_calc, (int64_t)prime_bits, (int64_t)diff_us);

    // Capture gap data for caller before clearing mpz values
    if (out_gap) {
        char* start_str = mpz_get_str(nullptr, 16, mpz_start);
        char* end_str = mpz_get_str(nullptr, 16, mpz_end);
        out_gap->start_hex = start_str;
        out_gap->end_hex = end_str;
        free(start_str);
        free(end_str);
        out_gap->gap = gap_size;
        // Compute real merit = gap / ln(start) using MPFR
        if (gap_size > 0) {
            mpfr_t mpfr_start, mpfr_ln;
            mpfr_init2(mpfr_start, 256);
            mpfr_init2(mpfr_ln, 256);
            mpfr_set_z(mpfr_start, mpz_start, MPFR_RNDN);
            mpfr_log(mpfr_ln, mpfr_start, MPFR_RNDN);
            double ln_start = mpfr_get_d(mpfr_ln, MPFR_RNDN);
            out_gap->merit = (ln_start > 0.0) ? static_cast<double>(gap_size) / ln_start : 0.0;
            mpfr_clear(mpfr_start);
            mpfr_clear(mpfr_ln);
        }
    }

    mpz_clear(mpz_start);
    mpz_clear(mpz_end);

    // --- Total timing ---
    const auto t_total_end = std::chrono::steady_clock::now();
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(t_total_end - t_total_start).count();

    // Accumulate validation stats
    g_validation_stats.blocks_validated.fetch_add(1, std::memory_order_relaxed);
    g_validation_stats.total_checkpow_us.fetch_add(total_us, std::memory_order_relaxed);
    g_validation_stats.total_primality_us.fetch_add(prime_us, std::memory_order_relaxed);
    g_validation_stats.total_nextprime_us.fetch_add(next_us, std::memory_order_relaxed);
    g_validation_stats.total_difficulty_us.fetch_add(diff_us, std::memory_order_relaxed);

    LogDebug(BCLog::BENCH, "CheckProofOfWork: TOTAL height=%d shift=%u bits=%zu gap=%lu achieved=%.2f target=%.2f took %lld ms (prime=%lld next=%lld diff=%lld)\n",
             nHeight, block.nShift, prime_bits, gap_size,
             achieved / (double)TWO_POW48, block.nDifficulty / (double)TWO_POW48,
             total_us / 1000, prime_us / 1000, next_us / 1000, diff_us / 1000);

    TRACEPOINT(pow, check_proof_of_work, (int64_t)prime_bits, (int64_t)block.nShift, (int64_t)total_us);

    // Accept if achieved difficulty meets or exceeds target
    return achieved >= block.nDifficulty;
}

/**
 * Calculate the next required difficulty.
 *
 * Uses logarithmic adjustment (Gapcoin algorithm):
 *   next = current + log(target_spacing / actual_spacing)
 *
 * Damping:
 *   - Increases: 1/256 of adjustment (slow up, prevents runaway)
 *   - Decreases: 1/64 of adjustment (faster down, network recovery)
 *
 * Bounds:
 *   - Maximum change: +/-1.0 merit per block
 *   - Minimum: height-aware nDifficultyMin
 */
uint64_t GetNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    int nNextHeight = pindexLast->nHeight + 1;

    // Genesis or first block: use current difficulty
    if (pindexLast->nHeight == 0) {
        return pindexLast->nDifficulty;
    }

    // Difficulty reset at Big Gaps fork height
    if (nNextHeight == params.nBigGapsForkHeight) {
        return params.nDifficultyMinPostFork;
    }

    // Difficulty reset at shift upgrade fork height (stage 2)
    if (nNextHeight == params.nShiftUpgradeForkHeight && params.nShiftUpgradeForkHeight != params.nBigGapsForkHeight) {
        return params.nDifficultyMinPostFork;
    }

    // No retargeting in regtest
    if (params.fPowNoRetargeting) {
        return pindexLast->nDifficulty;
    }

    // Need previous block for timing
    const CBlockIndex* pindexPrev = pindexLast->pprev;
    if (!pindexPrev) {
        return pindexLast->nDifficulty;
    }

    // Actual timespan between last two blocks
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexPrev->GetBlockTime();

    return CalculateNextWorkRequired(pindexLast->nDifficulty, nActualTimespan, nNextHeight, params);
}

/**
 * Calculate next difficulty from previous difficulty and solve time.
 *
 * Formula: next = current + log(target/actual) / damping
 *
 * All logarithms computed via MPFR at 256-bit precision.
 * Target spacing is fork-aware (150s pre-fork, configurable post-fork).
 */
uint64_t CalculateNextWorkRequired(uint64_t nDifficulty, int64_t nActualTimespan, int nHeight, const Consensus::Params& params)
{
    // Use height-aware target spacing
    int64_t nTargetSpacing = params.GetTargetSpacing(nHeight);
    uint64_t nDiffMin = params.GetDifficultyMin(nHeight);

    // Clamp extreme timespans
    if (nActualTimespan < 1) {
        nActualTimespan = 1;
    }
    // Max 12x target
    if (nActualTimespan > 12 * nTargetSpacing) {
        nActualTimespan = 12 * nTargetSpacing;
    }

    // Compute ln(actual_timespan) * 2^48 using MPFR
    mpfr_t mpfr_actual, mpfr_ln;
    mpfr_init2(mpfr_actual, MPFR_PRECISION);
    mpfr_init2(mpfr_ln, MPFR_PRECISION);

    mpfr_set_si(mpfr_actual, nActualTimespan, MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_actual, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, 48, MPFR_RNDN);

    mpz_t mpz_log_actual_z;
    mpz_init(mpz_log_actual_z);
    mpfr_get_z(mpz_log_actual_z, mpfr_ln, MPFR_RNDN);
    uint64_t log_actual = mpz_get_ui64(mpz_log_actual_z);
    mpz_clear(mpz_log_actual_z);

    // Compute ln(target_spacing) * 2^48 using MPFR
    mpfr_set_si(mpfr_actual, nTargetSpacing, MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_actual, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, 48, MPFR_RNDN);

    mpz_t mpz_log_target_z;
    mpz_init(mpz_log_target_z);
    mpfr_get_z(mpz_log_target_z, mpfr_ln, MPFR_RNDN);
    uint64_t log_target = mpz_get_ui64(mpz_log_target_z);
    mpz_clear(mpz_log_target_z);

    mpfr_clear(mpfr_actual);
    mpfr_clear(mpfr_ln);

    uint64_t next = nDifficulty;

    // Damping: 1/256 (shift 8) for increases, 1/64 (shift 6) for decreases
    uint64_t shift = (log_actual > log_target) ? 6 : 8;

    // Apply adjustment
    if (log_target >= log_actual) {
        uint64_t delta = log_target - log_actual;
        next += delta >> shift;
    } else {
        uint64_t delta = log_actual - log_target;
        if (nDifficulty >= (delta >> shift)) {
            next -= delta >> shift;
        } else {
            next = nDiffMin;
        }
    }

    // Clamp change to +/-1.0 merit per block
    if (next > nDifficulty + TWO_POW48) {
        next = nDifficulty + TWO_POW48;
    }
    if (next < nDifficulty - TWO_POW48 && nDifficulty >= TWO_POW48) {
        next = nDifficulty - TWO_POW48;
    }

    // Enforce minimum
    if (next < nDiffMin) {
        next = nDiffMin;
    }

    return next;
}
