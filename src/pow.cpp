// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2013-present The Freycoin developers
// Copyright (c) 2014-2017 Jonnie Frey (Gapcoin)
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <chain.h>
#include <crypto/sha256.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

#include <gmp.h>
#include <cstring>

/**
 * 2^48 - fixed-point precision for difficulty/merit calculations.
 * 1.0 merit = 0x0001_0000_0000_0000
 */
static constexpr uint64_t TWO_POW48 = 1ULL << 48;

/**
 * log2(e) * 2^112 - precomputed constant for integer log calculations.
 * Used for: merit = gap_len * log2(e) / log2(start)
 */
static const char* LOG2E_112_HEX = "171547652b82fe1777d0ffda0d23a";

/**
 * log2(e) * 2^64 - precomputed constant for difficulty adjustment.
 */
static const char* LOG2E_64_HEX = "171547652b82fe177";

/**
 * log(150) * 2^48 - precomputed for 150-second target spacing.
 */
static constexpr uint64_t LOG_150_48 = 0x502b8fea053a6ULL;

/**
 * Convert uint256 to mpz_t (little-endian).
 */
static void uint256_to_mpz(mpz_t result, const uint256& value)
{
    mpz_import(result, 32, -1, 1, -1, 0, value.begin());
}

/**
 * Calculate integer log2 with specified accuracy bits.
 * Returns log2(src) * 2^accuracy.
 *
 * Algorithm: Iteratively square the normalized value and track
 * when it exceeds 2, accumulating fractional bits.
 */
static void mpz_log2_fixed(mpz_t result, const mpz_t src, uint32_t accuracy)
{
    mpz_t tmp, n;
    mpz_init(tmp);
    mpz_init_set(n, src);

    // Integer part: floor(log2(src)) = bit_length - 1
    size_t int_log2 = mpz_sizeinbase(n, 2) - 1;
    mpz_set_ui(result, int_log2);

    uint32_t bits = 0;
    uint32_t shift = accuracy + int_log2;

    // Scale up for fractional precision
    mpz_mul_2exp(result, result, accuracy);
    mpz_mul_2exp(n, n, accuracy);

    for (;;) {
        mpz_fdiv_q_2exp(tmp, n, shift);

        // While n / 2^shift < 2, square n
        while (mpz_cmp_ui(tmp, 2) < 0 && bits <= accuracy) {
            mpz_mul(n, n, n);
            mpz_fdiv_q_2exp(n, n, shift);
            mpz_fdiv_q_2exp(tmp, n, shift);
            bits++;
        }

        if (bits > accuracy) break;

        // Add 2^(accuracy - bits) to result
        mpz_set_ui(tmp, 1);
        mpz_mul_2exp(tmp, tmp, accuracy - bits);
        mpz_add(result, result, tmp);

        // n = n / 2
        mpz_fdiv_q_2exp(n, n, 1);
    }

    mpz_clear(tmp);
    mpz_clear(n);
}

/**
 * Calculate merit of a prime gap.
 * merit = gap_size / ln(start) = gap_size * log2(e) / log2(start)
 *
 * Returns merit * 2^48 (fixed-point with 48-bit fractional precision).
 */
static uint64_t CalculateMerit(const mpz_t start, const mpz_t end)
{
    mpz_t merit, log_start, log2e112;
    mpz_init(merit);
    mpz_init(log_start);
    mpz_init_set_str(log2e112, LOG2E_112_HEX, 16);

    // merit = gap_len * log2(e) * 2^(64+48)
    mpz_sub(merit, end, start);
    mpz_mul(merit, merit, log2e112);

    // merit = merit / (log2(start) * 2^64)
    mpz_log2_fixed(log_start, start, 64);
    mpz_fdiv_q(merit, merit, log_start);

    uint64_t result = 0;
    if (mpz_fits_ulong_p(merit)) {
        result = mpz_get_ui(merit);
    } else if (mpz_sizeinbase(merit, 2) <= 64) {
        // Handle 64-bit values on 32-bit systems
        mpz_t high;
        mpz_init(high);
        mpz_fdiv_q_2exp(high, merit, 32);
        result = (static_cast<uint64_t>(mpz_get_ui(high)) << 32) | mpz_get_ui(merit);
        mpz_clear(high);
    }

    mpz_clear(merit);
    mpz_clear(log_start);
    mpz_clear(log2e112);

    return result;
}

/**
 * Generate deterministic random value from gap endpoints.
 * Uses SHA256d(start || end), XOR-folded to 64 bits.
 */
static uint64_t GapRandom(const mpz_t start, const mpz_t end)
{
    // Export start and end to byte arrays
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
    uint64_t result = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];

    return result;
}

/**
 * Calculate achieved difficulty for a prime gap.
 * difficulty = merit + random(start, end) % min_gap_distance_merit
 *
 * The random component provides sub-integer-merit precision,
 * making it harder to game the system with specific gap sizes.
 */
static uint64_t CalculateDifficulty(const mpz_t start, const mpz_t end)
{
    mpz_t log_start, min_gap_merit, log2e112;
    mpz_init(log_start);
    mpz_init(min_gap_merit);
    mpz_init_set_str(log2e112, LOG2E_112_HEX, 16);

    // Calculate 2/ln(start) * 2^48 - the merit of minimal gap distance
    // min_gap_merit = 2 * log2(e) * 2^(64+48) / (log2(start) * 2^64)
    mpz_set_ui(min_gap_merit, 2);
    mpz_mul(min_gap_merit, min_gap_merit, log2e112);
    mpz_log2_fixed(log_start, start, 64);
    mpz_fdiv_q(min_gap_merit, min_gap_merit, log_start);

    uint64_t min_gap_distance_merit = 1;
    if (mpz_fits_ulong_p(min_gap_merit)) {
        min_gap_distance_merit = mpz_get_ui(min_gap_merit);
    } else if (mpz_sizeinbase(min_gap_merit, 2) <= 64) {
        mpz_t high;
        mpz_init(high);
        mpz_fdiv_q_2exp(high, min_gap_merit, 32);
        min_gap_distance_merit = (static_cast<uint64_t>(mpz_get_ui(high)) << 32) | mpz_get_ui(min_gap_merit);
        mpz_clear(high);
    }

    mpz_clear(log_start);
    mpz_clear(min_gap_merit);
    mpz_clear(log2e112);

    // difficulty = merit + (rand % min_gap_distance_merit)
    uint64_t merit = CalculateMerit(start, end);
    uint64_t rand = GapRandom(start, end);
    uint64_t difficulty = merit + (rand % min_gap_distance_merit);

    return difficulty;
}

/**
 * Check whether a block satisfies the prime gap proof-of-work requirement.
 *
 * Algorithm:
 * 1. Validate nShift is in [MIN_SHIFT, MAX_SHIFT]
 * 2. Validate nDifficulty >= minimum
 * 3. Construct start = GetHash() * 2^nShift + nAdd
 * 4. Verify start is prime (BPSW via GMP, 25 Miller-Rabin rounds)
 * 5. Find next prime after start
 * 6. Calculate achieved difficulty = f(merit, random)
 * 7. Accept if achieved >= required
 */
bool CheckProofOfWork(const CBlockHeader& block, const Consensus::Params& params)
{
    // Validate shift range
    if (block.nShift < MIN_SHIFT) {
        return false;
    }
    if (block.nShift > MAX_SHIFT) {
        return false;
    }

    // Validate difficulty meets minimum
    if (block.nDifficulty < params.nDifficultyMin) {
        return false;
    }

    // Get consensus hash (84 bytes)
    uint256 hash = block.GetHash();

    // Convert hash to mpz
    mpz_t mpz_hash;
    mpz_init(mpz_hash);
    uint256_to_mpz(mpz_hash, hash);

    // Verify hash is in range (2^255, 2^256) - i.e., 256 bits
    size_t hash_bits = mpz_sizeinbase(mpz_hash, 2);
    if (hash_bits != 256) {
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

    // Verify start is prime (BPSW + 25 Miller-Rabin rounds)
    // mpz_probab_prime_p returns: 0 = composite, 1 = probably prime, 2 = definitely prime
    int prime_result = mpz_probab_prime_p(mpz_start, 25);
    if (prime_result == 0) {
        mpz_clear(mpz_start);
        return false;
    }

    // Find next prime after start
    mpz_t mpz_end;
    mpz_init(mpz_end);
    mpz_nextprime(mpz_end, mpz_start);

    // Calculate achieved difficulty
    uint64_t achieved = CalculateDifficulty(mpz_start, mpz_end);

    mpz_clear(mpz_start);
    mpz_clear(mpz_end);

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
 *   - Maximum change: ±1.0 merit per block
 *   - Minimum: params.nDifficultyMin
 */
uint64_t GetNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    // Genesis or first block: use current difficulty
    if (pindexLast->nHeight == 0) {
        return pindexLast->nDifficulty;
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

    return CalculateNextWorkRequired(pindexLast->nDifficulty, nActualTimespan, params);
}

/**
 * Calculate next difficulty from previous difficulty and solve time.
 *
 * Formula: next = current + log(target/actual) / damping
 *
 * This is consensus-critical code. The integer math must match exactly
 * across all implementations.
 */
uint64_t CalculateNextWorkRequired(uint64_t nDifficulty, int64_t nActualTimespan, const Consensus::Params& params)
{
    // Clamp extreme timespans
    if (nActualTimespan < 1) {
        nActualTimespan = 1;
    }
    // Max 12x target (30 minutes for 150s target)
    if (nActualTimespan > 12 * params.nPowTargetSpacing) {
        nActualTimespan = 12 * params.nPowTargetSpacing;
    }

    // Calculate log(actual_timespan) * 2^48 using integer math
    mpz_t mpz_log_actual, mpz_log2e64;
    mpz_init_set_ui(mpz_log_actual, static_cast<unsigned long>(nActualTimespan));
    mpz_init_set_str(mpz_log2e64, LOG2E_64_HEX, 16);

    // log_actual = (log2(actual) * 2^(64+48)) / (log2(e) * 2^64)
    mpz_log2_fixed(mpz_log_actual, mpz_log_actual, 64 + 48);
    mpz_fdiv_q(mpz_log_actual, mpz_log_actual, mpz_log2e64);

    uint64_t log_actual = 0;
    if (mpz_fits_ulong_p(mpz_log_actual)) {
        log_actual = mpz_get_ui(mpz_log_actual);
    } else if (mpz_sizeinbase(mpz_log_actual, 2) <= 64) {
        mpz_t high;
        mpz_init(high);
        mpz_fdiv_q_2exp(high, mpz_log_actual, 32);
        log_actual = (static_cast<uint64_t>(mpz_get_ui(high)) << 32) | mpz_get_ui(mpz_log_actual);
        mpz_clear(high);
    }

    mpz_clear(mpz_log_actual);
    mpz_clear(mpz_log2e64);

    const uint64_t log_target = LOG_150_48;

    uint64_t next = nDifficulty;

    // Damping: 1/256 (shift 8) for increases, 1/64 (shift 6) for decreases
    uint64_t shift = (log_actual > log_target) ? 6 : 8;

    // Apply adjustment
    if (log_target >= log_actual) {
        uint64_t delta = log_target - log_actual;
        next += delta >> shift;
    } else {
        uint64_t delta = log_actual - log_target;
        // Check for underflow
        if (nDifficulty >= (delta >> shift)) {
            next -= delta >> shift;
        } else {
            next = params.nDifficultyMin;
        }
    }

    // Clamp change to ±1.0 merit per block
    if (next > nDifficulty + TWO_POW48) {
        next = nDifficulty + TWO_POW48;
    }
    if (next < nDifficulty - TWO_POW48 && nDifficulty >= TWO_POW48) {
        next = nDifficulty - TWO_POW48;
    }

    // Enforce minimum
    if (next < params.nDifficultyMin) {
        next = params.nDifficultyMin;
    }

    return next;
}
