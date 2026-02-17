// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Combined Sieve Implementation
 *
 * Based on Seth Troisi's combined sieve technique (2020).
 * Processes COMBINED_SIEVE_BATCH intervals through a single prime table
 * iteration per segment, keeping primes in L1 cache.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/combined_sieve.h>
#include <pow/simd_presieve.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

static inline bool is_coprime_2310(uint64_t n) {
    if ((n & 1) == 0) return false;
    if (n % 3 == 0) return false;
    if (n % 5 == 0) return false;
    if (n % 7 == 0) return false;
    if (n % 11 == 0) return false;
    return true;
}

CombinedSegmentedSieve::CombinedSegmentedSieve(uint64_t n_primes, uint64_t total_sieve_size) {
    this->n_primes = n_primes;
    this->total_size = round_up(total_sieve_size, SEGMENT_SIZE_BITS);
    this->total_segments = this->total_size / SEGMENT_SIZE_BITS;
    this->current_segment = 0;

    segment_words = SEGMENT_SIZE_BITS / 64;

    // Allocate shared prime table
    primes = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * n_primes));
    if (!primes) {
        throw std::runtime_error("CombinedSegmentedSieve: failed to allocate prime table");
    }
    init_primes(n_primes);

    // Determine small prime limit
    small_prime_limit = 0;
    for (uint64_t i = 0; i < this->n_primes; i++) {
        if (primes[i] > SEGMENT_SIZE_BITS) {
            small_prime_limit = i;
            break;
        }
    }
    if (small_prime_limit == 0) small_prime_limit = this->n_primes;

    // Initialize intervals
    for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
        intervals[k].segment = static_cast<uint64_t*>(
            aligned_alloc(64, segment_words * sizeof(uint64_t)));
        if (!intervals[k].segment) {
            throw std::runtime_error("CombinedSegmentedSieve: failed to allocate segment");
        }

        intervals[k].small_starts = static_cast<uint32_t*>(
            std::malloc(sizeof(uint32_t) * this->n_primes));
        if (!intervals[k].small_starts) {
            throw std::runtime_error("CombinedSegmentedSieve: failed to allocate small_starts");
        }
        std::memset(intervals[k].small_starts, 0, sizeof(uint32_t) * this->n_primes);

        if (small_prime_limit < this->n_primes) {
            intervals[k].large_starts = static_cast<uint32_t*>(
                std::malloc(sizeof(uint32_t) * (this->n_primes - small_prime_limit)));
        } else {
            intervals[k].large_starts = nullptr;
        }

        mpz_init(intervals[k].mpz_start);
        intervals[k].base_mod = 0;
        intervals[k].active = false;
    }

    // Initialize presieve tables (shared, call once)
    presieve_generate_tables();

    // Initialize bucket storage
    buckets[0].reserve(4096);
    buckets[1].reserve(4096);
}

CombinedSegmentedSieve::~CombinedSegmentedSieve() {
    std::free(primes);

    for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
        aligned_free(intervals[k].segment);
        std::free(intervals[k].small_starts);
        if (intervals[k].large_starts) std::free(intervals[k].large_starts);
        mpz_clear(intervals[k].mpz_start);
    }
}

void CombinedSegmentedSieve::init_primes(uint64_t n) {
    // Upper bound for nth prime: p_n < n * (ln(n) + ln(ln(n))) for n >= 6
    uint64_t limit = static_cast<uint64_t>(n * (std::log(n) + std::log(std::log(n)))) + 100;
    limit = round_up(limit, 64);

    uint64_t* sieve_buf = static_cast<uint64_t*>(std::calloc(limit / 64 + 1, sizeof(uint64_t)));
    uint64_t sqrt_limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(limit))) + 1;

    for (uint64_t i = 3; i <= sqrt_limit; i += 2) {
        if (!test_bit64(sieve_buf, i >> 1)) {
            for (uint64_t j = i * i; j < limit; j += i * 2) {
                set_bit64(sieve_buf, j >> 1);
            }
        }
    }

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

void CombinedSegmentedSieve::init_interval(int k, mpz_t start) {
    if (k < 0 || k >= COMBINED_SIEVE_BATCH) return;

    mpz_set(intervals[k].mpz_start, start);
    intervals[k].base_mod = mpz_tdiv_ui(start, 2310);
    intervals[k].active = true;

    // Compute starting positions for all primes in this interval
    calc_starts(k);

    // Set presieve base offsets for this interval
    if (presieve_tables_ready()) {
        presieve_set_base_offsets(intervals[k].mpz_start);
    }
}

void CombinedSegmentedSieve::deactivate_interval(int k) {
    if (k >= 0 && k < COMBINED_SIEVE_BATCH) {
        intervals[k].active = false;
    }
}

void CombinedSegmentedSieve::reset_segments() {
    current_segment = 0;
    buckets[0].clear();
    buckets[1].clear();
}

int CombinedSegmentedSieve::active_count() const {
    int count = 0;
    for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
        if (intervals[k].active) count++;
    }
    return count;
}

void CombinedSegmentedSieve::calc_starts(int k) {
    IntervalState& iv = intervals[k];

    // Skip prime 2 (index 0) — not relevant in odd-only sieve
    for (uint64_t i = 1; i < n_primes; i++) {
        uint32_t p = primes[i];
        uint32_t remainder = mpz_tdiv_ui(iv.mpz_start, p);
        uint32_t offset = (remainder == 0) ? 0 : (p - remainder);

        // Ensure offset is odd
        if ((offset & 1) == 0) offset += p;

        // Convert to bit position (odd-only indexing)
        uint32_t bit_pos = offset / 2;

        iv.small_starts[i] = bit_pos;
        if (i >= small_prime_limit && iv.large_starts) {
            iv.large_starts[i - small_prime_limit] = bit_pos;
        }
    }
}

void CombinedSegmentedSieve::apply_presieve(int k, uint64_t seg_low) {
    if (!intervals[k].active) return;

    uint8_t* seg = reinterpret_cast<uint8_t*>(intervals[k].segment);
    size_t seg_bytes = segment_words * sizeof(uint64_t);

    // Presieve requires per-interval base offsets. We set them during init,
    // but for segments beyond the first, the offset advances linearly.
    // Presieve functions use seg_low to compute table positions.
    if (presieve_tables_ready()) {
        presieve_set_base_offsets(intervals[k].mpz_start);
        presieve_init(seg, seg_bytes, seg_low);
        presieve_apply(seg, seg_bytes, seg_low);
    } else {
        std::memset(intervals[k].segment, 0, seg_bytes);
    }
}

bool CombinedSegmentedSieve::sieve_next_segment() {
    if (current_segment >= total_segments) return false;
    if (active_count() == 0) return false;

    uint64_t seg_low = current_segment * (segment_words * sizeof(uint64_t));

    // Phase 1: Initialize/presieve each active interval's segment
    for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
        apply_presieve(k, seg_low);
    }

    // Phase 2: Combined small prime sieving — THE KEY OPTIMIZATION
    // Iterate primes once, apply each prime to all active intervals
    sieve_small_primes_combined();

    // Phase 3: Process large prime buckets for all intervals
    process_buckets_combined();

    current_segment++;

    if (current_segment < total_segments) {
        advance_buckets();
    }

    return true;
}

void CombinedSegmentedSieve::sieve_small_primes_combined() {
    // Core optimization: iterate prime table ONCE per segment.
    // For each prime, apply to all active intervals before moving to next prime.
    // This keeps the prime value in a register while touching K segment arrays.

    for (uint64_t i = 1; i < small_prime_limit; i++) {
        uint32_t p = primes[i];

        // Apply this prime to each active interval
        for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
            if (!intervals[k].active) continue;

            uint32_t start = intervals[k].small_starts[i];

            if (start >= SEGMENT_SIZE_BITS) {
                intervals[k].small_starts[i] = start - SEGMENT_SIZE_BITS;
                continue;
            }

            // Mark all odd multiples of p in this interval's segment
            uint64_t* seg = intervals[k].segment;
            uint32_t pos = start;
            for (; pos < SEGMENT_SIZE_BITS; pos += p) {
                set_bit64(seg, pos);
            }

            // Save starting position for next segment
            intervals[k].small_starts[i] = pos - SEGMENT_SIZE_BITS;
        }
    }
}

void CombinedSegmentedSieve::process_buckets_combined() {
    if (buckets[0].empty()) return;

    for (auto& entry : buckets[0]) {
        int k = entry.interval;
        if (!intervals[k].active) continue;

        uint32_t p = primes[entry.prime_idx];
        uint32_t pos = entry.next_hit;

        if (pos < SEGMENT_SIZE_BITS) {
            set_bit64(intervals[k].segment, pos);
        }

        uint32_t next = pos + p;
        if (next < SEGMENT_SIZE_BITS) {
            entry.next_hit = next;
        } else {
            next -= SEGMENT_SIZE_BITS;
            if (next / SEGMENT_SIZE_BITS < 1) {
                CombinedBucketEntry new_entry;
                new_entry.prime_idx = entry.prime_idx;
                new_entry.next_hit = next % SEGMENT_SIZE_BITS;
                new_entry.interval = k;
                buckets[1].push_back(new_entry);
            }
        }
    }

    buckets[0].clear();
}

void CombinedSegmentedSieve::advance_buckets() {
    std::swap(buckets[0], buckets[1]);
    buckets[1].clear();

    // Re-populate buckets with large primes for next segment
    // (Only needed on first segment; after that, process_buckets handles advancement)
    if (current_segment == 1) {
        for (int k = 0; k < COMBINED_SIEVE_BATCH; k++) {
            if (!intervals[k].active || !intervals[k].large_starts) continue;

            for (uint64_t i = small_prime_limit; i < n_primes; i++) {
                uint32_t start_pos = intervals[k].large_starts[i - small_prime_limit];
                uint32_t seg_idx = start_pos / SEGMENT_SIZE_BITS;
                if (seg_idx == 0) {
                    // Already processed in segment 0; advance
                    uint32_t p = primes[i];
                    uint32_t next = start_pos + p;
                    if (next / SEGMENT_SIZE_BITS == 0) {
                        CombinedBucketEntry entry;
                        entry.prime_idx = i;
                        entry.next_hit = next % SEGMENT_SIZE_BITS;
                        entry.interval = k;
                        buckets[0].push_back(entry);
                    }
                } else if (seg_idx == 1) {
                    CombinedBucketEntry entry;
                    entry.prime_idx = i;
                    entry.next_hit = start_pos % SEGMENT_SIZE_BITS;
                    entry.interval = k;
                    buckets[0].push_back(entry);
                }
            }
        }
    }
}

void CombinedSegmentedSieve::get_candidates(int k, std::vector<uint64_t>& candidates) {
    candidates.clear();
    if (k < 0 || k >= COMBINED_SIEVE_BATCH || !intervals[k].active) return;

    uint64_t seg_start = (current_segment > 0) ? (current_segment - 1) * SEGMENT_SIZE_BITS : 0;

    for (uint64_t bit = 0; bit < SEGMENT_SIZE_BITS; bit++) {
        if (!test_bit64(intervals[k].segment, bit)) {
            uint64_t integer_offset = 2 * (seg_start + bit) + 1;
            uint64_t pos_mod = (intervals[k].base_mod + integer_offset) % 2310;
            if (is_coprime_2310(pos_mod)) {
                candidates.push_back(integer_offset);
            }
        }
    }
}

uint64_t CombinedSegmentedSieve::get_segment_offset() const {
    return (current_segment > 0) ? (current_segment - 1) * SEGMENT_SIZE_BITS : 0;
}

void CombinedSegmentedSieve::get_start(int k, mpz_t result) const {
    if (k >= 0 && k < COMBINED_SIEVE_BATCH) {
        mpz_set(result, intervals[k].mpz_start);
    }
}

uint64_t CombinedSegmentedSieve::get_base_mod(int k) const {
    if (k >= 0 && k < COMBINED_SIEVE_BATCH) {
        return intervals[k].base_mod;
    }
    return 0;
}

uint64_t CombinedSegmentedSieve::get_total_size() const {
    return total_size;
}
