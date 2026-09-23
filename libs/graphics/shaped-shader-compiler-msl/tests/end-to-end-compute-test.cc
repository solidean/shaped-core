#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-shader-compiler-msl/all.hh>

// The whole chain in one test: MSL text in, a dispatch out, and the bytes read back.
//
// What it proves that the unit tests cannot is that the bindings this library reflects out of the text are the ones
// the backend's argument buffer encodes — a reflector can be self-consistent and still disagree with the backend.
// Until now the metal backend's fixtures were metallibs compiled by hand, with their reflection written out beside
// them, so nothing checked that a reflected layout and the encoded one agree.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
constexpr auto k_copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

/// The same shape double_compute.metal has: one argument buffer, one readwrite structured member at [[id(0)]].
constexpr char const* k_double_kernel = R"(
#include <metal_stdlib>
using namespace metal;

struct frame_bindings
{
    device uint* values [[id(0)]];
};

#pragma sc numthreads 1 1 1
kernel void main0(device frame_bindings& bindings [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    bindings.values[tid] = bindings.values[tid] * 2u;
}
)";

[[nodiscard]] mtl::metal_context_handle make_context()
{
    auto ctx = sg::create_metal_context({});
    if (ctx.has_error())
        return nullptr;
    return std::static_pointer_cast<mtl::metal_context>(ctx.value());
}
} // namespace

TEST("ssc::msl end to end - the reflected bindings build the layout the backend encodes")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader = comp.value().compile({.source = k_double_kernel, .entry_point = "main0"});
    REQUIRE(shader.has_value()).context(shader.has_error() ? shader.error().to_string() : cc::string());

    // One binding, reflected out of the text rather than written by hand beside it.
    REQUIRE(shader.value().bindings.size() == 1);
    CHECK(shader.value().bindings[0].name == "values");
    CHECK(shader.value().bindings[0].type == sg::binding_type::readwrite_structured_buffer);

    auto layout = ctx->create_metal_binding_group_layout(shader.value().bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());

    auto pipeline_layout_desc = sg::pipeline_layout_description{};
    pipeline_layout_desc.groups.push_back(layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(pipeline_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader.value(), .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());
}

ASYNC_TEST("ssc::msl end to end - a kernel compiled from text doubles the values it was given")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader = comp.value().compile({.source = k_double_kernel, .entry_point = "main0"});
    REQUIRE(shader.has_value());

    auto layout = ctx->create_metal_binding_group_layout(shader.value().bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    auto pipeline_layout_desc = sg::pipeline_layout_description{};
    pipeline_layout_desc.groups.push_back(layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(pipeline_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader.value(), .layout = pipeline_layout.value()},
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

    auto const* const values = reinterpret_cast<u32 const*>(bytes.value().data());
    auto mismatches = 0;
    for (auto i = 0; i < k_count; ++i)
        if (values[i] != source[i] * 2u)
            ++mismatches;
    CHECK(mismatches == 0)
        .context(cc::format("{} of {} values wrong; first read back as {}, expected {}", mismatches, k_count, values[0],
                            source[0] * 2u));
}

ASYNC_TEST("ssc::msl end to end - the source arm runs too, compiled by the driver at pipeline build")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    // No toolchain needed for this arm, which is the whole reason it exists.
    auto shader = comp.value().compile({.source = k_double_kernel, .entry_point = "main0"},
                                       {.artifact = ssc::msl::artifact_kind::msl_source});
    REQUIRE(shader.has_value());
    REQUIRE(shader.value().format == sg::shader_format::msl);

    auto layout = ctx->create_metal_binding_group_layout(shader.value().bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    auto pipeline_layout_desc = sg::pipeline_layout_description{};
    pipeline_layout_desc.groups.push_back(layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(pipeline_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader.value(), .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    constexpr auto k_count = 16;
    auto const buffer = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readwrite_buffer);

    auto source = cc::vector<u32>::create_uninitialized(k_count);
    for (auto i = 0; i < k_count; ++i)
        source[i] = u32(i + 3);

    auto const nv = sg::named_view{
        .name = "values",
        .view = buffer->as_raw_readwrite({.offset = 0, .size = buffer->size_in_bytes()}, isize(sizeof(u32)))};
    auto group = ctx->create_metal_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

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

    auto const* const values = reinterpret_cast<u32 const*>(bytes.value().data());
    auto mismatches = 0;
    for (auto i = 0; i < k_count; ++i)
        if (values[i] != source[i] * 2u)
            ++mismatches;
    CHECK(mismatches == 0);
}
