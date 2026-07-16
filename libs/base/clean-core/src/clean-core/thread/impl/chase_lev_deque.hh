#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/memory/allocation.hh>

#include <atomic>
#include <type_traits>

// Chase-Lev single-owner / multi-thief lock-free deque — the queue behind cc::async_thread_pool's workers.
//
// Implementation detail of the pool, not public API: it lives in impl/ and is not on the cheat sheet. It is
// templated only so it can be tested without dragging in the async node; the pool instantiates it with a raw
// async_node_base*.
//
// The orderings below are the Lê/Pop/Cohen/Nardelli formulation from "Correct and Efficient Work-Stealing for
// Weak Memory Models" (PPoPP'13), which is machine-checked against C11. They are load-bearing and subtle:
// DO NOT adjust one because it looks redundant on x86. TSO hides missing acquire/release; ARM64 is a shipping
// target here and does not.

namespace cc::impl
{
/// Outcome of a steal attempt. The empty/abort split matters: `abort` means the deque had work but another
/// thread won the race for it, so the thief should retry (a different victim is usually the better move),
/// whereas `empty` means this victim has nothing and is worth skipping. Collapsing the two into "no value"
/// makes a pool either spin on a contended victim or give up while work is available.
enum class steal_result
{
    success, ///< `out` holds a stolen value
    empty,   ///< nothing to steal here
    abort,   ///< lost a race; the deque may still have work
};

/// Single-owner / multi-thief lock-free deque (Chase-Lev).
///
/// The owner pushes and takes at the BOTTOM: LIFO, so freshly spawned children stay hot, and in the common case
/// it costs no atomic RMW and no cross-thread traffic at all. Thieves take from the TOP: FIFO, so the oldest
/// entry (the coldest, and usually the one rooting the biggest subtree) is the one that migrates.
///
/// Stores VALUES only — any ownership an entry carries (a refcount, say) belongs to the caller, **including for
/// entries still queued when the deque is destroyed**: the destructor frees its buffers and forgets the values.
/// A pool holding strong handles must therefore drain every deque itself, or it leaks whatever they pin.
///
/// `T` must be trivially copyable and lock-free as an atomic, because a thief reads a slot speculatively —
/// before the CAS that decides whether it may have it (see try_steal).
///
/// Threading contract, and it is not symmetric: push/try_take are **owner-only** and must all run on the same
/// thread; try_steal is for every OTHER thread. Two threads calling push, or the owner calling try_steal on its
/// own deque, breaks it.
template <class T>
struct chase_lev_deque
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "a thief reads a slot speculatively and may lose the race for "
                  "it, so a torn or stale read must be harmless");
    static_assert(std::atomic<T>::is_always_lock_free, "a lock-free deque of lock-ful slots is pointless");

    /// `initial_capacity` is rounded up to a power of two (the mask indexing needs one).
    explicit chase_lev_deque(cc::i64 initial_capacity = 256)
    {
        CC_ASSERT(initial_capacity > 0, "capacity must be positive");
        _ring.store(alloc_ring(cc::bit_ceil(cc::u64(initial_capacity))), std::memory_order_relaxed);
    }

    /// Frees the live buffer and every buffer a grow retired. Queued VALUES are not touched — see the class note.
    ~chase_lev_deque()
    {
        // Safe to free the retired buffers only because no thief can still be reading one. That is not a
        // property of this class: it is the pool's destructor joining every worker before destroying any deque.
        // See ~async_thread_pool.
        ring* r = _ring.load(std::memory_order_relaxed);
        while (r != nullptr)
        {
            ring* const older = r->older;
            free_ring(r);
            r = older;
        }
    }

    chase_lev_deque(chase_lev_deque const&) = delete;
    chase_lev_deque(chase_lev_deque&&) = delete;
    chase_lev_deque& operator=(chase_lev_deque const&) = delete;
    chase_lev_deque& operator=(chase_lev_deque&&) = delete;

    /// Owner only. Grows on demand.
    void push(T v)
    {
        cc::i64 const b = _bottom.load(std::memory_order_relaxed);
        cc::i64 const t = _top.load(std::memory_order_acquire);
        ring* a = _ring.load(std::memory_order_relaxed);

        if (b - t > a->mask) // size would exceed capacity - 1
            a = grow(a, b, t);

        a->put(b, v);
        std::atomic_thread_fence(std::memory_order_release); // the slot store must land before the publish
        _bottom.store(b + 1, std::memory_order_relaxed);     // the publish. RELAXED -- see the note in the pool's
                                                             // wake path, which needs its own fence because of it.
    }

    /// Owner only. Takes the newest entry (LIFO). False if empty, or if a thief won the last one — `out` is only
    /// meaningful when this returns true, and is clobbered otherwise.
    [[nodiscard]] bool try_take(T& out)
    {
        cc::i64 const b = _bottom.load(std::memory_order_relaxed) - 1;
        ring* const a = _ring.load(std::memory_order_relaxed);
        _bottom.store(b, std::memory_order_relaxed); // claim it speculatively

        // Half of the Dekker against try_steal: we store _bottom then load _top; a thief stores _top then loads
        // _bottom. Sequential consistency over the two fences means at least one side sees the other, which is
        // exactly what stops the last element being taken twice -- or lost by both.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        cc::i64 t = _top.load(std::memory_order_relaxed);

        if (t > b) // empty
        {
            _bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }

        out = a->get(b);
        if (t != b)
            return true; // more than one entry: no thief can be after this one

        // exactly one entry left, and a thief may want the same one -- settle it on _top
        bool const won = _top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
        _bottom.store(b + 1, std::memory_order_relaxed); // empty either way
        return won;
    }

    /// Any thread except the owner. Takes the oldest entry (FIFO). `out` is only meaningful on `success`.
    [[nodiscard]] steal_result try_steal(T& out)
    {
        cc::i64 t = _top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst); // the other half of try_take's Dekker
        cc::i64 const b = _bottom.load(std::memory_order_acquire);

        if (t >= b)
            return steal_result::empty;

        // AFTER _top, never before: a concurrent grow may have swapped the buffer, and the ordering is what
        // guarantees we do not read through a pointer older than the index we validated.
        ring* const a = _ring.load(std::memory_order_acquire);

        // Speculative: this slot may already belong to someone else. Only the CAS below decides. This read is
        // why T must be trivially copyable -- reading a stale value must be harmless, and the caller must not
        // look at `out` unless we return success.
        out = a->get(t);

        if (!_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return steal_result::abort;
        return steal_result::success;
    }

    /// Racy by construction — for heuristics and stats, never for control flow that must be correct.
    [[nodiscard]] cc::i64 size_estimate() const
    {
        cc::i64 const b = _bottom.load(std::memory_order_relaxed);
        cc::i64 const t = _top.load(std::memory_order_relaxed);
        return b - t > 0 ? b - t : 0;
    }

    /// Live buffer capacity. Test/diagnostic only; grows behind your back.
    [[nodiscard]] cc::i64 capacity() const { return _ring.load(std::memory_order_relaxed)->mask + 1; }

private:
    // One heap block: this header followed by its slots. `older` chains retired buffers for the destructor.
    struct ring
    {
        cc::i64 mask;
        ring* older;

        [[nodiscard]] std::atomic<T>* slots() { return reinterpret_cast<std::atomic<T>*>(this + 1); }

        [[nodiscard]] T get(cc::i64 i) { return slots()[i & mask].load(std::memory_order_relaxed); }
        void put(cc::i64 i, T v) { slots()[i & mask].store(v, std::memory_order_relaxed); }
    };
    static_assert(alignof(std::atomic<T>) <= alignof(ring), "the slots trail the header at alignof(ring)");

    [[nodiscard]] static ring* alloc_ring(cc::i64 cap)
    {
        CC_ASSERT(cc::has_single_bit(cc::u64(cap)), "capacity must be a power of two");

        cc::isize const bytes = cc::isize(sizeof(ring)) + cap * cc::isize(sizeof(std::atomic<T>));
        cc::byte* p = nullptr;
        cc::default_memory_resource->allocate_bytes(&p, bytes, bytes, alignof(ring),
                                                    cc::default_memory_resource->userdata);

        auto* r = reinterpret_cast<ring*>(p);
        r->mask = cap - 1;
        r->older = nullptr;
        for (cc::i64 i = 0; i < cap; ++i)
            new (cc::placement_new, r->slots() + i) std::atomic<T>();
        return r;
    }

    static void free_ring(ring* r)
    {
        cc::isize const bytes = cc::isize(sizeof(ring)) + (r->mask + 1) * cc::isize(sizeof(std::atomic<T>));
        cc::default_memory_resource->deallocate_bytes(reinterpret_cast<cc::byte*>(r), bytes, alignof(ring),
                                                      cc::default_memory_resource->userdata);
    }

    // Owner only, from push. Copies the live range into a buffer of twice the capacity and publishes it.
    //
    // The old buffer is RETIRED, not freed: a thief may be between its `_ring` load and its slot read right now,
    // and freeing under it would be a use-after-free. Retiring costs a bounded amount of memory -- capacity
    // doubles, so the whole chain is under 2x the live buffer -- and it is what the destructor's walk cleans up.
    // The alternative (a fixed ring plus an overflow list) buys that memory back by adding a branch to this hot
    // path and a second claim path to the pool, i.e. by adding correctness surface to the code least worth
    // hand-verifying. Not a trade worth making.
    [[nodiscard]] ring* grow(ring* old, cc::i64 b, cc::i64 t)
    {
        ring* const fresh = alloc_ring((old->mask + 1) * 2);
        for (cc::i64 i = t; i < b; ++i)
            fresh->put(i, old->get(i));

        fresh->older = old;
        _ring.store(fresh, std::memory_order_release);
        return fresh;
    }

    // Each on its own line. The owner writes _bottom on every push; thieves CAS _top on every steal; _ring is
    // read by both and written almost never. Sharing a line between any two of these would turn every push into
    // an invalidation of whatever the thieves are polling -- the exact traffic this deque exists to avoid.
    alignas(64) std::atomic<cc::i64> _top{0};
    alignas(64) std::atomic<cc::i64> _bottom{0};
    alignas(64) std::atomic<ring*> _ring{nullptr};
};
} // namespace cc::impl
