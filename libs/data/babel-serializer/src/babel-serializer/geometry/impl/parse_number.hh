#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

#include <charconv> // std::from_chars

namespace babel::impl
{
/// One whole token as an f32, the way strtof reads it: a value below f32's smallest subnormal rounds to a signed zero.
/// std::from_chars reports that underflow as out of range, which would reject a well-formed file (`3.1e-48` in an STL normal).
/// An overflow, or anything that is not a number to the token's end, is still refused.
[[nodiscard]] inline bool parse_f32(cc::string_view tok, cc::primitive_defines::f32& out)
{
    auto const* const first = tok.data();
    auto const* const last = tok.data() + tok.size();
    auto const [ptr, ec] = std::from_chars(first, last, out);
    if (ec == std::errc())
        return ptr == last;
    if (ec != std::errc::result_out_of_range || ptr != last)
        return false;

    // Out of f32's range: a double holds every exponent a mesh file writes, and rounds to f32 the way strtof does.
    auto wide = 0.0;
    auto const [wide_ptr, wide_ec] = std::from_chars(first, last, wide);
    if (wide_ec == std::errc() && wide_ptr == last)
    {
        if (wide > 3.4028234663852886e38 || wide < -3.4028234663852886e38)
            return false; // overflow
        out = cc::primitive_defines::f32(wide);
        return true;
    }

    // Out of a double's range as well: an underflow has a negative exponent, and is a zero of the mantissa's sign.
    for (auto const* p = first; p != last; ++p)
        if ((*p == 'e' || *p == 'E') && p + 1 != last && p[1] == '-')
        {
            out = *first == '-' ? -0.0f : 0.0f;
            return true;
        }
    return false;
}
} // namespace babel::impl
