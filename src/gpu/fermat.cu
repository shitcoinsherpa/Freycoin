/**
 * CUDA Fermat Primality Test Kernel for Freycoin
 *
 * Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
 * Copyright (c) 2025 The Freycoin developers
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * This implements Montgomery modular exponentiation for large integers
 * (320-bit and 352-bit) used in Fermat primality testing.
 *
 * Key algorithms:
 * - Montgomery multiplication/squaring (fully unrolled)
 * - Window-based modular exponentiation
 * - Multi-limb arithmetic (32-bit limbs)
 */

#include <cuda_runtime.h>
#include <stdint.h>

/* Configuration constants */
#define BLOCK_SIZE 64  /* Threads per block */

/* Lookup table for Montgomery inverse */
__constant__ uint32_t binvert_limb_table[128] = {
    0x01, 0xAB, 0xCD, 0xB7, 0x39, 0xA3, 0xC5, 0xEF,
    0xF1, 0x1B, 0x3D, 0xA7, 0x29, 0x13, 0x35, 0xDF,
    0xE1, 0x8B, 0xAD, 0x97, 0x19, 0x83, 0xA5, 0xCF,
    0xD1, 0xFB, 0x1D, 0x87, 0x09, 0xF3, 0x15, 0xBF,
    0xC1, 0x6B, 0x8D, 0x77, 0xF9, 0x63, 0x85, 0xAF,
    0xB1, 0xDB, 0xFD, 0x67, 0xE9, 0xD3, 0xF5, 0x9F,
    0xA1, 0x4B, 0x6D, 0x57, 0xD9, 0x43, 0x65, 0x8F,
    0x91, 0xBB, 0xDD, 0x47, 0xC9, 0xB3, 0xD5, 0x7F,
    0x81, 0x2B, 0x4D, 0x37, 0xB9, 0x23, 0x45, 0x6F,
    0x71, 0x9B, 0xBD, 0x27, 0xA9, 0x93, 0xB5, 0x5F,
    0x61, 0x0B, 0x2D, 0x17, 0x99, 0x03, 0x25, 0x4F,
    0x51, 0x7B, 0x9D, 0x07, 0x89, 0x73, 0x95, 0x3F,
    0x41, 0xEB, 0x0D, 0xF7, 0x79, 0xE3, 0x05, 0x2F,
    0x31, 0x5B, 0x7D, 0xE7, 0x69, 0x53, 0x75, 0x1F,
    0x21, 0xCB, 0xED, 0xD7, 0x59, 0xC3, 0xE5, 0x0F,
    0x11, 0x3B, 0x5D, 0xC7, 0x49, 0x33, 0x55, 0xFF
};

/* Power of 2 lookup table */
__constant__ uint32_t pow2[9] = {1, 2, 4, 8, 16, 32, 64, 128, 256};

/* Fermat candidate info structure */
struct fermat_t {
    uint32_t index;
    uint32_t hashid;
    uint8_t  origin;
    uint8_t  chainpos;
    uint8_t  type;
    uint8_t  reserved;
};

/*
 * Compute Montgomery inverse of lowest limb
 */
__device__ __forceinline__ uint32_t invert_limb(uint32_t limb) {
    uint32_t inv = binvert_limb_table[(limb >> 1) & 0x7F];
    inv = 2 * inv - inv * inv * limb;
    inv = 2 * inv - inv * inv * limb;
    return -inv;
}

/*
 * Subtraction with borrow propagation
 * a = a - b, returns 1 if borrow occurred
 */
__device__ __forceinline__ void sub10(uint32_t *a, const uint32_t *b) {
    uint32_t A[2];
    A[0] = a[0];
    a[0] -= b[0];
    #pragma unroll
    for (int i = 1; i < 10; i++) {
        A[1] = a[i];
        a[i] -= b[i] + (a[i-1] > A[0]);
        A[0] = A[1];
    }
}

__device__ __forceinline__ void sub11(uint32_t *a, const uint32_t *b) {
    uint32_t A[2];
    A[0] = a[0];
    a[0] -= b[0];
    #pragma unroll
    for (int i = 1; i < 11; i++) {
        A[1] = a[i];
        a[i] -= b[i] + (a[i-1] > A[0]);
        A[0] = A[1];
    }
}

/*
 * Montgomery squaring for 320-bit numbers (10 limbs)
 * Fully unrolled for maximum GPU performance
 */
__device__ void monSqr320(uint32_t *op, uint32_t *mod, uint32_t invm) {
    uint32_t invValue[10];
    uint64_t accLow = 0, accHi = 0;
    uint32_t low0, hi0, low1, hi1, low2, hi2, low3, hi3, low4, hi4;

    /* Step 0: i=0 */
    {
        accLow += (uint64_t)op[0]*op[0];
        accHi += __umulhi(op[0], op[0]);
        invValue[0] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[0]*mod[0];
        accHi += __umulhi(invValue[0], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 1: i=1 */
    {
        low0 = op[0]*op[1]; hi0 = __umulhi(op[0], op[1]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += (uint64_t)invValue[0]*mod[1];
        accHi += __umulhi(invValue[0], mod[1]);
        invValue[1] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[1]*mod[0];
        accHi += __umulhi(invValue[1], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 2: i=2 */
    {
        low0 = op[0]*op[2]; hi0 = __umulhi(op[0], op[2]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += (uint64_t)op[1]*op[1];
        accHi += __umulhi(op[1], op[1]);
        accLow += (uint64_t)invValue[0]*mod[2];
        accHi += __umulhi(invValue[0], mod[2]);
        accLow += (uint64_t)invValue[1]*mod[1];
        accHi += __umulhi(invValue[1], mod[1]);
        invValue[2] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[2]*mod[0];
        accHi += __umulhi(invValue[2], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 3: i=3 */
    {
        low0 = op[0]*op[3]; hi0 = __umulhi(op[0], op[3]);
        low1 = op[1]*op[2]; hi1 = __umulhi(op[1], op[2]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += (uint64_t)invValue[0]*mod[3];
        accHi += __umulhi(invValue[0], mod[3]);
        accLow += (uint64_t)invValue[1]*mod[2];
        accHi += __umulhi(invValue[1], mod[2]);
        accLow += (uint64_t)invValue[2]*mod[1];
        accHi += __umulhi(invValue[2], mod[1]);
        invValue[3] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[3]*mod[0];
        accHi += __umulhi(invValue[3], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 4: i=4 */
    {
        low0 = op[0]*op[4]; hi0 = __umulhi(op[0], op[4]);
        low1 = op[1]*op[3]; hi1 = __umulhi(op[1], op[3]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += (uint64_t)op[2]*op[2];
        accHi += __umulhi(op[2], op[2]);
        accLow += (uint64_t)invValue[0]*mod[4];
        accHi += __umulhi(invValue[0], mod[4]);
        accLow += (uint64_t)invValue[1]*mod[3];
        accHi += __umulhi(invValue[1], mod[3]);
        accLow += (uint64_t)invValue[2]*mod[2];
        accHi += __umulhi(invValue[2], mod[2]);
        accLow += (uint64_t)invValue[3]*mod[1];
        accHi += __umulhi(invValue[3], mod[1]);
        invValue[4] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[4]*mod[0];
        accHi += __umulhi(invValue[4], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 5: i=5 */
    {
        low0 = op[0]*op[5]; hi0 = __umulhi(op[0], op[5]);
        low1 = op[1]*op[4]; hi1 = __umulhi(op[1], op[4]);
        low2 = op[2]*op[3]; hi2 = __umulhi(op[2], op[3]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += (uint64_t)invValue[0]*mod[5];
        accHi += __umulhi(invValue[0], mod[5]);
        accLow += (uint64_t)invValue[1]*mod[4];
        accHi += __umulhi(invValue[1], mod[4]);
        accLow += (uint64_t)invValue[2]*mod[3];
        accHi += __umulhi(invValue[2], mod[3]);
        accLow += (uint64_t)invValue[3]*mod[2];
        accHi += __umulhi(invValue[3], mod[2]);
        accLow += (uint64_t)invValue[4]*mod[1];
        accHi += __umulhi(invValue[4], mod[1]);
        invValue[5] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[5]*mod[0];
        accHi += __umulhi(invValue[5], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 6: i=6 */
    {
        low0 = op[0]*op[6]; hi0 = __umulhi(op[0], op[6]);
        low1 = op[1]*op[5]; hi1 = __umulhi(op[1], op[5]);
        low2 = op[2]*op[4]; hi2 = __umulhi(op[2], op[4]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += (uint64_t)op[3]*op[3];
        accHi += __umulhi(op[3], op[3]);
        accLow += (uint64_t)invValue[0]*mod[6];
        accHi += __umulhi(invValue[0], mod[6]);
        accLow += (uint64_t)invValue[1]*mod[5];
        accHi += __umulhi(invValue[1], mod[5]);
        accLow += (uint64_t)invValue[2]*mod[4];
        accHi += __umulhi(invValue[2], mod[4]);
        accLow += (uint64_t)invValue[3]*mod[3];
        accHi += __umulhi(invValue[3], mod[3]);
        accLow += (uint64_t)invValue[4]*mod[2];
        accHi += __umulhi(invValue[4], mod[2]);
        accLow += (uint64_t)invValue[5]*mod[1];
        accHi += __umulhi(invValue[5], mod[1]);
        invValue[6] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[6]*mod[0];
        accHi += __umulhi(invValue[6], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 7: i=7 */
    {
        low0 = op[0]*op[7]; hi0 = __umulhi(op[0], op[7]);
        low1 = op[1]*op[6]; hi1 = __umulhi(op[1], op[6]);
        low2 = op[2]*op[5]; hi2 = __umulhi(op[2], op[5]);
        low3 = op[3]*op[4]; hi3 = __umulhi(op[3], op[4]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += low3; accLow += low3;
        accHi += hi3; accHi += hi3;
        accLow += (uint64_t)invValue[0]*mod[7];
        accHi += __umulhi(invValue[0], mod[7]);
        accLow += (uint64_t)invValue[1]*mod[6];
        accHi += __umulhi(invValue[1], mod[6]);
        accLow += (uint64_t)invValue[2]*mod[5];
        accHi += __umulhi(invValue[2], mod[5]);
        accLow += (uint64_t)invValue[3]*mod[4];
        accHi += __umulhi(invValue[3], mod[4]);
        accLow += (uint64_t)invValue[4]*mod[3];
        accHi += __umulhi(invValue[4], mod[3]);
        accLow += (uint64_t)invValue[5]*mod[2];
        accHi += __umulhi(invValue[5], mod[2]);
        accLow += (uint64_t)invValue[6]*mod[1];
        accHi += __umulhi(invValue[6], mod[1]);
        invValue[7] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[7]*mod[0];
        accHi += __umulhi(invValue[7], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 8: i=8 */
    {
        low0 = op[0]*op[8]; hi0 = __umulhi(op[0], op[8]);
        low1 = op[1]*op[7]; hi1 = __umulhi(op[1], op[7]);
        low2 = op[2]*op[6]; hi2 = __umulhi(op[2], op[6]);
        low3 = op[3]*op[5]; hi3 = __umulhi(op[3], op[5]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += low3; accLow += low3;
        accHi += hi3; accHi += hi3;
        accLow += (uint64_t)op[4]*op[4];
        accHi += __umulhi(op[4], op[4]);
        accLow += (uint64_t)invValue[0]*mod[8];
        accHi += __umulhi(invValue[0], mod[8]);
        accLow += (uint64_t)invValue[1]*mod[7];
        accHi += __umulhi(invValue[1], mod[7]);
        accLow += (uint64_t)invValue[2]*mod[6];
        accHi += __umulhi(invValue[2], mod[6]);
        accLow += (uint64_t)invValue[3]*mod[5];
        accHi += __umulhi(invValue[3], mod[5]);
        accLow += (uint64_t)invValue[4]*mod[4];
        accHi += __umulhi(invValue[4], mod[4]);
        accLow += (uint64_t)invValue[5]*mod[3];
        accHi += __umulhi(invValue[5], mod[3]);
        accLow += (uint64_t)invValue[6]*mod[2];
        accHi += __umulhi(invValue[6], mod[2]);
        accLow += (uint64_t)invValue[7]*mod[1];
        accHi += __umulhi(invValue[7], mod[1]);
        invValue[8] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[8]*mod[0];
        accHi += __umulhi(invValue[8], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 9: i=9 */
    {
        low0 = op[0]*op[9]; hi0 = __umulhi(op[0], op[9]);
        low1 = op[1]*op[8]; hi1 = __umulhi(op[1], op[8]);
        low2 = op[2]*op[7]; hi2 = __umulhi(op[2], op[7]);
        low3 = op[3]*op[6]; hi3 = __umulhi(op[3], op[6]);
        low4 = op[4]*op[5]; hi4 = __umulhi(op[4], op[5]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += low3; accLow += low3;
        accHi += hi3; accHi += hi3;
        accLow += low4; accLow += low4;
        accHi += hi4; accHi += hi4;
        accLow += (uint64_t)invValue[0]*mod[9];
        accHi += __umulhi(invValue[0], mod[9]);
        accLow += (uint64_t)invValue[1]*mod[8];
        accHi += __umulhi(invValue[1], mod[8]);
        accLow += (uint64_t)invValue[2]*mod[7];
        accHi += __umulhi(invValue[2], mod[7]);
        accLow += (uint64_t)invValue[3]*mod[6];
        accHi += __umulhi(invValue[3], mod[6]);
        accLow += (uint64_t)invValue[4]*mod[5];
        accHi += __umulhi(invValue[4], mod[5]);
        accLow += (uint64_t)invValue[5]*mod[4];
        accHi += __umulhi(invValue[5], mod[4]);
        accLow += (uint64_t)invValue[6]*mod[3];
        accHi += __umulhi(invValue[6], mod[3]);
        accLow += (uint64_t)invValue[7]*mod[2];
        accHi += __umulhi(invValue[7], mod[2]);
        accLow += (uint64_t)invValue[8]*mod[1];
        accHi += __umulhi(invValue[8], mod[1]);
        invValue[9] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[9]*mod[0];
        accHi += __umulhi(invValue[9], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Steps 10-18: output phase */
    {
        low0 = op[1]*op[9]; hi0 = __umulhi(op[1], op[9]);
        low1 = op[2]*op[8]; hi1 = __umulhi(op[2], op[8]);
        low2 = op[3]*op[7]; hi2 = __umulhi(op[3], op[7]);
        low3 = op[4]*op[6]; hi3 = __umulhi(op[4], op[6]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += low3; accLow += low3;
        accHi += hi3; accHi += hi3;
        accLow += (uint64_t)op[5]*op[5];
        accHi += __umulhi(op[5], op[5]);
        accLow += (uint64_t)invValue[1]*mod[9];
        accHi += __umulhi(invValue[1], mod[9]);
        accLow += (uint64_t)invValue[2]*mod[8];
        accHi += __umulhi(invValue[2], mod[8]);
        accLow += (uint64_t)invValue[3]*mod[7];
        accHi += __umulhi(invValue[3], mod[7]);
        accLow += (uint64_t)invValue[4]*mod[6];
        accHi += __umulhi(invValue[4], mod[6]);
        accLow += (uint64_t)invValue[5]*mod[5];
        accHi += __umulhi(invValue[5], mod[5]);
        accLow += (uint64_t)invValue[6]*mod[4];
        accHi += __umulhi(invValue[6], mod[4]);
        accLow += (uint64_t)invValue[7]*mod[3];
        accHi += __umulhi(invValue[7], mod[3]);
        accLow += (uint64_t)invValue[8]*mod[2];
        accHi += __umulhi(invValue[8], mod[2]);
        accLow += (uint64_t)invValue[9]*mod[1];
        accHi += __umulhi(invValue[9], mod[1]);
        op[0] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[2]*op[9]; hi0 = __umulhi(op[2], op[9]);
        low1 = op[3]*op[8]; hi1 = __umulhi(op[3], op[8]);
        low2 = op[4]*op[7]; hi2 = __umulhi(op[4], op[7]);
        low3 = op[5]*op[6]; hi3 = __umulhi(op[5], op[6]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += low3; accLow += low3;
        accHi += hi3; accHi += hi3;
        accLow += (uint64_t)invValue[2]*mod[9];
        accHi += __umulhi(invValue[2], mod[9]);
        accLow += (uint64_t)invValue[3]*mod[8];
        accHi += __umulhi(invValue[3], mod[8]);
        accLow += (uint64_t)invValue[4]*mod[7];
        accHi += __umulhi(invValue[4], mod[7]);
        accLow += (uint64_t)invValue[5]*mod[6];
        accHi += __umulhi(invValue[5], mod[6]);
        accLow += (uint64_t)invValue[6]*mod[5];
        accHi += __umulhi(invValue[6], mod[5]);
        accLow += (uint64_t)invValue[7]*mod[4];
        accHi += __umulhi(invValue[7], mod[4]);
        accLow += (uint64_t)invValue[8]*mod[3];
        accHi += __umulhi(invValue[8], mod[3]);
        accLow += (uint64_t)invValue[9]*mod[2];
        accHi += __umulhi(invValue[9], mod[2]);
        op[1] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[3]*op[9]; hi0 = __umulhi(op[3], op[9]);
        low1 = op[4]*op[8]; hi1 = __umulhi(op[4], op[8]);
        low2 = op[5]*op[7]; hi2 = __umulhi(op[5], op[7]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += (uint64_t)op[6]*op[6];
        accHi += __umulhi(op[6], op[6]);
        accLow += (uint64_t)invValue[3]*mod[9];
        accHi += __umulhi(invValue[3], mod[9]);
        accLow += (uint64_t)invValue[4]*mod[8];
        accHi += __umulhi(invValue[4], mod[8]);
        accLow += (uint64_t)invValue[5]*mod[7];
        accHi += __umulhi(invValue[5], mod[7]);
        accLow += (uint64_t)invValue[6]*mod[6];
        accHi += __umulhi(invValue[6], mod[6]);
        accLow += (uint64_t)invValue[7]*mod[5];
        accHi += __umulhi(invValue[7], mod[5]);
        accLow += (uint64_t)invValue[8]*mod[4];
        accHi += __umulhi(invValue[8], mod[4]);
        accLow += (uint64_t)invValue[9]*mod[3];
        accHi += __umulhi(invValue[9], mod[3]);
        op[2] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[4]*op[9]; hi0 = __umulhi(op[4], op[9]);
        low1 = op[5]*op[8]; hi1 = __umulhi(op[5], op[8]);
        low2 = op[6]*op[7]; hi2 = __umulhi(op[6], op[7]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += low2; accLow += low2;
        accHi += hi2; accHi += hi2;
        accLow += (uint64_t)invValue[4]*mod[9];
        accHi += __umulhi(invValue[4], mod[9]);
        accLow += (uint64_t)invValue[5]*mod[8];
        accHi += __umulhi(invValue[5], mod[8]);
        accLow += (uint64_t)invValue[6]*mod[7];
        accHi += __umulhi(invValue[6], mod[7]);
        accLow += (uint64_t)invValue[7]*mod[6];
        accHi += __umulhi(invValue[7], mod[6]);
        accLow += (uint64_t)invValue[8]*mod[5];
        accHi += __umulhi(invValue[8], mod[5]);
        accLow += (uint64_t)invValue[9]*mod[4];
        accHi += __umulhi(invValue[9], mod[4]);
        op[3] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[5]*op[9]; hi0 = __umulhi(op[5], op[9]);
        low1 = op[6]*op[8]; hi1 = __umulhi(op[6], op[8]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += (uint64_t)op[7]*op[7];
        accHi += __umulhi(op[7], op[7]);
        accLow += (uint64_t)invValue[5]*mod[9];
        accHi += __umulhi(invValue[5], mod[9]);
        accLow += (uint64_t)invValue[6]*mod[8];
        accHi += __umulhi(invValue[6], mod[8]);
        accLow += (uint64_t)invValue[7]*mod[7];
        accHi += __umulhi(invValue[7], mod[7]);
        accLow += (uint64_t)invValue[8]*mod[6];
        accHi += __umulhi(invValue[8], mod[6]);
        accLow += (uint64_t)invValue[9]*mod[5];
        accHi += __umulhi(invValue[9], mod[5]);
        op[4] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[6]*op[9]; hi0 = __umulhi(op[6], op[9]);
        low1 = op[7]*op[8]; hi1 = __umulhi(op[7], op[8]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += low1; accLow += low1;
        accHi += hi1; accHi += hi1;
        accLow += (uint64_t)invValue[6]*mod[9];
        accHi += __umulhi(invValue[6], mod[9]);
        accLow += (uint64_t)invValue[7]*mod[8];
        accHi += __umulhi(invValue[7], mod[8]);
        accLow += (uint64_t)invValue[8]*mod[7];
        accHi += __umulhi(invValue[8], mod[7]);
        accLow += (uint64_t)invValue[9]*mod[6];
        accHi += __umulhi(invValue[9], mod[6]);
        op[5] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[7]*op[9]; hi0 = __umulhi(op[7], op[9]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += (uint64_t)op[8]*op[8];
        accHi += __umulhi(op[8], op[8]);
        accLow += (uint64_t)invValue[7]*mod[9];
        accHi += __umulhi(invValue[7], mod[9]);
        accLow += (uint64_t)invValue[8]*mod[8];
        accHi += __umulhi(invValue[8], mod[8]);
        accLow += (uint64_t)invValue[9]*mod[7];
        accHi += __umulhi(invValue[9], mod[7]);
        op[6] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        low0 = op[8]*op[9]; hi0 = __umulhi(op[8], op[9]);
        accLow += low0; accLow += low0;
        accHi += hi0; accHi += hi0;
        accLow += (uint64_t)invValue[8]*mod[9];
        accHi += __umulhi(invValue[8], mod[9]);
        accLow += (uint64_t)invValue[9]*mod[8];
        accHi += __umulhi(invValue[9], mod[8]);
        op[7] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    {
        accLow += (uint64_t)op[9]*op[9];
        accHi += __umulhi(op[9], op[9]);
        accLow += (uint64_t)invValue[9]*mod[9];
        accHi += __umulhi(invValue[9], mod[9]);
        op[8] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
    }

    op[9] = (uint32_t)accLow;
    if ((uint32_t)(accLow >> 32))
        sub10(op, mod);
}

/*
 * Montgomery multiplication for 320-bit numbers (10 limbs)
 */
__device__ void monMul320(uint32_t *op1, uint32_t *op2, uint32_t *mod, uint32_t invm) {
    uint32_t invValue[10];
    uint64_t accLow = 0, accHi = 0;

    /* Step 0 */
    {
        accLow += (uint64_t)op1[0]*op2[0];
        accHi += __umulhi(op1[0], op2[0]);
        invValue[0] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[0]*mod[0];
        accHi += __umulhi(invValue[0], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Step 1 */
    {
        accLow += (uint64_t)op1[0]*op2[1];
        accHi += __umulhi(op1[0], op2[1]);
        accLow += (uint64_t)op1[1]*op2[0];
        accHi += __umulhi(op1[1], op2[0]);
        accLow += (uint64_t)invValue[0]*mod[1];
        accHi += __umulhi(invValue[0], mod[1]);
        invValue[1] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[1]*mod[0];
        accHi += __umulhi(invValue[1], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Steps 2-9 follow same pattern */
    #pragma unroll
    for (int i = 2; i < 10; i++) {
        for (int j = 0; j <= i; j++) {
            accLow += (uint64_t)op1[j]*op2[i-j];
            accHi += __umulhi(op1[j], op2[i-j]);
        }
        for (int j = 0; j < i; j++) {
            accLow += (uint64_t)invValue[j]*mod[i-j];
            accHi += __umulhi(invValue[j], mod[i-j]);
        }
        invValue[i] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[i]*mod[0];
        accHi += __umulhi(invValue[i], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Output phase: steps 10-18 */
    #pragma unroll
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) {
            accLow += (uint64_t)op1[j]*op2[10 + i - j];
            accHi += __umulhi(op1[j], op2[10 + i - j]);
        }
        for (int j = i + 1; j < 10; j++) {
            accLow += (uint64_t)invValue[j]*mod[10 + i - j];
            accHi += __umulhi(invValue[j], mod[10 + i - j]);
        }
        op1[i] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    op1[9] = (uint32_t)accLow;
    if ((uint32_t)(accLow >> 32))
        sub10(op1, mod);
}

/*
 * Montgomery reduction (half) for 320-bit numbers
 * Converts from Montgomery form to regular form
 */
__device__ void redcHalf320(uint32_t *op, uint32_t *mod, uint32_t invm) {
    uint32_t invValue[10];
    uint64_t accLow = op[0], accHi = op[1];

    /* Reduction phase */
    {
        invValue[0] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[0]*mod[0];
        accHi += __umulhi(invValue[0], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = op[2];
    }

    #pragma unroll
    for (int i = 1; i < 9; i++) {
        for (int j = 0; j < i; j++) {
            accLow += (uint64_t)invValue[j]*mod[i-j];
            accHi += __umulhi(invValue[j], mod[i-j]);
        }
        invValue[i] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[i]*mod[0];
        accHi += __umulhi(invValue[i], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = op[i+2];
    }

    /* i = 9 */
    {
        for (int j = 0; j < 9; j++) {
            accLow += (uint64_t)invValue[j]*mod[9-j];
            accHi += __umulhi(invValue[j], mod[9-j]);
        }
        invValue[9] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[9]*mod[0];
        accHi += __umulhi(invValue[9], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Output phase */
    #pragma unroll
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) {
            accLow += (uint64_t)invValue[j]*mod[10 + i - j];
            accHi += __umulhi(invValue[j], mod[10 + i - j]);
        }
        op[i] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    op[9] = (uint32_t)accLow;
    if ((uint32_t)(accLow >> 32))
        sub10(op, mod);
}

/*
 * Montgomery squaring for 352-bit numbers (11 limbs)
 */
__device__ void monSqr352(uint32_t *op, uint32_t *mod, uint32_t invm) {
    uint32_t invValue[11];
    uint64_t accLow = 0, accHi = 0;

    /* First phase: compute invValues and accumulate */
    #pragma unroll
    for (int i = 0; i <= 10; i++) {
        /* Diagonal term */
        if (i <= 10) {
            accLow += (uint64_t)op[i]*op[i];
            accHi += __umulhi(op[i], op[i]);
        }

        /* Cross products (doubled) */
        for (int j = 0; j < (i+1)/2; j++) {
            int k = i - j;
            if (k <= 10 && k != j) {
                uint64_t prod = (uint64_t)op[j]*op[k];
                accLow += prod; accLow += prod;
                accHi += __umulhi(op[j], op[k]); accHi += __umulhi(op[j], op[k]);
            }
        }

        /* Reduction terms */
        for (int j = 0; j < i; j++) {
            accLow += (uint64_t)invValue[j]*mod[i-j];
            accHi += __umulhi(invValue[j], mod[i-j]);
        }

        if (i <= 10) {
            invValue[i] = invm * (uint32_t)accLow;
            accLow += (uint64_t)invValue[i]*mod[0];
            accHi += __umulhi(invValue[i], mod[0]);
        }

        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Output phase */
    #pragma unroll
    for (int i = 0; i < 10; i++) {
        /* Cross products */
        for (int j = i + 1; j <= 10; j++) {
            int k = 11 + i - j;
            if (k >= 0 && k <= 10 && k > j) {
                uint64_t prod = (uint64_t)op[j]*op[k];
                accLow += prod; accLow += prod;
                accHi += __umulhi(op[j], op[k]); accHi += __umulhi(op[j], op[k]);
            }
        }

        /* Diagonal */
        int diag = (11 + i) / 2;
        if ((11 + i) % 2 == 0 && diag <= 10) {
            accLow += (uint64_t)op[diag]*op[diag];
            accHi += __umulhi(op[diag], op[diag]);
        }

        /* Reduction terms */
        for (int j = i + 1; j <= 10; j++) {
            accLow += (uint64_t)invValue[j]*mod[11 + i - j];
            accHi += __umulhi(invValue[j], mod[11 + i - j]);
        }

        op[i] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    op[10] = (uint32_t)accLow;
    if ((uint32_t)(accLow >> 32))
        sub11(op, mod);
}

/*
 * Montgomery multiplication for 352-bit numbers (11 limbs)
 */
__device__ void monMul352(uint32_t *op1, uint32_t *op2, uint32_t *mod, uint32_t invm) {
    uint32_t invValue[11];
    uint64_t accLow = 0, accHi = 0;

    /* First phase: compute invValues */
    #pragma unroll
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= i; j++) {
            accLow += (uint64_t)op1[j]*op2[i-j];
            accHi += __umulhi(op1[j], op2[i-j]);
        }
        for (int j = 0; j < i; j++) {
            accLow += (uint64_t)invValue[j]*mod[i-j];
            accHi += __umulhi(invValue[j], mod[i-j]);
        }
        invValue[i] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[i]*mod[0];
        accHi += __umulhi(invValue[i], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Output phase */
    #pragma unroll
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j <= 10; j++) {
            accLow += (uint64_t)op1[j]*op2[11 + i - j];
            accHi += __umulhi(op1[j], op2[11 + i - j]);
        }
        for (int j = i + 1; j <= 10; j++) {
            accLow += (uint64_t)invValue[j]*mod[11 + i - j];
            accHi += __umulhi(invValue[j], mod[11 + i - j]);
        }
        op1[i] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    op1[10] = (uint32_t)accLow;
    if ((uint32_t)(accLow >> 32))
        sub11(op1, mod);
}

/*
 * Montgomery reduction (half) for 352-bit numbers
 */
__device__ void redcHalf352(uint32_t *op, uint32_t *mod, uint32_t invm) {
    uint32_t invValue[11];
    uint64_t accLow = op[0], accHi = op[1];

    /* First phase */
    {
        invValue[0] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[0]*mod[0];
        accHi += __umulhi(invValue[0], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = op[2];
    }

    #pragma unroll
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < i; j++) {
            accLow += (uint64_t)invValue[j]*mod[i-j];
            accHi += __umulhi(invValue[j], mod[i-j]);
        }
        invValue[i] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[i]*mod[0];
        accHi += __umulhi(invValue[i], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = op[i+2];
    }

    /* i = 10 */
    {
        for (int j = 0; j < 10; j++) {
            accLow += (uint64_t)invValue[j]*mod[10-j];
            accHi += __umulhi(invValue[j], mod[10-j]);
        }
        invValue[10] = invm * (uint32_t)accLow;
        accLow += (uint64_t)invValue[10]*mod[0];
        accHi += __umulhi(invValue[10], mod[0]);
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    /* Output phase */
    #pragma unroll
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j <= 10; j++) {
            accLow += (uint64_t)invValue[j]*mod[11 + i - j];
            accHi += __umulhi(invValue[j], mod[11 + i - j]);
        }
        op[i] = (uint32_t)accLow;
        accHi += (uint32_t)(accLow >> 32);
        accLow = accHi;
        accHi = 0;
    }

    op[10] = (uint32_t)accLow;
    if ((uint32_t)(accLow >> 32))
        sub11(op, mod);
}

/*
 * Fermat test for 320-bit number using window exponentiation
 * Tests if 2^(p-1) = 1 (mod p)
 */
__device__ bool fermat320(uint32_t *p) {
    uint32_t mod[10], redcl[10], window[32][10];
    uint32_t invm;
    int bitCount, windowSize = 5;

    /* Copy modulus */
    #pragma unroll
    for (int i = 0; i < 10; i++) mod[i] = p[i];

    /* Get Montgomery inverse */
    invm = invert_limb(mod[0]);

    /* Find highest bit */
    bitCount = 320;
    for (int i = 9; i >= 0; i--) {
        if (mod[i]) {
            int clz = __clz(mod[i]);
            bitCount = i * 32 + (32 - clz);
            break;
        }
    }

    /* Initialize 2 in Montgomery form */
    #pragma unroll
    for (int i = 0; i < 10; i++) redcl[i] = 0;
    redcl[0] = 2;

    /* Convert 2 to Montgomery form */
    for (int i = 0; i < 320; i++) {
        uint32_t carry = 0;
        #pragma unroll
        for (int j = 0; j < 10; j++) {
            uint64_t tmp = ((uint64_t)redcl[j] << 1) | carry;
            redcl[j] = (uint32_t)tmp;
            carry = (uint32_t)(tmp >> 32);
        }
        if (carry || redcl[9] >= mod[9]) {
            int doSub = 1;
            for (int j = 9; j >= 0; j--) {
                if (redcl[j] > mod[j]) { doSub = 1; break; }
                if (redcl[j] < mod[j]) { doSub = 0; break; }
            }
            if (carry || doSub) sub10(redcl, mod);
        }
    }

    /* Build window table */
    #pragma unroll
    for (int i = 0; i < 10; i++) window[1][i] = redcl[i];

    for (int i = 2; i < 32; i++) {
        #pragma unroll
        for (int j = 0; j < 10; j++) window[i][j] = window[i-1][j];
        monMul320(window[i], window[1], mod, invm);
    }

    /* p-1 */
    uint32_t exp[10];
    #pragma unroll
    for (int i = 0; i < 10; i++) exp[i] = mod[i];
    exp[0] &= ~1u;

    /* Initialize result to 1 in Montgomery form */
    #pragma unroll
    for (int i = 0; i < 10; i++) redcl[i] = 0;
    redcl[0] = 1;
    for (int i = 0; i < 320; i++) {
        uint32_t carry = 0;
        #pragma unroll
        for (int j = 0; j < 10; j++) {
            uint64_t tmp = ((uint64_t)redcl[j] << 1) | carry;
            redcl[j] = (uint32_t)tmp;
            carry = (uint32_t)(tmp >> 32);
        }
        if (carry) sub10(redcl, mod);
        int doSub = 0;
        for (int j = 9; j >= 0; j--) {
            if (redcl[j] > mod[j]) { doSub = 1; break; }
            if (redcl[j] < mod[j]) break;
        }
        if (doSub) sub10(redcl, mod);
    }

    /* Main exponentiation loop with 5-bit windows */
    int remaining = bitCount - 1;
    while (remaining > 0) {
        int bits = (remaining >= windowSize) ? windowSize : remaining;
        int start = remaining - bits;

        int wordIdx = start / 32;
        int bitIdx = start % 32;
        uint32_t windowVal;
        if (bitIdx + bits <= 32) {
            windowVal = (exp[wordIdx] >> bitIdx) & ((1u << bits) - 1);
        } else {
            windowVal = (exp[wordIdx] >> bitIdx) | ((exp[wordIdx + 1] << (32 - bitIdx)) & ((1u << bits) - 1));
        }

        for (int i = 0; i < bits; i++) {
            monSqr320(redcl, mod, invm);
        }

        if (windowVal) {
            monMul320(redcl, window[windowVal], mod, invm);
        }

        remaining = start;
    }

    /* Convert back from Montgomery form */
    redcHalf320(redcl, mod, invm);

    /* Check if result is 1 */
    if (redcl[0] != 1) return false;
    #pragma unroll
    for (int i = 1; i < 10; i++) {
        if (redcl[i] != 0) return false;
    }
    return true;
}

/*
 * Fermat test for 352-bit number
 */
__device__ bool fermat352(uint32_t *p) {
    uint32_t mod[11], redcl[11], window[32][11];
    uint32_t invm;
    int bitCount, windowSize = 5;

    #pragma unroll
    for (int i = 0; i < 11; i++) mod[i] = p[i];

    invm = invert_limb(mod[0]);

    bitCount = 352;
    for (int i = 10; i >= 0; i--) {
        if (mod[i]) {
            int clz = __clz(mod[i]);
            bitCount = i * 32 + (32 - clz);
            break;
        }
    }

    /* Initialize 2 in Montgomery form */
    #pragma unroll
    for (int i = 0; i < 11; i++) redcl[i] = 0;
    redcl[0] = 2;

    for (int i = 0; i < 352; i++) {
        uint32_t carry = 0;
        #pragma unroll
        for (int j = 0; j < 11; j++) {
            uint64_t tmp = ((uint64_t)redcl[j] << 1) | carry;
            redcl[j] = (uint32_t)tmp;
            carry = (uint32_t)(tmp >> 32);
        }
        if (carry) sub11(redcl, mod);
        int doSub = 0;
        for (int j = 10; j >= 0; j--) {
            if (redcl[j] > mod[j]) { doSub = 1; break; }
            if (redcl[j] < mod[j]) break;
        }
        if (doSub) sub11(redcl, mod);
    }

    /* Build window table */
    #pragma unroll
    for (int i = 0; i < 11; i++) window[1][i] = redcl[i];

    for (int i = 2; i < 32; i++) {
        #pragma unroll
        for (int j = 0; j < 11; j++) window[i][j] = window[i-1][j];
        monMul352(window[i], window[1], mod, invm);
    }

    /* Initialize result */
    uint32_t exp[11];
    #pragma unroll
    for (int i = 0; i < 11; i++) exp[i] = mod[i];
    exp[0] &= ~1u;

    #pragma unroll
    for (int i = 0; i < 11; i++) redcl[i] = 0;
    redcl[0] = 1;
    for (int i = 0; i < 352; i++) {
        uint32_t carry = 0;
        #pragma unroll
        for (int j = 0; j < 11; j++) {
            uint64_t tmp = ((uint64_t)redcl[j] << 1) | carry;
            redcl[j] = (uint32_t)tmp;
            carry = (uint32_t)(tmp >> 32);
        }
        if (carry) sub11(redcl, mod);
        int doSub = 0;
        for (int j = 10; j >= 0; j--) {
            if (redcl[j] > mod[j]) { doSub = 1; break; }
            if (redcl[j] < mod[j]) break;
        }
        if (doSub) sub11(redcl, mod);
    }

    /* Main exponentiation loop */
    int remaining = bitCount - 1;
    while (remaining > 0) {
        int bits = (remaining >= windowSize) ? windowSize : remaining;
        int start = remaining - bits;

        int wordIdx = start / 32;
        int bitIdx = start % 32;
        uint32_t windowVal;
        if (bitIdx + bits <= 32) {
            windowVal = (exp[wordIdx] >> bitIdx) & ((1u << bits) - 1);
        } else {
            windowVal = (exp[wordIdx] >> bitIdx) | ((exp[wordIdx + 1] << (32 - bitIdx)) & ((1u << bits) - 1));
        }

        for (int i = 0; i < bits; i++) {
            monSqr352(redcl, mod, invm);
        }

        if (windowVal) {
            monMul352(redcl, window[windowVal], mod, invm);
        }

        remaining = start;
    }

    redcHalf352(redcl, mod, invm);

    if (redcl[0] != 1) return false;
    #pragma unroll
    for (int i = 1; i < 11; i++) {
        if (redcl[i] != 0) return false;
    }
    return true;
}

/*
 * CUDA Kernel: Batch Fermat test for 320-bit numbers
 */
__global__ void fermat_kernel_320(uint8_t *results, const uint32_t *primes, uint32_t count) {
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) return;

    uint32_t p[10];
    #pragma unroll
    for (int i = 0; i < 10; i++) {
        p[i] = primes[id * 10 + i];
    }

    results[id] = fermat320(p) ? 1 : 0;
}

/*
 * CUDA Kernel: Batch Fermat test for 352-bit numbers
 */
__global__ void fermat_kernel_352(uint8_t *results, const uint32_t *primes, uint32_t count) {
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) return;

    uint32_t p[11];
    #pragma unroll
    for (int i = 0; i < 11; i++) {
        p[i] = primes[id * 11 + i];
    }

    results[id] = fermat352(p) ? 1 : 0;
}

/*
 * CUDA Kernel: Filter results and continue chain
 */
__global__ void check_fermat(fermat_t *info_out,
                             uint32_t *count_out,
                             fermat_t *info_fin_out,
                             uint32_t *count_fin,
                             const uint8_t *results,
                             const fermat_t *info_in,
                             uint32_t depth,
                             uint32_t num_candidates) {
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= num_candidates) return;

    if (results[id] == 1) {
        fermat_t info = info_in[id];
        info.chainpos++;

        if (info.chainpos < depth) {
            uint32_t idx = atomicAdd(count_out, 1);
            info_out[idx] = info;
        } else {
            uint32_t idx = atomicAdd(count_fin, 1);
            info_fin_out[idx] = info;
        }
    }
}

/* Host-side wrapper functions */
extern "C" {

int cuda_fermat_init(int device_id) {
    cudaError_t err = cudaSetDevice(device_id);
    if (err != cudaSuccess) return -1;

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device_id);

    /* Require compute capability 3.0+ */
    if (prop.major < 3) return -2;

    return 0;
}

int cuda_fermat_batch(uint8_t *h_results,
                      const uint32_t *h_primes,
                      uint32_t count,
                      int bits) {
    if (count == 0) return 0;

    uint8_t *d_results;
    uint32_t *d_primes;

    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = count * limbs * sizeof(uint32_t);
    size_t results_size = count * sizeof(uint8_t);

    cudaMalloc(&d_results, results_size);
    cudaMalloc(&d_primes, primes_size);

    cudaMemcpy(d_primes, h_primes, primes_size, cudaMemcpyHostToDevice);

    int blocks = (count + BLOCK_SIZE - 1) / BLOCK_SIZE;

    if (bits <= 320) {
        fermat_kernel_320<<<blocks, BLOCK_SIZE>>>(d_results, d_primes, count);
    } else {
        fermat_kernel_352<<<blocks, BLOCK_SIZE>>>(d_results, d_primes, count);
    }

    cudaMemcpy(h_results, d_results, results_size, cudaMemcpyDeviceToHost);

    cudaFree(d_results);
    cudaFree(d_primes);

    return 0;
}

int cuda_get_device_count(void) {
    int count;
    cudaGetDeviceCount(&count);
    return count;
}

const char* cuda_get_device_name(int device_id) {
    static cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device_id);
    return prop.name;
}

size_t cuda_get_device_memory(int device_id) {
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device_id);
    return prop.totalGlobalMem;
}

int cuda_get_sm_count(int device_id) {
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device_id);
    return prop.multiProcessorCount;
}

} /* extern "C" */
