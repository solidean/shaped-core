#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

namespace
{
// An absent value is not a failure, so this returns an optional and no error text is invented for it.
cc::optional<int> first_digit(cc::string_view s)
{
    for (auto const c : s)
        if (c >= '0' && c <= '9')
            return c - '0';
    return cc::nullopt;
}

// A failure a caller must handle is a result, and the error always arrives via cc::error(...).
cc::result<int> parse_percent(cc::string_view s)
{
    if (!s.ends_with('%'))
        return cc::error(cc::format("'{}' does not end in '%'", s));

    auto const digit = first_digit(s);
    if (!digit.has_value())
        return cc::error(cc::format("'{}' holds no digit", s));

    return digit.value() * 10;
}

cc::result<int> parse_two(cc::string_view a, cc::string_view b)
{
    // CC_RETURN_IF_ERROR moves the error out, so the result it reads must be non-const.
    auto first = parse_percent(a);
    CC_RETURN_IF_ERROR(first);
    auto second = parse_percent(b);
    CC_RETURN_IF_ERROR(second);
    return first.value() + second.value();
}
} // namespace

EXAMPLE("clean-core/result")
{
    cc::println("first_digit('abc'):  {}", first_digit("abc").value_or(-1));
    cc::println("first_digit('a4c'):  {}", first_digit("a4c").value_or(-1));

    // map carries the transform into the optional, so the empty case needs no branch of its own.
    cc::println("doubled:             {}", first_digit("a4c").map([](int x) { return x * 2; }).value_or(-1));

    for (auto const input : {"3%", "30", "%"})
    {
        auto r = parse_percent(input);
        if (r.has_value())
            cc::println("parse_percent({:<4}) -> {}", input, r.value());
        else
            cc::println("parse_percent({:<4}) -> error: {}", input, r.error().to_string());
    }

    // Context accumulates on the way out, so the message names the operation as well as the cause.
    auto combined = parse_two("3%", "oops").with_context("while adding two percentages");
    cc::println("combined error: {}", combined.error().to_string());

    cc::println("both good:      {}", parse_two("3%", "4%").value());
}
