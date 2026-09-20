#include "../shaders/shader_fixtures.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_asset.hh>

// The package this test target declares itself (see sc_add_shader_package in shaped-graphics'
// CMakeLists). Generated into the build dir and private to this binary.
#include <sg_test_shaders.hh>

// A *consumer* of shaped-graphics declaring its own shaders.
// That is the whole point: sg does not depend on the shader library, only this test binary does.
// Yet a shader declared here builds and resolves against the very context sg handed us.
//
// Only built where a shader compiler exists (SC_HAS_DXC_COMPILER); the shader library's own tests cover
// the mechanism everywhere with a fake compiler.

TEST("sg - a consumer's shader package registers")
{
    (void)sg_test::shader_fixtures(); // the library the generated globals resolve through

    REQUIRE(sg::test::shaders::double_values.compute.main != nullptr);
    CHECK(sg::test::shaders::double_values.compute.main->stage() == sg::shader_stage::compute);
    CHECK(sg::test::shaders::double_values.compute.main->entry_point() == "main");
    CHECK(sg::test::shaders::package().definitions.size()
          == 3); // double_values, pattern_fill (routine-test), ping_pong (transient-benchmark)
}

ASYNC_INVOCABLE_TEST("sg - a consumer's shader compiles for the context it is acquired with",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    (void)sg_test::shader_fixtures();

    // HLSL genuinely reaches only DXC's two formats, so a WGSL context has nothing here — that is the gap the SGL
    // fixture beside this one exists to close, rather than something this test can assert its way around.
    if (!ctx->accepts_shader_format(sg::shader_format::dxil) && !ctx->accepts_shader_format(sg::shader_format::spirv))
        SKIP("no HLSL compiler reaches a format this context accepts");

    // Pass the context, get back what *it* accepts — the negotiation this whole seam exists for.
    // Which compiler ran is never named here; that it ran the right one is what the format assertion says.
    auto const& compiled = co_await sg::test::shaders::double_values.compute.main->acquire(*ctx);

    CHECK(ctx->accepts_shader_format(compiled.format));
    CHECK(compiled.stage == sg::shader_stage::compute);
    CHECK(compiled.bytecode.size() > 0);
    CHECK(compiled.workgroup_size.value().x == 64);
    REQUIRE(compiled.bindings.size() == 1);
    CHECK(compiled.bindings[0].name == "gValues");
}
