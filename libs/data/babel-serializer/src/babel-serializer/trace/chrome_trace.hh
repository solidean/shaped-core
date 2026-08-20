#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/streams/stream.hh> // cc::write_stream
#include <clean-core/string/string_view.hh>

// The Chrome Trace Event format, written from a cc::rec::recording.
//
// This is the first thing that lets anyone LOOK at a recording rather than assert on one.
// The output opens in chrome://tracing and in ui.perfetto.dev, neither of which needs anything installed.
//
// Read-side support is not here and is not planned: we write traces for other tools to read, and read our own
// recordings through cc::rec.
//
// The mapping, and what it costs:
//   scope_begin / scope_end  -> "B" / "E", so a scope that never closed still renders instead of being dropped
//   marker, value, log       -> "i" instant events on the recording thread, with the payload in `args`
//   stat_snapshot            -> "C" counter, carrying the reading
//   stat_accumulate          -> "C" counter, carrying the RUNNING TOTAL, since a counter track shows a level
//   gap, dropped_span        -> "i", when include_system_events is on: what the recorder knows it lost
//   a domain                 -> the event's `cat`, so Perfetto's category filter is per library

/// How a recording is rendered.
/// Every knob is about what to include; the mapping itself is not configurable.
struct babel::chrome_trace::write_options
{
    /// What the trace calls the process.
    /// Only one is emitted — cc::rec is in-process by design.
    i32 process_id = 1;
    cc::string_view process_name = "shaped-core";

    /// Include the recorder's own bookkeeping: gaps, chunk acquisition, decimated spans.
    /// Off by default because it is noise until you are asking about the recorder itself, and revealing when you are.
    bool include_system_events = false;

    bool include_logs = true;
    bool include_values = true;
    bool include_stats = true;
    bool include_scopes = true;

    /// One event per line.
    /// Costs bytes, and makes a diff or a grep possible.
    bool pretty = true;
};

namespace babel::chrome_trace
{
/// Renders `recording` as Chrome Trace Event JSON.
///
/// Timestamps are microseconds relative to the EARLIEST event in the recording, not since the epoch.
/// Absolute time would cost most of a double's precision for a number no viewer displays.
[[nodiscard]] cc::result<cc::vector<byte>> encode(cc::rec::recording const& recording, write_options opts = {});

/// encode, then write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out,
                                         cc::rec::recording const& recording,
                                         write_options opts = {});
} // namespace babel::chrome_trace
