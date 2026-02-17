// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * SIMD-Accelerated Pre-sieving for Freycoin Mining
 *
 * Based on techniques from Kim Walisch's primesieve:
 * https://github.com/kimwalisch/primesieve
 *
 * This implementation provides:
 * - Extended presieve covering primes up to 163 (vs original 13)
 * - AVX-512, AVX2, and SSE2 optimized pattern application
 * - Runtime CPU feature detection for dispatch
 *
 * Performance impact: Up to 30% speedup in sieving phase.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_SIMD_PRESIEVE_H
#define FREYCOIN_POW_SIMD_PRESIEVE_H

#include <cstdint>
#include <cstddef>
#include <cstring>

#include <gmp.h>

#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h>
#endif

/* Platform-specific SIMD headers */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

/*============================================================================
 * CPU Feature Detection
 *============================================================================*/

/** CPU feature flags (detected at runtime) */
struct CpuFeatures {
    bool has_sse2{false};
    bool has_avx2{false};
    bool has_avx512f{false};
    bool has_avx512bw{false};
    bool has_popcnt{false};
    bool detected{false};
};

/** Global CPU features (initialized on first use) */
extern CpuFeatures g_cpu_features;

/** Detect CPU features at runtime (thread-safe, idempotent) */
void detect_cpu_features();

/** Get pointer to detected features */
const CpuFeatures* get_cpu_features();

/*============================================================================
 * Extended Pre-sieve Tables
 *
 * primesieve uses 16 tables covering primes up to 163.
 * We use a similar structure but optimized for our odd-only representation.
 *
 * Table structure:
 *   - Each table is for a pair of primes with coprime periods
 *   - Tables are combined via bitwise OR during presieve application
 *   - Total memory: ~16KB (fits in L1 cache)
 *============================================================================*/

static constexpr int PRESIEVE_NUM_TABLES = 16;
static constexpr int PRESIEVE_MAX_PRIME = 163;

/** Table size info */
struct PresieveTableInfo {
    uint32_t period;           // Period in odd numbers (= product of table's primes)
    uint32_t byte_size;        // Table size in bytes (= period, for correct byte-level wrapping)
    const uint32_t* primes;    // Array of primes for this table
    uint8_t num_primes;        // Number of primes
};

/** Pre-computed presieve tables (generated at startup) */
extern const uint8_t* presieve_tables[PRESIEVE_NUM_TABLES];
extern const PresieveTableInfo presieve_info[PRESIEVE_NUM_TABLES];

/** Total size of all presieve tables in bytes */
extern const uint32_t presieve_total_bytes;

/*============================================================================
 * SIMD Pre-sieve Application Functions
 *============================================================================*/

/**
 * Initialize sieve segment with presieve pattern (Phase 1)
 *
 * Combines tables 0-3 via OR and writes result to sieve.
 * This initializes the sieve, overwriting any previous content.
 *
 * @param sieve       Output sieve buffer (should be 64-byte aligned)
 * @param sieve_bytes Size of sieve buffer in bytes
 * @param segment_low Segment start offset (for table position calculation)
 */
void presieve_init(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);

/**
 * Apply additional presieve tables (Phase 2)
 *
 * ORs tables 4-15 with existing sieve content.
 *
 * @param sieve       Sieve buffer
 * @param sieve_bytes Size of sieve buffer in bytes
 * @param segment_low Segment start offset
 */
void presieve_apply(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);

/**
 * Combined presieve (both phases)
 * Convenience function that calls presieve_init then presieve_apply
 */
void presieve_full(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);

/**
 * Set presieve base byte offsets for a new mpz_start using GMP.
 *
 * Must be called before presieve_init/apply when mpz_start changes.
 * Computes (mpz_start/2) mod (byte_size*8) / 8 for each table, so
 * that get_table_position() returns correct byte positions for the
 * odd-only sieve segment.
 *
 * @param mpz_start The sieve starting number (must be even)
 */
void presieve_set_base_offsets(mpz_t mpz_start);

/*============================================================================
 * SIMD Implementation Functions (Internal)
 *============================================================================*/

/* AVX-512 implementations (64 bytes per iteration) */
#if defined(__AVX512F__) && defined(__AVX512BW__)
void presieve_init_avx512(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
void presieve_apply_avx512(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
#endif

/* AVX2 implementations (32 bytes per iteration) */
#if defined(__AVX2__)
void presieve_init_avx2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
void presieve_apply_avx2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
#endif

/* SSE2 implementations (16 bytes per iteration) */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
void presieve_init_sse2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
void presieve_apply_sse2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
#endif

/* Portable implementation (8 bytes per iteration) */
void presieve_init_default(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);
void presieve_apply_default(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low);

/*============================================================================
 * Table Generation
 *============================================================================*/

/** Generate presieve tables (call once at startup) */
void presieve_generate_tables();

/** Free presieve tables */
void presieve_free_tables();

/** Check if tables are initialized */
bool presieve_tables_ready();

#endif // FREYCOIN_POW_SIMD_PRESIEVE_H
