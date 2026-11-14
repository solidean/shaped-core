#include "register.hh"

using namespace sgl;
using namespace sgl::builtins;

// PROTOTYPE (libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md): how a derivative passes through
// each builtin that gives a float and takes one, stated by name or by operator, for every overload of it.
// A builtin added later that can carry a derivative is `unstated` until it is listed here, which a registry test refuses.

namespace
{
struct stated
{
    cc::string_view name;
    derivative_kind kind;
};

constexpr derivative_kind linear = derivative_kind::linear;
constexpr derivative_kind rule = derivative_kind::rule;
constexpr derivative_kind zero = derivative_kind::zero;
constexpr derivative_kind none = derivative_kind::none;
constexpr derivative_kind forward_only = derivative_kind::forward_only;

// an operator by its spelling, every other builtin by its name
constexpr stated k_stated[] = {
    // the tangent is the call of the tangents, and the transpose is a call of the same shape
    {"+", linear},
    {"-", linear},
    {"as", linear},
    {"float", linear},
    {"float2", linear},
    {"float3", linear},
    {"float4", linear},
    {"half", linear},
    {"half2", linear},
    {"half3", linear},
    {"half4", linear},
    {"vec3", linear},
    {"pos3", linear},
    {"hpos4", linear},
    {"mat2", linear},
    {"mat3", linear},
    {"mat4", linear},
    {"transpose", linear},
    {"matrix_column", linear},
    {"matrix_row", linear},
    {"select", linear},
    {"affine3_of_rows", linear},
    {"affine3_row", linear},
    // a rule of the differentiator's own
    {"*", rule},
    {"/", rule},
    {"dot", rule},
    {"cross", rule},
    {"length", rule},
    {"distance", rule},
    {"normalize", rule},
    {"sin", rule},
    {"cos", rule},
    {"tan", rule},
    {"sinh", rule},
    {"cosh", rule},
    {"tanh", rule},
    {"asin", rule},
    {"acos", rule},
    {"atan", rule},
    {"atan2", rule},
    {"exp", rule},
    {"exp2", rule},
    {"log", rule},
    {"log2", rule},
    {"pow", rule},
    {"sqrt", rule},
    {"inverse_sqrt", rule},
    {"abs", rule},
    {"fract", rule},
    {"saturate", rule},
    {"min", rule},
    {"max", rule},
    {"clamp", rule},
    {"mix", rule},
    {"fma", rule},
    {"%", rule},
    {"smoothstep", rule},
    {"reflect", rule},
    {"sample", rule},
    // constant between the points where it jumps
    {"floor", zero},
    {"ceil", zero},
    {"round", zero},
    {"trunc", zero},
    {"sign", zero},
    {"step", zero},
    // what it gives is no function of its arguments: what another thread did, what the workgroup stored
    {"add", none},
    {"exchange", none},
    {"workgroup_uniform_load", none},
    // a rule of the differentiator's own, past the arithmetic
    {"determinant", rule},
    {"refract", rule},
    {"gather", rule},
    {"sample_compare", rule},
    {"subgroup_min", rule},
    {"subgroup_max", rule},
    {"subgroup_mul", rule},
    {"subgroup_inclusive_mul", rule},
    {"subgroup_exclusive_mul", rule},
    // linear across the subgroup, each with a transpose of its own
    {"subgroup_add", linear},
    {"subgroup_inclusive_add", linear},
    {"subgroup_exclusive_add", linear},
    {"subgroup_broadcast_first", linear},
    {"subgroup_shuffle_xor", linear},
    {"quad_swap_x", linear},
    {"quad_swap_y", linear},
    {"quad_swap_diagonal", linear},
    // a comparison's result is constant between the points where it jumps
    {"gather_compare", zero},
    // going forward alone: the transpose needs which lane each invocation is
    {"ddx", forward_only},
    {"ddy", forward_only},
    {"ddx_coarse", forward_only},
    {"ddy_coarse", forward_only},
    {"ddx_fine", forward_only},
    {"ddy_fine", forward_only},
    {"fwidth", forward_only},
    {"quad_broadcast", forward_only},
    {"subgroup_broadcast", forward_only},
    {"subgroup_shuffle", forward_only},
    {"subgroup_shuffle_up", forward_only},
    {"subgroup_shuffle_down", forward_only},
};

/// `@operator("+")`'s spelling, or empty.
cc::string_view operator_of(function_record const& f)
{
    auto const at = f.signature.find("@operator(\"");
    if (at < 0)
        return {};
    auto const rest = cc::string_view(f.signature).subview(at + 11);
    auto const end = rest.find('"');
    return end < 0 ? cc::string_view() : rest.subview({.offset = 0, .size = end});
}

bool is_float_type(registry const& r, cc::string_view spelled)
{
    auto const id = r.find_type(spelled);
    if (!is_valid(id))
        return false;
    auto const kind = r.at(id).leaf_kind;
    return kind == check::value_kind::scalar_float || kind == check::value_kind::scalar_half;
}
} // namespace

bool sgl::builtins::carries_derivative(registry const& r, function_record const& f)
{
    if (!is_valid(f.result)
        || (r.at(f.result).leaf_kind != check::value_kind::scalar_float
            && r.at(f.result).leaf_kind != check::value_kind::scalar_half))
        return false;
    for (auto const& p : f.parameters)
        if (is_float_type(r, p.starts_with("mut ") ? cc::string_view(p).subview(4) : cc::string_view(p)))
            return true;
    return false;
}

void sgl::builtins::state_derivatives(registry& r)
{
    for (auto& f : r.functions)
    {
        auto const op = operator_of(f);
        auto const key = op.empty() ? cc::string_view(f.name) : op;
        for (auto const& s : k_stated)
            if (s.name == key)
                f.derivative = s.kind;
    }
}
