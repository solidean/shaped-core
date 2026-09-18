// The tier-2 ray-tracing fixture, kept as source next to the blob compiled from it.
//
// Compiled by hand, and the result checked in as raytrace.metallib.h:
//
//   xcrun -sdk macosx metal -O2 -o raytrace.metallib raytrace.metal
//   xxd -i raytrace.metallib > raytrace.metallib.h
//
// That keeps the tier-2 binary free of the shader library and the compiler toolchain, the same way
// double_compute.metal does for the dispatch path.
//
// **HAND-WRITTEN, AND TEMPORARY.**
// The agreed shape is HLSL run through SPIRV-Cross, with the HLSL, the generated MSL and the metallib all checked in —
// because the argument-buffer layout this backend encodes was chosen to match what SPIRV-Cross emits, so a fixture
// written by hand can agree with the backend and still disagree with every real shader.
// Neither DXC nor SPIRV-Cross is available on an arm64 macOS host today, which is why this exists in the meantime.
// Regenerate it from HLSL once they are; libs/graphics/shaped-graphics/docs/TODO.md carries the gap.
//
// The binding shape is written by hand in the test beside it, so the test states what it means rather than inheriting
// whatever a reflector produced.
//
// One argument buffer per binding group, member [[id(n)]] = sg::binding::index, bound at buffer index = group index.
// The ray-tracing function tables live in sg::reserved_binding_group, which is buffer index 3.

#include <metal_raytracing>
#include <metal_stdlib>
using namespace metal;
using namespace raytracing;

/// Binding group 0: the scene to trace, and where to put the answer.
struct scene_bindings
{
    instance_acceleration_structure scene [[id(0)]];
    device float* out [[id(1)]];
};

/// The reserved group, whose four members are what sg::raytracing_shader_table encodes.
/// Each of sg's index spaces gets a table of its own, because a visible function table is typed by the signature of
/// what it holds and miss / closest-hit / callable signatures differ.
struct rt_tables
{
    intersection_function_table<instancing, triangle_data> hit [[id(0)]];
    visible_function_table<void(thread float&)> miss [[id(1)]];
    visible_function_table<void(thread float&, float)> closest_hit [[id(2)]];
    visible_function_table<void(thread float&)> callable [[id(3)]];
};

/// Thread 0 aims at the fixture triangle and thread 1 aims past it, so one dispatch covers the hit path and the miss
/// path and the two cannot be confused: a hit reports distance 1 and a miss reports -1.
static ray probe_ray(uint tid)
{
    ray r;
    r.origin = tid == 0 ? float3(0.25f, 0.25f, -1.0f) : float3(5.0f, 5.0f, -1.0f);
    r.direction = float3(0.0f, 0.0f, 1.0f);
    r.min_distance = 0.0f;
    r.max_distance = 100.0f;
    return r;
}

/// Inline ray query: an ordinary compute kernel traverses and shades in its own code.
/// This is the portable trace shape — the one dx12, vulkan and metal all have.
kernel void trace_inline(device scene_bindings& b [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    intersector<instancing, triangle_data> isect;
    auto const hit = isect.intersect(probe_ray(tid), b.scene, 0xFF);
    b.out[tid] = hit.type == intersection_type::none ? -1.0f : hit.distance;
}

[[visible]] void miss_marker(thread float& payload) { payload = -1.0f; }
[[visible]] void closest_hit_marker(thread float& payload, float distance) { payload = distance; }

/// An any-hit function: runs *during* traversal, on non-opaque geometry only, and decides whether a hit counts.
/// This one rejects every hit, so a scene it is attached to reports a miss where the same geometry marked opaque
/// reports a hit — which is what makes "the function ran" observable rather than assumed.
[[intersection(triangle, instancing, triangle_data)]]
bool any_hit_reject(float distance [[distance]])
{
    return false;
}

/// The result a procedural intersection function returns.
struct procedural_result
{
    bool accept [[accept_intersection]];
    float distance [[distance]];
};

/// A procedural primitive's intersection function: traversal hands it an AABB and it decides what, if anything, the
/// ray hit inside.
/// It reports a distance of 3 — not the AABB's own entry distance — so the number proves this function produced it.
[[intersection(bounding_box, instancing)]]
procedural_result procedural_hit(float3 origin [[origin]],
                                 float3 direction [[direction]],
                                 float min_distance [[min_distance]],
                                 float max_distance [[max_distance]])
{
    return {true, 3.0f};
}

/// The pipeline path's raygen shader, which on Metal IS the kernel rather than something a pipeline dispatches.
/// Nothing invokes miss or closest-hit for you here: the kernel traverses and then calls them through the tables.
kernel void raygen(device scene_bindings& b [[buffer(0)]],
                   device rt_tables& t [[buffer(3)]],
                   uint tid [[thread_position_in_grid]])
{
    intersector<instancing, triangle_data> isect;
    auto const hit = isect.intersect(probe_ray(tid), b.scene, 0xFF, t.hit);

    float payload = 0.0f;
    if (hit.type == intersection_type::none)
        t.miss[0](payload);
    else
        t.closest_hit[0](payload, hit.distance);

    b.out[tid] = payload;
}

/// The procedural counterpart, whose intersector carries no triangle_data because its geometry has no triangles.
/// A separate entry point rather than a branch: the table's tags are part of its type, so one kernel cannot hold both.
struct procedural_tables
{
    intersection_function_table<instancing> hit [[id(0)]];
    visible_function_table<void(thread float&)> miss [[id(1)]];
    visible_function_table<void(thread float&, float)> closest_hit [[id(2)]];
    visible_function_table<void(thread float&)> callable [[id(3)]];
};

kernel void raygen_procedural(device scene_bindings& b [[buffer(0)]],
                              device procedural_tables& t [[buffer(3)]],
                              uint tid [[thread_position_in_grid]])
{
    intersector<instancing> isect;
    auto const hit = isect.intersect(probe_ray(tid), b.scene, 0xFF, t.hit);

    float payload = 0.0f;
    if (hit.type == intersection_type::none)
        t.miss[0](payload);
    else
        t.closest_hit[0](payload, hit.distance);

    b.out[tid] = payload;
}

/// Indirect recursion, which is what DXR's MaxTraceRecursionDepth actually buys and what Metal's maxCallStackDepth
/// sizes the stack for.
///
/// The table that reaches this function names the function's own signature, so the type is self-referential — which is
/// expressible only because the recursive parameter is a *pointer* to the incomplete struct.
struct recursive_tables;
using recursive_shade = void(thread float&, device recursive_tables*, float);

struct recursive_tables
{
    intersection_function_table<instancing, triangle_data> hit [[id(0)]];
    visible_function_table<void(thread float&)> miss [[id(1)]];
    visible_function_table<recursive_shade> closest_hit [[id(2)]];
    visible_function_table<void(thread float&)> callable [[id(3)]];
};

/// Counts one per level and calls itself through the table, so the payload that comes back IS the depth reached.
/// A stack too small for the declared depth shows up here as a wrong number rather than as a diagnostic.
[[visible]] void closest_hit_recursive(thread float& payload, device recursive_tables* t, float remaining)
{
    payload += 1.0f;
    if (remaining > 1.0f)
        t->closest_hit[0](payload, t, remaining - 1.0f);
}

kernel void raygen_recursive(device scene_bindings& b [[buffer(0)]],
                             device recursive_tables& t [[buffer(3)]],
                             uint tid [[thread_position_in_grid]])
{
    intersector<instancing, triangle_data> isect;
    auto const hit = isect.intersect(probe_ray(tid), b.scene, 0xFF, t.hit);

    float payload = 0.0f;
    if (hit.type == intersection_type::none)
        t.miss[0](payload);
    else
        t.closest_hit[0](payload, &t, 4.0f); // four levels, which the pipeline declares as max_recursion_depth

    b.out[tid] = payload;
}
