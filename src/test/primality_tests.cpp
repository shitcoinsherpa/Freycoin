// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Rigorous tests for primality testing functions.
 *
 * BPSW (Baillie-PSW) is the gold standard for probable prime testing:
 * - No known BPSW pseudoprimes exist below 2^64
 * - Combines Miller-Rabin base 2 with Strong Lucas test
 *
 * These tests verify:
 * 1. Small primes (exhaustive for small ranges)
 * 2. Known large primes (Mersenne, Sophie Germain, etc.)
 * 3. Strong pseudoprimes to specific bases
 * 4. Carmichael numbers (absolute Fermat pseudoprimes)
 * 5. Lucas pseudoprimes
 * 6. Edge cases and boundary conditions
 */

#include <pow/mining_engine.h>
#include <pow/pow_common.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <gmp.h>
#include <vector>
#include <cmath>
#include <chrono>

BOOST_FIXTURE_TEST_SUITE(primality_tests, BasicTestingSetup)

/*============================================================================
 * Small prime verification
 *
 * Exhaustively verify all primes < 1000 and all composites < 1000.
 *============================================================================*/

// First 168 primes (all primes < 1000)
static const uint32_t SMALL_PRIMES[] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
    73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151,
    157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233,
    239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317,
    331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419,
    421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499, 503,
    509, 521, 523, 541, 547, 557, 563, 569, 571, 577, 587, 593, 599, 601, 607,
    613, 617, 619, 631, 641, 643, 647, 653, 659, 661, 673, 677, 683, 691, 701,
    709, 719, 727, 733, 739, 743, 751, 757, 761, 769, 773, 787, 797, 809, 811,
    821, 823, 827, 829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907, 911,
    919, 929, 937, 941, 947, 953, 967, 971, 977, 983, 991, 997
};
static const size_t NUM_SMALL_PRIMES = sizeof(SMALL_PRIMES) / sizeof(SMALL_PRIMES[0]);

BOOST_AUTO_TEST_CASE(bpsw_small_primes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    for (size_t i = 0; i < NUM_SMALL_PRIMES; i++) {
        mpz_set_ui(n, SMALL_PRIMES[i]);
        BOOST_CHECK_MESSAGE(tester.bpsw_test(n),
            SMALL_PRIMES[i] << " should be identified as prime");
    }

    mpz_clear(n);
}

BOOST_AUTO_TEST_CASE(bpsw_small_composites)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    std::vector<bool> is_prime(1000, false);
    for (size_t i = 0; i < NUM_SMALL_PRIMES; i++) {
        is_prime[SMALL_PRIMES[i]] = true;
    }

    for (uint32_t i = 2; i < 1000; i++) {
        if (!is_prime[i]) {
            mpz_set_ui(n, i);
            BOOST_CHECK_MESSAGE(!tester.bpsw_test(n),
                i << " should be identified as composite");
        }
    }

    mpz_clear(n);
}

/*============================================================================
 * Miller-Rabin base 2 tests
 *
 * Strong pseudoprimes to base 2 are well-documented.
 * These are composites that pass Miller-Rabin test with base 2.
 *============================================================================*/

// Strong pseudoprimes to base 2 (OEIS A001262)
static const uint64_t PSP2[] = {
    2047, 3277, 4033, 4681, 8321, 15841, 29341, 42799, 49141, 52633, 65281,
    74665, 80581, 85489, 88357, 90751, 104653, 130561, 196093, 220729
};
static const size_t NUM_PSP2 = sizeof(PSP2) / sizeof(PSP2[0]);

BOOST_AUTO_TEST_CASE(miller_rabin_base2_pseudoprimes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // These should PASS Miller-Rabin base 2 (they are strong pseudoprimes)
    for (size_t i = 0; i < NUM_PSP2; i++) {
        mpz_set_ui64(n, PSP2[i]);
        BOOST_CHECK_MESSAGE(tester.miller_rabin(n, 2),
            PSP2[i] << " should pass Miller-Rabin base 2 (it's a strong psp)");
    }

    // But they should FAIL BPSW (Strong Lucas catches them)
    for (size_t i = 0; i < NUM_PSP2; i++) {
        mpz_set_ui64(n, PSP2[i]);
        BOOST_CHECK_MESSAGE(!tester.bpsw_test(n),
            PSP2[i] << " should fail BPSW (composite)");
    }

    mpz_clear(n);
}

/*============================================================================
 * Carmichael numbers (absolute Fermat pseudoprimes)
 *
 * These pass Fermat test for ALL bases coprime to n.
 * They must fail Miller-Rabin and BPSW.
 *============================================================================*/

// First Carmichael numbers (OEIS A002997)
static const uint64_t CARMICHAEL[] = {
    561, 1105, 1729, 2465, 2821, 6601, 8911, 10585, 15841, 29341,
    41041, 46657, 52633, 62745, 63973, 75361, 101101, 115921, 126217, 162401
};
static const size_t NUM_CARMICHAEL = sizeof(CARMICHAEL) / sizeof(CARMICHAEL[0]);

BOOST_AUTO_TEST_CASE(bpsw_rejects_carmichael)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    for (size_t i = 0; i < NUM_CARMICHAEL; i++) {
        mpz_set_ui64(n, CARMICHAEL[i]);
        BOOST_CHECK_MESSAGE(!tester.bpsw_test(n),
            "Carmichael number " << CARMICHAEL[i] << " should fail BPSW");
    }

    mpz_clear(n);
}

BOOST_AUTO_TEST_CASE(fermat_fooled_by_carmichael)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // Carmichael numbers pass Fermat test (base 2 specifically)
    for (size_t i = 0; i < NUM_CARMICHAEL; i++) {
        mpz_set_ui64(n, CARMICHAEL[i]);
        // 2^(n-1) ≡ 1 (mod n) for Carmichael numbers when gcd(2,n)=1
        BOOST_CHECK_MESSAGE(tester.fermat_test(n),
            "Carmichael number " << CARMICHAEL[i] << " should pass Fermat test");
    }

    mpz_clear(n);
}

/*============================================================================
 * Strong Lucas pseudoprimes
 *
 * These pass Strong Lucas test but are composite.
 * Combined with Miller-Rabin base 2, BPSW should still reject them.
 *============================================================================*/

// Strong Lucas pseudoprimes with Selfridge parameters (OEIS A217255)
static const uint64_t LUCAS_PSP[] = {
    5459, 5777, 10877, 16109, 18971, 22499, 24569, 25199, 40309, 58519,
    75077, 97439, 100127, 113573, 115639, 130139, 155819, 158399, 161027, 162133
};
static const size_t NUM_LUCAS_PSP = sizeof(LUCAS_PSP) / sizeof(LUCAS_PSP[0]);

BOOST_AUTO_TEST_CASE(bpsw_rejects_lucas_pseudoprimes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    for (size_t i = 0; i < NUM_LUCAS_PSP; i++) {
        mpz_set_ui64(n, LUCAS_PSP[i]);
        BOOST_CHECK_MESSAGE(!tester.bpsw_test(n),
            "Lucas pseudoprime " << LUCAS_PSP[i] << " should fail BPSW");
    }

    mpz_clear(n);
}

/*============================================================================
 * Known large primes
 *
 * Test against mathematically verified large primes.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(bpsw_mersenne_primes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // Mersenne primes: 2^p - 1 for certain p
    int mersenne_exponents[] = {2, 3, 5, 7, 13, 17, 19, 31, 61, 89, 107, 127};

    for (int p : mersenne_exponents) {
        mpz_set_ui(n, 1);
        mpz_mul_2exp(n, n, p);
        mpz_sub_ui(n, n, 1);

        BOOST_CHECK_MESSAGE(tester.bpsw_test(n),
            "Mersenne prime 2^" << p << " - 1 should be identified as prime");
    }

    mpz_clear(n);
}

BOOST_AUTO_TEST_CASE(bpsw_sophie_germain_primes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // Sophie Germain primes: p where 2p+1 is also prime
    // Testing some larger ones
    uint64_t sophie_germain[] = {
        2, 3, 5, 11, 23, 29, 41, 53, 83, 89, 113, 131, 173, 179, 191, 233,
        239, 251, 281, 293, 359, 419, 431, 443, 491, 509, 593
    };

    for (uint64_t p : sophie_germain) {
        mpz_set_ui64(n, p);
        BOOST_CHECK_MESSAGE(tester.bpsw_test(n),
            "Sophie Germain prime " << p << " should be prime");

        // Also check 2p+1
        mpz_set_ui64(n, 2 * p + 1);
        BOOST_CHECK_MESSAGE(tester.bpsw_test(n),
            "Safe prime " << (2 * p + 1) << " should be prime");
    }

    mpz_clear(n);
}

BOOST_AUTO_TEST_CASE(bpsw_256bit_primes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // Known 256-bit primes (verified independently)
    // 2^255 + 95 is prime
    mpz_set_ui(n, 1);
    mpz_mul_2exp(n, n, 255);
    mpz_add_ui(n, n, 95);
    BOOST_CHECK_MESSAGE(tester.bpsw_test(n), "2^255 + 95 should be prime");

    // 2^256 - 189 is prime
    mpz_set_ui(n, 1);
    mpz_mul_2exp(n, n, 256);
    mpz_sub_ui(n, n, 189);
    BOOST_CHECK_MESSAGE(tester.bpsw_test(n), "2^256 - 189 should be prime");

    mpz_clear(n);
}

/*============================================================================
 * Edge cases
 *============================================================================*/

BOOST_AUTO_TEST_CASE(bpsw_edge_cases)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // 0 is not prime
    mpz_set_ui(n, 0);
    BOOST_CHECK(!tester.bpsw_test(n));

    // 1 is not prime
    mpz_set_ui(n, 1);
    BOOST_CHECK(!tester.bpsw_test(n));

    // 2 is prime (smallest)
    mpz_set_ui(n, 2);
    BOOST_CHECK(tester.bpsw_test(n));

    // 3 is prime
    mpz_set_ui(n, 3);
    BOOST_CHECK(tester.bpsw_test(n));

    // 4 is not prime
    mpz_set_ui(n, 4);
    BOOST_CHECK(!tester.bpsw_test(n));

    mpz_clear(n);
}

BOOST_AUTO_TEST_CASE(miller_rabin_perfect_squares)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // Perfect squares should always be rejected
    for (uint32_t i = 2; i <= 100; i++) {
        mpz_set_ui(n, i * i);
        BOOST_CHECK_MESSAGE(!tester.miller_rabin(n, 2),
            i << "^2 = " << (i * i) << " should fail Miller-Rabin");
    }

    mpz_clear(n);
}

BOOST_AUTO_TEST_CASE(fermat_test_primes)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // All primes should pass Fermat test
    for (size_t i = 0; i < NUM_SMALL_PRIMES; i++) {
        mpz_set_ui(n, SMALL_PRIMES[i]);
        BOOST_CHECK_MESSAGE(tester.fermat_test(n),
            "Prime " << SMALL_PRIMES[i] << " should pass Fermat test");
    }

    mpz_clear(n);
}

/*============================================================================
 * Performance sanity checks
 *
 * These tests verify that primality tests complete in reasonable time.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(bpsw_performance_256bit)
{
    PrimalityTester tester;
    mpz_t n;
    mpz_init(n);

    // Generate a 256-bit prime
    mpz_set_ui(n, 1);
    mpz_mul_2exp(n, n, 255);
    mpz_add_ui(n, n, 95);  // Known prime

    // Run BPSW 100 times to ensure it's fast enough
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        BOOST_CHECK(tester.bpsw_test(n));
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    BOOST_CHECK_MESSAGE(duration_ms < 5000,
        "100 BPSW tests on 256-bit prime took " << duration_ms << "ms, expected < 5000ms");

    mpz_clear(n);
}

/*============================================================================
 * Batch primality test interface
 *============================================================================*/

BOOST_AUTO_TEST_CASE(prepare_batch_320bit)
{
    PrimalityTester tester;
    mpz_t start;
    mpz_init(start);

    // Create a 256-bit starting point
    mpz_set_ui(start, 1);
    mpz_mul_2exp(start, start, 255);

    std::vector<uint64_t> candidates = {100, 200, 300, 400, 500};
    CandidateBatch batch;

    tester.prepare_batch(batch, start, candidates, 320);

    BOOST_CHECK_EQUAL(batch.bits, 320u);
    BOOST_CHECK_EQUAL(batch.count, 5u);
    BOOST_CHECK_EQUAL(batch.candidates.size(), 5u * 10);  // 10 limbs per 320-bit number
    BOOST_CHECK_EQUAL(batch.indices.size(), 5u);

    mpz_clear(start);
}

/*============================================================================
 * Consensus-critical determinism tests
 *
 * Primality tests must be deterministic across platforms.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(bpsw_deterministic)
{
    PrimalityTester tester1, tester2;
    mpz_t n;
    mpz_init(n);

    // Test that two independent testers give same results
    for (size_t i = 0; i < NUM_SMALL_PRIMES; i++) {
        mpz_set_ui(n, SMALL_PRIMES[i]);
        BOOST_CHECK_EQUAL(tester1.bpsw_test(n), tester2.bpsw_test(n));
    }

    for (size_t i = 0; i < NUM_CARMICHAEL; i++) {
        mpz_set_ui64(n, CARMICHAEL[i]);
        BOOST_CHECK_EQUAL(tester1.bpsw_test(n), tester2.bpsw_test(n));
    }

    mpz_clear(n);
}

BOOST_AUTO_TEST_SUITE_END()
