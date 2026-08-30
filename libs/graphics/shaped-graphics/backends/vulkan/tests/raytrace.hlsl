// raytrace.hlsl — a minimal ray-tracing library for the vulkan end-to-end trace test.
//
// One raygen, one miss and one closest-hit shader over a single triangle: the raygen traces one ray per dispatch
// index and writes 1 on a hit, 0 on a miss.
// Rays alternate between hitting and missing by index, so the readback distinguishes "traced correctly" from
// "wrote a constant".
//
// Compiled to one SPIR-V module with three entry points, which is what a shader library is:
//   dxc -T lib_6_3 -spirv -fspv-target-env=vulkan1.3 -Fh raytrace.spirv.h -Vn raytrace_spirv raytrace.hlsl

[[vk::binding(0, 0)]] RaytracingAccelerationStructure Scene : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> Output : register(u0);

struct ray_payload
{
    uint hit;
};

[shader("raygeneration")]
void rgen()
{
    uint const index = DispatchRaysIndex().x;

    RayDesc ray;
    // Even indices aim inside the triangle, odd ones well outside it.
    ray.Origin = float3(index % 2 == 0 ? 0.25f : 5.0f, 0.25f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 100.0f;

    ray_payload p;
    p.hit = 0;
    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
    Output[index] = p.hit;
}

[shader("miss")]
void rmiss(inout ray_payload p)
{
    p.hit = 0;
}

[shader("closesthit")]
void rchit(inout ray_payload p, in BuiltInTriangleIntersectionAttributes attr)
{
    p.hit = 1;
}
