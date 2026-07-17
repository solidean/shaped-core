#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/symbol_session.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/report/source_cache.hh>

namespace itrace
{
struct format_options
{
    bool stack = true;
    bool source = true;
    bool register_diffs = false;
};

/// Render one enriched trace: the stack at entry, then the retired instructions grouped under the
/// source lines they came from, with branch annotation derived from where control actually went.
/// Pure — everything it prints was resolved during enrichment.
cc::string format_trace(trace const& t, u32 total_traces, format_options const& opts, source_cache& sources);

/// The "symbol X is ambiguous" report, listing every candidate.
cc::string format_symbol_error(symbol_error const& error);

/// One line per instruction, without source grouping. Used by --no-source and by the tests.
cc::string format_instruction(recorded_instruction const& insn);

/// "00007ff6`11203410" — the grouped form debuggers print.
cc::string format_address(u64 address);
} // namespace itrace
