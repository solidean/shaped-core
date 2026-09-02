#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer_access.hh>

// Cross-list buffer access tracking — the one place vulkan needs machinery dx12 does not have at all.
// Pure logic with no device, so these run everywhere.

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr auto k_first = sg::command_list_slot(0);
constexpr auto k_second = sg::command_list_slot(1);
} // namespace

TEST("sg vulkan - a first write on a fresh buffer needs no barrier")
{
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);

    // Nothing is in flight, so the write is a freebie.
    CHECK(!access.flush(k_first).needed);
}

TEST("sg vulkan - a read after a write in one list barriers")
{
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    CHECK(!access.flush(k_first).needed);

    access.declare(k_first, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    auto const barrier = access.flush(k_first);
    REQUIRE(barrier.needed);
    CHECK(barrier.src_access.has(sg::access_flag::copy_write));
    CHECK(barrier.dst_access.has(sg::access_flag::shader_read));
}

TEST("sg vulkan - a write survives the list that recorded it, as the next list's entry barrier")
{
    // The divergence from dx12 in one test.
    // D3D12 decays a buffer to COMMON at ExecuteCommandLists, so a later list needs no barrier and dx12_buffer keeps
    // no between-lists state.
    // Vulkan has no decay: the second list must still synchronize against the first list's write.
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    CHECK(!access.flush(k_first).needed);
    CHECK(!access.finalize(k_first).needed); // nothing was submitted before it

    // The second list's body carries nothing: what its first op needs is resolved at ITS finalize, against what the
    // buffer is really in by then.
    access.declare(k_second, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    CHECK(!access.flush(k_second).needed);

    auto const entry = access.finalize(k_second);
    REQUIRE(entry.needed);
    CHECK(entry.src_access.has(sg::access_flag::copy_write));
    CHECK(entry.dst_access.has(sg::access_flag::shader_read));
}

TEST("sg vulkan - a dropped list leaves nothing for the next one to wait on")
{
    // Discarded work never runs, so it creates no hazard — the opposite of finalize.
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    CHECK(!access.flush(k_first).needed);
    access.discard(k_first);

    access.declare(k_second, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    CHECK(!access.flush(k_second).needed);
    CHECK(!access.finalize(k_second).needed);
}

TEST("sg vulkan - a concurrently recorded read is ordered against the write by its entry barrier")
{
    // The hazard this model exists to remove, and the one synchronization validation reported.
    // Two lists open at once still see nothing of each other while recording — that is what a private slot is for —
    // so neither body carries a barrier.
    // The ordering comes from the second list's entry barrier, computed at its finalize.
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    CHECK(!access.flush(k_first).needed);
    access.declare(k_second, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    CHECK(!access.flush(k_second).needed);

    CHECK(!access.finalize(k_first).needed);
    CHECK(access.active_slot_count == 1);

    auto const entry = access.finalize(k_second);
    CHECK(access.active_slot_count == 0);
    REQUIRE(entry.needed);
    CHECK(entry.src_access.has(sg::access_flag::copy_write));
    CHECK(entry.dst_access.has(sg::access_flag::shader_read));
}

TEST("sg vulkan - concurrent lists track the same buffer independently")
{
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    access.declare(k_second, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);

    // Each slot has its own state, so neither sees the other's declares as a hazard.
    CHECK(!access.flush(k_first).needed);
    CHECK(!access.flush(k_second).needed);
    CHECK(access.active_slot_count == 2);
}

TEST("sg vulkan - the per-op and per-list flags each fire once")
{
    auto access = vulkan::vulkan_buffer_access();

    // A buffer bound several times to one op enqueues one barrier, not one per binding.
    CHECK(access.mark_pending_barrier(k_first));
    CHECK(!access.mark_pending_barrier(k_first));
    access.declare(k_first, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    (void)access.flush(k_first); // clears it for the next op
    CHECK(access.mark_pending_barrier(k_first));

    // The finalize-set flag instead lasts the whole list.
    CHECK(access.mark_recorded(k_first));
    CHECK(!access.mark_recorded(k_first));
}

TEST("sg vulkan - a read after a read costs no second barrier")
{
    auto access = vulkan::vulkan_buffer_access();
    access.declare(k_first, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    CHECK(!access.flush(k_first).needed);

    access.declare(k_first, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    CHECK(access.flush(k_first).needed); // syncs the read against the write

    // The same read again is already ordered against that write.
    access.declare(k_first, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read);
    CHECK(!access.flush(k_first).needed);
}
