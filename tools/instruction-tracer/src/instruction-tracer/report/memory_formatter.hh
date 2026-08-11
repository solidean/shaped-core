#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/debug/trace_record.hh>
#include <instruction-tracer/fwd.hh>

namespace itrace
{
struct memory_view_options;
} // namespace itrace

namespace itrace
{
/// The 64-byte cacheline the whole tool assumes.
/// The touched-byte footprint mask is one bit per byte of it, so this is fixed at 64 rather than configurable.
inline constexpr u32 cacheline_bytes = 64;

} // namespace itrace

/// Which regions to show, and whether to annotate with the accessing instruction.
/// Mirrors the `memory_regions` CLI struct, kept separate so report/ does not depend on cli/.
struct itrace::memory_view_options
{
    bool heap = true;
    bool frame = false;
    bool stack = true;
    bool instructions = false;
    bool instruction_addresses = false;

    bool includes(access_region region) const;
};

namespace itrace
{

/// The raw chronological access list: one line per touched location, in execution order, per trace.
/// Each line is address, size, read/write, region and the symbol it hit — a global, or the function owning the stack frame.
/// With `instruction_addresses`, the accessing rip is prefixed.
cc::string format_memory_raw(cc::span<trace const> traces, memory_view_options const& opts);

/// Accesses bucketed by cacheline, aggregated over every trace: one line per touched cacheline in ascending address order, a blank line where two are not adjacent.
/// Shows how many accesses hit the line, how many of its 64 bytes (the footprint), read/write, and the distinct symbols it covers, comma-separated and truncated.
cc::string format_memory_cachelines(cc::span<trace const> traces, memory_view_options const& opts);
} // namespace itrace
