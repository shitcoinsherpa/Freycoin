/**
 * Minimal device-only Fermat kernel for PTX generation.
 * Only device code — no host wrappers, no cuda_runtime.h dependency.
 * Compile: nvcc -ptx -arch=sm_50 fermat_device_only.cu -o fermat.ptx
 */

#include <stdint.h>

/* Montgomery inverse lookup table */
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

__device__ __forceinline__ uint32_t invert_limb(uint32_t limb) {
    uint32_t inv = binvert_limb_table[(limb >> 1) & 0x7F];
    inv = 2 * inv - inv * inv * limb;
    inv = 2 * inv - inv * inv * limb;
    return -inv;
}

/* CIOS Montgomery Multiplication — 10 limbs (320-bit) */
__device__ void monPro10(uint32_t *result, const uint32_t *a, const uint32_t *b,
                         const uint32_t *n, uint32_t n0inv) {
    uint32_t t[11];
    #pragma unroll
    for (int k = 0; k <= 10; k++) t[k] = 0;

    for (int i = 0; i < 10; i++) {
        uint32_t bi = b[i];
        uint64_t carry = 0;
        #pragma unroll
        for (int j = 0; j < 10; j++) {
            uint64_t prod = (uint64_t)a[j] * bi + (uint64_t)t[j] + carry;
            t[j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        uint64_t s = (uint64_t)t[10] + carry;
        t[10] = (uint32_t)s;

        uint32_t m = t[0] * n0inv;
        uint64_t mn = (uint64_t)m * n[0] + (uint64_t)t[0];
        carry = mn >> 32;
        #pragma unroll
        for (int j = 1; j < 10; j++) {
            mn = (uint64_t)m * n[j] + (uint64_t)t[j] + carry;
            t[j - 1] = (uint32_t)mn;
            carry = mn >> 32;
        }
        s = (uint64_t)t[10] + carry;
        t[9] = (uint32_t)s;
        t[10] = (uint32_t)(s >> 32);
    }

    int gt = (t[10] != 0);
    if (!gt) {
        for (int j = 9; j >= 0; j--) {
            if (t[j] > n[j]) { gt = 1; break; }
            if (t[j] < n[j]) break;
        }
    }
    if (gt) {
        uint64_t borrow = 0;
        #pragma unroll
        for (int j = 0; j < 10; j++) {
            uint64_t diff = (uint64_t)t[j] - (uint64_t)n[j] - borrow;
            result[j] = (uint32_t)diff;
            borrow = (diff >> 63) & 1;
        }
    } else {
        #pragma unroll
        for (int j = 0; j < 10; j++) result[j] = t[j];
    }
}

__device__ __forceinline__ void dblMod10(uint32_t *x, const uint32_t *n) {
    uint32_t carry = 0;
    #pragma unroll
    for (int j = 0; j < 10; j++) {
        uint64_t tmp = ((uint64_t)x[j] << 1) | carry;
        x[j] = (uint32_t)tmp;
        carry = (uint32_t)(tmp >> 32);
    }
    int need_sub = carry;
    if (!need_sub) {
        for (int j = 9; j >= 0; j--) {
            if (x[j] > n[j]) { need_sub = 1; break; }
            if (x[j] < n[j]) break;
        }
    }
    if (need_sub) {
        uint64_t borrow = 0;
        #pragma unroll
        for (int j = 0; j < 10; j++) {
            uint64_t diff = (uint64_t)x[j] - (uint64_t)n[j] - borrow;
            x[j] = (uint32_t)diff;
            borrow = (diff >> 63) & 1;
        }
    }
}

__device__ bool fermat320(uint32_t *p) {
    uint32_t n[10], mont1[10], base[10], result[10], temp[10];
    #pragma unroll
    for (int i = 0; i < 10; i++) n[i] = p[i];
    uint32_t n0inv = invert_limb(n[0]);

    int bitCount = 0;
    for (int i = 9; i >= 0; i--) {
        if (n[i]) { bitCount = i * 32 + (32 - __clz(n[i])); break; }
    }
    if (bitCount <= 1) return false;

    #pragma unroll
    for (int i = 0; i < 10; i++) mont1[i] = 0;
    mont1[0] = 1;
    for (int iter = 0; iter < 320; iter++) dblMod10(mont1, n);

    #pragma unroll
    for (int i = 0; i < 10; i++) base[i] = mont1[i];
    dblMod10(base, n);

    uint32_t exp[10];
    #pragma unroll
    for (int i = 0; i < 10; i++) exp[i] = n[i];
    exp[0] &= ~1u;

    #pragma unroll
    for (int i = 0; i < 10; i++) result[i] = mont1[i];
    for (int bit = bitCount - 1; bit >= 0; bit--) {
        monPro10(temp, result, result, n, n0inv);
        #pragma unroll
        for (int i = 0; i < 10; i++) result[i] = temp[i];
        if ((exp[bit / 32] >> (bit % 32)) & 1) {
            monPro10(temp, result, base, n, n0inv);
            #pragma unroll
            for (int i = 0; i < 10; i++) result[i] = temp[i];
        }
    }

    #pragma unroll
    for (int i = 0; i < 10; i++) temp[i] = 0;
    temp[0] = 1;
    monPro10(result, result, temp, n, n0inv);

    if (result[0] != 1) return false;
    #pragma unroll
    for (int i = 1; i < 10; i++) if (result[i] != 0) return false;
    return true;
}

/* CIOS Montgomery Multiplication — 11 limbs (352-bit) */
__device__ void monPro11(uint32_t *result, const uint32_t *a, const uint32_t *b,
                         const uint32_t *n, uint32_t n0inv) {
    uint32_t t[12];
    #pragma unroll
    for (int k = 0; k <= 11; k++) t[k] = 0;

    for (int i = 0; i < 11; i++) {
        uint32_t bi = b[i];
        uint64_t carry = 0;
        #pragma unroll
        for (int j = 0; j < 11; j++) {
            uint64_t prod = (uint64_t)a[j] * bi + (uint64_t)t[j] + carry;
            t[j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        uint64_t s = (uint64_t)t[11] + carry;
        t[11] = (uint32_t)s;

        uint32_t m = t[0] * n0inv;
        uint64_t mn = (uint64_t)m * n[0] + (uint64_t)t[0];
        carry = mn >> 32;
        #pragma unroll
        for (int j = 1; j < 11; j++) {
            mn = (uint64_t)m * n[j] + (uint64_t)t[j] + carry;
            t[j - 1] = (uint32_t)mn;
            carry = mn >> 32;
        }
        s = (uint64_t)t[11] + carry;
        t[10] = (uint32_t)s;
        t[11] = (uint32_t)(s >> 32);
    }

    int gt = (t[11] != 0);
    if (!gt) {
        for (int j = 10; j >= 0; j--) {
            if (t[j] > n[j]) { gt = 1; break; }
            if (t[j] < n[j]) break;
        }
    }
    if (gt) {
        uint64_t borrow = 0;
        #pragma unroll
        for (int j = 0; j < 11; j++) {
            uint64_t diff = (uint64_t)t[j] - (uint64_t)n[j] - borrow;
            result[j] = (uint32_t)diff;
            borrow = (diff >> 63) & 1;
        }
    } else {
        #pragma unroll
        for (int j = 0; j < 11; j++) result[j] = t[j];
    }
}

__device__ __forceinline__ void dblMod11(uint32_t *x, const uint32_t *n) {
    uint32_t carry = 0;
    #pragma unroll
    for (int j = 0; j < 11; j++) {
        uint64_t tmp = ((uint64_t)x[j] << 1) | carry;
        x[j] = (uint32_t)tmp;
        carry = (uint32_t)(tmp >> 32);
    }
    int need_sub = carry;
    if (!need_sub) {
        for (int j = 10; j >= 0; j--) {
            if (x[j] > n[j]) { need_sub = 1; break; }
            if (x[j] < n[j]) break;
        }
    }
    if (need_sub) {
        uint64_t borrow = 0;
        #pragma unroll
        for (int j = 0; j < 11; j++) {
            uint64_t diff = (uint64_t)x[j] - (uint64_t)n[j] - borrow;
            x[j] = (uint32_t)diff;
            borrow = (diff >> 63) & 1;
        }
    }
}

__device__ bool fermat352(uint32_t *p) {
    uint32_t n[11], mont1[11], base[11], result[11], temp[11];
    #pragma unroll
    for (int i = 0; i < 11; i++) n[i] = p[i];
    uint32_t n0inv = invert_limb(n[0]);

    int bitCount = 0;
    for (int i = 10; i >= 0; i--) {
        if (n[i]) { bitCount = i * 32 + (32 - __clz(n[i])); break; }
    }
    if (bitCount <= 1) return false;

    #pragma unroll
    for (int i = 0; i < 11; i++) mont1[i] = 0;
    mont1[0] = 1;
    for (int iter = 0; iter < 352; iter++) dblMod11(mont1, n);

    #pragma unroll
    for (int i = 0; i < 11; i++) base[i] = mont1[i];
    dblMod11(base, n);

    uint32_t exp[11];
    #pragma unroll
    for (int i = 0; i < 11; i++) exp[i] = n[i];
    exp[0] &= ~1u;

    #pragma unroll
    for (int i = 0; i < 11; i++) result[i] = mont1[i];
    for (int bit = bitCount - 1; bit >= 0; bit--) {
        monPro11(temp, result, result, n, n0inv);
        #pragma unroll
        for (int i = 0; i < 11; i++) result[i] = temp[i];
        if ((exp[bit / 32] >> (bit % 32)) & 1) {
            monPro11(temp, result, base, n, n0inv);
            #pragma unroll
            for (int i = 0; i < 11; i++) result[i] = temp[i];
        }
    }

    #pragma unroll
    for (int i = 0; i < 11; i++) temp[i] = 0;
    temp[0] = 1;
    monPro11(result, result, temp, n, n0inv);

    if (result[0] != 1) return false;
    #pragma unroll
    for (int i = 1; i < 11; i++) if (result[i] != 0) return false;
    return true;
}

/* Kernel entry points */
extern "C" __global__ void fermat_kernel_320(uint8_t *results, const uint32_t *primes, uint32_t count) {
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) return;
    uint32_t p[10];
    #pragma unroll
    for (int i = 0; i < 10; i++) p[i] = primes[id * 10 + i];
    results[id] = fermat320(p) ? 1 : 0;
}

extern "C" __global__ void fermat_kernel_352(uint8_t *results, const uint32_t *primes, uint32_t count) {
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) return;
    uint32_t p[11];
    #pragma unroll
    for (int i = 0; i < 11; i++) p[i] = primes[id * 11 + i];
    results[id] = fermat352(p) ? 1 : 0;
}

extern "C" __global__ void fermat_selftest(uint8_t *results) {
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    uint32_t prime[10] = {
        0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0x00000000u, 0x00000000u
    };
    results[0] = fermat320(prime) ? 1 : 0;
    uint32_t mersenne[10] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u
    };
    results[1] = fermat320(mersenne) ? 1 : 0;
    uint32_t composite[10] = {
        0x0000000Fu, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u
    };
    results[2] = fermat320(composite) ? 1 : 0;
    results[3] = 0xAA;
}
