#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/context.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

// The package this test target declares itself (see sc_add_shader_package in shaped-graphics'
// CMakeLists). Generated into the build dir and private to this binary.
#include <sg_test_shaders.hh>

// A *consumer* of shaped-graphics declaring its own shaders. That is the whole point: sg does not depend
// on the shader library — only this test binary does — yet a shader declared here builds and resolves
// against the very context sg handed us.
//
// Only built where a shader compiler exists (SC_HAS_DXC_COMPILER); the shader library's own tests cover
// the mechanism everywhere with a fake compiler.

TEST("sg - a consumer's shader package registers")
{
    slib::shader_library lib;
    lib.add_package(sg::test::shaders::package());

    REQUIRE(sg::test::shaders::double_values.compute.main != nullptr);
    CHECK(sg::test::shaders::double_values.compute.main->stage() == sg::shader_stage::compute);
    CHECK(sg::test::shaders::double_values.compute.main->entry_point() == "main");
    CHECK(sg::test::shaders::package().definitions.size() == 2); // double_values + pattern_fill (routine-test)
}

INVOCABLE_TEST("sg - a consumer's shader compiles for the context it is acquired with", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    slib::shader_library lib;
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sg::test::shaders::package());

    // Pass the context, get back what *it* accepts — the negotiation this whole seam exists for.
    auto const shader = sg::test::shaders::double_values.compute.main->acquire(*ctx);
    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get_singlethreaded(shader); // no async pool here, so drive it

    if (ctx->accepts_shader_format(sg::shader_format::dxil))
    {
        REQUIRE(shader->has_value());
        auto const& compiled = *shader->try_value();
        CHECK(compiled.format == sg::shader_format::dxil);
        CHECK(compiled.stage == sg::shader_stage::compute);
        CHECK(compiled.bytecode.size() > 0);
        CHECK(compiled.workgroup_size.value().x == 64);
        CHECK(compiled.bindings.size() == 1);
        CHECK(compiled.bindings[0].name == "gValues");
    }
    else
    {
        // A vulkan context wants SPIR-V, and only an HLSL->DXIL compiler is registered. It reports that
        // rather than handing back DXIL the context cannot use — the reason acquire takes a context at
        // all instead of assuming a format.
        CHECK(shader->has_error());
    }
}
