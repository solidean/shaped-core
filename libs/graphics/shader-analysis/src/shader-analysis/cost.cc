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

cost_model const& default_cost_model()
{
    static auto const model = []
    {
        auto m = cost_model{.name = "documented rates"};
        m.per_lane[isize(cost_class::alu)] = 1.0;
        m.per_lane[isize(cost_class::int_multiply)] = 4.0;
        m.per_lane[isize(cost_class::int_divide)] = 48.0;
        m.per_lane[isize(cost_class::conversion)] = 1.0;
        m.per_lane[isize(cost_class::transcendental)] = 4.0;
        m.per_lane[isize(cost_class::control)] = 1.0;
        return m;
    }();
    return model;
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
