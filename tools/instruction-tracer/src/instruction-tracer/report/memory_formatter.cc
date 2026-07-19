#include "memory_formatter.hh"

#include <clean-core/container/map.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <instruction-tracer/decode/memory_access.hh>
#include <instruction-tracer/report/console.hh>
#include <instruction-tracer/report/trace_formatter.hh>

#include <algorithm> // std::sort: cachelines print in ascending address order

namespace itrace
{
namespace
{
/// "R" / "W" / "RW" for a single access.
cc::string_view rw_text(bool reads, bool writes)
{
    if (reads && writes)
        return "RW";
    return reads ? "R" : "W";
}

template <class T>
void add_distinct(cc::vector<T>& into, T const& value)
{
    for (auto const& v : into)
        if (v == value)
            return;
    into.push_back(value);
}

/// Distinct symbols hitting one cacheline, comma-joined and truncated to 100 chars — several
/// adjacent globals, or a global next to its padding, all land on one line and are worth naming.
cc::string join_symbols(cc::span<cc::string const> symbols)
{
    constexpr isize limit = 100;

    cc::string out;
    for (auto const& s : symbols)
    {
        if (s.empty())
            continue;

        auto const sep = out.empty() ? cc::string_view("") : cc::string_view(", ");
        if (out.size() + sep.size() + s.size() > limit)
        {
            out += cc::string(sep) + "…";
            break;
        }
        out += cc::string(sep) + s;
    }
    return out;
}

/// Accessing instruction addresses for one cacheline, comma-joined and truncated to 100 chars.
cc::string join_rips(cc::span<u64 const> rips)
{
    constexpr isize limit = 100;

    cc::string out;
    for (auto const rip : rips)
    {
        auto const cell = format_address(rip);
        auto const sep = out.empty() ? cc::string_view("") : cc::string_view(", ");
        if (out.size() + sep.size() + cell.size() > limit)
        {
            out += cc::string(sep) + "…";
            break;
        }
        out += cc::string(sep) + cell;
    }
    return out;
}
} // namespace

bool memory_view_options::includes(access_region region) const
{
    switch (region)
    {
    case access_region::heap:
        return heap;
    case access_region::frame:
        return frame;
    case access_region::stack:
        return stack;
    case access_region::instructions:
        return instructions;
    }
    return false;
}

cc::string format_memory_raw(cc::span<trace const> traces, memory_view_options const& opts)
{
    cc::string out;
    out += bold("=== memory accesses ===") + "\n";

    isize shown = 0;
    for (auto const& t : traces)
    {
        if (traces.size() > 1)
            out += "\n" + dim(cc::format("--- trace {} ---", t.index)) + "\n";

        for (auto const& insn : t.instructions)
            for (auto const& acc : insn.memory_accesses)
            {
                if (!opts.includes(acc.region))
                    continue;

                ++shown;
                out += "  ";
                if (opts.instruction_addresses)
                    out += dim(format_address(insn.rip)) + "  ";

                cc::format_append(out, "{}  {:>3} B  {:<2}", format_address(acc.address), acc.size,
                                  rw_text(acc.is_read, acc.is_write));
                out += dim(cc::format("  ; {:<12}", region_name(acc.region)));
                if (!acc.symbol.empty())
                    out += " " + cyan(acc.symbol);
                out += '\n';
            }
    }

    if (shown == 0)
        out += dim("  (no accesses in the selected regions)") + "\n";

    return out;
}

cc::string format_memory_cachelines(cc::span<trace const> traces, memory_view_options const& opts)
{
    struct bucket
    {
        u64 line = 0; // address / cacheline_bytes
        u32 accesses = 0;
        u64 mask = 0; // one bit per touched byte of the line
        bool reads = false;
        bool writes = false;
        cc::vector<cc::string> symbols;
        cc::vector<u64> rips;
    };

    // line index -> position in `buckets`; the buckets stay in insertion order and are sorted below.
    cc::map<u64, isize> index;
    cc::vector<bucket> buckets;

    for (auto const& t : traces)
        for (auto const& insn : t.instructions)
            for (auto const& acc : insn.memory_accesses)
            {
                if (!opts.includes(acc.region) || acc.size == 0)
                    continue;

                auto const first = acc.address / cacheline_bytes;
                auto const last = (acc.address + acc.size - 1) / cacheline_bytes;
                for (auto line = first; line <= last; ++line)
                {
                    auto e = index.entry(line);
                    if (!e.exists())
                    {
                        e.emplace(buckets.size());
                        buckets.push_back(bucket{.line = line});
                    }
                    auto& b = buckets[e.value()];

                    auto const line_base = line * cacheline_bytes;
                    auto const lo = cc::max(acc.address, line_base) - line_base;
                    auto const hi = cc::min(acc.address + acc.size, line_base + cacheline_bytes) - line_base;
                    for (auto bit = lo; bit < hi; ++bit)
                        b.mask |= u64(1) << bit;

                    b.accesses++;
                    b.reads |= acc.is_read;
                    b.writes |= acc.is_write;
                    add_distinct(b.symbols, acc.symbol);
                    if (opts.instruction_addresses)
                        add_distinct(b.rips, insn.rip);
                }
            }

    cc::string out;
    out += bold("=== cachelines ===") + "\n";

    if (buckets.empty())
    {
        out += dim("  (no accesses in the selected regions)") + "\n";
        return out;
    }

    std::sort(buckets.begin(), buckets.end(), [](bucket const& a, bucket const& b) { return a.line < b.line; });

    for (isize i = 0; i < buckets.size(); ++i)
    {
        auto const& b = buckets[i];

        // A gap: the previous printed line is not adjacent to this one in memory.
        if (i > 0 && b.line != buckets[i - 1].line + 1)
            out += '\n';

        cc::format_append(out, "  {}  {:>4} acc  {:>2}/{} B  {:<2}", format_address(b.line * cacheline_bytes),
                          b.accesses, cc::popcount(b.mask), cacheline_bytes, rw_text(b.reads, b.writes));

        auto const names = join_symbols(b.symbols);
        if (!names.empty())
            out += dim("  ; ") + cyan(names);
        if (opts.instruction_addresses && !b.rips.empty())
            out += dim("  ; @ ") + dim(join_rips(b.rips));
        out += '\n';
    }

    return out;
}
} // namespace itrace
