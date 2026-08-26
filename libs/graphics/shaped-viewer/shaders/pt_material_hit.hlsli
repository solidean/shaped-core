#pragma once

#include "pt_common.hlsli"
#include "background.hlsli" // background_radiance (the SH environment probe)

// The path tracer's closest-hit, as the tail of a GENERATED material permutation.
//
// It is an epilogue rather than a shader of its own: it calls `sv_evaluate_material`, which the generated file defines just above
// it, so HLSL needs it emitted after that definition rather than included at the top.
// `sv::material_shader_options::epilogue_include` is what puts it there.
//
// One of these exists per permutation, and they differ only in the material they call.
// That is what a hit group per permutation buys: the shading is specialized, and everything around it — the geometry read, the
// two-sided normal, the estimators, the payload — is this one file.
//
// The SHADING happens here rather than in the raygen, because the material's BSDF is a layered closure of some twenty
// parameters and this is the only place it exists as a value.
// So the hit evaluates it, estimates direct light through it, samples the continuation from it, and hands the raygen a result
// it only has to accumulate.

/// The acceleration structure the shadow rays are traced against.
/// The global root signature covers every stage, so this is the same `t0` the raygen declares.
RaytracingAccelerationStructure scene : register(t0);

/// The per-item table, indexed by `InstanceID()` — mirrors `sv::instance_gpu`.
/// An ordinary binding rather than a bindless one: there is exactly one table, and what varies per instance is what it *points* at.
StructuredBuffer<sv::instance> Instances : register(t1, space0);

struct PtAttributes
{
    float2 bary;
};

/// One object-space position out of the instance's own vertex buffer.
/// Positions are `float3`, tightly packed, which is the layout `sv::mesh_manager` uploads.
float3 pt_instance_position(sv::instance inst, uint vertex)
{
    ByteAddressBuffer positions = gBindlessBuffers[NonUniformResourceIndex(inst.vertices)];
    return asfloat(positions.Load3(vertex * 12));
}

/// Whether anything blocks the segment from `origin` along `dir` for `dist`.
///
/// Callers offset the origin along the GEOMETRIC normal rather than the shading one: a normal map can tilt the shading normal
/// far enough that an offset along it starts the ray below the surface, which self-shadows the very pixels it was meant to
/// protect.
///
/// Skips the closest-hit — only visibility matters — and routes to the shadow miss (miss index 1), which is what sets
/// `visible` on a clear path.
bool pt_occluded(float3 origin, float3 dir, float dist)
{
    ShadowPayload sp;
    sp.visible = 0.0; // assume occluded; only the shadow miss flips this

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-3;
    ray.TMax = dist;
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 0, 1, ray, sp);

    return sp.visible < 0.5;
}

/// Direct light from the rectangular area light, with the BSDF folded in.
///
/// No multiple-importance weight is needed here: the light is analytic rather than geometry, so a BSDF-sampled ray can never
/// hit it and there is no second strategy to balance against.
float3 pt_estimate_area_light(sv::bsdf bsdf, sv::frame frame, float3 wo_local, float3 p, float3 n_geom, inout uint rng)
{
    // uniform sample on the oriented rectangle: center +/- along each world half-edge vector
    float s = pt_rand(rng) * 2.0 - 1.0;
    float t = pt_rand(rng) * 2.0 - 1.0;
    float3 lp = light.center + s * light.u + t * light.v;

    float3 to_light = lp - p;
    float dist2 = dot(to_light, to_light);
    float dist = sqrt(dist2);
    float3 wi = to_light / dist;

    float cos_light = dot(light.normal, -wi);
    if (cos_light <= 0.0)
        return float3(0, 0, 0); // the light's emitting face is turned away

    float3 wi_local = sv::to_local(frame, wi);
    if (wi_local.z <= 0.0)
        return float3(0, 0, 0); // below the surface, so the BSDF is zero there anyway

    float3 f = sv::bsdf_eval(bsdf, wo_local, wi_local);
    if (all(f <= float3(0, 0, 0)))
        return float3(0, 0, 0);

    // Stop just short of the light surface, so the light's own geometry does not count as an occluder.
    if (pt_occluded(p + n_geom * 1e-3, wi, dist - 2e-3))
        return float3(0, 0, 0);

    // Solid-angle pdf of uniform area sampling; the rect's full edges are 2*light.u and 2*light.v, so its area is
    // |cross(2u, 2v)| = 4 |cross(u, v)|.
    float area = 4.0 * length(cross(light.u, light.v));
    float pdf = dist2 / max(area * cos_light, 1e-9);

    return light.emission * f * (wi_local.z / pdf);
}

/// Direct light from the SH environment, with the BSDF folded in and weighted against the BSDF sampler.
///
/// One uniform-hemisphere sample: the probe has no sharp features, so a radiance-proportional sampler is not worth its cost,
/// and the multiple-importance weight already cuts the variance a bright, non-uniform sky would add.
float3 pt_estimate_environment(sv::bsdf bsdf, sv::frame frame, float3 wo_local, float3 p, float3 n_geom, inout uint rng)
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
    float w = pt_mis_weight(PT_ENV_PDF, sv::bsdf_pdf(bsdf, wo_local, wi_local));

    return background_radiance(wi) * f * (wi_local.z / PT_ENV_PDF) * w;
}

[shader("closesthit")]
void PtClosestHit(inout PtPayload payload, in PtAttributes attribs)
{
    sv::instance inst = Instances[InstanceID()];
    ByteAddressBuffer index_buffer = gBindlessBuffers[NonUniformResourceIndex(inst.indices)];
    sv::shading_context ctx = sv::make_context(inst, index_buffer, PrimitiveIndex(), attribs.bary);

    // Flat face normal from the triangle's own corners, moved into world space.
    // Read through the instance rather than a global vertex buffer, which is what lets one view hold many meshes.
    float3 v0 = pt_instance_position(inst, ctx.corner.x);
    float3 v1 = pt_instance_position(inst, ctx.corner.y);
    float3 v2 = pt_instance_position(inst, ctx.corner.z);
    float3 n_obj = normalize(cross(v1 - v0, v2 - v0));
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), n_obj));

    float3 V = -normalize(WorldRayDirection());
    if (dot(N, V) < 0.0)
        N = -N; // two-sided: face the incoming ray so arbitrary winding still shades

    payload.normal = N;
    payload.hit_t = RayTCurrent();

    // The random state round-trips through every path out of here, so a stream is never left half-advanced.
    uint rng = payload.rng;

    sv::surface surface = sv_evaluate_material(ctx);
    sv::bsdf bsdf = sv::bsdf_prepare(surface);

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

    // A normal map is a rotation of the frame, not a replacement for it: the tangent is carried across so the frame keeps
    // following the uv layout.
    if (any(surface.geometry_normal != float3(0, 0, 1)))
        frame = sv::perturb_frame(frame, surface.geometry_normal);

    float3 wo_local = sv::to_local(frame, V);
    float3 p = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    payload.emission = bsdf.emission;

    // A grazing hit whose shading frame turned away has no upper hemisphere to integrate over.
    if (wo_local.z <= 0.0)
    {
        payload.direct = float3(0, 0, 0);
        payload.throughput = float3(0, 0, 0);
        payload.direction = float3(0, 0, 0);
        payload.bsdf_pdf = 0.0;
        payload.rng = rng;
        return;
    }

    payload.direct = pt_estimate_area_light(bsdf, frame, wo_local, p, N, rng)
                   + pt_estimate_environment(bsdf, frame, wo_local, p, N, rng);

    // The continuation, importance-sampled from the closure the material just described.
    float3 u = float3(pt_rand(rng), pt_rand(rng), pt_rand(rng));
    sv::bsdf_sample s = sv::bsdf_sample_direction(bsdf, wo_local, u);

    if (s.valid)
    {
        payload.throughput = s.value * (s.direction.z / s.pdf);
        payload.direction = sv::to_world(frame, s.direction);
        payload.bsdf_pdf = s.pdf;
    }
    else
    {
        payload.throughput = float3(0, 0, 0);
        payload.direction = float3(0, 0, 0);
        payload.bsdf_pdf = 0.0;
    }

    payload.rng = rng;
}
