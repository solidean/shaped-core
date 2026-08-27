#include "report.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/record/desc.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace
{
namespace console = cc::console;

// floor(log10(x)) for x > 0, without a math library.
//
// Repeated scaling rather than a logarithm: clean-core has no scalar math at this tier, and a display routine needs
// the exponent rather than a precise logarithm anyway.
isize decimal_exponent(f64 x)
{
    CC_ASSERT(x > 0, "decimal_exponent needs a positive value");

    auto e = isize(0);
    while (x >= 10)
    {
        x /= 10;
        ++e;
    }
    while (x < 1)
    {
        x *= 10;
        --e;
    }
    return e;
}

struct scaled_value
{
    f64 value = 0;
    f64 factor = 1; // what the original was divided by
    cc::string_view prefix;
};

// SI prefixes down to nano, which is where a per-iteration timing lives.
constexpr cc::string_view si_prefixes[] = {"p", "n", "u", "m", "", "k", "M", "G", "T"};
constexpr auto si_zero_index = isize(4); // the index of the empty prefix

constexpr cc::string_view binary_prefixes[] = {"", "Ki", "Mi", "Gi", "Ti"};

// Picks the prefix that puts the value in [1, base).
//
// "u" rather than the micro sign: this goes to a terminal, a log file and a markdown table, and one of those three
// will mangle a non-ASCII byte.
scaled_value apply_prefix(f64 value, u32 base)
{
    if (value == 0 || base == 0)
        return {.value = value};

    auto const magnitude = value < 0 ? -value : value;

    if (base == 1024)
    {
        auto factor = f64(1);
        auto index = isize(0);
        while (magnitude / factor >= 1024 && index + 1 < isize(CC_ARRAY_COUNT_OF(binary_prefixes)))
        {
            factor *= 1024;
            ++index;
        }
        return {.value = value / factor, .factor = factor, .prefix = binary_prefixes[index]};
    }

    auto factor = f64(1);
    auto index = si_zero_index;
    while (magnitude / factor >= 1000 && index + 1 < isize(CC_ARRAY_COUNT_OF(si_prefixes)))
    {
        factor *= 1000;
        ++index;
    }
    while (magnitude / factor < 1 && index > 0)
    {
        factor /= 1000;
        --index;
    }
    return {.value = value / factor, .factor = factor, .prefix = si_prefixes[index]};
}

// cc::format validates its format string at compile time, so a runtime precision has to be a switch over literals.
// Without an explicit `f` the precision is ignored outright and a double renders shortest-round-trip, which is exactly
// the seven-digits-it-does-not-have problem this file exists to prevent.
cc::string fixed(f64 v, isize decimals)
{
    switch (decimals)
    {
    case 0:
        return cc::format("{:.0f}", v);
    case 1:
        return cc::format("{:.1f}", v);
    case 2:
        return cc::format("{:.2f}", v);
    case 3:
        return cc::format("{:.3f}", v);
    case 4:
        return cc::format("{:.4f}", v);
    case 5:
        return cc::format("{:.5f}", v);
    default:
        return cc::format("{:.6f}", v);
    }
}

cc::string unit_suffix(cc::rec::unit const* unit, cc::string_view prefix)
{
    auto const symbol = unit != nullptr ? cc::string_view(unit->symbol) : cc::string_view();
    if (prefix.empty() && symbol.empty())
        return {};
    return cc::format(" {}{}", prefix, symbol);
}
} // namespace

nx::bench::report_style nx::bench::report_style::for_console()
{
    return {
        .color = console::color_enabled(),
        .width = console::terminal_width().value_or(100),
    };
}

cc::string nx::bench::format_quantity(f64 value, cc::rec::unit const* unit)
{
    auto const base = unit != nullptr ? unit->prefix_base : u32(1000);
    auto const s = apply_prefix(value, base);

    // Three significant digits is what a figure nobody is comparing digit by digit needs.
    auto const magnitude = s.value < 0 ? -s.value : s.value;
    auto const decimals
        = magnitude == 0 ? isize(0) : cc::clamp(isize(2) - decimal_exponent(magnitude), isize(0), isize(6));

    return cc::format("{}{}", fixed(s.value, decimals), unit_suffix(unit, s.prefix));
}

namespace
{
// The shared body, with the scale already chosen.
//
// A table has to pick ONE scale for a column: `89[7] ps` beside `1.82[2] ns` is two numbers a reader has to convert
// before comparing, which is exactly the work the table exists to save.
cc::string format_uncertain_at(f64 half_width,
                               cc::rec::unit const* unit,
                               nx::bench::report_style const& style,
                               scaled_value const& s)
{
    auto const h = half_width / s.factor;

    if (h <= 0)
        return cc::format("{}{}", fixed(s.value, 3), unit_suffix(unit, s.prefix));

    auto const value_exp = decimal_exponent(s.value);
    auto const noise_exp = decimal_exponent(h);

    // The uncertainty reaches the leading digit, so there is no reliable digit to show.
    // That is a failed measurement rather than a wide error bar, and it has to read as one.
    if (noise_exp >= value_exp)
        return cc::format("unstable (+/-{}{})", fixed(h, cc::clamp(isize(2) - noise_exp, isize(0), isize(6))),
                          unit_suffix(unit, s.prefix));

    // Digits at decimal positions at or below the uncertainty's leading digit are the unreliable ones.
    // Integer positions are always all printed, so a positive exponent brackets several digits and a negative one
    // brackets exactly the last.
    auto const decimals = cc::clamp(-noise_exp, isize(0), isize(6));
    auto const text = fixed(s.value, decimals);

    auto unreliable = noise_exp < 0 ? isize(1) : noise_exp + 1;
    if (-noise_exp > 6)
        unreliable = 0; // the uncertainty sits below everything printed, so every shown digit is real

    auto const digits = isize(text.size());
    if (unreliable <= 0 || unreliable >= digits)
        return cc::format("{}{}", text, unit_suffix(unit, s.prefix));

    auto const head = cc::string_view(text).subview_clamped(0, digits - unreliable);
    auto const tail = cc::string_view(text).subview(digits - unreliable);

    if (style.color)
        return cc::format("{}{}{}", head, console::colorize(console::color::bright_black, tail, true),
                          unit_suffix(unit, s.prefix));

    return cc::format("{}[{}]{}", head, tail, unit_suffix(unit, s.prefix));
}
} // namespace

cc::string nx::bench::format_uncertain(f64 value, f64 half_width, cc::rec::unit const* unit, report_style const& style)
{
    if (value <= 0)
        return format_quantity(value, unit);

    auto const base = unit != nullptr ? unit->prefix_base : u32(1000);
    return format_uncertain_at(half_width, unit, style, apply_prefix(value, base));
}

namespace
{
using nx::bench::result;

// The conservative interval on a ratio, from the two independent intervals it is built out of.
//
// [b_low / a_high, b_high / a_low] cannot be tighter than the truth, which is the right direction to be wrong in:
// it never claims a difference that is not there.
// A bootstrap would give a narrower one and would need a seed, which the rest of these statistics deliberately do not.
struct ratio_interval
{
    f64 low = 0;
    f64 high = 0;
    bool spans_one = false;
};

ratio_interval compare_to(result const& baseline, result const& other)
{
    auto r = ratio_interval{};
    if (baseline.time.ci95_low <= 0 || baseline.time.ci95_high <= 0)
        return r;

    r.low = other.time.ci95_low / baseline.time.ci95_high;
    r.high = other.time.ci95_high / baseline.time.ci95_low;
    r.spans_one = r.low <= 1.0 && r.high >= 1.0;
    return r;
}

cc::string format_comparison(result const& baseline, result const& other, nx::bench::report_style const& style)
{
    if (&baseline == &other)
        return cc::string("baseline");

    auto const r = compare_to(baseline, other);
    if (r.low <= 0)
        return cc::string("-");

    // An interval spanning 1.0 is not a small difference; it is no measured difference at all, and drawing a number
    // there is the single most common way a benchmark table misleads.
    if (r.spans_one)
        return style.color ? console::colorize(console::color::bright_black, "~same", true) : cc::string("~same");

    auto const midpoint = other.time.median / baseline.time.median;
    auto const percent = (midpoint - 1.0) * 100;
    auto const faster = percent < 0;
    auto const text = cc::format("{}{:.1f}%", faster ? "" : "+", percent);

    if (!style.color)
        return text;
    return console::colorize(faster ? console::color::green : console::color::red, text, true);
}

// `attribute` names the loop, which a table needs and a single-loop block does not: two identical-looking warnings
// under a table say nothing about which row produced them.
void append_warnings(cc::string& out, result const& r, nx::bench::report_style const& style, bool attribute)
{
    for (auto const& w : r.warnings)
    {
        auto const label = w.severity == nx::bench::warning_severity::error   ? cc::string_view("error")
                         : w.severity == nx::bench::warning_severity::warning ? cc::string_view("warning")
                                                                              : cc::string_view("note");
        auto const colored
            = style.color
                ? console::colorize(w.severity == nx::bench::warning_severity::error     ? console::color::red
                                    : w.severity == nx::bench::warning_severity::warning ? console::color::yellow
                                                                                         : console::color::bright_black,
                                    label, true)
                : cc::string(label);

        if (attribute && !r.name.empty())
            out.appendf("  {}  {}: {}\n", colored, r.name, w.detail);
        else
            out.appendf("  {}  {}\n", colored, w.detail);
    }
}

// The full statistics block, which is what a single loop gets instead of a one-row table.
void append_block(cc::string& out, result const& r, nx::bench::report_style const& style)
{
    auto const* secs = &cc::rec::unit_seconds;
    auto const half = (r.time.ci95_high - r.time.ci95_low) * 0.5;

    out.appendf("  median     {}\n", nx::bench::format_uncertain(r.time.median, half, secs, style));
    out.appendf("  interval   [{}, {}] ci95{}\n", nx::bench::format_quantity(r.time.ci95_low, secs),
                nx::bench::format_quantity(r.time.ci95_high, secs), r.time.ci_is_bound ? " (sample range)" : "");
    out.appendf("  min {}   mean {}   trimmed {}\n", nx::bench::format_quantity(r.time.min, secs),
                nx::bench::format_quantity(r.time.mean, secs), nx::bench::format_quantity(r.time.trimmed_mean, secs));
    out.appendf("  spread     mad {}\n", nx::bench::format_quantity(r.time.mad, secs));
    out.appendf("  samples    {} x {} iterations over {}   {}   {} outlier(s)\n", r.samples.size(), r.batch_size,
                nx::bench::format_quantity(r.measured_seconds, secs), r.converged ? "converged" : "NOT converged",
                r.time.outliers);

    if (r.items > 0)
    {
        auto const* count = &cc::rec::unit_count;
        out.appendf("  items      {}/s   {} per iteration\n", nx::bench::format_quantity(r.items_per_second, count),
                    r.measured_iterations > 0 ? r.items / r.measured_iterations : 0);
    }

    for (auto const& q : r.quantities)
    {
        if (q.per_second > 0)
            out.appendf("  {}      {}   {}/s\n", q.name, nx::bench::format_quantity(q.per_iteration, q.unit),
                        nx::bench::format_quantity(q.per_second, q.unit));
        else
            out.appendf("  {}      {}\n", q.name, nx::bench::format_quantity(q.total, q.unit));
    }

    // Counters, where the machine had a PMU to read.
    // Per item as well as per iteration where the body declared items, since that is the figure that stays comparable
    // across input sizes.
    for (auto const& c : r.counters)
    {
        if (c.per_item > 0)
            out.appendf("  {:<10} {:.2f} per iteration   {:.2f} per item\n", c.name, c.per_iteration, c.per_item);
        else
            out.appendf("  {:<10} {:.2f} per iteration\n", c.name, c.per_iteration);
    }

    out.appendf("  overhead   {:.1f}% of per-iteration time\n", r.overhead_fraction * 100);
    append_warnings(out, r, style, false);
}
} // namespace

cc::string nx::bench::format_report(cc::string_view title, cc::span<result const> loops, report_style const& style)
{
    auto out = cc::string();
    if (loops.empty())
        return out;

    out.appendf("{}\n", title);

    if (loops.size() == 1)
    {
        // Nothing to compare against, so the width a table would spend on columns goes into depth instead.
        append_block(out, loops[0], style);
        return out;
    }

    // The baseline is the first loop declared, unless one asked to be it.
    auto baseline = isize(0);
    for (auto i = isize(0); i < loops.size(); ++i)
        if (loops[i].config.is_baseline)
        {
            baseline = i;
            break;
        }

    // The comparison column always shows, against the first loop declared unless one asked to be the baseline.
    //
    // A sweep has no meaningful baseline — comparing every size against the smallest says nothing — but opting out of
    // it belongs to the sweep rather than to a guess here.
    // An earlier version dropped the column once there were more than three loops, which quietly took it away from a
    // four-row comparison that wanted it.
    auto const show_comparison = true;

    auto const* secs = &cc::rec::unit_seconds;

    auto name_width = isize(4);
    for (auto const& r : loops)
        name_width = cc::max(name_width, isize(r.name.size()));

    auto const bar = style.markdown ? cc::string_view(" | ") : cc::string_view("   ");
    if (style.markdown)
        out.appendf("\n| {} | median | items/s |{}\n", cc::string_view("name"), show_comparison ? " vs base |" : "");
    else
        out.appendf("\n");

    if (style.markdown)
        out.appendf("|---|---|---|{}\n", show_comparison ? "---|" : "");

    // One scale for the whole column, taken from the slowest row.
    // Picking it per row would put `89[7] ps` beside `1.82[2] ns`, which is two conversions a reader has to do before
    // the comparison the table exists for.
    auto slowest = f64(0);
    for (auto const& r : loops)
        slowest = cc::max(slowest, r.time.median);
    auto const column_scale = apply_prefix(slowest > 0 ? slowest : 1.0, secs->prefix_base);

    for (auto i = isize(0); i < loops.size(); ++i)
    {
        auto const& r = loops[i];
        auto const half = (r.time.ci95_high - r.time.ci95_low) * 0.5;

        auto row = cc::string();
        row.appendf("{}", r.name);
        while (isize(row.size()) < name_width)
            row += ' ';

        auto scaled_row = column_scale;
        scaled_row.value = r.time.median / column_scale.factor;
        row.appendf("{}{}", bar, format_uncertain_at(half, secs, style, scaled_row));
        row.appendf("{}{}", bar,
                    r.items_per_second > 0 ? format_quantity(r.items_per_second, &cc::rec::unit_count) + "/s"
                                           : cc::string("-"));

        if (show_comparison)
            row.appendf("{}{}", bar, format_comparison(loops[baseline], r, style));

        if (style.markdown)
            out.appendf("| {} |\n", row);
        else
            out.appendf("  {}\n", row);
    }

    for (auto const& r : loops)
    {
        if (style.verbose)
        {
            out.appendf("\n{}\n", r.name);
            append_block(out, r, style);
        }
        else
        {
            append_warnings(out, r, style, true);
        }
    }

    return out;
}
