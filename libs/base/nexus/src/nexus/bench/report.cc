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

/// Printed width, counting what a terminal shows rather than what the string holds.
///
/// A muted uncertainty is wrapped in `\x1b[90m` and `\x1b[0m` -- nine bytes that print as none -- so measuring bytes
/// would pad a coloured cell by nine columns too few and knock every column to its right out of line.
isize display_width(cc::string_view s)
{
    auto width = isize(0);
    for (auto i = isize(0); i < isize(s.size()); ++i)
    {
        if (s[i] != '\x1b')
        {
            // A UTF-8 continuation byte is the tail of a character already counted, so it is worth no column of its
            // own — without this the two bytes of `±` pad every cell carrying one a column too wide.
            if ((u8(s[i]) & 0xC0) != 0x80)
                ++width;
            continue;
        }
        // An SGR sequence runs to its terminating 'm'; nothing else here emits an escape.
        while (i < isize(s.size()) && s[i] != 'm')
            ++i;
    }
    return width;
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
// U+00B1, written as its own UTF-8 bytes so no source-encoding guess can change what reaches the terminal.
//
// The one non-ASCII character this file emits, and a deliberate exception to the rule that keeps the micro prefix as
// "u": a prefix has a plain ASCII spelling that costs the reader nothing, and a plus-minus sign does not.
constexpr cc::string_view plus_minus = "\xc2\xb1";

// The shared body, with the scale already chosen.
//
// A table has to pick ONE scale for a column: `89 ± 7 ps` beside `1.82 ± 0.02 ns` is two numbers a reader has to
// convert before comparing, which is exactly the work the table exists to save.
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

    // The uncertainty sits below every digit that would be printed, so there is nothing left to qualify.
    if (-noise_exp > 6)
        return cc::format("{}{}", fixed(s.value, 3), unit_suffix(unit, s.prefix));

    // **Both numbers are printed at the uncertainty's own decimal place.**
    // A value carrying digits its interval does not reach claims a precision the measurement never had, which is the
    // failure this rendering exists to prevent.
    auto const decimals = cc::clamp(-noise_exp, isize(0), isize(6));
    auto const value_text = fixed(s.value, decimals);
    auto const noise_text = fixed(h, decimals);

    // The uncertainty reaches the leading digit, so not one digit of the value survives it.
    // That is a failed measurement rather than a wide error bar, and it has to read as one.
    if (noise_exp >= value_exp)
        return cc::format("unstable ({} {} {}{})", value_text, plus_minus, noise_text, unit_suffix(unit, s.prefix));

    // Only the uncertainty is muted, so the eye lands on the value and the text stays identical either way.
    if (style.color)
        return cc::format(
            "{} {}{}", value_text,
            console::colorize(console::color::bright_black, cc::format("{} {}", plus_minus, noise_text), true),
            unit_suffix(unit, s.prefix));

    return cc::format("{} {} {}{}", value_text, plus_minus, noise_text, unit_suffix(unit, s.prefix));
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
    auto const faster = midpoint < 1.0;

    // A percentage stops being readable once the ratio leaves the neighbourhood of 1, and a factor of two is where
    // that happens: nobody reads "-83.0%" and thinks "5.9x faster", which is the number they would go on to quote.
    // Past the boundary a percentage only gets worse — "+18424.1%" is a true and useless way to say 184x.
    // Inside it the percentage wins, because "-43.4%" is clearer than "1.8x faster" for a difference that small.
    auto const text = midpoint > 2.0 ? cc::format("{:.1f}x slower", midpoint)
                    : midpoint < 0.5 ? cc::format("{:.1f}x faster", 1.0 / midpoint)
                                     : cc::format("{}{:.1f}%", faster ? "" : "+", (midpoint - 1.0) * 100);

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

    // Printed only where one sample is one iteration.
    // Under batching a sample is a batch MEAN, so its tail is the tail of an average and reporting it as a latency
    // percentile would be a claim the numbers do not support.
    if (!r.config.batch)
        out.appendf("  tail       p95 {}   p99 {}   max {}\n", nx::bench::format_quantity(r.time.p95, secs),
                    nx::bench::format_quantity(r.time.p99, secs), nx::bench::format_quantity(r.time.max, secs));
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

    // The comparison column shows against the first loop declared, unless one asked to be the baseline — or unless a
    // loop opted the table out of comparison entirely.
    //
    // A sweep has no meaningful baseline, and opting out belongs to the sweep rather than to a guess here: an earlier
    // version dropped the column once there were more than three loops, which quietly took it away from a four-row
    // comparison that wanted it.
    auto show_comparison = true;
    for (auto const& r : loops)
        if (r.config.no_baseline)
            show_comparison = false;

    auto const* secs = &cc::rec::unit_seconds;

    // A column per quantity the loops recorded, in first-seen order.
    //
    // Without this a benchmark that records bytes gets its rate into the JSON and nowhere a reader looks: the table
    // would show only time and items, and the figure the author went to the trouble of recording would be missing.
    auto quantity_names = cc::vector<cc::string>();
    for (auto const& r : loops)
        for (auto const& q : r.quantities)
        {
            auto seen = false;
            for (auto const& known : quantity_names)
                if (known == q.name)
                    seen = true;
            if (!seen)
                quantity_names.push_back(q.name);
        }

    // A column no row fills is noise, and a benchmark measuring whole passes rather than elements fills none of this
    // one -- so the sweep that declares no items gets no items column, on the same argument that drops the comparison.
    auto show_items = false;
    for (auto const& r : loops)
        if (r.items_per_second > 0)
            show_items = true;

    // One scale for the whole time column, taken from the slowest row.
    // Picking it per row would put `89 ± 7 ps` beside `1.82 ± 0.02 ns`, which is two conversions a reader has to do
    // before the comparison the table exists for.
    //
    // **A table with no comparison scales per row instead.**
    // The shared scale is what makes a column readable *down*, and that is worth having only where the rows are being
    // read against each other.
    // A sweep spans decades by construction, so one scale spells its small end `0.000108 ± 0.000004 ms` -- four
    // leading zeroes on each half of a number every reader would rather see as `108 ± 4 ns`.
    auto slowest = f64(0);
    for (auto const& r : loops)
        slowest = cc::max(slowest, r.time.median);
    auto const column_scale = apply_prefix(slowest > 0 ? slowest : 1.0, secs->prefix_base);

    // Every cell is rendered before any is printed, because a column is only as wide as its widest entry and that is
    // not known until the last row exists.
    // Appending straight to the output instead is what puts two differently-sized cells in one column.
    auto header = cc::vector<cc::string>();
    header.push_back(cc::string("name"));
    header.push_back(cc::string("median"));
    if (show_items)
        header.push_back(cc::string("items/s"));
    for (auto const& q : quantity_names)
        header.push_back(q);
    if (show_comparison)
        header.push_back(cc::string("vs base"));

    auto rows = cc::vector<cc::vector<cc::string>>();
    rows.reserve(loops.size());
    for (auto const& r : loops)
    {
        auto const half = (r.time.ci95_high - r.time.ci95_low) * 0.5;
        auto scaled_row = show_comparison ? column_scale : apply_prefix(r.time.median, secs->prefix_base);
        if (show_comparison)
            scaled_row.value = r.time.median / column_scale.factor;

        auto row = cc::vector<cc::string>();
        row.push_back(cc::string(r.name));
        row.push_back(format_uncertain_at(half, secs, style, scaled_row));
        if (show_items)
            row.push_back(r.items_per_second > 0 ? format_quantity(r.items_per_second, &cc::rec::unit_count) + "/s"
                                                 : cc::string("-"));

        for (auto const& wanted : quantity_names)
        {
            auto const* const q = r.find_quantity(wanted);
            if (q == nullptr)
            {
                row.push_back(cc::string("-"));
                continue;
            }

            // A rate where the unit sums, and the value itself where it averages: a mean of ratios per second is
            // nonsense, and the unit is what says which of the two this is.
            if (q->per_second > 0)
                row.push_back(format_quantity(q->per_second, q->unit) + "/s");
            else
                row.push_back(format_quantity(q->total, q->unit));
        }

        if (show_comparison)
            row.push_back(format_comparison(loops[baseline], r, style));

        rows.push_back(cc::move(row));
    }

    // A quantity column is named by the header and nothing else, so the header is printed unless the only columns are
    // the self-describing ones.
    auto const show_header = style.markdown || !quantity_names.empty();

    auto widths = cc::vector<isize>();
    for (auto i = isize(0); i < header.size(); ++i)
        widths.push_back(show_header ? display_width(header[i]) : isize(0));
    for (auto const& row : rows)
        for (auto i = isize(0); i < row.size(); ++i)
            widths[i] = cc::max(widths[i], display_width(row[i]));

    auto const bar = style.markdown ? cc::string_view(" | ") : cc::string_view("   ");

    // The trailing cell is left unpadded: in plain text that would be invisible trailing whitespace, and a markdown
    // table does not need it to line up.
    auto const emit_row = [&](cc::vector<cc::string> const& row)
    {
        auto line = cc::string();
        for (auto i = isize(0); i < row.size(); ++i)
        {
            if (i > 0)
                line += bar;
            line += row[i];
            if (i + 1 < row.size())
                for (auto pad = display_width(row[i]); pad < widths[i]; ++pad)
                    line += ' ';
        }
        if (style.markdown)
            out.appendf("| {} |\n", line);
        else
            out.appendf("  {}\n", line);
    };

    out.appendf("\n");

    // The header earns its line as soon as a column has a name a reader cannot guess.
    // Time and items are self-describing; a recorded quantity is a bare number under nothing at all, and four of them
    // side by side are unreadable without their names.
    if (show_header)
        emit_row(header);

    if (style.markdown)
    {
        // The separator is written tight rather than through emit_row: padding it to the column widths would spell
        // the rule as `| ------- |`, which renders the same and reads as a row of content in the source.
        out.appendf("|");
        for (auto i = isize(0); i < header.size(); ++i)
            out.appendf("---|");
        out.appendf("\n");
    }
    for (auto const& row : rows)
        emit_row(row);

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
