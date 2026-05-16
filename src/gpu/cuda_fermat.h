// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * CUDA Fermat Primality Test Interface
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Uses CUDA Driver API loaded dynamically at runtime — no CUDA Toolkit
 * needed at build time. Works on any system with NVIDIA GPU drivers.
 * PTX kernels are JIT-compiled to the user's specific GPU architecture.
 */

#ifndef FREYCOIN_GPU_CUDA_FERMAT_H
#define FREYCOIN_GPU_CUDA_FERMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize CUDA device for Fermat primality testing.
 * Loads CUDA Driver API, creates context, JIT-compiles PTX kernels.
 * @param device_id CUDA device index (0 for first GPU)
 * @return 0 on success, -1 on error, -2 if CUDA not available
 */
int cuda_fermat_init(int device_id);

/**
 * Cleanup CUDA resources (context, module, etc).
 */
void cuda_fermat_cleanup(void);

/**
 * Run batch Fermat primality test on GPU.
 * Tests if 2^(p-1) ≡ 1 (mod p) for each prime candidate.
 *
 * @param h_results Output array: 1 = probably prime, 0 = composite
 * @param h_primes  Input array of candidates (limb-packed format)
 * @param count     Number of candidates to test
 * @param bits      Bit size (320/352 via PTX, larger via CGBN fallback)
 * @return 0 on success, -1 on error
 */
int cuda_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                      uint32_t count, int bits);

/**
 * Run batch Fermat primality test on GPU (asynchronous, stream-scoped).
 *
 * Launches the kernel and async memcpys on the provided stream and returns
 * immediately. The caller is responsible for synchronizing the stream (via
 * cuda_fermat_stream_sync() or a cuLaunchHostFunc completion callback)
 * before reading h_results.
 *
 * For maximum throughput, h_primes and h_results SHOULD point at page-locked
 * host memory allocated via cuda_fermat_host_alloc(). Without pinning, the
 * async memcpys silently degrade to synchronous behavior.
 *
 * @param stream  Caller-managed stream (opaque CUstream cast to void*).
 *                Pass nullptr for the default null-stream (sync) behavior.
 * @return 0 on success, -1 on error (work may still be in flight on stream)
 */
int cuda_fermat_batch_async(uint8_t *h_results, const uint32_t *h_primes,
                             uint32_t count, int bits, void* stream);

/** Stream lifecycle (CUstream cast to/from void*). */
void* cuda_fermat_stream_create(void);
void  cuda_fermat_stream_destroy(void* stream);
int   cuda_fermat_stream_sync(void* stream);
int   cuda_fermat_stream_idle(void* stream);

/**
 * Register host completion callback. Fires on a CUDA-internal thread after
 * the preceding stream work completes. Returns -1 if the driver predates
 * CUDA 10 (no cuLaunchHostFunc). Caller must keep userdata alive until cb.
 */
int   cuda_fermat_stream_on_complete(void* stream, void (*cb)(void*), void* userdata);

/** Page-locked host memory for async memcpy. */
void* cuda_fermat_host_alloc(size_t bytes);
void  cuda_fermat_host_free(void* p);

/**
 * Get number of available CUDA devices.
 * @return Number of CUDA-capable GPUs, 0 if none
 */
int cuda_get_device_count(void);

/**
 * Get device name string.
 * @param device_id CUDA device index
 * @return Device name (static buffer, do not free)
 */
const char* cuda_get_device_name(int device_id);

/**
 * Get device global memory size.
 * @param device_id CUDA device index
 * @return Memory size in bytes
 */
size_t cuda_get_device_memory(int device_id);

/**
 * Get device streaming multiprocessor count.
 * @param device_id CUDA device index
 * @return Number of SMs
 */
int cuda_get_sm_count(int device_id);

/**
 * Run self-test to verify Montgomery math correctness.
 * Tests fermat320 with known primes and composites on the GPU.
 * Must be called after cuda_fermat_init().
 *
 * @return 0 if all tests pass, -1 on failure
 */
int cuda_fermat_selftest(void);

/**
 * Check if CUDA is available on this system.
 * Attempts to load the CUDA driver and detect devices.
 * @return 1 if available, 0 if not
 */
int cuda_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_GPU_CUDA_FERMAT_H
