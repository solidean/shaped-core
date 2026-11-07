#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;
using namespace sgl::builtins::impl;

// CHK-458: a double's rounding, roots and geometry are SGL in core.sgl, over what this file registers.
// What stays here is what SGL cannot say: `sign` and `saturate`, which HLSL takes a double to, `fma`, a double's bits, and
// the two answers only the interpreter gives, the nearest root and smoothstep's missing value (EVAL-108).

namespace
{
using check::scalar;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

template <f64 (*Fn)(f64)>
void unary(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of_double(Fn(x.as_double())));
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

/// EVAL-108: smoothstep's own steps, every one rounded to a double, which core.sgl takes on the interpreter alone.
void smoothstep(leaves in, result& out)
{
    auto const low = in[0].as_double();
    auto const high = in[1].as_double();
    auto const t = saturate_of((in[2].as_double() - low) / (high - low));
    out.push_back(scalar::of_double(t * t * (3.0 - 2.0 * t)));
}
cc::string_view smoothstep_undefined(leaves in)
{
    if (!(in[0].as_double() < in[1].as_double()))
        return "smoothstep whose low edge is not below its high one";
    return {};
}

/// HLSL's `sign` of a double is an int, so it converts back to the type `data` names; no other target takes a double.
written write_sign(call_context const& c)
{
    if (c.target != language::hlsl)
        return {.text = cc::format("sign({})", c.arguments[0].text)};
    return {.text = cc::format("{}(sign({}))", c.builtins.at(builtin_type_id(i32(c.data))).hlsl, c.arguments[0].text)};
}

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

f64 ceil_of(f64 x)
{
    return -soft_floor(-x);
}
f64 fract_of(f64 x)
{
    return x - soft_floor(x);
}
f64 inverse_root_of(f64 x)
{
    return 1.0 / correctly_rounded_sqrt(x);
}

/// EMIT-256: a GLSL.std.450 instruction vulkan takes a double to, which core.sgl calls where the API is vulkan.
struct spirv_double_op
{
    cc::string_view name;
    cc::string_view spirv;
    u32 opcode;
    evaluator evaluate;
    cc::string_view doc;
};
constexpr spirv_double_op k_spirv_double_ops[] = {
    {"trunc", "Trunc", 3, unary<soft_trunc>, "/// The integral part, as GLSL.std.450's `Trunc`."},
    {"floor", "Floor", 8, unary<soft_floor>, "/// The integral part toward negative infinity, as GLSL.std.450's `Floor`."},
    {"ceil", "Ceil", 9, unary<ceil_of>, "/// The integral part toward positive infinity, as GLSL.std.450's `Ceil`."},
    {"round", "RoundEven", 2, unary<soft_round>,
     "/// The nearest integer, a tie to the even one, as GLSL.std.450's `RoundEven`."},
    {"fract", "Fract", 10, unary<fract_of>, "/// `x - floor(x)`, as GLSL.std.450's `Fract`."},
    {"sqrt", "Sqrt", 31, unary<correctly_rounded_sqrt>, "/// The root, as GLSL.std.450's `Sqrt`."},
    {"inverse_sqrt", "InverseSqrt", 32, unary<inverse_root_of>, "/// `1 / sqrt(x)`, as GLSL.std.450's `InverseSqrt`."},
};

/// What `hlsl-vulkan` declares for one of them: DXC's inline SPIR-V, since its own `floor` of a double takes a float.
cc::string spirv_double_helper(helper_context const& h)
{
    if (h.target != language::hlsl || !h.is_vulkan)
        return {};
    auto const& op = k_spirv_double_ops[h.data];
    return cc::format("[[vk::ext_instruction(/* {} */ {}, \"GLSL.std.450\")]] double sgl_spirv_{}(double x);\n",
                      op.spirv, op.opcode, op.name);
}

constexpr cc::string_view k_spirv_double_names[] = {
    "sgl_spirv_trunc", "sgl_spirv_floor", "sgl_spirv_ceil",         "sgl_spirv_round",
    "sgl_spirv_fract", "sgl_spirv_sqrt",  "sgl_spirv_inverse_sqrt",
};

constexpr cc::string_view k_double_bits_names[] = {"asuint", "asdouble", "sgl_double_bits", "sgl_double_from_bits"};
constexpr cc::string_view k_sign[] = {"sign"};
constexpr cc::string_view k_saturate[] = {"saturate"};
} // namespace

void sgl::builtins::register_double_math(registry& r)
{
    using namespace impl;

    r.add_comment(
        "// what a double's maths needs of the registry (CHK-458): `sign` and `saturate`, which HLSL takes a\n"
        "// double to, and `fma`; core.sgl writes the rest over them and a double's bits, and no transcendental\n"
        "// takes a double, which no target computes in 64 bits");
    for (auto width = 1; width <= 4; ++width)
    {
        auto const type = width == 1 ? cc::string("double") : cc::format("double{}", width);
        auto const t = cc::string_view(type);
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
    }

    r.add_comment("// a double's bits as two words, the low first, so no ulong and no shader_int64 stands between "
                  "(CHK-517);\n"
                  "// `d.bits` and `double.from_bits(u)` in core.sgl are how a program spells them");
    add_function(r, "reinterpret_as_uint", {"x", "double"}, "uint2", double_as_uint,
                 {.hlsl = "sgl_double_bits", .helper = double_bits_helper, .hlsl_names = k_double_bits_names});
    add_function(r, "reinterpret_as_double", {"x", "uint2"}, "double", uint_as_double,
                 {.hlsl = "sgl_double_from_bits", .helper = double_from_bits_helper, .hlsl_names = k_double_bits_names});

    r.add_comment("// what the interpreter gives where a GPU gives its own: core.sgl calls these where the language is "
                  "`.sgl`");
    r.add(function_record{
        .signature = "@internal @pure fun nearest_sqrt(x: double) -> double",
        .doc = "/// The double nearest the root, as IEEE 754's sqrt.",
        .evaluate = unary<correctly_rounded_sqrt>,
    });
    r.add(function_record{
        .signature = "@internal @pure fun ordered_smoothstep(low: double, high: double, x: double) -> double",
        .doc = "/// smoothstep's steps, with no value where `low` is not below `high`.",
        .evaluate = smoothstep,
        .undefined_when = smoothstep_undefined,
    });

    r.add_comment("// what vulkan takes a double to, which core.sgl calls where the API is vulkan (EMIT-256)");
    for (auto i = 0u; i < u32(sizeof(k_spirv_double_ops) / sizeof(k_spirv_double_ops[0])); ++i)
    {
        auto const& op = k_spirv_double_ops[i];
        r.add(function_record{
            .signature = cc::format("@internal @pure fun spirv_{}(x: double) -> double", op.name),
            .doc = cc::string(op.doc),
            .evaluate = op.evaluate,
            .write = {.hlsl = cc::format("sgl_spirv_{}", op.name),
                      .helper = spirv_double_helper,
                      .data = i,
                      .hlsl_names = k_spirv_double_names},
        });
    }
}
