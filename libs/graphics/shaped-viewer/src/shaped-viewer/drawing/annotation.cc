#include <clean-core/common/utility.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/drawing/annotation.hh>
#include <typed-geometry/linalg/vec_ops.hh>

namespace sv::impl
{
namespace
{
/// Which way each part of an annotation tests its anchor, under one occlusion policy.
struct part_visibility
{
    sr::slug_visibility content = sr::slug_visibility::always;
    sr::slug_visibility marker = sr::slug_visibility::always;
    sr::slug_visibility leader = sr::slug_visibility::always;

    /// The hollow marker and dashed leader drawn in their place while the anchor is hidden; none without one.
    bool has_hidden_look = false;
};

[[nodiscard]] part_visibility visibility_of(annotation_occluded policy)
{
    switch (policy)
    {
    case annotation_occluded::hidden_line:
        return {.marker = sr::slug_visibility::if_visible,
                .leader = sr::slug_visibility::if_visible,
                .has_hidden_look = true};
    case annotation_occluded::hide:
        return {.content = sr::slug_visibility::if_visible,
                .marker = sr::slug_visibility::if_visible,
                .leader = sr::slug_visibility::if_visible};
    case annotation_occluded::show:
        break;
    }
    return {};
}

/// The direction (x, y) of `side` away from the anchor at `p`, in a view `size` across, y down.
[[nodiscard]] tg::vec2f direction_of(annotation_side side, tg::pos2f p, tg::vec2f size)
{
    switch (side)
    {
    case annotation_side::above_right:
        return tg::vec2f(1, -1);
    case annotation_side::above_left:
        return tg::vec2f(-1, -1);
    case annotation_side::below_right:
        return tg::vec2f(1, 1);
    case annotation_side::below_left:
        return tg::vec2f(-1, 1);
    case annotation_side::automatic:
        break;
    }
    return tg::vec2f(p[0] >= size[0] * 0.5f ? 1.0f : -1.0f, p[1] >= size[1] * 0.5f ? 1.0f : -1.0f);
}

/// The points of the leader from the marker's edge to the box, or none where the box covers the anchor.
[[nodiscard]] cc::vector<tg::pos2f> leader_points(annotation_record const& a, tg::pos2f p, tg::aabb2f box)
{
    auto points = cc::vector<tg::pos2f>();
    auto const inside = p[0] >= box.min[0] && p[0] <= box.max[0] && p[1] >= box.min[1] && p[1] <= box.max[1];
    if (inside)
        return points;

    points.push_back(p);
    if (a.leader == leader_shape::straight)
        points.push_back(tg::pos2f(cc::clamp(p[0], box.min[0], box.max[0]), cc::clamp(p[1], box.min[1], box.max[1])));
    else
    {
        // a short shelf into the side of the box facing the anchor, at the box's middle height
        auto const centre_x = (box.min[0] + box.max[0]) * 0.5f;
        auto const toward = centre_x >= p[0] ? 1.0f : -1.0f;
        auto const attach = tg::pos2f(toward > 0.0f ? box.min[0] : box.max[0], (box.min[1] + box.max[1]) * 0.5f);
        auto const shelf = cc::min(10.0f, tg::abs(attach[0] - p[0]));
        auto const knee = tg::pos2f(attach[0] - toward * shelf, attach[1]);
        if (knee != p)
            points.push_back(knee);
        points.push_back(attach);
    }

    // start at the marker's edge rather than its centre
    auto const first = points[1] - points[0];
    auto const length = first.length();
    auto const radius = a.marker_radius;
    if (length <= radius)
    {
        points.clear();
        return points;
    }
    points[0] = points[0] + first * (radius / length);
    return points;
}
} // namespace

void place_annotation(annotation_record const& a,
                      tg::mat4f const& world_to_clip,
                      tg::vec2f logical_size,
                      cc::vector<drawing_placement>& out)
{
    auto const clip = world_to_clip * tg::vec4f(a.anchor[0], a.anchor[1], a.anchor[2], 1.0f);
    if (clip[3] <= 0.0f)
        return;
    auto const ndc_x = clip[0] / clip[3];
    auto const ndc_y = clip[1] / clip[3];
    if (tg::abs(ndc_x) > 1.0f || tg::abs(ndc_y) > 1.0f)
        return;
    auto const p = tg::pos2f((ndc_x + 1.0f) * 0.5f * logical_size[0], (1.0f - ndc_y) * 0.5f * logical_size[1]);
    auto const probe = tg::vec2f(p[0] / logical_size[0], p[1] / logical_size[1]);
    auto const depth = clip[2] / clip[3];

    // The box: offset away from the anchor on its side, then pushed back inside the view's margins.
    auto const away = direction_of(a.side, p, logical_size);
    auto const near_corner = p + tg::vec2f(away[0] * a.offset[0], away[1] * a.offset[1]);
    auto lo = tg::pos2f(away[0] > 0.0f ? near_corner[0] : near_corner[0] - a.box_size[0],
                        away[1] > 0.0f ? near_corner[1] : near_corner[1] - a.box_size[1]);
    for (auto axis = 0; axis < 2; ++axis)
        lo[axis] = cc::max(a.margin, cc::min(lo[axis], logical_size[axis] - a.margin - a.box_size[axis]));
    auto const box = tg::aabb2f(lo, lo + a.box_size);

    auto const visibility = visibility_of(a.occluded);
    auto const probed = [&](drawing_placement q, sr::slug_visibility v)
    {
        q.visibility = v;
        q.probe = probe;
        q.probe_depth = depth;
        q.from = corner::top_left;
        q.reach = tg::vec2f(0, 0);
        q.offset = tg::vec2f(0, 0);
        return q;
    };
    auto const at = [](tg::pos2f q) { return tg::pos3f(q[0], q[1], 0); };

    for (auto const& c : a.content)
    {
        auto q = probed(c, visibility.content);
        q.at = q.at + tg::vec3f(lo[0], lo[1], 0);
        out.push_back(q);
    }

    // The marker's drawings are centred on their own origin, at one logical pixel per unit.
    auto disk = probed(a.disk, visibility.marker);
    disk.at = at(p);
    out.push_back(disk);
    if (visibility.has_hidden_look)
    {
        auto ring = probed(a.ring, sr::slug_visibility::if_hidden);
        ring.at = at(p);
        out.push_back(ring);
    }

    auto const points = leader_points(a, p, box);
    if (points.size() < 2)
        return;

    // One unit segment per straight run of the leader, or per dash of it, and a dot at each bend.
    auto const segment = [&](tg::pos2f from, tg::pos2f to, sr::slug_visibility v)
    {
        auto const along = to - from;
        auto const length = along.length();
        if (length <= 0.0f)
            return;
        auto const across = tg::vec2f(-along[1], along[0]) * (a.leader_width / length);
        auto q = probed(a.segment, v);
        q.at = at(from);
        q.x_axis = tg::vec3f(along[0], along[1], 0);
        q.y_axis = tg::vec3f(across[0], across[1], 0);
        out.push_back(q);
    };
    auto const leader = [&](cc::span<f32 const> given, sr::slug_visibility v)
    {
        // an odd pattern repeats once, so on and off keep alternating
        auto dashes = cc::vector<f32>();
        for (auto const d : given)
            dashes.push_back(cc::max(d, 0.0f));
        if (dashes.size() % 2 == 1)
            for (auto i = isize(0); i < given.size(); ++i)
                dashes.push_back(dashes[i]);
        auto total = 0.0f;
        for (auto const d : dashes)
            total += d;
        auto const dashed = !dashes.empty() && total > 0.0f;

        for (auto i = isize(1); i + 1 < points.size(); ++i)
            if (!dashed)
            {
                auto q = probed(a.dot, v);
                q.at = at(points[i]);
                q.x_axis = tg::vec3f(a.leader_width * 0.5f, 0, 0);
                q.y_axis = tg::vec3f(0, a.leader_width * 0.5f, 0);
                out.push_back(q);
            }

        // the pattern runs on across bends, so a dash may turn a corner as two segments
        auto index = isize(0);
        auto remaining = dashed ? dashes[0] : 0.0f;
        for (auto i = isize(1); i < points.size(); ++i)
        {
            if (!dashed)
            {
                segment(points[i - 1], points[i], v);
                continue;
            }
            auto const from = points[i - 1];
            auto const along = points[i] - from;
            auto const length = along.length();
            auto travelled = 0.0f;
            while (travelled < length)
            {
                auto const step = cc::min(remaining, length - travelled);
                if (index % 2 == 0 && step > 0.0f)
                    segment(from + along * (travelled / length), from + along * ((travelled + step) / length), v);
                travelled += step;
                remaining -= step;
                if (remaining <= 0.0f)
                {
                    index = (index + 1) % dashes.size();
                    remaining = dashes[index];
                }
            }
        }
    };
    leader(a.dashes, visibility.leader);
    if (visibility.has_hidden_look)
        leader(a.hidden_dashes, sr::slug_visibility::if_hidden);
}
} // namespace sv::impl
