#pragma once

#include "metal-test-common.hh"

#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <metal_sgl_shaders.hh>
#include <nexus/async-test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-shader-library/compiler/metal_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

// The tier-2 fixtures, as the shaders the SGL package in tests/shaders compiles them to.
//
// A test reaches the backend through the same calls it always did; only the shader and its bindings come from here.
// So the argument-buffer convention it exercises is the one every real shader's reflection states.

namespace sg::backend::metal::test
{
namespace shaders = ::metal_test::sgl_shaders;

/// `symbol`'s shader, compiled for `ctx` through a shader library of its own, which ends with the call.
/// A test calling it runs `exclusive("slib-shader-library")`, since at most one library may exist at a time.
template <class Symbol>
[[nodiscard]] cc::shared_async<sg::compiled_shader> sgl_shader(metal_context& ctx, Symbol const& symbol)
{
    auto lib = slib::shader_library();
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(shaders::package());

    auto const shader = symbol->acquire(ctx);
    co_await cc::async_settled(shader);
    REQUIRE(shader->has_value()).context(shader->has_error() ? shader->try_error()->underlying().to_string() : cc::string());
    co_return *shader->try_value();
}

/// `symbol`'s ray-tracing pipeline description, its shaders compiled for `ctx` the way `sgl_shader` compiles one.
/// The layout in it is `ctx`'s cached one, so a group the cache builds for the same binding fits it.
template <class Pipeline>
[[nodiscard]] cc::shared_async<sg::raytracing_pipeline_description> sgl_pipeline(metal_context& ctx,
                                                                                 Pipeline const& symbol)
{
    auto lib = slib::shader_library();
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(shaders::package());
    co_return co_await symbol.description(ctx);
}
} // namespace sg::backend::metal::test
