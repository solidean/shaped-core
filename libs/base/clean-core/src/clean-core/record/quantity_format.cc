#include <clean-core/record/desc.hh>
#include <clean-core/record/quantity_format.hh>
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace cc::rec
{
namespace
{
/// The SI prefixes, from 1e-9 up, with the empty one at `k_si_zero`.
constexpr cc::string_view k_si_prefixes[] = {"n", "u", "m", "", "k", "M", "G", "T", "P", "E"};
constexpr isize k_si_zero = 3;

/// The binary prefixes.
/// There is no sub-unit half, because a fraction of a byte is not a thing.
constexpr cc::string_view k_binary_prefixes[] = {"", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei"};

/// Enough digits to distinguish neighbouring values without turning a log line into a spreadsheet.
/// Three significant figures is what a human reads at a glance, so the decimals shrink as the number grows.
void append_scaled(cc::string& out, f64 value)
{
    auto const magnitude = value < 0 ? -value : value;
    if (magnitude >= 100)
        out += cc::format("{:.0f}", value);
    else if (magnitude >= 10)
        out += cc::format("{:.1f}", value);
    else
        out += cc::format("{:.2f}", value);
}

/// Scales `value` into its prefix, appends the number, and returns the prefix for whatever follows it.
///
/// The prefix belongs to the symbol rather than to the number — "1.00 KiB", never "1.00Ki B" — so it cannot be
/// appended here and has to be handed back.
cc::string_view append_scaled_with_prefix(cc::string& out, f64 value, u32 prefix_base)
{
    if (prefix_base == 0 || value == 0)
    {
        append_scaled(out, value);
        return "";
    }

    auto const base = f64(prefix_base);
    auto magnitude = value < 0 ? -value : value;
    auto scaled = value;

    if (prefix_base == 1024)
    {
        auto index = isize(0);
        while (magnitude >= base && index + 1 < isize(CC_ARRAY_COUNT_OF(k_binary_prefixes)))
        {
            scaled /= base;
            magnitude /= base;
            ++index;
        }
        append_scaled(out, scaled);
        return k_binary_prefixes[index];
    }

    auto index = k_si_zero;
    while (magnitude >= base && index + 1 < isize(CC_ARRAY_COUNT_OF(k_si_prefixes)))
    {
        scaled /= base;
        magnitude /= base;
        ++index;
    }

    // Downward too, which is what makes a duration read as "5.00 ms" rather than "0.01 s".
    while (magnitude < 1 && magnitude > 0 && index > 0)
    {
        scaled *= base;
        magnitude *= base;
        --index;
    }

    append_scaled(out, scaled);
    return k_si_prefixes[index];
}
} // namespace
} // namespace cc::rec

void cc::rec::format_quantity_to(cc::string& out, f64 value, cc::rec::unit const& u)
{
    if (u.format != nullptr)
    {
        u.format(out, value, u);
        return;
    }

    // No symbol and no prefix is a bare quantity — a ratio, a count of nothing in particular.
    // Printing "0.75 ratios" would be worse than printing the number.
    auto const symbol = cc::string_view(u.symbol);
    if (symbol.empty() && u.prefix_base == 0)
    {
        cc::rec::append_scaled(out, value);
        return;
    }

    auto const prefix = cc::rec::append_scaled_with_prefix(out, value, u.prefix_base);

    if (!symbol.empty())
    {
        // The prefix binds to the symbol: "1.00 KiB", "5.00 ms".
        out += ' ';
        out += prefix;
        out += symbol;
        return;
    }

    // With no symbol the prefix binds to the number instead, the way a count is written: "1.50k items".
    out += prefix;

    // The unit's own name stands in for the missing symbol, and singular is for exactly one.
    auto const name = value == 1 ? cc::string_view(u.singular) : cc::string_view(u.plural);
    if (!name.empty())
    {
        out += ' ';
        out += name;
    }
}

cc::string cc::rec::format_quantity(f64 value, cc::rec::unit const& u)
{
    auto out = cc::string();
    cc::rec::format_quantity_to(out, value, u);
    return out;
}
