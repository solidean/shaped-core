#include <shaped-rendering/render_routine_library.hh>

namespace sr
{
void render_routine_library::add_package(std::shared_ptr<render_routine_package> const& package)
{
    if (package == nullptr)
        return;

    // Dedup by instance identity: a singleton reached twice is walked once.
    for (auto const& p : _all_packages)
        if (p == package)
            return;

    // Mark visited before recursing, so a dependency cycle in a hand-built graph terminates.
    _all_packages.push_back(package);

    // Dependencies first, so their routines precede the packages that depend on them.
    for (auto const& dependency : package->dependencies())
        add_package(dependency);

    for (auto const& routine : package->routines())
        _all_routines.push_back(routine);
}

void render_routine_library::ensure_all_initialized_no_materialize(sg::context& ctx)
{
    for (auto const& routine : _all_routines)
        routine->ensure_initialized_no_materialize(ctx);
}

void render_routine_library::ensure_all_initialized(sg::command_list& cmd)
{
    for (auto const& routine : _all_routines)
        routine->ensure_initialized(cmd);
}
} // namespace sr
