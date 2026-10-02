#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::is_16_bit;
using check::scalar;
using check::value_kind;
using check::wide_kind_of;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

/// One numeric family: its SGL stem, which is MSL's too, and its scalar as HLSL and WGSL spell it.
struct numeric
{
    cc::string_view name;
    cc::string_view hlsl;
    cc::string_view wgsl_scalar;
    cc::string_view wgsl_vector_suffix;
    value_kind kind;
};

// WGSL has no 16-bit integer, so no text holds the spellings of short and ushort there (EMIT-109).
constexpr numeric k_numerics[] = {
    {.name = "float", .hlsl = "float", .wgsl_scalar = "f32", .wgsl_vector_suffix = "f", .kind = value_kind::scalar_float},
    {.name = "int", .hlsl = "int", .wgsl_scalar = "i32", .wgsl_vector_suffix = "i", .kind = value_kind::scalar_int},
    {.name = "uint", .hlsl = "uint", .wgsl_scalar = "u32", .wgsl_vector_suffix = "u", .kind = value_kind::scalar_uint},
    {.name = "half", .hlsl = "float16_t", .wgsl_scalar = "f16", .wgsl_vector_suffix = "h", .kind = value_kind::scalar_half},
    {.name = "short", .hlsl = "int16_t", .wgsl_scalar = "i16", .wgsl_vector_suffix = "", .kind = value_kind::scalar_short},
    {.name = "ushort", .hlsl = "uint16_t", .wgsl_scalar = "u16", .wgsl_vector_suffix = "", .kind = value_kind::scalar_ushort},
};

/// CHK-197: the families of one width convert among themselves, and each family between its widths.
bool converts(numeric const& from, numeric const& to)
{
    if (from.name == to.name)
        return false;
    return is_16_bit(from.kind) == is_16_bit(to.kind) || wide_kind_of(from.kind) == wide_kind_of(to.kind);
}

/// A float to an integer truncates toward zero; out of range, and for a NaN, the value is unspecified (CHK-197).
/// This is the interpreter's choice of it: saturation at the integer's bounds, and 0 for a NaN.
/// The 2^31 and 2^32 bounds are exact floats, so a value at or past one saturates rather than overflows.
i32 saturated_int(f32 x)
{
    if (!(x == x))
        return 0;
    if (x >= 2147483648.0f)
        return 2147483647;
    if (x <= -2147483648.0f)
        return i32(0x80000000u);
    return i32(x);
}
u32 saturated_uint(f32 x)
{
    if (!(x > 0.0f))
        return 0;
    if (x >= 4294967296.0f)
        return 0xffffffffu;
    return u32(x);
}
/// The same at 16 bits, whose bounds are exact floats as well.
i32 saturated_short(f32 x)
{
    if (!(x == x))
        return 0;
    return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : i32(x);
}
u32 saturated_ushort(f32 x)
{
    if (!(x > 0.0f))
        return 0;
    return x >= 65535.0f ? 65535u : u32(x);
}

scalar converted(scalar from, value_kind to)
{
    switch (to)
    {
    case value_kind::scalar_float:
        if (from.kind == value_kind::scalar_int)
            return scalar::of(f32(from.as_int()));
        if (from.kind == value_kind::scalar_uint)
            return scalar::of(f32(from.as_uint()));
        return from;
    case value_kind::scalar_int:
        if (from.kind == value_kind::scalar_float)
            return scalar::of(saturated_int(from.as_float()));
        // Between int and uint the bits stay, as every target converts.
        return {.kind = value_kind::scalar_int, .bits = from.bits};
    case value_kind::scalar_uint:
        if (from.kind == value_kind::scalar_float)
            return scalar::of_uint(saturated_uint(from.as_float()));
        return scalar::of_uint(from.bits);
    // a 16-bit result: a float saturates at the 16-bit bounds, and an integer keeps its low 16 bits
    case value_kind::scalar_short:
    case value_kind::scalar_ushort:
    {
        auto const bits = from.kind != value_kind::scalar_float ? from.bits
                        : to == value_kind::scalar_short        ? u32(saturated_short(from.as_float()))
                                                                : saturated_ushort(from.as_float());
        return {.kind = to, .bits = bits & 0xffffu};
    }
    // a half is rounded from the float the interpreter narrows the result to
    case value_kind::scalar_half:
        return converted(from, value_kind::scalar_float);
    default:
        return from;
    }
}

template <value_kind To>
void convert(leaves in, result& out)
{
    for (auto const& leaf : in)
        out.push_back(converted(leaf, To));
}

evaluator converter_to(value_kind kind)
{
    switch (kind)
    {
    case value_kind::scalar_float:
        return convert<value_kind::scalar_float>;
    case value_kind::scalar_int:
        return convert<value_kind::scalar_int>;
    case value_kind::scalar_half:
        return convert<value_kind::scalar_half>;
    case value_kind::scalar_short:
        return convert<value_kind::scalar_short>;
    case value_kind::scalar_ushort:
        return convert<value_kind::scalar_ushort>;
    default:
        return convert<value_kind::scalar_uint>;
    }
}

void bool_to_bool32(leaves in, result& out)
{
    out.push_back(scalar::of_uint(in[0].as_bool() ? 1u : 0u));
}

void bool32_to_bool(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_uint() != 0));
}

/// `1` where the bool is true and `0` where not, as each target selects.
written write_bool_to_bool32(call_context const& c)
{
    auto const x = builtins::wrapped(c.arguments[0], precedence::comparison);
    if (c.target == language::wgsl)
        return {.text = cc::format("select(0u, 1u, {})", c.arguments[0].text)};
    return {.text = cc::format("({} ? 1u : 0u)", x)};
}

/// Anything but 0 is true, as a GPU reads a bool from memory.
written write_bool32_to_bool(call_context const& c)
{
    return {.text = cc::format("{} != 0u", builtins::wrapped(c.arguments[0], precedence::additive)),
            .binds = precedence::comparison};
}

constexpr cc::string_view k_select[] = {"select"};

cc::string type_name(numeric const& n, i32 width)
{
    return width == 1 ? cc::string(n.name) : cc::format("{}{}", n.name, width);
}
} // namespace

void sgl::builtins::register_conversions(registry& r)
{
    r.add_comment("// `x as T` between the numeric families of one width, and between the widths of one family,\n"
                  "// component for component; each target writes a conversion to `T`");
    for (auto width = 1; width <= 4; ++width)
        for (auto const& from : k_numerics)
            for (auto const& to : k_numerics)
            {
                if (!converts(from, to))
                    continue;
                auto const source = type_name(from, width);
                auto const target = type_name(to, width);
                auto const hlsl = width == 1 ? cc::string(to.hlsl) : cc::format("{}{}", to.hlsl, width);
                auto const wgsl = width == 1                    ? cc::string(to.wgsl_scalar)
                                : to.wgsl_vector_suffix.empty() ? cc::format("vec{}<{}>", width, to.wgsl_scalar)
                                                                : cc::format("vec{}{}", width, to.wgsl_vector_suffix);
                r.add(function_record{
                    .signature = cc::format("@pure @operator(\"as\") fun convert_{}_to_{}(x: {}) -> {}", source, target,
                                            source, target),
                    .evaluate = converter_to(to.kind),
                    // WGSL folds a constant exactly, and no uint holds a negative int (CHK-312)
                    .unrepresentable_when_constant
                    = from.kind == value_kind::scalar_int && to.kind == value_kind::scalar_uint ? impl::negative_as_uint
                                                                                                : nullptr,
                    .write = {.hlsl = hlsl, .wgsl = wgsl, .msl = target},
                });
            }

    r.add_comment("// A bool32 is a bool as GPU memory holds one, and `as` is how either becomes the other");
    r.add(function_record{
        .signature = "@pure @operator(\"as\") fun convert_bool_to_bool32(x: bool) -> bool32",
        .evaluate = bool_to_bool32,
        .write = {.kind = spelling_kind::custom, .custom = write_bool_to_bool32, .wgsl_names = k_select},
    });
    r.add(function_record{
        .signature = "@pure @operator(\"as\") fun convert_bool32_to_bool(x: bool32) -> bool",
        .evaluate = bool32_to_bool,
        .write = {.kind = spelling_kind::custom, .custom = write_bool32_to_bool},
    });
}
