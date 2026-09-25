#include "webgpu-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Compute on WebGPU end to end: pipelines built both ways, inline constants emulated in group 3, and the group-3
// sampler a pipeline layout's bound_sampler becomes.

namespace
{
namespace webgpu = sg::backend::webgpu;
using webgpu::test::make_shader;

constexpr char const* k_double = R"(
@group(0) @binding(0) var<storage, read_write> Output: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x < arrayLength(&Output)) {
        Output[id.x] = id.x * 2u;
    }
}
)";

constexpr char const* k_affine = R"(
struct Constants {
    scale: u32,
    add: u32,
}
@group(3) @binding(0) var<uniform> constants: Constants;
@group(0) @binding(0) var<storage, read_write> Output: array<u32>;

@compute @workgroup_size(16)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x < arrayLength(&Output)) {
        Output[id.x] = id.x * constants.scale + constants.add;
    }
}
)";

constexpr char const* k_increment = R"(
@group(0) @binding(0) var<storage, read_write> Values: array<u32>;

@compute @workgroup_size(16)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x < arrayLength(&Values)) {
        Values[id.x] = Values[id.x] + 1u;
    }
}
)";

constexpr char const* k_sample = R"(
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> Output: array<u32>;
@group(3) @binding(1) var point: sampler;

@compute @workgroup_size(1)
fn main() {
    // The centre of texel (1, 0) of a 2x2 texture, read through the pipeline layout's nearest sampler.
    let c = textureSampleLevel(source, point, vec2f(0.75, 0.25), 0.0);
    Output[0] = u32(round(c.r * 255.0));
    Output[1] = u32(round(c.g * 255.0));
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

[[nodiscard]] bool all_equal(cc::span<u32 const> data, auto const& expected)
{
    for (isize i = 0; i < data.size(); ++i)
        if (data[i] != expected(u32(i)))
            return false;
    return true;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg webgpu - a cached compute pipeline builds asynchronously and dispatches",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int count = 256;

    auto const shader = make_shader(sg::shader_stage::compute, k_double, "main", {storage_binding("Output", 0)},
                                    sg::compute_dimensions{.x = 64});

    auto buf = ctx.persistent.create_raw_buffer(isize(count) * 4,
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));

    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer<u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    CHECK(all_equal(data, [](u32 i) { return i * 2; }));
}

ASYNC_INVOCABLE_TEST("sg webgpu - a group and a pipeline dropped between bind and dispatch still dispatch",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int count = 64;

    auto const shader = make_shader(sg::shader_stage::compute, k_double, "main", {storage_binding("Output", 0)},
                                    sg::compute_dimensions{.x = 64});
    auto buf = ctx.persistent.create_raw_buffer(isize(count) * 4,
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto group_layout = ctx.uncached.create_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {group_layout}});
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));

    // The list replays what was bound once the dispatch opens its pass, so it has to hold both, not the caller.
    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    group = nullptr;
    pipeline = nullptr;
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer<u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    CHECK(all_equal(data, [](u32 i) { return i * 2; }));
}

ASYNC_INVOCABLE_TEST("sg webgpu - inline constants reach group 3 with their own offset per dispatch",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int count = 64;

    auto const shader = make_shader(sg::shader_stage::compute, k_affine, "main", {storage_binding("Output", 0)},
                                    sg::compute_dimensions{.x = 16});
    auto group_layout = ctx.uncached.create_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({
        .groups = {group_layout},
        .inline_constants
        = sg::binding{.name = "constants", .index = 0, .type = sg::binding_type::uniform_buffer, .block_size = 8},
    });
    // The synchronous build, which the uncached tier uses.
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    auto const make_output = [&]
    {
        auto buf = ctx.persistent.create_raw_buffer(isize(count) * 4,
                                                    sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
        return std::pair(buf, ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1)));
    };
    auto const [a, group_a] = make_output();
    auto const [b, group_b] = make_output();
    auto const [c, group_c] = make_output();

    struct constants
    {
        u32 scale;
        u32 add;
    };

    // Three dispatches in one list: two blocks differing, and a third rebuilt by partial updates to equal the first.
    // Only the last block written is deduplicated, so the third is written again rather than reusing the first's placement.
    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group_a);
    cmd->compute.set_inline_constants(constants{.scale = 3, .add = 1});
    cmd->compute.dispatch_threads(count);
    cmd->compute.bind_group(0, *group_b);
    cmd->compute.set_inline_constants(constants{.scale = 5, .add = 7});
    cmd->compute.dispatch_threads(count);
    cmd->compute.bind_group(0, *group_c);
    cmd->compute.set_inline_constants(u32(3), isize(0)); // a partial update back to the first block
    cmd->compute.set_inline_constants(u32(1), isize(4));
    cmd->compute.dispatch_threads(count);
    auto const fa = cmd->download.data_from_buffer<u32>(a, 0, count);
    auto const fb = cmd->download.data_from_buffer<u32>(b, 0, count);
    auto const fc = cmd->download.data_from_buffer<u32>(c, 0, count);
    ctx.submit_command_list(cc::move(cmd));

    CHECK(all_equal(co_await fa.data(), [](u32 i) { return i * 3 + 1; }));
    CHECK(all_equal(co_await fb.data(), [](u32 i) { return i * 5 + 7; }));
    CHECK(all_equal(co_await fc.data(), [](u32 i) { return i * 3 + 1; }));
}

ASYNC_INVOCABLE_TEST("sg webgpu - uploads between dispatches land in recording order",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    constexpr int count = 64;

    auto const shader = make_shader(sg::shader_stage::compute, k_increment, "main", {storage_binding("Values", 0)},
                                    sg::compute_dimensions{.x = 16});
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    auto buf = ctx.persistent.create_raw_buffer(
        isize(count) * 4, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    sg::named_view const values = {.name = "Values", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&values, 1));

    auto initial = cc::vector<u32>();
    for (int i = 0; i < count; ++i)
        initial.push_back(u32(i) * 10);
    auto const middle = cc::vector<u32>::create_filled(16, u32(1000));

    // Upload, dispatch, overwrite the middle, dispatch again: the ring's spans are written at record time, so only
    // the copies' positions in the list decide what each dispatch sees.
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_buffer(buf, cc::as_bytes(cc::span<u32 const>(initial)));
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    cmd->upload.bytes_to_buffer(buf, cc::as_bytes(cc::span<u32 const>(middle)), 16 * 4);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer<u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    CHECK(all_equal(data, [](u32 i) { return i >= 16 && i < 32 ? u32(1001) : i * 10 + 2; }));
}

ASYNC_INVOCABLE_TEST("sg webgpu - a bound sampler is group 3 binding index + 1",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;

    auto const shader = make_shader(sg::shader_stage::compute, k_sample, "main",
                                    {
                                        sg::binding{.name = "source",
                                                    .group_index = 0,
                                                    .index = 0,
                                                    .type = sg::binding_type::readonly_texture,
                                                    .texture_dimension = sg::texture_view_dimension::tex_2d,
                                                    .sample_type = sg::texture_sample_type::filterable_float},
                                        storage_binding("Output", 1),
                                    },
                                    sg::compute_dimensions{});

    auto texture = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                     .width = 2,
                                                     .height = 2,
                                                     .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto out = ctx.persistent.create_raw_buffer(8, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    auto const nearest = sg::sampler{.min_filter = sg::sampler_filter::nearest,
                                     .mag_filter = sg::sampler_filter::nearest,
                                     .mip_filter = sg::sampler_filter::nearest,
                                     .address_u = sg::sampler_address_mode::clamp_edge,
                                     .address_v = sg::sampler_address_mode::clamp_edge};
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({
        .groups = {group_layout},
        .static_samplers = {sg::bound_sampler{.binding = {.name = "point", .index = 0, .type = sg::binding_type::sampler},
                                              .sampler = nearest}},
    });
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    sg::named_view const views[] = {
        {.name = "source", .view = texture.as_texture_view()},
        {.name = "Output", .view = sg::buffer<u32>::from_raw(out).as_readwrite_buffer()},
    };
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(views));

    u8 const texels[] = {10, 20, 0, 255, 30, 40, 0, 255, 50, 60, 0, 255, 70, 80, 0, 255};
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(texture.raw(), cc::as_bytes(cc::span<u8 const>(texels)));
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_groups(1, 1, 1);
    auto const future = cmd->download.data_from_buffer<u32>(out, 0, 2);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 2);
    CHECK(data[0] == 30);
    CHECK(data[1] == 40);
}
