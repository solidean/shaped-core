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
// The transmissive and subsurface bases share the specular interface and differ only in what lies beyond it, and a
// dispersive crossing collapses the path onto one wavelength rather than bending three at once.

namespace sv
{
static const float pi = 3.14159265358979323846;

// A perfectly smooth lobe is a delta the estimator cannot sample, so this floor is load-bearing rather than cosmetic.
static const float min_alpha = 1e-3;

// ---------------------------------------------------------------------------------------------------------------------------
// Microfacet primitives (GGX / Trowbridge-Reitz, anisotropic)
//
// Every one of these takes `alpha` as a float2: the roughness along the frame's tangent and along its bitangent.
// An isotropic lobe is the special case where the two are equal, so nothing needs a second code path — and the local frame's
// tangent is what the two axes are measured against, which is why an anisotropic surface needs a frame that follows its uv
// layout rather than the arbitrary one `make_frame` invents.

/// The normal distribution at half-vector `h`, which must lie in the upper hemisphere of the local frame.
float ggx_d(float3 h, float2 alpha)
{
    float3 v = float3(h.x / alpha.x, h.y / alpha.y, h.z);
    float t = dot(v, v);
    return 1.0 / max(pi * alpha.x * alpha.y * t * t, 1e-9);
}

/// Smith's lambda for GGX: the ratio of masked to visible area for a direction.
float ggx_lambda(float3 w, float2 alpha)
{
    float c2 = w.z * w.z;
    float t2 = (alpha.x * alpha.x * w.x * w.x + alpha.y * alpha.y * w.y * w.y) / max(c2, 1e-9);
    return 0.5 * (sqrt(1.0 + t2) - 1.0);
}

/// Height-correlated Smith masking-shadowing for the pair, which is the term `ggx_d` is meant to be paired with.
float ggx_g2(float3 wo, float3 wi, float2 alpha)
{
    return 1.0 / (1.0 + ggx_lambda(wo, alpha) + ggx_lambda(wi, alpha));
}

/// Masking for one direction alone, which is what the visible-normal pdf is normalized by.
float ggx_g1(float3 w, float2 alpha)
{
    return 1.0 / (1.0 + ggx_lambda(w, alpha));
}

/// A half-vector drawn from the distribution of VISIBLE normals (Heitz 2018), which is what keeps a sampled weight bounded.
/// `wo` must lie in the upper hemisphere.
float3 ggx_sample_vndf(float3 wo, float2 alpha, float2 u)
{
    // Stretch the view direction into the configuration the routine is derived in.
    float3 vh = normalize(float3(alpha.x * wo.x, alpha.y * wo.y, wo.z));

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
    return normalize(float3(alpha.x * nh.x, alpha.y * nh.y, max(1e-6, nh.z)));
}

/// The solid-angle pdf of a direction produced by reflecting about a `ggx_sample_vndf` half-vector.
float ggx_pdf(float3 wo, float3 wi, float2 alpha)
{
    if (wo.z <= 0.0 || wi.z <= 0.0)
        return 0.0;

    float3 h = normalize(wo + wi);
    float d_vis = ggx_g1(wo, alpha) * ggx_d(h, alpha) * max(0.0, dot(wo, h)) / max(wo.z, 1e-9);
    return d_vis / (4.0 * max(dot(wo, h), 1e-9));
}

/// The half-vector a REFRACTION through relative index `eta` happened about, oriented into the upper hemisphere.
///
/// `wo` leaves the surface on the +z side and `wi` on the -z side, and `eta` is the index on `wi`'s side over the index on
/// `wo`'s — so it inverts when the path crosses back out.
/// Each direction is weighted by the index of the side it is ON (Walter 2007), which is what makes the microfacet normal
/// the one Snell's law would have bent about.
float3 refraction_half_vector(float3 wo, float3 wi, float eta)
{
    float3 h = -normalize(wo + eta * wi);
    return h.z < 0.0 ? -h : h;
}

/// The Jacobian taking a half-vector density to a refracted-direction density, which is what turns the visible-normal
/// distribution into a pdf over `wi`.
float refraction_jacobian(float3 wo, float3 wi, float3 h, float eta)
{
    float dot_o = dot(wo, h);
    float dot_i = dot(wi, h);
    float denom = dot_o + eta * dot_i;
    return eta * eta * abs(dot_i) / max(denom * denom, 1e-9);
}

/// The solid-angle pdf of a direction produced by refracting about a `ggx_sample_vndf` half-vector.
float ggx_refraction_pdf(float3 wo, float3 wi, float2 alpha, float eta)
{
    if (wo.z <= 0.0 || wi.z >= 0.0)
        return 0.0;

    float3 h = refraction_half_vector(wo, wi, eta);
    float dot_o = dot(wo, h);
    if (dot_o <= 0.0)
        return 0.0;

    float d_vis = ggx_g1(wo, alpha) * ggx_d(h, alpha) * dot_o / max(wo.z, 1e-9);
    return d_vis * refraction_jacobian(wo, wi, h, eta);
}

/// The isotropic roughness an anisotropic lobe reflects about as much as, for the fits that have no anisotropic form.
///
/// The directional-albedo fit below is isotropic and there is no anisotropic version of it, so the layer coupling and the
/// energy compensation both reduce through here.
/// The geometric mean is the reduction that keeps `alpha.x * alpha.y` — the lobe's solid angle — rather than its width along
/// either axis, so a stretched highlight couples like the round one covering the same area.
float alpha_iso(float2 alpha)
{
    return sqrt(max(alpha.x * alpha.y, 1e-12));
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
float3 ggx_albedo(float mu, float2 alpha, float3 f0)
{
    float2 ab = ggx_albedo_split(mu, alpha_iso(alpha));
    return f0 * ab.x + ab.y;
}

/// The multiple-scattering energy a single-scattering GGX lobe is missing, as a factor to multiply it by (Turquin 2019).
/// It is 1 for a smooth surface and grows with roughness, which is the loss it exists to put back.
float3 ggx_energy_compensation(float mu, float2 alpha, float3 f0)
{
    float2 ab = ggx_albedo_split(mu, alpha_iso(alpha));
    float e_ss = ab.x + ab.y; // the white-furnace albedo of the lobe, which is what the loss is measured against
    return float3(1, 1, 1) + f0 * (1.0 / max(e_ss, 1e-3) - 1.0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Thin film
//
// A film thin enough that the light reflected off its top and off its bottom still interfere, which is what makes a soap
// bubble or an oxide layer colored: the two paths differ by an optical distance, so each wavelength is reinforced or
// cancelled by a different amount.
//
// This evaluates that interference at THREE wavelengths, one per output channel, rather than integrating it over the
// spectrum.
// Belcour and Barla's spectral formulation is the full answer and the deviation here: with three samples a thin film shows
// its first interference order faithfully and its higher orders alias into colors the spectrum would have averaged away, so
// a film past roughly a micron drifts from what it should be.

/// The wavelengths in nanometres a channel stands for, shared by the thin film and by dispersion.
/// Three samples is what an RGB pipeline has to say about a spectrum, and both features are limited by it the same way.
static const float3 channel_wavelengths = float3(620.0, 550.0, 450.0);

/// The wavelengths in nanometres the film is evaluated at, one per channel.
static const float3 thin_film_wavelengths = channel_wavelengths;

/// The index of refraction at one wavelength, from the index at the sodium D line and an Abbe number.
///
/// Cauchy's two-term relation, with the B coefficient solved so the spread between the F and C lines is exactly what the
/// Abbe number states — which is what makes `transmission_dispersion_abbe_number` mean the thing opticians print on a glass.
/// `scale` multiplies that spread, so 0 collapses every wavelength back onto the base index and 2 doubles the fan.
///
/// Smaller Abbe numbers disperse MORE, which is the opposite of how the parameter reads.
float dispersive_ior(float base_ior, float abbe, float scale, float wavelength)
{
    if (scale <= 0.0 || abbe <= 0.0)
        return base_ior;

    const float lambda_d = 587.6; // the sodium D line, where `base_ior` is defined
    const float lambda_f = 486.1;
    const float lambda_c = 656.3;

    float inv_f2 = 1.0 / (lambda_f * lambda_f);
    float inv_c2 = 1.0 / (lambda_c * lambda_c);

    float b = scale * (base_ior - 1.0) / (abbe * (inv_f2 - inv_c2));
    float a = base_ior - b / (lambda_d * lambda_d);

    return max(1.0 + 1e-3, a + b / max(wavelength * wavelength, 1e-6));
}

/// The SIGNED amplitude reflectances at one interface, s and p polarization, for a real relative index `eta`.
///
/// The sign is what carries the half-wave phase shift, and the sign is the whole effect: it decides whether the two paths add
/// or cancel at a given thickness, which is where the color comes from.
/// A magnitude-only Fresnel would produce a film that brightens and dims without ever changing hue.
void fresnel_amplitude(float cos_i, float3 eta, out float3 r_s, out float3 r_p)
{
    float3 s2 = (1.0 - cos_i * cos_i) / max(eta * eta, float3(1e-6, 1e-6, 1e-6));
    float3 cos_t = sqrt(max(float3(0, 0, 0), 1.0 - s2));

    r_s = (cos_i - eta * cos_t) / max(abs(cos_i + eta * cos_t), float3(1e-6, 1e-6, 1e-6));
    r_p = (eta * cos_i - cos_t) / max(abs(eta * cos_i + cos_t), float3(1e-6, 1e-6, 1e-6));

    // Past the critical angle everything is reflected, and the phase this drops is what the three-wavelength sampling
    // cannot represent anyway.
    r_s = select(s2 >= 1.0, float3(1, 1, 1), r_s);
    r_p = select(s2 >= 1.0, float3(1, 1, 1), r_p);
}

/// The Airy summation over the film's internal reflections, for one polarization.
float3 airy_reflectance(float3 r1, float3 r2, float3 phase)
{
    float3 c = cos(phase);
    float3 num = r1 * r1 + r2 * r2 + 2.0 * r1 * r2 * c;
    float3 den = 1.0 + r1 * r1 * r2 * r2 + 2.0 * r1 * r2 * c;
    return saturate(num / max(den, float3(1e-6, 1e-6, 1e-6)));
}

/// What a film of `thickness` nanometres and index `film_ior` reflects, sitting on a base whose own reflectance is `f0`.
///
/// The base enters as an equivalent REAL index rather than a complex one, so a metal's absorption does not shift the phase it
/// reflects with.
/// That costs the slight hue rotation a real conductor's substrate adds and keeps the model to one closed form for metal and
/// dielectric alike.
float3 thin_film_reflectance(float mu, float3 f0, float film_ior, float thickness)
{
    float eta_film = max(1.0 + 1e-3, film_ior);

    // The angle inside the film, which sets both the second interface's incidence and the optical path length.
    float s2 = (1.0 - mu * mu) / (eta_film * eta_film);
    if (s2 >= 1.0)
        return float3(1, 1, 1);
    float cos_film = sqrt(1.0 - s2);

    float3 r01_s = float3(0, 0, 0);
    float3 r01_p = float3(0, 0, 0);
    fresnel_amplitude(mu, float3(eta_film, eta_film, eta_film), r01_s, r01_p);

    // The base as the index that would reflect `f0` at normal incidence, relative to the film it sits under.
    float3 root = clamp(sqrt(saturate(f0)), 0.0, 0.99);
    float3 eta_base = ((1.0 + root) / (1.0 - root)) / eta_film;

    float3 r12_s = float3(0, 0, 0);
    float3 r12_p = float3(0, 0, 0);
    fresnel_amplitude(cos_film, eta_base, r12_s, r12_p);

    // Twice the film's optical thickness along the refracted path, in radians per wavelength.
    float3 phase = (4.0 * pi * eta_film * max(0.0, thickness) * cos_film) / thin_film_wavelengths;

    return 0.5 * (airy_reflectance(r01_s, r12_s, phase) + airy_reflectance(r01_p, r12_p, phase));
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
    float2 metal_alpha;

    float3 spec_f0;
    float2 spec_alpha;
    float spec_weight;

    /// The transparent base, which replaces the diffuse substrate under the same interface by `trans_weight`.
    ///
    /// `trans_eta` is the index on the far side of that interface over the index on this one, so it is the reciprocal on the
    /// way out — which is why `bsdf_prepare` has to be told which side it is on.
    /// `trans_tint` is what the crossing costs: `transmission_color` when the depth is 0 or the wall is thin, and white when
    /// a medium is doing the absorbing instead.
    float trans_weight;
    float3 trans_tint;
    float trans_eta;
    float thin_walled;

    /// The interior the transparent base refracts into, which the INTEGRATOR walks rather than the closure.
    ///
    /// `sigma_t` is extinction per unit length and `albedo` the fraction of it that scatters rather than absorbs, so an
    /// albedo of 0 is the pure Beer-Lambert interior `transmission_depth` describes and anything above it is a random walk.
    /// Zero extinction is vacuum, which is every surface that does not transmit.
    float3 medium_sigma_t;
    float3 medium_albedo;
    float medium_g;

    /// The subsurface interior, and how much of the OPAQUE base is it rather than the diffuse substrate.
    /// A separate medium because the two are chosen between per sample: they refract through the same interface and differ
    /// only in what lies beyond it.
    float sss_weight;
    float3 sss_sigma_t;
    float3 sss_albedo;
    float sss_g;

    float coat_f0;
    float2 coat_alpha;

    /// The coat's own frame, in the closure's local coordinates — its normal plus a tangent carried over from the base's.
    ///
    /// Identity when the coat shares the base's normal, which is the overwhelmingly common case and the one every cosine
    /// below is written for.
    float3 coat_n;
    float3 coat_t;
    float3 coat_b;
    float3 coat_tint;
    float coat_weight;
    float coat_darkening;

    float3 fuzz_color;
    float fuzz_alpha;
    float fuzz_weight;

    float metalness;

    float thin_film_weight;
    float thin_film_thickness;
    float thin_film_ior;

    float3 emission;
};

/// How likely each lobe is to be picked for one outgoing direction, in the order the evaluator sums them.
/// The six always add to 1: `bsdf_lobe_probs` normalizes, and falls back to the diffuse lobe when every weight is zero.
struct lobe_probs
{
    float fuzz;
    float coat;
    float metal;
    float spec;
    float diffuse;
    float transmission;
};

/// What one sampled direction carries back: the direction itself, the BSDF value there, and the pdf that produced it.
/// `pdf` is the FULL closure pdf rather than the chosen lobe's, so a caller can weight it against a light sampler directly.
struct bsdf_sample
{
    float3 direction; ///< may point BELOW the surface, which is a refraction rather than a failure
    float3 value;     ///< the BSDF at (wo, direction), cosine NOT folded in
    float pdf;
    bool valid; ///< false when the direction grazed the surface, total internal reflection ended it, or the pdf collapsed

    /// Which interior the direction crossed into: `medium_none`, `medium_transmission` or `medium_subsurface`.
    ///
    /// The two refracting bases share one lobe, so this is the only thing that says which of them a sample belongs to —
    /// and the integrator needs it, because what happens beyond the interface is entirely a property of the medium.
    ///
    /// `medium_none` means the continuation entered no interior, so the caller keeps whatever the incoming segment was
    /// already travelling through.
    /// On a solid that leaves exactly one shape: `medium_none` requires `direction.z > 0`, because a direction crossing
    /// the interface always enters one of the two interiors.
    /// A thin wall is the exception and the only one — it encloses nothing, so it transmits and still reports
    /// `medium_none`, with a direction below the surface.
    /// `probe_medium` asserts both halves, which is what catches a reflective lobe leaking a below-horizon direction.
    uint medium;
};

static const uint medium_none = 0;
static const uint medium_transmission = 1;
static const uint medium_subsurface = 2;

/// The perceptual-to-microfacet roughness mapping OpenPBR specifies, floored so no lobe becomes a delta.
///
/// `anisotropy` stretches the lobe along the frame's tangent and squeezes it along the bitangent, and the pair is scaled so
/// the two together cover about the solid angle the isotropic lobe did — which is what keeps turning anisotropy up from also
/// making the surface look rougher.
/// 0 is isotropic and 1 is as stretched as the model goes.
float2 alpha_of(float roughness, float anisotropy)
{
    float r = saturate(roughness);
    float a = max(min_alpha, r * r);

    float t = 1.0 - saturate(anisotropy);
    float ax = a * sqrt(2.0 / (1.0 + t * t));
    return float2(max(min_alpha, ax), max(min_alpha, t * ax));
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
/// Transmission and subsurface are absent rather than defaulted, so a material cannot ask for one and silently get something
/// else.
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
    float specular_roughness_anisotropy;
    float specular_ior;

    // transmission
    float transmission_weight;
    float3 transmission_color;
    float transmission_depth; ///< the distance `transmission_color` is the color AT; 0 tints the interface instead

    /// What the interior SCATTERS, as a coefficient per unit of `transmission_depth`.
    ///
    /// Black is clear glass — the interior only absorbs, and a ray crosses it in a straight line.
    /// Anything above it makes the interior milky, and light spreads sideways through it the way it does through jade or a
    /// thick liquid.
    /// It needs a positive `transmission_depth` to mean anything: at 0 there is no volume for it to happen in.
    float3 transmission_scatter;

    /// The Henyey-Greenstein g for that scattering: forward at +1, back at -1, isotropic at 0.
    float transmission_scatter_anisotropy;

    /// How far apart the wavelengths are bent, as a multiple of what `transmission_dispersion_abbe_number` implies.
    /// 0 is no dispersion at all, which is what keeps a path from being collapsed onto one wavelength for nothing.
    float transmission_dispersion_scale;

    /// The Abbe number: how little a real glass disperses, so SMALLER means more.
    /// Around 64 for crown glass and 30 for flint; 0 is treated as no dispersion.
    float transmission_dispersion_abbe_number;

    // subsurface
    float subsurface_weight;
    float3 subsurface_color;  ///< the albedo the random walk is inverted to reproduce
    float3 subsurface_radius; ///< the mean free path per channel, before `subsurface_radius_scale`
    float subsurface_radius_scale;
    float subsurface_scatter_anisotropy; ///< the Henyey-Greenstein g: forward at +1, back at -1, isotropic at 0

    // coat
    float coat_weight;
    float3 coat_color;
    float coat_roughness;
    float coat_roughness_anisotropy;
    float coat_ior;
    float coat_darkening;

    // fuzz
    float fuzz_weight;
    float3 fuzz_color;
    float fuzz_roughness;

    // thin film
    float thin_film_weight;
    float thin_film_thickness; ///< nanometres
    float thin_film_ior;

    // emission
    float emission_luminance;
    float3 emission_color;

    // geometry
    /// whether the surface is a shell with no interior — a leaf, a bubble, a pane
    ///
    /// A thin wall does not refract and encloses no medium, so light passes straight through and `transmission_color` tints
    /// it at the crossing whatever `transmission_depth` says.
    /// Nonzero is thin-walled; the parameter is a float because the generated parameter block carries no bools.
    float geometry_thin_walled;

    float3 geometry_normal;

    /// the coat's own shading normal, expressed in the frame the closure works in
    ///
    /// (0, 0, 1) means the coat shares the base's normal, which is what an unbound coat normal must come out as — so the hit
    /// carries an authored one through the base's normal map first, rather than handing it over in the authored tangent
    /// space the base's `geometry_normal` is written in.
    /// A coat with its own bumps over a smooth base is the case this exists for.
    float3 geometry_coat_normal;
    float geometry_opacity;

    /// the surface's tangent frame as a unit quaternion, taking tangent space to OBJECT space
    ///
    /// Tangent space is +x tangent, +y bitangent, +z normal, so the identity rotation is object-space +z — which is a frame
    /// belonging to no particular surface.
    /// That is why the hit consults `SV_ATTR_SUPPLIED_tangent_frame` rather than comparing against the identity: an
    /// unsupplied frame must fall back to the geometric one, and an authored identity is a legitimate value.
    float4 geometry_tangent_frame;

    /// the direction the anisotropic lobes are stretched along, in TANGENT space
    ///
    /// (1, 0, 0) is the frame's own tangent, which is what an unrotated anisotropic highlight follows.
    /// Only the component in the tangent plane is used, so a caller may hand over an object-space direction transformed into
    /// the frame without projecting it first — and a vector that lands on the normal leaves the frame alone rather than
    /// producing an arbitrary one.
    float3 geometry_tangent;

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
    s.specular_roughness_anisotropy = 0.0;
    s.specular_ior = 1.5;

    s.transmission_weight = 0.0;
    s.transmission_color = float3(1, 1, 1);
    s.transmission_depth = 0.0;
    s.transmission_scatter = float3(0, 0, 0);
    s.transmission_scatter_anisotropy = 0.0;
    s.transmission_dispersion_scale = 0.0;
    s.transmission_dispersion_abbe_number = 20.0;

    s.subsurface_weight = 0.0;
    s.subsurface_color = float3(0.8, 0.8, 0.8);
    s.subsurface_radius = float3(1.0, 0.5, 0.25);
    s.subsurface_radius_scale = 0.1;
    s.subsurface_scatter_anisotropy = 0.0;

    s.coat_weight = 0.0;
    s.coat_color = float3(1, 1, 1);
    s.coat_roughness = 0.0;
    s.coat_roughness_anisotropy = 0.0;
    s.coat_ior = 1.6;
    s.coat_darkening = 1.0;

    s.fuzz_weight = 0.0;
    s.fuzz_color = float3(1, 1, 1);
    s.fuzz_roughness = 0.5;

    s.thin_film_weight = 0.0;
    s.thin_film_thickness = 500.0;
    s.thin_film_ior = 1.4;

    s.emission_luminance = 0.0;
    s.emission_color = float3(1, 1, 1);

    s.geometry_thin_walled = 0.0;
    s.geometry_normal = float3(0, 0, 1);
    s.geometry_coat_normal = float3(0, 0, 1);
    s.geometry_opacity = 1.0;
    s.geometry_tangent_frame = float4(0, 0, 0, 1);
    s.geometry_tangent = float3(1, 0, 0);
    s.geometry_handedness = 1.0;

    return s;
}

/// The lobes `s` describes, with every quantity that does not depend on the incoming direction folded in.
///
/// `exiting` is whether the path is LEAVING the surface's interior rather than entering it.
/// The shading frame is always turned to face the incoming ray, so the closure cannot read that off the geometry — and it
/// changes the direction of every refraction, which is why it is a parameter rather than something inferred.
/// It means nothing for a surface that does not transmit.
///
/// `channel` is which wavelength the path has been collapsed onto, or 3 for one that still carries all of them.
/// It only reaches the refraction index, and only when the material actually disperses — a path that never met a dispersive
/// interface keeps its three channels and pays nothing.
bsdf bsdf_prepare(surface s, bool exiting, uint channel)
{
    bsdf b;

    b.metalness = saturate(s.base_metalness);

    b.diffuse_albedo = saturate(s.base_weight) * saturate(s.base_color);
    b.diffuse_roughness = saturate(s.base_diffuse_roughness);

    // The metal's normal-incidence reflectance is the base color itself, and the specular color is its tint at 82 degrees.
    b.metal_f0 = saturate(s.base_weight * s.base_color);
    b.metal_tint = saturate(s.specular_weight * s.specular_color);
    b.metal_alpha = alpha_of(s.specular_roughness, s.specular_roughness_anisotropy);

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
    b.spec_alpha = alpha_of(s.specular_roughness, s.specular_roughness_anisotropy);

    // The coat's frame: its normal, with the base's tangent spun onto it so an anisotropic coat still follows the uv layout.
    // `geometry_coat_tangent` would let the coat point its own way; nothing needs that yet.
    b.coat_n = normalize(s.geometry_coat_normal);
    {
        float3 t = float3(1, 0, 0) - b.coat_n * b.coat_n.x;
        float len = length(t);
        b.coat_t = len > 1e-6 ? t / len : float3(0, 1, 0);
        b.coat_b = cross(b.coat_n, b.coat_t);
    }

    // The transparent base, under the same dielectric interface the diffuse substrate sits under.
    b.trans_weight = saturate(s.transmission_weight);
    b.thin_walled = s.geometry_thin_walled != 0.0 ? 1.0 : 0.0;

    {
        float interface_ior = max(1.0 + 1e-3, s.specular_ior);

        // A collapsed path refracts at ITS wavelength's index, which is the whole of dispersion: the lobe is unchanged and
        // only the angle it bends through differs between the channels.
        if (channel < 3u && s.transmission_dispersion_scale > 0.0)
            interface_ior = dispersive_ior(interface_ior, s.transmission_dispersion_abbe_number,
                                           s.transmission_dispersion_scale, channel_wavelengths[channel]);

        b.trans_eta = exiting ? 1.0 / interface_ior : interface_ior;

        // A positive depth makes the color a property of the VOLUME, so the crossing costs nothing and the integrator
        // attenuates over the distance instead.
        // A thin wall encloses no volume at all, so it always pays at the crossing however the depth is authored.
        bool volumetric = s.transmission_depth > 0.0 && b.thin_walled == 0.0;

        b.trans_tint = volumetric ? float3(1, 1, 1) : saturate(s.transmission_color);

        // Beer-Lambert, solved so that `transmission_color` is what survives exactly `transmission_depth` of travel — and
        // the scattering coefficient over that same depth, so the two are read against one scale.
        float3 c = clamp(saturate(s.transmission_color), 1e-4, 1.0);
        float depth = max(s.transmission_depth, 1e-6);

        float3 sigma_a = volumetric ? -log(c) / depth : float3(0, 0, 0);
        float3 sigma_s = volumetric ? max(float3(0, 0, 0), s.transmission_scatter) / depth : float3(0, 0, 0);

        // Extinction is what the two do together, and the albedo is scattering's share of it.
        // A clear interior leaves that share at zero, which is what tells the integrator it can cross in a straight line
        // rather than walking.
        b.medium_sigma_t = sigma_a + sigma_s;
        b.medium_albedo = sigma_s / max(b.medium_sigma_t, float3(1e-9, 1e-9, 1e-9));
        b.medium_g = clamp(s.transmission_scatter_anisotropy, -0.95, 0.95);
    }

    // The subsurface interior: a dense scattering medium under the same interface, which the integrator random-walks.
    b.sss_weight = b.thin_walled != 0.0 ? 0.0 : saturate(s.subsurface_weight);
    {
        // The mean free path is what `subsurface_radius` states, scaled — so extinction is its reciprocal.
        float3 mfp = max(saturate(s.subsurface_radius) * max(s.subsurface_radius_scale, 0.0), float3(1e-4, 1e-4, 1e-4));
        b.sss_sigma_t = 1.0 / mfp;

        // Chiang's inversion: the single-scattering albedo whose random walk comes back out the color that was ASKED for.
        // Without it `subsurface_color` would be the albedo of one scattering event, and a walk of dozens of them would
        // return something far darker than the value the author typed.
        float3 a = saturate(s.subsurface_color);
        b.sss_albedo = saturate(1.0 - exp(-5.09406 * a + 2.61188 * a * a - 4.31805 * a * a * a));
        b.sss_g = clamp(s.subsurface_scatter_anisotropy, -0.95, 0.95);
    }

    float cior = max(1.0 + 1e-3, s.coat_ior);
    float cr0 = (cior - 1.0) / (cior + 1.0);
    b.coat_f0 = cr0 * cr0;
    b.coat_alpha = alpha_of(s.coat_roughness, s.coat_roughness_anisotropy);
    b.coat_tint = saturate(s.coat_color);
    b.coat_weight = saturate(s.coat_weight);
    b.coat_darkening = saturate(s.coat_darkening);

    b.fuzz_weight = saturate(s.fuzz_weight);
    b.fuzz_color = saturate(s.fuzz_color);
    b.fuzz_alpha = clamp(s.fuzz_roughness, min_alpha, 1.0);

    b.thin_film_weight = saturate(s.thin_film_weight);
    b.thin_film_thickness = max(0.0, s.thin_film_thickness);
    b.thin_film_ior = max(1.0 + 1e-3, s.thin_film_ior);

    b.emission = max(float3(0, 0, 0), s.emission_luminance * s.emission_color);

    return b;
}

/// What the fuzz layer transmits to everything below it, in one direction.
float fuzz_transmission(bsdf b, float mu)
{
    return 1.0 - b.fuzz_weight * sheen_albedo(mu, b.fuzz_alpha);
}

/// `w` expressed in the coat's own frame, which is where its lobe is evaluated and sampled.
float3 to_coat(bsdf b, float3 w)
{
    return float3(dot(w, b.coat_t), dot(w, b.coat_b), dot(w, b.coat_n));
}

/// A direction in the coat's frame, back in the closure's own.
float3 from_coat(bsdf b, float3 w)
{
    return w.x * b.coat_t + w.y * b.coat_b + w.z * b.coat_n;
}

/// What the coat transmits to the base, in one direction.
float coat_transmission(bsdf b, float mu)
{
    float2 ab = ggx_albedo_split(mu, alpha_iso(b.coat_alpha));
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

/// What two crossings of the coat cost, for a pair that may straddle the surface.
///
/// A transmitted direction leaves on the far side, where the coat is not — so only the entry crossing is charged, and the
/// exit is charged at the mirrored cosine as the closest thing to it this model has.
float coat_crossing(bsdf b, float3 wo, float3 wi)
{
    float3 wo_c = to_coat(b, wo);
    float mu_o = wo_c.z > 0.0 ? wo_c.z : 0.0;
    float mu_i = abs(to_coat(b, wi).z);

    if (mu_o <= 0.0)
        return 1.0;

    return coat_transmission(b, mu_o) * coat_transmission(b, mu_i);
}

/// What the dielectric interface lets THROUGH at one incidence — exactly the complement of what its reflection lobe keeps.
///
/// Reading it off the reflection rather than computing a Fresnel of its own is the point.
/// The two halves of one interface have to sum to 1 or the surface invents or destroys light, and they did not: the
/// reflection used Schlick through `spec_f0` while the refraction used the exact dielectric relation.
/// Those two agree closely at the indices real glass has, which is why the error stayed under the probe's energy bound —
/// but they diverge completely as the index approaches 1, where Schlick's grazing tail still climbs to white and the exact
/// relation correctly goes to nothing.
///
/// Past the critical angle this understates the internal reflection, since Schlick has no critical angle.
/// The transmitted DIRECTION does not exist there — `refract` reports it and the sample ends — so nothing is created; what
/// is lost is the energy that should have bounced back inside, which the viewer TODO records.
float3 interface_transmittance(bsdf b, float mu)
{
    return saturate(float3(1, 1, 1) - b.spec_weight * fresnel_schlick(max(mu, 1e-6), b.spec_f0));
}

/// The rough-refraction BTDF through the dielectric interface (Walter 2007), Fresnel included and UNTINTED.
///
/// The tint belongs to the caller because the transparent base and the subsurface base refract through this same interface
/// and differ only in what lies beyond it — one tints at the crossing, the other does not.
///
/// The radiance-compression factor is deliberately ABSENT.
///
/// A beam entering a denser medium is squeezed into a narrower cone, so the radiance along it rises by the square of the
/// index ratio — and a path traced from the LIGHT would have to carry that.
/// This one is traced from the camera, which transports importance rather than radiance, and the two conventions differ by
/// exactly that factor.
/// Including it here would have a lossless interface return 2.25 times the light it received, which is what the probe's
/// energy bound caught.
///
/// A BTDF is not reciprocal either way: `f(wo, wi)` and `f(wi, wo)` differ by that same ratio, which is a property of
/// radiance rather than an error — so the probe compares only directions on one side.
float3 transmission_btdf(bsdf b, float3 wo, float3 wi)
{
    // A thin wall has no interior to refract into: light goes straight on through, spread by the interface's roughness.
    // So the lobe is the reflection lobe mirrored about the surface, which is what `-wi` recovers.
    if (b.thin_walled != 0.0)
    {
        float3 wi_mirror = float3(wi.x, wi.y, -wi.z);
        float3 h = normalize(wo + wi_mirror);
        float mu_h = max(dot(wo, h), 1e-6);
        float d = ggx_d(h, b.spec_alpha);
        float g = ggx_g2(wo, wi_mirror, b.spec_alpha);

        return interface_transmittance(b, mu_h) * (d * g / (4.0 * wo.z * max(wi_mirror.z, 1e-6)));
    }

    float eta = b.trans_eta;
    float3 h = refraction_half_vector(wo, wi, eta);

    float dot_o = dot(wo, h);
    float dot_i = dot(wi, h);
    if (dot_o <= 0.0 || dot_i >= 0.0)
        return float3(0, 0, 0); // the pair does not correspond to a refraction about any microfacet

    float d = ggx_d(h, b.spec_alpha);
    float g = ggx_g2(wo, float3(wi.x, wi.y, -wi.z), b.spec_alpha);

    float denom = dot_o + eta * dot_i;
    float scale = abs(dot_o) * abs(dot_i) / max(wo.z * abs(wi.z) * denom * denom, 1e-9);

    return interface_transmittance(b, dot_o) * d * g * scale;
}

/// The full BSDF at (`wo`, `wi`), with the cosine NOT folded in.
/// Both directions must be unit and in the local frame; a direction below the surface evaluates to zero.
float3 bsdf_eval(bsdf b, float3 wo, float3 wi)
{
    if (wo.z <= 0.0)
        return float3(0, 0, 0);

    // A direction on the far side is a TRANSMISSION, and only the transparent base produces one.
    // Everything layered above it — the coat, the fuzz — reflects, so what reaches here has already crossed both and pays
    // for both again on the way out.
    if (wi.z < 0.0)
    {
        // What the two refracting bases weigh between them. The transparent one pays its tint at the crossing; the
        // subsurface one pays nothing here, because everything it costs happens inside.
        float3 tint = b.trans_weight * b.trans_tint + (1.0 - b.trans_weight) * b.sss_weight * float3(1, 1, 1);
        if (all(tint <= float3(0, 0, 0)))
            return float3(0, 0, 0);

        float3 f_btdf = transmission_btdf(b, wo, wi);
        float t_coat_x = coat_crossing(b, wo, wi);
        float t_fuzz_x = fuzz_transmission(b, wo.z) * fuzz_transmission(b, abs(wi.z));

        return f_btdf * tint * t_coat_x * t_fuzz_x * (1.0 - b.metalness);
    }

    if (wi.z <= 0.0)
        return float3(0, 0, 0);

    float mu_o = wo.z;
    float mu_i = wi.z;
    float3 h = normalize(wo + wi);
    float mu_h = max(dot(wo, h), 1e-6);
    float denom = 4.0 * mu_o * mu_i;

    // The metal and the dielectric specular share a half-vector and a roughness, so they share D and G2 as well.
    float d_spec = ggx_d(h, b.spec_alpha);
    float g_spec = ggx_g2(wo, wi, b.spec_alpha);

    // The film replaces the interface's Fresnel rather than tinting what it returned, because the interference is what the
    // reflectance IS at that thickness — a multiply could only darken where the film should be shifting hue.
    //
    // The layer coupling below still reads the film-free `spec_f0`. Averaged over the spectrum a film redistributes
    // reflectance rather than adding it, so what reaches the diffuse substrate is close; a film-aware transmission would
    // need the same albedo table the energy compensation is waiting on.
    float3 f_metal_fresnel = fresnel_f82(mu_h, b.metal_f0, b.metal_tint);
    float3 f_spec_fresnel = fresnel_schlick(mu_h, b.spec_f0);
    if (b.thin_film_weight > 0.0)
    {
        f_metal_fresnel = lerp(f_metal_fresnel,
                               thin_film_reflectance(mu_h, b.metal_f0, b.thin_film_ior, b.thin_film_thickness),
                               b.thin_film_weight);
        f_spec_fresnel = lerp(f_spec_fresnel,
                              thin_film_reflectance(mu_h, b.spec_f0, b.thin_film_ior, b.thin_film_thickness),
                              b.thin_film_weight);
    }

    float3 f_metal = f_metal_fresnel * (d_spec * g_spec / denom)
                   * ggx_energy_compensation(mu_o, b.metal_alpha, b.metal_f0);

    float3 f_spec = b.spec_weight * f_spec_fresnel * (d_spec * g_spec / denom)
                  * ggx_energy_compensation(mu_o, b.spec_alpha, b.spec_f0);

    // The diffuse substrate takes what the specular layer let through, both on the way in and on the way out.
    float3 t_spec = spec_transmission(b, mu_o) * spec_transmission(b, mu_i);
    float3 f_diffuse = (1.0 - b.trans_weight) * (1.0 - b.sss_weight) * b.diffuse_albedo
                     * (oren_nayar(wo, wi, b.diffuse_roughness) / pi) * t_spec;

    float3 f_base = lerp(f_spec + f_diffuse, f_metal, b.metalness);

    // The coat, in its OWN frame: it may carry a normal the base does not, and every cosine its lobe needs is measured
    // against that normal rather than the base's.
    //
    // What comes back is still added as a BSDF about the BASE normal, which is the standard simplification — the two frames
    // disagree by a cosine ratio that no closed form absorbs, and the alternative is a second integrator.
    // A direction below the coat's horizon sees no coat at all: it neither reflects nor blocks, so the base is charged
    // nothing for a crossing that did not happen.
    float3 wo_c = to_coat(b, wo);
    float3 wi_c = to_coat(b, wi);

    float3 f_coat = float3(0, 0, 0);
    float t_coat = 1.0;
    if (wo_c.z > 0.0 && wi_c.z > 0.0)
    {
        float3 h_c = normalize(wo_c + wi_c);
        float mu_hc = max(dot(wo_c, h_c), 1e-6);
        float d_coat = ggx_d(h_c, b.coat_alpha);
        float g_coat = ggx_g2(wo_c, wi_c, b.coat_alpha);

        f_coat = b.coat_weight * fresnel_dielectric(mu_hc, (1.0 + sqrt(b.coat_f0)) / max(1e-3, 1.0 - sqrt(b.coat_f0)))
               * (d_coat * g_coat / (4.0 * wo_c.z * wi_c.z));

        t_coat = coat_transmission(b, wo_c.z) * coat_transmission(b, wi_c.z);
    }
    float3 coat_absorption = lerp(float3(1, 1, 1), b.coat_tint, b.coat_weight);
    float3 below_coat = f_base * t_coat * coat_absorption * coat_darkening_factor(b);

    // The fuzz sits above everything, and takes its share on both crossings.
    float3 f_fuzz = b.fuzz_weight * b.fuzz_color * sheen_d(h, b.fuzz_alpha) * sheen_v(wo, wi);
    float t_fuzz = fuzz_transmission(b, mu_o) * fuzz_transmission(b, mu_i);

    return f_fuzz + t_fuzz * (f_coat + below_coat);
}

/// How the six lobes split one outgoing direction's sampling budget.
/// Each weight is roughly what its lobe reflects, so a lobe that contributes nothing is never picked.
lobe_probs bsdf_lobe_probs(bsdf b, float3 wo)
{
    float mu = max(wo.z, 1e-4);

    lobe_probs p;
    p.fuzz = b.fuzz_weight * luminance(b.fuzz_color) * sheen_albedo(mu, b.fuzz_alpha);

    float t_fuzz = fuzz_transmission(b, mu);

    // The coat is ranked by what it reflects at ITS cosine, so a coat tilted away from the view is picked less often — and
    // a coat turned past the horizon is not picked at all, which is the same case `bsdf_eval` charges nothing for.
    float mu_coat = to_coat(b, wo).z;
    float2 coat_ab = ggx_albedo_split(max(mu_coat, 1e-4), alpha_iso(b.coat_alpha));
    p.coat = mu_coat > 0.0 ? t_fuzz * b.coat_weight * (b.coat_f0 * coat_ab.x + coat_ab.y) : 0.0;

    float below = t_fuzz * (mu_coat > 0.0 ? coat_transmission(b, mu_coat) : 1.0);
    p.metal = below * b.metalness * luminance(ggx_albedo(mu, b.metal_alpha, b.metal_f0));
    p.spec = below * (1.0 - b.metalness) * b.spec_weight * luminance(ggx_albedo(mu, b.spec_alpha, b.spec_f0));

    float3 t_spec = spec_transmission(b, mu);
    float opaque = (1.0 - b.trans_weight);
    p.diffuse = below * (1.0 - b.metalness) * opaque * (1.0 - b.sss_weight) * luminance(b.diffuse_albedo * t_spec);

    // Both refracting bases share ONE sampling lobe: they cross the same interface at the same roughness and differ only
    // in what lies beyond it, so giving them separate lobes would be two names for one distribution.
    // Which interior a drawn direction entered is decided after the fact, in `bsdf_sample_direction`.
    float3 through = t_spec * (b.trans_weight * b.trans_tint + opaque * b.sss_weight);
    p.transmission = below * (1.0 - b.metalness) * luminance(through);

    float sum = p.fuzz + p.coat + p.metal + p.spec + p.diffuse + p.transmission;
    if (sum <= 1e-6)
    {
        // Nothing reflects anything worth sampling; the diffuse lobe keeps the pdf well-defined rather than zero.
        p.fuzz = 0.0;
        p.coat = 0.0;
        p.metal = 0.0;
        p.spec = 0.0;
        p.diffuse = 1.0;
        p.transmission = 0.0;
        return p;
    }

    float inv = 1.0 / sum;
    p.fuzz *= inv;
    p.coat *= inv;
    p.metal *= inv;
    p.spec *= inv;
    p.diffuse *= inv;
    p.transmission *= inv;
    return p;
}

/// The solid-angle pdf `bsdf_sample` would produce for `wi`, which is what the multiple-importance weight against a light
/// sampler needs.
float bsdf_pdf(bsdf b, float3 wo, float3 wi)
{
    if (wo.z <= 0.0)
        return 0.0;

    lobe_probs p = bsdf_lobe_probs(b, wo);

    // Only the transparent base reaches the far side, so a transmitted direction is that lobe's density alone.
    if (wi.z < 0.0)
    {
        if (p.transmission <= 0.0)
            return 0.0;

        if (b.thin_walled != 0.0)
            return p.transmission * ggx_pdf(wo, float3(wi.x, wi.y, -wi.z), b.spec_alpha);

        return p.transmission * ggx_refraction_pdf(wo, wi, b.spec_alpha, b.trans_eta);
    }

    if (wi.z <= 0.0)
        return 0.0;

    float cosine = wi.z / pi;

    return p.fuzz * cosine                                //
         + p.coat * ggx_pdf(to_coat(b, wo), to_coat(b, wi), b.coat_alpha) //
         + p.metal * ggx_pdf(wo, wi, b.metal_alpha)    //
         + p.spec * ggx_pdf(wo, wi, b.spec_alpha)      //
         + p.diffuse * cosine;
}

/// One direction drawn from the closure, with the BSDF and the full pdf there.
///
/// `u` is three uniforms in [0, 1): the first picks the lobe and the other two the direction within it.
/// The lobe choice is a one-sample estimator over the six, so the returned pdf is the mixture's rather than the picked
/// lobe's — which is what keeps a lobe that another lobe could also have produced from being counted twice.
///
/// The direction may leave the UPPER hemisphere: a transparent base refracts, and the caller has to follow it through the
/// surface rather than treating the sample as invalid.
bsdf_sample bsdf_sample_direction(bsdf b, float3 wo, float3 u)
{
    bsdf_sample r;
    r.direction = float3(0, 0, 1);
    r.value = float3(0, 0, 0);
    r.pdf = 0.0;
    r.valid = false;
    r.medium = medium_none;

    if (wo.z <= 0.0)
        return r;

    lobe_probs p = bsdf_lobe_probs(b, wo);

    // Walk the six in the order the struct lists them, so the pick is a single pass over the cdf.
    float pick = u.x;
    float3 wi = float3(0, 0, 1);

    // A REFLECTIVE lobe that produced a direction below the horizon has failed, and each one ends the sample itself.
    //
    // Rejecting here rather than at the bottom is what keeps the two populations apart: `bsdf_eval` and `bsdf_pdf` both read
    // a direction below the surface as a transmission, so a below-horizon reflection surviving to them would be valued as a
    // BTDF and scored against a refraction density that did not produce it — and reported as `medium_none` while pointing
    // through the surface, which sends the path into an interior it never entered.
    // On an opaque surface the pdf collapses to zero and the sample is dropped anyway; on a transmissive one nothing
    // downstream can tell the two apart.
    if (pick < p.fuzz)
    {
        wi = sample_cosine_local(u.yz);
        if (wi.z <= 0.0)
            return r;
    }
    else if (pick < p.fuzz + p.coat)
    {
        // Drawn in the coat's frame and brought back, so a tilted coat reflects where its own normal says rather than
        // where the base's does.
        float3 wo_c = to_coat(b, wo);
        if (wo_c.z <= 0.0)
            return r;

        float3 h_c = ggx_sample_vndf(wo_c, b.coat_alpha, u.yz);
        wi = from_coat(b, reflect(-wo_c, h_c));

        // The coat's own horizon is not the base's, so this rejects against the frame the closure is evaluated in.
        if (wi.z <= 0.0)
            return r;
    }
    else if (pick < p.fuzz + p.coat + p.metal)
    {
        float3 h = ggx_sample_vndf(wo, b.metal_alpha, u.yz);
        wi = reflect(-wo, h);
        if (wi.z <= 0.0)
            return r;
    }
    else if (pick < p.fuzz + p.coat + p.metal + p.spec)
    {
        float3 h = ggx_sample_vndf(wo, b.spec_alpha, u.yz);
        wi = reflect(-wo, h);
        if (wi.z <= 0.0)
            return r;
    }
    else if (pick < p.fuzz + p.coat + p.metal + p.spec + p.diffuse)
    {
        wi = sample_cosine_local(u.yz);
        if (wi.z <= 0.0)
            return r;
    }
    else
    {
        // A refracting base. Which of the two is decided here, in proportion to what each contributes — the direction is
        // the same either way, so the pick costs no extra distribution and changes only the interior reported.
        float w_trans = b.trans_weight * luminance(b.trans_tint);
        float w_sss = (1.0 - b.trans_weight) * b.sss_weight;
        float w_sum = w_trans + w_sss;

        // The lobe pick already consumed `u.x`; reusing where it landed INSIDE the transmission slice keeps this free of a
        // fourth uniform, and the two choices are independent of the direction the slice goes on to draw.
        float slice_lo = p.fuzz + p.coat + p.metal + p.spec + p.diffuse;
        float slice = max(p.transmission, 1e-9);
        float within = saturate((pick - slice_lo) / slice);

        // A thin wall encloses nothing, so passing through one enters no interior at all — and saying so here is what
        // keeps that fact in the closure, which is the only thing that knows it.
        if (b.thin_walled != 0.0)
            r.medium = medium_none;
        else
            r.medium = (w_sum <= 0.0 || within * w_sum < w_trans) ? medium_transmission : medium_subsurface;

        // Refracted about a visible microfacet, or straight through when the wall is thin.
        float3 h = ggx_sample_vndf(wo, b.spec_alpha, u.yz);
        if (b.thin_walled != 0.0)
        {
            float3 wi_mirror = reflect(-wo, h);
            if (wi_mirror.z <= 1e-6)
                return r;
            wi = float3(wi_mirror.x, wi_mirror.y, -wi_mirror.z);
        }
        else
        {
            wi = refract(-wo, h, 1.0 / b.trans_eta);

            // Total internal reflection: `refract` returns zero, and there is no transmitted direction to hand back.
            // The reflected lobe already carries that energy through its own Fresnel, so this ends the sample rather
            // than turning it into a reflection the pdf does not describe.
            if (dot(wi, wi) < 1e-6 || wi.z >= -1e-6)
                return r;
            wi = normalize(wi);
        }
    }

    if (wi.z > -1e-6 && wi.z <= 1e-6)
        return r;

    if (wi.z > 0.0)
        r.medium = medium_none; // a reflected direction crossed nothing, whichever lobe produced it

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
/// The fallback for a surface that supplies no tangent frame, and the tangent it invents follows nothing.
/// So an anisotropic highlight on such a surface points somewhere arbitrary but consistent, and a normal map applied through
/// it does not follow the uv layout — both want `frame_from_quaternion` and a mesh that carries the frame.
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

/// `f` spun about its own normal so its tangent points along the tangent-space direction `t_tangent`.
///
/// This is what `geometry_tangent` asks for, and it is the only thing that gives an anisotropic highlight a direction: the
/// two roughness axes are measured against the frame's tangent, so rotating the frame rotates the highlight.
/// A direction with nothing left in the tangent plane — one that came out parallel to the normal — leaves the frame alone
/// rather than picking an arbitrary replacement.
frame rotate_frame_to_tangent(frame f, float3 t_tangent)
{
    float3 t = t_tangent.x * f.t + t_tangent.y * f.b;
    float len = length(t);
    if (len < 1e-6)
        return f;

    frame r;
    r.n = f.n;
    r.t = t / len;
    r.b = cross(f.n, r.t) * (dot(cross(f.n, r.t), f.b) < 0.0 ? -1.0 : 1.0);
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
