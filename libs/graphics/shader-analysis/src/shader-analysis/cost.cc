#include "cost.hh"

#include "metrics.hh"

namespace shan
{
cc::string_view name_of(cost_class c)
{
    switch (c)
    {
    case cost_class::free:
        return "free";
    case cost_class::alu:
        return "alu";
    case cost_class::int_multiply:
        return "int_multiply";
    case cost_class::int_divide:
        return "int_divide";
    case cost_class::conversion:
        return "conversion";
    case cost_class::transcendental:
        return "transcendental";
    case cost_class::memory:
        return "memory";
    case cost_class::control:
        return "control";
    }
    return "?";
}

namespace
{
// `pipeline-stats throughput` on 2026-11-08: each operation's time past an empty kernel's, against an fma's
[[nodiscard]] cost_model measured(cc::string_view name, f64 int_multiply, f64 int_divide, f64 conversion, f64 transcendental)
{
    auto m = cost_model{.name = name};
    m.per_lane[isize(cost_class::alu)] = 1.0;
    m.per_lane[isize(cost_class::int_multiply)] = int_multiply;
    m.per_lane[isize(cost_class::int_divide)] = int_divide;
    m.per_lane[isize(cost_class::conversion)] = conversion;
    m.per_lane[isize(cost_class::transcendental)] = transcendental;
    // not measured: a branch as one slot
    m.per_lane[isize(cost_class::control)] = 1.0;
    return m;
}
} // namespace

cost_model const& xe2_cost_model()
{
    static auto const model = measured("Arc B580, Xe2", 7.46, 17.9, 0.85, 3.16);
    return model;
}

cost_model const& rdna2_cost_model()
{
    static auto const model = measured("Radeon iGPU, RDNA 2", 4.37, 23.0, 1.64, 3.37);
    return model;
}

cost_model const& default_cost_model()
{
    return xe2_cost_model();
}

f64 compute_cost(function_metrics const& f, cost_model const& model)
{
    auto cost = 0.0;
    for (auto c = isize(0); c < k_cost_class_count; ++c)
    {
        auto const uniform = f.executed_uniform_class_lanes[c];
        cost += model.per_lane[c] * (f.executed_class_lanes[c] - uniform + model.uniform_factor * uniform);
    }
    return cost;
}
} // namespace shan
