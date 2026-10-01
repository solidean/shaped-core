#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// EMIT-150 and EMIT-151: a coherent member's declaration, an image's subscript, and an atomic image's update.

namespace
{
/// A single-pass reduction's shape: a coherent image and counter, an atomic image of each sign, and plain images.
constexpr cc::string_view k_reduce = "require device_coherence\n"
                                     "require image_atomics\n"
                                     "\n"
                                     "binding set:\n"
                                     "    @coherent mip: mut image_2d[.r32_float]\n"
                                     "    @coherent counter: mut buffer[atomic[uint]]\n"
                                     "    @atomic depth: mut image_2d[.r32_uint]\n"
                                     "    @atomic offsets: mut image_2d_array[.r32_sint]\n"
                                     "\n"
                                     "@compute(8, 8) fun reduce(@thread_id id: int3){set}:\n"
                                     "    let xy = id.xy\n"
                                     "    set.mip[xy] = 2.0\n"
                                     "    texture_barrier()\n"
                                     "    let before = set.depth[xy / 2].max(7u)\n"
                                     "    let shifted = set.offsets[xy, layer = 1].add(-3)\n"
                                     "    set.depth[xy].store(before + set.depth[xy].load())\n"
                                     "    let seen = set.counter[0].add(1u)\n";

/// Subscripts of plain images: a store, a compound assignment, and an array's layer.
constexpr cc::string_view k_copy = "binding canvas:\n"
                                   "    weights: mut image_2d[.r32_float]\n"
                                   "    target: out image_2d[.rgba8_unorm]\n"
                                   "    source: image_2d[.rgba8_unorm]\n"
                                   "    layers: mut image_2d_array[.r32_float]\n"
                                   "\n"
                                   "@compute(8, 8) fun copy(@thread_id id: int3){canvas}:\n"
                                   "    let xy = id.xy\n"
                                   "    canvas.target[xy] = canvas.source[xy]\n"
                                   "    canvas.weights[xy] += 0.5\n"
                                   "    canvas.layers[xy, layer = 2] = canvas.weights[xy]\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - a coherent member is globallycoherent in HLSL and coherent(device) in MSL")
{
    // EMIT-150
    auto const dx12 = text_of(k_reduce, target::hlsl_dx12);
    CHECK(dx12.contains("globallycoherent RWTexture2D<float> set_mip : register(u0, space0);\n"));
    CHECK(dx12.contains("globallycoherent RWStructuredBuffer<uint> set_counter : register(u1, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<uint> set_depth : register(u2, space0);\n"));
    CHECK(!dx12.contains("globallycoherent RWTexture2D<uint>"));
    // DXC writes it as SPIR-V's Coherent
    auto const vulkan = text_of(k_reduce, target::hlsl_vulkan);
    CHECK(vulkan.contains("[[vk::binding(0, 0)]] [[vk::image_format(\"r32f\")]] globallycoherent RWTexture2D<float> "
                          "set_mip;\n"));
    CHECK(vulkan.contains("[[vk::binding(1, 0)]] globallycoherent RWStructuredBuffer<uint> set_counter;\n"));

    auto const msl = text_of(k_reduce, target::msl);
    CHECK(msl.contains("    coherent(device) texture2d<float, access::read_write> set_mip [[id(0)]];\n"));
    CHECK(msl.contains("    device coherent(device) atomic_uint* set_counter [[id(1)]];\n"));
    CHECK(msl.contains("    texture2d<uint, access::read_write> set_depth [[id(2)]];\n"));
    // the barrier that publishes an image's writes is the texture barrier, device-scoped in both
    CHECK(dx12.contains("    DeviceMemoryBarrierWithGroupSync();\n"));
    CHECK(msl.contains("    threadgroup_barrier(mem_flags::mem_texture);\n"));
}

TEST("sgl emit - an atomic image's update is the target's own atomic on the texel")
{
    // EMIT-151: HLSL takes the texel as the place `Interlocked*` updates, and gives the value before as a buffer's does
    auto const dx12 = text_of(k_reduce, target::hlsl_dx12);
    CHECK(dx12.contains("    uint atomic_before;\n"
                        "    InterlockedMax(set_depth[xy / 2], 7u, atomic_before);\n"
                        "    const uint before = atomic_before;\n"));
    CHECK(dx12.contains("    InterlockedAdd(set_offsets[int3(xy, 1)], (-3), atomic_before_1);\n"));
    // a load is an `or` with nothing, and a store an exchange whose value before nobody reads
    CHECK(dx12.contains("InterlockedOr(set_depth[xy], 0u, "));
    CHECK(dx12.contains("InterlockedExchange(set_depth[xy], "));

    // MSL calls the texture's own atomics, whose value is four wide
    auto const msl = text_of(k_reduce, target::msl);
    CHECK(msl.contains("    const uint before = set_depth.atomic_fetch_max(uint2(xy / 2), 7u).x;\n"));
    CHECK(msl.contains("    const int shifted = set_offsets.atomic_fetch_add(uint2(xy), uint(1), -3).x;\n"));
    CHECK(msl.contains("set_depth.atomic_store(uint2(xy), uint4(before + set_depth.atomic_load(uint2(xy)).x));\n"));

    // EMIT-109: WebGPU has neither, and an entry point needing both names each
    CHECK(sgl::emit::dump_errors(emit_source(k_reduce, 0, target::wgsl))
          == "target-lacks-feature reduce needs device_coherence, which WebGPU does not have\n"
             "target-lacks-feature reduce needs image_atomics, which WebGPU does not have\n");
}

TEST("sgl emit - an image's subscript is the load or the store it stands for")
{
    // EMIT-151: HLSL subscripts the image itself, and a compound assignment loads the texel into the expression
    auto const dx12 = text_of(k_copy, target::hlsl_dx12);
    CHECK(dx12.contains("    canvas_target[xy] = canvas_source[xy];\n"));
    CHECK(dx12.contains("    canvas_weights[xy] = canvas_weights[xy] + 0.5;\n"));
    // the store takes its value before the layer, so a value read in the order written is bound ahead of it
    CHECK(dx12.contains("    const float value = canvas_weights[xy];\n"
                        "    canvas_layers[int3(xy, 2)] = value;\n"));

    auto const wgsl = text_of(k_copy, target::wgsl);
    CHECK(wgsl.contains("    textureStore(canvas_target, xy, textureLoad(canvas_source, xy));\n"));
    CHECK(wgsl.contains("    textureStore(canvas_weights, xy, vec4f(textureLoad(canvas_weights, xy).x + 0.5, 0.0, 0.0, "
                        "0.0));\n"));

    auto const msl = text_of(k_copy, target::msl);
    CHECK(msl.contains("    canvas_target.write(canvas_source.read(uint2(xy)), uint2(xy));\n"));
    CHECK(msl.contains("    canvas_layers.write(float4(value, 0.0, 0.0, 0.0), uint2(xy), uint(2));\n"));
}
