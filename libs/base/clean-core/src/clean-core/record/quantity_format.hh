#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/string/string.hh>

/// Rendering a recorded number the way its unit says it should read.
///
/// cc::rec::unit has carried `symbol`, `prefix_base`, `singular` and `plural` since it was written, and nothing has
/// ever formatted a value through them — the fields were serialized, round-tripped and asserted on, and never used.
/// This is what cashes that promise, so a listener can print a stat it has never heard of and get "1.50 GiB" rather
/// than "1610612736".
///
/// The generic rule is the whole of it: pick the prefix from `prefix_base`, scale, append `symbol`, or fall back to the
/// unit's own name where there is no symbol.
/// A unit whose rendering that rule gets wrong sets `unit::format` instead — see there for what does not survive
/// serialization.

namespace cc::rec
{
/// The value as its unit says it reads: "1.50 GiB", "5.00 ms", "3.20 GHz", "42 items".
[[nodiscard]] cc::string format_quantity(f64 value, rec::unit const& u);

/// The same, appended to an existing string, for a listener assembling a line.
void format_quantity_to(cc::string& out, f64 value, rec::unit const& u);
} // namespace cc::rec
