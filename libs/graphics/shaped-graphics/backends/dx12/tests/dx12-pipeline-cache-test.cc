#include "dx12-test-common.hh"

#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh> // brings ctx.cached (context.cached.hh) + pipeline_cache

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2). See dx12-compute-test.cc.
#include "double_compute.dxil.h"

// Exercises the sg-level built-in cache (ctx.cached) end to end on WARP: group-layout and pipeline-layout
// acquire each dedup to one handle, compute-pipeline acquire dedups to one async node, the async resolves
// (driven inline here — no pool installed), and the cached pipeline actually dispatches correctly.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::compiled_shader make_double_shader()
{
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    shader.bytecode = cc::make_pinned_data(cc::span<cc::byte const>(
        reinterpret_cast<cc::byte const*>(double_compute_dxil), cc::isize(sizeof(double_compute_dxil))));
    shader.bindings.push_back(sg::binding{
        .name = "Output",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });
    return shader;
}
} // namespace

TEST("sg pipeline_cache - ctx.cached dedups group layout + pipeline layout + async compute pipeline")
{
    auto handle = dx12::make_warp_context(); // fresh context -> empty built-in cache
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();

    // Group-layout caching: identical bindings return the same shared handle.
    auto group_layout1 = ctx.cached.acquire_binding_group_layout(shader.bindings);
    auto group_layout2 = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout1 != nullptr);
    CHECK(group_layout1.get() == group_layout2.get());

    // Pipeline-layout caching: identical (ordered) group layouts return the same shared handle.
    auto pipeline_layout1 = ctx.cached.acquire_pipeline_layout({.groups = {group_layout1}});
    auto pipeline_layout2 = ctx.cached.acquire_pipeline_layout({.groups = {group_layout1}});
    REQUIRE(pipeline_layout1 != nullptr);
    CHECK(pipeline_layout1.get() == pipeline_layout2.get());

    // Compute-pipeline caching: identical (shader, pipeline layout) return the same async node.
    sg::compute_pipeline_description const desc{.shader = shader, .layout = pipeline_layout1};
    auto p1 = ctx.cached.acquire_compute_pipeline(desc);
    auto p2 = ctx.cached.acquire_compute_pipeline(desc);
    REQUIRE(p1 != nullptr);
    CHECK(p1.get() == p2.get());

    // Drive the async build inline (no pool installed) and confirm it resolved to a real pipeline.
    sg::compute_pipeline_handle pipeline = cc::async_blocking_get(p1);
    REQUIRE(pipeline != nullptr);
    CHECK(pipeline->workgroup_size().x == 64);

    // End-to-end: dispatch with the cached pipeline and read the result back.
    constexpr int count = 256; // multiple of the 64-thread workgroup
    auto buf = ctx.persistent.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);

    sg::named_view const out{.name = "Output", .view = sg::buffer<sg::u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout1, cc::span<sg::named_view const>(&out, 1));
    REQUIRE(group != nullptr);

    auto disp = ctx.create_command_list();
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(count);
    ctx.submit_command_list(cc::move(disp));

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<sg::u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(down));

    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == cc::isize(count));
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != cc::u32(i) * 2)
            ok = false;
    CHECK(ok);
}

TEST("sg pipeline_cache - static samplers participate in the layout key")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    // A texture SRV (t0) plus one static sampler (s0). The sampler is baked into the layout, so it must
    // be part of the cache key: the same bindings with a different static sampler is a different layout.
    sg::binding const bindings[] = {
        {.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_texture},
        {.name = "Samp", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::sampler},
    };
    sg::named_sampler const clamp[] = {{.name = "Samp", .sampler = {.address_u = sg::sampler_address_mode::clamp_edge}}};
    sg::named_sampler const repeat[] = {{.name = "Samp", .sampler = {.address_u = sg::sampler_address_mode::repeat}}};

    auto a = ctx.cached.acquire_binding_group_layout(bindings, clamp);
    auto a_again = ctx.cached.acquire_binding_group_layout(bindings, clamp);
    auto b = ctx.cached.acquire_binding_group_layout(bindings, repeat);

    REQUIRE(a != nullptr);
    CHECK(a.get() == a_again.get()); // identical (bindings, static samplers) => one shared handle
    CHECK(a.get() != b.get());       // a different static sampler => a different cached group layout
}

TEST("sg pipeline_cache - a different shader yields a different pipeline node")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    sg::compute_pipeline_description const desc{.shader = shader, .layout = pipeline_layout};
    auto base = ctx.cached.acquire_compute_pipeline(desc);

    // Same pipeline layout but shader content differs (entry point) -> different key -> different node.
    sg::compiled_shader altered = make_double_shader();
    altered.entry_point = "other_main";
    sg::compute_pipeline_description const desc2{.shader = altered, .layout = pipeline_layout};
    auto other = ctx.cached.acquire_compute_pipeline(desc2);

    CHECK(base.get() != other.get());
}

TEST("sg pipeline_cache - pipeline-level static samplers participate in the pipeline-layout key")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::binding const bindings[] = {
        {.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_texture},
    };
    auto gl = ctx.cached.acquire_binding_group_layout(bindings);
    REQUIRE(gl != nullptr);

    // Same group layout, but the pipeline layout carries a register-bound static sampler that changes the
    // root signature — so it must be part of the pipeline-layout key.
    auto make_desc = [&](sg::sampler_address_mode mode)
    {
        sg::pipeline_layout_description d;
        d.groups = {gl};
        d.static_samplers.push_back(
            {.binding = {.name = "Samp", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::sampler},
             .sampler = {.address_u = mode}});
        return d;
    };

    auto a = ctx.cached.acquire_pipeline_layout(make_desc(sg::sampler_address_mode::clamp_edge));
    auto a_again = ctx.cached.acquire_pipeline_layout(make_desc(sg::sampler_address_mode::clamp_edge));
    auto b = ctx.cached.acquire_pipeline_layout(make_desc(sg::sampler_address_mode::repeat));

    REQUIRE(a != nullptr);
    CHECK(a.get() == a_again.get()); // identical static sampler => one shared pipeline layout
    CHECK(a.get() != b.get());       // a different static sampler => a different cached pipeline layout
}

TEST("sg pipeline_cache - inline constants participate in the pipeline-layout key")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::binding const bindings[] = {
        {.name = "Out", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readwrite_structured_buffer},
    };
    auto gl = ctx.cached.acquire_binding_group_layout(bindings);
    REQUIRE(gl != nullptr);

    // Same group layout, but an inline-constants block adds a 32-bit-constants root parameter that changes
    // the root signature — so it (and its block_size) must be part of the pipeline-layout key.
    auto make_desc = [&](cc::isize block_size)
    {
        sg::pipeline_layout_description d;
        d.groups = {gl};
        d.inline_constants = sg::binding{.name = "Params",
                                         .set = 0,
                                         .index = 0,
                                         .count = 1,
                                         .type = sg::binding_type::uniform_buffer,
                                         .block_size = block_size};
        return d;
    };

    auto no_ic = ctx.cached.acquire_pipeline_layout({.groups = {gl}});
    auto ic8 = ctx.cached.acquire_pipeline_layout(make_desc(8));
    auto ic8_again = ctx.cached.acquire_pipeline_layout(make_desc(8));
    auto ic16 = ctx.cached.acquire_pipeline_layout(make_desc(16));

    REQUIRE(no_ic != nullptr);
    CHECK(ic8.get() == ic8_again.get()); // identical inline constants => one shared pipeline layout
    CHECK(ic8.get() != ic16.get());      // a different block_size => a different cached pipeline layout
    CHECK(ic8.get() != no_ic.get());     // presence of inline constants => a different cached layout
}
