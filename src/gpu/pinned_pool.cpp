// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pinned_pool.h"
#include "cuda_fermat.h"

#include <cassert>
#include <cstdlib>

namespace gpu_pool {

bool PinnedPool::reserve(size_t count, size_t slot_bytes) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_slots.size() == count && m_slot_bytes == slot_bytes) {
        return true;  // already at requested shape
    }

    // Release any prior allocation
    for (auto& s : m_slots) {
        if (s.data) cuda_fermat_host_free(s.data);
    }
    m_slots.clear();
    m_free_list.clear();
    m_slot_bytes = 0;
    m_in_use.store(0, std::memory_order_relaxed);

    m_slots.resize(count);
    m_free_list.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        void* p = cuda_fermat_host_alloc(slot_bytes);
        if (!p) {
            // CUDA not available or alloc failed mid-way — fall back to malloc
            // so the pool still functions (just without true async overlap).
            p = std::malloc(slot_bytes);
            if (!p) {
                // Total failure: roll back everything
                for (size_t j = 0; j < i; ++j) {
                    if (m_slots[j].data) {
                        // Mixed bag: we don't track which slots came from
                        // cuda_fermat_host_alloc vs malloc. To be safe, try
                        // cuda free first; if not pinned, the call no-ops.
                        cuda_fermat_host_free(m_slots[j].data);
                    }
                }
                m_slots.clear();
                m_free_list.clear();
                return false;
            }
        }
        m_slots[i].data = p;
        m_slots[i].size = slot_bytes;
        m_slots[i].index = static_cast<int>(i);
        m_free_list.push_back(static_cast<int>(i));
    }
    m_slot_bytes = slot_bytes;
    return true;
}

PinnedSlot* PinnedPool::acquire() {
    std::unique_lock<std::mutex> lk(m_mu);
    if (m_free_list.empty()) {
        m_blocking_waits.fetch_add(1, std::memory_order_relaxed);
        m_cv.wait(lk, [this]() { return !m_free_list.empty(); });
    }
    int idx = m_free_list.back();
    m_free_list.pop_back();
    size_t in_use = m_in_use.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t prev_max = m_max_in_use.load(std::memory_order_relaxed);
    while (in_use > prev_max && !m_max_in_use.compare_exchange_weak(prev_max, in_use)) {
        // retry CAS
    }
    return &m_slots[idx];
}

PinnedSlot* PinnedPool::try_acquire() {
    std::unique_lock<std::mutex> lk(m_mu);
    if (m_free_list.empty()) return nullptr;
    int idx = m_free_list.back();
    m_free_list.pop_back();
    size_t in_use = m_in_use.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t prev_max = m_max_in_use.load(std::memory_order_relaxed);
    while (in_use > prev_max && !m_max_in_use.compare_exchange_weak(prev_max, in_use)) {}
    return &m_slots[idx];
}

void PinnedPool::release(PinnedSlot* slot) {
    if (!slot) return;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_free_list.push_back(slot->index);
        m_in_use.fetch_sub(1, std::memory_order_relaxed);
    }
    m_cv.notify_one();
}

void PinnedPool::reset() {
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& s : m_slots) {
        if (s.data) cuda_fermat_host_free(s.data);
        s.data = nullptr;
    }
    m_slots.clear();
    m_free_list.clear();
    m_slot_bytes = 0;
    m_in_use.store(0, std::memory_order_relaxed);
}

} // namespace gpu_pool
