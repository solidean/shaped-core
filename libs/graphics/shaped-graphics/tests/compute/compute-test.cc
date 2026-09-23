#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/layout_fit.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_asset.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

ASYNC_INVOCABLE_TEST("sg - a compute shader doubles every element of a buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // The entry point's binding list is `{work}`, so its pipeline needs nothing but the entry point.
    auto const pipeline = co_await shaders::double_values.compute.main.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::work>();

    constexpr auto count = 256;
    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::move(initial), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto const group
        = ctx->transient.create_binding_group(group_layout, shaders::work{.values = values.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(data[i] == float(i) * 2.0f);
}

ASYNC_INVOCABLE_TEST("sg - one group binds at whichever slot the entry point lists it", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // `scaled` lists `{factor, work}`, so `work` is its group 1 where it is `main`'s group 0.
    // The group's type and its layout are the same object either way; only the pipeline layout places them.
    auto const& shader = co_await shaders::double_values.compute.scaled->acquire(*ctx);
    auto const factor_layout = ctx->cached.acquire_binding_group_layout<shaders::factor>();
    auto const work_layout = ctx->cached.acquire_binding_group_layout<shaders::work>();
    auto const layout
        = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {factor_layout, work_layout}});
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = layout});

    constexpr auto count = 64;
    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::move(initial), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const by = ctx->persistent.create_buffer_from_pod(3.0f, sg::buffer_usage::readonly_buffer);

    auto const factor
        = ctx->transient.create_binding_group(factor_layout, shaders::factor{.by = by.as_readonly_buffer()});
    auto const work
        = ctx->transient.create_binding_group(work_layout, shaders::work{.values = values.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *factor);
    cmd->compute.bind_group(1, *work);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(data[i] == float(i) * 3.0f);
}

ASYNC_INVOCABLE_TEST("sg - a group's plain members reach the shader through the constant buffer it owns",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const& shader = co_await shaders::double_values.compute.affine_map->acquire(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::affine>();
    auto const layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = layout});

    constexpr auto count = 64;
    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::move(initial), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    // Both scopes, since each owns the constant buffer for its own lifetime: one frame, or as long as the group.
    auto const transient = ctx->transient.create_binding_group(
        group_layout, shaders::affine{.scale = 3.0f, .bias = 1.0f, .values = values.as_readwrite_buffer()});
    auto const persistent = ctx->persistent.create_binding_group(
        group_layout, shaders::affine{.scale = 0.5f, .bias = -2.0f, .values = values.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *transient);
    cmd->compute.dispatch_threads(count);
    cmd->compute.bind_group(0, *persistent);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(data[i] == (float(i) * 3.0f + 1.0f) * 0.5f - 2.0f);
}

ASYNC_INVOCABLE_TEST("sg - a pipeline whose shader does not fit its layout is refused at creation",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // `main` lists `{work}`, so `work` is its group 0, and this layout has `work` at slot 1.
    auto const& shader = co_await shaders::double_values.compute.main->acquire(*ctx);
    auto const misplaced = ctx->cached.acquire_pipeline_layout<shaders::factor, shaders::work>();
    CHECK(!sg::describe_layout_misfit(shader, *misplaced).empty());

    // A shader is input, and a hot reload can hand in one that no longer fits: the build fails with why, and nothing asserts.
    auto const refused = ctx->uncached.create_compute_pipeline_async({.shader = shader, .layout = misplaced});
    co_await cc::async_settled(refused);
    REQUIRE(refused->has_error());
    CHECK(refused->try_error()->underlying().to_string().contains("does not fit its pipeline layout"));

    // The layout the entry point states fits, which is the check passing rather than being absent.
    CHECK(sg::describe_layout_misfit(shader, *shaders::double_values.compute.main.acquire_layout(*ctx)) == "");
}

ASYNC_INVOCABLE_TEST("sg - a compute shader that calls a helper keeps its workgroup and thread id",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // `at_most` returns early, so the entry point is legalized before it is written; its dispatch shape must survive.
    auto const pipeline = co_await shaders::double_values.compute.clamp_values.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::work>();

    constexpr auto count = 128; // two workgroups of 64: one thread per element only if the size reached the text
    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::move(initial), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group
        = ctx->transient.create_binding_group(group_layout, shaders::work{.values = values.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(data[i] == cc::min(float(i), 10.0f));
}
