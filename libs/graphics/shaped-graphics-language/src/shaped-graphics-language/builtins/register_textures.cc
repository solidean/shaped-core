#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::builtins;

// The texture and image methods, for every shape a binding may hold (the spec's bindings file, "Shapes").
//
// Each record carries a data word saying which call it is: the operation, the shape, the texel and the options.
// One writer per target reads it, so the hundreds of records share a handful of spellings rather than one each.
// The records are generated here in the order their parameters stand, and the writers read the arguments by that order.

namespace
{
using check::scalar;
using check::value_kind;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

enum class op : u8
{
    sample,
    sample_level,
    sample_bias,
    sample_grad,
    gather,
    sample_compare,
    sample_compare_level,
    gather_compare,
    load,
    size,
    layer_count,
    level_count,
    sample_count,
    image_load,
    image_store,
    image_size,
    image_layer_count,
};

using check::texture_shape;

/// What a shape's calls need to know of it beyond the check pass's table.
struct shape_traits
{
    texture_shape s;
    cc::string_view texture;
    /// Empty for a shape no target has a depth texture of.
    cc::string_view depth;
    /// Empty for a shape no target has an image of.
    cc::string_view image;
    /// sg's name of the shape, which names a helper per shape.
    cc::string_view sg_name;
    /// How many coordinates address a texel: 1, 2 or 3; a cube's direction is 3.
    int dim;
    bool is_array;
    bool is_cube;
    bool is_ms;
};

shape_traits traits_of(texture_shape s)
{
    auto const& c = check::info_of(s);
    auto const is_cube = s == texture_shape::cube || s == texture_shape::cube_array;
    auto const dim = s == texture_shape::d1 || s == texture_shape::d1_array ? 1
                   : s == texture_shape::d3 || is_cube                      ? 3
                                                                            : 2;
    return {.s = s,
            .texture = c.texture,
            .depth = c.depth,
            .image = c.image,
            .sg_name = c.sg_name,
            .dim = dim,
            .is_array = s == texture_shape::d1_array || s == texture_shape::d2_array || s == texture_shape::d2_ms_array
                     || s == texture_shape::cube_array,
            .is_cube = is_cube,
            .is_ms = s == texture_shape::d2_ms || s == texture_shape::d2_ms_array};
}

/// One texture call, packed into a record's `spelling::data`.
struct call
{
    op what = op::sample;
    texture_shape where = texture_shape::d2;
    /// The texel's width, 1 to 4; what a sample of a depth texture gives is 1.
    int width = 4;
    value_kind kind = value_kind::scalar_float;
    bool is_depth = false;
    bool has_offset = false;

    [[nodiscard]] u32 pack() const
    {
        return u32(what) | u32(where) << 5 | u32(width) << 9 | u32(kind) << 12 | u32(is_depth) << 15
             | u32(has_offset) << 16;
    }
    [[nodiscard]] static call unpack(u32 d)
    {
        return {.what = op(d & 31u),
                .where = texture_shape((d >> 5) & 15u),
                .width = int((d >> 9) & 7u),
                .kind = value_kind((d >> 12) & 7u),
                .is_depth = ((d >> 15) & 1u) != 0,
                .has_offset = ((d >> 16) & 1u) != 0};
    }
};

// ---- where each argument stands, which the signature and every writer agree on ----------------------------------

/// Positions in a call's arguments; -1 for one the call does not take.
struct argument_layout
{
    int sampler = -1;
    int layer = -1;
    /// The level, the bias, the component, the reference, the sample or the value, by what the call is.
    int first = -1;
    /// The second gradient, or a compare's level.
    int second = -1;
    int offset = -1;
};

bool takes_sampler(op o)
{
    return o == op::sample || o == op::sample_level || o == op::sample_bias || o == op::sample_grad || o == op::gather
        || o == op::sample_compare || o == op::sample_compare_level || o == op::gather_compare;
}

argument_layout layout_of(call const& c)
{
    auto const& s = traits_of(c.where);
    auto result = argument_layout();
    auto next = 2;
    if (c.what == op::image_store)
    {
        result.first = next++;
        if (s.is_array)
            result.layer = next++;
        return result;
    }
    if (c.what == op::load)
    {
        // a level, or a multisampled texture's sample, stands before the layer, so `t.load(xy, 1)` needs no name
        result.first = next++;
        if (s.is_array)
            result.layer = next++;
        return result;
    }
    if (takes_sampler(c.what))
        result.sampler = next++;
    if (s.is_array && c.what != op::size && c.what != op::layer_count && c.what != op::level_count
        && c.what != op::sample_count && c.what != op::image_size && c.what != op::image_layer_count)
        result.layer = next++;
    switch (c.what)
    {
    case op::sample_level:
    case op::sample_bias:
    case op::sample_compare:
    case op::gather_compare:
        result.first = next++;
        break;
    case op::gather:
        if (!c.is_depth)
            result.first = next++;
        break;
    case op::sample_grad:
    case op::sample_compare_level:
        result.first = next++;
        result.second = next++;
        break;
    case op::size:
        if (!s.is_ms)
            result.first = 1;
        break;
    default:
        break;
    }
    if (c.has_offset)
        result.offset = next++;
    return result;
}

// ---- the interpreter: no texture has texels it could read, so every read gives zeros ------------------------------

template <int Width, value_kind Kind>
void zeros(leaves, result& out)
{
    for (auto i = 0; i < Width; ++i)
        out.push_back({.kind = Kind, .bits = 0});
}
void zero_int(leaves, result& out)
{
    out.push_back(scalar::of(0));
}
void zero_int2(leaves, result& out)
{
    zeros<2, value_kind::scalar_int>({}, out);
}
void zero_int3(leaves, result& out)
{
    zeros<3, value_kind::scalar_int>({}, out);
}
void nothing(leaves, result&)
{
}

evaluator zeros_of(int width, value_kind kind)
{
    constexpr evaluator floats[] = {zeros<1, value_kind::scalar_float>, zeros<2, value_kind::scalar_float>,
                                    zeros<3, value_kind::scalar_float>, zeros<4, value_kind::scalar_float>};
    constexpr evaluator ints[] = {zeros<1, value_kind::scalar_int>, zeros<2, value_kind::scalar_int>,
                                  zeros<3, value_kind::scalar_int>, zeros<4, value_kind::scalar_int>};
    constexpr evaluator uints[] = {zeros<1, value_kind::scalar_uint>, zeros<2, value_kind::scalar_uint>,
                                   zeros<3, value_kind::scalar_uint>, zeros<4, value_kind::scalar_uint>};
    auto const& table = kind == value_kind::scalar_float ? floats : kind == value_kind::scalar_int ? ints : uints;
    return table[width - 1];
}

// ---- shared spelling helpers -----------------------------------------------------------------------------------

cc::string type_name(cc::string_view stem, int width)
{
    return width == 1 ? cc::string(stem) : cc::format("{}{}", stem, width);
}

cc::string_view stem_of(value_kind kind)
{
    return kind == value_kind::scalar_float ? "float" : kind == value_kind::scalar_int ? "int" : "uint";
}

/// WGSL and MSL give four channels, so a narrower texel takes the ones it has.
cc::string narrowed(cc::string text, int width)
{
    constexpr cc::string_view swizzles[] = {"", ".x", ".xy", ".xyz", ""};
    return cc::format("{}{}", text, swizzles[width]);
}

/// A value padded to four channels with zeros of its kind, as a WGSL or MSL store takes it.
cc::string padded(cc::string_view value, int width, value_kind kind, cc::string_view vector)
{
    auto const zero = kind == value_kind::scalar_float ? "0.0" : kind == value_kind::scalar_int ? "0" : "0u";
    auto text = cc::string(value);
    for (auto i = width; i < 4; ++i)
        text.appendf(", {}", zero);
    return width == 4 ? cc::string(value) : cc::format("{}({})", vector, text);
}

cc::string const& argument(call_context const& ctx, int index)
{
    return ctx.arguments[index].text;
}

/// A gather's component, which the check pass holds to a constant (CHK-280), so it arrives as a literal: 0 to 3.
int component_of(call_context const& ctx, argument_layout const& l)
{
    auto const& text = argument(ctx, l.first);
    CC_ASSERT(text.size() == 1 && text[0] >= '0' && text[0] <= '3', "a gather component that is no literal case");
    return text[0] - '0';
}

// ---- HLSL ------------------------------------------------------------------------------------------------------

/// A texture object's coordinate with its layer, as HLSL packs it: `float3(uv, layer)`.
cc::string hlsl_sample_coordinate(call const& c, call_context const& ctx, argument_layout const& l)
{
    auto const& s = traits_of(c.where);
    if (!s.is_array)
        return argument(ctx, 1);
    return cc::format("float{}({}, float({}))", s.dim + 1, argument(ctx, 1), argument(ctx, l.layer));
}

cc::string hlsl_offset(argument_layout const& l, call_context const& ctx)
{
    return l.offset >= 0 ? cc::format(", {}", argument(ctx, l.offset)) : cc::string();
}

written hlsl_call(call const& c, call_context const& ctx)
{
    auto const& s = traits_of(c.where);
    auto const l = layout_of(c);
    auto const& t = argument(ctx, 0);
    auto const smp = l.sampler >= 0 ? argument(ctx, l.sampler) : cc::string();
    auto const coord = takes_sampler(c.what) ? hlsl_sample_coordinate(c, ctx, l) : cc::string();
    switch (c.what)
    {
    case op::sample:
        return {.text = cc::format("{}.Sample({}, {}{})", t, smp, coord, hlsl_offset(l, ctx))};
    case op::sample_level:
    {
        // a depth texture's level is an int, as WGSL takes it
        auto const& level = argument(ctx, l.first);
        return {.text = cc::format("{}.SampleLevel({}, {}, {}{})", t, smp, coord,
                                   c.is_depth ? cc::format("float({})", level) : level, hlsl_offset(l, ctx))};
    }
    case op::sample_bias:
        return {.text = cc::format("{}.SampleBias({}, {}, {})", t, smp, coord, argument(ctx, l.first))};
    case op::sample_grad:
        return {.text = cc::format("{}.SampleGrad({}, {}, {}, {})", t, smp, coord, argument(ctx, l.first),
                                   argument(ctx, l.second))};
    case op::gather:
        if (c.is_depth)
            return {.text = cc::format("{}.Gather({}, {})", t, smp, coord)};
        {
            constexpr cc::string_view channels[] = {"Red", "Green", "Blue", "Alpha"};
            return {.text = cc::format("{}.Gather{}({}, {}{})", t, channels[component_of(ctx, l)], smp, coord,
                                       hlsl_offset(l, ctx))};
        }
    case op::sample_compare:
        return {.text = cc::format("{}.SampleCmp({}, {}, {})", t, smp, coord, argument(ctx, l.first))};
    case op::sample_compare_level:
        return {.text = cc::format("{}.SampleCmpLevelZero({}, {}, {})", t, smp, coord, argument(ctx, l.first))};
    case op::gather_compare:
        return {.text = cc::format("{}.GatherCmp({}, {}, {})", t, smp, coord, argument(ctx, l.first))};
    case op::load:
    {
        auto const& xy = argument(ctx, 1);
        auto const packed = s.is_array ? cc::format("{}, {}", xy, argument(ctx, l.layer)) : cc::string(xy);
        auto const lanes = s.dim + (s.is_array ? 1 : 0);
        if (s.is_ms)
            return {.text = cc::format("{}.Load({}, {})", t, lanes == 2 ? cc::string(xy) : cc::format("int3({})", packed),
                                       argument(ctx, l.first))};
        return {.text = cc::format("{}.Load(int{}({}, {}))", t, lanes + 1, packed, argument(ctx, l.first))};
    }
    case op::size:
    case op::image_size:
        return {.text = l.first >= 0 ? cc::format("sgl_size({}, {})", t, argument(ctx, l.first))
                                     : cc::format("sgl_size({})", t)};
    case op::layer_count:
    case op::image_layer_count:
        return {.text = cc::format("sgl_layers({})", t)};
    case op::level_count:
        return {.text = cc::format("sgl_levels({})", t)};
    case op::sample_count:
        return {.text = cc::format("sgl_samples({})", t)};
    case op::image_load:
    case op::image_store:
    {
        auto const& xy = argument(ctx, 1);
        auto const index
            = s.is_array ? cc::format("int{}({}, {})", s.dim + 1, xy, argument(ctx, l.layer)) : cc::string(xy);
        if (c.what == op::image_load)
            return {.text = cc::format("{}[{}]", t, index)};
        return {.text = cc::format("{}[{}] = {}", t, index, argument(ctx, l.first))};
    }
    }
    return {};
}

// ---- WGSL ------------------------------------------------------------------------------------------------------

/// WGSL has 1D textures only as 2D ones one texel high (EMIT-100), so a 1D coordinate gains a second.
cc::string wgsl_sample_coordinate(shape_traits const& s, cc::string const& coord)
{
    return s.dim == 1 ? cc::format("vec2f({}, 0.5)", coord) : cc::string(coord);
}
cc::string wgsl_texel_coordinate(shape_traits const& s, cc::string const& coord)
{
    return s.dim == 1 ? cc::format("vec2i({}, 0)", coord) : cc::string(coord);
}
cc::string wgsl_gradient(shape_traits const& s, cc::string const& g)
{
    return s.dim == 1 ? cc::format("vec2f({}, 0.0)", g) : cc::string(g);
}

written wgsl_call(call const& c, call_context const& ctx)
{
    auto const& s = traits_of(c.where);
    auto const l = layout_of(c);
    auto const& t = argument(ctx, 0);
    auto const smp = l.sampler >= 0 ? argument(ctx, l.sampler) : cc::string();
    auto const coord = ctx.arguments.size() > 1 ? wgsl_sample_coordinate(s, argument(ctx, 1)) : cc::string();
    auto const layer = l.layer >= 0 ? cc::format(", {}", argument(ctx, l.layer)) : cc::string();
    auto const offset = l.offset >= 0
                          ? cc::format(", {}", s.dim == 1 ? cc::format("vec2i({}, 0)", argument(ctx, l.offset))
                                                          : argument(ctx, l.offset))
                          : cc::string();
    auto const texel = [&](cc::string text) -> written
    { return {.text = c.is_depth ? cc::move(text) : narrowed(cc::move(text), c.width)}; };
    switch (c.what)
    {
    case op::sample:
        return texel(cc::format("textureSample({}, {}, {}{}{})", t, smp, coord, layer, offset));
    case op::sample_level:
        return texel(
            cc::format("textureSampleLevel({}, {}, {}{}, {}{})", t, smp, coord, layer, argument(ctx, l.first), offset));
    case op::sample_bias:
        return texel(cc::format("textureSampleBias({}, {}, {}{}, {})", t, smp, coord, layer, argument(ctx, l.first)));
    case op::sample_grad:
        return texel(cc::format("textureSampleGrad({}, {}, {}{}, {}, {})", t, smp, coord, layer,
                                wgsl_gradient(s, argument(ctx, l.first)), wgsl_gradient(s, argument(ctx, l.second))));
    case op::gather:
        if (c.is_depth)
            return {.text = cc::format("textureGather({}, {}, {}{})", t, smp, coord, layer)};
        return {.text
                = cc::format("textureGather({}, {}, {}, {}{}{})", component_of(ctx, l), t, smp, coord, layer, offset)};
    case op::sample_compare:
        return {.text
                = cc::format("textureSampleCompare({}, {}, {}{}, {})", t, smp, coord, layer, argument(ctx, l.first))};
    case op::sample_compare_level:
        return {.text = cc::format("textureSampleCompareLevel({}, {}, {}{}, {})", t, smp, coord, layer,
                                   argument(ctx, l.first))};
    case op::gather_compare:
        return {.text
                = cc::format("textureGatherCompare({}, {}, {}{}, {})", t, smp, coord, layer, argument(ctx, l.first))};
    case op::load:
        return texel(cc::format("textureLoad({}, {}{}, {})", t, wgsl_texel_coordinate(s, argument(ctx, 1)), layer,
                                argument(ctx, l.first)));
    case op::size:
    case op::image_size:
    {
        auto const level = l.first >= 0 ? cc::format(", {}", argument(ctx, l.first)) : cc::string();
        auto const dims = cc::format("textureDimensions({}{})", t, level);
        if (s.dim == 1)
            return {.text = cc::format("i32({}.x)", dims)};
        return {.text = cc::format("vec{}i({})", s.is_cube ? 2 : s.dim, dims)};
    }
    case op::layer_count:
    case op::image_layer_count:
        return {.text = cc::format("i32(textureNumLayers({}))", t)};
    case op::level_count:
        return {.text = cc::format("i32(textureNumLevels({}))", t)};
    case op::sample_count:
        return {.text = cc::format("i32(textureNumSamples({}))", t)};
    case op::image_load:
        return texel(cc::format("textureLoad({}, {}{})", t, wgsl_texel_coordinate(s, argument(ctx, 1)), layer));
    case op::image_store:
    {
        auto const vector = c.kind == value_kind::scalar_float ? "vec4f"
                          : c.kind == value_kind::scalar_int   ? "vec4i"
                                                               : "vec4u";
        return {.text = cc::format("textureStore({}, {}{}, {})", t, wgsl_texel_coordinate(s, argument(ctx, 1)), layer,
                                   padded(argument(ctx, l.first), c.width, c.kind, vector))};
    }
    }
    return {};
}

// ---- MSL -------------------------------------------------------------------------------------------------------

cc::string msl_gradient(shape_traits const& s, cc::string const& dx, cc::string const& dy)
{
    if (s.is_cube)
        return cc::format(", gradientcube({}, {})", dx, dy);
    return s.dim == 3 ? cc::format(", gradient3d({}, {})", dx, dy) : cc::format(", gradient2d({}, {})", dx, dy);
}

written msl_call(call const& c, call_context const& ctx)
{
    auto const& s = traits_of(c.where);
    auto const l = layout_of(c);
    auto const& t = argument(ctx, 0);
    auto const smp = l.sampler >= 0 ? argument(ctx, l.sampler) : cc::string();
    auto const coord = ctx.arguments.size() > 1 ? argument(ctx, 1) : cc::string();
    auto const layer = l.layer >= 0 ? cc::format(", uint({})", argument(ctx, l.layer)) : cc::string();
    auto const offset = l.offset >= 0 ? cc::format(", {}", argument(ctx, l.offset)) : cc::string();
    auto const texel = [&](cc::string text) -> written
    { return {.text = c.is_depth ? cc::move(text) : narrowed(cc::move(text), c.width)}; };
    // Metal's 1D textures have no levels, so their samples take no level option.
    auto const has_levels = s.dim != 1;
    auto const uint_coord = s.dim == 1 ? cc::format("uint({})", coord) : cc::format("uint{}({})", s.dim, coord);
    switch (c.what)
    {
    case op::sample:
        return texel(cc::format("{}.sample({}, {}{}{})", t, smp, coord, layer, offset));
    case op::sample_level:
    {
        auto const& lod = argument(ctx, l.first);
        auto const level = c.is_depth ? cc::format(", level(float({}))", lod) : cc::format(", level({})", lod);
        return texel(
            cc::format("{}.sample({}, {}{}{}{})", t, smp, coord, layer, has_levels ? level : cc::string(), offset));
    }
    case op::sample_bias:
        return texel(cc::format("{}.sample({}, {}{}{})", t, smp, coord, layer,
                                has_levels ? cc::format(", bias({})", argument(ctx, l.first)) : cc::string()));
    case op::sample_grad:
        return texel(
            cc::format("{}.sample({}, {}{}{})", t, smp, coord, layer,
                       has_levels ? msl_gradient(s, argument(ctx, l.first), argument(ctx, l.second)) : cc::string()));
    case op::gather:
        if (c.is_depth)
            return {.text = cc::format("{}.gather({}, {}{})", t, smp, coord, layer)};
        {
            // a cube's gather takes no offset, and every other shape's takes one before the component
            constexpr cc::string_view channels[] = {"x", "y", "z", "w"};
            auto const before = s.is_cube ? cc::string() : l.offset >= 0 ? offset : cc::string(", int2(0)");
            return {.text = cc::format("{}.gather({}, {}{}{}, component::{})", t, smp, coord, layer, before,
                                       channels[component_of(ctx, l)])};
        }
    case op::sample_compare:
        return {.text = cc::format("{}.sample_compare({}, {}{}, {})", t, smp, coord, layer, argument(ctx, l.first))};
    case op::sample_compare_level:
        return {.text
                = cc::format("{}.sample_compare({}, {}{}, {}, level(0))", t, smp, coord, layer, argument(ctx, l.first))};
    case op::gather_compare:
        return {.text = cc::format("{}.gather_compare({}, {}{}, {})", t, smp, coord, layer, argument(ctx, l.first))};
    case op::load:
        return texel(cc::format("{}.read({}{}, uint({}))", t, uint_coord, layer, argument(ctx, l.first)));
    case op::size:
    case op::image_size:
    {
        auto const level = l.first >= 0 && has_levels ? cc::string(argument(ctx, l.first)) : cc::string();
        if (s.dim == 1)
            return {.text = cc::format("int({}.get_width())", t)};
        auto const w = cc::format("{}.get_width({})", t, level);
        auto const h = cc::format("{}.get_height({})", t, level);
        if (s.dim == 3 && !s.is_cube)
            return {.text = cc::format("int3({}, {}, {}.get_depth({}))", w, h, t, level)};
        return {.text = cc::format("int2({}, {})", w, h)};
    }
    case op::layer_count:
    case op::image_layer_count:
        return {.text = cc::format("int({}.get_array_size())", t)};
    case op::level_count:
        return {.text = cc::format("int({}.get_num_mip_levels())", t)};
    case op::sample_count:
        return {.text = cc::format("int({}.get_num_samples())", t)};
    case op::image_load:
        return texel(cc::format("{}.read({}{})", t, uint_coord, layer));
    case op::image_store:
    {
        auto const vector = cc::format("{}4", stem_of(c.kind));
        return {.text = cc::format("{}.write({}, {}{})", t, padded(argument(ctx, l.first), c.width, c.kind, vector),
                                   uint_coord, layer)};
    }
    }
    return {};
}

// Every name a texture call writes, one list per target for the whole family.
constexpr cc::string_view k_textures_hlsl[] = {"sgl_layers", "sgl_levels", "sgl_samples", "sgl_size"};
constexpr cc::string_view k_textures_wgsl[] = {
    "textureDimensions",  "textureGather",        "textureGatherCompare",      "textureLoad",
    "textureNumLayers",   "textureNumLevels",     "textureNumSamples",         "textureSample",
    "textureSampleBias",  "textureSampleCompare", "textureSampleCompareLevel", "textureSampleGrad",
    "textureSampleLevel", "textureStore",
};
constexpr cc::string_view k_textures_msl[] = {"bias", "component", "gradient2d", "gradient3d", "gradientcube", "level"};

written write_texture_call(call_context const& ctx)
{
    auto const c = call::unpack(ctx.data);
    switch (ctx.target)
    {
    case language::hlsl:
        return hlsl_call(c, ctx);
    case language::wgsl:
        return wgsl_call(c, ctx);
    case language::msl:
        return msl_call(c, ctx);
    }
    return {};
}

// ---- HLSL's sizes, which it gives through out parameters, as helpers the text declares -------------------------

/// HLSL's `GetDimensions` writes through out parameters, so a size is a helper per texture type a call passes.
cc::string size_helper(helper_context const& h)
{
    if (h.target != language::hlsl)
        return {};
    auto const c = call::unpack(h.data);
    auto const& s = traits_of(c.where);
    auto const& type = h.argument_types[0];
    auto const is_image = c.what == op::image_size || c.what == op::image_layer_count;
    // what GetDimensions writes, in the order it writes them, which differs by shape
    auto outs = cc::vector<cc::string_view>{"width"};
    if (s.dim >= 2)
        outs.push_back("height");
    if (s.dim == 3 && !s.is_cube)
        outs.push_back("depth");
    if (s.is_array)
        outs.push_back("layers");
    if (s.is_ms)
        outs.push_back("samples");
    else if (!is_image)
        outs.push_back("levels");
    auto declared = cc::string();
    auto names = cc::string();
    for (auto const& o : outs)
    {
        declared.appendf("{}{}", declared.empty() ? "uint " : ", ", o);
        names.appendf("{}{}", names.empty() ? "" : ", ", o);
    }
    auto const listed = cc::format(", {}", names);
    auto const takes_level = !is_image && !s.is_ms;
    auto const call_args = takes_level ? cc::format("uint(level){}", listed) : names;
    auto const head = [&](cc::string_view result, cc::string_view name, bool level)
    {
        return cc::format("{} {}({} t{})\n{{\n    {};\n    t.GetDimensions({});\n", result, name, type,
                          level ? ", int level" : "", declared, call_args);
    };

    switch (c.what)
    {
    case op::size:
    case op::image_size:
    {
        auto const dims = s.dim == 3 && !s.is_cube ? 3 : s.dim == 1 ? 1 : 2;
        auto const value = dims == 1 ? cc::string("int(width)")
                         : dims == 2 ? cc::string("int2(width, height)")
                                     : cc::string("int3(width, height, depth)");
        auto const name = dims == 1 ? "int" : dims == 2 ? "int2" : "int3";
        if (takes_level)
            return head(name, "sgl_size", true) + cc::format("    return {};\n}}\n", value);
        return head(name, "sgl_size", false) + cc::format("    return {};\n}}\n", value);
    }
    case op::layer_count:
    case op::image_layer_count:
        return takes_level ? cc::format("int sgl_layers({} t)\n{{\n    {};\n    t.GetDimensions(0{});\n    return "
                                        "int(layers);\n}}\n",
                                        type, declared, listed)
                           : head("int", "sgl_layers", false) + "    return int(layers);\n}\n";
    case op::level_count:
        return cc::format("int sgl_levels({} t)\n{{\n    {};\n    t.GetDimensions(0{});\n    return int(levels);\n}}\n",
                          type, declared, listed);
    case op::sample_count:
        return head("int", "sgl_samples", false) + "    return int(samples);\n}\n";
    default:
        return {};
    }
}

// ---- the records -------------------------------------------------------------------------------------------------

cc::string coordinate_type(shape_traits const& s, bool is_float)
{
    return type_name(is_float ? "float" : "int", s.dim);
}

cc::string offset_type(shape_traits const& s)
{
    return type_name("int", s.dim);
}

/// Metal samples a 1D texture with no offset, so no target does.
bool takes_offset(shape_traits const& s)
{
    return !s.is_cube && !s.is_ms && s.dim != 1;
}

struct emitted
{
    cc::string signature;
    evaluator evaluate;
    bool uses_derivatives = false;
    helper_writer helper = nullptr;
};

/// The signature of one texture call, with or without its sampler.
emitted texture_record(call const& c, cc::string_view texture_type, bool with_sampler)
{
    auto const& s = traits_of(c.where);
    auto const texel = c.is_depth ? cc::string("float") : type_name(stem_of(c.kind), c.width);
    auto params = cc::format("t: {}, coord: {}", texture_type, coordinate_type(s, c.what != op::load));
    auto const sampler_type
        = c.what == op::sample_compare || c.what == op::sample_compare_level || c.what == op::gather_compare
            ? "comparison_sampler"
            : "sampler";
    if (with_sampler && takes_sampler(c.what))
        params.appendf(", smp: {}", sampler_type);
    auto result_type = texel;
    auto attributes = cc::string("@pure");
    auto uses_derivatives = false;
    helper_writer helper = nullptr;
    switch (c.what)
    {
    case op::sample:
        if (s.is_array)
            params += ", .layer: int";
        attributes += " @stages(.pixel)";
        uses_derivatives = true;
        break;
    case op::sample_level:
        // WGSL takes a depth texture's level as an integer, so every target is given one
        params.appendf("{}, .level: {}", s.is_array ? ", .layer: int" : "", c.is_depth ? "int" : "float");
        break;
    case op::sample_bias:
        params.appendf("{}, .bias: float", s.is_array ? ", .layer: int" : "");
        attributes += " @stages(.pixel)";
        uses_derivatives = true;
        break;
    case op::sample_grad:
        params.appendf("{}, .grad_x: {}, .grad_y: {}", s.is_array ? ", .layer: int" : "", coordinate_type(s, true),
                       coordinate_type(s, true));
        break;
    case op::gather:
        params.appendf("{}{}", s.is_array ? ", .layer: int" : "",
                       c.is_depth ? "" : ", .component: texel_component = texel_component.x");
        result_type = "float4";
        break;
    case op::sample_compare:
        params.appendf("{}, .reference: float", s.is_array ? ", .layer: int" : "");
        attributes += " @stages(.pixel)";
        uses_derivatives = true;
        break;
    case op::sample_compare_level:
        params.appendf("{}, .reference: float, .level: float", s.is_array ? ", .layer: int" : "");
        break;
    case op::gather_compare:
        params.appendf("{}, .reference: float", s.is_array ? ", .layer: int" : "");
        result_type = "float4";
        break;
    case op::load:
        params = cc::format("t: {}, xy: {}", texture_type, coordinate_type(s, false));
        params += s.is_ms ? ", .sample: int" : ", level: int = 0";
        if (s.is_array)
            params += ", .layer: int";
        break;
    default:
        break;
    }
    if (c.has_offset)
        params.appendf(", .offset: {}", offset_type(s));
    auto const width = c.is_depth ? 1 : result_type == "float4" ? 4 : c.width;
    auto const kind = result_type == "float4" || c.is_depth ? value_kind::scalar_float : c.kind;
    return {.signature = cc::format("{} fun {}({}) -> {}", attributes,
                                    c.what == op::sample_level || c.what == op::sample_bias || c.what == op::sample_grad
                                        ? cc::string_view("sample")
                                    : c.what == op::sample_compare_level ? cc::string_view("sample_compare")
                                    : c.what == op::gather               ? cc::string_view("gather")
                                    : c.what == op::sample_compare       ? cc::string_view("sample_compare")
                                    : c.what == op::gather_compare       ? cc::string_view("gather_compare")
                                    : c.what == op::load                 ? cc::string_view("load")
                                                                         : cc::string_view("sample"),
                                    params, result_type),
            .evaluate = zeros_of(width, kind),
            .uses_derivatives = uses_derivatives,
            .helper = helper};
}

/// A texture call, and its twin without a sampler for a texture whose `@sampler` supplies one (CHK-279).
void add_texture_call(registry& r, call const& c, cc::string_view texture_type)
{
    auto const with = texture_record(c, texture_type, true);
    auto const spelled = spelling{.kind = spelling_kind::custom,
                                  .custom = write_texture_call,
                                  .helper = with.helper,
                                  .data = c.pack(),
                                  .hlsl_names = k_textures_hlsl,
                                  .wgsl_names = k_textures_wgsl,
                                  .msl_names = k_textures_msl};
    auto const id = r.add(function_record{.signature = with.signature,
                                          .evaluate = with.evaluate,
                                          .write = spelled,
                                          .uses_derivatives = with.uses_derivatives});
    if (!takes_sampler(c.what))
        return;
    auto const without = texture_record(c, texture_type, false);
    r.add(function_record{.signature = without.signature,
                          .evaluate = with.evaluate,
                          .write = spelled,
                          .with_default_sampler = id,
                          .uses_derivatives = with.uses_derivatives});
}

void add_sampled_shape(registry& r, shape_traits const& s)
{
    // loads of every component type and width; samples and gathers of floats alone, since only they filter
    for (auto const kind : {value_kind::scalar_float, value_kind::scalar_int, value_kind::scalar_uint})
        for (auto width = 1; width <= 4; ++width)
        {
            auto const texture_type = cc::format("{}[{}]", s.texture, type_name(stem_of(kind), width));
            auto const base = call{.where = s.s, .width = width, .kind = kind};
            if (!s.is_cube)
            {
                auto load = base;
                load.what = op::load;
                add_texture_call(r, load, texture_type);
            }
            if (kind != value_kind::scalar_float || s.is_ms)
                continue;
            for (auto const o : {op::sample, op::sample_level, op::sample_bias, op::sample_grad})
            {
                auto sampled = base;
                sampled.what = o;
                add_texture_call(r, sampled, texture_type);
                if (takes_offset(s) && (o == op::sample || o == op::sample_level))
                {
                    sampled.has_offset = true;
                    add_texture_call(r, sampled, texture_type);
                }
            }
            // a gather reads four texels of 2D or cube shapes on every target
            if (s.dim >= 2 && (s.s == texture_shape::d2 || s.s == texture_shape::d2_array || s.is_cube))
            {
                auto gathered = base;
                gathered.what = op::gather;
                add_texture_call(r, gathered, texture_type);
                if (takes_offset(s))
                {
                    gathered.has_offset = true;
                    add_texture_call(r, gathered, texture_type);
                }
            }
        }
}

void add_depth_shape(registry& r, shape_traits const& s)
{
    auto const base = call{.where = s.s, .width = 1, .kind = value_kind::scalar_float, .is_depth = true};
    if (!s.is_cube)
    {
        auto load = base;
        load.what = op::load;
        add_texture_call(r, load, s.depth);
    }
    if (s.is_ms)
        return;
    for (auto const o :
         {op::sample, op::sample_level, op::gather, op::sample_compare, op::sample_compare_level, op::gather_compare})
    {
        auto sampled = base;
        sampled.what = o;
        add_texture_call(r, sampled, s.depth);
    }
}

void add_image_shape(registry& r, shape_traits const& s)
{
    auto const layer = s.is_array ? ", .layer: int" : "";
    for (auto const kind : {value_kind::scalar_float, value_kind::scalar_int, value_kind::scalar_uint})
        for (auto width = 1; width <= 4; ++width)
        {
            auto const texel = type_name(stem_of(kind), width);
            auto const c = call{.where = s.s, .width = width, .kind = kind};
            auto load = c;
            load.what = op::image_load;
            // An image is read where another invocation may have written it, so its load keeps its place: no @pure.
            r.add(function_record{
                .signature = cc::format("fun load(i: {}[{}], xy: {}{}) -> {}", s.image, texel,
                                        coordinate_type(s, false), layer, texel),
                .evaluate = zeros_of(width, kind),
                .write = {.kind = spelling_kind::custom,
                          .custom = write_texture_call,
                          .data = load.pack(),
                          .hlsl_names = k_textures_hlsl,
                          .wgsl_names = k_textures_wgsl,
                          .msl_names = k_textures_msl},
            });
            auto store = c;
            store.what = op::image_store;
            // Core WebGPU has no writable storage in a vertex stage.
            r.add(function_record{
                .signature = cc::format("@stages(.pixel, .compute) fun store(i: out {}[{}], xy: {}, value: {}{})",
                                        s.image, texel, coordinate_type(s, false), texel, layer),
                .evaluate = nothing,
                .write = {.kind = spelling_kind::custom,
                          .custom = write_texture_call,
                          .data = store.pack(),
                          .hlsl_names = k_textures_hlsl,
                          .wgsl_names = k_textures_wgsl,
                          .msl_names = k_textures_msl},
            });
        }
}

void add_sizes(registry& r, shape_traits const& s, cc::string_view type_name_text, bool is_image)
{
    auto const dims = s.dim == 3 && !s.is_cube ? 3 : s.dim == 1 ? 1 : 2;
    auto const result = dims == 1 ? "int" : dims == 2 ? "int2" : "int3";
    auto const evaluate = dims == 1 ? zero_int : dims == 2 ? zero_int2 : zero_int3;
    auto const spelled = [&](op o)
    {
        auto const c = call{.what = o, .where = s.s, .width = 1};
        return spelling{.kind = spelling_kind::custom,
                        .custom = write_texture_call,
                        .helper = size_helper,
                        .data = c.pack(),
                        .hlsl_names = k_textures_hlsl,
                        .wgsl_names = k_textures_wgsl,
                        .msl_names = k_textures_msl};
    };
    auto const takes_level = !is_image && !s.is_ms;
    r.add(function_record{
        .signature
        = cc::format("@pure fun size(t: {}{}) -> {}", type_name_text, takes_level ? ", level: int = 0" : "", result),
        .evaluate = evaluate,
        .write = spelled(is_image ? op::image_size : op::size),
    });
    if (s.is_array)
        r.add(function_record{
            .signature = cc::format("@pure fun layer_count(t: {}) -> int", type_name_text),
            .evaluate = zero_int,
            .write = spelled(is_image ? op::image_layer_count : op::layer_count),
        });
    if (is_image)
        return;
    if (s.is_ms)
        r.add(function_record{
            .signature = cc::format("@pure fun sample_count(t: {}) -> int", type_name_text),
            .evaluate = zero_int,
            .write = spelled(op::sample_count),
        });
    else
        r.add(function_record{
            .signature = cc::format("@pure fun level_count(t: {}) -> int", type_name_text),
            .evaluate = zero_int,
            .write = spelled(op::level_count),
        });
}
} // namespace

namespace
{
/// Its argument unchanged: what the mark changes is how the index into a binding array is written.
written write_nonuniform(call_context const& ctx)
{
    return ctx.arguments[0];
}

void identity(leaves in, result& out)
{
    out.push_back(in[0]);
}
} // namespace

void sgl::builtins::register_textures(registry& r)
{
    r.add_comment(
        "// Textures and images, called as methods: `tex.sample(uv, smp)` is `sample(tex, uv, smp)`.\n"
        "// A texture samples, gathers and loads; an image loads where the shader may read it and stores where "
        "it may write it.\n"
        "// A sample takes its level from derivatives, or from a `level`, `bias` or `grad_x` / `grad_y` named "
        "at the call.\n"
        "// An array's layer is always named, and an offset is a constant from -8 to 7 (CHK-280).\n"
        "// Each sampling form has a twin without its sampler, which the texture's `@sampler` supplies "
        "(CHK-279).");

    // The component a gather reads.
    r.add(type_record{
        .declaration = "enum texel_component:\n    x\n    y\n    z\n    w",
        .doc = "/// Which channel of four texels a `gather` reads.",
        .hlsl = "int",
        .wgsl = "i32",
        .msl = "int",
        .leaf_kind = value_kind::scalar_int,
        .leaf_count = 1,
    });

    r.add_comment("// `nonuniform i` marks an index into a binding array that differs between invocations (CHK-300).");
    for (auto const index : {"int", "uint"})
        r.add(function_record{
            .signature = cc::format("@pure fun nonuniform(i: {0}) -> {0}", index),
            .doc = "/// `i`, which the hardware is told may differ between the invocations that index with it.",
            .evaluate = identity,
            .write = {.kind = spelling_kind::custom, .custom = write_nonuniform},
            .is_nonuniform_mark = true,
        });

    for (auto const& entry : check::k_shapes)
    {
        auto const s = traits_of(entry.shape);
        add_sampled_shape(r, s);
        add_sizes(r, s, s.texture, false);
        if (!s.depth.empty())
        {
            add_depth_shape(r, s);
            add_sizes(r, s, s.depth, false);
        }
        if (!s.image.empty())
        {
            add_image_shape(r, s);
            add_sizes(r, s, s.image, true);
        }
    }
}
