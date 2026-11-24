#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

// EMIT-150 and EMIT-151: a coherent member's declaration, an image's subscript, and an atomic image's update.
// CHK-390 and CHK-391: a texel converted to another width, a swizzle where it narrows and a construction where it widens.

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

/// A texel narrowed, and one widened from a buffer's element (CHK-391).
constexpr cc::string_view k_texels = "binding io:\n"
                                     "    source: texture_2d[float4]\n"
                                     "    flags: buffer[uint2]\n"
                                     "    narrow: out image_2d[.rg32_float]\n"
                                     "    single: out image_2d[.r32_float]\n"
                                     "    wide: out image_2d[.rgba32_uint]\n"
                                     "\n"
                                     "@compute(8, 8) fun convert(@thread_id id: int3){io}:\n"
                                     "    let c = io.source.load(id.xy)\n"
                                     "    io.narrow[id.xy] = c as texel[.rg32_float]\n"
                                     "    io.single[id.xy] = c as texel[.r32_float]\n"
                                     "    io.wide[id.xy] = io.flags[id.x] as texel[.rgba32_uint]\n";

/// A store into the format the host picks, which is one texel per compile (CHK-390).
constexpr cc::string_view k_picked = "@option const out_format: pixel_format = .rgba8_unorm\n"
                                     "\n"
                                     "binding io:\n"
                                     "    source: texture_2d[float4]\n"
                                     "    picked: out image_2d[out_format]\n"
                                     "\n"
                                     "@compute(8, 8) fun pick(@thread_id id: int3){io}:\n"
                                     "    let c = io.source.load(id.xy)\n"
                                     "    io.picked[id.xy] = c.xy as texel[out_format]\n";

/// An `out` image of each 16-bit normalized format, a read-only one, and a `mut` one in an entry point of its own.
constexpr cc::string_view k_norm16 = "require extended_image_formats\n"
                                     "require readwrite_image_formats\n"
                                     "\n"
                                     "binding outs:\n"
                                     "    a: out image_2d[.r16_unorm]\n"
                                     "    b: out image_2d[.r16_snorm]\n"
                                     "    c: out image_2d[.rg16_unorm]\n"
                                     "    d: out image_2d[.rg16_snorm]\n"
                                     "    e: out image_2d[.rgba16_unorm]\n"
                                     "    f: out image_2d[.rgba16_snorm]\n"
                                     "    source: image_2d[.rg16_snorm]\n"
                                     "\n"
                                     "binding muts:\n"
                                     "    plain: mut image_2d[.r32_float]\n"
                                     "    normal: mut image_2d[.rg16_unorm]\n"
                                     "\n"
                                     "@compute(8, 8) fun store(@thread_id id: int3){outs}:\n"
                                     "    outs.a[id.xy] = 0.25\n"
                                     "    outs.b[id.xy] = -0.25\n"
                                     "    outs.c[id.xy] = float2(0.25, 0.5)\n"
                                     "    outs.d[id.xy] = outs.source[id.xy]\n"
                                     "    outs.e[id.xy] = float4(1.0)\n"
                                     "    outs.f[id.xy] = float4(-1.0)\n"
                                     "\n"
                                     "@compute(8, 8) fun flip(@thread_id id: int3){muts}:\n"
                                     "    muts.normal[id.xy] = muts.normal[id.xy].yx\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

cc::string picked_text(cc::string_view format, target t)
{
    sgl::check::option_value const given[] = {{.name = "out_format", .value = cc::string(format)}};
    auto const r = sgl::compile_to_text({.source = k_picked, .entry_point = "pick", .target = t, .options = given});
    REQUIRE(r.has_value());
    return r.value().text;
}
} // namespace

TEST("sgl emit - a 16-bit normalized image's HLSL texel says unorm or snorm, and vulkan names its format")
{
    // EMIT-281 and EMIT-98: the texel names the normalization, and `[[vk::image_format]]` is DXC's spelling
    auto const dx12 = text_of(k_norm16, target::hlsl_dx12);
    CHECK(dx12.contains("RWTexture2D<unorm float> outs_a : register(u0, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<snorm float> outs_b : register(u1, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<unorm float2> outs_c : register(u2, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<snorm float2> outs_d : register(u3, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<unorm float4> outs_e : register(u4, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<snorm float4> outs_f : register(u5, space0);\n"));
    CHECK(dx12.contains("RWTexture2D<snorm float2> outs_source : register(u6, space0);\n"));

    auto const vulkan = text_of(k_norm16, target::hlsl_vulkan);
    CHECK(vulkan.contains("[[vk::binding(0, 0)]] [[vk::image_format(\"r16\")]] RWTexture2D<unorm float> outs_a;\n"));
    CHECK(vulkan.contains("[[vk::binding(1, 0)]] [[vk::image_format(\"r16snorm\")]] RWTexture2D<snorm float> "
                          "outs_b;\n"));
    CHECK(vulkan.contains("[[vk::binding(2, 0)]] [[vk::image_format(\"rg16\")]] RWTexture2D<unorm float2> outs_c;\n"));
    CHECK(vulkan.contains("[[vk::binding(3, 0)]] [[vk::image_format(\"rg16snorm\")]] RWTexture2D<snorm float2> "
                          "outs_d;\n"));
    CHECK(vulkan.contains("[[vk::binding(4, 0)]] [[vk::image_format(\"rgba16\")]] RWTexture2D<unorm float4> "
                          "outs_e;\n"));
    CHECK(vulkan.contains("[[vk::binding(5, 0)]] [[vk::image_format(\"rgba16snorm\")]] RWTexture2D<snorm float4> "
                          "outs_f;\n"));

    // a `mut` one is the same type, and no other format's texel takes a qualifier
    auto const flipped = emit_source(k_norm16, 1, target::hlsl_dx12);
    CHECK(sgl::emit::dump_errors(flipped) == "");
    CHECK(flipped.text.contains("RWTexture2D<float> muts_plain : register(u0, space0);\n"));
    CHECK(flipped.text.contains("RWTexture2D<unorm float2> muts_normal : register(u1, space0);\n"));
    CHECK(!text_of(k_copy, target::hlsl_dx12).contains("norm "));
}

TEST("sgl emit - WGSL and MSL write an out or read-only 16-bit normalized image, and refuse a mut one")
{
    // the texel format of texture-formats-tier1, which WGSL has as a language extension and so enables nothing
    auto const wgsl = text_of(k_norm16, target::wgsl);
    CHECK(wgsl.contains("var outs_a: texture_storage_2d<r16unorm, write>;\n"));
    CHECK(wgsl.contains("var outs_b: texture_storage_2d<r16snorm, write>;\n"));
    CHECK(wgsl.contains("var outs_c: texture_storage_2d<rg16unorm, write>;\n"));
    CHECK(wgsl.contains("var outs_d: texture_storage_2d<rg16snorm, write>;\n"));
    CHECK(wgsl.contains("var outs_e: texture_storage_2d<rgba16unorm, write>;\n"));
    CHECK(wgsl.contains("var outs_f: texture_storage_2d<rgba16snorm, write>;\n"));
    CHECK(wgsl.contains("var outs_source: texture_storage_2d<rg16snorm, read>;\n"));
    CHECK(!wgsl.contains("enable"));

    auto const msl = text_of(k_norm16, target::msl);
    CHECK(msl.contains("    texture2d<float, access::write> outs_c [[id(2)]];\n"));
    CHECK(msl.contains("    texture2d<float, access::read> outs_source [[id(6)]];\n"));

    // EMIT-280: neither has a device that reads and writes one, so the entry point that lists it has no text there
    CHECK(sgl::emit::dump_errors(emit_source(k_norm16, 1, target::wgsl))
          == "target-lacks-feature flip lists `muts.normal`, a `mut` image of .rg16_unorm, which no WebGPU device "
             "reads and writes; declare it `out`, or read it as a texture\n");
    CHECK(sgl::emit::dump_errors(emit_source(k_norm16, 1, target::msl))
          == "target-lacks-feature flip lists `muts.normal`, a `mut` image of .rg16_unorm, which no Metal device "
             "reads and writes; declare it `out`, or read it as a texture\n");
    // the native targets write it
    CHECK(sgl::emit::dump_errors(emit_source(k_norm16, 1, target::hlsl_vulkan)) == "");
}

TEST("sgl emit - a mut 16-bit normalized image in a binding array, a generic or an option's format is refused and "
     "qualified as a plain one is")
{
    // EMIT-280 looks through a binding array to its element, and HLSL's array keeps the element's qualifier
    constexpr auto arrayed = "require extended_image_formats, readwrite_image_formats, binding_arrays\n"
                             "\n"
                             "binding many:\n"
                             "    normals: mut image_2d[.rg16_snorm][4]\n"
                             "\n"
                             "@compute(8, 8) fun spread(@thread_id id: int3){many}:\n"
                             "    many.normals[1].store(id.xy, -many.normals[0].load(id.xy))\n";
    CHECK(text_of(arrayed, target::hlsl_dx12)
              .contains("RWTexture2D<snorm float2> many_normals[4] : register(u0, space0);\n"));
    CHECK(sgl::emit::dump_errors(emit_source(arrayed, 0, target::msl)).contains("a `mut` image of .rg16_snorm"));

    // a generic over the format takes the binding's image, and no text of it drops the qualifier
    constexpr auto generic = "require extended_image_formats, readwrite_image_formats\n"
                             "\n"
                             "binding frames:\n"
                             "    color: mut image_2d[.rgba16_unorm]\n"
                             "\n"
                             "fun copy_texel[f: pixel_format](img: mut image_2d[f], from: int2, to: int2):\n"
                             "    img.store(to, img.load(from))\n"
                             "\n"
                             "@compute(8, 8) fun shuffle(@thread_id id: int3){frames}:\n"
                             "    copy_texel(frames.color, id.xy, id.yx)\n";
    auto const generic_dx12 = text_of(generic, target::hlsl_dx12);
    CHECK(generic_dx12.contains("RWTexture2D<unorm float4> frames_color : register(u0, space0);\n"));
    CHECK(!generic_dx12.contains("RWTexture2D<float4>"));
    CHECK(sgl::emit::dump_errors(emit_source(generic, 0, target::wgsl)).contains("a `mut` image of .rgba16_unorm"));

    // an option's format is the format the compile gave, judged and spelled as a written one
    constexpr auto picked = "require extended_image_formats, readwrite_image_formats\n"
                            "\n"
                            "@option const out_format: pixel_format = .r32_float\n"
                            "\n"
                            "binding io:\n"
                            "    picked: mut image_2d[out_format]\n"
                            "\n"
                            "@compute(8, 8) fun pick(@thread_id id: int3){io}:\n"
                            "    io.picked[id.xy] = io.picked[id.xy]\n";
    sgl::check::option_value const given[] = {{.name = "out_format", .value = cc::string(".rg16_unorm")}};
    auto const native
        = sgl::compile_to_text({.source = picked, .entry_point = "pick", .target = target::hlsl_dx12, .options = given});
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    CHECK(native.value().text.contains("RWTexture2D<unorm float2> io_picked : register(u0, space0);\n"));
    for (auto const t : {target::wgsl, target::msl})
    {
        auto const refused
            = sgl::compile_to_text({.source = picked, .entry_point = "pick", .target = t, .options = given});
        REQUIRE(refused.has_error());
        CHECK(refused.error().contains("a `mut` image of .rg16_unorm")).dump("error", refused.error());
        // the default format reads and writes everywhere
        CHECK(sgl::compile_to_text({.source = picked, .entry_point = "pick", .target = t}).has_value());
    }
}

TEST("sgl emit - a coherent member is globallycoherent in HLSL, and device-coherent memory or a device-coherent "
     "texture in MSL")
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
    // an argument buffer's field takes no `coherent` qualifier, so a texture says it in its template arguments
    CHECK(msl.contains("    texture2d<float, access::read_write, memory_coherence_device> set_mip [[id(0)]];\n"));
    CHECK(!msl.contains("coherent(device) texture"));
    CHECK(msl.contains("    device coherent(device) atomic_uint* set_counter [[id(1)]];\n"));
    CHECK(msl.contains("    texture2d<uint, access::read_write> set_depth [[id(2)]];\n"));
    // the barrier that publishes an image's writes is the texture barrier, device-scoped in both
    CHECK(dx12.contains("    DeviceMemoryBarrierWithGroupSync();\n"));
    // Metal's barrier orders the threadgroup alone, so a device-scoped fence follows it
    CHECK(msl.contains("    threadgroup_barrier(mem_flags::mem_texture);\n"
                       "    atomic_thread_fence(mem_flags::mem_texture, memory_order_seq_cst, "
                       "thread_scope_device);\n"));
}

TEST("sgl emit - MSL fences a storage or texture barrier device-wide only where a member is coherent")
{
    constexpr auto barriers = "{}binding work:\n"
                              "    {}values: mut buffer[uint]\n"
                              "\n"
                              "@workgroup binding shared:\n"
                              "    n: uint\n"
                              "\n"
                              "@compute(64) fun cs(@thread_id id: int3){{work, shared}}:\n"
                              "    work.values[id.x] = 1u\n"
                              "    storage_barrier()\n"
                              "    texture_barrier()\n"
                              "    workgroup_barrier()\n";
    auto const coherent = text_of(cc::format(barriers, "require device_coherence\n\n", "@coherent "), target::msl);
    CHECK(coherent.contains(
        "    threadgroup_barrier(mem_flags::mem_device);\n"
        "    atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n"
        "    threadgroup_barrier(mem_flags::mem_texture);\n"
        "    atomic_thread_fence(mem_flags::mem_texture, memory_order_seq_cst, thread_scope_device);\n"
        "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"));
    // workgroup memory is the threadgroup's, which the barrier alone orders
    CHECK(!coherent.contains("atomic_thread_fence(mem_flags::mem_threadgroup"));

    auto const plain = text_of(cc::format(barriers, "", ""), target::msl);
    CHECK(plain.contains("    threadgroup_barrier(mem_flags::mem_device);\n"
                         "    threadgroup_barrier(mem_flags::mem_texture);\n"));
    CHECK(!plain.contains("atomic_thread_fence"));
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
    CHECK(msl.contains("set_depth.atomic_store(uint4(before + set_depth.atomic_load(uint2(xy)).x), uint2(xy));\n"));

    // EMIT-109: WebGPU has neither, and an entry point needing both names each
    CHECK(sgl::emit::dump_errors(emit_source(k_reduce, 0, target::wgsl))
          == "target-lacks-feature reduce needs device_coherence, which WebGPU does not have\n"
             "target-lacks-feature reduce needs image_atomics, which WebGPU does not have\n");
}

TEST("sgl emit - an atomic image's compare-exchange is HLSL's own, and a loop over MSL's weak one")
{
    // EMIT-161 on a texel: MSL's texture atomic hands back four wide, and the loop retries a failure that read `expected`
    constexpr auto source = "require image_atomics\n"
                            "\n"
                            "binding set:\n"
                            "    @atomic depth: mut image_2d[.r32_uint]\n"
                            "    @atomic offsets: mut image_2d_array[.r32_sint]\n"
                            "\n"
                            "@compute(8, 8) fun claim(@thread_id id: int3){set}:\n"
                            "    let a = set.depth[id.xy].compare_exchange_strong(0u, 7u)\n"
                            "    let b = set.offsets[id.xy, layer = 2].compare_exchange_strong(0, id.x)\n"
                            "    set.depth[id.xy].store(a + (b as uint))\n";
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("    InterlockedCompareExchange(set_depth[id.xy], 0u, 7u, atomic_before);\n"
                        "    const uint a = atomic_before;\n"));
    CHECK(dx12.contains("    InterlockedCompareExchange(set_offsets[int3(id.xy, 2)], 0, id.x, atomic_before_1);\n"));

    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    const uint2 exchange_texel = uint2(id.xy);\n"
                       "    const uint exchange_expected = 0u;\n"
                       "    const uint exchange_desired = 7u;\n"
                       "    uint4 exchange_before = uint4(exchange_expected);\n"
                       "    while (!set_depth.atomic_compare_exchange_weak(exchange_texel, &exchange_before, "
                       "uint4(exchange_desired)) && exchange_before.x == exchange_expected)\n"
                       "    {\n"
                       "    }\n"
                       "    const uint a = exchange_before.x;\n"));
    CHECK(msl.contains("    const uint exchange_layer = uint(2);\n"));
    CHECK(msl.contains("    while (!set_offsets.atomic_compare_exchange_weak(exchange_texel_1, exchange_layer, "
                       "&exchange_before_1, int4(exchange_desired_1)) && exchange_before_1.x == "
                       "exchange_expected_1)\n"));
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

TEST("sgl emit - MSL fences a store to an image the entry point also reads, so the thread's own read sees it")
{
    // EMIT-151: `weights` is read and written, and its store is fenced before the read that follows
    auto const msl = text_of(k_copy, target::msl);
    CHECK(msl.contains("    canvas_weights.write(float4(canvas_weights.read(uint2(xy)).x + 0.5, 0.0, 0.0, 0.0), "
                       "uint2(xy));\n"
                       "    canvas_weights.fence();\n"
                       "    const float value = canvas_weights.read(uint2(xy)).x;\n"));
    // `target` is only written and `layers` only stored to, so neither is fenced
    CHECK(!msl.contains("canvas_target.fence()"));
    CHECK(!msl.contains("canvas_layers.fence()"));
    // HLSL and WGSL order one invocation's own accesses
    CHECK(!text_of(k_copy, target::hlsl_dx12).contains("fence"));
    CHECK(!text_of(k_copy, target::wgsl).contains("fence"));
}

TEST("sgl emit - a texel narrows by a swizzle and widens by the target's own vector, filled from (0, 0, 0, 1)")
{
    // CHK-391: the operand of a widening is bound once, and every component read from the local
    auto const dx12 = text_of(k_texels, target::hlsl_dx12);
    CHECK(dx12.contains("    io_narrow[id.xy] = c.xy;\n"));
    CHECK(dx12.contains("    io_single[id.xy] = c.x;\n"));
    CHECK(dx12.contains("    const uint2 texel = io_flags[id.x];\n"
                        "    io_wide[id.xy] = uint4(texel, 0u, 1u);\n"));
    CHECK(text_of(k_texels, target::hlsl_vulkan).contains("    io_wide[id.xy] = uint4(texel, 0u, 1u);\n"));

    auto const wgsl = text_of(k_texels, target::wgsl);
    CHECK(wgsl.contains("    textureStore(io_narrow, id.xy, vec4f(c.xy, 0.0, 0.0));\n"));
    CHECK(wgsl.contains("    textureStore(io_single, id.xy, vec4f(c.x, 0.0, 0.0, 0.0));\n"));
    CHECK(wgsl.contains("    let texel: vec2u = io_flags[id.x];\n"
                        "    textureStore(io_wide, id.xy, vec4u(texel, 0u, 1u));\n"));

    auto const msl = text_of(k_texels, target::msl);
    CHECK(msl.contains("    io_narrow.write(float4(c.xy, 0.0, 0.0), uint2(id.xy));\n"));
    CHECK(msl.contains("    io_single.write(float4(c.x, 0.0, 0.0, 0.0), uint2(id.xy));\n"));
    CHECK(msl.contains("    io_wide.write(uint4(texel, 0u, 1u), uint2(id.xy));\n"));
}

TEST("sgl emit - a texel of an option's format is the texel of the format the compile gave")
{
    // CHK-390: widened for a four-channel format, on every target
    CHECK(picked_text(".rgba8_unorm", target::hlsl_dx12).contains("    io_picked[id.xy] = float4(texel, 0.0, 1.0);\n"));
    CHECK(picked_text(".rgba8_unorm", target::wgsl)
              .contains("    textureStore(io_picked, id.xy, vec4f(texel, 0.0, 1.0));\n"));
    // the value itself for a format of its own width, and its first component for a single channel
    auto const same = picked_text(".rg32_float", target::hlsl_vulkan);
    CHECK(same.contains("[[vk::image_format(\"rg32f\")]] RWTexture2D<float2> io_picked;\n"));
    CHECK(same.contains("    io_picked[id.xy] = c.xy;\n"));
    CHECK(picked_text(".r32_float", target::msl).contains("    io_picked.write(float4(c.x, 0.0, 0.0, 0.0), "));
}
