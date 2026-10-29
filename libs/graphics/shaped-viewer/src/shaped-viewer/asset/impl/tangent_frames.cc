#include <shaped-viewer/asset/impl/asset_import.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>

// Building a tangent frame out of what a file supplied.
//
// These know nothing about rendering — they are vector math over a normal and a tangent — so they are written as free
// functions and belong in `tg::mesh` once it exists.
// Moving them down is then a file move rather than a rewrite.
//
// The uv-aligned tangents generated here do not match MikkTSpace, which nearly every DCC tool bakes normal maps against.
// issue: feat-sv-mikktspace-tangents

namespace sv
{
namespace
{
/// A unit vector perpendicular to `n`, picked to stay well-conditioned.
///
/// The axis furthest from `n` is what the cross product is taken against, since crossing with a nearly-parallel axis
/// loses every digit it had.
[[nodiscard]] tg::vec3f any_perpendicular(tg::vec3f n)
{
    auto const axis = tg::abs(n[2]) < 0.9f ? tg::vec3f(0, 0, 1) : tg::vec3f(1, 0, 0);
    return tg::dual(tg::cross(axis, n)).normalized();
}
} // namespace

bool impl::is_usable_normal(tg::vec3f n)
{
    auto const length_sqr = tg::dot(n, n);

    // A NaN fails the self-comparison, an infinity fails the upper bound, and a zero fails the lower one.
    // The bounds are generous on purpose: this rejects what cannot be normalized, not what looks unusual.
    return length_sqr == length_sqr && length_sqr > 1e-16f && length_sqr < 1e16f;
}

tg::quat_f impl::tangent_frame_of(tg::vec3f normal, tg::vec3f tangent)
{
    auto const n = normal.normalized();

    // Gram-Schmidt: the component of the tangent along the normal is removed rather than trusted away.
    auto const projected = tangent - n * tg::dot(n, tangent);
    auto const t = projected.length() > 1e-8f ? projected.normalized() : any_perpendicular(n);

    // Right-handed by construction; the file's own handedness travels separately, as `tangent_handedness`.
    return tg::quat_f::make_from_basis(t, tg::dual(tg::cross(n, t)), n).normalized();
}

tg::quat_f impl::tangent_frame_of(tg::vec3f normal)
{
    auto const n = normal.normalized();
    auto const t = any_perpendicular(n);
    return tg::quat_f::make_from_basis(t, tg::dual(tg::cross(n, t)), n).normalized();
}

impl::uv_tangent_frames impl::tangent_frames_from_uvs(cc::span<tg::pos3f const> positions,
                                                      cc::span<tg::vec3f const> normals,
                                                      cc::span<tg::vec2f const> uvs,
                                                      cc::span<u32 const> indices)
{
    auto tangents = cc::vector<tg::vec3f>::create_filled(positions.size(), tg::vec3f::zero);
    auto bitangents = cc::vector<tg::vec3f>::create_filled(positions.size(), tg::vec3f::zero);

    for (auto i = isize(0); i + 2 < indices.size(); i += 3)
    {
        auto const a = isize(indices[i]);
        auto const b = isize(indices[i + 1]);
        auto const c = isize(indices[i + 2]);

        auto const e1 = positions[b] - positions[a];
        auto const e2 = positions[c] - positions[a];
        auto const d1 = uvs[b] - uvs[a];
        auto const d2 = uvs[c] - uvs[a];

        // Solving e = t * du + b * dv for both edges; a uv-degenerate triangle says nothing about either direction.
        auto const det = d1[0] * d2[1] - d2[0] * d1[1];
        if (!(det * det > 1e-20f))
            continue;

        // Each direction normalized, then weighted by the triangle's object-space area.
        // Left as solved, a triangle would weigh by its uv stretch instead, and a tiny one mapped to a sliver of uv space would outvote its neighbors.
        auto const r = 1.0f / det;
        auto const area = tg::dual(tg::cross(e1, e2)).length() * 0.5f;
        auto t = (e1 * d2[1] - e2 * d1[1]) * r;
        auto bt = (e2 * d1[0] - e1 * d2[0]) * r;
        auto const t_length = t.length();
        auto const bt_length = bt.length();
        if (!(t_length > 0.0f) || !(bt_length > 0.0f))
            continue;
        t = t * (area / t_length);
        bt = bt * (area / bt_length);
        for (auto const v : {a, b, c})
        {
            tangents[v] += t;
            bitangents[v] += bt;
        }
    }

    auto out = uv_tangent_frames();
    out.frames.reserve(positions.size());
    out.handedness.reserve(positions.size());
    for (auto v = isize(0); v < positions.size(); ++v)
    {
        auto const n = normals[v].normalized();
        out.frames.push_back(tangent_frame_of(n, tangents[v]));

        // The mirror bit: whether the uv layout's v runs along cross(n, t) or against it.
        out.handedness.push_back(tg::dot(tg::dual(tg::cross(n, tangents[v])), bitangents[v]) < 0.0f ? -1.0f : 1.0f);
    }
    return out;
}
} // namespace sv
