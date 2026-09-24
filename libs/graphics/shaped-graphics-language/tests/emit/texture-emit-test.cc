#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// One of each resource a post-processing pass has: sample a texture, write one image, accumulate into another.
constexpr cc::string_view k_blur = "binding post:\n"
                                   "    texel_size: float2\n"
                                   "    src: texture2d[float4]\n"
                                   "    dst: out image2d[.rgba8_unorm]\n"
                                   "    acc: mut image2d[.r32_float]\n"
                                   "    sampler bilinear:\n"
                                   "        filter = .linear\n"
                                   "        address = .clamp_edge\n"
                                   "\n"
                                   "@compute(8, 8) fun blur(@thread_id id: int3){post}:\n"
                                   "    let xy = int2(id.x, id.y)\n"
                                   "    let uv = ((xy as float2) + float2(0.5, 0.5)) * post.texel_size\n"
                                   "    let c = DEBUG_sample_level(post.src, uv, 0.0, post.bilinear)\n"
                                   "    DEBUG_store(post.dst, xy, c)\n"
                                   "    DEBUG_store(post.acc, xy, DEBUG_load(post.acc, xy) + c.x)\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - textures, images and a static sampler are resources of the group, in declaration order")
{
    CHECK(text_of(k_blur, target::wgsl)
          == "// SGL compute entry point 'blur', written as WGSL.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct post_data {\n"
             "    texel_size: vec2f,\n"
             "}\n"
             "\n"
             "@group(0) @binding(0) var<uniform> post: post_data;\n"
             "@group(0) @binding(1) var post_src: texture_2d<f32>;\n"
             "@group(0) @binding(2) var post_dst: texture_storage_2d<rgba8unorm, write>;\n"
             "@group(0) @binding(3) var post_acc: texture_storage_2d<r32float, read_write>;\n"
             "@group(0) @binding(4) var post_bilinear: sampler;\n"
             "\n"
             "@compute @workgroup_size(8, 8, 1)\n"
             "fn blur(@builtin(global_invocation_id) id_in: vec3u) {\n"
             "    let id: vec3i = vec3i(id_in);\n"
             "    let xy: vec2i = vec2i(id.x, id.y);\n"
             "    let uv: vec2f = (vec2f(xy) + vec2f(0.5, 0.5)) * post.texel_size;\n"
             "    let c: vec4f = textureSampleLevel(post_src, post_bilinear, uv, 0.0);\n"
             "    textureStore(post_dst, xy, vec4f(c));\n"
             "    textureStore(post_acc, xy, vec4f(textureLoad(post_acc, xy).x + c.x, 0.0, 0.0, 0.0));\n"
             "}\n");

    auto const vulkan = text_of(k_blur, target::hlsl_vulkan);
    CHECK(vulkan.contains("#pragma sc group 0\n"
                          "namespace post_bindings\n"
                          "{\n"
                          "    ConstantBuffer<post_data> post;\n"
                          "    Texture2D<float4> post_src;\n"
                          "#pragma sc format rgba8_unorm\n"
                          "    RWTexture2D<float4> post_dst;\n"
                          "#pragma sc format r32_float\n"
                          "    RWTexture2D<float> post_acc;\n"
                          "#pragma sc static filter=(linear, linear, linear) address=(clamp_edge, clamp_edge, "
                          "clamp_edge)\n"
                          "    SamplerState post_bilinear;\n"
                          "}\n"));
    CHECK(vulkan.contains("    const float4 c = post_bindings::post_src.SampleLevel(post_bindings::post_bilinear, uv, "
                          "0.0);\n"
                          "    post_bindings::post_dst[xy] = c;\n"
                          "    post_bindings::post_acc[xy] = post_bindings::post_acc[xy] + c.x;\n"));

    // Both HLSL targets state the format the same way, and slib's pass writes what SPIR-V needs from it.
    auto const dx12 = text_of(k_blur, target::hlsl_dx12);
    CHECK(dx12.contains("#pragma sc format r32_float\n    RWTexture2D<float> post_acc;\n"));
}

TEST("sgl emit - each texture shape and depth is its target's own type, and 1D is 2D on WebGPU")
{
    constexpr auto shapes = "binding set:\n"
                            "    a: texture1d[float]\n"
                            "    b: texture2d_array[uint4]\n"
                            "    c: texture_cube[float3]\n"
                            "    d: texture2d_depth\n"
                            "    e: image3d[.rgba16_float]\n"
                            "    f: comparison_sampler\n"
                            "\n"
                            "@compute(1) fun cs(@thread_id id: int3){set}:\n"
                            "    let unused = id.x\n";
    auto const wgsl = text_of(shapes, target::wgsl);
    CHECK(wgsl.contains("var set_a: texture_2d<f32>;\n"));
    CHECK(wgsl.contains("var set_b: texture_2d_array<u32>;\n"));
    CHECK(wgsl.contains("var set_c: texture_cube<f32>;\n"));
    CHECK(wgsl.contains("var set_d: texture_depth_2d;\n"));
    CHECK(wgsl.contains("var set_e: texture_storage_3d<rgba16float, read>;\n"));
    CHECK(wgsl.contains("var set_f: sampler_comparison;\n"));

    auto const hlsl = text_of(shapes, target::hlsl_dx12);
    CHECK(hlsl.contains("    Texture1D<float> set_a;\n"));
    CHECK(hlsl.contains("    Texture2DArray<uint4> set_b;\n"));
    CHECK(hlsl.contains("    TextureCube<float3> set_c;\n"));
    CHECK(hlsl.contains("    Texture2D<float> set_d;\n"));
    CHECK(hlsl.contains("    RWTexture3D<float4> set_e;\n"));
    CHECK(hlsl.contains("    SamplerComparisonState set_f;\n"));
}

TEST("sgl emit - MSL declines a group of textures as it declines one of buffers")
{
    CHECK(sgl::emit::dump_errors(emit_source(k_blur, 0, target::msl)).contains("unsupported"));
}
