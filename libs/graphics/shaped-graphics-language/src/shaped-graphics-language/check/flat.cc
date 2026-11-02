#include "flat.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

bool sgl::check::name_mint::is_taken(cc::string_view name) const
{
    return index.contains(name) || (parent != nullptr && parent->is_taken(name))
        || (also_taken != nullptr && also_taken->contains(name));
}

bool sgl::check::name_mint::reserve(cc::string_view name)
{
    if ((parent != nullptr && parent->is_taken(name)) || (also_taken != nullptr && also_taken->contains(name)))
        return false;
    if (!index.insert(name))
        return false;
    taken.push_back(name);
    return true;
}

cc::string sgl::check::name_mint::mint(cc::string_view desired)
{
    auto const base = desired.empty() ? cc::string_view("_") : desired;
    auto name = cc::string(base);
    // a name ending in `_` takes its number directly, since a second underscore makes a name C++ reserves
    auto const separator = base.ends_with('_') ? cc::string_view() : cc::string_view("_");
    if (is_taken(name))
    {
        auto& suffix = next_suffix[base];
        suffix = cc::max(suffix, 1);
        name = cc::format("{}{}{}", base, separator, suffix);
        while (is_taken(name))
        {
            ++suffix;
            name = cc::format("{}{}{}", base, separator, suffix);
        }
        ++suffix;
    }
    (void)index.insert(name);
    taken.push_back(name);
    return name;
}
