// Copyright (c) 2025-2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow/fast_nextprime.h>

#include <gmp.h>
#if !defined(__GNU_MP_VERSION) || (__GNU_MP_VERSION < 6) || \
    (__GNU_MP_VERSION == 6 && __GNU_MP_VERSION_MINOR < 3)
#error "GMP >= 6.3 required (mpz_probab_prime_p reps=0 means BPSW only in 6.3+)"
#endif

#include <bit>
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
#include <pow/simd_presieve.h>
#include <util/trace.h>

#ifdef HAVE_GWNUM
#include <gwnum.h>
#endif

static std::atomic<bool> g_pow_interrupted{false};

void InterruptPow() { g_pow_interrupted.store(true, std::memory_order_release); }
void ResetPowInterrupt() { g_pow_interrupted.store(false, std::memory_order_release); }
bool IsPowInterrupted() { return g_pow_interrupted.load(std::memory_order_acquire); }

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
// primes. These flat blocks are the leaves of the product tree (see below).
// At each leaf we compute base_mod_block via mpz_fdiv_r, then extract
// individual remainders with cheap hardware divisions.
static constexpr unsigned PRODUCT_BLOCK_SIZE = 32;
struct PrimeBlock {
    unsigned start_idx;  // index into g_primes
    unsigned count;      // number of primes in this block
    mpz_t product;       // product of primes in this block
};
static std::vector<PrimeBlock> g_prime_blocks;

// First index into g_primes whose prime is > PRESIEVE_MAX_PRIME (163).
// Primes <= 163 are handled by SIMD presieve tables; the scalar marking loop
// starts from this index to avoid double-marking composites.
static unsigned g_first_large_prime_idx = 0;

// ────────────────────────────────────────────────────────────────────────
// Product-tree multi-mod (Tier 2.2)
// ────────────────────────────────────────────────────────────────────────
//
// Instead of doing P/BLOCK big-by-medium divisions (one per leaf), we build a
// balanced binary product tree over the leaves and descend recursively. At
// each internal node we compute (parent_rem mod child_product) which halves
// the work at each level. The top of the tree — where child_product > rem —
// is skipped entirely because no reduction is needed. This is the same
// two-tier architecture FLINT uses in fmpz_multi_mod_ui (remainder tree +
// leaf Barrett), reimplemented directly in GMP for MIT license cleanliness.
//
// Expected speedup vs flat grouped-product: 5-10x at 12K bits (research: task #70).
// Depth: log2(leaves) + 1 ≈ 15 for 20768 leaves. Top ~8-9 levels are skipped
// at 12K bits because the accumulated product > N.
//
// Tree layout: `g_prime_tree[0]` is the root. Leaves are the last
// `g_prime_blocks.size()` entries; internal nodes pair children above them.
struct PrimeTreeNode {
    mpz_t product;            // product of all primes under this node
    int32_t left_child;       // index into g_prime_tree (-1 for leaf)
    int32_t right_child;      // index into g_prime_tree (-1 for leaf)
    int32_t leaf_block_idx;   // index into g_prime_blocks (-1 for internal)
};
static std::vector<PrimeTreeNode> g_prime_tree;
static int32_t g_prime_tree_root = -1;
static int32_t g_prime_tree_max_depth = 0;

// Thread-local scratch: one pair of (left_rem, right_rem) per tree depth level.
// Lazily sized on first use to match g_prime_tree_max_depth.
// mpz_t is an array type, so we wrap it in a struct to place it in a vector.
struct TreeScratch { mpz_t val; };
static thread_local std::vector<TreeScratch> g_tree_scratch_lr;
static thread_local bool g_tree_scratch_inited = false;

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

    // Build grouped products for fast multi-mod (these become tree leaves)
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

    // Find first prime > PRESIEVE_MAX_PRIME for scalar-marking loop
    g_first_large_prime_idx = static_cast<unsigned>(g_primes.size());
    for (size_t i = 0; i < g_primes.size(); i++) {
        if (g_primes[i] > PRESIEVE_MAX_PRIME) {
            g_first_large_prime_idx = static_cast<unsigned>(i);
            break;
        }
    }

    // Generate presieve tables (idempotent; safe if already done elsewhere)
    presieve_generate_tables();

    // ─── Build balanced product tree over leaves ────────────────────────
    //
    // Bottom-up construction: start with leaves (one tree node per prime
    // block), then pair adjacent nodes into parents level by level until
    // only one remains (the root). Odd-numbered levels carry a trailing
    // singleton up to the next level unchanged.
    const auto t_tree = std::chrono::steady_clock::now();

    // Reserve capacity to avoid any vector reallocation while we are
    // reading elements by index during level construction. Balanced tree
    // with N leaves has at most 2*N-1 nodes.
    g_prime_tree.reserve(g_prime_blocks.size() * 2 + 4);

    // Level 0: one tree node per prime block (the leaves).
    std::vector<int32_t> prev_level;
    prev_level.reserve(g_prime_blocks.size());
    for (size_t bi = 0; bi < g_prime_blocks.size(); bi++) {
        PrimeTreeNode node;
        mpz_init_set(node.product, g_prime_blocks[bi].product);
        node.left_child = -1;
        node.right_child = -1;
        node.leaf_block_idx = static_cast<int32_t>(bi);
        g_prime_tree.push_back(std::move(node));
        prev_level.push_back(static_cast<int32_t>(g_prime_tree.size() - 1));
    }

    int depth = 0;
    while (prev_level.size() > 1) {
        std::vector<int32_t> next_level;
        next_level.reserve((prev_level.size() + 1) / 2);
        for (size_t i = 0; i + 1 < prev_level.size(); i += 2) {
            PrimeTreeNode node;
            mpz_init(node.product);
            mpz_mul(node.product,
                    g_prime_tree[prev_level[i]].product,
                    g_prime_tree[prev_level[i + 1]].product);
            node.left_child = prev_level[i];
            node.right_child = prev_level[i + 1];
            node.leaf_block_idx = -1;
            g_prime_tree.push_back(std::move(node));
            next_level.push_back(static_cast<int32_t>(g_prime_tree.size() - 1));
        }
        // Carry trailing odd node to next level unchanged.
        if (prev_level.size() & 1) {
            next_level.push_back(prev_level.back());
        }
        prev_level = std::move(next_level);
        depth++;
    }
    g_prime_tree_root = prev_level.empty() ? -1 : prev_level[0];
    g_prime_tree_max_depth = depth;

    const auto t_tree_end = std::chrono::steady_clock::now();
    const auto tree_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_tree_end - t_tree).count();

    // Rough memory estimate: root product size in limbs (dominates tree).
    const size_t root_limbs = g_prime_tree_root >= 0
        ? mpz_size(g_prime_tree[g_prime_tree_root].product) : 0;

    LogPrintf("Sieve: %zu primes up to %u, %zu leaves, product tree depth=%d nodes=%zu "
              "root=%zu limbs (%zu ms build), first_large_idx=%u (p=%u), presieve=%s\n",
              g_primes.size(), SIEVE_LIMIT, g_prime_blocks.size(),
              g_prime_tree_max_depth, g_prime_tree.size(), root_limbs, tree_ms,
              g_first_large_prime_idx,
              g_first_large_prime_idx < g_primes.size() ? g_primes[g_first_large_prime_idx] : 0,
              presieve_tables_ready() ? "ready" : "disabled");
}

// Ensure the thread-local tree descent scratch is sized & initialized for
// the current tree depth. Called on first descent from each thread.
static void EnsureTreeScratch()
{
    const size_t needed = static_cast<size_t>(g_prime_tree_max_depth) * 2 + 2;
    if (!g_tree_scratch_inited) {
        g_tree_scratch_lr.resize(needed);
        for (size_t i = 0; i < needed; i++) mpz_init(g_tree_scratch_lr[i].val);
        g_tree_scratch_inited = true;
        return;
    }
    if (g_tree_scratch_lr.size() < needed) {
        const size_t old = g_tree_scratch_lr.size();
        g_tree_scratch_lr.resize(needed);
        for (size_t i = old; i < needed; i++) mpz_init(g_tree_scratch_lr[i].val);
    }
}

// Recursive descent of the product tree.
// `rem` is the current remainder (base mod product of this subtree's siblings).
// Writes leaf-level remainders to rems[start_idx .. start_idx+count-1].
static void DescendTree(int32_t node_idx, const mpz_t rem, int depth,
                        std::vector<unsigned long>& rems)
{
    const PrimeTreeNode& node = g_prime_tree[node_idx];

    // Leaf case: extract per-prime remainders via cheap mpz_fdiv_ui
    if (node.leaf_block_idx >= 0) {
        const PrimeBlock& block = g_prime_blocks[node.leaf_block_idx];
        for (unsigned j = 0; j < block.count; j++) {
            rems[block.start_idx + j] = mpz_fdiv_ui(rem, g_primes[block.start_idx + j]);
        }
        return;
    }

    // Internal case: compute rem mod each child's product, then recurse.
    // Top-of-tree optimization: if the child product is already larger than
    // rem, the remainder equals rem itself — no reduction needed.
    const PrimeTreeNode& left = g_prime_tree[node.left_child];
    const PrimeTreeNode& right = g_prime_tree[node.right_child];

    mpz_ptr left_scratch = g_tree_scratch_lr[depth * 2].val;
    mpz_ptr right_scratch = g_tree_scratch_lr[depth * 2 + 1].val;

    if (mpz_cmpabs(left.product, rem) > 0) {
        DescendTree(node.left_child, rem, depth + 1, rems);
    } else {
        mpz_fdiv_r(left_scratch, rem, left.product);
        DescendTree(node.left_child, left_scratch, depth + 1, rems);
    }

    if (mpz_cmpabs(right.product, rem) > 0) {
        DescendTree(node.right_child, rem, depth + 1, rems);
    } else {
        mpz_fdiv_r(right_scratch, rem, right.product);
        DescendTree(node.right_child, right_scratch, depth + 1, rems);
    }
}

// Compute all sieve remainders via product-tree descent.
// At 12K bits, the top ~8-9 levels are skipped because the accumulated
// product > N; the effective tree depth is ~6-7 non-trivial levels.
static void ComputeSieveRemainders(const mpz_t base, std::vector<unsigned long>& rems)
{
    if (g_prime_tree_root < 0) {
        // Defensive fallback: use flat grouped-product if tree is missing.
        mpz_t block_rem;
        mpz_init(block_rem);
        for (const auto& block : g_prime_blocks) {
            mpz_fdiv_r(block_rem, base, block.product);
            for (unsigned j = 0; j < block.count; j++) {
                rems[block.start_idx + j] = mpz_fdiv_ui(block_rem, g_primes[block.start_idx + j]);
            }
        }
        mpz_clear(block_rem);
        return;
    }

    EnsureTreeScratch();
    DescendTree(g_prime_tree_root, base, 0, rems);
}

#ifdef HAVE_GWNUM
#include <atomic>
#include <thread>

// Number of gwnum threads for FFT squaring. At 4K+ bits the FFT length
// is large enough that 2+ threads can help on multi-core VPS nodes.
// Perf profiling at shift=4096 shows FMA3 FFT kernels consume >70% of
// validation cycles (yfft_r4_256_op_FMA3 22%, yfft_r4_256_op_ac_FMA3 18%,
// yr1FMA3 15%) — multi-threaded FFT splits that work across cores.
//
// Chosen dynamically at first use from hardware_concurrency(), capped at
// 2 so that a 2-vcpu validator still has a core for networking/RPC work.
// Single-vcpu nodes fall back to 1 thread (same as before).
static int GwnumThreadCount()
{
    static const int n = []() {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) return 1;
        if (hc == 1) return 1;
        return 2;
    }();
    return n;
}

// Number of WORKER threads for parallel Miller-Rabin testing across
// candidates. Each worker has its own gwhandle and tests independent
// survivors concurrently. First worker to find a prime (smallest offset
// wins via atomic CAS) terminates the others.
//
// Trade-off vs gwnum's internal FFT threading:
//   - N parallel workers each with gwnum threads=1 → ~N x candidate throughput
//     (clean scaling if not memory-bandwidth bound)
//   - 1 worker with gwnum threads=N → ~N x FFT speedup per candidate
//     but Amdahl losses (thread-pool setup + sync overhead)
// Perf profile (shift=4096): gwnum threads=2 gives only -13% per-call at
// 8448 bits and 0% at 4352 bits. Parallel MR at 2 workers gives nearly
// linear scaling because the FFT hot path is compute-bound (FMA3 instructions,
// not memory).
//
// On 2+ cores, always use parallel workers. On single-core, fall back to
// serial (1 worker = serial code path).
static int ParallelMRWorkerCount()
{
    static const int n = []() {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) return 1;
        if (hc == 1) return 1;
        return 2;  // cap at 2 to leave a core for networking/RPC
    }();
    return n;
}

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
    size_t nlimbs, mpz_t& res,
    int gw_threads_override = 0)
{
    const size_t cand_nlimbs = (mpz_sizeinbase(candidate, 2) + 63) / 64;
    std::memset(mod_array, 0, nlimbs * sizeof(uint64_t));
    mpz_export(mod_array, nullptr, -1, 8, -1, 0, candidate);

    gwhandle gw;
    gwinit(&gw);
    gwset_maxmulbyconst(&gw, 2);
    // Multi-threaded gwnum FFT only pays off when the candidate is large
    // enough that per-test FFT work dominates thread-pool setup+sync overhead.
    // Measured on 2vCPU Cascade Lake:
    //   shift=2048 (2304 bits): threads=1 is 11% faster than threads=2
    //   shift=4096 (4352 bits): threads=1 is 10% faster than threads=2
    //   shift=8192 (8448 bits): threads=2 is 13% faster than threads=1
    //   shift=12000 (12256 bits): threads=2 is expected >20% faster
    // Threshold set at 6000 bits (between the 4K and 8K data points).
    //
    // Parallel-MR callers override this: each of N parallel worker threads
    // runs gwnum with threads=1 so that N workers × 1 FFT thread = N cores
    // busy, which beats 1 worker × N FFT threads due to Amdahl overhead.
    int n_threads;
    if (gw_threads_override > 0) {
        n_threads = gw_threads_override;
    } else {
        const size_t cand_bits_n = mpz_sizeinbase(candidate, 2);
        n_threads = (cand_bits_n > 6000) ? GwnumThreadCount() : 1;
    }
    gwset_num_threads(&gw, n_threads);

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
            bool interrupted = false;
            for (long bi = static_cast<long>(d_bits) - 2; bi >= 0; bi--) {
                if (IsPowInterrupted()) { interrupted = true; break; }
                gwsquare2(&gw, x, x, 0);
                if (mpz_tstbit(d, bi)) {
                    gwsmallmul(&gw, 2.0, x);
                }
            }
            if (interrupted) {
                mpz_clear(d);
                gwfree(&gw, x);
                gwdone(&gw);
                return false;
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
                    if (IsPowInterrupted()) break;
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

    // SIMD presieve sieve is bit-packed: 1 bit per odd candidate.
    // SEG = 65536 candidates → 8192 bytes = 8 KB, fits in L1D on every CPU
    // (vs 64 KB byte-per-candidate which spills L1D on typical 32 KB L1Ds).
    // Alignment 64 is required by AVX-512 presieve kernels.
    static_assert((SEG % 64) == 0, "SEG must be a multiple of 64 for bit-packed sieve");
    static constexpr unsigned SEG_BYTES = SEG / 8;   // 8192
    static constexpr unsigned SEG_WORDS = SEG / 64;  // 1024
    alignas(64) uint64_t sieve_bits[SEG_WORDS];

    mpz_t base, candidate;
    mpz_init_set(base, n);
    mpz_add_ui(base, base, 1);
    if (mpz_even_p(base)) mpz_add_ui(base, base, 1);
    // base is now the first odd candidate (>= n+1).

    // Align an even starting point for presieve. presieve_set_base_offsets
    // requires mpz_start even with (mpz_start/2) byte-aligned — multiple of
    // 16 is sufficient. We round base down to the previous multiple of 16.
    //
    //   low4            = base mod 16   (odd in [1..15])
    //   base_aligned    = base - low4   (even, divisible by 16)
    //   sieve_start     = base - (low4 - 1)  (first odd > base_aligned; ≤ base)
    //   skip_bits_first = (low4 - 1) / 2   (bits in segment 0 before `base`)
    //
    // Bit k of segment s corresponds to candidate:
    //   c(s,k) = sieve_start + 2·(SEG·s + k) = base + 2k' - (low4 - 1)
    // where k' = SEG·s + k. At (s=0, k=skip_bits_first) this equals base.
    const unsigned long low4 = mpz_fdiv_ui(base, 16);
    const unsigned skip_bits_first = static_cast<unsigned>((low4 - 1) / 2);

    mpz_t base_aligned, sieve_start;
    mpz_init_set(base_aligned, base);
    mpz_sub_ui(base_aligned, base_aligned, low4);         // even, divisible by 16
    mpz_init_set(sieve_start, base);
    mpz_sub_ui(sieve_start, sieve_start, low4 - 1);        // odd, == base_aligned + 1

    const auto t_sieve_init = std::chrono::steady_clock::now();

    // Grouped-product sieve init: compute all remainders against sieve_start.
    // (Candidates are sieve_start + 2k, so rems must be sieve_start mod p.)
    std::vector<unsigned long> rems(g_primes.size());
    ComputeSieveRemainders(sieve_start, rems);

    // Set up SIMD presieve base offsets for this validation call.
    const bool use_presieve = presieve_tables_ready();
    if (use_presieve) {
        presieve_set_base_offsets(base_aligned);
    }

    const auto t_sieve_init_end = std::chrono::steady_clock::now();
    const auto init_us = std::chrono::duration_cast<std::chrono::microseconds>(t_sieve_init_end - t_sieve_init).count();
    g_validation_stats.total_sieve_init_us.fetch_add(init_us, std::memory_order_relaxed);
    LogDebug(BCLog::BENCH, "GwnumNextPrime: sieve init (%zu primes, %zu blocks, presieve=%s) took %lld ms\n",
             g_primes.size(), g_prime_blocks.size(), use_presieve ? "on" : "off", init_us / 1000);

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
        mpz_clear(base_aligned);
        mpz_clear(sieve_start);
        return;
    }

    mpz_t nm1, res;
    mpz_init(nm1);
    mpz_init(res);

    mpz_init(candidate);

    // Scalar marking covers all primes NOT handled by the SIMD presieve.
    // simd_presieve tables cover primes 7..163 (inclusive). Primes 2, 3, 5 and
    // all primes > 163 must be marked by the scalar loop.
    //  - Prime 2 is always skipped (odd-only sieve representation).
    //  - Primes 3 and 5 are NOT in any presieve table — skipping them causes
    //    ~1.89x more survivors (factor of (3/2)*(5/4) = 1.875) which doubles
    //    the cost of the downstream Miller-Rabin loop. Do NOT omit them.
    //  - Primes 7..163 are presieved (skip them in the scalar loop when
    //    presieve is active to avoid redundant work).
    //  - Primes > 163 must be scalar-marked in all cases.
    //
    // When presieve is unavailable, fall back to marking all odd primes
    // (matching the pre-Tier-1.5 semantics).
    unsigned total_fermat = 0;
    unsigned total_survivors = 0;
    int64_t total_fermat_us = 0;

    for (unsigned seg = 0; ; seg++) {
        const auto t_seg = std::chrono::steady_clock::now();

        // Phase 1: initialize sieve (1 = composite, 0 = candidate).
        const uint64_t seg_low_bytes = static_cast<uint64_t>(seg) * SEG_BYTES;
        if (use_presieve) {
            uint8_t* sieve_u8 = reinterpret_cast<uint8_t*>(sieve_bits);
            presieve_init(sieve_u8, SEG_BYTES, seg_low_bytes);
            presieve_apply(sieve_u8, SEG_BYTES, seg_low_bytes);
        } else {
            std::memset(sieve_bits, 0, SEG_BYTES);
        }

        // Phase 2: in segment 0 only, mask off the `skip_bits_first` bits
        // that precede `base` (they represent odd numbers < base).
        if (seg == 0 && skip_bits_first > 0) {
            for (unsigned i = 0; i < skip_bits_first; i++) {
                sieve_bits[i >> 6] |= (uint64_t{1} << (i & 63));
            }
        }

        // Phase 3: scalar marking. Cover everything not in the presieve tables.
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            if (p == 2) continue;
            // Skip primes already covered by presieve (7..163 inclusive).
            // Primes 3 and 5 are NOT in presieve — always mark them here.
            if (use_presieve && p >= 7 && p <= static_cast<unsigned>(PRESIEVE_MAX_PRIME)) continue;

            unsigned long r = rems[pi];
            unsigned long inv2 = (static_cast<unsigned long>(p) + 1) / 2;
            unsigned long si = (r == 0) ? 0 : ((static_cast<unsigned long>(p) - r) % p * inv2) % p;
            for (unsigned long idx = si; idx < SEG; idx += p) {
                sieve_bits[idx >> 6] |= (uint64_t{1} << (idx & 63));
            }
        }

        const auto t_seg_end = std::chrono::steady_clock::now();
        const auto seg_us = std::chrono::duration_cast<std::chrono::microseconds>(t_seg_end - t_seg).count();
        g_validation_stats.total_sieve_segment_us.fetch_add(seg_us, std::memory_order_relaxed);

        // Count survivors (bit == 0)
        unsigned seg_survivors = 0;
        for (unsigned w = 0; w < SEG_WORDS; w++) {
            seg_survivors += static_cast<unsigned>(std::popcount(~sieve_bits[w]));
        }
        // Account for the always-composite bits we added in seg 0 so the
        // reported survivor count reflects real surviving candidates.
        // (popcount(~x) already excludes the set bits, so no adjustment needed.)
        total_survivors += seg_survivors;

        // Collect survivor offsets up front so workers can pull by index.
        // Survivors are in monotonically increasing offset order, so a
        // monotonic fetch-add counter naturally gives workers candidates in
        // ascending offset — the smallest-offset prime wins via atomic CAS.
        std::vector<unsigned long> offsets;
        offsets.reserve(seg_survivors + 1);
        for (unsigned i = 0; i < SEG; i++) {
            if (sieve_bits[i >> 6] & (uint64_t{1} << (i & 63))) continue; // composite
            const long long k_total = static_cast<long long>(SEG) * seg + i;
            const long long off_ll = 2 * k_total - static_cast<long long>(low4 - 1);
            if (off_ll < 0) continue;
            offsets.push_back(static_cast<unsigned long>(off_ll));
        }

        // Test survivors with gwnum Miller-Rabin base-2 screen, parallelized
        // across N worker threads. Each worker has its own gwhandle + scratch
        // so gwnum's internal state never crosses threads.
        const int n_workers = ParallelMRWorkerCount();
        std::atomic<size_t> next_idx{0};
        std::atomic<long long> winning_off{-1};
        std::atomic<unsigned> seg_fermat{0};
        std::atomic<int64_t> seg_fermat_us{0};
        std::atomic<int64_t> seg_bpsw_us{0};

        auto worker_fn = [&](int worker_id) {
            // Per-worker scratch buffers (aligned_calloc for gwnum's 64-byte
            // alignment requirement on Windows/MSVC).
            uint64_t* mod_array_w = static_cast<uint64_t*>(aligned_calloc(nlimbs, sizeof(uint64_t)));
            uint64_t* result_array_w = static_cast<uint64_t*>(aligned_calloc(nlimbs + 1, sizeof(uint64_t)));
            if (!mod_array_w || !result_array_w) {
                aligned_free(mod_array_w);
                aligned_free(result_array_w);
                return;
            }

            mpz_t candidate_w, nm1_w, res_w;
            mpz_init(candidate_w);
            mpz_init(nm1_w);
            mpz_init(res_w);

            while (true) {
                if (IsPowInterrupted()) break;
                const size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= offsets.size()) break;

                // Early exit: a smaller offset has already been claimed by a
                // concurrent worker. No point testing larger offsets.
                const long long winner_now = winning_off.load(std::memory_order_acquire);
                const long long this_off = static_cast<long long>(offsets[idx]);
                if (winner_now >= 0 && this_off > winner_now) break;

                mpz_set(candidate_w, base);
                mpz_add_ui(candidate_w, candidate_w, offsets[idx]);
                mpz_sub_ui(nm1_w, candidate_w, 1);

                const auto t_fw = std::chrono::steady_clock::now();

                // Force gwnum threads=1 per worker so that N workers × 1 FFT
                // thread = N cores, instead of contending with gwnum's own
                // multi-threaded FFT.
                const bool mr_pass = GwnumMillerRabin2(candidate_w, nm1_w,
                                                       mod_array_w, result_array_w,
                                                       nlimbs, res_w,
                                                       /*gw_threads_override=*/1);

                const auto t_fw_end = std::chrono::steady_clock::now();
                const int64_t fw_us = std::chrono::duration_cast<std::chrono::microseconds>(t_fw_end - t_fw).count();
                seg_fermat.fetch_add(1, std::memory_order_relaxed);
                seg_fermat_us.fetch_add(fw_us, std::memory_order_relaxed);

                TRACEPOINT(pow, fermat_test,
                           (int64_t)mpz_sizeinbase(candidate_w, 2),
                           mr_pass ? 1 : 0, fw_us);

                if (!mr_pass) continue;

                // MR passed — confirm with full GMP BPSW
                const auto t_bw = std::chrono::steady_clock::now();
                if (mpz_probab_prime_p(candidate_w, 0) > 0) {
                    const auto t_bw_end = std::chrono::steady_clock::now();
                    const int64_t bw_us = std::chrono::duration_cast<std::chrono::microseconds>(t_bw_end - t_bw).count();
                    seg_bpsw_us.fetch_add(bw_us, std::memory_order_relaxed);

                    // Atomically claim the winning offset, but only if ours is
                    // smaller than any concurrent winner. Loop over CAS to
                    // handle the rare case where two workers both find primes.
                    long long expected = winning_off.load(std::memory_order_relaxed);
                    while (true) {
                        if (expected >= 0 && this_off >= expected) break;  // larger; don't overwrite
                        if (winning_off.compare_exchange_weak(
                                expected, this_off,
                                std::memory_order_release,
                                std::memory_order_relaxed)) {
                            break;
                        }
                        // expected was updated by CAS; retry loop re-evaluates
                    }
                }
            }

            mpz_clear(candidate_w);
            mpz_clear(nm1_w);
            mpz_clear(res_w);
            aligned_free(mod_array_w);
            aligned_free(result_array_w);
        };

        if (n_workers <= 1 || offsets.empty()) {
            worker_fn(0);
        } else {
            std::vector<std::thread> workers;
            workers.reserve(n_workers);
            for (int w = 0; w < n_workers; w++) {
                workers.emplace_back(worker_fn, w);
            }
            for (auto& t : workers) t.join();
        }

        total_fermat += seg_fermat.load();
        total_fermat_us += seg_fermat_us.load();

        const long long winner = winning_off.load();
        if (winner >= 0) {
            const int64_t bpsw_us = seg_bpsw_us.load();

            // Accumulate profiling stats
            g_validation_stats.total_fermat_count.fetch_add(total_fermat, std::memory_order_relaxed);
            g_validation_stats.total_fermat_us.fetch_add(total_fermat_us, std::memory_order_relaxed);
            g_validation_stats.total_bpsw_confirm_us.fetch_add(bpsw_us, std::memory_order_relaxed);
            g_validation_stats.total_survivors.fetch_add(total_survivors, std::memory_order_relaxed);
            g_validation_stats.tier_gwnum.fetch_add(1, std::memory_order_relaxed);

            LogDebug(BCLog::BENCH, "GwnumNextPrime: found prime after %u segments, %u survivors, %u MR tests (%lld ms, %d workers), BPSW %lld ms, sieve_init=%lld ms\n",
                     seg + 1, total_survivors, total_fermat, total_fermat_us / 1000,
                     n_workers, bpsw_us / 1000, init_us / 1000);

            mpz_set(candidate, base);
            mpz_add_ui(candidate, candidate, static_cast<unsigned long>(winner));
            mpz_set(result, candidate);
            mpz_clear(nm1);
            mpz_clear(res);
            mpz_clear(base);
            mpz_clear(base_aligned);
            mpz_clear(sieve_start);
            mpz_clear(candidate);
            aligned_free(mod_array);
            aligned_free(result_array);
            return;
        }

        // Advance remainders for next segment (only primes we actually mark).
        // Must match the scalar marking loop's filter above so that rems[] stays
        // consistent for primes 3, 5, and 167+ (and unused for 2, 7..163).
        for (size_t pi = 0; pi < g_primes.size(); pi++) {
            unsigned p = g_primes[pi];
            if (p == 2) continue;
            if (use_presieve && p >= 7 && p <= static_cast<unsigned>(PRESIEVE_MAX_PRIME)) continue;
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
