#include <shaped-viewer/rendering/shaders.hh>
#include <sv_shaders.hh>

namespace sv
{
slib::shader_package const& shader_package()
{
    return sv::shaders::package();
}
} // namespace sv
