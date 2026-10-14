#pragma once

#include <nexus/fwd.hh>

// What nx::run prints and guards around the tests it runs, beyond their own output.

namespace nx::impl
{
struct run_framing
{
    /// The `nexus: run seed N` line a run starts with.
    bool prints_seed = false;

    /// The `host` line before the tests and the `load` line after them, which a benchmark's numbers are a statement about.
    bool describes_host = false;

    /// The CPU and RAM line after the result.
    bool reports_resources = false;

    /// Whether the hang watchdog may watch the run at all.
    /// A connected debugger rules it out as well, which is the caller's to check.
    bool is_watched = false;
};

/// Keyed on what `schedule` holds as well as on the bucket `config` swept.
/// An example or a benchmark named exactly is reached from the normal sweep, without its bucket flag, and is framed as that bucket's sweep frames it all the same.
[[nodiscard]] run_framing frame_run(test_schedule_config const& config, test_schedule const& schedule, bool is_entry_run);
} // namespace nx::impl
