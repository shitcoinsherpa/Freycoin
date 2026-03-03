/*
 * ecpp_gpu_shim.h — GPU batch acceleration for CM's ECPP downrun
 *
 * Provides GPU-accelerated replacements for CM's serial inner loops:
 *   1. Tonelli-Shanks batch (compute_qroot)
 *   2. Cornacchia batch (cm_ecpp_compute_cardinalities)
 *   3. BPSW batch (contains_ecpp_discriminant primality tests)
 *
 * Marshals between GMP's mpz_t (64-bit limbs) and GPU's uint32_t[]
 * (32-bit little-endian limb arrays).
 *
 * Copyright (c) 2026 The Freycoin developers
 * Licensed under GPL v3+
 */

#ifndef ECPP_GPU_SHIM_H
#define ECPP_GPU_SHIM_H

#include <gmp.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize GPU shim (calls ecpp_gpu_init). Returns 0 on success. */
int ecpp_gpu_shim_init(int device_id);

/* Clean up GPU resources. */
void ecpp_gpu_shim_cleanup(void);

/* Check if GPU is available. */
int ecpp_gpu_shim_is_available(void);

/* Get GPU device name (from ecpp_gpu_driver). */
const char* ecpp_gpu_device_name(void);

/*
 * GPU-accelerated batch Tonelli-Shanks.
 *
 * Replaces CM's compute_qroot() serial loop.
 * Computes sqrt(qstar[i]) mod p for i = 0..count-1.
 *
 * @param qroot   Output array (mpz_t, must be initialized)
 * @param qstar   Signed prime array (small integers)
 * @param count   Number of elements
 * @param p       The prime modulus (large, e.g. 12K bits)
 * @param e       2-adic valuation: p-1 = 2^e * q
 * @param r       Odd part q of p-1
 * @param z       QNR^q mod p (Tonelli generator)
 * @return 0 on success, negative on error
 */
int gpu_batch_tonelli(mpz_t *qroot, const long int *qstar, int count,
                      mpz_srcptr p, unsigned int e, mpz_srcptr r,
                      mpz_srcptr z);

/*
 * GPU-accelerated batch Cornacchia with sqrt(d) reconstruction.
 *
 * For each discriminant d[i], reconstructs sqrt(d[i]) mod N from
 * the cached qroot values, then solves 4N = t^2 + |d|*v^2 via
 * Cornacchia on the GPU.
 *
 * @param t_out     Output traces (mpz_t array, count elements, initialized)
 * @param v_out     Output norms (mpz_t array, count elements, initialized)
 * @param success   Output success flags (count bytes)
 * @param d         Discriminant array (small integers)
 * @param count     Number of discriminants
 * @param N         The prime
 * @param qstar     Signed prime array
 * @param no_qstar  Length of qstar
 * @param qroot     Cached square roots of qstar mod N
 * @return 0 on success, negative on error
 */
int gpu_batch_cornacchia(mpz_t *t_out, mpz_t *v_out, uint8_t *success,
                         const int64_t *d, int count,
                         mpz_srcptr N, const long int *qstar,
                         int no_qstar, mpz_t *qroot);

/*
 * GPU-accelerated batch primality test (strong MR base 2).
 *
 * Replaces CM's serial cm_nt_is_prime() loop in contains_ecpp_discriminant.
 *
 * @param results   Output: 1=probable prime, 0=composite (count bytes)
 * @param n         Array of numbers to test (mpz_t, count elements)
 * @param count     Number of elements
 * @return 0 on success, negative on error
 */
int gpu_batch_primality(uint8_t *results, mpz_t *n, int count);

/*
 * GPU-accelerated batch polynomial powmod (ECPP Phase 2 uprun).
 *
 * Computes (X+a_i)^e mod f mod p for each a_i in batch_a[0..count-1].
 *
 * @param h_results   Output [count * H * limbs] coefficients (LE uint32_t).
 *                    Coefficient j of batch element b is at
 *                    h_results[(b*H + j) * limbs .. +limbs-1].
 * @param h_f         [H * limbs] modular polynomial (non-leading coefficients
 *                    of the monic degree-H polynomial f, index 0..H-1).
 * @param h_e         [e_limbs] exponent in LE uint32_t words.
 * @param h_p         [limbs] prime modulus in LE uint32_t words.
 * @param batch_a     [count] uint64_t linear-term values (small integers).
 * @param H           Degree of f (must be 1..16).
 * @param bits        Bit length of the prime p.
 * @param e_bits      Number of significant bits in e.
 * @param count       Batch size (number of a values to process).
 * @return 0 on success, -1 on CUDA error, -2 if H > POLY_H_MAX.
 */
int gpu_poly_powmod_batch(uint32_t       *h_results,
                          const uint32_t *h_f,
                          const uint32_t *h_e,
                          const uint32_t *h_p,
                          const uint64_t *batch_a,
                          int             H,
                          int             bits,
                          uint32_t        e_bits,
                          int             count);

/* --- Utility: mpz_t <-> uint32_t[] marshaling --- */

/* Export mpz_t to little-endian uint32_t array. Pads to limbs words. */
void mpz_to_u32_le(uint32_t *out, mpz_srcptr in, int limbs);

/* Import little-endian uint32_t array to mpz_t. */
void u32_le_to_mpz(mpz_ptr out, const uint32_t *in, int limbs);

#ifdef __cplusplus
}
#endif

#endif /* ECPP_GPU_SHIM_H */
