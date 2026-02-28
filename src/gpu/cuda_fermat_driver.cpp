// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * CUDA Fermat Primality Test — Driver API Implementation
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Uses the CUDA Driver API (nvcuda.dll / libcuda.so) loaded dynamically
 * at runtime. No CUDA Toolkit or nvcc needed at build time.
 *
 * PTX kernels are embedded as string constants and JIT-compiled by the
 * driver to the user's specific GPU architecture at initialization.
 * This is the same approach used by OpenCL (source → JIT compile),
 * and means a single binary works on all NVIDIA GPUs from Maxwell
 * (GTX 750 Ti, 2014) through current Ada Lovelace (RTX 4090).
 *
 * Architecture:
 *   fermat.cu → nvcc -ptx (dev machine) → fermat_ptx_source.h (committed)
 *   Build: cuda_fermat_driver.cpp compiled as plain C++ (no nvcc)
 *   Runtime: cuda_loader loads nvcuda.dll → cuModuleLoadData(ptx) → JIT
 */

#include "cuda_fermat.h"
#include "cuda_loader.h"
#include "cgbn_fermat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Embedded PTX source (generated from fermat.cu by nvcc -ptx -arch=compute_75) */
static const char* ptx_source =
#include "fermat_ptx_source.h"
;

/* Embedded CGBN PTX source (generated from cgbn_fermat.cu by nvcc -ptx -arch=compute_75) */
static const char* cgbn_ptx_source =
#include "cgbn_fermat_ptx_source.h"
;

/* Thread block size — must match the BLOCK_SIZE in fermat.cu */
#define BLOCK_SIZE 64

/* CGBN kernel block size — 128 threads (multiple instances per block for small TPI) */
#define CGBN_BLOCK_SIZE 128

/* Global CUDA Driver API state — base kernels (320/352-bit) */
static CUdevice   g_device = -1;
static CUcontext  g_context = nullptr;
static CUmodule   g_module = nullptr;
static CUfunction g_kernel_320 = nullptr;
static CUfunction g_kernel_352 = nullptr;
static CUfunction g_kernel_selftest = nullptr;
static int        g_initialized = 0;
static char       g_device_name[256] = {0};
static size_t     g_device_memory = 0;

/* CGBN module state — post-fork kernels (arbitrary bit width) */
static CUmodule   g_cgbn_module = nullptr;
static int        g_cgbn_initialized = 0;

/* CGBN tier → kernel mapping */
struct CgbnTierInfo {
    int bits;
    int tpi;
    const char* kernel_name;
    CUfunction kernel;
};

static CgbnTierInfo g_cgbn_tiers[] = {
    {   320,  8, "cgbn_fermat_kernel_320",   nullptr},
    {   384, 16, "cgbn_fermat_kernel_384",   nullptr},
    {   512, 16, "cgbn_fermat_kernel_512",   nullptr},
    {  1024, 32, "cgbn_fermat_kernel_1024",  nullptr},
    {  1280, 32, "cgbn_fermat_kernel_1280",  nullptr},
    {  2048, 32, "cgbn_fermat_kernel_2048",  nullptr},
    {  4096, 32, "cgbn_fermat_kernel_4096",  nullptr},
    {  8192, 32, "cgbn_fermat_kernel_8192",  nullptr},
    {  8448, 32, "cgbn_fermat_kernel_8448",  nullptr},
    { 12288, 32, "cgbn_fermat_kernel_12288", nullptr},
    { 16384, 32, "cgbn_fermat_kernel_16384", nullptr},
    { 16640, 32, "cgbn_fermat_kernel_16640", nullptr},
};
static const int g_cgbn_num_tiers = sizeof(g_cgbn_tiers) / sizeof(g_cgbn_tiers[0]);

int cuda_fermat_init(int device_id) {
    CUresult res;

    if (g_initialized) return 0;

    /* Ensure CUDA Driver API is dynamically loaded */
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) {
            return -2;  /* CUDA driver not available */
        }
    }

    /* Get device count */
    int device_count = 0;
    res = cu_cuDeviceGetCount(&device_count);
    if (res != CUDA_SUCCESS || device_count == 0) {
        return -2;
    }

    /* Clamp device_id */
    if (device_id >= device_count) {
        device_id = 0;
    }

    /* Get device handle */
    res = cu_cuDeviceGet(&g_device, device_id);
    if (res != CUDA_SUCCESS) {
        return -1;
    }

    /* Query device info */
    cu_cuDeviceGetName(g_device_name, sizeof(g_device_name), g_device);
    cu_cuDeviceTotalMem(&g_device_memory, g_device);

    /* Check compute capability — PTX compiled for compute_75 (Turing+).
     * CUDA 13.1 dropped all architectures below sm_75, so the JIT compiler
     * cannot target GPUs older than Turing (RTX 2000 / GTX 1600 series). */
    int cc_major = 0, cc_minor = 0;
    cu_cuDeviceGetAttribute(&cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, g_device);
    cu_cuDeviceGetAttribute(&cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, g_device);
    if (cc_major < 7 || (cc_major == 7 && cc_minor < 5)) {
        fprintf(stderr, "CUDA: GPU compute capability %d.%d too old (need 7.5+ / Turing)\n",
                cc_major, cc_minor);
        return -2;
    }

    /* Create CUDA context */
    res = cu_cuCtxCreate(&g_context, 0, g_device);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to create context (error %d)\n", res);
        return -1;
    }

    /* JIT-compile PTX to native GPU code */
    char jit_error_log[4096] = {0};
    char jit_info_log[4096] = {0};

    CUjit_option jit_options[] = {
        CU_JIT_ERROR_LOG_BUFFER,
        CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
        CU_JIT_INFO_LOG_BUFFER,
        CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
    };
    void* jit_option_values[] = {
        (void*)jit_error_log,
        (void*)(size_t)sizeof(jit_error_log),
        (void*)jit_info_log,
        (void*)(size_t)sizeof(jit_info_log),
    };

    res = cu_cuModuleLoadDataEx(&g_module, ptx_source, 4, jit_options, jit_option_values);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to JIT-compile PTX (error %d)\n", res);
        if (jit_error_log[0]) {
            fprintf(stderr, "CUDA JIT error:\n%s\n", jit_error_log);
        }
        cu_cuCtxDestroy(g_context);
        g_context = nullptr;
        return -1;
    }

    /* Get kernel function handles */
    res = cu_cuModuleGetFunction(&g_kernel_320, g_module, "fermat_kernel_320");
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to find kernel 'fermat_kernel_320' (error %d)\n", res);
        cu_cuModuleUnload(g_module);
        cu_cuCtxDestroy(g_context);
        g_module = nullptr;
        g_context = nullptr;
        return -1;
    }

    res = cu_cuModuleGetFunction(&g_kernel_352, g_module, "fermat_kernel_352");
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to find kernel 'fermat_kernel_352' (error %d)\n", res);
        cu_cuModuleUnload(g_module);
        cu_cuCtxDestroy(g_context);
        g_module = nullptr;
        g_context = nullptr;
        return -1;
    }

    /* Load self-test kernel (optional — kept for standalone test tools) */
    res = cu_cuModuleGetFunction(&g_kernel_selftest, g_module, "fermat_selftest");
    if (res != CUDA_SUCCESS) {
        g_kernel_selftest = nullptr;  /* Not fatal — selftest uses batch kernel */
    }

    g_initialized = 1;

    /* Initialize CGBN module for post-fork large-number kernels.
     * Non-fatal if it fails — pre-fork mining still works without it. */
    if (cgbn_fermat_init() == 0) {
        fprintf(stderr, "CUDA: CGBN module loaded (%d tier kernels, up to %d-bit)\n",
                g_cgbn_num_tiers, g_cgbn_tiers[g_cgbn_num_tiers - 1].bits);
    } else {
        fprintf(stderr, "CUDA: CGBN module not available (post-fork kernels disabled)\n");
    }

    /* Self-test is NOT run here — callers (gpu_worker, gpu_worker_func) run
     * cuda_fermat_selftest() explicitly and handle failure with OpenCL fallback.
     * Running it here would be redundant and would lack fallback logic. */

    return 0;
}

void cuda_fermat_cleanup(void) {
    if (!g_initialized) return;

    cgbn_fermat_cleanup();

    if (g_module) {
        cu_cuModuleUnload(g_module);
        g_module = nullptr;
    }
    if (g_context) {
        cu_cuCtxDestroy(g_context);
        g_context = nullptr;
    }

    g_kernel_320 = nullptr;
    g_kernel_352 = nullptr;
    g_kernel_selftest = nullptr;
    g_initialized = 0;
}

int cuda_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                      uint32_t count, int bits) {
    if (!g_initialized) return -1;
    if (count == 0) return 0;

    /* Post-fork large numbers: route through CGBN (nvcc-compiled kernels).
     * The PTX path only has 320/352-bit Montgomery math. For bits > 352,
     * CGBN provides warp-cooperative Montgomery at arbitrary widths. */
    if (bits > 352) {
        if (cgbn_is_available()) {
            return cgbn_fermat_batch(h_results, h_primes, count, bits);
        }
        fprintf(stderr, "CUDA: bits=%d exceeds PTX kernel range (max 352) "
                "and CGBN not available\n", bits);
        return -1;
    }

    CUresult res;
    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = (size_t)count * limbs * sizeof(uint32_t);
    size_t results_size = (size_t)count * sizeof(uint8_t);

    /* Allocate device memory */
    CUdeviceptr d_primes = 0, d_results = 0;

    res = cu_cuMemAlloc(&d_primes, primes_size);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: cuMemAlloc primes failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuMemAlloc results failed (error %d)\n", res);
        return -1;
    }

    /* Copy primes to device */
    res = cu_cuMemcpyHtoD(d_primes, h_primes, primes_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuMemcpyHtoD failed (error %d)\n", res);
        return -1;
    }

    /* Select kernel (pre-fork sizes only) */
    CUfunction kernel = (bits <= 320) ? g_kernel_320 : g_kernel_352;

    /* Launch kernel */
    unsigned int grid_x = (count + BLOCK_SIZE - 1) / BLOCK_SIZE;

    void* kernel_params[] = {
        &d_results,
        &d_primes,
        &count
    };

    res = cu_cuLaunchKernel(kernel,
                            grid_x, 1, 1,          /* grid dimensions */
                            BLOCK_SIZE, 1, 1,       /* block dimensions */
                            0,                      /* shared memory */
                            nullptr,                /* stream (default) */
                            kernel_params,          /* kernel parameters */
                            nullptr);               /* extra */
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuLaunchKernel failed (error %d)\n", res);
        return -1;
    }

    /* Synchronize and copy results back */
    res = cu_cuCtxSynchronize();
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuCtxSynchronize failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuMemcpyDtoH(h_results, d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuMemcpyDtoH failed (error %d)\n", res);
        return -1;
    }

    /* Free device memory */
    cu_cuMemFree(d_results);
    cu_cuMemFree(d_primes);

    return 0;
}

int cuda_fermat_selftest(void) {
    if (!g_initialized || !g_kernel_320) return -1;

    CUresult res;

    /* Self-test uses the ACTUAL batch kernel (fermat_kernel_320) with test data
     * uploaded to device memory.  This tests the exact code path used during
     * mining, avoiding potential nvcc constant-folding optimizer bugs that can
     * occur in the dedicated fermat_selftest kernel where all inputs are
     * compile-time constants.
     *
     * Test vectors (each 10 × uint32_t, little-endian):
     *   [0] secp256k1 prime  — known prime, expect Fermat pass (1)
     *   [1] Mersenne M127    — known prime, expect Fermat pass (1)
     *   [2] Composite 15     — known composite, expect Fermat fail (0)
     */
    static const uint32_t test_primes[3 * 10] = {
        /* secp256k1: 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F */
        0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0x00000000u, 0x00000000u,
        /* Mersenne M127 = 2^127 - 1 */
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u,
        /* Composite 15 */
        0x0000000Fu, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u,
    };

    const uint32_t count = 3;
    const size_t primes_size = count * 10 * sizeof(uint32_t);
    const size_t results_size = count * sizeof(uint8_t);

    /* Allocate device memory */
    CUdeviceptr d_primes = 0, d_results = 0;

    res = cu_cuMemAlloc(&d_primes, primes_size);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA selftest: cuMemAlloc primes failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA selftest: cuMemAlloc results failed (error %d)\n", res);
        return -1;
    }

    /* Upload test data and zero results */
    cu_cuMemcpyHtoD(d_primes, test_primes, primes_size);
    uint8_t zeros[3] = {0, 0, 0};
    cu_cuMemcpyHtoD(d_results, zeros, results_size);

    /* Launch fermat_kernel_320: 1 block of 64 threads, 3 candidates.
     * Only threads 0-2 will do work (id < count guard in kernel). */
    void* params[] = { &d_results, &d_primes, (void*)&count };
    res = cu_cuLaunchKernel(g_kernel_320,
                            1, 1, 1,            /* grid: 1 block */
                            BLOCK_SIZE, 1, 1,   /* block: 64 threads */
                            0, nullptr, params, nullptr);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA selftest: launch failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuCtxSynchronize();
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA selftest: sync failed (error %d)\n", res);
        return -1;
    }

    uint8_t h_results[3] = {0};
    res = cu_cuMemcpyDtoH(h_results, d_results, results_size);
    cu_cuMemFree(d_results);
    cu_cuMemFree(d_primes);

    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA selftest: readback failed (error %d)\n", res);
        return -1;
    }

    fprintf(stderr, "CUDA selftest results (batch kernel):\n");
    fprintf(stderr, "  secp256k1 prime (expect 1): %d\n", h_results[0]);
    fprintf(stderr, "  Mersenne M127   (expect 1): %d\n", h_results[1]);
    fprintf(stderr, "  Composite 15    (expect 0): %d\n", h_results[2]);

    int pass = 1;
    if (h_results[0] != 1) { fprintf(stderr, "  FAIL: secp256k1 prime returned %d\n", h_results[0]); pass = 0; }
    if (h_results[1] != 1) { fprintf(stderr, "  FAIL: Mersenne M127 returned %d\n", h_results[1]); pass = 0; }
    if (h_results[2] != 0) { fprintf(stderr, "  FAIL: Composite 15 returned %d\n", h_results[2]); pass = 0; }

    if (pass) {
        fprintf(stderr, "CUDA selftest: PASSED — batch kernel Montgomery math verified\n");
        return 0;
    } else {
        fprintf(stderr, "CUDA selftest: FAILED\n");
        return -1;
    }
}

/* =========================================================================
 * CGBN: warp-cooperative Fermat kernels for post-fork large numbers.
 * Loaded as a second PTX module from the same CUDA context.
 * ========================================================================= */

int cgbn_fermat_init(void) {
    if (g_cgbn_initialized) return 0;
    if (!g_initialized || !g_context) return -1;

    CUresult res;
    char jit_error_log[4096] = {0};
    CUjit_option jit_options[] = {
        CU_JIT_ERROR_LOG_BUFFER,
        CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
    };
    void* jit_option_values[] = {
        (void*)jit_error_log,
        (void*)(size_t)sizeof(jit_error_log),
    };

    res = cu_cuModuleLoadDataEx(&g_cgbn_module, cgbn_ptx_source, 2, jit_options, jit_option_values);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CGBN: Failed to JIT-compile PTX (error %d)\n", res);
        if (jit_error_log[0]) {
            fprintf(stderr, "CGBN JIT error:\n%s\n", jit_error_log);
        }
        return -1;
    }

    /* Resolve all tier kernels */
    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        res = cu_cuModuleGetFunction(&g_cgbn_tiers[i].kernel, g_cgbn_module,
                                      g_cgbn_tiers[i].kernel_name);
        if (res != CUDA_SUCCESS) {
            fprintf(stderr, "CGBN: Failed to find kernel '%s' (error %d)\n",
                    g_cgbn_tiers[i].kernel_name, res);
            cu_cuModuleUnload(g_cgbn_module);
            g_cgbn_module = nullptr;
            return -1;
        }
    }

    g_cgbn_initialized = 1;
    return 0;
}

int cgbn_is_available(void) {
    return g_cgbn_initialized;
}

void cgbn_fermat_cleanup(void) {
    if (!g_cgbn_initialized) return;
    if (g_cgbn_module) {
        cu_cuModuleUnload(g_cgbn_module);
        g_cgbn_module = nullptr;
    }
    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        g_cgbn_tiers[i].kernel = nullptr;
    }
    g_cgbn_initialized = 0;
}

int cgbn_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                      uint32_t count, int bits) {
    if (!g_cgbn_initialized) return -1;
    if (count == 0) return 0;

    /* Select CGBN tier: round UP to nearest supported bit width */
    CgbnTierInfo* tier = nullptr;
    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        if (g_cgbn_tiers[i].bits >= bits) {
            tier = &g_cgbn_tiers[i];
            break;
        }
    }
    if (!tier) {
        tier = &g_cgbn_tiers[g_cgbn_num_tiers - 1];  /* Largest tier */
    }

    int input_limbs = (bits + 31) / 32;
    int tier_limbs = tier->bits / 32;
    size_t results_size = (size_t)count * sizeof(uint8_t);

    CUresult res;
    CUdeviceptr d_primes = 0, d_results = 0;

    /* Allocate device memory at tier's limb count (zero-padded) */
    size_t primes_size = (size_t)count * tier_limbs * sizeof(uint32_t);
    res = cu_cuMemAlloc(&d_primes, primes_size);
    if (res != CUDA_SUCCESS) return -1;

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        return -1;
    }

    /* Upload with zero-padding if input limbs < tier limbs */
    if (input_limbs < tier_limbs) {
        uint32_t* padded = (uint32_t*)calloc((size_t)count * tier_limbs, sizeof(uint32_t));
        if (!padded) {
            cu_cuMemFree(d_results);
            cu_cuMemFree(d_primes);
            return -1;
        }
        for (uint32_t i = 0; i < count; i++) {
            memcpy(&padded[i * tier_limbs],
                   &h_primes[i * input_limbs],
                   input_limbs * sizeof(uint32_t));
        }
        cu_cuMemcpyHtoD(d_primes, padded, primes_size);
        free(padded);
    } else {
        cu_cuMemcpyHtoD(d_primes, h_primes, primes_size);
    }

    /* Launch: threads_needed = count * TPI */
    unsigned int threads_needed = count * tier->tpi;
    unsigned int grid_x = (threads_needed + CGBN_BLOCK_SIZE - 1) / CGBN_BLOCK_SIZE;

    void* params[] = { &d_results, &d_primes, &count };
    res = cu_cuLaunchKernel(tier->kernel,
                             grid_x, 1, 1,
                             CGBN_BLOCK_SIZE, 1, 1,
                             0, nullptr, params, nullptr);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CGBN: launch failed for %d-bit tier (error %d)\n", tier->bits, res);
        return -1;
    }

    res = cu_cuCtxSynchronize();
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        return -1;
    }

    cu_cuMemcpyDtoH(h_results, d_results, results_size);
    cu_cuMemFree(d_results);
    cu_cuMemFree(d_primes);

    return 0;
}

int cuda_get_device_count(void) {
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return 0;
    }

    int count = 0;
    if (cu_cuDeviceGetCount(&count) != CUDA_SUCCESS) {
        return 0;
    }
    return count;
}

const char* cuda_get_device_name(int device_id) {
    static char name[256];
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return "N/A";
    }

    CUdevice dev;
    if (cu_cuDeviceGet(&dev, device_id) != CUDA_SUCCESS) {
        return "Unknown";
    }

    if (cu_cuDeviceGetName(name, sizeof(name), dev) != CUDA_SUCCESS) {
        return "Unknown";
    }

    return name;
}

size_t cuda_get_device_memory(int device_id) {
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return 0;
    }

    CUdevice dev;
    size_t memory = 0;
    if (cu_cuDeviceGet(&dev, device_id) != CUDA_SUCCESS) {
        return 0;
    }

    if (cu_cuDeviceTotalMem(&memory, dev) != CUDA_SUCCESS) {
        return 0;
    }

    return memory;
}

int cuda_get_sm_count(int device_id) {
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return 0;
    }

    CUdevice dev;
    int sm_count = 0;
    if (cu_cuDeviceGet(&dev, device_id) != CUDA_SUCCESS) {
        return 0;
    }

    if (cu_cuDeviceGetAttribute(&sm_count, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev) != CUDA_SUCCESS) {
        return 0;
    }

    return sm_count;
}

int cuda_is_available(void) {
    return cuda_get_device_count() > 0 ? 1 : 0;
}
