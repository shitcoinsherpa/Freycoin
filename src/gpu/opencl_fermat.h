// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * OpenCL Fermat Primality Test Interface
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_GPU_OPENCL_FERMAT_H
#define FREYCOIN_GPU_OPENCL_FERMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize OpenCL for Fermat primality testing.
 * @param device_id Device index (0 for first GPU)
 * @return 0 on success, -1 on error, -2 if OpenCL not available
 */
int opencl_fermat_init(int device_id);

/**
 * Cleanup OpenCL resources.
 */
void opencl_fermat_cleanup(void);

/**
 * Run batch Fermat primality test on GPU.
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
