// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * AVX-512 IFMA Vectorized Primality Testing
 *
 * Processes 8 candidate primes simultaneously using AVX-512 Integer Fused
 * Multiply-Add (IFMA) instructions: VPMADD52LO/VPMADD52HI.
 *
 * These instructions perform 52-bit × 52-bit → 104-bit multiply-add,
 * which maps perfectly onto Residue Number System (RNS) Montgomery
 * multiplication for big numbers represented as 52-bit limbs.
 *
 * For 320-bit numbers: 7 limbs of 52 bits each (7 × 52 = 364 > 320)
 * For 352-bit numbers: 7 limbs of 52 bits each (7 × 52 = 364 > 352)
 *
 * Each __m512i register holds 8 × 64-bit lanes. We use "vertical" SIMD:
 * - 7 registers hold the 7 limbs of 8 different numbers
 * - All 8 numbers undergo the same arithmetic operations in lockstep
 * - Result: 8 Fermat tests in the time of ~1.2 scalar tests
 *
 * Hardware requirements:
 * - Intel: Ice Lake (2019+), Tiger Lake, Alder Lake P-cores, Sapphire Rapids
 * - AMD: Zen 4 (Ryzen 7000+), Zen 5
 * These CPUs support AVX-512F + AVX-512IFMA (VPMADD52).
 *
 * Fallback: When IFMA is not available, the caller uses scalar BPSW
 * (PrimalityTester::bpsw_test) which works on all CPUs.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_AVX512_PRIMALITY_H
#define FREYCOIN_POW_AVX512_PRIMALITY_H

#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * Check if the CPU supports AVX-512 IFMA instructions.
 * Thread-safe, cached after first call.
 *
 * @return true if VPMADD52LO/HI are available
 */
bool avx512_ifma_available();

/**
 * Batch Fermat primality test: 2^(p-1) ≡ 1 (mod p) for up to 8 candidates.
 *
 * Processes candidates in groups of 8 using AVX-512 IFMA vectorized
 * Montgomery exponentiation. Remaining candidates (count % 8) are
 * handled by a final masked operation.
 *
 * @param results    Output array: 1 = Fermat probable prime, 0 = composite
 * @param candidates Input: limb-packed candidate numbers (little-endian uint32_t)
 * @param count      Number of candidates
 * @param bits       Bit width of candidates (320 or 352)
 * @return 0 on success, -1 if IFMA not available
 *
 * Note: Fermat test is a pre-filter. Candidates passing Fermat should be
 * confirmed with full BPSW for consensus validation. The mining pipeline
 * already does this — GPU/AVX-512 Fermat → CPU BPSW confirmation.
 */
int avx512_fermat_batch(uint8_t *results,
                         const uint32_t *candidates,
                         uint32_t count,
                         int bits);

/**
 * Single-candidate Fermat test using AVX-512 IFMA.
 * Pads to 8 candidates internally (waste 7/8 of SIMD width).
 * Use avx512_fermat_batch for better throughput.
 *
 * @param candidate  Limb-packed candidate number
 * @param bits       Bit width (320 or 352)
 * @return true if 2^(p-1) ≡ 1 (mod p)
 */
bool avx512_fermat_single(const uint32_t *candidate, int bits);

#endif // FREYCOIN_POW_AVX512_PRIMALITY_H
