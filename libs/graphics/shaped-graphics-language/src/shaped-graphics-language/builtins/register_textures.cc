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

// ---- sizes: HLSL's GetDimensions writes through out parameters, so its call is a helper the text declares ----

void size_of(leaves, result& out)
{
    out.push_back(scalar::of(0));
    out.push_back(scalar::of(0));
}

written write_texture_size(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("sgl_size({}, {})", a[0].text, a[1].text)};
    case language::wgsl:
        return {.text = cc::format("vec2i(textureDimensions({}, {}))", a[0].text, a[1].text)};
    case language::msl:
        return {.text = cc::format("int2({0}.get_width({1}), {0}.get_height({1}))", a[0].text, a[1].text)};
    }
    return {};
}

written write_image_size(call_context const& c)
{
    auto const& a = c.arguments;
    switch (c.target)
    {
    case language::hlsl:
        return {.text = cc::format("sgl_size({})", a[0].text)};
    case language::wgsl:
        return {.text = cc::format("vec2i(textureDimensions({}))", a[0].text)};
    case language::msl:
        return {.text = cc::format("int2({0}.get_width(), {0}.get_height())", a[0].text)};
    }
    return {};
}

/// One overload of `sgl_size` per texture type a call passes, which is what lets every call spell it the same.
cc::string texture_size_helper(helper_context const& c)
{
    if (c.target != language::hlsl)
        return {};
    return cc::format("int2 sgl_size({} t, int level)\n"
                      "{{\n"
                      "    uint width, height, levels;\n"
                      "    t.GetDimensions(uint(level), width, height, levels);\n"
                      "    return int2(width, height);\n"
                      "}}\n",
                      c.argument_types[0]);
}

cc::string image_size_helper(helper_context const& c)
{
    if (c.target != language::hlsl)
        return {};
    return cc::format("int2 sgl_size({} i)\n"
                      "{{\n"
                      "    uint width, height;\n"
                      "    i.GetDimensions(width, height);\n"
                      "    return int2(width, height);\n"
                      "}}\n",
                      c.argument_types[0]);
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
            .uses_derivatives = true,
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
    // Core WebGPU has no writable storage in a vertex stage.
    r.add(function_record{
        .signature = cc::format("@stages(.pixel, .compute) fun DEBUG_store(i: out image2d[{}], xy: int2, "
                                "value: {})",
                                texel, texel),
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

    // A size is the same whatever a texture holds or however an image is read, so one record takes every one.
    r.add(function_record{
        .signature = "@pure fun DEBUG_size(t: texture2d, level: int) -> int2",
        .evaluate = size_of,
        .write = {.kind = spelling_kind::custom, .custom = write_texture_size, .helper = texture_size_helper},
    });
    r.add(function_record{
        .signature = "@pure fun DEBUG_size(i: image2d) -> int2",
        .evaluate = size_of,
        .write = {.kind = spelling_kind::custom, .custom = write_image_size, .helper = image_size_helper},
    });
}
