#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture_access.hh>

// Per-texture layout tracking.
// Pure logic with no device, so these run everywhere.
// Unlike the buffer tracker beside it this is not a vulkan-specific divergence — textures need between-lists state on
// any backend, because a layout is physical and whatever one list leaves behind is what the next list finds.
//
// The shape to hold on to: a list's private partition starts EMPTY, entering each box at the layout its first declare
// asks for, so `flush` never produces that entry transition.
// `finalize` does, against the layout the texture is really in by the time the list submits, and the caller prepends
// what it returns to that submit.

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

// A tracker over one subresource, starting in `resting`.
// The resting layout is where the texture starts before anything uses it; the UNDEFINED it is really in after
// vkCreateImage is closed separately, by the initial transition whichever submit path claims it records.
vulkan::vulkan_texture_access tracker(sg::texture_layout resting = sg::texture_layout::general)
{
    return vulkan::vulkan_texture_access(single(), resting);
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

TEST("sg vulkan - the entry transition comes out of finalize, not out of the list's own flush")
{
    auto access = tracker(sg::texture_layout::shader_readonly);
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);

    // The list enters the box at copy_dst, so its body has nothing to transition from.
    CHECK(access.flush(k_first).empty());

    auto const entry = access.finalize(k_first);
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].barrier.needed);
    CHECK(entry[0].barrier.dst_layout == sg::texture_layout::copy_dst);

    // The discard from UNDEFINED is the initial transition's job, and it has already run by the time this barrier
    // executes.
    // What the entry barrier starts from is the layout the texture is really in.
    CHECK(entry[0].barrier.src_layout == sg::texture_layout::shader_readonly);
}

TEST("sg vulkan - the initial transition is claimed exactly once")
{
    auto access = tracker(sg::texture_layout::render_target);

    // Before anyone claims it, the image is still in the layout vkCreateImage left it in.
    CHECK(access.needs_initial_transition());
    CHECK(access.resting_layout() == sg::texture_layout::render_target);

    // The claim is what puts the UNDEFINED -> resting barrier on one submit path, so exactly one caller may win it
    // however many command lists and transfer jobs race for it.
    CHECK(access.claim_initial_transition());
    CHECK(!access.needs_initial_transition());
    CHECK(!access.claim_initial_transition());
}

TEST("sg vulkan - reading the current layout does not spend the initial claim")
{
    // The two are deliberately separate calls, and the async transfer path takes them at different moments: it reads
    // the layout to restore when the transfer is enqueued, and claims the discard only where it records the barrier.
    // A claim spent on a job that is later skipped would leave the image in UNDEFINED with every list assuming it
    // rests somewhere real.
    auto access = tracker(sg::texture_layout::copy_dst);

    CHECK(access.current_layout_of(whole(single())) == sg::texture_layout::copy_dst);
    CHECK(access.current_layout_of(whole(single())) == sg::texture_layout::copy_dst);
    CHECK(access.needs_initial_transition());

    CHECK(access.claim_initial_transition());
    CHECK(access.current_layout_of(whole(single())) == sg::texture_layout::copy_dst);
}

TEST("sg vulkan - every finalize commits its layout")
{
    auto access = tracker();
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_first);
    (void)access.finalize(k_first);
    CHECK(access.active_slot_count() == 0);

    // The next list therefore finds it in copy_dst, and its entry barrier starts there.
    access.declare(k_second, whole(single()), sg::pipeline_stage_flag::fragment, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    CHECK(access.flush(k_second).empty());

    auto const entry = access.finalize(k_second);
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(entry[0].barrier.dst_layout == sg::texture_layout::shader_readonly);
}

TEST("sg vulkan - a concurrently recorded list enters from what the earlier one left, not from what it assumed")
{
    // Two lists open at once, and the case the entry model exists for.
    // Neither can know what the texture will be in when it submits, so neither records a transition into it: the
    // second list's entry barrier is computed at ITS finalize, by which time the first has committed copy_dst.
    auto access = tracker(sg::texture_layout::shader_readonly);

    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    CHECK(access.flush(k_first).empty());
    access.declare(k_second, whole(single()), sg::pipeline_stage_flag::fragment, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    CHECK(access.flush(k_second).empty());
    CHECK(access.active_slot_count() == 2);

    // The first list enters from the layout the texture starts in, and leaves it in copy_dst.
    auto const first_entry = access.finalize(k_first);
    REQUIRE(first_entry.size() == 1);
    CHECK(first_entry[0].barrier.src_layout == sg::texture_layout::shader_readonly);
    CHECK(first_entry[0].barrier.dst_layout == sg::texture_layout::copy_dst);

    // The second enters from copy_dst — what is really there — although it recorded while the texture was elsewhere.
    // The old model reverted instead, which held only while command lists were the only things moving a layout.
    auto const second_entry = access.finalize(k_second);
    REQUIRE(second_entry.size() == 1);
    CHECK(second_entry[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(second_entry[0].barrier.dst_layout == sg::texture_layout::shader_readonly);
}

TEST("sg vulkan - a dropped list leaves the current layout alone")
{
    auto access = tracker(sg::texture_layout::shader_readonly);
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_first);
    access.discard(k_first);
    CHECK(access.active_slot_count() == 0);

    // Its work never ran, so the next list still enters from the starting layout rather than from copy_dst.
    access.declare(k_second, whole(single()), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_second);
    auto const entry = access.finalize(k_second);
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].barrier.src_layout == sg::texture_layout::shader_readonly);
}

TEST("sg vulkan - mip levels are tracked independently")
{
    // The point of the covering partition: transitioning one mip must not claim to transition its siblings.
    auto const extent = sg::subresource_extent{.mip_count = 4, .array_count = 1, .aspect_count = 1};
    auto access = vulkan::vulkan_texture_access(extent, sg::texture_layout::general);

    auto one_mip = sg::subresource_range();
    one_mip.mip_range = {.start = 1, .end = 2};

    access.declare(k_first, one_mip, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                   sg::texture_layout::copy_dst);
    (void)access.flush(k_first);

    // Only the touched mip carries an entry requirement, so only it is transitioned.
    auto const entry = access.finalize(k_first);
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].range.mip_range.start == 1);
    CHECK(entry[0].range.mip_range.end == 2);
}

TEST("sg vulkan - one texture bound twice to one op yields one barrier")
{
    auto access = tracker();

    // Declared twice for the same op with the same layout; the flush merges them.
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::fragment, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    CHECK(access.mark_pending_barrier(k_first));
    access.declare(k_first, whole(single()), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                   sg::texture_layout::shader_readonly);
    CHECK(!access.mark_pending_barrier(k_first)); // already enqueued for this op

    CHECK(access.flush(k_first).empty()); // the entry transition is the submit's, and both declares merged into it

    auto const entry = access.finalize(k_first);
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].barrier.dst_stages.has(sg::pipeline_stage_flag::fragment));
    CHECK(entry[0].barrier.dst_stages.has(sg::pipeline_stage_flag::compute));
}
