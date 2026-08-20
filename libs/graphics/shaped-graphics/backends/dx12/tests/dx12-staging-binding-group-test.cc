#include "dx12-test-common.hh"

#include <clean-core/thread/async.hh> // cc::async_blocking_get
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2). See that file for the dxc command.
#include "double_compute.dxil.h"

using namespace cc::primitive_defines;

// What only a real device can answer about staging_binding_group: that the descriptors a snapshot copies out
// of the staging heap actually drive a dispatch, that re-pointing one binding takes effect on the next
// snapshot, and that snapshots are persistent groups whose ranges come back to the heap.
// A snapshot's CopyDescriptorsSimple would be a debug-layer error if the staging heap were shader-visible, and
// the suite fails a test on any validation message, so the correct heap flags are checked by construction here.
// Everything drives the public sg API bar the DXIL blob and the hand-sized descriptor heap, which are dx12 knobs.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr int k_count = 256; // a multiple of the shader's 64-thread workgroup

// The compiled compute shader for double_compute.hlsl: embedded DXIL plus its hand-authored reflection —
// one read-write structured binding "Output" at (set 0, index 0).
sg::compiled_shader make_double_shader()
{
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    shader.bytecode = cc::make_pinned_data(
        cc::span<byte const>(reinterpret_cast<byte const*>(double_compute_dxil), isize(sizeof(double_compute_dxil))));
    shader.bindings.push_back(sg::binding{
        .name = "Output",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });
    return shader;
}

sg::raw_buffer_handle make_output_buffer(sg::context_handle const& ctx)
{
    return ctx->persistent.create_raw_buffer(isize(k_count) * isize(sizeof(u32)),
                                             sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
}

void dispatch_through(sg::context_handle const& ctx, sg::compute_pipeline const& pipeline, sg::binding_group const& group)
{
    auto disp = ctx->create_command_list();
    REQUIRE(disp != nullptr);
    disp->compute.bind_pipeline(pipeline);
    disp->compute.bind_group(0, group);
    disp->compute.dispatch_threads(k_count);
    ctx->submit_command_list(cc::move(disp));
}

// Whether the buffer holds the shader's output, i*2 for every element.
bool holds_doubled(sg::context_handle const& ctx, sg::raw_buffer_handle const& buf)
{
    auto down = ctx->create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<u32>(buf, 0, k_count);
    ctx->submit_command_list(cc::move(down));

    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == isize(k_count));
    for (int i = 0; i < k_count; ++i)
        if (data.value()[i] != u32(i) * 2)
            return false;
    return true;
}
} // namespace

INVOCABLE_TEST("sg dx12 - a staging snapshot drives a dispatch", (dx12::dx12_context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx->cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = cc::async_blocking_get(ctx->cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout}));
    REQUIRE(pipeline != nullptr);

    auto first = make_output_buffer(ctx);
    auto second = make_output_buffer(ctx);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    auto staging = ctx->persistent.create_staging_binding_group(group_layout);
    REQUIRE(staging != nullptr);

    auto const out = staging->slot_of("Output");
    REQUIRE(out != sg::binding_slot::invalid);
    REQUIRE(!staging->is_array(out));

    staging->set_binding(out, sg::buffer<u32>::from_raw(first).as_readwrite_buffer());
    auto const to_first = staging->snapshot();
    REQUIRE(to_first != nullptr);
    dispatch_through(ctx, *pipeline, *to_first);
    CHECK(holds_doubled(ctx, first));

    // Re-point the one binding: the next snapshot is a different group, and it writes the other buffer.
    staging->set_binding(out, sg::buffer<u32>::from_raw(second).as_readwrite_buffer());
    auto const to_second = staging->snapshot();
    REQUIRE(to_second != nullptr);
    CHECK(to_second != to_first);
    dispatch_through(ctx, *pipeline, *to_second);
    CHECK(holds_doubled(ctx, second));
}

INVOCABLE_TEST("sg dx12 - a staging snapshot outlives the epoch that minted it", (dx12::dx12_context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx->cached.acquire_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    auto pipeline = cc::async_blocking_get(ctx->cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout}));
    REQUIRE(pipeline != nullptr);

    auto buf = make_output_buffer(ctx);
    REQUIRE(buf != nullptr);

    auto staging = ctx->persistent.create_staging_binding_group(group_layout);
    REQUIRE(staging != nullptr);
    staging->set_binding("Output", sg::buffer<u32>::from_raw(buf).as_readwrite_buffer());

    auto const group = staging->snapshot();
    REQUIRE(group != nullptr);

    // A snapshot is a PERSISTENT group: binding one several epochs later is fine, where a transient group's
    // descriptors would have been recycled and the bind would trip its epoch tripwire.
    for (int i = 0; i < 3; ++i)
        ctx->advance_epoch_and_wait_for_idle();

    dispatch_through(ctx, *pipeline, *group);
    CHECK(holds_doubled(ctx, buf));

    // Still clean across all of that — nothing was set, so the cached snapshot is still the answer.
    CHECK(!staging->is_dirty());
    CHECK(staging->snapshot() == group);
}

// Snapshot and release repeatedly on a tiny persistent descriptor region (4 slots).
// Each snapshot takes 1 descriptor, so 50 rounds far exceed the region: the minted group's range must be
// returned to the free list (epoch-deferred) and reused, exactly as a directly-created persistent group's is.
TEST("sg dx12 - staging snapshots free and reuse their descriptor range")
{
    auto ctx_r = dx12::make_test_context({.descriptor_heap_capacity = 8, .descriptor_transient_fraction = 0.5f});
    REQUIRE(ctx_r.has_value());
    auto ctx = ctx_r.value();

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx->cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf != nullptr);

    auto staging = ctx->persistent.create_staging_binding_group(group_layout);
    REQUIRE(staging != nullptr);
    auto const out = staging->slot_of("Output");

    for (int i = 0; i < 50; ++i)
    {
        // Re-set the same view: the group is dirtied, so every round really does mint a new one.
        staging->set_binding(out, sg::buffer<u32>::from_raw(buf).as_readwrite_buffer());
        auto group = staging->snapshot();
        REQUIRE(group != nullptr); // never exhausts: released ranges are reclaimed
        group.reset();
        ctx->advance_epoch_and_wait_for_idle();
    }
    CHECK(true);
}
