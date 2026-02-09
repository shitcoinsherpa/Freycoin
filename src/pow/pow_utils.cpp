// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Implementation of PoW utility functions.
 *
 * All logarithmic and exponential computations use MPFR (256-bit precision)
 * for correctness. No home-grown approximations — every block we mine is a
 * mathematical proof, and the math must be exact.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/pow_utils.h>
#include <crypto/sha256.h>
#include <gmp.h>
#include <mpfr.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

// MPFR precision for all computations (256 bits ≈ 77 decimal digits)
static constexpr mpfr_prec_t MPFR_PRECISION = 256;

PoWUtils::PoWUtils() {
    // Compute log(150) * 2^48 at init using MPFR for exactness
    mpfr_t mpfr_150, mpfr_log150, mpfr_scaled;
    mpfr_init2(mpfr_150, MPFR_PRECISION);
    mpfr_init2(mpfr_log150, MPFR_PRECISION);
    mpfr_init2(mpfr_scaled, MPFR_PRECISION);

    mpfr_set_ui(mpfr_150, 150, MPFR_RNDN);
    mpfr_log(mpfr_log150, mpfr_150, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_scaled, mpfr_log150, 48, MPFR_RNDN);

    mpz_t mpz_tmp;
    mpz_init(mpz_tmp);
    mpfr_get_z(mpz_tmp, mpfr_scaled, MPFR_RNDN);
    log_150_48_computed = mpz_get_ui(mpz_tmp);
    mpz_clear(mpz_tmp);

    mpfr_clear(mpfr_150);
    mpfr_clear(mpfr_log150);
    mpfr_clear(mpfr_scaled);
}

PoWUtils::~PoWUtils() {
}

uint64_t PoWUtils::gettime_usec() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

/**
 * Compute ln(src) * 2^precision as an integer using MPFR.
 *
 * This replaces the old mpz_log2-based approach. MPFR's mpfr_log is
 * proven correct to the last bit at any requested precision.
 */
static void mpfr_ln_fixed(mpz_t result, mpz_t src, uint32_t precision) {
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

uint64_t PoWUtils::merit(mpz_t mpz_start, mpz_t mpz_end) {
    // merit = gap / ln(start), returned as fixed-point * 2^48
    //
    // Computed as: (gap * 2^48) / (ln(start) * 2^48) * 2^48
    // Simplified:  gap * 2^48 / ln_fixed(start, 48)

    mpz_t mpz_gap, mpz_ln, mpz_merit;
    mpz_init(mpz_gap);
    mpz_init(mpz_ln);
    mpz_init(mpz_merit);

    mpz_sub(mpz_gap, mpz_end, mpz_start);

    // ln(start) * 2^48
    mpfr_ln_fixed(mpz_ln, mpz_start, 48);

    // merit_fp48 = gap * 2^48 / ln_fixed
    // But gap is small (fits uint64), so: gap << 48 / ln_fixed
    // Actually: (gap * 2^48) is what we want divided by (ln(start) * 2^48 / 2^48)
    // = gap * 2^48 * 2^48 / (ln(start) * 2^48) = gap * 2^48 / ln(start)
    // Which is: gap * 2^96 / ln_fixed(48)
    mpz_mul_2exp(mpz_merit, mpz_gap, 96);
    mpz_fdiv_q(mpz_merit, mpz_merit, mpz_ln);

    uint64_t result = 0;
    if (mpz_fits_ui64(mpz_merit)) {
        result = mpz_get_ui64(mpz_merit);
    }

    mpz_clear(mpz_gap);
    mpz_clear(mpz_ln);
    mpz_clear(mpz_merit);

    return result;
}

uint64_t PoWUtils::rand(mpz_t mpz_start, mpz_t mpz_end) {
    // Export start and end to byte arrays
    size_t start_len = 0, end_len = 0;
    uint8_t* start_bytes = mpz_to_ary(mpz_start, &start_len);
    uint8_t* end_bytes = mpz_to_ary(mpz_end, &end_len);

    // SHA256(start || end)
    uint8_t tmp[CSHA256::OUTPUT_SIZE];
    CSHA256()
        .Write(start_bytes, start_len)
        .Write(end_bytes, end_len)
        .Finalize(tmp);

    // SHA256(tmp) - double hash
    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(tmp, CSHA256::OUTPUT_SIZE).Finalize(hash);

    // XOR-fold 256 bits to 64 bits
    const uint64_t* ptr = reinterpret_cast<const uint64_t*>(hash);
    uint64_t result = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];

    free(start_bytes);
    free(end_bytes);

    return result;
}

uint64_t PoWUtils::difficulty(mpz_t mpz_start, mpz_t mpz_end) {
    // min_gap_distance_merit = 2 / ln(start), in 2^48 fixed-point
    mpz_t mpz_ln;
    mpz_init(mpz_ln);

    // ln(start) * 2^48
    mpfr_ln_fixed(mpz_ln, mpz_start, 48);

    // 2 * 2^48 * 2^48 / (ln(start) * 2^48) = 2 * 2^48 / ln(start)
    mpz_t mpz_num;
    mpz_init(mpz_num);
    mpz_set_ui(mpz_num, 2);
    mpz_mul_2exp(mpz_num, mpz_num, 96);
    mpz_fdiv_q(mpz_num, mpz_num, mpz_ln);

    uint64_t min_gap_distance_merit = 1;
    if (mpz_fits_ui64(mpz_num)) {
        min_gap_distance_merit = mpz_get_ui64(mpz_num);
    }

    mpz_clear(mpz_ln);
    mpz_clear(mpz_num);

    // difficulty = merit + (rand % min_gap_distance_merit)
    uint64_t m = merit(mpz_start, mpz_end);
    uint64_t r = rand(mpz_start, mpz_end);
    uint64_t diff = m + (r % min_gap_distance_merit);

    return diff;
}

uint64_t PoWUtils::target_size(mpz_t mpz_start, uint64_t difficulty) {
    // target_size = difficulty * ln(start), with difficulty in 2^48 fixed-point
    // = (difficulty / 2^48) * ln(start)
    // = difficulty * ln_fixed(start, 48) / 2^(48 + 48)
    // = difficulty * ln_fixed(start, 48) / 2^96

    mpz_t mpz_ln, mpz_result;
    mpz_init(mpz_ln);
    mpz_init(mpz_result);

    mpfr_ln_fixed(mpz_ln, mpz_start, 48);
    mpz_set_ui64(mpz_result, difficulty);
    mpz_mul(mpz_result, mpz_result, mpz_ln);
    mpz_fdiv_q_2exp(mpz_result, mpz_result, 96);

    uint64_t result = 0;
    if (mpz_fits_ui64(mpz_result)) {
        result = mpz_get_ui64(mpz_result);
    }

    mpz_clear(mpz_ln);
    mpz_clear(mpz_result);

    return result;
}

void PoWUtils::target_work(std::vector<uint8_t>& n_primes, uint64_t difficulty) {
    // work = exp(difficulty / 2^48)
    mpz_t mpz_n_primes;
    mpz_init(mpz_n_primes);

    mpfr_t mpfr_difficulty;
    mpfr_init2(mpfr_difficulty, MPFR_PRECISION);
    mpfr_set_ui(mpfr_difficulty, static_cast<unsigned long>(difficulty >> 32), MPFR_RNDN);
    mpfr_mul_2exp(mpfr_difficulty, mpfr_difficulty, 32, MPFR_RNDN);
    mpfr_add_ui(mpfr_difficulty, mpfr_difficulty, static_cast<unsigned long>(difficulty & 0xffffffff), MPFR_RNDN);
    mpfr_div_2exp(mpfr_difficulty, mpfr_difficulty, 48, MPFR_RNDN);

    mpfr_exp(mpfr_difficulty, mpfr_difficulty, MPFR_RNDN);
    mpfr_get_z(mpz_n_primes, mpfr_difficulty, MPFR_RNDN);

    mpfr_clear(mpfr_difficulty);

    size_t len;
    uint8_t* ary = mpz_to_ary(mpz_n_primes, &len);
    n_primes.assign(ary, ary + len);
    free(ary);

    mpz_clear(mpz_n_primes);
}

uint64_t PoWUtils::next_difficulty(uint64_t difficulty, uint64_t actual_timespan, bool /*testnet*/) {
    // Compute ln(actual_timespan) * 2^48 using MPFR
    mpfr_t mpfr_actual, mpfr_ln;
    mpfr_init2(mpfr_actual, MPFR_PRECISION);
    mpfr_init2(mpfr_ln, MPFR_PRECISION);

    mpfr_set_ui(mpfr_actual, static_cast<unsigned long>(actual_timespan), MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_actual, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, 48, MPFR_RNDN);

    mpz_t mpz_log_actual_z;
    mpz_init(mpz_log_actual_z);
    mpfr_get_z(mpz_log_actual_z, mpfr_ln, MPFR_RNDN);

    const uint64_t log_target = log_150_48_computed;
    const uint64_t log_actual = mpz_get_ui64(mpz_log_actual_z);

    mpz_clear(mpz_log_actual_z);
    mpfr_clear(mpfr_actual);
    mpfr_clear(mpfr_ln);

    uint64_t next = difficulty;
    uint64_t shift = 8;  // 1/256 for increases

    // Faster correction for hash rate loss
    if (log_actual > log_target) {
        shift = 6;  // 1/64 for decreases
    }

    // Apply adjustment
    if (log_target >= log_actual) {
        uint64_t delta = log_target - log_actual;
        next += delta >> shift;
    } else {
        uint64_t delta = log_actual - log_target;
        if (difficulty >= (delta >> shift)) {
            next -= delta >> shift;
        } else {
            next = MIN_DIFFICULTY;
        }
    }

    // Clamp change to +/-1.0 merit per block
    if (next > difficulty + TWO_POW48) {
        next = difficulty + TWO_POW48;
    }
    if (next < difficulty - TWO_POW48 && difficulty >= TWO_POW48) {
        next = difficulty - TWO_POW48;
    }

    // Enforce minimum
    if (next < MIN_DIFFICULTY) {
        next = MIN_DIFFICULTY;
    }

    return next;
}

uint64_t PoWUtils::max_difficulty_decrease(uint64_t difficulty, int64_t time, bool /*testnet*/) {
    while (time > 0 && difficulty > MIN_DIFFICULTY) {
        if (difficulty >= TWO_POW48) {
            difficulty -= TWO_POW48;
        }
        time -= 26100;  // 174 * 150 seconds
    }

    if (difficulty < MIN_DIFFICULTY) {
        difficulty = MIN_DIFFICULTY;
    }

    return difficulty;
}

double PoWUtils::gaps_per_day(double pps, uint64_t difficulty) {
    return (60.0 * 60.0 * 24.0) / (target_work_d(difficulty) / pps);
}

double PoWUtils::mpz_log_d(mpz_t mpz) {
    mpfr_t mpfr_tmp;
    mpfr_init2(mpfr_tmp, MPFR_PRECISION);
    mpfr_set_z(mpfr_tmp, mpz, MPFR_RNDN);
    mpfr_log(mpfr_tmp, mpfr_tmp, MPFR_RNDN);
    double res = mpfr_get_d(mpfr_tmp, MPFR_RNDN);
    mpfr_clear(mpfr_tmp);
    return res;
}

double PoWUtils::merit_d(mpz_t mpz_start, mpz_t mpz_end) {
    mpz_t mpz_len;
    mpz_init(mpz_len);
    mpz_sub(mpz_len, mpz_end, mpz_start);

    double m = 0.0;
    if (mpz_fits_ui64(mpz_len)) {
        m = static_cast<double>(mpz_get_ui64(mpz_len)) / mpz_log_d(mpz_start);
    }

    mpz_clear(mpz_len);
    return m;
}

double PoWUtils::rand_d(mpz_t mpz_start, mpz_t mpz_end) {
    size_t start_len = 0, end_len = 0;
    uint8_t* start_bytes = mpz_to_ary(mpz_start, &start_len);
    uint8_t* end_bytes = mpz_to_ary(mpz_end, &end_len);

    uint8_t tmp[CSHA256::OUTPUT_SIZE];
    CSHA256()
        .Write(start_bytes, start_len)
        .Write(end_bytes, end_len)
        .Finalize(tmp);

    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(tmp, CSHA256::OUTPUT_SIZE).Finalize(hash);

    const uint32_t* ptr = reinterpret_cast<const uint32_t*>(hash);
    uint32_t r = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3] ^ ptr[4] ^ ptr[5] ^ ptr[6] ^ ptr[7];

    free(start_bytes);
    free(end_bytes);

    return static_cast<double>(r) / static_cast<double>(UINT32_MAX);
}

double PoWUtils::difficulty_d(mpz_t mpz_start, mpz_t mpz_end) {
    double diff = merit_d(mpz_start, mpz_end) +
                  (2.0 / mpz_log_d(mpz_start)) * rand_d(mpz_start, mpz_end);
    return diff < 0.0 ? 0.0 : diff;
}

double PoWUtils::next_difficulty_d(double difficulty, uint64_t actual_timespan, bool /*testnet*/) {
    // Use MPFR for log(150 / actual)
    mpfr_t mpfr_ratio, mpfr_log_ratio;
    mpfr_init2(mpfr_ratio, MPFR_PRECISION);
    mpfr_init2(mpfr_log_ratio, MPFR_PRECISION);

    mpfr_set_d(mpfr_ratio, 150.0 / static_cast<double>(actual_timespan), MPFR_RNDN);
    mpfr_log(mpfr_log_ratio, mpfr_ratio, MPFR_RNDN);
    double log_ratio = mpfr_get_d(mpfr_log_ratio, MPFR_RNDN);

    mpfr_clear(mpfr_ratio);
    mpfr_clear(mpfr_log_ratio);

    uint64_t shift = 8;
    if (actual_timespan > 150) {
        shift = 6;
    }

    double next = difficulty + log_ratio / (1 << shift);

    if (next > difficulty + 1.0) next = difficulty + 1.0;
    if (next < difficulty - 1.0) next = difficulty - 1.0;

    double min_diff = static_cast<double>(MIN_DIFFICULTY) / TWO_POW48;
    if (next < min_diff) next = min_diff;

    return next;
}

double PoWUtils::target_work_d(uint64_t difficulty) {
    // Use MPFR for exp(d)
    mpfr_t mpfr_d, mpfr_result;
    mpfr_init2(mpfr_d, MPFR_PRECISION);
    mpfr_init2(mpfr_result, MPFR_PRECISION);

    mpfr_set_d(mpfr_d, static_cast<double>(difficulty) / TWO_POW48, MPFR_RNDN);
    mpfr_exp(mpfr_result, mpfr_d, MPFR_RNDN);
    double result = mpfr_get_d(mpfr_result, MPFR_RNDN);

    mpfr_clear(mpfr_d);
    mpfr_clear(mpfr_result);

    return result;
}
