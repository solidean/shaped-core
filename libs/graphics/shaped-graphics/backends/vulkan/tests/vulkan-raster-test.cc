#include "vulkan-test-common.hh"

#include <clean-core/thread/async.hh> // cc::async_blocking_get
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded SPIR-V for triangle.hlsl and double_compute.hlsl.
// See those files for the dxc commands.
#include "double_compute.spirv.h"
#include "triangle.ps.spirv.h"
#include "triangle.psbuf.spirv.h"
#include "triangle.vs.spirv.h"

using namespace cc::primitive_defines;

// End-to-end raster path: two rendering scopes into one texture, then a readback checking every pixel.
//
// The first scope clears and draws nothing, so it pins the attachment load/store ops on their own; the second
// preserves and covers half the target with a triangle, so it pins the pipeline, the vertex fetch and the fragment
// output — and leaves the other half showing the clear, so one readback checks both.
// Splitting them is what makes a failure legible: a broken clear and a broken draw would otherwise look alike.
//
// Everything drives the public sg API; only the SPIR-V blobs are vulkan-specific, which is why this is a tier-2 test.

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr int k_extent = 32;

struct vertex
{
    float x, y;
    float r, g, b, a;
};

sg::compiled_shader make_shader(sg::shader_stage stage, cc::span<byte const> bytecode, cc::string_view entry_point)
{
    sg::compiled_shader shader;
    shader.stage = stage;
    shader.format = sg::shader_format::spirv;
    shader.entry_point = entry_point;
    shader.bytecode = cc::make_pinned_data(bytecode);
    return shader;
}

/// The vertex layout the shader's `[[vk::location]]` annotations agree with: position at 0, color at 1.
sg::vertex_input_layout make_vertex_layout()
{
    sg::vertex_input_layout layout;
    layout.slots.push_back({.stride = isize(sizeof(vertex)), .per_instance = false});
    layout.attributes.push_back(
        {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec2f, .offset = 0, .slot = 0});
    layout.attributes.push_back(
        {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec4f, .offset = isize(sizeof(float) * 2), .slot = 0});
    return layout;
}
} // namespace

TEST("sg vulkan - a rendering scope clears, draws and stores")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    // The render target, also readable back.
    auto target
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = k_extent,
                                            .height = k_extent,
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    REQUIRE(target.raw() != nullptr);

    // A triangle over half the target in clip space, so the readback sees both it and the clear.
    // Which half depends on the viewport's Y flip, which is exactly why the checks below count pixels rather than
    // naming coordinates: the test is about the pipeline running, not about the orientation convention.
    vertex const vertices[] = {
        {.x = -1.0f, .y = -1.0f, .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        {.x = 1.0f, .y = -1.0f, .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        {.x = -1.0f, .y = 1.0f, .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
    };
    auto vertex_buffer = ctx.persistent.create_raw_buffer(isize(sizeof(vertices)),
                                                          sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(vertex_buffer != nullptr);

    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{});
    REQUIRE(pipeline_layout != nullptr);

    auto pipeline = cc::async_blocking_get(ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = pipeline_layout,
        .vertex_shader = make_shader(
            sg::shader_stage::vertex,
            cc::span<byte const>(reinterpret_cast<byte const*>(triangle_vs_spirv), isize(sizeof(triangle_vs_spirv))),
            "vs_main"),
        .fragment_shader = make_shader(
            sg::shader_stage::fragment,
            cc::span<byte const>(reinterpret_cast<byte const*>(triangle_ps_spirv), isize(sizeof(triangle_ps_spirv))),
            "ps_main"),
        .vertex_input = make_vertex_layout(),
        .color_targets = {{.format = sg::pixel_format::rgba8_unorm}},
    }));
    REQUIRE(pipeline != nullptr);

    auto cmd = ctx.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<vertex const>(vertices, 3)));

    auto const rtv = target.as_render_target_view();

    // Scope one: clear to blue and draw nothing.
    {
        auto pass = cmd->raster.render_to({.color_targets = {rtv.cleared(tg::vec4f(0, 0, 1, 1))}});
    }

    // Scope two: preserve what is there, then cover every pixel with the triangle.
    {
        auto pass = cmd->raster.render_to({.color_targets = {rtv.preserved()}});
        pass.bind_pipeline(*pipeline);
        pass.bind_vertex_buffers({{.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(vertex))}});
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    ctx.submit_command_list(cc::move(cmd));

    auto down = ctx.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_texture(target.raw());
    ctx.submit_command_list(cc::move(down));

    auto const pixels = ctx.wait_for(future);
    REQUIRE(pixels.has_value());
    REQUIRE(pixels.value().size() == isize(k_extent) * isize(k_extent) * 4);

    // Every pixel is either the triangle's red or the scope's blue, and a half-covering triangle produces
    // meaningfully many of each.
    int reds = 0;
    int blues = 0;
    for (int i = 0; i < k_extent * k_extent; ++i)
    {
        auto const* p = reinterpret_cast<u8 const*>(pixels.value().data()) + isize(i) * 4;
        if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255)
            ++reds;
        else if (p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255)
            ++blues;
    }
    CHECK(reds + blues == k_extent * k_extent); // nothing else was written
    CHECK(reds > k_extent * k_extent / 4);      // the draw happened
    CHECK(blues > k_extent * k_extent / 4);     // and the clear survived where it did not cover
}

// A draw that depends on a compute dispatch recorded in the same list, so the buffer needs a shader_write ->
// shader_read barrier *while the rendering scope is open*.
//
// Vulkan forbids a pipeline barrier inside a dynamic-rendering instance outright, where D3D12 lets a draw flush its
// barriers in place — so this is the test for the backend's answer, which is to close the instance around the barrier
// and reopen it with LOAD ops.
// Without that the validation layer reports VUID-vkCmdPipelineBarrier2-None-09553, and with a broken reopen the clear
// would come back instead of the drawn pixels.
TEST("sg vulkan - a draw depending on a dispatch in the same list")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    auto target
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = k_extent,
                                            .height = k_extent,
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    REQUIRE(target.raw() != nullptr);

    // Written by the compute dispatch, then read by the fragment shader: Values[3] == 3 * 2 == 6.
    auto values = ctx.persistent.create_raw_buffer(
        256 * isize(sizeof(u32)), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::readonly_buffer);
    REQUIRE(values != nullptr);

    // The compute half: the same double_compute shader the compute test uses.
    sg::compiled_shader compute_shader;
    compute_shader.stage = sg::shader_stage::compute;
    compute_shader.format = sg::shader_format::spirv;
    compute_shader.entry_point = "main";
    compute_shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    compute_shader.bytecode = cc::make_pinned_data(
        cc::span<byte const>(reinterpret_cast<byte const*>(double_compute_spirv), isize(sizeof(double_compute_spirv))));
    compute_shader.bindings.push_back(
        {.name = "Output", .group_index = 0, .index = 0, .count = 1, .type = sg::binding_type::readwrite_structured_buffer});

    auto compute_group_layout = ctx.cached.acquire_binding_group_layout(compute_shader.bindings);
    auto compute_pipeline_layout
        = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {compute_group_layout}});
    auto compute_pipeline = cc::async_blocking_get(ctx.cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = compute_shader, .layout = compute_pipeline_layout}));
    REQUIRE(compute_pipeline != nullptr);

    sg::named_view const compute_out
        = {.name = "Output", .view = sg::buffer<u32>::from_raw(values).as_readwrite_buffer()};
    auto compute_group
        = ctx.persistent.create_binding_group(compute_group_layout, cc::span<sg::named_view const>(&compute_out, 1));
    REQUIRE(compute_group != nullptr);

    // The raster half, reading the same buffer.
    auto raster_shader_bindings = cc::vector<sg::binding>{
        {.name = "Values", .group_index = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_structured_buffer}};
    auto raster_group_layout = ctx.cached.acquire_binding_group_layout(raster_shader_bindings);
    auto raster_pipeline_layout
        = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {raster_group_layout}});

    auto vertex_buffer = ctx.persistent.create_raw_buffer(isize(sizeof(vertex)) * 3,
                                                          sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(vertex_buffer != nullptr);
    vertex const vertices[] = {
        {.x = -1.0f, .y = -1.0f, .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        {.x = 3.0f, .y = -1.0f, .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        {.x = -1.0f, .y = 3.0f, .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
    };

    auto pipeline = cc::async_blocking_get(ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = raster_pipeline_layout,
        .vertex_shader = make_shader(
            sg::shader_stage::vertex,
            cc::span<byte const>(reinterpret_cast<byte const*>(triangle_vs_spirv), isize(sizeof(triangle_vs_spirv))),
            "vs_main"),
        .fragment_shader = make_shader(sg::shader_stage::fragment,
                                       cc::span<byte const>(reinterpret_cast<byte const*>(triangle_psbuf_spirv),
                                                            isize(sizeof(triangle_psbuf_spirv))),
                                       "ps_from_buffer"),
        .vertex_input = make_vertex_layout(),
        .color_targets = {{.format = sg::pixel_format::rgba8_unorm}},
    }));
    REQUIRE(pipeline != nullptr);

    sg::named_view const raster_in = {.name = "Values", .view = sg::buffer<u32>::from_raw(values).as_readonly_buffer()};
    auto raster_group
        = ctx.persistent.create_binding_group(raster_group_layout, cc::span<sg::named_view const>(&raster_in, 1));
    REQUIRE(raster_group != nullptr);

    // One list: upload, dispatch, then a rendering scope whose draw reads what the dispatch wrote.
    auto cmd = ctx.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<vertex const>(vertices, 3)));
    cmd->compute.bind_pipeline(*compute_pipeline);
    cmd->compute.bind_group(0, *compute_group);
    cmd->compute.dispatch_threads(256);

    {
        auto pass
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 1, 0, 1))}});
        pass.bind_pipeline(*pipeline);
        pass.bind_group(0, *raster_group);
        pass.bind_vertex_buffers({{.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(vertex))}});
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    ctx.submit_command_list(cc::move(cmd));

    auto down = ctx.create_command_list();
    auto future = down->download.bytes_from_texture(target.raw());
    ctx.submit_command_list(cc::move(down));

    auto const pixels = ctx.wait_for(future);
    REQUIRE(pixels.has_value());

    // The triangle covers every pixel, and each carries Values[3] == 6 in its red channel.
    // A green pixel would mean the clear survived, so the reopen lost the draw.
    bool all_six = true;
    for (int i = 0; i < k_extent * k_extent; ++i)
    {
        auto const* p = reinterpret_cast<u8 const*>(pixels.value().data()) + isize(i) * 4;
        if (p[0] != 6 || p[1] != 0 || p[2] != 0 || p[3] != 255)
            all_six = false;
    }
    CHECK(all_six);
}

// The context, not the last handle, is what destroys a cached pipeline's device objects.
//
// A handle outliving its context used to destroy them against a device that was already gone: a leak at
// vkDestroyDevice, then vkDestroyPipelineLayout on VK_NULL_HANDLE and an abort.
// It reached the suite as a rare failure in whichever test happened to be running, because sg produced this state on
// its own — a pool worker holding the build node past release_cached_pipelines — rather than because anyone wrote
// the code below.
// Declaring the handle before the context is the deterministic form of the same thing.
TEST("sg vulkan - a pipeline handle may outlive its context")
{
    sg::raster_pipeline_handle pipeline; // declared first, so it is destroyed LAST — after the context
    {
        auto handle = vulkan::test::make_context();
        if (handle == nullptr)
            SKIP("no vulkan device");
        auto& ctx = *handle;

        auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{});
        REQUIRE(pipeline_layout != nullptr);

        pipeline = cc::async_blocking_get(ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
            .layout = pipeline_layout,
            .vertex_shader = make_shader(
                sg::shader_stage::vertex,
                cc::span<byte const>(reinterpret_cast<byte const*>(triangle_vs_spirv), isize(sizeof(triangle_vs_spirv))),
                "vs_main"),
            .fragment_shader = make_shader(
                sg::shader_stage::fragment,
                cc::span<byte const>(reinterpret_cast<byte const*>(triangle_ps_spirv), isize(sizeof(triangle_ps_spirv))),
                "ps_main"),
            .vertex_input = make_vertex_layout(),
            .color_targets = {{.format = sg::pixel_format::rgba8_unorm}},
        }));
        REQUIRE(pipeline != nullptr);
    } // the context releases the pipeline's device objects here, and validation must stay quiet
}
