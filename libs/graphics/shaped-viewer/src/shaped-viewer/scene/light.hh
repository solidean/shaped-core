#pragma once

#include <clean-core/container/variant.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/stable_id.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// The lights a scene holds: one record, `sv::light`, whatever kind of light it is.
///
/// The kind is the *path the path tracer takes* to sample it, not a geometric shape, so the tag enumerates exactly the
/// branches the integrator has.
/// A caller never writes the tag: fixture factories (`point`, `spot`, `rect`, `directional`, `sun`) pick the path and
/// fill its payload, and chained setters fill the rest — `light::spot(p, down, 25_deg_f).candela(800)`.
///
/// libs/graphics/shaped-viewer/docs/lights.md is the design: why one record rather than a type per kind, why the tag is
/// a path, what the units mean, and what is designed but not landed yet.

/// The path a light takes through the integrator.
/// Two lights share a path exactly when they are sampled, weighted and intersected the same way.
enum class sv::light_path : sv::u8
{
    /// A point source: a delta light, falling off with the inverse square, never reached by a sampled ray.
    point,

    /// A flat emitting patch, sampled by area; `area_shape` says which in-region test it runs.
    area,

    /// A point source infinitely far away: a delta light arriving along one direction, with no falloff.
    distant_point,

    /// A disc infinitely far away, with an angular size — the sun, sampled within the cone it subtends.
    distant_disc,
};

/// The in-region test an `area` light runs.
/// A disc shares the `area` path, differing only in this test and its area; it is designed and not landed yet.
enum class sv::area_shape : sv::u8
{
    rect,
};

/// The unit a light's intensity is given in.
///
/// Which units a light accepts depends on its path, and an unaccepted pair asserts where it is written:
/// `candela` and `lumen` for a point, `nit`, `candela` and `lumen` for an area, `lux` for either distant path.
///
/// **One nit is one unit of the path tracer's radiance**, which is also what OpenPBR's `emission_luminance` means — so
/// an emissive surface and an area light of equal luminance give equal radiance.
enum class sv::light_unit : sv::u8
{
    /// Luminous intensity, lumen per steradian — what a point or spot is specified in, and what glTF gives one.
    candela,

    /// Illuminance, lumen per square meter on a surface facing the light — what a distant light is specified in.
    lux,

    /// Luminance, candela per square meter of emitting surface.
    /// Held fixed while an area light is resized, so a bigger light is brighter.
    nit,

    /// Luminous flux, the whole output in every direction — the number a bulb is sold by.
    /// Held fixed while an area light is resized, so a bigger light is softer rather than brighter.
    lumen,
};

/// Which face of an area light emits.
/// The front is the one the placement's -Z points out of.
enum class sv::light_face : sv::u8
{
    front,
    back,
    both,
};

enum class sv::light_shaping_kind : sv::u8
{
    none,

    /// Restricted to a cone about the placement's -Z, falling off between `inner` and `outer` by glTF's formula.
    cone,
};

/// A directional restriction applied on top of a light's path — what makes a point a spot and a rect a gridded softbox.
struct sv::light_shaping
{
    light_shaping_kind kind = light_shaping_kind::none;

    /// Half-angles from the -Z axis; `inner` must not exceed `outer`, and equal angles are a hard edge.
    tg::angle_f inner = {};
    tg::angle_f outer = {};

    [[nodiscard]] friend constexpr bool operator==(light_shaping const&, light_shaping const&) = default;
};

/// What a `point` light carries beyond its placement: nothing, since its position is the placement's origin.
struct sv::point_payload
{
    static constexpr light_path path = light_path::point;

    [[nodiscard]] friend constexpr bool operator==(point_payload const&, point_payload const&) = default;
};

/// What an `area` light carries: its in-region test and its size in its own frame, spanning local x and y.
struct sv::area_payload
{
    static constexpr light_path path = light_path::area;

    area_shape shape = area_shape::rect;
    tg::vec2f half_extents = {};

    [[nodiscard]] friend constexpr bool operator==(area_payload const&, area_payload const&) = default;
};

/// What a `distant_point` light carries: nothing, since the direction it arrives along is the placement's -Z.
struct sv::distant_point_payload
{
    static constexpr light_path path = light_path::distant_point;

    [[nodiscard]] friend constexpr bool operator==(distant_point_payload const&, distant_point_payload const&) = default;
};

/// What a `distant_disc` light carries: the angular radius of the disc it subtends.
struct sv::distant_disc_payload
{
    static constexpr light_path path = light_path::distant_disc;

    tg::angle_f angular_radius = {};

    [[nodiscard]] friend constexpr bool operator==(distant_disc_payload const&, distant_disc_payload const&) = default;
};

/// Everything about a light that does not depend on its path — a plain aggregate, so it takes designated initializers.
///
/// `unit` defaults to `candela`, which only a point or an area accepts: a designated emission for a distant light has
/// to name `lux`.
struct sv::light_emission
{
    /// chromaticity, nominally in [0, 1]; the brightness is `intensity`
    tg::vec3f color = tg::vec3f(1);

    /// in `unit`, and must be >= 0
    f32 intensity = 1;
    light_unit unit = light_unit::candela;

    /// stops on top of `intensity`: the light emits `intensity * 2^exposure`
    f32 exposure = 0;

    /// area lights only; the others emit from no face at all
    light_face face = light_face::front;

    /// Whether a camera ray reaching the light sees it.
    /// Only a light with an extent — an area light or a sun — can be seen; off by default, so a softbox lights a shot
    /// without appearing in it.
    bool visible_to_camera = false;

    /// Whether geometry between the light and a surface blocks it.
    /// Off lights everything as though nothing stood in the way — a fill that illuminates without darkening.
    bool casts_shadows = true;

    [[nodiscard]] friend constexpr bool operator==(light_emission const&, light_emission const&) = default;
};

/// One light, of any kind.
///
/// **Factories are the only way to build one**, and each leaves it valid: the path and its payload agree, and the unit
/// is one the path accepts.
/// The setters assert rather than allow an invalid pair, so a light that exists is a light the renderer can read.
///
/// `placement` may move, turn and uniformly scale a light but never shear it, which is what keeps every light one its
/// path's sampler can handle.
/// A light points along its placement's -Z: a spot's axis, a rect's front face, the direction a distant light arrives
/// along.
class sv::light
{
public:
    using payload_variant = cc::variant<point_payload, area_payload, distant_point_payload, distant_disc_payload>;

    // factories
public:
    /// A point source at `position`, in candela.
    [[nodiscard]] static light point(tg::pos3f position);

    /// A point source at `position` restricted to a cone about `direction`, in candela.
    /// The cone is glTF's: full intensity inside `inner_half_angle`, falling to zero at `outer_half_angle`.
    [[nodiscard]] static light spot(tg::pos3f position,
                                    tg::vec3f direction,
                                    tg::angle_f outer_half_angle,
                                    tg::angle_f inner_half_angle = {});

    /// The rect spanning `center ± half_extent_u ± half_extent_v`, emitting along `cross(half_extent_u, half_extent_v)`,
    /// in nits.
    /// The two half-extents must be perpendicular, since the placement cannot shear.
    [[nodiscard]] static light rect(tg::pos3f center, tg::vec3f half_extent_u, tg::vec3f half_extent_v);

    /// The rect spanning `half_extents` along the placement's local x and y, emitting along its -Z, in nits.
    [[nodiscard]] static light rect(tg::similarity_transform3f const& placement, tg::vec2f half_extents);

    /// Parallel light travelling along `direction`, in lux.
    [[nodiscard]] static light directional(tg::vec3f direction);

    /// The sun: a disc of `angular_diameter` whose light travels along `direction`, in lux.
    /// The default is the real sun's, about half a degree.
    [[nodiscard]] static light sun(tg::vec3f direction,
                                   tg::angle_f angular_diameter = tg::angle_f::make_from_degree(0.53f));

    // setters, chained on a fresh light
public:
    /// Sets the intensity and its unit together; each asserts that this light's path accepts the unit.
    light& candela(f32 value);
    light& lux(f32 value);
    light& nits(f32 value);
    light& lumens(f32 value);

    light& color(tg::vec3f c);
    light& exposure(f32 stops);

    /// Which face emits; asserts on anything but an area light.
    light& face(light_face f);

    /// Restricts the light to a cone about its -Z; asserts on a distant light, where a cone would be a barn door.
    light& cone(tg::angle_f inner_half_angle, tg::angle_f outer_half_angle);

    /// An area light's own emission narrowed to a hard-edged cone — a softbox with a grid over it.
    light& spread(tg::angle_f half_angle);

    /// Whether the camera sees the light itself; asserts on a point or parallel light, which has no extent to be seen.
    light& visible_to_camera(bool visible = true);

    /// Whether geometry blocks the light.
    light& casts_shadows(bool casts);

    // queries
public:
    [[nodiscard]] light_path path() const;
    [[nodiscard]] payload_variant const& payload() const { return _payload; }

    /// Each asserts the path it reads from.
    [[nodiscard]] area_shape shape() const;
    [[nodiscard]] tg::vec2f half_extents() const;
    [[nodiscard]] tg::angle_f angular_radius() const;

    /// The world-space area of an area light's emitting surface, per face.
    [[nodiscard]] f32 area() const;

    [[nodiscard]] friend bool operator==(light const&, light const&) = default;

    // members
public:
    tg::similarity_transform3f placement = {};
    light_emission emission = {};
    light_shaping shaping = {};

private:
    light(payload_variant payload, tg::similarity_transform3f const& placement, light_unit unit);

    payload_variant _payload;
};

namespace sv
{
/// What is wrong with `l`, or empty when nothing is: a unit its path does not accept, a cone on a distant light, a face
/// on anything but an area light, a negative intensity, a light with no extent made visible to the camera.
/// The setters and `scene_ref::add_light` assert on it; exposed so a caller holding a light from elsewhere can check.
[[nodiscard]] cc::string_view light_problem(light const& l);

/// The light a scene layer with no lights of its own is traced under, unless the layer turns it off.
/// A sun from above and slightly behind, about as bright on the ground as the old overhead key light was.
[[nodiscard]] light default_fallback_light();
} // namespace sv

/// One light as a scene layer holds it: the light, plus the identity it keeps across frames.
struct sv::scene_light
{
    light_id id;
    sv::light light;
};

/// One light as the path tracer reads it — mirrors `sv::light` in shaders/light.hlsli, so keep the two in lockstep.
///
/// Tagged by `path` rather than typed per kind, which is what lets the trace hold every light in one buffer.
/// Units are resolved before this, so the shader never sees one, and `emission` means one canonical quantity per path:
///
///   point          intensity — irradiance at distance d along the axis is emission / d^2
///   area           radiance of each emitting face
///   distant_point  irradiance on a surface facing the light
///   distant_disc   radiance of the disc
///
/// `area` is stored rather than formed in the shader because both estimators must use the same number — the next-event
/// sample and the bounce ray reaching the light weight against each other, and a density the two disagree about makes the
/// image wrong rather than noisy.
///
/// The cone is stored as the scale and offset of glTF's falloff, `saturate(cos * cone_scale + cone_offset)^2`, so an
/// unshaped light is scale 0 and offset 1 and needs no branch.
///
/// Every byte is written, pads included, since the trace hash covers these bytes and equal lights must hash equal.
struct sv::light_gpu
{
    /// Bits of `flags`, each chosen so that 0 is the default — a zeroed record is an ordinary, shadowing, unseen light.
    static constexpr u32 flag_two_sided = 1u << 0;
    static constexpr u32 flag_visible_to_camera = 1u << 1;
    static constexpr u32 flag_casts_no_shadow = 1u << 2;

    tg::vec3f position = {}; ///< point: the source; area: the rect's center; distant: unused
    u32 path = 0;            ///< a `light_path`
    tg::vec3f u = {};        ///< area: world half-extent spanning the rect's first axis
    f32 area = 0;            ///< area: the world area of one face
    tg::vec3f v = {};        ///< area: world half-extent spanning the rect's second axis
    u32 flags = 0;           ///< `flag_*` bits
    tg::vec3f emission = {}; ///< the canonical quantity above, per path
    f32 cone_scale = 0;      ///< glTF's falloff scale; 0 for an unshaped light
    tg::vec3f normal
        = {}; ///< the placement's -Z: a rect's front face, a spot's axis, the direction a distant light travels
    f32 cone_offset = 1;        ///< glTF's falloff offset; 1 for an unshaped light
    f32 cos_angular_radius = 1; ///< distant_disc: cosine of the disc's angular radius

    /// RESERVED for light linking, and read by nothing yet: a light will affect an instance when the two masks share a bit.
    /// All ones is "everything", which is what every light does today.
    u32 link_mask = ~0u;
    f32 _pad0[2] = {};

    /// Lays `l` out for the tracer, its intensity converted to the canonical quantity of its path.
    /// A `back` face flips the normal rather than setting a flag, so only `both` needs one.
    [[nodiscard]] static light_gpu from(light const& l);
};

namespace sv
{
static_assert(sizeof(light_gpu) == 96, "light_gpu must match sv::light in shaders/light.hlsli");
} // namespace sv
