#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

#if SLIB_HAS_DXC
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#endif

// The package this target declares itself (see sc_add_shader_package in shaped-graphics' CMakeLists).
#include <sg_test_sgl_shaders.hh>

// Tier 1's compute execution test written in SGL, and the reason it is worth having beside the HLSL one:
// the HLSL fixture cannot exist without DXC, so on a context that wants WGSL there is nothing to run.
// This source reaches every backend, and the WGSL arm needs no external compiler at all.

using namespace cc::primitive_defines;

namespace
{
/// Every SGL edge this build can make: WGSL is native to slib, and DXC serves dxil and spirv where it exists.
void add_sgl_compilers(slib::shader_library& lib)
{
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
#if SLIB_HAS_DXC
#ifdef CC_OS_WINDOWS
    if (auto dxil = slib::create_dxc_compiler(); dxil.has_value())
        lib.add_compiler(slib::create_sgl_compiler(cc::move(dxil.value())));
#endif
    if (auto spirv = slib::create_dxc_spirv_compiler(); spirv.has_value())
        lib.add_compiler(slib::create_sgl_compiler(cc::move(spirv.value())));
#endif
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader dispatches and doubles a buffer",
                     (sg::context_handle const& ctx),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx != nullptr);

    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_package(sg::test::sgl_shaders::package());

    // The asset picks the format by asking the context what it accepts, so this test names no backend.
    auto const shader = sg::test::sgl_shaders::double_values.compute.main->acquire(*ctx);
    REQUIRE(shader != nullptr);
    co_await cc::async_settled(shader);
    auto const* const compiled = shader->try_value();
    if (compiled == nullptr)
        SKIP("no SGL compiler reaches a shader format this context accepts");

    CHECK(compiled->stage == sg::shader_stage::compute);
    REQUIRE(compiled->workgroup_size.has_value());
    CHECK(compiled->workgroup_size.value().x == 64);

    // One buffer, bound by the name SGL minted for it: `<binding>_<member>`.
    REQUIRE(compiled->bindings.size() == 1);
    CHECK(compiled->bindings[0].name == "work_values");
    CHECK(compiled->bindings[0].type == sg::binding_type::readwrite_structured_buffer);

    constexpr auto count = 256; // a multiple of the shader's 64-thread workgroup
    auto const values = ctx->persistent.create_buffer<float>(
        count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(values.raw() != nullptr);

    auto initial = cc::vector<float>::create_defaulted(count);
    for (auto i = 0; i < count; ++i)
        initial[i] = float(i);

    auto up = ctx->create_command_list();
    up->upload.data_to_buffer<float>(values, cc::span<float const>(initial), 0);
    ctx->submit_command_list(cc::move(up));

    auto const group_layout = ctx->cached.acquire_binding_group_layout(compiled->bindings);
    auto const layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});
    auto pipeline
        = ctx->cached.acquire_compute_pipeline(sg::compute_pipeline_description{.shader = *compiled, .layout = layout});
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    REQUIRE(built != nullptr);
    REQUIRE(*built != nullptr);

    auto disp = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        group_layout, {{.name = "work_values", .view = values.as_readwrite_buffer()}});
    disp->compute.bind_pipeline(**built);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(count);
    ctx->submit_command_list(cc::move(disp));

    // A second list, since the buffer decays to COMMON between submits.
    auto down = ctx->create_command_list();
    auto const future = down->download.data_from_buffer<float>(values.raw(), 0, count);
    ctx->submit_command_list(cc::move(down));

    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    auto ok = true;
    for (auto i = 0; i < count; ++i)
        ok = ok && data[i] == float(i) * 2.0f;
    CHECK(ok);
}
