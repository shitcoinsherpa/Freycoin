// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Rigorous tests for PoW mathematical functions.
 *
 * These tests verify consensus-critical calculations using:
 * 1. Known mathematical constants
 * 2. Independent calculation cross-validation
 * 3. Edge cases and boundary conditions
 * 4. Real prime gap records from literature
 *
 * All fixed-point values use 2^48 precision (1.0 = 281474976710656).
 */

#include <pow/pow_common.h>
#include <pow/pow_utils.h>
#include <pow/pow.h>
#include <crypto/sha256.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <gmp.h>
#include <cmath>
#include <limits>

BOOST_FIXTURE_TEST_SUITE(pow_math_tests, BasicTestingSetup)

/*============================================================================
 * mpz utility function tests
 *============================================================================*/

BOOST_AUTO_TEST_CASE(mpz_set_get_ui64_roundtrip)
{
    mpz_t mpz;
    mpz_init(mpz);

    // Test powers of 2
    for (int i = 0; i < 64; i++) {
        uint64_t val = 1ULL << i;
        mpz_set_ui64(mpz, val);
        BOOST_CHECK_EQUAL(mpz_get_ui64(mpz), val);
    }

    // Test maximum uint64
    mpz_set_ui64(mpz, UINT64_MAX);
    BOOST_CHECK_EQUAL(mpz_get_ui64(mpz), UINT64_MAX);

    // Test zero
    mpz_set_ui64(mpz, 0);
    BOOST_CHECK_EQUAL(mpz_get_ui64(mpz), 0);

    // Test specific values
    mpz_set_ui64(mpz, 0xDEADBEEFCAFEBABEULL);
    BOOST_CHECK_EQUAL(mpz_get_ui64(mpz), 0xDEADBEEFCAFEBABEULL);

    mpz_clear(mpz);
}

BOOST_AUTO_TEST_CASE(mpz_fits_ui64_boundary)
{
    mpz_t mpz;
    mpz_init(mpz);

    // 2^64 - 1 should fit
    mpz_set_ui64(mpz, UINT64_MAX);
    BOOST_CHECK(mpz_fits_ui64(mpz));

    // 2^64 should NOT fit
    mpz_set_ui(mpz, 1);
    mpz_mul_2exp(mpz, mpz, 64);
    BOOST_CHECK(!mpz_fits_ui64(mpz));

    // 2^65 should NOT fit
    mpz_set_ui(mpz, 1);
    mpz_mul_2exp(mpz, mpz, 65);
    BOOST_CHECK(!mpz_fits_ui64(mpz));

    mpz_clear(mpz);
}

BOOST_AUTO_TEST_CASE(ary_to_mpz_little_endian)
{
    mpz_t mpz;
    mpz_init(mpz);

    // 0x0102030405060708 in little-endian: 08 07 06 05 04 03 02 01
    uint8_t ary[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    ary_to_mpz(mpz, ary, 8);
    BOOST_CHECK_EQUAL(mpz_get_ui64(mpz), 0x0102030405060708ULL);

    // Single byte
    uint8_t single[] = {0xFF};
    ary_to_mpz(mpz, single, 1);
    BOOST_CHECK_EQUAL(mpz_get_ui64(mpz), 0xFF);

    mpz_clear(mpz);
}

/*============================================================================
 * Merit precision tests
 *
 * merit = gap / ln(start), computed via MPFR at 256-bit precision.
 * We verify against independently computed values.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(merit_exact_known_values)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // gap(2, 3) = 1, ln(2) = 0.693147..., merit = 1/0.693147 = 1.442695...
    mpz_set_ui(start, 2);
    mpz_set_ui(end, 3);
    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;
    BOOST_CHECK_MESSAGE(std::abs(merit_d - 1.442695) < 0.001,
        "merit(2,3) = " << merit_d << ", expected ~1.442695 (1/ln(2))");

    // gap(3, 5) = 2, ln(3) = 1.098612..., merit = 2/1.098612 = 1.820478...
    mpz_set_ui(start, 3);
    mpz_set_ui(end, 5);
    merit = utils.merit(start, end);
    merit_d = static_cast<double>(merit) / TWO_POW48;
    BOOST_CHECK_MESSAGE(std::abs(merit_d - 1.820478) < 0.001,
        "merit(3,5) = " << merit_d << ", expected ~1.820478");

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_CASE(merit_large_prime_precision)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // 2^255 + 95 is prime, next prime gives a gap
    mpz_set_ui(start, 1);
    mpz_mul_2exp(start, start, 255);
    mpz_add_ui(start, start, 95);
    BOOST_REQUIRE(mpz_probab_prime_p(start, 25) > 0);

    mpz_nextprime(end, start);

    mpz_t gap;
    mpz_init(gap);
    mpz_sub(gap, end, start);
    uint64_t gap_size = mpz_get_ui64(gap);
    mpz_clear(gap);

    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;

    // ln(2^255 + 95) = 255 * ln(2) + ln(1 + 95/2^255) ≈ 176.752
    double expected = static_cast<double>(gap_size) / (255.0 * std::log(2.0));
    BOOST_CHECK_MESSAGE(std::abs(merit_d - expected) < 0.01,
        "256-bit merit = " << merit_d << ", expected ~" << expected);

    mpz_clear(start);
    mpz_clear(end);
}

/*============================================================================
 * Merit calculation tests
 *
 * merit = gap / ln(start) = gap * log2(e) / log2(start)
 *
 * We use known prime gap records for verification.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(merit_small_gaps)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // Gap between 7 and 11: gap=4, ln(7)=1.9459..., merit=4/1.9459=2.056
    mpz_set_ui(start, 7);
    mpz_set_ui(end, 11);
    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;
    BOOST_CHECK_MESSAGE(std::abs(merit_d - 2.0558) < 0.001,
        "merit(7,11) = " << merit_d << ", expected ~2.0558");

    // Gap between 23 and 29: gap=6, ln(23)=3.1355..., merit=6/3.1355=1.914
    mpz_set_ui(start, 23);
    mpz_set_ui(end, 29);
    merit = utils.merit(start, end);
    merit_d = static_cast<double>(merit) / TWO_POW48;
    BOOST_CHECK_MESSAGE(std::abs(merit_d - 1.9138) < 0.001,
        "merit(23,29) = " << merit_d << ", expected ~1.9138");

    // Gap between 89 and 97: gap=8, ln(89)=4.4886..., merit=8/4.4886=1.782
    mpz_set_ui(start, 89);
    mpz_set_ui(end, 97);
    merit = utils.merit(start, end);
    merit_d = static_cast<double>(merit) / TWO_POW48;
    BOOST_CHECK_MESSAGE(std::abs(merit_d - 1.7823) < 0.001,
        "merit(89,97) = " << merit_d << ", expected ~1.7823");

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_CASE(merit_known_record_gaps)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // Thomas R. Nicely's record gap #1 (as of 2004):
    // Start: 1693182318746371 (first prime after 1693182318746370)
    // This is actually 1693182318746371 which is prime
    // Gap: 1132 (next prime is 1693182318747503)
    // Merit ≈ 1132 / ln(1693182318746371) ≈ 1132 / 35.07 ≈ 32.28
    mpz_set_str(start, "1693182318746371", 10);

    // Verify it's actually prime
    BOOST_REQUIRE_MESSAGE(mpz_probab_prime_p(start, 25) > 0,
        "Test start value 1693182318746371 should be prime");

    mpz_set_str(end, "1693182318747503", 10);
    BOOST_REQUIRE_MESSAGE(mpz_probab_prime_p(end, 25) > 0,
        "Test end value 1693182318747503 should be prime");

    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;

    // Gap = 1132, merit should be ~32.28
    BOOST_CHECK_MESSAGE(merit_d > 32.0 && merit_d < 33.0,
        "merit for Nicely gap = " << merit_d << ", expected ~32.28");

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_CASE(merit_256bit_primes)
{
    PoWUtils utils;
    mpz_t start, end, gap;
    mpz_init(start);
    mpz_init(end);
    mpz_init(gap);

    // Create a 256-bit prime: 2^255 + 95
    mpz_set_ui(start, 1);
    mpz_mul_2exp(start, start, 255);
    mpz_add_ui(start, start, 95);
    BOOST_REQUIRE_MESSAGE(mpz_probab_prime_p(start, 25) > 0,
        "2^255 + 95 should be prime");

    // Find next prime
    mpz_nextprime(end, start);

    // Calculate gap
    mpz_sub(gap, end, start);
    uint64_t gap_size = mpz_get_ui64(gap);

    // Merit should be gap / (255 * ln(2)) ≈ gap / 176.75
    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;
    double expected = static_cast<double>(gap_size) / (255.0 * std::log(2.0));

    BOOST_CHECK_MESSAGE(std::abs(merit_d - expected) < 0.1,
        "256-bit merit = " << merit_d << ", expected ~" << expected);

    mpz_clear(start);
    mpz_clear(end);
    mpz_clear(gap);
}

/*============================================================================
 * Random generation tests (SHA256d XOR-folded)
 *
 * The random function must be deterministic for consensus.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(rand_deterministic)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    mpz_set_ui(start, 1000003);  // Prime
    mpz_set_ui(end, 1000033);    // Next prime

    // Same inputs must produce same output
    uint64_t r1 = utils.rand(start, end);
    uint64_t r2 = utils.rand(start, end);
    BOOST_CHECK_EQUAL(r1, r2);

    // Different inputs must (almost certainly) produce different output
    mpz_set_ui(end, 1000037);  // Different prime
    uint64_t r3 = utils.rand(start, end);
    BOOST_CHECK_NE(r1, r3);

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_CASE(rand_sha256_verification)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // Small primes for manual verification
    mpz_set_ui(start, 7);
    mpz_set_ui(end, 11);

    // The rand function computes SHA256d(start || end) and XOR-folds to 64 bits
    // start = 7 = 0x07 (1 byte little-endian)
    // end = 11 = 0x0B (1 byte little-endian)

    uint8_t data[] = {0x07, 0x0B};
    uint8_t tmp[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(data, 2).Finalize(tmp);
    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(tmp, CSHA256::OUTPUT_SIZE).Finalize(hash);

    const uint64_t* ptr = reinterpret_cast<const uint64_t*>(hash);
    uint64_t expected = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];

    uint64_t actual = utils.rand(start, end);
    BOOST_CHECK_EQUAL(actual, expected);

    mpz_clear(start);
    mpz_clear(end);
}

/*============================================================================
 * Difficulty calculation tests
 *
 * difficulty = merit + (rand % min_gap_distance_merit)
 * where min_gap_distance_merit = 2 / ln(start) * 2^48
 *============================================================================*/

BOOST_AUTO_TEST_CASE(difficulty_exceeds_merit)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // For any valid gap, difficulty >= merit
    mpz_set_ui(start, 1000003);
    mpz_nextprime(end, start);

    uint64_t merit = utils.merit(start, end);
    uint64_t diff = utils.difficulty(start, end);

    BOOST_CHECK_GE(diff, merit);

    // The random term is bounded by 2/ln(start), so difficulty < merit + 2/ln(start)
    double ln_start = std::log(1000003.0);
    double max_random_merit = 2.0 / ln_start;
    uint64_t max_random = static_cast<uint64_t>(max_random_merit * TWO_POW48);

    BOOST_CHECK_LE(diff, merit + max_random);

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_CASE(difficulty_deterministic)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    mpz_set_ui(start, 104729);  // 10000th prime
    mpz_nextprime(end, start);

    uint64_t d1 = utils.difficulty(start, end);
    uint64_t d2 = utils.difficulty(start, end);
    BOOST_CHECK_EQUAL(d1, d2);

    mpz_clear(start);
    mpz_clear(end);
}

/*============================================================================
 * Target size calculation tests
 *
 * target_size = difficulty * ln(start)
 *============================================================================*/

BOOST_AUTO_TEST_CASE(target_size_consistency)
{
    PoWUtils utils;
    mpz_t start;
    mpz_init(start);

    // For a 256-bit number, ln(start) ≈ 177
    // target_size(difficulty=1.0) ≈ 177
    mpz_set_ui(start, 1);
    mpz_mul_2exp(start, start, 255);

    uint64_t target = utils.target_size(start, TWO_POW48);  // difficulty = 1.0
    double target_d = static_cast<double>(target);

    // Expected: 255 * ln(2) ≈ 176.75
    BOOST_CHECK_MESSAGE(target_d > 170 && target_d < 180,
        "target_size for 256-bit, difficulty=1.0 = " << target_d << ", expected ~177");

    mpz_clear(start);
}

/*============================================================================
 * Difficulty adjustment (next_difficulty) tests
 *
 * Uses logarithmic adjustment with asymmetric damping:
 * - Increases: 1/256 of adjustment
 * - Decreases: 1/64 of adjustment
 * - Clamped to ±1.0 merit per block
 *============================================================================*/

BOOST_AUTO_TEST_CASE(next_difficulty_on_target)
{
    PoWUtils utils;

    // When actual_timespan == target (150s), difficulty should barely change
    uint64_t diff = 20 * TWO_POW48;  // 20.0 merit
    uint64_t next = utils.next_difficulty(diff, 150, false);

    // Should be very close to original (small rounding only)
    int64_t delta = static_cast<int64_t>(next) - static_cast<int64_t>(diff);
    double delta_merit = static_cast<double>(delta) / TWO_POW48;

    BOOST_CHECK_MESSAGE(std::abs(delta_merit) < 0.001,
        "On-target difficulty change = " << delta_merit << ", expected ~0");
}

BOOST_AUTO_TEST_CASE(next_difficulty_slow_blocks)
{
    PoWUtils utils;

    // Block took 300s (2x target) - difficulty should decrease
    uint64_t diff = 20 * TWO_POW48;  // 20.0 merit
    uint64_t next = utils.next_difficulty(diff, 300, false);

    BOOST_CHECK_LT(next, diff);

    // Decrease is damped by 1/64, so: next ≈ diff - ln(2)/64 * 2^48
    double expected_delta = -std::log(2.0) / 64.0;
    double actual_delta = static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(diff)) / TWO_POW48;

    BOOST_CHECK_MESSAGE(std::abs(actual_delta - expected_delta) < 0.01,
        "Slow block delta = " << actual_delta << ", expected ~" << expected_delta);
}

BOOST_AUTO_TEST_CASE(next_difficulty_fast_blocks)
{
    PoWUtils utils;

    // Block took 75s (0.5x target) - difficulty should increase
    uint64_t diff = 20 * TWO_POW48;  // 20.0 merit
    uint64_t next = utils.next_difficulty(diff, 75, false);

    BOOST_CHECK_GT(next, diff);

    // Increase is damped by 1/256, so: next ≈ diff + ln(2)/256 * 2^48
    double expected_delta = std::log(2.0) / 256.0;
    double actual_delta = static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(diff)) / TWO_POW48;

    BOOST_CHECK_MESSAGE(std::abs(actual_delta - expected_delta) < 0.01,
        "Fast block delta = " << actual_delta << ", expected ~" << expected_delta);
}

BOOST_AUTO_TEST_CASE(next_difficulty_clamp_maximum_increase)
{
    PoWUtils utils;

    // Extremely fast block (1 second) - should be clamped to +1.0 merit
    uint64_t diff = 20 * TWO_POW48;
    uint64_t next = utils.next_difficulty(diff, 1, false);

    BOOST_CHECK_LE(next, diff + TWO_POW48);
}

BOOST_AUTO_TEST_CASE(next_difficulty_clamp_maximum_decrease)
{
    PoWUtils utils;

    // Extremely slow block (1 hour) - should be clamped to -1.0 merit
    uint64_t diff = 20 * TWO_POW48;
    uint64_t next = utils.next_difficulty(diff, 3600, false);

    BOOST_CHECK_GE(next, diff - TWO_POW48);
}

BOOST_AUTO_TEST_CASE(next_difficulty_minimum_enforced)
{
    PoWUtils utils;

    // Very low difficulty with slow blocks should hit minimum
    uint64_t diff = MIN_DIFFICULTY;
    uint64_t next = utils.next_difficulty(diff, 3600, false);

    BOOST_CHECK_GE(next, MIN_DIFFICULTY);
}

/*============================================================================
 * PoW validation tests
 *============================================================================*/

BOOST_AUTO_TEST_CASE(pow_shift_bounds)
{
    // Shift must be in [MIN_SHIFT, MAX_SHIFT]
    mpz_t hash, adder;
    mpz_init_set_ui(hash, 0);
    mpz_init_set_ui(adder, 0);

    // 256-bit hash
    mpz_set_ui(hash, 1);
    mpz_mul_2exp(hash, hash, 255);

    // Shift too small (< MIN_SHIFT=14)
    PoW pow_small(hash, 13, adder, TWO_POW48, 0);
    BOOST_CHECK(!pow_small.valid());

    // Shift at minimum
    PoW pow_min(hash, MIN_SHIFT, adder, TWO_POW48, 0);
    // (validity depends on whether resulting number is prime)

    // Shift at maximum
    PoW pow_max(hash, MAX_SHIFT, adder, TWO_POW48, 0);
    // (validity depends on whether resulting number is prime)

    mpz_clear(hash);
    mpz_clear(adder);
}

BOOST_AUTO_TEST_CASE(pow_readable_difficulty_conversion)
{
    // Test the static conversion function
    BOOST_CHECK_CLOSE(PoWUtils::get_readable_difficulty(TWO_POW48), 1.0, 0.0001);
    BOOST_CHECK_CLOSE(PoWUtils::get_readable_difficulty(10 * TWO_POW48), 10.0, 0.0001);
    BOOST_CHECK_CLOSE(PoWUtils::get_readable_difficulty(TWO_POW48 / 2), 0.5, 0.0001);
}

/*============================================================================
 * Edge case tests
 *============================================================================*/

BOOST_AUTO_TEST_CASE(merit_gap_size_one)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // Twin primes: 11 and 13, gap=2
    mpz_set_ui(start, 11);
    mpz_set_ui(end, 13);
    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;

    // merit = 2 / ln(11) = 2 / 2.3979 ≈ 0.834
    BOOST_CHECK_MESSAGE(std::abs(merit_d - 0.834) < 0.01,
        "Twin prime merit = " << merit_d << ", expected ~0.834");

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_CASE(merit_twin_primes_boundary)
{
    PoWUtils utils;
    mpz_t start, end;
    mpz_init(start);
    mpz_init(end);

    // Large twin primes: 1000000007 and next prime
    mpz_set_ui(start, 1000000007);
    BOOST_REQUIRE(mpz_probab_prime_p(start, 25) > 0);
    mpz_nextprime(end, start);

    uint64_t merit = utils.merit(start, end);
    double merit_d = static_cast<double>(merit) / TWO_POW48;

    // merit should be positive and reasonable
    BOOST_CHECK_GT(merit_d, 0.0);
    BOOST_CHECK_LT(merit_d, 100.0);

    mpz_clear(start);
    mpz_clear(end);
}

BOOST_AUTO_TEST_SUITE_END()
