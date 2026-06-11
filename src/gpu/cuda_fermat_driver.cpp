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

#include <stdarg.h>
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

/* CGBN tier → kernel mapping.
 * validated: -1 = not probed, 1 = probe passed, 0 = probe failed. */
struct CgbnTierInfo {
    int bits;
    int tpi;
    const char* kernel_name;
    CUfunction kernel;
    int validated;
};

static CgbnTierInfo g_cgbn_tiers[] = {
    {   320,  8, "cgbn_fermat_kernel_320",   nullptr, -1},
    {   384, 16, "cgbn_fermat_kernel_384",   nullptr, -1},
    {   512, 16, "cgbn_fermat_kernel_512",   nullptr, -1},
    {  1024, 32, "cgbn_fermat_kernel_1024",  nullptr, -1},
    {  1280, 32, "cgbn_fermat_kernel_1280",  nullptr, -1},
    {  2048, 32, "cgbn_fermat_kernel_2048",  nullptr, -1},
    {  4096, 32, "cgbn_fermat_kernel_4096",  nullptr, -1},
    {  8192, 32, "cgbn_fermat_kernel_8192",  nullptr, -1},
    {  8448, 32, "cgbn_fermat_kernel_8448",  nullptr, -1},
    { 12288, 32, "cgbn_fermat_kernel_12288", nullptr, -1},
    { 16384, 32, "cgbn_fermat_kernel_16384", nullptr, -1},
    { 16640, 32, "cgbn_fermat_kernel_16640", nullptr, -1},
    { 16672, 32, "cgbn_fermat_kernel_16672", nullptr, -1},
};
static const int g_cgbn_num_tiers = sizeof(g_cgbn_tiers) / sizeof(g_cgbn_tiers[0]);

/* A tier must leave >=32 bits of slack above the input width so the
 * modulus can never fill its environment (see cgbn_fermat.cu). */
#define CGBN_TIER_SLACK_BITS 32

/* stderr is invisible in a Windows GUI app; diagnostics go through a
 * caller-installed logger (the node wires it to LogPrintf), with stderr
 * as the fallback. */
static cuda_fermat_log_fn g_log_fn = nullptr;

void cuda_fermat_set_logger(cuda_fermat_log_fn fn) {
    g_log_fn = fn;
}

static void cuda_logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log_fn) {
        g_log_fn(buf);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
}

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
        cuda_logf("CUDA: GPU compute capability %d.%d too old (need 7.5+ / Turing)",
                cc_major, cc_minor);
        return -2;
    }

    /* Create CUDA context */
    res = cu_cuCtxCreate(&g_context, 0, g_device);
    if (res != CUDA_SUCCESS) {
        cuda_logf("CUDA: Failed to create context (error %d)", res);
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
        cuda_logf("CUDA: Failed to JIT-compile PTX (error %d)", res);
        if (jit_error_log[0]) {
            cuda_logf("CUDA JIT error:\n%s", jit_error_log);
        }
        cu_cuCtxDestroy(g_context);
        g_context = nullptr;
        return -1;
    }

    /* Get kernel function handles */
    res = cu_cuModuleGetFunction(&g_kernel_320, g_module, "fermat_kernel_320");
    if (res != CUDA_SUCCESS) {
        cuda_logf("CUDA: Failed to find kernel 'fermat_kernel_320' (error %d)", res);
        cu_cuModuleUnload(g_module);
        cu_cuCtxDestroy(g_context);
        g_module = nullptr;
        g_context = nullptr;
        return -1;
    }

    res = cu_cuModuleGetFunction(&g_kernel_352, g_module, "fermat_kernel_352");
    if (res != CUDA_SUCCESS) {
        cuda_logf("CUDA: Failed to find kernel 'fermat_kernel_352' (error %d)", res);
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
        cuda_logf("CUDA: CGBN module loaded (%d tier kernels, up to %d-bit)",
                g_cgbn_num_tiers, g_cgbn_tiers[g_cgbn_num_tiers - 1].bits);
    } else {
        cuda_logf("CUDA: CGBN module not available (post-fork kernels disabled)");
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

/* Per-async-batch context that owns its device buffers. Stored on the heap
 * and freed by a cuLaunchHostFunc callback after the stream finishes the
 * DtoH copy. Without cuLaunchHostFunc (CUDA <10), the async path falls back
 * to a sync allocate/copy/launch/sync/copy/free cycle. */
struct AsyncBatchCtx {
    CUdeviceptr d_primes;
    CUdeviceptr d_results;
};

static void async_batch_complete_cb(void* userdata) {
    /* This runs on a CUDA-internal thread. Keep it brief and CUDA-free
     * except for the explicit frees. cuMemFree is safe to call from
     * a host callback. */
    AsyncBatchCtx* ctx = (AsyncBatchCtx*)userdata;
    if (cu_cuMemFree) {
        if (ctx->d_results) cu_cuMemFree(ctx->d_results);
        if (ctx->d_primes)  cu_cuMemFree(ctx->d_primes);
    }
    delete ctx;
}

int cuda_fermat_batch_async(uint8_t *h_results, const uint32_t *h_primes,
                             uint32_t count, int bits, void* stream) {
    if (!g_initialized) return -1;
    if (count == 0) return 0;

    /* Post-fork large numbers: route through CGBN's stream-aware async batch.
     * Both paths use the same stream/callback contract so the GPU worker
     * can pipeline them identically. */
    if (bits > 352) {
        if (cgbn_is_available()) {
            return cgbn_fermat_batch_async(h_results, h_primes, count, bits, stream);
        }
        cuda_logf("CUDA: bits=%d exceeds PTX kernel range (max 352) "
                "and CGBN not available\n", bits);
        return -1;
    }

    CUresult res;
    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = (size_t)count * limbs * sizeof(uint32_t);
    size_t results_size = (size_t)count * sizeof(uint8_t);

    /* Allocate device memory. Pool optimization in v2511.9 axis (b) replaces
     * this with a per-stream pre-allocated arena. */
    CUdeviceptr d_primes = 0, d_results = 0;

    res = cu_cuMemAlloc(&d_primes, primes_size);
    if (res != CUDA_SUCCESS) {
        cuda_logf("CUDA: cuMemAlloc primes failed (error %d)", res);
        return -1;
    }

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        cuda_logf("CUDA: cuMemAlloc results failed (error %d)", res);
        return -1;
    }

    CUstream s = (CUstream)stream;

    /* H2D async (overlaps with prior stream's kernel under Hyper-Q) */
    if (s && cu_cuMemcpyHtoDAsync) {
        res = cu_cuMemcpyHtoDAsync(d_primes, h_primes, primes_size, s);
    } else {
        res = cu_cuMemcpyHtoD(d_primes, h_primes, primes_size);
    }
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        cuda_logf("CUDA: HtoD failed (error %d)", res);
        return -1;
    }

    CUfunction kernel = (bits <= 320) ? g_kernel_320 : g_kernel_352;
    unsigned int grid_x = (count + BLOCK_SIZE - 1) / BLOCK_SIZE;
    void* kernel_params[] = { &d_results, &d_primes, &count };

    res = cu_cuLaunchKernel(kernel,
                            grid_x, 1, 1,
                            BLOCK_SIZE, 1, 1,
                            0,
                            s,                      /* stream — null = default */
                            kernel_params,
                            nullptr);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        cuda_logf("CUDA: cuLaunchKernel failed (error %d)", res);
        return -1;
    }

    /* DtoH async — the caller MUST cuStreamSynchronize(s) before reading
     * h_results, or wait for the registered completion callback. */
    if (s && cu_cuMemcpyDtoHAsync) {
        res = cu_cuMemcpyDtoHAsync(h_results, d_results, results_size, s);
    } else {
        /* Either no stream was supplied or driver doesn't expose async.
         * Force synchronization before reading. */
        res = cu_cuCtxSynchronize();
        if (res != CUDA_SUCCESS) {
            cu_cuMemFree(d_results);
            cu_cuMemFree(d_primes);
            cuda_logf("CUDA: cuCtxSynchronize failed (error %d)", res);
            return -1;
        }
        res = cu_cuMemcpyDtoH(h_results, d_results, results_size);
    }
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        cuda_logf("CUDA: DtoH failed (error %d)", res);
        return -1;
    }

    /* If we used a real stream + have cuLaunchHostFunc, queue the free for
     * after the stream's DtoH completes. Otherwise free synchronously now. */
    if (s && cu_cuLaunchHostFunc) {
        AsyncBatchCtx* ctx = new AsyncBatchCtx{d_primes, d_results};
        res = cu_cuLaunchHostFunc(s, async_batch_complete_cb, ctx);
        if (res != CUDA_SUCCESS) {
            /* Callback registration failed — degrade to sync free */
            delete ctx;
            cu_cuStreamSynchronize(s);
            cu_cuMemFree(d_results);
            cu_cuMemFree(d_primes);
        }
    } else {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
    }
    return 0;
}

int cuda_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                      uint32_t count, int bits) {
    /* Sync wrapper: queue work on default null-stream and wait for it.
     * Same observable behavior as before the stream API was added. */
    int rc = cuda_fermat_batch_async(h_results, h_primes, count, bits, nullptr);
    if (rc != 0) return rc;
    /* When stream==nullptr, cuda_fermat_batch_async already did its own
     * synchronization via cu_cuCtxSynchronize() or sync memcpy. Nothing
     * more to do. */
    return 0;
}

/* === Stream lifecycle helpers === */
void* cuda_fermat_stream_create(void) {
    if (!g_initialized || !cu_cuStreamCreate) return nullptr;
    CUstream s = nullptr;
    /* Flag 0 = CU_STREAM_DEFAULT — synchronize with the legacy null stream
     * for ordering. Use 1 (CU_STREAM_NON_BLOCKING) if/when we want full
     * concurrency vs. validation kernels. For now stay default-ordered to
     * preserve correctness while sharing the context with the validation
     * GPU path. */
    if (cu_cuStreamCreate(&s, 0) != CUDA_SUCCESS) return nullptr;
    return (void*)s;
}

void cuda_fermat_stream_destroy(void* stream) {
    if (!stream || !cu_cuStreamDestroy) return;
    cu_cuStreamDestroy((CUstream)stream);
}

int cuda_fermat_stream_sync(void* stream) {
    if (!stream) {
        return cu_cuCtxSynchronize ? cu_cuCtxSynchronize() : -1;
    }
    if (!cu_cuStreamSynchronize) return -1;
    return cu_cuStreamSynchronize((CUstream)stream) == CUDA_SUCCESS ? 0 : -1;
}

int cuda_fermat_stream_idle(void* stream) {
    if (!stream || !cu_cuStreamQuery) return -1;
    CUresult r = cu_cuStreamQuery((CUstream)stream);
    if (r == CUDA_SUCCESS) return 1;
    /* CUDA_ERROR_NOT_READY = 600 on all known drivers; treat anything else
     * as an error. */
    return 0;
}

int cuda_fermat_stream_on_complete(void* stream, void (*cb)(void*), void* userdata) {
    if (!stream || !cb || !cu_cuLaunchHostFunc) return -1;
    return cu_cuLaunchHostFunc((CUstream)stream, cb, userdata) == CUDA_SUCCESS ? 0 : -1;
}

void* cuda_fermat_host_alloc(size_t bytes) {
    if (!g_initialized || !cu_cuMemHostAlloc) return nullptr;
    void* p = nullptr;
    /* Flag 0 = CU_MEMHOSTALLOC_DEFAULT — page-locked, portable across CUDA
     * contexts in the same process. Sufficient for async memcpy overlap. */
    if (cu_cuMemHostAlloc(&p, bytes, 0) != CUDA_SUCCESS) return nullptr;
    return p;
}

void cuda_fermat_host_free(void* p) {
    if (!p || !cu_cuMemFreeHost) return;
    cu_cuMemFreeHost(p);
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
        cuda_logf("CUDA selftest: cuMemAlloc primes failed (error %d)", res);
        return -1;
    }

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        cuda_logf("CUDA selftest: cuMemAlloc results failed (error %d)", res);
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
        cuda_logf("CUDA selftest: launch failed (error %d)", res);
        return -1;
    }

    res = cu_cuCtxSynchronize();
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        cuda_logf("CUDA selftest: sync failed (error %d)", res);
        return -1;
    }

    uint8_t h_results[3] = {0};
    res = cu_cuMemcpyDtoH(h_results, d_results, results_size);
    cu_cuMemFree(d_results);
    cu_cuMemFree(d_primes);

    if (res != CUDA_SUCCESS) {
        cuda_logf("CUDA selftest: readback failed (error %d)", res);
        return -1;
    }

    cuda_logf("CUDA selftest results (batch kernel):");
    cuda_logf("  secp256k1 prime (expect 1): %d", h_results[0]);
    cuda_logf("  Mersenne M127   (expect 1): %d", h_results[1]);
    cuda_logf("  Composite 15    (expect 0): %d", h_results[2]);

    int pass = 1;
    if (h_results[0] != 1) { cuda_logf("  FAIL: secp256k1 prime returned %d", h_results[0]); pass = 0; }
    if (h_results[1] != 1) { cuda_logf("  FAIL: Mersenne M127 returned %d", h_results[1]); pass = 0; }
    if (h_results[2] != 0) { cuda_logf("  FAIL: Composite 15 returned %d", h_results[2]); pass = 0; }

    if (pass) {
        cuda_logf("CUDA selftest: PASSED — batch kernel Montgomery math verified");
        return 0;
    } else {
        cuda_logf("CUDA selftest: FAILED");
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
        cuda_logf("CGBN: Failed to JIT-compile PTX (error %d)", res);
        if (jit_error_log[0]) {
            cuda_logf("CGBN JIT error:\n%s", jit_error_log);
        }
        return -1;
    }

    /* Resolve all tier kernels */
    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        res = cu_cuModuleGetFunction(&g_cgbn_tiers[i].kernel, g_cgbn_module,
                                      g_cgbn_tiers[i].kernel_name);
        if (res != CUDA_SUCCESS) {
            cuda_logf("CGBN: Failed to find kernel '%s' (error %d)",
                    g_cgbn_tiers[i].kernel_name, res);
            cu_cuModuleUnload(g_cgbn_module);
            g_cgbn_module = nullptr;
            return -1;
        }
    }

    g_cgbn_initialized = 1;

    /* Known-answer probe of every tier (~30 single-candidate tests, <3s;
     * JIT already paid). A failed tier is excluded from mining. Probe
     * failures do not fail init — validated tiers remain usable. */
    cgbn_fermat_probe_tiers();
    return 0;
}

/* =========================================================================
 * Known-answer tier probe
 *
 * Vectors live in cgbn_kat_vectors.h: a BPSW-verified probable prime and
 * a composite at the maximum input width of every tier. A wrong verdict
 * for either marks the tier failed.
 * ========================================================================= */

#include "cgbn_kat_vectors.h"

/* Parse a big-endian hex string ("0x..." or bare) into little-endian
 * 32-bit limbs. Returns the number of limbs written, or -1 on error. */
static int kat_hex_to_limbs(const char* hex, uint32_t* limbs, int max_limbs) {
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    size_t n = strlen(hex);
    if (n == 0) return -1;
    memset(limbs, 0, (size_t)max_limbs * sizeof(uint32_t));
    int top_limb = 0;
    for (size_t i = 0; i < n; i++) {
        char c = hex[n - 1 - i];
        uint32_t v;
        if (c >= '0' && c <= '9')      v = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint32_t)(c - 'A' + 10);
        else return -1;
        int limb = (int)(i / 8);
        if (limb >= max_limbs) return -1;
        limbs[limb] |= v << ((i % 8) * 4);
        top_limb = limb;
    }
    return top_limb + 1;
}

static CgbnTierInfo* cgbn_tier_for_bits(int bits) {
    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        if (g_cgbn_tiers[i].bits >= bits + CGBN_TIER_SLACK_BITS) {
            return &g_cgbn_tiers[i];
        }
    }
    return nullptr;
}

int cgbn_fermat_probe_tiers(void) {
    if (!g_cgbn_initialized) return -1;

    enum { KAT_MAX_LIMBS = 16672 / 32 };
    static uint32_t limbs[KAT_MAX_LIMBS];  /* single-threaded init path */
    int failed_tiers = 0;

    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        int tested = 0, wrong = 0, errors = 0;

        for (int v = 0; v < CGBN_KAT_NUM_VECTORS; v++) {
            const CgbnKatVector* vec = &CGBN_KAT_VECTORS[v];
            CgbnTierInfo* tier = cgbn_tier_for_bits(vec->bits);
            if (tier != &g_cgbn_tiers[i]) continue;  /* vector probes another tier */

            int input_limbs = (vec->bits + 31) / 32;
            if (kat_hex_to_limbs(vec->hex, limbs, KAT_MAX_LIMBS) < 0 ||
                input_limbs > KAT_MAX_LIMBS) {
                cuda_logf("CGBN probe: bad KAT vector (%d bits) — generator bug", vec->bits);
                errors++;
                continue;
            }

            uint8_t result = 0xAA;  /* sentinel: detects never-written results */
            int rc = cgbn_fermat_batch(&result, limbs, 1, vec->bits);
            tested++;
            if (rc != 0 || result == 0xAA) {
                cuda_logf("CGBN probe: tier %d batch error rc=%d result=0x%02X (%d-bit vector)",
                          g_cgbn_tiers[i].bits, rc, result, vec->bits);
                errors++;
            } else if ((int)result != vec->expected) {
                cuda_logf("CGBN probe: tier %d WRONG VERDICT — %d-bit %s reported %s",
                          g_cgbn_tiers[i].bits, vec->bits,
                          vec->expected ? "probable prime" : "composite",
                          result ? "prime" : "composite");
                wrong++;
            }
        }

        if (tested == 0) {
            /* No vector maps here (tier covered transitively by a wider
             * vector set) — leave as unprobed rather than claiming validity. */
            continue;
        }
        g_cgbn_tiers[i].validated = (wrong == 0 && errors == 0) ? 1 : 0;
        if (g_cgbn_tiers[i].validated) {
            cuda_logf("CGBN probe: tier %d OK (%d vectors)", g_cgbn_tiers[i].bits, tested);
        } else {
            cuda_logf("CGBN probe: tier %d FAILED (%d wrong, %d errors of %d) — "
                      "mining at shifts mapping to this tier is DISABLED",
                      g_cgbn_tiers[i].bits, wrong, errors, tested);
            failed_tiers++;
        }
    }
    return failed_tiers;
}

int cgbn_fermat_bits_validated(int bits) {
    if (!g_cgbn_initialized) return 0;
    CgbnTierInfo* tier = cgbn_tier_for_bits(bits);
    if (!tier) return 0;
    return tier->validated == 1;
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

/* Async CGBN batch — same pattern as cuda_fermat_batch_async() but launches
 * the warp-cooperative CGBN kernel selected by the bit tier. Returns
 * immediately after queueing work + DtoH on the supplied stream. Caller
 * must cu_cuStreamSynchronize(stream) before reading h_results. */
int cgbn_fermat_batch_async(uint8_t *h_results, const uint32_t *h_primes,
                             uint32_t count, int bits, void* stream) {
    if (!g_cgbn_initialized) return -1;
    if (count == 0) return 0;

    /* Round UP to the nearest tier with CGBN_TIER_SLACK_BITS of headroom.
     * No fitting tier is a hard error, not a largest-tier fallback:
     * consensus caps inputs at 16640 bits, which the 16672 tier covers. */
    CgbnTierInfo* tier = nullptr;
    for (int i = 0; i < g_cgbn_num_tiers; i++) {
        if (g_cgbn_tiers[i].bits >= bits + CGBN_TIER_SLACK_BITS) {
            tier = &g_cgbn_tiers[i];
            break;
        }
    }
    if (!tier) {
        cuda_logf("CGBN: no tier with %d-bit headroom for %d-bit input (max usable %d) — rejecting batch",
                  CGBN_TIER_SLACK_BITS, bits,
                  g_cgbn_tiers[g_cgbn_num_tiers - 1].bits - CGBN_TIER_SLACK_BITS);
        return -2;
    }

    int input_limbs = (bits + 31) / 32;
    int tier_limbs = tier->bits / 32;
    size_t results_size = (size_t)count * sizeof(uint8_t);
    size_t primes_size = (size_t)count * tier_limbs * sizeof(uint32_t);

    CUresult res;
    CUdeviceptr d_primes = 0, d_results = 0;

    res = cu_cuMemAlloc(&d_primes, primes_size);
    if (res != CUDA_SUCCESS) return -1;

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        return -1;
    }

    CUstream s = (CUstream)stream;

    /* Zero-pad if input limb count is below tier limbs. The padded buffer
     * is short-lived; we copy synchronously then free it before launching
     * the kernel so we don't have to track its lifetime across the stream.
     * This is fine for correctness since cu_cuMemcpyHtoD blocks the host
     * until the copy is staged into the driver's pinned bounce buffer;
     * the device-side enqueue is still asynchronous from the host's view. */
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
        /* Use sync HtoD for the padded path — padded is on the stack and
         * goes out of scope immediately. Async would race. */
        cu_cuMemcpyHtoD(d_primes, padded, primes_size);
        free(padded);
    } else if (s && cu_cuMemcpyHtoDAsync) {
        res = cu_cuMemcpyHtoDAsync(d_primes, h_primes, primes_size, s);
        if (res != CUDA_SUCCESS) {
            cu_cuMemFree(d_results);
            cu_cuMemFree(d_primes);
            return -1;
        }
    } else {
        cu_cuMemcpyHtoD(d_primes, h_primes, primes_size);
    }

    unsigned int threads_needed = count * tier->tpi;
    unsigned int grid_x = (threads_needed + CGBN_BLOCK_SIZE - 1) / CGBN_BLOCK_SIZE;

    void* params[] = { &d_results, &d_primes, &count };
    res = cu_cuLaunchKernel(tier->kernel,
                             grid_x, 1, 1,
                             CGBN_BLOCK_SIZE, 1, 1,
                             0, s, params, nullptr);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        cuda_logf("CGBN: launch failed for %d-bit tier (error %d)", tier->bits, res);
        return -1;
    }

    if (s && cu_cuMemcpyDtoHAsync) {
        res = cu_cuMemcpyDtoHAsync(h_results, d_results, results_size, s);
    } else {
        res = cu_cuCtxSynchronize();
        if (res != CUDA_SUCCESS) {
            cu_cuMemFree(d_results);
            cu_cuMemFree(d_primes);
            return -1;
        }
        res = cu_cuMemcpyDtoH(h_results, d_results, results_size);
    }
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        return -1;
    }

    /* Queue device-memory free for after DtoH completes (CUDA 10+), or
     * free synchronously now. */
    if (s && cu_cuLaunchHostFunc) {
        AsyncBatchCtx* ctx = new AsyncBatchCtx{d_primes, d_results};
        res = cu_cuLaunchHostFunc(s, async_batch_complete_cb, ctx);
        if (res != CUDA_SUCCESS) {
            delete ctx;
            cu_cuStreamSynchronize(s);
            cu_cuMemFree(d_results);
            cu_cuMemFree(d_primes);
        }
    } else {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
    }
    return 0;
}

int cgbn_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                      uint32_t count, int bits) {
    /* Sync wrapper: legacy null-stream behavior. cgbn_fermat_batch_async
     * with stream==nullptr falls back to sync HtoD/launch/sync/DtoH chain. */
    return cgbn_fermat_batch_async(h_results, h_primes, count, bits, nullptr);
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
