#include "double_compute.metallib.h"
#include "metal-test-common.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

// Compute, end to end: a checked-in metallib, a hand-written binding shape, a dispatch, and the bytes read back.
//
// The tier-1 suite has no compute execution test at all — it cannot, because bytecode is per-backend by construction —
// so this tier is the specification for the dispatch path, the way dx12's and vulkan's are for theirs.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
constexpr auto k_copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

/// The reflection, written by hand next to the shader rather than produced by a reflector.
/// That is what makes the test state the binding shape it means — see double_compute.metal.
[[nodiscard]] sg::compiled_shader double_compute_shader()
{
    auto shader = sg::compiled_shader{};
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::metal_lib;
    shader.entry_point = "main0";
    auto blob = cc::pinned_data<byte>::create_uninitialized(isize(sizeof(mtl::test::double_compute_metallib)));
    cc::memcpy(blob.data(), mtl::test::double_compute_metallib, sizeof(mtl::test::double_compute_metallib));
    shader.bytecode = cc::pinned_data<byte const>(cc::move(blob));
    shader.workgroup_size = sg::compute_dimensions{.x = 1, .y = 1, .z = 1};
    shader.bindings.push_back({
        .name = "values",
        .space = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });
    return shader;
}
} // namespace

TEST("sg metal - a compute pipeline builds from a metal library")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const shader = double_compute_shader();
    auto layout = ctx->create_metal_binding_group_layout(shader.bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());

    auto pipeline_layout_desc = sg::pipeline_layout_description{};
    pipeline_layout_desc.groups.push_back(layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(pipeline_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());
    CHECK(pipeline.value()->state() != nullptr);

    // Metal reports no serialized blob on this path, so the honest answer is empty rather than a fabricated one.
    CHECK(pipeline.value()->cached_pipeline_data().empty());
    CHECK(!pipeline.value()->used_cached_pipeline());
}

TEST("sg metal - a compute pipeline refuses a shader of the wrong format")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto shader = double_compute_shader();
    shader.format = sg::shader_format::spirv;

    auto const pipeline
        = ctx->create_metal_compute_pipeline({.shader = shader, .layout = nullptr}, sg::lifetime_scope::persistent);
    CHECK(pipeline.has_error());
}

ASYNC_TEST("sg metal - a dispatch doubles the values it was given")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const shader = double_compute_shader();
    auto layout = ctx->create_metal_binding_group_layout(shader.bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    auto pipeline_layout_desc = sg::pipeline_layout_description{};
    pipeline_layout_desc.groups.push_back(layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(pipeline_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value());

    constexpr auto k_count = 64;
    auto const buffer = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readwrite_buffer);

    auto source = cc::vector<u32>::create_uninitialized(k_count);
    for (auto i = 0; i < k_count; ++i)
        source[i] = u32(i + 1);

    auto const nv = sg::named_view{
        .name = "values",
        .view = buffer->as_raw_readwrite({.offset = 0, .size = buffer->size_in_bytes()}, isize(sizeof(u32)))};
    auto group = ctx->create_metal_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<u32 const>(source)));
    cmd->compute.bind_pipeline(*pipeline.value());
    cmd->compute.bind_group(0, *group.value());
    cmd->compute.dispatch_groups(k_count, 1, 1);
    auto future = cmd->download.bytes_from_buffer(buffer, 0, buffer->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == buffer->size_in_bytes());

    auto const* const values = reinterpret_cast<u32 const*>(bytes.value().data());
    auto mismatches = 0;
    for (auto i = 0; i < k_count; ++i)
        if (values[i] != source[i] * 2u)
            ++mismatches;
    CHECK(mismatches == 0)
        .context(cc::format("{} of {} values wrong; first read back as {}, expected {}", mismatches, k_count, values[0],
                            source[0] * 2u));
}
