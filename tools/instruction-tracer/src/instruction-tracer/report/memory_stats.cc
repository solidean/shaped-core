#include "memory_stats.hh"

#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <instruction-tracer/report/console.hh>
#include <instruction-tracer/report/trace_stats.hh> // strip_template_args

#include <algorithm> // std::sort: rank the table by traffic, with a name tie-break

namespace itrace
{
namespace
{
struct row_cells
{
    cc::string accesses;
    cc::string rw;
    cc::string lines;
    cc::string bytes;
    cc::string symbol;
};

row_cells cells_of(memory_symbol_stats const& s)
{
    row_cells c;
    c.accesses = cc::format("{}", s.accesses);
    c.rw = cc::format("{}/{}", s.reads, s.writes);
    c.lines = cc::format("{}", s.cachelines);
    c.bytes = cc::format("{}", s.bytes);
    c.symbol = s.symbol;
    return c;
}

struct column_widths
{
    isize accesses = 0;
    isize rw = 0;
    isize lines = 0;
    isize bytes = 0;
};

void widen(column_widths& w, row_cells const& c)
{
    w.accesses = cc::max(w.accesses, c.accesses.size());
    w.rw = cc::max(w.rw, c.rw.size());
    w.lines = cc::max(w.lines, c.lines.size());
    w.bytes = cc::max(w.bytes, c.bytes.size());
}

cc::string pad_left(cc::string_view s, isize width)
{
    cc::string out;
    for (isize i = s.size(); i < width; ++i)
        out += ' ';
    out += s;
    return out;
}

cc::string row_line(row_cells const& c, column_widths const& w)
{
    cc::string out;
    out += "  " + pad_left(c.accesses, w.accesses);
    out += "  " + pad_left(c.rw, w.rw);
    out += "  " + pad_left(c.lines, w.lines);
    out += "  " + pad_left(c.bytes, w.bytes);
    out += "  " + c.symbol;
    return out;
}
} // namespace

memory_stats_summary collect_memory_stats(cc::span<trace const> traces, memory_view_options const& opts)
{
    memory_stats_summary summary;
    summary.traces = u32(traces.size());

    cc::map<cc::string, isize> index;
    cc::vector<cc::set<u64>> row_lines; // distinct cachelines per row, parallel to summary.rows
    cc::set<u64> total_lines;

    for (auto const& t : traces)
    {
        if (t.reason == step_reason::instruction_budget)
            summary.truncated = true;

        for (auto const& insn : t.instructions)
        {
            for (auto const& acc : insn.memory_accesses)
            {
                if (!opts.includes(acc.region))
                    continue;

                auto name = insn.owner_symbol.empty() ? cc::string("(unknown)") : strip_template_args(insn.owner_symbol);

                auto e = index.entry(name);
                if (!e.exists())
                {
                    e.emplace(summary.rows.size());
                    memory_symbol_stats fresh;
                    fresh.symbol = cc::move(name);
                    summary.rows.push_back(cc::move(fresh));
                    row_lines.emplace_back();
                }

                auto& row = summary.rows[e.value()];
                row.accesses++;
                row.reads += acc.is_read ? 1 : 0;
                row.writes += acc.is_write ? 1 : 0;
                row.bytes += acc.size;
                summary.total.accesses++;
                summary.total.reads += acc.is_read ? 1 : 0;
                summary.total.writes += acc.is_write ? 1 : 0;
                summary.total.bytes += acc.size;

                if (acc.size > 0)
                {
                    auto const first = acc.address / cacheline_bytes;
                    auto const last = (acc.address + acc.size - 1) / cacheline_bytes;
                    for (auto line = first; line <= last; ++line)
                    {
                        row_lines[e.value()].insert(line);
                        total_lines.insert(line);
                    }
                }
            }
        }
    }

    for (isize i = 0; i < summary.rows.size(); ++i)
        summary.rows[i].cachelines = u32(row_lines[i].size());
    summary.total.cachelines = u32(total_lines.size());

    std::sort(summary.rows.begin(), summary.rows.end(),
              [](memory_symbol_stats const& a, memory_symbol_stats const& b)
              {
                  if (a.accesses != b.accesses)
                      return a.accesses > b.accesses;
                  return a.symbol.compare(b.symbol) < 0;
              });

    return summary;
}

cc::string format_memory_stats(memory_stats_summary const& summary)
{
    cc::string out;
    out += bold("=== memory stats ===") + "\n";

    if (summary.rows.empty())
    {
        out += dim("  (no accesses in the selected regions)") + "\n";
        return out;
    }

    row_cells header;
    header.accesses = "acc";
    header.rw = "r/w";
    header.lines = "lines";
    header.bytes = "bytes";
    header.symbol = "symbol";

    auto total = cells_of(summary.total);
    total.symbol = summary.traces == 1 ? cc::string("total (1 trace)") : cc::format("total ({} traces)", summary.traces);

    column_widths w;
    widen(w, header);
    widen(w, total);

    cc::vector<row_cells> cells;
    for (auto const& r : summary.rows)
    {
        cells.push_back(cells_of(r));
        widen(w, cells.back());
    }

    out += dim(row_line(header, w)) + "\n";
    for (auto const& c : cells)
        out += row_line(c, w) + "\n";

    auto const rule_width = 2 + w.accesses + 2 + w.rw + 2 + w.lines + 2 + w.bytes;
    cc::string rule;
    for (isize i = 0; i < rule_width; ++i)
        rule += i < 2 ? ' ' : '-';
    out += dim(rule) + "\n";

    out += row_line(total, w) + "\n";

    if (summary.truncated)
        out += "\n" + yellow("TRUNCATED: a trace hit --instructions; these counts are incomplete.") + "\n";

    return out;
}
} // namespace itrace
