// Copyright (c) 2025-2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow/fast_nextprime.h>

#include <gmp.h>
#if !defined(__GNU_MP_VERSION) || (__GNU_MP_VERSION < 6) || \
    (__GNU_MP_VERSION == 6 && __GNU_MP_VERSION_MINOR < 3)
#error "GMP >= 6.3 required (mpz_probab_prime_p reps=0 means BPSW only in 6.3+)"
#endif

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif

#include <logging.h>
#include <pow/gpu_coordinator.h>
#include <pow/gpu_accel/gpu_nextprime.h>
#include <pow/pow_common.h>
#include <util/trace.h>

#ifdef HAVE_GWNUM
#include <gwnum.h>
#endif

// USDT tracepoint semaphores
TRACEPOINT_SEMAPHORE(pow, fast_nextprime);
TRACEPOINT_SEMAPHORE(pow, sieve_segment);
TRACEPOINT_SEMAPHORE(pow, fermat_test);
TRACEPOINT_SEMAPHORE(pow, gpu_lock_wait);

// gwnum's AVX2/AVX-512 FFT routines use vmovapd (aligned 32/64-byte loads/stores).
// Windows malloc only guarantees 16-byte alignment, causing ACCESS_VIOLATION.
// These helpers provide 64-byte aligned allocation on Windows.
static void* aligned_calloc(size_t count, size_t size)
{
    size_t total = count * size;
#ifdef _WIN32
    void* p = _aligned_malloc(total, 64);
    if (p) memset(p, 0, total);
    return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, total) != 0) return nullptr;
    memset(p, 0, total);
    return p;
#endif
}

static void aligned_free(void* p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

// Sieve primes up to 10M — eliminates ~97% of candidates at 12000 bits.
// At 12K bits, each Fermat test costs ~31ms; each additional sieve prime
// costs ~0.07µs to init. Break-even is in the billions, so extending from
// 500K → 10M eliminates ~50 extra Fermat tests, saving ~1.5 seconds per
// block validation at a cost of only ~43ms in sieve init.
// Mertens' theorem: survivor fraction ≈ 2·e^(-γ)/ln(P).
//   P=500K → 8.6% survivors; P=10M → 6.9% survivors.
static constexpr unsigned SIEVE_LIMIT = 10000000;

// Segment size: number of odd candidates per sieve segment.
// Covers a range of 2*SEG consecutive integers.
// Expected prime gap at 12000 bits is ~8317, so 65536 covers ~16x margin.
static constexpr unsigned SEG = 65536;

// Threshold below which mpz_nextprime is fast enough on its own
static constexpr size_t GWNUM_THRESHOLD_BITS = 2000;

// GPU library threshold — same as gwnum, GPU has overhead below this
static constexpr size_t GPU_THRESHOLD_BITS = 2000;

// ─── GPU batch BPSW nextprime (statically linked) ───────────────────────────
//
// GPU batch BPSW via CUDA Driver API (~9s at 12K bits vs ~31s gwnum).
// If no NVIDIA GPU is present, init fails gracefully and we fall back to gwnum → GMP.

// GPU access coordinator — defined here, declared extern in gpu_coordinator.h
std::shared_mutex g_gpu_access;

static std::once_flag g_gpu_init;
static bool           g_gpu_available = false;

static void InitGpuNextprime()
{
    int rc = gpu_nextprime_init(0);
    if (rc == 0) {
        g_gpu_available = true;
        // 50% intensity for block validation — balances speed (~12s at 12K bits)
        // against system responsiveness. Mining engine uses its own GPU path.
        gpu_nextprime_set_intensity(0.50f);
        LogPrintf("GPU nextprime: initialized successfully (validation intensity: 50%%)\n");
    } else {
        g_gpu_available = false;
        LogPrintf("GPU nextprime: init failed (rc=%d), using fallback\n", rc);
    }
}

static std::once_flag g_primes_init;
static std::vector<unsigned> g_primes;

// Grouped-product sieve init: precompute products of BLOCK_SIZE consecutive
// primes. To compute base % p_i for all primes, we first compute
// base % block_product (one big-by-medium division), then extract individual
// remainders from the small result with cheap hardware divisions.
// This reduces init cost from O(P × n_limbs) to O(P/BLOCK × n_limbs + P).
static constexpr unsigned PRODUCT_BLOCK_SIZE = 32;
struct PrimeBlock {
    unsigned start_idx;  // index into g_primes
    unsigned count;      // number of primes in this block
    mpz_t product;       // product of primes in this block
};
static std::vector<PrimeBlock> g_prime_blocks;

static void InitSievePrimes()
{
    std::vector<char> is_p(SIEVE_LIMIT + 1, 1);
    is_p[0] = is_p[1] = 0;
    for (unsigned i = 2; static_cast<unsigned long>(i) * i <= SIEVE_LIMIT; i++) {
        if (is_p[i]) {
            for (unsigned j = i * i; j <= SIEVE_LIMIT; j += i)
                is_p[j] = 0;
        }
    }
    for (unsigned i = 2; i <= SIEVE_LIMIT; i++) {
        if (is_p[i]) g_primes.push_back(i);
    }

    // Build grouped products for fast multi-mod
    for (size_t i = 0; i < g_primes.size(); i += PRODUCT_BLOCK_SIZE) {
        PrimeBlock block;
        block.start_idx = static_cast<unsigned>(i);
        block.count = static_cast<unsigned>(std::min<size_t>(PRODUCT_BLOCK_SIZE, g_primes.size() - i));
        mpz_init_set_ui(block.product, 1);
        for (unsigned j = 0; j < block.count; j++) {
            mpz_mul_ui(block.product, block.product, g_primes[i + j]);
        }
        g_prime_blocks.push_back(std::move(block));
    }

    LogPrintf("Sieve: %zu primes up to %u, %zu product blocks\n",
              g_primes.size(), SIEVE_LIMIT, g_prime_blocks.size());
}

// Compute all sieve remainders using grouped-product approach.
// Instead of P individual mpz_fdiv_ui calls on the full 12K-bit base,
// we do P/32 divisions of the full base by ~736-bit products, then
// extract individual remainders from the small (~736-bit) result.
static void ComputeSieveRemainders(const mpz_t base, std::vector<unsigned long>& rems)
{
    mpz_t block_rem;
    mpz_init(block_rem);

    for (const auto& block : g_prime_blocks) {
        // One big division: base mod (p_i * p_{i+1} * ... * p_{i+31})
        mpz_fdiv_r(block_rem, base, block.product);

        // Extract individual remainders from the small result
        for (unsigned j = 0; j < block.count; j++) {
            rems[block.start_idx + j] = mpz_fdiv_ui(block_rem, g_primes[block.start_idx + j]);
        }
    }

    mpz_clear(block_rem);
}

#ifdef HAVE_GWNUM

// Number of gwnum threads for FFT squaring. At 12K bits the FFT length
// is large enough that 2 threads can help on multi-core VPS nodes.
// Set to 1 to disable (original behavior).
static constexpr int GWNUM_THREADS = 1;

/**
 * gwnum Miller-Rabin base-2 test on a single candidate.
 *
 * Strictly stronger than Fermat: same gwnum squarings but checks intermediate
 * values for nontrivial square roots of 1. Catches strong pseudoprimes that
 * Fermat misses (Carmichael numbers). Same performance as Fermat since the
 * number of squarings is identical (r trailing zeros are typically 1-2).
 *
 * Returns true if candidate is a strong probable prime base 2.
 */
static bool GwnumMillerRabin2(
    const mpz_t candidate, const mpz_t nm1,
    uint64_t* mod_array, uint64_t* result_array,
    size_t nlimbs, mpz_t& res)
{
    const size_t cand_nlimbs = (mpz_sizeinbase(candidate, 2) + 63) / 64;
    std::memset(mod_array, 0, nlimbs * sizeof(uint64_t));
    mpz_export(mod_array, nullptr, -1, 8, -1, 0, candidate);

    gwhandle gw;
    gwinit(&gw);
    gwset_maxmulbyconst(&gw, 2);

    bool is_sprp = false;
    int err = gwsetup_general_mod_64(&gw, mod_array, cand_nlimbs);
    if (!err) {
        gwnum x = gwalloc(&gw);
        if (x) {
            // Factor nm1 = d * 2^r (find odd part and trailing zeros)
            unsigned long r = mpz_scan1(nm1, 0);
            mpz_t d;
            mpz_init(d);
            mpz_tdiv_q_2exp(d, nm1, r);

            // Compute x = 2^d mod n via left-to-right binary exponentiation
            size_t d_bits = mpz_sizeinbase(d, 2);
            dbltogw(&gw, 2.0, x);
            for (long bi = static_cast<long>(d_bits) - 2; bi >= 0; bi--) {
                gwsquare2(&gw, x, x, 0);
                if (mpz_tstbit(d, bi)) {
                    gwsmallmul(&gw, 2.0, x);
                }
            }

            // Extract x = 2^d mod n
            std::memset(result_array, 0, (nlimbs + 1) * sizeof(uint64_t));
            gwtobinary64(&gw, x, result_array, static_cast<uint32_t>(cand_nlimbs + 1));
            mpz_import(res, cand_nlimbs + 1, -1, 8, -1, 0, result_array);

            // Check x == 1 → strong probable prime
            if (mpz_cmp_ui(res, 1) == 0) {
                is_sprp = true;
            }
            // Check x == n-1 → strong probable prime
            else if (mpz_cmp(res, nm1) == 0) {
                is_sprp = true;
            }
            else {
                // Square r-1 more times, checking for n-1
                for (unsigned long i = 1; i < r; i++) {
                    gwsquare2(&gw, x, x, 0);
                    std::memset(result_array, 0, (nlimbs + 1) * sizeof(uint64_t));
                    gwtobinary64(&gw, x, result_array, static_cast<uint32_t>(cand_nlimbs + 1));
                    mpz_import(res, cand_nlimbs + 1, -1, 8, -1, 0, result_array);

                    if (mpz_cmp(res, nm1) == 0) {
                        is_sprp = true;
                        break;
                    }
                    if (mpz_cmp_ui(res, 1) == 0) {
                        // Nontrivial square root of 1 → composite
                        is_sprp = false;
                        break;
                    }
                }
            }

            mpz_clear(d);
            gwfree(&gw, x);
        }
    }
    gwdone(&gw);
    return is_sprp;
}

/**
 * gwnum-accelerated nextprime: sieve + gwnum Miller-Rabin screen + GMP BPSW.
 *
 * Pipeline:
 *  1. Grouped-product sieve init (32 primes per block, ~5x faster than naive)
 *  2. Segmented sieve eliminates ~93% of candidates (10M primes)
 *  3. gwnum Miller-Rabin base-2 screens survivors (~108ms each at 12K bits)
 *  4. GMP BPSW confirms the ~1 candidate that passes MR
 *
 * Profiling breakdown at 12K bits per Fermat/MR test:
 *   gwsetup: 8.7ms (8.1%), squaring: 98.7ms (91.5%), extraction: 0.03ms (0%)
 * MR adds 2-3 extra extractions vs Fermat = +0.06ms = negligible, so we use
 * MR for strictly stronger primality screening at zero practical cost.
 *
 * Fine-grained profiling: accumulates sieve init, segment, fermat, and
 * BPSW timing into g_validation_stats for getpowstats RPC.
 */
static void GwnumNextPrime(mpz_t result, const mpz_t n)
{
    std::call_once(g_primes_init, InitSievePrimes);

    mpz_t base, candidate;
    mpz_init_set(base, n);
    mpz_add_ui(base, base, 1);
    if (mpz_even_p(base)) mpz_add_ui(base, base, 1);

    const auto t_sieve_init = std::chrono::steady_clock::now();

    // Grouped-product sieve init: compute all remainders via product blocks
    std::vector<unsigned long> rems(g_primes.size());
    ComputeSieveRemainders(base, rems);

    const auto t_sieve_init_end = std::chrono::steady_clock::now();
    const auto init_us = std::chrono::duration_cast<std::chrono::microseconds>(t_sieve_init_end - t_sieve_init).count();
    g_validation_stats.total_sieve_init_us.fetch_add(init_us, std::memory_order_relaxed);
    LogDebug(BCLog::BENCH, "GwnumNextPrime: sieve init (%zu primes, %zu blocks) took %lld ms\n",
             g_primes.size(), g_prime_blocks.size(), init_us / 1000);

    // Pre-allocate buffers for the Miller-Rabin test loop
    const size_t approx_bits = mpz_sizeinbase(base, 2) + 2;
    const size_t nlimbs = (approx_bits + 63) / 64;

    uint64_t* mod_array = static_cast<uint64_t*>(aligned_calloc(nlimbs, sizeof(uint64_t)));
    uint64_t* result_array = static_cast<uint64_t*>(aligned_calloc(nlimbs + 1, sizeof(uint64_t)));
    if (!mod_array || !result_array) {
        aligned_free(mod_array);
        aligned_free(result_array);
        mpz_nextprime(result, n);
        g_validation_stats.tier_gmp.fetch_add(1, std::memory_order_relaxed);
        mpz_clear(base);
        return;
    }

    mpz_t nm1, res;
    mpz_init(nm1);
    mpz_init(res);

    char sieve[SEG];
    mpz_init(candidate);

    unsigned total_fermat = 0;
    unsigned total_survivors = 0;
    int64_t total_fermat_us = 0;

    for (unsigned seg = 0; ; seg++) {
        const auto t_seg = std::chrono::steady_clock::now();

        // Mark composites in this segment
        std::memset(sieve, 1, SEG);
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            if (p == 2) continue;

            unsigned long r = rems[pi];
            unsigned long inv2 = (static_cast<unsigned long>(p) + 1) / 2;
            unsigned long si = (r == 0) ? 0 : ((static_cast<unsigned long>(p) - r) % p * inv2) % p;
            for (unsigned long idx = si; idx < SEG; idx += p) {
                sieve[idx] = 0;
            }
        }

        const auto t_seg_end = std::chrono::steady_clock::now();
        const auto seg_us = std::chrono::duration_cast<std::chrono::microseconds>(t_seg_end - t_seg).count();
        g_validation_stats.total_sieve_segment_us.fetch_add(seg_us, std::memory_order_relaxed);

        // Count survivors
        unsigned seg_survivors = 0;
        for (unsigned i = 0; i < SEG; i++) {
            if (sieve[i]) seg_survivors++;
        }
        total_survivors += seg_survivors;

        // Test survivors with gwnum Miller-Rabin base-2 screen
        for (unsigned i = 0; i < SEG; i++) {
            if (!sieve[i]) continue;

            unsigned long off = 2UL * (static_cast<unsigned long>(SEG) * seg + i);
            mpz_set(candidate, base);
            mpz_add_ui(candidate, candidate, off);

            const auto t_fermat = std::chrono::steady_clock::now();
            total_fermat++;

            mpz_sub_ui(nm1, candidate, 1);
            bool mr_pass = GwnumMillerRabin2(candidate, nm1, mod_array, result_array, nlimbs, res);

            const auto t_fermat_end = std::chrono::steady_clock::now();
            const auto fermat_us = std::chrono::duration_cast<std::chrono::microseconds>(t_fermat_end - t_fermat).count();
            total_fermat_us += fermat_us;

            TRACEPOINT(pow, fermat_test, (int64_t)mpz_sizeinbase(candidate, 2), mr_pass ? 1 : 0, (int64_t)fermat_us);

            if (!mr_pass) continue;

            // MR passed — confirm with full GMP BPSW
            const auto t_bpsw = std::chrono::steady_clock::now();
            if (mpz_probab_prime_p(candidate, 0) > 0) {
                const auto t_bpsw_end = std::chrono::steady_clock::now();
                const auto bpsw_us = std::chrono::duration_cast<std::chrono::microseconds>(t_bpsw_end - t_bpsw).count();

                // Accumulate profiling stats
                g_validation_stats.total_fermat_count.fetch_add(total_fermat, std::memory_order_relaxed);
                g_validation_stats.total_fermat_us.fetch_add(total_fermat_us, std::memory_order_relaxed);
                g_validation_stats.total_bpsw_confirm_us.fetch_add(bpsw_us, std::memory_order_relaxed);
                g_validation_stats.total_survivors.fetch_add(total_survivors, std::memory_order_relaxed);
                g_validation_stats.tier_gwnum.fetch_add(1, std::memory_order_relaxed);

                LogDebug(BCLog::BENCH, "GwnumNextPrime: found prime after %u segments, %u survivors, %u MR tests (%lld ms), BPSW %lld ms, sieve_init=%lld ms\n",
                         seg + 1, total_survivors, total_fermat, total_fermat_us / 1000, bpsw_us / 1000, init_us / 1000);

                mpz_set(result, candidate);
                mpz_clear(nm1);
                mpz_clear(res);
                mpz_clear(base);
                mpz_clear(candidate);
                aligned_free(mod_array);
                aligned_free(result_array);
                return;
            }
        }

        // Advance remainders for next segment
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            rems[pi] = (rems[pi] + (2UL * SEG) % p) % p;
        }
    }
}

#endif // HAVE_GWNUM

bool fast_is_fermat_prp(const mpz_t n)
{
    const size_t bits = mpz_sizeinbase(n, 2);

#ifdef HAVE_GWNUM
    if (bits >= GWNUM_THRESHOLD_BITS) {
        const size_t nlimbs = (bits + 63) / 64;

        uint64_t* mod_array = static_cast<uint64_t*>(aligned_calloc(nlimbs, sizeof(uint64_t)));
        uint64_t* result_array = static_cast<uint64_t*>(aligned_calloc(nlimbs + 1, sizeof(uint64_t)));
        if (!mod_array || !result_array) {
            aligned_free(mod_array);
            aligned_free(result_array);
            return mpz_probab_prime_p(n, 0) > 0;
        }

        mpz_t nm1, res;
        mpz_init(nm1);
        mpz_init(res);
        mpz_sub_ui(nm1, n, 1);

        bool is_sprp = GwnumMillerRabin2(n, nm1, mod_array, result_array, nlimbs, res);

        mpz_clear(nm1);
        mpz_clear(res);
        aligned_free(mod_array);
        aligned_free(result_array);

        return is_sprp;
    }
#endif

    // Fallback: GMP BPSW
    return mpz_probab_prime_p(n, 0) > 0;
}

void fast_nextprime(mpz_t result, const mpz_t n)
{
    const auto t_start = std::chrono::steady_clock::now();
    const size_t bits = mpz_sizeinbase(n, 2);

    // Try GPU batch BPSW first (statically linked, init'd once on first call).
    // Exclusive lock pauses mining GPU kernels to avoid CUDA contention.
    std::call_once(g_gpu_init, InitGpuNextprime);
    if (g_gpu_available && bits >= GPU_THRESHOLD_BITS) {
        const auto t_lock_start = std::chrono::steady_clock::now();
        std::unique_lock<std::shared_mutex> gpu_lock(g_gpu_access);
        const auto t_lock_acquired = std::chrono::steady_clock::now();

        const auto lock_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(t_lock_acquired - t_lock_start).count();
        g_validation_stats.gpu_lock_wait_us.fetch_add(lock_wait_us, std::memory_order_relaxed);

        TRACEPOINT(pow, gpu_lock_wait, (int64_t)lock_wait_us);

        if (lock_wait_us > 100000) { // > 100ms
            LogDebug(BCLog::BENCH, "fast_nextprime: GPU lock wait took %lld ms\n", lock_wait_us / 1000);
        }

        if (gpu_nextprime(result, n) == 0) {
            const auto t_end = std::chrono::steady_clock::now();
            const auto hold_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_lock_acquired).count();
            const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
            g_validation_stats.gpu_lock_hold_us.fetch_add(hold_us, std::memory_order_relaxed);
            g_validation_stats.tier_gpu.fetch_add(1, std::memory_order_relaxed);

            // Compute gap for tracepoint
            mpz_t gap;
            mpz_init(gap);
            mpz_sub(gap, result, n);
            unsigned long gap_size = mpz_get_ui(gap);
            mpz_clear(gap);

            LogDebug(BCLog::BENCH, "fast_nextprime: GPU path (%zu bits) took %lld ms (lock_wait=%lld, compute=%lld)\n",
                     bits, total_us / 1000, lock_wait_us / 1000, hold_us / 1000);

            TRACEPOINT(pow, fast_nextprime, (int64_t)bits, (int64_t)gap_size, (int64_t)total_us, 0 /* GPU */);
            return;
        }
    }

#ifdef HAVE_GWNUM
    // gwnum only benefits large numbers — FFT setup overhead dominates for small ones
    if (bits >= GWNUM_THRESHOLD_BITS) {
        GwnumNextPrime(result, n);

        const auto t_end = std::chrono::steady_clock::now();
        const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();

        mpz_t gap;
        mpz_init(gap);
        mpz_sub(gap, result, n);
        unsigned long gap_size = mpz_get_ui(gap);
        mpz_clear(gap);

        LogDebug(BCLog::BENCH, "fast_nextprime: gwnum path (%zu bits) took %lld ms\n", bits, total_us / 1000);

        TRACEPOINT(pow, fast_nextprime, (int64_t)bits, (int64_t)gap_size, (int64_t)total_us, 1 /* gwnum */);
        return;
    }
#endif
    // Fallback: GMP 6.3 mpz_nextprime has its own sieve optimization
    mpz_nextprime(result, n);
    g_validation_stats.tier_gmp.fetch_add(1, std::memory_order_relaxed);

    const auto t_end = std::chrono::steady_clock::now();
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();

    mpz_t gap;
    mpz_init(gap);
    mpz_sub(gap, result, n);
    unsigned long gap_size = mpz_get_ui(gap);
    mpz_clear(gap);

    LogDebug(BCLog::BENCH, "fast_nextprime: GMP fallback (%zu bits) took %lld ms\n", bits, total_us / 1000);

    TRACEPOINT(pow, fast_nextprime, (int64_t)bits, (int64_t)gap_size, (int64_t)total_us, 2 /* GMP */);
}
