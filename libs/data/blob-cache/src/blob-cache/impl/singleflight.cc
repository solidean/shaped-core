#include <blob-cache/impl/singleflight.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>

namespace bcache::impl
{
u64 flight_table::next_generation()
{
    // Process-wide and monotonic, so a generation is unique across every table in this process — which keeps the counter out of the lock without making two caches able to mint the same one.
    static cc::atomic<u64> counter = {1};
    return counter.fetch_add(1, cc::memory_order_relaxed);
}

flight_table::claim flight_table::claim_or_join(cache_key const& key, cc::shared_async<blob> fresh, u64 generation)
{
    return _state.lock(
        [&](state& s) -> claim
        {
            if (auto* slot = s.in_flight.get_ptr(key))
            {
                if (auto existing = slot->operation.lock())
                    return {.operation = cc::move(existing), .is_owner = false};

                // Expired: the last consumer let go.
                // Reuse the slot rather than sweeping it.
                *slot = {.operation = fresh, .generation = generation};
                return {.operation = cc::move(fresh), .is_owner = true};
            }

            s.in_flight[key] = {.operation = fresh, .generation = generation};
            return {.operation = cc::move(fresh), .is_owner = true};
        });
}

void flight_table::release(cache_key const& key, u64 generation)
{
    _state.lock(
        [&](state& s)
        {
            auto const* slot = s.in_flight.get_ptr(key);
            if (slot != nullptr && slot->generation == generation)
                s.in_flight.erase(key);
        });
}

void flight_table::cancel_all()
{
    // Collected under the lock, resolved OUTSIDE it: push_error wakes dependents synchronously, and a continuation that re-entered this table while we still held it would deadlock.
    auto live = cc::vector<cc::shared_async<blob>>();

    _state.lock(
        [&](state& s)
        {
            for (auto const& [key, slot] : s.in_flight)
                if (auto op = slot.operation.lock())
                    live.push_back(cc::move(op));
            s.in_flight.clear();
        });

    for (auto const& op : live)
        if (!op->is_ready())
            op->push_error(cc::async_error::make_cancelled());
}

isize flight_table::in_flight_count() const
{
    return _state.lock(
        [](state& s)
        {
            auto n = isize(0);
            for (auto const& [key, slot] : s.in_flight)
                if (slot.operation.lock() != nullptr)
                    ++n;
            return n;
        });
}
} // namespace bcache::impl
