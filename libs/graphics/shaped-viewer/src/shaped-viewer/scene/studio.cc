#include <clean-core/common/utility.hh> // cc::max
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/scene/studio.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::log, tg::abs, tg::atan, tg::sin_cos

namespace sv
{
namespace
{
/// sRGB 122 of 255 decoded to linear: what the card must read on screen.
constexpr f32 card_display_linear = 0.19461f;

[[nodiscard]] tg::angle_f degrees(f32 d)
{
    return tg::angle_f::make_from_degree(d);
}
} // namespace

dome studio_dome(studio_setup setup, f32 horizon)
{
    auto const ceiling = setup == studio_setup::ceiling;
    auto const h = tg::vec3f(horizon, horizon, horizon);
    auto d = dome{.horizon = h,
                  .zenith = h * (ceiling ? 0.04f : 1.6f),
                  .rise_start = ceiling ? 0.0f : degrees(25.0f).sin(),
                  .backdrop_scale = 2.4f,
                  .follows_camera = true};
    d.lobes[0] = {.direction = camera_relative_direction(degrees(50.0f), degrees(ceiling ? -25.0f : 45.0f)),
                  .radiance = h * 40.0f};
    d.lobes[1] = {.direction = camera_relative_direction(degrees(-75.0f), degrees(18.0f)), .radiance = h * 8.0f};
    d.lobes[2] = {.direction = camera_relative_direction(degrees(180.0f), degrees(55.0f)), .radiance = h * 10.0f};
    d.lobe_count = 3;
    return d;
}

f32 studio_exposure(dome const& d)
{
    // The card faces the key; the gradient is the world's and the lobes turn about +y alone, so any camera gives the same irradiance.
    auto const turned = d.seen_from(camera{});
    auto const key = d.lobe_count > 0 ? tg::normalize(turned.lobes[0].direction) : tg::vec3f(0, 1, 0);
    auto const e = turned.irradiance(key);
    auto const luminance = 0.2126f * e[0] + 0.7152f * e[1] + 0.0722f * e[2];
    auto const card = studio_card_albedo / tg::pi<f32> * cc::max(luminance, 1e-12f);
    return tg::log(card_display_linear / card) / tg::log(2.0f);
}

mesh studio_floor(f32 height, f32 half_extent)
{
    auto const lib = acquire_material_library();
    CC_ASSERT(lib.has_value(), "shaped-viewer: no material library for the studio floor");
    auto const type = lib.value()->acquire_type(builtin_material::openpbr);
    CC_ASSERT(type.has_value(), "the studio floor needs the builtin 'openpbr' type — register_builtin_material_types");
    auto const material = lib.value()->acquire(
        material::create("studio floor", type.value(),
                         {material_attribute_binding::of(
                              "base_color", tg::vec3f(studio_floor_albedo, studio_floor_albedo, studio_floor_albedo)),
                          material_attribute_binding::of("specular_weight", 0.0f)}));

    auto const e = half_extent;
    return mesh{.name = "studio floor",
                .geometry = triangle_geometry::create_from_positions(cc::vector<tg::pos3f>{
                    tg::pos3f(-e, height, -e), tg::pos3f(e, height, e), tg::pos3f(e, height, -e),
                    tg::pos3f(-e, height, -e), tg::pos3f(-e, height, e), tg::pos3f(e, height, e)}),
                .material = material};
}

camera studio_camera(tg::aabb3f const& bounds, studio_view view, f64 aspect, tg::angle_d front_azimuth)
{
    // 36 mm wide at 50 mm: the horizontal half-angle, and the vertical one the aspect leaves.
    auto const tan_half_h = 18.0 / 50.0;
    auto const tan_half_v = tan_half_h / aspect;

    auto const lo = tg::vec3d(bounds.min[0], bounds.min[1], bounds.min[2]);
    auto const diagonal = tg::vec3d(bounds.max[0], bounds.max[1], bounds.max[2]) - lo;
    auto const center = tg::pos3d::zero + (lo + diagonal * 0.5);

    // The direction first, at unit distance; the distance is then the least at which every corner of the box is in view.
    auto cam = camera{};
    if (view == studio_view::top)
    {
        auto const [s, c] = tg::sin_cos(front_azimuth);
        cam = camera::looking_at(center + tg::vec3d(0, 1, 0), center, tg::vec3d(-s, 0, c));
    }
    else
    {
        auto const azimuth = [&]
        {
            switch (view)
            {
            case studio_view::front_right:
                return 45.0;
            case studio_view::right:
                return 90.0;
            case studio_view::back:
                return 180.0;
            case studio_view::front_left:
                return -45.0;
            default:
                return 0.0;
            }
        }();
        cam = camera::orbiting(center, 1.0, front_azimuth + tg::angle_d::make_from_degree(azimuth),
                               tg::angle_d::make_from_degree(10.0));
    }

    // A corner at (x, y, z) about the center, in the camera's axes, is in view from distance d where |x| <= (d + z) tan_h, and the same for y.
    // A tenth is kept free around the box, so it never touches the frame.
    auto const basis = cam.basis();
    auto const margin = 1.1;
    auto distance = 1e-6;
    auto nearest = 1e30;
    for (auto corner = 0; corner < 8; ++corner)
    {
        auto const offset
            = tg::vec3d(corner & 1 ? diagonal[0] : 0.0, corner & 2 ? diagonal[1] : 0.0, corner & 4 ? diagonal[2] : 0.0)
            - diagonal * 0.5;
        auto const x = tg::dot(offset, basis.right);
        auto const y = tg::dot(offset, basis.up);
        auto const z = tg::dot(offset, basis.forward);
        distance = cc::max(distance, cc::max(tg::abs(x) * margin / tan_half_h - z, tg::abs(y) * margin / tan_half_v - z));
        nearest = z < nearest ? z : nearest;
    }

    cam.position = center - basis.forward * distance;
    cam.projection.vertical_fov = tg::angle_d::make_from_radians(2.0 * tg::atan(tan_half_v).radians());
    cam.projection.aspect_ratio = aspect;
    cam.projection.near_plane = cc::max((distance + nearest) * 0.5, 1e-4);
    return cam;
}
} // namespace sv
