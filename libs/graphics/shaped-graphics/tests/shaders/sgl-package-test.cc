#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_asset.hh>

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

ASYNC_INVOCABLE_TEST("sg - an SGL package's generated group is what its compiled shader reflects",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // The asset picks the format by asking the context what it accepts, so this names no backend.
    auto const& compiled = co_await shaders::double_values.compute.main->acquire(*ctx);

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
        CHECK(compiled.bindings[i].name == declared[i].name); // `<binding>_<member>`: `work_values`
        CHECK(compiled.bindings[i].index == declared[i].index);
        CHECK(compiled.bindings[i].count == declared[i].count);
        CHECK(compiled.bindings[i].type == declared[i].type); // `mut buffer[float]` is read-write
    }
}
