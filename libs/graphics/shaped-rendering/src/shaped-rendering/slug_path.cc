#include <clean-core/common/utility.hh>
#include <shaped-rendering/slug_path.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sr
{
namespace
{
/// The shapes' curves stay within this fraction of their size of the true curve.
constexpr f32 shape_tolerance = 1.0f / 4096.0f;

/// How often the stroker may halve a quadratic, which bounds the pieces one curve becomes at 2^16.
constexpr int max_split_depth = 16;

[[nodiscard]] f32 cross(tg::vec2f a, tg::vec2f b)
{
    return a[0] * b[1] - a[1] * b[0];
}

/// `v` turned a quarter from +x toward +y.
[[nodiscard]] tg::vec2f left_of(tg::vec2f v)
{
    return tg::vec2f(-v[1], v[0]);
}

[[nodiscard]] tg::vec2f direction_of(f32 radians)
{
    auto const a = tg::angle_f::make_from_radians(radians);
    return tg::vec2f(tg::cos(a), tg::sin(a));
}

[[nodiscard]] tg::pos2f midpoint(tg::pos2f a, tg::pos2f b)
{
    return a + (b - a) * 0.5f;
}

[[nodiscard]] tg::pos2f point_at(slug_curve const& c, f32 t)
{
    auto const u = 1.0f - t;
    return tg::pos2f(u * u * c.p1[0] + 2.0f * u * t * c.p2[0] + t * t * c.p3[0],
                     u * u * c.p1[1] + 2.0f * u * t * c.p2[1] + t * t * c.p3[1]);
}

[[nodiscard]] tg::vec2f velocity_at(slug_curve const& c, f32 t)
{
    return ((c.p2 - c.p1) * (1.0f - t) + (c.p3 - c.p2) * t) * 2.0f;
}

/// The unit tangent; where the velocity vanishes, at a control point doubling an end, the chord's direction.
[[nodiscard]] tg::vec2f direction_at(slug_curve const& c, f32 t)
{
    auto const v = velocity_at(c, t);
    if (v.length() > 0.0f)
        return v.normalized();
    auto const chord = c.p3 - c.p1;
    if (chord.length() > 0.0f)
        return chord.normalized();
    return tg::vec2f(1, 0);
}

/// The part of `c` between `ta` and `tb`, exactly: a quadratic's piece is a quadratic.
[[nodiscard]] slug_curve sub_curve(slug_curve const& c, f32 ta, f32 tb)
{
    auto const a = point_at(c, ta);
    return {.p1 = a, .p2 = a + velocity_at(c, ta) * ((tb - ta) * 0.5f), .p3 = point_at(c, tb)};
}

[[nodiscard]] bool is_point(slug_curve const& c)
{
    return c.p1 == c.p2 && c.p2 == c.p3;
}

/// Straight from p1 to p3, its control point on the segment between them.
[[nodiscard]] bool is_line(slug_curve const& c)
{
    if (c.p2 == c.p1 || c.p2 == c.p3)
        return true;
    auto const along = c.p3 - c.p1;
    auto const off = c.p2 - c.p1;
    auto const area = cross(off, along);
    return tg::abs(area) <= 1e-6f * tg::dot(along, along) && tg::dot(off, along) >= 0.0f
        && tg::dot(c.p3 - c.p2, along) >= 0.0f;
}

/// Arc length between `ta` and `tb`, by Gauss-Legendre quadrature over eight intervals.
[[nodiscard]] f32 length_of(slug_curve const& c, f32 ta, f32 tb)
{
    if (is_line(c))
        return (point_at(c, tb) - point_at(c, ta)).length();
    constexpr f32 nodes[] = {-0.9061798f, -0.5384693f, 0.0f, 0.5384693f, 0.9061798f};
    constexpr f32 weights[] = {0.2369269f, 0.4786287f, 0.5688889f, 0.4786287f, 0.2369269f};
    constexpr int intervals = 8;
    auto const step = (tb - ta) / f32(intervals);
    auto sum = 0.0f;
    for (auto i = 0; i < intervals; ++i)
    {
        auto const mid = ta + step * (f32(i) + 0.5f);
        for (auto k = 0; k < 5; ++k)
            sum += weights[k] * velocity_at(c, mid + nodes[k] * step * 0.5f).length();
    }
    return sum * step * 0.5f;
}

/// The parameter `s` along `c`, whose whole length is `length`.
[[nodiscard]] f32 parameter_at(slug_curve const& c, f32 s, f32 length)
{
    if (s <= 0.0f)
        return 0.0f;
    if (s >= length)
        return 1.0f;
    if (is_line(c))
    {
        // a line's control point may sit anywhere on it, so its parameter is not proportional to its length
        auto lo = 0.0f;
        auto hi = 1.0f;
        for (auto i = 0; i < 32; ++i)
        {
            auto const mid = (lo + hi) * 0.5f;
            if ((point_at(c, mid) - c.p1).length() < s)
                lo = mid;
            else
                hi = mid;
        }
        return (lo + hi) * 0.5f;
    }
    auto lo = 0.0f;
    auto hi = 1.0f;
    for (auto i = 0; i < 32; ++i)
    {
        auto const mid = (lo + hi) * 0.5f;
        if (length_of(c, 0.0f, mid) < s)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) * 0.5f;
}

/// The smallest radius of curvature `c` has between `t0` and `t1`; zero at a cusp, and larger than any width on a line.
[[nodiscard]] f32 min_radius(slug_curve const& c, f32 t0, f32 t1)
{
    // The velocity is 2 * (a + d t) and the acceleration 2 * d, so their cross product is the constant 4 * cross(a, c),
    // and the radius |v|^3 / |v x acc| is smallest where the speed is.
    auto const a = c.p2 - c.p1;
    auto const d = (c.p3 - c.p2) - a;
    auto const turn = tg::abs(cross(a, c.p3 - c.p2));
    auto const dd = tg::dot(d, d);
    auto const slowest = dd > 0.0f ? cc::clamp(-tg::dot(a, d) / dd, t0, t1) : t0;
    auto const v = a + d * slowest;
    auto const speed = v.length();
    if (turn == 0.0f)
        return speed == 0.0f ? 0.0f : 3.0e38f;
    return 2.0f * speed * speed * speed / turn;
}

/// Signed area of a closed run of curves: the polygon of their end points plus each curve's parabolic segment.
[[nodiscard]] f32 signed_area(cc::span<slug_curve const> curves)
{
    auto area = 0.0f;
    for (auto const& c : curves)
    {
        area += cross(c.p1 - tg::pos2f(0, 0), c.p3 - tg::pos2f(0, 0)) * 0.5f;
        area += cross(c.p2 - c.p1, c.p3 - c.p1) / 3.0f;
    }
    return area;
}

/// The offset of a piece from `a` to `b`, each end moved by its own offset, as one quadratic.
/// Its control point is where the end tangents meet, which is what keeps the offset parallel to the curve at both ends.
[[nodiscard]] slug_curve offset_piece(tg::pos2f a,
                                      tg::pos2f b,
                                      tg::vec2f along_a,
                                      tg::vec2f along_b,
                                      tg::vec2f offset_a,
                                      tg::vec2f offset_b)
{
    auto const s = a + offset_a;
    auto const e = b + offset_b;
    auto const den = cross(along_a, along_b);
    if (tg::abs(den) < 1e-6f)
        return {.p1 = s, .p2 = midpoint(s, e), .p3 = e};
    auto const u = cross(e - s, along_b) / den;
    return {.p1 = s, .p2 = s + along_a * u, .p3 = e};
}

class stroker
{
public:
    stroker(stroke_style const& style, f32 tolerance) : _style(style), _half(style.width * 0.5f), _tolerance(tolerance)
    {
        // Disks this far apart cover all but `tolerance` of the stroke's width between them.
        _disk_spacing = cc::min(_half, 2.0f * tg::sqrt(2.0f * _half * tolerance));
        _out.fill_rule = slug_fill_rule::nonzero;
    }

    void contour(cc::span<slug_curve const> all, bool closed)
    {
        auto curves = cc::vector<slug_curve>();
        for (auto const& c : all)
            if (!is_point(c))
                curves.push_back(c);
        if (curves.empty())
            return;

        for (auto const& c : curves)
            segment(c);
        for (auto i = isize(1); i < curves.size(); ++i)
            join(curves[i].p1, direction_at(curves[i - 1], 1.0f), direction_at(curves[i], 0.0f));
        if (closed)
            join(curves[0].p1, direction_at(curves.back(), 1.0f), direction_at(curves[0], 0.0f));
        else
        {
            cap(curves[0].p1, -direction_at(curves[0], 0.0f));
            cap(curves.back().p3, direction_at(curves.back(), 1.0f));
        }
    }

    [[nodiscard]] slug_outline take() { return cc::move(_out); }

private:
    /// Appends one simple closed piece, wound the same way as every other, so overlaps add rather than cancel.
    void piece(slug_path const& p)
    {
        auto const outline = p.to_outline();
        auto const area = signed_area(outline.curves);
        if (area == 0.0f)
            return;
        if (area > 0.0f)
        {
            for (auto const& c : outline.curves)
                _out.curves.push_back(c);
        }
        else
        {
            for (auto i = outline.curves.size() - 1; i >= 0; --i)
            {
                auto const& c = outline.curves[i];
                _out.curves.push_back({.p1 = c.p3, .p2 = c.p2, .p3 = c.p1});
            }
        }
        _out.contour_ends.push_back(i32(_out.curves.size()));
    }

    void segment(slug_curve const& c)
    {
        if (!is_line(c))
        {
            quad_range(c, 0.0f, 1.0f, 0);
            return;
        }
        auto const n = left_of((c.p3 - c.p1).normalized()) * _half;
        auto p = slug_path();
        p.move_to(c.p1 + n).line_to(c.p3 + n).line_to(c.p3 - n).line_to(c.p1 - n).close();
        piece(p);
    }

    void quad_range(slug_curve const& c, f32 t0, f32 t1, int depth)
    {
        auto const a = point_at(c, t0);
        auto const b = point_at(c, t1);
        auto const along_a = direction_at(c, t0);
        auto const along_b = direction_at(c, t1);
        auto const n0 = left_of(along_a) * _half;
        auto const n1 = left_of(along_b) * _half;

        // Where the curve bends tighter than the half-width, the inner offset folds back over itself; the outer half is
        // still a clean offset, and disks along the curve stand in for the inner one.
        auto const folds = min_radius(c, t0, t1) <= _half;
        auto const outer = cross(c.p2 - c.p1, c.p3 - c.p2) > 0.0f ? -1.0f : 1.0f;
        auto const left = offset_piece(a, b, along_a, along_b, n0, n1);
        auto const right = offset_piece(a, b, along_a, along_b, -n0, -n1);

        if (depth < max_split_depth && must_split(c, t0, t1, folds, outer, left, right))
        {
            auto const tm = (t0 + t1) * 0.5f;
            quad_range(c, t0, tm, depth + 1);
            quad_range(c, tm, t1, depth + 1);
            return;
        }

        auto p = slug_path();
        if (!folds)
        {
            p.move_to(left.p1).quad_to(left.p2, left.p3).line_to(right.p3).quad_to(right.p2, right.p1).close();
            piece(p);
            return;
        }
        auto const& o = outer > 0.0f ? left : right;
        auto const base = sub_curve(c, t0, t1);
        p.move_to(o.p1).quad_to(o.p2, o.p3).line_to(b).quad_to(base.p2, a).close();
        piece(p);
        if (!_has_last_disk || _last_disk != a)
            disk(a);
        disk(b);
        _last_disk = b;
        _has_last_disk = true;
    }

    [[nodiscard]] bool must_split(slug_curve const& c,
                                  f32 t0,
                                  f32 t1,
                                  bool folds,
                                  f32 outer,
                                  slug_curve const& left,
                                  slug_curve const& right) const
    {
        // a piece turning more than 30 degrees has end tangents meeting too far out to bound a good offset
        if (tg::dot(direction_at(c, t0), direction_at(c, t1)) < 0.866f)
            return true;
        if (folds && (point_at(c, t1) - point_at(c, t0)).length() > _disk_spacing)
            return true;

        auto const tm = (t0 + t1) * 0.5f;
        auto const mid = point_at(c, tm);
        auto const n = left_of(direction_at(c, tm)) * _half;
        auto const off
            = [&](slug_curve const& piece, f32 side) { return (point_at(piece, 0.5f) - (mid + n * side)).length(); };
        if (folds)
            return off(outer > 0.0f ? left : right, outer) > _tolerance;
        return off(left, 1.0f) > _tolerance || off(right, -1.0f) > _tolerance;
    }

    void join(tg::pos2f p, tg::vec2f in, tg::vec2f out)
    {
        auto const cos_turn = tg::dot(in, out);
        auto const turn = tg::atan2(cross(in, out), cos_turn).radians();
        if (tg::abs(turn) < 1e-4f)
            return;

        // turning toward +y bends about a centre on the left, so the gap to fill opens on the right
        auto const side = turn > 0.0f ? -1.0f : 1.0f;
        auto const a = p + left_of(in) * (_half * side);
        auto const b = p + left_of(out) * (_half * side);
        auto path = slug_path();
        switch (_style.join)
        {
        case stroke_join::round:
            path.move_to(p).line_to(a).arc_to(p, tg::angle_f::make_from_radians(turn), _tolerance).close();
            break;
        case stroke_join::miter:
            // the miter reaches 1 / cos(turn / 2) half-widths from the corner point
            if (1.0f + cos_turn > 1e-6f && (1.0f + cos_turn) * 0.5f * _style.miter_limit * _style.miter_limit >= 1.0f)
            {
                auto const tip = p + ((a - p) + (b - p)) / (1.0f + cos_turn);
                path.move_to(p).line_to(a).line_to(tip).line_to(b).close();
                break;
            }
            [[fallthrough]];
        case stroke_join::bevel:
            path.move_to(p).line_to(a).line_to(b).close();
            break;
        }
        piece(path);
    }

    void cap(tg::pos2f p, tg::vec2f outward)
    {
        auto const n = left_of(outward) * _half;
        auto path = slug_path();
        switch (_style.cap)
        {
        case stroke_cap::butt:
            return;
        case stroke_cap::square:
        {
            auto const reach = outward * _half;
            path.move_to(p + n).line_to(p + n + reach).line_to(p - n + reach).line_to(p - n).close();
            break;
        }
        case stroke_cap::round:
            // from the left edge back through the outward direction to the right edge
            path.move_to(p + n).arc_to(p, tg::angle_f::make_from_radians(-tg::pi<f32>), _tolerance).close();
            break;
        }
        piece(path);
    }

    void disk(tg::pos2f p)
    {
        auto path = slug_path();
        path.move_to(p + tg::vec2f(_half, 0)).arc_to(p, tg::angle_f::make_from_radians(2.0f * tg::pi<f32>), _tolerance);
        piece(path.close());
    }

    stroke_style const& _style;
    f32 _half = 0.0f;
    f32 _tolerance = 0.0f;
    f32 _disk_spacing = 0.0f;
    tg::pos2f _last_disk = tg::pos2f(0, 0);
    bool _has_last_disk = false;
    slug_outline _out;
};

/// Calls `f(curves, closed)` for each non-empty contour of `path`.
template <class F>
void for_each_contour(slug_path const& path, F&& f)
{
    auto first = i32(0);
    for (auto const& contour : path.contours)
    {
        if (contour.end > first)
            f(cc::span<slug_curve const>(path.curves).subspan({.start = first, .end = contour.end}), contour.closed);
        first = contour.end;
    }
}
} // namespace

slug_path& slug_path::move_to(tg::pos2f p)
{
    // the contour begins with its first curve, so a move with nothing drawn after it leaves none
    _start = p;
    _cursor = p;
    _open = false;
    return *this;
}

void slug_path::push(slug_curve const& c)
{
    if (!_open)
    {
        contours.push_back({.end = i32(curves.size()), .closed = false});
        _start = _cursor;
        _open = true;
    }
    curves.push_back(c);
    contours.back().end = i32(curves.size());
    _cursor = c.p3;
}

slug_path& slug_path::line_to(tg::pos2f p)
{
    if (p != _cursor)
        push({.p1 = _cursor, .p2 = p, .p3 = p});
    return *this;
}

slug_path& slug_path::quad_to(tg::pos2f control, tg::pos2f p)
{
    push({.p1 = _cursor, .p2 = control, .p3 = p});
    return *this;
}

slug_path& slug_path::cubic_to(tg::pos2f c0, tg::pos2f c1, tg::pos2f p, f32 tolerance)
{
    // the outline already splits a cubic within a tolerance; borrow it rather than keep a second copy
    auto o = slug_outline();
    o.move_to(_cursor);
    o.cubic_to(c0, c1, p, tolerance);
    for (auto const& c : o.curves)
        push(c);
    return *this;
}

slug_path& slug_path::arc_to(tg::pos2f center, tg::angle_f sweep, f32 tolerance)
{
    auto const from = _cursor - center;
    auto const radius = from.length();
    auto const total = sweep.radians();
    if (radius == 0.0f || total == 0.0f)
        return *this;

    // A quadratic through both ends of an arc of half-angle h, controlled where their tangents meet, strays furthest at
    // its middle, by r (1 - cos h)^2 / (2 cos h), about r h^4 / 8; pieces are at most an eighth of a turn.
    auto const half_angle = cc::min(tg::pow(8.0f * cc::max(tolerance, 1e-7f) / radius, 0.25f), tg::pi<f32> / 8.0f);
    auto const pieces = cc::clamp(int(tg::ceil(tg::abs(total) / (2.0f * half_angle))), 1, 4096);
    auto const start = tg::atan2(from[1], from[0]).radians();
    auto const step = total / f32(pieces);
    auto const reach = radius / tg::cos(tg::angle_f::make_from_radians(step * 0.5f));
    auto const first = _cursor;
    for (auto i = 0; i < pieces; ++i)
    {
        auto const a0 = start + step * f32(i);
        auto end = center + direction_of(a0 + step) * radius;
        // a whole turn ends exactly where it began, so a contour closed after it gets no sliver of a line
        if (i + 1 == pieces && (end - first).length() <= radius * 1e-5f)
            end = first;
        push({.p1 = _cursor, .p2 = center + direction_of(a0 + step * 0.5f) * reach, .p3 = end});
    }
    return *this;
}

slug_path& slug_path::close()
{
    if (!_open)
        return *this;
    if (_cursor != _start)
        push({.p1 = _cursor, .p2 = _start, .p3 = _start});
    contours.back().closed = true;
    _cursor = _start;
    _open = false;
    return *this;
}

tg::aabb2f slug_path::bounds() const
{
    if (curves.empty())
        return tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(0, 0));
    auto lo = curves[0].p1;
    auto hi = curves[0].p1;
    for (auto const& c : curves)
        for (auto const p : {c.p1, c.p2, c.p3})
        {
            lo = tg::pos2f(cc::min(lo[0], p[0]), cc::min(lo[1], p[1]));
            hi = tg::pos2f(cc::max(hi[0], p[0]), cc::max(hi[1], p[1]));
        }
    return tg::aabb2f(lo, hi);
}

slug_outline slug_path::to_outline(slug_fill_rule rule) const
{
    auto out = slug_outline();
    out.fill_rule = rule;
    for_each_contour(*this,
                     [&](cc::span<slug_curve const> run, bool)
                     {
                         for (auto const& c : run)
                             out.curves.push_back(c);
                         if (run.back().p3 != run.front().p1)
                             out.curves.push_back({.p1 = run.back().p3, .p2 = run.front().p1, .p3 = run.front().p1});
                         out.contour_ends.push_back(i32(out.curves.size()));
                     });
    return out;
}

slug_path slug_path::rectangle(tg::aabb2f box)
{
    auto p = slug_path();
    p.move_to(box.min)
        .line_to(tg::pos2f(box.max[0], box.min[1]))
        .line_to(box.max)
        .line_to(tg::pos2f(box.min[0], box.max[1]))
        .close();
    return p;
}

slug_path slug_path::rounded_rectangle(tg::aabb2f box, f32 radius)
{
    auto const w = box.max[0] - box.min[0];
    auto const h = box.max[1] - box.min[1];
    auto const r = cc::clamp(radius, 0.0f, cc::min(w, h) * 0.5f);
    if (r <= 0.0f)
        return rectangle(box);

    auto const tolerance = cc::max(w, h) * shape_tolerance;
    auto const quarter = tg::angle_f::make_from_radians(tg::pi<f32> * 0.5f);
    auto const x0 = box.min[0];
    auto const y0 = box.min[1];
    auto const x1 = box.max[0];
    auto const y1 = box.max[1];
    auto p = slug_path();
    p.move_to(tg::pos2f(x0 + r, y0))
        .line_to(tg::pos2f(x1 - r, y0))
        .arc_to(tg::pos2f(x1 - r, y0 + r), quarter, tolerance)
        .line_to(tg::pos2f(x1, y1 - r))
        .arc_to(tg::pos2f(x1 - r, y1 - r), quarter, tolerance)
        .line_to(tg::pos2f(x0 + r, y1))
        .arc_to(tg::pos2f(x0 + r, y1 - r), quarter, tolerance)
        .line_to(tg::pos2f(x0, y0 + r))
        .arc_to(tg::pos2f(x0 + r, y0 + r), quarter, tolerance)
        .close();
    return p;
}

slug_path slug_path::circle(tg::pos2f center, f32 radius)
{
    return ellipse(center, tg::vec2f(radius, radius));
}

slug_path slug_path::ellipse(tg::pos2f center, tg::vec2f radii)
{
    // a unit circle stretched: an affine map keeps a quadratic a quadratic, and scales its error by at most the larger
    // radius, which the unit tolerance already divides out
    auto unit = slug_path();
    unit.move_to(tg::pos2f(1, 0))
        .arc_to(tg::pos2f(0, 0), tg::angle_f::make_from_radians(2.0f * tg::pi<f32>), 2.0f * shape_tolerance)
        .close();
    auto const map = [&](tg::pos2f p) { return center + tg::vec2f(p[0] * radii[0], p[1] * radii[1]); };
    for (auto& c : unit.curves)
        c = {.p1 = map(c.p1), .p2 = map(c.p2), .p3 = map(c.p3)};
    return unit;
}

slug_path slug_path::polygon(cc::span<tg::pos2f const> points)
{
    auto p = polyline(points);
    return p.close();
}

slug_path slug_path::polyline(cc::span<tg::pos2f const> points)
{
    auto p = slug_path();
    if (points.empty())
        return p;
    p.move_to(points[0]);
    for (auto i = isize(1); i < points.size(); ++i)
        p.line_to(points[i]);
    return p;
}

slug_path dash_path(slug_path const& path, stroke_style const& style)
{
    auto pattern = style.dashes;
    auto const given = pattern.size();
    if (given % 2 == 1)
        for (auto i = isize(0); i < given; ++i)
            pattern.push_back(pattern[i]);
    auto total = 0.0f;
    for (auto const d : pattern)
    {
        if (d < 0.0f)
            return path;
        total += d;
    }
    if (total <= 0.0f)
        return path;

    auto out = slug_path();
    for_each_contour(path,
                     [&](cc::span<slug_curve const> run, bool)
                     {
                         // where in the pattern the contour starts
                         auto phase = style.dash_offset - total * tg::floor(style.dash_offset / total);
                         auto index = isize(0);
                         while (phase >= pattern[index] && phase > 0.0f)
                         {
                             phase -= pattern[index];
                             index = (index + 1) % pattern.size();
                         }
                         auto remaining = pattern[index] - phase;
                         auto drawing = false;

                         for (auto const& c : run)
                         {
                             if (is_point(c))
                                 continue;
                             auto const length = length_of(c, 0.0f, 1.0f);
                             auto at = 0.0f;
                             while (at < length)
                             {
                                 auto const step = cc::min(remaining, length - at);
                                 if (index % 2 == 0 && step > 0.0f)
                                 {
                                     auto const piece
                                         = sub_curve(c, parameter_at(c, at, length), parameter_at(c, at + step, length));
                                     if (!drawing)
                                         out.move_to(piece.p1);
                                     drawing = true;
                                     out.quad_to(piece.p2, piece.p3);
                                 }
                                 at += step;
                                 remaining -= step;
                                 if (remaining <= 0.0f)
                                 {
                                     drawing = false;
                                     index = (index + 1) % pattern.size();
                                     remaining = pattern[index];
                                 }
                             }
                         }
                     });
    return out;
}

slug_outline stroke_outline(slug_path const& path, stroke_style const& style, f32 tolerance)
{
    if (!(style.width > 0.0f))
        return {};
    auto const dashed = dash_path(path, style);
    auto s = stroker(style, cc::max(tolerance, 1e-6f));
    for_each_contour(dashed, [&](cc::span<slug_curve const> run, bool closed) { s.contour(run, closed); });
    return s.take();
}
} // namespace sr
