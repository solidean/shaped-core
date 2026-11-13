// An 8 x 4 grid of rays straight down +z onto one triangle that covers the grid.
// The closest hit stores 10 into the payload and the raygen writes the payload out, so every word must be 10.
// The raygen asks for its invocations to be regrouped first, which NO_REORDER leaves out.
//
// repro.cc compiles this file into two pipelines on one device, the second with -disable-payload-qualifiers.
// On NVIDIA the second pipeline's raygen then reads the 0 it stored, as if the closest hit had written nothing.

struct [raypayload] radiance
{
    int value : read(caller, closesthit) : write(caller, closesthit);
};

RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> result : register(u0);

[shader("raygeneration")]
void rg()
{
    uint2 const id = DispatchRaysIndex().xy;
#ifndef NO_REORDER
    dx::MaybeReorderThread(id.x < 4 ? 0u : 1u, 1);
#endif
    radiance p;
    p.value = 0;
    RayDesc r;
    r.Origin = float3(float(id.x) + 0.25, float(id.y) + 0.5, -1.0);
    r.TMin = 0.0;
    r.Direction = float3(0.0, 0.0, 1.0);
    r.TMax = 1e30;
    TraceRay(scene, 0, 0xff, 0, 1, 0, r, p);
    result[id.x + id.y * 8] = uint(p.value);
}

[shader("miss")]
void ms(inout radiance p)
{
}

[shader("closesthit")]
void ch(inout radiance p, BuiltInTriangleIntersectionAttributes a)
{
    p.value = 10;
}
