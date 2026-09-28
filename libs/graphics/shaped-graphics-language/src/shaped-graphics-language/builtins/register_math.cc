#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;
using namespace sgl::builtins::impl;

namespace
{
using check::scalar;
using check::value_kind;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

// ---- componentwise floats: each computed in f64 and rounded to f32, which is what a result is ---------------------

template <f64 (*Fn)(f64)>
void unary(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of(f32(Fn(f64(x.as_float())))));
}

/// Two arguments of one width, or a scalar last one: `pow(v, 2.0)` does not exist, so both are always one width here.
template <f64 (*Fn)(f64, f64)>
void binary(leaves in, result& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of(f32(Fn(f64(in[i].as_float()), f64(in[width + i].as_float())))));
}

f64 tan_of(f64 x)
{
    return soft_sin(x) / soft_cos(x);
}
f64 asin_of(f64 x)
{
    return soft_atan2(x, soft_sqrt(1.0 - x * x));
}
f64 acos_of(f64 x)
{
    return soft_atan2(soft_sqrt(1.0 - x * x), x);
}
f64 sinh_of(f64 x)
{
    return 0.5 * (soft_exp(x) - soft_exp(-x));
}
f64 cosh_of(f64 x)
{
    return 0.5 * (soft_exp(x) + soft_exp(-x));
}
f64 tanh_of(f64 x)
{
    if (x > 20.0)
        return 1.0;
    if (x < -20.0)
        return -1.0;
    auto const e = soft_exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}
f64 exp2_of(f64 x)
{
    return soft_exp(x * 0.693147180559945309417232121458176568);
}
f64 log2_of(f64 x)
{
    return soft_log(x) / 0.693147180559945309417232121458176568;
}
f64 inverse_sqrt_of(f64 x)
{
    return 1.0 / soft_sqrt(x);
}
f64 ceil_of(f64 x)
{
    return -soft_floor(-x);
}
f64 fract_of(f64 x)
{
    return x - soft_floor(x);
}
f64 sign_of(f64 x)
{
    return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0;
}
f64 pow_of(f64 x, f64 y)
{
    if (x == 0.0)
        return 0.0;
    return soft_exp(y * soft_log(x));
}
f64 step_of(f64 edge, f64 x)
{
    return x < edge ? 0.0 : 1.0;
}

void smoothstep(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const low = f64(in[i].as_float());
        auto const high = f64(in[width + i].as_float());
        auto t = (f64(in[2 * width + i].as_float()) - low) / (high - low);
        t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
        out.push_back(scalar::of(f32(t * t * (3.0 - 2.0 * t))));
    }
}

/// A single invocation has no neighbours to difference against, so the interpreter's derivative is 0.
void no_derivative(leaves in, result& out)
{
    for (auto i = isize(0); i < in.size(); ++i)
        out.push_back(scalar::of(0.0f));
}

// ---- where WGSL leaves a value indeterminate, the program has no behaviour (EVAL-84's rule) ------------------------

cc::string_view pow_undefined(leaves in)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const x = in[i].as_float();
        if (x < 0.0f || (x == 0.0f && in[width + i].as_float() <= 0.0f))
            return "pow of a negative base, or of zero to a power that is not positive";
    }
    return {};
}
cc::string_view inverse_trig_undefined(leaves in)
{
    for (auto const& x : in)
        if (x.as_float() < -1.0f || x.as_float() > 1.0f)
            return "asin or acos of a value outside -1 to 1";
    return {};
}
cc::string_view smoothstep_undefined(leaves in)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
        if (!(in[i].as_float() < in[width + i].as_float()))
            return "smoothstep whose low edge is not below its high one";
    return {};
}

// ---- geometry ---------------------------------------------------------------------------------------------------

void cross(leaves in, result& out)
{
    auto const a = [&](int i) { return f64(in[i].as_float()); };
    auto const b = [&](int i) { return f64(in[3 + i].as_float()); };
    out.push_back(scalar::of(f32(a(1) * b(2) - a(2) * b(1))));
    out.push_back(scalar::of(f32(a(2) * b(0) - a(0) * b(2))));
    out.push_back(scalar::of(f32(a(0) * b(1) - a(1) * b(0))));
}
f64 dot_leaves(leaves a, leaves b)
{
    auto sum = 0.0;
    for (auto i = isize(0); i < a.size(); ++i)
        sum += f64(a[i].as_float()) * f64(b[i].as_float());
    return sum;
}
void distance(leaves in, result& out)
{
    auto const width = in.size() / 2;
    auto sum = 0.0;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const d = f64(in[i].as_float()) - f64(in[width + i].as_float());
        sum += d * d;
    }
    out.push_back(scalar::of(f32(soft_sqrt(sum))));
}
/// `v - 2 dot(n, v) n`.
void reflect(leaves in, result& out)
{
    auto const v = in.subspan({.offset = 0, .size = 3});
    auto const n = in.subspan({.offset = 3, .size = 3});
    auto const k = 2.0 * dot_leaves(n, v);
    for (auto i = 0; i < 3; ++i)
        out.push_back(scalar::of(f32(f64(v[i].as_float()) - k * f64(n[i].as_float()))));
}
/// WGSL's and HLSL's formula: zero where the ray is reflected entirely.
void refract(leaves in, result& out)
{
    auto const v = in.subspan({.offset = 0, .size = 3});
    auto const n = in.subspan({.offset = 3, .size = 3});
    auto const eta = f64(in[6].as_float());
    auto const d = dot_leaves(n, v);
    auto const k = 1.0 - eta * eta * (1.0 - d * d);
    for (auto i = 0; i < 3; ++i)
        out.push_back(scalar::of(
            k < 0.0 ? 0.0f : f32(eta * f64(v[i].as_float()) - (eta * d + soft_sqrt(k)) * f64(n[i].as_float()))));
}

// ---- integer bits -------------------------------------------------------------------------------------------------

void count_bits(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back({.kind = x.kind, .bits = u32(cc::popcount(x.bits))});
}
/// The highest bit that differs from the sign of an int, or the highest set bit of a uint; all ones where there is none.
void first_bit_high(leaves in, result& out)
{
    for (auto const& x : in)
    {
        auto const v = x.kind == value_kind::scalar_int && x.as_int() < 0 ? ~x.bits : x.bits;
        out.push_back({.kind = x.kind, .bits = v == 0 ? ~0u : u32(31 - cc::count_leading_zeroes(v))});
    }
}
void first_bit_low(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back({.kind = x.kind, .bits = x.bits == 0 ? ~0u : u32(cc::count_trailing_zeroes(x.bits))});
}
void reverse_bits(leaves in, result& out)
{
    for (auto const& x : in)
    {
        auto r = 0u;
        for (auto i = 0; i < 32; ++i)
            r |= ((x.bits >> i) & 1u) << (31 - i);
        out.push_back({.kind = x.kind, .bits = r});
    }
}

// ---- packing ------------------------------------------------------------------------------------------------------

u32 byte_of(f64 v)
{
    return u32(i32(soft_round(v))) & 0xffu;
}
void pack_unorm4x8(leaves in, result& out)
{
    auto packed = 0u;
    for (auto i = 0; i < 4; ++i)
    {
        auto const v = f64(in[i].as_float());
        packed |= byte_of((v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v) * 255.0) << (8 * i);
    }
    out.push_back(scalar::of_uint(packed));
}
void unpack_unorm4x8(leaves in, result& out)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(scalar::of(f32((in[0].bits >> (8 * i)) & 0xffu) / 255.0f));
}
void pack_snorm4x8(leaves in, result& out)
{
    auto packed = 0u;
    for (auto i = 0; i < 4; ++i)
    {
        auto const v = f64(in[i].as_float());
        packed |= byte_of((v < -1.0 ? -1.0 : v > 1.0 ? 1.0 : v) * 127.0) << (8 * i);
    }
    out.push_back(scalar::of_uint(packed));
}
void unpack_snorm4x8(leaves in, result& out)
{
    for (auto i = 0; i < 4; ++i)
    {
        auto const b = f32(i32(in[0].bits << (24 - 8 * i)) >> 24) / 127.0f;
        out.push_back(scalar::of(b < -1.0f ? -1.0f : b));
    }
}
void pack_half2x16(leaves in, result& out)
{
    out.push_back(scalar::of_uint(half_bits_of(in[0].as_float()) | (half_bits_of(in[1].as_float()) << 16)));
}
void unpack_half2x16(leaves in, result& out)
{
    out.push_back(scalar::of(float_of_half_bits(in[0].bits & 0xffffu)));
    out.push_back(scalar::of(float_of_half_bits(in[0].bits >> 16)));
}

void reinterpret(leaves in, result& out, value_kind kind)
{
    for (auto const& x : in)
        out.push_back({.kind = kind, .bits = x.bits});
}
void reinterpret_as_uint(leaves in, result& out)
{
    reinterpret(in, out, value_kind::scalar_uint);
}
void reinterpret_as_int(leaves in, result& out)
{
    reinterpret(in, out, value_kind::scalar_int);
}
void reinterpret_as_float(leaves in, result& out)
{
    reinterpret(in, out, value_kind::scalar_float);
}

// ---- how targets write what they spell differently ------------------------------------------------------------

cc::string type_name(cc::string_view stem, int width)
{
    return width == 1 ? cc::string(stem) : cc::format("{}{}", stem, width);
}

/// HLSL's `sign` of a float is an int, so it converts back.
template <int Width>
written write_sign(call_context const& c)
{
    if (c.target == language::hlsl)
        return {.text = cc::format("{}(sign({}))", type_name("float", Width), c.arguments[0].text)};
    return {.text = cc::format("sign({})", c.arguments[0].text)};
}

/// HLSL's bit functions give a uint, and `reversebits` takes one, so an int converts at the call.
template <int Width, bool IsSigned>
written write_bit_function(call_context const& c, cc::string_view hlsl, cc::string_view wgsl, cc::string_view msl)
{
    auto const& x = c.arguments[0].text;
    switch (c.target)
    {
    case language::hlsl:
        if (!IsSigned)
            return {.text = cc::format("{}({})", hlsl, x)};
        if (hlsl == "reversebits")
            return {.text = cc::format("{}(reversebits({}({})))", type_name("int", Width), type_name("uint", Width), x)};
        return {.text = cc::format("{}({}({}))", type_name("int", Width), hlsl, x)};
    case language::wgsl:
        return {.text = cc::format("{}({})", wgsl, x)};
    case language::msl:
        return {.text = cc::format("{}({})", msl, x)};
    }
    return {};
}
template <int Width, bool IsSigned>
written write_count_bits(call_context const& c)
{
    return write_bit_function<Width, IsSigned>(c, "countbits", "countOneBits", "popcount");
}
template <int Width, bool IsSigned>
written write_first_bit_high(call_context const& c)
{
    return write_bit_function<Width, IsSigned>(c, "firstbithigh", "firstLeadingBit", "sgl_first_bit_high");
}
template <int Width, bool IsSigned>
written write_first_bit_low(call_context const& c)
{
    return write_bit_function<Width, IsSigned>(c, "firstbitlow", "firstTrailingBit", "sgl_first_bit_low");
}
template <int Width, bool IsSigned>
written write_reverse_bits(call_context const& c)
{
    return write_bit_function<Width, IsSigned>(c, "reversebits", "reverseBits", "reverse_bits");
}

/// MSL has `clz` and `ctz`, which count from the other end and give 32 for zero, so the first bit is a helper.
cc::string first_bit_high_helper(helper_context const& c)
{
    if (c.target != language::msl)
        return {};
    auto const& t = c.argument_types[0];
    return cc::format("{0} sgl_first_bit_high({0} x)\n"
                      "{{\n"
                      "    {0} v = select(x, ~x, x < {0}(0));\n"
                      "    return select({0}(31) - {0}(clz(v)), {0}(-1), v == {0}(0));\n"
                      "}}\n",
                      t);
}
cc::string first_bit_low_helper(helper_context const& c)
{
    if (c.target != language::msl)
        return {};
    return cc::format("{0} sgl_first_bit_low({0} x)\n"
                      "{{\n"
                      "    return select({0}(ctz(x)), {0}(-1), x == {0}(0));\n"
                      "}}\n",
                      c.argument_types[0]);
}

/// WGSL and MSL name the type a reinterpretation gives, which HLSL's `asuint` does not.
template <char Kind, int Width>
written write_reinterpret(call_context const& c)
{
    auto const& x = c.arguments[0].text;
    auto const wgsl_scalar = Kind == 'f' ? "f32" : Kind == 'u' ? "u32" : "i32";
    auto const stem = Kind == 'f' ? "float" : Kind == 'u' ? "uint" : "int";
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("as{}({})", stem, x)};
    case language::wgsl:
        return {.text = Width == 1 ? cc::format("bitcast<{}>({})", wgsl_scalar, x)
                                   : cc::format("bitcast<vec{}{}>({})", Width, Kind, x)};
    case language::msl:
        return {.text = cc::format("as_type<{}>({})", type_name(stem, Width), x)};
    }
    return {};
}

/// HLSL has no packing of four bytes before shader model 6.6's integer forms, so the text declares one per function.
cc::string hlsl_packing_helper(helper_context const& c, cc::string_view text)
{
    return c.target == language::hlsl ? cc::string(text) : cc::string();
}
cc::string pack_unorm_helper(helper_context const& c)
{
    return hlsl_packing_helper(c, "uint sgl_pack_unorm4x8(float4 v)\n"
                                  "{\n"
                                  "    uint4 b = uint4(round(saturate(v) * 255.0));\n"
                                  "    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);\n"
                                  "}\n");
}
cc::string unpack_unorm_helper(helper_context const& c)
{
    return hlsl_packing_helper(c, "float4 sgl_unpack_unorm4x8(uint u)\n"
                                  "{\n"
                                  "    return float4(u & 0xff, (u >> 8) & 0xff, (u >> 16) & 0xff, u >> 24) / 255.0;\n"
                                  "}\n");
}
cc::string pack_snorm_helper(helper_context const& c)
{
    return hlsl_packing_helper(c, "uint sgl_pack_snorm4x8(float4 v)\n"
                                  "{\n"
                                  "    uint4 b = uint4(int4(round(clamp(v, -1.0, 1.0) * 127.0))) & 0xff;\n"
                                  "    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);\n"
                                  "}\n");
}
cc::string unpack_snorm_helper(helper_context const& c)
{
    return hlsl_packing_helper(c, "float4 sgl_unpack_snorm4x8(uint u)\n"
                                  "{\n"
                                  "    int4 b = int4(uint4(u << 24, u << 16, u << 8, u)) >> 24;\n"
                                  "    return max(float4(b) / 127.0, -1.0);\n"
                                  "}\n");
}
cc::string pack_half_helper(helper_context const& c)
{
    return hlsl_packing_helper(c, "uint sgl_pack_half2x16(float2 v)\n"
                                  "{\n"
                                  "    return f32tof16(v.x) | (f32tof16(v.y) << 16);\n"
                                  "}\n");
}
cc::string unpack_half_helper(helper_context const& c)
{
    return hlsl_packing_helper(c, "float2 sgl_unpack_half2x16(uint u)\n"
                                  "{\n"
                                  "    return float2(f16tof32(u), f16tof32(u >> 16));\n"
                                  "}\n");
}
written write_pack_half(call_context const& c)
{
    auto const& x = c.arguments[0].text;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("sgl_pack_half2x16({})", x)};
    case language::wgsl:
        return {.text = cc::format("pack2x16float({})", x)};
    case language::msl:
        return {.text = cc::format("as_type<uint>(half2({}))", x)};
    }
    return {};
}
written write_unpack_half(call_context const& c)
{
    auto const& x = c.arguments[0].text;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("sgl_unpack_half2x16({})", x)};
    case language::wgsl:
        return {.text = cc::format("unpack2x16float({})", x)};
    case language::msl:
        return {.text = cc::format("float2(as_type<half2>({}))", x)};
    }
    return {};
}

// ---- the names each custom spelling writes, which no name of the program may take in that target ---------------

constexpr cc::string_view k_sign[] = {"sign"};
constexpr cc::string_view k_count_bits_hlsl[] = {"countbits"};
constexpr cc::string_view k_count_bits_wgsl[] = {"countOneBits"};
constexpr cc::string_view k_count_bits_msl[] = {"popcount"};
constexpr cc::string_view k_first_bit_high_hlsl[] = {"firstbithigh"};
constexpr cc::string_view k_first_bit_high_wgsl[] = {"firstLeadingBit"};
constexpr cc::string_view k_first_bit_high_msl[] = {"sgl_first_bit_high", "clz", "select"};
constexpr cc::string_view k_first_bit_low_hlsl[] = {"firstbitlow"};
constexpr cc::string_view k_first_bit_low_wgsl[] = {"firstTrailingBit"};
constexpr cc::string_view k_first_bit_low_msl[] = {"sgl_first_bit_low", "ctz", "select"};
constexpr cc::string_view k_reverse_bits_hlsl[] = {"reversebits"};
constexpr cc::string_view k_reverse_bits_wgsl[] = {"reverseBits"};
constexpr cc::string_view k_reverse_bits_msl[] = {"reverse_bits"};
constexpr cc::string_view k_reinterpret_hlsl[] = {"asfloat", "asint", "asuint"};
constexpr cc::string_view k_reinterpret_wgsl[] = {"bitcast"};
constexpr cc::string_view k_as_type[] = {"as_type"};
constexpr cc::string_view k_pack_unorm_hlsl[] = {"round", "saturate"};
constexpr cc::string_view k_pack_snorm_hlsl[] = {"round", "clamp"};
constexpr cc::string_view k_unpack_snorm_hlsl[] = {"max"};
constexpr cc::string_view k_pack_half_hlsl[] = {"sgl_pack_half2x16", "f32tof16"};
constexpr cc::string_view k_pack_half_wgsl[] = {"pack2x16float"};
constexpr cc::string_view k_unpack_half_hlsl[] = {"sgl_unpack_half2x16", "f16tof32"};
constexpr cc::string_view k_unpack_half_wgsl[] = {"unpack2x16float"};

struct float_type
{
    cc::string_view name;
    custom_writer sign;
};

struct integer_type
{
    cc::string_view name;
    custom_writer count_bits;
    custom_writer first_bit_high;
    custom_writer first_bit_low;
    custom_writer reverse_bits;
};

template <int Width, bool IsSigned>
constexpr integer_type integer_of(cc::string_view name)
{
    return {.name = name,
            .count_bits = write_count_bits<Width, IsSigned>,
            .first_bit_high = write_first_bit_high<Width, IsSigned>,
            .first_bit_low = write_first_bit_low<Width, IsSigned>,
            .reverse_bits = write_reverse_bits<Width, IsSigned>};
}

struct reinterpreted
{
    cc::string_view from;
    cc::string_view to;
    evaluator evaluate;
    custom_writer write;
};

void add(registry& r,
         cc::string_view signature,
         evaluator evaluate,
         spelling write,
         undefined_check undefined_when = nullptr,
         bool uses_derivatives = false,
         cc::string_view doc = {})
{
    r.add(function_record{
        .signature = cc::string(signature),
        .doc = doc,
        .evaluate = evaluate,
        .undefined_when = undefined_when,
        .write = cc::move(write),
        .uses_derivatives = uses_derivatives,
    });
}
} // namespace

void sgl::builtins::register_math(registry& r)
{
    float_type const floats[] = {
        {.name = "float", .sign = write_sign<1>},
        {.name = "float2", .sign = write_sign<2>},
        {.name = "float3", .sign = write_sign<3>},
        {.name = "float4", .sign = write_sign<4>},
    };

    struct unary_function
    {
        cc::string_view name;
        evaluator evaluate;
        spelling write = {};
        undefined_check undefined_when = nullptr;
    };
    unary_function const unaries[] = {
        {.name = "sin", .evaluate = unary<soft_sin>},
        {.name = "cos", .evaluate = unary<soft_cos>},
        {.name = "tan", .evaluate = unary<tan_of>},
        {.name = "asin", .evaluate = unary<asin_of>, .undefined_when = inverse_trig_undefined},
        {.name = "acos", .evaluate = unary<acos_of>, .undefined_when = inverse_trig_undefined},
        {.name = "atan", .evaluate = unary<soft_atan>},
        {.name = "sinh", .evaluate = unary<sinh_of>},
        {.name = "cosh", .evaluate = unary<cosh_of>},
        {.name = "tanh", .evaluate = unary<tanh_of>},
        {.name = "exp", .evaluate = unary<soft_exp>},
        {.name = "exp2", .evaluate = unary<exp2_of>},
        {.name = "log", .evaluate = unary<soft_log>},
        {.name = "log2", .evaluate = unary<log2_of>},
        {.name = "sqrt", .evaluate = unary<soft_sqrt>},
        {.name = "inverse_sqrt",
         .evaluate = unary<inverse_sqrt_of>,
         .write = {.hlsl = "rsqrt", .wgsl = "inverseSqrt", .msl = "rsqrt"}},
        {.name = "floor", .evaluate = unary<soft_floor>},
        {.name = "ceil", .evaluate = unary<ceil_of>},
        {.name = "trunc", .evaluate = unary<soft_trunc>},
        {.name = "round", .evaluate = unary<soft_round>, .write = {.msl = "rint"}},
        {.name = "fract", .evaluate = unary<fract_of>, .write = {.hlsl = "frac"}},
    };

    r.add_comment("// the maths of float and its plain vectors, componentwise; `round` is ties to even everywhere,\n"
                  "// and a value WGSL leaves indeterminate - pow of a negative base, asin of 2 - has no behaviour");
    for (auto const& t : floats)
    {
        for (auto const& f : unaries)
            add(r, cc::format("@pure fun {}(x: {}) -> {}", f.name, t.name, t.name), f.evaluate, f.write,
                f.undefined_when);
        add(r, cc::format("@pure fun sign(x: {}) -> {}", t.name, t.name), unary<sign_of>,
            {.kind = spelling_kind::custom, .custom = t.sign, .hlsl_names = k_sign, .wgsl_names = k_sign, .msl_names = k_sign},
            nullptr, false, "/// -1, 0 or 1, by the sign of each component.");
        add(r, cc::format("@pure fun atan2(y: {0}, x: {0}) -> {0}", t.name), binary<soft_atan2>, {}, nullptr, false,
            "/// The angle of (x, y), in -pi to pi; `y` comes first, as in every target.");
        add(r, cc::format("@pure fun pow(x: {0}, y: {0}) -> {0}", t.name), binary<pow_of>, {}, pow_undefined);
        add(r, cc::format("@pure fun step(edge: {0}, x: {0}) -> {0}", t.name), binary<step_of>, {}, nullptr, false,
            "/// 0 where `x` is below `edge`, and 1 from it on.");
        add(r, cc::format("@pure fun smoothstep(low: {0}, high: {0}, x: {0}) -> {0}", t.name), smoothstep, {},
            smoothstep_undefined, false, "/// 0 up to `low`, 1 from `high` on, and a smooth Hermite curve between.");
    }

    r.add_comment("// derivatives across the 2x2 quad of pixels, which only a pixel stage has;\n"
                  "// a run of one invocation has no neighbours, and reads 0");
    for (auto const& t : floats)
    {
        add(r, cc::format("@pure @stages(.pixel) fun ddx(x: {0}) -> {0}", t.name), no_derivative,
            {.wgsl = "dpdx", .msl = "dfdx"}, nullptr, true);
        add(r, cc::format("@pure @stages(.pixel) fun ddy(x: {0}) -> {0}", t.name), no_derivative,
            {.wgsl = "dpdy", .msl = "dfdy"}, nullptr, true);
    }

    r.add_comment("// geometry");
    add(r, "@pure fun cross(a: vec3, b: vec3) -> vec3", cross, {});
    add(r, "@pure fun cross(a: float3, b: float3) -> float3", cross, {});
    add(r, "@pure fun distance(a: pos3, b: pos3) -> float", distance, {});
    for (auto const t : {cc::string_view("float2"), cc::string_view("float3"), cc::string_view("float4")})
        add(r, cc::format("@pure fun distance(a: {0}, b: {0}) -> float", t), distance, {});
    add(r, "@pure fun reflect(v: vec3, n: vec3) -> vec3", reflect, {}, nullptr, false,
        "/// `v` mirrored at the plane whose unit normal is `n`.");
    add(r, "@pure fun refract(v: vec3, n: vec3, eta: float) -> vec3", refract, {}, nullptr, false,
        "/// `v` bent through a surface of unit normal `n` by the ratio of indices `eta`; zero where it reflects "
        "entirely.");

    integer_type const integers[] = {
        integer_of<1, true>("int"),    integer_of<2, true>("int2"),   integer_of<3, true>("int3"),
        integer_of<4, true>("int4"),   integer_of<1, false>("uint"),  integer_of<2, false>("uint2"),
        integer_of<3, false>("uint3"), integer_of<4, false>("uint4"),
    };
    r.add_comment("// integer bits, componentwise; `first_bit_*` gives all ones - -1 for an int - where there is no "
                  "bit");
    for (auto const& t : integers)
    {
        add(r, cc::format("@pure fun count_bits(x: {0}) -> {0}", t.name), count_bits,
            {.kind = spelling_kind::custom,
             .custom = t.count_bits,
             .hlsl_names = k_count_bits_hlsl,
             .wgsl_names = k_count_bits_wgsl,
             .msl_names = k_count_bits_msl});
        add(r, cc::format("@pure fun first_bit_high(x: {0}) -> {0}", t.name), first_bit_high,
            {.kind = spelling_kind::custom,
             .custom = t.first_bit_high,
             .helper = first_bit_high_helper,
             .hlsl_names = k_first_bit_high_hlsl,
             .wgsl_names = k_first_bit_high_wgsl,
             .msl_names = k_first_bit_high_msl},
            nullptr, false, "/// The highest set bit of a uint; of an int, the highest bit that differs from its sign.");
        add(r, cc::format("@pure fun first_bit_low(x: {0}) -> {0}", t.name), first_bit_low,
            {.kind = spelling_kind::custom,
             .custom = t.first_bit_low,
             .helper = first_bit_low_helper,
             .hlsl_names = k_first_bit_low_hlsl,
             .wgsl_names = k_first_bit_low_wgsl,
             .msl_names = k_first_bit_low_msl});
        add(r, cc::format("@pure fun reverse_bits(x: {0}) -> {0}", t.name), reverse_bits,
            {.kind = spelling_kind::custom,
             .custom = t.reverse_bits,
             .hlsl_names = k_reverse_bits_hlsl,
             .wgsl_names = k_reverse_bits_wgsl,
             .msl_names = k_reverse_bits_msl});
    }

    r.add_comment("// the bits of a float, read and written; `x.bits` and `float.from_bits(u)` in core.sgl are how a\n"
                  "// program spells them, and `as` stays a numeric conversion");
    reinterpreted const reinterpretations[] = {
        {"float", "uint", reinterpret_as_uint, write_reinterpret<'u', 1>},
        {"float2", "uint2", reinterpret_as_uint, write_reinterpret<'u', 2>},
        {"float3", "uint3", reinterpret_as_uint, write_reinterpret<'u', 3>},
        {"float4", "uint4", reinterpret_as_uint, write_reinterpret<'u', 4>},
        {"uint", "float", reinterpret_as_float, write_reinterpret<'f', 1>},
        {"uint2", "float2", reinterpret_as_float, write_reinterpret<'f', 2>},
        {"uint3", "float3", reinterpret_as_float, write_reinterpret<'f', 3>},
        {"uint4", "float4", reinterpret_as_float, write_reinterpret<'f', 4>},
        {"uint", "int", reinterpret_as_int, write_reinterpret<'i', 1>},
        {"uint2", "int2", reinterpret_as_int, write_reinterpret<'i', 2>},
        {"uint3", "int3", reinterpret_as_int, write_reinterpret<'i', 3>},
        {"uint4", "int4", reinterpret_as_int, write_reinterpret<'i', 4>},
    };
    for (auto const& x : reinterpretations)
        add(r,
            cc::format("@pure fun reinterpret_as_{}(x: {}) -> {}",
                       x.to.starts_with("float")  ? "float"
                       : x.to.starts_with("uint") ? "uint"
                                                  : "int",
                       x.from, x.to),
            x.evaluate,
            {.kind = spelling_kind::custom,
             .custom = x.write,
             .hlsl_names = k_reinterpret_hlsl,
             .wgsl_names = k_reinterpret_wgsl,
             .msl_names = k_as_type});

    r.add_comment("// packing into a uint and back: bytes rounded to nearest, and halves rounded to even");
    add(r, "@pure fun pack_unorm4x8(v: float4) -> uint", pack_unorm4x8,
        {.hlsl = "sgl_pack_unorm4x8",
         .wgsl = "pack4x8unorm",
         .msl = "pack_float_to_unorm4x8",
         .helper = pack_unorm_helper,
         .hlsl_names = k_pack_unorm_hlsl});
    add(r, "@pure fun unpack_unorm4x8(u: uint) -> float4", unpack_unorm4x8,
        {.hlsl = "sgl_unpack_unorm4x8",
         .wgsl = "unpack4x8unorm",
         .msl = "unpack_unorm4x8_to_float",
         .helper = unpack_unorm_helper});
    add(r, "@pure fun pack_snorm4x8(v: float4) -> uint", pack_snorm4x8,
        {.hlsl = "sgl_pack_snorm4x8",
         .wgsl = "pack4x8snorm",
         .msl = "pack_float_to_snorm4x8",
         .helper = pack_snorm_helper,
         .hlsl_names = k_pack_snorm_hlsl});
    add(r, "@pure fun unpack_snorm4x8(u: uint) -> float4", unpack_snorm4x8,
        {.hlsl = "sgl_unpack_snorm4x8",
         .wgsl = "unpack4x8snorm",
         .msl = "unpack_snorm4x8_to_float",
         .helper = unpack_snorm_helper,
         .hlsl_names = k_unpack_snorm_hlsl});
    add(r, "@pure fun pack_half2x16(v: float2) -> uint", pack_half2x16,
        {.kind = spelling_kind::custom,
         .custom = write_pack_half,
         .helper = pack_half_helper,
         .hlsl_names = k_pack_half_hlsl,
         .wgsl_names = k_pack_half_wgsl,
         .msl_names = k_as_type});
    add(r, "@pure fun unpack_half2x16(u: uint) -> float2", unpack_half2x16,
        {.kind = spelling_kind::custom,
         .custom = write_unpack_half,
         .helper = unpack_half_helper,
         .hlsl_names = k_unpack_half_hlsl,
         .wgsl_names = k_unpack_half_wgsl,
         .msl_names = k_as_type});
}
