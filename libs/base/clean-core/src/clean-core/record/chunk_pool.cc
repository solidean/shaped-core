#include "chunk_pool.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/thread/thread.hh>

using namespace cc::primitive_defines;

namespace
{
/// Where a chunk's data starts, relative to its header.
/// Rounded to a cache line so the data never shares one with the header's hot atomics.
constexpr isize chunk_header_bytes()
{
    constexpr isize line = 64;
    return (isize(sizeof(cc::rec::chunk)) + line - 1) / line * line;
}

/// How many yields a backpressured producer gives up before it starts checking again in earnest.
/// Deliberately a yield loop rather than a condition variable: exhausting the budget already means the consumer is
/// losing, and a scheduling yield on a path that is by then pathological buys a much simpler pool.
constexpr int backpressure_yields_per_attempt = 4;
} // namespace

cc::rec::chunk_pool::chunk_pool(isize chunk_bytes, isize budget_bytes, rec::overflow_policy policy)
  : _chunk_bytes(chunk_bytes), _budget_bytes(budget_bytes), _policy(policy)
{
    CC_ASSERT(chunk_bytes > chunk_header_bytes() + 4096, "a chunk must be substantially larger than its header");
    CC_ASSERT(budget_bytes >= 2 * chunk_bytes,
              "the budget must fit at least two chunks — the consumer can only release a chunk once its successor "
              "exists, so one per thread is always retained");
}

cc::rec::chunk_pool::~chunk_pool()
{
    _state.lock(
        [](state& s)
        {
            for (auto* c : s.ready)
            {
                c->release_pins();
                c->~chunk();
            }
            s.ready.clear();
            s.blocks.clear();
        });
}

isize cc::rec::chunk_pool::data_bytes_per_chunk() const
{
    return _chunk_bytes - chunk_header_bytes();
}

cc::rec::chunk* cc::rec::chunk_pool::_grow_locked(state& s)
{
    if (_policy != rec::overflow_policy::grow_unbounded && s.allocated_bytes + _chunk_bytes > _budget_bytes)
        return nullptr;

    auto block = cc::allocation<byte>::create_empty(_chunk_bytes, 64, nullptr);
    auto* const base = block.alloc_start;
    s.blocks.push_back(cc::move(block));
    s.allocated_bytes += _chunk_bytes;

    auto* const c = new (base) rec::chunk();
    c->data = base + chunk_header_bytes();
    c->capacity = u32(data_bytes_per_chunk());
    c->pool = this;
    c->refs.store(0, cc::memory_order_relaxed); // sitting in the free list; acquire() hands out the first reference

    // Touch every page here rather than on whichever thread first writes into it.
    for (isize i = 0; i < isize(c->capacity); i += 4096)
        c->data[i] = byte{};

    return c;
}

cc::rec::chunk* cc::rec::chunk_pool::acquire(rec::impl::thread_state* owner, u64 seq, u16 layer)
{
    for (;;)
    {
        auto* const c = _state.lock(
            [&](state& s) -> rec::chunk*
            {
                if (!s.ready.empty())
                {
                    auto* const taken = s.ready.back();
                    s.ready.pop_back();
                    return taken;
                }
                auto* const grown = _grow_locked(s);
                if (grown == nullptr)
                    ++s.failed_acquires;
                return grown;
            });

        if (c != nullptr)
        {
            c->committed.store(0, cc::memory_order_relaxed);
            c->next_in_thread.store(nullptr, cc::memory_order_relaxed);
            c->is_sealed.store(false, cc::memory_order_relaxed);
            c->pin_count.store(0, cc::memory_order_relaxed);
            c->refs.store(1, cc::memory_order_relaxed);
            c->owner = owner;
            c->seq = seq;
            c->layer = layer;
            c->seal_cycles = 0;
            c->seal_wall_secs = 0;
            c->base_cycles = cc::current_cycles();
            c->base_wall_secs = cc::current_time_wall_secs();
            return c;
        }

        if (_policy != rec::overflow_policy::backpressure)
            return nullptr;

        for (int i = 0; i < backpressure_yields_per_attempt; ++i)
            cc::this_thread_yield();
    }
}

void cc::rec::chunk_pool::recycle(rec::chunk* c)
{
    CC_ASSERT(c->ref_count() == 0, "recycling a chunk somebody still holds");

    c->release_pins();
    c->owner = nullptr;

    _state.lock([&](state& s) { s.ready.push_back(c); });
}

void cc::rec::chunk_pool::refill(isize target)
{
    _state.lock(
        [&](state& s)
        {
            while (isize(s.ready.size()) < target)
            {
                auto* const c = _grow_locked(s);
                if (c == nullptr)
                    return;
                s.ready.push_back(c);
            }
        });
}

isize cc::rec::chunk_pool::allocated_bytes() const
{
    return const_cast<cc::mutex<state>&>(_state).lock([](state const& s) { return s.allocated_bytes; });
}

isize cc::rec::chunk_pool::ready_count() const
{
    return const_cast<cc::mutex<state>&>(_state).lock([](state const& s) { return isize(s.ready.size()); });
}

u64 cc::rec::chunk_pool::failed_acquires() const
{
    return const_cast<cc::mutex<state>&>(_state).lock([](state const& s) { return s.failed_acquires; });
}
