#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

// DEBUG: stand-ins for the texture methods, as libs/graphics/shaped-graphics-language/docs/TODO.md records.
// They are free functions in the argument order the methods will have: texture, coordinate, level, sampler.
// Only 2D shapes have them, since the stand-ins exist for one end-to-end test and not as the surface.

namespace
{
using check::scalar;
using check::value_kind;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

/// A texture or an image has no texels the interpreter could read, so every read gives zeros of its width.
template <int Width, value_kind Kind>
void zeros(leaves, result& out)
{
    for (auto i = 0; i < Width; ++i)
        out.push_back({.kind = Kind, .bits = 0});
}

void nothing(leaves, result&)
{
}

/// WGSL always gives four channels, so a narrower texel takes the ones it has.
template <int Width>
cc::string wgsl_narrowed(cc::string text)
{
    constexpr cc::string_view swizzles[] = {"", ".x", ".xy", ".xyz", ""};
    return cc::format("{}{}", text, swizzles[Width]);
}

template <int Width>
written write_sample_level(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}.SampleLevel({}, {}, {})", a[0].text, a[3].text, a[1].text, a[2].text)};
    case language::wgsl:
        return {.text = wgsl_narrowed<Width>(
                    cc::format("textureSampleLevel({}, {}, {}, {})", a[0].text, a[3].text, a[1].text, a[2].text))};
    case language::msl:
        return {.text = cc::format("{}.sample({}, {}, level({}))", a[0].text, a[3].text, a[1].text, a[2].text)};
    }
    return {};
}

template <int Width>
written write_sample(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}.Sample({}, {})", a[0].text, a[2].text, a[1].text)};
    case language::wgsl:
        return {.text = wgsl_narrowed<Width>(cc::format("textureSample({}, {}, {})", a[0].text, a[2].text, a[1].text))};
    case language::msl:
        return {.text = cc::format("{}.sample({}, {})", a[0].text, a[2].text, a[1].text)};
    }
    return {};
}

template <int Width>
written write_texture_load(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}.Load(int3({}, {}))", a[0].text, a[1].text, a[2].text)};
    case language::wgsl:
        return {.text = wgsl_narrowed<Width>(cc::format("textureLoad({}, {}, {})", a[0].text, a[1].text, a[2].text))};
    case language::msl:
        return {.text = cc::format("{}.read(uint2({}), {})", a[0].text, a[1].text, a[2].text)};
    }
    return {};
}

template <int Width>
written write_image_load(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}[{}]", a[0].text, a[1].text)};
    case language::wgsl:
        return {.text = wgsl_narrowed<Width>(cc::format("textureLoad({}, {})", a[0].text, a[1].text))};
    case language::msl:
        return {.text = cc::format("{}.read(uint2({}))", a[0].text, a[1].text)};
    }
    return {};
}

/// WGSL stores four channels, so a narrower texel is padded with zeros of its kind.
template <int Width, value_kind Kind>
written write_image_store(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}[{}] = {}", a[0].text, a[1].text, a[2].text)};
    case language::wgsl:
    {
        constexpr auto zero = Kind == value_kind::scalar_float ? "0.0" : Kind == value_kind::scalar_int ? "0" : "0u";
        constexpr auto vector = Kind == value_kind::scalar_float ? "vec4f"
                              : Kind == value_kind::scalar_int   ? "vec4i"
                                                                 : "vec4u";
        auto value = cc::string(a[2].text);
        for (auto i = Width; i < 4; ++i)
            value.appendf(", {}", zero);
        return {.text = cc::format("textureStore({}, {}, {}({}))", a[0].text, a[1].text, vector, value)};
    }
    case language::msl:
        return {.text = cc::format("{}.write({}, uint2({}))", a[0].text, a[2].text, a[1].text)};
    }
    return {};
}

cc::string type_name(cc::string_view stem, int width)
{
    return width == 1 ? cc::string(stem) : cc::format("{}{}", stem, width);
}

template <int Width, value_kind Kind>
void add_family(registry& r, cc::string_view stem, bool samples)
{
    auto const texel = type_name(stem, Width);
    auto const custom = [](custom_writer w) { return spelling{.kind = spelling_kind::custom, .custom = w}; };

    // The level comes from screen-space derivatives, which only a pixel stage has on every target.
    if (samples)
        r.add(function_record{
            .signature = cc::format("@pure @stages(.pixel) fun DEBUG_sample(t: texture2d[{}], coord: float2, "
                                    "s: sampler) -> {}",
                                    texel, texel),
            .evaluate = zeros<Width, Kind>,
            .write = custom(write_sample<Width>),
        });
    if (samples)
        r.add(function_record{
            .signature = cc::format("@pure fun DEBUG_sample_level(t: texture2d[{}], coord: float2, level: float, "
                                    "s: sampler) -> {}",
                                    texel, texel),
            .evaluate = zeros<Width, Kind>,
            .write = custom(write_sample_level<Width>),
        });
    r.add(function_record{
        .signature = cc::format("@pure fun DEBUG_load(t: texture2d[{}], xy: int2, level: int) -> {}", texel, texel),
        .evaluate = zeros<Width, Kind>,
        .write = custom(write_texture_load<Width>),
    });
    // An image is read where another invocation may have written it, so its load keeps its place: no @pure.
    r.add(function_record{
        .signature = cc::format("fun DEBUG_load(i: image2d[{}], xy: int2) -> {}", texel, texel),
        .evaluate = zeros<Width, Kind>,
        .write = custom(write_image_load<Width>),
    });
    r.add(function_record{
        .signature = cc::format("fun DEBUG_store(i: out image2d[{}], xy: int2, value: {})", texel, texel),
        .evaluate = nothing,
        .write = custom(write_image_store<Width, Kind>),
    });
}

template <value_kind Kind>
void add_widths(registry& r, cc::string_view stem, bool samples)
{
    add_family<1, Kind>(r, stem, samples);
    add_family<2, Kind>(r, stem, samples);
    add_family<3, Kind>(r, stem, samples);
    add_family<4, Kind>(r, stem, samples);
}
} // namespace

void sgl::builtins::register_textures(registry& r)
{
    r.add_comment("// DEBUG: stand-ins for the texture methods, in the order the methods will take their arguments.\n"
                  "// A texture samples and loads, an image loads where the shader may read it and stores where it "
                  "may write it.");
    add_widths<value_kind::scalar_float>(r, "float", true);
    add_widths<value_kind::scalar_int>(r, "int", false);
    add_widths<value_kind::scalar_uint>(r, "uint", false);
}
