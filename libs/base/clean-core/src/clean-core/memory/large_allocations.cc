#include <clean-core/container/map.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/platform/file_access.hh>
#include <clean-core/thread/mutex.hh>

using namespace cc::primitive_defines;

namespace
{
/// The threshold while not tracking: no size reaches it.
constexpr isize off = isize((u64(1) << 63) - 1);
} // namespace

namespace cc::large_allocations::impl
{
cc::atomic<isize> g_threshold = {off};
} // namespace cc::large_allocations::impl

namespace
{
struct block
{
    u64 context = 0;
    isize bytes = 0;
};

struct books
{
    /// The live large blocks, by address, so a free credits the context that allocated it.
    cc::map<uintptr_t, block> blocks;
    cc::map<u64, cc::large_allocations::context_stats> contexts;
};

auto g_books = cc::mutex<books>();

/// The books allocate their own nodes through the same resource; a table grown past the threshold would book itself
/// under the lock it already holds.
thread_local bool t_booking = false;

struct booking_scope
{
    bool const entered = !t_booking;
    booking_scope() { t_booking = true; }
    ~booking_scope()
    {
        if (entered)
            t_booking = false;
    }
};
} // namespace

void cc::large_allocations::impl::note_allocated(void* p, isize bytes)
{
    if (t_booking || p == nullptr)
        return;
    auto const scope = booking_scope();
    auto const context = cc::file_access::current_context();
    g_books.lock(
        [&](books& b)
        {
            b.blocks[uintptr_t(p)] = block{.context = context, .bytes = bytes};
            auto& s = b.contexts[context];
            s.context = context;
            s.live_bytes += bytes;
            s.peak_live_bytes = s.live_bytes > s.peak_live_bytes ? s.live_bytes : s.peak_live_bytes;
            s.allocations += 1;
            s.allocated_bytes += bytes;
        });
}

void cc::large_allocations::impl::note_freed(void* p)
{
    if (t_booking || p == nullptr)
        return;
    auto const scope = booking_scope();
    g_books.lock(
        [&](books& b)
        {
            auto const* found = b.blocks.get_ptr(uintptr_t(p));
            if (found == nullptr)
                return; // allocated before tracking started, or below the threshold then
            auto const blk = *found;
            b.blocks.erase(uintptr_t(p));
            b.contexts[blk.context].live_bytes -= blk.bytes;
        });
}

void cc::large_allocations::start_tracking(isize threshold)
{
    impl::g_threshold.store(threshold > 0 ? threshold : 1, cc::memory_order_relaxed);
}

void cc::large_allocations::stop_tracking()
{
    impl::g_threshold.store(off, cc::memory_order_relaxed);
}

bool cc::large_allocations::is_tracking()
{
    return impl::g_threshold.load(cc::memory_order_relaxed) != off;
}

cc::vector<cc::large_allocations::context_stats> cc::large_allocations::snapshot()
{
    auto const scope = booking_scope();
    return g_books.lock(
        [](books& b)
        {
            auto out = cc::vector<context_stats>();
            for (auto const& entry : b.contexts)
                out.push_back(entry.value);
            return out;
        });
}
