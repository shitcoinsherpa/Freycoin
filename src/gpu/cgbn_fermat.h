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
 * supported CGBN tier that leaves at least 32 bits of slack (a
 * full-width modulus would take an unvalidated Barrett normalization
 * path — see cgbn_fermat.cu). Input data is zero-padded to the tier.
 *
 * Supported tiers: 320, 384, 512, 1024, 1280, 2048, 4096,
 *                  8192, 8448, 12288, 16384, 16640, 16672
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

/**
 * v2511.9: stream-async variant of cgbn_fermat_batch.
 * Queues H2D + kernel + DtoH on the supplied stream and returns immediately.
 * Caller must cuda_fermat_stream_sync(stream) before reading h_results.
 * Pass stream=nullptr to fall back to the legacy null-stream/sync behavior.
 */
int cgbn_fermat_batch_async(uint8_t *h_results,
                            const uint32_t *h_primes,
                            uint32_t count,
                            int bits,
                            void* stream);

/** Check if CGBN PTX module is loaded and ready */
int cgbn_is_available(void);

/**
 * Known-answer probe of every CGBN tier (runs at the end of
 * cgbn_fermat_init). A wrong verdict or batch error marks the tier
 * failed. Returns the number of failed tiers, or -1 if CGBN is not
 * initialized.
 */
int cgbn_fermat_probe_tiers(void);

/**
 * Returns 1 only if the tier selected for `bits`-wide inputs passed its
 * known-answer probe. Mining must not start at a shift whose width
 * returns 0 here.
 */
int cgbn_fermat_bits_validated(int bits);

/** Clean up CGBN module resources */
void cgbn_fermat_cleanup(void);

/** Largest tier; usable input width is CGBN_MAX_BITS - 32 (slack rule). */
#define CGBN_MAX_BITS 16672

#ifdef __cplusplus
}
#endif

#endif /* FREYCOIN_GPU_CGBN_FERMAT_H */
