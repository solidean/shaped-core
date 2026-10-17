#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/camera/camera.hh>

// Every projection family, so a test can state a property once and check it for all six conventions.

namespace tgtest
{
using namespace cc::primitive_defines;

struct projection_family;
} // namespace tgtest

struct tgtest::projection_family
{
    char const* name;
    tg::clip_depth depth;
    bool right_handed;
    tg::projective_transform3d (*perspective)(tg::angle_d, f64, f64, f64);
    tg::projective_transform3d (*perspective_infinite)(tg::angle_d, f64, f64);
    tg::projective_transform3d (*perspective_off_center)(f64, f64, f64, f64, f64, f64);
    tg::projective_transform3d (*orthographic)(f64, f64, f64, f64);
    tg::projective_transform3d (*orthographic_off_center)(f64, f64, f64, f64, f64, f64);

    /// the depth the near and the far plane map to
    [[nodiscard]] f64 near_depth() const
    {
        return depth == tg::clip_depth::reverse_zero_to_one ? 1.0 : depth == tg::clip_depth::zero_to_one ? 0.0 : -1.0;
    }
    [[nodiscard]] f64 far_depth() const { return depth == tg::clip_depth::reverse_zero_to_one ? 0.0 : 1.0; }

    /// a view-space point `distance` along the view axis, with this family's handedness
    [[nodiscard]] tg::pos3d ahead(f64 x, f64 y, f64 distance) const
    {
        return tg::pos3d(x, y, right_handed ? -distance : distance);
    }
};

namespace tgtest
{

// the orthographic functions return the narrowest class their convention allows, so they are widened here
#define TGTEST_FAMILY(SUFFIX, DEPTH, RH)                                                                             \
    projection_family                                                                                                \
    {                                                                                                                \
        .name = #SUFFIX, .depth = DEPTH, .right_handed = RH,                                                         \
        .perspective                                                                                                 \
            = [](tg::angle_d fov, f64 aspect, f64 n, f64 f) { return tg::perspective_##SUFFIX(fov, aspect, n, f); }, \
        .perspective_infinite                                                                                        \
            = [](tg::angle_d fov, f64 aspect, f64 n) { return tg::perspective_##SUFFIX(fov, aspect, n); },           \
        .perspective_off_center = [](f64 l, f64 r, f64 b, f64 t, f64 n, f64 f)                                       \
        { return tg::perspective_off_center_##SUFFIX(l, r, b, t, n, f); },                                           \
        .orthographic = [](f64 w, f64 h, f64 n, f64 f)                                                               \
        { return tg::projective_transform3d(tg::orthographic_##SUFFIX(w, h, n, f)); },                               \
        .orthographic_off_center = [](f64 l, f64 r, f64 b, f64 t, f64 n, f64 f)                                      \
        { return tg::projective_transform3d(tg::orthographic_off_center_##SUFFIX(l, r, b, t, n, f)); },              \
    }

inline cc::fixed_array<projection_family, 6> const k_projection_families = {
    TGTEST_FAMILY(lh_reverse_z, tg::clip_depth::reverse_zero_to_one, false),
    TGTEST_FAMILY(rh_reverse_z, tg::clip_depth::reverse_zero_to_one, true),
    TGTEST_FAMILY(lh_zero_to_one, tg::clip_depth::zero_to_one, false),
    TGTEST_FAMILY(rh_zero_to_one, tg::clip_depth::zero_to_one, true),
    TGTEST_FAMILY(lh_minus_one_to_one, tg::clip_depth::minus_one_to_one, false),
    TGTEST_FAMILY(rh_minus_one_to_one, tg::clip_depth::minus_one_to_one, true),
};

#undef TGTEST_FAMILY
} // namespace tgtest
