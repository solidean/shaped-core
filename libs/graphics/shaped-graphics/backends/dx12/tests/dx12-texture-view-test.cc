#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

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
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // storage texture → UAV
    {
        auto tex = c.create_dx12_texture(tex_desc(sg::texture_usage::readwrite_texture), sg::allocation_info{});
        REQUIRE(tex.has_value());
        sg::binding const b{.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readwrite_texture};
        auto layout = c.create_dx12_binding_layout(cc::span<sg::binding const>(&b, 1), sg::lifetime_scope::persistent);
        REQUIRE(layout.has_value());

        sg::texture_2d const typed(tex.value());
        sg::named_view const nv{.name = "Tex", .view = typed.as_readwrite_view()};
        auto group = c.create_dx12_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1),
                                                 sg::lifetime_scope::persistent);
        REQUIRE(group.has_value()); // create_texture_view UAV succeeded + the debug layer accepted it
    }

    // sampled texture → SRV
    {
        auto tex = c.create_dx12_texture(tex_desc(sg::texture_usage::readonly_texture), sg::allocation_info{});
        REQUIRE(tex.has_value());
        sg::binding const b{.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_texture};
        auto layout = c.create_dx12_binding_layout(cc::span<sg::binding const>(&b, 1), sg::lifetime_scope::persistent);
        REQUIRE(layout.has_value());

        sg::texture_2d const typed(tex.value());
        sg::named_view const nv{.name = "Tex", .view = typed.as_readonly_view()};
        auto group = c.create_dx12_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1),
                                                 sg::lifetime_scope::persistent);
        REQUIRE(group.has_value());
    }
}

TEST("sg dx12 - compute dispatch with a bound storage texture transitions + validates it")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

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

    auto buf
        = c.create_dx12_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                               sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src, sg::allocation_info{});
    REQUIRE(buf.has_value());
    auto tex = c.create_dx12_texture(tex_desc(sg::texture_usage::readwrite_texture), sg::allocation_info{});
    REQUIRE(tex.has_value());

    auto layout = c.create_dx12_binding_layout(shader.bindings, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());
    auto pipeline = c.create_dx12_compute_pipeline(shader, layout.value(), sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value());

    sg::texture_2d const typed(tex.value());
    sg::named_view const views[] = {
        {.name = "Output", .view = buf.value()->as_readwrite_buffer<sg::u32>()},
        {.name = "Tex", .view = typed.as_readwrite_view()},
    };
    auto group = c.create_dx12_binding_group(layout.value(), cc::span<sg::named_view const>(views, 2),
                                             sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

    auto disp = c.create_dx12_command_list();
    REQUIRE(disp.has_value());
    disp.value()->compute.bind_pipeline(*pipeline.value());
    disp.value()->compute.bind_group(0, *group.value());
    disp.value()->compute.dispatch_threads(count);
    c.submit_dx12_command_list(cc::move(disp.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.data_from_buffer<sg::u32>(buf.value(), 0, count);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const data = c.wait_for(future);
    REQUIRE(data.has_value());
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != cc::u32(i) * 2)
            ok = false;
    CHECK(ok);
}
