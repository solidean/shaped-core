#include "target_spec.hh"

#include <clean-core/string/format.hh>

namespace itrace
{
namespace
{
cc::optional<u8> hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return u8(c - '0');
    if (c >= 'a' && c <= 'f')
        return u8(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return u8(c - 'A' + 10);
    return {};
}
} // namespace

cc::string target_spec::to_string() const
{
    switch (form)
    {
    case kind::symbol:
        return symbol;
    case kind::address:
        return cc::format("{:#x}", address);
    case kind::module_symbol:
        return cc::format("{}!{}", module, symbol);
    case kind::module_offset:
        return cc::format("{}+{:#x}", module, address);
    }
    return {};
}

cc::result<u64> parse_address(cc::string_view text)
{
    if (text.starts_with("0x") || text.starts_with("0X"))
        text.remove_prefix(2);

    if (text.empty())
        return cc::error("expected a hex address");

    u64 value = 0;
    for (char const c : text)
    {
        // Tolerate the `7ff6`00001000` grouping windbg prints.
        if (c == '`' || c == '\'')
            continue;

        auto const digit = hex_digit(c);
        if (!digit.has_value())
            return cc::error(cc::format("'{}' is not a hex digit in address '{}'", c, text));

        if (value > (u64(-1) >> 4))
            return cc::error(cc::format("address '{}' does not fit in 64 bits", text));

        value = (value << 4) | digit.value();
    }

    return value;
}

cc::result<target_spec> parse_target_spec(cc::string_view spec)
{
    if (spec.empty())
        return cc::error("empty target spec");

    target_spec out;

    // mod!sym — the `!` wins over `+`, since a mangled symbol may itself contain `+`.
    if (auto const bang = spec.find('!'); bang >= 0)
    {
        out.form = target_spec::kind::module_symbol;
        out.module = spec.subview({.start = 0, .end = bang});
        out.symbol = spec.subview({.start = bang + 1, .end = spec.size()});

        if (out.module.empty())
            return cc::error(cc::format("missing module before '!' in '{}'", spec));
        if (out.symbol.empty())
            return cc::error(cc::format("missing symbol after '!' in '{}'", spec));

        return out;
    }

    // mod+0xN
    if (auto const plus = spec.find('+'); plus >= 0)
    {
        out.form = target_spec::kind::module_offset;
        out.module = spec.subview({.start = 0, .end = plus});

        if (out.module.empty())
            return cc::error(cc::format("missing module before '+' in '{}'", spec));

        auto offset = parse_address(spec.subview({.start = plus + 1, .end = spec.size()}));
        CC_RETURN_IF_ERROR(offset);
        out.address = offset.value();

        return out;
    }

    // 0xN — an absolute address. A bare hex run stays a symbol: `abc` is a plausible function name.
    if (spec.starts_with("0x") || spec.starts_with("0X"))
    {
        out.form = target_spec::kind::address;

        auto address = parse_address(spec);
        CC_RETURN_IF_ERROR(address);
        out.address = address.value();

        return out;
    }

    out.form = target_spec::kind::symbol;
    out.symbol = spec;
    return out;
}
} // namespace itrace
