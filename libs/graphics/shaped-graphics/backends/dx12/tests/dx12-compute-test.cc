#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
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

sg::context_handle make_warp()
{
    auto ctx = sg::create_dx12_context({.use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sg dx12 - compute dispatch writes a structured buffer")
{
    auto handle = make_warp();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    constexpr sg::u32 count = 256; // a multiple of the shader's 64-thread workgroup

    // The compiled compute shader: embedded DXIL bytecode + its (hand-authored) reflection — one
    // read-write structured binding named "Output" at (set 0, index 0) = register(u0, space0).
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    for (unsigned char b : double_compute_dxil)
        shader.bytecode.push_back(cc::byte(b));
    shader.bindings.push_back(sg::binding{
        .name = "Output",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });

    // The output buffer: UAV (readwrite) for the dispatch + copy_src to read it back.
    auto buf
        = c.create_dx12_buffer(cc::isize(count) * cc::isize(sizeof(sg::u32)),
                               sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src, sg::allocation_info{});
    REQUIRE(buf.has_value());

    auto layout = c.create_dx12_binding_layout(shader.bindings);
    REQUIRE(layout.has_value());
    auto pipeline = c.create_dx12_compute_pipeline(shader, layout.value());
    REQUIRE(pipeline.has_value());

    // Bind the output buffer's read-write structured view to "Output".
    sg::named_view const out{.name = "Output", .view = buf.value()->as_readwrite_buffer<sg::u32>()};
    auto group = c.create_dx12_binding_group(layout.value(), cc::span<sg::named_view const>(&out, 1));
    REQUIRE(group.has_value());

    // Record + submit the dispatch.
    auto disp = c.create_dx12_command_list();
    REQUIRE(disp.has_value());
    disp.value()->compute.bind_pipeline(*pipeline.value());
    disp.value()->compute.bind_group(0, *group.value());
    disp.value()->compute.dispatch(count / 64, 1, 1);
    c.submit_dx12_command_list(cc::move(disp.value()));

    // Read the result back in a second list (the buffer decays to COMMON between submits).
    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.data_from_buffer<sg::u32>(buf.value(), 0, count);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const data = future.wait_get_data();
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == cc::isize(count));
    bool ok = true;
    for (sg::u32 i = 0; i < count; ++i)
        if (data.value()[i] != i * 2)
            ok = false;
    CHECK(ok);
}
