#include "value.hh"

#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

using namespace cc::primitive_defines;

// The error strings here are read by someone who mistyped a command line, so they name what was expected.
// Telling apart "not a number" from "too big for this type" is worth the extra shape check: the two have
// completely different fixes, and cc::from_string reports only that it refused.

namespace
{
bool looks_like_integer(cc::string_view token)
{
    auto const digits = token.starts_with('-') ? token.subview(1) : token;
    if (digits.empty())
        return false;

    for (auto const c : digits)
        if (!cc::is_digit(c))
            return false;

    return true;
}

bool equals_ignore_case(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (auto i = isize(0); i < a.size(); ++i)
        if (cc::to_lower(a[i]) != cc::to_lower(b[i]))
            return false;

    return true;
}

bool matches_any(cc::string_view token, cc::span<cc::string_view const> candidates)
{
    for (auto const& candidate : candidates)
        if (equals_ignore_case(token, candidate))
            return true;

    return false;
}

constexpr cc::string_view true_spellings[] = {"true", "yes", "on", "1"};
constexpr cc::string_view false_spellings[] = {"false", "no", "off", "0"};
} // namespace

bool nx::impl::parse_bool_value(cc::string_view token, bool& out, cc::string& error)
{
    if (matches_any(token, true_spellings))
        return out = true, true;

    if (matches_any(token, false_spellings))
        return out = false, true;

    error = "expected true/false, yes/no, on/off or 1/0";
    return false;
}

void nx::impl::bool_values(cc::vector<cc::string>& out)
{
    for (auto const& spelling : true_spellings)
        out.push_back(cc::string(spelling));

    for (auto const& spelling : false_spellings)
        out.push_back(cc::string(spelling));
}

bool nx::impl::parse_signed_value(cc::string_view token, i64& out, cc::string& error, i64 min, i64 max)
{
    auto const value = cc::from_string<i64>(token);
    if (!value.has_value())
    {
        // A well-formed integer that cc::from_string still refused overflowed i64 itself.
        error = looks_like_integer(token) ? cc::format("out of range, expected {} to {}", min, max)
                                          : cc::string("expected an integer");
        return false;
    }

    if (value.value() < min || value.value() > max)
    {
        error = cc::format("out of range, expected {} to {}", min, max);
        return false;
    }

    out = value.value();
    return true;
}

bool nx::impl::parse_unsigned_value(cc::string_view token, u64& out, cc::string& error, u64 max)
{
    // Reported ahead of the parse, because "expected a non-negative integer" is the actual fix and
    // cc::from_string would only say it refused.
    if (token.starts_with('-'))
    {
        error = cc::format("expected a non-negative integer, at most {}", max);
        return false;
    }

    auto const value = cc::from_string<u64>(token);
    if (!value.has_value())
    {
        error = looks_like_integer(token) ? cc::format("out of range, expected 0 to {}", max)
                                          : cc::string("expected a non-negative integer");
        return false;
    }

    if (value.value() > max)
    {
        error = cc::format("out of range, expected 0 to {}", max);
        return false;
    }

    out = value.value();
    return true;
}

bool nx::impl::parse_float_value(cc::string_view token, f32& out, cc::string& error)
{
    auto const value = cc::from_string<f32>(token);
    if (!value.has_value())
    {
        error = "expected a number";
        return false;
    }

    out = value.value();
    return true;
}

bool nx::impl::parse_double_value(cc::string_view token, f64& out, cc::string& error)
{
    auto const value = cc::from_string<f64>(token);
    if (!value.has_value())
    {
        error = "expected a number";
        return false;
    }

    out = value.value();
    return true;
}
