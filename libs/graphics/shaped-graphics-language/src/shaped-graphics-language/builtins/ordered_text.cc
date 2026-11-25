#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

// EMIT-NEW: a strict scope's products and smoothstep on the text route.
// DXC lowers HLSL's `mul` to chains of `FMad` even under `-Gis`, which NVIDIA evaluates fused, and `dot` to DXIL's
// `dot` operation and SPIR-V's `OpDot`, whose order of summation is the driver's; Metal's are its own as well.
// A product written out as `*` and `+` stays the separate operations it names, so the helpers below write each one.

namespace
{
constexpr cc::string_view k_product_hlsl[] = {"mul", "sgl_ordered_mul"};
constexpr cc::string_view k_product_msl[] = {"sgl_ordered_mul"};
constexpr cc::string_view k_dot[] = {"dot", "sgl_ordered_dot"};
constexpr cc::string_view k_smoothstep[] = {"smoothstep", "saturate", "sgl_ordered_smoothstep"};

/// A type as the target spells it: `float3x4` is float, 3 and 4, `half3` is half, 3 and 0, `float` is float, 1 and 0.
struct spelled_shape
{
    cc::string_view scalar;
    int first = 1;
    int second = 0;
};

[[nodiscard]] spelled_shape shape_of(cc::string_view type)
{
    auto at = isize(0);
    while (at < type.size() && (type[at] < '0' || type[at] > '9'))
        ++at;
    auto shape = spelled_shape{.scalar = type.subview({.start = 0, .end = at})};
    if (at < type.size())
        shape.first = type[at] - '0';
    if (at + 2 < type.size() && type[at + 1] == 'x')
        shape.second = type[at + 2] - '0';
    return shape;
}

/// `((t0 + t1) + t2) + t3`, left to right.
[[nodiscard]] cc::string ordered_sum(cc::span<cc::string const> terms)
{
    auto sum = terms[0];
    for (auto k = isize(1); k < terms.size(); ++k)
        sum = cc::format("({} + {})", sum, terms[k]);
    return sum;
}

/// The vector type of `count` lanes of `scalar`, as HLSL and MSL both spell it.
[[nodiscard]] cc::string vector_of(cc::string_view scalar, int count)
{
    return count == 1 ? cc::string(scalar) : cc::format("{}{}", scalar, count);
}
} // namespace

cc::span<cc::string_view const> impl::ordered_product_names(language l)
{
    return l == language::msl ? cc::span<cc::string_view const>(k_product_msl)
                              : cc::span<cc::string_view const>(k_product_hlsl);
}

cc::span<cc::string_view const> impl::ordered_dot_names()
{
    return k_dot;
}

cc::span<cc::string_view const> impl::ordered_smoothstep_names()
{
    return k_smoothstep;
}

written impl::write_ordered_product(written const& lhs, written const& rhs)
{
    return {.text = cc::format("sgl_ordered_mul({}, {})", lhs.text, rhs.text)};
}

cc::string impl::ordered_product_helper(helper_context const& c)
{
    if (!c.is_precise || c.target == language::wgsl || c.argument_types.size() < 2)
        return {};
    auto const matrix = c.argument_types[0];
    auto const shape = shape_of(matrix);
    auto const is_msl = c.target == language::msl;
    // HLSL spells rows by columns and indexes `m[row][column]`; MSL spells columns by rows and indexes
    // `m[column][row]`, but for an affine3, whose columns are its rows, so that `v * a` is its product
    auto const is_affine_msl = is_msl && shape.first != shape.second;
    auto const rows = is_msl && !is_affine_msl ? shape.second : shape.first;
    auto const inner = is_msl && !is_affine_msl ? shape.first : shape.second;
    auto const element = [&](cc::string_view m, int row, int column)
    {
        return is_msl && !is_affine_msl ? cc::format("{}[{}][{}]", m, column, row)
                                        : cc::format("{}[{}][{}]", m, row, column);
    };
    auto terms = cc::vector<cc::string>();
    auto out = cc::string();
    if (shape_of(c.argument_types[1]).second != 0)
    {
        // a square matrix times another: column j of the product is `a` times column j of `b`
        out.appendf("{0} sgl_ordered_mul({0} a, {0} b)\n{{\n    {0} o;\n", matrix);
        for (auto j = 0; j < inner; ++j)
            for (auto r = 0; r < rows; ++r)
            {
                terms.clear();
                for (auto k = 0; k < inner; ++k)
                    terms.push_back(cc::format("{} * {}", element("a", r, k), element("b", k, j)));
                out.appendf("    {} = {};\n", element("o", r, j), ordered_sum(terms));
            }
        out += "    return o;\n}\n";
        return out;
    }
    auto const result = vector_of(shape.scalar, rows);
    out.appendf("{} sgl_ordered_mul({} m, {} v)\n{{\n    return {}(", result, matrix, vector_of(shape.scalar, inner),
                result);
    for (auto r = 0; r < rows; ++r)
    {
        terms.clear();
        for (auto k = 0; k < inner; ++k)
            terms.push_back(cc::format("{} * v[{}]", element("m", r, k), k));
        out.appendf("{}{}", r == 0 ? "" : ",\n        ", ordered_sum(terms));
    }
    out += ");\n}\n";
    return out;
}

written impl::write_ordered_dot(call_context const& c)
{
    auto const is_ordered = c.is_precise && c.target != language::wgsl;
    return {.text = cc::format("{}({}, {})", is_ordered ? "sgl_ordered_dot" : "dot", c.arguments[0].text,
                               c.arguments[1].text)};
}

cc::string impl::ordered_dot_helper(helper_context const& c)
{
    if (!c.is_precise || c.target == language::wgsl || c.argument_types.empty())
        return {};
    auto const type = c.argument_types[0];
    auto const shape = shape_of(type);
    auto terms = cc::vector<cc::string>();
    for (auto k = 0; k < shape.first; ++k)
        terms.push_back(shape.first == 1 ? cc::string("a * b") : cc::format("a[{0}] * b[{0}]", k));
    return cc::format("{} sgl_ordered_dot({} a, {} b)\n{{\n    return {};\n}}\n", shape.scalar, type, type,
                      ordered_sum(terms));
}

written impl::write_ordered_smoothstep(call_context const& c)
{
    auto const is_ordered = c.is_precise && c.target != language::wgsl;
    return {.text = cc::format("{}({}, {}, {})", is_ordered ? "sgl_ordered_smoothstep" : "smoothstep",
                               c.arguments[0].text, c.arguments[1].text, c.arguments[2].text)};
}

cc::string impl::ordered_smoothstep_helper(helper_context const& c)
{
    if (!c.is_precise || c.target == language::wgsl || c.argument_types.empty())
        return {};
    auto const type = c.argument_types[0];
    auto const scalar = shape_of(type).scalar;
    // t * (t * (3 - 2t)), DXC's grouping, which SGL's own DXIL and SPIR-V write too: see `lowering::strict_form`
    return cc::format("{0} sgl_ordered_smoothstep({0} low, {0} high, {0} x)\n"
                      "{{\n"
                      "    {0} t = saturate((x - low) / (high - low));\n"
                      "    return t * (t * (({1})3.0 - ({1})2.0 * t));\n"
                      "}}\n",
                      type, scalar);
}
