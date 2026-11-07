#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/platform/file_access.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// The stats of the context a block allocated here would be booked to.
cc::large_allocations::context_stats mine()
{
    auto const context = cc::file_access::current_context();
    for (auto const& s : cc::large_allocations::snapshot())
        if (s.context == context)
            return s;
    return {};
}

/// Tracking is process-wide and a runner may track this whole run: switched on here, it is switched off only if it was off.
struct tracking_scope
{
    bool was = cc::large_allocations::is_tracking();
    tracking_scope() { cc::large_allocations::start_tracking(); }
    ~tracking_scope()
    {
        if (!was)
            cc::large_allocations::stop_tracking();
    }
};

byte* allocate(isize bytes)
{
    auto const& res = *cc::default_memory_resource;
    byte* p = nullptr;
    CHECK(res.allocate_bytes(&p, bytes, bytes, 16, res.userdata) >= bytes);
    return p;
}

void deallocate(byte* p, isize bytes)
{
    auto const& res = *cc::default_memory_resource;
    res.deallocate_bytes(p, bytes, 16, res.userdata);
}
} // namespace

// Exclusive of everything: without a runner's hook every test shares context 0, so another test's large block would land in the same books.
TEST("large allocations - a large block is booked while alive and credited back when freed", exclusive())
{
    auto const scope = tracking_scope();
    auto const before = mine();
    auto* const a = allocate(2 << 20);
    auto* const b = allocate(3 << 20);
    auto const both = mine();
    CHECK(both.allocations - before.allocations == 2);
    CHECK(both.live_bytes - before.live_bytes == (5 << 20));
    CHECK(both.peak_live_bytes >= before.live_bytes + (5 << 20));
    deallocate(a, 2 << 20);
    deallocate(b, 3 << 20);
    auto const after = mine();
    CHECK(after.live_bytes == before.live_bytes);
    CHECK(after.peak_live_bytes == both.peak_live_bytes);
}

TEST("large allocations - a block below the threshold is not booked", exclusive())
{
    auto const scope = tracking_scope();
    auto const before = mine();
    auto* const p = allocate(cc::large_allocations::default_threshold - 1);
    CHECK(mine().allocations == before.allocations);
    deallocate(p, cc::large_allocations::default_threshold - 1);
}

TEST("large allocations - nothing is booked while tracking is off", exclusive())
{
    if (cc::large_allocations::is_tracking())
        SKIP("a runner tracks this whole run, so the off state cannot be observed here");
    auto const before = mine();
    auto* const p = allocate(2 << 20);
    deallocate(p, 2 << 20);
    CHECK(mine().allocations == before.allocations);
}
