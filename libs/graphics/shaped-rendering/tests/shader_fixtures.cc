#include "shader_fixtures.hh"

#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>

namespace
{
slib::shader_library& create_library()
{
    static slib::shader_library lib;

    // Every edge, so a test names no backend; which one runs follows from what the context accepts.
    // A DXC that fails to create here is a broken install rather than a build without one — this file is compiled only
    // where CMake found one — so it is left to fail the test that needed it.
    slib::add_available_compilers(lib);
    sr::add_shader_packages(lib);
    return lib;
}
} // namespace

slib::shader_library& sr_test::shader_fixtures()
{
    // The inner static is only ever reached through this one, so the guard here is what makes the fill run once.
    static slib::shader_library& lib = create_library();
    return lib;
}
