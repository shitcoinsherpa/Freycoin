// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Freycoin Advanced Mining Engine Implementation
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 * His prime gap discoveries still hold the #17 spot on global merit rankings.
 *
 * This implementation incorporates state-of-the-art optimizations from:
 * - Kim Walisch's primesieve (segmented sieve, L1/L2 cache optimization)
 * - Tomas Oliveira e Silva's bucket algorithm
 * - Robert Gerbicz's prime gap search techniques (primegap-list-project)
 * - Dana Jacobsen's Math::Prime::Util (BPSW implementation)
 *
 * C++20 adaptations:
 * - std::thread instead of boost::thread
 * - std::mutex instead of boost::mutex
 * - std::condition_variable instead of boost::condition_variable
 * - Bitcoin Core's CSHA256 instead of OpenSSL SHA256
 */

#include <pow/mining_engine.h>
#include <pow/simd_presieve.h>
#include <crypto/sha256.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

#ifdef HAVE_CUDA
#include <gpu/cuda_fermat.h>
#endif

#ifdef HAVE_OPENCL
#include <gpu/opencl_fermat.h>
#endif

/*============================================================================
 * Utility macros
 *============================================================================*/

#define set_bit64(ary, i) ((ary)[(i) >> 6] |= (1ULL << ((i) & 0x3f)))
#define clear_bit64(ary, i) ((ary)[(i) >> 6] &= ~(1ULL << ((i) & 0x3f)))
#define test_bit64(ary, i) (((ary)[(i) >> 6] & (1ULL << ((i) & 0x3f))) != 0)
#define round_up(x, y) ((((x) + (y) - 1) / (y)) * (y))

static inline uint64_t get_time_usec() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

static inline bool is_coprime_2310(uint64_t n) {
    if ((n & 1) == 0) return false;
    if (n % 3 == 0) return false;
    if (n % 5 == 0) return false;
    if (n % 7 == 0) return false;
    if (n % 11 == 0) return false;
    return true;
}

/*============================================================================
 * Pre-sieve pattern for small primes
 *============================================================================*/

static constexpr int PRESIEVE_PRIMES_COUNT = 5;
static const uint32_t presieve_primes[PRESIEVE_PRIMES_COUNT] = {3, 5, 7, 11, 13};
static constexpr uint32_t PRESIEVE_PERIOD = 15015;  // 3*5*7*11*13

/*============================================================================
 * SegmentedSieve Implementation
 *============================================================================*/

SegmentedSieve::SegmentedSieve(uint64_t n_primes, uint64_t total_sieve_size) {
    this->n_primes = n_primes;
    this->total_size = round_up(total_sieve_size, SEGMENT_SIZE_BITS);
    this->total_segments = this->total_size / SEGMENT_SIZE_BITS;
    this->current_segment = 0;
    this->hash_initialized = false;

    // Allocate segment buffer (L1 cache sized)
    segment_words = SEGMENT_SIZE_BITS / 64;
    segment = static_cast<uint64_t*>(aligned_alloc(64, segment_words * sizeof(uint64_t)));
    if (!segment) {
        throw std::runtime_error("SegmentedSieve: failed to allocate segment buffer");
    }

    // Allocate prime tables
    primes = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * n_primes));
    primes2 = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * n_primes));
    if (!primes || !primes2) {
        if (primes) std::free(primes);
        if (primes2) std::free(primes2);
        aligned_free(segment);
        throw std::runtime_error("SegmentedSieve: failed to allocate prime tables");
    }

    // Initialize primes
    init_primes(n_primes);

    // Determine small prime limit (primes that directly sieve all segments)
    small_prime_limit = 0;
    for (uint64_t i = 0; i < n_primes; i++) {
        if (primes2[i] > SEGMENT_SIZE_BITS) {
            small_prime_limit = i;
            break;
        }
    }
    if (small_prime_limit == 0) small_prime_limit = n_primes;

    // Allocate large prime starts
    if (small_prime_limit < n_primes) {
        large_prime_starts = static_cast<uint32_t*>(
            std::malloc(sizeof(uint32_t) * (n_primes - small_prime_limit)));
    } else {
        large_prime_starts = nullptr;
    }

    // Initialize pre-sieve pattern
    init_presieve();

    // Initialize buckets
    init_buckets();

    // Initialize GMP variable
    mpz_init(mpz_start);
}

SegmentedSieve::~SegmentedSieve() {
    aligned_free(segment);
    std::free(primes);
    std::free(primes2);
    if (large_prime_starts) std::free(large_prime_starts);
    if (presieve_pattern) std::free(presieve_pattern);
    mpz_clear(mpz_start);
}

void SegmentedSieve::init_primes(uint64_t n) {
    // Upper bound for nth prime: p_n < n * (ln(n) + ln(ln(n))) for n >= 6
    uint64_t limit = static_cast<uint64_t>(n * (std::log(n) + std::log(std::log(n)))) + 100;
    limit = round_up(limit, 64);

    // Simple sieve to generate small primes
    uint64_t* sieve_buf = static_cast<uint64_t*>(std::calloc(limit / 64 + 1, sizeof(uint64_t)));
    uint64_t sqrt_limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(limit))) + 1;

    // Mark composites
    for (uint64_t i = 3; i <= sqrt_limit; i += 2) {
        if (!test_bit64(sieve_buf, i >> 1)) {
            for (uint64_t j = i * i; j < limit; j += i * 2) {
                set_bit64(sieve_buf, j >> 1);
            }
        }
    }

    // Extract primes
    primes[0] = 2;
    primes2[0] = 4;
    uint64_t count = 1;
    for (uint64_t i = 3; count < n && i < limit; i += 2) {
        if (!test_bit64(sieve_buf, i >> 1)) {
            primes[count] = i;
            primes2[count] = i * 2;
            count++;
        }
    }

    std::free(sieve_buf);
    this->n_primes = count;
}

void SegmentedSieve::init_presieve() {
    // Initialize SIMD presieve tables
    presieve_generate_tables();

    // Allocate fallback pattern buffer
    presieve_period = PRESIEVE_PERIOD;
    uint64_t pattern_words = (presieve_period + 63) / 64;
    presieve_pattern = static_cast<uint64_t*>(std::calloc(pattern_words, sizeof(uint64_t)));

    // Generate fallback pattern for primes 3, 5, 7, 11, 13
    for (int p = 0; p < PRESIEVE_PRIMES_COUNT; p++) {
        uint32_t prime = presieve_primes[p];
        for (uint64_t i = prime; i < presieve_period; i += prime * 2) {
            if (i & 1) {
                set_bit64(presieve_pattern, i >> 1);
            }
        }
    }
}

void SegmentedSieve::init_buckets() {
    buckets.resize(2);
    for (auto& bucket : buckets) {
        bucket.entries.reserve(1024);
    }
}

void SegmentedSieve::init_for_hash(mpz_t start) {
    mpz_set(mpz_start, start);
    current_segment = 0;
    hash_initialized = true;

    // Calculate starting positions for all primes
    calc_starts();

    // Clear buckets
    for (auto& bucket : buckets) {
        bucket.entries.clear();
    }

    // Initialize buckets with large primes
    if (large_prime_starts) {
        for (uint64_t i = small_prime_limit; i < n_primes; i++) {
            uint32_t start_pos = large_prime_starts[i - small_prime_limit];
            if (start_pos < SEGMENT_SIZE_BITS) {
                BucketEntry entry;
                entry.prime_idx = i;
                entry.next_hit = start_pos;
                buckets[0].entries.push_back(entry);
            } else {
                uint32_t seg_idx = start_pos / SEGMENT_SIZE_BITS;
                if (seg_idx < buckets.size()) {
                    BucketEntry entry;
                    entry.prime_idx = i;
                    entry.next_hit = start_pos % SEGMENT_SIZE_BITS;
                    buckets[seg_idx].entries.push_back(entry);
                }
            }
        }
    }

    stats.sieve_runs++;
}

void SegmentedSieve::calc_starts() {
    uint64_t start_time = get_time_usec();

    for (uint64_t i = 0; i < n_primes; i++) {
        uint32_t p = primes[i];
        uint32_t remainder = mpz_tdiv_ui(mpz_start, p);
        uint32_t start_idx = (remainder == 0) ? 0 : (p - remainder);

        // Ensure start_idx is odd
        if ((start_idx & 1) == 0) {
            start_idx += p;
        }

        if (i >= small_prime_limit) {
            large_prime_starts[i - small_prime_limit] = start_idx;
        }
    }

    stats.time_sieving_us += get_time_usec() - start_time;
}

bool SegmentedSieve::sieve_next_segment() {
    if (!hash_initialized || current_segment >= total_segments) {
        return false;
    }

    uint64_t start_time = get_time_usec();
    uint64_t seg_start = current_segment * SEGMENT_SIZE_BITS;

    // SIMD Pre-sieve (primes up to 163)
    if (presieve_tables_ready()) {
        size_t sieve_bytes = segment_words * sizeof(uint64_t);
        presieve_full(reinterpret_cast<uint8_t*>(segment), sieve_bytes, seg_start / 2);
    } else {
        // Fallback to old pattern-based presieve
        uint64_t pattern_offset = (seg_start / 2) % presieve_period;
        uint64_t pattern_words_count = (presieve_period + 63) / 64;

        for (uint64_t w = 0; w < segment_words; w++) {
            uint64_t bit_pos = (w * 64 + pattern_offset) % presieve_period;
            uint64_t word_idx = bit_pos / 64;
            uint64_t bit_offset = bit_pos % 64;

            if (bit_offset == 0 && word_idx < pattern_words_count) {
                segment[w] = presieve_pattern[word_idx];
            } else {
                uint64_t lo = presieve_pattern[word_idx % pattern_words_count];
                uint64_t hi = presieve_pattern[(word_idx + 1) % pattern_words_count];
                segment[w] = (lo >> bit_offset) | (hi << (64 - bit_offset));
            }
        }
    }

    // Sieve small primes directly
    sieve_small_primes();

    // Process bucket for this segment
    process_bucket();

    // Advance to next segment
    current_segment++;

    // Advance buckets
    if (current_segment < total_segments) {
        advance_buckets();
    }

    stats.time_sieving_us += get_time_usec() - start_time;

    return current_segment < total_segments;
}

void SegmentedSieve::sieve_small_primes() {
    uint64_t seg_start = current_segment * SEGMENT_SIZE_BITS;

    // Start index based on presieve coverage
    uint64_t start_idx;
    if (presieve_tables_ready()) {
        start_idx = 38;  // SIMD presieve covers primes up to 163 (index 37)
    } else {
        start_idx = 6;   // Fallback presieve only covers primes up to 13 (index 5)
    }

    for (uint64_t i = start_idx; i < small_prime_limit; i++) {
        uint32_t p = primes[i];
        uint32_t p2 = primes2[i];

        // Calculate starting position in this segment
        uint32_t remainder = mpz_tdiv_ui(mpz_start, p);
        uint32_t start_pos = (remainder == 0) ? 0 : (p - remainder);

        // Adjust for segment offset
        if (start_pos < seg_start) {
            uint64_t diff = seg_start - start_pos;
            start_pos = (diff % p2 == 0) ? 0 : (p2 - (diff % p2));
        } else {
            start_pos -= seg_start;
        }

        // Ensure odd
        if ((start_pos & 1) == 0) {
            start_pos += p;
        }

        // Sieve all multiples in this segment
        for (uint32_t pos = start_pos; pos < SEGMENT_SIZE_BITS; pos += p2) {
            set_bit64(segment, pos);
        }
    }
}

void SegmentedSieve::process_bucket() {
    if (buckets.empty()) return;

    Bucket& bucket = buckets[0];

    for (auto& entry : bucket.entries) {
        uint32_t p = primes[entry.prime_idx];
        uint32_t p2 = primes2[entry.prime_idx];
        uint32_t pos = entry.next_hit;

        // Mark position as composite
        if (pos < SEGMENT_SIZE_BITS) {
            set_bit64(segment, pos);
        }

        // Calculate next hit
        uint32_t next = pos + p2;

        if (next < SEGMENT_SIZE_BITS) {
            entry.next_hit = next;
        } else {
            next -= SEGMENT_SIZE_BITS;
            uint32_t seg_idx = next / SEGMENT_SIZE_BITS + 1;
            if (seg_idx < buckets.size()) {
                BucketEntry new_entry;
                new_entry.prime_idx = entry.prime_idx;
                new_entry.next_hit = next % SEGMENT_SIZE_BITS;
                buckets[seg_idx].entries.push_back(new_entry);
            }
        }
    }

    bucket.entries.clear();
}

void SegmentedSieve::advance_buckets() {
    if (buckets.size() >= 2) {
        std::swap(buckets[0], buckets[1]);
        buckets[1].entries.clear();
    }
}

void SegmentedSieve::get_candidates(std::vector<uint64_t>& candidates, uint64_t base_mod) {
    candidates.clear();
    uint64_t seg_start = (current_segment > 0) ? (current_segment - 1) * SEGMENT_SIZE_BITS : 0;

    for (uint64_t i = 1; i < SEGMENT_SIZE_BITS; i += 2) {
        if (!test_bit64(segment, i)) {
            uint64_t pos = seg_start + i;
            uint64_t pos_mod = (base_mod + pos) % 2310;
            if (is_coprime_2310(pos_mod)) {
                candidates.push_back(pos);
            }
        }
    }
}

bool SegmentedSieve::is_composite(uint64_t pos) const {
    return test_bit64(segment, pos);
}

uint64_t SegmentedSieve::get_segment_offset() const {
    return (current_segment > 0) ? (current_segment - 1) * SEGMENT_SIZE_BITS : 0;
}

uint64_t SegmentedSieve::get_total_size() const {
    return total_size;
}

/*============================================================================
 * PrimalityTester Implementation
 *============================================================================*/

PrimalityTester::PrimalityTester() {
    mpz_init(mpz_tmp);
    mpz_init(mpz_d);
    mpz_init(mpz_s);
    mpz_init(mpz_a);
    mpz_init(mpz_x);
    mpz_init_set_ui(mpz_two, 2);
    mpz_init(mpz_n_minus_1);
    mpz_init(mpz_u);
    mpz_init(mpz_v);
    mpz_init(mpz_q);
}

PrimalityTester::~PrimalityTester() {
    mpz_clear(mpz_tmp);
    mpz_clear(mpz_d);
    mpz_clear(mpz_s);
    mpz_clear(mpz_a);
    mpz_clear(mpz_x);
    mpz_clear(mpz_two);
    mpz_clear(mpz_n_minus_1);
    mpz_clear(mpz_u);
    mpz_clear(mpz_v);
    mpz_clear(mpz_q);
}

bool PrimalityTester::bpsw_test(mpz_t n) {
    // Handle small cases
    if (mpz_cmp_ui(n, 2) < 0) return false;
    if (mpz_cmp_ui(n, 2) == 0) return true;
    if (mpz_even_p(n)) return false;

    // Trial division by small primes
    static const unsigned long small_primes[] = {
        3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
        59, 61, 67, 71, 73, 79, 83, 89, 97
    };
    for (int i = 0; i < 24; i++) {
        if (mpz_cmp_ui(n, small_primes[i]) == 0) return true;
        if (mpz_divisible_ui_p(n, small_primes[i])) return false;
    }

    // Miller-Rabin base 2
    if (!miller_rabin(n, 2)) return false;

    // Strong Lucas-Selfridge test
    if (!strong_lucas_selfridge(n)) return false;

    return true;
}

bool PrimalityTester::miller_rabin(mpz_t n, unsigned long base) {
    // n - 1 = d * 2^s
    mpz_sub_ui(mpz_n_minus_1, n, 1);
    mpz_set(mpz_d, mpz_n_minus_1);

    unsigned long s = 0;
    while (mpz_even_p(mpz_d)) {
        mpz_tdiv_q_2exp(mpz_d, mpz_d, 1);
        s++;
    }

    // x = base^d mod n
    mpz_set_ui(mpz_a, base);
    mpz_powm(mpz_x, mpz_a, mpz_d, n);

    // Check if x == 1 or x == n-1
    if (mpz_cmp_ui(mpz_x, 1) == 0) return true;
    if (mpz_cmp(mpz_x, mpz_n_minus_1) == 0) return true;

    // Square s-1 times
    for (unsigned long r = 1; r < s; r++) {
        mpz_powm_ui(mpz_x, mpz_x, 2, n);
        if (mpz_cmp(mpz_x, mpz_n_minus_1) == 0) return true;
        if (mpz_cmp_ui(mpz_x, 1) == 0) return false;
    }

    return false;
}

long PrimalityTester::find_selfridge_d(mpz_t n) {
    long d = 5;
    long sign = 1;

    while (true) {
        mpz_set_si(mpz_tmp, d);
        int jacobi = mpz_jacobi(mpz_tmp, n);

        if (jacobi == -1) return d;
        if (jacobi == 0) {
            if (mpz_cmpabs_ui(n, std::abs(d)) != 0) return 0;
        }

        sign = -sign;
        d = sign * (std::abs(d) + 2);

        if (std::abs(d) > 1000000) {
            return 0;
        }
    }
}

bool PrimalityTester::strong_lucas_selfridge(mpz_t n) {
    // Check for perfect square
    if (mpz_perfect_square_p(n)) return false;

    // Find Selfridge parameter D
    long d_param = find_selfridge_d(n);
    if (d_param == 0) return false;

    // P = 1, Q = (1 - D) / 4
    long p_param = 1;
    long q_param = (1 - d_param) / 4;

    // Calculate n + 1 = d * 2^s where d is odd
    mpz_add_ui(mpz_tmp, n, 1);
    mpz_set(mpz_d, mpz_tmp);

    unsigned long s = 0;
    while (mpz_even_p(mpz_d)) {
        mpz_tdiv_q_2exp(mpz_d, mpz_d, 1);
        s++;
    }

    // Compute Lucas U_d and V_d
    mpz_set_ui(mpz_u, 1);
    mpz_set_si(mpz_v, p_param);
    mpz_set_si(mpz_q, q_param);

    size_t d_bits = mpz_sizeinbase(mpz_d, 2);

    mpz_t u_k, v_k, q_k;
    mpz_init_set_ui(u_k, 1);
    mpz_init_set_si(v_k, p_param);
    mpz_init_set_si(q_k, q_param);

    for (long i = d_bits - 2; i >= 0; i--) {
        // Double
        mpz_mul(mpz_tmp, u_k, v_k);
        mpz_mod(u_k, mpz_tmp, n);

        mpz_mul(mpz_tmp, v_k, v_k);
        mpz_submul_ui(mpz_tmp, q_k, 2);
        mpz_mod(v_k, mpz_tmp, n);

        mpz_mul(q_k, q_k, q_k);
        mpz_mod(q_k, q_k, n);

        // If bit is set, increment
        if (mpz_tstbit(mpz_d, i)) {
            // U_{k+1} = (P*U_k + V_k) / 2
            mpz_mul_si(mpz_tmp, u_k, p_param);
            mpz_add(mpz_tmp, mpz_tmp, v_k);
            if (mpz_odd_p(mpz_tmp)) mpz_add(mpz_tmp, mpz_tmp, n);
            mpz_tdiv_q_2exp(mpz_u, mpz_tmp, 1);
            mpz_mod(mpz_u, mpz_u, n);

            // V_{k+1} = (D*U_k + P*V_k) / 2
            mpz_mul_si(mpz_tmp, u_k, d_param);
            mpz_addmul_ui(mpz_tmp, v_k, static_cast<unsigned long>(p_param));
            if (mpz_odd_p(mpz_tmp)) mpz_add(mpz_tmp, mpz_tmp, n);
            mpz_tdiv_q_2exp(mpz_v, mpz_tmp, 1);
            mpz_mod(v_k, mpz_v, n);

            mpz_set(u_k, mpz_u);

            mpz_mul_si(q_k, q_k, q_param);
            mpz_mod(q_k, q_k, n);
        }
    }

    // Check U_d = 0 (mod n)
    if (mpz_sgn(u_k) == 0) {
        mpz_clear(u_k);
        mpz_clear(v_k);
        mpz_clear(q_k);
        return true;
    }

    // Check V_{d*2^r} = 0 (mod n) for r = 0, 1, ..., s-1
    if (mpz_sgn(v_k) == 0) {
        mpz_clear(u_k);
        mpz_clear(v_k);
        mpz_clear(q_k);
        return true;
    }

    for (unsigned long r = 1; r < s; r++) {
        mpz_mul(mpz_tmp, v_k, v_k);
        mpz_submul_ui(mpz_tmp, q_k, 2);
        mpz_mod(v_k, mpz_tmp, n);

        if (mpz_sgn(v_k) == 0) {
            mpz_clear(u_k);
            mpz_clear(v_k);
            mpz_clear(q_k);
            return true;
        }

        mpz_mul(q_k, q_k, q_k);
        mpz_mod(q_k, q_k, n);
    }

    mpz_clear(u_k);
    mpz_clear(v_k);
    mpz_clear(q_k);
    return false;
}

bool PrimalityTester::fermat_test(mpz_t n) {
    mpz_sub_ui(mpz_tmp, n, 1);
    mpz_powm(mpz_x, mpz_two, mpz_tmp, n);
    return mpz_cmp_ui(mpz_x, 1) == 0;
}

void PrimalityTester::prepare_batch(CandidateBatch& batch,
                                     mpz_t mpz_start,
                                     const std::vector<uint64_t>& candidates,
                                     uint32_t max_bits) {
    batch.bits = max_bits;
    batch.count = candidates.size();
    uint32_t limbs = (max_bits + 31) / 32;

    batch.candidates.resize(batch.count * limbs);
    batch.indices.resize(batch.count);

    for (size_t i = 0; i < candidates.size(); i++) {
        mpz_add_ui(mpz_tmp, mpz_start, candidates[i]);

        size_t count;
        mpz_export(&batch.candidates[i * limbs], &count, -1, sizeof(uint32_t),
                   0, 0, mpz_tmp);

        for (size_t j = count; j < limbs; j++) {
            batch.candidates[i * limbs + j] = 0;
        }

        batch.indices[i] = candidates[i];
    }
}

/*============================================================================
 * MiningPipeline Implementation
 *============================================================================*/

MiningPipeline::MiningPipeline(MiningTier tier, uint32_t n_threads)
    : tier(tier), n_threads(n_threads), current_pow(nullptr), processor(nullptr),
      last_prime_offset(UINT64_MAX), have_first_prime(false), min_gap(0),
      gap_state_initialized(false) {
    mpz_init(mpz_start);
    mpz_init(mpz_adder);
}

MiningPipeline::~MiningPipeline() {
    stop_mining();
    mpz_clear(mpz_start);
    mpz_clear(mpz_adder);
}

void MiningPipeline::init_gap_state(mpz_t start, uint64_t target_gap) {
    std::lock_guard<std::mutex> lock(gap_mutex);
    mpz_set(mpz_start, start);
    min_gap = target_gap;
    last_prime_offset = UINT64_MAX;
    have_first_prime = false;
    gap_state_initialized = true;
}

void MiningPipeline::reset_gap_state() {
    std::lock_guard<std::mutex> lock(gap_mutex);
    last_prime_offset = UINT64_MAX;
    have_first_prime = false;
    gap_state_initialized = false;
}

void MiningPipeline::set_processor(PoWProcessor* processor) {
    this->processor = processor;
}

void MiningPipeline::start_mining(PoW* pow, std::vector<uint8_t>* offset) {
    if (mining_active) stop_mining();

    current_pow = pow;
    mining_active = true;
    shutdown_requested = false;
    stats.reset();

    // Start sieve workers
    for (uint32_t i = 0; i < n_threads; i++) {
        sieve_threads.emplace_back(&MiningPipeline::sieve_worker, this, i);
    }

    // Start GPU worker if applicable
    if (tier == MiningTier::CPU_CUDA || tier == MiningTier::CPU_OPENCL) {
        gpu_thread = std::thread(&MiningPipeline::gpu_worker, this);
    }
}

void MiningPipeline::stop_mining() {
    if (!mining_active) return;

    shutdown_requested = true;
    mining_active = false;

    // Wake up GPU worker
    queue_cv.notify_all();

    // Wait for threads
    for (auto& t : sieve_threads) {
        if (t.joinable()) t.join();
    }
    sieve_threads.clear();

    if (gpu_thread.joinable()) {
        gpu_thread.join();
    }
}

void MiningPipeline::wait_for_completion() {
    for (auto& t : sieve_threads) {
        if (t.joinable()) t.join();
    }
    sieve_threads.clear();

    if (gpu_thread.joinable()) {
        gpu_thread.join();
    }

    mining_active = false;
}

MiningStatsSnapshot MiningPipeline::get_stats() const {
    return stats.snapshot();
}

void MiningPipeline::sieve_worker(uint32_t thread_id) {
    (void)thread_id;  // Suppress unused warning

    const uint64_t n_primes = 100000;
    PrimalityTester tester;

    mpz_t local_mpz_start, mpz_candidate, local_mpz_adder;
    mpz_init(local_mpz_start);
    mpz_init(mpz_candidate);
    mpz_init(local_mpz_adder);

    SegmentedSieve* sieve = nullptr;
    uint16_t last_shift = 0;
    uint64_t sieve_size = 0;

    while (mining_active && !shutdown_requested) {
        // Get hash from current PoW
        current_pow->get_hash(local_mpz_start);
        uint16_t shift = current_pow->get_shift();
        mpz_mul_2exp(local_mpz_start, local_mpz_start, shift);

        // Sieve the full valid adder range: [0, 2^shift)
        uint64_t max_offset = (1ULL << shift);
        sieve_size = (max_offset < 33554432) ? max_offset : 33554432;

        // Recreate sieve if shift changed
        if (shift != last_shift) {
            delete sieve;
            sieve = nullptr;
            try {
                sieve = new SegmentedSieve(n_primes, sieve_size);
            } catch (const std::exception& e) {
                mpz_clear(local_mpz_start);
                mpz_clear(mpz_candidate);
                mpz_clear(local_mpz_adder);
                return;
            }
            last_shift = shift;
        }

        // Initialize sieve for this hash
        sieve->init_for_hash(local_mpz_start);

        uint64_t base_mod = mpz_tdiv_ui(local_mpz_start, 2310);

        // Get target gap size for this difficulty
        uint64_t local_min_gap = current_pow->target_size(local_mpz_start);
        if (local_min_gap & 1) local_min_gap++;

        // Track last confirmed prime for gap detection
        uint64_t local_last_prime_offset = UINT64_MAX;
        bool local_have_first_prime = false;

        // Initialize gap state for GPU path
        if (tier != MiningTier::CPU_ONLY) {
            init_gap_state(local_mpz_start, local_min_gap);
        }

        // Process segments
        while (sieve->sieve_next_segment() && mining_active) {
            std::vector<uint64_t> candidates;
            sieve->get_candidates(candidates, base_mod);

            if (tier == MiningTier::CPU_ONLY) {
                // Test candidates directly on CPU using BPSW
                for (uint64_t offset : candidates) {
                    mpz_add_ui(mpz_candidate, local_mpz_start, offset);

                    // Primorial GCD pre-filter
                    if (mpz_gcd_ui(nullptr, mpz_candidate, PRIMORIAL_23) != 1) {
                        continue;
                    }

                    stats.tests_performed++;

                    if (tester.bpsw_test(mpz_candidate)) {
                        stats.primes_found++;

                        if (!local_have_first_prime) {
                            local_last_prime_offset = offset;
                            local_have_first_prime = true;
                        } else {
                            uint64_t gap = offset - local_last_prime_offset;

                            if (gap >= local_min_gap) {
                                mpz_set_ui(local_mpz_adder, local_last_prime_offset);
                                current_pow->set_adder(local_mpz_adder);

                                if (current_pow->valid()) {
                                    stats.gaps_found++;
                                    if (processor) {
                                        bool continue_mining = processor->process(current_pow);
                                        if (!continue_mining) {
                                            mining_active = false;
                                            delete sieve;
                                            mpz_clear(local_mpz_start);
                                            mpz_clear(mpz_candidate);
                                            mpz_clear(local_mpz_adder);
                                            return;
                                        }
                                    }
                                }
                            }
                            local_last_prime_offset = offset;
                        }
                    }
                }
            } else {
                // Queue candidates for GPU
                if (!candidates.empty()) {
                    CandidateBatch batch;
                    tester.prepare_batch(batch, local_mpz_start, candidates, 320);

                    std::lock_guard<std::mutex> lock(queue_mutex);
                    gpu_queue.push(std::move(batch));
                    queue_cv.notify_one();
                }
            }
        }
    }

    delete sieve;
    mpz_clear(local_mpz_start);
    mpz_clear(mpz_candidate);
    mpz_clear(local_mpz_adder);
}

void MiningPipeline::gpu_worker() {
#ifdef HAVE_CUDA
    if (tier == MiningTier::CPU_CUDA) {
        cuda_fermat_init(0);
    }
#endif
#ifdef HAVE_OPENCL
    if (tier == MiningTier::CPU_OPENCL) {
        opencl_fermat_init(0);
    }
#endif

    while (mining_active && !shutdown_requested) {
        CandidateBatch batch;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this] {
                return !gpu_queue.empty() || shutdown_requested;
            });

            if (shutdown_requested && gpu_queue.empty()) break;

            batch = std::move(gpu_queue.front());
            gpu_queue.pop();
        }

        // Run GPU Fermat test
        std::vector<uint8_t> results(batch.count);

#ifdef HAVE_CUDA
        if (tier == MiningTier::CPU_CUDA) {
            cuda_fermat_batch(results.data(), batch.candidates.data(),
                              batch.count, batch.bits);
        }
#endif
#ifdef HAVE_OPENCL
        if (tier == MiningTier::CPU_OPENCL) {
            opencl_fermat_batch(results.data(), batch.candidates.data(),
                                batch.count, batch.bits);
        }
#endif

        // Process results
        process_gpu_results(batch, results);
    }

#ifdef HAVE_OPENCL
    if (tier == MiningTier::CPU_OPENCL) {
        opencl_fermat_cleanup();
    }
#endif
}

void MiningPipeline::process_gpu_results(const CandidateBatch& batch,
                                          const std::vector<uint8_t>& results) {
    std::vector<uint64_t> prime_offsets;
    prime_offsets.reserve(batch.count / 10);

    for (size_t i = 0; i < batch.count; i++) {
        if (results[i]) {
            stats.primes_found++;
            prime_offsets.push_back(batch.indices[i]);
        }
    }
    stats.tests_performed += batch.count;

    if (prime_offsets.empty()) return;

    std::lock_guard<std::mutex> lock(gap_mutex);

    if (!gap_state_initialized) return;

    for (uint64_t offset : prime_offsets) {
        if (!have_first_prime) {
            last_prime_offset = offset;
            have_first_prime = true;
        } else {
            uint64_t gap = offset - last_prime_offset;

            if (gap >= min_gap) {
                mpz_set_ui(mpz_adder, last_prime_offset);
                current_pow->set_adder(mpz_adder);

                if (current_pow->valid()) {
                    stats.gaps_found++;
                    if (processor) {
                        bool continue_mining = processor->process(current_pow);
                        if (!continue_mining) {
                            mining_active = false;
                            return;
                        }
                    }
                }
            }
            last_prime_offset = offset;
        }
    }
}

/*============================================================================
 * MiningEngine Implementation
 *============================================================================*/

MiningEngine::MiningEngine()
    : pipeline(nullptr) {
    n_threads = std::thread::hardware_concurrency();
    tier = detect_tier();
    pipeline = std::make_unique<MiningPipeline>(tier, 1);
}

MiningEngine::MiningEngine(MiningTier tier)
    : tier(tier), pipeline(nullptr) {
    n_threads = std::thread::hardware_concurrency();
    pipeline = std::make_unique<MiningPipeline>(tier, 1);
    detect_tier();  // Just to populate hardware_info
}

MiningEngine::~MiningEngine() {
    stop();
}

MiningTier MiningEngine::detect_tier() {
#ifdef HAVE_CUDA
    int cuda_devices = cuda_get_device_count();
    if (cuda_devices > 0) {
        std::snprintf(hardware_info, sizeof(hardware_info),
                 "CUDA: %d device(s) - %s",
                 cuda_devices, cuda_get_device_name(0));
        return MiningTier::CPU_CUDA;
    }
#endif

#ifdef HAVE_OPENCL
    int opencl_devices = opencl_get_device_count();
    if (opencl_devices > 0) {
        std::snprintf(hardware_info, sizeof(hardware_info),
                 "OpenCL: %d device(s) - %s",
                 opencl_devices, opencl_get_device_name(0));
        return MiningTier::CPU_OPENCL;
    }
#endif

    std::snprintf(hardware_info, sizeof(hardware_info),
             "CPU only: %u threads", n_threads);
    return MiningTier::CPU_ONLY;
}

void MiningEngine::run_sieve(PoW* pow, PoWProcessor* processor,
                             std::vector<uint8_t>* offset) {
    pipeline->set_processor(processor);
    pipeline->start_mining(pow, offset);
    pipeline->wait_for_completion();
}

void MiningEngine::mine_parallel(const std::vector<uint8_t>& header_template,
                                  size_t nonce_offset,
                                  uint16_t shift,
                                  uint64_t target_difficulty,
                                  uint32_t start_nonce,
                                  PoWProcessor* processor) {
    if (parallel_mining_active) {
        stop();
    }

    parallel_mining_active = true;
    stop_requested = false;
    mining_threads.clear();

    // Launch worker threads
    for (uint32_t i = 0; i < n_threads; i++) {
        mining_threads.emplace_back(&MiningEngine::parallel_worker, this,
                                     i, header_template, nonce_offset,
                                     shift, target_difficulty, start_nonce,
                                     processor);
    }

    // Wait for all threads to complete
    for (auto& t : mining_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    mining_threads.clear();
    parallel_mining_active = false;
}

void MiningEngine::parallel_worker(uint32_t thread_id,
                                    std::vector<uint8_t> header_template,
                                    size_t nonce_offset,
                                    uint16_t shift,
                                    uint64_t target_difficulty,
                                    uint32_t start_nonce,
                                    PoWProcessor* processor) {
    const uint64_t n_primes = 100000;
    const uint64_t sieve_size = std::min(1ULL << shift, 33554432ULL);

    SegmentedSieve sieve(n_primes, sieve_size);
    PrimalityTester tester;

    mpz_t mpz_hash, local_mpz_start, mpz_candidate, local_mpz_adder;
    mpz_init(mpz_hash);
    mpz_init(local_mpz_start);
    mpz_init(mpz_candidate);
    mpz_init(local_mpz_adder);

    uint32_t nonce = start_nonce + thread_id;

    while (!stop_requested) {
        // Write nonce into header (little-endian)
        header_template[nonce_offset + 0] = (nonce >> 0) & 0xFF;
        header_template[nonce_offset + 1] = (nonce >> 8) & 0xFF;
        header_template[nonce_offset + 2] = (nonce >> 16) & 0xFF;
        header_template[nonce_offset + 3] = (nonce >> 24) & 0xFF;

        // Compute hash = SHA256(SHA256(header)) using Bitcoin Core's CSHA256
        uint8_t hash1[CSHA256::OUTPUT_SIZE];
        uint8_t hash2[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(header_template.data(), header_template.size()).Finalize(hash1);
        CSHA256().Write(hash1, CSHA256::OUTPUT_SIZE).Finalize(hash2);

        // Convert hash to mpz
        mpz_import(mpz_hash, CSHA256::OUTPUT_SIZE, -1, 1, -1, 0, hash2);

        // Check hash is in valid range (2^255 < hash < 2^256)
        size_t hash_bits = mpz_sizeinbase(mpz_hash, 2);
        if (hash_bits != 256) {
            nonce += n_threads;
            continue;
        }

        // Compute start = hash << shift
        mpz_mul_2exp(local_mpz_start, mpz_hash, shift);

        // Initialize sieve for this hash
        sieve.init_for_hash(local_mpz_start);

        uint64_t base_mod = mpz_tdiv_ui(local_mpz_start, 2310);

        // Get target gap size for this difficulty
        PoWUtils utils;
        uint64_t local_min_gap = utils.target_size(local_mpz_start, target_difficulty);
        if (local_min_gap & 1) local_min_gap++;

        // Track consecutive primes for gap detection
        uint64_t local_last_prime_offset = UINT64_MAX;
        bool local_have_first_prime = false;

        // Process sieve segments
        while (sieve.sieve_next_segment() && !stop_requested) {
            std::vector<uint64_t> candidates;
            sieve.get_candidates(candidates, base_mod);

            for (uint64_t offset : candidates) {
                if (stop_requested) break;

                mpz_add_ui(mpz_candidate, local_mpz_start, offset);

                // Primorial GCD pre-filter
                if (mpz_gcd_ui(nullptr, mpz_candidate, PRIMORIAL_23) != 1) {
                    continue;
                }

                if (tester.bpsw_test(mpz_candidate)) {
                    if (!local_have_first_prime) {
                        local_last_prime_offset = offset;
                        local_have_first_prime = true;
                    } else {
                        uint64_t gap = offset - local_last_prime_offset;

                        if (gap >= local_min_gap) {
                            mpz_set_ui(local_mpz_adder, local_last_prime_offset);

                            // Create PoW object for verification
                            PoW pow(mpz_hash, shift, local_mpz_adder, target_difficulty, nonce);

                            if (pow.valid()) {
                                if (processor) {
                                    bool continue_mining = processor->process(&pow);
                                    if (!continue_mining) {
                                        stop_requested = true;
                                        goto cleanup;
                                    }
                                }
                            }
                        }
                        local_last_prime_offset = offset;
                    }
                }
            }
        }

        nonce += n_threads;
    }

cleanup:
    mpz_clear(mpz_hash);
    mpz_clear(local_mpz_start);
    mpz_clear(mpz_candidate);
    mpz_clear(local_mpz_adder);
}

void MiningEngine::stop() {
    stop_requested = true;
    if (pipeline) {
        pipeline->stop_mining();
    }
    for (auto& t : mining_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    mining_threads.clear();
    parallel_mining_active = false;
    stop_requested = false;
}

bool MiningEngine::is_mining() const {
    return parallel_mining_active || (pipeline && pipeline->is_mining());
}

MiningStatsSnapshot MiningEngine::get_stats() const {
    return pipeline->get_stats();
}
