// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * SIMD-Accelerated Pre-sieving Implementation
 *
 * Based on techniques from Kim Walisch's primesieve.
 * See simd_presieve.h for detailed documentation.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/simd_presieve.h>

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

/*============================================================================
 * CPU Feature Detection
 *============================================================================*/

CpuFeatures g_cpu_features;

#ifdef _WIN32
static void run_cpuid(int leaf, int subleaf, int* eax, int* ebx, int* ecx, int* edx) {
    int regs[4];
    __cpuidex(regs, leaf, subleaf);
    *eax = regs[0];
    *ebx = regs[1];
    *ecx = regs[2];
    *edx = regs[3];
}
#else
static void run_cpuid(int leaf, int subleaf, int* eax, int* ebx, int* ecx, int* edx) {
    __cpuid_count(leaf, subleaf, *eax, *ebx, *ecx, *edx);
}
#endif

/* Portable xgetbv implementation */
static unsigned long long portable_xgetbv(unsigned int index) {
#if defined(_MSC_VER)
    return _xgetbv(index);
#else
    unsigned int xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(index));
    return ((unsigned long long)xcr0_hi << 32) | xcr0_lo;
#endif
}

/* Check if OS supports AVX/AVX2/AVX-512 state saving */
static bool os_supports_avx() {
    int eax, ebx, ecx, edx;
    run_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if ((ecx & (1 << 27)) == 0) {
        return false;
    }
    unsigned long long xcr0 = portable_xgetbv(0);
    return (xcr0 & 0x6) == 0x6;
}

static bool os_supports_avx512() {
    if (!os_supports_avx()) {
        return false;
    }
    unsigned long long xcr0 = portable_xgetbv(0);
    return (xcr0 & 0xE6) == 0xE6;
}

void detect_cpu_features() {
    if (g_cpu_features.detected) {
        return;
    }

    int eax, ebx, ecx, edx;

    /* CPUID leaf 1: Basic features */
    run_cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    /* SSE2 is in EDX bit 26 */
    g_cpu_features.has_sse2 = (edx & (1 << 26)) != 0;

    /* POPCNT is in ECX bit 23 */
    g_cpu_features.has_popcnt = (ecx & (1 << 23)) != 0;

    /* Check for extended features (leaf 7) */
    run_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 7) {
        run_cpuid(7, 0, &eax, &ebx, &ecx, &edx);

        /* AVX2 is in EBX bit 5 */
        if ((ebx & (1 << 5)) && os_supports_avx()) {
            g_cpu_features.has_avx2 = true;
        }

        /* AVX-512F is in EBX bit 16, AVX-512BW is in EBX bit 30 */
        if ((ebx & (1 << 16)) && (ebx & (1 << 30)) && os_supports_avx512()) {
            g_cpu_features.has_avx512f = true;
            g_cpu_features.has_avx512bw = true;
        }
    }

    g_cpu_features.detected = true;
}

const CpuFeatures* get_cpu_features() {
    detect_cpu_features();
    return &g_cpu_features;
}

/*============================================================================
 * Pre-sieve Table Definitions
 *============================================================================*/

/* Prime assignments for each table */
static const uint32_t table0_primes[] = {7, 23, 37};
static const uint32_t table1_primes[] = {11, 19, 31};
static const uint32_t table2_primes[] = {13, 17, 29};
static const uint32_t table3_primes[] = {41, 163};
static const uint32_t table4_primes[] = {43, 157};
static const uint32_t table5_primes[] = {47, 151};
static const uint32_t table6_primes[] = {53, 149};
static const uint32_t table7_primes[] = {59, 139};
static const uint32_t table8_primes[] = {61, 137};
static const uint32_t table9_primes[] = {67, 131};
static const uint32_t table10_primes[] = {71, 127};
static const uint32_t table11_primes[] = {73, 113};
static const uint32_t table12_primes[] = {79, 109};
static const uint32_t table13_primes[] = {83, 107};
static const uint32_t table14_primes[] = {89, 103};
static const uint32_t table15_primes[] = {97, 101};

const PresieveTableInfo presieve_info[PRESIEVE_NUM_TABLES] = {
    {5957, (5957 + 7) / 8, table0_primes, 3},
    {6479, (6479 + 7) / 8, table1_primes, 3},
    {6409, (6409 + 7) / 8, table2_primes, 3},
    {6683, (6683 + 7) / 8, table3_primes, 2},
    {6751, (6751 + 7) / 8, table4_primes, 2},
    {7097, (7097 + 7) / 8, table5_primes, 2},
    {7897, (7897 + 7) / 8, table6_primes, 2},
    {8201, (8201 + 7) / 8, table7_primes, 2},
    {8357, (8357 + 7) / 8, table8_primes, 2},
    {8777, (8777 + 7) / 8, table9_primes, 2},
    {9017, (9017 + 7) / 8, table10_primes, 2},
    {8249, (8249 + 7) / 8, table11_primes, 2},
    {8611, (8611 + 7) / 8, table12_primes, 2},
    {8881, (8881 + 7) / 8, table13_primes, 2},
    {9167, (9167 + 7) / 8, table14_primes, 2},
    {9797, (9797 + 7) / 8, table15_primes, 2}
};

const uint32_t presieve_total_bytes = 15801;

/* Actual table data (dynamically allocated) */
static uint8_t* presieve_table_data[PRESIEVE_NUM_TABLES] = {nullptr};
const uint8_t* presieve_tables[PRESIEVE_NUM_TABLES] = {nullptr};

static bool tables_initialized = false;

/*============================================================================
 * Table Generation
 *============================================================================*/

static void generate_table(int table_idx) {
    const PresieveTableInfo* info = &presieve_info[table_idx];
    uint32_t period = info->period;
    uint32_t byte_size = info->byte_size;

    /* Allocate with 64-byte alignment for SIMD */
    uint32_t alloc_size = ((byte_size + 63) / 64) * 64;
    uint8_t* table = (uint8_t*)aligned_alloc(64, alloc_size);
    if (!table) {
        return;
    }

    /* Initialize to all zeros (all positions potentially prime) */
    std::memset(table, 0, alloc_size);

    /* Mark multiples of each prime */
    for (int p = 0; p < info->num_primes; p++) {
        uint32_t prime = info->primes[p];
        uint32_t start_bit = (3 * prime - 1) / 2;

        for (uint32_t pos = start_bit; pos < period; pos += prime) {
            table[pos / 8] |= (1 << (pos % 8));
        }
    }

    presieve_table_data[table_idx] = table;
    presieve_tables[table_idx] = table;
}

void presieve_generate_tables() {
    if (tables_initialized) {
        return;
    }

    for (int i = 0; i < PRESIEVE_NUM_TABLES; i++) {
        generate_table(i);
    }

    tables_initialized = true;
}

void presieve_free_tables() {
    for (int i = 0; i < PRESIEVE_NUM_TABLES; i++) {
        if (presieve_table_data[i]) {
            aligned_free(presieve_table_data[i]);
            presieve_table_data[i] = nullptr;
            presieve_tables[i] = nullptr;
        }
    }
    tables_initialized = false;
}

bool presieve_tables_ready() {
    return tables_initialized;
}

/*============================================================================
 * Default (Portable) Implementation
 *============================================================================*/

static inline uint32_t get_table_position(int table_idx, uint64_t segment_low) {
    uint32_t period = presieve_info[table_idx].period;
    return (uint32_t)(segment_low % period);
}

void presieve_init_default(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) {
        presieve_generate_tables();
    }

    uint32_t pos0 = get_table_position(0, segment_low);
    uint32_t pos1 = get_table_position(1, segment_low);
    uint32_t pos2 = get_table_position(2, segment_low);
    uint32_t pos3 = get_table_position(3, segment_low);

    const uint8_t* t0 = presieve_tables[0];
    const uint8_t* t1 = presieve_tables[1];
    const uint8_t* t2 = presieve_tables[2];
    const uint8_t* t3 = presieve_tables[3];

    uint32_t size0 = presieve_info[0].byte_size;
    uint32_t size1 = presieve_info[1].byte_size;
    uint32_t size2 = presieve_info[2].byte_size;
    uint32_t size3 = presieve_info[3].byte_size;

    /* Process 8 bytes at a time */
    size_t i = 0;
    for (; i + 8 <= sieve_bytes; i += 8) {
        uint64_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;

        for (int b = 0; b < 8; b++) {
            v0 |= ((uint64_t)t0[pos0] << (b * 8));
            v1 |= ((uint64_t)t1[pos1] << (b * 8));
            v2 |= ((uint64_t)t2[pos2] << (b * 8));
            v3 |= ((uint64_t)t3[pos3] << (b * 8));
            pos0 = (pos0 + 1) % size0;
            pos1 = (pos1 + 1) % size1;
            pos2 = (pos2 + 1) % size2;
            pos3 = (pos3 + 1) % size3;
        }

        uint64_t result = v0 | v1 | v2 | v3;
        std::memcpy(sieve + i, &result, 8);
    }

    for (; i < sieve_bytes; i++) {
        uint8_t v = t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
        sieve[i] = v;
        pos0 = (pos0 + 1) % size0;
        pos1 = (pos1 + 1) % size1;
        pos2 = (pos2 + 1) % size2;
        pos3 = (pos3 + 1) % size3;
    }
}

void presieve_apply_default(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) {
        return;
    }

    for (int group = 1; group < 4; group++) {
        int base = group * 4;
        if (base >= PRESIEVE_NUM_TABLES) break;

        uint32_t pos0 = get_table_position(base + 0, segment_low);
        uint32_t pos1 = get_table_position(base + 1, segment_low);
        uint32_t pos2 = get_table_position(base + 2, segment_low);
        uint32_t pos3 = get_table_position(base + 3, segment_low);

        const uint8_t* t0 = presieve_tables[base + 0];
        const uint8_t* t1 = presieve_tables[base + 1];
        const uint8_t* t2 = presieve_tables[base + 2];
        const uint8_t* t3 = presieve_tables[base + 3];

        uint32_t size0 = presieve_info[base + 0].byte_size;
        uint32_t size1 = presieve_info[base + 1].byte_size;
        uint32_t size2 = presieve_info[base + 2].byte_size;
        uint32_t size3 = presieve_info[base + 3].byte_size;

        size_t i = 0;
        for (; i + 8 <= sieve_bytes; i += 8) {
            uint64_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;

            for (int b = 0; b < 8; b++) {
                v0 |= ((uint64_t)t0[pos0] << (b * 8));
                v1 |= ((uint64_t)t1[pos1] << (b * 8));
                v2 |= ((uint64_t)t2[pos2] << (b * 8));
                v3 |= ((uint64_t)t3[pos3] << (b * 8));
                pos0 = (pos0 + 1) % size0;
                pos1 = (pos1 + 1) % size1;
                pos2 = (pos2 + 1) % size2;
                pos3 = (pos3 + 1) % size3;
            }

            uint64_t existing;
            std::memcpy(&existing, sieve + i, 8);
            uint64_t result = existing | v0 | v1 | v2 | v3;
            std::memcpy(sieve + i, &result, 8);
        }

        for (; i < sieve_bytes; i++) {
            uint8_t v = t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
            sieve[i] |= v;
            pos0 = (pos0 + 1) % size0;
            pos1 = (pos1 + 1) % size1;
            pos2 = (pos2 + 1) % size2;
            pos3 = (pos3 + 1) % size3;
        }
    }
}

/*============================================================================
 * SSE2 Implementation (16 bytes per iteration)
 *============================================================================*/

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)

static inline __m128i load_cyclic_sse2(const uint8_t* table, uint32_t table_size,
                                        uint32_t* pos) {
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) {
        buf[i] = table[*pos];
        *pos = (*pos + 1) % table_size;
    }
    return _mm_loadu_si128((const __m128i*)buf);
}

void presieve_init_sse2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) {
        presieve_generate_tables();
    }

    uint32_t pos0 = get_table_position(0, segment_low);
    uint32_t pos1 = get_table_position(1, segment_low);
    uint32_t pos2 = get_table_position(2, segment_low);
    uint32_t pos3 = get_table_position(3, segment_low);

    const uint8_t* t0 = presieve_tables[0];
    const uint8_t* t1 = presieve_tables[1];
    const uint8_t* t2 = presieve_tables[2];
    const uint8_t* t3 = presieve_tables[3];

    uint32_t size0 = presieve_info[0].byte_size;
    uint32_t size1 = presieve_info[1].byte_size;
    uint32_t size2 = presieve_info[2].byte_size;
    uint32_t size3 = presieve_info[3].byte_size;

    size_t i = 0;
    for (; i + 16 <= sieve_bytes; i += 16) {
        __m128i v0 = load_cyclic_sse2(t0, size0, &pos0);
        __m128i v1 = load_cyclic_sse2(t1, size1, &pos1);
        __m128i v2 = load_cyclic_sse2(t2, size2, &pos2);
        __m128i v3 = load_cyclic_sse2(t3, size3, &pos3);

        __m128i result = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
        _mm_storeu_si128((__m128i*)(sieve + i), result);
    }

    for (; i < sieve_bytes; i++) {
        sieve[i] = t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
        pos0 = (pos0 + 1) % size0;
        pos1 = (pos1 + 1) % size1;
        pos2 = (pos2 + 1) % size2;
        pos3 = (pos3 + 1) % size3;
    }
}

void presieve_apply_sse2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) return;

    for (int group = 1; group < 4; group++) {
        int base = group * 4;
        if (base >= PRESIEVE_NUM_TABLES) break;

        uint32_t pos0 = get_table_position(base + 0, segment_low);
        uint32_t pos1 = get_table_position(base + 1, segment_low);
        uint32_t pos2 = get_table_position(base + 2, segment_low);
        uint32_t pos3 = get_table_position(base + 3, segment_low);

        const uint8_t* t0 = presieve_tables[base + 0];
        const uint8_t* t1 = presieve_tables[base + 1];
        const uint8_t* t2 = presieve_tables[base + 2];
        const uint8_t* t3 = presieve_tables[base + 3];

        uint32_t size0 = presieve_info[base + 0].byte_size;
        uint32_t size1 = presieve_info[base + 1].byte_size;
        uint32_t size2 = presieve_info[base + 2].byte_size;
        uint32_t size3 = presieve_info[base + 3].byte_size;

        size_t i = 0;
        for (; i + 16 <= sieve_bytes; i += 16) {
            __m128i v0 = load_cyclic_sse2(t0, size0, &pos0);
            __m128i v1 = load_cyclic_sse2(t1, size1, &pos1);
            __m128i v2 = load_cyclic_sse2(t2, size2, &pos2);
            __m128i v3 = load_cyclic_sse2(t3, size3, &pos3);

            __m128i existing = _mm_loadu_si128((const __m128i*)(sieve + i));
            __m128i combined = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
            __m128i result = _mm_or_si128(existing, combined);
            _mm_storeu_si128((__m128i*)(sieve + i), result);
        }

        for (; i < sieve_bytes; i++) {
            sieve[i] |= t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
            pos0 = (pos0 + 1) % size0;
            pos1 = (pos1 + 1) % size1;
            pos2 = (pos2 + 1) % size2;
            pos3 = (pos3 + 1) % size3;
        }
    }
}

#endif /* SSE2 */

/*============================================================================
 * AVX2 Implementation (32 bytes per iteration)
 *============================================================================*/

#if defined(__AVX2__)

static inline __m256i load_cyclic_avx2(const uint8_t* table, uint32_t table_size,
                                        uint32_t* pos) {
    uint8_t buf[32];
    for (int i = 0; i < 32; i++) {
        buf[i] = table[*pos];
        *pos = (*pos + 1) % table_size;
    }
    return _mm256_loadu_si256((const __m256i*)buf);
}

void presieve_init_avx2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) {
        presieve_generate_tables();
    }

    uint32_t pos0 = get_table_position(0, segment_low);
    uint32_t pos1 = get_table_position(1, segment_low);
    uint32_t pos2 = get_table_position(2, segment_low);
    uint32_t pos3 = get_table_position(3, segment_low);

    const uint8_t* t0 = presieve_tables[0];
    const uint8_t* t1 = presieve_tables[1];
    const uint8_t* t2 = presieve_tables[2];
    const uint8_t* t3 = presieve_tables[3];

    uint32_t size0 = presieve_info[0].byte_size;
    uint32_t size1 = presieve_info[1].byte_size;
    uint32_t size2 = presieve_info[2].byte_size;
    uint32_t size3 = presieve_info[3].byte_size;

    size_t i = 0;
    for (; i + 32 <= sieve_bytes; i += 32) {
        __m256i v0 = load_cyclic_avx2(t0, size0, &pos0);
        __m256i v1 = load_cyclic_avx2(t1, size1, &pos1);
        __m256i v2 = load_cyclic_avx2(t2, size2, &pos2);
        __m256i v3 = load_cyclic_avx2(t3, size3, &pos3);

        __m256i result = _mm256_or_si256(_mm256_or_si256(v0, v1), _mm256_or_si256(v2, v3));
        _mm256_storeu_si256((__m256i*)(sieve + i), result);
    }

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
    for (; i + 16 <= sieve_bytes; i += 16) {
        __m128i v0 = load_cyclic_sse2(t0, size0, &pos0);
        __m128i v1 = load_cyclic_sse2(t1, size1, &pos1);
        __m128i v2 = load_cyclic_sse2(t2, size2, &pos2);
        __m128i v3 = load_cyclic_sse2(t3, size3, &pos3);
        __m128i result = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
        _mm_storeu_si128((__m128i*)(sieve + i), result);
    }
#endif

    for (; i < sieve_bytes; i++) {
        sieve[i] = t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
        pos0 = (pos0 + 1) % size0;
        pos1 = (pos1 + 1) % size1;
        pos2 = (pos2 + 1) % size2;
        pos3 = (pos3 + 1) % size3;
    }
}

void presieve_apply_avx2(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) return;

    for (int group = 1; group < 4; group++) {
        int base = group * 4;
        if (base >= PRESIEVE_NUM_TABLES) break;

        uint32_t pos0 = get_table_position(base + 0, segment_low);
        uint32_t pos1 = get_table_position(base + 1, segment_low);
        uint32_t pos2 = get_table_position(base + 2, segment_low);
        uint32_t pos3 = get_table_position(base + 3, segment_low);

        const uint8_t* t0 = presieve_tables[base + 0];
        const uint8_t* t1 = presieve_tables[base + 1];
        const uint8_t* t2 = presieve_tables[base + 2];
        const uint8_t* t3 = presieve_tables[base + 3];

        uint32_t size0 = presieve_info[base + 0].byte_size;
        uint32_t size1 = presieve_info[base + 1].byte_size;
        uint32_t size2 = presieve_info[base + 2].byte_size;
        uint32_t size3 = presieve_info[base + 3].byte_size;

        size_t i = 0;
        for (; i + 32 <= sieve_bytes; i += 32) {
            __m256i v0 = load_cyclic_avx2(t0, size0, &pos0);
            __m256i v1 = load_cyclic_avx2(t1, size1, &pos1);
            __m256i v2 = load_cyclic_avx2(t2, size2, &pos2);
            __m256i v3 = load_cyclic_avx2(t3, size3, &pos3);

            __m256i existing = _mm256_loadu_si256((const __m256i*)(sieve + i));
            __m256i combined = _mm256_or_si256(_mm256_or_si256(v0, v1), _mm256_or_si256(v2, v3));
            __m256i result = _mm256_or_si256(existing, combined);
            _mm256_storeu_si256((__m256i*)(sieve + i), result);
        }

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
        for (; i + 16 <= sieve_bytes; i += 16) {
            __m128i v0 = load_cyclic_sse2(t0, size0, &pos0);
            __m128i v1 = load_cyclic_sse2(t1, size1, &pos1);
            __m128i v2 = load_cyclic_sse2(t2, size2, &pos2);
            __m128i v3 = load_cyclic_sse2(t3, size3, &pos3);
            __m128i existing = _mm_loadu_si128((const __m128i*)(sieve + i));
            __m128i combined = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
            __m128i result = _mm_or_si128(existing, combined);
            _mm_storeu_si128((__m128i*)(sieve + i), result);
        }
#endif

        for (; i < sieve_bytes; i++) {
            sieve[i] |= t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
            pos0 = (pos0 + 1) % size0;
            pos1 = (pos1 + 1) % size1;
            pos2 = (pos2 + 1) % size2;
            pos3 = (pos3 + 1) % size3;
        }
    }
}

#endif /* AVX2 */

/*============================================================================
 * AVX-512 Implementation (64 bytes per iteration)
 *============================================================================*/

#if defined(__AVX512F__) && defined(__AVX512BW__)

static inline __m512i load_cyclic_avx512(const uint8_t* table, uint32_t table_size,
                                          uint32_t* pos) {
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) {
        buf[i] = table[*pos];
        *pos = (*pos + 1) % table_size;
    }
    return _mm512_loadu_si512((const __m512i*)buf);
}

void presieve_init_avx512(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) {
        presieve_generate_tables();
    }

    uint32_t pos0 = get_table_position(0, segment_low);
    uint32_t pos1 = get_table_position(1, segment_low);
    uint32_t pos2 = get_table_position(2, segment_low);
    uint32_t pos3 = get_table_position(3, segment_low);

    const uint8_t* t0 = presieve_tables[0];
    const uint8_t* t1 = presieve_tables[1];
    const uint8_t* t2 = presieve_tables[2];
    const uint8_t* t3 = presieve_tables[3];

    uint32_t size0 = presieve_info[0].byte_size;
    uint32_t size1 = presieve_info[1].byte_size;
    uint32_t size2 = presieve_info[2].byte_size;
    uint32_t size3 = presieve_info[3].byte_size;

    size_t i = 0;
    for (; i + 64 <= sieve_bytes; i += 64) {
        __m512i v0 = load_cyclic_avx512(t0, size0, &pos0);
        __m512i v1 = load_cyclic_avx512(t1, size1, &pos1);
        __m512i v2 = load_cyclic_avx512(t2, size2, &pos2);
        __m512i v3 = load_cyclic_avx512(t3, size3, &pos3);

        __m512i result = _mm512_or_si512(_mm512_or_si512(v0, v1), _mm512_or_si512(v2, v3));
        _mm512_storeu_si512((__m512i*)(sieve + i), result);
    }

#if defined(__AVX2__)
    for (; i + 32 <= sieve_bytes; i += 32) {
        __m256i v0 = load_cyclic_avx2(t0, size0, &pos0);
        __m256i v1 = load_cyclic_avx2(t1, size1, &pos1);
        __m256i v2 = load_cyclic_avx2(t2, size2, &pos2);
        __m256i v3 = load_cyclic_avx2(t3, size3, &pos3);
        __m256i result = _mm256_or_si256(_mm256_or_si256(v0, v1), _mm256_or_si256(v2, v3));
        _mm256_storeu_si256((__m256i*)(sieve + i), result);
    }
#endif

    for (; i < sieve_bytes; i++) {
        sieve[i] = t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
        pos0 = (pos0 + 1) % size0;
        pos1 = (pos1 + 1) % size1;
        pos2 = (pos2 + 1) % size2;
        pos3 = (pos3 + 1) % size3;
    }
}

void presieve_apply_avx512(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    if (!tables_initialized) return;

    for (int group = 1; group < 4; group++) {
        int base = group * 4;
        if (base >= PRESIEVE_NUM_TABLES) break;

        uint32_t pos0 = get_table_position(base + 0, segment_low);
        uint32_t pos1 = get_table_position(base + 1, segment_low);
        uint32_t pos2 = get_table_position(base + 2, segment_low);
        uint32_t pos3 = get_table_position(base + 3, segment_low);

        const uint8_t* t0 = presieve_tables[base + 0];
        const uint8_t* t1 = presieve_tables[base + 1];
        const uint8_t* t2 = presieve_tables[base + 2];
        const uint8_t* t3 = presieve_tables[base + 3];

        uint32_t size0 = presieve_info[base + 0].byte_size;
        uint32_t size1 = presieve_info[base + 1].byte_size;
        uint32_t size2 = presieve_info[base + 2].byte_size;
        uint32_t size3 = presieve_info[base + 3].byte_size;

        size_t i = 0;
        for (; i + 64 <= sieve_bytes; i += 64) {
            __m512i v0 = load_cyclic_avx512(t0, size0, &pos0);
            __m512i v1 = load_cyclic_avx512(t1, size1, &pos1);
            __m512i v2 = load_cyclic_avx512(t2, size2, &pos2);
            __m512i v3 = load_cyclic_avx512(t3, size3, &pos3);

            __m512i existing = _mm512_loadu_si512((const __m512i*)(sieve + i));
            __m512i combined = _mm512_or_si512(_mm512_or_si512(v0, v1), _mm512_or_si512(v2, v3));
            __m512i result = _mm512_or_si512(existing, combined);
            _mm512_storeu_si512((__m512i*)(sieve + i), result);
        }

#if defined(__AVX2__)
        for (; i + 32 <= sieve_bytes; i += 32) {
            __m256i v0 = load_cyclic_avx2(t0, size0, &pos0);
            __m256i v1 = load_cyclic_avx2(t1, size1, &pos1);
            __m256i v2 = load_cyclic_avx2(t2, size2, &pos2);
            __m256i v3 = load_cyclic_avx2(t3, size3, &pos3);
            __m256i existing = _mm256_loadu_si256((const __m256i*)(sieve + i));
            __m256i combined = _mm256_or_si256(_mm256_or_si256(v0, v1), _mm256_or_si256(v2, v3));
            __m256i result = _mm256_or_si256(existing, combined);
            _mm256_storeu_si256((__m256i*)(sieve + i), result);
        }
#endif

        for (; i < sieve_bytes; i++) {
            sieve[i] |= t0[pos0] | t1[pos1] | t2[pos2] | t3[pos3];
            pos0 = (pos0 + 1) % size0;
            pos1 = (pos1 + 1) % size1;
            pos2 = (pos2 + 1) % size2;
            pos3 = (pos3 + 1) % size3;
        }
    }
}

#endif /* AVX-512 */

/*============================================================================
 * Runtime Dispatch Functions
 *============================================================================*/

void presieve_init(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    detect_cpu_features();

#if defined(__AVX512F__) && defined(__AVX512BW__)
    if (g_cpu_features.has_avx512bw) {
        presieve_init_avx512(sieve, sieve_bytes, segment_low);
        return;
    }
#endif

#if defined(__AVX2__)
    if (g_cpu_features.has_avx2) {
        presieve_init_avx2(sieve, sieve_bytes, segment_low);
        return;
    }
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
    if (g_cpu_features.has_sse2) {
        presieve_init_sse2(sieve, sieve_bytes, segment_low);
        return;
    }
#endif

    presieve_init_default(sieve, sieve_bytes, segment_low);
}

void presieve_apply(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    detect_cpu_features();

#if defined(__AVX512F__) && defined(__AVX512BW__)
    if (g_cpu_features.has_avx512bw) {
        presieve_apply_avx512(sieve, sieve_bytes, segment_low);
        return;
    }
#endif

#if defined(__AVX2__)
    if (g_cpu_features.has_avx2) {
        presieve_apply_avx2(sieve, sieve_bytes, segment_low);
        return;
    }
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
    if (g_cpu_features.has_sse2) {
        presieve_apply_sse2(sieve, sieve_bytes, segment_low);
        return;
    }
#endif

    presieve_apply_default(sieve, sieve_bytes, segment_low);
}

void presieve_full(uint8_t* sieve, size_t sieve_bytes, uint64_t segment_low) {
    presieve_init(sieve, sieve_bytes, segment_low);
    presieve_apply(sieve, sieve_bytes, segment_low);
}
