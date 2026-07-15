#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/cli/target_spec.hh>
#include <instruction-tracer/report/console.hh>

namespace itrace
{
/// Parsed command line. See usage_text() for the flags and readme.md for what they mean.
struct options
{
    cc::string exe;
    target_spec target;
    /// Everything after `--`, passed to the debuggee verbatim.
    cc::vector<cc::string> target_args;

    u64 skip = 0;           // entry hits to ignore before the first recorded trace
    u32 traces = 1;         // invocations to record, across all threads
    u32 instructions = 100; // max retired instructions per trace

    bool until_return = true;
    bool stop_at_syscall = true;
    bool stack = true;
    bool source = true;
    bool terminate_after_traces = true;
    bool register_diffs = false;

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
