#include "vulkan-test-common.hh"

#include <clean-core/thread/async.hh> // cc::async_blocking_get
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded SPIR-V for double_compute.hlsl (Output[i] = i*2). See that file for the dxc command.
#include "double_compute.spirv.h"

using namespace cc::primitive_defines;

// End-to-end compute bind path: build a compiled_shader from the embedded blob plus hand-authored reflection, then
// create the layout / pipeline / binding_group, dispatch, and read the buffer back checking every element.
//
// Everything drives the public sg API; the only vulkan-specific piece is the SPIR-V blob, since shader bytecode is
// inherently per-backend — which is exactly why this is a tier-2 test and its dx12 twin is another.
// There is no tier-1 compute test to inherit: a dispatch cannot be written without bytecode.

namespace
{
namespace vulkan = sg::backend::vulkan;

// The compiled compute shader for double_compute.hlsl: embedded SPIR-V + its hand-authored reflection —
// one read-write structured binding "Output" at (set 0, index 0).
//
// `group_index` rather than `space` is the SPIR-V shape of a binding, and it is what the shader's
// `[[vk::binding(0, 0)]]` annotation actually declares.
sg::compiled_shader make_double_shader()
{
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::spirv;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    shader.bytecode = cc::make_pinned_data(
        cc::span<byte const>(reinterpret_cast<byte const*>(double_compute_spirv), isize(sizeof(double_compute_spirv))));
    shader.bindings.push_back(sg::binding{
        .name = "Output",
        .group_index = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });
    return shader;
}
} // namespace

TEST("sg vulkan - compute dispatch writes a structured buffer")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    constexpr int count = 256; // a multiple of the shader's 64-thread workgroup

    sg::compiled_shader const shader = make_double_shader();

    // The output buffer: a storage buffer for the dispatch + copy_src to read it back.
    auto buf = ctx.persistent.create_raw_buffer(isize(count) * isize(sizeof(u32)),
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);

    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = cc::async_blocking_get(ctx.cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout}));
    REQUIRE(pipeline != nullptr);

    sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
    REQUIRE(group != nullptr);

    // dispatch_threads auto-divides by the shader's 64-thread workgroup, so this is the count/64 = 4 groups a raw
    // dispatch_groups would spell out.
    auto disp = ctx.create_command_list();
    REQUIRE(disp != nullptr);
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(count);
    ctx.submit_command_list(cc::move(disp));

    // Read the result back in a second list.
    // Unlike dx12 there is no state decay between submits to ride on, so the RAW hazard from the dispatch to this
    // copy is carried by the buffer's canonical access state — which is what makes this test worth its own tier.
    auto down = ctx.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(down));

    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == isize(count));
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != u32(i) * 2)
            ok = false;
    CHECK(ok);
}

// The same dispatch with a TRANSIENT output buffer and TRANSIENT binding group each epoch, on a deliberately tiny
// descriptor heap so 40 iterations wrap its transient region many times over.
//
// This is what proves the heap's per-epoch checkpointing rather than its arithmetic: a transient range may only be
// reused once the epoch that wrote its descriptors has retired, and a GPU reading recycled bytes would show up as
// wrong data rather than as a validation message.
// The hand-sized heap is a vulkan knob, so this takes a vulkan config directly; the work itself is all public sg API.
TEST("sg vulkan - transient binding groups and buffers recycle across epochs")
{
    // Small enough that 40 epochs cannot all fit, large enough for one group's set plus alignment.
    auto handle = vulkan::test::make_context(
        {.enable_validation_layers = true, .descriptor_heap_bytes = 16 * 1024, .descriptor_transient_fraction = 0.5f});
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    constexpr int count = 256;
    sg::compiled_shader const shader = make_double_shader();

    // Layouts and pipeline are cached schemas — always persistent, built once.
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = cc::async_blocking_get(ctx.cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout}));
    REQUIRE(pipeline != nullptr);

    bool all_ok = true;
    for (int e = 0; e < 40; ++e)
    {
        auto buf = ctx.transient.create_raw_buffer(isize(count) * isize(sizeof(u32)),
                                                   sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        REQUIRE(buf != nullptr);

        sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
        auto group = ctx.transient.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
        REQUIRE(group != nullptr);

        auto disp = ctx.create_command_list();
        disp->compute.bind_pipeline(*pipeline);
        disp->compute.bind_group(0, *group);
        disp->compute.dispatch_threads(count);
        ctx.submit_command_list(cc::move(disp));

        auto down = ctx.create_command_list();
        auto future = down->download.data_from_buffer<u32>(buf, 0, count);
        ctx.submit_command_list(cc::move(down));

        auto const data = ctx.wait_for(future);
        REQUIRE(data.has_value());
        for (int i = 0; i < count; ++i)
            if (data.value()[i] != u32(i) * 2)
                all_ok = false;

        ctx.advance_epoch(2); // keep at most 2 epochs in flight, so the rings reclaim older windows
    }
    CHECK(all_ok); // one check for 40 epochs: a per-epoch check would bury the failure that matters
}

// Create and release many PERSISTENT binding groups on a tiny persistent region.
//
// A persistent group's range goes back to the heap's free list when it is released — epoch-deferred, since the GPU may
// still be reading it — so 50 iterations reuse the same few bytes.
// A bump allocator would exhaust after the first handful, which is what this distinguishes.
// The region is sized so that it does: holding the 50 groups instead of releasing them fails this test, which is what
// makes the free list load-bearing here rather than merely present.
TEST("sg vulkan - persistent binding groups free and reuse their descriptor range")
{
    auto handle = vulkan::test::make_context(
        {.enable_validation_layers = true, .descriptor_heap_bytes = 256, .descriptor_transient_fraction = 0.5f});
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);

    auto buf = ctx.persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf != nullptr);
    sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};

    bool all_created = true;
    for (int i = 0; i < 50; ++i)
    {
        auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
        if (group == nullptr)
            all_created = false;

        // Releasing here stages the range's return; the advance below is what actually runs it.
        group = nullptr;
        ctx.advance_epoch_and_wait_for_idle();
    }
    CHECK(all_created);
}
