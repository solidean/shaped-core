#include "trace_stats.hh"

#include <clean-core/container/map.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <instruction-tracer/report/console.hh>

#include <algorithm> // std::sort: rank the table by cost, with a name tie-break for a stable order

namespace itrace
{
namespace
{
bool is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// Length of an `operator` token starting at `i` — "operator", "operator<<", "operator<=>", … — or 0
/// if there is none. Its angle brackets are punctuation, not template brackets, so the depth count
/// must step over them rather than through them.
isize operator_token_length(cc::string_view name, isize i)
{
    constexpr cc::string_view keyword = "operator";

    if (i > 0 && is_ident_char(name[i - 1])) // a suffix of a longer identifier, e.g. "myoperator"
        return 0;
    if (!name.subview({.start = i, .end = name.size()}).starts_with(keyword))
        return 0;

    // Only the angle-bracket spellings can confuse the depth; the rest need no special care.
    isize n = keyword.size();
    while (i + n < name.size() && (name[i + n] == '<' || name[i + n] == '>' || name[i + n] == '='))
        ++n;

    return n;
}

/// Tally one expensive instruction against (mnemonic, symbol). A linear scan is right here: the list
/// is empty on almost every trace and a handful of entries on the rest.
void add_slow_op(cc::vector<slow_op>& ops, cc::string_view mnemonic, cc::string_view symbol)
{
    for (auto& op : ops)
        if (op.mnemonic == mnemonic && op.symbol == symbol)
        {
            op.count++;
            return;
        }

    slow_op fresh;
    fresh.mnemonic = mnemonic;
    fresh.symbol = symbol;
    fresh.count = 1;
    ops.push_back(cc::move(fresh));
}

struct row_cells
{
    cc::string self;
    cc::string atomics;
    cc::string slow;
    cc::string calls;
    cc::string memory;
    cc::string branches;
    cc::string symbol;
};

row_cells cells_of(symbol_stats const& s)
{
    row_cells c;
    c.self = cc::format("{}", s.instructions);
    c.atomics = cc::format("{}", s.atomics);
    c.slow = cc::format("{}", s.slow);
    c.calls = cc::format("{}/{}", s.direct_calls, s.indirect_calls);
    c.memory = cc::format("{}/{}", s.memory_reads, s.memory_writes);
    c.branches = cc::format("{} ({})", s.branches, s.branches_taken);
    c.symbol = s.symbol;
    return c;
}

struct column_widths
{
    isize self = 0;
    isize atomics = 0;
    isize slow = 0;
    isize calls = 0;
    isize memory = 0;
    isize branches = 0;
};

void widen(column_widths& w, row_cells const& c)
{
    w.self = cc::max(w.self, c.self.size());
    w.atomics = cc::max(w.atomics, c.atomics.size());
    w.slow = cc::max(w.slow, c.slow.size());
    w.calls = cc::max(w.calls, c.calls.size());
    w.memory = cc::max(w.memory, c.memory.size());
    w.branches = cc::max(w.branches, c.branches.size());
}

cc::string pad_left(cc::string_view s, isize width)
{
    cc::string out;
    for (isize i = s.size(); i < width; ++i)
        out += ' ';
    out += s;
    return out;
}

/// One line, numbers right-aligned and the variable-width symbol last. No color inside a cell, so
/// plain padding is honest here — unlike the trace formatter, which pads around color codes.
cc::string row_line(row_cells const& c, column_widths const& w)
{
    cc::string out;
    out += "  " + pad_left(c.self, w.self);
    out += "  " + pad_left(c.atomics, w.atomics);
    out += "  " + pad_left(c.slow, w.slow);
    out += "  " + pad_left(c.calls, w.calls);
    out += "  " + pad_left(c.memory, w.memory);
    out += "  " + pad_left(c.branches, w.branches);
    out += "  " + c.symbol;
    return out;
}

/// Named, one per line, because which instruction it is *is* the finding. Empty when there are none,
/// which is the normal case and needs no ceremony.
cc::string format_slow_ops(cc::span<slow_op const> ops)
{
    if (ops.empty())
        return {};

    isize name_width = 0;
    isize count_width = 0;
    for (auto const& op : ops)
    {
        name_width = cc::max(name_width, op.mnemonic.size());
        count_width = cc::max(count_width, cc::format("x{}", op.count).size());
    }

    cc::string out
        = "\n" + yellow("slow ops") + dim(" (tens of cycles each — the instruction count does not show these)") + "\n";
    for (auto const& op : ops)
    {
        out += "  " + pad_left(op.mnemonic, name_width);
        out += "  " + pad_left(cc::format("x{}", op.count), count_width);
        out += "  " + op.symbol + "\n";
    }

    return out;
}

row_cells total_of(cc::span<symbol_stats const> rows, u32 traces)
{
    symbol_stats sum;
    for (auto const& r : rows)
    {
        sum.instructions += r.instructions;
        sum.atomics += r.atomics;
        sum.slow += r.slow;
        sum.direct_calls += r.direct_calls;
        sum.indirect_calls += r.indirect_calls;
        sum.memory_reads += r.memory_reads;
        sum.memory_writes += r.memory_writes;
        sum.branches += r.branches;
        sum.branches_taken += r.branches_taken;
    }

    auto cells = cells_of(sum);
    cells.symbol = traces == 1 ? cc::string("total (1 trace)") : cc::format("total ({} traces)", traces);
    return cells;
}
} // namespace

cc::string strip_template_args(cc::string_view name)
{
    cc::string out;
    int depth = 0;

    for (isize i = 0; i < name.size();)
    {
        if (auto const n = operator_token_length(name, i); n > 0)
        {
            if (depth == 0)
                out += name.subview({.start = i, .end = i + n});
            i += n;
            continue;
        }

        char const c = name[i];
        if (c == '<')
            ++depth;
        else if (c == '>')
        {
            if (depth == 0) // unbalanced: a whole name beats a mangled one
                return cc::string(name);
            --depth;
        }
        else if (depth == 0)
            out += c;

        ++i;
    }

    if (depth != 0)
        return cc::string(name);

    // A component that was nothing but template arguments (a lambda, an instantiation tag) leaves an
    // empty one behind: "function_ref::<lambda>::__invoke" -> "function_ref::::__invoke".
    while (out.replace_all("::::", "::") > 0)
    {
    }

    return out;
}

stats_summary collect_stats(cc::span<trace const> traces)
{
    stats_summary summary;
    summary.traces = u32(traces.size());

    // symbol -> index into summary.rows; the rows themselves live in the vector so they stay sorted.
    cc::map<cc::string, isize> index;

    for (auto const& t : traces)
    {
        if (t.reason == step_reason::instruction_budget)
            summary.truncated = true;

        for (auto const& insn : t.instructions)
        {
            auto name = insn.owner_symbol.empty() ? cc::string("(unknown)") : strip_template_args(insn.owner_symbol);

            auto e = index.entry(name);
            if (!e.exists())
            {
                e.emplace(summary.rows.size());

                symbol_stats fresh;
                fresh.symbol = cc::move(name);
                summary.rows.push_back(cc::move(fresh));
            }

            auto& row = summary.rows[e.value()];
            row.instructions++;

            if (insn.is_atomic)
                row.atomics++;

            if (insn.slow_mnemonic != nullptr)
            {
                row.slow++;
                add_slow_op(summary.slow_ops, insn.slow_mnemonic, row.symbol);
            }
            if (insn.reads_memory)
                row.memory_reads++;
            if (insn.writes_memory)
                row.memory_writes++;

            if (insn.category == insn_category::call)
            {
                if (insn.is_indirect)
                    row.indirect_calls++;
                else
                    row.direct_calls++;
            }

            if (insn.category == insn_category::conditional_branch)
            {
                row.branches++;
                if (diverged(insn))
                    row.branches_taken++;
            }
        }
    }

    std::sort(summary.rows.begin(), summary.rows.end(),
              [](symbol_stats const& a, symbol_stats const& b)
              {
                  if (a.instructions != b.instructions)
                      return a.instructions > b.instructions;
                  return a.symbol.compare(b.symbol) < 0;
              });

    std::sort(summary.slow_ops.begin(), summary.slow_ops.end(),
              [](slow_op const& a, slow_op const& b)
              {
                  if (a.count != b.count)
                      return a.count > b.count;
                  if (auto const c = a.mnemonic.compare(b.mnemonic); c != 0)
                      return c < 0;
                  return a.symbol.compare(b.symbol) < 0;
              });

    return summary;
}

cc::string format_stats(stats_summary const& summary)
{
    if (summary.rows.empty())
        return dim("no instructions recorded") + "\n";

    row_cells header;
    header.self = "self";
    header.atomics = "atomics";
    header.slow = "slow";
    header.calls = "calls d/i";
    header.memory = "mem r/w";
    header.branches = "br (taken)";
    header.symbol = "symbol";

    auto const total = total_of(summary.rows, summary.traces);

    column_widths w;
    widen(w, header);
    widen(w, total);

    cc::vector<row_cells> cells;
    for (auto const& r : summary.rows)
    {
        cells.push_back(cells_of(r));
        widen(w, cells.back());
    }

    cc::string out;
    out += dim(row_line(header, w)) + "\n";

    for (auto const& c : cells)
        out += row_line(c, w) + "\n";

    // A rule the width of the numeric columns, so the totals row reads as a sum rather than a symbol.
    auto const rule_width = 2 + w.self + 2 + w.atomics + 2 + w.slow + 2 + w.calls + 2 + w.memory + 2 + w.branches;
    cc::string rule;
    for (isize i = 0; i < rule_width; ++i)
        rule += i < 2 ? ' ' : '-';
    out += dim(rule) + "\n";

    out += row_line(total, w) + "\n";

    out += format_slow_ops(summary.slow_ops);

    if (summary.truncated)
        out += "\n" + yellow("TRUNCATED: a trace hit --instructions; these counts are incomplete.") + "\n";

    return out;
}
} // namespace itrace
