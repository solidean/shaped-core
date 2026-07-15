#include "module_registry.hh"

#include <clean-core/string/char_predicates.hh>

namespace itrace
{
namespace
{
bool equals_ignore_case(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (isize i = 0; i < a.size(); ++i)
        if (cc::to_lower(a[i]) != cc::to_lower(b[i]))
            return false;

    return true;
}

/// "mymodule.exe" -> "mymodule"; a name without a dot is returned unchanged.
cc::string_view drop_extension(cc::string_view name)
{
    auto const dot = name.rfind('.');
    return dot < 0 ? name : name.subview({.start = 0, .end = dot});
}
} // namespace

cc::string_view path_file_name(cc::string_view path)
{
    auto const slash = path.rfind('\\');
    auto const fwd = path.rfind('/');
    auto const cut = cc::max(slash, fwd);
    return cut < 0 ? path : path.subview({.start = cut + 1, .end = path.size()});
}

void module_registry::add(module_info module)
{
    // The loader reuses a base after an unload; keep the registry a map, not a log.
    remove(module.base);
    _modules.push_back(cc::move(module));
}

void module_registry::remove(u64 base)
{
    _modules.remove_all_where([&](module_info const& m) { return m.base == base; });
}

module_info const* module_registry::find_by_address(u64 address) const
{
    for (auto const& m : _modules)
        if (m.contains(address))
            return &m;

    return nullptr;
}

module_info const* module_registry::find_by_name(cc::string_view name) const
{
    for (auto const& m : _modules)
        if (equals_ignore_case(m.name, name))
            return &m;

    // Fall back to a stem match so "mymodule" finds "mymodule.exe".
    for (auto const& m : _modules)
        if (equals_ignore_case(drop_extension(m.name), name))
            return &m;

    return nullptr;
}
} // namespace itrace
