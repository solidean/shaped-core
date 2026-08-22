#include "validation.hh"

nx::arg::impl::one_of_spec nx::arg::one_of(cc::span<cc::string_view const> allowed)
{
    auto spec = impl::one_of_spec();
    for (auto const& value : allowed)
        spec.allowed.push_back(cc::string(value));

    return spec;
}
