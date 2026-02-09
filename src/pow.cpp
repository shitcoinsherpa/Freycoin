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
#include <mpfr.h>
#include <cstring>

/**
 * 2^48 - fixed-point precision for difficulty/merit calculations.
 * 1.0 merit = 0x0001_0000_0000_0000
 */
static constexpr uint64_t TWO_POW48 = 1ULL << 48;

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
 * Extract a uint64_t from an mpz_t, handling both 32-bit and 64-bit platforms.
 */
static uint64_t mpz_get_uint64(const mpz_t value)
{
    if (mpz_fits_ulong_p(value)) {
        return mpz_get_ui(value);
    }
    if (mpz_sizeinbase(value, 2) <= 64) {
        mpz_t high;
        mpz_init(high);
        mpz_fdiv_q_2exp(high, value, 32);
        uint64_t result = (static_cast<uint64_t>(mpz_get_ui(high)) << 32) | mpz_get_ui(value);
        mpz_clear(high);
        return result;
    }
    return 0;
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

    uint64_t result = mpz_get_uint64(merit);

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

    uint64_t min_gap_distance_merit = mpz_get_uint64(min_gap_merit);
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

    // Verify start is prime (BPSW + 25 Miller-Rabin rounds)
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
 *   - Maximum change: +/-1.0 merit per block
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
 * All logarithms computed via MPFR at 256-bit precision.
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
    uint64_t log_actual = mpz_get_uint64(mpz_log_actual_z);
    mpz_clear(mpz_log_actual_z);

    // Compute ln(150) * 2^48 using MPFR (target spacing)
    mpfr_set_ui(mpfr_actual, 150, MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_actual, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, 48, MPFR_RNDN);

    mpz_t mpz_log_target_z;
    mpz_init(mpz_log_target_z);
    mpfr_get_z(mpz_log_target_z, mpfr_ln, MPFR_RNDN);
    uint64_t log_target = mpz_get_uint64(mpz_log_target_z);
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
            next = params.nDifficultyMin;
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
    if (next < params.nDifficultyMin) {
        next = params.nDifficultyMin;
    }

    return next;
}
