#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/cli/target_spec.hh>
#include <instruction-tracer/fwd.hh>

namespace itrace
{
struct memory_regions;
struct options;
struct output_sections;
} // namespace itrace

namespace itrace
{
/// What --instructions defaults to when a non-trace section or --html is on and no explicit value was given.
/// A truncated trace makes every aggregate silently wrong, and 100 would truncate anything worth tabling.
inline constexpr u32 stats_instruction_default = 100000;

} // namespace itrace

/// Which output sections to print, in any combination.
/// All come from one capture, since the memory data cannot be reliably reproduced across runs.
/// Nothing selected means the trace alone.
struct itrace::output_sections
{
    bool trace = false;        // the raw retired-instruction trace
    bool stats = false;        // the per-symbol instruction table
    bool memory = false;       // the raw chronological memory-access list
    bool cachelines = false;   // memory accesses bucketed by cacheline
    bool memory_stats = false; // the per-symbol memory table
    bool timing = false;       // the llvm-mca cost model (needs --mca)

    bool any_memory() const { return memory || cachelines || memory_stats; }
    bool any_non_trace() const { return stats || memory || cachelines || memory_stats || timing; }
    bool none() const { return !trace && !stats && !memory && !cachelines && !memory_stats && !timing; }
};

/// Which address regions the memory sections include.
/// Default heap + stack: the data accesses that answer "am I touching my data well", without the current frame's own spills or the code stream.
struct itrace::memory_regions
{
    bool heap = true;
    bool frame = false;
    bool stack = true;
    bool instructions = false;
};

/// Parsed command line.
/// See usage_text() for the flags, and tools/instruction-tracer/readme.md for what they mean.
struct itrace::options
{
    cc::string exe;
    target_spec target;
    /// Everything after `--`, passed to the debuggee verbatim.
    cc::vector<cc::string> target_args;

    u64 skip = 0;   // entry hits to ignore before the first recorded trace
    u32 traces = 1; // invocations to record, across all threads
    /// Max retired instructions per trace.
    /// Raised to stats_instruction_default where that constant's condition holds.
    u32 instructions = 100;

    bool until_return = true;
    bool stop_at_syscall = true;
    bool stack = true;
    bool source = true;
    bool terminate_after_traces = true;
    bool register_diffs = false;

    /// When set, write a self-contained HTML report to this path.
    /// Forces a full capture: source, owner, memory and register data, plus the stats instruction budget.
    /// An output format rather than a section, so it is orthogonal to --sections.
    cc::string html_path;

    /// Path to the llvm-mca binary, usually supplied by dev.py.
    /// Empty disables timing analysis: the `timing` section and the HTML timing views degrade to absent rather than failing.
    cc::string mca_tool;
    /// The µarch model for llvm-mca.
    /// Empty means host (`-mcpu=native`) with a baseline fallback.
    cc::string mca_cpu;

    /// Which output sections to print; never empty once parse_options returns successfully.
    output_sections sections;
    /// Whether --sections / --stats explicitly named a section.
    /// Lets --html choose between replacing stdout with a summary and also printing the requested sections.
    bool sections_explicit = false;
    /// Which address regions the memory sections show.
    memory_regions regions;
    /// Annotate the raw and cacheline memory views with the accessing instruction's address.
    bool memory_instruction_addresses = false;

    /// --colored / --plain; auto-detects otherwise.
    cc::console::color_mode color = cc::console::color_mode::automatic;

    /// Set by --help; main prints usage and exits 0.
    bool help = false;
};

namespace itrace
{

/// Parse argv; argv[0] is ignored.
/// Every boolean flag has a `--no-<flag>` form.
/// Fails on an unknown flag, a missing or malformed value, or a missing --exe / target.
cc::result<options, cc::string> parse_options(cc::span<char const* const> args);

cc::string_view usage_text();
} // namespace itrace
