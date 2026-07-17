#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2), reused here with an extra storage-texture
// binding the shader doesn't touch — enough to drive the texture UAV descriptor + the dispatch barrier.
#include "double_compute.dxil.h"

// End-to-end texture views on WARP: a texture's as_*_view() becomes a real D3D12 SRV/UAV inside a binding
// group, and binding one to a compute dispatch transitions it to the right layout via the barrier system.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::texture_description tex_desc(sg::texture_usage usage)
{
    sg::texture_description d;
    d.format = sg::pixel_format::rgba8_unorm;
    d.dimension = sg::texture_dimension::d2;
    d.width = 64;
    d.height = 64;
    d.usage = usage;
    return d;
}
} // namespace

TEST("sg dx12 - storage / sampled texture views create valid UAV / SRV descriptors")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    // storage texture → UAV
    {
        auto tex = c.persistent.create_raw_texture(tex_desc(sg::texture_usage::readwrite_texture));
        REQUIRE(tex != nullptr);
        sg::binding const b{.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readwrite_texture};
        auto layout = c.uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
        REQUIRE(layout != nullptr);

        auto const typed = sg::texture_2d::from_raw(tex);
        sg::named_view const nv{.name = "Tex", .view = typed.as_readwrite_view()};
        auto group = c.persistent.create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
        REQUIRE(group != nullptr); // create_texture_view UAV succeeded + the debug layer accepted it
    }

    // sampled texture → SRV
    {
        auto tex = c.persistent.create_raw_texture(tex_desc(sg::texture_usage::readonly_texture));
        REQUIRE(tex != nullptr);
        sg::binding const b{.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_texture};
        auto layout = c.uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
        REQUIRE(layout != nullptr);

        auto const typed = sg::texture_2d::from_raw(tex);
        sg::named_view const nv{.name = "Tex", .view = typed.as_readonly_view()};
        auto group = c.persistent.create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
        REQUIRE(group != nullptr);
    }
}

TEST("sg dx12 - compute dispatch with a bound storage texture transitions + validates it")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    constexpr int count = 256;

    // The double_compute shader writes Output[i]=i*2; the extra "Tex" storage-texture binding is unused by
    // the shader but exercises the texture UAV descriptor + the COMMON→storage layout barrier at dispatch.
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    shader.bytecode = cc::make_pinned_data(cc::span<cc::byte const>(
        reinterpret_cast<cc::byte const*>(double_compute_dxil), cc::isize(sizeof(double_compute_dxil))));
    shader.bindings.push_back(sg::binding{.name = "Output",
                                          .set = 0,
                                          .index = 0,
                                          .count = 1,
                                          .type = sg::binding_type::readwrite_structured_buffer});
    shader.bindings.push_back(
        sg::binding{.name = "Tex", .set = 0, .index = 1, .count = 1, .type = sg::binding_type::readwrite_texture});

    auto buf = c.persistent.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                                              sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);
    auto tex = c.persistent.create_raw_texture(tex_desc(sg::texture_usage::readwrite_texture));
    REQUIRE(tex != nullptr);

    auto group_layout = c.uncached.create_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = c.uncached.create_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = c.uncached.create_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    auto const typed = sg::texture_2d::from_raw(tex);
    sg::named_view const views[] = {
        {.name = "Output", .view = sg::buffer<sg::u32>::from_raw(buf).as_readwrite_buffer()},
        {.name = "Tex", .view = typed.as_readwrite_view()},
    };
    auto group = c.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(views, 2));
    REQUIRE(group != nullptr);

    auto disp = c.create_command_list();
    REQUIRE(disp != nullptr);
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(count);
    c.submit_command_list(cc::move(disp));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<sg::u32>(buf, 0, count);
    c.submit_command_list(cc::move(down));

    auto const data = c.wait_for(future);
    REQUIRE(data.has_value());
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != cc::u32(i) * 2)
            ok = false;
    CHECK(ok);
}
