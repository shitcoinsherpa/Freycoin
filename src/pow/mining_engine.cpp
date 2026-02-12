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
#include <pow/combined_sieve.h>
#include <pow/avx512_primality.h>
#include <pow/simd_presieve.h>
#include <crypto/sha256.h>
#include <logging.h>

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

#include <gpu/opencl_loader.h>
#include <gpu/opencl_fermat.h>

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

    // Allocate prime table
    primes = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * n_primes));
    if (!primes) {
        aligned_free(segment);
        throw std::runtime_error("SegmentedSieve: failed to allocate prime table");
    }

    // Initialize primes
    init_primes(n_primes);

    // Determine small prime limit: primes that can hit multiple times per segment.
    // In odd-only indexing, step is p, so a prime is "small" if p <= SEGMENT_SIZE_BITS.
    small_prime_limit = 0;
    for (uint64_t i = 0; i < n_primes; i++) {
        if (primes[i] > SEGMENT_SIZE_BITS) {
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

    // Allocate cached starting positions for small primes
    small_starts = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * n_primes));
    if (!small_starts) {
        throw std::runtime_error("SegmentedSieve: failed to allocate small_starts");
    }
    std::memset(small_starts, 0, sizeof(uint32_t) * n_primes);

    // Initialize SIMD presieve tables
    presieve_generate_tables();

    // Initialize buckets
    init_buckets();

    // Initialize GMP variable
    mpz_init(mpz_start);
}

SegmentedSieve::~SegmentedSieve() {
    aligned_free(segment);
    std::free(primes);
    if (large_prime_starts) std::free(large_prime_starts);
    if (small_starts) std::free(small_starts);
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
    uint64_t count = 1;
    for (uint64_t i = 3; count < n && i < limit; i += 2) {
        if (!test_bit64(sieve_buf, i >> 1)) {
            primes[count] = i;
            count++;
        }
    }

    std::free(sieve_buf);
    this->n_primes = count;
}

void SegmentedSieve::init_presieve() {
    presieve_generate_tables();
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

    // Calculate starting positions for all primes (odd-only indexing)
    calc_starts();

    // Compute presieve base offsets using GMP
    if (presieve_tables_ready()) {
        presieve_set_base_offsets(mpz_start);
    }

    // Clear buckets
    for (auto& bucket : buckets) {
        bucket.entries.clear();
    }

    // Initialize buckets with large primes
    if (large_prime_starts) {
        for (uint64_t i = small_prime_limit; i < n_primes; i++) {
            uint32_t start_pos = large_prime_starts[i - small_prime_limit];
            uint32_t seg_idx = start_pos / SEGMENT_SIZE_BITS;
            if (seg_idx < buckets.size()) {
                BucketEntry entry;
                entry.prime_idx = i;
                entry.next_hit = start_pos % SEGMENT_SIZE_BITS;
                buckets[seg_idx].entries.push_back(entry);
            }
        }
    }

    stats.sieve_runs++;
}

void SegmentedSieve::calc_starts() {
    uint64_t start_time = get_time_usec();

    // Skip prime 2 (index 0) — not relevant in odd-only sieve
    for (uint64_t i = 1; i < n_primes; i++) {
        uint32_t p = primes[i];
        uint32_t remainder = mpz_tdiv_ui(mpz_start, p);
        uint32_t offset = (remainder == 0) ? 0 : (p - remainder);

        // Ensure offset is odd (only odd multiples exist in odd-only sieve)
        if ((offset & 1) == 0) offset += p;

        // Convert integer offset to bit position: bit = offset / 2
        uint32_t bit_pos = offset / 2;

        small_starts[i] = bit_pos;
        if (i >= small_prime_limit) {
            large_prime_starts[i - small_prime_limit] = bit_pos;
        }
    }

    stats.time_sieving_us += get_time_usec() - start_time;
}

bool SegmentedSieve::sieve_next_segment() {
    if (!hash_initialized || current_segment >= total_segments) {
        return false;
    }

    uint64_t start_time = get_time_usec();

    // Initialize segment using SIMD presieve (eliminates ~70-80% of composites for primes 7-163)
    uint64_t seg_low = current_segment * (segment_words * sizeof(uint64_t));
    if (presieve_tables_ready()) {
        presieve_init(reinterpret_cast<uint8_t*>(segment), segment_words * sizeof(uint64_t), seg_low);
        presieve_apply(reinterpret_cast<uint8_t*>(segment), segment_words * sizeof(uint64_t), seg_low);
    } else {
        std::memset(segment, 0, segment_words * sizeof(uint64_t));
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

    // Return true: we successfully sieved a segment for the caller to process.
    // Next call will return false once current_segment >= total_segments.
    return true;
}

void SegmentedSieve::sieve_small_primes() {
    // Start from prime 3 (index 1). Index 0 is prime 2, not needed in odd-only sieve.
    // Presieve covers primes 7-163, but NOT 3 and 5. Re-sieving 7-163 is harmless.
    for (uint64_t i = 1; i < small_prime_limit; i++) {
        uint32_t p = primes[i];
        uint32_t start = small_starts[i];

        // If this prime's first hit is beyond this segment, advance for next time
        if (start >= SEGMENT_SIZE_BITS) {
            small_starts[i] = start - SEGMENT_SIZE_BITS;
            continue;
        }

        // Sieve all odd multiples of p in this segment (step = p in odd-only indexing)
        uint32_t pos = start;
        for (; pos < SEGMENT_SIZE_BITS; pos += p) {
            set_bit64(segment, pos);
        }

        // Save starting position for next segment
        small_starts[i] = pos - SEGMENT_SIZE_BITS;
    }
}

void SegmentedSieve::process_bucket() {
    if (buckets.empty()) return;

    Bucket& bucket = buckets[0];

    for (auto& entry : bucket.entries) {
        uint32_t p = primes[entry.prime_idx];
        uint32_t pos = entry.next_hit;

        // Mark position as composite
        if (pos < SEGMENT_SIZE_BITS) {
            set_bit64(segment, pos);
        }

        // In odd-only indexing, step between consecutive odd multiples is p
        uint32_t next = pos + p;

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

    // In odd-only indexing, bit k represents integer offset 2*(seg_start + k) + 1
    // Iterate all bits — each one is an odd number
    for (uint64_t k = 0; k < SEGMENT_SIZE_BITS; k++) {
        if (!test_bit64(segment, k)) {
            uint64_t integer_offset = 2 * (seg_start + k) + 1;
            uint64_t pos_mod = (base_mod + integer_offset) % 2310;
            if (is_coprime_2310(pos_mod)) {
                candidates.push_back(integer_offset);
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
    if (tier == MiningTier::CPU_OPENCL) {
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

    const uint64_t n_primes = DEFAULT_SIEVE_PRIMES;
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
        // Odd-only: each bit covers 2 integers, so half the bits cover the same range
        sieve_size = std::min((1ULL << shift) / 2, 16777216ULL);

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
                // Pre-filter with AVX-512 IFMA batch Fermat when available.
                // This tests 8 candidates simultaneously, then only runs full BPSW
                // on those that pass the Fermat pre-screen.
                // On CPUs without IFMA, falls through to direct BPSW testing.
                bool use_ifma = avx512_ifma_available() && candidates.size() >= 8;

                if (use_ifma) {
                    // Prepare batch for AVX-512 Fermat pre-filter
                    CandidateBatch ifma_batch;
                    tester.prepare_batch(ifma_batch, local_mpz_start, candidates, 320);

                    std::vector<uint8_t> fermat_results(ifma_batch.count, 0);
                    avx512_fermat_batch(fermat_results.data(), ifma_batch.candidates.data(),
                                        ifma_batch.count, ifma_batch.bits);

                    stats.tests_performed += ifma_batch.count;

                    // Only run full BPSW on candidates that passed Fermat
                    for (size_t ci = 0; ci < candidates.size(); ci++) {
                        if (!fermat_results[ci]) continue;

                        uint64_t offset = candidates[ci];
                        mpz_add_ui(mpz_candidate, local_mpz_start, offset);

                        // Full BPSW confirmation (consensus-grade)
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
                    continue; // Skip the scalar path below
                }

                // Scalar path: test candidates directly on CPU using BPSW
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
    if (tier == MiningTier::CPU_OPENCL) {
        opencl_fermat_init(0);
    }

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

        // Run OpenCL Fermat test
        std::vector<uint8_t> results(batch.count);
        opencl_fermat_batch(results.data(), batch.candidates.data(),
                            batch.count, batch.bits);

        // Process results
        process_gpu_results(batch, results);
    }

    if (tier == MiningTier::CPU_OPENCL) {
        opencl_fermat_cleanup();
    }
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

MiningEngine::MiningEngine(unsigned int num_threads)
    : pipeline(nullptr) {
    n_threads = (num_threads > 0) ? num_threads : std::thread::hardware_concurrency();
    tier = detect_tier();
    pipeline = std::make_unique<MiningPipeline>(tier, 1);
}

MiningEngine::MiningEngine(MiningTier tier)
    : tier(tier), pipeline(nullptr) {
    n_threads = std::thread::hardware_concurrency();
    pipeline = std::make_unique<MiningPipeline>(tier, 1);
    detect_tier();  // Just to populate hardware_info
}

MiningEngine::MiningEngine(MiningTier tier, unsigned int num_threads)
    : tier(tier), pipeline(nullptr) {
    n_threads = (num_threads > 0) ? num_threads : 1;
    pipeline = std::make_unique<MiningPipeline>(tier, 1);
    detect_tier();  // Just to populate hardware_info
}

MiningEngine::~MiningEngine() {
    stop();  // Stop sieve threads

    // Shut down all persistent GPU worker threads
    gpu_shutdown = true;
    for (auto& w : gpu_workers) {
        w->cv.notify_all();
    }
    for (auto& w : gpu_workers) {
        if (w->thread.joinable()) w->thread.join();
    }
    gpu_workers.clear();
    gpu_initialized = false;

    // Release all OpenCL devices
    if (tier == MiningTier::CPU_OPENCL) {
        opencl_fermat_cleanup_all();
    }
}

MiningTier MiningEngine::detect_tier() {
    // OpenCL: dynamically loaded at runtime, no SDK needed at build time
    // Works for all GPU vendors (NVIDIA, AMD, Intel)
    if (opencl_load() == 0) {
        int opencl_devices = opencl_get_device_count();
        if (opencl_devices > 0) {
            std::snprintf(hardware_info, sizeof(hardware_info),
                     "OpenCL: %d device(s) - %s",
                     opencl_devices, opencl_get_device_name(0));
            return MiningTier::CPU_OPENCL;
        }
    }

    if (avx512_ifma_available()) {
        std::snprintf(hardware_info, sizeof(hardware_info),
                 "CPU only: %u threads (AVX-512 IFMA)", n_threads);
    } else {
        std::snprintf(hardware_info, sizeof(hardware_info),
                 "CPU only: %u threads", n_threads);
    }
    return MiningTier::CPU_ONLY;
}

void MiningEngine::set_gpu_intensity(int intensity) {
    m_gpu_intensity = std::clamp(intensity, 1, 10);
}

/** Map GPU intensity (1-10) to sieve size cap.
 *  Higher intensity = larger sieve range per nonce = more candidates for GPU/CPU testing.
 */
static uint64_t intensity_to_sieve_cap(int intensity) {
    static constexpr uint64_t caps[] = {
         1048576,  //  1: 1M   (minimal — fast nonce iteration)
         2097152,  //  2: 2M
         4194304,  //  3: 4M
         8388608,  //  4: 8M
        16777216,  //  5: 16M  (default — balanced)
        25165824,  //  6: 24M
        33554432,  //  7: 32M
        50331648,  //  8: 48M
        67108864,  //  9: 64M
       134217728,  // 10: 128M (maximum — thorough search, high GPU load)
    };
    int idx = std::clamp(intensity, 1, 10) - 1;
    return caps[idx];
}

uint16_t MiningEngine::compute_shift(int intensity) {
    const uint64_t sieve_cap = intensity_to_sieve_cap(std::clamp(intensity, 1, 10));
    uint16_t shift = MIN_SHIFT;
    // Need 2^shift / 2 >= sieve_cap, i.e. 2^shift >= 2 * sieve_cap
    const uint64_t required = 2 * sieve_cap;
    while ((1ULL << shift) < required && shift < MAX_SHIFT) {
        shift++;
    }
    return shift;
}

void MiningEngine::run_sieve(PoW* pow, PoWProcessor* processor,
                             std::vector<uint8_t>* offset) {
    pipeline->set_processor(processor);
    pipeline->start_mining(pow, offset);
    pipeline->wait_for_completion();
}

void MiningEngine::gpu_worker_func(GPUWorker* worker) {
    int dev = worker->device_id;

    // Initialize this GPU device (once for the lifetime of the engine)
    if (opencl_fermat_init_device(dev) != 0) {
        LogPrintf("Mining: GPU worker %d failed to init OpenCL\n", dev);
        worker->initialized = false;
        return;
    }
    worker->initialized = true;
    gpu_initialized = true;  // At least one GPU is ready
    LogPrintf("Mining: GPU worker %d initialized (%s)\n", dev, opencl_get_device_name(dev));

    // Process batches until the engine is destroyed (gpu_shutdown).
    // Between mine_parallel calls the thread idles on the condition variable.
    while (!gpu_shutdown) {
        std::shared_ptr<GPURequest> request;
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->cv.wait(lock, [&] {
                return !worker->queue.empty() || gpu_shutdown.load();
            });
            if (gpu_shutdown && worker->queue.empty()) break;
            if (worker->queue.empty()) continue;
            request = worker->queue.front();
            worker->queue.pop();
        }

        // Run OpenCL Fermat primality pre-filter on THIS device
        opencl_fermat_batch_device(dev, request->results.data(),
                                   request->batch.candidates.data(),
                                   request->batch.count, request->batch.bits);

        // Signal the submitting CPU thread that results are ready
        {
            std::lock_guard<std::mutex> lock(request->mtx);
            request->done = true;
        }
        request->cv.notify_one();
    }

    LogPrintf("Mining: GPU worker %d stopped\n", dev);
}

void MiningEngine::submit_gpu_request(std::shared_ptr<GPURequest> request) {
    if (gpu_workers.empty()) return;

    // Round-robin across GPU workers
    int idx = gpu_round_robin.fetch_add(1, std::memory_order_relaxed) % (int)gpu_workers.size();
    GPUWorker* w = gpu_workers[idx].get();

    {
        std::lock_guard<std::mutex> lock(w->mutex);
        w->queue.push(request);
    }
    w->cv.notify_one();
}

void MiningEngine::ensure_gpu_running() {
    if (tier == MiningTier::CPU_ONLY) return;
    if (gpu_initialized.load()) return;  // Already running
    if (!gpu_workers.empty()) return;    // Workers exist, still initializing

    gpu_shutdown = false;
    num_gpu_devices = opencl_get_device_count();
    if (num_gpu_devices <= 0) {
        LogPrintf("Mining: No OpenCL devices found\n");
        return;
    }

    LogPrintf("Mining: Starting %d GPU worker thread(s)\n", num_gpu_devices);

    // Create one worker thread per GPU device
    for (int i = 0; i < num_gpu_devices; i++) {
        auto w = std::make_unique<GPUWorker>();
        w->device_id = i;
        w->thread = std::thread(&MiningEngine::gpu_worker_func, this, w.get());
        gpu_workers.push_back(std::move(w));
    }

    // Wait for at least one GPU to initialize (up to 30s)
    for (int i = 0; i < 300 && !gpu_initialized.load() && !gpu_shutdown.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Count how many actually initialized
    int ready = 0;
    for (auto& w : gpu_workers) {
        if (w->initialized.load()) ready++;
    }

    if (ready > 0) {
        LogPrintf("Mining: %d/%d GPU device(s) ready\n", ready, num_gpu_devices);
    } else {
        LogPrintf("Mining: WARNING - No GPU devices initialized, falling back to CPU\n");
    }
}

void MiningEngine::mine_parallel(const std::vector<uint8_t>& header_template,
                                  size_t nonce_offset,
                                  uint16_t shift,
                                  uint64_t target_difficulty,
                                  uint32_t start_nonce,
                                  PoWProcessor* processor) {
    if (parallel_mining_active) {
        // Stop previous sieve threads (GPU thread persists)
        stop_requested = true;
        for (auto& t : mining_threads) {
            if (t.joinable()) t.join();
        }
        mining_threads.clear();
        parallel_mining_active = false;
    }

    parallel_mining_active = true;
    stop_requested = false;
    par_primes = 0;
    par_gaps = 0;
    par_tests = 0;
    par_nonces = 0;
    mining_threads.clear();

    // Ensure shift allows full sieve range for the configured intensity
    uint16_t min_shift = compute_shift(m_gpu_intensity);
    if (shift < min_shift) {
        LogPrintf("Mining: Adjusting shift from %u to %u for intensity %d (sieve_cap=%llu)\n",
                  shift, min_shift, m_gpu_intensity,
                  (unsigned long long)intensity_to_sieve_cap(m_gpu_intensity));
        shift = min_shift;
    }

    // Ensure GPU thread is running (no-op if already initialized)
    ensure_gpu_running();

    // Launch CPU sieve worker threads
    for (uint32_t i = 0; i < n_threads; i++) {
        mining_threads.emplace_back(&MiningEngine::parallel_worker, this,
                                     i, header_template, nonce_offset,
                                     shift, target_difficulty, start_nonce,
                                     processor);
    }

    // Wait for all CPU sieve threads to complete
    for (auto& t : mining_threads) {
        if (t.joinable()) t.join();
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
    // Odd-only: each bit covers 2 integers, so half the bits cover the same range
    const uint64_t sieve_cap = intensity_to_sieve_cap(m_gpu_intensity);
    const uint64_t half_range = (1ULL << shift) / 2;
    const uint64_t sieve_size = (half_range < sieve_cap) ? half_range : sieve_cap;

    CombinedSegmentedSieve combined_sieve(DEFAULT_SIEVE_PRIMES, sieve_size);
    PrimalityTester tester;
    PoWUtils utils;

    // Per-interval state for gap detection
    struct GapState {
        mpz_t mpz_hash;
        mpz_t mpz_start;
        mpz_t mpz_candidate;
        mpz_t mpz_adder;
        uint32_t nonce;
        uint64_t min_gap;
        uint64_t last_prime_offset;
        bool have_first_prime;
        uint64_t primes_found;
        uint64_t valid_gaps;
        bool valid_hash;  // Hash passed entropy check
    };

    GapState states[COMBINED_SIEVE_BATCH];
    for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
        mpz_init(states[k].mpz_hash);
        mpz_init(states[k].mpz_start);
        mpz_init(states[k].mpz_candidate);
        mpz_init(states[k].mpz_adder);
    }

    // Nonce stride: each thread advances by n_threads per nonce
    // Within a batch, nonces are contiguous from this thread's perspective:
    // batch 0: [base, base+n_threads, base+2*n_threads, base+3*n_threads]
    uint32_t nonce_base = start_nonce + thread_id;

    while (!stop_requested) {
        // Reset sieve state for this batch of nonces
        combined_sieve.reset_segments();

        // Phase 1: Compute hashes for COMBINED_SIEVE_BATCH nonces
        int active_intervals = 0;
        for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
            uint32_t nonce = nonce_base + k * n_threads;
            states[k].nonce = nonce;
            states[k].last_prime_offset = UINT64_MAX;
            states[k].have_first_prime = false;
            states[k].primes_found = 0;
            states[k].valid_gaps = 0;
            states[k].valid_hash = false;

            // Write nonce into header (little-endian)
            header_template[nonce_offset + 0] = (nonce >> 0) & 0xFF;
            header_template[nonce_offset + 1] = (nonce >> 8) & 0xFF;
            header_template[nonce_offset + 2] = (nonce >> 16) & 0xFF;
            header_template[nonce_offset + 3] = (nonce >> 24) & 0xFF;

            // Compute hash = SHA256d(header)
            uint8_t hash1[CSHA256::OUTPUT_SIZE];
            uint8_t hash2[CSHA256::OUTPUT_SIZE];
            CSHA256().Write(header_template.data(), header_template.size()).Finalize(hash1);
            CSHA256().Write(hash1, CSHA256::OUTPUT_SIZE).Finalize(hash2);

            mpz_import(states[k].mpz_hash, CSHA256::OUTPUT_SIZE, -1, 1, -1, 0, hash2);

            // Check hash has minimum entropy
            size_t hash_bits = mpz_sizeinbase(states[k].mpz_hash, 2);
            if (hash_bits < 200) {
                combined_sieve.deactivate_interval(k);
                continue;
            }

            states[k].valid_hash = true;

            // Compute start = hash << shift
            mpz_mul_2exp(states[k].mpz_start, states[k].mpz_hash, shift);

            // Get target gap size
            states[k].min_gap = utils.target_size(states[k].mpz_start, target_difficulty);
            if (states[k].min_gap & 1) states[k].min_gap++;

            // Initialize this interval in the combined sieve
            combined_sieve.init_interval(k, states[k].mpz_start);
            active_intervals++;
        }

        if (active_intervals == 0) {
            nonce_base += COMBINED_SIEVE_BATCH * n_threads;
            continue;
        }

        // Phase 2: Sieve all intervals simultaneously
        // Determine if GPU is available for Fermat pre-filtering
        const bool use_gpu = gpu_initialized && (tier != MiningTier::CPU_ONLY);

        while (combined_sieve.sieve_next_segment() && !stop_requested) {
            // Phase 3: For each interval, extract candidates and test primality
            for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
                if (!states[k].valid_hash) continue;
                if (stop_requested) break;

                std::vector<uint64_t> candidates;
                combined_sieve.get_candidates(k, candidates);
                if (candidates.empty()) continue;

                // === GPU PATH: Fermat pre-filter on GPU, BPSW confirm on CPU ===
                if (use_gpu && candidates.size() >= 16) {
                    // Prepare batch for GPU
                    CandidateBatch batch;
                    tester.prepare_batch(batch, states[k].mpz_start, candidates, 320);

                    auto request = std::make_shared<GPURequest>();
                    request->batch = std::move(batch);
                    request->results.resize(request->batch.count, 0);

                    // Submit to a GPU worker (round-robin across devices)
                    submit_gpu_request(request);

                    // Wait for GPU to finish this batch
                    {
                        std::unique_lock<std::mutex> lock(request->mtx);
                        request->cv.wait(lock, [&]{ return request->done.load(); });
                    }

                    par_tests.fetch_add(candidates.size(), std::memory_order_relaxed);

                    // Only BPSW-confirm candidates that passed GPU Fermat
                    for (size_t ci = 0; ci < candidates.size(); ci++) {
                        if (stop_requested) break;
                        if (!request->results[ci]) continue;

                        uint64_t offset = candidates[ci];
                        mpz_add_ui(states[k].mpz_candidate, states[k].mpz_start, offset);

                        // Full BPSW confirmation (consensus-grade)
                        if (tester.bpsw_test(states[k].mpz_candidate)) {
                            states[k].primes_found++;
                            par_primes.fetch_add(1, std::memory_order_relaxed);

                            if (!states[k].have_first_prime) {
                                states[k].last_prime_offset = offset;
                                states[k].have_first_prime = true;
                            } else {
                                uint64_t gap = offset - states[k].last_prime_offset;

                                if (gap >= states[k].min_gap) {
                                    states[k].valid_gaps++;
                                    mpz_set_ui(states[k].mpz_adder, states[k].last_prime_offset);

                                    PoW pow(states[k].mpz_hash, shift, states[k].mpz_adder,
                                            target_difficulty, states[k].nonce);

                                    if (pow.valid()) {
                                        LogPrintf("Mining: VALID PROOF found! nonce=%u gap=%llu offset=%llu\n",
                                                  states[k].nonce, (unsigned long long)gap,
                                                  (unsigned long long)states[k].last_prime_offset);
                                        if (processor) {
                                            bool continue_mining = processor->process(&pow);
                                            if (!continue_mining) {
                                                stop_requested = true;
                                                goto cleanup;
                                            }
                                        }
                                    }
                                }
                                states[k].last_prime_offset = offset;
                            }
                        }
                    }
                    continue;  // Skip CPU-only paths below
                }

                // === AVX-512 IFMA PATH: batch Fermat on CPU SIMD, BPSW confirm ===
                bool use_ifma = avx512_ifma_available() && candidates.size() >= 8;
                if (use_ifma) {
                    CandidateBatch ifma_batch;
                    tester.prepare_batch(ifma_batch, states[k].mpz_start, candidates, 320);

                    std::vector<uint8_t> fermat_results(ifma_batch.count, 0);
                    avx512_fermat_batch(fermat_results.data(), ifma_batch.candidates.data(),
                                        ifma_batch.count, ifma_batch.bits);
                    par_tests.fetch_add(ifma_batch.count, std::memory_order_relaxed);

                    for (size_t ci = 0; ci < candidates.size(); ci++) {
                        if (stop_requested) break;
                        if (!fermat_results[ci]) continue;

                        uint64_t offset = candidates[ci];
                        mpz_add_ui(states[k].mpz_candidate, states[k].mpz_start, offset);

                        if (tester.bpsw_test(states[k].mpz_candidate)) {
                            states[k].primes_found++;
                            par_primes.fetch_add(1, std::memory_order_relaxed);

                            if (!states[k].have_first_prime) {
                                states[k].last_prime_offset = offset;
                                states[k].have_first_prime = true;
                            } else {
                                uint64_t gap = offset - states[k].last_prime_offset;

                                if (gap >= states[k].min_gap) {
                                    states[k].valid_gaps++;
                                    mpz_set_ui(states[k].mpz_adder, states[k].last_prime_offset);

                                    PoW pow(states[k].mpz_hash, shift, states[k].mpz_adder,
                                            target_difficulty, states[k].nonce);

                                    if (pow.valid()) {
                                        LogPrintf("Mining: VALID PROOF found! nonce=%u gap=%llu offset=%llu\n",
                                                  states[k].nonce, (unsigned long long)gap,
                                                  (unsigned long long)states[k].last_prime_offset);
                                        if (processor) {
                                            bool continue_mining = processor->process(&pow);
                                            if (!continue_mining) {
                                                stop_requested = true;
                                                goto cleanup;
                                            }
                                        }
                                    }
                                }
                                states[k].last_prime_offset = offset;
                            }
                        }
                    }
                    continue;  // Skip scalar path
                }

                // === SCALAR CPU PATH: direct BPSW test (no pre-filter) ===
                for (uint64_t offset : candidates) {
                    if (stop_requested) break;

                    mpz_add_ui(states[k].mpz_candidate, states[k].mpz_start, offset);

                    // Primorial GCD pre-filter
                    if (mpz_gcd_ui(nullptr, states[k].mpz_candidate, PRIMORIAL_23) != 1) {
                        continue;
                    }

                    par_tests.fetch_add(1, std::memory_order_relaxed);

                    if (tester.bpsw_test(states[k].mpz_candidate)) {
                        states[k].primes_found++;
                        par_primes.fetch_add(1, std::memory_order_relaxed);

                        if (!states[k].have_first_prime) {
                            states[k].last_prime_offset = offset;
                            states[k].have_first_prime = true;
                        } else {
                            uint64_t gap = offset - states[k].last_prime_offset;

                            if (gap >= states[k].min_gap) {
                                states[k].valid_gaps++;
                                mpz_set_ui(states[k].mpz_adder, states[k].last_prime_offset);

                                PoW pow(states[k].mpz_hash, shift, states[k].mpz_adder,
                                        target_difficulty, states[k].nonce);

                                if (pow.valid()) {
                                    LogPrintf("Mining: VALID PROOF found! nonce=%u gap=%llu offset=%llu\n",
                                              states[k].nonce, (unsigned long long)gap,
                                              (unsigned long long)states[k].last_prime_offset);
                                    if (processor) {
                                        bool continue_mining = processor->process(&pow);
                                        if (!continue_mining) {
                                            stop_requested = true;
                                            goto cleanup;
                                        }
                                    }
                                }
                            }
                            states[k].last_prime_offset = offset;
                        }
                    }
                }
            }
        }

        // Accumulate stats (primes already incremented in real-time above)
        for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
            par_gaps.fetch_add(states[k].valid_gaps, std::memory_order_relaxed);
        }
        par_nonces.fetch_add(COMBINED_SIEVE_BATCH, std::memory_order_relaxed);

        // Periodic progress logging
        if ((nonce_base / n_threads) % 100 == 0) {
            LogPrintf("Mining progress: nonce_base=%u batch=%d primes=%llu\n",
                      nonce_base, COMBINED_SIEVE_BATCH,
                      (unsigned long long)par_primes.load(std::memory_order_relaxed));
        }

        nonce_base += COMBINED_SIEVE_BATCH * n_threads;
    }

cleanup:
    for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
        mpz_clear(states[k].mpz_hash);
        mpz_clear(states[k].mpz_start);
        mpz_clear(states[k].mpz_candidate);
        mpz_clear(states[k].mpz_adder);
    }
}

void MiningEngine::request_stop() {
    stop_requested = true;
    if (pipeline) {
        pipeline->stop_mining();
    }
}

void MiningEngine::stop() {
    request_stop();
    for (auto& t : mining_threads) {
        if (t.joinable()) t.join();
    }
    mining_threads.clear();
    parallel_mining_active = false;
    stop_requested = false;
    // GPU thread stays alive — it persists across mine_parallel calls
}

bool MiningEngine::is_mining() const {
    return parallel_mining_active || (pipeline && pipeline->is_mining());
}

MiningStatsSnapshot MiningEngine::get_stats() const {
    if (parallel_mining_active) {
        MiningStatsSnapshot snap = {};
        snap.primes_found = par_primes.load(std::memory_order_relaxed);
        snap.gaps_found = par_gaps.load(std::memory_order_relaxed);
        snap.tests_performed = par_tests.load(std::memory_order_relaxed);
        snap.nonces_tested = par_nonces.load(std::memory_order_relaxed);
        return snap;
    }
    return pipeline->get_stats();
}
