#include "pt_common.hlsli"

// The path-tracing raygen: a pinhole camera shoots `samples_per_pixel` jittered primary rays per pixel and
// integrates global illumination by following the continuation each hit hands back.
//
// The SHADING is not here. Each closest-hit evaluates its material's OpenPBR BSDF, estimates direct light through
// it toward both sources — the rectangular area light and the SH environment — and importance-samples the next
// direction from it; see pt_material_hit.hlsli. What is left for this file is the loop: accumulate what a hit
// reports, carry the throughput, and decide when a path ends.
//
// Both light sources are gathered by two strategies combined with balance-heuristic multiple importance sampling: the
// hit's own next-event ray, and the BSDF-sampled bounce ray when it reaches the same source.
// The weight for the second is applied here, because only the caller knows where the bounce went — escaping to the
// environment, or crossing the area light's rect, which is analytic and so is intersected rather than traced.

RaytracingAccelerationStructure scene : register(t0);

// The view's accumulator: the running mean of every sample this estimate has drawn, read back and blended into.
//
// Read-modify-write at the dispatch's OWN pixel, which is what lets one texture do the job of a ping-pong pair.
// `accum_frame` is the number of frames already folded in, so a frame's weight is 1 / (accum_frame + 1) — the
// estimate is per view rather than per pixel, and the CPU restarts it by sending 0.
RWTexture2D<float4> Output : register(u0);

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
        float prev_pdf = 0.0; // pdf of the direction the last hit sampled, for the escaped-environment MIS

        // What the next segment travels through: vacuum until the path refracts into a solid.
        // Tracked here rather than in the hit because what a medium does is a property of the DISTANCE travelled, and the
        // distance is not known until the segment ends.
        float3 medium_sigma_t = float3(0, 0, 0);
        float3 medium_albedo = float3(0, 0, 0);
        float medium_g = 0.0;

        // Which wavelength this path has been collapsed onto, or 3 while it still carries all three.
        uint channel = 3u;

        // Scattering events do not count against `max_bounces`.
        //
        // A random walk through skin or marble takes dozens of them before it comes back out, and a budget shared with the
        // surface bounces would make a dense medium simply black — the walk would run out before it ever escaped.
        // They get their own, much larger cap, which is a termination guard rather than a quality control.
        int scatters = 0;

        int b = 0;
        while (b < max_bounces)
        {
            PtPayload p;
            p.rng = rng;
            p.medium_sigma_t = medium_sigma_t;
            p.medium_albedo = medium_albedo;
            p.medium_g = medium_g;
            p.channel = channel;

            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = dir;
            ray.TMin = 1e-3;
            ray.TMax = 1e4;
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);

            // Read every payload field into locals right away, so the payload-access analyzer sees them consumed,
            // then branch on the hit. The random state comes back advanced by whatever the hit drew from it.
            rng = p.rng;
            float3 next_sigma_t = p.medium_sigma_t;
            float3 next_albedo = p.medium_albedo;
            float next_g = p.medium_g;
            uint next_channel = p.channel;
            float hit_t = p.hit_t;
            float3 direct = p.direct;
            float3 emission = p.emission;
            float3 weight = p.throughput;
            float3 next_dir = p.direction;
            float3 N = p.normal;
            float pdf = p.bsdf_pdf;

            // The BSDF strategy for the area light: the continuation this ray came from may have aimed at the rect.
            //
            // Only from b >= 1, because that is what pairs with a next-event estimate — the primary ray has none to
            // balance against, and weighting it here would make the light visible to the camera, which it is not.
            // The rect is analytic and absent from the TLAS, so `hit_t` is the whole occlusion test: geometry nearer
            // than the light blocks it, and nothing else can.
            bool const inside = any(medium_sigma_t > float3(0, 0, 0));
            bool const scattering = inside && any(medium_albedo > float3(0, 0, 0));

            if (scattering)
            {
                // A scattering interior is walked rather than crossed: the segment may end at a particle instead of at the
                // surface the trace found, and it usually does — that is what makes light spread sideways through skin.
                //
                // Distance is drawn against ONE channel's extinction and the estimate is corrected for all three, so a
                // medium whose channels differ (which is every interesting one) stays unbiased rather than needing three
                // separate walks.
                uint dc = min(uint(pt_rand(rng) * 3.0), 2u);
                float sigma_pick = max(medium_sigma_t[dc], 1e-6);
                float t = -log(max(1.0 - pt_rand(rng), 1e-9)) / sigma_pick;

                bool const reaches_surface = hit_t >= 0.0 && t >= hit_t;
                float const travelled = reaches_surface ? hit_t : t;

                float3 transmittance = exp(-medium_sigma_t * travelled);

                // The pdf of what actually happened, averaged over the channel the distance could have been drawn against.
                float3 pdf3 = reaches_surface ? transmittance : (medium_sigma_t * transmittance);
                float pdf = (pdf3.x + pdf3.y + pdf3.z) / 3.0;
                if (pdf <= 1e-12)
                    break;

                if (!reaches_surface)
                {
                    // A real scattering event: the path turns here and never reaches the surface the trace found.
                    // The phase function is its own pdf, so the turn itself carries no weight beyond the albedo.
                    throughput *= (transmittance * medium_sigma_t * medium_albedo) / pdf;

                    origin = origin + dir * t;
                    dir = pt_sample_hg(dir, medium_g, pt_rand(rng), pt_rand(rng));
                    prev_pdf = pt_hg_phase(1.0, medium_g); // the density at the direction actually drawn

                    ++scatters;
                    if (scatters > 256 || !pt_is_finite(throughput))
                        break;
                    continue; // NOT a bounce: the surface budget is untouched
                }

                throughput *= transmittance / pdf;
            }
            else if (inside && hit_t > 0.0)
            {
                // A purely absorbing interior, which needs no walk: Beer-Lambert over the whole segment.
                // Applied BEFORE anything this hit contributes, since the estimate and the emission both happen at the far
                // end of it and are attenuated by the whole crossing.
                throughput *= exp(-medium_sigma_t * hit_t);
            }

            if (b > 0)
            {
                float t_light = 0.0;
                float cos_light = 0.0;
                if (pt_light_intersect(origin, dir, t_light, cos_light) && (hit_t < 0.0 || t_light < hit_t))
                {
                    // `dir` is unit, so the distance along it is the distance to the rect.
                    float w = pt_mis_weight(prev_pdf, pt_light_pdf(t_light * t_light, cos_light));
                    radiance += throughput * light.emission * w;
                }
            }

            if (hit_t < 0.0)
            {
                // A ray that escapes while still inside a solid travelled an unbounded distance through it, so nothing
                // survives. It means the transmissive geometry is not closed, which is an authoring fact rather than a
                // case worth estimating.
                if (inside)
                    break;

                // Escaped to the SH environment (PtMiss wrote its radiance back in `emission`). The primary ray
                // sees the sky directly, at full weight; a bounce ray is the BSDF strategy of the hit's own
                // environment estimate, so weight it against that sampler's uniform-hemisphere pdf.
                float w = (b == 0) ? 1.0 : pt_mis_weight(prev_pdf, PT_ENV_PDF);
                radiance += throughput * emission * w;
                break;
            }

            // A surface's own emission reaches the camera directly and contributes nothing indirectly.
            //
            // Not a double-count guard: next-event estimation samples the analytic area light alone, so an emissive
            // MESH is never picked as a light and a deeper bounce has nothing to double-count against.
            // Emissive geometry lighting a scene needs light sampling over emissive triangles, which is a feature
            // this tracer does not have — see the viewer TODO.
            if (b == 0)
                radiance += throughput * emission;

            // What the hit already estimated toward the area light and the environment, through its own BSDF.
            radiance += throughput * direct;

            // A closure that sampled nothing — fully absorbed, or a lobe that collapsed — ends the path here.
            if (all(weight <= float3(0, 0, 0)))
                break;

            throughput *= weight;
            prev_pdf = pdf;

            // The offset follows the direction rather than the normal: a refracted continuation leaves on the far side, and
            // pushing it along +N would start it back inside the surface it just crossed.
            origin = origin + dir * hit_t + N * (dot(next_dir, N) < 0.0 ? -1e-3 : 1e-3);
            dir = next_dir;
            medium_sigma_t = next_sigma_t;
            medium_albedo = next_albedo;
            medium_g = next_g;
            channel = next_channel;
            ++b;
        }

        // One non-finite path would poison this pixel forever: the mean is blended in place, so a NaN carried into
        // the target keeps reproducing itself and no later frame can wash it out.
        // Dropping the path costs one sample out of `spp`; keeping it costs the pixel.
        if (pt_is_finite(radiance))
            accum += radiance;
    }

    float3 color = accum / float(spp);

    // Progressive accumulation: this frame's estimate folded into the running mean already in the target.
    //
    // Exact rather than exponential — every frame ever folded in carries the same weight, so a view left alone
    // converges to the ground truth instead of hovering around it.
    // Nothing ages out and nothing is carried across a camera move: the CPU restarts the estimate by sending
    // `accum_frame` 0, which is what makes "the image is either this scene from this eye, or it is nothing" hold.
    //
    // The read is at this dispatch's own pixel, so it needs no second texture to be race-free.
    // rgba32_float is what keeps this uncapped: at frame n the update is scaled by 1 / (n + 1), and a half float
    // would stop moving the mean somewhere around n = 2048.
    if (accum_frame > 0)
    {
        float n = float(accum_frame);
        color = (Output[px].rgb * n + color) / (n + 1.0);
    }

    Output[px] = float4(color, 1.0);
}
