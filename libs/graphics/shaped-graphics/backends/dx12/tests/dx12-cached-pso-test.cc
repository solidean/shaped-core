#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2). See dx12-compute-test.cc.
#include "double_compute.dxil.h"

// Exercises the optional cached-PSO path on WARP: a pipeline's serialized blob (cached_pipeline_data)
// can seed a second pipeline's creation (compute_pipeline_description::cached_pipeline), the seeded
// pipeline still dispatches correctly, a garbage blob degrades to a fresh build, and the blob is not
// part of the built-in cache key.

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

// Dispatch `pipeline` over `count` threads, reading back Output[i] and checking it equals i*2.
void check_doubles(sg::context& ctx,
                   sg::compute_pipeline const& pipeline,
                   sg::binding_group_layout_handle group_layout,
                   int count)
{
    auto buf = ctx.persistent.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);

    sg::named_view const out{.name = "Output", .view = sg::buffer<sg::u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
    REQUIRE(group != nullptr);

    auto disp = ctx.create_command_list();
    disp->compute.bind_pipeline(pipeline);
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
} // namespace

TEST("sg cached PSO - round-trips a blob and the seeded pipeline still dispatches")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    // Build a pipeline from scratch, then read its serialized PSO blob.
    auto first = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(first != nullptr);
    auto const blob = first->cached_pipeline_data();
    CHECK(!blob.empty()); // WARP supports GetCachedBlob

    // Seed a second pipeline with that blob and confirm it dispatches identically.
    auto seeded
        = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = blob});
    REQUIRE(seeded != nullptr);
    check_doubles(ctx, *seeded, group_layout, 256);
}

TEST("sg cached PSO - a garbage blob degrades to a fresh build")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    // A stale/mismatched blob must not hard-fail: creation retries without the cache.
    cc::byte const garbage[64] = {};
    auto res = ctx.uncached.try_create_compute_pipeline(
        {.shader = shader,
         .layout = pipeline_layout,
         .cached_pipeline = cc::make_pinned_data(cc::span<cc::byte const>(garbage))});
    REQUIRE(res.has_value());
    check_doubles(ctx, *res.value(), group_layout, 256);
}

TEST("sg cached PSO - the blob is not part of the built-in cache key")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    // Build once to obtain a real blob, then acquire twice: with and without it. Same shader + layout =>
    // same async node regardless of the accelerator blob.
    auto first = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(first != nullptr);
    auto const blob = first->cached_pipeline_data();

    auto a = ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    auto b = ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = blob});
    REQUIRE(a != nullptr);
    CHECK(a.get() == b.get());
}
