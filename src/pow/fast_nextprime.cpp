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
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc / _aligned_free
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <logging.h>
#include <pow/gpu_coordinator.h>

#ifdef HAVE_GWNUM
#include <gwnum.h>
#endif

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

// Sieve primes up to 500K — eliminates ~95% of candidates at 12000 bits.
// Larger sieve limits have diminishing returns because the per-prime remainder
// computation (mpz_fdiv_ui on 12000-bit numbers) becomes the bottleneck.
static constexpr unsigned SIEVE_LIMIT = 500000;

// Segment size: number of odd candidates per sieve segment.
// Covers a range of 2*SEG consecutive integers.
// Expected prime gap at 12000 bits is ~8317, so 65536 covers ~16x margin.
static constexpr unsigned SEG = 65536;

// Threshold below which mpz_nextprime is fast enough on its own
static constexpr size_t GWNUM_THRESHOLD_BITS = 2000;

// GPU library threshold — same as gwnum, GPU has overhead below this
static constexpr size_t GPU_THRESHOLD_BITS = 2000;

// ─── GPU shared library (dlopen/LoadLibrary) ───────────────────────────────
//
// If libgpu_nextprime.so / gpu_nextprime.dll is present alongside the binary,
// we load it at first call and dispatch to GPU batch BPSW (~9s at 12K bits).
// If absent or init fails, we silently fall back to gwnum → GMP.

using gpu_nextprime_init_fn    = int  (*)(int);
using gpu_nextprime_fn         = int  (*)(mpz_ptr, mpz_srcptr);
using gpu_nextprime_cleanup_fn = void (*)();

// GPU access coordinator — defined here, declared extern in gpu_coordinator.h
std::shared_mutex g_gpu_access;

static std::once_flag           g_gpu_init;
static gpu_nextprime_fn         g_gpu_nextprime = nullptr;
static gpu_nextprime_cleanup_fn g_gpu_cleanup   = nullptr;

static void LoadGpuLibrary()
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("gpu_nextprime.dll");
    if (!lib) {
        LogPrintf("GPU nextprime: gpu_nextprime.dll not found, using fallback\n");
        return;
    }
    auto init_fn = reinterpret_cast<gpu_nextprime_init_fn>(
        GetProcAddress(lib, "gpu_nextprime_init"));
    g_gpu_nextprime = reinterpret_cast<gpu_nextprime_fn>(
        GetProcAddress(lib, "gpu_nextprime"));
    g_gpu_cleanup = reinterpret_cast<gpu_nextprime_cleanup_fn>(
        GetProcAddress(lib, "gpu_nextprime_cleanup"));
#else
    void* lib = dlopen("libgpu_nextprime.so", RTLD_NOW);
    if (!lib) {
        LogPrintf("GPU nextprime: libgpu_nextprime.so not found (%s), using fallback\n", dlerror());
        return;
    }
    auto init_fn = reinterpret_cast<gpu_nextprime_init_fn>(
        dlsym(lib, "gpu_nextprime_init"));
    g_gpu_nextprime = reinterpret_cast<gpu_nextprime_fn>(
        dlsym(lib, "gpu_nextprime"));
    g_gpu_cleanup = reinterpret_cast<gpu_nextprime_cleanup_fn>(
        dlsym(lib, "gpu_nextprime_cleanup"));
#endif

    if (!init_fn || !g_gpu_nextprime || !g_gpu_cleanup) {
        LogPrintf("GPU nextprime: library loaded but missing symbols, using fallback\n");
        g_gpu_nextprime = nullptr;
        g_gpu_cleanup = nullptr;
        return;
    }

    int rc = init_fn(0);
    if (rc != 0) {
        LogPrintf("GPU nextprime: init failed (rc=%d), using fallback\n", rc);
        g_gpu_nextprime = nullptr;
        g_gpu_cleanup = nullptr;
        return;
    }

    LogPrintf("GPU nextprime: loaded successfully\n");
}

static std::once_flag g_primes_init;
static std::vector<unsigned> g_primes;

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
}

#ifdef HAVE_GWNUM

/**
 * Fermat base-2 PRP test using gwnum FFT arithmetic.
 * Returns true if 2^(n-1) ≡ 1 (mod n).
 *
 * Uses left-to-right binary exponentiation. Since the base is 2,
 * multiplication by the base is just gwsmallmul(2.0), which is
 * essentially free compared to the squaring step.
 */
static bool GwnumFermat2(const mpz_t n)
{
    size_t nlimbs = (mpz_sizeinbase(n, 2) + 63) / 64;
    uint64_t* mod_array = static_cast<uint64_t*>(aligned_calloc(nlimbs, sizeof(uint64_t)));
    if (!mod_array) return false;
    mpz_export(mod_array, nullptr, -1, 8, -1, 0, n);

    gwhandle gw;
    gwinit(&gw);
    int err = gwsetup_general_mod_64(&gw, mod_array, nlimbs);
    if (err) {
        aligned_free(mod_array);
        return false;
    }

    gwnum x = gwalloc(&gw);
    if (!x) {
        gwdone(&gw);
        aligned_free(mod_array);
        return false;
    }

    // Compute 2^(n-1) mod n via repeated squaring
    mpz_t nm1;
    mpz_init(nm1);
    mpz_sub_ui(nm1, n, 1);
    size_t nbits = mpz_sizeinbase(nm1, 2);

    // x = 2
    dbltogw(&gw, 2.0, x);

    // Left-to-right binary exponentiation: scan from MSB-1 down to 0
    for (long i = static_cast<long>(nbits) - 2; i >= 0; i--) {
        gwsquare2(&gw, x, x, 0);
        if (mpz_tstbit(nm1, i)) {
            gwsmallmul(&gw, 2.0, x);
        }
    }

    // Convert result back to mpz and check if == 1
    uint64_t* result_array = static_cast<uint64_t*>(aligned_calloc(nlimbs + 1, sizeof(uint64_t)));
    bool is_one = false;
    if (result_array) {
        gwtobinary64(&gw, x, result_array, static_cast<uint32_t>(nlimbs + 1));
        mpz_t res;
        mpz_init(res);
        mpz_import(res, nlimbs + 1, -1, 8, -1, 0, result_array);
        is_one = (mpz_cmp_ui(res, 1) == 0);
        mpz_clear(res);
        aligned_free(result_array);
    }

    mpz_clear(nm1);
    gwfree(&gw, x);
    gwdone(&gw);
    aligned_free(mod_array);
    return is_one;
}

/**
 * gwnum-accelerated nextprime: sieve + gwnum Fermat + GMP BPSW.
 *
 * Most composites that survive the sieve fail the Fermat test (cheap with gwnum).
 * Only the ~1 candidate that passes Fermat gets the full GMP BPSW test.
 * This avoids running expensive GMP modular exponentiation on 262+ losers.
 */
static void GwnumNextPrime(mpz_t result, const mpz_t n)
{
    std::call_once(g_primes_init, InitSievePrimes);

    mpz_t base, candidate;
    mpz_init_set(base, n);
    mpz_add_ui(base, base, 1);
    if (mpz_even_p(base)) mpz_add_ui(base, base, 1);

    // Precompute base % p for each sieve prime (one bignum mod per prime)
    std::vector<unsigned long> rems(g_primes.size());
    for (size_t pi = 0; pi < g_primes.size(); pi++) {
        rems[pi] = mpz_fdiv_ui(base, g_primes[pi]);
    }

    char sieve[SEG];
    mpz_init(candidate);

    for (unsigned seg = 0; ; seg++) {
        // Mark composites in this segment
        std::memset(sieve, 1, SEG);
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            if (p == 2) continue; // all candidates are odd

            unsigned long r = rems[pi];
            // inv2 = modular inverse of 2 mod p = (p+1)/2 for odd prime p
            unsigned long inv2 = (static_cast<unsigned long>(p) + 1) / 2;
            unsigned long si = (r == 0) ? 0 : ((static_cast<unsigned long>(p) - r) % p * inv2) % p;
            for (unsigned long idx = si; idx < SEG; idx += p) {
                sieve[idx] = 0;
            }
        }

        // Test survivors
        for (unsigned i = 0; i < SEG; i++) {
            if (!sieve[i]) continue;

            unsigned long off = 2UL * (static_cast<unsigned long>(SEG) * seg + i);
            mpz_set(candidate, base);
            mpz_add_ui(candidate, candidate, off);

            // Fast Fermat base-2 test using gwnum (4x faster than GMP modexp)
            if (!GwnumFermat2(candidate)) continue;

            // Fermat passed — confirm with full GMP BPSW (reps=0 does BPSW only).
            // This runs on ~1 candidate per gap, so the cost is negligible.
            if (mpz_probab_prime_p(candidate, 0) > 0) {
                mpz_set(result, candidate);
                mpz_clear(base);
                mpz_clear(candidate);
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

void fast_nextprime(mpz_t result, const mpz_t n)
{
    // Try GPU library first (dlopen'd once on first call).
    // Exclusive lock pauses mining GPU kernels to avoid CUDA contention.
    std::call_once(g_gpu_init, LoadGpuLibrary);
    if (g_gpu_nextprime && mpz_sizeinbase(n, 2) >= GPU_THRESHOLD_BITS) {
        std::unique_lock<std::shared_mutex> gpu_lock(g_gpu_access);
        if (g_gpu_nextprime(result, n) == 0) return;
    }

#ifdef HAVE_GWNUM
    // gwnum only benefits large numbers — FFT setup overhead dominates for small ones
    if (mpz_sizeinbase(n, 2) >= GWNUM_THRESHOLD_BITS) {
        GwnumNextPrime(result, n);
        return;
    }
#endif
    // Fallback: GMP 6.3 mpz_nextprime has its own sieve optimization
    mpz_nextprime(result, n);
}
