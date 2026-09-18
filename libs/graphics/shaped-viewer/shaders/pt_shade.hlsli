#pragma once

#include "pt_common.hlsli"

// The path tracer's SHADING, once a hit has been located — shared by every geometry kind.
//
// It is an epilogue's tail rather than a shader: it calls `sv_evaluate_material`, which a generated file defines above it, so
// this is included by a hit epilogue rather than at the top of one.
//
// What a caller does first is find the hit and describe it — a triangle reads its corners, a quadric solves its quadratic — and
// everything from there on is the same: evaluate the material, prepare the closure, estimate direct light through it, sample the
// continuation, and hand the raygen a result it only has to accumulate.
// That split is why this file exists: the two geometry kinds differ only in how `ctx`, `N` and `V` are obtained, and shading
// them twice would be two copies of the layered closure to keep in step.
//
// The SHADING happens here rather than in the raygen, because the material's BSDF is a layered closure of some twenty
// parameters and this is the only place it exists as a value.
/// Whether anything blocks the segment from `origin` along `dir` for `dist`.
///
/// Callers offset the origin along the GEOMETRIC normal rather than the shading one: a normal map can tilt the shading normal
/// far enough that an offset along it starts the ray below the surface, which self-shadows the very pixels it was meant to
/// protect.
///
/// Skips the closest-hit — only visibility matters — and routes to the shadow miss (miss index 1), which is what sets
/// `visible` on a clear path.
///
/// `RayContributionToHitGroupIndex` is 1, so this selects the permutation's SHADOW hit record rather than its primary one.
/// A hit group's any-hit is invoked with whatever payload its caller passed, and an any-hit can declare only one type — so
/// the two rays cannot share a record while they carry different payloads.
/// The shadow record holds `PtShadowAnyHit` and no closest hit, which is why a cutout still casts the shadow it should.
bool pt_occluded(float3 origin, float3 dir, float dist)
{
    ShadowPayload sp;
    sp.visible = 0.0; // assume occluded; only the shadow miss flips this

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-3;
    ray.TMax = dist;
    TraceRay(pt_bindings::scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 0, 1, ray, sp);

    return sp.visible < 0.5;
}

/// Direct light from one of the trace's lights, with the BSDF folded in and weighted against the BSDF sampler.
///
/// One light is picked uniformly and its choice is part of the density (`pt_light_pdf`, `pt_disc_pdf`), which is what keeps
/// one shadow ray per hit an unbiased estimate of every light at once.
///
/// A point or a parallel light is a delta: no sampled ray can ever reach it, so it takes no weighting at all and its estimate
/// is divided by the probability of picking it alone.
/// A rect and a disc are reachable by the raygen's continuation ray too, and weight against it.
/// That second strategy is what makes this bounded on a near-smooth surface.
/// Without it, light sampling alone has to carry the whole GGX peak: `bsdf_eval` at a uniformly picked point on the rect is
/// ~1/(pi*alpha^2) where the half-vector lines up and near zero everywhere else, so a mirror lit by a small light produces a
/// huge value at a tiny probability — one bright pixel per few thousand samples, which is what a firefly is.
float3 pt_estimate_light(sv::bsdf bsdf,
                         sv::frame frame,
                         float3 wo_local,
                         float3 p,
                         float3 n_geom,
                         bool last_bounce,
                         inout uint rng)
{
    uint const count = pt_bindings::frame.light_count;
    if (count == 0u)
        return float3(0, 0, 0);

    // No draw when there is nothing to choose between, so a one-light scene keeps the sample sequence it always had.
    uint const index = count == 1u ? 0u : min(uint(pt_rand(rng) * float(count)), count - 1u);
    sv::light light = pt_bindings::Lights[index];

    // Filled per path: where the shadow ray goes and how far, what arrives along it before the BSDF — radiance, or
    // irradiance for a delta light — and the density of having chosen that direction, the light's pick included.
    float3 wi = float3(0, 0, 0);
    float shadow_dist = 0.0;
    float3 incoming = float3(0, 0, 0);
    float pdf = 0.0;
    bool delta = false;

    if (light.path == sv::light_path_area)
    {
        // uniform sample on the oriented rectangle: center +/- along each world half-edge vector
        float s = pt_rand(rng) * 2.0 - 1.0;
        float t = pt_rand(rng) * 2.0 - 1.0;
        float3 to_light = light.position + s * light.u + t * light.v - p;
        float dist2 = dot(to_light, to_light);
        float dist = sqrt(dist2);
        wi = to_light / dist;

        float cos_face = dot(light.normal, -wi);
        float cos_light = (light.flags & sv::light_flag_two_sided) != 0u ? abs(cos_face) : cos_face;
        if (cos_light <= 0.0)
            return float3(0, 0, 0); // the light's emitting face is turned away

        // Stop just short of the light surface, so the light's own geometry does not count as an occluder.
        shadow_dist = dist - 2e-3;
        incoming = light.emission * sv::light_cone(light, cos_light);

        // Formed in pt_common.hlsli, because the raygen's half of this weighting has to arrive at the same number.
        pdf = pt_light_pdf(light, dist2, cos_light);
    }
    else if (light.path == sv::light_path_point)
    {
        float3 to_light = light.position - p;
        float dist2 = max(dot(to_light, to_light), 1e-12);
        float dist = sqrt(dist2);
        wi = to_light / dist;

        shadow_dist = dist - 2e-3;
        incoming = light.emission * (sv::light_cone(light, dot(light.normal, -wi)) / dist2);
        pdf = pt_light_select_pdf();
        delta = true;
    }
    else if (light.path == sv::light_path_distant_point)
    {
        wi = -light.normal;
        shadow_dist = 1e20;
        incoming = light.emission;
        pdf = pt_light_select_pdf();
        delta = true;
    }
    else // sv::light_path_distant_disc
    {
        wi = pt_sample_cone(-light.normal, light.cos_angular_radius, pt_rand(rng), pt_rand(rng));
        shadow_dist = 1e20;
        incoming = light.emission;
        pdf = pt_disc_pdf(light);
    }

    if (all(incoming <= float3(0, 0, 0)))
        return float3(0, 0, 0);

    float3 wi_local = sv::to_local(frame, wi);
    if (wi_local.z <= 0.0)
        return float3(0, 0, 0); // below the surface, so the BSDF is zero there anyway

    float3 f = sv::bsdf_eval(bsdf, wo_local, wi_local);
    if (all(f <= float3(0, 0, 0)))
        return float3(0, 0, 0);

    if (sv::light_casts_shadows(light) && pt_occluded(p + n_geom * 1e-3, wi, shadow_dist))
        return float3(0, 0, 0);

    // The other strategy for this direction is the BSDF sample the raygen may take, so balance the two — except for a
    // delta light, which that sample can never reach, and on the last bounce, where the raygen takes none.
    float w = delta || last_bounce ? 1.0 : pt_mis_weight(pdf, sv::bsdf_pdf(bsdf, wo_local, wi_local));

    return incoming * f * (wi_local.z / pdf) * w;
}

/// Direct light from the SH environment, with the BSDF folded in and weighted against the BSDF sampler.
///
/// One uniform-hemisphere sample: the probe has no sharp features, so a radiance-proportional sampler is not worth its cost,
/// and the multiple-importance weight already cuts the variance a bright, non-uniform sky would add.
float3 pt_estimate_environment(sv::bsdf bsdf,
                               sv::frame frame,
                               float3 wo_local,
                               float3 p,
                               float3 n_geom,
                               bool last_bounce,
                               inout uint rng)
{
    // cos(theta) = u1 uniform in [0, 1] gives a uniform solid-angle pick about the normal.
    float u1 = pt_rand(rng);
    float u2 = pt_rand(rng);
    float r = sqrt(max(0.0, 1.0 - u1 * u1));
    float phi = 2.0 * PT_PI * u2;

    float3 wi_local = float3(r * cos(phi), r * sin(phi), u1);
    float3 wi = sv::to_world(frame, wi_local);

    float3 f = sv::bsdf_eval(bsdf, wo_local, wi_local);
    if (all(f <= float3(0, 0, 0)))
        return float3(0, 0, 0);

    if (pt_occluded(p + n_geom * 1e-3, wi, 1e4))
        return float3(0, 0, 0);

    // The other strategy for this direction is the BSDF sample the raygen may take, so balance the two.
    // No bounce ray follows the last hit, so there is nothing to balance against there.
    float w = last_bounce ? 1.0 : pt_mis_weight(PT_ENV_PDF, sv::bsdf_pdf(bsdf, wo_local, wi_local));
    return background_radiance(pt_bindings::background.sh, wi) * f * (wi_local.z / PT_ENV_PDF) * w;
}

/// Shades a located hit and writes the whole payload.
///
/// `ctx` is what the material reads its attributes through, `N` is the GEOMETRIC normal already turned to face the ray, `V` is
/// the direction back toward the viewer, and `exiting` says the ray arrived from inside the surface — which a refraction needs,
/// since the index ratio inverts between going in and coming back out.
///
/// Valid only in a closest-hit: it reads `RayTCurrent()` and `ObjectToWorld3x4()`, and it traces shadow rays.
void pt_shade(inout PtPayload payload, sv::shading_context ctx, float3 N, float3 V, bool exiting)
{
    payload.normal = N;
    payload.hit_t = RayTCurrent();

    // The random state round-trips through every path out of here, so a stream is never left half-advanced.
    uint rng = payload.rng;

    sv::surface surface = sv_evaluate_material(ctx);

    // A dispersive interface collapses the path onto ONE wavelength, because three channels bent through three different
    // angles are no longer one ray and nothing downstream may treat them as one.
    // Drawn here, before the closure is prepared, since the index it refracts at is what the choice decides.
    // A path that already collapsed keeps its wavelength — collapsing twice would mask it twice over.
    uint channel = payload.channel;
    bool const collapsing = channel >= 3u && surface.transmission_dispersion_scale > 0.0
                          && surface.transmission_weight > 0.0;
    if (collapsing)
        channel = min(uint(pt_rand(rng) * 3.0), 2u);

    sv::bsdf bsdf = sv::bsdf_prepare(surface, exiting, channel);

    // The shading frame, in three steps: the authored one if there is one, faced toward the ray, then the normal map on top.
    //
    // `SV_ATTR_SUPPLIED_tangent_frame` is the generator's, one constant per permutation, and it is what separates "the mesh
    // carries a frame" from "the declaration's identity default came through" — the second is object-space +z, which belongs
    // to no surface.
    sv::frame frame = sv::make_frame(N);

#if SV_ATTR_SUPPLIED_tangent_frame
    {
        // The frame is authored in object space, so it moves with the instance the geometry does.
        // Rotating the three axes by ObjectToWorld and renormalizing is exact for a rigid or uniformly scaled placement; a
        // non-uniform scale would want the inverse transpose for the normal, and shears the frame here. See the viewer TODO.
        sv::frame local = sv::frame_from_quaternion(surface.geometry_tangent_frame, surface.geometry_handedness);
        float3x3 to_world = (float3x3)ObjectToWorld3x4();

        frame.t = normalize(mul(to_world, local.t));
        frame.b = normalize(mul(to_world, local.b));
        frame.n = normalize(mul(to_world, local.n));

        // Two-sided, against the same test the geometric normal took: an authored frame faces the surface's front, and a ray
        // arriving from behind needs the whole basis turned rather than only its normal.
        if (dot(frame.n, V) < 0.0)
            frame = sv::flip_frame(frame);
    }
#endif

    // The frame as authored, kept because the coat's normal is written in the same tangent space the base's is — so it has
    // to be carried through THIS frame rather than through the one the base's own normal map produced.
    sv::frame const authored = frame;

    // A normal map is a rotation of the frame, not a replacement for it: the tangent is carried across so the frame keeps
    // following the uv layout.
    if (any(surface.geometry_normal != float3(0, 0, 1)))
        frame = sv::perturb_frame(frame, surface.geometry_normal);

    // The anisotropy direction, applied AFTER the normal map, because it is a rotation about the shading normal and the
    // normal map is what decides which normal that is.
    if (any(surface.geometry_tangent != float3(1, 0, 0)))
        frame = sv::rotate_frame_to_tangent(frame, surface.geometry_tangent);

    // The coat's normal, re-expressed in the frame the closure works in.
    // An unbound one is the base's own normal, which is (0, 0, 1) there whatever the base's normal map did — so the guard
    // is what keeps "the coat shares the base's normal" exact rather than nearly so.
    if (any(surface.geometry_coat_normal != float3(0, 0, 1)))
    {
        float3 const coat_world = normalize(sv::to_world(authored, normalize(surface.geometry_coat_normal)));
        surface.geometry_coat_normal = normalize(sv::to_local(frame, coat_world));
    }

    float3 wo_local = sv::to_local(frame, V);
    float3 p = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    payload.emission = bsdf.emission;

    // What the medium was on the way in, so a continuation that stays on this side keeps travelling through it.
    float3 const in_sigma_t = payload.medium_sigma_t;
    float3 const in_albedo = payload.medium_albedo;
    float const in_g = payload.medium_g;

    // A grazing hit whose shading frame turned away has no hemisphere to integrate over.
    if (wo_local.z <= 0.0)
    {
        payload.direct = float3(0, 0, 0);
        payload.throughput = float3(0, 0, 0);
        payload.direction = float3(0, 0, 0);
        payload.bsdf_pdf = 0.0;
        payload.rng = rng;
        payload.medium_sigma_t = in_sigma_t;
        payload.medium_albedo = in_albedo;
        payload.medium_g = in_g;
        return;
    }

    bool const last_bounce = payload.last_bounce != 0u;
    payload.direct = pt_estimate_light(bsdf, frame, wo_local, p, N, last_bounce, rng)
                   + pt_estimate_environment(bsdf, frame, wo_local, p, N, last_bounce, rng);

    // The continuation, importance-sampled from the closure the material just described.
    float3 u = float3(pt_rand(rng), pt_rand(rng), pt_rand(rng));
    sv::bsdf_sample s = sv::bsdf_sample_direction(bsdf, wo_local, u);

    if (s.valid)
    {
        // The cosine is the one at the SURFACE, so a refracted direction contributes its own magnitude rather than a
        // negative weight — which direction it left on is the offset's business, not the estimator's.
        float3 weight = s.value * (abs(s.direction.z) / s.pdf);

        // Collapsing onto one wavelength means keeping one channel of three, so what survives is scaled back up by three —
        // the estimate stays unbiased and the other two channels are carried by other samples of the same pixel.
        if (collapsing)
        {
            float3 mask = float3(channel == 0u ? 1.0 : 0.0, channel == 1u ? 1.0 : 0.0, channel == 2u ? 1.0 : 0.0);
            weight *= mask * 3.0;
        }

        payload.throughput = weight;
        payload.direction = sv::to_world(frame, s.direction);
        payload.bsdf_pdf = s.pdf;
        payload.channel = channel;

        // A continuation that crossed the surface changes which medium it travels in: into the interior the closure says it
        // entered, or back out to vacuum when it left.
        // `medium_none` covers a reflection and a thin wall alike — the closure is what knows the difference.
        if (s.medium == sv::medium_none)
        {
            payload.medium_sigma_t = in_sigma_t;
            payload.medium_albedo = in_albedo;
            payload.medium_g = in_g;
        }
        else if (exiting)
        {
            payload.medium_sigma_t = float3(0, 0, 0);
            payload.medium_albedo = float3(0, 0, 0);
            payload.medium_g = 0.0;
        }
        else if (s.medium == sv::medium_subsurface)
        {
            payload.medium_sigma_t = bsdf.sss_sigma_t;
            payload.medium_albedo = bsdf.sss_albedo;
            payload.medium_g = bsdf.sss_g;
        }
        else
        {
            payload.medium_sigma_t = bsdf.medium_sigma_t;
            payload.medium_albedo = bsdf.medium_albedo;
            payload.medium_g = bsdf.medium_g;
        }
    }
    else
    {
        payload.throughput = float3(0, 0, 0);
        payload.direction = float3(0, 0, 0);
        payload.bsdf_pdf = 0.0;
        payload.medium_sigma_t = in_sigma_t;
        payload.medium_albedo = in_albedo;
        payload.medium_g = in_g;
    }

    payload.rng = rng;
}
