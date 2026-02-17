// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * CGBN-based Fermat Primality Test — Host API
 *
 * Warp-cooperative big number arithmetic using NVIDIA's CGBN library
 * (bundled). Supports arbitrary bit widths from 320 to 16640, covering
 * both pre-fork (shift 14-256) and post-fork (shift 1024-16384).
 *
 * CGBN kernels are pre-compiled to PTX and embedded in the binary —
 * no nvcc or CUDA SDK needed at build time. JIT-compiled by the CUDA
 * driver at runtime.
 *
 * At runtime, the requested bit width is rounded UP to the nearest
 * supported CGBN tier. Input data is zero-padded to match.
 *
 * Supported tiers: 320, 384, 512, 1024, 1280, 2048, 4096,
 *                  8192, 8448, 12288, 16384, 16640
 */

#ifndef FREYCOIN_GPU_CGBN_FERMAT_H
#define FREYCOIN_GPU_CGBN_FERMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the CGBN module. Loads the embedded CGBN PTX and
 * JIT-compiles it. Must be called after cuda_fermat_init().
 *
 * @return 0 on success, -1 on error
 */
int cgbn_fermat_init(void);

/**
 * Batch Fermat primality test using CGBN cooperative groups.
 *
 * @param h_results  Output: 1 = probable prime, 0 = composite
 * @param h_primes   Input: limb-packed candidate numbers (little-endian 32-bit limbs)
 * @param count      Number of candidates
 * @param bits       Bit width of input data (will be rounded up to nearest CGBN tier)
 * @return 0 on success, negative on error
 *
 * Input layout: h_primes[i * limbs + j] where limbs = ceil(bits/32).
 * The function handles zero-padding to the CGBN tier's limb count internally.
 */
int cgbn_fermat_batch(uint8_t *h_results,
                      const uint32_t *h_primes,
                      uint32_t count,
                      int bits);

/** Check if CGBN PTX module is loaded and ready */
int cgbn_is_available(void);

/** Clean up CGBN module resources */
void cgbn_fermat_cleanup(void);

/** Maximum supported bit width */
#define CGBN_MAX_BITS 16640

#ifdef __cplusplus
}
#endif

#endif /* FREYCOIN_GPU_CGBN_FERMAT_H */
