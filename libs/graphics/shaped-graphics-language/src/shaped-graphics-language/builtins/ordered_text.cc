#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

// EMIT-281: a strict scope's products and smoothstep on the text route.
// DXC lowers HLSL's `mul` to chains of `FMad` even under `-Gis`, which NVIDIA evaluates fused, and `dot` to DXIL's
// `dot` operation and SPIR-V's `OpDot`, whose order of summation is the driver's; Metal's are its own as well.
// A product written out as `*` and `+` stays the separate operations it names, so the helpers below write each one.

namespace
{
constexpr cc::string_view k_product_hlsl[] = {"mul", "sgl_ordered_mul"};
constexpr cc::string_view k_product_msl[] = {"sgl_ordered_mul"};
constexpr cc::string_view k_dot[] = {"dot", "sgl_ordered_dot"};
constexpr cc::string_view k_dot_wgsl[] = {"dot"};
constexpr cc::string_view k_smoothstep[] = {"smoothstep", "saturate", "sgl_ordered_smoothstep"};
constexpr cc::string_view k_smoothstep_wgsl[] = {"smoothstep"};

/// A type as the target spells it: `float3x4` is float, 3 and 4, `half3` is half, 3 and 0, `float` is float, 1 and 0.
struct spelled_shape
{
    cc::string_view scalar;
    int first = 1;
    int second = 0;
};

/// Read from the end, since a scalar's own name may hold digits: HLSL spells a half vector `float16_t3`.
[[nodiscard]] spelled_shape shape_of(cc::string_view type)
{
    auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto const n = type.size();
    if (n >= 3 && type[n - 2] == 'x' && is_digit(type[n - 3]) && is_digit(type[n - 1]))
        return {.scalar = type.subview({.start = 0, .end = n - 3}),
                .first = type[n - 3] - '0',
                .second = type[n - 1] - '0'};
    if (n >= 2 && is_digit(type[n - 1]) && !is_digit(type[n - 2]))
        return {.scalar = type.subview({.start = 0, .end = n - 1}), .first = type[n - 1] - '0'};
    return {.scalar = type};
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

cc::span<cc::string_view const> impl::ordered_dot_names(language l)
{
    return l == language::wgsl ? cc::span<cc::string_view const>(k_dot_wgsl) : cc::span<cc::string_view const>(k_dot);
}

cc::span<cc::string_view const> impl::ordered_smoothstep_names(language l)
{
    return l == language::wgsl ? cc::span<cc::string_view const>(k_smoothstep_wgsl)
                               : cc::span<cc::string_view const>(k_smoothstep);
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

namespace
{
/// The geometry and blend that hide a dot or a sum, in `impl::ordered_call`'s order.
struct ordered_call_names
{
    cc::string_view helper;
    /// The target's own call, in HLSL and in MSL and WGSL.
    cc::string_view hlsl;
    cc::string_view other;
};

constexpr ordered_call_names k_ordered_calls[] = {
    {"sgl_ordered_length", "length", "length"},
    {"sgl_ordered_distance", "distance", "distance"},
    {"sgl_ordered_normalize", "normalize", "normalize"},
    {"sgl_ordered_reflect", "reflect", "reflect"},
    {"sgl_ordered_refract", "refract", "refract"},
    {"sgl_ordered_mix", "lerp", "mix"},
    {"sgl_ordered_cross", "cross", "cross"},
};

/// The names each language's text calls or declares: HLSL calls `mix` `lerp`, and WGSL writes no helper.
constexpr cc::string_view k_ordered_call_hlsl[] = {
    "length",
    "distance",
    "normalize",
    "reflect",
    "refract",
    "lerp",
    "sqrt",
    "sgl_ordered_length",
    "sgl_ordered_distance",
    "sgl_ordered_normalize",
    "sgl_ordered_reflect",
    "sgl_ordered_refract",
    "sgl_ordered_mix",
    "cross",
    "sgl_ordered_cross",
};
constexpr cc::string_view k_ordered_call_msl[] = {
    "length",
    "distance",
    "normalize",
    "reflect",
    "refract",
    "mix",
    "sqrt",
    "sgl_ordered_length",
    "sgl_ordered_distance",
    "sgl_ordered_normalize",
    "sgl_ordered_reflect",
    "sgl_ordered_refract",
    "sgl_ordered_mix",
    "cross",
    "sgl_ordered_cross",
};
constexpr cc::string_view k_ordered_call_wgsl[]
    = {"length", "distance", "normalize", "reflect", "refract", "mix", "cross"};

/// The ordered dot of `a` and `b`, `lanes` lanes each.
[[nodiscard]] cc::string dot_text(cc::string_view a, cc::string_view b, int lanes)
{
    auto terms = cc::vector<cc::string>();
    for (auto k = 0; k < lanes; ++k)
        terms.push_back(cc::format("{0}[{2}] * {1}[{2}]", a, b, k));
    return ordered_sum(terms);
}

written write_ordered_call(call_context const& c)
{
    auto const& names = k_ordered_calls[c.data];
    auto const name = c.is_precise && c.target != language::wgsl ? names.helper
                    : c.target == language::hlsl                 ? names.hlsl
                                                                 : names.other;
    auto text = cc::format("{}(", name);
    for (auto i = isize(0); i < c.arguments.size(); ++i)
        text.appendf("{}{}", i == 0 ? "" : ", ", c.arguments[i].text);
    text += ")";
    return {.text = cc::move(text)};
}

/// Each form is the one SGL's own DXIL and SPIR-V lower to, and why each was chosen is said there,
/// in `lowering::strict_math`.
cc::string ordered_call_helper(helper_context const& c)
{
    if (!c.is_precise || c.target == language::wgsl || c.argument_types.empty())
        return {};
    auto const type = c.argument_types[0];
    auto const shape = shape_of(type);
    auto const scalar = shape.scalar;
    auto const lanes = shape.first;
    switch (impl::ordered_call(c.data))
    {
    case impl::ordered_call::length:
        return cc::format("{} sgl_ordered_length({} v)\n{{\n    return sqrt({});\n}}\n", scalar, type,
                          dot_text("v", "v", lanes));
    case impl::ordered_call::distance:
        return cc::format("{0} sgl_ordered_distance({1} a, {1} b)\n{{\n    {1} d = a - b;\n    return sqrt({2});\n}}\n",
                          scalar, type, dot_text("d", "d", lanes));
    case impl::ordered_call::normalize:
        return cc::format("{1} sgl_ordered_normalize({1} v)\n{{\n    {0} s = ({0})1.0 / sqrt({2});\n    return v * "
                          "s;\n}}\n",
                          scalar, type, dot_text("v", "v", lanes));
    case impl::ordered_call::reflect:
        return cc::format("{1} sgl_ordered_reflect({1} i, {1} n)\n{{\n    {0} k = ({0})2.0 * {2};\n    return i - n * "
                          "k;\n}}\n",
                          scalar, type, dot_text("n", "i", lanes));
    case impl::ordered_call::refract:
    {
        if (c.argument_types.size() < 3)
            return {};
        auto const zero = c.target == language::hlsl ? cc::format("({})0.0", type) : cc::format("{}(0.0)", type);
        return cc::format("{1} sgl_ordered_refract({1} i, {1} n, {2} eta)\n"
                          "{{\n"
                          "    {0} d = {3};\n"
                          "    {0} k = ({0})1.0 - (eta * eta) * (({0})1.0 - d * d);\n"
                          "    return k >= ({0})0.0 ? eta * i - (eta * d + sqrt(k)) * n : {4};\n"
                          "}}\n",
                          scalar, type, c.argument_types[2], dot_text("n", "i", lanes), zero);
    }
    case impl::ordered_call::cross:
        // each lane a_p * b_q - a_q * b_p, as DXC writes `cross` and SGL's own DXIL does at every level
        return cc::format("{0} sgl_ordered_cross({0} a, {0} b)\n{{\n    return {0}(a[1] * b[2] - a[2] * b[1], a[2] * "
                          "b[0] - "
                          "a[0] * b[2], a[0] * b[1] - a[1] * b[0]);\n}}\n",
                          type);
    case impl::ordered_call::mix:
        if (c.argument_types.size() < 3)
            return {};
        return cc::format("{0} sgl_ordered_mix({0} a, {0} b, {1} t)\n{{\n    return a + t * (b - a);\n}}\n", type,
                          c.argument_types[2]);
    }
    return {};
}
} // namespace

spelling impl::ordered_spelling(ordered_call which)
{
    return {.kind = spelling_kind::custom,
            .hlsl = cc::string(k_ordered_calls[u32(which)].hlsl),
            .custom = write_ordered_call,
            .helper = ordered_call_helper,
            .data = u32(which),
            .hlsl_names = k_ordered_call_hlsl,
            .wgsl_names = k_ordered_call_wgsl,
            .msl_names = k_ordered_call_msl};
}
