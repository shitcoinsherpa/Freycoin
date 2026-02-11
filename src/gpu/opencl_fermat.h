// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * OpenCL Fermat Primality Test Interface
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Supports multi-GPU: each device gets independent context, queue, kernels.
 * Legacy single-device API operates on device 0 for backward compatibility.
 */

#ifndef FREYCOIN_GPU_OPENCL_FERMAT_H
#define FREYCOIN_GPU_OPENCL_FERMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Legacy single-device API (backward compatible, operates on device 0)
// ============================================================================

/**
 * Initialize OpenCL for Fermat primality testing on a single device.
 * @param device_id Device index (0 for first GPU)
 * @return 0 on success, -1 on error, -2 if OpenCL not available
 */
int opencl_fermat_init(int device_id);

/**
 * Cleanup OpenCL resources for device 0.
 */
void opencl_fermat_cleanup(void);

/**
 * Run batch Fermat primality test on device 0.
 * Tests if 2^(p-1) ≡ 1 (mod p) for each prime candidate.
 *
 * @param h_results Output array: 1 = probably prime, 0 = composite
 * @param h_primes  Input array of candidates (limb-packed format)
 * @param count     Number of candidates to test
 * @param bits      Bit size: 320 or 352
 * @return 0 on success, -1 on error
 */
int opencl_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                        uint32_t count, int bits);

// ============================================================================
// Multi-GPU API
// ============================================================================

/**
 * Initialize all available GPU devices for Fermat testing.
 * @param num_initialized Output: number of devices successfully initialized
 * @return 0 if at least one device initialized, -1 on total failure, -2 no OpenCL
 */
int opencl_fermat_init_all(int* num_initialized);

/**
 * Initialize a specific GPU device.
 * @param device_id Device index (0-based)
 * @return 0 on success, -1 on error, -2 if device out of range or no OpenCL
 */
int opencl_fermat_init_device(int device_id);

/**
 * Cleanup a specific GPU device.
 */
void opencl_fermat_cleanup_device(int device_id);

/**
 * Cleanup all GPU devices.
 */
void opencl_fermat_cleanup_all(void);

/**
 * Run batch Fermat primality test on a specific GPU device.
 * Thread-safe for different device_ids (each device has its own state).
 */
int opencl_fermat_batch_device(int device_id, uint8_t *h_results,
                               const uint32_t *h_primes,
                               uint32_t count, int bits);

/**
 * Get number of currently initialized GPU devices.
 */
int opencl_get_num_initialized(void);

// ============================================================================
// Query functions
// ============================================================================

/**
 * Get number of available OpenCL devices.
 * @return Number of OpenCL-capable GPUs, 0 if none
 */
int opencl_get_device_count(void);

/**
 * Get device name string.
 * @param device_id Device index
 * @return Device name (static buffer, do not free)
 */
const char* opencl_get_device_name(int device_id);

/**
 * Get device global memory size.
 * @param device_id Device index
 * @return Memory size in bytes
 */
size_t opencl_get_device_memory(int device_id);

/**
 * Check if OpenCL is available on this system.
 * @return 1 if available, 0 if not
 */
int opencl_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_GPU_OPENCL_FERMAT_H
