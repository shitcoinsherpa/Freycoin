// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Implementation of PoW utility functions.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/pow_utils.h>
#include <crypto/sha256.h>
#include <gmp.h>
#ifdef HAVE_MPFR
#include <mpfr.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Pre-computed constants (hex strings)
static const char* LOG2E_112_HEX = "171547652b82fe1777d0ffda0d23a";
static const char* LOG2E_64_HEX = "171547652b82fe177";

PoWUtils::PoWUtils() {
    mpz_init_set_str(mpz_log2e112, LOG2E_112_HEX, 16);
    mpz_init_set_str(mpz_log2e64, LOG2E_64_HEX, 16);
}

PoWUtils::~PoWUtils() {
    mpz_clear(mpz_log2e112);
    mpz_clear(mpz_log2e64);
}

uint64_t PoWUtils::gettime_usec() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

void PoWUtils::mpz_log2(mpz_t result, mpz_t src, uint32_t accuracy) {
    mpz_t tmp, n;
    mpz_init(tmp);
    mpz_init_set(n, src);

    // Integer part: floor(log2(src)) = bit_length - 1
    uint64_t int_log2 = mpz_sizeinbase(n, 2) - 1;
    mpz_set_ui64(result, int_log2);

    uint32_t bits = 0;
    uint32_t shift = accuracy + int_log2;

    // Scale up for fractional precision
    mpz_mul_2exp(result, result, accuracy);
    mpz_mul_2exp(n, n, accuracy);

    for (;;) {
        mpz_fdiv_q_2exp(tmp, n, shift);

        // While n / 2^shift < 2, square n
        while (mpz_get_ui64(tmp) < 2 && bits <= accuracy) {
            mpz_mul(n, n, n);
            mpz_fdiv_q_2exp(n, n, shift);
            mpz_fdiv_q_2exp(tmp, n, shift);
            bits++;
        }

        if (bits > accuracy) break;

        // Add 2^(accuracy - bits) to result
        mpz_set_ui64(tmp, 1);
        mpz_mul_2exp(tmp, tmp, accuracy - bits);
        mpz_add(result, result, tmp);

        // n = n / 2
        mpz_fdiv_q_2exp(n, n, 1);
    }

    mpz_clear(tmp);
    mpz_clear(n);
}

uint64_t PoWUtils::merit(mpz_t mpz_start, mpz_t mpz_end) {
    mpz_t mpz_merit, mpz_ld;
    mpz_init(mpz_merit);
    mpz_init(mpz_ld);

    // merit = gap_len * log2(e) * 2^(64 + 48)
    mpz_sub(mpz_merit, mpz_end, mpz_start);
    mpz_mul(mpz_merit, mpz_merit, mpz_log2e112);

    // merit = merit / (log2(start) * 2^64)
    mpz_log2(mpz_ld, mpz_start, 64);
    mpz_fdiv_q(mpz_merit, mpz_merit, mpz_ld);

    uint64_t result = 0;
    if (mpz_fits_ui64(mpz_merit)) {
        result = mpz_get_ui64(mpz_merit);
    }

    mpz_clear(mpz_merit);
    mpz_clear(mpz_ld);

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
    mpz_t mpz_ld, mpz_tmp;
    mpz_init(mpz_ld);

    // tmp = 2 * log2(e) * 2^(64 + 48)
    mpz_init_set_ui64(mpz_tmp, 2);
    mpz_mul(mpz_tmp, mpz_tmp, mpz_log2e112);

    // tmp = 2 / log(start) with 48-bit precision
    mpz_log2(mpz_ld, mpz_start, 64);
    mpz_fdiv_q(mpz_tmp, mpz_tmp, mpz_ld);

    uint64_t min_gap_distance_merit = 1;
    if (mpz_fits_ui64(mpz_tmp)) {
        min_gap_distance_merit = mpz_get_ui64(mpz_tmp);
    }

    mpz_clear(mpz_ld);
    mpz_clear(mpz_tmp);

    // difficulty = merit + (rand % min_gap_distance_merit)
    uint64_t m = merit(mpz_start, mpz_end);
    uint64_t r = rand(mpz_start, mpz_end);
    uint64_t diff = m + (r % min_gap_distance_merit);

    return diff;
}

uint64_t PoWUtils::target_size(mpz_t mpz_start, uint64_t difficulty) {
    mpz_t mpz_target_size, mpz_difficulty;
    mpz_init(mpz_target_size);
    mpz_init_set_ui64(mpz_difficulty, difficulty);

    // target_size = (difficulty * log2(start)) / log2(e)
    mpz_log2(mpz_target_size, mpz_start, 64);
    mpz_mul(mpz_target_size, mpz_target_size, mpz_difficulty);
    mpz_fdiv_q(mpz_target_size, mpz_target_size, mpz_log2e112);

    uint64_t result = 0;
    if (mpz_fits_ui64(mpz_target_size)) {
        result = mpz_get_ui64(mpz_target_size);
    }

    mpz_clear(mpz_target_size);
    mpz_clear(mpz_difficulty);

    return result;
}

void PoWUtils::target_work(std::vector<uint8_t>& n_primes, uint64_t difficulty) {
    mpz_t mpz_n_primes;
    mpz_init(mpz_n_primes);

#ifdef HAVE_MPFR
    // Use MPFR for exp() - integer part is exact enough
    mpfr_t mpfr_difficulty;
    mpfr_init_set_ui(mpfr_difficulty, static_cast<unsigned long>(difficulty >> 32), MPFR_RNDD);
    mpfr_mul_2exp(mpfr_difficulty, mpfr_difficulty, 32, MPFR_RNDD);
    mpfr_add_ui(mpfr_difficulty, mpfr_difficulty, static_cast<unsigned long>(difficulty & 0xffffffff), MPFR_RNDD);
    mpfr_div_2exp(mpfr_difficulty, mpfr_difficulty, 48, MPFR_RNDD);

    mpfr_exp(mpfr_difficulty, mpfr_difficulty, MPFR_RNDD);
    mpfr_get_z(mpz_n_primes, mpfr_difficulty, MPFR_RNDD);

    mpfr_clear(mpfr_difficulty);
#else
    // Integer approximation: exp(d) = 2^(d * log2(e))
    // d = difficulty / 2^48 (in floating point merit units)
    // d * log2(e) = difficulty * log2(e) / 2^48
    // We use mpz_log2e112 = log2(e) * 2^112, so:
    // d * log2(e) * 2^64 = difficulty * mpz_log2e112 / 2^48
    // Then shift right by 64 to get the integer part for 2^shift
    mpz_t mpz_shifted;
    mpz_init_set_ui64(mpz_shifted, difficulty);
    mpz_mul(mpz_shifted, mpz_shifted, mpz_log2e112);
    mpz_fdiv_q_2exp(mpz_shifted, mpz_shifted, 48 + 64);  // Now we have floor(d * log2(e))

    uint64_t shift = mpz_get_ui64(mpz_shifted);
    mpz_set_ui(mpz_n_primes, 1);
    mpz_mul_2exp(mpz_n_primes, mpz_n_primes, shift);

    mpz_clear(mpz_shifted);
#endif

    size_t len;
    uint8_t* ary = mpz_to_ary(mpz_n_primes, &len);
    n_primes.assign(ary, ary + len);
    free(ary);

    mpz_clear(mpz_n_primes);
}

uint64_t PoWUtils::next_difficulty(uint64_t difficulty, uint64_t actual_timespan, bool /*testnet*/) {
    // Calculate log(actual_timespan) * 2^48
    mpz_t mpz_log_actual;
    mpz_init_set_ui64(mpz_log_actual, actual_timespan);

    // log_actual = (log2(actual) * 2^(64 + 48)) / (log2(e) * 2^64)
    mpz_log2(mpz_log_actual, mpz_log_actual, 64 + 48);
    mpz_fdiv_q(mpz_log_actual, mpz_log_actual, mpz_log2e64);

    const uint64_t log_target = log_150_48;
    const uint64_t log_actual = mpz_get_ui64(mpz_log_actual);

    mpz_clear(mpz_log_actual);

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

    // Clamp change to ±1.0 merit per block
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
        // Difficulty can max decrease about 1 per ~174 blocks (factor e)
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

// Debug versions using MPFR (or fallbacks without MPFR)
#ifdef HAVE_MPFR
double PoWUtils::mpz_log_d(mpz_t mpz) {
    mpfr_t mpfr_tmp;
    mpfr_init_set_z(mpfr_tmp, mpz, MPFR_RNDD);
    mpfr_log(mpfr_tmp, mpfr_tmp, MPFR_RNDD);
    double res = mpfr_get_d(mpfr_tmp, MPFR_RNDD);
    mpfr_clear(mpfr_tmp);
    return res;
}
#else
double PoWUtils::mpz_log_d(mpz_t mpz) {
    // Fallback using integer log2 and conversion
    // ln(x) = log2(x) / log2(e) = log2(x) * ln(2)
    mpz_t mpz_log;
    mpz_init(mpz_log);
    mpz_log2(mpz_log, mpz, 48);
    double log2_val = static_cast<double>(mpz_get_ui64(mpz_log)) / (1ULL << 48);
    mpz_clear(mpz_log);
    return log2_val * 0.693147180559945309417; // ln(2)
}
#endif

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
    // Same as rand() but returns normalized double [0, 1)
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
    uint64_t shift = 8;
    if (actual_timespan > 150) {
        shift = 6;
    }

    double next = difficulty + std::log(150.0 / static_cast<double>(actual_timespan)) / (1 << shift);

    if (next > difficulty + 1.0) next = difficulty + 1.0;
    if (next < difficulty - 1.0) next = difficulty - 1.0;

    double min_diff = static_cast<double>(MIN_DIFFICULTY) / TWO_POW48;
    if (next < min_diff) next = min_diff;

    return next;
}

double PoWUtils::target_work_d(uint64_t difficulty) {
    double d = static_cast<double>(difficulty) / TWO_POW48;
    return std::exp(d);
}
