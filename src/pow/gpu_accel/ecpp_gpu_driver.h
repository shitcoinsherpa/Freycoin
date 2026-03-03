/*
 * ecpp_gpu_driver.h — CUDA Driver API host interface for ECPP kernels
 *
 * Loads pre-compiled PTX kernels at runtime via the CUDA Driver API.
 * No CUDA SDK needed at runtime — only nvcuda.dll (ships with all
 * NVIDIA display drivers).
 *
 * Copyright (c) 2026 The Freycoin developers
 * Licensed under GPL v3+
 */

#ifndef ECPP_GPU_DRIVER_H
#define ECPP_GPU_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Initialization ─────────────────────────────────────────────────── */

/*
 * Initialize CUDA for ECPP: load driver API, create context, JIT-compile
 * all embedded PTX modules.
 *
 * @param device_id  CUDA device index (0 = first GPU)
 * @return 0 on success, -1 on error, -2 if no CUDA available
 */
int ecpp_gpu_init(int device_id);

/* Clean up all CUDA resources. */
void ecpp_gpu_cleanup(void);

/* Check if GPU acceleration is available and initialized. */
int ecpp_gpu_is_available(void);

/* Get GPU device name (after init). */
const char* ecpp_gpu_device_name(void);

/* Get GPU memory in bytes (after init). */
uint64_t ecpp_gpu_device_memory(void);

/* ─── Batch Tonelli-Shanks ───────────────────────────────────────────── */

/*
 * Batch modular square root: compute sqrt(a_i) mod p for i = 0..count-1.
 *
 * All instances share the same prime p. Caller precomputes:
 *   p-1 = 2^e * q  (q odd),  z = QNR^q mod p
 *
 * Data layout (all limb arrays have `limbs = ceil(bits/32)` uint32_t words,
 * little-endian):
 *
 *   h_params:  [p | q | z] contiguous, 3 * limbs words
 *   h_a:       count * limbs words (per-instance a values, reduced mod p)
 *
 * Output:
 *   h_roots:   count * limbs words (valid if h_success[i] == 1)
 *   h_success: count bytes (1 = root found, 0 = a not a QR)
 *
 * @param bits  Actual bit width of the prime (rounded up to nearest tier)
 * @return 0 on success, negative on error
 */
int ecpp_gpu_tonelli_batch(
    uint32_t *h_roots,
    uint8_t  *h_success,
    const uint32_t *h_params,
    uint32_t e_val,
    const uint32_t *h_a,
    uint32_t count,
    int bits);

/* ─── Batch Cornacchia ──────────────────────────────────────────────── */

/*
 * Batch Cornacchia: solve 4p = t² + |d_i|·v² for i = 0..count-1.
 *
 * All instances share the same prime p. Caller provides:
 *   - sqrt(d_i) mod p for each discriminant (from Tonelli-Shanks batch)
 *   - d_i (small negative integers, int32_t array)
 *
 * Data layout (all limb arrays have `limbs = ceil(bits/32)` words, LE):
 *
 *   h_p:       limbs words (the prime, shared)
 *   h_sqrt_d:  count * limbs words (per-instance modular square roots)
 *   h_d:       count int32_t (per-instance discriminants)
 *
 * Output:
 *   h_t:       count * limbs words (trace, valid if h_success[i] == 1)
 *   h_v:       count * limbs words (norm, valid if h_success[i] == 1)
 *   h_success: count bytes (1 = solution found, 0 = no solution)
 *
 * @param bits  Actual bit width of the prime (tier selected as bits+3)
 * @return 0 on success, negative on error
 */
int ecpp_gpu_cornacchia_batch(
    uint32_t *h_t,
    uint32_t *h_v,
    uint8_t  *h_success,
    const uint32_t *h_p,
    const uint32_t *h_sqrt_d,
    const int32_t  *h_d,
    uint32_t count,
    int bits);

/* ─── Batch BPSW (Strong Miller-Rabin base 2) ──────────────────────── */

/*
 * Batch primality test: strong Miller-Rabin base 2 for each n_i.
 *
 * Each instance tests a DIFFERENT number n_i. No shared parameters.
 *
 * Data layout:
 *   h_n:       count * limbs words (per-instance numbers to test, LE)
 *
 * Output:
 *   h_results: count bytes (1 = probable prime, 0 = composite)
 *
 * @param bits  Actual bit width of the largest number
 * @return 0 on success, negative on error
 */
int ecpp_gpu_bpsw_batch(
    uint8_t  *h_results,
    const uint32_t *h_n,
    uint32_t count,
    int bits);

/* ─── Batch EC Point Multiplication ─────────────────────────────────── */

/*
 * Batch EC scalar multiplication: compute m_i · P_i on y² = x³ + ax + b
 * over F_p for i = 0..count-1.
 *
 * All instances share the same prime p and curve coefficient a.
 * Per-instance: affine point (Px, Py) and scalar m.
 *
 * Data layout (all limb arrays have `limbs = ceil(bits/32)` words, LE):
 *
 *   h_params:     [p | a] contiguous, 2 * limbs words
 *   h_points:     count * 2 * limbs words [Px | Py] per instance
 *   h_scalars:    count * limbs words [m] per instance
 *
 * Output:
 *   h_result_x:   count * limbs words (valid if h_is_infinity[i] == 0)
 *   h_result_y:   count * limbs words (valid if h_is_infinity[i] == 0)
 *   h_is_infinity: count bytes (1 = result is point at infinity)
 *
 * @param bits  Actual bit width of the prime
 * @return 0 on success, negative on error
 */
int ecpp_gpu_ec_multiply_batch(
    uint32_t *h_result_x,
    uint32_t *h_result_y,
    uint8_t  *h_is_infinity,
    const uint32_t *h_params,
    const uint32_t *h_points,
    const uint32_t *h_scalars,
    uint32_t count,
    int bits);

/* ─── Batch Polynomial Powmod (ECPP Phase 2) ─────────────────────────── */

/*
 * Batch polynomial powmod: compute (X+a_i)^e mod f mod p for i=0..count-1.
 *
 * Replaces the serial mpzx_xplusa_pow_modmod() loop in CM's
 * mpzx_onefactor_split_mod() during Cantor-Zassenhaus polynomial splitting.
 * All batch elements share the same f, e, p but have different a_i values.
 *
 * Data layout (all limb arrays have `limbs = ceil(bits/32)` uint32_t words,
 * little-endian):
 *
 *   h_f:       H * limbs words (polynomial coefficients f[0]..f[H-1])
 *   h_e:       e_limbs words (exponent, LE)
 *   h_p:       limbs words (prime modulus)
 *   h_a:       count uint64_t (one a value per batch element)
 *
 * Output:
 *   h_results: count * H * limbs words — polynomial coefficients per batch
 *              element.  Coefficient j of element b is at offset
 *              (b*H + j) * limbs.
 *
 * @param H         Degree of modular polynomial f (must be 1..POLY_H_MAX=16)
 * @param bits      Actual bit width of prime p
 * @param e_bits    Number of significant bits in e
 * @return 0 on success, -1 on CUDA error, -2 if H > POLY_H_MAX
 */
int ecpp_gpu_poly_powmod_batch(
    uint32_t       *h_results,
    const uint32_t *h_f,
    const uint32_t *h_e,
    const uint32_t *h_p,
    const uint64_t *h_a,
    uint32_t        H,
    int             bits,
    uint32_t        e_bits,
    uint32_t        count);

#ifdef __cplusplus
}
#endif

#endif /* ECPP_GPU_DRIVER_H */
