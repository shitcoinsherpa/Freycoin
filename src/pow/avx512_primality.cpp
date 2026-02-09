// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * AVX-512 IFMA Vectorized Primality Testing — Implementation
 *
 * Montgomery multiplication in 52-bit limb representation using VPMADD52.
 * Processes 8 candidates simultaneously in vertical SIMD layout.
 *
 * Representation: 320-bit number N = sum(limb[i] * 2^(52*i)) for i=0..6
 * Each limb fits in 52 bits (max value 2^52 - 1).
 * Stored in __m512i: 8 copies of limb[i], one per candidate.
 *
 * Montgomery parameter: R = 2^(52*7) = 2^364
 * Montgomery form: aR mod N
 * Multiplication: monMul(aR, bR) = abR mod N
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/avx512_primality.h>

#include <cstring>

#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h>
#endif

/* Number of 52-bit limbs for numbers up to 364 bits (covers 320 and 352) */
#define NLIMBS 7

/* 52-bit mask */
#define MASK52 0x000FFFFFFFFFFFFFULL

/*============================================================================
 * CPU Feature Detection
 *============================================================================*/

static bool g_ifma_detected = false;
static bool g_ifma_checked = false;

bool avx512_ifma_available() {
    if (g_ifma_checked) return g_ifma_detected;

#ifdef _WIN32
    int cpuinfo[4] = {0};
    __cpuidex(cpuinfo, 7, 0);
    /* ECX bit 21 = AVX-512 IFMA (VPMADD52) */
    /* EBX bit 16 = AVX-512F */
    bool has_avx512f = (cpuinfo[1] >> 16) & 1;
    bool has_ifma = (cpuinfo[2] >> 21) & 1;
    /* Also need AVX-512VL for 256-bit variants, but we use 512-bit */
    g_ifma_detected = has_avx512f && has_ifma;
#else
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        bool has_avx512f = (ebx >> 16) & 1;
        bool has_ifma = (ecx >> 21) & 1;
        g_ifma_detected = has_avx512f && has_ifma;
    }
#endif

    g_ifma_checked = true;
    return g_ifma_detected;
}

/*============================================================================
 * AVX-512 IFMA Implementation
 *
 * The actual SIMD code is conditionally compiled only on x86-64 with
 * AVX-512 IFMA support. On other platforms, the functions return -1.
 *============================================================================*/

#if defined(__AVX512F__) && defined(__AVX512IFMA__)
#include <immintrin.h>

/**
 * Convert 32-bit limb representation to 52-bit limb representation.
 * Input: 10 or 11 × 32-bit limbs (320 or 352 bits)
 * Output: 7 × 52-bit limbs packed into uint64_t
 *
 * The conversion re-slices the bit stream into 52-bit chunks:
 * limb52[0] = bits[0..51], limb52[1] = bits[52..103], etc.
 */
static inline void convert_to_52bit(uint64_t out[NLIMBS],
                                     const uint32_t *in,
                                     int in_limbs) {
    /* Treat input as a byte stream and extract 52-bit slices */
    uint8_t bytes[48] = {0};  /* 384 bits = 48 bytes, zero-padded */
    std::memcpy(bytes, in, in_limbs * sizeof(uint32_t));

    /* Extract 52-bit limbs from the byte stream (little-endian) */
    uint64_t full[6] = {0};
    for (int i = 0; i < 6 && i * 8 < (int)(in_limbs * 4); i++) {
        std::memcpy(&full[i], &bytes[i * 8], 8);
    }

    /* Re-slice into 52-bit limbs */
    /* Limb 0: bits 0-51 */
    out[0] = full[0] & MASK52;
    /* Limb 1: bits 52-103 */
    out[1] = ((full[0] >> 52) | (full[1] << 12)) & MASK52;
    /* Limb 2: bits 104-155 */
    out[2] = ((full[1] >> 40) | (full[2] << 24)) & MASK52;
    /* Limb 3: bits 156-207 */
    out[3] = ((full[2] >> 28) | (full[3] << 36)) & MASK52;
    /* Limb 4: bits 208-259 */
    out[4] = ((full[3] >> 16) | (full[4] << 48)) & MASK52;
    /* Limb 5: bits 260-311 */
    out[5] = ((full[4] >> 4)) & MASK52;
    /* Limb 6: bits 312-363 */
    out[6] = ((full[4] >> 56) | (full[5] << 8)) & MASK52;
}

/**
 * Convert 52-bit limb representation back to 32-bit for result checking.
 * Only needs to check if the number equals 1.
 */
static inline bool is_one_52bit(const uint64_t limbs[NLIMBS]) {
    if (limbs[0] != 1) return false;
    for (int i = 1; i < NLIMBS; i++) {
        if (limbs[i] != 0) return false;
    }
    return true;
}

/**
 * Vectorized Montgomery multiplication for 8 numbers simultaneously.
 *
 * Computes c = a * b * R^(-1) mod m for 8 independent (a, m, b) triples.
 *
 * Uses the CIOS (Coarsely Integrated Operand Scanning) variant of
 * Montgomery multiplication, adapted for 52-bit limbs with VPMADD52.
 *
 * Each __m512i holds 8 × 64-bit values, one per candidate number.
 * The 7 limbs of each number are stored across 7 separate registers.
 */
static void monMul_avx512(__m512i c[NLIMBS],        /* output: a*b*R^-1 mod m */
                           const __m512i a[NLIMBS],  /* input a in Montgomery form */
                           const __m512i b[NLIMBS],  /* input b in Montgomery form */
                           const __m512i m[NLIMBS],  /* modulus */
                           const __m512i m_inv)      /* -m^(-1) mod 2^52 */
{
    __m512i t[NLIMBS + 1];
    const __m512i mask52 = _mm512_set1_epi64(MASK52);
    const __m512i zero = _mm512_setzero_si512();

    /* Initialize accumulator to zero */
    for (int i = 0; i <= NLIMBS; i++) {
        t[i] = zero;
    }

    /* CIOS Montgomery multiplication */
    for (int i = 0; i < NLIMBS; i++) {
        __m512i bi = b[i]; /* broadcast b[i] across all 8 lanes (already there) */

        /* Step 1: t = t + a * b[i] */
        __m512i carry = zero;
        for (int j = 0; j < NLIMBS; j++) {
            /* t[j] += a[j] * bi (low 52 bits) */
            t[j] = _mm512_madd52lo_epu64(t[j], a[j], bi);
            /* Add carry from previous limb */
            t[j] = _mm512_add_epi64(t[j], carry);
            /* Extract carry: bits above 52 */
            carry = _mm512_srli_epi64(t[j], 52);
            t[j] = _mm512_and_epi64(t[j], mask52);

            /* Add high part of a[j] * bi */
            __m512i hi = _mm512_madd52hi_epu64(zero, a[j], bi);
            carry = _mm512_add_epi64(carry, hi);
        }
        t[NLIMBS] = _mm512_add_epi64(t[NLIMBS], carry);

        /* Step 2: Compute q = t[0] * m_inv mod 2^52 */
        __m512i q = _mm512_madd52lo_epu64(zero, t[0], m_inv);
        q = _mm512_and_epi64(q, mask52);

        /* Step 3: t = (t + q * m) / 2^52 */
        carry = zero;
        /* First limb: t[0] + q*m[0] should be divisible by 2^52 */
        __m512i tmp = _mm512_madd52lo_epu64(t[0], q, m[0]);
        carry = _mm512_srli_epi64(tmp, 52);
        carry = _mm512_add_epi64(carry, _mm512_madd52hi_epu64(zero, q, m[0]));

        for (int j = 1; j < NLIMBS; j++) {
            tmp = _mm512_madd52lo_epu64(t[j], q, m[j]);
            tmp = _mm512_add_epi64(tmp, carry);
            carry = _mm512_srli_epi64(tmp, 52);
            carry = _mm512_add_epi64(carry, _mm512_madd52hi_epu64(zero, q, m[j]));
            t[j - 1] = _mm512_and_epi64(tmp, mask52);
        }

        tmp = _mm512_add_epi64(t[NLIMBS], carry);
        t[NLIMBS - 1] = _mm512_and_epi64(tmp, mask52);
        t[NLIMBS] = _mm512_srli_epi64(tmp, 52);
    }

    /* Final subtraction: if t >= m, compute t - m */
    /* Compare t with m (lexicographic from high limb) */
    __mmask8 ge_mask = 0xFF; /* assume t >= m for all 8 */
    for (int j = NLIMBS - 1; j >= 0; j--) {
        __mmask8 gt = _mm512_cmpgt_epu64_mask(t[j], m[j]);
        __mmask8 lt = _mm512_cmpgt_epu64_mask(m[j], t[j]);
        ge_mask = ge_mask & ~lt; /* if m[j] > t[j], definitely t < m */
        ge_mask = ge_mask | gt;  /* if t[j] > m[j], definitely t >= m */
        /* If equal, continue to next limb */
    }
    /* Also check if t[NLIMBS] > 0 */
    ge_mask = ge_mask | _mm512_cmpgt_epu64_mask(t[NLIMBS], zero);

    /* Conditional subtraction */
    if (ge_mask) {
        __m512i borrow = zero;
        for (int j = 0; j < NLIMBS; j++) {
            __m512i diff = _mm512_sub_epi64(t[j], m[j]);
            diff = _mm512_sub_epi64(diff, borrow);
            /* Check for borrow: if diff > t[j] (unsigned), we borrowed */
            borrow = _mm512_srli_epi64(diff, 63); /* borrow is in bit 63 */
            diff = _mm512_and_epi64(diff, mask52);
            /* Blend: use diff where ge_mask is set, else keep t[j] */
            c[j] = _mm512_mask_blend_epi64(ge_mask, t[j], diff);
        }
    } else {
        for (int j = 0; j < NLIMBS; j++) {
            c[j] = t[j];
        }
    }
}

/**
 * Vectorized Montgomery squaring (calls monMul with a=b).
 * A dedicated squaring implementation could be ~1.5x faster by exploiting
 * symmetry, but monMul is simpler and correct.
 */
static inline void monSqr_avx512(__m512i c[NLIMBS],
                                   const __m512i a[NLIMBS],
                                   const __m512i m[NLIMBS],
                                   const __m512i m_inv) {
    monMul_avx512(c, a, a, m, m_inv);
}

/**
 * Compute -m^(-1) mod 2^52 for 8 moduli simultaneously.
 * Uses Newton's method: inv = inv * (2 - m * inv) iterated.
 */
static __m512i compute_mont_inv(__m512i m0) {
    const __m512i mask52 = _mm512_set1_epi64(MASK52);
    const __m512i two = _mm512_set1_epi64(2);

    /* Start with inv = m0 (odd, so m0 * m0 ≡ m0^2 ≡ ... eventually converges) */
    /* Actually, use the standard approach: inv = m0, then iterate */
    __m512i inv = m0; /* m0 is odd, so this is a valid starting approximation */

    /* Newton iterations: inv = inv * (2 - m0 * inv) mod 2^52 */
    for (int i = 0; i < 6; i++) { /* 6 iterations for 52-bit convergence */
        __m512i t = _mm512_madd52lo_epu64(_mm512_setzero_si512(), m0, inv);
        t = _mm512_and_epi64(t, mask52);
        t = _mm512_sub_epi64(two, t);
        t = _mm512_and_epi64(t, mask52);
        inv = _mm512_madd52lo_epu64(_mm512_setzero_si512(), inv, t);
        inv = _mm512_and_epi64(inv, mask52);
    }

    /* Negate: -inv mod 2^52 */
    __m512i neg_inv = _mm512_sub_epi64(_mm512_set1_epi64(MASK52 + 1), inv);
    neg_inv = _mm512_and_epi64(neg_inv, mask52);
    return neg_inv;
}

/**
 * Convert integer 2 to Montgomery form: 2 * R mod m, for 8 moduli.
 *
 * R = 2^(52*7) = 2^364. So 2R mod m = (2 << 364) mod m.
 * We compute this by repeated doubling: start with 2, left-shift 364 times,
 * reducing mod m at each step.
 */
static void to_mont_form_2(__m512i out[NLIMBS],
                            const __m512i m[NLIMBS]) {
    const __m512i mask52 = _mm512_set1_epi64(MASK52);

    /* Start with out = 2 */
    out[0] = _mm512_set1_epi64(2);
    for (int i = 1; i < NLIMBS; i++) {
        out[i] = _mm512_setzero_si512();
    }

    /* Left-shift by 1, 364 times, reducing mod m each time */
    /* This is equivalent to computing 2 * 2^364 mod m */
    for (int bit = 0; bit < 52 * NLIMBS; bit++) {
        /* Double: shift left by 1 across all limbs */
        __m512i carry = _mm512_setzero_si512();
        for (int j = 0; j < NLIMBS; j++) {
            __m512i shifted = _mm512_slli_epi64(out[j], 1);
            shifted = _mm512_or_epi64(shifted, carry);
            carry = _mm512_srli_epi64(out[j], 51); /* bit 51 becomes carry */
            carry = _mm512_and_epi64(carry, _mm512_set1_epi64(1));
            out[j] = _mm512_and_epi64(shifted, mask52);
        }

        /* Conditional subtraction: if out >= m, subtract m */
        /* Simple comparison: check if carry or high limb comparison */
        __mmask8 ge = _mm512_cmpgt_epu64_mask(carry, _mm512_setzero_si512());

        /* Also check lexicographic comparison */
        if (!ge) {
            for (int j = NLIMBS - 1; j >= 0; j--) {
                __mmask8 gt = _mm512_cmpgt_epu64_mask(out[j], m[j]);
                __mmask8 lt = _mm512_cmpgt_epu64_mask(m[j], out[j]);
                if (gt | lt) {
                    ge = gt;
                    break;
                }
            }
        }

        if (ge) {
            __m512i borrow = _mm512_setzero_si512();
            for (int j = 0; j < NLIMBS; j++) {
                __m512i diff = _mm512_sub_epi64(out[j], m[j]);
                diff = _mm512_sub_epi64(diff, borrow);
                borrow = _mm512_srli_epi64(diff, 63);
                diff = _mm512_and_epi64(diff, mask52);
                out[j] = _mm512_mask_blend_epi64(ge, out[j], diff);
            }
        }
    }
}

/**
 * Montgomery reduction: convert from Montgomery form to standard form.
 * Computes a * R^(-1) mod m by doing monMul(a, 1).
 */
static void from_mont_form(__m512i out[NLIMBS],
                            const __m512i a[NLIMBS],
                            const __m512i m[NLIMBS],
                            const __m512i m_inv) {
    __m512i one[NLIMBS];
    one[0] = _mm512_set1_epi64(1);
    for (int i = 1; i < NLIMBS; i++) {
        one[i] = _mm512_setzero_si512();
    }
    monMul_avx512(out, a, one, m, m_inv);
}

/**
 * Batch Fermat test: 2^(p-1) ≡ 1 (mod p) for 8 candidates.
 *
 * Uses left-to-right binary exponentiation (square-and-multiply)
 * with Montgomery multiplication.
 *
 * Exponent is p-1 for each candidate. Since candidates differ, the
 * exponent bits differ per lane. We use masked multiply to handle this:
 * always square, then conditionally multiply by base where the bit is set.
 */
static void fermat8(uint8_t results[8],
                     const uint64_t candidates[8][NLIMBS],
                     int bit_count) {
    __m512i m[NLIMBS], base_mont[NLIMBS], result[NLIMBS], exp[NLIMBS];

    /* Load 8 candidates into vertical SIMD layout */
    for (int j = 0; j < NLIMBS; j++) {
        uint64_t lanes[8];
        for (int k = 0; k < 8; k++) {
            lanes[k] = candidates[k][j];
        }
        m[j] = _mm512_loadu_epi64(lanes);
    }

    /* Compute Montgomery inverse: -m[0]^(-1) mod 2^52 */
    __m512i m_inv = compute_mont_inv(m[0]);

    /* Convert base=2 to Montgomery form: 2R mod m */
    to_mont_form_2(base_mont, m);

    /* Compute exponent = m - 1 (p - 1) for each candidate */
    for (int j = 0; j < NLIMBS; j++) {
        exp[j] = m[j];
    }
    /* Subtract 1 from limb 0 */
    exp[0] = _mm512_sub_epi64(exp[0], _mm512_set1_epi64(1));
    /* No borrow possible since all candidates are odd (limb0 has bit 0 set) */
    /* After subtracting 1, bit 0 of exp is now 0 */

    /* Initialize result to 1 in Montgomery form: 1*R mod m */
    /* Compute by: monMul(base_mont, base_mont) then ... no, easier to compute R mod m */
    /* Actually, 1*R mod m = R mod m. We can compute this during to_mont_form_2 by
       computing (1 << 364) mod m instead of (2 << 364) mod m. */
    /* Simpler: initialize result = base_mont (= 2R mod m), and start from bit (bitcount-2) */
    /* This computes 2^(exp) where exp starts from the MSB */

    /* Actually, let's use the standard approach:
       result = 1 in Montgomery form = R mod m
       for each bit of exponent from MSB to LSB:
         result = result^2 mod m  (monSqr)
         if bit is set: result = result * base mod m  (monMul)
       Then convert out of Montgomery form.
    */

    /* Compute R mod m by computing 1_mont = monMul(1, R^2 mod m) ... too complex.
       Easier: start from base_mont and adjust exponent.
       result = base_mont (= 2R mod m)
       We want 2^(p-1) mod p = (2R) * R^(-1) * 2^(p-2) ... no.

       Cleaner: result = base_mont, compute base_mont^(p-1) / 2^0 ...

       Actually the cleanest is:
       result = base_mont  (= 2 in Montgomery form)
       Compute result^(p-1) in Montgomery form using square-and-multiply.
       Then convert result out of Montgomery form.
       If result == 1, Fermat test passes.

       But result^(p-1) starts by squaring base_mont for the MSB.
       Standard left-to-right: result = 1_mont, then for each bit, square and conditionally multiply.
       We need 1_mont = R mod m.

       Compute R mod m the same way as to_mont_form_2, but starting with 1 instead of 2.
    */

    /* Compute 1 in Montgomery form: R mod m */
    __m512i one_mont[NLIMBS];
    one_mont[0] = _mm512_set1_epi64(1);
    for (int j = 1; j < NLIMBS; j++) {
        one_mont[j] = _mm512_setzero_si512();
    }
    /* Shift left by 52*7 = 364 bits, reducing mod m */
    const __m512i mask52 = _mm512_set1_epi64(MASK52);
    for (int bit = 0; bit < 52 * NLIMBS; bit++) {
        __m512i carry = _mm512_setzero_si512();
        for (int j = 0; j < NLIMBS; j++) {
            __m512i shifted = _mm512_slli_epi64(one_mont[j], 1);
            shifted = _mm512_or_epi64(shifted, carry);
            carry = _mm512_srli_epi64(one_mont[j], 51);
            carry = _mm512_and_epi64(carry, _mm512_set1_epi64(1));
            one_mont[j] = _mm512_and_epi64(shifted, mask52);
        }
        /* Conditional subtract if >= m */
        __mmask8 ge = _mm512_cmpgt_epu64_mask(carry, _mm512_setzero_si512());
        if (!ge) {
            for (int j = NLIMBS - 1; j >= 0; j--) {
                __mmask8 gt = _mm512_cmpgt_epu64_mask(one_mont[j], m[j]);
                __mmask8 lt = _mm512_cmpgt_epu64_mask(m[j], one_mont[j]);
                if (gt | lt) { ge = gt; break; }
            }
        }
        if (ge) {
            __m512i borrow = _mm512_setzero_si512();
            for (int j = 0; j < NLIMBS; j++) {
                __m512i diff = _mm512_sub_epi64(one_mont[j], m[j]);
                diff = _mm512_sub_epi64(diff, borrow);
                borrow = _mm512_srli_epi64(diff, 63);
                diff = _mm512_and_epi64(diff, mask52);
                one_mont[j] = _mm512_mask_blend_epi64(ge, one_mont[j], diff);
            }
        }
    }

    /* result = 1 in Montgomery form */
    for (int j = 0; j < NLIMBS; j++) {
        result[j] = one_mont[j];
    }

    /* Left-to-right binary exponentiation */
    /* Extract exponent bits per lane. Since each candidate may have different
       bit count, we iterate from the maximum bit count down to 0. */
    for (int bit = bit_count - 1; bit >= 0; bit--) {
        /* Square */
        __m512i sq[NLIMBS];
        monSqr_avx512(sq, result, m, m_inv);
        for (int j = 0; j < NLIMBS; j++) result[j] = sq[j];

        /* Extract bit 'bit' from each candidate's exponent */
        int limb_idx = bit / 52;
        int bit_idx = bit % 52;
        __m512i exp_limb = exp[limb_idx];
        __m512i bit_val = _mm512_srli_epi64(exp_limb, bit_idx);
        bit_val = _mm512_and_epi64(bit_val, _mm512_set1_epi64(1));
        __mmask8 bit_set = _mm512_cmpgt_epu64_mask(bit_val, _mm512_setzero_si512());

        /* Conditional multiply: result = result * base where bit is set */
        if (bit_set) {
            __m512i mul[NLIMBS];
            monMul_avx512(mul, result, base_mont, m, m_inv);
            for (int j = 0; j < NLIMBS; j++) {
                result[j] = _mm512_mask_blend_epi64(bit_set, result[j], mul[j]);
            }
        }
    }

    /* Convert result out of Montgomery form */
    __m512i final_result[NLIMBS];
    from_mont_form(final_result, result, m, m_inv);

    /* Check if result == 1 for each of 8 candidates */
    __mmask8 is_one = _mm512_cmpeq_epu64_mask(final_result[0], _mm512_set1_epi64(1));
    for (int j = 1; j < NLIMBS; j++) {
        is_one = is_one & _mm512_cmpeq_epu64_mask(final_result[j], _mm512_setzero_si512());
    }

    /* Store results */
    for (int k = 0; k < 8; k++) {
        results[k] = (is_one >> k) & 1;
    }
}

int avx512_fermat_batch(uint8_t *results,
                         const uint32_t *candidates,
                         uint32_t count,
                         int bits) {
    if (!avx512_ifma_available()) return -1;

    int in_limbs = (bits <= 320) ? 10 : 11;
    int bit_count = bits;

    /* Process in groups of 8 */
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        /* Convert 8 candidates from 32-bit to 52-bit representation */
        uint64_t cands52[8][NLIMBS];
        for (int k = 0; k < 8; k++) {
            convert_to_52bit(cands52[k], &candidates[(i + k) * in_limbs], in_limbs);
        }
        fermat8(&results[i], cands52, bit_count);
    }

    /* Handle remainder with zero-padding */
    if (i < count) {
        uint64_t cands52[8][NLIMBS];
        uint8_t tmp_results[8] = {0};

        /* Load remaining candidates, pad unused slots with a known composite (4) */
        for (int k = 0; k < 8; k++) {
            if (i + k < count) {
                convert_to_52bit(cands52[k], &candidates[(i + k) * in_limbs], in_limbs);
            } else {
                /* Pad with 4 (composite): Fermat test will correctly return 0 */
                std::memset(cands52[k], 0, sizeof(cands52[k]));
                cands52[k][0] = 4;
            }
        }
        fermat8(tmp_results, cands52, bit_count);

        for (uint32_t k = 0; i + k < count; k++) {
            results[i + k] = tmp_results[k];
        }
    }

    return 0;
}

bool avx512_fermat_single(const uint32_t *candidate, int bits) {
    if (!avx512_ifma_available()) return false;

    int in_limbs = (bits <= 320) ? 10 : 11;
    uint64_t cands52[8][NLIMBS];

    /* Load the candidate into slot 0, pad rest with 4 */
    convert_to_52bit(cands52[0], candidate, in_limbs);
    for (int k = 1; k < 8; k++) {
        std::memset(cands52[k], 0, sizeof(cands52[k]));
        cands52[k][0] = 4;
    }

    uint8_t results[8] = {0};
    fermat8(results, cands52, bits);
    return results[0] != 0;
}

#else /* No AVX-512 IFMA support at compile time */

int avx512_fermat_batch(uint8_t *results,
                         const uint32_t *candidates,
                         uint32_t count,
                         int bits) {
    (void)results; (void)candidates; (void)count; (void)bits;
    return -1; /* Not available */
}

bool avx512_fermat_single(const uint32_t *candidate, int bits) {
    (void)candidate; (void)bits;
    return false;
}

#endif /* __AVX512F__ && __AVX512IFMA__ */
