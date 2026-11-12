#include "../shaders/shader_fixtures.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_native_spirv.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The route the "sg vulkan native-spirv backend" sweep runs every test body through, which only this file can tell apart.

ASYNC_INVOCABLE_TEST("sg - a vulkan context that prefers SGL's SPIR-V is handed it, with no compiler behind it",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->prefers_native_spirv())
        SKIP("this context takes its compiler's output");

    auto const shader = co_await shaders::array_equality::compare_arrays->acquire(*ctx);
    CHECK(shader.compiler.name == "sgl");
    CHECK(shader.format == sg::shader_format::spirv);
    CHECK(!shader.bytecode.empty());
    // what the host binds comes from SGL either way
    CHECK(shader.bindings.size() == 2);
}

ASYNC_INVOCABLE_TEST("sg - a package that states NATIVE_SPIRV takes SGL's SPIR-V on any vulkan context",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const is_spirv = [&]
    {
        for (auto const format : ctx->accepted_shader_formats())
            return format == sg::shader_format::spirv;
        return false;
    }();
    if (!is_spirv)
        SKIP("this context takes no SPIR-V");

    auto const shader = co_await sg::test::native_spirv::native_spirv::fill->acquire(*ctx);
    CHECK(shader.compiler.name == "sgl");
}
