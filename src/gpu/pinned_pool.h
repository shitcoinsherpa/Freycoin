// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_GPU_PINNED_POOL_H
#define FREYCOIN_GPU_PINNED_POOL_H

/**
 * Pool of page-locked (pinned) host buffers for GPU async transfers.
 *
 * cuMemcpyHtoDAsync and cuMemcpyDtoHAsync only actually overlap with kernel
 * execution if the host pointer is page-locked — otherwise the driver
 * silently degrades to a synchronous staged copy. Re-allocating via
 * cuMemHostAlloc per batch costs ~500us-1ms each, dominating short kernels.
 *
 * This pool allocates a fixed number of equally-sized slots once at engine
 * start (lazy on first use). acquire() blocks if all slots are in flight,
 * which provides natural back-pressure across producers. release() must be
 * called from the consumer thread (or its host-completion callback) after
 * the slot's data is no longer needed by either the GPU or the CPU
 * post-processing step.
 *
 * Thread-safe.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gpu_pool {

struct PinnedSlot {
    void*    data{nullptr};
    size_t   size{0};
    int      index{-1};
    /** Last batch's logical count written to this slot, for telemetry. */
    uint32_t last_count{0};
};

class PinnedPool {
public:
    PinnedPool() = default;
    ~PinnedPool() { reset(); }

    /**
     * Reserve `count` slots of `slot_bytes` each. Returns true on success.
     * If pool already initialized at the same shape, returns true as a no-op.
     * If CUDA is not available or cuMemHostAlloc fails for any slot, returns
     * false; caller falls back to plain malloc'd buffers.
     */
    bool reserve(size_t count, size_t slot_bytes);

    /**
     * Block until a slot is available; returns its index. The pool retains
     * ownership of the underlying memory — caller must release().
     */
    PinnedSlot* acquire();

    /**
     * Non-blocking acquire. Returns nullptr if no slot is immediately free.
     */
    PinnedSlot* try_acquire();

    /** Return a slot to the pool. Must match a prior acquire(). */
    void release(PinnedSlot* slot);

    /** Free all underlying memory. Outstanding slots become invalid. */
    void reset();

    size_t total_slots()   const { return m_slots.size(); }
    size_t slot_size()     const { return m_slot_bytes; }
    size_t in_use_now()    const { return m_in_use.load(std::memory_order_relaxed); }
    size_t max_in_use()    const { return m_max_in_use.load(std::memory_order_relaxed); }
    /** Cumulative count of acquire() calls that had to wait on a slot. */
    size_t blocking_waits() const { return m_blocking_waits.load(std::memory_order_relaxed); }

private:
    std::mutex                    m_mu;
    std::condition_variable       m_cv;
    std::vector<PinnedSlot>       m_slots;
    std::vector<int>              m_free_list;       // indices of free slots
    size_t                        m_slot_bytes{0};
    std::atomic<size_t>           m_in_use{0};
    std::atomic<size_t>           m_max_in_use{0};
    std::atomic<size_t>           m_blocking_waits{0};
};

} // namespace gpu_pool

#endif // FREYCOIN_GPU_PINNED_POOL_H
