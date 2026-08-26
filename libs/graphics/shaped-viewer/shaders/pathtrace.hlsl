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

// This frame's primary-hit geometry: float4(normal.xyz, hit_t), with hit_t < 0 where the ray escaped to the
// environment. Overwritten every recorded frame rather than accumulated — it describes where *this* frame's pixels
// are, which is what a later frame needs to reproject its history onto them.
RWTexture2D<float4> GBuffer : register(u1);

// The previous recorded frame's Output and GBuffer. Separate textures rather than the ones above, because
// reprojection reads at a different pixel than it writes — in place that would be a race.
// HistoryColor's .a carries that pixel's own accumulated sample count, which is what makes the estimator per-pixel.
//
// t4 and up: the global root signature covers every stage, so these share a register space with the closest-hit's
// Instances (t1) — see pt_material_hit.hlsli.
Texture2D<float4> HistoryColor : register(t4);
Texture2D<float4> HistoryGBuffer : register(t5);

// Where `world` sat in `cam`'s image, as a pixel coordinate; `ok` is false behind the camera.
//
// Inverts the raygen's own ray construction rather than a projection matrix: `right_scaled`, `up_scaled` and
// `forward` are mutually orthogonal, so the ndc that produced a direction falls straight out of three dot products.
float2 reproject(Camera cam, float3 world, float2 dim, out bool ok)
{
    float3 v = world - cam.position;

    // The unnormalized ray to `world` is s * (forward + right_scaled*a - up_scaled*b); forward is unit, so s is
    // just the forward-depth, and s <= 0 means the point is behind the eye.
    float s = dot(v, cam.forward);
    ok = s > 1e-6;
    if (!ok)
        return float2(0, 0);

    float a = dot(v, cam.right_scaled) / (s * dot(cam.right_scaled, cam.right_scaled));
    float b = -dot(v, cam.up_scaled) / (s * dot(cam.up_scaled, cam.up_scaled));
    return (float2(a, b) * 0.5 + 0.5) * dim;
}

// Direct lighting at surface point P with normal N: sample one point on the area light, test visibility with
// a shadow ray, and return the light's contribution *without* the surface albedo (the caller folds that in).
float3 estimate_direct(float3 P, float3 N, inout uint rng)
{
    // uniform sample on the oriented rectangle: center +/- along each world half-edge vector
    float s = pt_rand(rng) * 2.0 - 1.0;
    float t = pt_rand(rng) * 2.0 - 1.0;
    float3 lp = light.center + s * light.u + t * light.v;

    float3 to_light = lp - P;
    float dist2 = dot(to_light, to_light);
    float dist = sqrt(dist2);
    float3 wi = to_light / dist;

    float cos_surf = dot(N, wi);
    float cos_light = dot(light.normal, -wi);
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
    // the rect's full edges are 2*light.u and 2*light.v, so its area is |cross(2u, 2v)| = 4 |cross(u, v)|.
    float area = 4.0 * length(cross(light.u, light.v));
    float pdf = dist2 / (area * cos_light);
    return light.emission * (cos_surf / (PT_PI * pdf));
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

    // This pixel's geometry, from a dedicated ray through the pixel *center*.
    //
    // It must not come from one of the jittered samples, however cheap that would be.
    // At any pixel straddling a geometric edge — every corner seam, every silhouette — the jitter lands on one
    // surface this frame and the other the next, so a sampled normal flips by the angle between them from frame to
    // frame. The disocclusion test then sees a different surface every single frame and rejects the history
    // forever, pinning exactly those pixels at zero carried samples: permanent noise along every edge.
    //
    // One extra primary ray — no bounces, no shadow rays — buys a G-buffer that is a function of the pixel alone,
    // which is what the test downstream assumes it is comparing.
    float3 gb_normal = float3(0, 0, 0);
    float gb_t = -1.0; // < 0 => escaped, so there is no surface here to reproject onto
    {
        float2 c_ndc = (float2(px) + 0.5) / float2(dim) * 2.0 - 1.0;

        PtPayload gp;
        RayDesc gray;
        gray.Origin = camera.position;
        gray.Direction = normalize(camera.forward + camera.right_scaled * c_ndc.x - camera.up_scaled * c_ndc.y);
        gray.TMin = 1e-3;
        gray.TMax = 1e4;
        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, gray, gp);

        // Read every field the payload declares, as the access analyzer expects of the caller; only two are kept.
        float3 unused = gp.albedo + gp.emissive;
        gb_normal = gp.normal + unused * 0.0;
        gb_t = gp.hit_t;
    }

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

    // Temporal reuse: find where this pixel's surface sat in the previous frame's image and carry that pixel's
    // running mean forward. The count rides in HistoryColor's alpha, so the estimator is per pixel rather than per
    // view — which is what lets a camera move keep its converged image instead of discarding all of it.
    //
    // `n` ends as the number of prior samples this pixel may keep; 0 means it starts over, and only this pixel does.
    float n = 0.0;
    float3 prev_color = float3(0, 0, 0);

    if (has_history != 0 && accum_frame > 0 && gb_t >= 0.0)
    {
        // Where this pixel's surface is: the same pixel-center ray the G-buffer above was traced with, so `world`
        // is exactly the point whose normal and depth were recorded, not an approximation of it.
        float2 ndc = (float2(px) + 0.5) / float2(dim) * 2.0 - 1.0;
        float3 dir = normalize(camera.forward + camera.right_scaled * ndc.x - camera.up_scaled * ndc.y);
        float3 world = camera.position + dir * gb_t;

        bool in_front = false;
        float2 prev_px = reproject(prev_camera, world, float2(dim), in_front);
        int2 prev_i = int2(floor(prev_px));

        // Nearest, never bilinear: filtering depth across a silhouette blends two surfaces into a position that
        // is on neither, and the disocclusion test below would then accept it.
        bool on_screen = in_front && all(prev_i >= int2(0, 0)) && all(prev_i < int2(dim));
        if (on_screen)
        {
            float4 prev_g = HistoryGBuffer[prev_i];
            float prev_t = prev_g.w;

            // Reconstruct what the previous frame actually had there, and demand it be the same surface.
            float2 prev_ndc = (float2(prev_i) + 0.5) / float2(dim) * 2.0 - 1.0;
            float3 prev_dir = normalize(prev_camera.forward + prev_camera.right_scaled * prev_ndc.x
                                        - prev_camera.up_scaled * prev_ndc.y);
            float3 prev_world = prev_camera.position + prev_dir * prev_t;

            float dist = dot(camera.forward, world - camera.position); // this pixel's own view depth, as the scale

            // Distance from the previous hit to *this pixel's surface plane*, not to its point.
            //
            // The difference decides whether a grazing surface can ever accumulate. Both positions carry a
            // tangential error — the jitter above, and a whole pixel of reprojection rounding — and at a grazing
            // angle that slide is many times the depth itself. A point-to-point test reads that as a disocclusion
            // and rejects the pixel every frame, which is exactly the permanent noise a corner shows.
            // Measuring across the normal ignores sliding along the surface and still catches a step in depth,
            // which is the only thing that actually invalidates the history.
            float plane_distance = abs(dot(prev_world - world, gb_normal));

            // A surface seen edge-on has a plane that nearly contains the view ray, so something far behind it can
            // still sit near that plane. This second test is what rules that out; it is deliberately loose, since
            // the plane test above is the discriminating one.
            float expected_t = length(world - prev_camera.position);
            bool depth_plausible = abs(prev_t - expected_t) <= 0.1 * max(expected_t, 1e-3);

            bool same_surface = prev_t >= 0.0 // it was a surface, not the environment
                             && plane_distance <= 0.01 * max(dist, 1e-3) && depth_plausible
                             && dot(gb_normal, prev_g.xyz) > 0.9; // and facing the same way

            if (same_surface)
            {
                float4 hist = HistoryColor[prev_i];
                prev_color = hist.rgb;

                // The hybrid: uncapped while the camera holds still, so a static view keeps the exact running mean
                // and converges; capped once it moves, so a sample dragged along by reprojection ages out.
                n = min(hist.a, float(history_max_frames));
            }
        }
    }

    color = (prev_color * n + color) / (n + 1.0);

    // Debug: show what each pixel actually kept, rather than what it renders.
    //
    // Red is "this pixel carried nothing" — the history was rejected, so it is showing a raw single-frame estimate
    // and will keep doing so every frame. Anything green is accumulating normally.
    // That separates the two causes of a noisy region, which look identical in the final image: a rejection bug
    // pins pixels at red forever, while a genuinely high-variance region still ramps to green.
    //
    // It overwrites the color the history stores, on purpose — the count in `.a` is what is being read, and it goes
    // on evolving untouched.
    if (debug_view == 1)
    {
        float g = saturate(n / 64.0);
        color = n < 0.5 ? float3(1, 0, 0) : float3(1.0 - g, g, 0.0);
    }

    Output[px] = float4(color, n + 1.0);
    GBuffer[px] = float4(gb_normal, gb_t);
}
