#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_sgl_shaders.hh>

#if SR_HAS_HLSL_SHADERS
#include <sr_shaders.hh>
#endif

namespace sr
{
void add_shader_packages(slib::shader_library& lib)
{
    lib.add_package(sr::sgl_shaders::package());
#if SR_HAS_HLSL_SHADERS
    lib.add_package(sr::shaders::package());
#endif
}
} // namespace sr
