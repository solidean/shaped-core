#include "common.hlsli"
#include "background.hlsli"
#include "mesh.hlsli" // Vertices / Indices + mesh_triangle
#include "pbr.hlsli"

// Miss + closest-hit for the basic flat-shaded PBR path.
// The material is looked up per triangle by PrimitiveIndex(); the flat face normal is recomputed from the
// mesh's own vertices, so a random triangle soup shades correctly with no per-vertex normals.

StructuredBuffer<PbrMaterial> Materials : register(t1);

struct Attributes
{
    float2 bary;
};

[shader("miss")]
void Miss(inout Payload payload)
{
    float3 dir = normalize(WorldRayDirection());
    payload.color = float4(background_radiance(dir), 1.0);
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in Attributes attribs)
{
    uint prim = PrimitiveIndex();
    PbrMaterial m = Materials[prim];

    // Flat face normal from the triangle's object-space vertices, moved into world space.
    Triangle3 tri = mesh_triangle(prim, frame.mesh_is_indexed != 0);
    float3 n_obj = normalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), n_obj));

    float3 V = -normalize(WorldRayDirection());
    if (dot(N, V) < 0.0)
        N = -N; // two-sided: face the viewer so random winding still shades

    // Image-based lighting from the SH environment probe (no analytic light):
    //  - diffuse: the Lambertian response to the probe's irradiance, E(N) * albedo / PI, metals excluded
    //  - specular: a Fresnel-weighted environment reflection along the mirror direction. The probe is
    //    inherently low-frequency, so this reads as a broad, rough reflection; roughness is not yet used.
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), m.base_color, m.metallic);
    float n_dot_v = saturate(dot(N, V));
    float3 F = fresnel_schlick(n_dot_v, f0);

    float3 kd = (float3(1, 1, 1) - F) * (1.0 - m.metallic);
    float3 diffuse = kd * m.base_color * background_irradiance(N) / SV_PI;
    float3 specular = F * background_radiance(reflect(-V, N));

    float3 color = diffuse + specular + m.emissive;
    payload.color = float4(color, 1.0);
}
