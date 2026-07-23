#include <shaped-rendering/shaders.hh>
#include <sr_shaders.hh>

namespace sr
{
slib::shader_package const& shader_package()
{
    return sr::shaders::package();
}
} // namespace sr
