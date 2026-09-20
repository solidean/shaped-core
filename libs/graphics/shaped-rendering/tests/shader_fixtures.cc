#include "shader_fixtures.hh"

#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

namespace
{
slib::shader_library& create_library()
{
    static slib::shader_library lib;

    // Both formats, so a test names no backend; which one runs follows from what the context accepts.
    // A compiler that fails to create here is a broken DXC install rather than a build without one — this file is
    // compiled only where CMake found one — so it is left to fail the test that needed it.
    if (auto dxil = slib::create_dxc_compiler(); dxil.has_value())
        lib.add_compiler(cc::move(dxil.value()));
    if (auto spirv = slib::create_dxc_spirv_compiler(); spirv.has_value())
        lib.add_compiler(cc::move(spirv.value()));

    lib.add_package(sr::shader_package());
    return lib;
}
} // namespace

slib::shader_library& sr_test::shader_fixtures()
{
    // The inner static is only ever reached through this one, so the guard here is what makes the fill run once.
    static slib::shader_library& lib = create_library();
    return lib;
}
