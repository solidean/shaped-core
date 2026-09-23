#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/compiler/metal_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <slib_test_sgl_shaders.hh>

#if SLIB_HAS_METAL

// The metal edge, and the one thing it exists for: an SGL package reaching metal.
// Both arms of the compiler behind it produce something the metal backend accepts, so these assert what the edge
// resolves and what comes out, not which arm ran — that is a property of the host, not of the package.

namespace
{
sg::compiled_shader const& value_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}
} // namespace

TEST("slib metal compiler - the edge is metal source to a metal library")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);
    CHECK(compiler->source_language() == slib::shader_language::metal);
    CHECK(compiler->target_format() == sg::shader_format::metal_lib);
}

TEST("slib metal compiler - preprocess hands the text back, because MSL needs no flattening here")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);

    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };
    auto const text = compiler->preprocess({.source = "kernel void k() {}", .entry_point = "k"}, resolve);
    REQUIRE(text.has_value());
    CHECK(text.value().source == "kernel void k() {}");
}

TEST("slib metal compiler - MSL compiles, and its bindings are reflected out of the text")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);

    auto const shader = compiler->compile({.source = R"(
#include <metal_stdlib>
using namespace metal;
struct frame { device uint* values [[id(0)]]; };
kernel void main0(device frame& f [[buffer(0)]], uint t [[thread_position_in_grid]]) { f.values[t] *= 2u; }
)",
                                           .entry_point = "main0",
                                           .stage = sg::shader_stage::compute});

    auto const& compiled = value_of(shader);
    CHECK(compiled.entry_point == "main0");
    REQUIRE(compiled.bindings.size() == 1);
    CHECK(compiled.bindings[0].name == "values");
    CHECK(compiled.bindings[0].type == sg::binding_type::readwrite_structured_buffer);

    // A metallib where this host has the toolchain, MSL source where it does not — the backend takes either.
    auto const is_metal_format
        = compiled.format == sg::shader_format::metal_lib || compiled.format == sg::shader_format::msl;
    CHECK(is_metal_format);
}

TEST("slib metal compiler - the sgl edge carries a package to metal", exclusive("slib-shader-library"))
{
    // The point of the whole arm: one SGL source, compiled for metal like any other target.
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());

    auto const& vs = value_of(slib_test::sgl_shaders::cube.vertex.main_vs->acquire(sg::shader_format::metal_lib));
    CHECK(vs.stage == sg::shader_stage::vertex);
    CHECK(vs.entry_point == "main_vs");

    auto const& ps = value_of(slib_test::sgl_shaders::cube.pixel.main_ps->acquire(sg::shader_format::metal_lib));
    CHECK(ps.stage == sg::shader_stage::fragment);
}

#endif
