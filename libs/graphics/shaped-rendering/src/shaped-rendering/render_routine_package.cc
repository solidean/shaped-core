#include <shaped-rendering/render_routine_package.hh>

namespace sr::impl
{
cc::vector<void const*>& package_construction_stack()
{
    static thread_local cc::vector<void const*> stack;
    return stack;
}
} // namespace sr::impl
