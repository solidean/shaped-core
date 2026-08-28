#include "openpbr.hlsli"

// A Monte-Carlo probe over the OpenPBR closure, so a test can assert on NUMBERS rather than on an image.
//
// The path tracer's own tests pin that the pipeline builds and the trace runs; none of them can say what the BSDF returned.
// This is the shader that closes that gap: the CPU hands over a list of surfaces and what to measure at each, and reads back
// one accumulator per work item.
//
// Every mode is an estimator whose EXPECTED value is known in closed form, which is what lets a tolerance be a real assertion
// rather than a threshold picked to make the run pass:
//
//   - `probe_albedo` integrates the closure against its own sampler, so the result is the directional albedo.
//     Energy conservation is `<= 1` in every channel, and a white furnace (a lobe that absorbs nothing) is `== 1`.
//   - `probe_pdf_norm` integrates `bsdf_pdf` over the hemisphere, which must come to 1 for a normalized density.
//     It is what catches a pdf that disagrees with the sampler it is supposed to describe — the failure that shows up as
//     bias no amount of convergence removes.
//   - `probe_reciprocity` compares `f(wo, wi)` against `f(wi, wo)` over direction pairs, which Helmholtz reciprocity
//     requires to be equal.
//
// Work is split so a case gets many more samples than one thread would finish: item `i` is case `i / blocks_per_case`,
// drawing its own independent block of samples. The CPU sums the blocks belonging to one case.

namespace sv
{
// Which estimator a case runs — mirrors sv_test::probe_mode.
static const uint probe_albedo = 0;
static const uint probe_pdf_norm = 1;
static const uint probe_reciprocity = 2;
static const uint probe_echo = 3;
static const uint probe_medium = 4;

/// One measurement to make — mirrors `sv_test::probe_case` lane-for-lane, so keep the two in lockstep.
///
/// `surface` sits last and at a 16-byte offset deliberately: it is the one member whose packing this file does not control,
/// so nothing else depends on where it ends.
/// `probe_echo` exists to pin that agreement, rather than leaving a layout mismatch to surface as a wrong number.
struct probe_case
{
    float3 wo;
    uint mode;

    uint samples; ///< samples this work item draws, before the CPU sums the blocks of one case
    uint seed;

    /// Whether the closure is prepared as if the ray were INSIDE the surface, which is where the index ratio inverts.
    /// Total internal reflection exists only on that side, so a case that leaves this at 0 cannot measure it at all.
    uint exiting;
    uint pad1;

    surface s;

    float pad2;
    float pad3;
    float pad4;
};

StructuredBuffer<probe_case> Cases : register(t0);

/// One accumulator per work item, summed across a case's blocks by the CPU. What the lanes mean is per mode:
///
///   - `probe_albedo`: `xyz` is the summed estimate and `w` the samples that produced it.
///   - `probe_pdf_norm`: `x` is the summed estimate, `w` the sample count.
///   - `probe_reciprocity`: `x` is the summed absolute difference and `y` the summed magnitude it is relative to.
///   - `probe_medium`: how many samples reported an interior disagreeing with the side they went to, how many entered the
///     transmissive interior, and how many the subsurface one.
///   - `probe_echo`: the decoded fields the CPU checks its own packing against.
RWStructuredBuffer<float4> Results : register(u0);

/// A hashed per-lane RNG (PCG-style), seeded per work item so every block draws an independent stream.
float probe_rand(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    return float((w >> 22) ^ w) * (1.0 / 4294967296.0);
}

/// A uniform direction in the upper hemisphere, whose solid-angle pdf is the constant `1 / (2 pi)`.
float3 probe_uniform_hemisphere(float2 u)
{
    float z = u.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * pi * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

/// A cosine-weighted direction on EITHER side of the surface, whose density is `0.5 * |wi.z| / pi` over the whole sphere.
/// The closure transmits, so a proposal covering only the upper hemisphere would leave the refracted lobe unmeasured.
float3 probe_sample_two_sided_cosine(float3 u)
{
    float3 wi = sample_cosine_local(u.yz);
    return u.x < 0.5 ? wi : float3(wi.x, wi.y, -wi.z);
}

/// The estimator `probe_pdf_norm` runs: is `bsdf_pdf` the density that `bsdf_sample_direction` actually draws from?
///
/// That is the property multiple importance sampling depends on, and it is strictly stronger than "the pdf integrates to 1" —
/// a pdf can integrate to 1 and still describe a different distribution than the sampler produces.
/// It is also the only form that survives a near-specular lobe. Integrating a pdf directly means landing on a spike that a
/// broad proposal almost never hits and a sharp one resolves to the limits of float precision, so the estimate swings by
/// hundreds of percent at any budget — noise indistinguishable from the bias the mode exists to detect.
///
/// Directions come half from a cosine lobe and half from the closure's own sampler, so the true sampling density is
/// `0.5 * cosine + 0.5 * (whatever the sampler really draws)`.
/// Weighting by `0.5 * cosine + 0.5 * bsdf_pdf` gives a mean of exactly 1 when those two agree, and the ratio is bounded by 2
/// either way — which is what keeps a smooth lobe measurable at all.
float probe_consistency_weight(float3 wi, float claimed_pdf)
{
    float cosine = 0.5 * abs(wi.z) / pi;
    return claimed_pdf / max(0.5 * cosine + 0.5 * claimed_pdf, 1e-9);
}

/// The estimate one work item contributes, which the CPU sums with its siblings.
float4 probe_run(probe_case c, uint item)
{
    bsdf b = bsdf_prepare(c.s, c.exiting != 0u, 3u); // carrying all three wavelengths
    uint rng = item * 9781u + c.seed * 26699u + 1u;

    if (c.mode == probe_echo)
        return float4(c.s.base_color.x, c.s.specular_roughness, c.s.geometry_tangent_frame.w, float(c.samples));

    float3 wo = normalize(c.wo);
    float3 sum = float3(0, 0, 0);

    for (uint i = 0u; i < c.samples; ++i)
    {
        if (c.mode == probe_albedo)
        {
            // The closure's own sampler, so the estimator is `f * cos / pdf` and its mean is the directional albedo.
            // An invalid sample contributes zero rather than being discarded: dropping it would renormalize the estimate
            // over the samples that survived, which is exactly the bias this mode exists to detect.
            float3 u = float3(probe_rand(rng), probe_rand(rng), probe_rand(rng));
            bsdf_sample s = bsdf_sample_direction(b, wo, u);
            if (s.valid)
                sum += s.value * abs(s.direction.z) / s.pdf; // `abs`: a refracted direction leaves on the far side
        }
        else if (c.mode == probe_pdf_norm)
        {
            // Half the draws come from a cosine lobe and half from the closure itself, and both are weighted by what
            // `bsdf_pdf` claims about them — so the mean is 1 exactly when the claim matches the sampler.
            float pick = probe_rand(rng);
            float2 u = float2(probe_rand(rng), probe_rand(rng));

            if (pick < 0.5)
            {
                float3 wi = probe_sample_two_sided_cosine(float3(probe_rand(rng), u));
                if (abs(wi.z) > 1e-6)
                    sum.x += probe_consistency_weight(wi, bsdf_pdf(b, wo, wi));
            }
            else
            {
                bsdf_sample s = bsdf_sample_direction(b, wo, float3(probe_rand(rng), u));
                if (s.valid)
                    sum.x += probe_consistency_weight(s.direction, s.pdf);
            }
        }
        else if (c.mode == probe_medium)
        {
            // Which interior each drawn direction entered, and whether that agreed with the side the direction went to.
            //
            // `x` is the DISAGREEMENT count rather than a count of `medium_none`, because the latter is implied by the
            // other two and nothing was asserting it.
            // The disagreement is exact rather than statistical: a sample reporting no interior while pointing through the
            // surface came from a reflective lobe that produced a direction below the horizon, and would be valued as a
            // BTDF and scored against a refraction density that did not produce it.
            // A thin wall encloses nothing, so it legitimately transmits into no interior and is the one exclusion.
            float3 u = float3(probe_rand(rng), probe_rand(rng), probe_rand(rng));
            bsdf_sample s = bsdf_sample_direction(b, wo, u);
            if (s.valid)
            {
                bool const crossed = s.direction.z < 0.0;
                bool const entered = s.medium != medium_none;
                bool const thin = c.s.geometry_thin_walled != 0.0;

                sum.x += (crossed != entered && !(crossed && thin)) ? 1.0 : 0.0;
                sum.y += s.medium == medium_transmission ? 1.0 : 0.0;
                sum.z += s.medium == medium_subsurface ? 1.0 : 0.0;
            }
        }
        else // probe_reciprocity
        {
            // Both directions drawn rather than one held fixed: a reciprocity break that only shows at grazing incidence
            // would hide behind a `wo` the case pinned.
            // Both directions stay in the UPPER hemisphere, so only the reflective lobes are compared.
            // A BTDF is deliberately not reciprocal — it carries the ratio of the two indices squared, which is a property
            // of radiance rather than an error — so a transmitted pair has nothing to assert here.
            float3 wa = probe_uniform_hemisphere(float2(probe_rand(rng), probe_rand(rng)));
            float3 wb = probe_uniform_hemisphere(float2(probe_rand(rng), probe_rand(rng)));

            float3 fab = bsdf_eval(b, wa, wb);
            float3 fba = bsdf_eval(b, wb, wa);

            sum.x += abs(fab.x - fba.x) + abs(fab.y - fba.y) + abs(fab.z - fba.z);
            sum.y += abs(fab.x) + abs(fab.y) + abs(fab.z) + abs(fba.x) + abs(fba.y) + abs(fba.z);
        }
    }

    return float4(sum, float(c.samples));
}
} // namespace sv

[numthreads(64, 1, 1)] void BsdfProbe(uint3 tid : SV_DispatchThreadID)
{
    uint item = tid.x;

    uint count = 0u;
    uint stride = 0u;
    sv::Results.GetDimensions(count, stride);
    if (item >= count)
        return;

    // One case per block of work items, so a case's sample budget is not capped by what one thread can finish.
    uint case_count = 0u;
    uint case_stride = 0u;
    sv::Cases.GetDimensions(case_count, case_stride);
    uint blocks_per_case = count / max(case_count, 1u);

    sv::probe_case c = sv::Cases[min(item / max(blocks_per_case, 1u), case_count - 1u)];
    sv::Results[item] = sv::probe_run(c, item);
}
