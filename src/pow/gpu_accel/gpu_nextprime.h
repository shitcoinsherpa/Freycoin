// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_POW_GPU_NEXTPRIME_H
#define FREYCOIN_POW_GPU_NEXTPRIME_H

#include <gmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize GPU for nextprime computation. Returns 0 on success. */
int gpu_nextprime_init(int device_id);

/** Find the next prime after n using GPU batch BPSW. Returns 0 on success. */
int gpu_nextprime(mpz_ptr result, mpz_srcptr n);

/** Set GPU intensity for nextprime (0.05–1.0). Lower = less GPU load. */
void gpu_nextprime_set_intensity(float intensity);

/** Release GPU resources. */
void gpu_nextprime_cleanup(void);

/** Telemetry: cumulative GPU kernel launches since process start.
 *  Use deltas across mining template iterations to verify the GPU is doing
 *  real work — nvidia-smi samples at ~1Hz and routinely misses sub-second
 *  FFT bursts, so on healthy nodes it can falsely appear "idle". */
unsigned long long gpu_nextprime_kernel_launches(void);

/** Telemetry: cumulative GPU candidates batched (sum of batch sizes). */
unsigned long long gpu_nextprime_candidates_tested(void);

/** Telemetry: cumulative GPU wall time, milliseconds. */
unsigned long long gpu_nextprime_gpu_time_ms(void);

/** Telemetry: cumulative GPU PRP hits (probably-prime, before GMP confirm). */
unsigned long long gpu_nextprime_prp_hits(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_POW_GPU_NEXTPRIME_H
