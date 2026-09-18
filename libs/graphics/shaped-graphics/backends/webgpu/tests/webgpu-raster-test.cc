#include "webgpu-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Raster, textures and presentation on WebGPU: a draw into a target, a pass reopened around a copy, 1D textures,
// row-padded texture copies, and the headless swapchain.

namespace
{
namespace webgpu = sg::backend::webgpu;
using webgpu::test::make_shader;

constexpr int k_extent = 16;

constexpr char const* k_triangle = R"(
struct VsOut {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
}

@vertex
fn vs_main(@location(0) position: vec2f, @location(1) color: vec4f) -> VsOut {
    var out: VsOut;
    out.position = vec4f(position, 0.0, 1.0);
    out.color = color;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    return in.color;
}
)";

struct vertex
{
    f32 x, y;
    f32 r, g, b, a;
};

[[nodiscard]] sg::vertex_input_layout vertex_layout()
{
    auto layout = sg::vertex_input_layout();
    layout.slots.push_back({.stride = isize(sizeof(vertex))});
    layout.attributes.push_back({.semantic = "POSITION", .format = sg::vertex_attribute_format::vec2f, .offset = 0});
    layout.attributes.push_back({.semantic = "COLOR", .format = sg::vertex_attribute_format::vec4f, .offset = 8});
    return layout;
}

struct pixel_counts
{
    int reds = 0;
    int blues = 0;
    int others = 0;
};

[[nodiscard]] pixel_counts count_pixels(cc::span<byte const> pixels)
{
    auto counts = pixel_counts();
    for (isize i = 0; i + 3 < pixels.size(); i += 4)
    {
        auto const* p = reinterpret_cast<u8 const*>(pixels.data()) + i;
        if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255)
            ++counts.reds;
        else if (p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255)
            ++counts.blues;
        else
            ++counts.others;
    }
    return counts;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg webgpu - a rendering scope clears, draws, and survives a copy in its middle",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;

    auto target
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = k_extent,
                                            .height = k_extent,
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    vertex const vertices[] = {
        {.x = -1.0f, .y = -1.0f, .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        {.x = 1.0f, .y = -1.0f, .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        {.x = -1.0f, .y = 1.0f, .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
    };
    auto vertex_buffer = ctx.persistent.create_raw_buffer(isize(sizeof(vertices)),
                                                          sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);

    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{});
    auto pipeline = co_await ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = pipeline_layout,
        .vertex_shader = make_shader(sg::shader_stage::vertex, k_triangle, "vs_main"),
        .fragment_shader = make_shader(sg::shader_stage::fragment, k_triangle, "fs_main"),
        .vertex_input = vertex_layout(),
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = {{.format = sg::pixel_format::rgba8_unorm}},
    });
    REQUIRE(pipeline != nullptr);

    auto const rtv = target.as_render_target_view();
    auto cmd = ctx.create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {rtv.cleared(tg::vec4f(0, 0, 1, 1))}});

        // The copy closes the pass; the draw below reopens it with a load, so the clear survives.
        cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<vertex const>(vertices, 3)));

        pass.bind_pipeline(*pipeline);
        pass.bind_vertex_buffers({{.buffer = vertex_buffer,
                                   .size_in_bytes = isize(sizeof(vertices)),
                                   .stride_in_bytes = isize(sizeof(vertex))}});
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto const future = cmd->download.bytes_from_texture(target.raw());
    ctx.submit_command_list(cc::move(cmd));

    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == isize(k_extent) * k_extent * 4);
    auto const counts = count_pixels(pixels);
    CHECK(counts.others == 0);
    CHECK(counts.reds > k_extent * k_extent / 4);
    CHECK(counts.blues > k_extent * k_extent / 4);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a 1D texture is a 2D texture of height 1",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int width = 300; // rows of 1200 bytes, which a buffer copy pads to 1280

    auto texture = ctx.persistent.create_raw_texture(sg::texture_description{
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d1,
        .width = width,
        .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });
    REQUIRE(texture != nullptr);

    auto texels = cc::vector<u8>();
    for (int i = 0; i < width * 4; ++i)
        texels.push_back(u8(i * 7));

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(texture, cc::as_bytes(cc::span<u8 const>(texels)));
    auto const future = cmd->download.bytes_from_texture(texture);
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == texels.size());
    CHECK(cc::memcmp(bytes.data(), texels.data(), size_t(texels.size())) == 0);
}

ASYNC_INVOCABLE_TEST("sg webgpu - texture copies strip the row padding in both directions",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int width = 37;
    constexpr int height = 5;

    auto texture = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                     .width = width,
                                                     .height = height,
                                                     .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst});
    auto texels = cc::vector<u8>();
    for (int i = 0; i < width * height * 4; ++i)
        texels.push_back(u8(i * 13 + 1));

    // Inline both ways, then the async tier's read of the same texels.
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(texture.raw(), cc::as_bytes(cc::span<u8 const>(texels)));
    auto const inline_future = cmd->download.bytes_from_texture(texture.raw());
    ctx.submit_command_list(cc::move(cmd));

    auto const inline_bytes = co_await inline_future.bytes();
    REQUIRE(inline_bytes.size() == texels.size());
    CHECK(cc::memcmp(inline_bytes.data(), texels.data(), size_t(texels.size())) == 0);

    auto const async_future = ctx.download.bytes_from_texture(texture.raw());
    auto const async_bytes = co_await async_future.bytes();
    REQUIRE(async_bytes.size() == texels.size());
    CHECK(cc::memcmp(async_bytes.data(), texels.data(), size_t(texels.size())) == 0);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a texture upload after an odd-sized buffer upload lands at its block alignment",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;

    // Four bytes leave the upload ring's head at a word boundary that is not a 16-byte one.
    // A buffer-to-texture copy must start at a multiple of the texel block's size, so the texture's rows must not follow at that head.
    auto buffer = ctx.persistent.create_raw_buffer(4, sg::buffer_usage::copy_dst);
    auto texture = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                     .width = 3,
                                                     .height = 2,
                                                     .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst});
    auto texels = cc::vector<float>();
    for (int i = 0; i < 3 * 2 * 4; ++i)
        texels.push_back(float(i) + 0.5f);
    u32 const word = 7;

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<u32 const>(&word, 1)), 0);
    cmd->upload.bytes_to_texture(texture.raw(), cc::as_bytes(cc::span<float const>(texels)));
    auto const future = cmd->download.bytes_from_texture(texture.raw());
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == texels.size() * isize(sizeof(float)));
    CHECK(cc::memcmp(bytes.data(), texels.data(), size_t(bytes.size())) == 0);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a BC mip smaller than a block round-trips",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    if (!wgpuDeviceHasFeature(ctx.device(), WGPUFeatureName_TextureCompressionBC))
    {
        SKIP("this device has no texture-compression-bc");
        co_return;
    }

    // Mips 8, 4, 2, 1: the last two are one partial 4x4 block each, whose copy extent WebGPU wants as a whole block.
    auto texture = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bc1_rgba_unorm,
                                                     .width = 8,
                                                     .height = 8,
                                                     .mip_levels = 4,
                                                     .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst});
    auto block = cc::vector<byte>();
    for (int i = 0; i < 8; ++i) // one BC1 block is 8 bytes
        block.push_back(byte(0x11 * (i + 1)));

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(texture.raw(), block, sg::subresource_index{.mip_level = 2});
    auto const future = cmd->download.bytes_from_texture(texture.raw(), sg::subresource_index{.mip_level = 2});
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == block.size());
    CHECK(cc::memcmp(bytes.data(), block.data(), size_t(block.size())) == 0);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a headless swapchain rotates and its presented frame stays readable",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    REQUIRE(ctx.supports(sg::feature::headless_present));

    auto chain = ctx.create_swapchain(
        {.headless_extent = tg::vec2i(8, 8), .buffer_count = 2, .format = sg::pixel_format::rgba8_unorm});
    REQUIRE(chain != nullptr);

    auto const first = chain->acquire_backbuffer();
    auto cmd = ctx.create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {first.cleared(tg::vec4f(0, 0, 1, 1))}});
    }
    auto const future = cmd->download.bytes_from_texture(first.texture());
    ctx.submit_command_list_and_present(*chain, cc::move(cmd));

    auto const second = chain->acquire_backbuffer();
    CHECK(second.texture() != first.texture());

    auto const pixels = co_await future.bytes();
    auto const counts = count_pixels(pixels);
    CHECK(counts.blues == 64);
}

ASYNC_INVOCABLE_TEST("sg webgpu - two timestamps around work are ordered", (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    if (!ctx.supports(sg::feature::timestamp_query))
        SKIP("this adapter offers no timestamp-query");

    auto cmd = ctx.create_command_list();
    auto const before = cmd->query.record_gpu_timestamp();
    auto const after = cmd->query.record_gpu_timestamp();
    ctx.submit_command_list(cc::move(cmd));

    REQUIRE(before.is_valid());
    auto const t0 = co_await before.ticks();
    auto const t1 = co_await after.ticks();
    CHECK(t1 >= t0);
}
