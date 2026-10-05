#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-rendering/impl/slug_geometry.hh>
#include <shaped-rendering/slug_shape.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sr
{
namespace
{
/// The largest coordinate magnitude a half float holds every integer up to.
constexpr f32 exact_half_range = 2048.0f;

/// Glyphs bound their band count well below this, and the header has room for 256 horizontal bands.
constexpr int max_bands_per_axis = 16;

/// A run takes one texel per curve plus its closing texel, and must fit one row of the 4096-wide curve texture.
constexpr isize max_curves_per_run = 4095;

/// What resolving one glyph may cost, counted as one per record visited plus one per point it yields.
/// Depth alone does not bound a composite: components that each repeat the level below grow exponentially with it.
constexpr isize max_glyph_work = 65536;

using impl::lerp;
using impl::midpoint;

struct glyf_point
{
    tg::pos2f p;
    bool on_curve = true;
};

struct flat_glyph
{
    cc::vector<glyf_point> points;
    cc::vector<i32> contour_ends; // index of each contour's last point
};

/// `work` is what this glyph may still cost, shared by every level of the recursion.
[[nodiscard]] cc::result<flat_glyph> flatten(babel::font::face const& face,
                                             babel::font::glyph_id glyph,
                                             int depth,
                                             isize& work)
{
    if (depth > 16)
        return cc::error("a composite glyph nests deeper than 16 levels");

    auto record = face.outline(glyph);
    CC_RETURN_IF_ERROR(record);
    auto const& o = record.value();

    work -= 1 + o.points.size();
    if (work < 0)
        return cc::error(cc::format("a composite glyph expands past {} records and points", max_glyph_work));

    auto out = flat_glyph();
    if (!o.is_composite())
    {
        for (auto const& p : o.points)
            out.points.push_back({.p = tg::pos2f(f32(p.position[0]), f32(p.position[1])), .on_curve = p.on_curve});
        out.contour_ends = o.contour_ends;
        return out;
    }

    for (auto const& c : o.components)
    {
        auto child = flatten(face, c.glyph, depth + 1, work);
        CC_RETURN_IF_ERROR(child);
        auto& part = child.value();
        for (auto& q : part.points)
        {
            auto const x = q.p[0];
            auto const y = q.p[1];
            q.p = tg::pos2f(c.xx * x + c.yx * y, c.xy * x + c.yy * y);
        }

        auto offset = tg::vec2f(0.0f, 0.0f);
        if (c.args_are_offset)
        {
            offset = tg::vec2f(f32(c.arg1), f32(c.arg2));
            // SCALED_COMPONENT_OFFSET: the offset is in the component's own, transformed space
            if ((c.flags & 0x0800) != 0)
                offset = tg::vec2f(c.xx * offset[0] + c.yx * offset[1], c.xy * offset[0] + c.yy * offset[1]);
        }
        else
        {
            if (c.arg1 < 0 || c.arg1 >= out.points.size() || c.arg2 < 0 || c.arg2 >= part.points.size())
                return cc::error("a composite glyph matches a point it does not have");
            offset = out.points[c.arg1].p - part.points[c.arg2].p;
        }

        auto const base = i32(out.points.size());
        for (auto const& q : part.points)
            out.points.push_back({.p = q.p + offset, .on_curve = q.on_curve});
        for (auto const e : part.contour_ends)
            out.contour_ends.push_back(base + e);
    }
    return out;
}

/// One rounded curve, and the bounds and kinds the band builder asks about.
struct stored_curve
{
    slug_curve c;
    i32 texel = 0;

    [[nodiscard]] f32 min_x() const { return cc::min(cc::min(c.p1[0], c.p2[0]), c.p3[0]); }
    [[nodiscard]] f32 max_x() const { return cc::max(cc::max(c.p1[0], c.p2[0]), c.p3[0]); }
    [[nodiscard]] f32 min_y() const { return cc::min(cc::min(c.p1[1], c.p2[1]), c.p3[1]); }
    [[nodiscard]] f32 max_y() const { return cc::max(cc::max(c.p1[1], c.p2[1]), c.p3[1]); }

    /// A horizontal line never crosses a horizontal ray, so it belongs in no horizontal band; the same for vertical.
    [[nodiscard]] bool is_horizontal() const { return c.p1[1] == c.p2[1] && c.p2[1] == c.p3[1]; }
    [[nodiscard]] bool is_vertical() const { return c.p1[0] == c.p2[0] && c.p2[0] == c.p3[0]; }
};

/// The curves crossing each of `count` bands along one axis, overlapping by `epsilon` so a curve on a border is in both.
[[nodiscard]] cc::vector<cc::vector<i32>> bands_of(cc::span<stored_curve const> curves,
                                                   int count,
                                                   bool horizontal,
                                                   f32 low,
                                                   f32 high,
                                                   f32 epsilon)
{
    auto out = cc::vector<cc::vector<i32>>();
    out.resize_to_defaulted(count);
    auto const step = (high - low) / f32(count);
    for (auto i = isize(0); i < curves.size(); ++i)
    {
        auto const& c = curves[i];
        if (horizontal ? c.is_horizontal() : c.is_vertical())
            continue;
        auto const lo = horizontal ? c.min_y() : c.min_x();
        auto const hi = horizontal ? c.max_y() : c.max_x();
        for (auto b = 0; b < count; ++b)
        {
            auto const band_lo = low + step * f32(b) - epsilon;
            auto const band_hi = low + step * f32(b + 1) + epsilon;
            if (hi >= band_lo && lo <= band_hi)
                out[b].push_back(i32(i));
        }
    }
    return out;
}

[[nodiscard]] isize longest(cc::vector<cc::vector<i32>> const& bands)
{
    auto n = isize(0);
    for (auto const& b : bands)
        n = cc::max(n, b.size());
    return n;
}

/// The band count that keeps the longest list shortest, preferring fewer bands on a tie.
[[nodiscard]] cc::vector<cc::vector<i32>> best_bands(cc::span<stored_curve const> curves,
                                                     bool horizontal,
                                                     f32 low,
                                                     f32 high,
                                                     f32 epsilon)
{
    auto best = bands_of(curves, 1, horizontal, low, high, epsilon);
    if (high <= low)
        return best;
    auto best_cost = longest(best);
    for (auto count = 2; count <= max_bands_per_axis; ++count)
    {
        auto candidate = bands_of(curves, count, horizontal, low, high, epsilon);
        auto const cost = longest(candidate);
        if (cost < best_cost)
        {
            best = cc::move(candidate);
            best_cost = cost;
        }
    }
    return best;
}

[[nodiscard]] f32 rounded(f32 v)
{
    return f32(tg::half_float(v));
}

[[nodiscard]] u16 half_bits(f32 v)
{
    return tg::half_float(v).bits();
}
} // namespace

void slug_outline::move_to(tg::pos2f p)
{
    close();
    _start = p;
    _cursor = p;
    _open = true;
}

void slug_outline::line_to(tg::pos2f p)
{
    if (p == _cursor)
        return;
    curves.push_back({.p1 = _cursor, .p2 = p, .p3 = p});
    _cursor = p;
}

void slug_outline::quad_to(tg::pos2f control, tg::pos2f p)
{
    curves.push_back({.p1 = _cursor, .p2 = control, .p3 = p});
    _cursor = p;
}

void slug_outline::cubic_to(tg::pos2f c0, tg::pos2f c1, tg::pos2f p, float tolerance)
{
    // A quadratic's distance from a cubic over a piece of length 1/n falls with n^3, so solve for n.
    auto const p0 = _cursor;
    auto const third = tg::vec2f(p[0] - 3.0f * c1[0] + 3.0f * c0[0] - p0[0], p[1] - 3.0f * c1[1] + 3.0f * c0[1] - p0[1]);
    auto const error = tg::sqrt(third[0] * third[0] + third[1] * third[1]) * (tg::sqrt(3.0f) / 36.0f);
    auto const pieces = cc::clamp(int(tg::ceil(tg::pow(error / cc::max(tolerance, 1e-6f), 1.0f / 3.0f))), 1, 64);

    auto const at = [&](f32 t)
    {
        auto const a = lerp(p0, c0, t);
        auto const b = lerp(c0, c1, t);
        auto const c = lerp(c1, p, t);
        auto const d = lerp(a, b, t);
        auto const e = lerp(b, c, t);
        return lerp(d, e, t);
    };
    auto const tangent = [&](f32 t)
    {
        // the derivative of the cubic, which the piece's quadratic control point is built from
        auto const u = 1.0f - t;
        return tg::vec2f(3.0f * (u * u * (c0[0] - p0[0]) + 2.0f * u * t * (c1[0] - c0[0]) + t * t * (p[0] - c1[0])),
                         3.0f * (u * u * (c0[1] - p0[1]) + 2.0f * u * t * (c1[1] - c0[1]) + t * t * (p[1] - c1[1])));
    };

    for (auto i = 0; i < pieces; ++i)
    {
        auto const t0 = f32(i) / f32(pieces);
        auto const t1 = f32(i + 1) / f32(pieces);
        auto const a = at(t0);
        auto const b = i + 1 == pieces ? p : at(t1);
        // The control point both end tangents meet at, averaged from the two sides for stability.
        auto const h = (t1 - t0) * 0.5f;
        auto const from_a = a + tangent(t0) * h;
        auto const from_b = b - tangent(t1) * h;
        quad_to(midpoint(from_a, from_b), b);
    }
}

void slug_outline::close()
{
    if (!_open)
        return;
    if (_cursor != _start)
        line_to(_start);
    if (contour_ends.empty() || contour_ends.back() != i32(curves.size()))
        contour_ends.push_back(i32(curves.size()));
    _open = false;
}

slug_outline slug_outline::rectangle(tg::aabb2f box)
{
    auto o = slug_outline();
    o.move_to(box.min);
    o.line_to(tg::pos2f(box.max[0], box.min[1]));
    o.line_to(box.max);
    o.line_to(tg::pos2f(box.min[0], box.max[1]));
    o.close();
    return o;
}

bool slug_outline::is_closed() const
{
    auto first = i32(0);
    for (auto const end : contour_ends)
    {
        if (end < first || end > curves.size())
            return false;
        if (end > first && curves[end - 1].p3 != curves[first].p1)
            return false;
        first = end;
    }
    return first == curves.size();
}

isize slug_compiled_shape::band_texel_count() const
{
    auto n = horizontal_bands.size() + vertical_bands.size();
    for (auto const& b : horizontal_bands)
        n += b.size();
    for (auto const& b : vertical_bands)
        n += b.size();
    return n;
}

cc::result<slug_outline> slug_outline_of(babel::font::face const& face, babel::font::glyph_id glyph)
{
    auto work = max_glyph_work;
    auto flat = flatten(face, glyph, 0, work);
    CC_RETURN_IF_ERROR(flat);
    auto const& g = flat.value();

    auto out = slug_outline();
    auto first = i32(0);
    for (auto const end : g.contour_ends)
    {
        auto const n = end - first + 1;
        if (n < 2 || end >= g.points.size())
        {
            first = end + 1;
            continue;
        }

        // The contour walked from an on-curve point; with none, from the midpoint the last and first points imply.
        auto sequence = cc::vector<glyf_point>();
        auto start = -1;
        for (auto i = 0; i < n; ++i)
            if (g.points[first + i].on_curve)
            {
                start = i;
                break;
            }
        if (start < 0)
        {
            sequence.push_back({.p = midpoint(g.points[end].p, g.points[first].p), .on_curve = true});
            for (auto i = 0; i < n; ++i)
                sequence.push_back(g.points[first + i]);
        }
        else
        {
            for (auto i = 0; i < n; ++i)
                sequence.push_back(g.points[first + (start + i) % n]);
        }

        out.move_to(sequence[0].p);
        auto control = tg::pos2f();
        auto has_control = false;
        for (auto i = isize(1); i <= sequence.size(); ++i)
        {
            auto const q = i < sequence.size() ? sequence[i] : sequence[0];
            if (q.on_curve)
            {
                if (has_control)
                    out.quad_to(control, q.p);
                else
                    out.line_to(q.p);
                has_control = false;
            }
            else
            {
                if (has_control)
                    out.quad_to(control, midpoint(control, q.p));
                control = q.p;
                has_control = true;
            }
        }
        out.close();
        first = end + 1;
    }
    return out;
}

slug_compiled_shape compile_slug_shape(slug_outline const& outline)
{
    CC_ASSERT(outline.is_closed(), "the outline has an open contour; call close() after its last curve");

    auto out = slug_compiled_shape();
    out.fill_rule = outline.fill_rule;

    if (outline.curves.empty())
        return out;

    // Stored space is centred on the outline, so precision follows the shape's size rather than its distance from (0, 0).
    auto outline_lo = outline.curves[0].p1;
    auto outline_hi = outline.curves[0].p1;
    for (auto const& c : outline.curves)
    {
        for (auto const& p : {c.p1, c.p2, c.p3})
        {
            outline_lo = tg::pos2f(cc::min(outline_lo[0], p[0]), cc::min(outline_lo[1], p[1]));
            outline_hi = tg::pos2f(cc::max(outline_hi[0], p[0]), cc::max(outline_hi[1], p[1]));
        }
    }
    auto const centre = tg::pos2f((outline_lo[0] + outline_hi[0]) * 0.5f, (outline_lo[1] + outline_hi[1]) * 0.5f);
    auto const largest = cc::max(outline_hi[0] - centre[0], outline_hi[1] - centre[1]);
    if (largest == 0.0f)
        return out;
    out.stored_origin = centre;

    // A power of two, so the scaling itself is exact, bringing the largest half-extent into (1024, 2048].
    auto scale = 1.0f;
    while (largest * scale > exact_half_range)
        scale *= 0.5f;
    while (largest * scale * 2.0f <= exact_half_range && scale < 65536.0f)
        scale *= 2.0f;
    out.em_scale = scale;

    auto const store = [&](tg::pos2f p)
    { return tg::pos2f(rounded((p[0] - centre[0]) * scale), rounded((p[1] - centre[1]) * scale)); };

    // Curves in rounded form; a curve that collapsed to a point covers nothing and is dropped.
    auto curves = cc::vector<stored_curve>();
    for (auto const& c : outline.curves)
    {
        auto const s = slug_curve{.p1 = store(c.p1), .p2 = store(c.p2), .p3 = store(c.p3)};
        if (s.p1 == s.p2 && s.p2 == s.p3)
            continue;
        curves.push_back({.c = s});
    }
    if (curves.empty())
        return out;

    // Runs: consecutive curves sharing an end point share a texel, and a gap or a full row starts a new run.
    auto run_length = isize(0);
    for (auto i = isize(0); i < curves.size(); ++i)
    {
        auto& c = curves[i];
        auto const continues = i > 0 && curves[i - 1].c.p3 == c.c.p1 && run_length < max_curves_per_run;
        if (!continues && i > 0)
        {
            auto const& last = curves[i - 1].c;
            out.curve_texels.push_back({half_bits(last.p3[0]), half_bits(last.p3[1]), 0, 0});
            out.run_ends.push_back(i32(out.curve_texels.size()));
            run_length = 0;
        }
        c.texel = i32(out.curve_texels.size());
        out.curve_texels.push_back(
            {half_bits(c.c.p1[0]), half_bits(c.c.p1[1]), half_bits(c.c.p2[0]), half_bits(c.c.p2[1])});
        ++run_length;
    }
    auto const& last = curves.back().c;
    out.curve_texels.push_back({half_bits(last.p3[0]), half_bits(last.p3[1]), 0, 0});
    out.run_ends.push_back(i32(out.curve_texels.size()));

    auto lo = tg::pos2f(curves[0].min_x(), curves[0].min_y());
    auto hi = tg::pos2f(curves[0].max_x(), curves[0].max_y());
    for (auto const& c : curves)
    {
        lo = tg::pos2f(cc::min(lo[0], c.min_x()), cc::min(lo[1], c.min_y()));
        hi = tg::pos2f(cc::max(hi[0], c.max_x()), cc::max(hi[1], c.max_y()));
    }
    out.em_bounds = tg::aabb2f(lo, hi);

    // Bands overlap by 1/1024 of the shape's extent, as the reference recommends per em.
    auto const epsilon = cc::max(hi[0] - lo[0], hi[1] - lo[1]) / 1024.0f;
    auto horizontal = best_bands(curves, true, lo[1], hi[1], epsilon);
    auto vertical = best_bands(curves, false, lo[0], hi[0], epsilon);

    // The sort is what lets the shader stop at the first curve wholly on the far side of its sample.
    for (auto& band : horizontal)
        cc::sort(band, [&](i32 a, i32 b) { return curves[a].max_x() > curves[b].max_x(); });
    for (auto& band : vertical)
        cc::sort(band, [&](i32 a, i32 b) { return curves[a].max_y() > curves[b].max_y(); });

    for (auto& band : horizontal)
        for (auto& i : band)
            i = curves[i].texel;
    for (auto& band : vertical)
        for (auto& i : band)
            i = curves[i].texel;

    auto const width = hi[0] - lo[0];
    auto const height = hi[1] - lo[1];
    auto const scale_x = width > 0.0f ? f32(vertical.size()) / width : 0.0f;
    auto const scale_y = height > 0.0f ? f32(horizontal.size()) / height : 0.0f;
    out.banding = tg::vec4f(scale_x, scale_y, -lo[0] * scale_x, -lo[1] * scale_y);
    out.horizontal_bands = cc::move(horizontal);
    out.vertical_bands = cc::move(vertical);
    return out;
}
} // namespace sr
