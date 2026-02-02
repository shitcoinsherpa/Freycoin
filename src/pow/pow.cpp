// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * PoW class implementation.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/pow.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>

PoW::PoW(mpz_t hash, uint16_t shift, mpz_t adder, uint64_t difficulty, uint32_t nonce)
    : nonce(nonce), shift(shift), target_difficulty(difficulty) {
    mpz_init_set(mpz_hash, hash);
    mpz_init_set(mpz_adder, adder);
    utils = new PoWUtils();
}

PoW::PoW(const std::vector<uint8_t>& hash, uint16_t shift,
         const std::vector<uint8_t>& adder, uint64_t difficulty, uint32_t nonce)
    : nonce(nonce), shift(shift), target_difficulty(difficulty) {
    mpz_init(mpz_hash);
    mpz_init(mpz_adder);

    if (!hash.empty()) {
        ary_to_mpz(mpz_hash, hash.data(), hash.size());
    }
    if (!adder.empty()) {
        ary_to_mpz(mpz_adder, adder.data(), adder.size());
    }

    utils = new PoWUtils();
}

PoW::~PoW() {
    mpz_clear(mpz_hash);
    mpz_clear(mpz_adder);
    delete utils;
}

bool PoW::get_end_points(mpz_t mpz_start, mpz_t mpz_end) {
    // Validate shift range
    if (shift < MIN_SHIFT || shift > MAX_SHIFT) {
        return false;
    }

    // Verify hash is 256 bits
    size_t hash_bits = mpz_sizeinbase(mpz_hash, 2);
    if (hash_bits != 256) {
        return false;
    }

    // Verify adder < 2^shift
    size_t adder_bits = mpz_sizeinbase(mpz_adder, 2);
    if (adder_bits > shift) {
        return false;
    }

    // Construct start = hash * 2^shift + adder
    mpz_set(mpz_start, mpz_hash);
    mpz_mul_2exp(mpz_start, mpz_start, shift);
    mpz_add(mpz_start, mpz_start, mpz_adder);

    // Verify start is prime (BPSW + 25 Miller-Rabin rounds)
    if (mpz_probab_prime_p(mpz_start, 25) == 0) {
        return false;
    }

    // Find next prime
    mpz_nextprime(mpz_end, mpz_start);

    return true;
}

uint64_t PoW::difficulty() {
    mpz_t mpz_start, mpz_end;
    mpz_init(mpz_start);
    mpz_init(mpz_end);

    uint64_t diff = 0;
    if (get_end_points(mpz_start, mpz_end)) {
        diff = utils->difficulty(mpz_start, mpz_end);
    }

    mpz_clear(mpz_start);
    mpz_clear(mpz_end);
    return diff;
}

uint64_t PoW::merit() {
    mpz_t mpz_start, mpz_end;
    mpz_init(mpz_start);
    mpz_init(mpz_end);

    uint64_t m = 0;
    if (get_end_points(mpz_start, mpz_end)) {
        m = utils->merit(mpz_start, mpz_end);
    }

    mpz_clear(mpz_start);
    mpz_clear(mpz_end);
    return m;
}

bool PoW::get_gap(std::vector<uint8_t>& start, std::vector<uint8_t>& end) {
    mpz_t mpz_start, mpz_end;
    mpz_init(mpz_start);
    mpz_init(mpz_end);

    bool ok = get_end_points(mpz_start, mpz_end);
    if (ok) {
        size_t start_len, end_len;
        uint8_t* start_ary = mpz_to_ary(mpz_start, &start_len);
        uint8_t* end_ary = mpz_to_ary(mpz_end, &end_len);

        start.assign(start_ary, start_ary + start_len);
        end.assign(end_ary, end_ary + end_len);

        free(start_ary);
        free(end_ary);
    }

    mpz_clear(mpz_start);
    mpz_clear(mpz_end);
    return ok;
}

uint64_t PoW::gap_len() {
    mpz_t mpz_start, mpz_end, mpz_len;
    mpz_init(mpz_start);
    mpz_init(mpz_end);
    mpz_init(mpz_len);

    uint64_t len = 0;
    if (get_end_points(mpz_start, mpz_end)) {
        mpz_sub(mpz_len, mpz_end, mpz_start);
        if (mpz_fits_ui64(mpz_len)) {
            len = mpz_get_ui64(mpz_len);
        }
    }

    mpz_clear(mpz_start);
    mpz_clear(mpz_end);
    mpz_clear(mpz_len);
    return len;
}

bool PoW::valid() {
    return difficulty() >= target_difficulty;
}

uint64_t PoW::target_size(mpz_t mpz_start) {
    return utils->target_size(mpz_start, target_difficulty);
}

void PoW::get_hash(mpz_t result) {
    mpz_set(result, mpz_hash);
}

void PoW::get_adder(mpz_t result) {
    mpz_set(result, mpz_adder);
}

void PoW::get_adder(std::vector<uint8_t>& result) {
    size_t len;
    uint8_t* ary = mpz_to_ary(mpz_adder, &len);
    result.assign(ary, ary + len);
    free(ary);
}

void PoW::set_adder(mpz_t adder) {
    mpz_set(mpz_adder, adder);
}

void PoW::set_adder(const std::vector<uint8_t>& adder) {
    if (!adder.empty()) {
        ary_to_mpz(mpz_adder, adder.data(), adder.size());
    } else {
        mpz_set_ui(mpz_adder, 0);
    }
}

std::string PoW::to_string() {
    std::ostringstream oss;

    char* hash_str = mpz_get_str(nullptr, 16, mpz_hash);
    char* adder_str = mpz_get_str(nullptr, 16, mpz_adder);

    oss << "PoW{"
        << "nonce=" << nonce
        << ", shift=" << shift
        << ", hash=0x" << hash_str
        << ", adder=0x" << adder_str
        << ", target=" << std::fixed << std::setprecision(6)
        << PoWUtils::get_readable_difficulty(target_difficulty)
        << ", achieved=" << std::fixed << std::setprecision(6)
        << PoWUtils::get_readable_difficulty(difficulty())
        << ", gap=" << gap_len()
        << ", valid=" << (valid() ? "true" : "false")
        << "}";

    free(hash_str);
    free(adder_str);

    return oss.str();
}
