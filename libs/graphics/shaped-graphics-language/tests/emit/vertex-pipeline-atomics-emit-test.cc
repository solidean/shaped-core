#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::check::feature;
using sgl::emit::mesh_form;
using sgl::emit::target;

// An atomic in a vertex, a geometry and the tessellation stages, which needs vertex_pipeline_stores (CHK-531).
// What a target writes for the atomic is what it writes in any other stage; only the feature differs.

namespace
{
constexpr auto k_program = cc::string_view(R"(require geometry_shader, tessellation_shader, vertex_pipeline_stores

binding trace:
    count: mut buffer[atomic[uint]]
    records: mut buffer[uint]

@vertex struct corner_in:
    position: float2

struct staged:
    @position position: hpos4

struct control_point:
    position: float2

struct tri_factors:
    @edge_factors edges: float[3]
    @inside_factors inside: float

@vertex fun counted_vs(v: corner_in, @vertex_index i: int){trace} -> staged:
    trace.count[1].max(i as uint)
    return {position = hpos4(v.position.x, v.position.y, 0.5, 1.0)}

@vertex fun corner_vs(v: corner_in) -> control_point:
    return {position = v.position}

@geometry(max_vertices = 3)
fun appending_gs(tri: staged[3], @primitive_id primitive: int, stream: mut triangle_stream[staged]){trace}:
    let slot = trace.count[0].add(1u)
    trace.records[slot] = primitive as uint
    for i in 0 ..< 3:
        stream.emit(tri[i])

@tessellation_control(partitioning = .integer, winding = .counter_clockwise)
fun counted_tc(patch: control_point[3]){trace} -> tri_factors:
    trace.count[2].add(1u)
    return {edges = [2.0, 2.0, 2.0], inside = 2.0}

@tessellation_evaluation
fun counted_te(patch: control_point[3], f: tri_factors, @domain_location uvw: float3){trace} -> staged:
    trace.count[3].bit_or(1u)
    let p = patch[0].position * uvw.x + patch[1].position * uvw.y + patch[2].position * uvw.z
    return {position = hpos4(p.x, p.y, 0.5, 1.0)}
)");

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view entry,
                                                                   target t,
                                                                   sgl::emit::native_route native
                                                                   = sgl::emit::native_route::text)
{
    return sgl::compile_to_text({.source = k_program,
                                 .entry_point = entry,
                                 .target = t,
                                 .device_features = sgl::check::feature_set(feature::vertex_pipeline_stores),
                                 .native = native});
}

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> compute_half(cc::string_view entry,
                                                                       target t,
                                                                       cc::string_view vertex,
                                                                       cc::string_view control = "")
{
    return sgl::compile_to_text({.source = k_program,
                                 .entry_point = entry,
                                 .target = t,
                                 .mesh = mesh_form::compute,
                                 .polyfill_vertex = vertex,
                                 .polyfill_control = control});
}

constexpr cc::string_view k_stages[] = {"counted_vs", "appending_gs", "counted_tc", "counted_te"};
} // namespace

TEST("sgl emit - an atomic in a vertex, geometry or tessellation stage is HLSL's own on dx12 and vulkan (CHK-531)")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
        for (auto const entry : k_stages)
        {
            auto const e = compiled(entry, t);
            REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK(e.value().features.has(feature::vertex_pipeline_stores)).dump("entry", entry);
            CHECK(e.value().text.contains("Interlocked")).dump("entry", entry).dump("text", e.value().text);
        }
    // dx12 declares the buffer the atomic updates as the plain integers it holds, in any stage
    CHECK(compiled("appending_gs", target::hlsl_dx12)
              .value()
              .text.contains("RWStructuredBuffer<uint> trace_count : register(u0, space0);"));
}

TEST("sgl emit - an atomic in a vertex, geometry or tessellation stage lowers to SGL's own SPIR-V and DXIL (CHK-531)")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
        for (auto const entry : k_stages)
        {
            auto const e = compiled(entry, t, sgl::emit::native_route::require);
            REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK((t == target::hlsl_vulkan ? !e.value().spirv.empty() : !e.value().dxil.empty())).dump("entry", entry);
        }
}

TEST("sgl emit - metal writes a vertex stage's atomic, and WebGPU refuses it by naming the feature (EMIT-109)")
{
    auto const msl = compiled("counted_vs", target::msl);
    REQUIRE(msl.has_value()).dump("error", msl.has_error() ? msl.error() : cc::string());
    CHECK(msl.value().text.contains("device atomic_uint* trace_count")).dump("text", msl.value().text);
    CHECK(msl.value().text.contains("atomic_fetch_max_explicit(&trace_count[1]")).dump("text", msl.value().text);

    auto const wgsl = compiled("counted_vs", target::wgsl);
    REQUIRE(wgsl.has_error());
    CHECK(wgsl.error().contains("target-lacks-feature")).dump("error", wgsl.error());
    CHECK(wgsl.error().contains("counted_vs needs vertex_pipeline_stores, which WebGPU does not have"))
        .dump("error", wgsl.error());

    // a vertex stage with no atomic needs nothing of the device
    auto const plain = compiled("corner_vs", target::wgsl);
    REQUIRE(plain.has_value()).dump("error", plain.has_error() ? plain.error() : cc::string());
    CHECK(!plain.value().features.has(feature::vertex_pipeline_stores));
}

TEST("sgl emit - a polyfilled geometry or tessellation stage is a compute pass, which needs no vertex_pipeline_stores")
{
    // CHK-525 and CHK-527: the compute half writes its atomics as any compute stage does, on every target
    for (auto const t : {target::wgsl, target::msl, target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const geometry = compute_half("appending_gs", t, "counted_vs");
        REQUIRE(geometry.has_value()).dump("error", geometry.has_error() ? geometry.error() : cc::string());
        CHECK(geometry.value().stage == sgl::check::stage::compute);
        CHECK(!geometry.value().features.has(feature::vertex_pipeline_stores));
        CHECK(!geometry.value().features.has(feature::geometry_shader));

        auto const control = compute_half("counted_tc", t, "corner_vs");
        REQUIRE(control.has_value()).dump("error", control.has_error() ? control.error() : cc::string());
        CHECK(!control.value().features.has(feature::vertex_pipeline_stores));

        auto const evaluation = compute_half("counted_te", t, "", "counted_tc");
        REQUIRE(evaluation.has_value()).dump("error", evaluation.has_error() ? evaluation.error() : cc::string());
        CHECK(!evaluation.value().features.has(feature::vertex_pipeline_stores));
    }
    // the vertex stage's own atomic and the geometry stage's both stand in the one compute half
    auto const wgsl = compute_half("appending_gs", target::wgsl, "counted_vs");
    CHECK(wgsl.value().text.contains("atomicMax(&trace_count[1]")).dump("text", wgsl.value().text);
    CHECK(wgsl.value().text.contains("atomicAdd(&trace_count[0], 1u)")).dump("text", wgsl.value().text);
}
