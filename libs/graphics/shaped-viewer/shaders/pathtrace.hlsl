#include "pt_common.hlsli"

// The path-tracing raygen: a pinhole camera shoots `samples_per_pixel` jittered primary rays per pixel and
// integrates global illumination by following the continuation each hit hands back.
//
// The SHADING is not here. Each closest-hit evaluates its material's OpenPBR BSDF, estimates direct light through
// it toward both sources — the rectangular area light and the SH environment — and importance-samples the next
// direction from it; see pt_material_hit.hlsli. What is left for this file is the loop: accumulate what a hit
// reports, carry the throughput, and decide when a path ends.
//
// The environment is gathered by two strategies combined with balance-heuristic multiple importance sampling: the
// hit's own next-event ray (uniform-hemisphere) and the BSDF-sampled bounce ray when it escapes. The weight for
// the second is applied here, because only the caller knows the bounce escaped.

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
        gp.shade = 0; // geometry only: no material, no shadow rays, no continuation
        gp.rng = rng;

        RayDesc gray;
        gray.Origin = camera.position;
        gray.Direction = normalize(camera.forward + camera.right_scaled * c_ndc.x - camera.up_scaled * c_ndc.y);
        gray.TMin = 1e-3;
        gray.TMax = 1e4;
        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, gray, gp);

        // Read every field the payload declares, as the access analyzer expects of the caller; only two are kept.
        float3 unused = gp.direct + gp.emission + gp.throughput + gp.direction;
        float unused_scalar = gp.bsdf_pdf + float(gp.rng) * 0.0;
        gb_normal = gp.normal + unused * 0.0 + unused_scalar * 0.0;
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
        float prev_pdf = 0.0; // pdf of the direction the last hit sampled, for the escaped-environment MIS

        for (int b = 0; b < max_bounces; ++b)
        {
            PtPayload p;
            p.shade = 1;
            p.rng = rng;

            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = dir;
            ray.TMin = 1e-3;
            ray.TMax = 1e4;
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);

            // Read every payload field into locals right away, so the payload-access analyzer sees them consumed,
            // then branch on the hit. The random state comes back advanced by whatever the hit drew from it.
            rng = p.rng;
            float hit_t = p.hit_t;
            float3 direct = p.direct;
            float3 emission = p.emission;
            float3 weight = p.throughput;
            float3 next_dir = p.direction;
            float3 N = p.normal;
            float pdf = p.bsdf_pdf;

            if (hit_t < 0.0)
            {
                // Escaped to the SH environment (PtMiss wrote its radiance back in `emission`). The primary ray
                // sees the sky directly, at full weight; a bounce ray is the BSDF strategy of the hit's own
                // environment estimate, so weight it against that sampler's uniform-hemisphere pdf.
                float w = (b == 0) ? 1.0 : pt_mis_weight(prev_pdf, PT_ENV_PDF);
                radiance += throughput * emission * w;
                break;
            }

            // Count a hit emitter directly only on the primary ray; deeper bounces already get emitters through
            // NEE at the previous hit, so adding emission again would double-count.
            if (b == 0)
                radiance += throughput * emission;

            // What the hit already estimated toward the area light and the environment, through its own BSDF.
            radiance += throughput * direct;

            // A closure that sampled nothing — fully absorbed, or a lobe that collapsed — ends the path here.
            if (all(weight <= float3(0, 0, 0)))
                break;

            throughput *= weight;
            prev_pdf = pdf;
            origin = origin + dir * hit_t + N * 1e-3;
            dir = next_dir;
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
