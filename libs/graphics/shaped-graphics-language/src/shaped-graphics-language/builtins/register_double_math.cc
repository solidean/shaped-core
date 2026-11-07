#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;
using namespace sgl::builtins::impl;

// CHK-458: neither DXIL nor DXC's SPIR-V has a double sqrt or rounding, and DXC narrows a double handed to its float
// intrinsics to a float without a word, so every function here is HLSL the text declares, over the arithmetic, `fma`
// and the bits of a double.
// The interpreter computes each with the same steps, every one rounded to a double, which is what EVAL-108 promises.

namespace
{
using check::scalar;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

// ---- what the interpreter computes, step for step as the HLSL below ------------------------------------------------

/// The integral part, its sign kept: what the HLSL takes off the mantissa bit by bit, and soft_trunc gives exactly.
f64 trunc_of(f64 x)
{
    return soft_trunc(x);
}
f64 floor_of(f64 x)
{
    auto const t = trunc_of(x);
    return t > x ? t - 1.0 : t;
}
f64 ceil_of(f64 x)
{
    auto const t = trunc_of(x);
    return t < x ? t + 1.0 : t;
}
/// To the nearest integer, a tie to the even one; a zero keeps the sign of `x`.
f64 round_of(f64 x)
{
    auto const t = trunc_of(x);
    auto const rest = x - t;
    auto const distance = rest < 0.0 ? -rest : rest;
    auto const away = x < 0.0 ? t - 1.0 : t + 1.0;
    if (distance > 0.5)
        return away;
    if (distance == 0.5 && trunc_of(t * 0.5) * 2.0 != t)
        return away;
    return t;
}
f64 fract_of(f64 x)
{
    return x - floor_of(x);
}
f64 sqrt_of(f64 x)
{
    return correctly_rounded_sqrt(x);
}
f64 inverse_sqrt_of(f64 x)
{
    return 1.0 / correctly_rounded_sqrt(x);
}
/// HLSL's `sign`, which is 0 for a NaN, as no comparison holds for one.
f64 sign_of(f64 x)
{
    return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0;
}
/// A NaN passes through, as a float's does; the targets are free to give 0 for it.
f64 saturate_of(f64 x)
{
    return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x;
}

template <f64 (*Fn)(f64)>
void unary(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of_double(Fn(x.as_double())));
}

template <bool IsNan>
void non_finite_each(leaves in, result& out)
{
    for (auto const& x : in)
    {
        auto const magnitude = x.wide_bits() & ~(u64(1) << 63);
        auto const infinity = u64(0x7ff0000000000000ull);
        out.push_back(scalar::of(IsNan ? magnitude > infinity : magnitude == infinity));
    }
}

/// CHK-517: a double's 64 bits as a uint2, the low word first.
void double_as_uint(leaves in, result& out)
{
    auto const bits = in[0].wide_bits();
    out.push_back(scalar::of_uint(u32(bits)));
    out.push_back(scalar::of_uint(u32(bits >> 32)));
}
/// CHK-517: the double a uint2's two words are, the low word first.
void uint_as_double(leaves in, result& out)
{
    out.push_back(scalar::of_wide(check::value_kind::scalar_double, u64(in[1].bits) << 32 | in[0].bits));
}

/// EVAL-146: `a * b + c` rounded once.
void fma(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(
            scalar::of_double(soft_fma(in[i].as_double(), in[width + i].as_double(), in[2 * width + i].as_double())));
}

/// `a * (1 - t) + b * t`, exact at both ends, with one `t` for every component.
void mix(leaves in, result& out)
{
    auto const width = (in.size() - 1) / 2;
    auto const t = in[2 * width].as_double();
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of_double(in[i].as_double() * (1.0 - t) + in[width + i].as_double() * t));
}
/// `mix` with a `t` per component.
void mix_componentwise(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const t = in[2 * width + i].as_double();
        out.push_back(scalar::of_double(in[i].as_double() * (1.0 - t) + in[width + i].as_double() * t));
    }
}

void step(leaves in, result& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of_double(in[width + i].as_double() >= in[i].as_double() ? 1.0 : 0.0));
}

void smoothstep(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const low = in[i].as_double();
        auto const high = in[width + i].as_double();
        auto const t = saturate_of((in[2 * width + i].as_double() - low) / (high - low));
        out.push_back(scalar::of_double(t * t * (3.0 - 2.0 * t)));
    }
}
cc::string_view smoothstep_undefined(leaves in)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
        if (!(in[i].as_double() < in[width + i].as_double()))
            return "smoothstep whose low edge is not below its high one";
    return {};
}

/// The products summed from the first component on, each sum rounded.
f64 dot_of(leaves a, leaves b)
{
    auto sum = a[0].as_double() * b[0].as_double();
    for (auto i = isize(1); i < a.size(); ++i)
        sum = sum + a[i].as_double() * b[i].as_double();
    return sum;
}
void dot(leaves in, result& out)
{
    auto const width = in.size() / 2;
    out.push_back(scalar::of_double(
        dot_of(in.subspan({.offset = 0, .size = width}), in.subspan({.offset = width, .size = width}))));
}
void length(leaves in, result& out)
{
    out.push_back(scalar::of_double(sqrt_of(dot_of(in, in))));
}
void distance(leaves in, result& out)
{
    auto const width = in.size() / 2;
    auto difference = result();
    for (auto i = isize(0); i < width; ++i)
        difference.push_back(scalar::of_double(in[i].as_double() - in[width + i].as_double()));
    length(difference, out);
}
/// `v * inverse_sqrt(dot(v, v))`.
void normalize(leaves in, result& out)
{
    auto const k = inverse_sqrt_of(dot_of(in, in));
    for (auto const& x : in)
        out.push_back(scalar::of_double(x.as_double() * k));
}
void cross(leaves in, result& out)
{
    auto const a = [&](int i) { return in[i].as_double(); };
    auto const b = [&](int i) { return in[3 + i].as_double(); };
    out.push_back(scalar::of_double(a(1) * b(2) - a(2) * b(1)));
    out.push_back(scalar::of_double(a(2) * b(0) - a(0) * b(2)));
    out.push_back(scalar::of_double(a(0) * b(1) - a(1) * b(0)));
}

// ---- the HLSL every one of them calls ------------------------------------------------------------------------------

/// The text of the scalar functions, which the vector ones below call per component.
/// sqrt and inverse_sqrt start from the float's rsqrt of the mantissa, brought to 1..4, and Newton's method takes it
/// from 22 bits to past 53; the last step of sqrt corrects by the residual `fma` computes.
constexpr cc::string_view k_scalar_text = R"(double sgl_double_power_of_two(int k)
{
    return asdouble(0u, uint(1023 + k) << 20);
}
double sgl_double_trunc(double x)
{
    uint lo, hi;
    asuint(x, lo, hi);
    int e = int((hi >> 20) & 0x7ffu) - 1023;
    if (e < 0)
        return asdouble(0u, hi & 0x80000000u);
    if (e >= 52)
        return x;
    if (e < 20)
    {
        hi &= ~(0xfffffu >> uint(e));
        lo = 0u;
    }
    else
        lo &= ~(0xffffffffu >> uint(e - 20));
    return asdouble(lo, hi);
}
double sgl_double_floor(double x)
{
    double t = sgl_double_trunc(x);
    return t > x ? t - 1.0 : t;
}
double sgl_double_ceil(double x)
{
    double t = sgl_double_trunc(x);
    return t < x ? t + 1.0 : t;
}
double sgl_double_round(double x)
{
    double t = sgl_double_trunc(x);
    double rest = abs(x - t);
    double away = x < 0.0 ? t - 1.0 : t + 1.0;
    if (rest > 0.5)
        return away;
    if (rest == 0.5 && sgl_double_trunc(t * 0.5) * 2.0 != t)
        return away;
    return t;
}
double sgl_double_fract(double x)
{
    return x - sgl_double_floor(x);
}
bool sgl_double_is_nan(double x)
{
    uint lo, hi;
    asuint(x, lo, hi);
    return (hi & 0x7fffffffu) > 0x7ff00000u || ((hi & 0x7fffffffu) == 0x7ff00000u && lo != 0u);
}
bool sgl_double_is_inf(double x)
{
    uint lo, hi;
    asuint(x, lo, hi);
    return (hi & 0x7fffffffu) == 0x7ff00000u && lo == 0u;
}
// x = m * 2^(2j) * 2^-(2 * scale) with m in 1..4, the exponent split off by its bits; nothing where x is a NaN, an
// infinity, a zero or negative, which the callers answer themselves.
// A subnormal's mantissa becomes a normal number, 1.M - 1, by its bits: a device that flushes subnormals would read
// x * 2^54 as zero.
double sgl_double_sqrt_mantissa(double x, out int j, out int scale)
{
    uint lo, hi;
    asuint(x, lo, hi);
    uint e = (hi >> 20) & 0x7ffu;
    scale = 0;
    if (e == 0u)
    {
        asuint(asdouble(lo, (hi & 0xfffffu) | 0x3ff00000u) - 1.0, lo, hi);
        e = (hi >> 20) & 0x7ffu;
        scale = 511;
    }
    j = (int(e) - 1023) >> 1;
    return asdouble(lo, (hi & 0x800fffffu) | (uint(int(e) - 2 * j) << 20));
}
double sgl_double_inverse_sqrt_of_mantissa(double m)
{
    double y = double(rsqrt(float(m)));
    y = y * (1.5 - 0.5 * m * y * y);
    return y * (1.5 - 0.5 * m * y * y);
}
double sgl_double_sqrt(double x)
{
    uint lo, hi;
    asuint(x, lo, hi);
    if ((hi & 0x7ff00000u) == 0x7ff00000u)
        return (hi >> 31) == 0u || sgl_double_is_nan(x) ? x : asdouble(0u, 0x7ff80000u);
    if (((hi & 0x7fffffffu) | lo) == 0u)
        return x;
    if ((hi >> 31) != 0u)
        return asdouble(0u, 0x7ff80000u);
    int j, scale;
    double m = sgl_double_sqrt_mantissa(x, j, scale);
    double y = sgl_double_inverse_sqrt_of_mantissa(m);
    double s = m * y;
    s = fma(fma(-s, s, m), 0.5 * y, s);
    return s * sgl_double_power_of_two(j - scale);
}
double sgl_double_inverse_sqrt(double x)
{
    uint lo, hi;
    asuint(x, lo, hi);
    if ((hi & 0x7ff00000u) == 0x7ff00000u)
        return sgl_double_is_nan(x) ? x : (hi >> 31) == 0u ? 0.0 : asdouble(0u, 0x7ff80000u);
    if (((hi & 0x7fffffffu) | lo) == 0u)
        return asdouble(0u, (hi & 0x80000000u) | 0x7ff00000u);
    if ((hi >> 31) != 0u)
        return asdouble(0u, 0x7ff80000u);
    int j, scale;
    double m = sgl_double_sqrt_mantissa(x, j, scale);
    return sgl_double_inverse_sqrt_of_mantissa(m) * sgl_double_power_of_two(scale - j);
}
)";

constexpr cc::string_view k_unary_names[]
    = {"trunc", "floor", "ceil", "round", "fract", "sqrt", "inverse_sqrt", "is_nan", "is_inf"};
constexpr cc::string_view k_component[] = {"x", "y", "z", "w"};

cc::string vector_type(cc::string_view stem, int width)
{
    return width == 1 ? cc::string(stem) : cc::format("{}{}", stem, width);
}

/// `name(x.x), name(x.y)` and so on, over the first `width` components.
cc::string per_component(int width, cc::string_view name)
{
    auto text = cc::string();
    for (auto i = 0; i < width; ++i)
        text += cc::format("{}{}(x.{})", i > 0 ? ", " : "", name, k_component[i]);
    return text;
}

cc::string library_text()
{
    auto text = cc::string(k_scalar_text);
    for (auto width = 2; width <= 4; ++width)
    {
        auto const t = vector_type("double", width);
        for (auto const name : k_unary_names)
        {
            auto const result = name.starts_with("is_") ? vector_type("bool", width) : t;
            text += cc::format("{0} sgl_double_{1}({2} x)\n{{\n    return {0}({3});\n}}\n", result, name, t,
                               per_component(width, cc::format("sgl_double_{}", name)));
        }
        text += cc::format("double sgl_double_dot({0} a, {0} b)\n{{\n    return {1};\n}}\n", t,
                           [&]
                           {
                               auto sum = cc::string("a.x * b.x");
                               for (auto i = 1; i < width; ++i)
                                   sum = cc::format("{} + a.{} * b.{}", sum, k_component[i], k_component[i]);
                               return sum;
                           }());
        text
            += cc::format("double sgl_double_length({0} v)\n{{\n    return sgl_double_sqrt(sgl_double_dot(v, v));\n}}\n"
                          "double sgl_double_distance({0} a, {0} b)\n{{\n    return sgl_double_length(a - b);\n}}\n"
                          "{0} sgl_double_normalize({0} v)\n{{\n"
                          "    return v * sgl_double_inverse_sqrt(sgl_double_dot(v, v));\n}}\n",
                          t);
    }
    text += "double3 sgl_double_cross(double3 a, double3 b)\n{\n"
            "    return double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);\n}\n";
    for (auto width = 1; width <= 4; ++width)
    {
        auto const t = vector_type("double", width);
        text += cc::format("{0} sgl_double_mix({0} a, {0} b, double t)\n{{\n    return a * (1.0 - t) + b * t;\n}}\n"
                           "{0} sgl_double_step({0} edge, {0} x)\n{{\n    return {0}(x >= edge);\n}}\n"
                           "{0} sgl_double_smoothstep({0} low, {0} high, {0} x)\n{{\n"
                           "    {0} t = saturate((x - low) / (high - low));\n"
                           "    return t * t * (3.0 - 2.0 * t);\n}}\n",
                           t);
        if (width > 1)
            text += cc::format("{0} sgl_double_mix({0} a, {0} b, {0} t)\n{{\n    return a * (1.0 - t) + b * t;\n}}\n", t);
    }
    return text;
}

/// One text for every function, so two calls of different ones still declare it once and nothing twice.
cc::string library_helper(helper_context const& c)
{
    return c.target == language::hlsl ? library_text() : cc::string();
}

/// HLSL's `sign` of a double is an int, so it converts back to the type `data` names; no other target takes a double.
written write_sign(call_context const& c)
{
    if (c.target != language::hlsl)
        return {.text = cc::format("sign({})", c.arguments[0].text)};
    return {.text = cc::format("{}(sign({}))", c.builtins.at(builtin_type_id(i32(c.data))).hlsl, c.arguments[0].text)};
}

constexpr cc::string_view k_library_names[] = {
    "asuint",
    "asdouble",
    "rsqrt",
    "fma",
    "abs",
    "saturate",
    "sgl_double_power_of_two",
    "sgl_double_trunc",
    "sgl_double_floor",
    "sgl_double_ceil",
    "sgl_double_round",
    "sgl_double_fract",
    "sgl_double_is_nan",
    "sgl_double_is_inf",
    "sgl_double_sqrt_mantissa",
    "sgl_double_inverse_sqrt_of_mantissa",
    "sgl_double_sqrt",
    "sgl_double_inverse_sqrt",
    "sgl_double_dot",
    "sgl_double_length",
    "sgl_double_distance",
    "sgl_double_normalize",
    "sgl_double_cross",
    "sgl_double_mix",
    "sgl_double_step",
    "sgl_double_smoothstep",
};
constexpr cc::string_view k_sign[] = {"sign"};
constexpr cc::string_view k_saturate[] = {"saturate"};

// EMIT-255: HLSL's `asuint` of a double hands its words to two out parameters, so each direction is a function of the text
cc::string double_bits_helper(helper_context const& c)
{
    return c.target == language::hlsl ? cc::string("uint2 sgl_double_bits(double x)\n{\n    uint2 u;\n    asuint(x, "
                                                   "u.x, u.y);\n    return u;\n}\n")
                                      : cc::string();
}
cc::string double_from_bits_helper(helper_context const& c)
{
    return c.target == language::hlsl
             ? cc::string("double sgl_double_from_bits(uint2 u)\n{\n    return asdouble(u.x, u.y);\n}\n")
             : cc::string();
}
constexpr cc::string_view k_double_bits_names[] = {"asuint", "asdouble", "sgl_double_bits", "sgl_double_from_bits"};

spelling library_call(cc::string_view name)
{
    return {.hlsl = cc::format("sgl_double_{}", name), .helper = library_helper, .hlsl_names = k_library_names};
}
} // namespace

void sgl::builtins::register_double_math(registry& r)
{
    using namespace impl;
    struct unary_function
    {
        cc::string_view name;
        evaluator evaluate;
        cc::string_view doc = {};
    };
    unary_function const unaries[] = {
        {.name = "sqrt", .evaluate = unary<sqrt_of>},   {.name = "inverse_sqrt", .evaluate = unary<inverse_sqrt_of>},
        {.name = "floor", .evaluate = unary<floor_of>}, {.name = "ceil", .evaluate = unary<ceil_of>},
        {.name = "trunc", .evaluate = unary<trunc_of>}, {.name = "round", .evaluate = unary<round_of>},
        {.name = "fract", .evaluate = unary<fract_of>},
    };

    r.add_comment("// the maths of a double and its vectors (CHK-458): rounding exact, `sqrt` within an ULP and\n"
                  "// `inverse_sqrt` within two, written as HLSL over a double's arithmetic, since no target has "
                  "them;\n"
                  "// no transcendental, which no target computes in 64 bits either");
    for (auto width = 1; width <= 4; ++width)
    {
        auto const type = vector_type("double", width);
        auto const t = cc::string_view(type);
        auto const bools = vector_type("bool", width);
        for (auto const& f : unaries)
            add_function(r, f.name, {"x", t}, t, f.evaluate, library_call(f.name));
        add_function(r, "sign", {"x", t}, t, unary<sign_of>,
                     {.kind = spelling_kind::custom,
                      .custom = write_sign,
                      .data = u32(index_of(registered_type(r, t))),
                      .hlsl_names = k_sign,
                      .wgsl_names = k_sign,
                      .msl_names = k_sign},
                     "/// -1, 0 or 1, by the sign of each component.");
        add_function(r, "saturate", {"x", t}, t, unary<saturate_of>, {.hlsl_names = k_saturate});
        add_function(r, "fma", {"a", t, "b", t, "c", t}, t, fma, {},
                     "/// `a * b + c`, rounded once on the interpreter and by DXIL; vulkan may round the product "
                     "first.");
        add_function(r, "is_nan", {"x", t}, bools, non_finite_each<true>, library_call("is_nan"),
                     "/// Whether each component is a NaN, read from its bits.");
        add_function(r, "is_inf", {"x", t}, bools, non_finite_each<false>, library_call("is_inf"),
                     "/// Whether each component is an infinity, of either sign, read from its bits.");
        add_function(r, "mix", {"a", t, "b", t, "t", "double"}, t, mix, library_call("mix"),
                     "/// `a * (1 - t) + b * t`, `a` where `t` is 0 and `b` where it is 1.");
        if (width > 1)
            add_function(r, "mix", {"a", t, "b", t, "t", t}, t, mix_componentwise, library_call("mix"),
                         "/// `mix` of each component by its own `t`.");
        add_function(r, "step", {"edge", t, "x", t}, t, step, library_call("step"),
                     "/// 0 where `x` is below `edge`, and 1 from it on.");
        add_function(r, "smoothstep", {"low", t, "high", t, "x", t}, t, smoothstep, library_call("smoothstep"),
                     "/// 0 up to `low`, 1 from `high` on, and a smooth Hermite curve between.", smoothstep_undefined);
        if (width == 1)
            continue;
        add_function(r, "dot", {"a", t, "b", t}, "double", dot, library_call("dot"),
                     "/// The products of the components, summed from the first on.");
        add_function(r, "length", {"v", t}, "double", length, library_call("length"));
        add_function(r, "distance", {"a", t, "b", t}, "double", distance, library_call("distance"));
        add_function(r, "normalize", {"v", t}, t, normalize, library_call("normalize"),
                     "/// `v * inverse_sqrt(dot(v, v))`.");
    }
    add_function(r, "cross", {"a", "double3", "b", "double3"}, "double3", cross, library_call("cross"));

    r.add_comment("// a double's bits as two words, the low first, so no ulong and no shader_int64 stands between "
                  "(CHK-517);\n"
                  "// `d.bits` and `double.from_bits(u)` in core.sgl are how a program spells them");
    add_function(r, "reinterpret_as_uint", {"x", "double"}, "uint2", double_as_uint,
                 {.hlsl = "sgl_double_bits", .helper = double_bits_helper, .hlsl_names = k_double_bits_names});
    add_function(r, "reinterpret_as_double", {"x", "uint2"}, "double", uint_as_double,
                 {.hlsl = "sgl_double_from_bits", .helper = double_from_bits_helper, .hlsl_names = k_double_bits_names});
}
