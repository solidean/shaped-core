#include "flat.hh"

#include <clean-core/string/format.hh>

bool sgl::check::name_mint::is_taken(cc::string_view name) const
{
    for (auto const& t : taken)
        if (t == name)
            return true;
    return false;
}

bool sgl::check::name_mint::reserve(cc::string_view name)
{
    if (is_taken(name))
        return false;
    taken.push_back(name);
    return true;
}

cc::string sgl::check::name_mint::mint(cc::string_view desired)
{
    auto const base = desired.empty() ? cc::string_view("_") : desired;
    auto name = cc::string(base);
    for (auto i = 1; is_taken(name); ++i)
        name = cc::format("{}_{}", base, i);
    taken.push_back(name);
    return name;
}
