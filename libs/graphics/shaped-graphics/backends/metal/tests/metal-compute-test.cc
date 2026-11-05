#include "metal-test-common.hh"
#include "sgl-fixture.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

// Compute, end to end: a kernel of the SGL package, its reflected bindings, a dispatch, and the bytes read back.
//
// The tier-1 suite has no compute execution test at all — it cannot, because bytecode is per-backend by construction —
// so this tier is the specification for the dispatch path, the way dx12's and vulkan's are for theirs.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
namespace kernels = mtl::test::shaders::kernels;

/// The fixtures' buffer array, `summed.inputs`, as an array declaration takes it.
/// Named by the package: these layouts come from create_metal_binding_group_layout, which the by-name lookup does not see.
constexpr auto k_inputs_array = kernels::summed::arrays::inputs;

constexpr auto k_copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;
} // namespace

ASYNC_TEST("sg metal - a compute pipeline builds from a metal library", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const shader = co_await mtl::test::sgl_shader(*ctx, kernels::double_main);
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

ASYNC_TEST("sg metal - a compute pipeline refuses a shader of the wrong format", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto shader = co_await mtl::test::sgl_shader(*ctx, kernels::double_main);
    shader.format = sg::shader_format::spirv;

    auto const pipeline
        = ctx->create_metal_compute_pipeline({.shader = shader, .layout = nullptr}, sg::lifetime_scope::persistent);
    CHECK(pipeline.has_error());
}

ASYNC_TEST("sg metal - a dispatch doubles the values it was given", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const shader = co_await mtl::test::sgl_shader(*ctx, kernels::double_main);
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
        .name = "doubled.values",
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

// -- inline constants and array bindings -------------------------------------------------------------
//
// Both are paths Metal has no native form of: there are no root constants, and which elements of an array a kernel
// indexes is something no backend can infer.

ASYNC_TEST("sg metal - compute inline constants reach the kernel", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const shader = co_await mtl::test::sgl_shader(*ctx, kernels::scale_main);

    auto group_layout = ctx->create_metal_binding_group_layout(kernels::scaled::declared_bindings(), {},
                                                               sg::lifetime_scope::persistent);
    REQUIRE(group_layout.has_value());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    layout_desc.inline_constants = kernels::scale::inline_binding();
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value())
        .context(pipeline_layout.has_error() ? pipeline_layout.error().to_string() : cc::string());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    constexpr auto k_count = 16;
    auto const buffer = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readwrite_buffer);

    auto source = cc::vector<u32>::create_uninitialized(k_count);
    for (auto i = 0; i < k_count; ++i)
        source[i] = u32(i + 1);

    auto const nv = sg::named_view{
        .name = "scaled.values",
        .view = buffer->as_raw_readwrite({.offset = 0, .size = buffer->size_in_bytes()}, isize(sizeof(u32)))};
    auto group = ctx->create_metal_binding_group(group_layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<u32 const>(source)));
    cmd->compute.bind_pipeline(*pipeline.value());
    cmd->compute.bind_group(0, *group.value());

    // Set twice, dispatched once: the block is placed by the dispatch, so what runs is the value that was current
    // then — not the first one set.
    cmd->compute.set_inline_constants(kernels::scale{.factor = 2}.to_block());
    cmd->compute.set_inline_constants(kernels::scale{.factor = 3}.to_block());
    cmd->compute.dispatch_groups(k_count, 1, 1);

    auto future = cmd->download.bytes_from_buffer(buffer, 0, buffer->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto const* const values = reinterpret_cast<u32 const*>(bytes.value().data());
    auto mismatches = 0;
    for (auto i = 0; i < k_count; ++i)
        if (values[i] != source[i] * 3u)
            ++mismatches;
    CHECK(mismatches == 0)
        .context(cc::format("{} of {} values wrong; first read back as {}, expected {}", mismatches, k_count, values[0],
                            source[0] * 3u));
}

ASYNC_TEST("sg metal - an array binding's elements are declared one by one", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The array takes four consecutive slots from its own index, so the scalar output sits at index 4 — the spacing
    // rule a layout must follow and nothing checks.
    auto const shader = co_await mtl::test::sgl_shader(*ctx, kernels::array_sum_main);

    auto group_layout = ctx->create_metal_binding_group_layout(shader.bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(group_layout.has_value());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    constexpr auto k_count = 8;
    constexpr auto k_inputs = 4;

    auto inputs = cc::vector<sg::raw_buffer_handle>();
    auto element_views = cc::vector<sg::raw_view>();
    for (auto e = 0; e < k_inputs; ++e)
    {
        auto const buffer = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                              k_copy_both | sg::buffer_usage::readonly_buffer);
        element_views.push_back(
            buffer->as_raw_readonly({.offset = 0, .size = buffer->size_in_bytes()}, isize(sizeof(u32))));
        inputs.push_back(buffer);
    }

    auto const output = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readwrite_buffer);

    auto views = cc::vector<sg::named_view>();
    views.push_back({.name = "summed.inputs", .view = sg::bound_view(cc::move(element_views))});
    views.push_back(
        {.name = "summed.output",
         .view = output->as_raw_readwrite({.offset = 0, .size = output->size_in_bytes()}, isize(sizeof(u32)))});

    auto group = ctx->create_metal_binding_group(group_layout.value(), views, {}, sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    for (auto e = 0; e < k_inputs; ++e)
    {
        auto element = cc::vector<u32>::create_uninitialized(k_count);
        for (auto i = 0; i < k_count; ++i)
            element[i] = u32((e + 1) * (i + 1));
        cmd->upload.bytes_to_buffer(inputs[e], cc::as_bytes(cc::span<u32 const>(element)));
    }

    cmd->compute.bind_pipeline(*pipeline.value());
    cmd->compute.bind_group(0, *group.value());

    // **The declare is what tracks these four buffers at all.** The upload above wrote every one of them, so without
    // it the dispatch reads them with no barrier ordering it after the copies.
    auto declares = cc::vector<sg::array_buffer_access>();
    for (auto e = 0; e < k_inputs; ++e)
        declares.push_back({.index = e, .access = sg::access_flag::shader_read});
    auto const declared = cmd->compute.declare_array_buffer_access(k_inputs_array, declares);
    cmd->compute.dispatch_groups(k_count, 1, 1);

    auto future = cmd->download.bytes_from_buffer(output, 0, output->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    // Element e holds (e + 1) * (i + 1), so the sum over four elements is 10 * (i + 1).
    auto const* const values = reinterpret_cast<u32 const*>(bytes.value().data());
    auto mismatches = 0;
    for (auto i = 0; i < k_count; ++i)
        if (values[i] != 10u * u32(i + 1))
            ++mismatches;
    CHECK(mismatches == 0)
        .context(cc::format("{} of {} sums wrong; first read back as {}, expected {}", mismatches, k_count, values[0],
                            10u * 1u));
}

ASYNC_TEST("sg metal - a transient group freed after its bind is retained, and its successor is a group of its own",
           exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // **A bound group's address is what an array declaration's application is keyed on**, so the list retains every
    // group it binds: a transient one the caller drops cannot be freed, and a new one allocated at its address, while
    // the list records.
    // The successor is then a group of its own, resolved and checked again at its first op.
    auto const shader = co_await mtl::test::sgl_shader(*ctx, kernels::array_sum_main);

    auto group_layout = ctx->create_metal_binding_group_layout(shader.bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(group_layout.has_value());
    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());
    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    constexpr auto k_count = 8;
    auto const input = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                         k_copy_both | sg::buffer_usage::readonly_buffer);
    auto const output = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readwrite_buffer);

    // Element 3 holds the input in the first group and nothing in the second.
    auto const group_of = [&](bool last_filled)
    {
        auto elements = cc::vector<sg::raw_view>();
        for (auto e = 0; e < 4; ++e)
        {
            if (e == 3 && !last_filled)
                elements.push_back(sg::vacant_view{});
            else
                elements.push_back(
                    input->as_raw_readonly({.offset = 0, .size = input->size_in_bytes()}, isize(sizeof(u32))));
        }
        auto views = cc::vector<sg::named_view>();
        views.push_back({.name = "summed.inputs", .view = sg::bound_view(cc::move(elements))});
        views.push_back(
            {.name = "summed.output",
             .view = output->as_raw_readwrite({.offset = 0, .size = output->size_in_bytes()}, isize(sizeof(u32)))});
        auto group = ctx->create_metal_binding_group(group_layout.value(), views, {}, sg::lifetime_scope::transient);
        REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());
        return sg::binding_group_handle(group.value());
    };

    auto declares = cc::vector<sg::array_buffer_access>();
    for (auto e = 0; e < 4; ++e)
        declares.push_back({.index = e, .access = sg::access_flag::shader_read});

    auto cmd = ctx->create_command_list();
    {
        auto const declared = cmd->compute.declare_array_buffer_access(k_inputs_array, declares);
        cmd->compute.bind_pipeline(*pipeline.value());

        auto first = group_of(true);
        auto const first_seen = std::weak_ptr<sg::binding_group const>(first);
        cmd->compute.bind_group(0, *first);
        cmd->compute.dispatch_groups(k_count, 1, 1);
        first = nullptr;
        CHECK(first_seen.lock() != nullptr); // the list holds it

        auto const second = group_of(false);
        cmd->compute.bind_group(0, *second);
        CHECK_ASSERTS(cmd->compute.dispatch_groups(k_count, 1, 1));
    }
    ctx->drop_command_list(cc::move(cmd));
}
