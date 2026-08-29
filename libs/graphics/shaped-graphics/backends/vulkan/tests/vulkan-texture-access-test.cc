#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture_access.hh>

// Per-texture layout tracking.
// Pure logic with no device, so these run everywhere.
// Unlike the buffer tracker beside it this is not a vulkan-specific divergence — textures need between-lists state on
// any backend, because a layout is physical and whatever one list leaves behind is what the next list finds.

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr auto k_first = sg::command_list_slot(0);
constexpr auto k_second = sg::command_list_slot(1);

// One mip, one layer, one aspect — the simplest domain that still exercises the partition.
sg::subresource_extent single()
{
    return sg::subresource_extent{.mip_count = 1, .array_count = 1, .aspect_count = 1};
}

sg::subresource_range whole(sg::subresource_extent e)
{
    return sg::subresource_range::whole(e);
}
} // namespace

TEST("sg vulkan - combining a sampled and a storage view degrades to general")
{
    // Vulkan has no layout serving both, so GENERAL is the only correct answer — and it is slower to sample from,
    // which is why it reports degraded rather than ok.
    auto const c = vulkan::combine_layouts(sg::texture_layout::shader_readonly, sg::texture_layout::shader_readwrite);
    CHECK(c.layout == sg::texture_layout::general);
    CHECK(c.result == vulkan::layout_combine::degraded);
}

TEST("sg vulkan - combining with general is free, and a real mismatch is a conflict")
{
    auto const with_general = vulkan::combine_layouts(sg::texture_layout::general, sg::texture_layout::copy_dst);
    CHECK(with_general.layout == sg::texture_layout::general);
    CHECK(with_general.result == vulkan::layout_combine::ok);

    auto const same = vulkan::combine_layouts(sg::texture_layout::copy_dst, sg::texture_layout::copy_dst);
    CHECK(same.result == vulkan::layout_combine::ok);

    // A copy destination and a sampled read cannot coexist in one operation.
    auto const bad = vulkan::combine_layouts(sg::texture_layout::copy_dst, sg::texture_layout::shader_readonly);
    CHECK(bad.result == vulkan::layout_combine::conflict);
}

TEST("sg vulkan - a first layout transition is emitted, and undefined discards contents")
{
    auto access = vulkan::vulkan_texture_access(single());
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);

    auto const barriers = access.flush(k_first);
    REQUIRE(barriers.size() == 1);
    CHECK(barriers[0].barrier.needed);
    CHECK(barriers[0].barrier.dst_layout == sg::texture_layout::copy_dst);

    // A texture starts undefined, which is exactly Vulkan's "previous contents are not preserved".
    CHECK(barriers[0].barrier.src_layout == sg::texture_layout::undefined);
}

TEST("sg vulkan - the last list to finalize commits its layout")
{
    auto access = vulkan::vulkan_texture_access(single());
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_first);

    // Sole user, so its layout becomes canonical rather than being reverted.
    auto const reverts = access.finalize(k_first);
    CHECK(reverts.empty());
    CHECK(access.active_slot_count() == 0);

    // The next list therefore finds it in copy_dst and transitions from there.
    access.declare(k_second, whole(single()), sg::pipeline_stage_flag::fragment, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    auto const barriers = access.flush(k_second);
    REQUIRE(barriers.size() == 1);
    CHECK(barriers[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(barriers[0].barrier.dst_layout == sg::texture_layout::shader_readonly);
}

TEST("sg vulkan - a list that is not the last reverts to canonical")
{
    // Two lists open at once.
    // The first to finalize must hand the texture back as it found it: the second list's already-recorded barriers
    // were computed against the canonical layout.
    auto access = vulkan::vulkan_texture_access(single());

    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_first);
    access.declare(k_second, whole(single()), sg::pipeline_stage_flag::fragment, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    (void)access.flush(k_second);
    CHECK(access.active_slot_count() == 2);

    auto const reverts = access.finalize(k_first);
    REQUIRE(reverts.size() == 1);
    CHECK(reverts[0].barrier.needed);
    CHECK(reverts[0].barrier.dst_layout == sg::texture_layout::undefined); // back to what it entered with
}

TEST("sg vulkan - a dropped list leaves the canonical layout alone")
{
    auto access = vulkan::vulkan_texture_access(single());
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_first);
    access.discard(k_first);
    CHECK(access.active_slot_count() == 0);

    // Its work never ran, so the next list still finds the texture undefined rather than in copy_dst.
    access.declare(k_second, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    auto const barriers = access.flush(k_second);
    REQUIRE(barriers.size() == 1);
    CHECK(barriers[0].barrier.src_layout == sg::texture_layout::undefined);
}

TEST("sg vulkan - mip levels are tracked independently")
{
    // The point of the covering partition: transitioning one mip must not claim to transition its siblings.
    auto const extent = sg::subresource_extent{.mip_count = 4, .array_count = 1, .aspect_count = 1};
    auto access = vulkan::vulkan_texture_access(extent);

    auto one_mip = sg::subresource_range();
    one_mip.mip_range = {.start = 1, .end = 2};

    access.declare(k_first, one_mip, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    auto const barriers = access.flush(k_first);
    REQUIRE(barriers.size() == 1);
    CHECK(barriers[0].range.mip_range.start == 1);
    CHECK(barriers[0].range.mip_range.end == 2);
}

TEST("sg vulkan - one texture bound twice to one op yields one barrier")
{
    auto access = vulkan::vulkan_texture_access(single());

    // Declared twice for the same op with the same layout; the flush merges them.
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::fragment, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    CHECK(access.mark_pending_barrier(k_first));
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    CHECK(!access.mark_pending_barrier(k_first)); // already enqueued for this op

    auto const barriers = access.flush(k_first);
    REQUIRE(barriers.size() == 1);
    CHECK(barriers[0].barrier.dst_stages.has(sg::pipeline_stage_flag::fragment));
    CHECK(barriers[0].barrier.dst_stages.has(sg::pipeline_stage_flag::compute));
}
