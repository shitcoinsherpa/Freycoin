// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Freycoin Advanced Mining Engine
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 * His prime gap discoveries still hold the #17 spot on global merit rankings.
 *
 * This implementation incorporates state-of-the-art optimizations from:
 * - Kim Walisch's primesieve (segmented sieve, L1/L2 cache optimization)
 * - Tomás Oliveira e Silva's bucket algorithm
 * - Robert Gerbicz's prime gap search techniques (primegap-list-project)
 * - Dana Jacobsen's Math::Prime::Util (BPSW implementation)
 *
 * Architecture:
 *
 *   TIER 1 (CPU Only)     TIER 2 (CPU+OpenCL)    TIER 3 (CPU+CUDA)
 *         |                      |                      |
 *         v                      v                      v
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │              Segmented Sieve (CPU, L1 optimized)            │
 *   │  - 32KB segments fit in L1 cache                            │
 *   │  - Wheel-2310 filtering                                     │
 *   │  - Bucket algorithm for large primes                        │
 *   └─────────────────────────────────────────────────────────────┘
 *                               │
 *         ┌─────────────────────┼─────────────────────┐
 *         v                     v                     v
 *   ┌───────────┐       ┌───────────┐         ┌───────────┐
 *   │ CPU BPSW  │       │OpenCL Batch│        │CUDA Batch │
 *   │Sequential │       │  Fermat    │        │   BPSW    │
 *   └───────────┘       └───────────┘         └───────────┘
 */

#ifndef FREYCOIN_POW_MINING_ENGINE_H
#define FREYCOIN_POW_MINING_ENGINE_H

#include <pow/pow_common.h>
#include <pow/pow.h>
#include <pow/pow_utils.h>
#include <pow/pow_processor.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <gmp.h>

// Forward declarations
class SegmentedSieve;
class PrimalityTester;
class MiningPipeline;

/**
 * GPU batch request: submitted by CPU sieve threads, processed by GPU worker.
 * Each request carries a candidate batch and a result buffer. The submitting
 * thread blocks on the condition variable until the GPU thread sets done=true.
 */
struct GPURequest {
    CandidateBatch batch;
    std::vector<uint8_t> results;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> done{false};
};

/**
 * SegmentedSieve: L1 cache-optimized segmented sieve
 *
 * Key features:
 * - Processes sieve in L1-cache-sized segments (32KB)
 * - Uses bucket algorithm for large primes (Oliveira e Silva)
 * - Pre-sieve with small primes using SIMD when available
 * - Odd-only indexing (bit k = odd number at offset 2k+1 from base)
 */
class SegmentedSieve {
public:
    SegmentedSieve(uint64_t n_primes, uint64_t total_sieve_size);
    ~SegmentedSieve();

    // Non-copyable
    SegmentedSieve(const SegmentedSieve&) = delete;
    SegmentedSieve& operator=(const SegmentedSieve&) = delete;

    /** Initialize sieving for a new hash */
    void init_for_hash(mpz_t mpz_start);

    /** Sieve the next segment. Returns true if more segments remain. */
    bool sieve_next_segment();

    /** Get candidates from current segment that pass wheel filter */
    void get_candidates(std::vector<uint64_t>& candidates, uint64_t base_mod);

    /** Check if a position is marked as composite */
    bool is_composite(uint64_t pos) const;

    /** Get current segment offset */
    uint64_t get_segment_offset() const;

    /** Get total sieve size */
    uint64_t get_total_size() const;

    /** Get statistics */
    const MiningStats& get_stats() const { return stats; }

private:
    uint64_t n_primes;
    uint32_t* primes;

    uint64_t* segment;
    uint64_t segment_words;
    uint64_t current_segment;
    uint64_t total_segments;
    uint64_t total_size;

    uint32_t small_prime_limit;
    std::vector<Bucket> buckets;
    uint32_t* large_prime_starts;
    uint32_t* small_starts;  // Cached starting bit for each small prime in next segment

    mpz_t mpz_start;
    bool hash_initialized;

    MiningStats stats;

    void init_primes(uint64_t n);
    void init_presieve();
    void init_buckets();
    void calc_starts();
    void sieve_small_primes();
    void process_bucket();
    void advance_buckets();
};

/**
 * PrimalityTester: BPSW and Fermat primality testing
 */
class PrimalityTester {
public:
    PrimalityTester();
    ~PrimalityTester();

    // Non-copyable
    PrimalityTester(const PrimalityTester&) = delete;
    PrimalityTester& operator=(const PrimalityTester&) = delete;

    /** BPSW primality test (Miller-Rabin base 2 + Strong Lucas) */
    bool bpsw_test(mpz_t n);

    /** Miller-Rabin test with specific base */
    bool miller_rabin(mpz_t n, unsigned long base = 2);

    /** Strong Lucas test with Selfridge parameters */
    bool strong_lucas_selfridge(mpz_t n);

    /** Fermat test (for GPU compatibility) */
    bool fermat_test(mpz_t n);

    /** Batch prepare candidates for GPU testing */
    void prepare_batch(CandidateBatch& batch,
                       mpz_t mpz_start,
                       const std::vector<uint64_t>& candidates,
                       uint32_t max_bits = 320);

private:
    mpz_t mpz_tmp, mpz_d, mpz_s, mpz_a, mpz_x;
    mpz_t mpz_two, mpz_n_minus_1;
    mpz_t mpz_u, mpz_v, mpz_q;

    long find_selfridge_d(mpz_t n);
    void lucas_sequence(mpz_t u, mpz_t v, mpz_t n, long p, long q, mpz_t k);
};

/**
 * MiningPipeline: Async CPU-GPU pipeline for hybrid mining
 */
class MiningPipeline {
public:
    MiningPipeline(MiningTier tier, uint32_t n_threads = 1);
    ~MiningPipeline();

    // Non-copyable
    MiningPipeline(const MiningPipeline&) = delete;
    MiningPipeline& operator=(const MiningPipeline&) = delete;

    void set_processor(PoWProcessor* processor);
    void start_mining(PoW* pow, std::vector<uint8_t>* offset = nullptr);
    void stop_mining();
    void wait_for_completion();
    bool is_mining() const { return mining_active.load(); }
    MiningStatsSnapshot get_stats() const;

private:
    MiningTier tier;
    uint32_t n_threads;
    std::atomic<bool> mining_active{false};
    std::atomic<bool> shutdown_requested{false};

    std::vector<std::thread> sieve_threads;
    std::thread gpu_thread;

    std::queue<CandidateBatch> gpu_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    PoW* current_pow;
    PoWProcessor* processor;
    MiningStats stats;

    std::mutex gap_mutex;
    uint64_t last_prime_offset;
    bool have_first_prime;
    uint64_t min_gap;
    mpz_t mpz_start;
    mpz_t mpz_adder;
    bool gap_state_initialized;

    void sieve_worker(uint32_t thread_id);
    void gpu_worker();
    void process_gpu_results(const CandidateBatch& batch,
                             const std::vector<uint8_t>& results);
    void init_gap_state(mpz_t start, uint64_t target_gap);
    void reset_gap_state();
};

/**
 * MiningEngine: Main entry point for mining
 *
 * Detects available hardware and creates appropriate pipeline.
 * Supports two modes:
 * 1. run_sieve(): Single PoW, single nonce - caller handles nonce iteration
 * 2. mine_parallel(): Parallel nonce iteration - each thread has own nonce
 */
class MiningEngine {
public:
    /** Create mining engine with auto-detected tier */
    MiningEngine();

    /** Create mining engine with auto-detected tier and specific thread count */
    explicit MiningEngine(unsigned int num_threads);

    /** Create mining engine with specific tier */
    explicit MiningEngine(MiningTier tier);

    /** Create mining engine with specific tier and thread count */
    MiningEngine(MiningTier tier, unsigned int num_threads);

    ~MiningEngine();

    // Non-copyable
    MiningEngine(const MiningEngine&) = delete;
    MiningEngine& operator=(const MiningEngine&) = delete;

    /** Get detected mining tier */
    MiningTier get_tier() const { return tier; }

    /** Get hardware description string */
    const char* get_hardware_info() const { return hardware_info; }

    /**
     * Run mining for a single PoW (single nonce).
     * Use this when caller handles nonce iteration externally.
     */
    void run_sieve(PoW* pow, PoWProcessor* processor,
                   std::vector<uint8_t>* offset = nullptr);

    /**
     * Mine with parallel nonce iteration (production mining).
     * Each thread gets its own nonce, computes hash, sieves independently.
     *
     * @param header_template Serialized block header
     * @param nonce_offset Byte offset of nonce in header
     * @param shift PoW shift value
     * @param target_difficulty Target difficulty
     * @param start_nonce Starting nonce value
     * @param processor Callback for valid results
     */
    void mine_parallel(const std::vector<uint8_t>& header_template,
                       size_t nonce_offset,
                       uint16_t shift,
                       uint64_t target_difficulty,
                       uint32_t start_nonce,
                       PoWProcessor* processor);

    /** Set GPU intensity (1-10). Controls sieve range per nonce.
     *  Higher = more work per nonce, better GPU utilization. Default: 5. */
    void set_gpu_intensity(int intensity);

    /** Compute minimum shift needed for a given intensity level.
     *  Ensures 2^shift / 2 >= sieve_cap so the sieve can use its full range. */
    static uint16_t compute_shift(int intensity);

    /** Stop any ongoing mining operation (blocks until workers finish) */
    void stop();

    /** Signal workers to stop without waiting (non-blocking, GUI-safe) */
    void request_stop();

    /** Check if mining is active */
    bool is_mining() const;

    /** Get pipeline statistics */
    MiningStatsSnapshot get_stats() const;

private:
    MiningTier tier;
    uint32_t n_threads;
    int m_gpu_intensity{5};
    char hardware_info[256];
    std::unique_ptr<MiningPipeline> pipeline;

    std::atomic<bool> parallel_mining_active{false};
    std::atomic<bool> stop_requested{false};
    std::vector<std::thread> mining_threads;

    // Shared stats for parallel mining (updated by worker threads)
    std::atomic<uint64_t> par_primes{0};
    std::atomic<uint64_t> par_gaps{0};
    std::atomic<uint64_t> par_tests{0};
    std::atomic<uint64_t> par_nonces{0};

    // GPU worker threads — one per device, persist across mine_parallel calls.
    // Initialized on first mine_parallel, destroyed with the engine.
    struct GPUWorker {
        int device_id;
        std::thread thread;
        std::queue<std::shared_ptr<GPURequest>> queue;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> initialized{false};
    };
    std::vector<std::unique_ptr<GPUWorker>> gpu_workers;
    std::atomic<bool> gpu_initialized{false};  // True when at least one GPU is ready
    std::atomic<bool> gpu_shutdown{false};
    std::atomic<int> gpu_round_robin{0};  // For distributing work across GPUs
    int num_gpu_devices{0};

    void ensure_gpu_running();
    void gpu_worker_func(GPUWorker* worker);

    /** Submit a GPU request to the least-loaded GPU worker */
    void submit_gpu_request(std::shared_ptr<GPURequest> request);

    MiningTier detect_tier();
    void parallel_worker(uint32_t thread_id,
                         std::vector<uint8_t> header_template,
                         size_t nonce_offset,
                         uint16_t shift,
                         uint64_t target_difficulty,
                         uint32_t start_nonce,
                         PoWProcessor* processor);
};

#endif // FREYCOIN_POW_MINING_ENGINE_H
