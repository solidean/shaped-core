#include <shaped-graphics/routine_registry.hh>

namespace sg
{
void routine_registry::clear()
{
    _entries.lock([](cc::vector<entry>& entries) { entries.clear(); });
}
} // namespace sg
