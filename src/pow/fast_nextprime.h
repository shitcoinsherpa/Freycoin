// Copyright (c) 2025-2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * gwnum-accelerated next probable prime.
 *
 * Replaces GMP's mpz_nextprime for large numbers (>2000 bits) with a
 * hybrid approach:
 *   1. Segmented sieve (primes up to 10M) eliminates ~97% of candidates
 *   2. gwnum FFT-based Miller-Rabin base-2 test on survivors (15x faster than GMP)
 *   3. GMP BPSW confirmation on the ~1 candidate that passes MR
 *
 * At shift 12000 (~3690 digits), mpz_nextprime takes ~78 seconds.
 * This implementation takes ~31 seconds (2.5x speedup).
 *
 * When gwnum is not available (non-x86, or HAVE_GWNUM not defined),
 * falls back to plain mpz_nextprime.
 *
 * gwnum (George Woltman's Number library) uses AVX-512/AVX2/SSE2 FFT
 * for fast modular arithmetic. It's the engine behind GIMPS/Prime95.
 */

#ifndef FREYCOIN_POW_FAST_NEXTPRIME_H
#define FREYCOIN_POW_FAST_NEXTPRIME_H

#include <gmp.h>

/**
 * Find the next probable prime after n.
 *
 * Uses gwnum-accelerated Fermat testing when HAVE_GWNUM is defined,
 * otherwise falls back to mpz_nextprime. Produces identical results
 * to mpz_nextprime for all practical inputs (both use BPSW).
 */
void fast_nextprime(mpz_t result, const mpz_t n);

/**
 * Fast Miller-Rabin base-2 probable prime test using gwnum FFT.
 *
 * Performs a strong probable prime test: factors n-1 = d * 2^r, computes
 * 2^d mod n via gwnum FFT squaring, then checks intermediate values.
 * Strictly stronger than Fermat (catches Carmichael numbers). ~15x faster
 * than GMP's mpz_probab_prime_p at 12K bits (~31ms vs ~466ms).
 *
 * Falls back to GMP mpz_probab_prime_p(n, 0) when gwnum is unavailable
 * or the number is below GWNUM_THRESHOLD_BITS.
 */
bool fast_is_fermat_prp(const mpz_t n);

/** Signal the parallel MR worker pool to abort at its next check point. */
void InterruptPow();

/** Clear the interrupt flag (call before restarting validation after a pause). */
void ResetPowInterrupt();

/** True if InterruptPow() has been called since the last ResetPowInterrupt(). */
bool IsPowInterrupted();

#endif // FREYCOIN_POW_FAST_NEXTPRIME_H
