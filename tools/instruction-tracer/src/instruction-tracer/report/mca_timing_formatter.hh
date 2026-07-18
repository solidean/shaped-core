#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>

namespace itrace
{
struct trace;
struct mca_result;

/// Render the llvm-mca timing section for the terminal: per trace, a block summary, the bottleneck
/// breakdown with the limiting port(s), and a compact per-instruction line (uops / latency / retire
/// cycle). Secondary to the HTML report — terse by design. `mca[i]` corresponds to `traces[i]`;
/// unavailable or misaligned analyses degrade to a note rather than wrong numbers.
cc::string format_mca_timing(cc::span<trace const> traces, cc::span<mca_result const> mca);
} // namespace itrace
