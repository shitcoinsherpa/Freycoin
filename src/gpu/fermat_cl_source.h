/**
 * Embedded OpenCL Kernel Source
 *
 * Copyright (c) 2025 The Freycoin developers
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * This file is auto-generated. Do not edit manually.
 * If you need to modify the kernel, edit fermat.cl and regenerate this file.
 *
 * To regenerate: xxd -i fermat.cl > fermat_cl_source.h
 * (then manually wrap as a string literal)
 */

R"OPENCL_SOURCE(
/**
 * OpenCL Fermat Primality Test Kernel for Freycoin
 *
 * Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
 * Copyright (c) 2025 The Freycoin developers
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Montgomery multiplication uses the CIOS (Coarsely Integrated Operand
 * Scanning) algorithm from:
 *   C.K. Koc, T. Acar, B.S. Kaliski, "Analyzing and Comparing Montgomery
 *   Multiplication Algorithms", IEEE Micro, 16(3):26-33, June 1996.
 */

/* ========================================================================
 * Montgomery inverse lookup table
 * ======================================================================== */
__constant uint binvert_limb_table[128] = {
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

/** Compute -n^{-1} mod 2^32 using table lookup + Newton's method */
inline uint invert_limb(uint limb) {
    uint inv = binvert_limb_table[(limb >> 1) & 0x7F];
    inv = 2 * inv - inv * inv * limb;
    inv = 2 * inv - inv * inv * limb;
    return -inv;
}

/* ========================================================================
 * CIOS Montgomery Multiplication — 10 limbs (320-bit)
 *
 * Computes: result = a * b * R^{-1} mod n, where R = 2^320
 * Requires: 0 <= a, b < n, n odd, n < R
 * ======================================================================== */
void monPro10(__private uint *result,
              __private const uint *a,
              __private const uint *b,
              __private const uint *n,
              uint n0inv) {
    /* CIOS requires N+2 words for the accumulator (Koç et al., §2.2) */
    uint t[12];
    for (int k = 0; k < 12; k++) t[k] = 0;

    for (int i = 0; i < 10; i++) {
        uint bi = b[i];

        /* t += a * b[i] */
        ulong carry = 0;
        for (int j = 0; j < 10; j++) {
            ulong prod = (ulong)a[j] * bi + (ulong)t[j] + carry;
            t[j] = (uint)prod;
            carry = prod >> 32;
        }
        ulong s = (ulong)t[10] + carry;
        t[10] = (uint)s;
        t[11] = (uint)(s >> 32);

        /* m = t[0] * n0inv mod 2^32 */
        uint m = t[0] * n0inv;

        /* t = (t + m * n) >> 32 */
        ulong mn = (ulong)m * n[0] + (ulong)t[0];
        carry = mn >> 32;
        for (int j = 1; j < 10; j++) {
            mn = (ulong)m * n[j] + (ulong)t[j] + carry;
            t[j - 1] = (uint)mn;
            carry = mn >> 32;
        }
        s = (ulong)t[10] + carry;
        t[9] = (uint)s;
        t[10] = t[11] + (uint)(s >> 32);
    }

    /* Final: if t >= n, subtract n */
    int gt = (t[10] != 0);
    if (!gt) {
        for (int j = 9; j >= 0; j--) {
            if (t[j] > n[j]) { gt = 1; break; }
            if (t[j] < n[j]) break;
        }
    }
    if (gt) {
        ulong borrow = 0;
        for (int j = 0; j < 10; j++) {
            ulong diff = (ulong)t[j] - (ulong)n[j] - borrow;
            result[j] = (uint)diff;
            borrow = (diff >> 63) & 1;
        }
    } else {
        for (int j = 0; j < 10; j++) result[j] = t[j];
    }
}

/* ========================================================================
 * Double-and-reduce: x = 2*x mod n  (10 limbs)
 * ======================================================================== */
inline void dblMod10(__private uint *x, __private const uint *n) {
    uint carry = 0;
    for (int j = 0; j < 10; j++) {
        ulong tmp = ((ulong)x[j] << 1) | carry;
        x[j] = (uint)tmp;
        carry = (uint)(tmp >> 32);
    }
    int need_sub = carry;
    if (!need_sub) {
        for (int j = 9; j >= 0; j--) {
            if (x[j] > n[j]) { need_sub = 1; break; }
            if (x[j] < n[j]) break;
        }
    }
    if (need_sub) {
        ulong borrow = 0;
        for (int j = 0; j < 10; j++) {
            ulong diff = (ulong)x[j] - (ulong)n[j] - borrow;
            x[j] = (uint)diff;
            borrow = (diff >> 63) & 1;
        }
    }
}

/* ========================================================================
 * Fermat test for 320-bit number: 2^(p-1) = 1 (mod p)?
 * ======================================================================== */
bool fermat320(__private uint *p) {
    uint n[10], mont1[10], base[10], result[10], temp[10];

    for (int i = 0; i < 10; i++) n[i] = p[i];
    uint n0inv = invert_limb(n[0]);

    /* Find bit count */
    int bitCount = 0;
    for (int i = 9; i >= 0; i--) {
        if (n[i]) {
            uint x = n[i];
            int pos = 0;
            if (x >= 0x10000u) { pos += 16; x >>= 16; }
            if (x >= 0x100u)   { pos += 8;  x >>= 8; }
            if (x >= 0x10u)    { pos += 4;  x >>= 4; }
            if (x >= 0x4u)     { pos += 2;  x >>= 2; }
            if (x >= 0x2u)     { pos += 1; }
            bitCount = i * 32 + pos + 1;
            break;
        }
    }
    if (bitCount <= 1) return false;

    /* Compute R mod n = mont(1) via 320 doublings of 1 */
    for (int i = 0; i < 10; i++) mont1[i] = 0;
    mont1[0] = 1;
    for (int iter = 0; iter < 320; iter++)
        dblMod10(mont1, n);

    /* Compute 2R mod n = mont(2) via one more doubling */
    for (int i = 0; i < 10; i++) base[i] = mont1[i];
    dblMod10(base, n);

    /* Exponent = p - 1 */
    uint exp[10];
    for (int i = 0; i < 10; i++) exp[i] = n[i];
    exp[0] &= ~1u;

    /* Square-and-multiply: result = base^exp in Montgomery domain */
    for (int i = 0; i < 10; i++) result[i] = mont1[i];
    for (int bit = bitCount - 1; bit >= 0; bit--) {
        monPro10(temp, result, result, n, n0inv);
        for (int i = 0; i < 10; i++) result[i] = temp[i];

        if ((exp[bit / 32] >> (bit % 32)) & 1) {
            monPro10(temp, result, base, n, n0inv);
            for (int i = 0; i < 10; i++) result[i] = temp[i];
        }
    }

    /* Convert back from Montgomery form */
    for (int i = 0; i < 10; i++) temp[i] = 0;
    temp[0] = 1;
    monPro10(result, result, temp, n, n0inv);

    /* Check result == 1 */
    if (result[0] != 1) return false;
    for (int i = 1; i < 10; i++)
        if (result[i] != 0) return false;
    return true;
}

/** Kernel: batch Fermat test for 320-bit numbers */
__kernel void fermat_kernel_320(__global uchar *results,
                                 __global const uint *primes,
                                 uint count) {
    uint id = get_global_id(0);
    if (id >= count) return;

    uint p[10];
    for (int i = 0; i < 10; i++)
        p[i] = primes[id * 10 + i];

    results[id] = fermat320(p) ? 1 : 0;
}

/* ========================================================================
 * CIOS Montgomery Multiplication — 11 limbs (352-bit)
 * ======================================================================== */
void monPro11(__private uint *result,
              __private const uint *a,
              __private const uint *b,
              __private const uint *n,
              uint n0inv) {
    /* CIOS requires N+2 words for the accumulator (Koç et al., §2.2) */
    uint t[13];
    for (int k = 0; k < 13; k++) t[k] = 0;

    for (int i = 0; i < 11; i++) {
        uint bi = b[i];

        ulong carry = 0;
        for (int j = 0; j < 11; j++) {
            ulong prod = (ulong)a[j] * bi + (ulong)t[j] + carry;
            t[j] = (uint)prod;
            carry = prod >> 32;
        }
        ulong s = (ulong)t[11] + carry;
        t[11] = (uint)s;
        t[12] = (uint)(s >> 32);

        uint m = t[0] * n0inv;

        ulong mn = (ulong)m * n[0] + (ulong)t[0];
        carry = mn >> 32;
        for (int j = 1; j < 11; j++) {
            mn = (ulong)m * n[j] + (ulong)t[j] + carry;
            t[j - 1] = (uint)mn;
            carry = mn >> 32;
        }
        s = (ulong)t[11] + carry;
        t[10] = (uint)s;
        t[11] = t[12] + (uint)(s >> 32);
    }

    int gt = (t[11] != 0);
    if (!gt) {
        for (int j = 10; j >= 0; j--) {
            if (t[j] > n[j]) { gt = 1; break; }
            if (t[j] < n[j]) break;
        }
    }
    if (gt) {
        ulong borrow = 0;
        for (int j = 0; j < 11; j++) {
            ulong diff = (ulong)t[j] - (ulong)n[j] - borrow;
            result[j] = (uint)diff;
            borrow = (diff >> 63) & 1;
        }
    } else {
        for (int j = 0; j < 11; j++) result[j] = t[j];
    }
}

inline void dblMod11(__private uint *x, __private const uint *n) {
    uint carry = 0;
    for (int j = 0; j < 11; j++) {
        ulong tmp = ((ulong)x[j] << 1) | carry;
        x[j] = (uint)tmp;
        carry = (uint)(tmp >> 32);
    }
    int need_sub = carry;
    if (!need_sub) {
        for (int j = 10; j >= 0; j--) {
            if (x[j] > n[j]) { need_sub = 1; break; }
            if (x[j] < n[j]) break;
        }
    }
    if (need_sub) {
        ulong borrow = 0;
        for (int j = 0; j < 11; j++) {
            ulong diff = (ulong)x[j] - (ulong)n[j] - borrow;
            x[j] = (uint)diff;
            borrow = (diff >> 63) & 1;
        }
    }
}

bool fermat352(__private uint *p) {
    uint n[11], mont1[11], base[11], result[11], temp[11];

    for (int i = 0; i < 11; i++) n[i] = p[i];
    uint n0inv = invert_limb(n[0]);

    int bitCount = 0;
    for (int i = 10; i >= 0; i--) {
        if (n[i]) {
            uint x = n[i];
            int pos = 0;
            if (x >= 0x10000u) { pos += 16; x >>= 16; }
            if (x >= 0x100u)   { pos += 8;  x >>= 8; }
            if (x >= 0x10u)    { pos += 4;  x >>= 4; }
            if (x >= 0x4u)     { pos += 2;  x >>= 2; }
            if (x >= 0x2u)     { pos += 1; }
            bitCount = i * 32 + pos + 1;
            break;
        }
    }
    if (bitCount <= 1) return false;

    for (int i = 0; i < 11; i++) mont1[i] = 0;
    mont1[0] = 1;
    for (int iter = 0; iter < 352; iter++)
        dblMod11(mont1, n);

    for (int i = 0; i < 11; i++) base[i] = mont1[i];
    dblMod11(base, n);

    uint exp[11];
    for (int i = 0; i < 11; i++) exp[i] = n[i];
    exp[0] &= ~1u;

    for (int i = 0; i < 11; i++) result[i] = mont1[i];
    for (int bit = bitCount - 1; bit >= 0; bit--) {
        monPro11(temp, result, result, n, n0inv);
        for (int i = 0; i < 11; i++) result[i] = temp[i];

        if ((exp[bit / 32] >> (bit % 32)) & 1) {
            monPro11(temp, result, base, n, n0inv);
            for (int i = 0; i < 11; i++) result[i] = temp[i];
        }
    }

    for (int i = 0; i < 11; i++) temp[i] = 0;
    temp[0] = 1;
    monPro11(result, result, temp, n, n0inv);

    if (result[0] != 1) return false;
    for (int i = 1; i < 11; i++)
        if (result[i] != 0) return false;
    return true;
}

/** Kernel: batch Fermat test for 352-bit numbers */
__kernel void fermat_kernel_352(__global uchar *results,
                                 __global const uint *primes,
                                 uint count) {
    uint id = get_global_id(0);
    if (id >= count) return;

    uint p[11];
    for (int i = 0; i < 11; i++)
        p[i] = primes[id * 11 + i];

    results[id] = fermat352(p) ? 1 : 0;
}

/* =========================================================================
 * Cooperative Fermat Primality Kernel (for post-fork large numbers)
 *
 * 32 threads cooperate on one candidate via local memory + barriers.
 * Uses correct CIOS Montgomery multiplication.
 * ========================================================================= */

#define COOP_TPI 32
#define COOP_MAX_LPT 17

inline uint coop_bcast(__local uint *comm, uint lane, uint src, uint val) {
    if (lane == src) comm[src] = val;
    barrier(CLK_LOCAL_MEM_FENCE);
    uint r = comm[src];
    barrier(CLK_LOCAL_MEM_FENCE);
    return r;
}

inline void coop_carry(__local uint *scr, uint lane,
                       uint *c, int lpt, uint carry_out) {
    scr[lane] = carry_out;
    barrier(CLK_LOCAL_MEM_FENCE);
    uint inc = (lane > 0) ? scr[lane - 1] : 0;
    barrier(CLK_LOCAL_MEM_FENCE);

    uint ripple = inc;
    for (int j = 0; j < lpt && ripple; j++) {
        ulong s = (ulong)c[j] + ripple;
        c[j] = (uint)s;
        ripple = (uint)(s >> 32);
    }

    scr[lane] = ripple;
    barrier(CLK_LOCAL_MEM_FENCE);
    inc = (lane > 0) ? scr[lane - 1] : 0;
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int j = 0; j < lpt && inc; j++) {
        ulong s = (ulong)c[j] + inc;
        c[j] = (uint)s;
        inc = (uint)(s >> 32);
    }
}

inline int coop_cmp(const uint *a, const uint *b,
                    __local uint *comm, uint lane, int lpt) {
    int local_cmp = 0;
    for (int j = lpt - 1; j >= 0; j--) {
        if (a[j] > b[j]) { local_cmp = 1; break; }
        if (a[j] < b[j]) { local_cmp = -1; break; }
    }
    comm[lane] = (uint)local_cmp;
    barrier(CLK_LOCAL_MEM_FENCE);

    int result = 0;
    if (lane == 0) {
        for (int t = COOP_TPI - 1; t >= 0; t--) {
            if ((int)comm[t] != 0) { result = (int)comm[t]; break; }
        }
        comm[0] = (uint)result;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    result = (int)comm[0];
    barrier(CLK_LOCAL_MEM_FENCE);
    return result;
}

inline void coop_sub(uint *c, const uint *n,
                     __local uint *scr, uint lane, int lpt) {
    uint borrow = 0;
    for (int j = 0; j < lpt; j++) {
        ulong diff = (ulong)c[j] - n[j] - borrow;
        c[j] = (uint)diff;
        borrow = (diff >> 32) != 0 ? 1 : 0;
    }

    scr[lane] = borrow;
    barrier(CLK_LOCAL_MEM_FENCE);
    uint inc = (lane > 0) ? scr[lane - 1] : 0;
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int j = 0; j < lpt && inc; j++) {
        if (c[j] >= inc) { c[j] -= inc; inc = 0; }
        else { c[j] -= inc; inc = 1; }
    }

    scr[lane] = inc;
    barrier(CLK_LOCAL_MEM_FENCE);
    inc = (lane > 0) ? scr[lane - 1] : 0;
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int j = 0; j < lpt && inc; j++) {
        if (c[j] >= inc) { c[j] -= inc; inc = 0; }
        else { c[j] -= inc; inc = 1; }
    }
}

inline void coop_cond_sub(uint *a, const uint *n,
                          __local uint *comm, __local uint *scr,
                          uint lane, int lpt) {
    int cmp = coop_cmp(a, n, comm, lane, lpt);
    if (cmp >= 0)
        coop_sub(a, n, scr, lane, lpt);
}

inline void coop_dbl_mod(uint *a, const uint *n,
                         __local uint *comm, __local uint *scr,
                         uint lane, int lpt) {
    uint msb = a[lpt - 1] >> 31;
    for (int j = lpt - 1; j > 0; j--)
        a[j] = (a[j] << 1) | (a[j - 1] >> 31);
    a[0] <<= 1;

    scr[lane] = msb;
    barrier(CLK_LOCAL_MEM_FENCE);
    uint carry_in = (lane > 0) ? scr[lane - 1] : 0;
    uint overflow = scr[COOP_TPI - 1];
    barrier(CLK_LOCAL_MEM_FENCE);

    a[0] |= carry_in;

    if (overflow) {
        coop_sub(a, n, scr, lane, lpt);
    } else {
        coop_cond_sub(a, n, comm, scr, lane, lpt);
    }
}

inline void coop_mont_mul(uint *c, const uint *a, const uint *b,
                          const uint *n, uint n0inv,
                          __local uint *comm, __local uint *scr,
                          uint lane, int lpt) {
    for (int j = 0; j < COOP_MAX_LPT; j++) c[j] = 0;

    uint padded = COOP_TPI * (uint)lpt;

    for (uint i = 0; i < padded; i++) {
        uint owner = i / (uint)lpt;
        uint li = i % (uint)lpt;
        uint b_i = coop_bcast(comm, lane, owner,
                              (lane == owner) ? b[li] : 0);

        ulong carry = 0;
        for (int j = 0; j < lpt; j++) {
            ulong prod = (ulong)a[j] * b_i + c[j] + carry;
            c[j] = (uint)prod;
            carry = prod >> 32;
        }
        coop_carry(scr, lane, c, lpt, (uint)carry);

        uint m = coop_bcast(comm, lane, 0,
                            (lane == 0) ? (c[0] * n0inv) : 0);

        carry = 0;
        for (int j = 0; j < lpt; j++) {
            ulong prod = (ulong)n[j] * m + c[j] + carry;
            c[j] = (uint)prod;
            carry = prod >> 32;
        }
        coop_carry(scr, lane, c, lpt, (uint)carry);

        scr[lane] = c[0];
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int j = 0; j < lpt - 1; j++)
            c[j] = c[j + 1];
        c[lpt - 1] = (lane < COOP_TPI - 1) ? scr[lane + 1] : 0;
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    coop_cond_sub(c, n, comm, scr, lane, lpt);
}

__kernel void fermat_kernel_coop(__global uchar *results,
                                 __global const uint *primes,
                                 uint count,
                                 uint limbs) {
    uint lane = get_local_id(0);
    uint inst = get_group_id(0);
    if (inst >= count) return;

    __local uint comm[COOP_TPI];
    __local uint scr[COOP_TPI];

    int lpt = ((int)limbs + COOP_TPI - 1) / COOP_TPI;

    uint n[COOP_MAX_LPT];
    for (int j = 0; j < COOP_MAX_LPT; j++) {
        uint idx = lane * (uint)lpt + (uint)j;
        n[j] = (j < lpt && idx < limbs)
             ? primes[(ulong)inst * limbs + idx] : 0;
    }

    uint n0 = coop_bcast(comm, lane, 0, (lane == 0) ? n[0] : 0);
    uint n0inv = invert_limb(n0);

    uint result[COOP_MAX_LPT];
    for (int j = 0; j < COOP_MAX_LPT; j++) result[j] = 0;
    if (lane == 0) result[0] = 1;

    uint padded = COOP_TPI * (uint)lpt;
    for (uint bit = 0; bit < padded * 32; bit++)
        coop_dbl_mod(result, n, comm, scr, lane, lpt);

    uint base[COOP_MAX_LPT];
    for (int j = 0; j < COOP_MAX_LPT; j++) base[j] = result[j];
    coop_dbl_mod(base, n, comm, scr, lane, lpt);

    uint temp[COOP_MAX_LPT];

    for (int li = (int)limbs - 1; li >= 0; li--) {
        uint owner = (uint)li / (uint)lpt;
        uint local_idx = (uint)li % (uint)lpt;
        uint exp_limb = coop_bcast(comm, lane, owner,
                                   (lane == owner) ? n[local_idx] : 0);

        if (li == 0) exp_limb &= ~1u;

        for (int bit = 31; bit >= 0; bit--) {
            coop_mont_mul(temp, result, result, n, n0inv, comm, scr, lane, lpt);
            for (int j = 0; j < lpt; j++) result[j] = temp[j];

            if ((exp_limb >> bit) & 1) {
                coop_mont_mul(temp, result, base, n, n0inv, comm, scr, lane, lpt);
                for (int j = 0; j < lpt; j++) result[j] = temp[j];
            }
        }
    }

    uint one[COOP_MAX_LPT];
    for (int j = 0; j < COOP_MAX_LPT; j++) one[j] = 0;
    if (lane == 0) one[0] = 1;

    coop_mont_mul(temp, result, one, n, n0inv, comm, scr, lane, lpt);
    for (int j = 0; j < lpt; j++) result[j] = temp[j];

    int is_one = 1;
    if (lane == 0) {
        if (result[0] != 1) is_one = 0;
        for (int j = 1; j < lpt; j++)
            if (result[j] != 0) is_one = 0;
    } else {
        for (int j = 0; j < lpt; j++)
            if (result[j] != 0) is_one = 0;
    }

    comm[lane] = (uint)is_one;
    barrier(CLK_LOCAL_MEM_FENCE);

    if (lane == 0) {
        uint pass = 1;
        for (int t = 0; t < COOP_TPI; t++) {
            if (comm[t] == 0) { pass = 0; break; }
        }
        results[inst] = pass ? 1 : 0;
    }
}
)OPENCL_SOURCE"
