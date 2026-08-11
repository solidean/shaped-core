#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/fwd.hh>
#include <instruction-tracer/report/memory_formatter.hh>

namespace itrace
{
struct memory_stats_summary;
struct memory_symbol_stats;
} // namespace itrace

/// What one symbol's code touched in memory, summed over every trace.
/// Grouped by the function *making* the accesses (the instruction's owner), like the instruction table.
/// So it answers "which function moves how much memory", not "which data was hit".
struct itrace::memory_symbol_stats
{
    cc::string symbol;
    u32 accesses = 0;
    u32 reads = 0;
    u32 writes = 0;
    /// Distinct cachelines touched — a working-set proxy.
    /// An access spanning two lines counts both.
    u32 cachelines = 0;
    /// Bytes moved (traffic): the sum of access sizes, so a location hit twice counts twice.
    u64 bytes = 0;
};

/// Per-symbol rows plus a totals row.
///
/// `total.cachelines` is the distinct-line union across every symbol, not the sum of the rows' `cachelines` — two functions touching one line count it once here and once each above.
/// The other totals are plain column sums.
struct itrace::memory_stats_summary
{
    cc::vector<memory_symbol_stats> rows; // sorted by accesses descending, then symbol
    memory_symbol_stats total;
    u32 traces = 0;
    bool truncated = false;
};

namespace itrace
{

/// Bucket every included memory access by its instruction's containing symbol.
memory_stats_summary collect_memory_stats(cc::span<trace const> traces, memory_view_options const& opts);

/// The table: one row per symbol, a totals row, and a warning when the counts are truncated.
cc::string format_memory_stats(memory_stats_summary const& summary);
} // namespace itrace
