#pragma once
#include "shared_memory.h"
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>

namespace anyslider::mmio
{
    // Windows x64 shared mappings: all participants must use these aligned,
    // lock-free accesses (or equivalent Interlocked operations), not memcpy.
    static_assert(std::atomic_ref<int64_t>::is_always_lock_free);
    static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
    static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);
    static_assert(std::atomic_ref<uint8_t>::is_always_lock_free);
    inline std::atomic_ref<int64_t> OutputSequence(SharedBuffer& b)
    {
        return std::atomic_ref<int64_t>(*const_cast<int64_t*>(&b.output_sequence));
    }
    inline std::atomic_ref<const int64_t> OutputSequence(const SharedBuffer& b)
    {
        return std::atomic_ref<const int64_t>(*const_cast<const int64_t*>(&b.output_sequence));
    }

    inline void PublishLedOutput(SharedBuffer& buf, const LedOutput& frame)
    {
        auto seq = OutputSequence(buf);
        const int64_t previous = seq.load(std::memory_order_relaxed); // 理論上，不會有其他thread修改這個值，因此relaxed就夠了

        const int64_t next = previous + 1;

        const size_t slot = OutputSlotIndex(next);
        auto& output = buf.output[slot];

        memcpy(&output, &frame, sizeof(LedOutput));

        seq.store(next, std::memory_order_release);
    }
    inline bool TryReadLedOutput(const SharedBuffer& buf, LedOutput& frame)
    {
        auto seq = OutputSequence(buf);

        const int64_t before = seq.load(std::memory_order_acquire);

        // Sequence zero means there is no valid snapshot.
        if (before <= 0)
            return false;

        const size_t slot = OutputSlotIndex(before);

        LedOutput candidate = buf.output[slot];

        const int64_t after = seq.load(std::memory_order_seq_cst);

        // Discard if 3 or more newer slots was published during the read. (Highly unlikely, unless the CPU has been preempted for quite some time.)
        if (before + 3 <= after)
        {
            return false;
        }

        frame = candidate;
        return true;
    }
}
