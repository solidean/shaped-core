#include "diff.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shader-analysis/cost.hh>
#include <shader-analysis/metrics.hh>

namespace shan
{
f64 difference::ratio() const
{
    if (b == 0)
        return a == 0 ? 1.0 : cc::bit_cast<f64>(u64(0x7ff0000000000000));
    return f64(a) / f64(b);
}

cc::vector<difference> diff(module_metrics const& a, module_metrics const& b)
{
    auto out = cc::vector<difference>();
    auto const add = [&](cc::string name, i64 x, i64 y)
    {
        if (x != y)
            out.push_back({.name = cc::move(name), .a = x, .b = y});
    };
    auto const& x = a.total;
    auto const& y = b.total;
    add("instructions", x.instructions, y.instructions);
    add("blocks", x.blocks, y.blocks);
    add("loops", x.loops, y.loops);
    add("selections", x.selections, y.selections);
    add("calls", x.calls, y.calls);
    add("variable_words", x.variable_words, y.variable_words);
    add("compute_cost", i64(compute_cost(x, default_cost_model()) + 0.5),
        i64(compute_cost(y, default_cost_model()) + 0.5));
    add("max_live_words", x.max_live_words, y.max_live_words);
    add("max_live_vector_words", x.max_live_vector_words, y.max_live_vector_words);
    add("max_live_scalar_words", x.max_live_scalar_words, y.max_live_scalar_words);
    add("critical_path", x.critical_path, y.critical_path);
    for (auto k = isize(0); k < k_op_kind_count; ++k)
        add(cc::format("kind.{}", name_of(op_kind(k))), x.kinds[k], y.kinds[k]);

    // the size of a difference relative to the larger side, so a metric that doubles outranks one that grew by a tenth
    auto const weight = [](difference const& d)
    {
        auto const magnitude = [](i64 v) { return v < 0 ? -v : v; };
        auto const most = cc::max(magnitude(d.a), magnitude(d.b));
        return most == 0 ? 0.0 : f64(magnitude(d.a - d.b)) / f64(most);
    };
    cc::sort(out, [&](difference const& l, difference const& r) { return weight(l) > weight(r); });
    return out;
}

cc::string to_text(cc::span<difference const> differences)
{
    auto out = cc::string();
    for (auto const& d : differences)
        out.appendf("{:<28} {:>8} {:>8}  {:.3f}\n", d.name, d.a, d.b, d.ratio());
    return out;
}
} // namespace shan
