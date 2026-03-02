// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_POW_GPU_COORDINATOR_H
#define FREYCOIN_POW_GPU_COORDINATOR_H

#include <shared_mutex>

// Shared mutex for GPU access coordination between mining and validation.
//
// Mining GPU workers hold shared locks (concurrent mining OK).
// Block validation (fast_nextprime GPU path) holds an exclusive lock,
// pausing mining for ~9s while GPU BPSW runs, then releasing.
//
// This prevents CUDA context contention on the same device.
extern std::shared_mutex g_gpu_access;

#endif // FREYCOIN_POW_GPU_COORDINATOR_H
