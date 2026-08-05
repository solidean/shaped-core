#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/routine/reload_generation.hh>

using namespace cc::primitive_defines;

namespace
{
// Process-global, not per-anything: only the aggregate "something reloaded" fact is tracked.
// Degrades to a plain value where threads are off, since cc::atomic keeps its API.
cc::atomic<u64> g_reload_generation = {0};
} // namespace

namespace sg
{
u64 reload_generation()
{
    return g_reload_generation.load();
}

void signal_reload()
{
    g_reload_generation.fetch_add(1);
}
} // namespace sg
