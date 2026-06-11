/**
 * CGBN-based Fermat Primality Test Kernel for Freycoin
 *
 * Copyright (c) 2025 The Freycoin developers
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Uses NVIDIA's CGBN (Cooperative Groups Big Numbers) library for
 * warp-cooperative Montgomery multiplication. CGBN distributes a single
 * big-number operation across multiple threads in a warp, achieving
 * lower latency per operation than per-thread Montgomery.
 *
 * Pre-fork kernels (320/384-bit):
 *   TPI=8/16 — multiple instances per warp for high throughput
 *
 * Post-fork kernels (512-bit through 16640-bit):
 *   TPI=32 — one instance per warp, necessary for large limb counts
 *
 * Supported bit widths (CGBN_BITS must be compile-time constant):
 *   320, 384, 512, 1024, 1280, 2048, 4096, 8192, 8448, 12288, 16384, 16640
 *
 * At runtime, the requested bit width is rounded UP to the nearest supported
 * tier. Data is zero-padded to the tier's limb count.
 *
 * Requires: CGBN headers from https://github.com/NVlabs/CGBN
 * Build with: -DWITH_CGBN=ON -DCGBN_INCLUDE_DIR=/path/to/cgbn/include
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifdef HAVE_CGBN

#include <cuda_runtime.h>
#include <stdint.h>
#include <string.h>
#include <cgbn/cgbn.h>

/*
 * Macro to define a CGBN Fermat kernel at a given TPI and BITS.
 *
 * Each kernel computes 2^(p-1) mod p for a batch of candidate primes.
 * CGBN handles Montgomery form conversion, modular exponentiation, and
 * reduction internally using optimized PTX instructions.
 *
 * The extern "C" linkage gives unmangled names for CUDA Driver API lookup
 * via cuModuleGetFunction().
 */
#define DEFINE_CGBN_FERMAT_KERNEL(SUFFIX, TPI, BITS)                           \
extern "C" __global__ void cgbn_fermat_kernel_##SUFFIX(                        \
    uint8_t *results, const uint32_t *primes, uint32_t count)                  \
{                                                                              \
    int32_t instance = (blockIdx.x * blockDim.x + threadIdx.x) / (TPI);       \
    if (instance >= (int32_t)count) return;                                    \
                                                                               \
    typedef cgbn_context_t<(TPI), cgbn_default_parameters_t> ctx_t;            \
    typedef cgbn_env_t<ctx_t, (BITS)> env_t;                                   \
                                                                               \
    ctx_t bn_context(cgbn_no_checks);                                          \
    env_t bn_env(bn_context);                                                  \
                                                                               \
    typename env_t::cgbn_t p, base, exp, result, one;                          \
                                                                               \
    const uint32_t limbs = (BITS) / 32;                                        \
    cgbn_load(bn_env, p,                                                       \
              (cgbn_mem_t<(BITS)>*)&primes[instance * limbs]);                  \
                                                                               \
    cgbn_set_ui32(bn_env, base, 2);                                            \
    cgbn_sub_ui32(bn_env, exp, p, 1);                                          \
    cgbn_modular_power(bn_env, result, base, exp, p);                          \
                                                                               \
    cgbn_set_ui32(bn_env, one, 1);                                             \
    bool is_prime = cgbn_equals(bn_env, result, one);                          \
                                                                               \
    if (threadIdx.x % (TPI) == 0) {                                            \
        results[instance] = is_prime ? 1 : 0;                                  \
    }                                                                          \
}

/* =========================================================================
 * Pre-fork kernels (high throughput for small numbers)
 * ========================================================================= */

/* 320-bit: TPI=8, 4 instances per warp. Shift 14-64 (typical pre-fork). */
DEFINE_CGBN_FERMAT_KERNEL(320, 8, 320)

/* 384-bit: TPI=16, 2 instances per warp. Covers 352-bit (shift 65-128). */
DEFINE_CGBN_FERMAT_KERNEL(384, 16, 384)

/* 512-bit: TPI=16, 2 instances per warp. Full pre-fork range (shift up to 256). */
DEFINE_CGBN_FERMAT_KERNEL(512, 16, 512)

/* =========================================================================
 * Post-fork kernels (TPI=32: one instance per warp, for large numbers)
 *
 * Tier selection requires >=32 bits of slack so a modulus can never fill
 * its environment: CGBN's Barrett reduction takes a clz-dependent
 * normalization path at full width that upstream never validated
 * (cf. NVlabs/CGBN #15, #23). Tier 16672 covers consensus-max inputs
 * (shift 16384 = 16640 bits) with slack.
 *
 * Coverage map (shift → total bits → CGBN tier, 32-bit slack rule):
 *   shift  257-736   → bits  513-992   → tier 1024
 *   shift  737-992   → bits  993-1248  → tier 1280
 *   shift  993-1760  → bits 1249-2016  → tier 2048
 *   shift 1761-3808  → bits 2017-4064  → tier 4096
 *   shift 3809-7904  → bits 4065-8160  → tier 8192
 *   shift 7905-8160  → bits 8161-8416  → tier 8448
 *   shift 8161-12000 → bits 8417-12256 → tier 12288 [mainnet MIN_SHIFT 12000]
 *   shift 12001-16096→ bits 12257-16352→ tier 16384
 *   shift 16097-16384→ bits 16353-16640→ tier 16672 [mainnet MAX_SHIFT 16384]
 * ========================================================================= */

DEFINE_CGBN_FERMAT_KERNEL(1024,  32, 1024)
DEFINE_CGBN_FERMAT_KERNEL(1280,  32, 1280)
DEFINE_CGBN_FERMAT_KERNEL(2048,  32, 2048)
DEFINE_CGBN_FERMAT_KERNEL(4096,  32, 4096)
DEFINE_CGBN_FERMAT_KERNEL(8192,  32, 8192)
DEFINE_CGBN_FERMAT_KERNEL(8448,  32, 8448)
DEFINE_CGBN_FERMAT_KERNEL(12288, 32, 12288)
DEFINE_CGBN_FERMAT_KERNEL(16384, 32, 16384)
DEFINE_CGBN_FERMAT_KERNEL(16640, 32, 16640)
DEFINE_CGBN_FERMAT_KERNEL(16672, 32, 16672)

/* =========================================================================
 * Tier selection: round requested bits UP to next supported CGBN kernel.
 *
 * Returns the CGBN_BITS tier and the corresponding TPI.
 * ========================================================================= */
struct CgbnTier {
    int bits;
    int tpi;
};

static CgbnTier select_tier(int requested_bits) {
    /* Ascending order — first tier with >=32 bits of slack (see above). */
    static const CgbnTier tiers[] = {
        {  320,  8},
        {  384, 16},
        {  512, 16},
        { 1024, 32},
        { 1280, 32},
        { 2048, 32},
        { 4096, 32},
        { 8192, 32},
        { 8448, 32},
        {12288, 32},
        {16384, 32},
        {16640, 32},
        {16672, 32},
    };
    static const int n_tiers = sizeof(tiers) / sizeof(tiers[0]);

    for (int i = 0; i < n_tiers; i++) {
        if (tiers[i].bits >= requested_bits + 32) {
            return tiers[i];
        }
    }
    /* Requested size exceeds all tiers — return largest */
    return tiers[n_tiers - 1];
}

/* =========================================================================
 * Host-side batch dispatcher
 *
 * Accepts arbitrary bit widths. Rounds up to nearest supported CGBN tier,
 * zero-pads input data to the tier's limb count, then dispatches to the
 * correct kernel instantiation.
 * ========================================================================= */
extern "C" {

int cgbn_fermat_batch(uint8_t *h_results,
                      const uint32_t *h_primes,
                      uint32_t count,
                      int bits) {
    if (count == 0) return 0;

    CgbnTier tier = select_tier(bits);
    int input_limbs = (bits + 31) / 32;
    int tier_limbs = tier.bits / 32;

    uint8_t *d_results;
    uint32_t *d_primes;

    size_t primes_size = (size_t)count * tier_limbs * sizeof(uint32_t);
    size_t results_size = (size_t)count * sizeof(uint8_t);

    cudaMalloc(&d_results, results_size);
    cudaMalloc(&d_primes, primes_size);

    /* If input limbs < tier limbs, we need to zero-pad each candidate */
    if (input_limbs < tier_limbs) {
        uint32_t* padded = (uint32_t*)calloc((size_t)count * tier_limbs, sizeof(uint32_t));
        if (!padded) {
            cudaFree(d_results);
            cudaFree(d_primes);
            return -1;
        }
        for (uint32_t i = 0; i < count; i++) {
            memcpy(&padded[i * tier_limbs],
                   &h_primes[i * input_limbs],
                   input_limbs * sizeof(uint32_t));
            /* Remaining limbs already zero from calloc */
        }
        cudaMemcpy(d_primes, padded, primes_size, cudaMemcpyHostToDevice);
        free(padded);
    } else {
        /* Input already matches tier size — direct copy */
        cudaMemcpy(d_primes, h_primes, primes_size, cudaMemcpyHostToDevice);
    }

    /* Dispatch to the correct kernel.
     * Block size = 128 threads. Instances per block = 128 / TPI. */
    int block_size = 128;
    int threads_needed = count * tier.tpi;
    int blocks = (threads_needed + block_size - 1) / block_size;

    switch (tier.bits) {
    case 320:
        cgbn_fermat_kernel_320<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 384:
        cgbn_fermat_kernel_384<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 512:
        cgbn_fermat_kernel_512<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 1024:
        cgbn_fermat_kernel_1024<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 1280:
        cgbn_fermat_kernel_1280<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 2048:
        cgbn_fermat_kernel_2048<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 4096:
        cgbn_fermat_kernel_4096<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 8192:
        cgbn_fermat_kernel_8192<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 8448:
        cgbn_fermat_kernel_8448<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 12288:
        cgbn_fermat_kernel_12288<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 16384:
        cgbn_fermat_kernel_16384<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 16640:
        cgbn_fermat_kernel_16640<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    case 16672:
        cgbn_fermat_kernel_16672<<<blocks, block_size>>>(d_results, d_primes, count);
        break;
    default:
        cudaFree(d_results);
        cudaFree(d_primes);
        return -1;  /* Unsupported tier — should never happen */
    }

    cudaMemcpy(h_results, d_results, results_size, cudaMemcpyDeviceToHost);

    cudaFree(d_results);
    cudaFree(d_primes);

    return 0;
}

int cgbn_is_available(void) {
    return 1;  /* If compiled with CGBN, it's available */
}

} /* extern "C" */

#endif /* HAVE_CGBN */
