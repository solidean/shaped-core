#include "shader_fixtures.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

// The package this target declares itself (see sc_add_shader_package in shaped-graphics' CMakeLists).
#include <sg_test_sgl_shaders.hh>

// What an SGL package gives the host, checked against what the compiled shader says on every backend.
//
// One source reaches dx12, vulkan and webgpu, and the WebGPU arm needs no external compiler at all: that is the reason
// SGL is tier 1's shading language rather than HLSL, whose fixtures cannot exist without DXC.
// `double_values.sgl:*` declares the package, and the build asked the SGL compiler what that file holds:
// its entry point, with the stage the source gives it, and `binding work`, which became `shaders::work`.

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// A group fixes no index of its own: SGL numbers it by its position in each entry point's list.
static_assert(sg::declared_binding_set<shaders::work>);
static_assert(!sg::declared_binding_group<shaders::work>);

// A field is the view its member's access and element take, so a wrong buffer fails to compile rather than to bind.
static_assert(std::is_assignable_v<decltype(shaders::work::values)&, sg::readwrite_buffer_view<float>>);
static_assert(!std::is_assignable_v<decltype(shaders::work::values)&, sg::readonly_buffer_view<float>>); // `mut`
static_assert(!std::is_assignable_v<decltype(shaders::work::values)&, sg::readwrite_buffer_view<int>>);  // `[float]`
static_assert(std::is_assignable_v<decltype(shaders::factor::by)&, sg::readonly_buffer_view<float>>);

TEST("sg - the SGL fixtures reach at least one format on every build")
{
    // The guard `shaders_reach` applies is what keeps the shader-using tier-1 tests off a metal context, and a guard
    // that answered "no" everywhere would skip them on every backend while the suite still reported green.
    // SGL's own compiler needs no external toolchain, so at least one format is reachable on every host there is —
    // which makes an empty answer a broken registration rather than a thin environment.
    auto const formats = sg_test::shader_fixtures().supported_formats(slib::shader_language::sgl);
    CHECK(formats.size() > 0);
}

ASYNC_INVOCABLE_TEST("sg - an SGL package's generated group is what its compiled shader reflects",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // The asset picks the format by asking the context what it accepts, so this names no backend.
    auto const& compiled = co_await shaders::double_values.main->acquire(*ctx);

    CHECK(compiled.stage == sg::shader_stage::compute);
    REQUIRE(compiled.workgroup_size.has_value());
    CHECK(compiled.workgroup_size.value().x == 64); // `@compute(64)`

    // The table the host builds its layout from, and the bindings the shader reflects, name the same resources at the
    // same slots with the same kinds.
    // The group and the space are the entry point's to decide, so they are the one thing the generated table leaves out.
    auto const declared = shaders::work::declared_bindings();
    REQUIRE(compiled.bindings.size() == declared.size());
    for (auto i = isize(0); i < declared.size(); ++i)
    {
        CHECK(compiled.bindings[i].name == declared[i].name); // the SGL path: `work.values`
        // What the target's compiler called it, kept for a diagnostic; `.` is in no target's identifiers.
        CHECK(!compiled.bindings[i].reflected_name.empty());
        CHECK(!compiled.bindings[i].reflected_name.contains('.'));
        CHECK(compiled.bindings[i].index == declared[i].index);
        CHECK(compiled.bindings[i].count == declared[i].count);
        CHECK(compiled.bindings[i].type == declared[i].type); // `mut buffer[float]` is read-write
    }
}

ASYNC_INVOCABLE_TEST("sg - every entry point of the SGL package reflects what the groups it lists declare",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // Generated for the whole package, so a shader added to it is checked here without this test changing.
    auto const mismatch = co_await shaders::check_reflection(*ctx);
    CHECK(mismatch == "");
}

ASYNC_TEST("sg - the WGSL an SGL group becomes reflects every fact its generated table declares")
{
    // A WebGPU layout is built from the generated table, and WebGPU refuses one that disagrees with the module.
    // So every fact beyond the kind has to agree with what the WGSL declares, which DXIL and SPIR-V never state.
    auto const formats = sg_test::shader_fixtures().supported_formats(slib::shader_language::sgl);
    auto has_wgsl = false;
    for (auto const f : formats)
        has_wgsl = has_wgsl || f == sg::shader_format::wgsl;
    if (!has_wgsl)
        SKIP("no WGSL compiler is registered in this build");

    auto const& compiled = co_await shaders::textures.copy_accumulate->acquire(sg::shader_format::wgsl);
    auto const declared = shaders::post::declared_bindings();
    REQUIRE(compiled.bindings.size() == declared.size());
    for (auto const& want : declared)
    {
        sg::binding const* got = nullptr;
        for (auto const& b : compiled.bindings)
            if (b.name == want.name)
                got = &b;
        REQUIRE(got != nullptr);
        CHECK(got->type == want.type);
        CHECK(got->texture_dimension == want.texture_dimension);
        CHECK(got->sample_type == want.sample_type);
        CHECK(got->image_format == want.image_format);
        CHECK(got->storage_access == want.storage_access);
        CHECK(got->sampler_type == want.sampler_type);
    }
}
