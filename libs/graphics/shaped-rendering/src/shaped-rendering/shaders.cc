#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_sgl_shaders.hh>
#include <sr_shaders.hh>

namespace sr
{
void add_shader_packages(slib::shader_library& lib)
{
    lib.add_package(sr::sgl_shaders::package());
    lib.add_package(sr::shaders::package());
}
} // namespace sr
