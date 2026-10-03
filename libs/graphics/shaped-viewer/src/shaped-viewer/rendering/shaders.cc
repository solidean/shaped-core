#include <shaped-viewer/rendering/shaders.hh>
#include <sv_sgl_shaders.hh>
#include <sv_shaders.hh>

namespace sv
{
slib::shader_package const& shader_package()
{
    return sv::shaders::package();
}

slib::shader_package const& sgl_shader_package()
{
    return sv::sgl_shaders::package();
}
} // namespace sv
