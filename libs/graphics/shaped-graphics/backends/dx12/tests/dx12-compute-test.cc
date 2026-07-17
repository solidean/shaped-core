#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2). See that file for the dxc command.
#include "double_compute.dxil.h"

// End-to-end compute bind path: build a compiled_shader from the embedded blob + hand-authored reflection,
// create the layout / pipeline / binding_group, dispatch, then read the buffer back and check every element.
// Runs on WARP so it exercises the real GPU paths on headless CI. Everything drives the public sg API — the
// only dx12-specific piece is the DXIL blob (shader bytecode is inherently per-backend); the descriptor-ring
// tests below additionally take a dx12 context with a tiny, hand-sized descriptor heap, which is a dx12 knob.

namespace
{
namespace dx12 = sg::backend::dx12;

// The compiled compute shader for double_compute.hlsl: embedded DXIL + its hand-authored reflection —
// one read-write structured binding "Output" at (set 0, index 0). Shared by the tests below.
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

TEST("sg dx12 - compute dispatch writes a structured buffer")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    constexpr int count = 256; // a multiple of the shader's 64-thread workgroup

    sg::compiled_shader const shader = make_double_shader();

    // The output buffer: UAV (readwrite) for the dispatch + copy_src to read it back.
    auto buf = ctx->persistent.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                                                 sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);

    auto group_layout = ctx->uncached.create_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout
        = ctx->uncached.create_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = ctx->uncached.create_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    // Bind the output buffer's read-write structured view to "Output".
    sg::named_view const out{.name = "Output", .view = sg::buffer<sg::u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx->persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
    REQUIRE(group != nullptr);

    // Record + submit the dispatch. dispatch_threads auto-divides the thread count by the shader's
    // 64-thread workgroup, so this is the count/64 = 4 groups a raw dispatch_groups would spell out.
    auto disp = ctx->create_command_list();
    REQUIRE(disp != nullptr);
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(count); // y, z default to 1
    ctx->submit_command_list(cc::move(disp));

    // Read the result back in a second list (the buffer decays to COMMON between submits).
    auto down = ctx->create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.data_from_buffer<sg::u32>(buf, 0, count);
    ctx->submit_command_list(cc::move(down));

    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == cc::isize(count));
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != cc::u32(i) * 2)
            ok = false;
    CHECK(ok);
}

// Drive the same dispatch with a TRANSIENT output buffer + TRANSIENT binding group each epoch, on a
// deliberately tiny transient descriptor region (32 slots) so 40 iterations wrap the ring several
// times. Proves the transient descriptor ring + transient buffer heap reclaim end-to-end on the GPU:
// no exhaustion, and every epoch's result is correct (Output[i] == i*2). The tiny hand-sized descriptor
// heap is a dx12 knob, so this takes a dx12 context directly; the work itself is all public sg API.
TEST("sg dx12 - transient binding groups + buffers recycle across epochs")
{
    auto ctx_r = sg::create_dx12_context(
        {.use_warp = true, .descriptor_heap_capacity = 64, .descriptor_transient_fraction = 0.5f});
    REQUIRE(ctx_r.has_value());
    auto ctx = ctx_r.value();

    constexpr int count = 256;
    sg::compiled_shader const shader = make_double_shader();

    // Layouts + pipeline are cached schemas — always persistent, built once.
    auto group_layout = ctx->uncached.create_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout
        = ctx->uncached.create_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = ctx->uncached.create_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    for (int e = 0; e < 40; ++e)
    {
        auto buf = ctx->transient.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                                                    sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        REQUIRE(buf != nullptr);

        sg::named_view const out{.name = "Output", .view = sg::buffer<sg::u32>::from_raw(buf).as_readwrite_buffer()};
        auto group = ctx->transient.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
        REQUIRE(group != nullptr);

        auto disp = ctx->create_command_list();
        REQUIRE(disp != nullptr);
        disp->compute.bind_pipeline(*pipeline);
        disp->compute.bind_group(0, *group);
        disp->compute.dispatch_threads(count);
        ctx->submit_command_list(cc::move(disp));

        auto down = ctx->create_command_list();
        REQUIRE(down != nullptr);
        auto future = down->download.data_from_buffer<sg::u32>(buf, 0, count);
        ctx->submit_command_list(cc::move(down));

        auto const data = ctx->wait_for(future);
        REQUIRE(data.has_value());
        bool ok = true;
        for (int i = 0; i < count; ++i)
            if (data.value()[i] != cc::u32(i) * 2)
                ok = false;
        CHECK(ok);

        ctx->advance_epoch(2); // keep at most 2 epochs in flight → the rings reclaim older slots/windows
    }
}

// Create + release many PERSISTENT binding groups on a tiny persistent descriptor region (4 slots).
// Each group takes 1 descriptor; 50 iterations far exceed the region, so the group's range must be
// returned to the free list (epoch-deferred) and reused — a bump allocator would exhaust after 4. The
// hand-sized descriptor heap is a dx12 knob; the group create/release cycle is all public sg API.
TEST("sg dx12 - persistent binding groups free and reuse their descriptor range")
{
    auto ctx_r = sg::create_dx12_context(
        {.use_warp = true, .descriptor_heap_capacity = 8, .descriptor_transient_fraction = 0.5f}); // 4 persistent slots
    REQUIRE(ctx_r.has_value());
    auto ctx = ctx_r.value();

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx->uncached.create_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf != nullptr);

    for (int i = 0; i < 50; ++i)
    {
        sg::named_view const out{.name = "Output", .view = sg::buffer<sg::u32>::from_raw(buf).as_readwrite_buffer()};
        auto group = ctx->persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
        REQUIRE(group != nullptr);              // never exhausts: released ranges are reclaimed
        group.reset();                          // drop -> schedules the range's deferred free
        ctx->advance_epoch_and_wait_for_idle(); // retire -> the finalizer returns it to the free list
    }
    CHECK(true);
}
