#include <shaped-graphics/routine_registry.hh>

namespace sg
{
void routine_registry::clear()
{
    _entries.lock([](routine_map& entries) { entries.clear(); });
}
} // namespace sg
