#include "webgpu-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// What a WebGPU layout entry has to state up front that dx12 and vulkan take from the bound view,
// what WebGPU refuses outright, and the inline-constant pages under a list that fills more than one.

namespace
{
namespace webgpu = sg::backend::webgpu;
using webgpu::test::make_shader;

constexpr char const* k_fill_storage_texture = R"(
@group(0) @binding(0) var canvas: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(4, 4)
fn main(@builtin(global_invocation_id) id: vec3u) {
    textureStore(canvas, vec2i(id.xy), vec4f(f32(id.x) / 255.0, f32(id.y) / 255.0, 1.0, 1.0));
}
)";

constexpr char const* k_load_multisampled = R"(
@group(0) @binding(0) var source: texture_multisampled_2d<f32>;
@group(0) @binding(1) var<storage, read_write> Output: array<f32>;

@compute @workgroup_size(1)
fn main() {
    Output[0] = textureLoad(source, vec2i(0, 0), 0).r;
}
)";

constexpr char const* k_store_constant = R"(
struct Constants {
    slot: u32,
    value: u32,
}
@group(3) @binding(0) var<uniform> constants: Constants;
@group(0) @binding(0) var<storage, read_write> Output: array<u32>;

@compute @workgroup_size(1)
fn main() {
    Output[constants.slot] = constants.value;
}
)";

[[nodiscard]] sg::binding storage_binding(cc::string_view name, u32 index)
{
    return sg::binding{
        .name = cc::string(name),
        .group_index = 0,
        .index = index,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    };
}
} // namespace

ASYNC_INVOCABLE_TEST("sg webgpu - a write-only storage texture is written by a dispatch and reads back",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int extent = 4;

    // rgba8unorm has no read_write storage in core WebGPU, so a layout entry that did not carry `write` would fail
    // validation here.
    auto const shader = make_shader(sg::shader_stage::compute, k_fill_storage_texture, "main",
                                    {sg::binding{.name = "canvas",
                                                 .group_index = 0,
                                                 .index = 0,
                                                 .type = sg::binding_type::readwrite_texture,
                                                 .texture_dimension = sg::texture_view_dimension::tex_2d,
                                                 .storage_format = sg::pixel_format::rgba8_unorm,
                                                 .storage_access = sg::storage_access::write}},
                                    sg::compute_dimensions{.x = 4, .y = 4});

    auto texture
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = extent,
                                            .height = extent,
                                            .usage = sg::texture_usage::readwrite_texture | sg::texture_usage::copy_src});

    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    sg::named_view const canvas = {.name = "canvas", .view = texture.as_readwrite_view()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&canvas, 1));

    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_groups(1, 1, 1);
    auto const future = cmd->download.bytes_from_texture(texture.raw());
    ctx.submit_command_list(cc::move(cmd));

    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == isize(extent) * extent * 4);
    auto mismatches = 0;
    for (auto y = 0; y < extent; ++y)
        for (auto x = 0; x < extent; ++x)
        {
            auto const* p = pixels.data() + (y * extent + x) * 4;
            if (p[0] != byte(x) || p[1] != byte(y) || p[2] != byte(255) || p[3] != byte(255))
                ++mismatches;
        }
    CHECK(mismatches == 0);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a multisampled float texture is laid out unfilterable whatever the binding says",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;

    // `filterable_float` is what a reflector that cannot see multisampling would report; WebGPU refuses it on a
    // multisampled entry, so the pipeline below only builds if the layout overrides it.
    auto const shader = make_shader(sg::shader_stage::compute, k_load_multisampled, "main",
                                    {
                                        sg::binding{.name = "source",
                                                    .group_index = 0,
                                                    .index = 0,
                                                    .type = sg::binding_type::readonly_texture,
                                                    .texture_dimension = sg::texture_view_dimension::tex_2d_ms,
                                                    .sample_type = sg::texture_sample_type::filterable_float},
                                        storage_binding("Output", 1),
                                    },
                                    sg::compute_dimensions{});

    auto group_layout = ctx.uncached.create_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {group_layout}});
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    CHECK(pipeline != nullptr);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a multisampled array texture is refused at layout creation",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    sg::binding const binding = {.name = "source",
                                 .group_index = 0,
                                 .index = 0,
                                 .type = sg::binding_type::readonly_texture,
                                 .texture_dimension = sg::texture_view_dimension::tex_2d_ms_array};

    auto const layout = ctx.uncached.try_create_binding_group_layout(cc::span<sg::binding const>(&binding, 1));
    REQUIRE(layout.has_error());
    CHECK(layout.error().to_string().contains("multisampled array"));
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - a static sampler naming no sampler binding is refused",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    sg::binding const binding = {.name = "linear", .group_index = 0, .index = 0, .type = sg::binding_type::sampler};
    sg::named_sampler const misnamed = {.name = "lienar", .sampler = sg::sampler{}};

    auto const layout = ctx.uncached.try_create_binding_group_layout(cc::span<sg::binding const>(&binding, 1),
                                                                     cc::span<sg::named_sampler const>(&misnamed, 1));
    REQUIRE(layout.has_error());
    CHECK(layout.error().to_string().contains("'lienar'"));
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - two bound samplers at one register are refused",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;

    // Both would land at group 3 binding 1, which one layout entry cannot be twice.
    auto const layout = ctx.uncached.try_create_pipeline_layout({
        .static_samplers = {sg::bound_sampler{.binding = {.name = "a", .index = 0, .type = sg::binding_type::sampler},
                                              .sampler = sg::sampler{}},
                            sg::bound_sampler{.binding = {.name = "b", .index = 0, .type = sg::binding_type::sampler},
                                              .sampler = sg::sampler{}}},
    });
    REQUIRE(layout.has_error());
    CHECK(layout.error().to_string().contains("both take register 0"));
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - inline constants overflowing one page land on the next",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;

    // Every block is distinct and takes one aligned slot, so this many cannot fit a single page.
    auto const alignment = ctx.uniform_offset_alignment();
    auto const count = int(webgpu::webgpu_config{}.constant_page_bytes / alignment) + 44;

    auto const shader = make_shader(sg::shader_stage::compute, k_store_constant, "main", {storage_binding("Output", 0)},
                                    sg::compute_dimensions{});
    auto group_layout = ctx.uncached.create_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({
        .groups = {group_layout},
        .inline_constants
        = sg::binding{.name = "constants", .index = 0, .type = sg::binding_type::uniform_buffer, .block_size = 8},
    });
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    auto buf = ctx.persistent.create_raw_buffer(isize(count) * 4,
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));

    struct constants
    {
        u32 slot;
        u32 value;
    };

    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    for (auto i = 0; i < count; ++i)
    {
        cmd->compute.set_inline_constants(constants{.slot = u32(i), .value = u32(i) * 7 + 3});
        cmd->compute.dispatch_groups(1, 1, 1);
    }
    auto const future = cmd->download.data_from_buffer<u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    auto mismatches = 0;
    for (auto i = 0; i < count; ++i)
        if (data[i] != u32(i) * 7 + 3)
            ++mismatches;
    CHECK(mismatches == 0);
}

ASYNC_INVOCABLE_TEST("sg webgpu - a read_write rgba8 storage texture needs readwrite_storage_formats",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto bindings = cc::vector<sg::binding>();
    bindings.push_back(sg::binding{.name = "Target", .group_index = 0, .index = 0, .count = 1});
    bindings[0].type = sg::binding_type::readwrite_texture;
    bindings[0].texture_dimension = sg::texture_view_dimension::tex_2d;
    bindings[0].storage_format = sg::pixel_format::rgba8_unorm;
    bindings[0].storage_access = sg::storage_access::read_write;
    sg::apply_stage_visibility(bindings, sg::shader_stage::compute);

    // Refused where the device lacks texture-formats-tier2, built where it has it: never a later validation error.
    auto const layout = ctx.uncached.try_create_binding_group_layout(bindings);
    CHECK(layout.has_value() == ctx.supports(sg::feature::readwrite_storage_formats));

    // An r32 format is read_write in core, whatever the device offers.
    auto r32 = bindings;
    r32[0].storage_format = sg::pixel_format::r32_float;
    CHECK(ctx.uncached.try_create_binding_group_layout(r32).has_value());
    co_return;
}
