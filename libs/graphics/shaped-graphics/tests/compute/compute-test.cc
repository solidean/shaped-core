#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
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

    auto const& shader = co_await shaders::double_values.compute.main->acquire(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::work>();
    auto const layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = shader, .layout = layout});

    constexpr auto count = 256;
    auto const values = ctx->persistent.create_buffer<float>(
        count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);

    auto const group
        = ctx->transient.create_binding_group(group_layout, shaders::work{.values = values.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->upload.data_to_buffer<float>(values, initial);
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
    auto const values = ctx->persistent.create_buffer<float>(
        count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto const by
        = ctx->persistent.create_buffer<float>(1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);

    auto const factor
        = ctx->transient.create_binding_group(factor_layout, shaders::factor{.by = by.as_readonly_buffer()});
    auto const work
        = ctx->transient.create_binding_group(work_layout, shaders::work{.values = values.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->upload.data_to_buffer<float>(values, initial);
    cmd->upload.pod_to_buffer<float>(by, 3.0f);
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
