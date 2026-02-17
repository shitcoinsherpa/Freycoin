// Copyright (c) 2025-2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * gwnum-accelerated next probable prime.
 *
 * Replaces GMP's mpz_nextprime for large numbers (>2000 bits) with a
 * hybrid approach:
 *   1. Segmented sieve (primes up to 500K) eliminates ~95% of candidates
 *   2. gwnum FFT-based Fermat base-2 test on survivors (4x faster than GMP)
 *   3. GMP BPSW confirmation on the ~1 candidate that passes Fermat
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

#endif // FREYCOIN_POW_FAST_NEXTPRIME_H
