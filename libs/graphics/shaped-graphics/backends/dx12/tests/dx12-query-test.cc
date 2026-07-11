#include "dx12-test-common.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

// GPU queries: cmd.query.record_gpu_timestamp, resolved + read back at submit, on WARP. Covers the
// round-trip, heap rollover across leases, and the dropped-list path.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - gpu timestamp round-trips")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // A bit of GPU work between the two timestamps (contents are irrelevant — only the tick gap matters).
    auto src = c.create_dx12_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    auto dst = c.create_dx12_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(src.has_value());
    REQUIRE(dst.has_value());

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    CHECK(cmd.value()->query.is_supported());

    auto t0 = cmd.value()->query.record_gpu_timestamp();
    cmd.value()->copy.buffer_bytes_region({.src = src.value(), .dst = dst.value(), .size_in_bytes = 256});
    auto t1 = cmd.value()->query.record_gpu_timestamp();

    CHECK(t0.is_valid());
    CHECK(t1.is_valid());
    CHECK(!t0.is_ready()); // not submitted yet

    c.submit_dx12_command_list(cc::move(cmd.value()));

    auto const tick1 = c.wait_for_ticks(t1);
    auto const tick0 = c.wait_for_ticks(t0);
    REQUIRE(tick0.has_value());
    REQUIRE(tick1.has_value());
    CHECK(tick1.value() >= tick0.value()); // non-decreasing on a single queue

    REQUIRE(t0.is_ready());
    REQUIRE(t1.is_ready());
    CHECK(t0.try_get_ticks().value() == tick0.value());

    auto const s0 = t0.try_get_seconds();
    auto const s1 = t1.try_get_seconds();
    REQUIRE(s0.has_value());
    REQUIRE(s1.has_value());
    CHECK(s1.value() - s0.value() >= 0.0);
}

TEST("sg dx12 - timestamp heap rollover across leases")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());

    int const per_heap = dx12::dx12_query_system::SlotsPerHeap;
    int const n = per_heap + 5; // spills into a second leased heap

    cc::vector<sg::gpu_timestamp> ts;
    ts.reserve(n);
    for (int i = 0; i < n; ++i)
        ts.push_back(cmd.value()->query.record_gpu_timestamp());

    c.submit_dx12_command_list(cc::move(cmd.value()));

    // The last query lives in the second heap; the actor drains heaps in submission order, so waiting on
    // it implies the first heap's readback has landed too.
    REQUIRE(c.wait_for_ticks(ts.back()).has_value());

    // Sample across the heap boundary: last slot of heap 0, first slot of heap 1, and the final slot.
    int const sample[] = {0, per_heap - 1, per_heap, n - 1};
    cc::u64 prev = 0;
    bool first = true;
    for (int const idx : sample)
    {
        REQUIRE(ts[idx].is_ready());
        auto const tk = ts[idx].try_get_ticks();
        REQUIRE(tk.has_value());
        if (!first)
            CHECK(tk.value() >= prev); // non-decreasing across the lease boundary
        prev = tk.value();
        first = false;
    }
}

TEST("sg dx12 - dropped list leaves its timestamps not ready")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    auto t = cmd.value()->query.record_gpu_timestamp();
    CHECK(t.is_valid());

    c.drop_dx12_command_list(cc::move(cmd.value()));

    // A dropped list never resolves: the handle stays valid but never becomes ready, and blocking fails
    // instead of hanging (like a cancelled download).
    CHECK(!t.is_ready());
    CHECK(!t.try_get_ticks().has_value());
    CHECK(!c.wait_for_ticks(t).has_value());

    // The heap returned to the pool: a subsequent list records + reads back fine.
    auto cmd2 = c.create_dx12_command_list();
    REQUIRE(cmd2.has_value());
    auto t2 = cmd2.value()->query.record_gpu_timestamp();
    c.submit_dx12_command_list(cc::move(cmd2.value()));
    REQUIRE(c.wait_for_ticks(t2).has_value());
}
