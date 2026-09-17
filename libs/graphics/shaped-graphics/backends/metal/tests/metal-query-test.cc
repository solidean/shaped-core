#include "metal-test-common.hh"

#include <clean-core/common/time.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

// What the tier-1 query test cannot reach: the two facts metal's timestamp path is built on.
// The round trip itself is backend-agnostic and lives in tests/query/query-test.cc.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

TEST("sg - metal timestamps are nanoseconds on the CPU's own timebase", exclusive("metal-device"))
{
    // metal_query_system hands out a constant tick-to-seconds rather than querying one, because Metal offers nothing to
    // query — so this is what says the constant is still right.
    // A device that ever counted on its own clock would scale every measurement sg reports, silently.
    auto const ctx_r = sg::create_metal_context({});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();

    auto& metal_ctx = static_cast<mtl::metal_context&>(*ctx);
    REQUIRE(metal_ctx.queries().supports_timestamps());
    CHECK(metal_ctx.queries().timestamp_tick_to_seconds() == 1e-9);

    MTL::Timestamp cpu_before = 0;
    MTL::Timestamp gpu_before = 0;
    metal_ctx.device()->sampleTimestamps(&cpu_before, &gpu_before);

    // A measured interval rather than a pause: a ratio needs a baseline, and this is the baseline.
    // Short enough to cost nothing and long enough that a per-sample rounding error cannot reach 1%.
    auto const spin_start = cc::current_time_steady_secs();
    while (cc::current_time_steady_secs() - spin_start < 0.005)
    {
    }

    MTL::Timestamp cpu_after = 0;
    MTL::Timestamp gpu_after = 0;
    metal_ctx.device()->sampleTimestamps(&cpu_after, &gpu_after);

    auto const cpu_delta = double(cpu_after - cpu_before);
    auto const gpu_delta = double(gpu_after - gpu_before);
    REQUIRE(cpu_delta > 0);
    REQUIRE(gpu_delta > 0);

    // One timebase: the two deltas agree.
    // A unit change would miss by orders of magnitude rather than by the percent this allows.
    auto const timebase_ratio = gpu_delta / cpu_delta;
    CHECK(timebase_ratio > 0.99);
    CHECK(timebase_ratio < 1.01);

    // And that timebase counts nanoseconds, which is the half the constant actually depends on.
    auto const measured_seconds = cpu_delta * 1e-9 / (cc::current_time_steady_secs() - spin_start);
    CHECK(measured_seconds > 0.9);
    CHECK(measured_seconds < 1.1);
}

TEST("sg - a metal counter heap goes back to the free list and comes out clean", exclusive("metal-device"))
{
    // Heaps are pooled, so the slots one list wrote are what the next list would read had they not been invalidated.
    auto const ctx_r = sg::create_metal_context({});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();

    auto& queries = static_cast<mtl::metal_context&>(*ctx).queries();
    REQUIRE(queries.supports_timestamps());

    auto lease = queries.acquire_heap();
    REQUIRE(lease != nullptr);
    auto* const heap = lease->heap;
    lease->next_slot = 3;

    queries.release_heap(cc::move(lease));

    auto again = queries.acquire_heap();
    REQUIRE(again != nullptr);
    CHECK(again->heap == heap);               // pooled rather than remade
    CHECK(again->next_slot == 0);             // bump allocator reset
    CHECK(!again->shared_future->is_valid()); // and a fresh future, so the previous leaseholder's handles keep theirs
    queries.release_heap(cc::move(again));
}
