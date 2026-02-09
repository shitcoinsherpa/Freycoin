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
 * Copyright (c) 2025 The Freycoin developers
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

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

inline uint invert_limb(uint limb) {
    uint inv = binvert_limb_table[(limb >> 1) & 0x7F];
    inv = 2 * inv - inv * inv * limb;
    inv = 2 * inv - inv * inv * limb;
    return -inv;
}

inline void sub10(uint *a, __private uint *b) {
    uint A0 = a[0]; a[0] -= b[0];
    for (int i = 1; i < 10; i++) { uint A1 = a[i]; a[i] -= b[i] + (a[i-1] > A0 ? 1 : 0); A0 = A1; }
}

inline void sub11(uint *a, __private uint *b) {
    uint A0 = a[0]; a[0] -= b[0];
    for (int i = 1; i < 11; i++) { uint A1 = a[i]; a[i] -= b[i] + (a[i-1] > A0 ? 1 : 0); A0 = A1; }
}

void monSqr320(__private uint *op, __private uint *mod, uint invm);
void monMul320(__private uint *op1, __private uint *op2, __private uint *mod, uint invm);
void redcHalf320(__private uint *op, __private uint *mod, uint invm);
void monSqr352(__private uint *op, __private uint *mod, uint invm);
void monMul352(__private uint *op1, __private uint *op2, __private uint *mod, uint invm);
void redcHalf352(__private uint *op, __private uint *mod, uint invm);

inline int count_leading_zeros(uint x) {
    int n = 0;
    if (x == 0) return 32;
    if ((x & 0xFFFF0000) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000) == 0) { n += 8; x <<= 8; }
    if ((x & 0xF0000000) == 0) { n += 4; x <<= 4; }
    if ((x & 0xC0000000) == 0) { n += 2; x <<= 2; }
    if ((x & 0x80000000) == 0) { n += 1; }
    return n;
}

bool fermat320(__private uint *p) {
    uint mod[10], redcl[10], window[16][10];
    uint invm;
    int bitCount, windowSize = 4;
    for (int i = 0; i < 10; i++) mod[i] = p[i];
    invm = invert_limb(mod[0]);
    bitCount = 320;
    for (int i = 9; i >= 0; i--) { if (mod[i]) { bitCount = i * 32 + (32 - count_leading_zeros(mod[i])); break; } }
    for (int i = 0; i < 10; i++) redcl[i] = 0;
    redcl[0] = 2;
    for (int i = 0; i < 320; i++) {
        uint carry = 0;
        for (int j = 0; j < 10; j++) { ulong tmp = ((ulong)redcl[j] << 1) | carry; redcl[j] = (uint)tmp; carry = (uint)(tmp >> 32); }
        if (carry || redcl[9] >= mod[9]) { int doSub = 1; for (int j = 9; j >= 0; j--) { if (redcl[j] > mod[j]) { doSub = 1; break; } if (redcl[j] < mod[j]) { doSub = 0; break; } } if (carry || doSub) sub10(redcl, mod); }
    }
    for (int i = 0; i < 10; i++) window[1][i] = redcl[i];
    for (int i = 2; i < 16; i++) { for (int j = 0; j < 10; j++) window[i][j] = window[i-1][j]; monMul320(window[i], window[1], mod, invm); }
    uint exp[10]; for (int i = 0; i < 10; i++) exp[i] = mod[i]; exp[0] &= ~1u;
    for (int i = 0; i < 10; i++) redcl[i] = 0; redcl[0] = 1;
    for (int i = 0; i < 320; i++) { uint carry = 0; for (int j = 0; j < 10; j++) { ulong tmp = ((ulong)redcl[j] << 1) | carry; redcl[j] = (uint)tmp; carry = (uint)(tmp >> 32); } if (carry) sub10(redcl, mod); int doSub = 0; for (int j = 9; j >= 0; j--) { if (redcl[j] > mod[j]) { doSub = 1; break; } if (redcl[j] < mod[j]) break; } if (doSub) sub10(redcl, mod); }
    int remaining = bitCount - 1;
    while (remaining > 0) {
        int bits = (remaining >= windowSize) ? windowSize : remaining; int start = remaining - bits;
        int wordIdx = start / 32; int bitIdx = start % 32; uint windowVal;
        if (bitIdx + bits <= 32) windowVal = (exp[wordIdx] >> bitIdx) & ((1u << bits) - 1);
        else windowVal = (exp[wordIdx] >> bitIdx) | ((exp[wordIdx + 1] << (32 - bitIdx)) & ((1u << bits) - 1));
        for (int i = 0; i < bits; i++) monSqr320(redcl, mod, invm);
        if (windowVal) monMul320(redcl, window[windowVal], mod, invm);
        remaining = start;
    }
    redcHalf320(redcl, mod, invm);
    if (redcl[0] != 1) return false;
    for (int i = 1; i < 10; i++) if (redcl[i] != 0) return false;
    return true;
}

bool fermat352(__private uint *p) {
    uint mod[11], redcl[11], window[16][11];
    uint invm;
    int bitCount, windowSize = 4;
    for (int i = 0; i < 11; i++) mod[i] = p[i];
    invm = invert_limb(mod[0]);
    bitCount = 352;
    for (int i = 10; i >= 0; i--) { if (mod[i]) { bitCount = i * 32 + (32 - count_leading_zeros(mod[i])); break; } }
    for (int i = 0; i < 11; i++) redcl[i] = 0;
    redcl[0] = 2;
    for (int i = 0; i < 352; i++) {
        uint carry = 0;
        for (int j = 0; j < 11; j++) { ulong tmp = ((ulong)redcl[j] << 1) | carry; redcl[j] = (uint)tmp; carry = (uint)(tmp >> 32); }
        if (carry) sub11(redcl, mod);
        int doSub = 0;
        for (int j = 10; j >= 0; j--) { if (redcl[j] > mod[j]) { doSub = 1; break; } if (redcl[j] < mod[j]) break; }
        if (doSub) sub11(redcl, mod);
    }
    for (int i = 0; i < 11; i++) window[1][i] = redcl[i];
    for (int i = 2; i < 16; i++) { for (int j = 0; j < 11; j++) window[i][j] = window[i-1][j]; monMul352(window[i], window[1], mod, invm); }
    uint exp[11]; for (int i = 0; i < 11; i++) exp[i] = mod[i]; exp[0] &= ~1u;
    for (int i = 0; i < 11; i++) redcl[i] = 0; redcl[0] = 1;
    for (int i = 0; i < 352; i++) { uint carry = 0; for (int j = 0; j < 11; j++) { ulong tmp = ((ulong)redcl[j] << 1) | carry; redcl[j] = (uint)tmp; carry = (uint)(tmp >> 32); } if (carry) sub11(redcl, mod); int doSub = 0; for (int j = 10; j >= 0; j--) { if (redcl[j] > mod[j]) { doSub = 1; break; } if (redcl[j] < mod[j]) break; } if (doSub) sub11(redcl, mod); }
    int remaining = bitCount - 1;
    while (remaining > 0) {
        int bits = (remaining >= windowSize) ? windowSize : remaining; int start = remaining - bits;
        int wordIdx = start / 32; int bitIdx = start % 32; uint windowVal;
        if (bitIdx + bits <= 32) windowVal = (exp[wordIdx] >> bitIdx) & ((1u << bits) - 1);
        else windowVal = (exp[wordIdx] >> bitIdx) | ((exp[wordIdx + 1] << (32 - bitIdx)) & ((1u << bits) - 1));
        for (int i = 0; i < bits; i++) monSqr352(redcl, mod, invm);
        if (windowVal) monMul352(redcl, window[windowVal], mod, invm);
        remaining = start;
    }
    redcHalf352(redcl, mod, invm);
    if (redcl[0] != 1) return false;
    for (int i = 1; i < 11; i++) if (redcl[i] != 0) return false;
    return true;
}

__kernel void fermat_kernel_320(__global uchar *results, __global const uint *primes, uint count) {
    uint id = get_global_id(0);
    if (id >= count) return;
    uint p[10]; for (int i = 0; i < 10; i++) p[i] = primes[id * 10 + i];
    results[id] = fermat320(p) ? 1 : 0;
}

__kernel void fermat_kernel_352(__global uchar *results, __global const uint *primes, uint count) {
    uint id = get_global_id(0);
    if (id >= count) return;
    uint p[11]; for (int i = 0; i < 11; i++) p[i] = primes[id * 11 + i];
    results[id] = fermat352(p) ? 1 : 0;
}

void monSqr320(__private uint *op, __private uint *mod, uint invm) {
    uint invValue[10]; ulong accLow = 0, accHi = 0;
    for (int i = 0; i < 10; i++) {
        ulong prod = (ulong)op[i] * op[i]; accLow += (uint)prod; accHi += (uint)(prod >> 32);
        for (int j = 0; j < i; j++) { prod = (ulong)op[j] * op[i]; accLow += (uint)prod; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(prod >> 32); }
        for (int j = 0; j < i; j++) { prod = (ulong)invValue[j] * mod[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        invValue[i] = invm * (uint)accLow; prod = (ulong)invValue[i] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32);
        accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) { int k = 10 + i - j; if (k >= 0 && k < 10 && k > j) { ulong prod = (ulong)op[j] * op[k]; accLow += (uint)prod; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(prod >> 32); } }
        int diag = (10 + i) / 2; if ((10 + i) % 2 == 0 && diag < 10) { ulong prod = (ulong)op[diag] * op[diag]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = i + 1; j < 10; j++) { ulong prod = (ulong)invValue[j] * mod[10 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        op[i] = (uint)accLow; accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    op[9] = (uint)accLow; if ((uint)(accLow >> 32)) sub10(op, mod);
}

void monMul320(__private uint *op1, __private uint *op2, __private uint *mod, uint invm) {
    uint invValue[10]; ulong accLow = 0, accHi = 0;
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j <= i; j++) { ulong prod = (ulong)op1[j] * op2[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = 0; j < i; j++) { ulong prod = (ulong)invValue[j] * mod[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        invValue[i] = invm * (uint)accLow; ulong prod = (ulong)invValue[i] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32);
        accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) { ulong prod = (ulong)op1[j] * op2[10 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = i + 1; j < 10; j++) { ulong prod = (ulong)invValue[j] * mod[10 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        op1[i] = (uint)accLow; accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    op1[9] = (uint)accLow; if ((uint)(accLow >> 32)) sub10(op1, mod);
}

void redcHalf320(__private uint *op, __private uint *mod, uint invm) {
    uint invValue[10]; ulong accLow = op[0], accHi = op[1];
    invValue[0] = invm * (uint)accLow; ulong prod = (ulong)invValue[0] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(accLow >> 32); accLow = accHi; accHi = op[2];
    for (int i = 1; i < 9; i++) { for (int j = 0; j < i; j++) { prod = (ulong)invValue[j] * mod[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); } invValue[i] = invm * (uint)accLow; prod = (ulong)invValue[i] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(accLow >> 32); accLow = accHi; accHi = op[i+2]; }
    for (int j = 0; j < 9; j++) { prod = (ulong)invValue[j] * mod[9-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); } invValue[9] = invm * (uint)accLow; prod = (ulong)invValue[9] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    for (int i = 0; i < 9; i++) { for (int j = i + 1; j < 10; j++) { prod = (ulong)invValue[j] * mod[10 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); } op[i] = (uint)accLow; accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0; }
    op[9] = (uint)accLow; if ((uint)(accLow >> 32)) sub10(op, mod);
}

void monSqr352(__private uint *op, __private uint *mod, uint invm) {
    uint invValue[11]; ulong accLow = 0, accHi = 0;
    for (int i = 0; i <= 10; i++) {
        if (i <= 10) { ulong prod = (ulong)op[i] * op[i]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = 0; j < (i+1)/2; j++) { int k = i - j; if (k <= 10 && k != j) { ulong prod = (ulong)op[j] * op[k]; accLow += (uint)prod; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(prod >> 32); } }
        for (int j = 0; j < i; j++) { ulong prod = (ulong)invValue[j] * mod[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        if (i <= 10) { invValue[i] = invm * (uint)accLow; ulong prod = (ulong)invValue[i] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j <= 10; j++) { int k = 11 + i - j; if (k >= 0 && k <= 10 && k > j) { ulong prod = (ulong)op[j] * op[k]; accLow += (uint)prod; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(prod >> 32); } }
        int diag = (11 + i) / 2; if ((11 + i) % 2 == 0 && diag <= 10) { ulong prod = (ulong)op[diag] * op[diag]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = i + 1; j <= 10; j++) { ulong prod = (ulong)invValue[j] * mod[11 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        op[i] = (uint)accLow; accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    op[10] = (uint)accLow; if ((uint)(accLow >> 32)) sub11(op, mod);
}

void monMul352(__private uint *op1, __private uint *op2, __private uint *mod, uint invm) {
    uint invValue[11]; ulong accLow = 0, accHi = 0;
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= i; j++) { ulong prod = (ulong)op1[j] * op2[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = 0; j < i; j++) { ulong prod = (ulong)invValue[j] * mod[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        invValue[i] = invm * (uint)accLow; ulong prod = (ulong)invValue[i] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32);
        accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j <= 10; j++) { ulong prod = (ulong)op1[j] * op2[11 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        for (int j = i + 1; j <= 10; j++) { ulong prod = (ulong)invValue[j] * mod[11 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); }
        op1[i] = (uint)accLow; accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    }
    op1[10] = (uint)accLow; if ((uint)(accLow >> 32)) sub11(op1, mod);
}

void redcHalf352(__private uint *op, __private uint *mod, uint invm) {
    uint invValue[11]; ulong accLow = op[0], accHi = op[1];
    invValue[0] = invm * (uint)accLow; ulong prod = (ulong)invValue[0] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(accLow >> 32); accLow = accHi; accHi = op[2];
    for (int i = 1; i < 10; i++) { for (int j = 0; j < i; j++) { prod = (ulong)invValue[j] * mod[i-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); } invValue[i] = invm * (uint)accLow; prod = (ulong)invValue[i] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(accLow >> 32); accLow = accHi; accHi = op[i+2]; }
    for (int j = 0; j < 10; j++) { prod = (ulong)invValue[j] * mod[10-j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); } invValue[10] = invm * (uint)accLow; prod = (ulong)invValue[10] * mod[0]; accLow += (uint)prod; accHi += (uint)(prod >> 32); accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0;
    for (int i = 0; i < 10; i++) { for (int j = i + 1; j <= 10; j++) { prod = (ulong)invValue[j] * mod[11 + i - j]; accLow += (uint)prod; accHi += (uint)(prod >> 32); } op[i] = (uint)accLow; accHi += (uint)(accLow >> 32); accLow = accHi; accHi = 0; }
    op[10] = (uint)accLow; if ((uint)(accLow >> 32)) sub11(op, mod);
}
)OPENCL_SOURCE"
