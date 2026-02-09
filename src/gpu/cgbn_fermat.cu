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
 * For 320-bit numbers: TPI=8 threads per instance, 4 instances per warp
 * For 352-bit numbers: TPI=16 threads per instance, 2 instances per warp
 *
 * Requires: CGBN headers from https://github.com/NVlabs/CGBN
 * Build with: -DWITH_CGBN=ON -DCGBN_INCLUDE_DIR=/path/to/cgbn/include
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifdef HAVE_CGBN

#include <cuda_runtime.h>
#include <stdint.h>
#include <cgbn/cgbn.h>

/*
 * CGBN configuration for 320-bit and 384-bit (covers 352-bit) numbers.
 *
 * TPI (threads per instance): controls how many threads cooperate on one number.
 * - TPI=8: Each warp (32 threads) processes 4 numbers simultaneously.
 *   For 320-bit (10 × 32-bit limbs), 8 threads handle ~1.25 limbs each.
 * - TPI=16: Each warp processes 2 numbers.
 *   For 352-bit (11 × 32-bit limbs), 16 threads handle ~0.7 limbs each.
 *
 * BITS must be a multiple of 32. We use 320 and 384 (next multiple >= 352).
 */

/* 320-bit configuration: 8 threads per instance */
static const uint32_t CGBN_TPI_320 = 8;
static const uint32_t CGBN_BITS_320 = 320;

/* 384-bit configuration (for 352-bit numbers): 16 threads per instance */
static const uint32_t CGBN_TPI_352 = 16;
static const uint32_t CGBN_BITS_352 = 384;

/* Context type: no error checking for maximum performance */
typedef cgbn_context_t<CGBN_TPI_320, cgbn_default_parameters_t> context320_t;
typedef cgbn_env_t<context320_t, CGBN_BITS_320> env320_t;

typedef cgbn_context_t<CGBN_TPI_352, cgbn_default_parameters_t> context352_t;
typedef cgbn_env_t<context352_t, CGBN_BITS_352> env352_t;

/**
 * CGBN Fermat kernel for 320-bit numbers.
 *
 * Each instance uses TPI=8 threads to cooperatively compute 2^(p-1) mod p.
 * CGBN handles Montgomery form conversion, modular exponentiation, and
 * reduction internally using optimized PTX instructions.
 */
__global__ void cgbn_fermat_kernel_320(uint8_t *results,
                                        const uint32_t *primes,
                                        uint32_t count) {
    /* Calculate which instance this thread belongs to */
    int32_t instance = (blockIdx.x * blockDim.x + threadIdx.x) / CGBN_TPI_320;
    if (instance >= (int32_t)count) return;

    /* Create CGBN context and environment */
    context320_t bn_context(cgbn_no_checks);
    env320_t bn_env(bn_context);

    /* Declare big number variables */
    env320_t::cgbn_t p, base, exp, result, one;

    /* Load candidate prime from global memory */
    cgbn_load(bn_env, p, (cgbn_mem_t<CGBN_BITS_320>*)&primes[instance * 10]);

    /* base = 2 */
    cgbn_set_ui32(bn_env, base, 2);

    /* exp = p - 1 */
    cgbn_sub_ui32(bn_env, exp, p, 1);

    /* result = 2^(p-1) mod p using CGBN's optimized modular exponentiation */
    cgbn_modular_power(bn_env, result, base, exp, p);

    /* Check if result == 1 */
    cgbn_set_ui32(bn_env, one, 1);
    bool is_prime = cgbn_equals(bn_env, result, one);

    /* Only thread 0 of each instance writes the result */
    if (threadIdx.x % CGBN_TPI_320 == 0) {
        results[instance] = is_prime ? 1 : 0;
    }
}

/**
 * CGBN Fermat kernel for 352-bit numbers (using 384-bit CGBN width).
 */
__global__ void cgbn_fermat_kernel_352(uint8_t *results,
                                        const uint32_t *primes,
                                        uint32_t count) {
    int32_t instance = (blockIdx.x * blockDim.x + threadIdx.x) / CGBN_TPI_352;
    if (instance >= (int32_t)count) return;

    context352_t bn_context(cgbn_no_checks);
    env352_t bn_env(bn_context);

    env352_t::cgbn_t p, base, exp, result, one;

    /* Load 352-bit number into 384-bit container (upper bits zeroed by CGBN) */
    cgbn_load(bn_env, p, (cgbn_mem_t<CGBN_BITS_352>*)&primes[instance * 12]);

    cgbn_set_ui32(bn_env, base, 2);
    cgbn_sub_ui32(bn_env, exp, p, 1);
    cgbn_modular_power(bn_env, result, base, exp, p);

    cgbn_set_ui32(bn_env, one, 1);
    bool is_prime = cgbn_equals(bn_env, result, one);

    if (threadIdx.x % CGBN_TPI_352 == 0) {
        results[instance] = is_prime ? 1 : 0;
    }
}

/* Host-side wrapper functions */
extern "C" {

int cgbn_fermat_batch(uint8_t *h_results,
                      const uint32_t *h_primes,
                      uint32_t count,
                      int bits) {
    if (count == 0) return 0;

    uint8_t *d_results;
    uint32_t *d_primes;

    int limbs = (bits <= 320) ? 10 : 12;  /* 352-bit padded to 384 = 12 limbs */
    size_t primes_size = count * limbs * sizeof(uint32_t);
    size_t results_size = count * sizeof(uint8_t);

    cudaMalloc(&d_results, results_size);
    cudaMalloc(&d_primes, primes_size);

    /*
     * For 352-bit: input is 11 limbs but CGBN needs 12 (384-bit aligned).
     * We allocate padded buffers on host, zero-fill the 12th limb.
     */
    if (bits > 320 && limbs == 12) {
        uint32_t* padded = (uint32_t*)calloc(count * 12, sizeof(uint32_t));
        for (uint32_t i = 0; i < count; i++) {
            memcpy(&padded[i * 12], &h_primes[i * 11], 11 * sizeof(uint32_t));
            padded[i * 12 + 11] = 0;  /* Zero-fill padding limb */
        }
        cudaMemcpy(d_primes, padded, primes_size, cudaMemcpyHostToDevice);
        free(padded);
    } else {
        cudaMemcpy(d_primes, h_primes, primes_size, cudaMemcpyHostToDevice);
    }

    if (bits <= 320) {
        /* TPI=8: need 8 threads per instance. Block size should be multiple of 32 (warp). */
        int threads_needed = count * CGBN_TPI_320;
        int block_size = 128;  /* 128/8 = 16 instances per block */
        int blocks = (threads_needed + block_size - 1) / block_size;
        cgbn_fermat_kernel_320<<<blocks, block_size>>>(d_results, d_primes, count);
    } else {
        int threads_needed = count * CGBN_TPI_352;
        int block_size = 128;  /* 128/16 = 8 instances per block */
        int blocks = (threads_needed + block_size - 1) / block_size;
        cgbn_fermat_kernel_352<<<blocks, block_size>>>(d_results, d_primes, count);
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
