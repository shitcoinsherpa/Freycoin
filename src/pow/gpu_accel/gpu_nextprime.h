// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_POW_GPU_NEXTPRIME_H
#define FREYCOIN_POW_GPU_NEXTPRIME_H

#include <gmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize GPU for nextprime computation. Returns 0 on success. */
int gpu_nextprime_init(int device_id);

/** Find the next prime after n using GPU batch BPSW. Returns 0 on success. */
int gpu_nextprime(mpz_ptr result, mpz_srcptr n);

/** Release GPU resources. */
void gpu_nextprime_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_POW_GPU_NEXTPRIME_H
