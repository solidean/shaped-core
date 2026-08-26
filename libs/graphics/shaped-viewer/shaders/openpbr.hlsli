#pragma once

// The OpenPBR Surface BSDF: the layered model `sv::surface` describes, prepared into lobes and then evaluated or sampled.
//
// The layer stack, top to bottom, is fuzz over coat over the base, and the base is the metal BSDF mixed against a dielectric
// specular layer over a diffuse substrate.
// Coupling between two adjacent layers is the Fresnel reflectance of the upper one: what it does not reflect is what reaches
// what lies below, and it attenuates again on the way out.
//
// Everything here works in a LOCAL frame whose +z is the shading normal, so no lobe sees a world direction.
// `wo` points back along the incoming ray and `wi` toward the next path vertex; both are unit and leave the surface.
//
// Where this deviates from the specification it says so at the lobe.
// The fuzz is a Conty-Estevez sheen rather than the specified Zeltner microflake, the coat tints what passes through it once
// rather than absorbing along the true refracted path length, and GGX energy compensation uses an analytic fit rather than a
// tabulated directional albedo.
// Transmission, subsurface, thin-film, dispersion and anisotropy are not modelled at all, and `sv::surface` does not carry them.

namespace sv
{
static const float pi = 3.14159265358979323846;

// A perfectly smooth lobe is a delta the estimator cannot sample, so this floor is load-bearing rather than cosmetic.
static const float min_alpha = 1e-3;

// ---------------------------------------------------------------------------------------------------------------------------
// Microfacet primitives (GGX / Trowbridge-Reitz, isotropic)

/// The normal distribution at half-vector `h`, which must lie in the upper hemisphere of the local frame.
float ggx_d(float3 h, float alpha)
{
    float a2 = alpha * alpha;
    float t = h.z * h.z * (a2 - 1.0) + 1.0;
    return a2 / max(pi * t * t, 1e-9);
}

/// Smith's lambda for GGX: the ratio of masked to visible area for a direction.
float ggx_lambda(float3 w, float alpha)
{
    float c2 = w.z * w.z;
    float s2 = max(0.0, 1.0 - c2);
    float t2 = s2 / max(c2, 1e-9);
    return 0.5 * (sqrt(1.0 + alpha * alpha * t2) - 1.0);
}

/// Height-correlated Smith masking-shadowing for the pair, which is the term `ggx_d` is meant to be paired with.
float ggx_g2(float3 wo, float3 wi, float alpha)
{
    return 1.0 / (1.0 + ggx_lambda(wo, alpha) + ggx_lambda(wi, alpha));
}

/// Masking for one direction alone, which is what the visible-normal pdf is normalized by.
float ggx_g1(float3 w, float alpha)
{
    return 1.0 / (1.0 + ggx_lambda(w, alpha));
}

/// A half-vector drawn from the distribution of VISIBLE normals (Heitz 2018), which is what keeps a sampled weight bounded.
/// `wo` must lie in the upper hemisphere.
float3 ggx_sample_vndf(float3 wo, float alpha, float2 u)
{
    // Stretch the view direction into the configuration the routine is derived in.
    float3 vh = normalize(float3(alpha * wo.x, alpha * wo.y, wo.z));

    // An orthonormal basis around vh, staying finite when vh is near +z.
    float len2 = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = len2 > 0.0 ? float3(-vh.y, vh.x, 0.0) * rsqrt(len2) : float3(1, 0, 0);
    float3 t2 = cross(vh, t1);

    // A point on the projected disc, its lower half compressed onto the visible part of the hemisphere.
    float r = sqrt(u.x);
    float phi = 2.0 * pi * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;

    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(1e-6, nh.z)));
}

/// The solid-angle pdf of a direction produced by reflecting about a `ggx_sample_vndf` half-vector.
float ggx_pdf(float3 wo, float3 wi, float alpha)
{
    if (wo.z <= 0.0 || wi.z <= 0.0)
        return 0.0;

    float3 h = normalize(wo + wi);
    float d_vis = ggx_g1(wo, alpha) * ggx_d(h, alpha) * max(0.0, dot(wo, h)) / max(wo.z, 1e-9);
    return d_vis / (4.0 * max(dot(wo, h), 1e-9));
}

// ---------------------------------------------------------------------------------------------------------------------------
// Fresnel

/// Schlick's approximation from a normal-incidence reflectance.
float3 fresnel_schlick(float mu, float3 f0)
{
    float t = pow(saturate(1.0 - mu), 5.0);
    return f0 + (float3(1, 1, 1) - f0) * t;
}

/// The exact unpolarized dielectric reflectance for relative IOR `ior`, total internal reflection included.
float fresnel_dielectric(float mu, float ior)
{
    float c = saturate(mu);
    float s2 = (1.0 - c * c) / (ior * ior);
    if (s2 >= 1.0)
        return 1.0; // past the critical angle nothing is transmitted

    float ct = sqrt(1.0 - s2);
    float rs = (c - ior * ct) / (c + ior * ct);
    float rp = (ior * c - ct) / (ior * c + ct);
    return 0.5 * (rs * rs + rp * rp);
}

/// The conductor Fresnel OpenPBR specifies: Schlick through `f0`, pulled down off-normal so the reflectance at 82 degrees is
/// `tint` times what Schlick alone would give there.
///
/// That dip is what separates a real metal from a Schlick one: gold and copper darken toward grazing rather than washing out
/// to white.
float3 fresnel_f82(float mu, float3 f0, float3 tint)
{
    const float mu_bar = 1.0 / 7.0; // cos(82 degrees), where the tint is defined

    float3 f_bar = fresnel_schlick(mu_bar, f0);
    float3 a = f_bar * (float3(1, 1, 1) - tint) / (mu_bar * pow(1.0 - mu_bar, 6.0));
    return max(float3(0, 0, 0), fresnel_schlick(mu, f0) - a * mu * pow(saturate(1.0 - mu), 6.0));
}

/// The directional albedo of a Schlick-Fresnel GGX lobe, as the split-sum scale and bias (Lazarov's analytic fit).
/// The lobe's albedo is `f0 * result.x + result.y`, which is what both the layer coupling and the energy compensation need.
float2 ggx_albedo_split(float mu, float alpha)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);

    float4 r = alpha * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * saturate(mu))) * r.x + r.y;
    return float2(a004 * -1.04 + r.z, a004 * 1.04 + r.w);
}

/// What a GGX lobe with normal-incidence reflectance `f0` reflects in total from direction `mu`.
float3 ggx_albedo(float mu, float alpha, float3 f0)
{
    float2 ab = ggx_albedo_split(mu, alpha);
    return f0 * ab.x + ab.y;
}

/// The multiple-scattering energy a single-scattering GGX lobe is missing, as a factor to multiply it by (Turquin 2019).
/// It is 1 for a smooth surface and grows with roughness, which is the loss it exists to put back.
float3 ggx_energy_compensation(float mu, float alpha, float3 f0)
{
    float2 ab = ggx_albedo_split(mu, alpha);
    float e_ss = ab.x + ab.y; // the white-furnace albedo of the lobe, which is what the loss is measured against
    return float3(1, 1, 1) + f0 * (1.0 / max(e_ss, 1e-3) - 1.0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Diffuse

/// Oren-Nayar's qualitative model, reducing to Lambert at roughness 0 — which is OpenPBR's `base_diffuse_roughness` default.
float oren_nayar(float3 wo, float3 wi, float roughness)
{
    if (roughness <= 0.0)
        return 1.0;

    float s2 = roughness * roughness;
    float a = 1.0 - 0.5 * s2 / (s2 + 0.33);
    float b = 0.45 * s2 / (s2 + 0.09);

    // The azimuthal term without trigonometry: the two tangential components, normalized against each other.
    float2 to = wo.xy;
    float2 ti = wi.xy;
    float lo = length(to);
    float li = length(ti);
    float cos_phi = (lo > 1e-6 && li > 1e-6) ? dot(to, ti) / (lo * li) : 0.0;

    // sin(alpha) * tan(beta) over the larger and the smaller of the two angles, read off the same tangential lengths.
    float sin_alpha = max(lo, li);
    float tan_beta = min(lo, li) / max(max(wo.z, wi.z), 1e-6);

    return a + b * max(0.0, cos_phi) * sin_alpha * tan_beta;
}

/// A cosine-weighted direction about +z, whose pdf is `wi.z / PI`.
float3 sample_cosine_local(float2 u)
{
    float r = sqrt(u.x);
    float phi = 2.0 * pi * u.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

// ---------------------------------------------------------------------------------------------------------------------------
// Fuzz (sheen)

/// The Conty-Estevez "Charlie" sheen distribution: an inverted lobe peaking at grazing incidence.
///
/// The specification names Zeltner's microflake sheen, which conserves energy by construction and needs its own albedo table.
/// This is the compact stand-in, and the deliberate deviation named at the top of this file.
float sheen_d(float3 h, float alpha)
{
    float inv_a = 1.0 / max(alpha, min_alpha);
    float c2 = saturate(h.z * h.z);
    float s2 = 1.0 - c2;
    return (2.0 + inv_a) * pow(max(s2, 1e-6), inv_a * 0.5) / (2.0 * pi);
}

/// Ashikhmin's visibility term, which is what the Charlie distribution is normally paired with.
float sheen_v(float3 wo, float3 wi)
{
    return 1.0 / max(4.0 * (wo.z + wi.z - wo.z * wi.z), 1e-6);
}

/// Roughly what the sheen lobe reflects in total, so the layer below can take what is left.
/// A coarse fit rather than a tabulated albedo: it vanishes at normal incidence and rises toward grazing, which is the shape
/// the coupling depends on.
float sheen_albedo(float mu, float alpha)
{
    return 0.5 * alpha * pow(saturate(1.0 - mu), 3.0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The prepared closure

/// One surface's lobes in the local frame, with everything independent of `wi` already folded in.
///
/// Preparing once per hit is what stops the three calls a path vertex makes — sample, evaluate for next-event estimation, and
/// pdf for the multiple-importance weight — from rebuilding the layer coupling three times over.
struct bsdf
{
    float3 diffuse_albedo;
    float diffuse_roughness;

    float3 metal_f0;
    float3 metal_tint;
    float metal_alpha;

    float3 spec_f0;
    float spec_alpha;
    float spec_weight;

    float coat_f0;
    float coat_alpha;
    float3 coat_tint;
    float coat_weight;
    float coat_darkening;

    float3 fuzz_color;
    float fuzz_alpha;
    float fuzz_weight;

    float metalness;

    float3 emission;
};

/// How likely each lobe is to be picked for one outgoing direction, in the order the evaluator sums them.
/// The five always add to 1: `bsdf_lobe_probs` normalizes, and falls back to the diffuse lobe when every weight is zero.
struct lobe_probs
{
    float fuzz;
    float coat;
    float metal;
    float spec;
    float diffuse;
};

/// What one sampled direction carries back: the direction itself, the BSDF value there, and the pdf that produced it.
/// `pdf` is the FULL closure pdf rather than the chosen lobe's, so a caller can weight it against a light sampler directly.
struct bsdf_sample
{
    float3 direction;
    float3 value; ///< the BSDF at (wo, direction), cosine NOT folded in
    float pdf;
    bool valid; ///< false when the sample left the upper hemisphere or the pdf collapsed
};

/// The perceptual-to-microfacet roughness mapping OpenPBR specifies, floored so no lobe becomes a delta.
float alpha_of(float roughness)
{
    float r = saturate(roughness);
    return max(min_alpha, r * r);
}

/// The scalar a lobe's selection probability is ranked by; the closure only needs relative magnitudes, never a color.
float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// The average Fresnel reflectance of a Schlick lobe over the hemisphere.
float fresnel_average(float f0)
{
    return (1.0 + 20.0 * f0) / 21.0;
}

/// One surface point in OpenPBR's parameter vocabulary, prepared into lobes by `bsdf_prepare`.
///
/// This is the subset the viewer models, and the names are the specification's.
/// Transmission, subsurface, thin-film and anisotropy are absent rather than defaulted, so a material cannot ask for one and
/// silently get something else.
///
/// `geometry_normal` is TANGENT space, so (0, 0, 1) is the shading normal the hit already has.
struct surface
{
    // base
    float base_weight;
    float3 base_color;
    float base_metalness;
    float base_diffuse_roughness;

    // specular
    float specular_weight;
    float3 specular_color;
    float specular_roughness;
    float specular_ior;

    // coat
    float coat_weight;
    float3 coat_color;
    float coat_roughness;
    float coat_ior;
    float coat_darkening;

    // fuzz
    float fuzz_weight;
    float3 fuzz_color;
    float fuzz_roughness;

    // emission
    float emission_luminance;
    float3 emission_color;

    // geometry
    float3 geometry_normal;
    float geometry_opacity;

    /// the surface's tangent frame as a unit quaternion, taking tangent space to OBJECT space
    ///
    /// Tangent space is +x tangent, +y bitangent, +z normal, so the identity rotation is object-space +z — which is a frame
    /// belonging to no particular surface.
    /// That is why the hit consults `SV_ATTR_SUPPLIED_tangent_frame` rather than comparing against the identity: an
    /// unsupplied frame must fall back to the geometric one, and an authored identity is a legitimate value.
    float4 geometry_tangent_frame;

    /// +1 for a right-handed frame, -1 where the uv parameterization is mirrored
    ///
    /// Its own value rather than the sign of the quaternion's w, because the packed encodings canonicalize that sign away —
    /// so a frame that hid handedness there could not be quantized without a content migration.
    float geometry_handedness;
};

/// The surface a material fragment starts from: OpenPBR's own defaults, so one that writes nothing is the specification's
/// default surface rather than an arbitrary grey.
surface default_surface()
{
    surface s;

    s.base_weight = 1.0;
    s.base_color = float3(0.8, 0.8, 0.8);
    s.base_metalness = 0.0;
    s.base_diffuse_roughness = 0.0;

    s.specular_weight = 1.0;
    s.specular_color = float3(1, 1, 1);
    s.specular_roughness = 0.3;
    s.specular_ior = 1.5;

    s.coat_weight = 0.0;
    s.coat_color = float3(1, 1, 1);
    s.coat_roughness = 0.0;
    s.coat_ior = 1.6;
    s.coat_darkening = 1.0;

    s.fuzz_weight = 0.0;
    s.fuzz_color = float3(1, 1, 1);
    s.fuzz_roughness = 0.5;

    s.emission_luminance = 0.0;
    s.emission_color = float3(1, 1, 1);

    s.geometry_normal = float3(0, 0, 1);
    s.geometry_opacity = 1.0;
    s.geometry_tangent_frame = float4(0, 0, 0, 1);
    s.geometry_handedness = 1.0;

    return s;
}

/// The lobes `s` describes, with every quantity that does not depend on the incoming direction folded in.
bsdf bsdf_prepare(surface s)
{
    bsdf b;

    b.metalness = saturate(s.base_metalness);

    b.diffuse_albedo = saturate(s.base_weight) * saturate(s.base_color);
    b.diffuse_roughness = saturate(s.base_diffuse_roughness);

    // The metal's normal-incidence reflectance is the base color itself, and the specular color is its tint at 82 degrees.
    b.metal_f0 = saturate(s.base_weight * s.base_color);
    b.metal_tint = saturate(s.specular_weight * s.specular_color);
    b.metal_alpha = alpha_of(s.specular_roughness);

    // The dielectric's reflectance comes from its IOR and `specular_color` tints it, while grazing incidence still goes to
    // white — which is what makes a tinted dielectric read as coated rather than metallic.
    //
    // `specular_weight` scales the whole LOBE rather than only `f0`.
    // Folding it into `f0` would leave Schlick's grazing tail at full strength, so a surface asking for no specular at all
    // still reflected white at the horizon — and the diffuse below it was still charged for the crossing.
    float ior = max(1.0 + 1e-3, s.specular_ior);
    float r0 = (ior - 1.0) / (ior + 1.0);
    b.spec_f0 = saturate(saturate(s.specular_color) * (r0 * r0));
    b.spec_weight = saturate(s.specular_weight);
    b.spec_alpha = alpha_of(s.specular_roughness);

    float cior = max(1.0 + 1e-3, s.coat_ior);
    float cr0 = (cior - 1.0) / (cior + 1.0);
    b.coat_f0 = cr0 * cr0;
    b.coat_alpha = alpha_of(s.coat_roughness);
    b.coat_tint = saturate(s.coat_color);
    b.coat_weight = saturate(s.coat_weight);
    b.coat_darkening = saturate(s.coat_darkening);

    b.fuzz_weight = saturate(s.fuzz_weight);
    b.fuzz_color = saturate(s.fuzz_color);
    b.fuzz_alpha = clamp(s.fuzz_roughness, min_alpha, 1.0);

    b.emission = max(float3(0, 0, 0), s.emission_luminance * s.emission_color);

    return b;
}

/// What the fuzz layer transmits to everything below it, in one direction.
float fuzz_transmission(bsdf b, float mu)
{
    return 1.0 - b.fuzz_weight * sheen_albedo(mu, b.fuzz_alpha);
}

/// What the coat transmits to the base, in one direction.
float coat_transmission(bsdf b, float mu)
{
    float2 ab = ggx_albedo_split(mu, b.coat_alpha);
    return 1.0 - b.coat_weight * (b.coat_f0 * ab.x + ab.y);
}

/// What the dielectric specular layer transmits to the diffuse substrate, in one direction.
float3 spec_transmission(bsdf b, float mu)
{
    return float3(1, 1, 1) - b.spec_weight * ggx_albedo(mu, b.spec_alpha, b.spec_f0);
}

/// How much the base is darkened by sitting under the coat, beyond the transmission the two crossings already cost.
///
/// Light that reaches the base and comes back up meets the coat's inner interface, where most of it is past the critical
/// angle and is reflected back DOWN for another chance to be absorbed.
/// The factor is that series: a bright base survives the extra bounces and loses little, a dark one loses nearly all of it —
/// which is why the effect is a shift in saturation rather than a uniform dimming.
///
/// `coat_darkening` 1 is the physical result and 0 compensates it away, so the factor runs from 1 toward the series rather
/// than the other way round.
/// It is bounded ABOVE by 1 in every channel, which is what keeps a coated surface from returning more light than it
/// received — the failure the earlier form had, where compensating the darkening away multiplied the base by the series
/// instead of dividing the escape out of it.
float3 coat_darkening_factor(bsdf b)
{
    if (b.coat_weight <= 0.0)
        return float3(1, 1, 1);

    // The average reflectance seen from INSIDE the coat, which is what drives the internal-reflection series.
    float f_avg = fresnel_average(b.coat_f0);
    float ior = (1.0 + sqrt(b.coat_f0)) / max(1e-3, 1.0 - sqrt(b.coat_f0));
    float f_avg_internal = saturate(1.0 - (1.0 - f_avg) / max(ior * ior, 1e-3));

    float3 base_albedo = saturate(lerp(b.diffuse_albedo, b.metal_f0, b.metalness));

    // What escapes per attempt over what the series recovers. Both carry the same internal reflectance, which is what makes
    // the ratio conserve energy: it is 1 for a white base and falls toward `1 - f_avg_internal` for a black one.
    float3 escaped = (1.0 - f_avg_internal) / max(float3(1e-3, 1e-3, 1e-3), float3(1, 1, 1) - f_avg_internal * base_albedo);

    // At `coat_darkening` 0 nothing is darkened; at 1 the series applies in full.
    // A coat that is only partly there darkens only that far, so the weight rides along.
    return lerp(float3(1, 1, 1), saturate(escaped), b.coat_darkening * b.coat_weight);
}

/// The full BSDF at (`wo`, `wi`), with the cosine NOT folded in.
/// Both directions must be unit and in the local frame; a direction below the surface evaluates to zero.
float3 bsdf_eval(bsdf b, float3 wo, float3 wi)
{
    if (wo.z <= 0.0 || wi.z <= 0.0)
        return float3(0, 0, 0);

    float mu_o = wo.z;
    float mu_i = wi.z;
    float3 h = normalize(wo + wi);
    float mu_h = max(dot(wo, h), 1e-6);
    float denom = 4.0 * mu_o * mu_i;

    // The metal and the dielectric specular share a half-vector and a roughness, so they share D and G2 as well.
    float d_spec = ggx_d(h, b.spec_alpha);
    float g_spec = ggx_g2(wo, wi, b.spec_alpha);

    float3 f_metal = fresnel_f82(mu_h, b.metal_f0, b.metal_tint) * (d_spec * g_spec / denom)
                   * ggx_energy_compensation(mu_o, b.metal_alpha, b.metal_f0);

    float3 f_spec = b.spec_weight * fresnel_schlick(mu_h, b.spec_f0) * (d_spec * g_spec / denom)
                  * ggx_energy_compensation(mu_o, b.spec_alpha, b.spec_f0);

    // The diffuse substrate takes what the specular layer let through, both on the way in and on the way out.
    float3 t_spec = spec_transmission(b, mu_o) * spec_transmission(b, mu_i);
    float3 f_diffuse = b.diffuse_albedo * (oren_nayar(wo, wi, b.diffuse_roughness) / pi) * t_spec;

    float3 f_base = lerp(f_spec + f_diffuse, f_metal, b.metalness);

    // The coat: its own reflection, and what it passes down to the base.
    float d_coat = ggx_d(h, b.coat_alpha);
    float g_coat = ggx_g2(wo, wi, b.coat_alpha);
    float3 f_coat = b.coat_weight * fresnel_dielectric(mu_h, (1.0 + sqrt(b.coat_f0)) / max(1e-3, 1.0 - sqrt(b.coat_f0)))
                  * (d_coat * g_coat / denom);

    float t_coat = coat_transmission(b, mu_o) * coat_transmission(b, mu_i);
    float3 coat_absorption = lerp(float3(1, 1, 1), b.coat_tint, b.coat_weight);
    float3 below_coat = f_base * t_coat * coat_absorption * coat_darkening_factor(b);

    // The fuzz sits above everything, and takes its share on both crossings.
    float3 f_fuzz = b.fuzz_weight * b.fuzz_color * sheen_d(h, b.fuzz_alpha) * sheen_v(wo, wi);
    float t_fuzz = fuzz_transmission(b, mu_o) * fuzz_transmission(b, mu_i);

    return f_fuzz + t_fuzz * (f_coat + below_coat);
}

/// How the five lobes split one outgoing direction's sampling budget.
/// Each weight is roughly what its lobe reflects, so a lobe that contributes nothing is never picked.
lobe_probs bsdf_lobe_probs(bsdf b, float3 wo)
{
    float mu = max(wo.z, 1e-4);

    lobe_probs p;
    p.fuzz = b.fuzz_weight * luminance(b.fuzz_color) * sheen_albedo(mu, b.fuzz_alpha);

    float t_fuzz = fuzz_transmission(b, mu);
    float2 coat_ab = ggx_albedo_split(mu, b.coat_alpha);
    p.coat = t_fuzz * b.coat_weight * (b.coat_f0 * coat_ab.x + coat_ab.y);

    float below = t_fuzz * coat_transmission(b, mu);
    p.metal = below * b.metalness * luminance(ggx_albedo(mu, b.metal_alpha, b.metal_f0));
    p.spec = below * (1.0 - b.metalness) * b.spec_weight * luminance(ggx_albedo(mu, b.spec_alpha, b.spec_f0));
    p.diffuse = below * (1.0 - b.metalness) * luminance(b.diffuse_albedo * spec_transmission(b, mu));

    float sum = p.fuzz + p.coat + p.metal + p.spec + p.diffuse;
    if (sum <= 1e-6)
    {
        // Nothing reflects anything worth sampling; the diffuse lobe keeps the pdf well-defined rather than zero.
        p.fuzz = 0.0;
        p.coat = 0.0;
        p.metal = 0.0;
        p.spec = 0.0;
        p.diffuse = 1.0;
        return p;
    }

    float inv = 1.0 / sum;
    p.fuzz *= inv;
    p.coat *= inv;
    p.metal *= inv;
    p.spec *= inv;
    p.diffuse *= inv;
    return p;
}

/// The solid-angle pdf `bsdf_sample` would produce for `wi`, which is what the multiple-importance weight against a light
/// sampler needs.
float bsdf_pdf(bsdf b, float3 wo, float3 wi)
{
    if (wo.z <= 0.0 || wi.z <= 0.0)
        return 0.0;

    lobe_probs p = bsdf_lobe_probs(b, wo);
    float cosine = wi.z / pi;

    return p.fuzz * cosine                                //
         + p.coat * ggx_pdf(wo, wi, b.coat_alpha)      //
         + p.metal * ggx_pdf(wo, wi, b.metal_alpha)    //
         + p.spec * ggx_pdf(wo, wi, b.spec_alpha)      //
         + p.diffuse * cosine;
}

/// One direction drawn from the closure, with the BSDF and the full pdf there.
///
/// `u` is three uniforms in [0, 1): the first picks the lobe and the other two the direction within it.
/// The lobe choice is a one-sample estimator over the five, so the returned pdf is the mixture's rather than the picked
/// lobe's — which is what keeps a lobe that another lobe could also have produced from being counted twice.
bsdf_sample bsdf_sample_direction(bsdf b, float3 wo, float3 u)
{
    bsdf_sample r;
    r.direction = float3(0, 0, 1);
    r.value = float3(0, 0, 0);
    r.pdf = 0.0;
    r.valid = false;

    if (wo.z <= 0.0)
        return r;

    lobe_probs p = bsdf_lobe_probs(b, wo);

    // Walk the five in the order the struct lists them, so the pick is a single pass over the cdf.
    float pick = u.x;
    float3 wi = float3(0, 0, 1);

    if (pick < p.fuzz)
    {
        wi = sample_cosine_local(u.yz);
    }
    else if (pick < p.fuzz + p.coat)
    {
        float3 h = ggx_sample_vndf(wo, b.coat_alpha, u.yz);
        wi = reflect(-wo, h);
    }
    else if (pick < p.fuzz + p.coat + p.metal)
    {
        float3 h = ggx_sample_vndf(wo, b.metal_alpha, u.yz);
        wi = reflect(-wo, h);
    }
    else if (pick < p.fuzz + p.coat + p.metal + p.spec)
    {
        float3 h = ggx_sample_vndf(wo, b.spec_alpha, u.yz);
        wi = reflect(-wo, h);
    }
    else
    {
        wi = sample_cosine_local(u.yz);
    }

    if (wi.z <= 1e-6)
        return r;

    r.direction = wi;
    r.value = bsdf_eval(b, wo, wi);
    r.pdf = bsdf_pdf(b, wo, wi);
    r.valid = r.pdf > 1e-9;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The shading frame

/// The local frame a lobe is evaluated in: three world-space axes whose `n` is the shading normal.
struct frame
{
    float3 t;
    float3 b;
    float3 n;
};

/// An orthonormal frame around `n`, with an arbitrary but stable tangent.
///
/// The tangent is arbitrary because nothing here is anisotropic and no mesh carries tangents yet.
/// A normal map is therefore applied in a frame that does not follow the uv layout, which is why `geometry_normal` is left at
/// its default until tangents exist — see the viewer TODO.
frame make_frame(float3 n)
{
    frame f;
    f.n = normalize(n);

    float3 up = abs(f.n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    f.t = normalize(cross(up, f.n));
    f.b = cross(f.n, f.t);
    return f;
}

/// A world direction in the frame's local coordinates.
float3 to_local(frame f, float3 w)
{
    return float3(dot(w, f.t), dot(w, f.b), dot(w, f.n));
}

/// A local direction back in world space.
float3 to_world(frame f, float3 w)
{
    return w.x * f.t + w.y * f.b + w.z * f.n;
}

/// `v` rotated by the unit quaternion `q`, in xyzw order.
float3 quat_rotate(float4 q, float3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

/// The frame a tangent-frame quaternion describes, with `handedness` mirroring the bitangent.
///
/// The quaternion carries the rotation alone; a mirrored uv island is a REFLECTION and no rotation expresses one, which is why
/// handedness has to arrive beside it rather than inside it.
frame frame_from_quaternion(float4 q, float handedness)
{
    frame f;
    f.t = quat_rotate(q, float3(1, 0, 0));
    f.b = quat_rotate(q, float3(0, 1, 0)) * (handedness < 0.0 ? -1.0 : 1.0);
    f.n = quat_rotate(q, float3(0, 0, 1));
    return f;
}

/// The same frame with its normal reversed, for a surface hit from behind.
///
/// Exactly one tangent flips with it. Reversing the normal alone would leave `t x b` pointing away from `n`, turning the frame
/// left-handed and mirroring every anisotropic and normal-mapped result on backfaces.
frame flip_frame(frame f)
{
    frame r;
    r.t = f.t;
    r.b = -f.b;
    r.n = -f.n;
    return r;
}

/// `f` reoriented so its normal is the tangent-space direction `n_tangent`, which is what a normal map supplies.
///
/// The tangent is carried across rather than rebuilt, so the frame keeps following the uv layout — which is the whole reason
/// for having a tangent frame instead of a bare normal.
frame perturb_frame(frame f, float3 n_tangent)
{
    float3 n = normalize(to_world(f, normalize(n_tangent)));

    // Gram-Schmidt the old tangent against the new normal; a tangent that ended up parallel to it leaves the frame alone.
    float3 t = f.t - n * dot(n, f.t);
    float len = length(t);
    if (len < 1e-6)
        return make_frame(n);

    frame r;
    r.n = n;
    r.t = t / len;
    r.b = cross(n, r.t) * (dot(cross(n, r.t), f.b) < 0.0 ? -1.0 : 1.0);
    return r;
}
} // namespace sv
