#include "half_float.hh"

#include <clean-core/string/from_string.hh>
#include <clean-core/string/to_string.hh>

namespace tg::impl
{
f64 half_float_shortest_value(half_float v)
{
    CC_ASSERT(v.is_finite(), "only a finite f16 has digits");

    // Five significant digits always tell two halves apart, so the loop never runs out.
    auto const exact = v.to_f64();
    char buffer[cc::to_chars_float_max] = {};
    for (auto precision = 0; precision < 5; ++precision)
    {
        auto const length = cc::to_chars(cc::span<char>(buffer), exact, cc::float_notation::scientific, precision);
        auto candidate = 0.0;
        if (cc::from_string(cc::string_view(buffer, length), candidate) && half_float(candidate).bits() == v.bits())
            return candidate;
    }
    return exact;
}
} // namespace tg::impl
