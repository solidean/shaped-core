#pragma once

#include "probe.hh"

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>

// Turning measurements into something a person reads and something a person sends back.

namespace lod_probe
{
/// The tables for one adapter: each context against the specification, and where there are two, one against the other.
[[nodiscard]] cc::string text_report(cc::span<context_result const> contexts, cc::span<tile const> tiles);

/// Every tile of every pass of every context, for a machine to read.
[[nodiscard]] cc::string json_report(cc::span<context_result const> contexts, cc::span<tile const> tiles);
} // namespace lod_probe
