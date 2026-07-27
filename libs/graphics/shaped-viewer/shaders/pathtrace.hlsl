#include "pt_common.hlsli"
#include "background.hlsli" // background_radiance (the SH environment probe)

// The path-tracing raygen: a pinhole camera shoots `samples_per_pixel` jittered primary rays per pixel and
// integrates global illumination by bouncing them diffusely. Direct light at each hit is estimated with
// next-event estimation toward two sources — the rectangular ceiling light and the SH environment — one shadow
// ray each per bounce, so the image converges at far fewer samples than letting the bounce rays find the light.
//
// The environment is gathered by two strategies, combined with the balance-heuristic multiple-importance
// sampling: the NEE ray below (uniform-hemisphere sampling) and the diffuse bounce ray when it escapes. MIS
// keeps the sum unbiased while cutting the variance a non-uniform sky would otherwise add.

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> Output : register(u0);

// Direct lighting at surface point P with normal N: sample one point on the area light, test visibility with
// a shadow ray, and return the light's contribution *without* the surface albedo (the caller folds that in).
float3 estimate_direct(float3 P, float3 N, inout uint rng)
{
    // uniform sample on the oriented rectangle: center +/- along each world half-edge vector
    float s = pt_rand(rng) * 2.0 - 1.0;
    float t = pt_rand(rng) * 2.0 - 1.0;
    float3 lp = light_center + s * light_u + t * light_v;

    float3 to_light = lp - P;
    float dist2 = dot(to_light, to_light);
    float dist = sqrt(dist2);
    float3 wi = to_light / dist;

    float cos_surf = dot(N, wi);
    float cos_light = dot(light_normal, -wi);
    if (cos_surf <= 0.0 || cos_light <= 0.0)
        return float3(0, 0, 0); // surface or light face turned away

    // Shadow ray: occluded if anything sits between P and the light. Stop just short of the light surface, so
    // the light's own geometry does not count as an occluder. Skip the closest-hit shader — only visibility
    // matters — and route it to the shadow miss shader (miss index 1), which sets `visible` on a clear path.
    ShadowPayload sp;
    sp.visible = 0.0; // assume occluded; the shadow miss flips this to 1 when the ray reaches the light freely
    RayDesc sray;
    sray.Origin = P + N * 1e-3;
    sray.Direction = wi;
    sray.TMin = 1e-3;
    sray.TMax = dist - 2e-3;
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 0, 1, sray, sp);
    if (sp.visible < 0.5)
        return float3(0, 0, 0); // occluded

    // solid-angle pdf of uniform area sampling, times the Lambertian BRDF's 1/PI (albedo folded in by caller).
    // the rect's full edges are 2*light_u and 2*light_v, so its area is |cross(2u, 2v)| = 4 |cross(u, v)|.
    float area = 4.0 * length(cross(light_u, light_v));
    float pdf = dist2 / (area * cos_light);
    return light_emission * (cos_surf / (PT_PI * pdf));
}

// Environment next-event estimation at surface point P with normal N: one uniform-hemisphere sample toward the
// SH environment, visibility-tested to infinity, MIS-weighted (balance heuristic) against the diffuse bounce
// ray that samples the same environment. Returns the environment's contribution *without* the surface albedo
// (the caller folds that in). The SH probe has no sharp features, so a proportional-to-radiance sampler is not
// worth its cost — the uniform strategy plus MIS already cuts the variance of a bright, non-uniform sky.
float3 estimate_env(float3 P, float3 N, inout uint rng)
{
    // Uniform-hemisphere direction about N: cos(theta) = u1 uniform in [0, 1] gives a uniform solid-angle pick.
    float u1 = pt_rand(rng);
    float u2 = pt_rand(rng);
    float cos_surf = u1;
    float r = sqrt(max(0.0, 1.0 - u1 * u1));
    float phi = 2.0 * PT_PI * u2;

    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, N));
    float3 b = cross(N, t);
    float3 wi = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + N * cos_surf);

    // pdfs of the two strategies for this direction: uniform hemisphere here, cosine-weighted for the bounce.
    float p_env = 1.0 / (2.0 * PT_PI);
    float p_brdf = cos_surf / PT_PI;
    float w = p_env / (p_env + p_brdf); // balance heuristic

    // Visibility to infinity: occluded if any geometry sits along the ray. Reuse the shadow miss (index 1).
    ShadowPayload sp;
    sp.visible = 0.0;
    RayDesc sray;
    sray.Origin = P + N * 1e-3;
    sray.Direction = wi;
    sray.TMin = 1e-3;
    sray.TMax = 1e4;
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 0, 1, sray, sp);
    if (sp.visible < 0.5)
        return float3(0, 0, 0); // occluded

    // Lambertian estimator f*cos*L / p_env with f = albedo/PI (albedo folded in by the caller), times the MIS weight.
    return background_radiance(wi) * (cos_surf / (PT_PI * p_env)) * w;
}

[shader("raygeneration")]
void PathTraceRayGen()
{
    uint2 px = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint rng = pt_hash(px.x + px.y * dim.x + rng_seed * 9781u);

    int spp = max(1, samples_per_pixel);
    float3 accum = float3(0, 0, 0);

    for (int s = 0; s < spp; ++s)
    {
        // jittered pinhole primary ray
        float2 jitter = float2(pt_rand(rng), pt_rand(rng));
        float2 ndc = (float2(px) + jitter) / float2(dim) * 2.0 - 1.0; // [-1, 1], y down
        float3 origin = camera.position;
        float3 dir = normalize(camera.forward + camera.right_scaled * ndc.x - camera.up_scaled * ndc.y);

        float3 throughput = float3(1, 1, 1);
        float3 radiance = float3(0, 0, 0);
        float prev_cos = 0.0; // cos of the last bounce sample about its surface normal, for the escaped-env MIS

        for (int b = 0; b < max_bounces; ++b)
        {
            PtPayload p;
            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = dir;
            ray.TMin = 1e-3;
            ray.TMax = 1e4;
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);

            // Read every payload field into locals right away — before estimate_direct traces the shadow ray —
            // so the payload-access analyzer sees them consumed, then branch on the hit.
            float hit_t = p.hit_t;
            float3 albedo = p.albedo;
            float3 emissive = p.emissive;
            float3 N = p.normal;

            if (hit_t < 0.0)
            {
                // Escaped to the SH environment (PtMiss wrote its radiance back in `emissive`). The primary ray
                // sees the sky directly, at full weight; a bounce ray is the BRDF strategy of the env NEE below,
                // so weight it by the balance heuristic against the env sampler's pdf (uniform hemisphere).
                float w = (b == 0) ? 1.0 : (2.0 * prev_cos) / (2.0 * prev_cos + 1.0);
                radiance += throughput * emissive * w;
                break;
            }

            // Count a hit emitter directly only on the primary ray; deeper bounces already get emitters through
            // NEE at the previous hit, so adding emissive again would double-count.
            if (b == 0)
                radiance += throughput * emissive;

            float3 hit_p = origin + dir * hit_t;

            // Direct lighting by next-event estimation toward both sources: the area light and the SH environment.
            radiance += throughput * albedo * estimate_direct(hit_p, N, rng);
            radiance += throughput * albedo * estimate_env(hit_p, N, rng);

            // diffuse indirect bounce: cosine-weighted sampling makes (albedo/PI * cos) / pdf collapse to albedo
            float3 wi = pt_sample_cosine_hemisphere(N, pt_rand(rng), pt_rand(rng));
            prev_cos = max(dot(N, wi), 0.0); // pdf of this bounce = prev_cos / PI, for the env MIS on escape
            throughput *= albedo;
            origin = hit_p + N * 1e-3;
            dir = wi;
        }

        accum += radiance;
    }

    float3 color = accum / float(spp);

    // Progressive accumulation: blend this frame's estimate into the running mean already in Output. accum_frame
    // is 0 on the first frame after any change (camera move / resize), restarting the average; each subsequent
    // frame folds in one more estimate, so a static view converges instead of shimmering.
    if (accum_frame > 0)
    {
        float3 prev = Output[px].rgb;
        color = (prev * float(accum_frame) + color) / float(accum_frame + 1);
    }

    Output[px] = float4(color, 1.0);
}
