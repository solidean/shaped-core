#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// What one symbol did, summed over every recorded trace.
///
/// Self, not cumulative: each instruction is charged to the function containing its rip, so a callee's
/// work never lands on its caller. Needs `owner_symbol`, i.e. enrichment run with want_owner.
struct symbol_stats
{
    cc::string symbol;
    u32 instructions = 0;
    /// Locked RMWs — the column with the highest instruction-to-cycle ratio.
    u32 atomics = 0;
    /// Instructions that are not single-cycle (idiv, rdtsc, a fence, …). Usually 0; when it is not,
    /// stats_summary::slow_ops names them.
    u32 slow = 0;
    u32 direct_calls = 0;
    /// Through a register or memory: a vtable, function_ref or unique_function hop.
    u32 indirect_calls = 0;
    u32 memory_reads = 0;
    u32 memory_writes = 0;
    /// Conditional branches only, and how many actually went the other way.
    u32 branches = 0;
    u32 branches_taken = 0;
};

/// One kind of expensive instruction, and the symbol it ran in.
///
/// Named rather than merely counted because the bag is heterogeneous: "3" says nothing, while
/// "idiv x2" says where to look. Rare enough that it is worth a line each.
struct slow_op
{
    cc::string mnemonic;
    cc::string symbol;
    u32 count = 0;
};

/// Per-symbol rows plus what the reader needs to judge them.
struct stats_summary
{
    /// Sorted by instructions descending, then by symbol for a stable table.
    cc::vector<symbol_stats> rows;
    /// Every expensive instruction encountered, by count descending. Empty is the normal case — and
    /// is itself a result: it means the instruction count is a fair proxy for cost here.
    cc::vector<slow_op> slow_ops;
    u32 traces = 0;
    /// Some trace hit --instructions, so the counts below it are incomplete.
    bool truncated = false;
};

/// Bucket every instruction of every trace by its containing symbol.
stats_summary collect_stats(cc::span<trace const> traces);

/// The table: one row per symbol, a totals row, and a warning when the counts are truncated.
cc::string format_stats(stats_summary const& summary);

/// "foo<int>::bar<T>" -> "foo::bar". Real symbol names run to 300+ chars, almost all of it template
/// arguments. Returns `name` unchanged when the angle brackets do not balance, rather than mangling it.
cc::string strip_template_args(cc::string_view name);
} // namespace itrace
