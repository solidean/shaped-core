#include "dx12-test-common.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_access.hh>

// Texture subresource barrier tracking + dx12 emission. The tracker (dx12_texture_access) is pure logic,
// so most of this is GPU-free; a final WARP smoke test drives real D3D12_TEXTURE_BARRIERs through the
// debug layer. No public op records against a texture yet — these drive the wired substrate directly.

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

    // First use as a copy dest: transition from the committed (general / COMMON) layout to transfer_dst.
    auto b0 = acc.declare(slot, whole_of(d), sg::pipeline_stage_flags::copy, sg::access_flags::copy_write,
                          sg::texture_layout::copy_dst);
    REQUIRE(b0.size() == 1);
    CHECK(b0[0].barrier.needed);
    CHECK(b0[0].barrier.src_layout == sg::texture_layout::general);
    CHECK(b0[0].barrier.dst_layout == sg::texture_layout::copy_dst);
    CHECK(sg::has_all(b0[0].barrier.dst_access, sg::access_flags::copy_write));

    // Then sample it: transition transfer_dst → shader_read (a read-after-write hazard across layouts).
    auto b1 = acc.declare(slot, whole_of(d), sg::pipeline_stage_flags::compute, sg::access_flags::shader_read,
                          sg::texture_layout::shader_readonly);
    REQUIRE(b1.size() == 1);
    CHECK(b1[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(b1[0].barrier.dst_layout == sg::texture_layout::shader_readonly);
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

    auto a = acc.declare(slot, mip0, sg::pipeline_stage_flags::copy, sg::access_flags::copy_write,
                         sg::texture_layout::copy_dst);
    REQUIRE(a.size() == 1);
    CHECK(a[0].range.mip_range.start == 0);
    CHECK(a[0].range.mip_range.end == 1);

    auto b = acc.declare(slot, mip1, sg::pipeline_stage_flags::compute, sg::access_flags::shader_read,
                         sg::texture_layout::shader_readonly);
    REQUIRE(b.size() == 1);
    CHECK(b[0].range.mip_range.start == 1);

    // The whole texture now spans two differently-laid-out boxes → one barrier each.
    auto c = acc.declare(slot, whole_of(d), sg::pipeline_stage_flags::copy, sg::access_flags::copy_read,
                         sg::texture_layout::copy_src);
    CHECK(c.size() == 2);
}

TEST("sg dx12 - a non-final submit reverts the texture to its entry layout")
{
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));

    // Two concurrent lists. slot0 transitions the texture to transfer_dst.
    auto const s0 = sg::command_list_slot(0);
    (void)acc.declare(s0, whole_of(d), sg::pipeline_stage_flags::copy, sg::access_flags::copy_write,
                      sg::texture_layout::copy_dst);

    // slot0 submits while slot1 is still open (not the last list): it must restore the entry layout (general).
    auto revert = acc.finalize(s0, /*promote*/ false);
    REQUIRE(revert.size() == 1);
    CHECK(revert[0].barrier.src_layout == sg::texture_layout::copy_dst);
    CHECK(revert[0].barrier.dst_layout == sg::texture_layout::general);
}

TEST("sg dx12 - the final submit promotes without reverting")
{
    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    dx12::dx12_texture_access acc(dx12::subresource_extent_of(d));
    auto const slot = sg::command_list_slot(0);

    (void)acc.declare(slot, whole_of(d), sg::pipeline_stage_flags::copy, sg::access_flags::copy_write,
                      sg::texture_layout::copy_dst);
    auto out = acc.finalize(slot, /*promote*/ true);
    CHECK(out.empty()); // promote commits the new layout; nothing to emit

    // A fresh list now seeds from the committed transfer_dst layout: re-declaring transfer_dst needs no
    // layout transition (only a write-after-write hazard against the committed write remains).
    auto const slot2 = sg::command_list_slot(0);
    auto again = acc.declare(slot2, whole_of(d), sg::pipeline_stage_flags::copy, sg::access_flags::copy_write,
                             sg::texture_layout::copy_dst);
    for (auto const& sb : again)
        CHECK(sb.barrier.src_layout == sb.barrier.dst_layout); // no layout change, at most a WAW barrier
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

TEST("sg dx12 - emits well-formed texture barriers on WARP")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const d = desc_2d(sg::pixel_format::rgba8_unorm, 64, 64);
    auto tex = c.create_dx12_texture(d, sg::allocation_info{});
    REQUIRE(tex.has_value());
    auto const& dtex = tex.value();

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    auto const range = whole_of(d);

    // Drive the declare → emit path by hand (the future public texture op will do this via
    // track_texture_access): general → copy_dst, then copy_dst → shader_readonly.
    auto emit = [&](cc::span<dx12::dx12_subresource_barrier const> barriers)
    {
        cc::vector<D3D12_TEXTURE_BARRIER> batch;
        for (auto const& sb : barriers)
            batch.push_back(dx12::make_texture_barrier(dtex->_resource.Get(), sb.range, sb.barrier));
        dx12::submit_barriers(cmd.value()->_list.Get(), {}, batch);
    };
    emit(dtex->declare_texture_access(cmd.value()->slot(), range, sg::pipeline_stage_flags::copy,
                                      sg::access_flags::copy_write, sg::texture_layout::copy_dst));
    emit(dtex->declare_texture_access(cmd.value()->slot(), range, sg::pipeline_stage_flags::compute,
                                      sg::access_flags::shader_read, sg::texture_layout::shader_readonly));
    emit(dtex->finalize_slot(cmd.value()->slot(), /*promote*/ true));

    c.submit_dx12_command_list(cc::move(cmd.value()));
    c.advance_epoch_and_wait_for_idle();

    // Getting here without a device-removed means the debug layer accepted the texture barriers.
    CHECK(!c.is_shut_down());
}
