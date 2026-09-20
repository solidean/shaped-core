#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_asset.hh>

// The package this target declares itself (see sc_add_shader_package in shaped-graphics' CMakeLists).
#include <sg_test_sgl_shaders.hh>

// Tier 1's compute execution test written in SGL, and the reason it is worth having beside the HLSL one:
// the HLSL fixture cannot exist without DXC, so on a context that wants WGSL there is nothing to run.
// This source reaches every backend, and the WGSL arm needs no external compiler at all.

using namespace cc::primitive_defines;

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader dispatches and doubles a buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // The asset picks the format by asking the context what it accepts, so this test names no backend.
    // Every backend has an SGL edge, so a failure here is a failure — awaiting it reports whichever one it was.
    auto const& compiled = co_await sg::test::sgl_shaders::double_values.compute.main->acquire(*ctx);

    CHECK(compiled.stage == sg::shader_stage::compute);
    REQUIRE(compiled.workgroup_size.has_value());
    CHECK(compiled.workgroup_size.value().x == 64);

    // One buffer, bound by the name SGL minted for it: `<binding>_<member>`.
    REQUIRE(compiled.bindings.size() == 1);
    CHECK(compiled.bindings[0].name == "work_values");
    CHECK(compiled.bindings[0].type == sg::binding_type::readwrite_structured_buffer);

    auto const group_layout = ctx->cached.acquire_binding_group_layout(compiled.bindings);
    auto const layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        sg::compute_pipeline_description{.shader = compiled, .layout = layout});
    REQUIRE(pipeline != nullptr);

    constexpr auto count = 256; // a multiple of the shader's 64-thread workgroup
    auto const values = ctx->persistent.create_buffer<float>(
        count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(values.raw() != nullptr);

    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);

    // Upload, dispatch and read back in ONE list: sg infers the barriers between them, so splitting it would only
    // hide a defect in that inference.
    auto const group = ctx->transient.create_binding_group(
        group_layout, {{.name = "work_values", .view = values.as_readwrite_buffer()}});

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
