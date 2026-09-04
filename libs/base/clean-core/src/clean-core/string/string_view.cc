#include "string_view.hh"

#include <clean-core/string/string.hh>

namespace
{
/// The two case conversions differ only in which predicate and which shift, so they share the walk.
template <bool ToLower>
[[nodiscard]] cc::string converted(cc::string_view s)
{
    auto out = cc::string();
    out.reserve_back(s.size());
    for (auto const c : s)
        out.push_back(ToLower ? cc::to_lower(c) : cc::to_upper(c));
    return out;
}
} // namespace

cc::string cc::string_view::to_lower_ascii() const
{
    return converted<true>(*this);
}

cc::string cc::string_view::to_upper_ascii() const
{
    return converted<false>(*this);
}
