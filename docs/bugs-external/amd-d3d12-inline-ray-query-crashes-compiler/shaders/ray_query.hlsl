// The least an inline trace is: one query, traced and stepped, and its committed status written out.
// Compiled alone it creates a pipeline on every driver; it crashes only after `plain.hlsl`, both unsigned.
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> statuses : register(u1);

[numthreads(64, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID)
{
    RayDesc ray;
    ray.Origin = float3(id.x, 0.0, -1.0);
    ray.TMin = 0.0;
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMax = 1000.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    while (q.Proceed())
    {
    }
    statuses[id.x] = q.CommittedStatus();
}
