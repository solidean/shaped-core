#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <slib_test_sgl_shaders.hh>

#include <type_traits>

using namespace cc::primitive_defines;

// twin_a.sgl and twin_b.sgl declare the same names, and twin_b.sgl a binding named after twin_a.sgl.
// Each file's generated types, entry points and pipelines stand in a namespace of its own, so none of them clash.

namespace
{
namespace shaders = slib_test::sgl_shaders;

sg::compiled_shader const& value_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}
} // namespace

// `binding source` of each file is a type of its own, of the shape its file declares.
static_assert(!std::is_same_v<shaders::twin_a::source, shaders::twin_b::source>);
static_assert(std::is_same_v<decltype(shaders::twin_a::source::values), sg::readwrite_buffer_view<float>>);
static_assert(std::is_same_v<decltype(shaders::twin_b::source::values), sg::readwrite_buffer_view<i32>>);

// A binding named like another file stands in its own file's namespace, beside that file's.
static_assert(std::is_same_v<decltype(shaders::twin_b::twin_a::values), sg::readonly_buffer_view<i32>>);

// So does each file's entry point of one name.
static_assert(!std::is_same_v<decltype(shaders::twin_a::fill), decltype(shaders::twin_b::fill)>);

TEST("slib sgl file namespaces - two files' inline blocks of one name are each their own file's bytes")
{
    auto const a = shaders::twin_a::constants{.scale = 2.0f}.to_block();
    auto const b = shaders::twin_b::constants{.offset = 3}.to_block();

    auto scale = 0.0f;
    auto offset = i32(0);
    cc::memcpy(&scale, a.data(), sizeof(scale));
    cc::memcpy(&offset, b.data(), sizeof(offset));
    CHECK(scale == 2.0f);
    CHECK(offset == 3);
}

TEST("slib sgl file namespaces - each file's binding of one name is declared from its own file",
     exclusive("slib-shader-library"))
{
    REQUIRE(shaders::twin_a::source::declared_bindings().size() == 1);
    REQUIRE(shaders::twin_b::source::declared_bindings().size() == 1);
    CHECK(shaders::twin_a::source::declared_bindings()[0].access == sg::access_mode::read_write);
    CHECK(shaders::twin_b::twin_a::declared_bindings()[0].access == sg::access_mode::read);

    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(shaders::package());

    // Each `fill` is its own file's: twin_b's lists one binding more.
    CHECK(value_of(shaders::twin_a::fill->acquire(sg::shader_format::wgsl)).bindings.size() == 2);
    CHECK(value_of(shaders::twin_b::fill->acquire(sg::shader_format::wgsl)).bindings.size() == 3);
}

TEST("slib sgl file namespaces - a compiled pixel shader names its target set as its file's generated type",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(shaders::package());

    auto const& a = value_of(shaders::twin_a::paint->acquire(sg::shader_format::wgsl));
    auto const& b = value_of(shaders::twin_b::paint->acquire(sg::shader_format::wgsl));
    CHECK(a.target_set == shaders::twin_a::target::name);
    CHECK(b.target_set == shaders::twin_b::target::name);
    CHECK(a.target_set != b.target_set);
}
