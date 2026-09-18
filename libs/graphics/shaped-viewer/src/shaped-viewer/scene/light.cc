#include <clean-core/common/asserts.hh>
#include <shaped-viewer/scene/light.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs, tg::pow

namespace sv
{
namespace
{
/// A rotation taking local +Z onto the unit vector `z`, with an arbitrary but well-conditioned roll about it.
///
/// TEMPORARY: the same construction as `any_perpendicular` in asset/impl/tangent_frames.cc.
/// Both are waiting on typed-geometry growing an orthonormal-basis routine, which is where this belongs.
[[nodiscard]] tg::quat_f frame_with_z(tg::vec3f z)
{
    auto const axis = tg::abs(z[2]) < 0.9f ? tg::vec3f(0, 0, 1) : tg::vec3f(1, 0, 0);
    auto const x = tg::dual(tg::cross(axis, z)).normalized();
    return tg::quat_f::make_from_basis(x, tg::dual(tg::cross(z, x)), z).normalized();
}

/// A placement at `position` whose -Z points along `direction`.
[[nodiscard]] tg::similarity_transform3f pointing(tg::pos3f position, tg::vec3f direction)
{
    CC_ASSERT(direction.length() > 0.0f, "a light's direction must not be the zero vector");
    return tg::similarity_transform3f::make_translation(position - tg::pos3f::zero)
        .composed(tg::similarity_transform3f::make_rotation(frame_with_z(-direction.normalized())));
}

void assert_valid(light const& l)
{
    auto const problem = light_problem(l);
    CC_ASSERTS(problem.empty(), problem);
}

/// Applies `change` to a copy and commits it only once it is valid, so a setter that asserts leaves `l` as it was.
template <class F>
light& checked(light& l, F&& change)
{
    auto next = l;
    change(next);
    assert_valid(next);
    l = cc::move(next);
    return l;
}
} // namespace

light::light(payload_variant payload, tg::similarity_transform3f const& placement, light_unit unit)
  : placement(placement), emission({.unit = unit}), _payload(cc::move(payload))
{
}

// ---- factories -------------------------------------------------------------------------------------------

light light::point(tg::pos3f position)
{
    return light(point_payload{}, tg::similarity_transform3f::make_translation(position - tg::pos3f::zero),
                 light_unit::candela);
}

light light::spot(tg::pos3f position, tg::vec3f direction, tg::angle_f outer_half_angle, tg::angle_f inner_half_angle)
{
    auto l = light(point_payload{}, pointing(position, direction), light_unit::candela);
    l.cone(inner_half_angle, outer_half_angle);
    return l;
}

light light::rect(tg::pos3f center, tg::vec3f half_extent_u, tg::vec3f half_extent_v)
{
    auto const length_u = half_extent_u.length();
    auto const length_v = half_extent_v.length();
    CC_ASSERT(length_u > 0.0f && length_v > 0.0f, "a rect light's half-extents must not be zero");
    CC_ASSERT(tg::abs(tg::dot(half_extent_u, half_extent_v)) <= 1e-4f * length_u * length_v,
              "a rect light's half-extents must be perpendicular — its placement cannot shear");

    // The rect emits along cross(u, v), and a light emits along its -Z, so Z is the reverse of that.
    // Local x runs along u, and local y = cross(Z, x) then runs along -v, which spans the same rect.
    auto const z = -tg::dual(tg::cross(half_extent_u, half_extent_v)).normalized();
    auto const x = half_extent_u / length_u;
    auto const rotation = tg::quat_f::make_from_basis(x, tg::dual(tg::cross(z, x)), z).normalized();

    auto const placement = tg::similarity_transform3f::make_translation(center - tg::pos3f::zero)
                               .composed(tg::similarity_transform3f::make_rotation(rotation));
    return rect(placement, tg::vec2f(length_u, length_v));
}

light light::rect(tg::similarity_transform3f const& placement, tg::vec2f half_extents)
{
    CC_ASSERT(half_extents[0] > 0.0f && half_extents[1] > 0.0f, "a rect light's half-extents must be positive");
    return light(area_payload{.shape = area_shape::rect, .half_extents = half_extents}, placement, light_unit::nit);
}

light light::directional(tg::vec3f direction)
{
    return light(distant_point_payload{}, pointing(tg::pos3f::zero, direction), light_unit::lux);
}

light light::sun(tg::vec3f direction, tg::angle_f angular_diameter)
{
    CC_ASSERT(angular_diameter.radians() >= 0.0f, "a sun's angular diameter must not be negative");

    // A sun of no size is parallel light, and that is a different path rather than a degenerate disc.
    if (angular_diameter.radians() == 0.0f)
        return directional(direction);

    return light(distant_disc_payload{.angular_radius = angular_diameter / 2.0f}, pointing(tg::pos3f::zero, direction),
                 light_unit::lux);
}

// ---- setters ---------------------------------------------------------------------------------------------

light& light::candela(f32 value)
{
    return checked(*this,
                   [&](light& l)
                   {
                       l.emission.intensity = value;
                       l.emission.unit = light_unit::candela;
                   });
}

light& light::lux(f32 value)
{
    return checked(*this,
                   [&](light& l)
                   {
                       l.emission.intensity = value;
                       l.emission.unit = light_unit::lux;
                   });
}

light& light::nits(f32 value)
{
    return checked(*this,
                   [&](light& l)
                   {
                       l.emission.intensity = value;
                       l.emission.unit = light_unit::nit;
                   });
}

light& light::lumens(f32 value)
{
    return checked(*this,
                   [&](light& l)
                   {
                       l.emission.intensity = value;
                       l.emission.unit = light_unit::lumen;
                   });
}

light& light::color(tg::vec3f c)
{
    return checked(*this, [&](light& l) { l.emission.color = c; });
}

light& light::exposure(f32 stops)
{
    return checked(*this, [&](light& l) { l.emission.exposure = stops; });
}

light& light::face(light_face f)
{
    return checked(*this, [&](light& l) { l.emission.face = f; });
}

light& light::cone(tg::angle_f inner_half_angle, tg::angle_f outer_half_angle)
{
    return checked(
        *this, [&](light& l)
        { l.shaping = {.kind = light_shaping_kind::cone, .inner = inner_half_angle, .outer = outer_half_angle}; });
}

light& light::visible_to_camera(bool visible)
{
    return checked(*this, [&](light& l) { l.emission.visible_to_camera = visible; });
}

light& light::casts_shadows(bool casts)
{
    emission.casts_shadows = casts;
    return *this;
}

light& light::spread(tg::angle_f half_angle)
{
    CC_ASSERT(path() == light_path::area, "spread narrows an area light's own emission; a point light takes cone()");
    return cone(half_angle, half_angle);
}

// ---- queries ---------------------------------------------------------------------------------------------

light_path light::path() const
{
    return _payload.visit([](auto const& p) { return p.path; });
}

area_shape light::shape() const
{
    auto const* p = _payload.try_as<area_payload>();
    CC_ASSERT(p != nullptr, "shape() reads an area light");
    return p->shape;
}

tg::vec2f light::half_extents() const
{
    auto const* p = _payload.try_as<area_payload>();
    CC_ASSERT(p != nullptr, "half_extents() reads an area light");
    return p->half_extents;
}

tg::angle_f light::angular_radius() const
{
    auto const* p = _payload.try_as<distant_disc_payload>();
    CC_ASSERT(p != nullptr, "angular_radius() reads a distant_disc light");
    return p->angular_radius;
}

f32 light::area() const
{
    auto const h = half_extents();
    auto const s = placement.uniform_scale();
    return 4.0f * h[0] * h[1] * s * s;
}

cc::string_view light_problem(light const& l)
{
    auto const path = l.path();
    auto const unit = l.emission.unit;

    switch (path)
    {
    case light_path::point:
        if (unit != light_unit::candela && unit != light_unit::lumen)
            return "a point light takes candela or lumens; nits need an area and lux a distant light";
        break;
    case light_path::area:
        if (unit == light_unit::lux)
            return "an area light takes nits, candela or lumens; lux is what a distant light is given in";
        break;
    case light_path::distant_point:
    case light_path::distant_disc:
        if (unit != light_unit::lux)
            return "a distant light takes lux — it has no position for an intensity or a flux to be measured from";
        break;
    }

    if (!(l.emission.intensity >= 0.0f))
        return "a light's intensity must be >= 0";

    auto const c = l.emission.color;
    if (!(c[0] >= 0.0f && c[1] >= 0.0f && c[2] >= 0.0f))
        return "a light's color must be >= 0 in every channel";

    // Also false for a NaN, which is the case this exists for.
    if (!(tg::abs(l.emission.exposure) < 128.0f))
        return "a light's exposure must be within +-128 stops, where 2^exposure is a finite float";

    if (l.emission.face != light_face::front && path != light_path::area)
        return "only an area light has faces to choose between";

    if (l.emission.visible_to_camera && (path == light_path::point || path == light_path::distant_point))
        return "a point or parallel light has no extent for a camera to see";

    if (l.shaping.kind == light_shaping_kind::cone)
    {
        if (path == light_path::distant_point || path == light_path::distant_disc)
            return "a cone on a distant light would be a barn door, which nothing implements";
        if (l.shaping.inner > l.shaping.outer)
            return "a cone's inner half-angle must not exceed its outer one";
    }

    return {};
}

light default_fallback_light()
{
    return light::sun(tg::vec3f(-0.3f, -1.0f, -0.2f)).lux(2.0f);
}

// ---- GPU layout ------------------------------------------------------------------------------------------

light_gpu light_gpu::from(light const& l)
{
    assert_valid(l);

    auto out = light_gpu{};
    out.path = u32(l.path());

    auto const origin = l.placement.transform(tg::pos3f::zero);
    out.position = tg::vec3f(origin[0], origin[1], origin[2]);
    out.normal = l.placement.transform(tg::vec3f(0, 0, -1)).normalized();

    // glTF's falloff, `saturate(cos * scale + offset)^2`, stored as the two numbers it needs — 0 and 1 leave a light unshaped.
    //
    // The width is clamped only to keep the division finite for a hard edge.
    // glTF's sample code clamps at 1e-3, which caps the ramp's slope so a cone under ~2.6 degrees never reaches full
    // intensity; 1e-6 is a hard edge down to ~0.08 degrees.
    if (l.shaping.kind == light_shaping_kind::cone)
    {
        auto const cos_inner = tg::cos(l.shaping.inner);
        auto const cos_outer = tg::cos(l.shaping.outer);
        auto const width = cos_inner - cos_outer;
        out.cone_scale = 1.0f / (width > 1e-6f ? width : 1e-6f);
        out.cone_offset = -cos_outer * out.cone_scale;
    }

    // Converted to the canonical quantity of the path, which is all the shader ever reads.
    auto intensity = l.emission.intensity;
    switch (l.path())
    {
    case light_path::point:
        // Candela is the canonical intensity; a flux spreads over the whole sphere, whether or not a cone masks it.
        if (l.emission.unit == light_unit::lumen)
            intensity /= 4.0f * tg::pi<f32>;
        break;

    case light_path::area:
    {
        auto const h = l.half_extents();
        auto const x = l.placement.transform(tg::vec3f(h[0], 0, 0));
        auto const y = l.placement.transform(tg::vec3f(0, h[1], 0));

        // The record emits along cross(u, v), and cross(y, x) is the placement's -Z, the front.
        auto const back = l.emission.face == light_face::back;
        out.u = back ? x : y;
        out.v = back ? y : x;
        out.normal = tg::dual(tg::cross(out.u, out.v)).normalized();
        out.area = l.area();
        if (l.emission.face == light_face::both)
            out.flags |= flag_two_sided;

        // One nit is one unit of the tracer's radiance, so the others convert to it through the emitting area.
        // A Lambertian face of area A and radiance L emits pi * A * L lumens, and L * A candela along its normal.
        auto const sides = l.emission.face == light_face::both ? 2.0f : 1.0f;
        switch (l.emission.unit)
        {
        case light_unit::nit:
            break;
        case light_unit::candela:
            intensity /= out.area;
            break;
        case light_unit::lumen:
            intensity /= tg::pi<f32> * out.area * sides;
            break;
        case light_unit::lux:
            CC_UNREACHABLE("light_problem rejects lux on an area light");
        }
        break;
    }

    case light_path::distant_point:
        break; // lux is the canonical irradiance already

    case light_path::distant_disc:
    {
        // A uniform disc of radiance L and angular radius r delivers pi * sin(r)^2 * L to a surface facing it — the
        // projected solid angle of the cap — so that is what the illuminance is divided by.
        // 1 - cos(r) as 2 sin^2(r / 2), which keeps its precision where the cosine rounds to within a few ulps of 1.
        auto const r = l.angular_radius();
        auto const sin_r = tg::sin(r);
        auto const sin_half_r = tg::sin(r / 2.0f);
        out.one_minus_cos_angular_radius = 2.0f * sin_half_r * sin_half_r;
        intensity /= tg::pi<f32> * sin_r * sin_r;
        break;
    }
    }

    if (l.emission.visible_to_camera)
        out.flags |= flag_visible_to_camera;
    if (!l.emission.casts_shadows)
        out.flags |= flag_casts_no_shadow;

    out.emission = l.emission.color * (intensity * tg::pow(2.0f, l.emission.exposure));
    return out;
}
} // namespace sv
