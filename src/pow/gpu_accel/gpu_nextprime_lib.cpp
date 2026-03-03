/*
 * gpu_nextprime_lib.cpp — GPU-accelerated next-prime (statically linked)
 *
 * Sieve + GPU batch BPSW + GMP BPSW confirmation.
 * ~9s validation at 12K bits vs ~31s (gwnum) or ~78s (GMP).
 *
 * Copyright (c) 2026 The Freycoin developers
 * Licensed under MIT
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <climits>
#include <mutex>
#include <vector>
#include <chrono>
#include <gmp.h>

extern "C" {
#include <pow/gpu_accel/ecpp_gpu_shim.h>
}

// ─── Diagnostic file logging (Windows GUI apps have no stderr) ──────────────

static FILE* g_logfile = nullptr;
static std::once_flag g_log_init;

static void InitLog()
{
    g_logfile = fopen("gpu_nextprime_debug.log", "w");
    if (g_logfile) {
        fprintf(g_logfile, "gpu_nextprime_lib diagnostic log\n");
        fflush(g_logfile);
    }
}

static void LogMsg(const char* fmt, ...)
{
    std::call_once(g_log_init, InitLog);
    if (!g_logfile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logfile, fmt, ap);
    va_end(ap);
    fflush(g_logfile);
}

// No export macros needed — statically linked into the binary

// ─── Sieve parameters (match fast_nextprime.cpp / gpu_gap_filler.cpp) ──────

static constexpr unsigned SIEVE_LIMIT = 500000;
static constexpr unsigned SEG = 65536;
static constexpr size_t GPU_THRESHOLD_BITS = 2000;

static std::vector<unsigned> g_primes;
static std::once_flag g_primes_init;
static bool g_gpu_available = false;
static float g_intensity = 1.0f;
static unsigned g_effective_seg = SEG;

static void InitSievePrimes()
{
    std::vector<char> is_p(SIEVE_LIMIT + 1, 1);
    is_p[0] = is_p[1] = 0;
    for (unsigned i = 2; static_cast<unsigned long long>(i) * i <= SIEVE_LIMIT; i++) {
        if (is_p[i]) {
            for (unsigned j = i * i; j <= SIEVE_LIMIT; j += i)
                is_p[j] = 0;
        }
    }
    for (unsigned i = 2; i <= SIEVE_LIMIT; i++) {
        if (is_p[i]) g_primes.push_back(i);
    }
}

// ─── GPU-accelerated next-prime (sieve + GPU batch BPSW + GMP confirm) ─────

static int GpuNextPrime(mpz_ptr result, mpz_srcptr n)
{
    std::call_once(g_primes_init, InitSievePrimes);
    const unsigned eff_seg = g_effective_seg;

    auto t_start = std::chrono::steady_clock::now();
    size_t bits = mpz_sizeinbase(n, 2);
    LogMsg("GpuNextPrime called: %zu bits, %zu sieve primes, seg=%u (intensity=%.2f)\n",
           bits, g_primes.size(), eff_seg, g_intensity);

    mpz_t base;
    mpz_init_set(base, n);
    mpz_add_ui(base, base, 1);
    if (mpz_even_p(base)) mpz_add_ui(base, base, 1);

    // Precompute base % p for each sieve prime
    // NOTE: mpz_fdiv_ui returns unsigned long, but values are < p < 500000
    // so they fit in 32-bit. We store as uint64_t to avoid overflow in
    // the sieve index computation below (Windows LLP64: unsigned long = 32-bit).
    std::vector<uint64_t> rems(g_primes.size());
    for (size_t pi = 0; pi < g_primes.size(); pi++) {
        rems[pi] = mpz_fdiv_ui(base, g_primes[pi]);
    }

    // Candidate array for GPU batch — sized to effective segment
    std::vector<mpz_t> candidates(eff_seg);
    for (unsigned i = 0; i < eff_seg; i++) mpz_init(candidates[i]);

    std::vector<char> sieve(eff_seg);
    std::vector<uint8_t> gpu_results(eff_seg);
    int ret = -1;

    for (unsigned seg = 0; ; seg++) {
        // Sieve this segment
        std::memset(sieve.data(), 1, eff_seg);
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            if (p == 2) continue;

            uint64_t r = rems[pi];
            // inv2 = modular inverse of 2 mod p = (p+1)/2 for odd p
            uint64_t inv2 = (static_cast<uint64_t>(p) + 1) / 2;
            // si = starting sieve index such that base + 2*si ≡ 0 (mod p)
            // Intermediate (p - r) * inv2 can be up to ~500000 * 250000 = ~125 billion
            // MUST use 64-bit arithmetic (overflows 32-bit unsigned long on Windows)
            uint64_t si = (r == 0) ? 0 : ((static_cast<uint64_t>(p) - r) % p * inv2) % p;
            for (uint64_t idx = si; idx < eff_seg; idx += p) {
                sieve[idx] = 0;
            }
        }

        // Collect survivors
        int n_survivors = 0;
        for (unsigned i = 0; i < eff_seg; i++) {
            if (!sieve[i]) continue;
            uint64_t off = 2ULL * (static_cast<uint64_t>(eff_seg) * seg + i);
            mpz_set(candidates[n_survivors], base);
            if (off <= ULONG_MAX) {
                mpz_add_ui(candidates[n_survivors], candidates[n_survivors],
                           static_cast<unsigned long>(off));
            } else {
                // off > 32-bit: use mpz arithmetic
                mpz_t tmp;
                mpz_init_set_ui(tmp, static_cast<unsigned long>(off >> 32));
                mpz_mul_2exp(tmp, tmp, 32);
                mpz_add_ui(tmp, tmp, static_cast<unsigned long>(off & 0xFFFFFFFF));
                mpz_add(candidates[n_survivors], candidates[n_survivors], tmp);
                mpz_clear(tmp);
            }
            n_survivors++;
        }

        if (seg == 0) {
            LogMsg("  seg 0: %d survivors out of %u\n", n_survivors, eff_seg);
        }

        if (n_survivors == 0) {
            for (size_t pi = 0; pi < g_primes.size(); pi++) {
                unsigned p = g_primes[pi];
                rems[pi] = (rems[pi] + (2ULL * eff_seg) % p) % p;
            }
            continue;
        }

        // Batch GPU BPSW on survivors in sub-batches.
        // On Windows WDDM, the TDR (Timeout Detection & Recovery) kills
        // GPU kernels after 2 seconds. At 12K bits, ~64 candidates per
        // sub-batch takes ~400ms on RTX 4090, safely within TDR.
        {
            constexpr int TDR_SAFE_BATCH = 64;
            bool gpu_failed = false;
            int total_prp = 0;

            for (int batch_start = 0; batch_start < n_survivors; batch_start += TDR_SAFE_BATCH) {
                int batch_count = std::min(TDR_SAFE_BATCH, n_survivors - batch_start);

                int rc = gpu_batch_primality(
                    gpu_results.data() + batch_start,
                    candidates.data() + batch_start,
                    batch_count);

                if (rc != 0) {
                    LogMsg("  GPU BPSW sub-batch failed (rc=%d) at offset %d, GMP fallback\n",
                           rc, batch_start);
                    for (int i = batch_start; i < n_survivors; i++) {
                        if (mpz_probab_prime_p(candidates[i], 0) > 0) {
                            mpz_set(result, candidates[i]);
                            ret = 0;
                            auto elapsed = std::chrono::steady_clock::now() - t_start;
                            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                            LogMsg("  FOUND prime (GMP fallback) at candidate %d, seg %u, %lld ms\n",
                                   i, seg, (long long)ms);
                            goto cleanup;
                        }
                    }
                    gpu_failed = true;
                    break;
                }

                // Count PRP results in this sub-batch
                for (int i = batch_start; i < batch_start + batch_count; i++) {
                    if (gpu_results[i]) total_prp++;
                }

                // Check this sub-batch for confirmed primes
                for (int i = batch_start; i < batch_start + batch_count; i++) {
                    if (!gpu_results[i]) continue;
                    if (mpz_probab_prime_p(candidates[i], 0) > 0) {
                        mpz_set(result, candidates[i]);
                        ret = 0;
                        auto elapsed = std::chrono::steady_clock::now() - t_start;
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                        LogMsg("  FOUND prime at candidate %d, seg %u, %d PRP in seg, %lld ms\n",
                               i, seg, total_prp, (long long)ms);
                        goto cleanup;
                    }
                }
            }

            if (seg < 3 || (seg % 100 == 0)) {
                LogMsg("  seg %u: %d survivors, %d PRP, no confirmed prime%s\n",
                       seg, n_survivors, total_prp,
                       gpu_failed ? " (GPU failed)" : "");
            }

            (void)gpu_failed;
        }

        // Advance remainders
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            rems[pi] = (rems[pi] + (2ULL * eff_seg) % p) % p;
        }
    }

cleanup:
    for (unsigned i = 0; i < eff_seg; i++) mpz_clear(candidates[i]);
    mpz_clear(base);
    return ret;
}

// ─── Exported C ABI ────────────────────────────────────────────────────────

extern "C" {

int gpu_nextprime_init(int device_id)
{
    LogMsg("gpu_nextprime_init(device_id=%d) called\n", device_id);
    LogMsg("  sizeof(unsigned long)=%zu, sizeof(uint64_t)=%zu, sizeof(mp_limb_t)=%zu\n",
           sizeof(unsigned long), sizeof(uint64_t), sizeof(mp_limb_t));

    int rc = ecpp_gpu_shim_init(device_id);
    if (rc == 0) {
        g_gpu_available = true;
        const char* name = ecpp_gpu_device_name();
        LogMsg("  GPU initialized: %s\n", name ? name : "unknown");
    } else {
        g_gpu_available = false;
        LogMsg("  GPU init failed (rc=%d)\n", rc);
    }
    return rc;
}

void gpu_nextprime_set_intensity(float intensity)
{
    if (intensity < 0.05f) intensity = 0.05f;
    if (intensity > 1.0f) intensity = 1.0f;
    g_intensity = intensity;
    g_effective_seg = std::max(1024u, static_cast<unsigned>(SEG * intensity));
    LogMsg("gpu_nextprime_set_intensity: %.2f (effective segment: %u)\n",
           intensity, g_effective_seg);
}

int gpu_nextprime(mpz_ptr result, mpz_srcptr n)
{
    if (!g_gpu_available) return -1;

    size_t bits = mpz_sizeinbase(n, 2);
    if (bits < GPU_THRESHOLD_BITS) {
        return -1;
    }

    LogMsg("gpu_nextprime: %zu bits\n", bits);
    int rc = GpuNextPrime(result, n);
    if (rc == 0) {
        size_t result_bits = mpz_sizeinbase(result, 2);
        LogMsg("gpu_nextprime: SUCCESS, result %zu bits\n", result_bits);
    } else {
        LogMsg("gpu_nextprime: FAILED (rc=%d)\n", rc);
    }
    return rc;
}

void gpu_nextprime_cleanup(void)
{
    if (g_gpu_available) {
        ecpp_gpu_shim_cleanup();
        g_gpu_available = false;
    }
}

} // extern "C"
