/*
 * ecpp_gpu_shim.c — GPU batch acceleration for CM's ECPP downrun
 *
 * Marshals between GMP's mpz_t world and the GPU's uint32_t[] limb arrays,
 * then dispatches to the pre-compiled CUDA kernels via ecpp_gpu_driver.
 *
 * Copyright (c) 2026 The Freycoin developers
 * Licensed under GPL v3+
 */

#include <pow/gpu_accel/ecpp_gpu_shim.h>
#include <pow/gpu_accel/ecpp_gpu_driver.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Marshaling utilities ──────────────────────────────────────────── */

void mpz_to_u32_le(uint32_t *out, mpz_srcptr in, int limbs)
{
    size_t count = 0;
    memset(out, 0, (size_t)limbs * sizeof(uint32_t));
    if (mpz_sgn(in) == 0) return;

    /* Export absolute value as little-endian uint32_t words. */
    mpz_export(out, &count, -1, sizeof(uint32_t), 0, 0, in);
}

void u32_le_to_mpz(mpz_ptr out, const uint32_t *in, int limbs)
{
    /* Find actual length (strip trailing zeros). */
    int actual = limbs;
    while (actual > 0 && in[actual - 1] == 0) actual--;
    if (actual == 0) {
        mpz_set_ui(out, 0);
        return;
    }
    mpz_import(out, actual, -1, sizeof(uint32_t), 0, 0, in);
}

/* ─── Init / Cleanup ────────────────────────────────────────────────── */

int ecpp_gpu_shim_init(int device_id)
{
    return ecpp_gpu_init(device_id);
}

void ecpp_gpu_shim_cleanup(void)
{
    ecpp_gpu_cleanup();
}

int ecpp_gpu_shim_is_available(void)
{
    return ecpp_gpu_is_available();
}

/* ─── Batch Tonelli-Shanks ──────────────────────────────────────────── */

int gpu_batch_tonelli(mpz_t *qroot, const long int *qstar, int count,
                      mpz_srcptr p, unsigned int e, mpz_srcptr r,
                      mpz_srcptr z)
{
    if (count <= 0) return 0;

    int bits = (int)mpz_sizeinbase(p, 2);
    int limbs = (bits + 31) / 32;
    int rc;

    /* Allocate host buffers. */
    uint32_t *h_params = (uint32_t *)calloc(3 * (size_t)limbs, sizeof(uint32_t));
    uint32_t *h_a      = (uint32_t *)calloc((size_t)count * limbs, sizeof(uint32_t));
    uint32_t *h_roots  = (uint32_t *)calloc((size_t)count * limbs, sizeof(uint32_t));
    uint8_t  *h_success = (uint8_t *)calloc(count, sizeof(uint8_t));
    if (!h_params || !h_a || !h_roots || !h_success) {
        free(h_params); free(h_a); free(h_roots); free(h_success);
        return -1;
    }

    /* Marshal shared parameters: [p | q(=r) | z] */
    mpz_to_u32_le(h_params, p, limbs);
    mpz_to_u32_le(h_params + limbs, r, limbs);
    mpz_to_u32_le(h_params + 2 * limbs, z, limbs);

    /* Marshal per-instance a values.
     * qstar[i] is a small signed prime. The GPU Tonelli kernel expects
     * a = |qstar[i]| mod p (reduced, positive representative).
     * For negative qstar, compute p - |qstar| instead. */
    {
        mpz_t a_mpz;
        mpz_init(a_mpz);
        for (int i = 0; i < count; i++) {
            if (qstar[i] >= 0) {
                mpz_set_ui(a_mpz, (unsigned long)qstar[i]);
            } else {
                /* a = p - |qstar[i]| = p + qstar[i] */
                mpz_set_ui(a_mpz, (unsigned long)(-qstar[i]));
                mpz_sub(a_mpz, p, a_mpz);
            }
            mpz_to_u32_le(h_a + (size_t)i * limbs, a_mpz, limbs);
        }
        mpz_clear(a_mpz);
    }

    /* Dispatch to GPU. */
    rc = ecpp_gpu_tonelli_batch(h_roots, h_success, h_params,
                                (uint32_t)e, h_a, (uint32_t)count, bits);
    if (rc != 0) {
        fprintf(stderr, "GPU Tonelli batch failed: %d\n", rc);
        goto cleanup;
    }

    /* Unmarshal results back to mpz_t.
     * The GPU returns sqrt(|qstar[i]|) mod p. For negative qstar[i],
     * we need sqrt(qstar[i]) = sqrt(-1) * sqrt(|qstar[i]|) mod p.
     * Actually, CM's Tonelli-Shanks computes sqrt of the Kronecker
     * symbol directly. For negative qstar, the caller already checked
     * that qstar[i] is a QR mod p, so we compute sqrt(p + qstar[i])
     * which is the same as sqrt(qstar[i] mod p). The GPU computes
     * sqrt(a) where a = qstar[i] mod p, so the result is correct. */
    for (int i = 0; i < count; i++) {
        if (h_success[i]) {
            u32_le_to_mpz(qroot[i], h_roots + (size_t)i * limbs, limbs);
        } else {
            /* Should not happen if Kronecker filter is correct. */
            mpz_set_ui(qroot[i], 0);
            fprintf(stderr, "GPU Tonelli: qstar[%d]=%ld failed (not QR?)\n",
                    i, qstar[i]);
        }
    }

cleanup:
    free(h_params);
    free(h_a);
    free(h_roots);
    free(h_success);
    return rc;
}

/* ─── Batch Cornacchia ──────────────────────────────────────────────── */

int gpu_batch_cornacchia(mpz_t *t_out, mpz_t *v_out, uint8_t *success,
                         const int64_t *d, int count,
                         mpz_srcptr N, const long int *qstar,
                         int no_qstar, mpz_t *qroot)
{
    if (count <= 0) return 0;

    int bits = (int)mpz_sizeinbase(N, 2);
    int limbs = (bits + 31) / 32;
    int rc;

    /* Allocate host buffers. */
    uint32_t *h_p      = (uint32_t *)calloc(limbs, sizeof(uint32_t));
    uint32_t *h_sqrt_d = (uint32_t *)calloc((size_t)count * limbs, sizeof(uint32_t));
    int32_t  *h_d      = (int32_t *)calloc(count, sizeof(int32_t));
    uint32_t *h_t      = (uint32_t *)calloc((size_t)count * limbs, sizeof(uint32_t));
    uint32_t *h_v      = (uint32_t *)calloc((size_t)count * limbs, sizeof(uint32_t));
    uint8_t  *h_success = (uint8_t *)calloc(count, sizeof(uint8_t));
    if (!h_p || !h_sqrt_d || !h_d || !h_t || !h_v || !h_success) {
        free(h_p); free(h_sqrt_d); free(h_d);
        free(h_t); free(h_v); free(h_success);
        return -1;
    }

    /* Marshal shared prime N. */
    mpz_to_u32_le(h_p, N, limbs);

    /* For each discriminant, reconstruct sqrt(d) mod N from cached qroots.
     * This is CM's sqrt_d() logic: multiply qroot[j] for each prime factor
     * of d, reducing mod N at each step. */
    {
        mpz_t Droot;
        mpz_init(Droot);

        for (int i = 0; i < count; i++) {
            int64_t d_copy = d[i];
            h_d[i] = (int32_t)d[i];

            mpz_set_ui(Droot, 1);

            /* Multiply in roots for odd primes. */
            for (int j = 0; j < no_qstar; j++) {
                if (qstar[j] % 2 != 0 && d_copy % qstar[j] == 0) {
                    mpz_mul(Droot, Droot, qroot[j]);
                    mpz_mod(Droot, Droot, N);
                    d_copy /= qstar[j];
                }
            }
            /* Handle remaining even factor (if any). */
            if (d_copy != 1) {
                for (int j = 0; j < no_qstar; j++) {
                    if (d_copy == qstar[j]) {
                        mpz_mul(Droot, Droot, qroot[j]);
                        mpz_mod(Droot, Droot, N);
                        break;
                    }
                }
            }

            mpz_to_u32_le(h_sqrt_d + (size_t)i * limbs, Droot, limbs);
        }

        mpz_clear(Droot);
    }

    /* Dispatch to GPU Cornacchia. */
    rc = ecpp_gpu_cornacchia_batch(h_t, h_v, h_success, h_p,
                                   h_sqrt_d, h_d, (uint32_t)count, bits);
    if (rc != 0) {
        fprintf(stderr, "GPU Cornacchia batch failed: %d\n", rc);
        goto cleanup;
    }

    /* Unmarshal results. */
    for (int i = 0; i < count; i++) {
        success[i] = h_success[i];
        if (h_success[i]) {
            u32_le_to_mpz(t_out[i], h_t + (size_t)i * limbs, limbs);
            u32_le_to_mpz(v_out[i], h_v + (size_t)i * limbs, limbs);
        }
    }

cleanup:
    free(h_p);
    free(h_sqrt_d);
    free(h_d);
    free(h_t);
    free(h_v);
    free(h_success);
    return rc;
}

/* ─── Batch Primality (BPSW) ───────────────────────────────────────── */

int gpu_batch_primality(uint8_t *results, mpz_t *n, int count)
{
    if (count <= 0) return 0;

    /* Find max bit size across all candidates. */
    int max_bits = 0;
    for (int i = 0; i < count; i++) {
        int b = (int)mpz_sizeinbase(n[i], 2);
        if (b > max_bits) max_bits = b;
    }

    int limbs = (max_bits + 31) / 32;
    int rc;

    uint32_t *h_n = (uint32_t *)calloc((size_t)count * limbs, sizeof(uint32_t));
    if (!h_n) return -1;

    /* Marshal all candidates. */
    for (int i = 0; i < count; i++)
        mpz_to_u32_le(h_n + (size_t)i * limbs, n[i], limbs);

    /* Dispatch to GPU. */
    rc = ecpp_gpu_bpsw_batch(results, h_n, (uint32_t)count, max_bits);
    if (rc != 0)
        fprintf(stderr, "GPU BPSW batch failed: %d\n", rc);

    free(h_n);
    return rc;
}

/* ─── GPU batch polynomial powmod (Phase 2) ────────────────────────────── */

int gpu_poly_powmod_batch(uint32_t       *h_results,
                          const uint32_t *h_f,
                          const uint32_t *h_e,
                          const uint32_t *h_p,
                          const uint64_t *batch_a,
                          int             H,
                          int             bits,
                          uint32_t        e_bits,
                          int             count)
{
    if (!ecpp_gpu_is_available() || count == 0 || H <= 0 || H > 16)
        return (H > 16) ? -2 : -1;

    return ecpp_gpu_poly_powmod_batch(h_results, h_f, h_e, h_p, batch_a,
                                      (uint32_t)H, bits, e_bits,
                                      (uint32_t)count);
}
