// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * CGBN-based Fermat Primality Test — Host API
 *
 * Optional acceleration using NVIDIA's CGBN library for warp-cooperative
 * big number arithmetic. Falls back to custom Montgomery kernels when
 * CGBN is not available.
 *
 * Build: cmake -DWITH_CGBN=ON -DCGBN_INCLUDE_DIR=/path/to/cgbn/include
 * Source: https://github.com/NVlabs/CGBN
 */

#ifndef FREYCOIN_GPU_CGBN_FERMAT_H
#define FREYCOIN_GPU_CGBN_FERMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_CGBN

/**
 * Batch Fermat primality test using CGBN cooperative groups.
 *
 * @param h_results  Output: 1 = probable prime, 0 = composite
 * @param h_primes   Input: limb-packed candidate numbers
 * @param count      Number of candidates
 * @param bits       Bit width (320 or 352)
 * @return 0 on success, negative on error
 *
 * Note: For 352-bit, input should be 11 limbs per candidate.
 * This function internally pads to 12 limbs (384-bit) for CGBN alignment.
 */
int cgbn_fermat_batch(uint8_t *h_results,
                      const uint32_t *h_primes,
                      uint32_t count,
                      int bits);

/** Check if CGBN support was compiled in */
int cgbn_is_available(void);

#else

/* Stubs when CGBN is not available */
static inline int cgbn_fermat_batch(uint8_t *h_results,
                                     const uint32_t *h_primes,
                                     uint32_t count,
                                     int bits) {
    (void)h_results; (void)h_primes; (void)count; (void)bits;
    return -1;  /* Not available */
}

static inline int cgbn_is_available(void) {
    return 0;
}

#endif /* HAVE_CGBN */

#ifdef __cplusplus
}
#endif

#endif /* FREYCOIN_GPU_CGBN_FERMAT_H */
