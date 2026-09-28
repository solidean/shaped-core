// A tiny path tracer, here to produce the one thing a denoiser needs and a raster pass cannot give it: real Monte
// Carlo noise, with exact guide buffers beside it.
//
// Analytic geometry — a ground plane and three spheres — so there is no acceleration structure, no mesh and no asset
// in the way. What matters for the example is that the noise is the genuine article: a few samples per pixel of
// cosine-sampled indirect light and a stochastically sampled area light, which is exactly the signal sr's denoisers
// are built for. Turning `spp` down makes it worse in the way a real tracer gets worse.
//
// It writes five images in one dispatch:
//   gColor      the noisy radiance, and the running mean when the caller is accumulating
//   gAlbedo     the primary hit's diffuse reflectance, so the denoiser can filter lighting and keep texture
//   gNormal     the primary hit's world normal
//   gDepth      linear view depth, 0 where the ray escaped
//   gMotion     this pixel minus where the same surface was last frame, in pixels
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct scene_constants
{
    // The previous frame's view-projection, one ROW per float4, so nothing here depends on how a float4x4 would be
    // packed into a constant buffer.
    float4 prev_vp_row0;
    float4 prev_vp_row1;
    float4 prev_vp_row2;
    float4 prev_vp_row3;

    float4 origin;  // xyz eye, w tan(vertical fov / 2)
    float4 forward; // xyz, w aspect ratio
    float4 right;   // xyz, w unused
    float4 up;      // xyz, w unused

    uint frame;       // drives the per-frame random sequence
    uint accum_frame; // 0 restarts the running mean, >0 folds this frame into it
    int spp;          // samples per pixel this frame
    float light_size; // area light radius: bigger is softer and noisier
};

#pragma sc push_constants
ConstantBuffer<scene_constants> gConstants;

#pragma sc group 0
namespace scene_bindings
{
    RWTexture2D<float4> gColor;
    RWTexture2D<float4> gAlbedo;
    RWTexture2D<float4> gNormal;
    RWTexture2D<float4> gDepth;
    RWTexture2D<float4> gMotion;
}

using namespace scene_bindings;

static const float3 k_light_centre = float3(2.5, 6.0, 1.5);
static const float3 k_light_power = float3(320.0, 296.0, 250.0);

uint hash_u32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rand_next(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return float(hash_u32(state) >> 8) * (1.0 / 16777216.0);
}

// A sphere: xyz centre, w radius. The albedo table below is parallel to it.
static const float4 k_spheres[3] = {
    float4(-1.6, 1.0, 0.0, 1.0),
    float4(1.1, 0.75, 0.6, 0.75),
    float4(0.2, 0.45, -1.8, 0.45),
};
static const float3 k_sphere_albedo[3] = {
    float3(0.85, 0.28, 0.22),
    float3(0.24, 0.52, 0.86),
    float3(0.88, 0.76, 0.22),
};

// The ground's two-tone check, which is what makes albedo demodulation visible: the denoiser must keep this crisp
// while smoothing the light falling on it.
float3 ground_albedo(float3 p)
{
    int2 cell = int2(floor(p.xz * 0.5));
    bool odd = ((cell.x + cell.y) & 1) != 0;
    return odd ? float3(0.62, 0.62, 0.64) : float3(0.30, 0.31, 0.34);
}

struct hit
{
    float t;
    float3 position;
    float3 normal;
    float3 albedo;
};

// Nearest intersection along `o + t * d` for t in (t_min, t_max), or t < 0 for a miss.
hit trace(float3 o, float3 d, float t_min, float t_max)
{
    hit h;
    h.t = -1;
    h.position = float3(0, 0, 0);
    h.normal = float3(0, 0, 0);
    h.albedo = float3(0, 0, 0);

    float best = t_max;

    // The ground plane, y = 0, kept finite so the horizon is sky rather than an infinite floor.
    if (abs(d.y) > 1e-6)
    {
        float t = -o.y / d.y;
        if (t > t_min && t < best)
        {
            float3 p = o + t * d;
            if (dot(p.xz, p.xz) < 400.0)
            {
                best = t;
                h.t = t;
                h.position = p;
                h.normal = float3(0, 1, 0);
                h.albedo = ground_albedo(p);
            }
        }
    }

    for (int i = 0; i < 3; ++i)
    {
        float3 c = k_spheres[i].xyz;
        float r = k_spheres[i].w;

        // Solved at the ray's closest approach rather than from the origin, which keeps the discriminant's sign
        // meaningful when the eye is far from a small sphere.
        float3 oc = o - c;
        float b = dot(oc, d);
        float3 closest = oc - b * d;
        float disc = r * r - dot(closest, closest);
        if (disc < 0)
            continue;

        float sq = sqrt(disc);
        float t = -b - sq;
        if (t <= t_min)
            t = -b + sq;
        if (t <= t_min || t >= best)
            continue;

        best = t;
        h.t = t;
        h.position = o + t * d;
        h.normal = normalize(h.position - c);
        h.albedo = k_sphere_albedo[i];
    }
    return h;
}

float3 sky(float3 d)
{
    float up = saturate(d.y * 0.5 + 0.5);
    return lerp(float3(0.10, 0.12, 0.16), float3(0.26, 0.38, 0.62), up);
}

// A cosine-weighted direction about `n`, which is what makes the diffuse estimator unbiased without a pdf term.
float3 cosine_hemisphere(float3 n, inout uint rng)
{
    float u1 = rand_next(rng);
    float u2 = rand_next(rng);
    float r = sqrt(u1);
    float phi = 6.2831853 * u2;

    float3 up = abs(n.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n));
    float3 ty = cross(n, tx);
    return normalize(tx * (r * cos(phi)) + ty * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

// Direct light from the area light, sampled at one point on it — the soft shadow, and most of the noise.
float3 direct_light(float3 p, float3 n, inout uint rng)
{
    float3 jitter = float3(rand_next(rng), rand_next(rng), rand_next(rng)) * 2.0 - 1.0;
    float3 lp = k_light_centre + jitter * gConstants.light_size;

    float3 to_light = lp - p;
    float dist2 = max(dot(to_light, to_light), 1e-4);
    float dist = sqrt(dist2);
    float3 l = to_light / dist;

    float ndotl = dot(n, l);
    if (ndotl <= 0)
        return float3(0, 0, 0);

    hit shadow = trace(p + n * 1e-3, l, 1e-3, dist - 1e-3);
    if (shadow.t > 0)
        return float3(0, 0, 0);

    return k_light_power * (ndotl / dist2);
}

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gColor.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int2 px = int2(id.xy);
    uint rng = hash_u32(id.x + id.y * size.x + gConstants.frame * 9781u);

    float3 o = gConstants.origin.xyz;
    float tan_half = gConstants.origin.w;
    float aspect = gConstants.forward.w;

    float3 radiance = float3(0, 0, 0);
    float3 primary_albedo = float3(0, 0, 0);
    float3 primary_normal = float3(0, 0, 0);
    float primary_depth = 0;
    float3 primary_position = float3(0, 0, 0);
    bool primary_hit = false;

    int spp = max(1, gConstants.spp);
    for (int s = 0; s < spp; ++s)
    {
        // Jittered inside the pixel, so more samples anti-alias as well as converge.
        float2 jitter = float2(rand_next(rng), rand_next(rng));
        float2 ndc = (float2(px) + jitter) / float2(size) * 2.0 - 1.0;
        float3 d = normalize(gConstants.forward.xyz + gConstants.right.xyz * (ndc.x * tan_half * aspect)
                             - gConstants.up.xyz * (ndc.y * tan_half));

        hit h = trace(o, d, 1e-3, 1e4);
        if (h.t < 0)
        {
            radiance += sky(d);
            continue;
        }

        // The guides describe the surface rather than the estimate, so one sample's worth of them is exact.
        // Taken from the first sample only: every sample of a pixel sees essentially the same primary surface, and
        // averaging normals across a silhouette would soften the very edge the denoiser leans on.
        if (!primary_hit)
        {
            primary_hit = true;
            primary_albedo = h.albedo;
            primary_normal = h.normal;
            primary_position = h.position;
            primary_depth = dot(h.position - o, gConstants.forward.xyz);
        }

        // Direct, then one cosine-sampled diffuse bounce. Two terms is enough to look like global illumination and
        // to be visibly noisy at low sample counts, which is the whole point of the scene.
        float3 sample_radiance = h.albedo * direct_light(h.position, h.normal, rng) * 0.03183; // 1/pi

        float3 bounce_dir = cosine_hemisphere(h.normal, rng);
        hit b = trace(h.position + h.normal * 1e-3, bounce_dir, 1e-3, 1e4);
        float3 incoming = b.t < 0 ? sky(bounce_dir) : b.albedo * direct_light(b.position, b.normal, rng) * 0.03183;
        sample_radiance += h.albedo * incoming;

        radiance += sample_radiance;
    }
    radiance /= float(spp);

    // Progressive accumulation, read-modify-write at this dispatch's own pixel, so no second texture is needed.
    // The caller restarts it by sending accum_frame 0, which is what a camera move or a settings change does.
    if (gConstants.accum_frame > 0)
    {
        float n = float(gConstants.accum_frame);
        radiance = (gColor[id.xy].rgb * n + radiance) / (n + 1.0);
    }

    // Where this pixel's surface was last frame, in pixels — what a temporal denoiser reprojects along.
    // A pixel that hit nothing has no surface to follow, so it carries no motion.
    float2 motion = float2(0, 0);
    if (primary_hit)
    {
        float4 p4 = float4(primary_position, 1.0);
        float4 prev_clip = float4(dot(gConstants.prev_vp_row0, p4), dot(gConstants.prev_vp_row1, p4),
                                  dot(gConstants.prev_vp_row2, p4), dot(gConstants.prev_vp_row3, p4));
        if (prev_clip.w > 1e-6)
        {
            float2 prev_ndc = prev_clip.xy / prev_clip.w;
            float2 prev_px = (float2(prev_ndc.x, -prev_ndc.y) * 0.5 + 0.5) * float2(size);
            motion = (float2(px) + 0.5) - prev_px;
        }
    }

    gColor[id.xy] = float4(radiance, 1.0);
    gAlbedo[id.xy] = float4(primary_hit ? primary_albedo : float3(1, 1, 1), 1.0);
    gNormal[id.xy] = float4(primary_normal, 0.0);
    gDepth[id.xy] = float4(primary_depth, 0, 0, 0);
    gMotion[id.xy] = float4(motion, 0, 0);
}
