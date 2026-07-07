#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2). See that file for the dxc command.
#include "double_compute.dxil.h"

// End-to-end dx12 compute bind path: build a compiled_shader from the embedded blob + hand-authored
// reflection, create the layout / pipeline / binding_group, dispatch, then read the buffer back and
// check every element. Runs on WARP so it exercises the real GPU paths on headless CI.

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
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    constexpr int count = 256; // a multiple of the shader's 64-thread workgroup

    sg::compiled_shader const shader = make_double_shader();

    // The output buffer: UAV (readwrite) for the dispatch + copy_src to read it back.
    auto buf
        = c.create_dx12_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                               sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src, sg::allocation_info{});
    REQUIRE(buf.has_value());

    auto layout = c.create_dx12_binding_layout(shader.bindings, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());
    auto pipeline = c.create_dx12_compute_pipeline(shader, layout.value(), sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value());

    // Bind the output buffer's read-write structured view to "Output".
    sg::named_view const out{.name = "Output", .view = buf.value()->as_readwrite_buffer<sg::u32>()};
    auto group = c.create_dx12_binding_group(layout.value(), cc::span<sg::named_view const>(&out, 1),
                                             sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

    // Record + submit the dispatch. dispatch_threads auto-divides the thread count by the shader's
    // 64-thread workgroup, so this is the count/64 = 4 groups a raw dispatch_groups would spell out.
    auto disp = c.create_dx12_command_list();
    REQUIRE(disp.has_value());
    disp.value()->compute.bind_pipeline(*pipeline.value());
    disp.value()->compute.bind_group(0, *group.value());
    disp.value()->compute.dispatch_threads(count); // y, z default to 1
    c.submit_dx12_command_list(cc::move(disp.value()));

    // Read the result back in a second list (the buffer decays to COMMON between submits).
    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.data_from_buffer<sg::u32>(buf.value(), 0, count);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const data = c.wait_for(future);
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
// no exhaustion, and every epoch's result is correct (Output[i] == i*2).
TEST("sg dx12 - transient binding groups + buffers recycle across epochs")
{
    auto ctx_r = sg::create_dx12_context(
        {.use_warp = true, .descriptor_heap_capacity = 64, .descriptor_transient_fraction = 0.5f});
    REQUIRE(ctx_r.has_value());
    auto handle = ctx_r.value();
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    constexpr int count = 256;
    sg::compiled_shader const shader = make_double_shader();

    // Layout + pipeline are cached schemas — always persistent, built once.
    auto layout = c.create_dx12_binding_layout(shader.bindings, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());
    auto pipeline = c.create_dx12_compute_pipeline(shader, layout.value(), sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value());

    for (int e = 0; e < 40; ++e)
    {
        auto buf = c.transient.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                                                 sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        REQUIRE(buf.has_value());

        sg::named_view const out{.name = "Output", .view = buf.value()->as_readwrite_buffer<sg::u32>()};
        auto group = c.transient.create_binding_group(layout.value(), cc::span<sg::named_view const>(&out, 1));
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

        c.advance_epoch(2); // keep at most 2 epochs in flight → the rings reclaim older slots/windows
    }
}

// Create + release many PERSISTENT binding groups on a tiny persistent descriptor region (4 slots).
// Each group takes 1 descriptor; 50 iterations far exceed the region, so the group's range must be
// returned to the free list (epoch-deferred) and reused — a bump allocator would exhaust after 4.
TEST("sg dx12 - persistent binding groups free and reuse their descriptor range")
{
    auto ctx_r = sg::create_dx12_context(
        {.use_warp = true, .descriptor_heap_capacity = 8, .descriptor_transient_fraction = 0.5f}); // 4 persistent slots
    REQUIRE(ctx_r.has_value());
    auto handle = ctx_r.value();
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    sg::compiled_shader const shader = make_double_shader();
    auto layout = c.create_dx12_binding_layout(shader.bindings, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf.has_value());

    for (int i = 0; i < 50; ++i)
    {
        sg::named_view const out{.name = "Output", .view = buf.value()->as_readwrite_buffer<sg::u32>()};
        auto group = c.create_dx12_binding_group(layout.value(), cc::span<sg::named_view const>(&out, 1),
                                                 sg::lifetime_scope::persistent);
        REQUIRE(group.has_value());          // never exhausts: released ranges are reclaimed
        group.value().reset();               // drop -> schedules the range's deferred free
        c.advance_epoch_and_wait_for_idle(); // retire -> the finalizer returns it to the free list
    }
    CHECK(true);
}
