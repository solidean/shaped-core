#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/cli/target_spec.hh>
#include <instruction-tracer/report/console.hh>

namespace itrace
{
/// What --instructions defaults to when a table/memory section is requested, absent an explicit
/// value: a truncated trace makes for silently wrong aggregates, and 100 would truncate anything
/// worth tabling.
inline constexpr u32 stats_instruction_default = 100000;

/// Which output sections to print, in any combination — all from one capture, since the memory
/// data cannot be reliably reproduced across runs. Empty (nothing selected) means the trace alone.
struct output_sections
{
    bool trace = false;        // the raw retired-instruction trace
    bool stats = false;        // the per-symbol instruction table
    bool memory = false;       // the raw chronological memory-access list
    bool cachelines = false;   // memory accesses bucketed by cacheline
    bool memory_stats = false; // the per-symbol memory table

    bool any_memory() const { return memory || cachelines || memory_stats; }
    bool any_non_trace() const { return stats || memory || cachelines || memory_stats; }
    bool none() const { return !trace && !stats && !memory && !cachelines && !memory_stats; }
};

/// Which address regions the memory sections include. Default heap + stack: the data accesses that
/// answer "am I touching my data well", without the current frame's own spills or the code stream.
struct memory_regions
{
    bool heap = true;
    bool frame = false;
    bool stack = true;
    bool instructions = false;
};

/// Parsed command line. See usage_text() for the flags and readme.md for what they mean.
struct options
{
    cc::string exe;
    target_spec target;
    /// Everything after `--`, passed to the debuggee verbatim.
    cc::vector<cc::string> target_args;

    u64 skip = 0;   // entry hits to ignore before the first recorded trace
    u32 traces = 1; // invocations to record, across all threads
    /// Max retired instructions per trace. Defaults to stats_instruction_default under --stats: a
    /// truncated trace silently produces a wrong table, and 100 would truncate anything interesting.
    u32 instructions = 100;

    bool until_return = true;
    bool stop_at_syscall = true;
    bool stack = true;
    bool source = true;
    bool terminate_after_traces = true;
    bool register_diffs = false;

    /// Which output sections to print. Empty after parsing means "trace only" (the default).
    output_sections sections;
    /// Which address regions the memory sections show.
    memory_regions regions;
    /// Annotate the raw and cacheline memory views with the accessing instruction's address.
    bool memory_instruction_addresses = false;

    /// --colored / --plain; auto-detects otherwise.
    color_mode color = color_mode::automatic;

    /// Set by --help; main prints usage and exits 0.
    bool help = false;
};

/// Parse argv (including argv[0], which is ignored). Every boolean flag has a `--no-<flag>` form.
/// Fails on an unknown flag, a missing or malformed value, or a missing --exe / target.
cc::result<options> parse_options(cc::span<char const* const> args);

cc::string_view usage_text();
} // namespace itrace
