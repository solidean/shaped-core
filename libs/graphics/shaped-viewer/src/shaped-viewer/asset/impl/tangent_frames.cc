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
// TODO, once generated frames land: ours will not match MikkTSpace, which nearly every DCC tool bakes normal maps
// against.
// Nothing here generates a tangent yet, so the divergence has nowhere to show up.

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
} // namespace sv
