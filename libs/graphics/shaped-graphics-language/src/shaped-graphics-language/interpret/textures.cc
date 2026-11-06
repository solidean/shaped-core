#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>
#include <shaped-graphics-language/interpret/impl/textures.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::builtins::texture;

// EVAL-120 to EVAL-126: what a texture call reads of the texels a run binds.
// Every step is f32 arithmetic in the order the rules spell it, so two runs meet the same bits on every machine.

namespace
{
using impl::coordinate_gradients;
using impl::texture_failure;

texture_failure no_value(cc::string detail)
{
    return {.status = run_status::program_error, .detail = cc::move(detail)};
}

texture_failure malformed(cc::string detail)
{
    return {.status = run_status::type_error, .detail = cc::move(detail)};
}

/// Level `l`'s extent along an axis of `n` texels at level 0.
i32 at_level(i32 n, i32 l)
{
    auto const v = n >> l;
    return v < 1 ? 1 : v;
}

/// Where a texture's texels stand, and what it reads at a texel index.
struct texels
{
    texture_contents const& t;
    shape_axes axes;

    [[nodiscard]] i32 width(i32 l) const { return at_level(t.width, l); }
    [[nodiscard]] i32 height(i32 l) const { return axes.dim >= 2 ? at_level(t.height, l) : 1; }
    [[nodiscard]] i32 depth(i32 l) const { return axes.dim == 3 && !axes.is_cube ? at_level(t.depth, l) : 1; }

    [[nodiscard]] isize level_offset(i32 l) const
    {
        auto offset = isize(0);
        for (auto k = 0; k < l; ++k)
            offset += isize(width(k)) * height(k) * depth(k) * t.layers * t.samples * t.channels;
        return offset;
    }

    /// The first scalar of the texel, which every coordinate must already address.
    [[nodiscard]] isize offset_of(i32 l, i32 layer, i32 x, i32 y, i32 z, i32 sample) const
    {
        auto const in_level = ((((isize(layer) * depth(l) + z) * height(l) + y) * width(l) + x) * t.samples + sample);
        return level_offset(l) + in_level * t.channels;
    }

    [[nodiscard]] scalar at(i32 l, i32 layer, i32 x, i32 y, i32 z, i32 sample, i32 channel) const
    {
        return t.texels[offset_of(l, layer, x, y, z, sample) + channel];
    }
};

/// The texel index `i` an address mode makes of one outside `0 ..< n` (EVAL-123).
i32 addressed(i32 i, i32 n, u8 mode)
{
    switch (mode)
    {
    case 0: // repeat
        return ((i % n) + n) % n;
    case 1: // mirror_repeat
    {
        auto const t = ((i % (2 * n)) + 2 * n) % (2 * n);
        return t < n ? t : 2 * n - 1 - t;
    }
    default: // clamp_edge
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

f32 floor_of(f32 x)
{
    return f32(builtins::impl::soft_floor(f64(x)));
}

/// `a * (1 - w) + b * w`, the one interpolation every filter uses, which keeps `a` at 0 and `b` at 1 exactly.
f32 mixed(f32 a, f32 b, f32 w)
{
    return a * (1.0f - w) + b * w;
}

/// Whether `ref op texel` holds for a comparison sampler's op, in `k_compare_ops`' order (EVAL-125).
bool compared(i32 op, f32 ref, f32 texel)
{
    switch (op)
    {
    case 0:
        return false;
    case 1:
        return ref < texel;
    case 2:
        return ref == texel;
    case 3:
        return ref <= texel;
    case 4:
        return ref > texel;
    case 5:
        return ref != texel;
    case 6:
        return ref >= texel;
    default:
        return true;
    }
}

/// One sampling of `t` at one level: the face or layer, the coordinate in [0, 1) texture space, and what each texel
/// reads as a float; the result has `channels` floats.
struct level_sample
{
    texels const& t;
    sampler_state const& s;
    i32 level = 0;
    i32 layer = 0;
    /// The address modes per axis; a cube face's are clamp_edge whatever the sampler says (EVAL-126).
    u8 modes[3] = {0, 0, 0};
    i32 offset[3] = {0, 0, 0};
    i32 channels = 1;
    /// A comparison: each texel reads as 1 where `ref op texel` holds and 0 where not (EVAL-125).
    i32 compare = -1;
    f32 reference = 0.0f;

    [[nodiscard]] f32 read(i32 x, i32 y, i32 z, i32 channel) const
    {
        auto const v = t.at(level, layer, x, y, z, 0, compare >= 0 ? 0 : channel).as_float();
        return compare >= 0 ? (compared(compare, reference, v) ? 1.0f : 0.0f) : v;
    }

    /// The texels of the filter's footprint along one axis: `i0` and `i1` addressed, and the weight of `i1`.
    struct footprint
    {
        i32 i0 = 0;
        i32 i1 = 0;
        f32 weight = 0.0f;
    };

    [[nodiscard]] footprint along(f32 coord, i32 n, int axis, bool is_linear) const
    {
        auto const texel_space = coord * f32(n);
        if (!is_linear)
        {
            auto const i = addressed(i32(floor_of(texel_space)) + offset[axis], n, modes[axis]);
            return {.i0 = i, .i1 = i, .weight = 0.0f};
        }
        auto const shifted = texel_space - 0.5f;
        auto const base = floor_of(shifted);
        auto const i = i32(base) + offset[axis];
        return {.i0 = addressed(i, n, modes[axis]), .i1 = addressed(i + 1, n, modes[axis]), .weight = shifted - base};
    }

    void run(f32 const* coord, int dim, bool is_linear, f32* out) const
    {
        auto const u = along(coord[0], t.width(level), 0, is_linear);
        auto const v = dim >= 2 ? along(coord[1], t.height(level), 1, is_linear) : footprint();
        auto const w = dim >= 3 ? along(coord[2], t.depth(level), 2, is_linear) : footprint();
        for (auto ch = 0; ch < channels; ++ch)
        {
            auto const row = [&](i32 y, i32 z) { return mixed(read(u.i0, y, z, ch), read(u.i1, y, z, ch), u.weight); };
            auto const slice = [&](i32 z) { return mixed(row(v.i0, z), row(v.i1, z), v.weight); };
            out[ch] = mixed(slice(w.i0), slice(w.i1), w.weight);
        }
    }
};

/// A cube's face, 0 to 5 as +x, -x, +y, -y, +z, -z, and the coordinate on it, from a direction (EVAL-126).
void cube_face_of(f32 x, f32 y, f32 z, i32& face, f32& u, f32& v)
{
    auto const ax = x < 0.0f ? -x : x;
    auto const ay = y < 0.0f ? -y : y;
    auto const az = z < 0.0f ? -z : z;
    auto sc = 0.0f;
    auto tc = 0.0f;
    auto ma = 0.0f;
    if (az >= ax && az >= ay)
    {
        face = z >= 0.0f ? 4 : 5;
        sc = z >= 0.0f ? x : -x;
        tc = -y;
        ma = az;
    }
    else if (ay >= ax)
    {
        face = y >= 0.0f ? 2 : 3;
        sc = x;
        tc = y >= 0.0f ? z : -z;
        ma = ay;
    }
    else
    {
        face = x >= 0.0f ? 0 : 1;
        sc = x >= 0.0f ? -z : z;
        tc = -y;
        ma = ax;
    }
    u = 0.5f * (sc / ma + 1.0f);
    v = 0.5f * (tc / ma + 1.0f);
}

/// The scalars of argument `i`, which the check pass typed; empty where the call has none there.
cc::span<scalar const> arg(cc::span<value const> args, int i)
{
    if (i < 0 || i >= args.size())
        return {};
    return args[i].leaves;
}

i32 int_arg(cc::span<value const> args, int i, i32 fallback = 0)
{
    auto const a = arg(args, i);
    return a.empty() ? fallback : a[0].as_int();
}

texture_failure sizes(call const& c, texels const& t, cc::span<value const> args, cc::vector<scalar>& out)
{
    auto const& tex = t.t;
    auto const l = layout_of(c);
    switch (c.what)
    {
    case op::layer_count:
    case op::image_layer_count:
        out.push_back(scalar::of(t.axes.is_cube ? tex.layers / 6 : tex.layers));
        return {};
    case op::level_count:
        out.push_back(scalar::of(tex.width == 0 ? 0 : tex.levels));
        return {};
    case op::sample_count:
        out.push_back(scalar::of(tex.width == 0 ? 0 : tex.samples));
        return {};
    default:
        break;
    }
    auto const level = c.what == op::size ? int_arg(args, l.first) : 0;
    auto const dims = t.axes.dim == 3 && !t.axes.is_cube ? 3 : t.axes.dim == 1 ? 1 : 2;
    if (tex.width == 0)
    {
        for (auto i = 0; i < dims; ++i)
            out.push_back(scalar::of(0));
        return {};
    }
    if (level < 0 || level >= tex.levels)
        return no_value(cc::format("the size of level {} of a texture of {} levels", level, tex.levels));
    out.push_back(scalar::of(t.width(level)));
    if (dims >= 2)
        out.push_back(scalar::of(t.height(level)));
    if (dims == 3)
        out.push_back(scalar::of(t.depth(level)));
    return {};
}

/// A load's or a store's texel, every coordinate inside the texture or no value (EVAL-121).
texture_failure addressed_texel(call const& c, texels const& t, cc::span<value const> args, isize& offset)
{
    auto const& tex = t.t;
    auto const l = layout_of(c);
    auto const coord = arg(args, 1);
    i32 xyz[3] = {0, 0, 0};
    for (auto i = 0; i < t.axes.dim && i < coord.size(); ++i)
        xyz[i] = coord[i].as_int();
    auto const is_image = c.what == op::image_load || c.what == op::image_store;
    auto const level = is_image || t.axes.is_ms ? 0 : int_arg(args, l.first);
    auto const sample = t.axes.is_ms ? int_arg(args, l.first) : 0;
    auto const layer = t.axes.is_array ? int_arg(args, l.layer) : 0;
    if (level < 0 || level >= tex.levels)
        return no_value(cc::format("level {} of a texture of {} levels", level, tex.levels));
    i32 const extent[3] = {t.width(level), t.height(level), t.depth(level)};
    for (auto i = 0; i < t.axes.dim; ++i)
        if (xyz[i] < 0 || xyz[i] >= extent[i])
            return no_value(
                cc::format("the texel coordinate {} is outside 0 ..< {} at level {}", xyz[i], extent[i], level));
    if (layer < 0 || layer >= tex.layers)
        return no_value(cc::format("layer {} of a texture of {} layers", layer, tex.layers));
    if (sample < 0 || sample >= tex.samples)
        return no_value(cc::format("sample {} of a texture of {} samples", sample, tex.samples));
    offset = t.offset_of(level, layer, xyz[0], xyz[1], xyz[2], sample);
    return {};
}

/// The level a sample reads, from its explicit level or gradients, the sampler's bias and its clamps (EVAL-124).
/// An implicit level is from the quad's `gradients`; a run of one lane has none, so it is the lowest the clamps allow.
f32 lod_of(call const& c,
           texels const& t,
           sampler_state const& s,
           cc::span<value const> args,
           coordinate_gradients const* gradients)
{
    auto const l = layout_of(c);
    constexpr auto lowest = -3.4028235e38f;
    auto lod = lowest;
    if (c.what == op::sample_level || c.what == op::sample_compare_level)
    {
        // a depth texture's level is an int (WGSL's), and a compare's the literal 0.0
        auto const level_at = c.what == op::sample_compare_level ? l.second : l.first;
        auto const a = arg(args, level_at);
        lod = a.empty() ? 0.0f : a[0].kind == value_kind::scalar_float ? a[0].as_float() : f32(a[0].as_int());
    }
    else if (c.what == op::sample_grad
             || (gradients != nullptr
                 && (c.what == op::sample || c.what == op::sample_bias || c.what == op::sample_compare)))
    {
        auto const is_implicit = c.what != op::sample_grad;
        auto const gx = is_implicit ? cc::span<scalar const>(gradients->x) : arg(args, l.first);
        auto const gy = is_implicit ? cc::span<scalar const>(gradients->y) : arg(args, l.second);
        f32 const extent[3] = {f32(t.width(0)), f32(t.height(0)), f32(t.depth(0))};
        auto const length = [&](cc::span<scalar const> g)
        {
            auto sum = 0.0f;
            for (auto i = 0; i < g.size() && i < 3; ++i)
            {
                auto const d = g[i].as_float() * extent[i];
                sum += d * d;
            }
            return f32(builtins::impl::soft_sqrt(f64(sum)));
        };
        auto const rho = length(gx) > length(gy) ? length(gx) : length(gy);
        lod = rho > 0.0f ? f32(builtins::impl::soft_log(f64(rho)) / 0.693147180559945309417232121458176568) : lowest;
    }
    if (lod != lowest)
    {
        lod += s.mip_lod_bias;
        if (c.what == op::sample_bias)
            lod += arg(args, l.first)[0].as_float();
    }
    if (lod < s.min_lod)
        lod = s.min_lod;
    if (lod > s.max_lod)
        lod = s.max_lod;
    return lod;
}

texture_failure sampled(call const& c,
                        texels const& t,
                        sampler_state const& s,
                        cc::span<value const> args,
                        cc::vector<scalar>& out,
                        coordinate_gradients const* gradients)
{
    auto const& tex = t.t;
    auto const l = layout_of(c);
    if (t.axes.is_ms)
        return malformed("a sample of a multisampled texture");
    auto const is_compare
        = c.what == op::sample_compare || c.what == op::sample_compare_level || c.what == op::gather_compare;
    if (is_compare && s.compare < 0)
        return malformed("a comparison through a sampler that compares nothing");

    auto const coord = arg(args, 1);
    f32 xyz[3] = {0.0f, 0.0f, 0.0f};
    for (auto i = 0; i < 3 && i < coord.size(); ++i)
        xyz[i] = coord[i].as_float();
    auto layer = t.axes.is_array ? int_arg(args, l.layer) : 0;
    auto const cube_count = t.axes.is_cube ? tex.layers / 6 : tex.layers;
    // a sample clamps its layer, as D3D and Vulkan both define it
    layer = layer < 0 ? 0 : layer >= cube_count ? cube_count - 1 : layer;
    auto dim = t.axes.dim;
    u8 modes[3] = {s.address_u, s.address_v, s.address_w};
    if (t.axes.is_cube)
    {
        auto face = 0;
        auto u = 0.0f;
        auto v = 0.0f;
        cube_face_of(xyz[0], xyz[1], xyz[2], face, u, v);
        xyz[0] = u;
        xyz[1] = v;
        layer = layer * 6 + face;
        dim = 2;
        modes[0] = modes[1] = modes[2] = 2;
    }

    auto base = level_sample{.t = t, .s = s, .layer = layer, .channels = c.is_depth ? 1 : c.width};
    for (auto i = 0; i < 3; ++i)
        base.modes[i] = modes[i];
    if (c.has_offset)
    {
        auto const o = arg(args, l.offset);
        for (auto i = 0; i < 3 && i < o.size(); ++i)
            base.offset[i] = o[i].as_int();
    }
    if (is_compare)
    {
        base.compare = s.compare;
        base.reference = arg(args, l.first)[0].as_float();
    }

    // EVAL-125: a gather reads one channel of the four texels a bilinear sample of level 0 would weigh
    if (c.what == op::gather || c.what == op::gather_compare)
    {
        auto const component = c.what == op::gather && !c.is_depth ? int_arg(args, l.first) : 0;
        auto const u = base.along(xyz[0], t.width(0), 0, true);
        auto const v = base.along(xyz[1], t.height(0), 1, true);
        i32 const corners[4][2] = {{u.i0, v.i1}, {u.i1, v.i1}, {u.i1, v.i0}, {u.i0, v.i0}};
        for (auto const& k : corners)
            if (c.kind == value_kind::scalar_float)
                out.push_back(scalar::of(base.read(k[0], k[1], 0, component)));
            else
                out.push_back(t.at(0, base.layer, k[0], k[1], 0, 0, component));
        return {};
    }

    auto const lod = lod_of(c, t, s, args, gradients);
    auto const is_magnified = lod <= 0.0f;
    auto const is_linear = (is_magnified ? s.mag_filter : s.min_filter) == 1;
    auto const top = f32(tex.levels - 1);
    auto const d = lod < 0.0f ? 0.0f : lod > top ? top : lod;
    f32 first[4] = {};
    f32 second[4] = {};
    if (s.mip_filter == 0)
    {
        auto const level = i32(floor_of(d + 0.5f));
        auto at = base;
        at.level = level;
        at.run(xyz, dim, is_linear, first);
        for (auto ch = 0; ch < base.channels; ++ch)
            out.push_back(scalar::of(first[ch]));
        return {};
    }
    auto const low = i32(floor_of(d));
    auto const high = low + 1 < tex.levels ? low + 1 : low;
    auto const weight = d - f32(low);
    auto at_low = base;
    at_low.level = low;
    at_low.run(xyz, dim, is_linear, first);
    auto at_high = base;
    at_high.level = high;
    at_high.run(xyz, dim, is_linear, second);
    for (auto ch = 0; ch < base.channels; ++ch)
        out.push_back(scalar::of(mixed(first[ch], second[ch], weight)));
    return {};
}
} // namespace

impl::texture_failure impl::run_texture_call(call const& c,
                                             texture_contents& texture,
                                             sampler_state const* sampler,
                                             cc::span<value const> args,
                                             cc::vector<scalar>& out,
                                             bool& stored,
                                             coordinate_gradients const* gradients)
{
    auto const t = texels{.t = texture, .axes = axes_of(c.where)};
    switch (c.what)
    {
    case op::size:
    case op::layer_count:
    case op::level_count:
    case op::sample_count:
    case op::image_size:
    case op::image_layer_count:
        return sizes(c, t, args, out);
    case op::test_texel:
        return malformed("a texel's statement run as a read");
    default:
        break;
    }
    // EVAL-121: a texture the run binds nothing to has no texel
    if (texture.width == 0)
        return no_value("a read of a texture the run binds no texels to");
    switch (c.what)
    {
    case op::load:
    case op::image_load:
    {
        auto offset = isize(0);
        if (auto f = addressed_texel(c, t, args, offset); !f.is_ok())
            return f;
        for (auto ch = 0; ch < texture.channels; ++ch)
            out.push_back(texture.texels[offset + ch]);
        return {};
    }
    case op::image_store:
    {
        auto offset = isize(0);
        if (auto f = addressed_texel(c, t, args, offset); !f.is_ok())
            return f;
        auto const v = arg(args, layout_of(c).first);
        if (v.size() != texture.channels)
            return malformed("an image store of a value whose width is not its texel's");
        for (auto ch = 0; ch < texture.channels; ++ch)
            texture.texels[offset + ch] = v[ch];
        stored = true;
        return {};
    }
    default:
        if (sampler == nullptr)
            return malformed("a sample without its sampler");
        return sampled(c, t, *sampler, args, out, gradients);
    }
}

impl::texture_failure impl::state_texel(call const& c, texture_contents& texture, bool may_grow, cc::span<value const> args)
{
    auto const axes = axes_of(c.where);
    auto const l = layout_of(c);
    auto const coord = arg(args, 1);
    auto const dim = axes.is_cube ? 2 : axes.dim;
    i32 xyz[3] = {0, 0, 0};
    for (auto i = 0; i < dim && i < coord.size(); ++i)
        xyz[i] = coord[i].as_int();
    auto const level = int_arg(args, l.level);
    auto const sample = int_arg(args, l.sample);
    auto const face = int_arg(args, l.face);
    auto const layer = axes.is_cube ? int_arg(args, l.layer) * 6 + face : int_arg(args, l.layer);
    auto const value = arg(args, l.first);
    auto const channels = c.is_depth ? 1 : c.width;
    if (value.size() != channels)
        return malformed("a texel's statement of a value whose width is not its texel's");
    for (auto i = 0; i < dim; ++i)
        if (xyz[i] < 0)
            return no_value(cc::format("a texel stated at the coordinate {}", xyz[i]));
    if (level < 0 || level > 15 || sample < 0 || face < 0 || face > 5 || layer < 0)
        return no_value(cc::format("a texel stated at level {}, layer {}, face {} or sample {}", level,
                                   int_arg(args, l.layer), face, sample));

    // the smallest texture that holds every texel stated so far, every other texel zero
    if (may_grow)
    {
        auto const is_fresh = texture.width == 0;
        // a fresh texture holds nothing yet, whatever its default extent says
        auto const grown = [&](i32 old, i32 needed) { return is_fresh || needed > old ? needed : old; };
        auto next = texture_contents{
            .binding = texture.binding,
            .member = texture.member,
            .element = texture.element,
            .width = grown(texture.width, (xyz[0] + 1) << level),
            .height = dim >= 2 ? grown(texture.height, (xyz[1] + 1) << level) : 1,
            .depth = dim == 3 ? grown(texture.depth, (xyz[2] + 1) << level) : 1,
            .layers = grown(texture.layers, layer + 1),
            .levels = grown(texture.levels, level + 1),
            .samples = grown(texture.samples, sample + 1),
            .channels = channels,
        };
        if (axes.is_cube && next.layers % 6 != 0)
            next.layers += 6 - next.layers % 6;
        auto const is_changed = is_fresh || next.width != texture.width || next.height != texture.height
                             || next.depth != texture.depth || next.layers != texture.layers
                             || next.levels != texture.levels || next.samples != texture.samples;
        if (is_changed)
        {
            auto const from = texels{.t = texture, .axes = axes};
            auto const to = texels{.t = next, .axes = axes};
            next.texels.resize_to_filled(to.level_offset(next.levels), scalar{.kind = c.kind, .bits = 0});
            for (auto lv = 0; !is_fresh && lv < texture.levels; ++lv)
                for (auto ly = 0; ly < texture.layers; ++ly)
                    for (auto z = 0; z < from.depth(lv); ++z)
                        for (auto y = 0; y < from.height(lv); ++y)
                            for (auto x = 0; x < from.width(lv); ++x)
                                for (auto sm = 0; sm < texture.samples; ++sm)
                                    for (auto ch = 0; ch < channels; ++ch)
                                        next.texels[to.offset_of(lv, ly, x, y, z, sm) + ch]
                                            = texture.texels[from.offset_of(lv, ly, x, y, z, sm) + ch];
            texture = cc::move(next);
        }
    }

    auto const t = texels{.t = texture, .axes = axes};
    if (level >= texture.levels || layer >= texture.layers || sample >= texture.samples)
        return no_value(cc::format("a texel stated at level {}, layer {} or sample {}, outside the texture the driver "
                                   "bound",
                                   level, layer, sample));
    i32 const extent[3] = {t.width(level), t.height(level), t.depth(level)};
    for (auto i = 0; i < dim; ++i)
        if (xyz[i] >= extent[i])
            return no_value(cc::format("a texel stated at the coordinate {}, outside 0 ..< {} of the texture the "
                                       "driver bound",
                                       xyz[i], extent[i]));
    auto const offset = t.offset_of(level, layer, xyz[0], xyz[1], xyz[2], sample);
    for (auto ch = 0; ch < channels; ++ch)
        texture.texels[offset + ch] = value[ch];
    return {};
}

impl::texture_failure impl::texel_offset(texture_contents const& texture,
                                         shape_axes axes,
                                         cc::span<scalar const> coord,
                                         i32 layer,
                                         isize& offset)
{
    if (texture.width == 0)
        return no_value("an atomic of an image the run binds no texels to");
    auto const t = texels{.t = texture, .axes = axes};
    i32 xyz[3] = {0, 0, 0};
    for (auto i = 0; i < axes.dim && i < coord.size(); ++i)
        xyz[i] = coord[i].as_int();
    i32 const extent[3] = {t.width(0), t.height(0), t.depth(0)};
    for (auto i = 0; i < axes.dim; ++i)
        if (xyz[i] < 0 || xyz[i] >= extent[i])
            return no_value(cc::format("the texel coordinate {} is outside 0 ..< {}", xyz[i], extent[i]));
    if (layer < 0 || layer >= texture.layers)
        return no_value(cc::format("layer {} of an image of {} layers", layer, texture.layers));
    offset = t.offset_of(0, layer, xyz[0], xyz[1], xyz[2], 0);
    return {};
}
