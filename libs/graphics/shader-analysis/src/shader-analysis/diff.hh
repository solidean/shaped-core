#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shader-analysis/fwd.hh>

// Two modules' metrics side by side: what a native module and DXC's of the same entry point differ in, worst first.

/// One metric of the totals, in two modules.
struct shan::difference
{
    /// `instructions`, `max_live_words`, `kind.load`.
    cc::string name;
    i64 a = 0;
    i64 b = 0;

    /// `a` over `b`, 1 where both are 0, and infinite where only `b` is.
    [[nodiscard]] f64 ratio() const;
};

namespace shan
{
/// Every metric of the totals in which `a` and `b` differ, the largest relative difference first.
/// A count both modules leave at zero is left out.
[[nodiscard]] cc::vector<difference> diff(module_metrics const& a, module_metrics const& b);

/// The differences as text, one line each: the name, both values, and their ratio.
[[nodiscard]] cc::string to_text(cc::span<difference const> differences);
} // namespace shan
