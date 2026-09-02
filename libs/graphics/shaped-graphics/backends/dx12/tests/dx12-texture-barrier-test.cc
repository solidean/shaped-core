#include "dx12-test-common.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_access.hh>

// Texture subresource barrier tracking plus dx12 emission.
// The tracker (dx12_texture_access) is pure logic, so most of this is GPU-free; a final WARP smoke test drives real D3D12_TEXTURE_BARRIERs through the debug layer.
// These drive the tracker directly, rather than through a public op recording against the texture.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::texture_description desc_2d(sg::pixel_format fmt, int w, int h, int mips = 1)
{
    sg::texture_description d;
    d.format = fmt;
    d.dimension = sg::texture_dimension::d2;
    d.width = w;
    d.height = h;
    d.mip_levels = mips;
    return d;
}

// A whole-subresource range for `d`.
sg::subresource_range whole_of(sg::texture_description const& d)
{
    return sg::subresource_range::whole(dx12::subresource_extent_of(d));
}

// Declare one access and immediately flush it — models a single op declaring a single binding, returning the barriers that op would emit.
// The real path declares every binding first, then flushes once; these tests each declare a single access per op.
cc::small_vector<dx12::dx12_subresource_barrier, 4> declare_flush(dx12::dx12_texture_access& acc,
                                                                  sg::command_list_slot slot,
                                                                  sg::subresource_range range,
                                                                  sg::pipeline_stage_flags stages,
                                                                  sg::access_flags access,
                                                                  sg::texture_layout layout)
{
    acc.declare(slot, range, stages, access, layout);
    return acc.flush(slot);
}
} // namespace

TEST("sg dx12 - subresource_extent_of maps the texture grid")
{
    auto e1 = dx12::subresource_extent_of(desc_2d(sg::pixel_format::rgba8_unorm, 64, 64, 3));
    CHECK(e1.mip_count == 3);
    CHECK(e1.array_count == 1);
    CHECK(e1.aspect_count == 1);

    // A cube is 6 array slices per cube; a cube array of 2 is 12.
    sg::texture_description cube = desc_2d(sg::pixel_format::rgba8_unorm, 32, 32);
    cube.is_cube = true;
    CHECK(dx12::subresource_extent_of(cube).array_count == 6);
    cube.array_layers = 2;
    CHECK(dx12::subresource_extent_of(cube).array_count == 12);

    // A combined depth+stencil format has two aspect planes.
    auto ds = dx12::subresource_extent_of(desc_2d(sg::pixel_format::depth32_float_stencil8, 16, 16));
    CHECK(ds.aspect_count == 2);
    auto depth = dx12::subresource_extent_of(desc_2d(sg::pixel_format::depth32_float, 16, 16));
    CHECK(depth.aspect_count == 1);
}

TEST("sg dx12 - texture access declares layout transitions")
{
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    // First use as a copy dest: the list enters the box at copy_dst, so its body emits nothing.
    // The transition from the current (general / COMMON) layout is the submit's, out of finalize.
    auto b0 = declare_flush(acc, slot, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                            sg::texture_layout::copy_dst);
    CHECK(b0.empty());

    // Then sample it: transition copy_dst → shader_readonly (a read-after-write hazard across layouts).
    auto b1 = declare_flush(acc, slot, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                            sg::texture_layout::shader_readonly);
    REQUIRE(b1.size() == 1);
    CHECK(b1[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(b1[0].barrier.dst_layout == sg::texture_layout::shader_readonly);

    // And the entry barrier the submit prepends carries the first use, from where the texture really is.
    auto const entry = acc.finalize(slot);
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].barrier.needed);
    CHECK(entry[0].barrier.src_layout == sg::texture_layout::general);
    CHECK(entry[0].barrier.dst_layout == sg::texture_layout::copy_dst);
    CHECK(entry[0].barrier.dst_access.has(sg::access_flag::copy_write));
}

TEST("sg dx12 - multiple declares before one flush merge into a single barrier")
{
    // A resource bound more than once to the same op declares more than once before the op's single flush.
    // The flush must emit ONE barrier carrying the union of the declared stages/access — not one per declare.
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    // Same texture bound twice: a read and a read-write, both needing the read-write (UAV) layout.
    acc.declare(slot, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                sg::texture_layout::shader_readwrite);
    acc.declare(slot, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_write,
                sg::texture_layout::shader_readwrite);
    auto b = acc.flush(slot);

    REQUIRE(b.size() == 1); // one merged barrier for the box, not two
    CHECK(b[0].barrier.dst_layout == sg::texture_layout::shader_readwrite);
    CHECK(b[0].barrier.dst_access.has_all(sg::access_flag::shader_read | sg::access_flag::shader_write));
}

TEST("sg dx12 - combine_layouts folds sampled+storage to COMMON and flags real conflicts")
{
    using sg::texture_layout;
    // Equal layouts combine cleanly.
    CHECK(dx12::combine_layouts(texture_layout::shader_readonly, texture_layout::shader_readonly).result
          == dx12::layout_combine::ok);
    // Sampled (SRV) + storage (UAV): no specialized layout serves both, so COMMON, degraded (order-independent).
    auto const c = dx12::combine_layouts(texture_layout::shader_readonly, texture_layout::shader_readwrite);
    CHECK(c.layout == texture_layout::general);
    CHECK(c.result == dx12::layout_combine::degraded);
    CHECK(dx12::combine_layouts(texture_layout::shader_readwrite, texture_layout::shader_readonly).layout
          == texture_layout::general);
    // general/COMMON already serves any access.
    CHECK(dx12::combine_layouts(texture_layout::general, texture_layout::shader_readwrite).result
          == dx12::layout_combine::ok);
    // A copy dest that is also sampled in one op is a real hazard.
    CHECK(dx12::combine_layouts(texture_layout::copy_dst, texture_layout::shader_readonly).result
          == dx12::layout_combine::conflict);
}

TEST("sg dx12 - a texture bound as sampled + storage in one op transitions to COMMON")
{
    // Two views of one texture in the same op — shader_readonly (SRV) and shader_readwrite (UAV) — combine to the COMMON (general) layout.
    // They arrive as a single barrier carrying both accesses, plus a one-time perf warning.
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    // First put the texture in a non-general layout, so the combined transition is observable (a fresh
    // texture is already general, so SRV+UAV -> general would be a freebie).
    acc.declare(slot, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                sg::texture_layout::copy_dst);
    (void)acc.flush(slot);

    acc.declare(slot, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                sg::texture_layout::shader_readonly);
    acc.declare(slot, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_write,
                sg::texture_layout::shader_readwrite);
    auto b = acc.flush(slot);

    REQUIRE(b.size() == 1);
    CHECK(b[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(b[0].barrier.dst_layout == sg::texture_layout::general); // combined SRV+UAV -> COMMON
    CHECK(b[0].barrier.dst_access.has_all(sg::access_flag::shader_read | sg::access_flag::shader_write));
}

TEST("sg dx12 - mark_pending_barrier enqueues a texture for the flush exactly once per op")
{
    // The command list enqueues a texture for the pre-op barrier flush only when mark_pending_barrier returns true, on the first binding of the op.
    // flush clears the flag, so the next op enqueues it again.
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    acc.declare(slot, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                sg::texture_layout::shader_readwrite);
    CHECK(acc.mark_pending_barrier(slot));  // first binding this op -> enqueue
    CHECK(!acc.mark_pending_barrier(slot)); // already enqueued this op
    (void)acc.flush(slot);                  // flush clears the flag
    CHECK(acc.mark_pending_barrier(slot));  // next op -> enqueue again
}

TEST("sg dx12 - mark_recorded reports the slot's first record")
{
    // The command list uses mark_recorded to add a texture to its finalize set exactly once, in O(1) with no scan.
    // True the first time per slot, false after, and true again once the slot is cleared.
    // In the real flow a slot is always declared (seeded active) before it is recorded, and discard requires an active slot.
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const s0 = sg::command_list_slot(0);
    auto const s1 = sg::command_list_slot(1);
    (void)acc.declare(s0, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                      sg::texture_layout::copy_dst);
    (void)acc.declare(s1, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                      sg::texture_layout::copy_dst);

    CHECK(acc.mark_recorded(s0));  // first record of slot 0
    CHECK(!acc.mark_recorded(s0)); // already recorded
    CHECK(acc.mark_recorded(s1));  // a different slot is independent

    acc.discard(s0);              // dropping slot 0 clears its recorded flag
    CHECK(acc.mark_recorded(s0)); // records again
}

TEST("sg dx12 - texture access fragments per subresource range")
{
    // Two mips; touch each separately with a different layout, then the whole texture at once.
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64, 2);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    sg::subresource_range mip0; // default = mip [0,1), array [0,1), aspect [0,1)
    sg::subresource_range mip1;
    mip1.mip_range = {.start = 1, .end = 2};

    // Each mip enters at its own first use, so neither first touch emits anything in the body.
    CHECK(declare_flush(acc, slot, mip0, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                        sg::texture_layout::copy_dst)
              .empty());
    CHECK(declare_flush(acc, slot, mip1, sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                        sg::texture_layout::shader_readonly)
              .empty());

    // The whole texture now spans two differently-laid-out boxes → one barrier each.
    auto c = declare_flush(acc, slot, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_read,
                           sg::texture_layout::copy_src);
    CHECK(c.size() == 2);

    // And the entry barriers keep the mips apart too: one per box, each from the layout it was really in.
    auto const entry = acc.finalize(slot);
    REQUIRE(entry.size() == 2);
    CHECK(entry[0].range.mip_range.end - entry[0].range.mip_range.start == 1);
    CHECK(entry[1].range.mip_range.end - entry[1].range.mip_range.start == 1);
}

TEST("sg dx12 - every submit commits its layout, and the next list enters from it")
{
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    (void)declare_flush(acc, slot, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                        sg::texture_layout::copy_dst);
    auto out = acc.finalize(slot);
    REQUIRE(out.size() == 1);
    CHECK(out[0].barrier.src_layout == sg::texture_layout::general);
    CHECK(out[0].barrier.dst_layout == sg::texture_layout::copy_dst);

    // A fresh list enters from the committed copy_dst layout: re-declaring copy_dst needs no layout transition
    // (only a write-after-write hazard against the committed write remains).
    auto const slot2 = sg::command_list_slot(0);
    (void)declare_flush(acc, slot2, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                        sg::texture_layout::copy_dst);
    for (auto const& sb : acc.finalize(slot2))
        CHECK(sb.barrier.src_layout == sb.barrier.dst_layout); // no layout change, at most a WAW barrier
}

TEST("sg dx12 - a concurrently recorded list enters from what the earlier one left")
{
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const s0 = sg::command_list_slot(0);
    auto const s1 = sg::command_list_slot(1);

    // Two concurrent lists touch the same texture: s0 -> copy_dst, s1 -> shader_readonly (active count 2).
    // Neither can know what the texture will be in when it submits, so neither records a transition into it.
    (void)declare_flush(acc, s0, whole_of(d), sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                        sg::texture_layout::copy_dst);
    (void)declare_flush(acc, s1, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                        sg::texture_layout::shader_readonly);

    // s0 submits first, entering from the layout the texture starts in and leaving it in copy_dst.
    auto r0 = acc.finalize(s0);
    REQUIRE(r0.size() == 1);
    CHECK(r0[0].barrier.src_layout == sg::texture_layout::general);
    CHECK(r0[0].barrier.dst_layout == sg::texture_layout::copy_dst);

    // s1 submits second, and enters from copy_dst — what is really there — although it recorded before that.
    auto r1 = acc.finalize(s1);
    REQUIRE(r1.size() == 1);
    CHECK(r1[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(r1[0].barrier.dst_layout == sg::texture_layout::shader_readonly);

    // A fresh list now enters from the committed shader_readonly layout: re-declaring it needs no transition.
    auto const s2 = sg::command_list_slot(0);
    (void)declare_flush(acc, s2, whole_of(d), sg::pipeline_stage_flag::compute, sg::access_flag::shader_read,
                        sg::texture_layout::shader_readonly);
    for (auto const& sb : acc.finalize(s2))
        CHECK(sb.barrier.src_layout == sb.barrier.dst_layout);
}

TEST("sg dx12 - d3d12_layout_from maps the layouts")
{
    CHECK(dx12::d3d12_layout_from(sg::texture_layout::undefined) == D3D12_BARRIER_LAYOUT_UNDEFINED);
    CHECK(dx12::d3d12_layout_from(sg::texture_layout::general) == D3D12_BARRIER_LAYOUT_COMMON);
    CHECK(dx12::d3d12_layout_from(sg::texture_layout::shader_readonly) == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    CHECK(dx12::d3d12_layout_from(sg::texture_layout::shader_readwrite) == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    CHECK(dx12::d3d12_layout_from(sg::texture_layout::copy_dst) == D3D12_BARRIER_LAYOUT_COPY_DEST);
    CHECK(dx12::d3d12_layout_from(sg::texture_layout::copy_src) == D3D12_BARRIER_LAYOUT_COPY_SOURCE);
}

INVOCABLE_TEST("sg dx12 - emits well-formed texture barriers on WARP", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    auto tex = c.create_dx12_texture(d, sg::allocation_info{});
    REQUIRE(tex.has_value());
    auto const& dtex = tex.value();

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    auto const range = whole_of(d);

    // Drive the declare → emit path by hand, the way track_texture_access does for a real op.
    // The list enters at copy_dst and then transitions copy_dst → shader_readonly in its own body; the entry
    // transition out of COMMON is finalize's, and goes into the pre-list executed ahead of this one — which is what
    // submit does for a real list.
    auto emit = [&](ID3D12GraphicsCommandList* list, cc::span<dx12::dx12_subresource_barrier const> barriers)
    {
        cc::vector<D3D12_TEXTURE_BARRIER> batch;
        for (auto const& sb : barriers)
            batch.push_back(dx12::make_texture_barrier(dtex->_resource.Get(), sb.range, sb.barrier));
        dx12::submit_barriers(list, {}, batch);
    };
    dtex->declare_texture_access(cmd.value()->slot(), range, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write,
                                 sg::texture_layout::copy_dst);
    emit(cmd.value()->_list.Get(), dtex->flush_texture_access(cmd.value()->slot()));
    dtex->declare_texture_access(cmd.value()->slot(), range, sg::pipeline_stage_flag::compute,
                                 sg::access_flag::shader_read, sg::texture_layout::shader_readonly);
    emit(cmd.value()->_list.Get(), dtex->flush_texture_access(cmd.value()->slot()));
    // The pre-list is created on demand, so a caller recording into it by hand has to ask for one.
    emit(c.acquire_pre_list(*cmd.value()), dtex->finalize_slot(cmd.value()->slot()));

    c.submit_dx12_command_list(cc::move(cmd.value()));
    c.advance_epoch_and_wait_for_idle();

    // Getting here without a device-removed means the debug layer accepted the texture barriers.
    CHECK(!c.is_shut_down());
}
