#include <clean-core/container/fixed_array.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/driver/test_source.hh>

// PROTOTYPE (libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md): the registry states how a
// derivative passes through each builtin (builtins/register_derivatives.cc), and the differentiator holds to it.

using namespace cc::primitive_defines;
namespace builtins = sgl::builtins;

namespace
{
/// A plain float or float vector, the types a generated call takes and gives; 0 for any other.
int width_of(cc::string_view type)
{
    if (type == "float")
        return 1;
    if (type == "float2")
        return 2;
    if (type == "float3")
        return 3;
    if (type == "float4")
        return 4;
    return 0;
}

/// A literal of `type` whose components lie in (0.2, 0.8), in every domain the rules have, and differ from each other.
cc::string value_of(cc::string_view type, int& seed)
{
    auto const component = [&]
    {
        seed = (seed * 37 + 11) % 600;
        return cc::format("0.{}", 200 + seed);
    };
    auto const width = width_of(type);
    if (width == 1)
        return component();
    auto out = cc::format("{}(", type);
    for (auto i = 0; i < width; ++i)
        out += cc::format("{}{}", i == 0 ? "" : ", ", component());
    return out + ")";
}

/// A float of `value` of `type`: itself, or its dot with fixed weights, so every component of a result counts.
cc::string reduced(cc::string_view type, cc::string_view value, cc::string_view weights)
{
    return width_of(type) == 1 ? cc::format("({}) * {}", value, weights) : cc::format("dot({}, {})", value, weights);
}

/// `name(a0, a1)`, or the operator its record spells.
cc::string call_of(builtins::function_record const& f, isize arity)
{
    auto const at = f.signature.find("@operator(\"");
    if (at >= 0)
    {
        auto const rest = cc::string_view(f.signature).subview(at + 11);
        auto const op = rest.subview({.offset = 0, .size = rest.find('"')});
        return arity == 1 ? cc::format("{}a0", op) : cc::format("a0 {} a1", op);
    }
    // a clamp whose low bound is above its high one has no value (EVAL), and every argument is positive
    if (f.name == "clamp")
        return "clamp(a0, a1, a1 + a2)";
    auto out = cc::format("{}(", f.name);
    for (auto i = isize(0); i < arity; ++i)
        out += cc::format("{}a{}", i == 0 ? "" : ", ", i);
    return out + ")";
}
} // namespace

TEST("sgl autodiff rules - every builtin that gives a float and takes one states how a derivative passes through it")
{
    auto const& r = builtins::default_registry();
    auto unstated = cc::string();
    auto counts = cc::fixed_array<int, 6>{};
    for (auto const& f : r.functions)
    {
        if (!builtins::carries_derivative(r, f))
            continue;
        ++counts[int(f.derivative)];
        if (f.derivative == builtins::derivative_kind::unstated)
            unstated += f.signature + "\n";
    }
    // a builtin added to the registry is listed in register_derivatives.cc, as a rule or as not differentiable yet
    CHECK(unstated == "");
    CHECK(counts[int(builtins::derivative_kind::rule)] > 0);
    CHECK(counts[int(builtins::derivative_kind::linear)] > 0);
}

TEST("sgl autodiff rules - every rule and every linear builtin of floats agrees with a central difference, both modes")
{
    auto const& r = builtins::default_registry();
    auto source = cc::string();
    auto count = 0;
    auto seed = 1;
    for (auto const& f : r.functions)
    {
        if (f.derivative != builtins::derivative_kind::rule && f.derivative != builtins::derivative_kind::linear)
            continue;
        if (f.signature.contains("@internal") || width_of(r.at(f.result).name) == 0 || f.parameters.empty())
            continue;
        auto is_plain = true;
        for (auto i = isize(0); i < f.parameters.size(); ++i)
            is_plain = is_plain && width_of(f.parameters[i]) > 0 && f.named_only[i].empty();
        if (!is_plain)
            continue;

        auto const arity = f.parameters.size();
        auto const result = cc::string(r.at(f.result).name);
        auto const k = count++;
        auto parameters = cc::string();
        for (auto i = isize(0); i < arity; ++i)
            parameters += cc::format("{}a{}: {}", i == 0 ? "" : ", ", i, f.parameters[i]);
        source += cc::format("// {}\nfun f{}({}) -> {} => {}\n", f.signature, k, parameters, result, call_of(f, arity));

        auto const weights = value_of(result, seed);
        source += cc::format("fun agrees{}() -> bool:\n    let h = 1e-3\n", k);
        auto plus = cc::string();
        auto minus = cc::string();
        auto primals = cc::string();
        auto forward = cc::string();
        auto backward = cc::string();
        for (auto i = isize(0); i < arity; ++i)
        {
            source += cc::format("    let x{0} = {1}\n    let d{0} = {2}\n    let mut g{0} = {3}(0.0)\n", i,
                                 value_of(f.parameters[i], seed), value_of(f.parameters[i], seed), f.parameters[i]);
            auto const comma = i == 0 ? "" : ", ";
            plus += cc::format("{}x{} + d{} * h", comma, i, i);
            minus += cc::format("{}x{} - d{} * h", comma, i, i);
            primals += cc::format("{}x{}, mut g{}", comma, i, i);
            forward += cc::format("{}x{}, d{}", comma, i, i);
            backward += cc::format(
                "{}{}", i == 0 ? "" : " + ",
                width_of(f.parameters[i]) == 1 ? cc::format("g{} * d{}", i, i) : cc::format("dot(g{}, d{})", i, i));
        }
        source += cc::format("    let numeric = ({} - {}) / (2.0 * h)\n",
                             reduced(result, cc::format("f{}({})", k, plus), weights),
                             reduced(result, cc::format("f{}({})", k, minus), weights));
        source += cc::format("    bwd_diff(f{}, {}, {})\n", k, primals, weights);
        source += cc::format("    let tolerance = 1e-2 * (1.0 + abs(numeric))\n");
        source += cc::format("    return nearly_equal({}, numeric, within = tolerance)\n",
                             reduced(result, cc::format("fwd_diff(f{}, {})", k, forward), weights));
        source += cc::format("        and nearly_equal({}, numeric, within = tolerance)\n", backward);
        source += cc::format("test agrees{}()\n\n", k);
    }
    auto const tested = sgl::test_source(source, "autodiff-rules.sgl");
    CHECK(tested.errors == "").dump("source", source);
    CHECK(tested.tests_passed == count);
    CHECK(count > 40);
}
